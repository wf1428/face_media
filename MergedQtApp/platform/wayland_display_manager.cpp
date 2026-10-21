#include "wayland_display_manager.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QHash>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QWidget>
#include <QWindow>
#include <QtEndian>
#include <QTimer>
#ifdef QT_YCEST_FIXED_WL_SHELL_OUTPUT
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef QT_YCEST_FIXED_WL_SHELL_OUTPUT
namespace {
// xdg-output v2: only the standard destroy/get_xdg_output requests and output events.
// Keep the wire descriptors local, so the existing Qt plugin and generated symbols do not collide.
const wl_message outputRequests[] = {{"destroy", "", nullptr}};
const wl_message outputEvents[] = {
    {"logical_position", "ii", nullptr}, {"logical_size", "ii", nullptr},
    {"done", "", nullptr}, {"name", "2s", nullptr}, {"description", "2s", nullptr}
};
const wl_interface outputInterface = {
    "zxdg_output_v1", 2, 1, outputRequests, 5, outputEvents
};
const wl_interface *outputRequestTypes[] = {&outputInterface, &wl_output_interface};
const wl_message managerRequests[] = {
    {"destroy", "", nullptr}, {"get_xdg_output", "no", outputRequestTypes}
};
const wl_interface managerInterface = {
    "zxdg_output_manager_v1", 2, 2, managerRequests, 0, nullptr
};
struct OutputListener {
    void (*position)(void *, wl_proxy *, int32_t, int32_t);
    void (*size)(void *, wl_proxy *, int32_t, int32_t);
    void (*done)(void *, wl_proxy *);
    void (*name)(void *, wl_proxy *, const char *);
    void (*description)(void *, wl_proxy *, const char *);
};
void destroyProtocolObject(wl_proxy *proxy)
{
    if (proxy) {
        wl_proxy_marshal(proxy, 0);
        wl_proxy_destroy(proxy);
    }
}
}

class WaylandDisplayManager::Impl
{
public:
    struct Output {
        QPointer<QScreen> screen;
        wl_output *nativeOutput = nullptr; // Borrowed from Qt; never bind, listen to, or destroy it.
        wl_proxy *metadata = nullptr;
        QString name;
        bool complete = false;
    };

    explicit Impl(WaylandDisplayManager *owner)
    {
        if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"))) return;
        native_ = QGuiApplication::platformNativeInterface();
        display_ = native_ ? static_cast<wl_display *>(
                    native_->nativeResourceForIntegration(QByteArrayLiteral("wl_display"))) : nullptr;
        if (!display_) return;
        queue_ = wl_display_create_queue(display_);
        if (!queue_) return;

        // Use a private event queue. Qt remains the only reader of its connection;
        // this class only dispatches already-read events on the GUI thread.
        auto *wrapper = static_cast<wl_proxy *>(wl_proxy_create_wrapper(display_));
        if (!wrapper) return;
        wl_proxy_set_queue(wrapper, queue_);
        registry_ = wl_display_get_registry(reinterpret_cast<wl_display *>(wrapper));
        sync_ = wl_display_sync(reinterpret_cast<wl_display *>(wrapper));
        wl_proxy_wrapper_destroy(wrapper);
        if (!registry_ || !sync_) return;

        static const wl_registry_listener registryListener = {global, globalRemoved};
        static const wl_callback_listener syncListener = {registryDone};
        wl_registry_add_listener(registry_, &registryListener, this);
        wl_callback_add_listener(sync_, &syncListener, this);
        discovering_ = true;
        timer_ = new QTimer(owner);
        timer_->setInterval(20);
        QObject::connect(timer_, &QTimer::timeout, owner, [this]() { pump(); });
        timer_->start();
        flush();
    }

