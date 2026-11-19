/**
 * @file rk3566_platform.h
 * @brief RK3566 部署路径、设备节点和系统操作的统一适配层。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-17；修改人：Du lin；先规范化媒体绝对路径再按挂载模式映射，补充共享设备身份的读取顺序。
 */

#ifndef RK3566_PLATFORM_H
#define RK3566_PLATFORM_H

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QString>
#include <QStringList>

/**
 * @brief RK3566 部署路径、设备节点和系统操作的统一适配层。
 *
 * 路径与设备配置通常按“环境变量、INI 配置、已存在兼容节点、默认值”的优先级解析，
 * 避免业务模块散布板级常量。
 */
namespace Rk3566Platform {

/** @return 应用根目录；优先使用 QT_YCEST_APP_ROOT。 */
QString applicationRoot();

/**
 * @brief 判断启动脚本是否选择了已挂载的共享 userdata。
 * @return FACE_MEDIA_AB_STORAGE 为 1 时返回 true；未设置或其他值沿用原路径。
 * @note 只读取进程的启动环境，可用于 QApplication 初始化之前。
 */
inline bool usesSharedDataStorage()
{
    return qgetenv("FACE_MEDIA_AB_STORAGE") == "1";
}

/**
 * @brief 在共享存储模式下为业务绝对路径添加 /userdata 前缀。
 * @param[in] path 原路径；相对路径和已经添加前缀的路径可原样传入。
 * @return 共享存储的对应绝对路径，或原路径。
 */
inline QString dataPath(const QString &path)
{
    if (!usesSharedDataStorage() || !path.startsWith(QLatin1Char('/'))) {
        return path;
    }

    const QString prefix = QStringLiteral("/userdata");
    const QString clean = QDir::cleanPath(path);
    if (clean == prefix || clean.startsWith(prefix + QLatin1Char('/'))) {
        return clean;
    }
    return prefix + clean;
}

/**
 * @brief 将原媒体目录内的旧路径映射到当前数据分区，保留用户指定的其他位置。
 * @param[in] path 配置中的媒体路径，可为空或为相对路径。
 * @return 规范化的 /mnt/UDISK 对应路径；其他位置保留传入值。
 */
inline QString storedMediaPath(const QString &path)
{
    const QString root = QStringLiteral("/mnt/UDISK");
    if (!path.startsWith(QLatin1Char('/'))) return path;
    const QString clean = QDir::cleanPath(path);
    const QString sharedRoot = QStringLiteral("/userdata") + root;
    if (clean == sharedRoot || clean.startsWith(sharedRoot + QLatin1Char('/'))) {
        return dataPath(clean.mid(int(sizeof("/userdata") - 1)));
    }
    if (clean == root || clean.startsWith(root + QLatin1Char('/'))) {
        return dataPath(clean);
    }
    return path;
}

/** @return 应用业务数据根目录，可执行文件和内置资源仍位于 applicationRoot()。 */
inline QString applicationDataRoot()
{
    return dataPath(applicationRoot());
}

/**
 * @brief 选择业务容量统计所使用的文件系统路径。
 * @param[in] legacyPath 单分区原来用于容量查询的路径。
 * @return 共享存储模式返回 /userdata，其他情况返回 legacyPath。
 */
inline QString dataVolumePath(const QString &legacyPath)
{
    return usesSharedDataStorage() ? QStringLiteral("/userdata") : legacyPath;
}

/** @return 可写配置目录，优先系统目录，失败时回退到用户应用数据目录。 */
QString configDir();

/** @return 当前业务分区的配置目录；共享模式下由写入方创建目录。 */
inline QString persistentConfigDir()
{
    if (!usesSharedDataStorage()) {
        return configDir();
    }
    const QString env = QString::fromLocal8Bit(qgetenv("QT_YCEST_CONFIG_DIR")).trimmed();
    const QString dir = env.isEmpty() ? QStringLiteral("/var/lib/qt_ycest")
                                     : QDir::cleanPath(QFileInfo(env).absoluteFilePath());
    return dataPath(dir);
}

/** @return 网络配置文件所在目录。 */
QString netConfigDir();

/** @return 固定在可写磁盘目录中的网络配置文件路径。 */
QString netConfigPath();

/** @return UI configuration file path on writable storage. */
QString uiConfigPath();

/** @return FaceGate configuration file path on writable storage. */
QString faceGateConfigPath();

/** Create missing runtime configuration files from embedded templates. */
bool prepareRuntimeConfigFiles(QString *error = nullptr);

/** @return 设备可移动存储根目录。 */
QString storageRoot();

/** @return 部署在可移动存储上的静态资源根目录。 */
QString staticRoot();

/** @return 可播放视频目录，并在首次使用时补充缺失的默认媒体文件。 */
QString videoDir();

/** @return SQLite 数据库路径，并确保其父目录存在。 */
QString databasePath();

/** @return 音量持久化文件路径。 */
QString volumeFile();

/** @return IC 探针日志文件路径。 */
QString probeLogFile();

/** @return 运行心跳文件路径；优先使用 /run，失败时回退到配置目录。 */
QString heartbeatFile();

/** @return 信号板串口节点。 */
QString signalBoardDevice();

/** @return 二维码扫描器串口节点。 */
QString qrDevice();

/** @return IC 卡读取串口节点。 */
QString cardDevice();

/** @return RS485 数据串口节点。 */
QString rs485Device();

/** @return RTC 设备节点。 */
QString rtcDevice();

/** @return 物理按键输入节点；仅在显式配置或兼容节点存在时启用。 */
QString keyDevice();

/** @return 配置的 GStreamer 视频输出插件名称。 */
QString gstVideoSink();

/** @return 配置的 GStreamer 音频输出插件名称。 */
QString gstAudioSink();

/** @return ALSA 声卡编号或名称。 */
QString alsaCard();

/** @return ALSA PCM 设备名称。 */
QString alsaDevice();

/** @return ALSA 混音器控制项名称。 */
QString alsaMixerControl();

/** @return 功放控制设备节点或配置值。 */
QString amplifierDevice();

/**
 * @return 设备序列号，共享模式优先读取已保存的身份，首次按原顺序取值并持久化；
 * 单分区依次读取环境变量、设备树、machine-id 和非回环网卡 MAC；
 * 均不可用时返回 "rk3566-unknown"。
 */
QString deviceSerial();

/** @return names 中第一个可执行程序的完整路径；均不可用时返回空字符串。 */
QString findExecutable(const QStringList &names);

/**
 * @brief 使用 CLOCK_REALTIME 设置系统时间。
 * @param localDateTime 待设置的本地日期时间。
 * @param error 可选错误信息输出。
 * @return 参数有效且系统调用成功时返回 true。
 */
bool setSystemDateTime(const QDateTime &localDateTime, QString *error = nullptr);

/**
 * @brief 将当前 UTC 系统时间写入 RTC。
 * @param error 可选错误信息输出。
 * @return RTC 打开和 RTC_SET_TIME 均成功时返回 true。
 */
bool syncRtcFromSystem(QString *error = nullptr);

/**
 * @brief 同步文件系统后异步发起系统重启。
 *
 * 优先调用 systemctl reboot，失败时尝试常见 reboot 可执行文件。
 */
bool reboot(QString *error = nullptr);

} // namespace Rk3566Platform

#endif // RK3566_PLATFORM_H
