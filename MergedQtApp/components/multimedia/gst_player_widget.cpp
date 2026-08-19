/**
 * @file gst_player_widget.cpp
 * @brief 实现 GStreamer 播放管线、三槽帧邮箱和 Qt OpenGL 视频画布。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "gst_player_widget.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QOpenGLContext>
#include <QSharedPointer>
#include <QThread>
#include <QMetaObject>
#include <QPalette>
#include <QStringList>
#include <QMutexLocker>
#include <QUrl>

#include <gst/video/videooverlay.h>
#include <gst/allocators/gstdmabuf.h>

#include "RgaImageProcessor.h"
#include "platform/rk3566_platform.h"

// Qt 5.12 的 qopengl.h 在部分 GLES2 工具链中不会暴露 GLES3 纹理常量，
// 但运行时 Mali 可能实际返回 OpenGL ES 3.x 上下文。这里补齐常量，
// 使同一份代码能够在 GLES2 与 GLES3 上选择合法的纹理格式。
#ifndef GL_RED
#define GL_RED 0x1903  /* GLES3 单通道纹理外部格式。 */
#endif
#ifndef GL_RG
#define GL_RG 0x8227   /* GLES3 双通道纹理外部格式。 */
#endif
#ifndef GL_R8
#define GL_R8 0x8229   /* GLES3 8 位单通道纹理内部格式。 */
#endif
#ifndef GL_RG8
#define GL_RG8 0x822B  /* GLES3 8 位双通道纹理内部格式。 */
#endif

namespace {

/**
 * @brief 监听视频宿主控件几何和可见性变化的内部事件过滤器。
 *
 * 事件只触发区域更新请求，实际 overlay/画布调整由播放器在事件循环中合并执行。
 */
class VideoWidgetEventFilter final : public QObject
{
public:
    /** @brief 绑定需要接收显示区域更新请求的播放器。 */
    explicit VideoWidgetEventFilter(GstPlayerWidget *owner)
        : QObject(owner), owner_(owner)
    {
    }

protected:
    /** @brief 将影响视频区域的窗口事件转换为一次节流更新请求。 */
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (!owner_) return false;
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::ParentChange:
        case QEvent::ZOrderChange:
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
            owner_->requestDisplayRectUpdate();
            break;
        default:
            break;
        }
        return false;
    }

private:
    GstPlayerWidget *owner_ = nullptr;
};

/** @brief 以进程级单次初始化方式启动 GStreamer，并返回初始化失败原因。 */
bool initGStreamer(QString *error)
{
    static std::once_flag once;
    static bool ok = false;
    static QString initError;

    std::call_once(once, []() {
        GError *gstError = nullptr;
        ok = gst_init_check(nullptr, nullptr, &gstError);
        if (!ok && gstError) {
            initError = QString::fromUtf8(gstError->message);
            g_error_free(gstError);
        }

        if (!ok) return;

        // Rockchip BSP 常见的硬件解码器工厂名。提高 rank 后 playbin/uridecodebin
        // 会优先选择 MPP 解码，插件不存在时不会影响软件解码回退。
        const char *features[] = {
            "mppvideodec", "mpph264dec", "mpph265dec", "rkvideodec"
        };
        GstRegistry *registry = gst_registry_get();
        for (const char *name : features) {
            GstPluginFeature *feature = gst_registry_find_feature(registry, name, GST_TYPE_ELEMENT_FACTORY);
            if (feature) {
                gst_plugin_feature_set_rank(feature, GST_RANK_PRIMARY + 100);
                gst_object_unref(feature);
            }
        }
    });

    if (!ok && error) {
        *error = initError.isEmpty() ? QStringLiteral("gst_init_check failed") : initError;
    }
    return ok;
}

/** @brief 检查 GObject 是否公开指定属性，避免向不兼容插件写入属性。 */
bool hasProperty(GObject *object, const char *name)
{
    return object && g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
}

/** @brief 仅在插件公开目标属性时写入布尔配置。 */
void setBooleanProperty(GObject *object, const char *name, gboolean value)
{
    if (hasProperty(object, name)) {
        g_object_set(object, name, value, nullptr);
    }
}

/** @brief 仅在插件公开目标属性时写入整数配置。 */
void setIntegerProperty(GObject *object, const char *name, gint value)
{
    if (hasProperty(object, name)) {
        g_object_set(object, name, value, nullptr);
    }
}

/** @brief 读取 GStreamer 元素的工厂名称，供插件兼容判断和日志使用。 */
const char *elementFactoryName(GstElement *element)
{
    if (!element) return nullptr;
    GstElementFactory *factory = gst_element_get_factory(element);
    return factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;
}

/** @brief 构造 H.264 字节流且按访问单元对齐的媒体能力描述。 */
GstCaps *h264ByteStreamAuCaps()
{
    return gst_caps_new_simple("video/x-h264",
                               "stream-format", G_TYPE_STRING, "byte-stream",
                               "alignment", G_TYPE_STRING, "au",
                               nullptr);
}


/** @brief 把颜色分量限制在 8 位无符号范围。 */
inline int clampByte(int value)
{
    return std::max(0, std::min(255, value));
}

/** @brief 按视频限幅范围把一个 YUV 像素转换为 RGB。 */
inline QRgb yuvToRgb(int y, int u, int v)
{
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;
    return qRgb(clampByte(r), clampByte(g), clampByte(b));
}

/** @brief 保持宽高比计算 NV12 目标尺寸，并向下对齐为偶数。 */
QSize fittedEvenNv12Size(int sourceWidth,
                         int sourceHeight,
                         int targetBoxWidth,
                         int targetBoxHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        targetBoxWidth <= 1 || targetBoxHeight <= 1) {
        return QSize();
    }

    int width = targetBoxWidth;
    int height = static_cast<int>((static_cast<qint64>(width) * sourceHeight +
                                   sourceWidth / 2) / sourceWidth);
    if (height > targetBoxHeight) {
        height = targetBoxHeight;
        width = static_cast<int>((static_cast<qint64>(height) * sourceWidth +
                                  sourceHeight / 2) / sourceHeight);
    }

    // NV12 的宽高必须为偶数。向下对齐，保证目标图像不超出 Qt 视频区域。
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) return QSize();
    return QSize(width, height);
}

/** @brief 按优先级创建第一个可用的 GStreamer 插件元素。 */
GstElement *makeFirstAvailable(const QStringList &factories)
{
    for (const QString &factory : factories) {
        const QByteArray name = factory.trimmed().toUtf8();
        if (name.isEmpty()) continue;
        GstElement *element = gst_element_factory_make(name.constData(), nullptr);
        if (element) {
            qInfo() << "[GStreamer] selected element:" << factory;
            return element;
        }
    }
    return nullptr;
}

class SharedAudioMixer final
{
public:
    static SharedAudioMixer &instance()
    {
        static SharedAudioMixer mixer;
        return mixer;
    }

    GstFlowReturn pushSample(GstPlayerWidget *owner, GstSample *sample)
    {
        if (!owner || !sample) return GST_FLOW_ERROR;

        QMutexLocker locker(&mutex_);
        QString error;
        SourceBranch *branch = ensureSourceLocked(owner, &error);
        if (!branch) {
            qWarning() << "[GStreamer-AUDIO] shared mixer input unavailable:" << error;
            return GST_FLOW_ERROR;
        }

        GstCaps *caps = gst_sample_get_caps(sample);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (!caps || !buffer) return GST_FLOW_ERROR;

        if (!branch->caps || !gst_caps_is_equal(branch->caps, caps)) {
            if (branch->caps) gst_caps_unref(branch->caps);
            branch->caps = gst_caps_copy(caps);
            g_object_set(branch->appsrc, "caps", caps, nullptr);
        }

        GstBuffer *copy = gst_buffer_copy(buffer);
        if (!copy) return GST_FLOW_ERROR;
        GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_OFFSET(copy) = GST_BUFFER_OFFSET_NONE;
        GST_BUFFER_OFFSET_END(copy) = GST_BUFFER_OFFSET_NONE;

        GstFlowReturn flow = GST_FLOW_ERROR;
        g_signal_emit_by_name(branch->appsrc, "push-buffer", copy, &flow);
        gst_buffer_unref(copy);
        return flow;
    }

    void removeSource(GstPlayerWidget *owner)
    {
        if (!owner) return;
        QMutexLocker locker(&mutex_);
        SourceBranch *branch = sources_.take(owner);
        if (!branch) return;
        destroySourceLocked(branch);

        if (sources_.isEmpty() && pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
        }
    }

private:
    struct SourceBranch {
        GstElement *appsrc = nullptr;
        GstElement *queue = nullptr;
        GstElement *convert = nullptr;
        GstElement *resample = nullptr;
        GstPad *sourcePad = nullptr;
        GstPad *mixerPad = nullptr;
        GstCaps *caps = nullptr;
    };

    SharedAudioMixer() = default;
    ~SharedAudioMixer()
    {
        QMutexLocker locker(&mutex_);
        while (!sources_.isEmpty()) {
            auto it = sources_.begin();
            SourceBranch *branch = it.value();
            sources_.erase(it);
            destroySourceLocked(branch);
        }
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
        }
        if (bus_) {
            gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (pipeline_) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        mixer_ = nullptr;
        outputSink_ = nullptr;
    }

    SharedAudioMixer(const SharedAudioMixer &) = delete;
    SharedAudioMixer &operator=(const SharedAudioMixer &) = delete;

