/**
 * @file vol_ctrl.cpp
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-16；修改人：Du lin；音量持久化路径及旧配置目录中的音量兼容路径应用共享分区前缀。
 */

#include "vol_ctrl.h"
#include "common/configuration_work.h"
#include "common/serial_work_queue.h"
#include "src/call/AudioOutputFocus.h"

#include "gst_player_widget.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegExp>
#include <QSettings>
#include <QTextStream>
#include <QtMath>

#include "platform/rk3566_platform.h"

namespace {
constexpr int kPlaybackPathNumId = 1;
constexpr int kPlaybackVolumeNumId = 3;
constexpr int kMinimumPlaybackRaw = 100; /**< 视频/系统非零音量起点，对应 1%。 */
constexpr int kMaximumUserPlaybackRaw = 230; /**< 视频/系统 100% 对应的 RK809 原始值。 */
constexpr int kMinimumPromptPlaybackRaw = 200;
constexpr int kMaximumPromptPlaybackRaw = 250;
constexpr int kDefaultPromptVolumePercent = 60; // 对应旧版固定原始值 230。
constexpr int kMaximumHardwarePlaybackRaw = 255;
const char kVolumeStatePath[] = "/home/cat/face_media/volume.dat";

/** @brief UI 线程串行访问的通话和广播文件音频硬件写入保护。 */
struct CallOutputState {
    QPointer<AmpVolumeController> owner; /**< 通话焦点的控制器，借用；恢复完成后清空。 */
    QPointer<AmpVolumeController> promptOwner; /**< 广播文件音频的控制器，借用；用户增益恢复完成后清空。 */
    QSet<QProcess *> writers; /**< 已启动的异步 amixer；以指针作键，退出/析构移除。 */
    quint64 generation=0; /**< 无符号硬件代次，焦点接管/放开时递增并自然回绕。 */
    int latestSystemPercent=-1; /**< 保护期间最后一次 setVolume；-1 表示从持久化文件恢复。 */
};
/** @return 仅供 UI 线程访问的共享硬件保护状态，不创建控制器或执行硬件操作。 */
CallOutputState &callOutputState()
{
    static CallOutputState state;return state;
}

struct PersistedVolumeState {
    int volume = 50;
    int promptVolume = kDefaultPromptVolumePercent;
    bool ampFlag = true;
    bool fileExists = false;
    bool promptVolumeExists = false;
};

PersistedVolumeState readPersistedVolumeState(const QString &path)
{
    PersistedVolumeState state;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return state;

    state.fileExists = true;
    const QString content = QString::fromUtf8(file.readAll());
    QRegExp volumeRx(QStringLiteral("(^|\\n)volume:(\\d+)"));
    QRegExp promptRx(QStringLiteral("(^|\\n)prompt_volume:(\\d+)"));
    QRegExp ampRx(QStringLiteral("(^|\\n)amp_flag:(\\d+)"));
    if (volumeRx.indexIn(content) >= 0) state.volume = volumeRx.cap(2).toInt();
    if (promptRx.indexIn(content) >= 0) {
        state.promptVolume = promptRx.cap(2).toInt();
        state.promptVolumeExists = true;
    }
    if (ampRx.indexIn(content) >= 0) state.ampFlag = ampRx.cap(2).toInt() != 0;
    state.volume = qBound(0, state.volume, 100);
    state.promptVolume = qBound(0, state.promptVolume, 100);
    return state;
}

bool writePersistedVolumeState(const QString &path, const PersistedVolumeState &state)
{
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[audio] open volume file failed:" << path << file.errorString();
        return false;
    }
    QTextStream stream(&file);
    stream << "volume:" << qBound(0, state.volume, 100) << "\n"
           << "prompt_volume:" << qBound(0, state.promptVolume, 100) << "\n"
           << "amp_flag:" << (state.ampFlag ? 1 : 0) << "\n";
    return stream.status() == QTextStream::Ok;
}
}

/**
 * @brief 从当前业务分区恢复音量并初始化功放与防抖定时器。
 * @param[in] parent 可选的 Qt 父对象。
 */
