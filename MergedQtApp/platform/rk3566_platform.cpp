/**
 * @file rk3566_platform.cpp
 * @brief RK3566 部署路径、设备节点和系统操作的统一适配层的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "rk3566_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {

/** @return 指定环境变量去除首尾空白后的本地编码文本。 */
QString envValue(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name)).trimmed();
}

/** @return candidates 中第一个实际存在的路径；均不存在时返回 fallback。 */
QString firstExisting(const QStringList &candidates, const QString &fallback = QString())
{
    for (const QString &candidate : candidates) {
        if (!candidate.trimmed().isEmpty() && QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return fallback;
}

/**
 * @brief 读取 UTF-8 文本文件，并将设备树属性中的 NUL 字节替换为空格。
 * @return 去除首尾空白后的文本；文件无法读取时返回空字符串。
 */
QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QByteArray data = file.readAll();
    data.replace('\0', ' ');
    return QString::fromUtf8(data).trimmed();
}

/** @return 网络配置文件中指定键的非空值，否则返回 fallback。 */
QString settingValue(const QString &key, const QString &fallback = QString())
{
    const QString cfg = Rk3566Platform::netConfigPath();
    if (!QFileInfo::exists(cfg)) {
        return fallback;
    }

    QSettings settings(cfg, QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    const QString value = settings.value(key, fallback).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

/**
 * @brief 按环境变量、INI 配置、已存在候选节点、默认值的顺序解析设备路径。
 */
QString configuredDevice(const char *envName,
                         const QString &settingsKey,
                         const QStringList &candidates,
                         const QString &fallback)
{
    const QString env = envValue(envName);
    if (!env.isEmpty()) {
        return env;
    }

    const QString configured = settingValue(settingsKey);
    if (!configured.isEmpty()) {
        return configured;
    }

    return firstExisting(candidates, fallback);
}

/** @return 目录存在或创建成功且可写时返回 true。 */
bool ensureDirectory(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }
    return QDir().mkpath(path) && QFileInfo(path).isWritable();
}

bool copyConfigTemplateIfMissing(const QString &templatePath,
                                 const QString &targetPath,
                                 QString *error)
{
    const QFileInfo targetInfo(targetPath);
    if (targetInfo.exists()) {
        if (!targetInfo.isFile()) {
            if (error) {
                *error = QStringLiteral("configuration target is not a file: %1")
                                 .arg(targetPath);
            }
            return false;
        }
        if (!targetInfo.isWritable()) {
            if (error) {
                *error = QStringLiteral("configuration file is not writable: %1")
                                 .arg(targetPath);
            }
            return false;
        }
        return true;
    }

    const QString targetDir = targetInfo.absolutePath();
    if (!QDir().mkpath(targetDir) || !QFileInfo(targetDir).isWritable()) {
        if (error) {
            *error = QStringLiteral("cannot create writable configuration directory: %1")
                             .arg(targetDir);
        }
        return false;
    }

    QFile source(templatePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open embedded configuration template: %1")
                             .arg(templatePath);
        }
        return false;
    }

    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("cannot create configuration file: %1 (%2)")
                             .arg(targetPath, target.errorString());
        }
        return false;
    }

    const QByteArray data = source.readAll();
    if (target.write(data) != data.size() || !target.commit()) {
        if (error) {
            *error = QStringLiteral("cannot write configuration file: %1 (%2)")
                             .arg(targetPath, target.errorString());
        }
        return false;
    }

    const QFileDevice::Permissions permissions =
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ReadGroup | QFileDevice::ReadOther;
    // Some removable filesystems do not implement POSIX chmod semantics.
    // The writability check is authoritative; permission adjustment is best effort.
    QFile::setPermissions(targetPath, permissions);
    if (!QFileInfo(targetPath).isWritable()) {
        if (error) {
            *error = QStringLiteral("created configuration file is not writable: %1")
                             .arg(targetPath);
        }
        return false;
    }

    return true;
}

/**
 * @brief 确保业务视频目录存在，并复制静态资源中尚未存在的默认媒体。
 *
 * 只补充缺失文件，不覆盖用户已经放入目标目录的同名内容。
 */