    static GstBusSyncReply busSyncHandler(GstBus *, GstMessage *message, gpointer)
    {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            qWarning() << "[GStreamer-AUDIO] shared mixer error:"
                       << (error ? QString::fromUtf8(error->message)
                                 : QStringLiteral("unknown error"))
                       << (debug ? QString::fromUtf8(debug) : QString());
            if (error) g_error_free(error);
            if (debug) g_free(debug);
        }
        return GST_BUS_DROP;
    }

    bool ensurePipelineLocked(QString *error)
    {
        if (pipeline_) return true;

        QString initError;
        if (!initGStreamer(&initError)) {
            if (error) *error = initError;
            return false;
        }

        GstElement *pipeline = gst_pipeline_new("qt-ycest-shared-audio");
        GstElement *mixer = gst_element_factory_make("audiomixer", "qt-ycest-audio-mixer");
        GstElement *convert = gst_element_factory_make("audioconvert", "qt-ycest-audio-convert");
        GstElement *resample = gst_element_factory_make("audioresample", "qt-ycest-audio-resample");

        QStringList candidates;
        const QString configured = Rk3566Platform::gstAudioSink();
        if (!configured.isEmpty()) candidates << configured;
        candidates << QStringLiteral("alsasink") << QStringLiteral("autoaudiosink");
        GstElement *sink = makeFirstAvailable(candidates);

        if (!pipeline || !mixer || !convert || !resample || !sink) {
            if (error) *error = QStringLiteral("missing audiomixer/audio conversion/output element");
            if (pipeline) gst_object_unref(pipeline);
            if (mixer) gst_object_unref(mixer);
            if (convert) gst_object_unref(convert);
            if (resample) gst_object_unref(resample);
            if (sink) gst_object_unref(sink);
            return false;
        }

        setBooleanProperty(G_OBJECT(mixer), "ignore-inactive-pads", TRUE);
        setBooleanProperty(G_OBJECT(sink), "sync", TRUE);
        setBooleanProperty(G_OBJECT(sink), "async", TRUE);
        if (hasProperty(G_OBJECT(sink), "device")) {
            const QByteArray device = Rk3566Platform::alsaDevice().toUtf8();
            if (!device.isEmpty()) g_object_set(sink, "device", device.constData(), nullptr);
        }

        gst_bin_add_many(GST_BIN(pipeline), mixer, convert, resample, sink, nullptr);
        if (!gst_element_link_many(mixer, convert, resample, sink, nullptr)) {
            if (error) *error = QStringLiteral("cannot link shared audio output pipeline");
            gst_object_unref(pipeline);
            return false;
        }

        pipeline_ = pipeline;
        mixer_ = mixer;
        outputSink_ = sink;
        bus_ = gst_element_get_bus(pipeline_);
        if (bus_) {
            gst_bus_set_sync_handler(bus_, &SharedAudioMixer::busSyncHandler, this, nullptr);
        }
        qInfo() << "[GStreamer-AUDIO] shared mixer initialized; one process-wide output sink";
        return true;
    }

    SourceBranch *ensureSourceLocked(GstPlayerWidget *owner, QString *error)
    {
        auto existing = sources_.constFind(owner);
        if (existing != sources_.constEnd()) return existing.value();
        if (!ensurePipelineLocked(error)) return nullptr;

        SourceBranch *branch = new SourceBranch;
        branch->appsrc = gst_element_factory_make("appsrc", nullptr);
        branch->queue = gst_element_factory_make("queue", nullptr);
        branch->convert = gst_element_factory_make("audioconvert", nullptr);
        branch->resample = gst_element_factory_make("audioresample", nullptr);
        if (!branch->appsrc || !branch->queue || !branch->convert || !branch->resample) {
            if (error) *error = QStringLiteral("cannot create shared audio input branch");
            if (branch->appsrc) gst_object_unref(branch->appsrc);
            if (branch->queue) gst_object_unref(branch->queue);
            if (branch->convert) gst_object_unref(branch->convert);
            if (branch->resample) gst_object_unref(branch->resample);
            delete branch;
            return nullptr;
        }

        g_object_set(branch->appsrc,
                     "is-live", TRUE,
                     "format", GST_FORMAT_TIME,
                     "do-timestamp", TRUE,
                     "block", FALSE,
                     nullptr);
        g_object_set(branch->queue,
                     "leaky", 2,
                     "max-size-buffers", static_cast<guint>(32),
                     nullptr);

        gst_bin_add_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                         branch->convert, branch->resample, nullptr);
        if (!gst_element_link_many(branch->appsrc, branch->queue,
                                   branch->convert, branch->resample, nullptr)) {
            if (error) *error = QStringLiteral("cannot link shared audio input branch");
            gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                branch->convert, branch->resample, nullptr);
            delete branch;
            return nullptr;
        }

        branch->sourcePad = gst_element_get_static_pad(branch->resample, "src");
        branch->mixerPad = gst_element_get_request_pad(mixer_, "sink_%u");
        if (!branch->sourcePad || !branch->mixerPad ||
            gst_pad_link(branch->sourcePad, branch->mixerPad) != GST_PAD_LINK_OK) {
            if (error) *error = QStringLiteral("cannot attach input branch to audiomixer");
            if (branch->mixerPad) {
                gst_element_release_request_pad(mixer_, branch->mixerPad);
                gst_object_unref(branch->mixerPad);
            }
            if (branch->sourcePad) gst_object_unref(branch->sourcePad);
            gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                branch->convert, branch->resample, nullptr);
            delete branch;
            return nullptr;
        }

        sources_.insert(owner, branch);
        gst_element_sync_state_with_parent(branch->appsrc);
        gst_element_sync_state_with_parent(branch->queue);
        gst_element_sync_state_with_parent(branch->convert);
        gst_element_sync_state_with_parent(branch->resample);
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            sources_.remove(owner);
            destroySourceLocked(branch);
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            if (error) *error = QStringLiteral("cannot start shared audio mixer");
            return nullptr;
        }

        qInfo() << "[GStreamer-AUDIO] shared input attached sources=" << sources_.size();
        return branch;
    }

    void destroySourceLocked(SourceBranch *branch)
    {
        if (!branch) return;
        if (branch->appsrc) gst_element_set_state(branch->appsrc, GST_STATE_NULL);
        if (branch->queue) gst_element_set_state(branch->queue, GST_STATE_NULL);
        if (branch->convert) gst_element_set_state(branch->convert, GST_STATE_NULL);
        if (branch->resample) gst_element_set_state(branch->resample, GST_STATE_NULL);

        if (branch->sourcePad && branch->mixerPad) {
            gst_pad_unlink(branch->sourcePad, branch->mixerPad);
        }
        if (branch->mixerPad && mixer_) {
            gst_element_release_request_pad(mixer_, branch->mixerPad);
        }
        if (branch->sourcePad) gst_object_unref(branch->sourcePad);
        if (branch->mixerPad) gst_object_unref(branch->mixerPad);
        if (branch->caps) gst_caps_unref(branch->caps);

        if (pipeline_ && branch->appsrc && branch->queue &&
            branch->convert && branch->resample) {
            gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                branch->convert, branch->resample, nullptr);
        }
        delete branch;
    }

    QMutex mutex_;
    QHash<GstPlayerWidget *, SourceBranch *> sources_;
    GstElement *pipeline_ = nullptr;
    GstElement *mixer_ = nullptr;
    GstElement *outputSink_ = nullptr;
    GstBus *bus_ = nullptr;
};

bool setNullStateAndWait(GstElement *element)
{
    if (!element) return true;
    if (gst_element_set_state(element, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    const GstStateChangeReturn waitResult =
            gst_element_get_state(element, nullptr, nullptr, 2 * GST_SECOND);
    return waitResult != GST_STATE_CHANGE_FAILURE && waitResult != GST_STATE_CHANGE_ASYNC;
}

} // namespace

/** @brief 创建 NV12 OpenGL 画布和渲染诊断定时器。 */
VideoHoleWidget::VideoHoleWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("videoWidget"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setCursor(Qt::BlankCursor);

    connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (lastSwapMs_ > 0) {
            maxSwapGapMs_ = qMax(maxSwapGapMs_, nowMs - lastSwapMs_);
        }
        lastSwapMs_ = nowMs;
        ++swappedFrames_;
    });
}

/** @brief 在有效上下文中释放 GL 资源和待消费帧。 */
VideoHoleWidget::~VideoHoleWidget()
{
    releasePendingFrameLease();
    if (context() && context()->isValid()) {
        makeCurrent();
        releaseGlResources();
        doneCurrent();
    }
}

/** @brief 提交一帧 NV12 数据，并在画布不再引用该帧时调用释放回调。 */
void VideoHoleWidget::setNv12Frame(int width,
                                   int height,
                                   const QByteArray &nv12Frame,
                                   const std::function<void()> &frameConsumedCallback)
{
    if (QThread::currentThread() != thread()) {
        qWarning() << "[GStreamer] 忽略非GUI线程提交的视频帧";
        if (frameConsumedCallback) frameConsumedCallback();
        return;
    }

    const qint64 requiredBytes = static_cast<qint64>(width) * height * 3 / 2;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        requiredBytes <= 0 || nv12Frame.size() < requiredBytes) {
        qWarning() << "[EGLFS-GL] 忽略无效的RGA NV12输出"
                   << "frame=" << QSize(width, height)
                   << "bytes=" << nv12Frame.size()
                   << "required=" << requiredBytes;
        if (frameConsumedCallback) frameConsumedCallback();
        return;
    }

    // 正常情况下同一时间只有一个待上传帧。若上一次帧尚未上传便被新帧替换，
    // 先归还旧邮箱槽，避免生产线程永久失去该槽。
    releasePendingFrameLease();

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastSubmitMs_ > 0) {
        maxSubmitGapMs_ = qMax(maxSubmitGapMs_, nowMs - lastSubmitMs_);
    }
    lastSubmitMs_ = nowMs;
    ++submittedFrames_;

    frameWidth_ = width;
    frameHeight_ = height;
    nv12Frame_ = nv12Frame;
    frameConsumedCallback_ = frameConsumedCallback;
    textureUploadPending_ = true;

    if (!firstFrameLogged_) {
        firstFrameLogged_ = true;
        qInfo() << "[GStreamer] first RGA-scaled NV12 frame delivered to Qt canvas:"
                << width << "x" << height
                << "bytes=" << nv12Frame_.size()
                << "canvas=" << size()
                << "visible=" << isVisible();
    }
    update();
}

/** @brief 清除待显示帧并释放关联的帧租约。 */
void VideoHoleWidget::clearVideoFrame()
{
    releasePendingFrameLease();
    frameWidth_ = 0;
    frameHeight_ = 0;
    textureUploadPending_ = false;
    update();
}

/** @return 当前已提交视频帧的 RGB 快照。 */
QImage VideoHoleWidget::videoSnapshot()
{
    return grabFramebuffer();
}

/** @brief 初始化着色器、纹理和顶点缓冲。 */
void VideoHoleWidget::initializeGL()
{
    initializeOpenGLFunctions();
    textureWidth_ = 0;
    textureHeight_ = 0;
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    const QOpenGLContext *currentContext = QOpenGLContext::currentContext();
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    const QString versionText = version
            ? QString::fromLatin1(reinterpret_cast<const char *>(version))
            : QString();
    const int contextMajor = currentContext ? currentContext->format().majorVersion() : 0;

    // GLES3 已移除 GL_LUMINANCE/GL_LUMINANCE_ALPHA。Mali 即使收到 2.0
    // 请求，也可能创建 ES 3.2 上下文，因此必须按实际上下文选择 R/RG 格式。
    useRedGreenTextures_ = contextMajor >= 3 ||
            versionText.startsWith(QStringLiteral("OpenGL ES 3"));

    glGenTextures(1, &yTexture_);
    glGenTextures(1, &uvTexture_);

    // GLES3 不允许使用客户端内存顶点数组。使用 VBO 后同时兼容 GLES2/3。
    static const GLfloat quadVertices[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f
    };
    glGenBuffers(1, &vertexBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const bool shaderOk = ensureShaderProgram();
    const GLenum initError = glGetError();
    qInfo() << "[EGLFS-GL] VideoHoleWidget initializeGL"
            << "size=" << size()
            << "contextValid=" << (currentContext && currentContext->isValid())
            << "contextMajor=" << contextMajor
            << "shader=" << shaderOk
            << "textureLayout=" << (useRedGreenTextures_ ? "R8/RG8" : "LUMINANCE/LA")
            << "vbo=" << vertexBuffer_
            << "glError=" << QStringLiteral("0x%1").arg(initError, 0, 16)
            << "renderer=" << (renderer ? reinterpret_cast<const char *>(renderer) : "<null>")
            << "version=" << (version ? reinterpret_cast<const char *>(version) : "<null>");
}

/** @brief 延迟创建并校验 NV12 转换着色器。 */
bool VideoHoleWidget::ensureShaderProgram()
{
    if (program_) return true;

    QScopedPointer<QOpenGLShaderProgram> program(new QOpenGLShaderProgram);
    static const char *vertexShader =
            "attribute highp vec2 vertexIn;\n"
            "attribute highp vec2 textureIn;\n"
            "varying highp vec2 textureOut;\n"
            "void main(void)\n"
            "{\n"
            "    gl_Position = vec4(vertexIn, 0.0, 1.0);\n"
            "    textureOut = textureIn;\n"
            "}\n";

    static const char *fragmentShader =
            "precision mediump float;\n"
            "varying highp vec2 textureOut;\n"
            "uniform sampler2D texY;\n"
            "uniform sampler2D texUV;\n"
            "uniform highp float uvUsesRg;\n"
            "void main(void)\n"
            "{\n"
            "    float y = texture2D(texY, textureOut).r;\n"
            "    vec4 uvSample = texture2D(texUV, textureOut);\n"
            "    vec2 uv = mix(uvSample.ra, uvSample.rg, vec2(uvUsesRg)) - vec2(0.5, 0.5);\n"
            "    y = 1.16438356 * (y - 0.0625);\n"
            "    float r = y + 1.59602678 * uv.y;\n"
            "    float g = y - 0.39176229 * uv.x - 0.81296764 * uv.y;\n"
            "    float b = y + 2.01723214 * uv.x;\n"
            "    gl_FragColor = vec4(r, g, b, 1.0);\n"
            "}\n";

    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
        !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader) ||
        !program->link()) {
        qWarning() << "[GStreamer] NV12 OpenGL着色器创建失败:" << program->log();
        return false;
    }

    program_.swap(program);
    return true;
}

