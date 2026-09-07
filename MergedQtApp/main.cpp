/**
 * @file main.cpp
 * @brief 应用程序入口及启动环境初始化。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include <unistd.h>
#include <cstdlib>
#include <clocale>
#include <langinfo.h>
#include <QLocale>
#include <QTextCodec>
#include <QProcessEnvironment>
#include <QDebug>
#include <QApplication>
#include <QTimer>
#include <QFile>
#include <QDateTime>
#include <QTextStream>
#include <QSurfaceFormat>
#include <QWidget>
#include <QScreen>
#include <QPalette>
#include <QEventLoop>
#include <QEvent>
#include <QInputMethod>
#include <QRect>

#include "shell/AppShell.h"
#include "components/input/InputCursorController.h"
#include "components/splash/splashscreen.h"
#include "common/sql/dbstore.h"
#include "ic_board/ic_board.h"
#include "platform/rk3566_platform.h"


/** Qt 默认消息处理器的保留句柄；当前实现不继续转发，避免重复输出。 */
static QtMessageHandler g_oldHandler = nullptr;

/**
 * @brief 输入控件获得焦点或被点击后主动唤起 Qt 虚拟键盘。
 *
 * 延迟 80 ms 等待焦点完成切换，且仅处理启用了输入法属性的 QWidget。
 */
class VirtualKeyboardFocusFilter final : public QObject
{
public:
    /** @brief 创建应用级虚拟键盘事件过滤器。 */
    explicit VirtualKeyboardFocusFilter(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

protected:
    /** @brief 在输入控件聚焦或鼠标释放后异步显示输入法面板。 */
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || !widget->testAttribute(Qt::WA_InputMethodEnabled)) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::FocusIn ||
            event->type() == QEvent::MouseButtonRelease) {
            QTimer::singleShot(80, []() {
                if (QGuiApplication::inputMethod()) {
                    QGuiApplication::inputMethod()->show();
                }
            });
        }
        return QObject::eventFilter(watched, event);
    }
};

/** @return 平台参数去除空白并忽略大小写后以 "linuxfb" 开头时返回 true。 */
static bool isLinuxFbPlatformValue(const QByteArray &value)
{
    return value.trimmed().toLower().startsWith(QByteArrayLiteral("linuxfb"));
}

/**
 * @brief 从 QApplication 参数中移除遗留的 linuxfb 平台选项。
 *
 * Qt 命令行参数优先于环境变量；必须在创建 QApplication 之前原地压缩 argv，
 * 才能保证 RK3566 使用 EGLFS。
 */
static void removeLegacyLinuxFbArguments(int &argc, char **argv)
{
    // Qt 的 -platform 命令行参数优先级高于 QT_QPA_PLATFORM。旧的启动脚本、
    // systemd unit 或 Qt Creator 配置若仍携带 -platform linuxfb，仅修改环境变量
    // 无法真正切回 EGLFS，因此在 QApplication 初始化前移除该遗留参数。
    int writeIndex = 1;
    for (int readIndex = 1; readIndex < argc; ++readIndex) {
        const QByteArray argument(argv[readIndex]);
        const QByteArray lower = argument.trimmed().toLower();

        if (lower == QByteArrayLiteral("-platform") && readIndex + 1 < argc &&
            isLinuxFbPlatformValue(QByteArray(argv[readIndex + 1]))) {
            ++readIndex;
            continue;
        }

        if (lower.startsWith(QByteArrayLiteral("-platform=linuxfb"))) {
            continue;
        }

        argv[writeIndex++] = argv[readIndex];
    }

    argc = writeIndex;
    argv[argc] = nullptr;
}

/**
 * @brief 判断一条已知且无业务价值的 Qt 图形/输入警告是否应被抑制。
 *
 * 只过滤明确匹配的 libpng 配置及已拔出 evdev 鼠标告警，其余消息保持原样输出。
 */
static bool shouldDropPngWarning(QtMsgType type, const QString &msg)
{
    // 只过滤 warning（避免误杀别的级别）
    if (type != QtWarningMsg && type != QtCriticalMsg) return false;

    // 1) libpng iCCP 警告
    if (msg.contains("libpng warning: iCCP: known incorrect sRGB profile") ||
        msg.contains("libpng warning: known incorrect sRGB profile")) {
        return true;
    }

    // 2) evdevmouse 设备拔出后刷屏
    // 原始日志是：
    // [E] evdevmouse: Could not read from input device (No such device)
    // Qt 内部通常是 warning 级别输出（不同版本可能前缀不同），所以用 contains 做关键字匹配最稳。
    if (msg.contains("evdevmouse: Could not read from input device") &&
        msg.contains("No such device")) {
        return true;
    }

    return false;
}

