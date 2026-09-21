/**
 * @file vol_ctrl.cpp
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "vol_ctrl.h"

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
constexpr int kMinimumPlaybackRaw = 60;
constexpr int kMaximumUserPlaybackRaw = 200;
constexpr int kMaximumHardwarePlaybackRaw = 255;
const char kVolumeStatePath[] = "/home/cat/face_media/volume.dat";
}

/** @brief 解析平台路径并初始化音量防抖定时器。 */
AmpVolumeController::AmpVolumeController(QObject *parent)
    : QObject(parent),
      // 音量属于业务持久化数据，路径固定，不受 QT_YCEST_CONFIG_DIR 影响。
      dataFile_(QString::fromLatin1(kVolumeStatePath)),
      ampDevice_(Rk3566Platform::amplifierDevice()),
      alsaCard_(Rk3566Platform::alsaCard())
{
    rawVolumeProcess_ = new QProcess(this);
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
    const QString legacyFile = QDir(Rk3566Platform::configDir())
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
    qInfo() << "[audio] volume file=" << dataFile_ << "percent=" << m_volume;
    initAudioPathOnce();
}

/** @return 0~100 线性映射并限制到 60~200 的 RK809 原始值。 */
int AmpVolumeController::volumePercentToRaw(int value)
{
    value = qBound(0, value, 100);
    return qRound(kMinimumPlaybackRaw
                  + value * (kMaximumUserPlaybackRaw - kMinimumPlaybackRaw) / 100.0);
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
    rawValue = qBound(0, rawValue, kMaximumHardwarePlaybackRaw);
    return runAmixer({QStringLiteral("-c"), alsaCard_, QStringLiteral("cset"),
                      QStringLiteral("numid=%1").arg(kPlaybackVolumeNumId),
                      QStringLiteral("%1,%1").arg(rawValue)});
}

/** @brief 打开功放控制设备。 */
bool AmpVolumeController::amplifierOpen()
{
    if (ampDevice_.isEmpty()) {
        return true;
    }

    QFile file(ampDevice_);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[audio] open amplifier device failed:" << ampDevice_ << file.errorString();
        return false;
    }
    const char value = 1;
    return file.write(&value, 1) == 1;
}

/** @brief 关闭功放控制设备。 */
bool AmpVolumeController::amplifierClose()
{
    if (ampDevice_.isEmpty()) {
        return true;
    }

    QFile file(ampDevice_);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[audio] open amplifier device failed:" << ampDevice_ << file.errorString();
        return false;
    }
    const char value = 0;
    return file.write(&value, 1) == 1;
}