/** @brief 上传最新 NV12 帧并完成 YUV 到 RGB 绘制。 */
void VideoHoleWidget::paintGL()
{
    QElapsedTimer paintTimer;
    paintTimer.start();

    const qint64 paintStartMs = QDateTime::currentMSecsSinceEpoch();
    if (lastPaintMs_ > 0) {
        maxPaintGapMs_ = qMax(maxPaintGapMs_, paintStartMs - lastPaintMs_);
    }
    lastPaintMs_ = paintStartMs;
    ++paintedFrames_;

    const qreal dpr = devicePixelRatioF();
    const int surfaceWidth = qMax(1, qRound(width() * dpr));
    const int surfaceHeight = qMax(1, qRound(height() * dpr));

    // 清掉 Qt 合成前遗留的 GL 错误，后面的错误日志才能准确指向本次视频绘制。
    while (glGetError() != GL_NO_ERROR) {}

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glViewport(0, 0, surfaceWidth, surfaceHeight);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!firstPaintLogged_) {
        firstPaintLogged_ = true;
        qInfo() << "[EGLFS-GL] VideoHoleWidget first paintGL"
                << "widget=" << size()
                << "surface=" << QSize(surfaceWidth, surfaceHeight)
                << "frame=" << QSize(frameWidth_, frameHeight_)
                << "defaultFbo=" << defaultFramebufferObject();
    }

    if (frameWidth_ <= 0 || frameHeight_ <= 0 ||
        !ensureShaderProgram() || yTexture_ == 0 || uvTexture_ == 0 ||
        vertexBuffer_ == 0) {
        if (textureUploadPending_) {
            qWarning() << "[EGLFS-GL] 视频帧待上传但GL资源无效，跳过该帧"
                       << "frame=" << QSize(frameWidth_, frameHeight_);
            textureUploadPending_ = false;
            releasePendingFrameLease();
        }
        return;
    }

    if (textureUploadPending_ && nv12Frame_.isEmpty()) {
        qWarning() << "[EGLFS-GL] 视频帧邮箱数据为空，跳过该帧"
                   << "frame=" << QSize(frameWidth_, frameHeight_);
        textureUploadPending_ = false;
        releasePendingFrameLease();
        return;
    }

    const GLint yInternalFormat = useRedGreenTextures_ ? GL_R8 : GL_LUMINANCE;
    const GLenum yUploadFormat = useRedGreenTextures_ ? GL_RED : GL_LUMINANCE;
    const GLint uvInternalFormat = useRedGreenTextures_ ? GL_RG8 : GL_LUMINANCE_ALPHA;
    const GLenum uvUploadFormat = useRedGreenTextures_ ? GL_RG : GL_LUMINANCE_ALPHA;

    if (textureUploadPending_) {
        QElapsedTimer uploadTimer;
        uploadTimer.start();
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const bool sizeChanged = textureWidth_ != frameWidth_ ||
                                 textureHeight_ != frameHeight_;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, yTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (sizeChanged) {
            glTexImage2D(GL_TEXTURE_2D, 0, yInternalFormat,
                         frameWidth_, frameHeight_, 0,
                         yUploadFormat, GL_UNSIGNED_BYTE, nullptr);
        }
        const uchar *nv12Data = reinterpret_cast<const uchar *>(nv12Frame_.constData());
        const uchar *yData = nv12Data;
        const uchar *uvData = nv12Data + static_cast<qint64>(frameWidth_) * frameHeight_;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frameWidth_, frameHeight_,
                        yUploadFormat, GL_UNSIGNED_BYTE, yData);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uvTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const int uvWidth = frameWidth_ / 2;
        const int uvHeight = (frameHeight_ + 1) / 2;
        if (sizeChanged) {
            glTexImage2D(GL_TEXTURE_2D, 0, uvInternalFormat,
                         uvWidth, uvHeight, 0,
                         uvUploadFormat, GL_UNSIGNED_BYTE, nullptr);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        uvWidth, uvHeight,
                        uvUploadFormat, GL_UNSIGNED_BYTE, uvData);

        const GLenum uploadError = glGetError();
        if (uploadError != GL_NO_ERROR) {
            qWarning() << "[EGLFS-GL] NV12纹理上传失败"
                       << "glError=" << QStringLiteral("0x%1").arg(uploadError, 0, 16)
                       << "layout=" << (useRedGreenTextures_ ? "R8/RG8" : "LUMINANCE/LA")
                       << "frame=" << QSize(frameWidth_, frameHeight_);
            textureUploadPending_ = false;
            releasePendingFrameLease();
            return;
        }

        textureWidth_ = frameWidth_;
        textureHeight_ = frameHeight_;
        textureUploadPending_ = false;
        ++uploadedFrames_;
        maxUploadUs_ = qMax(maxUploadUs_, uploadTimer.nsecsElapsed() / 1000);

        // glTexSubImage2D 返回后，CPU 侧 NV12 数据已不再被本次绘制使用。
        // 立即归还 Reading 槽，让 RGA 线程继续复用预分配缓冲区。
        releasePendingFrameLease();
    }

    int renderWidth = surfaceWidth;
    int renderHeight = static_cast<int>((static_cast<qint64>(renderWidth) * frameHeight_ +
                                         frameWidth_ / 2) / frameWidth_);
    if (renderHeight > surfaceHeight) {
        renderHeight = surfaceHeight;
        renderWidth = static_cast<int>((static_cast<qint64>(renderHeight) * frameWidth_ +
                                        frameHeight_ / 2) / frameHeight_);
    }
    renderWidth = qMax(1, renderWidth);
    renderHeight = qMax(1, renderHeight);
    const int x = (surfaceWidth - renderWidth) / 2;
    const int y = (surfaceHeight - renderHeight) / 2;
    glViewport(x, y, renderWidth, renderHeight);

    // Qt 在 QOpenGLWidget 合成过程中可能改变纹理绑定，因此每次绘制前都显式绑定。
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uvTexture_);

    program_->bind();
    program_->setUniformValue("texY", 0);
    program_->setUniformValue("texUV", 1);
    program_->setUniformValue("uvUsesRg", useRedGreenTextures_ ? 1.0f : 0.0f);

    const int vertexLocation = program_->attributeLocation("vertexIn");
    const int textureLocation = program_->attributeLocation("textureIn");
    if (vertexLocation < 0 || textureLocation < 0) {
        qWarning() << "[EGLFS-GL] NV12着色器属性位置无效"
                   << vertexLocation << textureLocation;
        program_->release();
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    program_->enableAttributeArray(vertexLocation);
    program_->enableAttributeArray(textureLocation);
    program_->setAttributeBuffer(vertexLocation, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    program_->setAttributeBuffer(textureLocation, GL_FLOAT, 2 * sizeof(GLfloat),
                                 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_->disableAttributeArray(vertexLocation);
    program_->disableAttributeArray(textureLocation);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    program_->release();

    const GLenum drawError = glGetError();
    if (!firstRenderedFrameLogged_ || drawError != GL_NO_ERROR) {
        firstRenderedFrameLogged_ = true;
        qInfo() << "[EGLFS-GL] NV12 frame draw"
                << "frame=" << QSize(frameWidth_, frameHeight_)
                << "viewport=" << QRect(x, y, renderWidth, renderHeight)
                << "layout=" << (useRedGreenTextures_ ? "R8/RG8" : "LUMINANCE/LA")
                << "glError=" << QStringLiteral("0x%1").arg(drawError, 0, 16);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    maxPaintUs_ = qMax(maxPaintUs_, paintTimer.nsecsElapsed() / 1000);
}

/** @brief 执行当前帧消费回调，允许邮箱槽位重新使用。 */
void VideoHoleWidget::releasePendingFrameLease()
{
    std::function<void()> callback = std::move(frameConsumedCallback_);
    frameConsumedCallback_ = std::function<void()>();
    nv12Frame_.clear();
    if (callback) callback();
}

/** @brief 在有效 OpenGL 上下文中释放纹理、缓冲和着色器。 */
void VideoHoleWidget::releaseGlResources()
{
    if (yTexture_) {
        glDeleteTextures(1, &yTexture_);
        yTexture_ = 0;
    }
    if (uvTexture_) {
        glDeleteTextures(1, &uvTexture_);
        uvTexture_ = 0;
    }
    if (vertexBuffer_) {
        glDeleteBuffers(1, &vertexBuffer_);
        vertexBuffer_ = 0;
    }
    textureWidth_ = 0;
    textureHeight_ = 0;
    program_.reset();
}

/** @brief 创建管线轮询、位置和诊断定时器。 */
GstPlayerWidget::GstPlayerWidget(QWidget *parent)
    : QWidget(parent)
{
    busTimer_.setInterval(20);
    connect(&busTimer_, &QTimer::timeout, this, &GstPlayerWidget::pollBus);

    positionTimer_.setInterval(200);
    connect(&positionTimer_, &QTimer::timeout, this, &GstPlayerWidget::pollPosition);

    QString error;
    if (!initGStreamer(&error)) {
        QTimer::singleShot(0, this, [this, error]() { emit errorOccured(error); });
    }
}

/** @brief 阻止新回调后停止并释放 GStreamer 管线。 */
GstPlayerWidget::~GstPlayerWidget()
{
    shuttingDown_.store(true);
    setVideoOutput(nullptr);
    releasePipeline();
}

/** @brief 指定承载视频画布或硬件 overlay 的目标控件。 */
void GstPlayerWidget::setVideoOutput(QWidget *widget)
{
    if (VideoHoleWidget *canvas = videoCanvas()) {
        canvas->clearVideoFrame();
    }
    guiDeliveryEventPending_.storeRelease(0);
    resetFrameMailbox();

    if (videoWidget_ && videoEventFilter_) {
        videoWidget_->removeEventFilter(videoEventFilter_);
    }
    delete videoEventFilter_;
    videoEventFilter_ = nullptr;

    if (videoSurface_) {
        videoSurface_->hide();
        videoSurface_->deleteLater();
        videoSurface_ = nullptr;
    }

    videoWidget_ = nullptr;
    windowId_.store(0);
    overlayWidth_.store(0);
    overlayHeight_.store(0);

    if (preloadMode_ || !widget) {
        return;
    }

    videoWidget_ = widget;
    if (configuredDisplayRect_.isValid() && videoWidget_->parentWidget()) {
        videoWidget_->setGeometry(configuredDisplayRect_);
    }

    if (useQtCompositedVideo()) {
        // EGLFS 只有一个 Qt 原生窗口。视频画布保持为普通 Qt 子控件，
        // 由 QOpenGLWidget 合成，不创建独立的 X11/KMS 视频窗口。
        videoWidget_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        videoWidget_->setCursor(Qt::BlankCursor);
        videoWidget_->show();
    } else {
        // 仅保留桌面/X11调试兼容路径。
        videoWidget_->setAttribute(Qt::WA_NativeWindow, true);
        ensureVideoSurface();
    }

    videoEventFilter_ = new VideoWidgetEventFilter(this);
    videoWidget_->installEventFilter(videoEventFilter_);
    requestDisplayRectUpdate();
}

/** @brief 设置本地路径或网络 URL，并使旧异步帧失效。 */
void GstPlayerWidget::setMedia(const QString &path)
{
    mediaPath_ = path.trimmed();
    hlsAuAlignmentEnabled_.storeRelease(
                mediaPath_.toLower().contains(QStringLiteral(".m3u8")) ? 1 : 0);
    mediaGeneration_.fetchAndAddOrdered(1);
    clearVideoCanvas();
    sourceWidth_ = 0;
    sourceHeight_ = 0;
    sourceSizeKnown_.storeRelease(0);
    requestDisplayRectUpdate();
    lastVideoFrameMs_.storeRelease(0);
    lastAudioFrameMs_.storeRelease(0);
    lastVideoSignalMs_.storeRelease(0);
    lastAudioSignalMs_.storeRelease(0);

    ensurePipeline();
    if (!playbin_) return;

    if (!setNullStateAndWait(playbin_)) {
        emit errorOccured(QStringLiteral("GStreamer 切换到 NULL 失败，无法安全切换媒体"));
        return;
    }
    SharedAudioMixer::instance().removeSource(this);
    if (bus_) {
        // 丢弃上一媒体源残留的 EOS/ERROR/STATE_CHANGED，避免切源后误触发。
        gst_bus_set_flushing(bus_, TRUE);
        gst_bus_set_flushing(bus_, FALSE);
    }
    prepared_ = false;
    playing_ = false;
    playRequested_ = false;

    const QString uri = mediaUri(mediaPath_);
    if (uri.isEmpty()) {
        emit errorOccured(QStringLiteral("媒体路径为空或无效"));
        return;
    }

    const QByteArray encoded = uri.toUtf8();
    g_object_set(playbin_, "uri", encoded.constData(), nullptr);
    setVolume(volume01_);

    const GstStateChangeReturn result = gst_element_set_state(playbin_, GST_STATE_PAUSED);
    if (result == GST_STATE_CHANGE_FAILURE) {
        emit errorOccured(QStringLiteral("GStreamer 切换到 PAUSED 失败: %1").arg(uri));
        return;
    }

    busTimer_.start();
    if (!preloadMode_) {
        positionTimer_.start();
    }
}

/** @brief 请求播放；预加载尚未完成时会在 prepared 后继续。 */
void GstPlayerWidget::play()
{
    ensurePipeline();
    if (!playbin_) return;

    playRequested_ = true;
    const GstStateChangeReturn result = gst_element_set_state(playbin_, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE) {
        emit errorOccured(QStringLiteral("GStreamer 切换到 PLAYING 失败"));
        return;
    }
    busTimer_.start();
    positionTimer_.start();
}

/** @brief 暂停当前管线。 */
void GstPlayerWidget::pause()
{
    if (!playbin_) return;
    playRequested_ = false;
    gst_element_set_state(playbin_, GST_STATE_PAUSED);
    playing_ = false;
}

/** @brief 停止播放并清除画布中的最后一帧。 */
void GstPlayerWidget::stop()
{
    playRequested_ = false;
    prepared_ = false;
    playing_ = false;
    positionTimer_.stop();
    if (playbin_) {
        if (!setNullStateAndWait(playbin_)) {
            qWarning() << "[GStreamer] stop: switching playbin to NULL did not complete";
        }
    }
    SharedAudioMixer::instance().removeSource(this);

    mediaGeneration_.fetchAndAddOrdered(1);
    clearVideoCanvas();
    lastVideoFrameMs_.storeRelease(0);
    lastAudioFrameMs_.storeRelease(0);
    lastVideoSignalMs_.storeRelease(0);
    lastAudioSignalMs_.storeRelease(0);
}

/** @brief 跳转到指定毫秒位置。 */
void GstPlayerWidget::seekMs(int ms)
{
    if (!playbin_ || ms < 0) return;
    gst_element_seek_simple(playbin_, GST_FORMAT_TIME,
                            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                            static_cast<gint64>(ms) * GST_MSECOND);
}

/** @brief 设置归一化音量，取值范围为 0.0～1.0。 */
void GstPlayerWidget::setVolume(float value01)
{
    volume01_ = std::max(0.0f, std::min(1.0f, value01));
    if (!playbin_) return;
    g_object_set(playbin_, "volume", static_cast<gdouble>(volume01_),
                 "mute", volume01_ <= 0.0001f, nullptr);
}

/** @brief 设置仅准备不自动播放的预加载模式。 */
void GstPlayerWidget::setPreloadMode(bool on)
{
    if (preloadMode_ == on) return;
    if (on) {
        setVideoOutput(nullptr);
    }
    preloadMode_ = on;
    releasePipeline();
}

/** @brief 设置配置文件指定的播放区域；rect 使用目标控件父窗口坐标。 */
void GstPlayerWidget::setDisplayRect(const QRect &rect)
{
    if (rect.width() <= 0 || rect.height() <= 0) {
        qWarning() << "[GStreamer] 忽略无效视频区域:" << rect;
        return;
    }

    configuredDisplayRect_ = rect;
    overlayWidth_.store(rect.width());
    overlayHeight_.store(rect.height());

    if (videoWidget_) {
        if (videoWidget_->parentWidget()) {
            videoWidget_->setGeometry(configuredDisplayRect_);
        } else {
            videoWidget_->resize(configuredDisplayRect_.size());
        }
    }

    requestDisplayRectUpdate();
}

/** @brief 异步请求重新计算硬件 overlay 或 Qt 画布区域。 */
void GstPlayerWidget::requestDisplayRectUpdate()
{
    if (preloadMode_) return;
    if (videoWidget_) {
        overlayWidth_.store(videoWidget_->width());
        overlayHeight_.store(videoWidget_->height());
    }
    QTimer::singleShot(0, this, &GstPlayerWidget::updateVideoOverlay);
}

/** @brief 记录源视频尺寸，用于保持宽高比。 */
void GstPlayerWidget::setSourceVideoSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (sourceWidth_ == width && sourceHeight_ == height &&
        sourceSizeKnown_.loadAcquire() != 0) {
        return;
    }

    sourceWidth_ = width;
    sourceHeight_ = height;
    sourceSizeKnown_.storeRelease(1);

    qInfo() << "[GStreamer] 源视频尺寸:" << width << "x" << height;
    requestDisplayRectUpdate();
}

/** @brief 请求从最新解码样本生成一张快照。 */
void GstPlayerWidget::requestSnapshot()
{
    QString error;
    QImage image;

    if (useQtCompositedVideo()) {
        if (VideoHoleWidget *canvas = videoCanvas()) {
            image = canvas->videoSnapshot();
        }
        if (image.isNull()) {
            error = QStringLiteral("Qt/EGLFS 视频画布尚无可截图内容");
        }
    } else if (videoSink_) {
        image = imageFromLastSample(&error);
    } else {
        error = QStringLiteral("GStreamer 视频 sink 不可用");
    }

    if (image.isNull() && videoWidget_ && videoWidget_->isVisible()) {
        image = videoWidget_->grab().toImage();
    }

    if (image.isNull()) {
        emit snapshotFailed(error.isEmpty() ? QStringLiteral("无法获取视频帧") : error);
        return;
    }
    emit snapshotReady(image);
}

/** @brief 显示时刷新视频输出区域。 */
void GstPlayerWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    requestDisplayRectUpdate();
}

/** @brief 尺寸变化时刷新视频输出区域。 */
void GstPlayerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    requestDisplayRectUpdate();
}