QString prepareVideoDirectory(const QString &path)
{
    const QString targetPath = QDir::cleanPath(path);
    QDir().mkpath(targetPath);

    // 安装包自带的默认媒体仍放在 static 下；业务播放目录改为可写存储后，
    // 首次启动把缺失文件复制过去，避免迁移后本地轮播为空。
    const QString seedPath = QDir(Rk3566Platform::staticRoot())
            .filePath(QStringLiteral("demoResources/images/video"));
    if (QDir::cleanPath(seedPath) != targetPath) {
        const QDir seedDir(seedPath);
        if (seedDir.exists()) {
            const QFileInfoList files = seedDir.entryInfoList(QDir::Files | QDir::Readable);
            for (const QFileInfo &file : files) {
                const QString destination = QDir(targetPath).filePath(file.fileName());
                if (!QFileInfo::exists(destination)) {
                    QFile::copy(file.absoluteFilePath(), destination);
                }
            }
        }
    }
    return targetPath;
}

} // namespace

namespace Rk3566Platform {

/** @return 应用根目录；优先使用 QT_YCEST_APP_ROOT。 */
QString applicationRoot()
{
    const QString env = envValue("QT_YCEST_APP_ROOT");
    if (!env.isEmpty()) {
        return QDir::cleanPath(env);
    }
    return QCoreApplication::applicationDirPath();
}

/** @return 可写配置目录，优先系统目录，失败时回退到用户应用数据目录。 */
QString configDir()
{
    static const QString cached = []() {
        const QString env = envValue("QT_YCEST_CONFIG_DIR");
        if (!env.isEmpty()) {
            QDir().mkpath(env);
            return QDir::cleanPath(env);
        }

        const QString systemDir = QStringLiteral("/var/lib/qt_ycest");
        if (ensureDirectory(systemDir)) {
            return systemDir;
        }

        QString userDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (userDir.isEmpty()) {
            userDir = QDir(applicationRoot()).filePath(QStringLiteral("data"));
        }
        QDir().mkpath(userDir);
        return QDir::cleanPath(userDir);
    }();
    return cached;
}

/** @return 固定在可写磁盘目录中的网络配置文件路径。 */
QString netConfigPath()
{
    const QString relativePath =
            QStringLiteral("demoResources/images/logo/net_cfg.ini");
    return QDir(staticRoot()).filePath(relativePath);
}

QString uiConfigPath()
{
    return QDir(staticRoot()).filePath(
            QStringLiteral("demoResources/images/logo/ycest_cfg.ini"));
}

QString faceGateConfigPath()
{
    return QStringLiteral(
            "/home/cat/face_media/modules/facegate/FaceGateQt/config/facegate.ini");
}

bool prepareRuntimeConfigFiles(QString *error)
{
    struct ConfigTemplate {
        QString resourcePath;
        QString targetPath;
    };

    const ConfigTemplate configs[] = {
        {QStringLiteral(":/static/demoResources/images/logo/net_cfg.ini"),
         netConfigPath()},
        {QStringLiteral(":/static/demoResources/images/logo/ycest_cfg.ini"),
         uiConfigPath()},
        {QStringLiteral(":/static/config/facegate.ini"),
         faceGateConfigPath()}
    };

    for (const ConfigTemplate &config : configs) {
        if (!copyConfigTemplateIfMissing(config.resourcePath,
                                         config.targetPath,
                                         error)) {
            return false;
        }
    }
    return true;
}

/** @return 网络配置文件所在目录。 */
QString netConfigDir()
{
    return QFileInfo(netConfigPath()).path();
}

/** @return 设备可移动存储根目录。 */
QString storageRoot()
{
    return QStringLiteral("/mnt/UDISK");
}

/** @return 部署在可移动存储上的静态资源根目录。 */
QString staticRoot()
{
    return QStringLiteral("/mnt/UDISK/res/static");
}

/** @return 可播放视频目录，并在首次使用时补充缺失的默认媒体文件。 */
QString videoDir()
{
    return prepareVideoDirectory(
            QStringLiteral("/mnt/UDISK/res/static/demoResources/images/video"));
}

/** @return SQLite 数据库路径，并确保其父目录存在。 */
QString databasePath()
{
    // IC、二维码、离线人脸和网络人员只允许使用这一份统一门禁数据库。
    const QString path = QStringLiteral("/home/cat/face_media/access_control.db");
    QDir().mkpath(QFileInfo(path).absolutePath());
    return path;
}

/** @return 音量持久化文件路径。 */
QString volumeFile()
{
    return QDir(configDir()).filePath(QStringLiteral("volume.dat"));
}

/** @return IC 探针日志文件路径。 */
QString probeLogFile()
{
    return QDir(configDir()).filePath(QStringLiteral("ic_probe.log"));
}

/** @return 运行心跳文件路径；优先使用 /run，失败时回退到配置目录。 */
QString heartbeatFile()
{
    const QString env = envValue("QT_YCEST_HEARTBEAT_FILE");
    if (!env.isEmpty()) {
        QDir().mkpath(QFileInfo(env).absolutePath());
        return env;
    }

    const QString runDir = QStringLiteral("/run/qt_ycest");
    if (QDir().mkpath(runDir)) {
        return QDir(runDir).filePath(QStringLiteral("heartbeat"));
    }
    return QDir(configDir()).filePath(QStringLiteral("heartbeat"));
}

/** @return 信号板串口节点。 */
QString signalBoardDevice()
{
    return configuredDevice("QT_YCEST_SIGNAL_UART", QStringLiteral("hardware/signal_board_uart"),
                            {QStringLiteral("/dev/ttyS5"), QStringLiteral("/dev/ttyAS5")}, QStringLiteral("/dev/ttyS5"));
}

/** @return 二维码扫描器串口节点。 */
QString qrDevice()
{
    return configuredDevice("QT_YCEST_QR_UART", QStringLiteral("hardware/qr_uart"),
                            {QStringLiteral("/dev/ttyS6"), QStringLiteral("/dev/ttyAS6")}, QStringLiteral("/dev/ttyS6"));
}

/** @return IC 卡读取串口节点。 */
QString cardDevice()
{
    return configuredDevice("QT_YCEST_CARD_UART", QStringLiteral("hardware/card_uart"),
                            {QStringLiteral("/dev/ttyS7"), QStringLiteral("/dev/ttyAS7")}, QStringLiteral("/dev/ttyS7"));
}

/** @return RS485 数据串口节点。 */
QString rs485Device()
{
    return configuredDevice("QT_YCEST_RS485_UART", QStringLiteral("hardware/rs485_uart"),
                            {QStringLiteral("/dev/ttyS8"), QStringLiteral("/dev/ttyAS8")}, QStringLiteral("/dev/ttyS8"));
}

/** @return RTC 设备节点。 */
QString rtcDevice()
{
    return configuredDevice("QT_YCEST_RTC_DEVICE", QStringLiteral("hardware/rtc_device"),
                            {QStringLiteral("/dev/rtc"), QStringLiteral("/dev/rtc0"), QStringLiteral("/dev/rtc1")}, QStringLiteral("/dev/rtc0"));
}

/** @return 物理按键输入节点；仅在显式配置或兼容节点存在时启用。 */
QString keyDevice()
{
    // 输入事件号在不同 RK3566 设备树中并不固定，不能默认占用 event0
    // （event0 很可能是触摸屏）。只有显式配置或兼容节点存在时才启用。
    return configuredDevice("QT_YCEST_KEY_DEVICE", QStringLiteral("hardware/key_device"),
                            {QStringLiteral("/dev/audio_key")}, QString());
}

/** @return 配置的 GStreamer 视频输出插件名称。 */
QString gstVideoSink()
{
    const QString env = envValue("QT_YCEST_GST_VIDEO_SINK");
    return env.isEmpty() ? settingValue(QStringLiteral("gstreamer/video_sink")) : env;
}

/** @return 配置的 GStreamer 音频输出插件名称。 */
QString gstAudioSink()
{
    const QString env = envValue("QT_YCEST_GST_AUDIO_SINK");
    return env.isEmpty() ? settingValue(QStringLiteral("gstreamer/audio_sink")) : env;
}

/** @return ALSA 声卡编号或名称。 */
QString alsaCard()
{
    const QString env = envValue("QT_YCEST_ALSA_CARD");
    return env.isEmpty() ? settingValue(QStringLiteral("audio/card"), QStringLiteral("0")) : env;
}

/** @return ALSA PCM 设备名称。 */
QString alsaDevice()
{
    const QString env = envValue("QT_YCEST_ALSA_DEVICE");
    return env.isEmpty() ? settingValue(QStringLiteral("audio/device"), QStringLiteral("default")) : env;
}

/** @return ALSA 混音器控制项名称。 */
QString alsaMixerControl()
{
    const QString env = envValue("QT_YCEST_ALSA_CONTROL");
    return env.isEmpty() ? settingValue(QStringLiteral("audio/mixer_control")) : env;
}

/** @return 功放控制设备节点或配置值。 */
QString amplifierDevice()
{
    const QString env = envValue("QT_YCEST_AMP_DEVICE");
    return env.isEmpty() ? settingValue(QStringLiteral("audio/amp_device")) : env;
}

/**
 * @return 设备序列号，依次尝试环境变量、设备树、machine-id 和非回环网卡 MAC；
 * 均不可用时返回 "rk3566-unknown"。
 */
QString deviceSerial()
{
    const QString env = envValue("QT_YCEST_DEVICE_SERIAL");
    if (!env.isEmpty()) {
        return env;
    }

    const QStringList serialFiles = {
        QStringLiteral("/sys/firmware/devicetree/base/serial-number"),
        QStringLiteral("/proc/device-tree/serial-number"),
        QStringLiteral("/etc/machine-id")
    };
    for (const QString &path : serialFiles) {
        const QString value = readTextFile(path);
        if (!value.isEmpty()) {
            return value.toLower();
        }
    }

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QString mac = iface.hardwareAddress().remove(':').trimmed();
        if (!mac.isEmpty() && mac != QStringLiteral("000000000000")) {
            return mac.toLower();
        }
    }

