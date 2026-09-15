/**
 * @file vol_ctrl.cpp
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "vol_ctrl.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegExp>
#include <QTextStream>
#include <QtMath>

#include "platform/rk3566_platform.h"

namespace {
constexpr int kPlaybackPathNumId = 1;
constexpr int kPlaybackVolumeNumId = 3;
constexpr int kMinimumPlaybackRaw = 60;
constexpr int kMaximumPlaybackRaw = 200;
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
                  + value * (kMaximumPlaybackRaw - kMinimumPlaybackRaw) / 100.0);
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
    rawValue = qBound(0, rawValue, kMaximumPlaybackRaw);
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

    amplifierOpen();
    setPlaybackVolumeRaw(volumePercentToRaw(value));
    writeVolumeFile(m_volume, m_ampFlag);
    emit volumeApplied(m_volume);
}

/** @brief 立即设置 0~200 RK809 原始值，用于提示音等临时增益。 */
void AmpVolumeController::setRawVolumeTemporary(int rawValue)
{
    // 临时提示音音量会接管硬件。若此时还有用户音量等待防抖应用，先将
    // 最新值持久化，避免取消定时器后丢失本次设置；提示结束后调用方会
    // 按 currentRawVolume() 恢复到这个值。
    if (m_pendingVolume >= 0) {
        writeVolumeFile(m_volume, m_ampFlag);
        emit volumeApplied(m_volume);
    }
    m_debounce.stop();
    m_pendingVolume = -1;

    rawValue = qBound(0, rawValue, kMaximumPlaybackRaw);
    if (rawValue <= 0) {
        setPlaybackVolumeRaw(0);
        amplifierClose();
        return;
    }

    amplifierOpen();
    setPlaybackVolumeRaw(rawValue);
}