/** @brief 位置变化时刷新硬件 overlay 坐标。 */
void GstPlayerWidget::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    requestDisplayRectUpdate();
}

/** @brief 延迟创建 playbin、sink、总线和探针。 */
void GstPlayerWidget::ensurePipeline()
{
    if (playbin_) return;

    QString error;
    if (!initGStreamer(&error)) {
        emit errorOccured(error);
        return;
    }

    playbin_ = gst_element_factory_make("playbin", nullptr);
    if (!playbin_) {
        emit errorOccured(QStringLiteral("缺少 GStreamer playbin 插件"));
        return;
    }

    videoSink_ = createVideoSink();
    audioSink_ = createAudioSink();
    if (!videoSink_ || !audioSink_) {
        emit errorOccured(QStringLiteral("无法创建 GStreamer 音视频 sink"));
        releasePipeline();
        return;
    }

    g_object_set(playbin_, "video-sink", videoSink_, "audio-sink", audioSink_, nullptr);
    g_signal_connect(playbin_, "source-setup", G_CALLBACK(&GstPlayerWidget::sourceSetup), this);
    g_signal_connect(playbin_, "element-setup", G_CALLBACK(&GstPlayerWidget::elementSetup), this);

    bus_ = gst_element_get_bus(playbin_);
    gst_bus_set_sync_handler(bus_, &GstPlayerWidget::busSyncHandler, this, nullptr);
    attachFrameProbes();
    setVolume(volume01_);
}

/** @brief 停止并释放全部 GStreamer 对象和帧邮箱。 */
void GstPlayerWidget::releasePipeline()
{
    busTimer_.stop();
    positionTimer_.stop();

    if (playbin_) {
        if (!setNullStateAndWait(playbin_)) {
            qWarning() << "[GStreamer] release: switching playbin to NULL did not complete";
        }
    }
    SharedAudioMixer::instance().removeSource(this);

    removeFrameProbes();

    if (bus_) {
        gst_bus_set_sync_handler(bus_, nullptr, nullptr, nullptr);
        gst_object_unref(bus_);
        bus_ = nullptr;
    }

    {
        QMutexLocker locker(&gstMutex_);
        if (overlayElement_) {
            gst_object_unref(overlayElement_);
            overlayElement_ = nullptr;
        }
    }

    if (playbin_) {
        gst_object_unref(playbin_);
        playbin_ = nullptr;
    }

    // videoSink_/audioSink_ 由 playbin 持有。
    videoSink_ = nullptr;
    audioSink_ = nullptr;
    appSink_ = nullptr;
    guiDeliveryEventPending_.storeRelease(0);
    mediaGeneration_.fetchAndAddOrdered(1);
    prepared_ = false;
    playing_ = false;
    clearVideoCanvas();
}

/** @brief 根据平台和配置选择视频 sink。 */
GstElement *GstPlayerWidget::createVideoSink()
{
    if (preloadMode_) {
        GstElement *sink = gst_element_factory_make("fakesink", nullptr);
        configureSink(sink, true);
        return sink;
    }

    if (useQtCompositedVideo()) {
        return createQtCompositedVideoSink();
    }

    QStringList candidates;
    const QString configured = Rk3566Platform::gstVideoSink();
    if (!configured.isEmpty()) candidates << configured;

    const QString platform = QGuiApplication::platformName().toLower();
    if (platform.contains(QStringLiteral("xcb"))) {
        candidates << QStringLiteral("rkximagesink")
                   << QStringLiteral("glimagesink")
                   << QStringLiteral("ximagesink")
                   << QStringLiteral("xvimagesink");
    } else if (platform.contains(QStringLiteral("wayland"))) {
        candidates << QStringLiteral("waylandsink")
                   << QStringLiteral("glimagesink");
    } else {
        candidates << QStringLiteral("autovideosink");
    }

    GstElement *sink = makeFirstAvailable(candidates);
    configureSink(sink, true);
    return sink;
}

