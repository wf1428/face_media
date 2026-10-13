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
#include <QDir>
#include <QtEndian>
#include <QSaveFile>
#include <QDateTime>
#include <QTextStream>
#include <QSurfaceFormat>
#include <QWidget>
#include <QScreen>
#include <QWindow>
#include <QPointer>
#include <QPalette>
#include <QEventLoop>
#include <QEvent>
#include <QInputMethod>
#include <QRect>

#ifdef QT_YCEST_FIXED_WL_SHELL_OUTPUT
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client-protocol.h>
#endif

#include "shell/AppShell.h"
#include "modules/web/web_bridge_server.h"
#include "components/input/InputCursorController.h"
#include "components/multimedia/multimediademo.h"
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
        // 多媒体后台中已接入项目自定义键盘的输入框自行管理输入面板。
        // 跳过全局 Qt 虚拟键盘，避免一次点击先后出现两套键盘；人脸模块
        // 未设置该属性，因此保持现有输入逻辑不变。
        if (widget->property("ycestCustomKeyboard").toBool()) {
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
 * @brief 创建应用存活心跳文件，并每 10 秒原子更新一次。
 *
 * 定时器以 app 为父对象，随应用生命周期自动释放；首次启动时立即写入一次，
 * 避免监控端必须等待第一个定时周期。
 */
static void setupQtHeartbeat(QCoreApplication *app)
{
    const QString heartbeatFile = Rk3566Platform::heartbeatFile();
    const qint64 processId = QCoreApplication::applicationPid();

    auto updateHeartbeat = [heartbeatFile, processId, sequence = quint64(0)]() mutable {
        QSaveFile file(heartbeatFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            {
                QTextStream out(&file);
                out << processId << ' '
                    << ++sequence << ' '
                    << QDateTime::currentSecsSinceEpoch() << "\n";
                out.flush();
            }
            file.commit();
        }
    };

    updateHeartbeat();

    QTimer *heartbeatTimer = new QTimer(app);
    QObject::connect(heartbeatTimer, &QTimer::timeout, app, updateHeartbeat);
    heartbeatTimer->start(10000);   // 每 10 秒更新一次
}

/** @return 显式输出名称，或启用 RK3568 LVDS 镜像时的固定源输出。 */
static QString preferredOutputName()
{
    const QString configured = QString::fromUtf8(qgetenv("QT_YCEST_OUTPUT_NAME")).trimmed();
    if (!configured.isEmpty()) {
        return configured;
    }
    if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"))) {
        return QString();
    }
    QFile compatible(QStringLiteral("/proc/device-tree/compatible"));
    if (compatible.open(QIODevice::ReadOnly) &&
        compatible.readAll().split('\0').contains(QByteArrayLiteral("rockchip,rk3568")) &&
        QFile::exists(QStringLiteral("/proc/device-tree/vop@fe040000/rockchip,lvds-hdmi-mirror"))) {
        return QStringLiteral("DSI-1");
    }
    return QString();
}