AmpVolumeController::AmpVolumeController(QObject *parent)
    : QObject(parent),
      // 音量使用平台数据分区中的固定路径，不依赖 QT_YCEST_CONFIG_DIR。
      dataFile_(Rk3566Platform::dataPath(QString::fromLatin1(kVolumeStatePath))),
      ampDevice_(Rk3566Platform::amplifierDevice()),
      alsaCard_(Rk3566Platform::alsaCard())
{
    rawVolumeProcess_ = new QProcess(this);
    // 控制器先析构时，QProcess 会自行结束；集合须以进程实际析构为屏障，不能提前放行。
    connect(rawVolumeProcess_,&QObject::destroyed,[process=rawVolumeProcess_]() {
        completeHardwareWrite(process);
    });
    connect(rawVolumeProcess_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (!rawVolumeRequestActive_) return;
        const QString detail = QString::fromLocal8Bit(rawVolumeProcess_->readAllStandardOutput())
                + QString::fromLocal8Bit(rawVolumeProcess_->readAllStandardError());
        finishRawVolumeRequest(exitStatus == QProcess::NormalExit && exitCode == 0,
                               detail.trimmed());
    });
    connect(rawVolumeProcess_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && rawVolumeRequestActive_) {
            finishRawVolumeRequest(false, rawVolumeProcess_->errorString());
        }
    });
    rawVolumeTimeout_.setSingleShot(true);
    rawVolumeTimeout_.setInterval(1500);
    connect(&rawVolumeTimeout_, &QTimer::timeout, this, [this]() {
        if (!rawVolumeRequestActive_ || !rawVolumeProcess_) return;
        qWarning() << "[audio] async amixer timeout raw=" << activeRawVolume_;
        rawVolumeProcess_->kill();
    });

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_debounce, &QTimer::timeout, this, [this]() {
        if (m_pendingVolume >= 0) {
            applyNow(m_pendingVolume);
            m_pendingVolume = -1;
        }
    });

    QDir().mkpath(QFileInfo(dataFile_).absolutePath());

    // 兼容旧版随 QT_YCEST_CONFIG_DIR 保存的 volume.dat。目标文件不存在时
    // 只迁移一次，保留用户上次音量；旧文件不删除，便于现场回退旧程序。
    const QString legacyFile = QDir(Rk3566Platform::persistentConfigDir())
            .filePath(QStringLiteral("volume.dat"));
    if (legacyFile != dataFile_ &&
        !QFileInfo::exists(dataFile_) && QFileInfo::exists(legacyFile)) {
        if (QFile::copy(legacyFile, dataFile_)) {
            qInfo() << "[audio] migrated volume file:" << legacyFile << "->" << dataFile_;
        } else {
            qWarning() << "[audio] failed to migrate volume file:"
                       << legacyFile << "->" << dataFile_;
        }
    }

    // 先读取（或按 50% 首次创建）持久化值，再初始化硬件，避免每次创建
    // 控制器时短暂把已保存音量覆盖为默认值。
    loadVolumeFromFile();
    qInfo() << "[audio] volume file=" << dataFile_
            << "systemPercent=" << m_volume
            << "promptPercent=" << m_promptVolume
            << "promptRaw=" << currentPromptRawVolume();
    initAudioPathOnce();
}

/** @return 0% 为静音 raw0；1~100% 线性映射到 100~230，并四舍五入。 */
int AmpVolumeController::volumePercentToRaw(int value)
{
    value = qBound(0, value, 100);
    if (value == 0) return 0;
    return qRound(kMinimumPlaybackRaw
                  + (value - 1) * (kMaximumUserPlaybackRaw - kMinimumPlaybackRaw) / 99.0);
}

/** @return 0~100 线性映射并限制到 200~250 的 RK809 原始值。 */
int AmpVolumeController::promptVolumePercentToRaw(int value)
{
    value = qBound(0, value, 100);
    return qRound(kMinimumPromptPlaybackRaw
                  + value * (kMaximumPromptPlaybackRaw - kMinimumPromptPlaybackRaw) / 100.0);
}