/**
 * @brief 将 Qt 日志统一编码为 UTF-8 并直接写入 stderr。
 *
 * Fatal 消息写出后立即终止进程；已知噪声由 shouldDropPngWarning() 提前过滤。
 */
static void myUtf8MessageHandler(QtMsgType type,
                                 const QMessageLogContext &ctx,
                                 const QString &msg)
{
    Q_UNUSED(ctx);

    // 先过滤确认无业务影响的高频告警，避免设备日志被重复信息淹没。
    if (shouldDropPngWarning(type, msg)) {
        return;
    }

    // 直接以 UTF-8 写入 stderr，绕开设备系统 locale 不一致造成的中文乱码。
    const char* t = "";
    switch (type) {
    case QtDebugMsg:    t = "D"; break;
    case QtInfoMsg:     t = "I"; break;
    case QtWarningMsg:  t = "W"; break;
    case QtCriticalMsg: t = "E"; break;
    case QtFatalMsg:    t = "F"; break;
    }

    QByteArray out;
    out.append('['); out.append(t); out.append("] ");
    out.append(msg.toUtf8());
    out.append('\n');
    ::write(2, out.constData(), out.size());

    // 如需同时进入 Qt 默认日志链，可恢复以下转发；当前关闭以避免同一消息输出两次。
    // if (g_oldHandler) g_oldHandler(type, ctx, msg);

    if (type == QtFatalMsg) {
        abort();
    }
}



/**
 * @brief 创建应用存活心跳文件，并每 8 秒以当前 Unix 时间戳覆盖更新。
 *
 * 定时器以 app 为父对象，随应用生命周期自动释放；首次启动时立即写入一次，
 * 避免监控端必须等待第一个定时周期。
 */
static void setupQtHeartbeat(QCoreApplication *app)
{
    const QString heartbeatFile = Rk3566Platform::heartbeatFile();

    auto updateHeartbeat = [heartbeatFile]() {
        QFile file(heartbeatFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream out(&file);
            out << QDateTime::currentSecsSinceEpoch() << "\n";
            file.flush();
            file.close();
        }
    };

    updateHeartbeat();

    QTimer *heartbeatTimer = new QTimer(app);
    QObject::connect(heartbeatTimer, &QTimer::timeout, app, updateHeartbeat);
    heartbeatTimer->start(8000);   // 每 8 秒更新一次
}


/**
 * @brief 融合应用入口。
 *
 * 在 QApplication 创建前完成 EGLFS、OpenGL ES、虚拟键盘和日志环境配置，
 * 随后初始化外壳、启动页、数据库退出清理及 IC 板 I/O。
 */