/** @return 设备树中的单值或 min/typ/max 属性的典型值。 */
static quint32 displayDeviceTreeCell(const QString &path)
{
    QFile property(path);
    if (!property.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray bytes = property.readAll();
    if (bytes.size() != 4 && bytes.size() != 12) {
        return 0;
    }
    const int offset = bytes.size() == 12 ? 4 : 0;
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

/** Qt 旧版 Wayland 可能只提供 Screen17 等名称，用固定面板信息消除歧义。 */
static QScreen *findRk3568PanelScreen()
{
    const QString panelPath = QStringLiteral("/proc/device-tree/dsi@fe060000/panel@0");
    if (!QDir(panelPath).exists()) {
        return nullptr;
    }
    const quint32 panelWidth = displayDeviceTreeCell(panelPath + QStringLiteral("/size,width"));
    const quint32 panelHeight = displayDeviceTreeCell(panelPath + QStringLiteral("/size,height"));
    QList<QScreen *> physicalMatches;
    for (QScreen *candidate : QGuiApplication::screens()) {
        const QSizeF physical = candidate->physicalSize();
        if (panelWidth && panelHeight &&
            ((qAbs(physical.width() - panelWidth) <= 1 &&
              qAbs(physical.height() - panelHeight) <= 1) ||
             (qAbs(physical.width() - panelHeight) <= 1 &&
              qAbs(physical.height() - panelWidth) <= 1))) {
            physicalMatches.append(candidate);
        }
    }
    if (physicalMatches.size() == 1) {
        qInfo() << "[DISPLAY] DSI-1 mapped by panel physical size to"
                << physicalMatches.first()->name();
        return physicalMatches.first();
    }

    // 尺寸未传给 Wayland 或多个屏幕尺寸相同时，再比较面板的原生时序。
    // 不能仅凭排列顺序/相同分辨率猜测，也不能固定使用 Screen18。
    const QDir timings(panelPath + QStringLiteral("/display-timings"));
    const QStringList entries = timings.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const quint32 nativeMode = displayDeviceTreeCell(timings.filePath(QStringLiteral("native-mode")));
    QString timingPath;
    for (const QString &entry : entries) {
        const QString path = timings.filePath(entry);
        if (entries.size() == 1 ||
            (nativeMode &&
             (displayDeviceTreeCell(path + QStringLiteral("/phandle")) == nativeMode ||
              displayDeviceTreeCell(path + QStringLiteral("/linux,phandle")) == nativeMode))) {
            timingPath = path;
            break;
        }
    }
    if (timingPath.isEmpty()) {
        return nullptr;
    }
    const auto cell = [&timingPath](const char *name) {
        return displayDeviceTreeCell(timingPath + QLatin1Char('/') + QString::fromLatin1(name));
    };
    const quint32 width = cell("hactive");
    const quint32 height = cell("vactive");
    const quint64 htotal = quint64(width) + cell("hsync-len") + cell("hback-porch") + cell("hfront-porch");
    const quint64 vtotal = quint64(height) + cell("vsync-len") + cell("vback-porch") + cell("vfront-porch");
    const quint32 clock = cell("clock-frequency");
    if (!width || !height || !clock || !htotal || !vtotal) {
        return nullptr;
    }
    const qreal refresh = qreal(clock) / qreal(htotal * vtotal);
    QList<QScreen *> modeMatches;
    const QList<QScreen *> candidates = physicalMatches.isEmpty()
            ? QGuiApplication::screens() : physicalMatches;
    for (QScreen *candidate : candidates) {
        const QSize pixels = candidate->size() * candidate->devicePixelRatio();
        if (((pixels.width() == int(width) && pixels.height() == int(height)) ||
             (pixels.width() == int(height) && pixels.height() == int(width))) &&
            qAbs(candidate->refreshRate() - refresh) <= 1.0) {
            modeMatches.append(candidate);
        }
    }
    if (modeMatches.size() == 1) {
        qInfo() << "[DISPLAY] DSI-1 mapped by panel timing to" << modeMatches.first()->name()
                << "native mode=" << QSize(int(width), int(height)) << "refresh=" << refresh;
        return modeMatches.first();
    }
    qWarning() << "[DISPLAY] DSI-1 mapping is unavailable or ambiguous;"
                  " set QT_YCEST_OUTPUT_NAME to the verified Qt screen name";
    return nullptr;
}

/** @return 指定连接器对应的 Qt 屏幕，兼容 card 前缀及旧版 Wayland 的名称。 */
static QScreen *findOutputScreen(const QString &outputName)
{
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (candidate->name() == outputName ||
            candidate->name().endsWith(QStringLiteral("-") + outputName)) {
            return candidate;
        }
    }
    if (outputName == QStringLiteral("DSI-1") &&
        QGuiApplication::platformName().contains(QStringLiteral("wayland"))) {
        QFile compatible(QStringLiteral("/proc/device-tree/compatible"));
        if (compatible.open(QIODevice::ReadOnly) &&
            compatible.readAll().split('\0').contains(QByteArrayLiteral("rockchip,rk3568"))) {
            return findRk3568PanelScreen();
        }
    }
    return nullptr;
}

/** Qt 5.12 wl-shell 的 showFullScreen 不指定输出，补发带目标 wl_output 的请求。 */
static bool requestFixedWaylandFullscreen(QWindow *window, QScreen *screen)
{
    if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"))) {
        return true;
    }
#ifdef QT_YCEST_FIXED_WL_SHELL_OUTPUT
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (!native || !window || !screen) {
        qCritical() << "[DISPLAY] native fullscreen request has no window, screen or interface";
        return false;
    }
    // 使用 Qt 同一条 Wayland 连接上的对象；不另建 surface，不改变窗口角色。
    auto *surface = static_cast<wl_shell_surface *>(native->nativeResourceForWindow(
                QByteArrayLiteral("wl_shell_surface"), window));
    auto *output = static_cast<wl_output *>(native->nativeResourceForScreen(
                QByteArrayLiteral("output"), screen));
    if (!surface || !output) {
        qCritical() << "[DISPLAY] fixed fullscreen requires QT_WAYLAND_SHELL_INTEGRATION=wl-shell"
                    << "shell-surface=" << static_cast<void *>(surface)
                    << "output=" << static_cast<void *>(output);
        return false;
    }
    wl_shell_surface_set_fullscreen(surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
                                    0, output);
    // Qt 的正常事件循环会 flush 这条请求，无须阻塞 roundtrip。
    qInfo() << "[DISPLAY] native wl-shell fullscreen requested on" << screen->name();
    return true;
#else
    Q_UNUSED(window);
    Q_UNUSED(screen);
    qCritical() << "[DISPLAY] fixed Wayland fullscreen support was not compiled";
    return false;
#endif
}