    ~Impl()
    {
        if (timer_) timer_->stop();
        for (Output *output : outputs_) {
            destroyProtocolObject(output->metadata);
            delete output;
        }
        destroyProtocolObject(manager_);
        if (sync_) wl_callback_destroy(sync_);
        if (registry_) wl_registry_destroy(registry_);
        if (queue_) wl_event_queue_destroy(queue_);
    }

    QScreen *findScreen(const QString &name, int waitMs)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        for (;;) {
            pump();
            QScreen *match = nullptr;
            for (Output *output : outputs_) {
                if (!output->screen || !output->complete || output->name != name ||
                    output->screen->geometry().isEmpty()) continue;
                if (match) return nullptr;
                match = output->screen.data();
            }
            if (match) return match;
            if (!authoritative() || waitMs <= 0 || elapsed.elapsed() >= waitMs) return nullptr;

            // Startup only: allow Qt to read Wayland events until metadata arrives, with a deadline.
            QEventLoop loop;
            QTimer wake;
            wake.setSingleShot(true);
            QObject::connect(&wake, &QTimer::timeout, &loop, &QEventLoop::quit);
            wake.start(std::max(1, std::min(20, waitMs - int(elapsed.elapsed()))));
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }

    bool authoritative() const { return discovering_ && (!registryReady_ || manager_); }

private:
    static void global(void *data, wl_registry *registry, uint32_t id,
                       const char *interface, uint32_t version)
    {
        auto *self = static_cast<Impl *>(data);
        if (std::strcmp(interface, managerInterface.name) != 0 || version < 2 || self->manager_) return;
        self->manager_ = static_cast<wl_proxy *>(wl_registry_bind(registry, id, &managerInterface, 2));
        self->managerId_ = id;
    }

    static void globalRemoved(void *data, wl_registry *, uint32_t id)
    {
        auto *self = static_cast<Impl *>(data);
        if (id != self->managerId_) return;
        for (Output *output : self->outputs_) {
            destroyProtocolObject(output->metadata);
            delete output;
        }
        self->outputs_.clear();
        destroyProtocolObject(self->manager_);
        self->manager_ = nullptr;
        self->managerId_ = 0;
    }

    static void registryDone(void *data, wl_callback *callback, uint32_t)
    {
        auto *self = static_cast<Impl *>(data);
        wl_callback_destroy(callback);
        self->sync_ = nullptr;
        self->registryReady_ = true;
        if (!self->manager_) {
            qWarning() << "[DISPLAY] xdg-output names unavailable; using existing screen matching";
        }
    }

    static void position(void *, wl_proxy *, int32_t, int32_t) {}
    static void size(void *, wl_proxy *, int32_t, int32_t) {}
    static void description(void *, wl_proxy *, const char *) {}
    static void name(void *data, wl_proxy *, const char *value)
    {
        static_cast<Output *>(data)->name = QString::fromUtf8(value);
    }
    static void done(void *data, wl_proxy *)
    {
        auto *output = static_cast<Output *>(data);
        output->complete = true;
        if (output->screen && !output->name.isEmpty()) {
            qInfo() << "[DISPLAY] Wayland output name" << output->name
                    << "mapped to" << output->screen->name();
        }
    }

    void syncScreens()
    {
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (auto it = outputs_.begin(); it != outputs_.end();) {
            Output *output = it.value();
            if (!output->screen || !screens.contains(output->screen.data())) {
                destroyProtocolObject(output->metadata);
                delete output;
                it = outputs_.erase(it);
            } else {
                ++it;
            }
        }
        if (!manager_) return;
        static const OutputListener listener = {position, size, done, name, description};
        for (QScreen *screen : screens) {
            if (outputs_.contains(screen)) continue;
            auto *nativeOutput = static_cast<wl_output *>(
                        native_->nativeResourceForScreen(QByteArrayLiteral("output"), screen));
            if (!nativeOutput) continue;
            auto *metadata = wl_proxy_marshal_constructor(manager_, 1, &outputInterface,
                                                          nullptr, nativeOutput);
            if (!metadata) continue;
            auto *output = new Output;
            output->screen = screen;
            output->nativeOutput = nativeOutput;
            output->metadata = metadata;
            wl_proxy_add_listener(metadata,
                                  reinterpret_cast<void (**)(void)>(const_cast<OutputListener *>(&listener)),
                                  output);
            outputs_.insert(screen, output);
        }
    }