/** @brief 创建 appsink，使解码帧进入 Qt 合成路径。 */
GstElement *GstPlayerWidget::createQtCompositedVideoSink()
{
    if (!QFileInfo::exists(QStringLiteral("/dev/rga"))) {
        qCritical() << "[RGA] /dev/rga不存在，终止EGLFS视频链路；禁止CPU缩放回退";
        return nullptr;
    }

    GstElement *bin = gst_bin_new("qt-ycest-video-bin");
    GstElement *queue = gst_element_factory_make("queue", "qt-ycest-video-queue");
    GstElement *capsFilter = gst_element_factory_make("capsfilter", "qt-ycest-video-caps");
    GstElement *sink = gst_element_factory_make("appsink", "qt-ycest-video-appsink");

    // RK3566 的 mppvideodec 原生输出 NV12。这里不再插入 videoconvert，
    // 避免 DMA-BUF 被映射到 CPU 后产生额外颜色转换或整帧复制。
    if (!bin || !queue || !capsFilter || !sink) {
        qWarning() << "[GStreamer] 创建 Qt 合成视频链路失败，缺少 queue/capsfilter/appsink";
        if (bin) gst_object_unref(bin);
        if (queue) gst_object_unref(queue);
        if (capsFilter) gst_object_unref(capsFilter);
        if (sink) gst_object_unref(sink);
        return nullptr;
    }

    // queue 必须使用非泄漏模式。downstream leaky 会让 MPP 以约 300 FPS
    // 向前解码，只保留十几秒后的未来帧，再被 appsink sync=true 长时间等待。
    // 单帧非泄漏队列通过反压保持正确的音视频时间轴。
    g_object_set(queue,
                 "leaky", 0,                 // no leaking
                 "max-size-buffers", 1,
                 "max-size-bytes", 0,
                 "max-size-time", static_cast<guint64>(0),
                 nullptr);

    // 强制 MPP 的 NV12 DMA-BUF 输出。普通 system-memory NV12 不会进入
    // CPU map/copy 回退路径，协商失败时由 GStreamer 明确报告错误。
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:DMABuf),format=NV12");
    g_object_set(capsFilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(sink,
                 "emit-signals", TRUE,
                 "sync", TRUE,
                 "max-buffers", 1,
                 "drop", TRUE,
                 "enable-last-sample", FALSE,
                 nullptr);

    gst_bin_add_many(GST_BIN(bin), queue, capsFilter, sink, nullptr);
    if (!gst_element_link_many(queue, capsFilter, sink, nullptr)) {
        qWarning() << "[GStreamer] Qt 合成视频 bin 链接失败";
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad *queueSinkPad = gst_element_get_static_pad(queue, "sink");
    GstPad *ghostPad = queueSinkPad ? gst_ghost_pad_new("sink", queueSinkPad) : nullptr;
    if (queueSinkPad) gst_object_unref(queueSinkPad);
    if (!ghostPad || !gst_element_add_pad(bin, ghostPad)) {
        qWarning() << "[GStreamer] Qt 合成视频 bin 创建 ghost sink pad 失败";
        if (ghostPad) gst_object_unref(ghostPad);
        gst_object_unref(bin);
        return nullptr;
    }

    g_signal_connect(sink, "new-sample",
                     G_CALLBACK(&GstPlayerWidget::appSinkNewSample), this);
    appSink_ = sink;

    qInfo() << "[GStreamer] EGLFS无桌面模式: MPP NV12 DMA-BUF"
            << "+ 非泄漏单帧queue + RGA缩放 + Qt OpenGL ES合成"
            << "CPU缩放回退=disabled";
    return bin;
}

/** @brief 创建可用的音频输出 sink。 */
GstElement *GstPlayerWidget::createAudioSink()
{
    if (preloadMode_) {
        GstElement *sink = gst_element_factory_make("fakesink", nullptr);
        configureSink(sink, false);
        return sink;
    }

    GstElement *sink = gst_element_factory_make("appsink", nullptr);
    if (!sink) return nullptr;

    // 每个 playbin 只把解码后的 PCM 送入共享混音器。ALSA 输出只由
    // SharedAudioMixer 持有，避免视频与刷卡提示音各自抢占 default 设备。
    g_object_set(sink,
                 "emit-signals", TRUE,
                 "sync", TRUE,
                 "async", TRUE,
                 "max-buffers", static_cast<guint>(12),
                 "drop", TRUE,
                 nullptr);
    g_signal_connect(sink, "new-sample",
                     G_CALLBACK(&GstPlayerWidget::sharedAudioAppSinkNewSample), this);
    return sink;
}

/** @brief 设置 sink 的同步、缓存和时延属性。 */
void GstPlayerWidget::configureSink(GstElement *sink, bool video)
{
    if (!sink) return;
    GObject *object = G_OBJECT(sink);
    setBooleanProperty(object, "sync", preloadMode_ ? FALSE : TRUE);
    setBooleanProperty(object, "async", preloadMode_ ? FALSE : TRUE);
    setBooleanProperty(object, "enable-last-sample", video && !preloadMode_ ? TRUE : FALSE);
    if (video) {
        // Qt 合成模式的宽高比由 QOpenGLWidget 控制；桌面兼容模式则由
        // videoSurface_ 几何控制。关闭 sink 自己的二次比例缩放。
        setBooleanProperty(object, "force-aspect-ratio", FALSE);
        setBooleanProperty(object, "keep-aspect-ratio", FALSE);
        setBooleanProperty(object, "keep-ratio", FALSE);
    } else if (hasProperty(object, "device")) {
        const QByteArray device = Rk3566Platform::alsaDevice().toUtf8();
        if (!device.isEmpty()) g_object_set(object, "device", device.constData(), nullptr);
    }
}

/** @brief 在音视频 pad 上安装到帧时间探针。 */
void GstPlayerWidget::attachFrameProbes()
{
    if (videoSink_) {
        videoProbePad_ = gst_element_get_static_pad(videoSink_, "sink");
        if (videoProbePad_) {
            videoProbeId_ = gst_pad_add_probe(videoProbePad_, GST_PAD_PROBE_TYPE_BUFFER,
                                              &GstPlayerWidget::videoPadProbe, this, nullptr);
        }
    }

    if (audioSink_) {
        audioProbePad_ = gst_element_get_static_pad(audioSink_, "sink");
        if (audioProbePad_) {
            audioProbeId_ = gst_pad_add_probe(audioProbePad_, GST_PAD_PROBE_TYPE_BUFFER,
                                              &GstPlayerWidget::audioPadProbe, this, nullptr);
        }
    }
}

/** @brief 移除探针，防止释放管线后继续回调。 */
void GstPlayerWidget::removeFrameProbes()
{
    if (videoProbePad_) {
        if (videoProbeId_) gst_pad_remove_probe(videoProbePad_, videoProbeId_);
        gst_object_unref(videoProbePad_);
        videoProbePad_ = nullptr;
        videoProbeId_ = 0;
    }
    if (audioProbePad_) {
        if (audioProbeId_) gst_pad_remove_probe(audioProbePad_, audioProbeId_);
        gst_object_unref(audioProbePad_);
        audioProbePad_ = nullptr;
        audioProbeId_ = 0;
    }
}

/** @brief 将管线标记为已准备并兑现等待中的播放请求。 */
void GstPlayerWidget::markPrepared()
{
    if (prepared_) return;
    prepared_ = true;
    emit prepared();

    if (playRequested_ && !preloadMode_ && playbin_) {
        gst_element_set_state(playbin_, GST_STATE_PLAYING);
    }
}

/** @brief 创建或绑定 Qt 视频画布。 */
void GstPlayerWidget::ensureVideoSurface()
{
    if (!videoWidget_ || videoSurface_) return;

    videoSurface_ = new QWidget(videoWidget_);
    videoSurface_->setObjectName(QStringLiteral("gstVideoSurface"));
    videoSurface_->setAttribute(Qt::WA_NativeWindow, true);
    videoSurface_->setAttribute(Qt::WA_OpaquePaintEvent, true);
    videoSurface_->setAttribute(Qt::WA_NoSystemBackground, false);
    videoSurface_->setAttribute(Qt::WA_TranslucentBackground, false);
    videoSurface_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    videoSurface_->setAutoFillBackground(true);

    QPalette pal = videoSurface_->palette();
    pal.setColor(QPalette::Window, Qt::black);
    videoSurface_->setPalette(pal);
    videoSurface_->setStyleSheet(QStringLiteral("background-color:#000000;border:none;"));

    const QRect initialRect = fittedVideoRect();
    videoSurface_->setGeometry(initialRect.isValid() ? initialRect : videoWidget_->rect());
    videoSurface_->show();
    videoSurface_->raise();

    const quintptr id = static_cast<quintptr>(videoSurface_->winId());
    windowId_.store(id);
    overlayWidth_.store(videoSurface_->width());
    overlayHeight_.store(videoSurface_->height());
}

/** @return 保持源宽高比后位于配置区域内的显示矩形。 */
QRect GstPlayerWidget::fittedVideoRect() const
{
    if (!videoWidget_) return QRect();

    const QRect dst = videoWidget_->rect();
    if (dst.width() <= 0 || dst.height() <= 0) return QRect();

    // 与原 TPlayer 的 calcFitAligned() 逻辑一致：源尺寸未知时先占满
    // videoRect；拿到 caps 后按源宽高比完整显示并居中，剩余区域由外层
    // 透明洞中未被视频 plane 覆盖的区域由底层显示背景填充。
    if (sourceWidth_ <= 0 || sourceHeight_ <= 0) {
        return dst;
    }

    int width = dst.width();
    int height = static_cast<int>((static_cast<qint64>(width) * sourceHeight_ +
                                   sourceWidth_ / 2) / sourceWidth_);

    if (height > dst.height()) {
        height = dst.height();
        width = static_cast<int>((static_cast<qint64>(height) * sourceWidth_ +
                                  sourceHeight_ / 2) / sourceHeight_);
    }

    // YUV/MPP 显示路径通常要求偶数尺寸。只向下对齐，绝不把矩形撑出
    // 配置区域；这比旧 TPlayer 的向外 16 对齐更适合 GstVideoOverlay。
    if (width > 1) width &= ~1;
    if (height > 1) height &= ~1;
    width = std::max(1, std::min(width, dst.width()));
    height = std::max(1, std::min(height, dst.height()));

    const int x = dst.x() + (dst.width() - width) / 2;
    const int y = dst.y() + (dst.height() - height) / 2;
    return QRect(x, y, width, height);
}

/** @brief 把当前显示矩形同步给硬件 overlay。 */
void GstPlayerWidget::updateVideoOverlay()
{
    if (preloadMode_ || !videoWidget_) return;

    if (configuredDisplayRect_.isValid() && videoWidget_->parentWidget() &&
        videoWidget_->geometry() != configuredDisplayRect_) {
        videoWidget_->setGeometry(configuredDisplayRect_);
    }

    if (useQtCompositedVideo()) {
        if (!videoWidget_->isVisible()) return;
        videoWidget_->show();
        videoWidget_->update();
        return;
    }

    ensureVideoSurface();
    if (!videoSurface_ || !videoWidget_->isVisible()) return;

    const QRect renderRect = fittedVideoRect();
    if (!renderRect.isValid()) return;

    if (videoSurface_->geometry() != renderRect) {
        videoSurface_->setGeometry(renderRect);
    }
    videoSurface_->show();

    const quintptr id = static_cast<quintptr>(videoSurface_->winId());
    const int width = videoSurface_->width();
    const int height = videoSurface_->height();
    windowId_.store(id);
    overlayWidth_.store(width);
    overlayHeight_.store(height);

    GstElement *overlay = nullptr;
    {
        QMutexLocker locker(&gstMutex_);
        if (overlayElement_) {
            overlay = GST_ELEMENT(gst_object_ref(overlayElement_));
        } else if (videoSink_ && GST_IS_VIDEO_OVERLAY(videoSink_)) {
            overlay = GST_ELEMENT(gst_object_ref(videoSink_));
        }
    }

    if (!overlay) return;

    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(overlay), static_cast<guintptr>(id));
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(overlay), 0, 0,
                                           width, height);
    gst_video_overlay_expose(GST_VIDEO_OVERLAY(overlay));
    gst_object_unref(overlay);
}

/** @return 当前配置选择 Qt 合成输出时返回 true。 */
bool GstPlayerWidget::useQtCompositedVideo() const
{
#ifdef QT_YCEST_RK3566
    // RK3566 正式版本固定走 EGLFS 单窗口 Qt 合成，禁止重新选择
    // linuxfb、kmssink 或其他独立 DRM/X11 视频层。
    return true;
#else
    const QString requested = QString::fromLocal8Bit(qgetenv("QT_YCEST_VIDEO_OUTPUT"))
            .trimmed().toLower();
    if (requested == QStringLiteral("qt") ||
        requested == QStringLiteral("eglfs") ||
        requested == QStringLiteral("appsink")) {
        return true;
    }
    if (requested == QStringLiteral("overlay") ||
        requested == QStringLiteral("native") ||
        requested == QStringLiteral("xcb")) {
        return false;
    }

    const QString platform = QGuiApplication::platformName().toLower();
    return platform.contains(QStringLiteral("eglfs")) ||
           platform.contains(QStringLiteral("minimalegl"));
#endif
}

/** @return 当前视频输出中的 Qt OpenGL 画布。 */
VideoHoleWidget *GstPlayerWidget::videoCanvas() const
{
    return dynamic_cast<VideoHoleWidget *>(videoWidget_.data());
}

/** @brief 清除 Qt 画布帧并归还邮箱槽位。 */
void GstPlayerWidget::clearVideoCanvas()
{
    if (VideoHoleWidget *canvas = videoCanvas()) {
        canvas->clearVideoFrame();
    }
    guiDeliveryEventPending_.storeRelease(0);
    resetFrameMailbox();
}

/** @brief 将本地路径或 URL 转换为 GStreamer URI。 */
QString GstPlayerWidget::mediaUri(const QString &path) const
{
    QString source = path.trimmed();
    if ((source.startsWith('"') && source.endsWith('"')) ||
        (source.startsWith('\'') && source.endsWith('\''))) {
        source = source.mid(1, source.size() - 2).trimmed();
    }
    if (source.isEmpty()) return QString();

    const QUrl url(source);
    if (!url.scheme().isEmpty()) {
        return QString::fromUtf8(url.toEncoded(QUrl::FullyEncoded));
    }

    const QFileInfo info(source);
    return QString::fromUtf8(QUrl::fromLocalFile(info.absoluteFilePath()).toEncoded(QUrl::FullyEncoded));
}

/** @brief 拉取并处理 GStreamer 总线上的状态、EOS 和错误消息。 */
void GstPlayerWidget::pollBus()
{
    if (!bus_) return;

    while (GstMessage *message = gst_bus_pop(bus_)) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const QString text = error ? QString::fromUtf8(error->message)
                                       : QStringLiteral("unknown GStreamer error");
            const QString detail = debug ? QString::fromUtf8(debug) : QString();
            if (error) g_error_free(error);
            if (debug) g_free(debug);
            prepared_ = false;
            playing_ = false;
            SharedAudioMixer::instance().removeSource(this);
            emit errorOccured(detail.isEmpty() ? text : text + QStringLiteral(" | ") + detail);
            break;
        }
        case GST_MESSAGE_EOS:
            prepared_ = false;
            playing_ = false;
            positionTimer_.stop();
            SharedAudioMixer::instance().removeSource(this);
            emit videoFinished();
            break;
        case GST_MESSAGE_ASYNC_DONE:
            markPrepared();
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(playbin_)) {
                GstState oldState = GST_STATE_NULL;
                GstState newState = GST_STATE_NULL;
                GstState pending = GST_STATE_VOID_PENDING;
                gst_message_parse_state_changed(message, &oldState, &newState, &pending);
                Q_UNUSED(oldState);
                Q_UNUSED(pending);
                if (newState == GST_STATE_PAUSED || newState == GST_STATE_PLAYING) {
                    markPrepared();
                }
                playing_ = (newState == GST_STATE_PLAYING);
            }
            break;
        default:
            break;
        }
        gst_message_unref(message);
    }
}