/** 保持固定源输出和全屏状态，HDMI 插拔时不将应用迁移到镜像目标。 */
class FixedOutputController final : public QObject
{
public:
    FixedOutputController(QWidget *window, const QString &outputName)
        : QObject(window), window_(window), outputName_(outputName)
    {
        connect(qApp, &QGuiApplication::screenAdded, this,
                [this](QScreen *) { schedule(true); });
        connect(qApp, &QGuiApplication::screenRemoved, this,
                [this](QScreen *) { schedule(true); });
        connect(window_->windowHandle(), &QWindow::screenChanged, this,
                [this](QScreen *screen) {
                    qInfo() << "[DISPLAY] server reported screen"
                            << (screen ? screen->name() : QStringLiteral("<none>"));
                    if (target_ && screen != target_) {
                        // 迟到的 HDMI enter 通知只重发带输出的请求，不反复切换窗口状态。
                        schedule(false);
                    }
                });
        // 首次显示已经指定源屏，不立即 showNormal/showFullScreen 再切换一次。
        schedule(false);
    }

private:
    void schedule(bool reassertFullscreen)
    {
        if (applying_) {
            return;
        }
        reassertFullscreen_ |= reassertFullscreen;
        if (queued_) {
            return;
        }
        queued_ = true;
        QTimer::singleShot(0, this, [this]() {
            queued_ = false;
            const bool reassert = reassertFullscreen_;
            reassertFullscreen_ = false;
            apply(reassert);
        });
    }