    void flush()
    {
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) discovering_ = false;
    }

    void pump()
    {
        if (!discovering_ || pumping_) return;
        pumping_ = true;
        syncScreens();
        if (wl_display_dispatch_queue_pending(display_, queue_) < 0) {
            discovering_ = false;
        } else {
            syncScreens();
            flush();
        }
        pumping_ = false;
    }

    QPlatformNativeInterface *native_ = nullptr;
    wl_display *display_ = nullptr;
    wl_event_queue *queue_ = nullptr;
    wl_registry *registry_ = nullptr;
    wl_callback *sync_ = nullptr;
    wl_proxy *manager_ = nullptr;
    uint32_t managerId_ = 0;
    QTimer *timer_ = nullptr;
    QHash<QScreen *, Output *> outputs_;
    bool discovering_ = false;
    bool registryReady_ = false;
    bool pumping_ = false;
};

#else
class WaylandDisplayManager::Impl {};
#endif

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
QScreen *WaylandDisplayManager::findScreen(const QString &outputName, int waitMs)
{
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (candidate->name() == outputName ||
            candidate->name().endsWith(QStringLiteral("-") + outputName)) {
            return candidate;
        }
    }
#ifdef QT_YCEST_FIXED_WL_SHELL_OUTPUT
    if (impl_) {
        if (QScreen *screen = impl_->findScreen(outputName, waitMs)) {
            return screen;
        }
        // 名称协议可用时以真实输出身份为准，同分辨率不能回退为时序猜测。
        if (impl_->authoritative()) return nullptr;
    }
#else
    Q_UNUSED(waitMs);
#endif
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
    FixedOutputController(QWidget *window, const QString &outputName, QScreen *initialScreen,
                          WaylandDisplayManager *displayManager)
        : QObject(window), window_(window), displayManager_(displayManager),
          outputName_(outputName), target_(initialScreen)
    {
        settleTimer_.setSingleShot(true);
        settleTimer_.setInterval(300);
        connect(&settleTimer_, &QTimer::timeout, this, [this]() { apply(); });
        connect(qApp, &QGuiApplication::screenAdded, this,
                [this](QScreen *) { schedule(); });
        connect(qApp, &QGuiApplication::screenRemoved, this,
                [this](QScreen *screen) {
                    if (target_ == screen) target_.clear();
                    schedule();
                });
        connect(window_->windowHandle(), &QWindow::screenChanged, this,
                [this](QScreen *screen) {
                    qInfo() << "[DISPLAY] server reported screen"
                            << (screen ? screen->name() : QStringLiteral("<none>"));
                    if (target_ && screen != target_) {
                        // 迟到的 HDMI enter 通知只重发带输出的请求，不反复切换窗口状态。
                        schedule();
                    }
                });
        bindGeometry(initialScreen);
        if (initialScreen) lastSourceGeometry_ = initialScreen->geometry();
        // 首次显示已指定源屏；等待热插拔事件及屏幕信息稳定后再核对。
        schedule();
    }