    return QStringLiteral("rk3566-unknown");
}

/** @return names 中第一个可执行程序的完整路径；均不可用时返回空字符串。 */
QString findExecutable(const QStringList &names)
{
    for (const QString &name : names) {
        if (name.contains('/')) {
            if (QFileInfo(name).isExecutable()) {
                return name;
            }
            continue;
        }
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return QString();
}

/**
 * @brief 使用 CLOCK_REALTIME 设置系统时间。
 * @param localDateTime 待设置的本地日期时间。
 * @param error 可选错误信息输出。
 * @return 参数有效且系统调用成功时返回 true。
 */
bool setSystemDateTime(const QDateTime &localDateTime, QString *error)
{
    if (!localDateTime.isValid()) {
        if (error) *error = QStringLiteral("datetime_invalid");
        return false;
    }

    timespec ts{};
    ts.tv_sec = localDateTime.toSecsSinceEpoch();
    ts.tv_nsec = 0;
    if (::clock_settime(CLOCK_REALTIME, &ts) != 0) {
        if (error) *error = QString::fromLocal8Bit(strerror(errno));
        return false;
    }
    return true;
}

/**
 * @brief 将当前 UTC 系统时间写入 RTC。
 * @param error 可选错误信息输出。
 * @return RTC 打开和 RTC_SET_TIME 均成功时返回 true。
 */
bool syncRtcFromSystem(QString *error)
{
    const QString rtcPath = rtcDevice();
    const int fd = ::open(QFile::encodeName(rtcPath).constData(), O_RDWR);
    if (fd < 0) {
        if (error) *error = QStringLiteral("open(%1) failed: %2")
                .arg(rtcPath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    const QDateTime utc = QDateTime::currentDateTimeUtc();
    rtc_time rt{};
    rt.tm_year = utc.date().year() - 1900;
    rt.tm_mon = utc.date().month() - 1;
    rt.tm_mday = utc.date().day();
    rt.tm_hour = utc.time().hour();
    rt.tm_min = utc.time().minute();
    rt.tm_sec = utc.time().second();

    const int rc = ::ioctl(fd, RTC_SET_TIME, &rt);
    ::close(fd);
    if (rc < 0) {
        if (error) *error = QStringLiteral("RTC_SET_TIME failed: %1")
                .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    return true;
}

/**
 * @brief 同步文件系统后异步发起系统重启。
 *
 * 优先调用 systemctl reboot，失败时尝试常见 reboot 可执行文件。
 */
bool reboot(QString *error)
{
    ::sync();

    const QString systemctl = findExecutable({QStringLiteral("systemctl")});
    if (!systemctl.isEmpty() && QProcess::startDetached(systemctl, {QStringLiteral("reboot")})) {
        return true;
    }

    const QString rebootBin = findExecutable({QStringLiteral("/sbin/reboot"),
                                               QStringLiteral("/usr/sbin/reboot"),
                                               QStringLiteral("reboot")});
    if (!rebootBin.isEmpty() && QProcess::startDetached(rebootBin, QStringList())) {
        return true;
    }

    if (error) {
        *error = QStringLiteral("systemctl/reboot command unavailable or permission denied");
    }
    return false;
}

} // namespace Rk3566Platform