    void apply(bool reassertFullscreen)
    {
        QScreen *target = findOutputScreen(outputName_);
        if (!target) {
            qWarning() << "[DISPLAY] waiting for fixed source output" << outputName_;
            return;
        }
        if (target_ != target) {
            QObject::disconnect(geometryConnection_);
            target_ = target;
            geometryConnection_ = connect(target, &QScreen::geometryChanged, this,
                                          [this](const QRect &) { schedule(true); });
        }
        QWindow *handle = window_->windowHandle();
        if (!handle) {
            return;
        }
        const bool changeScreen = handle->screen() != target;
        if (!reassertFullscreen && !changeScreen && window_->isFullScreen()) {
            return;
        }
        applying_ = true;
        // wl-shell 下服务端可能取消全屏而不更新 Qt 的状态；热插拔后主动重发。
        if (reassertFullscreen) {
            window_->showNormal();
        }
        if (changeScreen) {
            handle->setScreen(target);
        }
        window_->setGeometry(target->geometry());
        if (reassertFullscreen || !window_->isFullScreen()) {
            window_->showFullScreen();
        }
        requestFixedWaylandFullscreen(handle, target);
        applying_ = false;
        qInfo() << "[DISPLAY] fixed source" << target->name()
                << "geometry=" << target->geometry();
    }

    QWidget *window_;
    QString outputName_;
    QPointer<QScreen> target_;
    QMetaObject::Connection geometryConnection_;
    bool queued_ = false;
    bool applying_ = false;
    bool reassertFullscreen_ = false;
};


/**
 * @brief 融合应用入口。
 *
 * 在 QApplication 创建前完成 Wayland/OpenGL ES、虚拟键盘和日志环境配置，
 * 随后初始化外壳、启动页、数据库退出清理及 IC 板 I/O。
 */
