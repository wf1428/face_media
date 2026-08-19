/**
 * @file main.cpp
 * @brief 应用程序入口及启动环境初始化。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QByteArray>
#include <QEvent>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QInputMethod>
#include <QObject>
#include <QTimer>
#include <QWidget>

#include "AppConfig.h"
#include "MainWindow.h"

namespace {

/**
 * @brief 人脸门禁独立程序使用的虚拟键盘焦点过滤器。
 *
 * 输入控件完成焦点切换后再显示输入法，避免直接在焦点事件内重入 Qt 输入法。
 */
class VirtualKeyboardFocusFilter : public QObject {
public:
    /** @brief 创建应用级输入法事件过滤器。 */
    explicit VirtualKeyboardFocusFilter(QObject *parent = nullptr) : QObject(parent) {}

protected:
    /** @brief 在支持输入法的控件聚焦或点击后异步显示虚拟键盘。 */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || !widget->testAttribute(Qt::WA_InputMethodEnabled)) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonRelease) {
            QTimer::singleShot(80, []() {
                if (QGuiApplication::inputMethod()) {
                    QGuiApplication::inputMethod()->show();
                }
            });
        }

        return QObject::eventFilter(watched, event);
    }
};

}

/** @brief 初始化运行环境并启动 Qt 应用事件循环。 */
int main(int argc, char *argv[])
{
    // 平台和 OpenGL 后端完全由外部环境选择。此入口不再强制 XCB，
    // 因此既可独立调试，也不会污染融合程序的 EGLFS 正式运行路径。
    /*
     * 使用 Qt Virtual Keyboard 作为输入法。目标系统需要
     * 在 Qt 插件路径中安装 qtvirtualkeyboard inputcontext 插件。
     * 下面的焦点过滤器会为 Qt Widgets 输入框主动弹出键盘。键盘内嵌在 AdminPanel 中，因此禁用桌面顶层键盘窗口。
     */
    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));
    qputenv("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE", QByteArray("1"));

    // 创建 QApplication 后安装输入法焦点过滤器。
    QApplication app(argc, argv);
    app.setProperty("appStartMsecsSinceEpoch", QDateTime::currentMSecsSinceEpoch());
    VirtualKeyboardFocusFilter keyboardFilter(&app);
    app.installEventFilter(&keyboardFilter);
    /*
     * AdminPanel 全屏显示时 MainWindow 会被隐藏。如果 Qt 保持
     * 默认“最后一个窗口关闭即退出”的行为，关闭 AdminPanel 会导致应用退出，
     * 从而来不及重新显示 MainWindow。
     */
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName("FaceGateQt");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setOrganizationName("Dulin");

    QCommandLineParser parser;
    parser.setApplicationDescription("嵌入式 Qt 人脸闸机识别系统");
    parser.addHelpOption();
    parser.addOption({{"c", "config"}, "ini 配置文件路径", "config"});
    parser.process(app);

    const QString configPath = parser.value("config").trimmed();
    const QString defaultConfigPath = QCoreApplication::applicationDirPath() + "/facegate.ini";
    AppConfig config = AppConfig::load(configPath.isEmpty() ? defaultConfigPath : configPath);

    FaceGateMainWindow window(config);
    window.showFullScreen();

    return app.exec();
}