/** @return 统一播报总开关；配置缺失、文件缺失或值为空时默认开启。 */
bool AmpVolumeController::isPromptPlaybackEnabled()
{
    const QString path = Rk3566Platform::uiConfigPath();
    if (path.trimmed().isEmpty()) {
        return true;
    }

    QSettings settings(path, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif
    const QVariant value = settings.value(
                QStringLiteral("DisplayDirRect/MusicPlayEn"), 1);
    QString text = value.toString().trimmed();
    const int commentPos = text.indexOf(QLatin1Char(';'));
    if (commentPos >= 0) text = text.left(commentPos).trimmed();
    if (text.isEmpty()) return true;
    if (text == QStringLiteral("1") ||
        text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
        text.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (text == QStringLiteral("0") ||
        text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 ||
        text.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    bool ok = false;
    const int numeric = text.toInt(&ok);
    return ok ? numeric != 0 : true;
}

/** @brief 保存统一播报总开关，供人脸、刷卡和二维码共同读取。 */
bool AmpVolumeController::setPromptPlaybackEnabled(bool enabled)
{
    const QString path = Rk3566Platform::uiConfigPath();
    if (path.trimmed().isEmpty()) {
        return false;
    }
    QSettings settings(path, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif
    settings.setValue(QStringLiteral("DisplayDirRect/MusicPlayEn"), enabled ? 1 : 0);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

/** @brief 在配置声卡上执行一次 amixer 命令。 */
bool AmpVolumeController::runAmixer(const QStringList &arguments, QString *output) const
{
    const QString amixer = Rk3566Platform::findExecutable({QStringLiteral("amixer")});
    if (amixer.isEmpty()) {
        qWarning() << "[audio] amixer not found";
        return false;
    }

    QProcess process;
    process.start(amixer, arguments);
    if (!process.waitForStarted(500) || !process.waitForFinished(1500)) {
        qWarning() << "[audio] amixer timeout:" << arguments;
        process.kill();
        process.waitForFinished(200);
        return false;
    }

    const QString text = QString::fromLocal8Bit(process.readAllStandardOutput())
            + QString::fromLocal8Bit(process.readAllStandardError());
    if (output) *output = text;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "[audio] amixer failed:" << arguments << text.trimmed();
        return false;
    }
    return true;
}

/** @brief 使用 numid=3 设置 RK809 左右声道原始播放音量。 */
bool AmpVolumeController::setPlaybackVolumeRaw(int rawValue)
{
    if (callBlocksHardware()) return false;
    rawValue = qBound(0, rawValue, kMaximumHardwarePlaybackRaw);
    return runAmixer({QStringLiteral("-c"), alsaCard_, QStringLiteral("cset"),
                      QStringLiteral("numid=%1").arg(kPlaybackVolumeNumId),
                      QStringLiteral("%1,%1").arg(rawValue)});
}

/** @brief 打开功放控制设备。 */
bool AmpVolumeController::amplifierOpen()
{
    if (callBlocksHardware()) return true;
    if (ampDevice_.isEmpty()) {
        return true;
    }

    QFile file(ampDevice_);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[audio] open amplifier device failed:" << ampDevice_ << file.errorString();
        return false;
    }
    const char value = 1;
    const bool written = file.write(&value, 1) == 1;
    qInfo() << "[audio] amplifier request" << "controller=" << this
            << "enabled=" << true << "success=" << written;
    return written;
}

/** @brief 关闭功放控制设备。 */
bool AmpVolumeController::amplifierClose()
{
    if (callBlocksHardware()) return true;
    if (ampDevice_.isEmpty()) {
        return true;
    }

    QFile file(ampDevice_);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[audio] open amplifier device failed:" << ampDevice_ << file.errorString();
        return false;
    }
    const char value = 0;
    const bool written = file.write(&value, 1) == 1;
    qInfo() << "[audio] amplifier request" << "controller=" << this
            << "enabled=" << false << "success=" << written;
    return written;
}

/** @brief 首次初始化功放/路由；通话中延迟创建的其他控制器只保留逻辑配置。 */
void AmpVolumeController::initAudioPathOnce()
{
    if (callBlocksHardware()) return; // 通话中延迟创建提示控制器不能重写共享路由和增益。
    if (!runAmixer({QStringLiteral("-c"), alsaCard_, QStringLiteral("cset"),
                    QStringLiteral("numid=%1").arg(kPlaybackPathNumId),
                    QStringLiteral("SPK_HP")})) {
        qWarning() << "[audio] failed to enable RK809 Playback Path=SPK_HP";
    }

    const int rawVolume = volumePercentToRaw(m_volume);
    setPlaybackVolumeRaw(rawVolume);
    if (rawVolume > 0) {
        amplifierOpen();
    } else {
        amplifierClose();
    }
}

/** @brief 后台串行读改写系统音量/功放标志，保留播报值；完成回UI通知最新代次。 */
void AmpVolumeController::writeVolumeFile(int volume, bool ampFlag)
{
    const auto path=dataFile_;const auto revision=++volumePersistenceRevision_;
    const QPointer<AmpVolumeController> alive(this);
    ConfigurationWork::queue().enqueue([path,volume,ampFlag,revision,alive]() -> SerialWorkQueue::Task {
        return {[path,volume,ampFlag]() -> QString {
            auto state=readPersistedVolumeState(path);state.volume=qBound(0,volume,100);state.ampFlag=ampFlag;
            return writePersistedVolumeState(path,state) ? QString() : QStringLiteral("音量文件保存失败");
        },[alive,revision,volume](const QString &error) {
            if(!error.isEmpty()) qWarning()<<"[audio]"<<error;
            if(alive && revision==alive->volumePersistenceRevision_) emit alive->volumeApplied(volume);
        }};
    });
}

/** @brief 立即更新播报缓存，后台串行读改写文件以保留系统音量和功放标志。 */
void AmpVolumeController::writePromptVolumeFile(int promptVolume)
{
    m_promptVolume=qBound(0,promptVolume,100);const auto path=dataFile_;
    ConfigurationWork::queue().enqueue([path,promptVolume]() -> SerialWorkQueue::Task {
        return {[path,promptVolume]() -> QString {
            auto state=readPersistedVolumeState(path);state.promptVolume=qBound(0,promptVolume,100);state.promptVolumeExists=true;
            return writePersistedVolumeState(path,state) ? QString() : QStringLiteral("播报音量文件保存失败");
        },[](const QString &error) { if(!error.isEmpty()) qWarning()<<"[audio]"<<error; }};
    });
}

/** @brief 读取音量并归一化零音量功放标志；返回系统百分比，缺少配置时使用原默认值。 */
int AmpVolumeController::loadVolumeFromFile()
{
    const PersistedVolumeState state = readPersistedVolumeState(dataFile_);
    m_volume = state.volume;
    m_promptVolume = state.promptVolume;
    // 系统 0% 现在表示静音；同时归一化旧文件的功放标志，格式和百分比保持不变。
    m_ampFlag = m_volume > 0;
    if (!state.fileExists || !state.promptVolumeExists || state.ampFlag != m_ampFlag) {
        PersistedVolumeState migrated = state;
        migrated.volume = m_volume;
        migrated.promptVolume = m_promptVolume;
        migrated.promptVolumeExists = true;
        migrated.ampFlag = m_ampFlag;
        writePersistedVolumeState(dataFile_, migrated);
    }
    return m_volume;
}

/** @return 系统音量对应的逻辑功放标志，不代表通话/播报临时硬件开关。 */
bool AmpVolumeController::loadAmpFlagFromFile()
{
    return m_ampFlag;
}

/** @brief 丢弃播报期间的新请求和重复值，其余用户音量防抖保存。 */
void AmpVolumeController::setVolume(int value)
{
    value = qBound(0, value, 100);
    if (userVolumeBlocked()) {
        qInfo() << "[audio] user volume ignored during prompt" << "controller=" << this
                << "requested=" << value << "current=" << m_volume;
        return;
    }
    if (value == m_volume) {
        qInfo() << "[audio] unchanged user volume skipped" << "controller=" << this
                << "volume=" << value;
        return;
    }
    m_pendingVolume = value;
    // 对外立即反映用户刚设置的值。硬件写入仍保留 80 ms 防抖，避免拖动
    // 音量条时频繁启动 amixer；提示音若在防抖期间到来，也能保存并恢复
    // 到这个最新值，而不是恢复到上一次已经落盘的旧值。
    m_volume = m_pendingVolume;
    if (callOutputState().owner) callOutputState().latestSystemPercent=m_volume;
    m_ampFlag = m_volume > 0;
    m_debounce.start();
}

/** @return 广播文件音频写入权由另一控制器持有时返回 true。 */
bool AmpVolumeController::userVolumeBlocked() const
{
    const auto &state=callOutputState();
    return state.promptOwner && state.promptOwner.data()!=this;
}

/** @brief 保存语音播报百分比，正常媒体音量保持不变。 */
void AmpVolumeController::setPromptVolume(int value)
{
    m_promptVolume = qBound(0, value, 100);
    writePromptVolumeFile(m_promptVolume);
}

/** @brief 保存用户百分比并通知原回执；通话保护期间不提交媒体硬件覆盖。 */
void AmpVolumeController::applyNow(int value)
{
    value = qBound(0, value, 100);
    m_volume = value;
    m_ampFlag = m_volume > 0;
    m_pendingVolume = -1;

    writeVolumeFile(m_volume, m_ampFlag);
    // volumeApplied 在后台保存完成后发出；旧代次不确认新请求。
    // 用户拖动音量条后的实际 amixer 写入同样走异步串行队列，避免正常
    // 音量设置与提示音切换在 Qt 主线程中造成可见停顿。
    setRawVolumeTemporaryAsync(volumePercentToRaw(value));
}

/** @brief 立即设置 0~255 RK809 原始值，用于提示音等临时增益。 */
void AmpVolumeController::setRawVolumeTemporary(int rawValue)
{
    if (callBlocksHardware()) { pendingRawVolume_=-1;return; }
    // 本接口仅用于对象销毁等必须同步完成的兜底路径；先取消尚未完成的
    // 异步覆盖，防止外部 amixer 稍后退出后反向覆盖本次恢复值。
    pendingRawVolume_ = -1;
    rawVolumeTimeout_.stop();
    if (rawVolumeProcess_ && rawVolumeProcess_->state() != QProcess::NotRunning) {
        rawVolumeRequestActive_ = false;
        rawVolumeProcess_->kill();
        rawVolumeProcess_->waitForFinished(200);
    }
    activeRawVolume_ = -1;
    completeHardwareWrite(rawVolumeProcess_);

    // 临时提示音音量会接管硬件。若此时还有用户音量等待防抖应用，先将
    // 最新值持久化，避免取消定时器后丢失本次设置；提示结束后调用方会
    // 按 currentRawVolume() 恢复到这个值。
    if (m_pendingVolume >= 0) {
        writeVolumeFile(m_volume, m_ampFlag);
        // 保存完成回调负责volumeApplied，不能在此重复确认。
    }
    m_debounce.stop();
    m_pendingVolume = -1;

    rawValue = qBound(0, rawValue, kMaximumHardwarePlaybackRaw);
    if (rawValue <= 0) {
        setPlaybackVolumeRaw(0);
        amplifierClose();
        return;
    }

    amplifierOpen();
    setPlaybackVolumeRaw(rawValue);
}

/** @brief 为当前可写入者排入临时增益；受通话保护的外部实例丢弃硬件请求。 */
void AmpVolumeController::setRawVolumeTemporaryAsync(int rawValue)
{
    if (callBlocksHardware()) { pendingRawVolume_=-1;return; }
    pendingRawGeneration_=callOutputState().generation;
    rawValue = qBound(0, rawValue, kMaximumHardwarePlaybackRaw);
    pendingRawVolume_ = rawValue;
    startPendingRawVolumeRequest();
}

/** @brief 异步启动最新一次原始音量请求。 */
void AmpVolumeController::startPendingRawVolumeRequest()
{
    if (rawVolumeRequestActive_ || pendingRawVolume_ < 0 || !rawVolumeProcess_) return;
    auto &state=callOutputState();
    if (callBlocksHardware() || pendingRawGeneration_!=state.generation) { pendingRawVolume_=-1;return; }
    if (state.owner.data()==this || state.promptOwner.data()==this) {
        // 旧外部 amixer 必须实际退出，避免迟到的写入覆盖当前播报或通话增益。
        for (auto *writer:state.writers) if (writer!=rawVolumeProcess_) return;
    }

    if (m_pendingVolume >= 0) {
        writeVolumeFile(m_volume, m_ampFlag);
        // 保存完成回调负责volumeApplied，不能在此重复确认。
    }
    m_debounce.stop();
    m_pendingVolume = -1;

    activeRawGeneration_=pendingRawGeneration_;
    activeRawVolume_ = pendingRawVolume_;
    pendingRawVolume_ = -1;
    rawVolumeRequestActive_ = true;
    if (activeRawVolume_ > 0) amplifierOpen();

    const QString amixer = Rk3566Platform::findExecutable({QStringLiteral("amixer")});
    if (amixer.isEmpty()) {
        finishRawVolumeRequest(false, QStringLiteral("amixer not found"));
        return;
    }

    rawVolumeTimeout_.start();
    state.writers.insert(rawVolumeProcess_);
    rawVolumeProcess_->start(
                amixer,
                QStringList{QStringLiteral("-c"), alsaCard_, QStringLiteral("cset"),
                            QStringLiteral("numid=%1").arg(kPlaybackVolumeNumId),
                            QStringLiteral("%1,%1").arg(activeRawVolume_)});
}

/** @brief 收口一次异步 amixer 请求并串行执行等待中的最新值。 */
void AmpVolumeController::finishRawVolumeRequest(bool success, const QString &detail)
{
    if (!rawVolumeRequestActive_) return;
    const int completedRaw = activeRawVolume_;
    const bool currentGeneration=activeRawGeneration_==callOutputState().generation;
    rawVolumeTimeout_.stop();
    rawVolumeRequestActive_ = false;
    activeRawVolume_ = -1;

    if (!success) {
        qWarning() << "[audio] async amixer failed raw=" << completedRaw << detail;
    } else if (completedRaw <= 0 && currentGeneration) {
        amplifierClose();
    }
    completeHardwareWrite(rawVolumeProcess_);
    if (currentGeneration) emit rawVolumeTemporaryApplied(completedRaw, success);

    if (pendingRawVolume_ >= 0) {
        QTimer::singleShot(0, this, &AmpVolumeController::startPendingRawVolumeRequest);
    }
}

void AmpVolumeController::claimCallOutput()
{
    auto &state=callOutputState();
    if (state.owner.data()!=this) state.latestSystemPercent=-1;
    state.owner=this;++state.generation;
}
int AmpVolumeController::callRestoreRawVolume()
{
    loadVolumeFromFile();
    const int latest=callOutputState().latestSystemPercent;
    return latest>=0 ? volumePercentToRaw(latest) : currentRawVolume();
}
void AmpVolumeController::releaseCallOutput()
{
    auto &state=callOutputState();
    if (state.owner.data()!=this) return;
    state.owner.clear();state.latestSystemPercent=-1;++state.generation;
}
/** @brief 广播文件音频保护新音量请求；非通话场景废弃旧硬件代次并等待旧写者。 */
void AmpVolumeController::claimPromptOutput()
{
    auto &state=callOutputState();
    if (state.promptOwner.data()==this) return;
    state.promptOwner=this;
    // 与通话重叠时沿用通话持有者的代次，避免废弃尚在进行的通话增益。
    if (!state.owner) ++state.generation;
    qInfo() << "[audio-focus] prompt output claimed" << "controller=" << this
            << "writers=" << state.writers.size() << "generation=" << state.generation;
}
/** @brief 播报恢复音量后解除写入保护，不执行任何旧音量请求。 */
void AmpVolumeController::releasePromptOutput()
{
    auto &state=callOutputState();
    if (state.promptOwner.data()!=this) return;
    state.promptOwner.clear();
    ++state.generation;
    qInfo() << "[audio-focus] prompt output released" << "controller=" << this
            << "generation=" << state.generation;
}
bool AmpVolumeController::callBlocksHardware() const
{
    const auto &state=callOutputState();
    if (state.owner) return state.owner.data()!=this;
    return state.promptOwner && state.promptOwner.data()!=this;
}
void AmpVolumeController::completeHardwareWrite(QProcess *process)
{
    auto &state=callOutputState();state.writers.remove(process);
    if (state.owner) QTimer::singleShot(0,state.owner.data(),&AmpVolumeController::startPendingRawVolumeRequest);
    if (state.promptOwner && state.promptOwner.data()!=state.owner.data())
        QTimer::singleShot(0,state.promptOwner.data(),&AmpVolumeController::startPendingRawVolumeRequest);
}

/** @return 进程内唯一播报音频焦点管理器。 */
AudioPromptFocusManager &AudioPromptFocusManager::instance()
{
    static AudioPromptFocusManager manager;
    return manager;
}

/** @brief 初始化声卡交接、普通播报增益和既有通话音量回调。 */
AudioPromptFocusManager::AudioPromptFocusManager(QObject *parent)
    : QObject(parent)
{
    // 在界面初始化阶段完成一次控制器/音频路径准备，避免首次播报时同步
    // 查找 amixer 和初始化 RK809 造成可见停顿。
    // 混音器先于焦点管理器构造，退出时交接线程先关闭。
    GstPlayerWidget::holdVideoAudio(false);
    volumeController_ = new AmpVolumeController(this);
    connect(volumeController_, &AmpVolumeController::rawVolumeTemporaryApplied,
            this, [this](int rawValue, bool success) {
        if (rawValue == requestedBoostRaw_ && !owners_.isEmpty()) {
            boostReady_ = true;
            if (!success) {
                qWarning() << "[audio-focus] prompt volume boost failed; continuing";
            }
            if (activeOwner_) emit focusReady(activeOwner_);
            return;
        }
        if (owners_.isEmpty() && savedAmpRawVolume_ >= 0 &&
                rawValue == savedAmpRawVolume_) {
            if (!success) {
                qWarning() << "[audio-focus] user volume restore failed";
                if (requirePromptRestore_ && !restoreRetried_) {
                    restoreRetried_ = true;
                    volumeController_->setRawVolumeTemporaryAsync(savedAmpRawVolume_);
                    return;
                }
            }
            if (callOutputHeld_) {
                const int latest=volumeController_->callRestoreRawVolume();
                if (rawValue!=latest) {
                    savedAmpRawVolume_=latest;
                    volumeController_->setRawVolumeTemporaryAsync(latest);
                    return;
                }
                volumeController_->releaseCallOutput();callOutputHeld_=false;
            }
            if (!success && requirePromptRestore_) {
                qWarning() << "[audio-focus] video audio remains held after volume restore failure"
                           << "savedRaw=" << savedAmpRawVolume_ << "callOutputHeld=" << callOutputHeld_;
                return; // 增益仍未确认，保持视频静音。
            }
            requirePromptRestore_ = false;
            savedAmpRawVolume_ = -1;
            volumeController_->releasePromptOutput();
            restoreVideoPlayers();
        } else if (owners_.isEmpty() && savedAmpRawVolume_ >= 0) {
            qWarning() << "[audio-focus] volume result did not match pending restore"
                       << "completedRaw=" << rawValue << "savedRaw=" << savedAmpRawVolume_;
        }
    });

    boostDelayTimer_.setSingleShot(true);
    boostDelayTimer_.setInterval(100);
    connect(&boostDelayTimer_, &QTimer::timeout,
            this, &AudioPromptFocusManager::requestBoost);
}

AudioPromptFocusManager::~AudioPromptFocusManager()
{
    boostDelayTimer_.stop();
    promptHandoff_.reset();
    if (volumeController_ && callOutputHeld_) {
        savedAmpRawVolume_=volumeController_->callRestoreRawVolume();
        volumeController_->releaseCallOutput();callOutputHeld_=false;
    }
    if (volumeController_ && savedAmpRawVolume_ >= 0) {
        volumeController_->setRawVolumeTemporary(savedAmpRawVolume_);
    }
    if (volumeController_) volumeController_->releasePromptOutput();
    restoreVideoPlayers();
    delete volumeController_;
    volumeController_ = nullptr;
}

bool AudioPromptFocusManager::hasDuckedPlayer(GstPlayerWidget *player) const
{
    for (const QPointer<GstPlayerWidget> &saved : duckedPlayers_) {
        if (saved.data() == player) return true;
    }
    return false;
}

/** @brief 将当前新出现的业务视频纳入同一个播报静音所有权。 */
void AudioPromptFocusManager::duckNewActiveVideoPlayers()
{
    const auto widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets) {
        GstPlayerWidget *player = qobject_cast<GstPlayerWidget *>(widget);
        if (!player || !player->hasVideoOutput() || !player->hasActiveMedia() ||
                hasDuckedPlayer(player)) {
            continue;
        }
        duckedPlayers_.append(QPointer<GstPlayerWidget>(player));
        player->setAudioDucking(this, 0.0f);
    }
}

void AudioPromptFocusManager::restoreVideoPlayers()
{
    for (const QPointer<GstPlayerWidget> &player : duckedPlayers_) {
        if (player) player->clearAudioDucking(this);
    }
    duckedPlayers_.clear();
    GstPlayerWidget::holdVideoAudio(false);
    qInfo() << "[audio-focus] video audio gate released";
}

void AudioPromptFocusManager::beginPromptHandoff()
{
    boostDelayTimer_.stop();
    if (promptHandoffPending_ || requestedBoostRaw_ >= 0) return;
    if (!promptHandoff_) promptHandoff_.reset(new SerialWorkQueue(QStringLiteral("audio.prompt-handoff")));
    promptHandoffPending_ = true;
    const auto generation = ++promptHandoffGeneration_;
    const auto clearConfirmed = std::make_shared<std::atomic_bool>(false);
    promptHandoff_->enqueue([this,generation,clearConfirmed] {
        if (generation != promptHandoffGeneration_.load()) return SerialWorkQueue::Task{};
        return SerialWorkQueue::Task{[this,generation,clearConfirmed] {
            if (generation != promptHandoffGeneration_.load()) return QString();
            bool cleared = false;
            const bool resumed = GstPlayerWidget::discardQueuedAudioForPrompt(&cleared);
            clearConfirmed->store(cleared);
            return resumed ? QString() :
                QStringLiteral("shared audio clear was not confirmed");
        },[this,generation,clearConfirmed](const QString &error) {
            if (generation != promptHandoffGeneration_.load()) return;
            promptHandoffPending_ = false;
            if (!activeOwner_ || callOwnsFocus()) return;
            const auto priority = requests_.value(activeOwner_).priority;
            if (error.isEmpty() || (clearConfirmed->load() && priority <= Priority::Schedule)) {
                // 旧 PCM 已清除时增益不应依赖共享 sink 是否重开成功；避免该轮沿用用户音量。
                if (!error.isEmpty())
                    qWarning() << "[audio-focus] shared output resume failed after PCM clear; applying prompt gain";
                requirePromptRestore_ = true;
                requestBoost();
                return;
            }
            qWarning() << "[audio-focus]" << error << "; using current output volume";
            boostReady_ = true;
            emit focusReady(activeOwner_);
        }};
    });
}

void AudioPromptFocusManager::cancelPromptHandoff()
{
    ++promptHandoffGeneration_;
    promptHandoffPending_ = false;
}

/** @brief 普通播报清除旧采样后提高增益；通话与直播保留既有入口。 */
void AudioPromptFocusManager::requestBoost()
{
    if (owners_.isEmpty()) return;
    if (!volumeController_) return;
    // 每次播报前重新读取同一 volume.dat，确保管理界面刚保存的播报百分比
    // 无需重启即可生效；系统音量的恢复快照仍只在首个持有者进入时保存。
    volumeController_->loadVolumeFromFile();
    if (savedAmpRawVolume_ < 0) {
        savedAmpRawVolume_ = volumeController_->currentRawVolume();
    }
    requestedBoostRaw_ = volumeController_->currentPromptRawVolume();
    volumeController_->setRawVolumeTemporaryAsync(requestedBoostRaw_);
}

void AudioPromptFocusManager::acquire(QObject *owner, GstPlayerWidget *player, Priority priority)
{
    if (!owner || requests_.contains(owner)) return;
    if ((priority == Priority::Tagged || priority == Priority::Schedule) && volumeController_)
        volumeController_->claimPromptOutput();
    const bool hadCall = callOwnsFocus();
    const auto destroyed = connect(owner, &QObject::destroyed, this, [this](QObject *key) { release(key); });
    requests_.insert(owner, Request{player, priority, destroyed});
    order_.append(owner);
    owners_.insert(owner);
    qInfo() << "[audio-focus] acquire" << "owner=" << owner
            << "player=" << player << "priority=" << int(priority)
            << "owners=" << owners_.size();
    GstPlayerWidget::holdVideoAudio(true);
    duckNewActiveVideoPlayers();
    selectOwner();
    if (!hadCall && callOwnsFocus()) emit callFocusChanged(true);
}

bool AudioPromptFocusManager::callOwnsFocus() const
{
    const auto it = requests_.constFind(activeOwner_);
    return it != requests_.constEnd() && it->priority == Priority::Call;
}

bool AudioPromptFocusManager::blocks(Priority priority) const
{
    const auto it = requests_.constFind(activeOwner_);
    return it != requests_.constEnd() && it->priority > priority;
}

void AudioPromptFocusManager::selectOwner()
{
    QObject *next = nullptr;
    for (QObject *owner : order_) {
        if (!next || requests_[owner].priority > requests_[next].priority) next = owner;
    }
    if (next == activeOwner_) return;
    QObject *old = activeOwner_;
    activeOwner_ = next;
    qInfo() << "[audio-focus] owner selected" << "old=" << old << "next=" << next
            << "priority=" << (next ? int(requests_.value(next).priority) : -1)
            << "boostReady=" << boostReady_ << "handoffPending=" << promptHandoffPending_;
    if (next) {
        const Request &request = requests_[next];
        GstPlayerWidget::setPriorityAudioOwner(request.player, request.priority == Priority::Call);
    } else {
        GstPlayerWidget::setPriorityAudioOwner(nullptr, false);
    }
    if (old && requests_.contains(old)) emit focusSuspended(old);
    if (next && activeOwner_ == next) {
        if (!boostReady_) {
            const auto priority = requests_.value(next).priority;
            if (priority == Priority::Call) {
                cancelPromptHandoff();
                boostDelayTimer_.start();
            } else if (priority != Priority::Live || promptHandoffPending_) {
                beginPromptHandoff();
            } else if (requestedBoostRaw_ < 0) {
                boostDelayTimer_.start();
            }
        } else {
            const QPointer<QObject> guard(next);
            QTimer::singleShot(0, this, [this, guard] {
                if (guard && guard.data() == activeOwner_ && boostReady_) emit focusReady(guard);
            });
        }
    }
}

void AudioPromptFocusManager::acquireCall(QObject *owner)
{
    if (!owner || !volumeController_ || requests_.contains(owner)) return;
    volumeController_->claimCallOutput();
    callOutputHeld_ = true;
    boostReady_ = false;
    requestedBoostRaw_ = -1;
    acquire(owner, nullptr, Priority::Call);
}

void AudioPromptFocusManager::release(QObject *owner)
{
    const auto request = requests_.find(owner);
    if (request == requests_.end()) return;
    const bool hadCall = callOwnsFocus();
    qInfo() << "[audio-focus] release" << "owner=" << owner
            << "priority=" << int(request->priority) << "wasActive=" << (owner == activeOwner_)
            << "ownersBefore=" << owners_.size();
    // 直播控制线程等 ALSA 和共享输出交接完成后才撤销申请。
    if (request->priority == Priority::Live) requirePromptRestore_ = true;
    disconnect(request->destroyed);
    requests_.erase(request);
    order_.removeAll(owner);
    owners_.remove(owner);
    selectOwner();
    if (hadCall && !callOwnsFocus()) emit callFocusChanged(false);
    if (!owners_.isEmpty()) return;

    boostDelayTimer_.stop();
    cancelPromptHandoff();
    boostReady_ = false;
    requestedBoostRaw_ = -1;
    restoreRetried_ = false;
    if (volumeController_ && callOutputHeld_) savedAmpRawVolume_ = volumeController_->callRestoreRawVolume();
    if (volumeController_ && savedAmpRawVolume_ >= 0) {
        volumeController_->setRawVolumeTemporaryAsync(savedAmpRawVolume_);
    } else {
        requirePromptRestore_ = false;
        savedAmpRawVolume_ = -1;
        if (volumeController_) volumeController_->releasePromptOutput();
        restoreVideoPlayers();
    }
}

/** @brief 在应用初始化阶段复用既有音量焦点单例，后续呼叫不再首次创建控制器。 */
void VoiceCall::initializeOutputVolume()
{
    (void)AudioPromptFocusManager::instance();
}

/** @brief 异步复用工程原有音量焦点；申请票据随 owner 或取消释放，不排队旧回调。 */
void VoiceCall::acquireOutputVolume(QObject *owner,std::function<void()> ready)
{
    if (!owner || !ready) return;
    auto *ticket=new QObject(owner);
    ticket->setObjectName(QStringLiteral("voiceCallVolumeLease"));
    auto &focus=AudioPromptFocusManager::instance();
    QObject::connect(&focus,&AudioPromptFocusManager::focusReady,ticket,
                     [owner,ticket,ready](QObject *readyOwner) {
        if (readyOwner!=owner || ticket->property("ready").toBool()) return;
        ticket->setProperty("ready",true);
        ready();
    });
    focus.acquireCall(owner);
}

/** @brief 归还通话票据，后续硬件/视频恢复由既有焦点管理器执行。 */
void VoiceCall::releaseOutputVolume(QObject *owner)
{
    if (!owner) return;
    auto *ticket=owner->findChild<QObject *>(QStringLiteral("voiceCallVolumeLease"),Qt::FindDirectChildrenOnly);
    if (!ticket) return;
    delete ticket;
    AudioPromptFocusManager::instance().release(owner);
}