int main(int argc, char *argv[])
{
    removeLegacyLinuxFbArguments(argc, argv);

    // 尊重标准 Qt 外部平台变量；未指定时，RK3566 正式运行默认 Wayland-EGL。
    // 需要回退验证 EGLFS 时仍可显式设置 QT_QPA_PLATFORM=eglfs。
    // QT_YCEST_QPA_PLATFORM 仅作为兼容旧部署脚本的次级覆盖。
    QByteArray requestedQpa = qgetenv("QT_QPA_PLATFORM").trimmed();
    if (requestedQpa.isEmpty()) {
        requestedQpa = qgetenv("QT_YCEST_QPA_PLATFORM").trimmed();
    }
    if (requestedQpa.toLower().startsWith("linuxfb")) {
        requestedQpa.clear();
    }
    const QByteArray qpa = requestedQpa.isEmpty()
            ? QByteArrayLiteral("wayland-egl")
            : requestedQpa;
    qputenv("QT_QPA_PLATFORM", qpa);
    qputenv("QT_YCEST_VIDEO_OUTPUT", QByteArrayLiteral("qt"));
    qunsetenv("QT_QPA_FB_DRM");
    qputenv("QT_IM_MODULE", QByteArrayLiteral("qtvirtualkeyboard"));
    qputenv("QT_VIRTUALKEYBOARD_DESKTOP_DISABLE", QByteArrayLiteral("1"));

    // 融合程序会同时使用多媒体 QOpenGLWidget 和人脸后台 QQuickWidget。
    // Wayland 与 EGLFS 都应在 QApplication 创建前启用上下文共享，避免动态
    // 打开/关闭密码键盘和管理后台时重复建立互不兼容的 EGL share group。
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

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
    qInfo() << "native hardware cursor"
            << "platform=" << QGuiApplication::platformName();
    const QString activePlatform = QGuiApplication::platformName().trimmed().toLower();
    const bool eglfsRequested = qpa.toLower().startsWith("eglfs");
    const bool waylandRequested = qpa.toLower().startsWith("wayland");
    if (eglfsRequested &&
        !activePlatform.contains(QStringLiteral("eglfs")) &&
        !activePlatform.contains(QStringLiteral("minimalegl"))) {
        qCritical() << "平台初始化不一致，期望"
                    << qpa << "，实际为" << activePlatform
                    << "。请检查启动命令、QT_QPA_PLATFORM 和平台插件路径。";
        return EXIT_FAILURE;
    }
    if (waylandRequested && !activePlatform.contains(QStringLiteral("wayland"))) {
        qCritical() << "平台初始化不一致，期望"
                    << qpa << "，实际为" << activePlatform
                    << "。请检查 Weston、WAYLAND_DISPLAY 和平台插件路径。";
        return EXIT_FAILURE;
    }

    QObject::connect(&a, &QCoreApplication::aboutToQuit, []() {
        DbStore::checkpoint();
        DbStore::close();
    });

    setupQtHeartbeat(&a);

    // 根窗口首次 show() 前创建完整控件树。EGLFS 回退模式需要唯一 EGL
    // surface，Wayland 下也可避免启动阶段重复创建、销毁子 surface。
    const QString outputName = preferredOutputName();
    qInfo() << "[DISPLAY] selector=lvds-source-v3"
            << "Qt=" << qVersion() << "binary=" << QCoreApplication::applicationFilePath()
            << "requested=" << outputName;
    for (QScreen *candidate : QGuiApplication::screens()) {
        qInfo() << "[DISPLAY] available output" << candidate->name()
                << "geometry=" << candidate->geometry()
                << "physical-mm=" << candidate->physicalSize()
                << "refresh=" << candidate->refreshRate()
                << "make=" << candidate->manufacturer() << "model=" << candidate->model();
    }
    QScreen *screen = outputName.isEmpty()
            ? QGuiApplication::primaryScreen() : findOutputScreen(outputName);
    if (!outputName.isEmpty() && !screen) {
        for (QScreen *candidate : QGuiApplication::screens()) {
            qCritical() << "[DISPLAY] available output" << candidate->name()
                        << candidate->geometry();
        }
        qCritical() << "[DISPLAY] requested source output is unavailable:" << outputName;
        return EXIT_FAILURE;
    }
    const QRect screenGeometry = screen ? screen->geometry() : QRect(0, 0, 1024, 768);
    AppShell shell;
    if (!outputName.isEmpty()) {
        shell.winId();
        shell.windowHandle()->setScreen(screen);
    }
    shell.setGeometry(screenGeometry);
    if (!shell.initialize()) {
        qCritical() << "[SHELL] initialization failed";
        return EXIT_FAILURE;
    }

    // 新增 Web IPC 有独立工作线程；未启用或启动失败不影响现有 Qt 流程。
    WebRuntime webRuntime;

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

        // 让主界面先完成一次显示，再放行 GStreamer。本次 singleShot 的接收者
        // 归属控件树，应用退出时会自动取消尚未执行的回调。
        QWidget *multimediaRoot = shell.multimediaRootWidget();
        MultimediaDemo *multimedia = multimediaRoot
                ? multimediaRoot->findChild<MultimediaDemo *>()
                : nullptr;
        if (multimedia) {
            QTimer::singleShot(0, multimedia,
                               &MultimediaDemo::startPlaybackAfterSplash);
        } else {
            qWarning() << "splash finished but multimedia page was not found";
        }
        qInfo() << "splash hidden, main view exposed";
    });

    // 完整控件树准备好后，只创建一个由 Weston 管理的全屏顶层窗口。
    shell.showFullScreen();
    if (!outputName.isEmpty()) {
        if (!requestFixedWaylandFullscreen(shell.windowHandle(), screen)) {
            return EXIT_FAILURE;
        }
        new FixedOutputController(&shell, outputName);
    }
    splash.raise();
    a.processEvents(QEventLoop::AllEvents, 100);

    const QScreen *shownScreen = shell.windowHandle() ? shell.windowHandle()->screen() : nullptr;
    qInfo() << "root shown"
            << "platform=" << QGuiApplication::platformName()
            << "screen=" << (shownScreen ? shownScreen->name() : QStringLiteral("<none>"))
            << "geometry=" << shell.geometry()
            << "videoOutput=" << qgetenv("QT_YCEST_VIDEO_OUTPUT");

    IcToast::instance(shell.multimediaRootWidget());
    IcIoBootstrap::startAll(shell.multimediaRootWidget());
    QObject::connect(&a, &QCoreApplication::aboutToQuit,
                     &shell, &AppShell::shutdown);

    return a.exec();
}