/** @brief 周期读取播放位置和时长。 */
void GstPlayerWidget::pollPosition()
{
    if (!playbin_ || (!prepared_ && !playing_)) return;
    gint64 position = 0;
    gint64 duration = 0;
    if (!gst_element_query_position(playbin_, GST_FORMAT_TIME, &position)) return;
    gst_element_query_duration(playbin_, GST_FORMAT_TIME, &duration);
    emit positionChanged(static_cast<int>(position / GST_MSECOND),
                         static_cast<int>(duration / GST_MSECOND));
}

/** @brief 在同步总线回调中绑定硬件视频窗口句柄。 */
GstBusSyncReply GstPlayerWidget::busSyncHandler(GstBus *, GstMessage *message, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load()) return GST_BUS_PASS;

    // EGLFS Qt 合成模式不使用 GstVideoOverlay，也没有独立视频窗口句柄。
    if (self->useQtCompositedVideo()) return GST_BUS_PASS;

    if (!gst_is_video_overlay_prepare_window_handle_message(message)) {
        return GST_BUS_PASS;
    }

    const quintptr id = self->windowId_.load();
    if (!id || self->preloadMode_) return GST_BUS_PASS;

    GstElement *element = GST_ELEMENT(GST_MESSAGE_SRC(message));
    if (GST_IS_VIDEO_OVERLAY(element)) {
        {
            QMutexLocker locker(&self->gstMutex_);
            if (self->overlayElement_ != element) {
                if (self->overlayElement_) gst_object_unref(self->overlayElement_);
                self->overlayElement_ = GST_ELEMENT(gst_object_ref(element));
            }
        }
        setBooleanProperty(G_OBJECT(element), "force-aspect-ratio", FALSE);
        setBooleanProperty(G_OBJECT(element), "keep-aspect-ratio", FALSE);
        setBooleanProperty(G_OBJECT(element), "keep-ratio", FALSE);
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(element), static_cast<guintptr>(id));

        // prepare-window-handle 在 GStreamer 流线程中触发。这里立即应用配置
        // 区域的宽高，避免 sink 在 Qt 的延迟更新前按默认全屏区域输出。
        const int width = self->overlayWidth_.load();
        const int height = self->overlayHeight_.load();
        if (width > 0 && height > 0) {
            gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(element),
                                                   0, 0, width, height);
        }
    }
    return GST_BUS_DROP;
}

/** @brief 记录视频帧到达时间并节流发送监测信号。 */
GstPadProbeReturn GstPlayerWidget::videoPadProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    self->lastVideoFrameMs_.storeRelease(now);

    if (self->sourceSizeKnown_.loadAcquire() == 0) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        GstVideoInfo videoInfo;
        if (caps && gst_video_info_from_caps(&videoInfo, caps)) {
            const int width = GST_VIDEO_INFO_WIDTH(&videoInfo);
            const int height = GST_VIDEO_INFO_HEIGHT(&videoInfo);
            if (width > 0 && height > 0 && self->sourceSizeKnown_.testAndSetOrdered(0, 1)) {
                QMetaObject::invokeMethod(self, [self, width, height]() {
                    self->setSourceVideoSize(width, height);
                }, Qt::QueuedConnection);
            }
        }
        if (caps) gst_caps_unref(caps);
    }

    const qint64 lastSignal = self->lastVideoSignalMs_.loadAcquire();
    if (now - lastSignal >= 200) {
        self->lastVideoSignalMs_.storeRelease(now);
        QMetaObject::invokeMethod(self, [self, now]() {
            if (!self->shuttingDown_.load()) emit self->videoFrameArrived(now);
        }, Qt::QueuedConnection);
    }
    return GST_PAD_PROBE_OK;
}

/** @brief 记录音频帧到达时间并节流发送监测信号。 */
GstPadProbeReturn GstPlayerWidget::audioPadProbe(GstPad *, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    self->lastAudioFrameMs_.storeRelease(now);
    const qint64 lastSignal = self->lastAudioSignalMs_.loadAcquire();
    if (now - lastSignal >= 500) {
        self->lastAudioSignalMs_.storeRelease(now);
        QMetaObject::invokeMethod(self, [self, now]() {
            if (!self->shuttingDown_.load()) emit self->audioFrameArrived(now);
        }, Qt::QueuedConnection);
    }
    return GST_PAD_PROBE_OK;
}

/** @brief 为指定代次和尺寸取得可写邮箱槽位。 */
int GstPlayerWidget::acquireFrameMailboxSlot(int width,
                                              int height,
                                              int generation,
                                              int targetBytes,
                                              QString *error)
{
    QMutexLocker locker(&frameMailboxMutex_);

    const bool layoutChanged = frameMailboxWidth_ != width ||
                               frameMailboxHeight_ != height ||
                               frameMailboxGeneration_ != generation;
    if (layoutChanged) {
        bool busy = false;
        for (const FrameMailboxSlot &slot : frameMailboxSlots_) {
            if (slot.state == FrameSlotWriting || slot.state == FrameSlotReading) {
                busy = true;
                break;
            }
        }
        if (busy) {
            if (error) {
                *error = QStringLiteral("帧邮箱正在切换尺寸/媒体，等待Writing或Reading槽归还");
            }
            return -1;
        }

        for (FrameMailboxSlot &slot : frameMailboxSlots_) {
            if (slot.state == FrameSlotReady) {
                mailboxReadyOverwrites_.fetchAndAddOrdered(1);
            }
            slot.nv12Frame.resize(targetBytes);
            slot.sourceWidth = 0;
            slot.sourceHeight = 0;
            slot.width = width;
            slot.height = height;
            slot.generation = generation;
            slot.sampleArrivalMs = 0;
            slot.publishedMs = 0;
            slot.rgaElapsedUs = 0;
            slot.sequence = 0;
            slot.state = FrameSlotFree;
        }
        frameMailboxWidth_ = width;
        frameMailboxHeight_ = height;
        frameMailboxGeneration_ = generation;
        latestReadyFrameSlot_ = -1;
    }

    // Latest-frame-wins：若存在尚未被GUI读取的Ready帧，直接复用该槽。
    // 这样GUI恢复时不会先显示几百毫秒前的旧帧。
    int slotIndex = -1;
    if (latestReadyFrameSlot_ >= 0 &&
        latestReadyFrameSlot_ < kFrameMailboxSlotCount &&
        frameMailboxSlots_[latestReadyFrameSlot_].state == FrameSlotReady) {
        slotIndex = latestReadyFrameSlot_;
        frameMailboxSlots_[slotIndex].state = FrameSlotWriting;
        latestReadyFrameSlot_ = -1;
        mailboxReadyOverwrites_.fetchAndAddOrdered(1);
    } else {
        for (int index = 0; index < kFrameMailboxSlotCount; ++index) {
            if (frameMailboxSlots_[index].state == FrameSlotFree) {
                slotIndex = index;
                frameMailboxSlots_[index].state = FrameSlotWriting;
                break;
            }
        }
    }

    if (slotIndex < 0 && error) {
        *error = QStringLiteral("三缓冲帧邮箱无可写槽");
    }
    return slotIndex;
}

/** @brief 放弃未发布的写槽并恢复为空闲。 */
void GstPlayerWidget::abandonFrameMailboxSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kFrameMailboxSlotCount) return;
    QMutexLocker locker(&frameMailboxMutex_);
    FrameMailboxSlot &slot = frameMailboxSlots_[slotIndex];
    if (slot.state == FrameSlotWriting) {
        slot.state = FrameSlotFree;
    }
}

/** @brief 将写完的槽发布为最新就绪帧。 */
bool GstPlayerWidget::publishFrameMailboxSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kFrameMailboxSlotCount) return false;

    QMutexLocker locker(&frameMailboxMutex_);
    FrameMailboxSlot &slot = frameMailboxSlots_[slotIndex];
    if (slot.state != FrameSlotWriting ||
        slot.generation != mediaGeneration_.loadAcquire()) {
        if (slot.state == FrameSlotWriting) slot.state = FrameSlotFree;
        return false;
    }

    // 正常情况下只有一个Ready槽；这里仍做防御性清理，绝不让GUI追赶历史帧。
    for (int index = 0; index < kFrameMailboxSlotCount; ++index) {
        if (index != slotIndex && frameMailboxSlots_[index].state == FrameSlotReady) {
            frameMailboxSlots_[index].state = FrameSlotFree;
            mailboxReadyOverwrites_.fetchAndAddOrdered(1);
        }
    }

    slot.sequence = ++frameMailboxSequence_;
    slot.publishedMs = QDateTime::currentMSecsSinceEpoch();
    slot.state = FrameSlotReady;
    latestReadyFrameSlot_ = slotIndex;
    mailboxPublishedFrames_.fetchAndAddOrdered(1);
    return true;
}

/** @brief 合并多个解码回调，只排队一次 GUI 投递。 */
void GstPlayerWidget::scheduleLatestFrameDelivery()
{
    if (shuttingDown_.load()) return;

    {
        QMutexLocker locker(&frameMailboxMutex_);
        if (latestReadyFrameSlot_ < 0 ||
            latestReadyFrameSlot_ >= kFrameMailboxSlotCount ||
            frameMailboxSlots_[latestReadyFrameSlot_].state != FrameSlotReady) {
            return;
        }
    }

    if (!guiDeliveryEventPending_.testAndSetOrdered(0, 1)) {
        guiDeliveryCoalesced_.fetchAndAddOrdered(1);
        return;
    }

    QPointer<GstPlayerWidget> safeSelf(this);
    const bool queued = QMetaObject::invokeMethod(this, [safeSelf]() {
        if (safeSelf) safeSelf->deliverLatestFrameToCanvas();
    }, Qt::QueuedConnection);

    if (!queued) {
        guiDeliveryEventPending_.storeRelease(0);
    }
}

/** @brief GUI 线程取得最新帧并提交给 OpenGL 画布。 */
void GstPlayerWidget::deliverLatestFrameToCanvas()
{
    int slotIndex = -1;
    quint64 sequence = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int width = 0;
    int height = 0;
    int generation = 0;
    qint64 sampleArrivalMs = 0;
    qint64 publishedMs = 0;

    {
        QMutexLocker locker(&frameMailboxMutex_);
        if (latestReadyFrameSlot_ >= 0 &&
            latestReadyFrameSlot_ < kFrameMailboxSlotCount &&
            frameMailboxSlots_[latestReadyFrameSlot_].state == FrameSlotReady) {
            slotIndex = latestReadyFrameSlot_;
            FrameMailboxSlot &slot = frameMailboxSlots_[slotIndex];
            slot.state = FrameSlotReading;
            latestReadyFrameSlot_ = -1;
            sequence = slot.sequence;
            sourceWidth = slot.sourceWidth;
            sourceHeight = slot.sourceHeight;
            width = slot.width;
            height = slot.height;
            generation = slot.generation;
            sampleArrivalMs = slot.sampleArrivalMs;
            publishedMs = slot.publishedMs;
        }
    }

    if (slotIndex < 0) {
        guiDeliveryEventPending_.storeRelease(0);
        scheduleLatestFrameDelivery();
        return;
    }

    const qint64 guiNowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 queueLagMs = qMax<qint64>(0, guiNowMs - publishedMs);
    qint64 oldQueueMax = guiMaxQueueLagMs_.loadAcquire();
    while (queueLagMs > oldQueueMax &&
           !guiMaxQueueLagMs_.testAndSetOrdered(oldQueueMax, queueLagMs)) {
        oldQueueMax = guiMaxQueueLagMs_.loadAcquire();
    }

    const qint64 endToEndLagMs = qMax<qint64>(0, guiNowMs - sampleArrivalMs);
    qint64 oldEndToEndMax = guiMaxEndToEndLagMs_.loadAcquire();
    while (endToEndLagMs > oldEndToEndMax &&
           !guiMaxEndToEndLagMs_.testAndSetOrdered(oldEndToEndMax, endToEndLagMs)) {
        oldEndToEndMax = guiMaxEndToEndLagMs_.loadAcquire();
    }

    if (generation != mediaGeneration_.loadAcquire()) {
        finishFrameMailboxRead(slotIndex, sequence);
        return;
    }

    setSourceVideoSize(sourceWidth, sourceHeight);
    VideoHoleWidget *canvas = videoCanvas();
    if (!canvas) {
        finishFrameMailboxRead(slotIndex, sequence);
        return;
    }

    QPointer<GstPlayerWidget> safeSelf(this);
    const QByteArray &frameData = frameMailboxSlots_[slotIndex].nv12Frame;
    canvas->setNv12Frame(width, height, frameData,
                         [safeSelf, slotIndex, sequence]() {
        if (safeSelf) safeSelf->finishFrameMailboxRead(slotIndex, sequence);
    });
    guiDeliveredFrames_.fetchAndAddOrdered(1);
}

