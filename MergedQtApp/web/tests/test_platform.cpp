/**
 * @file test_platform.cpp
 * @brief 测试目标专用平台路径替身；所有旧仓储读写都重定向到 QTemporaryDir。
 * @author Du lin
 * @date 2026-10-09
 */

#include "platform/rk3566_platform.h"
#include "components/multimedia/gst_video_validation.h"
#include <QDir>
#include <QFile>
#include "components/cursoroverlay/keyboard_dialog.h"
#include "components/dialogs/ui_message_box.h"

namespace Rk3566Platform {

/**
 * @brief 取得测试进程指定的隔离根目录。
 * @return 测试进程初始化的临时根，不使用生产默认路径。
 */
QString applicationRoot()
{
    return QString::fromLocal8Bit(qgetenv("WEB_TEST_ROOT"));
}

/**
 * @brief 构造隔离测试的 SQLite 数据库路径。
 * @return 临时统一 SQLite。
 */
QString databasePath()
{
    return QDir(applicationRoot()).filePath("access_control.db");
}

/**
 * @brief 构造隔离测试的 static 资源目录。
 * @return 临时静态文件根。
 */
QString staticRoot()
{
    return QDir(applicationRoot()).filePath("static");
}

/**
 * @brief 隔离测试使用临时目录中的同一网络 INI。
 * @return 隔离测试的临时配置目录。
 */
QString netConfigDir()
{
    return applicationRoot();
}
/**
 * @brief 构造隔离测试使用的网络 INI 路径。
 * @return 临时 net_cfg.ini 完整路径。
 */
QString netConfigPath() { return QDir(applicationRoot()).filePath("net_cfg.ini"); }
/**
 * @brief 将测试视频目录重定向到临时静态资源根。
 * @return 临时 demoResources/images/video 目录。
 */
QString videoDir() { return QDir(staticRoot()).filePath("demoResources/images/video"); }

/**
 * @brief 所有后台页配置路径仍指向隔离测试目录。
 * @return 隔离测试的临时配置目录。
 */
QString configDir()
{
    return applicationRoot();
}

/**
 * @brief 隔离测试不运行 ip、DHCP 等系统程序。
 * @return 空字符串，确保测试不启动实际系统网络命令。
 */
QString findExecutable(const QStringList &)
{
    return {};
}
} // namespace Rk3566Platform

/** @brief 网络页链接替身；测试不弹出真实屏幕键盘。 */
QString KeyboardDialog::getText(
    QWidget *, const QString &, const QString &initial, QLineEdit::EchoMode, int, bool *accepted)
{
    if (accepted)
        *accepted = false;
    return initial;
}

namespace UiMessageBox {
/** @brief 隔离测试不打开模态设备消息框。 */
QMessageBox::StandardButton information(QWidget *,
                                        const QString &,
                                        const QString &,
                                        QMessageBox::StandardButtons,
                                        QMessageBox::StandardButton)
{
    return QMessageBox::Ok;
}

/** @brief 隔离测试不打开警告框。 */
QMessageBox::StandardButton warning(QWidget *,
                                    const QString &,
                                    const QString &,
                                    QMessageBox::StandardButtons,
                                    QMessageBox::StandardButton)
{
    return QMessageBox::Ok;
}

/** @brief 隔离测试不打开错误框。 */
QMessageBox::StandardButton critical(QWidget *,
                                     const QString &,
                                     const QString &,
                                     QMessageBox::StandardButtons,
                                     QMessageBox::StandardButton)
{
    return QMessageBox::Ok;
}
} // namespace UiMessageBox

/**
 * @brief 隔离测试不链接播放器；视频内容检查须由板端真实 GStreamer 验证。
 * @param path 待访问的文件或配置路径。
 * @return 媒体探测状态及失败说明；测试替身只接受固定夹具字节。
 */
GstVideoValidationResult validateVideoWithGStreamer(const QString &path, int)
{
    // 固定测试字节用于验证发布/自动播放的连接，生产仍使用真实 GStreamer 校验。
    QFile fixture(path);
    if (fixture.open(QIODevice::ReadOnly) && fixture.readAll() == "web-test-video-valid")
        return {GstVideoValidationStatus::Valid, {}};
    return {GstVideoValidationStatus::Unavailable, QStringLiteral("隔离测试未加载 GStreamer")};
}
