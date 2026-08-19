/**
 * @file rk3566_platform.h
 * @brief RK3566 部署路径、设备节点和系统操作的统一适配层。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef RK3566_PLATFORM_H
#define RK3566_PLATFORM_H

#include <QDateTime>
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

/** @return 可写配置目录，优先系统目录，失败时回退到用户应用数据目录。 */
QString configDir();

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

/** @return RS485 收发方向控制节点；未配置且无兼容节点时返回空字符串。 */
QString rs485DirectionDevice();

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
 * @return 设备序列号，依次尝试环境变量、设备树、machine-id 和非回环网卡 MAC；
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