/** @brief 画布消费完成后按序列号归还读槽。 */
void GstPlayerWidget::finishFrameMailboxRead(int slotIndex, quint64 sequence)
{
    if (slotIndex >= 0 && slotIndex < kFrameMailboxSlotCount) {
        QMutexLocker locker(&frameMailboxMutex_);
        FrameMailboxSlot &slot = frameMailboxSlots_[slotIndex];
        if (slot.state == FrameSlotReading && slot.sequence == sequence) {
            slot.state = FrameSlotFree;
        }
    }

    guiDeliveryEventPending_.storeRelease(0);
    scheduleLatestFrameDelivery();
}

/** @brief 清空邮箱状态并使遗留帧无效。 */
void GstPlayerWidget::resetFrameMailbox()
{
    QMutexLocker locker(&frameMailboxMutex_);
    latestReadyFrameSlot_ = -1;

    bool hasBusySlot = false;
    for (FrameMailboxSlot &slot : frameMailboxSlots_) {
        if (slot.state == FrameSlotReady) {
            slot.state = FrameSlotFree;
        } else if (slot.state == FrameSlotWriting || slot.state == FrameSlotReading) {
            hasBusySlot = true;
        }
    }

    // Writing槽可能正由GStreamer线程使用，不能在这里清空或resize。
    // generation 已经变化后，它会在publish阶段自行作废；下一帧再安全重建邮箱布局。
    if (!hasBusySlot) {
        frameMailboxWidth_ = 0;
        frameMailboxHeight_ = 0;
        frameMailboxGeneration_ = 0;
    }
}

/** @brief 把当前播放器解码出的 PCM 样本送入进程内共享音频混音器。 */
GstFlowReturn GstPlayerWidget::sharedAudioAppSinkNewSample(GstElement *sink, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || self->preloadMode_) {
        return GST_FLOW_FLUSHING;
    }

    GstSample *sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_EOS;

    SharedAudioMixer::instance().pushSample(self, sample);
    gst_sample_unref(sample);

    // 共享输出管线首次启动时可能仍处于异步状态，appsrc 会短暂返回
    // GST_FLOW_FLUSHING。这里已经成功消费了 playbin 的样本，不能把共享
    // 管线的瞬时状态反向传播给 appsink，否则 playbin 会永久停止音频流，
    // 表现为视频画面正常但全程无声。后续样本会在共享管线就绪后继续送入。
    return GST_FLOW_OK;
}

/** @brief 接收 appsink 样本，RGA 转换后写入帧邮箱。 */
GstFlowReturn GstPlayerWidget::appSinkNewSample(GstElement *sink, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || self->preloadMode_) {
        return GST_FLOW_FLUSHING;
    }

    GstSample *sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_EOS;

    const qint64 sampleArrivalMs = QDateTime::currentMSecsSinceEpoch();
    self->appSinkSamples_.fetchAndAddOrdered(1);
    const qint64 previousSampleMs = self->appSinkLastSampleMs_.fetchAndStoreOrdered(sampleArrivalMs);
    if (previousSampleMs > 0) {
        const qint64 gapMs = sampleArrivalMs - previousSampleMs;
        qint64 oldMax = self->appSinkMaxGapMs_.loadAcquire();
        while (gapMs > oldMax &&
               !self->appSinkMaxGapMs_.testAndSetOrdered(oldMax, gapMs)) {
            oldMax = self->appSinkMaxGapMs_.loadAcquire();
        }
    }

    int mailboxSlotIndex = -1;
    auto rejectFrame = [self, sample, &mailboxSlotIndex](const QString &reason,
                                                         GstFlowReturn result = GST_FLOW_OK)
            -> GstFlowReturn {
        if (mailboxSlotIndex >= 0) {
            self->abandonFrameMailboxSlot(mailboxSlotIndex);
            mailboxSlotIndex = -1;
        }
        const quint64 skipped = self->appSinkRgaSkippedFrames_.fetchAndAddOrdered(1) + 1;
        // 连续故障时限制日志量，但首批错误必须完整输出。
        if (skipped <= 5 || (skipped % 50) == 0) {
            qCritical() << "[RGA] 跳过视频帧，禁止CPU回退"
                        << "reason=" << reason
                        << "skipped=" << skipped;
        }
        gst_sample_unref(sample);
        return result;
    };

    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    gst_video_info_init(&info);

    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
        return rejectFrame(QStringLiteral("appsink未收到有效NV12 caps/buffer"),
                           GST_FLOW_NOT_NEGOTIATED);
    }

    const int sourceWidth = GST_VIDEO_INFO_WIDTH(&info);
    const int sourceHeight = GST_VIDEO_INFO_HEIGHT(&info);
    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        (sourceWidth & 1) || (sourceHeight & 1)) {
        return rejectFrame(QStringLiteral("NV12源尺寸无效: %1x%2")
                           .arg(sourceWidth).arg(sourceHeight),
                           GST_FLOW_NOT_NEGOTIATED);
    }

    const guint memoryCount = gst_buffer_n_memory(buffer);
    if (memoryCount != 1) {
        return rejectFrame(QStringLiteral("仅支持单FD连续NV12 DMA-BUF，当前memory数量=%1")
                           .arg(memoryCount));
    }

    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (!memory || !gst_is_dmabuf_memory(memory)) {
        return rejectFrame(QStringLiteral("输入不是DMA-BUF；CPU map/copy回退已禁用"));
    }

    gsize memoryOffset = 0;
    gsize memoryMaxSize = 0;
    const gsize memorySize = gst_memory_get_sizes(memory, &memoryOffset, &memoryMaxSize);
    if (memoryOffset != 0) {
        return rejectFrame(QStringLiteral("DMA-BUF存在非零offset=%1，RGA路径不做隐式CPU整理")
                           .arg(static_cast<qulonglong>(memoryOffset)));
    }

    const int dmaFd = gst_dmabuf_memory_get_fd(memory);
    if (dmaFd < 0) {
        return rejectFrame(QStringLiteral("无法取得DMA-BUF文件描述符"));
    }

    int sourceWStride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
    if (sourceWStride < sourceWidth) sourceWStride = sourceWidth;
    int sourceHStride = sourceHeight;

    // 优先使用 GstVideoMeta 的真实 stride/UV offset；MPP 常把1080行对齐到1088行。
    if (GstVideoMeta *videoMeta = gst_buffer_get_video_meta(buffer)) {
        if (videoMeta->n_planes >= 2 && videoMeta->stride[0] >= sourceWidth) {
            sourceWStride = videoMeta->stride[0];
            if (videoMeta->offset[1] > 0 &&
                (videoMeta->offset[1] % static_cast<gsize>(sourceWStride)) == 0) {
                const int inferredHStride = static_cast<int>(videoMeta->offset[1] /
                                                              sourceWStride);
                if (inferredHStride >= sourceHeight) sourceHStride = inferredHStride;
            }
        }
    }

    // video meta 缺失时，优先根据 GstMemory 的有效数据长度反推垂直 stride；
    // maxsize 可能包含页对齐尾部，不能用它推断 UV 平面起始位置。
    const gsize bufferDataSize = memorySize > 0 ? memorySize : gst_buffer_get_size(buffer);
    const gsize importSize = memoryMaxSize >= bufferDataSize ? memoryMaxSize : bufferDataSize;
    if (sourceWStride > 0 && bufferDataSize > 0 && sourceHStride == sourceHeight) {
        const quint64 numerator = static_cast<quint64>(bufferDataSize) * 2;
        const quint64 denominator = static_cast<quint64>(sourceWStride) * 3;
        if (denominator > 0 && numerator % denominator == 0) {
            const quint64 inferred = numerator / denominator;
            if (inferred >= static_cast<quint64>(sourceHeight) &&
                inferred <= static_cast<quint64>(sourceHeight + 256)) {
                sourceHStride = static_cast<int>(inferred);
            }
        }
    }

    const quint64 minimumSourceBytes = static_cast<quint64>(sourceWStride) *
                                       sourceHStride * 3 / 2;
    if (importSize > static_cast<gsize>(std::numeric_limits<int>::max())) {
        return rejectFrame(QStringLiteral("DMA-BUF过大，librga int size接口无法导入: %1")
                           .arg(static_cast<qulonglong>(importSize)));
    }
    if (importSize < minimumSourceBytes) {
        return rejectFrame(QStringLiteral("DMA-BUF容量不足: size=%1 required=%2 stride=%3x%4")
                           .arg(static_cast<qulonglong>(importSize))
                           .arg(static_cast<qulonglong>(minimumSourceBytes))
                           .arg(sourceWStride).arg(sourceHStride));
    }

    const int targetBoxWidth = self->overlayWidth_.load();
    const int targetBoxHeight = self->overlayHeight_.load();
    const QSize targetSize = fittedEvenNv12Size(sourceWidth, sourceHeight,
                                                targetBoxWidth, targetBoxHeight);
    if (!targetSize.isValid()) {
        return rejectFrame(QStringLiteral("Qt视频区域尚未就绪: box=%1x%2")
                           .arg(targetBoxWidth).arg(targetBoxHeight));
    }

    const qint64 targetBytes64 = static_cast<qint64>(targetSize.width()) *
                                 targetSize.height() * 3 / 2;
    if (targetBytes64 <= 0 || targetBytes64 > std::numeric_limits<int>::max()) {
        return rejectFrame(QStringLiteral("RGA目标缓冲区尺寸溢出: %1x%2")
                           .arg(targetSize.width()).arg(targetSize.height()));
    }

    const int generation = self->mediaGeneration_.loadAcquire();
    QString mailboxError;
    mailboxSlotIndex = self->acquireFrameMailboxSlot(targetSize.width(),
                                                      targetSize.height(),
                                                      generation,
                                                      static_cast<int>(targetBytes64),
                                                      &mailboxError);
    if (mailboxSlotIndex < 0) {
        const quint64 dropped = self->mailboxNoSlotDrops_.fetchAndAddOrdered(1) + 1;
        if (dropped <= 5 || (dropped % 50) == 0) {
            qWarning() << "[VIDEO-MAILBOX] 无可写槽，跳过当前实时帧"
                       << "reason=" << mailboxError
                       << "dropped=" << dropped;
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    FrameMailboxSlot &slot = self->frameMailboxSlots_[mailboxSlotIndex];
    slot.sourceWidth = sourceWidth;
    slot.sourceHeight = sourceHeight;
    slot.width = targetSize.width();
    slot.height = targetSize.height();
    slot.generation = generation;
    slot.sampleArrivalMs = sampleArrivalMs;

    QElapsedTimer rgaTimer;
    rgaTimer.start();

    // 保持原有 DMA-BUF -> 三缓冲 NV12 邮箱链路不变，仅统一走 FaceGate
    // 已使用的公共 RGA 处理接口；仍然禁止 CPU 缩放回退。
    /** @brief 缩短下方 RGA 缓冲区类型的限定名。 */
    using RgaImageProcessor::Buffer;
    /** @brief 缩短下方 RGA 像素格式类型的限定名。 */
    using RgaImageProcessor::PixelFormat;

    const Buffer sourceImage = Buffer::fromDmaFd(
        dmaFd,
        sourceWidth,
        sourceHeight,
        sourceWStride,
        sourceHStride,
        PixelFormat::Nv12);
    const Buffer targetImage = Buffer::fromVirtualAddress(
        slot.nv12Frame.data(),
        slot.width,
        slot.height,
        slot.width,
        slot.height,
        PixelFormat::Nv12);

    QString rgaError;
    if (!RgaImageProcessor::process(sourceImage, targetImage, &rgaError)) {
        return rejectFrame(QStringLiteral("%1 source=%2x%3 stride=%4x%5 target=%6x%7 fd=%8")
                           .arg(rgaError)
                           .arg(sourceWidth).arg(sourceHeight)
                           .arg(sourceWStride).arg(sourceHStride)
                           .arg(slot.width).arg(slot.height)
                           .arg(dmaFd));
    }

    slot.rgaElapsedUs = rgaTimer.nsecsElapsed() / 1000;
    self->appSinkRgaFrames_.fetchAndAddOrdered(1);
    qint64 oldRgaMax = self->appSinkMaxRgaUs_.loadAcquire();
    while (slot.rgaElapsedUs > oldRgaMax &&
           !self->appSinkMaxRgaUs_.testAndSetOrdered(oldRgaMax, slot.rgaElapsedUs)) {
        oldRgaMax = self->appSinkMaxRgaUs_.loadAcquire();
    }

    if (self->firstRgaSuccessLogged_.testAndSetOrdered(0, 1)) {
        qInfo() << "[RGA] first NV12 DMA-BUF resize success"
                << "source=" << QSize(sourceWidth, sourceHeight)
                << "sourceStride=" << QSize(sourceWStride, sourceHStride)
                << "target=" << targetSize
                << "inputBytes=" << static_cast<qulonglong>(importSize)
                << "outputBytes=" << slot.nv12Frame.size()
                << "elapsedUs=" << slot.rgaElapsedUs
                << "mailboxSlots=" << kFrameMailboxSlotCount
                << "CPU fallback=disabled";
    }

    gst_sample_unref(sample);
    sample = nullptr;

    if (generation != self->mediaGeneration_.loadAcquire()) {
        self->abandonFrameMailboxSlot(mailboxSlotIndex);
        return GST_FLOW_OK;
    }

    if (!self->publishFrameMailboxSlot(mailboxSlotIndex)) {
        return GST_FLOW_OK;
    }
    mailboxSlotIndex = -1;
    self->scheduleLatestFrameDelivery();
    return GST_FLOW_OK;
}

/** @brief 在 source 创建时配置网络源参数。 */
void GstPlayerWidget::sourceSetup(GstElement *, GstElement *source, gpointer)
{
    if (!source) return;

    // rtspsrc 的属性在 source-setup 阶段配置。延迟可通过环境变量覆盖，
    // 默认 300 ms 在 RK3566 局域网直播和抗抖动之间较平衡。
    bool ok = false;
    const int envLatency = QString::fromLocal8Bit(qgetenv("QT_YCEST_RTSP_LATENCY_MS")).toInt(&ok);
    const int latency = ok ? std::max(0, envLatency) : 300;
    setIntegerProperty(G_OBJECT(source), "latency", latency);
    setBooleanProperty(G_OBJECT(source), "drop-on-latency", TRUE);
    setBooleanProperty(G_OBJECT(source), "do-retransmission", FALSE);
}

/** @brief 在 element 创建时调整解码器和解析器属性。 */
void GstPlayerWidget::elementSetup(GstElement *, GstElement *element, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || !element || self->shuttingDown_.load() ||
        self->hlsAuAlignmentEnabled_.loadAcquire() == 0) {
        return;
    }

    const char *factoryName = elementFactoryName(element);
    if (!factoryName) return;

    if (std::strcmp(factoryName, "h264parse") != 0 ||
        g_object_get_data(G_OBJECT(element), "ycest-hls-au-query-hooked")) {
        return;
    }

    // 不修改 decodebin 已经创建的内部 capsfilter。HLS 开始推流后再改
    // capsfilter 会触发重新协商，并可能让 hlsdemux 报 not-negotiated。
    // 在 h264parse 首次协商输出格式时限制为完整 AU，使解析器先完成
    // NAL -> AU 组装，再把 byte-stream/AU 数据交给 Rockchip MPP。
    setIntegerProperty(G_OBJECT(element), "config-interval", -1);
    g_object_set_data(G_OBJECT(element),
                      "ycest-hls-au-query-hooked",
                      GINT_TO_POINTER(1));
    GstPad *srcPad = gst_element_get_static_pad(element, "src");
    if (!srcPad) return;
    gst_pad_add_probe(srcPad,
                      static_cast<GstPadProbeType>(
                              GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM |
                              GST_PAD_PROBE_TYPE_PUSH),
                      &GstPlayerWidget::hlsH264ParserQueryProbe,
                      self,
                      nullptr);
    gst_object_unref(srcPad);

    qInfo() << "[GStreamer-HLS] H.264 parser negotiation restricted to"
            << "stream-format=byte-stream alignment=au";
}