/** @brief 首次使用时初始化功放和 ALSA 音频路径。 */
void AmpVolumeController::initAudioPathOnce()
{
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

/** @brief 将用户音量和功放标志写入持久化文件。 */
void AmpVolumeController::writeVolumeFile(int volume, bool ampFlag)
{
    QFileInfo info(dataFile_);
    QDir().mkpath(info.absolutePath());

    QFile file(dataFile_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "[audio] open volume file failed:" << dataFile_ << file.errorString();
        return;
    }

    QTextStream stream(&file);
    stream << "volume:" << volume << "\n"
           << "amp_flag:" << (ampFlag ? 1 : 0) << "\n";
}

/** @return 从持久化文件读取的 0~100 音量；文件不存在时创建为 50%。 */
int AmpVolumeController::loadVolumeFromFile()
{
    int volume = 50;
    bool ampFlag = true;

    QFile file(dataFile_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        writeVolumeFile(volume, ampFlag);
        m_volume = volume;
        m_ampFlag = ampFlag;
        return m_volume;
    }

    const QString content = QString::fromUtf8(file.readAll());
    QRegExp volumeRx(QStringLiteral("volume:(\\d+)"));
    QRegExp ampRx(QStringLiteral("amp_flag:(\\d+)"));
    if (volumeRx.indexIn(content) >= 0) volume = volumeRx.cap(1).toInt();
    if (ampRx.indexIn(content) >= 0) ampFlag = ampRx.cap(1).toInt() != 0;

    m_volume = qBound(0, volume, 100);
    Q_UNUSED(ampFlag);
    // 新标尺的 0% 对应原始值 60，是最低可听音量而非静音。
    m_ampFlag = true;
    return m_volume;
}

/** @return 持久化文件记录的功放启用标志。 */
bool AmpVolumeController::loadAmpFlagFromFile()
{
    return m_ampFlag;
}

/** @brief 将用户音量限制到 0~100，并防抖应用和保存。 */
void AmpVolumeController::setVolume(int value)
{
    m_pendingVolume = qBound(0, value, 100);
    // 对外立即反映用户刚设置的值。硬件写入仍保留 80 ms 防抖，避免拖动
    // 音量条时频繁启动 amixer；提示音若在防抖期间到来，也能保存并恢复
    // 到这个最新值，而不是恢复到上一次已经落盘的旧值。
    m_volume = m_pendingVolume;
    m_ampFlag = true;
    m_debounce.start();
}

/** @brief 按百分比设置功放和 RK809 原始音量，并发出结果信号。 */
void AmpVolumeController::applyNow(int value)
{
    value = qBound(0, value, 100);
    m_volume = value;
    m_ampFlag = true;
    m_pendingVolume = -1;

    writeVolumeFile(m_volume, m_ampFlag);
    emit volumeApplied(m_volume);
    // 用户拖动音量条后的实际 amixer 写入同样走异步串行队列，避免正常
    // 音量设置与提示音切换在 Qt 主线程中造成可见停顿。
    setRawVolumeTemporaryAsync(volumePercentToRaw(value));
}

/** @brief 立即设置 0~255 RK809 原始值，用于提示音等临时增益。 */
void AmpVolumeController::setRawVolumeTemporary(int rawValue)
{
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

    // 临时提示音音量会接管硬件。若此时还有用户音量等待防抖应用，先将
    // 最新值持久化，避免取消定时器后丢失本次设置；提示结束后调用方会
    // 按 currentRawVolume() 恢复到这个值。
    if (m_pendingVolume >= 0) {
        writeVolumeFile(m_volume, m_ampFlag);
        emit volumeApplied(m_volume);
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

/** @brief 将临时 RK809 原始音量请求排入非阻塞 QProcess 队列。 */
void AmpVolumeController::setRawVolumeTemporaryAsync(int rawValue)
{
    rawValue = qBound(0, rawValue, kMaximumHardwarePlaybackRaw);
    pendingRawVolume_ = rawValue;
    startPendingRawVolumeRequest();
}

/** @brief 异步启动最新一次原始音量请求。 */
void AmpVolumeController::startPendingRawVolumeRequest()
{
    if (rawVolumeRequestActive_ || pendingRawVolume_ < 0 || !rawVolumeProcess_) return;

    if (m_pendingVolume >= 0) {
        writeVolumeFile(m_volume, m_ampFlag);
        emit volumeApplied(m_volume);
    }
    m_debounce.stop();
    m_pendingVolume = -1;

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
    rawVolumeTimeout_.stop();
    rawVolumeRequestActive_ = false;
    activeRawVolume_ = -1;

    if (!success) {
        qWarning() << "[audio] async amixer failed raw=" << completedRaw << detail;
    } else if (completedRaw <= 0) {
        amplifierClose();
    }
    emit rawVolumeTemporaryApplied(completedRaw, success);

    if (pendingRawVolume_ >= 0) {
        QTimer::singleShot(0, this, &AmpVolumeController::startPendingRawVolumeRequest);
    }
}

/** @return 进程内唯一播报音频焦点管理器。 */
AudioPromptFocusManager &AudioPromptFocusManager::instance()
{
    static AudioPromptFocusManager manager;
    return manager;
}

/** @brief 初始化视频排空延时和统一的异步硬件音量回调。 */
AudioPromptFocusManager::AudioPromptFocusManager(QObject *parent)
    : QObject(parent)
{
    // 在界面初始化阶段完成一次控制器/音频路径准备，避免首次播报时同步
    // 查找 amixer 和初始化 RK809 造成可见停顿。
    volumeController_ = new AmpVolumeController(this);
    connect(volumeController_, &AmpVolumeController::rawVolumeTemporaryApplied,
            this, [this](int rawValue, bool success) {
        if (rawValue == 230 && !owners_.isEmpty()) {
            boostReady_ = true;
            if (!success) {
                qWarning() << "[audio-focus] prompt volume boost failed; continuing";
            }
            const QSet<QObject *> readyOwners = pendingReadyOwners_;
            pendingReadyOwners_.clear();
            for (QObject *owner : readyOwners) {
                if (owners_.contains(owner)) emit focusReady(owner);
            }
            return;
        }
        if (owners_.isEmpty() && savedAmpRawVolume_ >= 0 &&
                rawValue == savedAmpRawVolume_) {
            if (!success) {
                qWarning() << "[audio-focus] user volume restore failed";
            }
            savedAmpRawVolume_ = -1;
            restoreVideoPlayers();
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
    restoreVideoPlayers();
    if (volumeController_ && savedAmpRawVolume_ >= 0) {
        volumeController_->setRawVolumeTemporary(savedAmpRawVolume_);
    }
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
}

/** @brief 视频旧采样排空后异步提升 RK809 音量。 */
void AudioPromptFocusManager::requestBoost()
{
    if (owners_.isEmpty()) return;
    if (!volumeController_) return;
    if (savedAmpRawVolume_ < 0) {
        volumeController_->loadVolumeFromFile();
        savedAmpRawVolume_ = volumeController_->currentRawVolume();
    }
    volumeController_->setRawVolumeTemporaryAsync(230);
}

/** @brief 获取可重叠的播报焦点。 */
void AudioPromptFocusManager::acquire(QObject *owner)
{
    if (!owner) return;
    const bool firstOwner = owners_.isEmpty();
    const bool newOwner = !owners_.contains(owner);
    owners_.insert(owner);
    pendingReadyOwners_.insert(owner);
    if (newOwner) {
        connect(owner, &QObject::destroyed, this,
                [this](QObject *destroyedOwner) { release(destroyedOwner); });
    }
    duckNewActiveVideoPlayers();

    if (boostReady_) {
        pendingReadyOwners_.remove(owner);
        const QPointer<QObject> safeOwner(owner);
        QTimer::singleShot(0, this, [this, safeOwner]() {
            if (safeOwner && owners_.contains(safeOwner.data())) {
                emit focusReady(safeOwner.data());
            }
        });
        return;
    }
    if (firstOwner) boostDelayTimer_.start();
}

/** @brief 释放焦点，并在全部播报结束后恢复用户音量。 */
void AudioPromptFocusManager::release(QObject *owner)
{
    if (!owner) return;
    pendingReadyOwners_.remove(owner);
    if (owners_.remove(owner) <= 0) return;
    if (!owners_.isEmpty()) return;

    boostDelayTimer_.stop();
    boostReady_ = false;
    if (volumeController_ && savedAmpRawVolume_ >= 0) {
        volumeController_->setRawVolumeTemporaryAsync(savedAmpRawVolume_);
    } else {
        savedAmpRawVolume_ = -1;
        restoreVideoPlayers();
    }
}