int main(int argc, char *argv[])
{
    removeLegacyLinuxFbArguments(argc, argv);

    // 尊重标准 Qt 外部平台变量；未指定时，RK3566 正式运行默认 EGLFS。
    // QT_YCEST_QPA_PLATFORM 仅作为兼容旧部署脚本的次级覆盖。
    QByteArray requestedQpa = qgetenv("QT_QPA_PLATFORM").trimmed();
    if (requestedQpa.isEmpty()) {
        requestedQpa = qgetenv("QT_YCEST_QPA_PLATFORM").trimmed();
    }
    if (requestedQpa.toLower().startsWith("linuxfb")) {
        requestedQpa.clear();
    }
    const QByteArray qpa = requestedQpa.isEmpty()
            ? QByteArrayLiteral("eglfs")
            : requestedQpa;
    qputenv("QT_QPA_PLATFORM", qpa);
    qputenv("QT_YCEST_VIDEO_OUTPUT", QByteArrayLiteral("qt"));
    qunsetenv("QT_QPA_FB_DRM");
    qputenv("QT_IM_MODULE", QByteArrayLiteral("qtvirtualkeyboard"));
    qputenv("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE", QByteArrayLiteral("1"));

    if (qpa.toLower().startsWith("eglfs")) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_EGLFS_INTEGRATION")) {
            qputenv("QT_QPA_EGLFS_INTEGRATION", QByteArrayLiteral("eglfs_kms"));
        }
        if (qEnvironmentVariableIsEmpty("QT_QPA_EGLFS_FORCE888")) {
            qputenv("QT_QPA_EGLFS_FORCE888", QByteArrayLiteral("1"));
        }
        if (qEnvironmentVariableIsEmpty("QT_QPA_EGLFS_SWAPINTERVAL")) {
            qputenv("QT_QPA_EGLFS_SWAPINTERVAL", QByteArrayLiteral("1"));
        }

        // RK3566 Linux 4.19 BSP 默认使用 legacy page-flip。需要主动测试
        // atomic 时显式设置 QT_YCEST_EGLFS_ATOMIC=1。
        const QByteArray atomicOverride = qgetenv("QT_YCEST_EGLFS_ATOMIC").trimmed();
        const bool enableAtomic = (atomicOverride == "1" ||
                                   atomicOverride.compare("true", Qt::CaseInsensitive) == 0 ||
                                   atomicOverride.compare("yes", Qt::CaseInsensitive) == 0);
        qputenv("QT_QPA_EGLFS_KMS_ATOMIC",
                enableAtomic ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));

        if (qEnvironmentVariableIsEmpty("QT_OPENGL")) {
            qputenv("QT_OPENGL", QByteArrayLiteral("es2"));
        }

        QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setVersion(2, 0);
        format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        format.setSwapInterval(1);
        QSurfaceFormat::setDefaultFormat(format);
    }

    // 忽略 libpng 的 iCCP 警告，并统一使用 UTF-8 输出。
    g_oldHandler = qInstallMessageHandler(myUtf8MessageHandler);
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    QApplication a(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName(QStringLiteral("MergedQtApp"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QString configPreparationError;
    if (!Rk3566Platform::prepareRuntimeConfigFiles(&configPreparationError)) {
        qCritical() << "[CONFIG] runtime configuration initialization failed:"
                    << configPreparationError;
        return EXIT_FAILURE;
    }
    qInfo() << "[CONFIG] runtime configuration files ready"
            << "net=" << Rk3566Platform::netConfigPath()
            << "ui=" << Rk3566Platform::uiConfigPath()
            << "facegate=" << Rk3566Platform::faceGateConfigPath();
    VirtualKeyboardFocusFilter keyboardFilter(&a);
    a.installEventFilter(&keyboardFilter);
    InputCursorController inputCursorController(&a);
    qInfo() << "[EGLFS-CURSOR] native KMS hardware cursor"
            << "platform=" << QGuiApplication::platformName();
    const QString activePlatform = QGuiApplication::platformName().trimmed().toLower();
    const bool eglfsRequested = qpa.toLower().startsWith("eglfs");
    if (eglfsRequested &&
        !activePlatform.contains(QStringLiteral("eglfs")) &&
        !activePlatform.contains(QStringLiteral("minimalegl"))) {
        qCritical() << "[EGLFS] 平台初始化不一致，期望 eglfs，实际为"
                    << activePlatform
                    << "。请检查启动命令、QT_QPA_PLATFORM 和平台插件路径。";
        return EXIT_FAILURE;
    }

    QObject::connect(&a, &QCoreApplication::aboutToQuit, []() {
        DbStore::checkpoint();
        DbStore::close();
    });

    setupQtHeartbeat(&a);

    // EGLFS 只有一个原生窗口和一个 EGL surface。必须在根窗口首次 show()
    // 之前创建包含 VideoHoleWidget(QOpenGLWidget) 的完整控件树，否则 Qt 5.12
    // 可能在后续引入 OpenGL 子控件时得到全黑画面。
    const QScreen *screen = QGuiApplication::primaryScreen();
    const QRect screenGeometry = screen ? screen->geometry() : QRect(0, 0, 1024, 768);
    AppShell shell;
    shell.setGeometry(screenGeometry);
    if (!shell.initialize()) {
        qCritical() << "[SHELL] initialization failed";
        return EXIT_FAILURE;
    }

    SplashScreen splash(&shell);
    splash.setGeometry(shell.rect());
    splash.show();
    splash.raise();

    QObject::connect(&splash, &SplashScreen::splashFinished, [&]() {
        splash.hide();
        if (QWidget *activeRoot = shell.activeRootWidget()) {
            activeRoot->setGeometry(shell.rect());
            activeRoot->raise();
            activeRoot->setFocus();
            activeRoot->update();
        }
        shell.update();
        qInfo() << "[EGLFS] splash hidden, main view exposed";
    });

    // 完整控件树准备好后，只创建一次 EGLFS/KMS 全屏原生窗口。
    shell.showFullScreen();
    splash.raise();
    a.processEvents(QEventLoop::AllEvents, 100);

    qInfo() << "[EGLFS] root shown"
            << "platform=" << QGuiApplication::platformName()
            << "screen=" << (screen ? screen->name() : QStringLiteral("<none>"))
            << "geometry=" << shell.geometry()
            << "atomic=" << qgetenv("QT_QPA_EGLFS_KMS_ATOMIC")
            << "swapInterval=" << qgetenv("QT_QPA_EGLFS_SWAPINTERVAL")
            << "videoOutput=" << qgetenv("QT_YCEST_VIDEO_OUTPUT");

    IcToast::instance(shell.multimediaRootWidget());
    IcIoBootstrap::startAll(shell.multimediaRootWidget());
    QObject::connect(&a, &QCoreApplication::aboutToQuit,
                     &shell, &AppShell::shutdown);

    return a.exec();
}