/** @brief 为 HLS H.264 caps 查询补充 AU 对齐要求。 */
GstPadProbeReturn GstPlayerWidget::hlsH264ParserQueryProbe(
        GstPad *pad,
        GstPadProbeInfo *info,
        gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || !pad || !info || self->shuttingDown_.load() ||
        self->hlsAuAlignmentEnabled_.loadAcquire() == 0) {
        return GST_PAD_PROBE_OK;
    }

    GstQuery *query = GST_PAD_PROBE_INFO_QUERY(info);
    if (!query) return GST_PAD_PROBE_OK;

    GstCaps *requiredCaps = h264ByteStreamAuCaps();
    if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
        GstCaps *candidateCaps = nullptr;
        gst_query_parse_accept_caps(query, &candidateCaps);
        const bool formatAllowed =
                candidateCaps &&
                gst_caps_can_intersect(candidateCaps, requiredCaps);
        gst_caps_unref(requiredCaps);
        if (formatAllowed) {
            return GST_PAD_PROBE_OK;
        }
        gst_query_set_accept_caps_result(query, FALSE);
        return GST_PAD_PROBE_HANDLED;
    }

    if (GST_QUERY_TYPE(query) != GST_QUERY_CAPS) {
        gst_caps_unref(requiredCaps);
        return GST_PAD_PROBE_OK;
    }

    GstCaps *requestedCaps = nullptr;
    gst_query_parse_caps(query, &requestedCaps);
    GstCaps *restrictedCaps = requestedCaps
            ? gst_caps_intersect_full(requestedCaps,
                                      requiredCaps,
                                      GST_CAPS_INTERSECT_FIRST)
            : gst_caps_ref(requiredCaps);

    GstCaps *resultCaps = nullptr;
    GstPad *peer = gst_pad_get_peer(pad);
    if (peer && !gst_caps_is_empty(restrictedCaps)) {
        GstQuery *peerQuery = gst_query_new_caps(restrictedCaps);
        if (gst_pad_query(peer, peerQuery)) {
            GstCaps *peerCaps = nullptr;
            gst_query_parse_caps_result(peerQuery, &peerCaps);
            if (peerCaps) {
                resultCaps = gst_caps_intersect_full(peerCaps,
                                                     requiredCaps,
                                                     GST_CAPS_INTERSECT_FIRST);
            }
        }
        gst_query_unref(peerQuery);
        gst_object_unref(peer);
    }

    if (!resultCaps) {
        resultCaps = gst_caps_ref(restrictedCaps);
    }
    gst_query_set_caps_result(query, resultCaps);
    gst_caps_unref(resultCaps);
    gst_caps_unref(restrictedCaps);
    gst_caps_unref(requiredCaps);
    return GST_PAD_PROBE_HANDLED;
}

/** @brief 从缓存的最新 sample 构建快照。 */
QImage GstPlayerWidget::imageFromLastSample(QString *error) const
{
    GstElement *sampleSink = nullptr;
    if (videoSink_ && hasProperty(G_OBJECT(videoSink_), "last-sample")) {
        sampleSink = GST_ELEMENT(gst_object_ref(videoSink_));
    } else {
        QMutexLocker locker(&gstMutex_);
        if (overlayElement_ && hasProperty(G_OBJECT(overlayElement_), "last-sample")) {
            sampleSink = GST_ELEMENT(gst_object_ref(overlayElement_));
        }
    }

    if (!sampleSink) {
        if (error) {
            *error = QStringLiteral("当前视频 sink 不支持 last-sample；可配置 ximagesink/glimagesink");
        }
        return QImage();
    }

    GstSample *sample = nullptr;
    g_object_get(sampleSink, "last-sample", &sample, nullptr);
    gst_object_unref(sampleSink);
    if (!sample) {
        if (error) *error = QStringLiteral("尚未取得可截图的视频帧");
        return QImage();
    }

    const QImage image = imageFromSample(sample, error);
    gst_sample_unref(sample);
    return image;
}

/** @brief 映射一个 GstSample 并转换为 QImage。 */
QImage GstPlayerWidget::imageFromSample(GstSample *sample, QString *error) const
{
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!caps || !buffer) {
        if (error) *error = QStringLiteral("GstSample 缺少 caps 或 buffer");
        return QImage();
    }

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        if (error) *error = QStringLiteral("无法解析视频 caps");
        return QImage();
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        if (error) *error = QStringLiteral("视频帧不可映射，可能是不可读的 DRM/DMABUF 零拷贝缓冲");
        return QImage();
    }

    const int width = GST_VIDEO_INFO_WIDTH(&info);
    const int height = GST_VIDEO_INFO_HEIGHT(&info);
    const GstVideoFormat format = GST_VIDEO_INFO_FORMAT(&info);
    QImage image(width, height, QImage::Format_ARGB32);

    const uchar *p0 = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int s0 = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);

    auto writePacked = [&](int bytesPerPixel, int rIndex, int gIndex, int bIndex, int aIndex) {
        for (int y = 0; y < height; ++y) {
            const uchar *src = p0 + y * s0;
            QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const uchar *pixel = src + x * bytesPerPixel;
                const int alpha = aIndex >= 0 ? pixel[aIndex] : 255;
                dst[x] = qRgba(pixel[rIndex], pixel[gIndex], pixel[bIndex], alpha);
            }
        }
    };

    switch (format) {
    case GST_VIDEO_FORMAT_BGRA: writePacked(4, 2, 1, 0, 3); break;
    case GST_VIDEO_FORMAT_BGRx: writePacked(4, 2, 1, 0, -1); break;
    case GST_VIDEO_FORMAT_RGBA: writePacked(4, 0, 1, 2, 3); break;
    case GST_VIDEO_FORMAT_RGBx: writePacked(4, 0, 1, 2, -1); break;
    case GST_VIDEO_FORMAT_ARGB: writePacked(4, 1, 2, 3, 0); break;
    case GST_VIDEO_FORMAT_xRGB: writePacked(4, 1, 2, 3, -1); break;
    case GST_VIDEO_FORMAT_ABGR: writePacked(4, 3, 2, 1, 0); break;
    case GST_VIDEO_FORMAT_xBGR: writePacked(4, 3, 2, 1, -1); break;
    case GST_VIDEO_FORMAT_RGB:  writePacked(3, 0, 1, 2, -1); break;
    case GST_VIDEO_FORMAT_BGR:  writePacked(3, 2, 1, 0, -1); break;
    case GST_VIDEO_FORMAT_GRAY8:
        for (int y = 0; y < height; ++y) {
            const uchar *src = p0 + y * s0;
            QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) dst[x] = qRgb(src[x], src[x], src[x]);
        }
        break;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21: {
        const uchar *uv = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
        const int suv = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
        const bool nv21 = format == GST_VIDEO_FORMAT_NV21;
        for (int y = 0; y < height; ++y) {
            const uchar *yr = p0 + y * s0;
            const uchar *uvr = uv + (y / 2) * suv;
            QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const int offset = (x / 2) * 2;
                const int u = uvr[offset + (nv21 ? 1 : 0)];
                const int v = uvr[offset + (nv21 ? 0 : 1)];
                dst[x] = yuvToRgb(yr[x], u, v);
            }
        }
        break;
    }
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12: {
        const uchar *plane1 = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
        const uchar *plane2 = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 2));
        const int s1 = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
        const int s2 = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2);
        const bool yv12 = format == GST_VIDEO_FORMAT_YV12;
        const uchar *uPlane = yv12 ? plane2 : plane1;
        const uchar *vPlane = yv12 ? plane1 : plane2;
        const int uStride = yv12 ? s2 : s1;
        const int vStride = yv12 ? s1 : s2;
        for (int y = 0; y < height; ++y) {
            const uchar *yr = p0 + y * s0;
            const uchar *ur = uPlane + (y / 2) * uStride;
            const uchar *vr = vPlane + (y / 2) * vStride;
            QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                dst[x] = yuvToRgb(yr[x], ur[x / 2], vr[x / 2]);
            }
        }
        break;
    }
    default:
        image = QImage();
        if (error) {
            const char *formatName = gst_video_format_to_string(format);
            *error = QStringLiteral("暂不支持截图像素格式: %1")
                    .arg(formatName ? QString::fromLatin1(formatName) : QStringLiteral("unknown"));
        }
        break;
    }

    gst_video_frame_unmap(&frame);
    return image;
}
