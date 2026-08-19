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

/** @brief 解析平台路径并初始化音量防抖定时器。 */
AmpVolumeController::AmpVolumeController(QObject *parent)
    : QObject(parent),
      dataFile_(Rk3566Platform::volumeFile()),
      ampDevice_(Rk3566Platform::amplifierDevice()),
      alsaCard_(Rk3566Platform::alsaCard()),
      mixerControl_(Rk3566Platform::alsaMixerControl())
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_debounce, &QTimer::timeout, this, [this]() {
        if (m_pendingVolume >= 0) {
            applyNow(m_pendingVolume);
            m_pendingVolume = -1;
        }
    });

    if (mixerControl_.isEmpty()) {
        mixerControl_ = detectMixerControl();
    }
    initAudioPathOnce();
}

/** @return 0~100 线性映射并限制到 0~63 的 DAC 值。 */
int AmpVolumeController::vol100ToDac63(int value)
{
    value = qBound(0, value, 100);
    return qRound(value * 63.0 / 100.0);
}

/** @return 0~63 线性映射并限制到 0~100 的用户音量。 */
int AmpVolumeController::dac63ToVol100(int dac)
{
    dac = qBound(0, dac, 63);
    return qRound(dac * 100.0 / 63.0);
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

/** @return 配置或自动检测到的混音器控制项。 */
QString AmpVolumeController::detectMixerControl()
{
    QString output;
    if (!runAmixer({QStringLiteral("-c"), alsaCard_, QStringLiteral("scontrols")}, &output)) {
        return QString();
    }

    const QStringList preferred = {
        QStringLiteral("Master"),
        QStringLiteral("Speaker"),
        QStringLiteral("Headphone"),
        QStringLiteral("Playback"),
        QStringLiteral("PCM"),
        QStringLiteral("DAC"),
        QStringLiteral("DAC Digital")
    };
    for (const QString &control : preferred) {
        if (output.contains(QStringLiteral("'%1'").arg(control), Qt::CaseInsensitive)) {
            qInfo() << "[audio] selected ALSA mixer control:" << control;
            return control;
        }
    }

    QRegExp rx(QStringLiteral("Simple mixer control '([^']+)'"));
    if (rx.indexIn(output) >= 0) {
        const QString control = rx.cap(1).trimmed();
        qInfo() << "[audio] selected first ALSA mixer control:" << control;
        return control;
    }
    return QString();
}

/** @brief 设置 ALSA 混音器百分比和静音状态。 */
bool AmpVolumeController::setMixerPercent(int percent, bool unmute)
{
    percent = qBound(0, percent, 100);
    if (mixerControl_.isEmpty()) {
        // 某些精简系统只有 GStreamer/ALSA 音量，没有可写的 simple mixer。
        return false;
    }

    QStringList args{QStringLiteral("-c"), alsaCard_, QStringLiteral("sset"),
                     mixerControl_, QStringLiteral("%1%").arg(percent)};
    args << (unmute ? QStringLiteral("unmute") : QStringLiteral("mute"));
    return runAmixer(args);
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
    amplifierOpen();
    if (!runAmixer({QStringLiteral("-c"), QStringLiteral("0"),
                    QStringLiteral("sset"), QStringLiteral("Playback Path"),
                    QStringLiteral("SPK")})) {
        qWarning() << "[audio] failed to enable ALSA Playback Path=SPK";
    }
    setMixerPercent(m_volume, true);
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

/** @return 从持久化文件加载并应用的 0~100 音量。 */
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
    m_ampFlag = ampFlag && m_volume > 0;
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
    m_debounce.start();
}

/** @brief 按音量决定功放开关、DAC 和混音器，并发出结果信号。 */
void AmpVolumeController::applyNow(int value)
{
    value = qBound(0, value, 100);
    m_volume = value;
    m_ampFlag = value > 0;

    if (value == 0) {
        setMixerPercent(0, false);
        amplifierClose();
    } else {
        amplifierOpen();
        setMixerPercent(value, true);
    }
    writeVolumeFile(m_volume, m_ampFlag);
    emit volumeApplied(m_volume);
}

/** @brief 立即设置 0~63 DAC，用于提示音等临时增益。 */
void AmpVolumeController::setRawDacTemporary(int dac)
{
    m_debounce.stop();
    m_pendingVolume = -1;

    const int percent = dac63ToVol100(dac);
    if (percent <= 0) {
        setMixerPercent(0, false);
        amplifierClose();
        return;
    }

    amplifierOpen();
    setMixerPercent(percent, true);
}