private:
    void schedule()
    {
        if (applying_) {
            return;
        }
        settleTimer_.start();
    }

    void bindGeometry(QScreen *screen)
    {
        QObject::disconnect(geometryConnection_);
        if (screen) {
            geometryConnection_ = connect(screen, &QScreen::geometryChanged, this,
                                          [this](const QRect &) { schedule(); });
        }
    }

    void apply()
    {
        // HDMI 变化不影响已确认且仍有效的 DSI 对象，不反复按分辨率重新识别。
        QScreen *target = target_.data();
        if (!target || !QGuiApplication::screens().contains(target)) {
            target = displayManager_->findScreen(outputName_);
        }
        if (!target) {
            if (!waiting_) qWarning() << "[DISPLAY] waiting for fixed source output" << outputName_;
            waiting_ = true;
            schedule();
            return;
        }
        waiting_ = false;
        const bool targetChanged = target_ != target;
        if (targetChanged) {
            target_ = target;
            bindGeometry(target);
        }
        QWindow *handle = window_->windowHandle();
        if (!handle) {
            return;
        }
        const bool changeScreen = handle->screen() != target;
        const bool geometryChanged = target->geometry() != lastSourceGeometry_;
        const bool needsFullscreen = !window_->isFullScreen();
        if (!targetChanged && !geometryChanged && !changeScreen && !needsFullscreen) {
            return;
        }
        applying_ = true;
        if (changeScreen) {
            handle->setScreen(target);
        }
        if (targetChanged || geometryChanged || changeScreen) {
            window_->setGeometry(target->geometry());
        }
        if (needsFullscreen) {
            window_->showFullScreen();
        }
        // 不经过 showNormal()；仅在目标/几何/窗口状态确实变化后重发原生请求。
        requestFixedWaylandFullscreen(handle, target);
        lastSourceGeometry_ = target->geometry();
        applying_ = false;
        qInfo() << "[DISPLAY] fixed source" << target->name()
                << "geometry=" << target->geometry();
    }

    QWidget *window_;
    WaylandDisplayManager *displayManager_;
    QString outputName_;
    QPointer<QScreen> target_;
    QMetaObject::Connection geometryConnection_;
    QTimer settleTimer_;
    QRect lastSourceGeometry_;
    bool applying_ = false;
    bool waiting_ = false;
};



WaylandDisplayManager::WaylandDisplayManager(QObject *parent)
    : QObject(parent), outputName_(preferredOutputName())
{
#ifdef QT_YCEST_FIXED_WL_SHELL_OUTPUT
    if (!outputName_.isEmpty() &&
        QGuiApplication::platformName().contains(QStringLiteral("wayland"))) {
        impl_.reset(new Impl(this));
    }
#endif
}

WaylandDisplayManager::~WaylandDisplayManager() = default;

QScreen *WaylandDisplayManager::selectStartupScreen()
{
    qInfo() << "[DISPLAY] selector=lvds-source-v4"
            << "Qt=" << qVersion() << "binary=" << QCoreApplication::applicationFilePath()
            << "requested=" << outputName_;
    for (QScreen *candidate : QGuiApplication::screens()) {
        qInfo() << "[DISPLAY] available output" << candidate->name()
                << "geometry=" << candidate->geometry()
                << "physical-mm=" << candidate->physicalSize()
                << "refresh=" << candidate->refreshRate()
                << "make=" << candidate->manufacturer() << "model=" << candidate->model();
    }
    QScreen *screen = outputName_.isEmpty()
            ? QGuiApplication::primaryScreen() : findScreen(outputName_, 2000);
    if (!outputName_.isEmpty() && !screen) {
        for (QScreen *candidate : QGuiApplication::screens()) {
            qCritical() << "[DISPLAY] available output" << candidate->name()
                        << candidate->geometry();
        }
        qCritical() << "[DISPLAY] requested source output is unavailable:" << outputName_;
    }
    return screen;
}

void WaylandDisplayManager::placeWindow(QWidget *window, QScreen *screen)
{
    if (hasFixedOutput()) {
        window->winId();
        window->windowHandle()->setScreen(screen);
    }
}

bool WaylandDisplayManager::trackFullscreenWindow(QWidget *window, QScreen *screen)
{
    if (!hasFixedOutput()) return true;
    if (!requestFixedWaylandFullscreen(window->windowHandle(), screen)) return false;
    new FixedOutputController(window, outputName_, screen, this);
    return true;
}
