/**
 * @file gst_player_widget.cpp
 * @brief 实现 GStreamer 播放管线、三槽帧邮箱和 Qt OpenGL 视频画布。
 *
 * @author Du lin
 * @note 最近修改：2026-11-14；修改人：Du lin；RTSP 超时日志直接读取流编号和 SSRC，同一媒体地址内按两字段去重。
 * @date 2026-07-25
 * @par 修改记录
 * 2026-11-05，Du lin：共享输出按当前音频占用者筛选输入及缓存，增加独立直播 PCM 入口。
 */

#include "gst_player_widget.h"
#include "common/monotonic_clock.h"
#include "src/call/AudioOutputFocus.h"
#include "gst_video_validation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <utility>

#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QWaitCondition>
#include <QVector>
#include <QOpenGLContext>
#include <QSharedPointer>
#include <QThread>
#include <QMetaObject>
#include <QPalette>
#include <QStringList>
#include <QMutexLocker>
#include <QUrl>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

#include <gst/video/videooverlay.h>
#ifdef QT_YCEST_RK3566
#include <gst/pbutils/pbutils.h>
#endif
#include <gst/allocators/gstdmabuf.h>
#include <gst/rtp/gstrtpbuffer.h>

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

constexpr qint64 kLiveDecoderCreditLagNs = 800 * GST_MSECOND;
constexpr qint64 kLiveDecoderMaximumInFlightAus = 12;
constexpr int kLiveDecoderCreditConfirmAus = 3;

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

        auto pluginVersion = [registry](const char *name) -> QString {
            GstPlugin *plugin = gst_registry_find_plugin(registry, name);
            if (!plugin) return QStringLiteral("missing");
            const gchar *version = gst_plugin_get_version(plugin);
            const QString result = version
                    ? QString::fromLatin1(version) : QStringLiteral("unknown");
            gst_object_unref(plugin);
            return result;
        };
        guint runtimeMajor = 0;
        guint runtimeMinor = 0;
        guint runtimeMicro = 0;
        guint runtimeNano = 0;
        gst_version(&runtimeMajor, &runtimeMinor, &runtimeMicro, &runtimeNano);
        qInfo().noquote() << QStringLiteral(
                "[GStreamer-DIAG] runtime=%1 rtpmanagerPlugin=%2 rtspPlugin=%3")
                .arg(QStringLiteral("%1.%2.%3.%4")
                     .arg(runtimeMajor)
                     .arg(runtimeMinor)
                     .arg(runtimeMicro)
                     .arg(runtimeNano))
                .arg(pluginVersion("rtpmanager"))
                .arg(pluginVersion("rtsp"));
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

/** @return GstRTSPSrcBufferMode 枚举值对应的稳定日志名称。 */
QString rtspBufferModeName(gint value)
{
    switch (value) {
    case 0: return QStringLiteral("none");
    case 1: return QStringLiteral("slave");
    case 2: return QStringLiteral("buffer");
    case 3: return QStringLiteral("auto");
    case 4: return QStringLiteral("synced");
    default: return QStringLiteral("value-%1").arg(value);
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

/** @brief 返回元素所属插件及其版本，供板端确认 Rockchip BSP 组件归属。 */
QString elementPluginVersion(GstElement *element)
{
    GstElementFactory *factory = element ? gst_element_get_factory(element) : nullptr;
    if (!factory) return QStringLiteral("unknown");
    const gchar *pluginName = gst_plugin_feature_get_plugin_name(
                GST_PLUGIN_FEATURE(factory));
    if (!pluginName || !*pluginName) return QStringLiteral("unknown");

    GstPlugin *plugin = gst_registry_find_plugin(gst_registry_get(), pluginName);
    if (!plugin) return QString::fromLatin1(pluginName) + QStringLiteral("/unknown");
    const gchar *version = gst_plugin_get_version(plugin);
    const QString result = QStringLiteral("%1/%2")
            .arg(QString::fromLatin1(pluginName),
                 version ? QString::fromLatin1(version) : QStringLiteral("unknown"));
    gst_object_unref(plugin);
    return result;
}

/** @brief 读取可选布尔属性并格式化；旧插件不公开属性时返回 unsupported。 */
QString booleanPropertyText(GObject *object, const char *name)
{
    if (!hasProperty(object, name)) return QStringLiteral("unsupported");
    gboolean value = FALSE;
    g_object_get(object, name, &value, nullptr);
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

/** @return 工厂名对应视频解码器时返回 true。 */
bool isVideoDecoderFactory(const QString &factory)
{
    return factory == QStringLiteral("mppvideodec") ||
           factory == QStringLiteral("mpph264dec") ||
           factory == QStringLiteral("mpph265dec") ||
           factory == QStringLiteral("rkvideodec") ||
           factory == QStringLiteral("avdec_h264") ||
           factory == QStringLiteral("avdec_h265") ||
           (factory.contains(QStringLiteral("video")) &&
            factory.endsWith(QStringLiteral("dec")));
}

/** @brief 识别 RTP 元素的媒体类型。
 * @param[in] element 待读取 sink caps 的借用元素，可为空。
 * @return caps 中的 media 小写值，无法识别时返回 unknown。
 */
QString rtpMediaType(GstElement *element)
{
    if (!element) return QStringLiteral("unknown");

    GstPad *sinkPad = gst_element_get_static_pad(element, "sink");
    if (!sinkPad) return QStringLiteral("unknown");
    GstCaps *caps = gst_pad_get_current_caps(sinkPad);
    if (!caps) caps = gst_pad_query_caps(sinkPad, nullptr);
    gst_object_unref(sinkPad);

    QString media = QStringLiteral("unknown");
    if (caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps)) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        if (structure) {
            const gchar *mediaValue = gst_structure_get_string(structure, "media");
            if (mediaValue && *mediaValue) {
                media = QString::fromLatin1(mediaValue).toLower();
            }
        }
    }
    if (caps) gst_caps_unref(caps);
    return media;
}

/**
 * @brief 计算 buffer PTS 对应运行时间相对当前管线运行时间的偏移。
 * @return 可取得时间段、时钟和有效 PTS 时返回 true；正值表示该帧位于未来。
 */
bool bufferPtsClockOffsetNs(GstPad *pad, GstBuffer *buffer, qint64 *offsetNs)
{
    if (!pad || !buffer || !offsetNs ||
        !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
        return false;
    }

    GstClockTime bufferRunningTime = GST_CLOCK_TIME_NONE;
    GstEvent *segmentEvent = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
    if (segmentEvent) {
        const GstSegment *segment = nullptr;
        gst_event_parse_segment(segmentEvent, &segment);
        if (segment && segment->format == GST_FORMAT_TIME) {
            bufferRunningTime = gst_segment_to_running_time(
                        segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
        }
        gst_event_unref(segmentEvent);
    }
    if (!GST_CLOCK_TIME_IS_VALID(bufferRunningTime)) return false;

    GstElement *element = gst_pad_get_parent_element(pad);
    if (!element) return false;
    GstClock *clock = gst_element_get_clock(element);
    const GstClockTime baseTime = gst_element_get_base_time(element);
    gst_object_unref(element);
    if (!clock || !GST_CLOCK_TIME_IS_VALID(baseTime)) {
        if (clock) gst_object_unref(clock);
        return false;
    }

    const GstClockTime clockTime = gst_clock_get_time(clock);
    gst_object_unref(clock);
    if (!GST_CLOCK_TIME_IS_VALID(clockTime) || clockTime < baseTime) return false;
    const GstClockTime pipelineRunningTime = clockTime - baseTime;

    const quint64 maximum = static_cast<quint64>(std::numeric_limits<qint64>::max());
    if (bufferRunningTime >= pipelineRunningTime) {
        const quint64 difference = bufferRunningTime - pipelineRunningTime;
        *offsetNs = difference > maximum
                ? std::numeric_limits<qint64>::max()
                : static_cast<qint64>(difference);
    } else {
        const quint64 difference = pipelineRunningTime - bufferRunningTime;
        *offsetNs = difference > maximum
                ? std::numeric_limits<qint64>::min()
                : -static_cast<qint64>(difference);
    }
    return true;
}

/** @brief 以无锁方式更新诊断区间内的最大绝对值。 */
void updateMaximumAbsolute(QAtomicInteger<qint64> *maximum, qint64 value)
{
    if (!maximum) return;
    const qint64 absoluteValue = value == std::numeric_limits<qint64>::min()
            ? std::numeric_limits<qint64>::max()
            : qAbs(value);
    qint64 oldMaximum = maximum->loadAcquire();
    while (absoluteValue > oldMaximum &&
           !maximum->testAndSetOrdered(oldMaximum, absoluteValue)) {
        oldMaximum = maximum->loadAcquire();
    }
}

/** @brief 更新相邻 buffer 的最大到达间隔。 */
void updateMaximumBufferGap(QAtomicInteger<qint64> *lastBufferMs,
                            QAtomicInteger<qint64> *maximumGapMs,
                            qint64 nowMs)
{
    if (!lastBufferMs || !maximumGapMs) return;
    const qint64 previousMs = lastBufferMs->fetchAndStoreOrdered(nowMs);
    if (previousMs <= 0 || nowMs <= previousMs) return;
    const qint64 gapMs = nowMs - previousMs;
    qint64 oldMaximum = maximumGapMs->loadAcquire();
    while (gapMs > oldMaximum &&
           !maximumGapMs->testAndSetOrdered(oldMaximum, gapMs)) {
        oldMaximum = maximumGapMs->loadAcquire();
    }
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

/** @brief 按宽高比计算 NV12 目标尺寸，宽向下按 4 对齐、高向下按 2 对齐。 */
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

    // 目标缓冲区按图像宽度紧密排列，RGA 要求 NV12 行跨度按 4 对齐。
    // 宽按 4、高按 2 向下对齐，保证图像不超出 Qt 视频区域。
    width &= ~3;
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

/** @brief 进程内唯一共享音频输出，供视频和播报共用声卡。
 * @details 视频及文件播报各自解码后在此动态接入 audiomixer，最终共用一个声卡 sink。
 *          输入表与退役表由 mutex_ 保护；退役输入先发送 EOS，后台在停止整条管线后
 *          拆除 request pad，避免运行中停止 queue。普通输入保留原有满时丢旧数据策略，
 *          后接阻塞队列为混音器提供可查询的等待余量；输入和 sink 的最近送样时间
 *          由回调线程原子更新，供后台断流恢复判断。
 *          总线线程只登记延迟变化，退役后台线程串行重算，避免在流线程中执行查询。
 */
class SharedAudioMixer final
{
public:
    static SharedAudioMixer &instance()
    {
        static SharedAudioMixer mixer;
        return mixer;
    }

    /** @brief 后台交接共享输出，状态切换绝不持有回调线程使用的 mutex_。
     * @param[in] exclusive true 释放声卡，false 恢复；调用方须串行调用。
     * @param[in] timeout 等待在途拆分支及 NULL 状态的时限，纳秒；NONE 按一秒处理。
     * @return true 已释放声卡或成功提交恢复；false 为拆分支超时或三次重开均失败。
     * @note 只有整条管线已确认 NULL 才销毁延迟分支；重开失败先确认 NULL，再等待
     *       80ms 重试两次。异步启动错误由总线通知后台重建；安全拆除旧分支后
     *       通知后台重新协商活动输入的延迟。
     */
    bool setCallExclusive(bool exclusive, GstClockTime timeout = GST_SECOND)
    {
        GstElement *pipeline=nullptr;
        const unsigned long waitBudgetMs=timeout==GST_CLOCK_TIME_NONE
                ? 1000UL : static_cast<unsigned long>(qBound<guint64>(
                          1,timeout/GST_MSECOND,5000));
        QElapsedTimer removalWait;
        removalWait.start();
        {
            QMutexLocker locker(&mutex_);
            const bool previousExclusive=callExclusive_;
            callExclusive_=true;
            // 已离开表锁的最后一个输入可继续停止；等待有界，失败时恢复原输入门。
            while (activeRemovals_>0) {
                const qint64 remaining=qint64(waitBudgetMs)-removalWait.elapsed();
                if (remaining<=0 || !removalFinished_.wait(&mutex_,
                        static_cast<unsigned long>(remaining))) {
                    qWarning() << "[audio-boundary] shared switch removal timeout"
                               << "exclusive=" << exclusive
                               << "activeRemovals=" << activeRemovals_
                               << "deferred=" << deferredSources_.size()
                               << "waitMs=" << removalWait.elapsed();
                    callExclusive_=previousExclusive;
                    return false;
                }
            }
            if (pipeline_) pipeline=GST_ELEMENT(gst_object_ref(pipeline_));
            for (auto *branch:sources_) branch->nextPts=GST_CLOCK_TIME_NONE;
        }
        if (!pipeline) {
            QMutexLocker locker(&mutex_);
            callExclusive_=exclusive;
            return true;
        }
        bool success=true;
        bool topologyChanged=false;
        for (;;) {
            QVector<SourceBranch *> deferred;
            bool resume=false;
            bool canRetire=false;
            if (!exclusive) {
                GstState current=GST_STATE_NULL;
                GstState pending=GST_STATE_VOID_PENDING;
                const auto stateResult=gst_element_get_state(pipeline,&current,&pending,0);
                canRetire=stateResult!=GST_STATE_CHANGE_FAILURE &&
                        current==GST_STATE_NULL && pending==GST_STATE_VOID_PENDING;
            }
            {
                QMutexLocker locker(&mutex_);
                if (canRetire) deferred.swap(deferredSources_);
                resume=!sources_.isEmpty();
            }
            // 仅在整条共享管线停止后销毁退役输入；运行中的 queue 不单独切 NULL。
            for (auto *branch:deferred) destroySourceLocked(branch);
            topologyChanged=topologyChanged || !deferred.isEmpty();
            if (!deferred.isEmpty())
                qInfo() << "[audio-boundary] shared retired branches destroyed"
                        << "count=" << deferred.size() << "pipeline=" << pipeline;
            const GstState target=exclusive || !resume ? GST_STATE_NULL : GST_STATE_PLAYING;
            auto result=gst_element_set_state(pipeline,target);
            if (exclusive) {
                success=success && result!=GST_STATE_CHANGE_FAILURE;
                GstState current=GST_STATE_NULL;
                const auto settled=gst_element_get_state(pipeline,&current,nullptr,timeout);
                success=success && settled!=GST_STATE_CHANGE_FAILURE && settled!=GST_STATE_CHANGE_ASYNC &&
                    current==GST_STATE_NULL;
                if (success) {
                    QVector<SourceBranch *> stoppedBranches;
                    {
                        QMutexLocker locker(&mutex_);
                        stoppedBranches.swap(deferredSources_);
                    }
                    for (auto *branch:stoppedBranches) destroySourceLocked(branch);
                    topologyChanged=topologyChanged || !stoppedBranches.isEmpty();
                    if (!stoppedBranches.isEmpty())
                        qInfo() << "[audio-boundary] shared retired branches destroyed"
                                << "count=" << stoppedBranches.size()
                                << "pipeline=" << pipeline;
                    outputFault_.store(false);
                }
                break;
            }
            if (target==GST_STATE_PLAYING && result==GST_STATE_CHANGE_FAILURE) {
                logOwnAlsaHandles("reopen-failed");
                for (int attempt=2;attempt<=3 && result==GST_STATE_CHANGE_FAILURE;++attempt) {
                    const auto nullResult=gst_element_set_state(pipeline,GST_STATE_NULL);
                    GstState current=GST_STATE_VOID_PENDING;
                    const auto settled=gst_element_get_state(
                                pipeline,&current,nullptr,200 * GST_MSECOND);
                    qWarning() << "[audio-boundary] shared sink reopen retry"
                               << "attempt=" << attempt << "nullResult=" << int(nullResult)
                               << "waitResult=" << int(settled) << "current=" << int(current)
                               << "fault=" << outputFault_.load();
                    if (nullResult==GST_STATE_CHANGE_FAILURE ||
                            settled==GST_STATE_CHANGE_FAILURE ||
                            settled==GST_STATE_CHANGE_ASYNC || current!=GST_STATE_NULL)
                        break;
                    QVector<SourceBranch *> stoppedBranches;
                    {
                        QMutexLocker locker(&mutex_);
                        stoppedBranches.swap(deferredSources_);
                    }
                    for (auto *branch:stoppedBranches) destroySourceLocked(branch);
                    topologyChanged=topologyChanged || !stoppedBranches.isEmpty();
                    if (!stoppedBranches.isEmpty())
                        qInfo() << "[audio-boundary] shared retired branches destroyed"
                                << "count=" << stoppedBranches.size()
                                << "pipeline=" << pipeline << "duringReopen=" << true;
                    outputFault_.store(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    result=gst_element_set_state(pipeline,GST_STATE_PLAYING);
                    qInfo() << "[audio-boundary] shared sink reopen result"
                            << "attempt=" << attempt << "setResult=" << int(result);
                }
            }
            success=success && result!=GST_STATE_CHANGE_FAILURE;
            QMutexLocker locker(&mutex_);
            if (canRetire && !deferredSources_.isEmpty()) continue;
            // 恢复需要新 PCM 完成 preroll，先打开输入门，不等待 PLAYING。
            callExclusive_=false;
            break;
        }
        if (topologyChanged) latencyPending_.store(true);
        if (!exclusive && success && latencyPending_.load()) requestRetirement();
        gst_object_unref(pipeline);
        return success;
    }

    /** @brief 直播工作线程接管声卡。
     * @param[in,out] stop 当前直播的停止标志；通话可置位，必须存活到归还。
     * @return true 声卡已释放且直播取得独占权；false 包括在途拆分支等待超时。
     * @note 失败时仅在逻辑门仍关闭的情况下恢复共享输出；恢复失败则重建管线，
     *       后续由直播失败路径释放焦点。
     */
    bool acquireBroadcastOutput(std::atomic_bool &stop)
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        if (stop.load() || callOutputRequested_ || broadcastStop_) {
            qWarning() << "[GStreamer-AUDIO] broadcast output acquire denied"
                       << "stopped=" << stop.load() << "callRequested=" << callOutputRequested_
                       << "broadcastOwned=" << (broadcastStop_ != nullptr);
            return false;
        }
        qInfo() << "[GStreamer-AUDIO] broadcast output acquire begin";
        broadcastOutputActive_.store(true);
        if (!setCallExclusive(true)) {
            qWarning() << "[GStreamer-AUDIO] broadcast output acquire failed to stop shared pipeline";
            bool gateStillClosed=false;
            {
                QMutexLocker locker(&mutex_);
                gateStillClosed=callExclusive_;
            }
            // 拆分支等待超时会自行恢复原门；只有状态切换已关门才尝试归还。
            if (gateStillClosed) {
                const bool restored=setCallExclusive(false);
                const bool rebuilt=!restored && resetSharedOutput();
                qWarning() << "[audio-boundary] broadcast acquire rollback"
                           << "restored=" << restored << "rebuilt=" << rebuilt;
            }
            broadcastOutputActive_.store(false);
            return false;
        }
        broadcastStop_ = &stop;
        qInfo() << "[GStreamer-AUDIO] broadcast output acquired";
        return true;
    }

    /** @brief ALSA 已关闭后在直播后台恢复共享输出。确认超时或出错时重建共享音频。
     * @return true 已恢复、无输入等下一帧创建，或已让给通话；false 重建失败。
     * @note 等待 PLAYING 时释放交接锁，通话可直接申请独占；视频 playbin 不参与重建。
     */
    bool releaseBroadcastOutput()
    {
        bool submitted = false;
        {
            std::lock_guard<std::mutex> lock(outputMutex_);
            if (!broadcastStop_) return true;
            broadcastStop_ = nullptr;
            broadcastOutputActive_.store(false);
            outputChanged_.notify_all();
            if (callOutputRequested_) {
                qInfo() << "[GStreamer-AUDIO] broadcast output handed to call";
                return true;
            }
            outputFault_.store(false);
            submitted = setCallExclusive(false);
            qInfo() << "[GStreamer-AUDIO] broadcast shared resume submitted" << "success=" << submitted;
        }
        if (submitted && sharedOutputReady()) {
            broadcastRecoveryPending_.store(false);
            qInfo() << "[GStreamer-AUDIO] broadcast output restored";
            return true;
        }

        std::lock_guard<std::mutex> lock(outputMutex_);
        if (callOutputRequested_ || broadcastStop_ || priorityOwner_.load() == this ||
                ringtoneToken_.load() != 0) {
            qInfo() << "[GStreamer-AUDIO] broadcast output recovery deferred to active owner"
                    << "callRequested=" << callOutputRequested_ << "broadcastOwned=" << (broadcastStop_ != nullptr)
                    << "callSelected=" << (priorityOwner_.load() == this)
                    << "ringtone=" << (ringtoneToken_.load() != 0);
            return true;
        }
        broadcastRecoveryPending_.store(true);
        qWarning() << "[GStreamer-AUDIO] broadcast output restore failed or timed out; rebuilding shared audio";
        const bool rebuilt = resetSharedOutput();
        qInfo() << "[GStreamer-AUDIO] broadcast audio rebuild" << "readyForPcm=" << rebuilt;
        return rebuilt;
    }

    /** @brief 确认有输入的共享管线到达 PLAYING，最多等 200ms；无输入时按需启动。 */
    bool sharedOutputReady()
    {
        GstElement *pipeline = nullptr;
        int sourceCount = 0;
        {
            QMutexLocker lock(&mutex_);
            sourceCount = sources_.size();
            if (sources_.isEmpty()) {
                qInfo() << "[GStreamer-AUDIO] shared output ready check" << "sources=0"
                        << "fault=" << outputFault_.load();
                return !outputFault_.load();
            }
            pipeline = GST_ELEMENT(gst_object_ref(pipeline_));
        }
        GstState current = GST_STATE_NULL;
        const auto result = gst_element_get_state(pipeline,&current,nullptr,200 * GST_MSECOND);
        gst_object_unref(pipeline);
        qInfo() << "[GStreamer-AUDIO] shared output ready check" << "sources=" << sourceCount
                << "state=" << int(current) << "result=" << int(result)
                << "fault=" << outputFault_.load();
        return result != GST_STATE_CHANGE_FAILURE && result != GST_STATE_CHANGE_ASYNC &&
            current == GST_STATE_PLAYING && !outputFault_.load();
    }

    /** @brief 在后台关闭并移除旧共享管线，后续 PCM 用原创建路径重建音频分支。
     * @return true 已清理故障管线并打开输入门，false 未能确认旧管线关闭。
     * @note 调用方持有交接锁；输入门关闭时，流线程只丢弃共享音频，继续处理视频。
     */
    bool resetSharedOutput()
    {
        if (!setCallExclusive(true,200 * GST_MSECOND)) {
            qWarning() << "[GStreamer-AUDIO] shared output rebuild aborted; NULL not confirmed";
            setCallExclusive(false);
            return false;
        }

        QVector<SourceBranch *> retired;
        {
            QMutexLocker lock(&mutex_);
            retired.swap(deferredSources_);
            for (auto *branch : sources_) retired.append(branch);
            sources_.clear();
        }
        for (auto *branch : retired) destroySourceLocked(branch);
        qInfo() << "[GStreamer-AUDIO] shared output branches retired" << "count=" << retired.size();

        QMutexLocker lock(&mutex_);
        removeDiagnosticProbesLocked();
        if (bus_) {
            gst_bus_set_sync_handler(bus_,nullptr,nullptr,nullptr);
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (pipeline_) gst_object_unref(pipeline_);
        pipeline_ = mixer_ = outputSink_ = nullptr;
        outputFault_.store(false);
        callExclusive_ = false;
        return true;
    }

    /** @brief 保留原通话声卡交接，并等已打开的直播输出归还。
     * @param[in] exclusive true申请通话独占，false归还。
     * @return true 已完成或提交状态切换；false 表示申请失败，或归还与重建均失败。
     * @note 仅在后台调用；等待直播最多一秒，条件变量等待期间释放交接锁。
     */
    bool setCallOutput(bool exclusive)
    {
        std::unique_lock<std::mutex> lock(outputMutex_);
        callOutputRequested_ = exclusive;
        if (!exclusive && broadcastStop_) return true;
        if (exclusive && broadcastStop_) {
            broadcastStop_->store(true);
            if (!outputChanged_.wait_for(lock, std::chrono::seconds(1), [this] { return !broadcastStop_; }))
                return false;
        }
        const bool changed=setCallExclusive(exclusive);
        if (!exclusive && !changed) return resetSharedOutput();
        return changed;
    }

    /** @brief 普通播报后台清空共享混音和声卡待播 PCM，视频 playbin 不变。
     * @param[out] nullConfirmed 可空；仅当共享管线已确认 NULL 时写 true，其他路径写 false。
     * @return true 已确认 NULL 且共享输出可重新送样；遇到独占或重建失败返回 false。
     * @note 不等待已有声卡交接；重开失败时在后台重建，避免焦点流程继续使用失败的 sink。
     */
    bool discardQueuedAudioForPrompt(bool *nullConfirmed = nullptr)
    {
        if (nullConfirmed) *nullConfirmed = false;
        std::unique_lock<std::mutex> outputLock(outputMutex_,std::try_to_lock);
        if (!outputLock) {
            qWarning() << "[GStreamer-AUDIO] prompt clear skipped; output handoff busy";
            return false;
        }
        if (callOutputRequested_ || broadcastStop_ ||
                priorityOwner_.load() == this || ringtoneToken_.load() != 0) {
            qWarning() << "[GStreamer-AUDIO] prompt clear skipped" << "callRequested=" << callOutputRequested_
                       << "broadcastOwned=" << (broadcastStop_ != nullptr)
                       << "callSelected=" << (priorityOwner_.load() == this)
                       << "ringtone=" << (ringtoneToken_.load() != 0);
            return false;
        }
        {
            QMutexLocker lock(&mutex_);
            if (activeRemovals_ != 0) {
                qWarning() << "[GStreamer-AUDIO] prompt clear skipped; active removals=" << activeRemovals_;
                return false;
            }
            callExclusive_=true; // 新的分支拆除只登记，交给本次后台恢复。
        }
        bool cleared = setCallExclusive(true,200 * GST_MSECOND);
        bool restored = setCallExclusive(false);
        if (!cleared || !restored) {
            const bool rebuilt=resetSharedOutput();
            qWarning() << "[audio-boundary] prompt shared output rebuilt"
                       << "initialNull=" << cleared << "initialResume=" << restored
                       << "rebuilt=" << rebuilt;
            if (rebuilt) cleared=restored=true;
        }
        if (nullConfirmed) *nullConfirmed = cleared;
        qInfo() << "[GStreamer-AUDIO] prompt clear result" << "nullConfirmed=" << cleared
                << "resumeSubmitted=" << restored;
        return cleared && restored;
    }

    /** @brief 在功放交接完成前保持普通视频输入静音，混音线程按样本读取。 */
    void holdVideoAudio(bool held)
    {
        const bool previous = videoAudioHeld_.exchange(held);
        if (previous != held)
            qInfo() << "[GStreamer-AUDIO] video audio hold changed" << "held=" << held;
    }

    /** @brief 原子更新混音分支选择。player为空且call=true时仅接受当前铃声。
     * @param[in] player 借用身份值，不在线程间解引用。
     * @param[in] call 是否仅放行通话铃声。
     */
    void selectAudio(GstPlayerWidget *player, bool call)
    {
        priorityOwner_.store(call ? static_cast<const void *>(this) : player);
        qInfo() << "[GStreamer-AUDIO] priority input selected" << "player=" << player
                << "call=" << call;
    }

    /** @return 本次选中的唯一铃声代次；只在 UI 申请，流线程读取原子值。 */
    uint64_t claimRingtone()
    {
        const auto token = ++nextRingtoneToken_;
        ringtoneToken_.store(token);
        return token;
    }

    /** @brief 归还匹配的铃声代次，过期释放不影响新拨号。
     * @param[in] token 申请时取得的代次。
     */
    void releaseRingtone(uint64_t token)
    {
        if (token > 1) ringtoneToken_.compare_exchange_strong(token, 0);
    }

    /** @brief 交接解码采样；视频在软抢占时继续入队，静音由混音输入探针执行。
     * @param[in] owner 解码播放器身份，作为分支键使用。
     * @param[in] sample 借用的解码采样。
     * @param[in] promptSource true 使用提示音的连续时间线。
     * @param[in] ringtoneToken 0 为普通媒体，非零为该播放器的铃声代次。
     * @param[in] reanchorTimeline true 以当前共享时钟重建连续时间线。
     * @param[out] submitted 可空，只有 appsrc 接收成功时才置 true。
     * @return 失焦返回 GST_FLOW_OK；声卡交接或可恢复输出故障时对受管提示音返回
     *         GST_FLOW_FLUSHING 以便保留当前样本重试，其余创建错误返回 ERROR。
     * @note 只在输入格式首次建立或变化时记录 caps，供后续定位 not-negotiated 错误。
     */
    GstFlowReturn pushSample(GstPlayerWidget *owner,
                             GstSample *sample,
                             bool promptSource,
                             uint64_t ringtoneToken,
                             bool reanchorTimeline = false,
                             bool *submitted = nullptr)
    {
        if (submitted) *submitted = false;
        if (!owner || !sample) return GST_FLOW_ERROR;
        const bool managed = owner->priorityManaged();
        const bool continuousTimeline = promptSource;
        if (audioDisposition(owner, ringtoneToken, managed) == AudioDisposition::Drop) return GST_FLOW_OK;

        GstCaps *caps = gst_sample_get_caps(sample);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (!caps || !buffer) return GST_FLOW_ERROR;

        GstBuffer *copy = gst_buffer_copy(buffer);
        if (!copy) return GST_FLOW_ERROR;

        SourceBranch *branch = nullptr;
        GstElement *appsrc = nullptr;
        GstClockTime previousNextPts = GST_CLOCK_TIME_NONE;
        GstClockTime scheduledNextPts = GST_CLOCK_TIME_NONE;
        {
            QMutexLocker locker(&mutex_);
            if (audioDisposition(owner, ringtoneToken, managed) == AudioDisposition::Drop) {
                gst_buffer_unref(copy);
                return GST_FLOW_OK;
            }
            if (callExclusive_) {
                gst_buffer_unref(copy);
                return promptSource && (managed || broadcastOutputActive_.load())
                        ? GST_FLOW_FLUSHING : GST_FLOW_OK;
            }
            if (outputFault_.load()) {
                gst_buffer_unref(copy);
                return promptSource ? GST_FLOW_FLUSHING : GST_FLOW_OK;
            }
            QString error;
            branch = ensureSourceLocked(owner, &error, promptSource, ringtoneToken, continuousTimeline);
            if (!branch) {
                qWarning() << "[GStreamer-AUDIO] shared mixer input unavailable:" << error;
                gst_buffer_unref(copy);
                return promptSource && outputFault_.load()
                        ? GST_FLOW_FLUSHING : GST_FLOW_ERROR;
            }

            if (!branch->caps || !gst_caps_is_equal(branch->caps, caps)) {
                if (branch->caps) gst_caps_unref(branch->caps);
                branch->caps = gst_caps_copy(caps);
                g_object_set(branch->appsrc, "caps", caps, nullptr);
            }

            // push-buffer 在解码/音频输出异常时可能等待 GStreamer 内部锁。
            // 对 appsrc 单独加引用后立即释放共享表锁，避免 Qt 主线程随后在
            // 提示音停止、EOS 收尾或新一轮播报时等待同一把锁而整体卡死。
            appsrc = GST_ELEMENT(gst_object_ref(branch->appsrc));
            previousNextPts = branch->nextPts;
            if (continuousTimeline) {
                // 提示音沿共享时钟累加样本时长，避免把解码线程的唤醒间隔写入时间戳。
                if (reanchorTimeline ||
                        !GST_CLOCK_TIME_IS_VALID(branch->nextPts)) {
                    GstClockTime now = GST_CLOCK_TIME_NONE;
                    GstClock *clock = pipeline_ ? gst_element_get_clock(pipeline_) : nullptr;
                    if (clock) {
                        const GstClockTime clockNow = gst_clock_get_time(clock);
                        const GstClockTime baseTime = gst_element_get_base_time(pipeline_);
                        if (GST_CLOCK_TIME_IS_VALID(clockNow) &&
                                GST_CLOCK_TIME_IS_VALID(baseTime) && clockNow >= baseTime) {
                            now = clockNow - baseTime;
                        }
                        gst_object_unref(clock);
                    }
                    branch->nextPts = GST_CLOCK_TIME_IS_VALID(now) ? now : 0;
                }
                GstClockTime duration = GST_BUFFER_DURATION(copy);
                if (!GST_CLOCK_TIME_IS_VALID(duration) || duration == 0) {
                    duration = 20 * GST_MSECOND;
                    GST_BUFFER_DURATION(copy) = duration;
                }
                GST_BUFFER_PTS(copy) = branch->nextPts;
                GST_BUFFER_DTS(copy) = branch->nextPts;
                branch->nextPts += duration;
                scheduledNextPts = branch->nextPts;
            } else {
                GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE;
                GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
            }
            GST_BUFFER_OFFSET(copy) = GST_BUFFER_OFFSET_NONE;
            GST_BUFFER_OFFSET_END(copy) = GST_BUFFER_OFFSET_NONE;
        }

        GstFlowReturn flow = GST_FLOW_ERROR;
        g_signal_emit_by_name(appsrc, "push-buffer", copy, &flow);
        if (submitted) *submitted = flow == GST_FLOW_OK;
        gst_object_unref(appsrc);
        gst_buffer_unref(copy);
        if (flow != GST_FLOW_OK && continuousTimeline) {
            QMutexLocker locker(&mutex_);
            SourceBranch *current = sources_.value(owner, nullptr);
            if (current == branch && current->nextPts == scheduledNextPts) {
                current->nextPts = previousNextPts;
            }
        }
        return flow;
    }

    /** @brief 按预卷样本的 PCM 格式给共享输出预热。
     * @param[in] owner 发起预热的播放器，调用期间借用。
     * @param[in] preroll 借用的已解码样本，提供 PCM 格式和块长度。
     * @param[in] targetDuration 目标静音时长，单位纳秒，实际按完整样本块和重复上限取整。
     * @param[in] ringtoneToken 当前铃声代次，普通播报为0。
     * @param[out] submitted 可空；只有静音真正入队才置 true。
     * @return appsrc 提交结果、焦点门状态或样本校验错误。
     */
    GstFlowReturn pushSilentPreroll(GstPlayerWidget *owner,
                                    GstSample *preroll,
                                    GstClockTime targetDuration,
                                    uint64_t ringtoneToken, bool *submitted = nullptr)
    {
        if (submitted) *submitted = false;
        if (!owner || !preroll) return GST_FLOW_ERROR;
        GstCaps *caps = gst_sample_get_caps(preroll);
        GstBuffer *source = gst_sample_get_buffer(preroll);
        if (!caps || !source) return GST_FLOW_ERROR;

        const gsize sourceSize = gst_buffer_get_size(source);
        if (sourceSize == 0) return GST_FLOW_ERROR;

        const GstClockTime sourceDuration = GST_BUFFER_DURATION(source);
        guint repeatCount = 6;
        if (GST_CLOCK_TIME_IS_VALID(sourceDuration) && sourceDuration > 0) {
            repeatCount = static_cast<guint>((targetDuration + sourceDuration - 1)
                                             / sourceDuration);
        }
        repeatCount = qBound<guint>(1, repeatCount, 16);
        if (sourceSize > G_MAXSIZE / repeatCount) return GST_FLOW_ERROR;

        GstBuffer *silence = gst_buffer_new_allocate(nullptr,
                                                      sourceSize * repeatCount,
                                                      nullptr);
        if (!silence) return GST_FLOW_ERROR;
        gst_buffer_memset(silence, 0, 0, sourceSize * repeatCount);
        if (GST_CLOCK_TIME_IS_VALID(sourceDuration)) {
            GST_BUFFER_DURATION(silence) = sourceDuration * repeatCount;
        }

        GstSample *silentSample = gst_sample_new(silence, caps, nullptr, nullptr);
        gst_buffer_unref(silence);
        if (!silentSample) return GST_FLOW_ERROR;
        // 前导静音是一段新提示音时间线的唯一校准点；其后的真实 PCM
        // 不再逐包重新对齐时钟，从而保持连续播放。
        const GstFlowReturn flow = pushSample(owner, silentSample, true, ringtoneToken, true, submitted);
        gst_sample_unref(silentSample);
        return flow;
    }

    /** @brief 旧解码器停止后释放提示输入；不在运行中的分支上逐元素重启。 */
    void resetPromptSource(GstPlayerWidget *owner)
    {
        removeSource(owner);
    }

    /** @return 提示分支已排队但尚未到播放时刻的数据时长，附带收尾余量。 */
    int promptDrainDelayMs(GstPlayerWidget *owner)
    {
        if (!owner) return 80;
        // 该函数由 Qt 主线程的 bus 定时器调用。音频流线程正好在创建分支
        // 时宁可采用安全的 80ms 收尾值，也不能让整个界面等待共享音频锁。
        if (!mutex_.tryLock(5)) {
            qWarning() << "[GStreamer-AUDIO] prompt drain lock busy; using fallback";
            return 80;
        }
        SourceBranch *branch = sources_.value(owner, nullptr);
        if (!branch || !branch->promptSource ||
                !GST_CLOCK_TIME_IS_VALID(branch->nextPts)) {
            mutex_.unlock();
            return 80;
        }

        const GstClockTime nextPts = branch->nextPts;
        GstElement *pipeline = pipeline_
                ? GST_ELEMENT(gst_object_ref(pipeline_)) : nullptr;
        mutex_.unlock();

        GstClockTime now = GST_CLOCK_TIME_NONE;
        GstClock *clock = pipeline ? gst_element_get_clock(pipeline) : nullptr;
        if (clock) {
            const GstClockTime clockNow = gst_clock_get_time(clock);
            const GstClockTime baseTime = gst_element_get_base_time(pipeline);
            if (GST_CLOCK_TIME_IS_VALID(clockNow) &&
                    GST_CLOCK_TIME_IS_VALID(baseTime) && clockNow >= baseTime) {
                now = clockNow - baseTime;
            }
            gst_object_unref(clock);
        }
        if (pipeline) gst_object_unref(pipeline);
        if (!GST_CLOCK_TIME_IS_VALID(now) || nextPts <= now) return 80;

        const guint64 remainingMs = (nextPts - now) / GST_MSECOND;
        return static_cast<int>(qBound<guint64>(80, remainingMs + 80, 1000));
    }

    /** @brief 从共享输入表移除播放器；其他输入仍活跃时发送 EOS 并在后台拆除。
     * @param[in] owner 借用的播放器身份；为空时直接返回，不查找输入。
     * @note 退役分支在 EOS 前丢弃残留缓冲，EOS 仍可通过探针进入混音器；物理拆除由后台
     *       在整条管线确认 NULL 后执行。EOS 提交后通知后台重新协商延迟。
     *       最后一个输入若未立即停稳，也交后台拆除。
     */
    void removeSource(GstPlayerWidget *owner)
    {
        if (!owner) return;
        SourceBranch *branch = nullptr;
        bool stopOutput = false;
        GstElement *retiredAppsrc=nullptr;
        bool deferred=false;
        {
            QMutexLocker locker(&mutex_);
            branch = sources_.take(owner);
            stopOutput=branch && sources_.isEmpty();
            if (branch && (callExclusive_ || !stopOutput)) {
                // 不在仍有其他输入的 PLAYING 管线上单独停 queue：板端曾卡在
                // gst_pad_stop_task，导致直播交接无限等待。
                if (branch->sourcePad) {
                    branch->retireProbeId=gst_pad_add_probe(
                                branch->sourcePad,GST_PAD_PROBE_TYPE_BUFFER,
                                &SharedAudioMixer::dropRetiredSourceProbe,nullptr,nullptr);
                    branch->retireEosProbeId=gst_pad_add_probe(
                                branch->sourcePad,GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                &SharedAudioMixer::observeRetiredEosProbe,branch,nullptr);
                }
                branch->retireStartedUs=g_get_monotonic_time();
                deferredSources_.push_back(branch);
                retiredAppsrc=branch->appsrc
                        ? GST_ELEMENT(gst_object_ref(branch->appsrc)) : nullptr;
                deferred=true;
            } else if (branch) {
                ++activeRemovals_;
            }
        }
        if (deferred) {
            // end-of-stream 可与解码线程的 push-buffer 竞争，不能持有输入表锁。
            GstFlowReturn eosFlow=GST_FLOW_ERROR;
            if (retiredAppsrc) {
                g_signal_emit_by_name(retiredAppsrc,"end-of-stream",&eosFlow);
                gst_object_unref(retiredAppsrc);
            }
            latencyPending_.store(true);
            requestRetirement();
            return;
        }
        if (!branch) return;

        // 状态切换和动态拆分支绝不能持有 pushSample 使用的共享锁。
        // pushSample 已对 appsrc 单独持有引用，并在调用 push-buffer 前释放
        // 共享锁，因此这里不需要发送 flush-start。共享 mixer 若只收到
        // flush-start 而没有配对 flush-stop，后续提示音和视频 PCM 都会被丢弃。
        bool lastSourceDeferred=false;
        if (stopOutput && pipeline_) {
            // 最后一个输入已经 EOS/停止时，先停整条共享输出，再拆分支。
            // 这样不会在运行中的 audiomixer 上动态删除最后一个 request pad。
            // 只做零等待状态确认；未停稳时转后台，避免在 Qt 线程等待。
            const auto result=gst_element_set_state(pipeline_, GST_STATE_NULL);
            GstState current=GST_STATE_VOID_PENDING;
            GstState pending=GST_STATE_VOID_PENDING;
            const auto settled=gst_element_get_state(pipeline_,&current,&pending,0);
            lastSourceDeferred=result==GST_STATE_CHANGE_FAILURE ||
                    settled==GST_STATE_CHANGE_FAILURE ||
                    current!=GST_STATE_NULL || pending!=GST_STATE_VOID_PENDING;
        }
        if (lastSourceDeferred) {
            QMutexLocker locker(&mutex_);
            branch->retireStartedUs=g_get_monotonic_time();
            deferredSources_.push_back(branch);
        } else {
            destroySourceLocked(branch);
        }
        {
            QMutexLocker locker(&mutex_);
            --activeRemovals_;
            removalFinished_.wakeAll();
        }
        if (lastSourceDeferred) requestRetirement();
    }

private:
    /** @brief 共享混音输入收到音频缓冲时的处置。 */
    enum class AudioDisposition {
        Drop, /**< 音源失焦或通话独占，禁止进入混音。 */
        Play, /**< 保持此输入原有音量。 */
        Mute /**< 视频继续供帧，混音器保留其时间线并静音此输入。 */
    };

    /** @brief 区分视频静音和必须禁止入队的输入。
     * @param[in] owner 只比较的播放器身份。
     * @param[in] token 通话铃声代次，其他输入为0。
     * @param[in] managed true 表示输入需要焦点，失焦后可暂停或清理。
     * @return 通话与铃声维持排他策略，普通媒体被抢占时保留时序。
     */
    AudioDisposition audioDisposition(const void *owner, uint64_t token, bool managed) const
    {
        const auto ringtone = ringtoneToken_.load();
        if (ringtone || token) return ringtone == token ? AudioDisposition::Play : AudioDisposition::Drop;

        const auto selected = priorityOwner_.load();
        if (selected == this) return AudioDisposition::Drop;
        if (!selected) {
            if (managed) return AudioDisposition::Drop;
            return videoAudioHeld_.load() || broadcastRecoveryPending_.load()
                    ? AudioDisposition::Mute : AudioDisposition::Play;
        }
        if (selected == owner) return AudioDisposition::Play;
        return managed ? AudioDisposition::Drop : AudioDisposition::Mute;
    }

    /** @brief pad 探针的非拥有身份及分支输入时间指针，永远不解引用播放器。
     * @note 时间指针指向分支成员，分支在移除探针并停止管线后释放。
     */
    struct AudioGate {
        const void *owner; /**< 创建分支的播放器身份。 */
        uint64_t token; /**< 铃声代次，普通输入为0。 */
        bool managed; /**< 是否为需要焦点的输入。 */
        std::atomic<gint64> *lastInputUs; /**< 借用分支的最近输入单调时钟，微秒。 */
        bool muted = false; /**< 该 pad 当前静音值，由此 pad 的串行缓冲探针维护。 */
    };

    /** @brief 复核焦点，在混音输入静音视频并记录最近送样时间。
     * @param[in] pad 当前分支的 audiomixer 输入，只调整此 pad 的 mute 属性。
     * @param[in] info 借用的缓冲探针信息，用于确认送样。
     * @param[in] data 探针拥有的分支代次，生命周期由 pad 管理，不访问播放器对象。
     * @return 受管失焦或通话独占时丢弃，其余缓冲保留。
     * @note 最近输入时间用于后台判定活动输入是否仍送样；逐帧路径不输出日志。
     */
    static GstPadProbeReturn applyQueuedAudioGate(GstPad *pad, GstPadProbeInfo *info, gpointer data)
    {
        auto &gate = *static_cast<AudioGate *>(data);
        if (GST_PAD_PROBE_INFO_BUFFER(info)) {
            gate.lastInputUs->store(g_get_monotonic_time());
        }
        const auto disposition = instance().audioDisposition(gate.owner, gate.token, gate.managed);
        if (disposition == AudioDisposition::Drop) return GST_PAD_PROBE_DROP;

        const bool muted = disposition == AudioDisposition::Mute;
        if (gate.muted != muted) {
            g_object_set(G_OBJECT(pad), "mute", static_cast<gboolean>(muted), nullptr);
            gate.muted = muted;
        }
        return GST_PAD_PROBE_OK;
    }

    /** @brief 丢弃已从输入表退役的分支缓冲，避免延迟拆除期间旧音频混入。
     * @return GST_PAD_PROBE_DROP；探针仅挂在缓冲路径，不影响状态事件。
     */
    static GstPadProbeReturn dropRetiredSourceProbe(GstPad *,GstPadProbeInfo *,gpointer)
    {
        return GST_PAD_PROBE_DROP;
    }

    /** @brief 一个播放器的共享输入元素、最近送样时间及退役期间的 EOS 状态。 */
    struct SourceBranch {
        GstElement *appsrc = nullptr;
        GstElement *queue = nullptr;
        GstElement *latencyQueue = nullptr; /**< 普通输入独有的阻塞缓冲；在原丢旧数据队列后提供 200ms 最大等待余量。 */
        GstElement *convert = nullptr;
        GstElement *resample = nullptr;
        GstPad *sourcePad = nullptr;
        GstPad *mixerPad = nullptr;
        gulong retireProbeId = 0; /**< 退役时挂在 sourcePad 的丢弃探针；物理拆除前移除。 */
        gulong retireEosProbeId = 0; /**< 退役分支的 EOS 事件观察探针编号；拆除前移除。 */
        std::atomic_bool retireEosSeen{false}; /**< true 表示退役分支的 EOS 已到达 sourcePad。 */
        gint64 retireStartedUs=0; /**< 退役起点，单调时钟微秒；用于后台有界等待 EOS。 */
        GstCaps *caps = nullptr;
        bool promptSource = false;
        GstClockTime nextPts = GST_CLOCK_TIME_NONE;
        std::atomic<gint64> mixerLastBufferUs{0}; /**< 最近到达混音输入 pad 的单调时钟微秒数。 */
    };

    /** @brief 只记录退役分支是否把 EOS 送到混音输入前，不拦截事件。
     * @param[in] info 借用的下行事件信息。
     * @param[in] data 借用的分支指针；销毁分支前必须先移除探针。
     * @return GST_PAD_PROBE_OK，保持 EOS 原有传播路径。
     */
    static GstPadProbeReturn observeRetiredEosProbe(GstPad *,GstPadProbeInfo *info,gpointer data)
    {
        GstEvent *event=GST_PAD_PROBE_INFO_EVENT(info);
        if (event && GST_EVENT_TYPE(event)==GST_EVENT_EOS)
            static_cast<SourceBranch *>(data)->retireEosSeen.store(true);
        return GST_PAD_PROBE_OK;
    }

    /** @brief 启动专用退役工作线程，避免在 Qt 或 GStreamer 回调线程停止运行中的 queue。 */
    SharedAudioMixer() : retirementThread_([this] { retireDeferredLoop(); }) {}
    /** @brief 停止共享管线，移除 sink 断流探针并释放共享分支和输出对象。
     * @note 先通知并等待退役线程退出，再持有输入表锁拆除管线。
     */
    ~SharedAudioMixer()
    {
        {
            std::lock_guard<std::mutex> lock(retirementMutex_);
            retirementStop_=true;
        }
        retirementChanged_.notify_all();
        if (retirementThread_.joinable()) retirementThread_.join();
        QMutexLocker locker(&mutex_);
        // 先停整条管线，再拆 queue/request pad；与运行时退役分支遵循同一顺序。
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
        }
        for (auto *branch:deferredSources_) destroySourceLocked(branch);
        deferredSources_.clear();
        while (!sources_.isEmpty()) {
            auto it = sources_.begin();
            SourceBranch *branch = it.value();
            sources_.erase(it);
            destroySourceLocked(branch);
        }
        removeDiagnosticProbesLocked();
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

    /** @brief 通知后台处理退役分支、共享输出故障或延迟变化；不等待声卡交接。 */
    void requestRetirement()
    {
        {
            std::lock_guard<std::mutex> lock(retirementMutex_);
            retirementPending_=true;
        }
        retirementChanged_.notify_one();
    }

    /** @brief 声卡重开失败时列出本进程仍持有的 ALSA PCM 文件描述符。
     * @param[in] reason 日志中的失败阶段，不包含设备或用户输入。
     * @note 调用方持有 outputMutex_；仅定位同进程句柄，其他进程仍需用 fuser 核实。
     */
    void logOwnAlsaHandles(const char *reason)
    {
#ifdef Q_OS_LINUX
        const QDir fdDirectory(QStringLiteral("/proc/self/fd"));
        QStringList handles;
        for (const QString &name:fdDirectory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            const QString target=QFileInfo(fdDirectory.filePath(name)).symLinkTarget();
            if (target.startsWith(QStringLiteral("/dev/snd/pcm")))
                handles.append(name + QLatin1Char('=') + target);
        }
        qWarning() << "[audio-boundary] own ALSA handles" << "reason=" << reason
                   << "pid=" << getpid() << "handles=" << handles
                   << "broadcastOwned=" << (broadcastStop_!=nullptr)
                   << "callRequested=" << callOutputRequested_;
#else
        Q_UNUSED(reason);
#endif
    }

    /** @brief 在退役后台线程为当前活动共享管线重新查询并分发延迟。
     * @details 总线同步回调只置位请求；本函数在 outputMutex_ 串行区间调用，
     *          先短暂取得输入表锁复制管线引用，再在无输入表锁时查询 GStreamer。
     * @note 独占交接期间保留请求，恢复输入门后再处理；无活动输入或旧管线故障时丢弃旧请求。
     */
    void recalculateSharedLatency()
    {
        if (!latencyPending_.exchange(false)) return;
        GstElement *pipeline=nullptr;
        bool exclusive=false;
        {
            QMutexLocker lock(&mutex_);
            exclusive=callExclusive_;
            if (pipeline_ && !exclusive && !sources_.isEmpty() && !outputFault_.load())
                pipeline=GST_ELEMENT(gst_object_ref(pipeline_));
        }
        if (!pipeline) {
            if (exclusive && !outputFault_.load()) latencyPending_.store(true);
            return;
        }
        const gboolean configured=gst_bin_recalculate_latency(GST_BIN(pipeline));
        if (!configured) {
            const gint64 nowUs=g_get_monotonic_time();
            gint64 previousUs=lastLatencyFailureUs_.load();
            if ((previousUs==0 || nowUs-previousUs>=5 * G_USEC_PER_SEC) &&
                    lastLatencyFailureUs_.compare_exchange_strong(previousUs,nowUs))
                qWarning() << "[GStreamer-AUDIO] shared latency recalculation failed";
        }
        gst_object_unref(pipeline);
    }

    /** @brief 处理延迟重算并监视退役输入和输出故障，只在确实断流时重启共享管线。
     * @details 无活动输入时确认 NULL 并拆旧分支，不重开声卡；健康输出的退役分支
     *          保留到下一次既有 NULL 交接。活动输入最近持续送样、sink 超过一秒无输出且退役已
     *          超过一秒时，才停止整条管线清理旧 pad。
     * @note 只在专用线程调用；健康退役每秒复查一次，延迟重算最密每 500ms 一次，
     *       不在回调线程阻塞。
     */
    void retireDeferredLoop()
    {
        gint64 lastFaultRecoveryUs=0;
        gint64 lastLatencyRecalcUs=0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(retirementMutex_);
                retirementChanged_.wait(lock,[this] { return retirementPending_ || retirementStop_; });
                if (retirementStop_) return;
                retirementPending_=false;
                retirementChanged_.wait_for(lock,std::chrono::milliseconds(100),
                                            [this] { return retirementStop_; });
                if (retirementStop_) return;
            }
            std::unique_lock<std::mutex> outputLock(outputMutex_,std::try_to_lock);
            if (!outputLock) {
                requestRetirement();
                continue;
            }
            const gint64 latencyNowUs=g_get_monotonic_time();
            if (!callOutputRequested_ && !broadcastStop_ && latencyPending_.load() &&
                    lastLatencyRecalcUs>0 &&
                    latencyNowUs-lastLatencyRecalcUs<500 * G_TIME_SPAN_MILLISECOND) {
                const auto remaining=std::chrono::microseconds(
                            500 * G_TIME_SPAN_MILLISECOND-
                            (latencyNowUs-lastLatencyRecalcUs));
                outputLock.unlock();
                std::unique_lock<std::mutex> waitLock(retirementMutex_);
                retirementPending_=true;
                retirementChanged_.wait_for(waitLock,remaining,
                                            [this] { return retirementStop_; });
                if (retirementStop_) return;
                continue;
            }
            if (!callOutputRequested_ && !broadcastStop_ && latencyPending_.load()) {
                recalculateSharedLatency();
                lastLatencyRecalcUs=latencyNowUs;
            }
            if (callOutputRequested_ || broadcastStop_ || priorityOwner_.load()==this ||
                    ringtoneToken_.load()!=0) {
                continue;
            }
            int deferredCount=0;
            bool promptActive=false;
            bool allEosSeen=true;
            qint64 oldestAgeMs=0;
            bool activeInputRecent=false;
            const gint64 nowUs=g_get_monotonic_time();
            {
                QMutexLocker lock(&mutex_);
                deferredCount=deferredSources_.size();
                for (auto *branch:deferredSources_) {
                    allEosSeen=allEosSeen && branch->retireEosSeen.load();
                    oldestAgeMs=qMax<qint64>(oldestAgeMs,
                            (g_get_monotonic_time()-branch->retireStartedUs)/1000);
                }
                for (auto *branch:sources_) {
                    promptActive=promptActive || branch->promptSource;
                    const gint64 lastInputUs=branch->mixerLastBufferUs.load();
                    activeInputRecent=activeInputRecent ||
                            (lastInputUs>0 && nowUs-lastInputUs<500 * G_TIME_SPAN_MILLISECOND);
                }
            }
            const bool fault=outputFault_.load();
            if (!fault && deferredCount==0) {
                continue;
            }
            if (!fault && deferredCount>0) {
                bool idleOutput=false;
                {
                    QMutexLocker lock(&mutex_);
                    if (!callExclusive_ && sources_.isEmpty()) {
                        // 阻止新分支在本次 NULL 确认和退役 pad 拆除期间启动输出。
                        callExclusive_=true;
                        idleOutput=true;
                    }
                }
                if (idleOutput) {
                    const bool stopped=setCallExclusive(true,200 * GST_MSECOND);
                    const bool resumed=setCallExclusive(false); // 无活动输入时仍保持 NULL。
                    qInfo() << "[audio-boundary] idle output retirement"
                            << "deferred=" << deferredCount << "nullConfirmed=" << stopped
                            << "inputGateReleased=" << resumed;
                    if (stopped && resumed) {
                        continue;
                    }
                }
            }
            if (fault && lastFaultRecoveryUs>0 && nowUs-lastFaultRecoveryUs<2 * G_USEC_PER_SEC) {
                const auto remainingMs=std::chrono::milliseconds(
                            (2 * G_USEC_PER_SEC-(nowUs-lastFaultRecoveryUs))/1000);
                outputLock.unlock();
                std::unique_lock<std::mutex> waitLock(retirementMutex_);
                retirementPending_=true;
                retirementChanged_.wait_for(waitLock,remainingMs,
                                            [this] { return retirementStop_; });
                if (retirementStop_) return;
                continue;
            }
            const gint64 sinkLastUs=sinkLastBufferUs_.load();
            const qint64 sinkAgeMs=sinkLastUs>0
                    ? qMax<qint64>(0,(g_get_monotonic_time()-sinkLastUs)/1000) : -1;
            const bool sinkStalled=sinkAgeMs<0 || sinkAgeMs>=1000;
            if (!fault && !(oldestAgeMs>=1000 && activeInputRecent && sinkStalled)) {
                outputLock.unlock();
                std::unique_lock<std::mutex> waitLock(retirementMutex_);
                retirementChanged_.wait_for(waitLock,std::chrono::seconds(1),
                                            [this] { return retirementStop_ || retirementPending_; });
                if (retirementStop_) return;
                retirementPending_=true;
                continue;
            }
            qInfo() << "[audio-boundary] shared retirement recovery begin"
                    << "deferred=" << deferredCount << "promptActive=" << promptActive
                    << "allEosSeen=" << allEosSeen << "oldestAgeMs=" << oldestAgeMs
                    << "sinkAgeMs=" << sinkAgeMs << "activeInputRecent=" << activeInputRecent
                    << "fault=" << fault;
            if (fault) logOwnAlsaHandles("output-fault");
            bool recovered=false;
            if (fault) {
                lastFaultRecoveryUs=nowUs;
                recovered=resetSharedOutput();
            } else {
                const bool stopped=setCallExclusive(true,200 * GST_MSECOND);
                const bool resumed=setCallExclusive(false);
                recovered=stopped && resumed;
                if (!recovered) recovered=resetSharedOutput();
            }
            qInfo() << "[audio-boundary] shared retirement recovery end"
                    << "success=" << recovered;
            if (!recovered) {
                outputLock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                requestRetirement();
            }
        }
    }

    SharedAudioMixer(const SharedAudioMixer &) = delete;
    SharedAudioMixer &operator=(const SharedAudioMixer &) = delete;

    /** @brief 在共享管线停止后移除 sink 断流时间探针并归还 pad 引用。
     * @note 调用方持有 mutex_；不操作业务音频分支。
     */
    void removeDiagnosticProbesLocked()
    {
        if (sinkProbePad_) {
            if (sinkProbeId_) gst_pad_remove_probe(sinkProbePad_,sinkProbeId_);
            gst_object_unref(sinkProbePad_);
            sinkProbePad_=nullptr;
            sinkProbeId_=0;
        }
    }

    /** @brief 记录共享声卡 sink 最近收到缓冲的时间，供退役恢复判断断流。
     * @param[in] info 借用的 pad 探针信息；仅检查缓冲是否存在。
     * @param[in] data 借用的共享混音器实例。
     * @return GST_PAD_PROBE_OK，保持原音频流。
     */
    static GstPadProbeReturn sinkInputProbe(GstPad *,GstPadProbeInfo *info,gpointer data)
    {
        if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
            return GST_PAD_PROBE_OK;
        if (GST_PAD_PROBE_INFO_BUFFER(info))
            static_cast<SharedAudioMixer *>(data)->sinkLastBufferUs_.store(g_get_monotonic_time());
        return GST_PAD_PROBE_OK;
    }

    /** @brief 在 GStreamer 总线回调线程处理共享故障、延迟变化及恢复状态。
     * @param[in] message 借用的总线消息；本函数读取但不释放。
     * @param[in] data 借用的 SharedAudioMixer 实例指针。
     * @return GST_BUS_DROP，保持原有同步消费消息行为。
     * @note 直播恢复的 PLAYING 消息仍清除恢复标志；错误消息置位故障并唤醒后台重建。
     *       LATENCY 只通知退役后台线程，不能在此同步总线回调线程执行延迟查询。
     */
    static GstBusSyncReply busSyncHandler(GstBus *, GstMessage *message, gpointer data)
    {
        auto *mixer = static_cast<SharedAudioMixer *>(data);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED &&
                mixer->broadcastRecoveryPending_.load() && GST_IS_PIPELINE(GST_MESSAGE_SRC(message))) {
            GstState current;
            gst_message_parse_state_changed(message,nullptr,&current,nullptr);
            if (current == GST_STATE_PLAYING && mixer->broadcastRecoveryPending_.exchange(false))
                qInfo() << "[GStreamer-AUDIO] rebuilt shared output playing";
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_LATENCY) {
            if (!mixer->latencyPending_.exchange(true))
                mixer->requestRetirement();
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            mixer->outputFault_.store(true);
            mixer->requestRetirement();
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
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
            GError *warning=nullptr;
            gchar *debug=nullptr;
            gst_message_parse_warning(message,&warning,&debug);
            const bool latencyWarning=debug &&
                    (std::strstr(debug,"Impossible to configure latency") ||
                     std::strstr(debug,"Not enough buffering available"));
            bool shouldLog=true;
            quint64 suppressed=0;
            if (latencyWarning) {
                const gint64 nowUs=g_get_monotonic_time();
                gint64 previousUs=mixer->lastLatencyWarningUs_.load();
                shouldLog=previousUs==0 || nowUs-previousUs>=5 * G_USEC_PER_SEC;
                if (shouldLog && mixer->lastLatencyWarningUs_.compare_exchange_strong(previousUs,nowUs))
                    suppressed=mixer->suppressedLatencyWarnings_.exchange(0);
                else shouldLog=false;
                if (!shouldLog) mixer->suppressedLatencyWarnings_.fetch_add(1);
            }
            if (shouldLog)
                qWarning() << "[audio-path] shared warning" << "source=" << GST_MESSAGE_SRC(message)
                           << "message=" << (warning ? QString::fromUtf8(warning->message) : QString())
                           << "debug=" << (debug ? QString::fromUtf8(debug) : QString())
                           << "suppressedSincePrevious=" << suppressed;
            if (warning) g_error_free(warning);
            if (debug) g_free(debug);
        }
        return GST_BUS_DROP;
    }

    /** @brief 创建共享混音及单一输出管线，并安装 sink 断流时间探针。
     * @param[out] error 可空；缺少元素或连线失败时写入原因。
     * @return true 表示管线已存在或创建完成；false 表示创建失败。
     * @note 调用方持有输入表锁；探针在管线停止后由 removeDiagnosticProbesLocked 移除。
     *       混音后的阻塞队列最多缓存 200ms，供声卡处理期限与输入延迟协商。
     */
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
        GstElement *outputQueue = gst_element_factory_make("queue", "qt-ycest-audio-output-queue");
        GstElement *convert = gst_element_factory_make("audioconvert", "qt-ycest-audio-convert");
        GstElement *resample = gst_element_factory_make("audioresample", "qt-ycest-audio-resample");

        QStringList candidates;
        const QString configured = Rk3566Platform::gstAudioSink();
        if (!configured.isEmpty()) candidates << configured;
        candidates << QStringLiteral("alsasink") << QStringLiteral("autoaudiosink");
        GstElement *sink = makeFirstAvailable(candidates);

        if (!pipeline || !mixer || !outputQueue || !convert || !resample || !sink) {
            if (error) *error = QStringLiteral("missing audiomixer/audio queue/conversion/output element");
            if (pipeline) gst_object_unref(pipeline);
            if (mixer) gst_object_unref(mixer);
            if (outputQueue) gst_object_unref(outputQueue);
            if (convert) gst_object_unref(convert);
            if (resample) gst_object_unref(resample);
            if (sink) gst_object_unref(sink);
            return false;
        }

        setBooleanProperty(G_OBJECT(mixer), "ignore-inactive-pads", TRUE);
        g_object_set(outputQueue,
                     "leaky", 0,
                     "max-size-buffers", static_cast<guint>(0),
                     "max-size-bytes", static_cast<guint>(0),
                     "max-size-time", static_cast<guint64>(200 * GST_MSECOND),
                     nullptr);
        setBooleanProperty(G_OBJECT(sink), "sync", TRUE);
        setBooleanProperty(G_OBJECT(sink), "async", TRUE);
        if (hasProperty(G_OBJECT(sink), "device")) {
            const QByteArray device = Rk3566Platform::alsaDevice().toUtf8();
            if (!device.isEmpty()) g_object_set(sink, "device", device.constData(), nullptr);
        }

        gst_bin_add_many(GST_BIN(pipeline), mixer, outputQueue, convert, resample, sink, nullptr);
        if (!gst_element_link_many(mixer, outputQueue, convert, resample, sink, nullptr)) {
            if (error) *error = QStringLiteral("cannot link shared audio output pipeline");
            gst_object_unref(pipeline);
            return false;
        }

        pipeline_ = pipeline;
        mixer_ = mixer;
        outputSink_ = sink;
        sinkLastBufferUs_.store(0);
        sinkProbePad_=gst_element_get_static_pad(outputSink_,"sink");
        if (sinkProbePad_)
            sinkProbeId_=gst_pad_add_probe(sinkProbePad_,GST_PAD_PROBE_TYPE_BUFFER,
                                          &SharedAudioMixer::sinkInputProbe,this,nullptr);
        bus_ = gst_element_get_bus(pipeline_);
        if (bus_) {
            gst_bus_set_sync_handler(bus_, &SharedAudioMixer::busSyncHandler, this, nullptr);
        }
        qInfo() << "[GStreamer-AUDIO] shared mixer initialized; one process-wide output sink";
        return true;
    }

    /** @brief 确保解码输入分支存在；连续时间线关闭 appsrc 自动打时戳。
     * @param[in] owner 借用的播放器身份，用作分支键。
     * @param[out] error 可空；失败时写入无法创建分支的原因。
     * @param[in] promptSource true 使用提示音的缓冲策略。
     * @param[in] ringtoneToken 铃声代次，普通音频为0。
     * @param[in] continuousTimeline true 由调用方连续提供 PCM 时间戳。
     * @return 借用的分支，失败返回空指针。
     * @note 调用方持有混音器锁；提示音与普通直播分别沿用自己的队列策略。
     *       普通输入原队列仍丢弃旧缓冲，后接不丢数据的 200ms 队列，避免
     *       leaky 队列把混音器可查询的最大延迟压成零。
     *       启动失败时先确认整管线 NULL，再拆本次新建分支；未确认时交后台重建。
     */
    SourceBranch *ensureSourceLocked(GstPlayerWidget *owner,
                                     QString *error,
                                     bool promptSource, uint64_t ringtoneToken, bool continuousTimeline)
    {
        auto existing = sources_.constFind(owner);
        if (existing != sources_.constEnd()) return existing.value();
        if (!ensurePipelineLocked(error)) return nullptr;

        SourceBranch *branch = new SourceBranch;
        branch->promptSource = promptSource;
        branch->appsrc = gst_element_factory_make("appsrc", nullptr);
        branch->queue = gst_element_factory_make("queue", nullptr);
        if (!promptSource)
            branch->latencyQueue = gst_element_factory_make("queue", nullptr);
        branch->convert = gst_element_factory_make("audioconvert", nullptr);
        branch->resample = gst_element_factory_make("audioresample", nullptr);
        if (!branch->appsrc || !branch->queue ||
                (!promptSource && !branch->latencyQueue) ||
                !branch->convert || !branch->resample) {
            if (error) *error = QStringLiteral("cannot create shared audio input branch");
            if (branch->appsrc) gst_object_unref(branch->appsrc);
            if (branch->queue) gst_object_unref(branch->queue);
            if (branch->latencyQueue) gst_object_unref(branch->latencyQueue);
            if (branch->convert) gst_object_unref(branch->convert);
            if (branch->resample) gst_object_unref(branch->resample);
            delete branch;
            return nullptr;
        }

        g_object_set(branch->appsrc,
                     "is-live", TRUE,
                     "format", GST_FORMAT_TIME,
                     "do-timestamp", continuousTimeline ? FALSE : TRUE,
                      // pushSample 不等待 appsrc 队列；否则在状态交接时仍可能
                      // 占住解码回调线程并延迟广播或视频分支的停止。
                     "block", FALSE,
                     nullptr);
        g_object_set(branch->queue,
                     "leaky", promptSource ? 0 : 2,
                     "max-size-buffers", static_cast<guint>(promptSource ? 64 : 32),
                     nullptr);
        if (branch->latencyQueue) {
            g_object_set(branch->latencyQueue,
                         "leaky", 0,
                         "max-size-buffers", static_cast<guint>(0),
                         "max-size-bytes", static_cast<guint>(0),
                         "max-size-time", static_cast<guint64>(200 * GST_MSECOND),
                         nullptr);
        }

        gst_bin_add_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                         branch->convert, branch->resample, nullptr);
        if (branch->latencyQueue)
            gst_bin_add(GST_BIN(pipeline_), branch->latencyQueue);
        const bool linked=branch->latencyQueue
                ? gst_element_link_many(branch->appsrc, branch->queue,
                                        branch->latencyQueue, branch->convert,
                                        branch->resample, nullptr)
                : gst_element_link_many(branch->appsrc, branch->queue,
                                        branch->convert, branch->resample, nullptr);
        if (!linked) {
            if (error) *error = QStringLiteral("cannot link shared audio input branch");
            if (branch->latencyQueue)
                gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                    branch->latencyQueue, branch->convert,
                                    branch->resample, nullptr);
            else
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
            if (branch->latencyQueue)
                gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                    branch->latencyQueue, branch->convert,
                                    branch->resample, nullptr);
            else
                gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                    branch->convert, branch->resample, nullptr);
            delete branch;
            return nullptr;
        }

        gst_pad_add_probe(
                    branch->mixerPad,GST_PAD_PROBE_TYPE_BUFFER,&applyQueuedAudioGate,
                    new AudioGate{owner, ringtoneToken, owner->priorityManaged(),
                                  &branch->mixerLastBufferUs},
                    [](gpointer gate) { delete static_cast<AudioGate *>(gate); });
        sources_.insert(owner, branch);
        gst_element_sync_state_with_parent(branch->appsrc);
        gst_element_sync_state_with_parent(branch->queue);
        if (branch->latencyQueue)
            gst_element_sync_state_with_parent(branch->latencyQueue);
        gst_element_sync_state_with_parent(branch->convert);
        gst_element_sync_state_with_parent(branch->resample);
        const auto pipelineResult=gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (pipelineResult == GST_STATE_CHANGE_FAILURE) {
            sources_.remove(owner);
            const auto nullResult=gst_element_set_state(pipeline_, GST_STATE_NULL);
            GstState current=GST_STATE_VOID_PENDING;
            const auto settled=gst_element_get_state(
                        pipeline_,&current,nullptr,200 * GST_MSECOND);
            const bool stopped=nullResult!=GST_STATE_CHANGE_FAILURE &&
                    settled!=GST_STATE_CHANGE_FAILURE &&
                    settled!=GST_STATE_CHANGE_ASYNC && current==GST_STATE_NULL;
            qWarning() << "[audio-boundary] failed input activation stopped"
                       << "branch=" << branch << "nullResult=" << int(nullResult)
                       << "waitResult=" << int(settled) << "current=" << int(current)
                       << "confirmed=" << stopped;
            if (stopped) destroySourceLocked(branch);
            else {
                branch->retireStartedUs=g_get_monotonic_time();
                deferredSources_.push_back(branch);
            }
            outputFault_.store(true);
            requestRetirement();
            if (error) *error = QStringLiteral("cannot start shared audio mixer");
            return nullptr;
        }

        qInfo() << "[GStreamer-AUDIO] shared input attached" << "owner=" << owner
                << "prompt=" << promptSource << "managed=" << owner->priorityManaged()
                << "sources=" << sources_.size();
        return branch;
    }

    /** @brief 从已停止的共享管线拆除元素和 request pad。
     * @param[in] branch 拥有的分支指针；调用后释放，不得再次访问。
     * @note 运行时调用方负责先确认整条管线 NULL；析构退出仅能尽力停止。
     *       不在仍运行的单个 queue 上执行停止，避免阻塞其他输出。
     */
    void destroySourceLocked(SourceBranch *branch)
    {
        if (!branch) return;
        if (branch->appsrc) gst_element_set_state(branch->appsrc, GST_STATE_NULL);
        if (branch->queue) gst_element_set_state(branch->queue, GST_STATE_NULL);
        if (branch->latencyQueue) gst_element_set_state(branch->latencyQueue, GST_STATE_NULL);
        if (branch->convert) gst_element_set_state(branch->convert, GST_STATE_NULL);
        if (branch->resample) gst_element_set_state(branch->resample, GST_STATE_NULL);

        if (branch->retireProbeId && branch->sourcePad)
            gst_pad_remove_probe(branch->sourcePad,branch->retireProbeId);
        if (branch->retireEosProbeId && branch->sourcePad)
            gst_pad_remove_probe(branch->sourcePad,branch->retireEosProbeId);
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
            if (branch->latencyQueue)
                gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                    branch->latencyQueue, branch->convert,
                                    branch->resample, nullptr);
            else
                gst_bin_remove_many(GST_BIN(pipeline_), branch->appsrc, branch->queue,
                                    branch->convert, branch->resample, nullptr);
        }
        delete branch;
    }

    QVector<SourceBranch *> deferredSources_; /**< 已发 EOS、等待后台或独占交接停止共享管线后物理拆除的分支。 */
    int activeRemovals_=0; /**< 最后输入的在途拆除数；独占后台有界等待其清零。 */
    QWaitCondition removalFinished_; /**< 最后输入拆除完成时唤醒有界等待的交接后台。 */
    std::atomic_bool videoAudioHeld_{false}; /**< 功放恢复前继续静音无焦点的视频输入。 */
    std::atomic<const void *> priorityOwner_{nullptr}; /**< 主线程授予的输出身份，混音线程只比较。 */
    std::atomic<uint64_t> ringtoneToken_{0}; /**< 已选铃声代次；0 时常规输出可混音。 */
    uint64_t nextRingtoneToken_=1; /**< 仅 UI 线程递增，1 保留给已停止的铃声播放器。 */
    std::mutex outputMutex_; /**< 串行直播与通话的声卡交接，不保护实时 PCM。 */
    std::condition_variable outputChanged_; /**< 直播关闭 ALSA 后通知后台通话。 */
    bool callOutputRequested_ = false; /**< 交接锁保护，通话等待期间禁止重新接管。 */
    std::atomic_bool *broadcastStop_ = nullptr; /**< 交接锁保护，借用到直播归还为止。 */
    std::atomic_bool broadcastOutputActive_{false}; /**< 直播独占期间提示音应等待声卡恢复。 */
    std::atomic_bool broadcastRecoveryPending_{false}; /**< 故障重建期间只放行视频静音 PCM，待共享输出 PLAYING。 */
    std::atomic_bool outputFault_{false}; /**< Gst 总线报告的共享输出错误，每次直播归还开始时重置。 */
    std::atomic_bool latencyPending_{false}; /**< 总线 LATENCY 消息置位；退役后台线程消费并重算。 */
    std::atomic<gint64> lastLatencyFailureUs_{0}; /**< 最近打印重算失败的单调时钟微秒数；防止故障期刷屏。 */
    std::atomic<gint64> lastLatencyWarningUs_{0}; /**< 最近打印延迟警告的单调时钟微秒数。 */
    std::atomic<quint64> suppressedLatencyWarnings_{0}; /**< 最近一条延迟警告后被压缩的重复次数。 */
    bool callExclusive_=false; /**< mutex_ 保护；true 时丢弃所有媒体/提示 PCM，后台关闭共享输出。 */
    QMutex mutex_;
    QHash<GstPlayerWidget *, SourceBranch *> sources_;
    GstElement *pipeline_ = nullptr;
    GstElement *mixer_ = nullptr;
    GstElement *outputSink_ = nullptr;
    GstBus *bus_ = nullptr;
    GstPad *sinkProbePad_ = nullptr; /**< 拥有的声卡 sink 输入 pad 引用；停止后移除断流探针。 */
    gulong sinkProbeId_ = 0; /**< sink 断流时间探针编号；0 表示未安装。 */
    std::atomic<gint64> sinkLastBufferUs_{0}; /**< 最近一个 sink 输入缓冲的单调时钟微秒数。 */
    std::mutex retirementMutex_; /**< 保护退役线程的待办和退出标志，不与 GStreamer 锁嵌套持有。 */
    std::condition_variable retirementChanged_; /**< 退役输入或声卡故障时唤醒后台线程。 */
    bool retirementPending_=false; /**< true 表示有新的退役或故障待后台处理。 */
    bool retirementStop_=false; /**< true 表示析构已要求后台线程退出。 */
    std::thread retirementThread_; /**< 唯一后台退役线程，析构前 join。 */
};

/**
 * @brief 尝试让管线进入 NULL，并核对当前状态和待切换状态。
 * @param[in] element 借用的元素，可为空；调用方负责引用和串行状态切换。
 * @return true 表示元素为空或已经确认处于 NULL 且无待切状态。
 * @note 100 ms 只限制 ASYNC 后的显式等待，FLUSH_START 和 set_state 本身可能阻塞。
 */
bool setNullStateAndWait(GstElement *element)
{
    if (!element) return true;

    GstState before = GST_STATE_NULL;
    GstState beforePending = GST_STATE_VOID_PENDING;
    const auto beforeResult = gst_element_get_state(element, &before, &beforePending, 0);
    if (beforeResult != GST_STATE_CHANGE_FAILURE &&
        before == GST_STATE_NULL && beforePending == GST_STATE_VOID_PENDING) {
        return true;
    }

    gst_element_send_event(element, gst_event_new_flush_start());
    const auto setResult = gst_element_set_state(element, GST_STATE_NULL);
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const auto result = gst_element_get_state(element, &current, &pending,
            setResult == GST_STATE_CHANGE_ASYNC ? 100 * GST_MSECOND : 0);
    const bool stopped = setResult != GST_STATE_CHANGE_FAILURE &&
            result != GST_STATE_CHANGE_FAILURE && result != GST_STATE_CHANGE_ASYNC &&
            current == GST_STATE_NULL && pending == GST_STATE_VOID_PENDING;
    return stopped;
}

} // namespace

#ifdef QT_YCEST_RK3566
/** @brief 用原播放器已经使用的 GStreamer 运行环境探测 Web 上传视频。 */
GstVideoValidationResult validateVideoWithGStreamer(const QString &path, int timeoutSeconds)
{
    // 复用原播放器的 GStreamer 初始化状态与插件环境。
    QString initError;
    if (!initGStreamer(&initError)) {
        return {GstVideoValidationStatus::Unavailable, QStringLiteral("设备 GStreamer 无法初始化")};
    }
    if (path.isEmpty() || timeoutSeconds < 1 || timeoutSeconds > 60 || !QFileInfo(path).isFile()) {
        return {GstVideoValidationStatus::Invalid, QStringLiteral("视频文件不存在或检查参数无效")};
    }

    // 仅探测媒体流，不播放、不转码，也不修改原视频文件。
    GError *error = nullptr;
    GstDiscoverer *discoverer =
        gst_discoverer_new(static_cast<GstClockTime>(timeoutSeconds) * GST_SECOND, &error);
    if (!discoverer) {
        if (error)
            g_error_free(error);
        return {GstVideoValidationStatus::Unavailable,
                QStringLiteral("设备 GStreamer 媒体探测组件不可用")};
    }
    if (error) {
        g_error_free(error);
        error = nullptr;
    }
    const QByteArray uri =
        QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded(QUrl::FullyEncoded);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
    if (error)
        g_error_free(error);

    // 至少发现一条有效视频流才允许发布到设备视频目录。
    GstVideoValidationResult result{GstVideoValidationStatus::Invalid,
                                    QStringLiteral("文件不含有效视频流")};
    if (info) {
        const GstDiscovererResult discovery = gst_discoverer_info_get_result(info);
        if (discovery == GST_DISCOVERER_OK) {
            GList *streams = gst_discoverer_info_get_video_streams(info);
            if (streams)
                result = {GstVideoValidationStatus::Valid, QString()};
            if (streams)
                gst_discoverer_stream_info_list_free(streams);
        } else if (discovery == GST_DISCOVERER_MISSING_PLUGINS) {
            result = {GstVideoValidationStatus::Unavailable,
                      QStringLiteral("设备缺少视频解析所需的 GStreamer 插件")};
        } else if (discovery == GST_DISCOVERER_TIMEOUT) {
            result.message = QStringLiteral("视频检查超时");
        }
        gst_discoverer_info_unref(info);
    }
    g_object_unref(discoverer);
    return result;
}
#endif

qint64 GstPlayerWidget::probeAudioDurationMs(const QString &path)
{
#ifdef QT_YCEST_RK3566
    if (!QFileInfo(path).isFile() || !initGStreamer(nullptr)) return 0;
    GError *error = nullptr;
    GstDiscoverer *discoverer = gst_discoverer_new(2 * GST_SECOND,&error);
    if (error) g_error_free(error);
    if (!discoverer) return 0;
    error = nullptr;
    const auto uri = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toEncoded(QUrl::FullyEncoded);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer,uri.constData(),&error);
    if (error) g_error_free(error);
    qint64 durationMs = 0;
    if (info) {
        if (gst_discoverer_info_get_result(info) == GST_DISCOVERER_OK) {
            GList *streams = gst_discoverer_info_get_audio_streams(info);
            const auto duration = gst_discoverer_info_get_duration(info);
            if (streams && GST_CLOCK_TIME_IS_VALID(duration) && duration > 0)
                durationMs = qMax<qint64>(1,qint64(duration / GST_MSECOND));
            if (streams) gst_discoverer_stream_info_list_free(streams);
        }
        gst_discoverer_info_unref(info);
    }
    g_object_unref(discoverer);
    return durationMs;
#else
    Q_UNUSED(path);
    return 0;
#endif
}

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
        qWarning() << "忽略无效的RGA NV12输出"
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

/** @return 当前画布抓取的 RGB 快照；此接口不做 JPEG 编码。 */
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
    qInfo() << "VideoHoleWidget initializeGL"
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

/** @brief 上传最新 NV12 纹理并绘制画布。 */
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
        qInfo() << "VideoHoleWidget first paintGL"
                << "widget=" << size()
                << "surface=" << QSize(surfaceWidth, surfaceHeight)
                << "frame=" << QSize(frameWidth_, frameHeight_)
                << "defaultFbo=" << defaultFramebufferObject();
    }

    if (frameWidth_ <= 0 || frameHeight_ <= 0 ||
        !ensureShaderProgram() || yTexture_ == 0 || uvTexture_ == 0 ||
        vertexBuffer_ == 0) {
        if (textureUploadPending_) {
            qWarning() << "视频帧待上传但GL资源无效，跳过该帧"
                       << "frame=" << QSize(frameWidth_, frameHeight_);
            textureUploadPending_ = false;
            releasePendingFrameLease();
        }
        return;
    }

    if (textureUploadPending_ && nv12Frame_.isEmpty()) {
        qWarning() << "视频帧邮箱数据为空，跳过该帧"
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
            qWarning() << "NV12纹理上传失败"
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
        qWarning() << "NV12着色器属性位置无效"
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
        qInfo() << "NV12 frame draw"
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

/** @brief 向共享混音器转交通话音频独占请求；完整契约见声明。 */
bool GstPlayerWidget::setCallAudioExclusive(bool exclusive)
{
    return SharedAudioMixer::instance().setCallOutput(exclusive);
}

bool GstPlayerWidget::discardQueuedAudioForPrompt(bool *nullConfirmed)
{
    return SharedAudioMixer::instance().discardQueuedAudioForPrompt(nullConfirmed);
}

void GstPlayerWidget::holdVideoAudio(bool held)
{
    SharedAudioMixer::instance().holdVideoAudio(held);
}

void GstPlayerWidget::setPriorityAudioOwner(GstPlayerWidget *player, bool call)
{
    SharedAudioMixer::instance().selectAudio(player, call);
}

bool GstPlayerWidget::acquireBroadcastAudioOutput(std::atomic_bool &stop)
{
    return SharedAudioMixer::instance().acquireBroadcastOutput(stop);
}

bool GstPlayerWidget::releaseBroadcastAudioOutput()
{
    return SharedAudioMixer::instance().releaseBroadcastOutput();
}

bool VoiceCall::setMediaAudioExclusive(bool exclusive)
{
    return GstPlayerWidget::setCallAudioExclusive(exclusive);
}

/** @brief 创建管线轮询与常规播放诊断定时器。
 * @param[in] parent 可空 Qt 父控件；按 Qt 对象树管理本控件生命周期。
 */
GstPlayerWidget::GstPlayerWidget(QWidget *parent)
    : QWidget(parent)
{
    pipelineQueue_.reset(new SerialWorkQueue(QStringLiteral("media.pipeline")));
    busTimer_.setInterval(20);
    connect(&busTimer_, &QTimer::timeout, this, &GstPlayerWidget::pollBus);

    positionTimer_.setInterval(200);
    connect(&positionTimer_, &QTimer::timeout, this, &GstPlayerWidget::pollPosition);

    playbackDiagnosticTimer_.setInterval(30000);
    connect(&playbackDiagnosticTimer_, &QTimer::timeout,
            this, &GstPlayerWidget::pollPlaybackDiagnostics);

    QString error;
    if (!initGStreamer(&error)) {
        QTimer::singleShot(0, this, [this, error]() { emit errorOccured(error); });
    }
}

/** @brief 阻止新回调后停止并释放 GStreamer 管线。 */
GstPlayerWidget::~GstPlayerWidget()
{
    shuttingDown_.store(true);pipelineFramesEnabled_.store(false);pipelineRevision_.fetch_add(1);
    pipelineQueue_->shutdown(); // 最终析构才等待，后台不得同步等待UI。
    setVideoOutput(nullptr);
    if(playbin_) setNullStateAndWait(playbin_);
    resetPromptPrerollState();SharedAudioMixer::instance().removeSource(this);
    releasePipelineResources();
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
        // 视频画布保持为普通 Qt 子控件，由 QOpenGLWidget 合成，不创建
        // 独立的 Wayland/X11/KMS 视频窗口。
        videoWidget_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
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

/** @brief 异步切源，选择自动播放时由当前媒体的预卷完成通知启动。
 * @param[in] path 待加载的本地路径或 URL。
 * @param[in] playWhenPrepared true 等待 PAUSED 预卷完成后自动播放。
 * @note GUI 先使旧回调失效，后台串行执行 NULL、URI 和 PAUSED；失败不开放播放入口。
 *       同一地址重建时保留已输出的诊断告警标记。
 */
void GstPlayerWidget::setMedia(const QString &path, bool playWhenPrepared)
{
    pipelineFramesEnabled_.store(false);
    const auto revision=pipelineRevision_.fetch_add(1)+1;
    const QString requested=path.trimmed();
    playRequested_=playWhenPrepared;prepared_=false;playing_=false;
    mediaLoadAccepted_=false;
    localAutoPlay_=playWhenPrepared;
    busTimer_.stop();positionTimer_.stop();playbackDiagnosticTimer_.stop();
    mediaGeneration_.fetchAndAddOrdered(1);clearVideoCanvas();
    pipelineQueue_->enqueue([this,requested,revision]() -> SerialWorkQueue::Task {
        if (mediaPath_ != requested) {
            invalidRtcpDestinationWarningLogged_ = false;
            loggedRtspTimeouts_.clear();
            liveCreditWarningLogged_ = false;
            liveTimelineWarningLogged_ = false;
        }
        mediaPath_ = requested;
        const QString lower=mediaPath_.toLower();const bool rtsp=lower.startsWith(QStringLiteral("rtsp://"));
        const bool liveRtsp=rtsp && !preloadMode_;
        liveRtspMode_.storeRelease(liveRtsp ? 1 : 0);
        h264AuAlignmentEnabled_.storeRelease((rtsp || lower.contains(QStringLiteral(".m3u8"))) ? 1 : 0);
        playbackDiagnosticTimer_.setInterval(liveRtsp ? 5000 : 30000);
        sourceWidth_=0;sourceHeight_=0;sourceSizeKnown_.storeRelease(0);requestDisplayRectUpdate();
        lastVideoFrameMs_.storeRelease(0);lastAudioFrameMs_.storeRelease(0);
        lastVideoSignalMs_.storeRelease(0);lastAudioSignalMs_.storeRelease(0);
        ensurePipeline();if(!playbin_ || !videoSink_ || !audioSink_) return {};
        resetPlaybackDiagnostics();
        const QString uri=mediaUri(mediaPath_);
        GstElement *pipeline=playbin_;GstElement *sink=appSink_;GstBus *bus=bus_;
        const bool prompt=promptAudioMode_.loadAcquire()!=0;
        const auto effectiveVolume=effectiveVolume01();
        return {[this,pipeline,sink,bus,uri,liveRtsp,prompt,revision,effectiveVolume]() -> QString {
            const bool nullReady=setNullStateAndWait(pipeline);
            if(!nullReady) return QStringLiteral("GStreamer 切换到 NULL 失败，无法安全切换媒体");
            resetPromptPrerollState();
            if(prompt) SharedAudioMixer::instance().resetPromptSource(this);
            else SharedAudioMixer::instance().removeSource(this);
            if(bus) { gst_bus_set_flushing(bus,TRUE);gst_bus_set_flushing(bus,FALSE); }
            if(revision!=pipelineRevision_.load()) return {}; // 已停止旧源，但不启动过期的新源。
            if(uri.isEmpty()) return QStringLiteral("媒体路径为空或无效");
            if(sink) setBooleanProperty(G_OBJECT(sink),"sync",liveRtsp ? FALSE : TRUE);
            const auto encoded=uri.toUtf8();g_object_set(pipeline,"uri",encoded.constData(),nullptr);
            g_object_set(pipeline,"volume",static_cast<gdouble>(effectiveVolume),"mute",effectiveVolume<=0.0001f,nullptr);
            if(revision!=pipelineRevision_.load()) return {};
            pipelineFrameRevision_.store(revision);pipelineFramesEnabled_.store(true);
            const auto result=gst_element_set_state(pipeline,GST_STATE_PAUSED);
            if (result == GST_STATE_CHANGE_FAILURE) {
                pipelineFramesEnabled_.store(false);
                return QStringLiteral("GStreamer 切换到 PAUSED 失败: %1").arg(uri);
            }
            return {};
        },[this,revision](const QString &error) {
            if(revision!=pipelineRevision_.load()) return;
            if(!error.isEmpty()) {
                playRequested_ = false;
                emit errorOccured(error);
                return;
            }
            mediaLoadAccepted_ = true;
            busTimer_.start();if(!preloadMode_) positionTimer_.start();
        }};
    },QStringLiteral("source"),[this,revision]{return revision!=pipelineRevision_.load();});
}

/** @brief 排队播放已通过切源屏障的媒体；提示音继续按原预卷条件放行。 */
void GstPlayerWidget::play()
{
    const auto revision=pipelineRevision_.load();playRequested_=true;
    pipelineQueue_->enqueue([this,revision]() -> SerialWorkQueue::Task {
        if (!mediaLoadAccepted_) {
            playRequested_ = false;
            return {};
        }
        if (localAutoPlay_ && !prepared_) return {}; // 保留意图，由当前媒体的预卷完成通知兑现。
        ensurePipeline();if(!playbin_) return {};
        const bool managedPrompt = promptAudioMode_.loadAcquire()!=0 && priorityManaged();
        if(!managedPrompt && promptAudioMode_.loadAcquire()!=0 &&
           (promptAudioPrimed_.loadAcquire()==0 || promptLeadInQueued_.loadAcquire()==0)) return {};
        GstElement *pipeline=playbin_;const auto effectiveVolume=effectiveVolume01();
        return {[this,pipeline,revision,effectiveVolume,managedPrompt]() -> QString {
            if(revision!=pipelineRevision_.load()) return {};
            if (managedPrompt && promptLeadInQueued_.loadAcquire()==0) {
                const auto flow = primeManagedPrompt(revision);
                if (flow == GST_FLOW_FLUSHING) {
                    return {}; // 首帧还未解码或焦点已切走。
                }
                if (flow != GST_FLOW_OK) {
                    qWarning() << "[audio-path] player play preroll failed"
                               << "player=" << this << "revision=" << revision
                               << "flow=" << int(flow);
                    return QStringLiteral("播报音频预热失败");
                }
            }
            pipelineFrameRevision_.store(revision);pipelineFramesEnabled_.store(true);
            g_object_set(pipeline,"volume",static_cast<gdouble>(effectiveVolume),"mute",effectiveVolume<=0.0001f,nullptr);
            const qint64 startedMs = MonotonicClock::nowMs();
            const auto result=gst_element_set_state(pipeline,GST_STATE_PLAYING);
            if (result == GST_STATE_CHANGE_FAILURE) {
                qWarning() << "[GStreamer] PLAYING transition failed" << "pipeline=" << pipeline
                           << "revision=" << revision
                           << "elapsedMs=" << MonotonicClock::nowMs() - startedMs;
            }
            return result==GST_STATE_CHANGE_FAILURE
                    ? QStringLiteral("GStreamer 切换到 PLAYING 失败") : QString();
        },[this,revision](const QString &error) {
            if(revision!=pipelineRevision_.load()) return;
            if(!error.isEmpty()) { emit errorOccured(error);return; }
            busTimer_.start();positionTimer_.start();
            if(!preloadMode_ && mediaPath_.startsWith(QStringLiteral("rtsp://"),Qt::CaseInsensitive)) {
                playbackDiagnosticElapsed_.start();playbackDiagnosticTimer_.start();
            }
        }};
    },QStringLiteral("state"),[this,revision]{return revision!=pipelineRevision_.load();});
}

/** @brief 暂停当前管线。 */
void GstPlayerWidget::pause()
{
    playbackDiagnosticTimer_.stop();playRequested_=false;playing_=false;
    const auto revision=pipelineRevision_.load();
    pipelineQueue_->enqueue([this,revision]() -> SerialWorkQueue::Task {
        if(!playbin_) return {};
        GstElement *pipeline=playbin_;
        const bool prompt=promptAudioMode_.loadAcquire()!=0;
        return {[this,pipeline,prompt,revision]() -> QString {
            if(revision!=pipelineRevision_.load()) return {};
            gst_element_set_state(pipeline,GST_STATE_PAUSED);
            if(!prompt || priorityManaged()) {
                SharedAudioMixer::instance().removeSource(this);
                if (prompt) {
                    promptAudioPrimed_.storeRelease(0);
                    promptLeadInQueued_.storeRelease(0);
                }
            }
            return {};
        },{}};
    },QStringLiteral("state"),[this,revision]{return revision!=pipelineRevision_.load();});
}

/** @brief 立即撤销播放意图，后台串行停止管线。 */
void GstPlayerWidget::stop()
{
    stopPipeline(false);
}

/** @brief 已脱离前台的播放器在后台确认 NULL 后才自销毁。 */
void GstPlayerWidget::retire()
{
    shuttingDown_.store(true);
    stopPipeline(true);
}

/** @brief 撤销 UI 播放意图，后台尝试停止；退役失败时延迟重试。
 * @param[in] retireAfterStop true 表示确认 NULL 后通过 GUI 事件循环销毁对象。
 */
void GstPlayerWidget::stopPipeline(bool retireAfterStop)
{
    pipelineFramesEnabled_.store(false);
    const auto revision=pipelineRevision_.fetch_add(1)+1;
    playRequested_=false;prepared_=false;playing_=false;
    busTimer_.stop();positionTimer_.stop();playbackDiagnosticTimer_.stop();
    mediaGeneration_.fetchAndAddOrdered(1);clearVideoCanvas();
    lastVideoFrameMs_.storeRelease(0);lastAudioFrameMs_.storeRelease(0);
    lastVideoSignalMs_.storeRelease(0);lastAudioSignalMs_.storeRelease(0);
    pipelineQueue_->enqueue([this,revision,retireAfterStop]() -> SerialWorkQueue::Task {
        GstElement *pipeline=playbin_;const bool prompt=promptAudioMode_.loadAcquire()!=0;
        return {[this,pipeline,prompt,retireAfterStop]() -> QString {
            const bool nullReady=setNullStateAndWait(pipeline);
            if(!nullReady) {
                qWarning()<<"[GStreamer] stop: switching playbin to NULL did not complete";
                if (retireAfterStop) return QStringLiteral("旧直播管线停止未完成");
                if(prompt) return QStringLiteral("提示播放器停止未完成，保留音频分支");
            }
            resetPromptPrerollState();
            if(prompt) SharedAudioMixer::instance().resetPromptSource(this);
            else SharedAudioMixer::instance().removeSource(this);
            return {};
        },[this,revision,retireAfterStop](const QString &error) {
            if (revision != pipelineRevision_.load()) return;
            if (retireAfterStop) {
                if (error.isEmpty()) {
                    qInfo() << "[GStreamer] retired LIVE pipeline stopped" << "player=" << this;
                    deleteLater();
                } else {
                    QTimer::singleShot(2000, this, &GstPlayerWidget::retire);
                }
            } else if (!error.isEmpty()) {
                emit errorOccured(error);
            }
        }};
    },QStringLiteral("source"),[this,revision]{return revision!=pipelineRevision_.load();});
}

/** @brief 完整释放并重建 playbin，确保 MPP 解码上下文随旧管线销毁。 */
bool GstPlayerWidget::recreatePipeline()
{
    releasePipeline();
    pipelineQueue_->enqueue([this]() -> SerialWorkQueue::Task { ensurePipeline();return {}; });
    return true; // 接受异步重建；创建失败仍通过既有 errorOccured 报告。
}

/** @brief 跳转到指定毫秒位置。 */
void GstPlayerWidget::seekMs(int ms)
{
    if(ms<0) return;
    const auto revision=pipelineRevision_.load();
    pipelineQueue_->enqueue([this,ms,revision]() -> SerialWorkQueue::Task {
        if(!playbin_) return {};
        GstElement *pipeline=playbin_;
        return {[this,pipeline,ms,revision]() -> QString {
            if(revision==pipelineRevision_.load()) gst_element_seek_simple(pipeline,GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH|GST_SEEK_FLAG_KEY_UNIT),static_cast<gint64>(ms)*GST_MSECOND);
            return {};
        },{}};
    },QStringLiteral("seek"),[this,revision]{return revision!=pipelineRevision_.load();});
}

/** @brief 设置归一化音量，取值范围为 0.0～1.0。 */
void GstPlayerWidget::setVolume(float value01)
{
    volume01_ = std::max(0.0f, std::min(1.0f, value01));
    applyEffectiveVolume();
}

/** @brief 为一个播报入口设置独立的临时压低系数。 */
void GstPlayerWidget::setAudioDucking(const QObject *owner, float factor01)
{
    if (!owner) return;
    audioDuckingFactors_.insert(owner,
                                std::max(0.0f, std::min(1.0f, factor01)));
    qInfo() << "[GStreamer-AUDIO] video ducking set" << "player=" << this
            << "owner=" << owner << "effectiveVolume=" << effectiveVolume01();
    applyEffectiveVolume();
}

/** @brief 释放一个播报入口持有的临时压低。 */
void GstPlayerWidget::clearAudioDucking(const QObject *owner)
{
    if (!owner || audioDuckingFactors_.remove(owner) <= 0) return;
    qInfo() << "[GStreamer-AUDIO] video ducking cleared" << "player=" << this
            << "owner=" << owner << "effectiveVolume=" << effectiveVolume01();
    applyEffectiveVolume();
}

/** @brief 应用业务音量与全部播报压低系数的乘积。 */
void GstPlayerWidget::applyEffectiveVolume()
{
    if (!playbin_) return;
    const auto effectiveVolume=effectiveVolume01();
    const auto owner = static_cast<const void *>(this);
    pipelineQueue_->enqueue([this,effectiveVolume,owner]() -> SerialWorkQueue::Task {
        if(!playbin_) return {};
        GstElement *pipeline=playbin_;
        return {[pipeline,effectiveVolume,owner]() -> QString {
            g_object_set(pipeline,"volume",static_cast<gdouble>(effectiveVolume),"mute",effectiveVolume<=0.0001f,nullptr);
            qInfo() << "[GStreamer-AUDIO] video volume applied" << "player=" << owner
                    << "effectiveVolume=" << effectiveVolume;
            return {};
        },{}};
    },QStringLiteral("volume"));
}

/** @return 仅UI读取的业务音量与全部压低系数乘积，限制到0～1。 */
float GstPlayerWidget::effectiveVolume01() const
{
    float effectiveVolume = volume01_;
    for (auto it = audioDuckingFactors_.constBegin();
         it != audioDuckingFactors_.constEnd(); ++it) {
        effectiveVolume *= it.value();
    }
    effectiveVolume = std::max(0.0f, std::min(1.0f, effectiveVolume));
    return effectiveVolume;
}

/** @brief 设置提示音模式，管线创建时据此接入预卷回调。 */
void GstPlayerWidget::setPromptAudioMode(bool on)
{
    promptAudioMode_.storeRelease(on ? 1 : 0);
}

void GstPlayerWidget::setCallRingtonePriority(bool active)
{
    auto &mixer = SharedAudioMixer::instance();
    if (active) {
        ringtoneToken_.store(mixer.claimRingtone());
    } else {
        mixer.releaseRingtone(ringtoneToken_.exchange(1));
    }
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
            error = QStringLiteral("Qt 视频画布尚无可截图内容");
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

    if (ringtoneToken_.load() != 0) {
        // 内置 MP3 含封面流，铃声只需要音频。保留 playbin 其它默认标志。
        constexpr guint visualStreams = (1U << 0) | (1U << 2) | (1U << 3); // GstPlayFlags: video/text/vis
        guint flags = 0;
        g_object_get(playbin_, "flags", &flags, nullptr);
        g_object_set(playbin_, "flags", flags & ~visualStreams, nullptr);
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

/** @brief 提交不可合并的后台停止及UI引用清理屏障，保证后继创建不会抢用旧句柄。 */
void GstPlayerWidget::releasePipeline()
{
    pipelineFramesEnabled_.store(false);
    pipelineRevision_.fetch_add(1);playRequested_=false;prepared_=false;playing_=false;
    busTimer_.stop();positionTimer_.stop();playbackDiagnosticTimer_.stop();
    mediaGeneration_.fetchAndAddOrdered(1);clearVideoCanvas();
    // 不可合并的结构屏障：后续 ensurePipeline 必须等旧回调停止及引用释放。
    pipelineQueue_->enqueue([this]() -> SerialWorkQueue::Task {
        // UI转移所有权，后台独占旧图；后续创建等本任务完成后发生。
        GstElement *pipeline=playbin_;GstBus *bus=bus_;
        GstPad *videoPad=videoProbePad_,*audioPad=audioProbePad_;
        const auto videoId=videoProbeId_,audioId=audioProbeId_;
        playbin_=nullptr;bus_=nullptr;appSink_=nullptr;audioSink_=nullptr;
        { QMutexLocker lock(&gstMutex_);videoSink_=nullptr; }
        videoProbePad_=nullptr;audioProbePad_=nullptr;videoProbeId_=0;audioProbeId_=0;
        return {[this,pipeline,bus,videoPad,audioPad,videoId,audioId]() -> QString {
            if(!setNullStateAndWait(pipeline)) qWarning()<<"[GStreamer] release: switching playbin to NULL did not complete";
            resetPromptPrerollState();SharedAudioMixer::instance().removeSource(this);
            if(videoPad) { if(videoId) gst_pad_remove_probe(videoPad,videoId);gst_object_unref(videoPad); }
            if(audioPad) { if(audioId) gst_pad_remove_probe(audioPad,audioId);gst_object_unref(audioPad); }
            if(bus) { gst_bus_set_sync_handler(bus,nullptr,nullptr,nullptr);gst_object_unref(bus); }
            GstElement *overlay=nullptr;
            { QMutexLocker lock(&gstMutex_);overlay=overlayElement_;overlayElement_=nullptr; }
            if(overlay) gst_object_unref(overlay);
            if(pipeline) gst_object_unref(pipeline);
            return {};
        },[this](const QString &) { guiDeliveryEventPending_.storeRelease(0);clearVideoCanvas(); }};
    });
}

/** @brief 最终析构在后台退出后清理尚未移交的引用；运行期销毁由后台释放。 */
void GstPlayerWidget::releasePipelineResources()
{
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
        qCritical() << "[RGA] /dev/rga不存在，终止Qt视频链路；禁止CPU缩放回退";
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
                 // RK3566 的 mppvideodec 收到 sink 上游 QoS 后可能持续跳过
                 // 已迟到的预测帧，最终退化为只在关键帧附近输出。关闭 QoS
                 // 反馈，但保留 sync=true 和原有单帧反压策略。
                 "qos", FALSE,
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

    qInfo() << "[GStreamer] Qt合成模式: MPP NV12 DMA-BUF"
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
    if (promptAudioMode_.loadAcquire() != 0) {
        g_signal_connect(sink, "new-preroll",
                         G_CALLBACK(&GstPlayerWidget::sharedAudioAppSinkNewPreroll), this);
    }
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

/** @brief 在当前媒体达到 PAUSED 后标记预卷完成，兑现该媒体的播放请求。 */
void GstPlayerWidget::markPrepared()
{
    if (prepared_ || !mediaLoadAccepted_) return;
    const auto revision = pipelineRevision_.load();
    prepared_ = true;
    emit prepared();
    if (revision != pipelineRevision_.load()) return;

    const bool promptReady = promptAudioMode_.loadAcquire() == 0 ||
            (promptAudioPrimed_.loadAcquire() != 0 &&
             promptLeadInQueued_.loadAcquire() != 0);
    if (playRequested_ && !preloadMode_ && playbin_ && promptReady) {
        play();
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
    // RK3566 正式版本固定走单窗口 Qt 合成，禁止重新选择 linuxfb、
    // kmssink 或其他独立 DRM/X11/Wayland 视频层。
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

/** @brief 拉取并处理 GStreamer 总线上的状态、EOS 和错误消息。
 * @note 仅首次记录当前媒体的每组 RTSP 超时标识，直接读取流编号和 SSRC。
 */
void GstPlayerWidget::pollBus()
{
    if(pipelineQueue_->busy()) return; // 状态任务完成前不并行查询/消费旧源。
    if (!bus_) return;

    while (GstMessage *message = gst_bus_pop(bus_)) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            qWarning() << "[audio-path] player bus error" << "player=" << this
                       << "revision=" << pipelineRevision_.load()
                       << "source=" << GST_MESSAGE_SRC(message);
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const QString text = error ? QString::fromUtf8(error->message)
                                       : QStringLiteral("unknown GStreamer error");
            const QString detail = debug ? QString::fromUtf8(debug) : QString();
            if (error) g_error_free(error);
            if (debug) g_free(debug);
            if (localAutoPlay_ && !prepared_) {
                // 已失败的预卷即使随后还有状态消息，也不得触发自动 PLAYING。
                mediaLoadAccepted_ = false;
                playRequested_ = false;
            }
            prepared_ = false;
            playing_ = false;
            playbackDiagnosticTimer_.stop();
            resetPromptPrerollState();
            if (promptAudioMode_.loadAcquire() != 0) {
                playRequested_ = false;
                mediaGeneration_.fetchAndAddOrdered(1);
                // 错误通知也不能在解码线程尚未停止时重置共享输入。
                // stop/setMedia/releasePipeline 负责停止后的实际释放。
            } else {
                SharedAudioMixer::instance().removeSource(this);
            }
            emit errorOccured(detail.isEmpty() ? text : text + QStringLiteral(" | ") + detail);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *warning = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_warning(message, &warning, &debug);
            const QString sourceName = QString::fromUtf8(
                        GST_OBJECT_NAME(GST_MESSAGE_SRC(message)));
            const QString warningText = warning
                    ? QString::fromUtf8(warning->message)
                    : QStringLiteral("unknown");
            const QString detail = debug
                    ? QString::fromUtf8(debug)
                    : QStringLiteral("none");
            const bool invalidRtcpDestination =
                    sourceName.startsWith(QStringLiteral("udpsink")) &&
                    warningText.contains(QStringLiteral("Error sending UDP packets")) &&
                    detail.contains(QStringLiteral(":0"));
            if (invalidRtcpDestination) {
                if (!invalidRtcpDestinationWarningLogged_) {
                    invalidRtcpDestinationWarningLogged_ = true;
                    qWarning().noquote()
                            << QStringLiteral(
                                   "[GStreamer-RTCP] server supplied an invalid RTCP destination "
                                   "(port 0); repeated warnings are suppressed for this media: %1")
                               .arg(detail);
                }
            } else {
                qWarning().noquote()
                        << QStringLiteral("[GStreamer-WARNING] source=%1 message=%2 detail=%3")
                           .arg(sourceName)
                           .arg(warningText)
                           .arg(detail);
            }
            if (warning) g_error_free(warning);
            if (debug) g_free(debug);
            break;
        }
        case GST_MESSAGE_QOS: {
            GstFormat format = GST_FORMAT_UNDEFINED;
            guint64 processed = 0;
            guint64 dropped = 0;
            gint64 jitter = 0;
            gdouble proportion = 0.0;
            gint quality = 0;
            gst_message_parse_qos_stats(message, &format, &processed, &dropped);
            gst_message_parse_qos_values(message, &jitter, &proportion, &quality);
            Q_UNUSED(format);
            Q_UNUSED(proportion);
            Q_UNUSED(quality);
            qosMessageCount_.fetchAndAddOrdered(1);
            qosLastProcessed_.storeRelease(processed);
            qosLastDropped_.storeRelease(dropped);
            qosLastSource_ = GST_MESSAGE_SRC(message)
                    ? QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(message)))
                    : QStringLiteral("unknown");
            qint64 oldMaximum = qosMaxLateNs_.loadAcquire();
            while (jitter > oldMaximum &&
                   !qosMaxLateNs_.testAndSetOrdered(oldMaximum, jitter)) {
                oldMaximum = qosMaxLateNs_.loadAcquire();
            }
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structure = gst_message_get_structure(message);
            if (structure && gst_structure_has_name(structure, "drop-msg")) {
                guint tooLate = 0;
                guint dropOnLatency = 0;
                gst_structure_get_uint(structure, "num-too-late", &tooLate);
                gst_structure_get_uint(structure, "num-drop-on-latency",
                                       &dropOnLatency);

                GstElement *sourceElement = GST_IS_ELEMENT(GST_MESSAGE_SRC(message))
                        ? GST_ELEMENT(GST_MESSAGE_SRC(message))
                        : nullptr;
                const QString media = rtpMediaType(sourceElement);
                if (media == QStringLiteral("video")) {
                    jitterVideoTooLateDrops_.fetchAndAddOrdered(tooLate);
                    jitterVideoLatencyDrops_.fetchAndAddOrdered(dropOnLatency);
                } else if (media == QStringLiteral("audio")) {
                    jitterAudioTooLateDrops_.fetchAndAddOrdered(tooLate);
                    jitterAudioLatencyDrops_.fetchAndAddOrdered(dropOnLatency);
                } else {
                    jitterUnknownTooLateDrops_.fetchAndAddOrdered(tooLate);
                    jitterUnknownLatencyDrops_.fetchAndAddOrdered(dropOnLatency);
                }
            } else if (structure &&
                gst_structure_has_name(structure, "GstRTSPSrcTimeout")) {
                gint streamNumber = 0;
                guint ssrc = 0;
                const qint64 streamKey = gst_structure_get_int(
                            structure, "stream-number", &streamNumber) ? streamNumber : -1;
                const qint64 ssrcKey = gst_structure_get_uint(structure, "ssrc", &ssrc)
                        ? static_cast<qint64>(ssrc) : -1;
                const auto timeoutKey = qMakePair(streamKey, ssrcKey);
                if (!loggedRtspTimeouts_.contains(timeoutKey)) {
                    loggedRtspTimeouts_.insert(timeoutKey);
                    qWarning().noquote()
                            << QStringLiteral("[GStreamer-RTSP] timeout stream-number=%1 ssrc=%2")
                               .arg(streamKey >= 0 ? QString::number(streamKey)
                                                   : QStringLiteral("unknown"))
                               .arg(ssrcKey >= 0 ? QString::number(ssrcKey)
                                                 : QStringLiteral("unknown"));
                }
            }
            break;
        }
        case GST_MESSAGE_EOS:
            prepared_ = false;
            playing_ = false;
            positionTimer_.stop();
            playbackDiagnosticTimer_.stop();
            if (promptAudioMode_.loadAcquire() != 0) {
                // playbin EOS 表示解码样本已经全部交给共享 appsrc，但前面
                // 排入的前导静音使共享输出仍有少量尾部待播放。给它留出
                // 排空时间后再恢复视频音量，避免截断提示音结尾。
                const int eosGeneration = mediaGeneration_.loadAcquire();
                const int drainDelayMs =
                        SharedAudioMixer::instance().promptDrainDelayMs(this);
                qInfo() << "[GStreamer-AUDIO] prompt EOS drain delayMs=" << drainDelayMs;
                QTimer::singleShot(drainDelayMs, this, [this, eosGeneration]() {
                    if (eosGeneration != mediaGeneration_.loadAcquire()) return;
                    playRequested_ = false;
                    resetPromptPrerollState();
                    // 文件播报 EOS 已排空，释放输入以便视频继续。铃声沿用
                    // 下一次 setMedia/stop 释放分支的独立生命周期。
                    if (priorityManaged()) SharedAudioMixer::instance().removeSource(this);
                    emit videoFinished();
                });
            } else {
                resetPromptPrerollState();
                SharedAudioMixer::instance().removeSource(this);
                emit videoFinished();
            }
            break;
        case GST_MESSAGE_ASYNC_DONE:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(playbin_)) markPrepared();
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(playbin_)) {
                GstState newState = GST_STATE_NULL;
                gst_message_parse_state_changed(message, nullptr, &newState, nullptr);
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
        if(pipelineQueue_->busy()) return; // 同步信号槽可提交下一源，不能继续消费旧源消息。
    }
}

/** @brief 原定时器只投递一次进度读取，真实同步查询与状态操作在同一后台队列串行。
 * @note busy时沿用原跳过策略，避免排队；完成值只有当前源代次仍有效才在UI发信号。
 * 后台只借用已准备的管线，串行队列在查询完成前不释放它，最终析构仍由队列屏障保护。
 */
void GstPlayerWidget::pollPosition()
{
    if(pipelineQueue_->busy() || !playbin_ || (!prepared_ && !playing_)) return;
    const auto revision=pipelineRevision_.load();
    pipelineQueue_->enqueue([this,revision]() -> SerialWorkQueue::Task {
        if(!playbin_ || (!prepared_ && !playing_)) return {};
        GstElement *pipeline=playbin_;
        /** @brief 单次查询值由后台run独占写、队列完成后UI读，后台不读取QWidget状态。 */
        struct PositionResult { gint64 position=0,duration=0;bool valid=false; /**< 时间单位GStreamer纳秒；valid仅代表位置查询成功。 */ };
        auto result=std::make_shared<PositionResult>();
        return {[this,pipeline,revision,result]() -> QString {
            if(revision!=pipelineRevision_.load()) return {};
            result->valid=gst_element_query_position(pipeline,GST_FORMAT_TIME,&result->position);
            if(result->valid) gst_element_query_duration(pipeline,GST_FORMAT_TIME,&result->duration);
            return {};
        },[this,pipeline,revision,result](const QString &error) {
            if(!error.isEmpty() || !result->valid || revision!=pipelineRevision_.load() ||
                    pipeline!=playbin_ || (!prepared_ && !playing_)) return;
            emit positionChanged(static_cast<int>(result->position/GST_MSECOND),static_cast<int>(result->duration/GST_MSECOND));
        }};
    },QStringLiteral("position"),[this,revision]{return revision!=pipelineRevision_.load();});
}

/** @brief 新媒体开始前清空 RTP 和解码采样状态，保留当前媒体的告警标记。 */
void GstPlayerWidget::resetPlaybackDiagnostics()
{
    appSinkSamples_.storeRelease(0);
    appSinkRgaFrames_.storeRelease(0);
    appSinkRgaSkippedFrames_.storeRelease(0);
    mailboxPublishedFrames_.storeRelease(0);
    mailboxReadyOverwrites_.storeRelease(0);
    mailboxNoSlotDrops_.storeRelease(0);
    guiDeliveryCoalesced_.storeRelease(0);
    guiDeliveredFrames_.storeRelease(0);
    appSinkLastSampleMs_.storeRelease(0);
    appSinkMaxGapMs_.storeRelease(0);
    appSinkMaxRgaUs_.storeRelease(0);
    guiMaxQueueLagMs_.storeRelease(0);
    guiMaxEndToEndLagMs_.storeRelease(0);
    firstRgaSuccessLogged_.storeRelease(0);
    qosMessageCount_.storeRelease(0);
    qosLastProcessed_.storeRelease(0);
    qosLastDropped_.storeRelease(0);
    qosMaxLateNs_.storeRelease(0);
    qosLastSource_.clear();
    jitterVideoTooLateDrops_.storeRelease(0);
    jitterVideoLatencyDrops_.storeRelease(0);
    jitterAudioTooLateDrops_.storeRelease(0);
    jitterAudioLatencyDrops_.storeRelease(0);
    jitterUnknownTooLateDrops_.storeRelease(0);
    jitterUnknownLatencyDrops_.storeRelease(0);
    decoderInputFrames_.storeRelease(0);
    decoderInputKeyFrames_.storeRelease(0);
    decoderInputCorruptedFrames_.storeRelease(0);
    decoderOutputFrames_.storeRelease(0);
    decoderOutputCorruptedFrames_.storeRelease(0);
    decoderInputLastBufferMs_.storeRelease(0);
    decoderInputMaxGapMs_.storeRelease(0);
    decoderInputPtsOffsetNs_.storeRelease(0);
    decoderInputMaxAbsPtsOffsetNs_.storeRelease(0);
    decoderInputPtsOffsetValid_.storeRelease(0);
    decoderOutputLastBufferMs_.storeRelease(0);
    decoderOutputMaxGapMs_.storeRelease(0);
    decoderOutputPtsOffsetNs_.storeRelease(0);
    decoderOutputMaxAbsPtsOffsetNs_.storeRelease(0);
    decoderOutputPtsOffsetValid_.storeRelease(0);
    liveAuReceived_.storeRelease(0);
    liveAuAccepted_.storeRelease(0);
    liveAuDropped_.storeRelease(0);
    liveInFlightAus_.storeRelease(0);
    liveMaximumInFlightAus_.storeRelease(0);
    liveLatestInputPtsNs_.storeRelease(0);
    liveLatestOutputPtsNs_.storeRelease(0);
    liveDecoderLagNs_.storeRelease(0);
    liveMaximumDecoderLagNs_.storeRelease(0);
    liveLatestInputPtsValid_.storeRelease(0);
    liveLatestOutputPtsValid_.storeRelease(0);
    liveDecoderAuAligned_.storeRelease(-1);
    liveDecoderCapsLogged_.storeRelease(0);
    liveWaitingForKeyframe_.storeRelease(liveRtspMode_.loadAcquire() != 0 ? 1 : 0);
    liveCreditRecoveryPending_.storeRelease(0);
    liveCreditOverLimitConsecutiveAus_.storeRelease(0);
    appSinkPtsOffsetNs_.storeRelease(0);
    appSinkMaxAbsPtsOffsetNs_.storeRelease(0);
    appSinkPtsOffsetValid_.storeRelease(0);
    rtpVideoRawPackets_.storeRelease(0);
    rtpVideoTimestampChanges_.storeRelease(0);
    rtpVideoTimestampAdvanceTicks_.storeRelease(0);
    rtpVideoArrivalAdvanceUs_.storeRelease(0);
    rtpVideoSequenceGaps_.storeRelease(0);
    rtpVideoSequenceReorders_.storeRelease(0);
    rtpVideoTimestampBackwards_.storeRelease(0);
    rtpVideoTimestampJumps_.storeRelease(0);
    rtpVideoSsrcChanges_.storeRelease(0);
    rtpVideoLastTimestamp_.storeRelease(0);
    rtpVideoLastSequence_.storeRelease(0);
    rtpVideoLastSsrc_.storeRelease(0);
    rtpVideoLastPayloadType_.storeRelease(0);
    rtpVideoLastTimestampArrivalUs_.storeRelease(0);
    rtpVideoClockRate_.storeRelease(0);
    rtpVideoPreviousPacketValid_.storeRelease(0);
    timelineFaultConsecutiveIntervals_ = 0;
    playbackDiagnosticElapsed_.invalidate();
}

/** @brief 释放旧媒体的提示音 preroll，并清空本次播放门闩。 */
void GstPlayerWidget::resetPromptPrerollState()
{
    promptAudioPrimed_.storeRelease(0);
    promptLeadInQueued_.storeRelease(0);
    QMutexLocker locker(&gstMutex_);
    if (promptPrerollSample_) {
        gst_sample_unref(promptPrerollSample_);
        promptPrerollSample_ = nullptr;
    }
}

/** @brief 在 GUI 线程周期检查 RTSP 视频解码速率和时间轴。
 * @details 每次确认故障均发送恢复信号，同一媒体的时间轴告警仅输出一次。
 */
void GstPlayerWidget::pollPlaybackDiagnostics()
{
    if(pipelineQueue_->busy()) return; // 状态任务完成前不并行查询/消费旧源。
    if (!playbin_ || !playing_ ||
        !mediaPath_.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive)) {
        return;
    }

    const qint64 intervalMs = playbackDiagnosticElapsed_.isValid()
            ? qMax<qint64>(1, playbackDiagnosticElapsed_.restart())
            : 30000;

    int maximumVideoJitterFillPercent = 0;
    GstIterator *iterator = GST_IS_BIN(playbin_)
            ? gst_bin_iterate_recurse(GST_BIN(playbin_))
            : nullptr;
    if (iterator) {
        GValue item = G_VALUE_INIT;
        bool finished = false;
        while (!finished) {
            switch (gst_iterator_next(iterator, &item)) {
            case GST_ITERATOR_OK: {
                GstElement *element = GST_ELEMENT(g_value_get_object(&item));
                const char *factoryName = elementFactoryName(element);
                const QString factory = factoryName
                        ? QString::fromLatin1(factoryName)
                        : QString();

                if (factory == QStringLiteral("rtpjitterbuffer") &&
                    rtpMediaType(element) == QStringLiteral("video") &&
                    hasProperty(G_OBJECT(element), "percent")) {
                    gint fillPercent = 0;
                    g_object_get(element, "percent", &fillPercent, nullptr);
                    maximumVideoJitterFillPercent = qMax(
                                maximumVideoJitterFillPercent, fillPercent);
                }
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(iterator);
                break;
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                finished = true;
                break;
            }
        }
        if (G_VALUE_TYPE(&item) != G_TYPE_INVALID) {
            g_value_unset(&item);
        }
        gst_iterator_free(iterator);
    }

    const double seconds = static_cast<double>(intervalMs) / 1000.0;

    // 保持探针计数的采样边界，解码判定使用本周期的输入、输出和 RTP 指标。
    const quint64 decoderInputFrames = decoderInputFrames_.fetchAndStoreOrdered(0);
    decoderInputKeyFrames_.fetchAndStoreOrdered(0);
    decoderInputCorruptedFrames_.fetchAndStoreOrdered(0);
    const quint64 decoderOutputFrames = decoderOutputFrames_.fetchAndStoreOrdered(0);
    decoderOutputCorruptedFrames_.fetchAndStoreOrdered(0);
    decoderInputPtsOffsetValid_.fetchAndStoreOrdered(0);
    const bool decoderPtsValid = decoderOutputPtsOffsetValid_.fetchAndStoreOrdered(0) != 0;
    appSinkPtsOffsetValid_.fetchAndStoreOrdered(0);
    const qint64 decoderPtsOffsetNs = decoderOutputPtsOffsetNs_.loadAcquire();
    decoderOutputMaxAbsPtsOffsetNs_.fetchAndStoreOrdered(0);
    appSinkMaxAbsPtsOffsetNs_.fetchAndStoreOrdered(0);
    decoderInputMaxAbsPtsOffsetNs_.fetchAndStoreOrdered(0);
    liveAuReceived_.fetchAndStoreOrdered(0);
    liveAuAccepted_.fetchAndStoreOrdered(0);
    liveAuDropped_.fetchAndStoreOrdered(0);
    liveMaximumInFlightAus_.fetchAndStoreOrdered(0);
    liveMaximumDecoderLagNs_.fetchAndStoreOrdered(0);

    rtpVideoRawPackets_.fetchAndStoreOrdered(0);
    const quint64 rawTimestampChanges =
            rtpVideoTimestampChanges_.fetchAndStoreOrdered(0);
    const quint64 rawTimestampTicks =
            rtpVideoTimestampAdvanceTicks_.fetchAndStoreOrdered(0);
    const quint64 rawArrivalUs =
            rtpVideoArrivalAdvanceUs_.fetchAndStoreOrdered(0);
    const quint64 rawSequenceGaps =
            rtpVideoSequenceGaps_.fetchAndStoreOrdered(0);
    rtpVideoSequenceReorders_.fetchAndStoreOrdered(0);
    const quint64 rawTimestampBackwards =
            rtpVideoTimestampBackwards_.fetchAndStoreOrdered(0);
    const quint64 rawTimestampJumps =
            rtpVideoTimestampJumps_.fetchAndStoreOrdered(0);
    const quint64 rawSsrcChanges =
            rtpVideoSsrcChanges_.fetchAndStoreOrdered(0);
    const qint64 rawClockRate = rtpVideoClockRate_.loadAcquire();
    const double rawMediaAdvanceMs = rawClockRate > 0
            ? static_cast<double>(rawTimestampTicks) * 1000.0 /
              static_cast<double>(rawClockRate)
            : 0.0;
    const double rawArrivalAdvanceMs = static_cast<double>(rawArrivalUs) / 1000.0;
    const double rawClockRateRatioValue = rawClockRate > 0 && rawArrivalUs > 0
            ? rawMediaAdvanceMs / rawArrivalAdvanceMs : 0.0;
    const QString rawClockRateRatio = rawClockRateRatioValue > 0.0
            ? QString::number(rawClockRateRatioValue, 'f', 6)
            : QStringLiteral("unavailable");

    jitterVideoTooLateDrops_.fetchAndStoreOrdered(0);
    jitterVideoLatencyDrops_.fetchAndStoreOrdered(0);
    jitterAudioTooLateDrops_.fetchAndStoreOrdered(0);
    jitterAudioLatencyDrops_.fetchAndStoreOrdered(0);
    jitterUnknownTooLateDrops_.fetchAndStoreOrdered(0);
    jitterUnknownLatencyDrops_.fetchAndStoreOrdered(0);
    decoderInputMaxGapMs_.fetchAndStoreOrdered(0);
    decoderOutputMaxGapMs_.fetchAndStoreOrdered(0);
    appSinkMaxGapMs_.fetchAndStoreOrdered(0);
    appSinkMaxRgaUs_.fetchAndStoreOrdered(0);
    guiMaxQueueLagMs_.fetchAndStoreOrdered(0);
    guiMaxEndToEndLagMs_.fetchAndStoreOrdered(0);
    qosMaxLateNs_.fetchAndStoreOrdered(0);

    const double rawFrameFps = static_cast<double>(rawTimestampChanges) / seconds;
    const double decoderInputFps =
            static_cast<double>(decoderInputFrames) / seconds;
    const double decoderOutputFps =
            static_cast<double>(decoderOutputFrames) / seconds;
    const bool rawVideoHealthy = rawClockRate > 0 && rawFrameFps >= 4.0 &&
            rawClockRateRatioValue >= 0.98 && rawClockRateRatioValue <= 1.02 &&
            rawSequenceGaps == 0 && rawTimestampBackwards == 0 &&
            rawTimestampJumps == 0 && rawSsrcChanges == 0;
    const bool decoderInputActive = decoderInputFps >= 1.0;
    const bool decoderOutputSlow =
            decoderOutputFps < rawFrameFps * 0.65;
    const bool decoderOutputPtsLagged = decoderPtsValid &&
            decoderPtsOffsetNs <= -static_cast<qint64>(10 * GST_SECOND);
    const bool jitterBackpressure = maximumVideoJitterFillPercent >= 90;
    const bool timelineFault =
            rawVideoHealthy && decoderInputActive &&
            (decoderOutputPtsLagged ||
             (decoderOutputSlow && jitterBackpressure));
    timelineFaultConsecutiveIntervals_ = timelineFault
            ? timelineFaultConsecutiveIntervals_ + 1 : 0;
    if (timelineFaultConsecutiveIntervals_ >= 2) {
        const QString reason = QStringLiteral(
                "live MPP output stall: rawFps=%1 decoderInFps=%2 decoderOutFps=%3 "
                "decoderOutputPtsLagMs=%4 fillPct=%5 rawClockRate=%6 rawRate=%7")
                .arg(rawFrameFps, 0, 'f', 2)
                .arg(decoderInputFps, 0, 'f', 2)
                .arg(decoderOutputFps, 0, 'f', 2)
                .arg(decoderPtsValid
                     ? static_cast<double>(decoderPtsOffsetNs) / 1000000.0
                     : 0.0, 0, 'f', 3)
                .arg(maximumVideoJitterFillPercent)
                .arg(rawClockRate)
                .arg(rawClockRateRatio);
        timelineFaultConsecutiveIntervals_ = 0;
        if (!liveTimelineWarningLogged_) {
            liveTimelineWarningLogged_ = true;
            qWarning().noquote() << "[GStreamer-WATCHDOG]" << reason;
        }
        emit liveTimelineFaultDetected(reason);
    }
}

/** @brief 在同步总线回调中绑定硬件视频窗口句柄。 */
GstBusSyncReply GstPlayerWidget::busSyncHandler(GstBus *, GstMessage *message, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load()) return GST_BUS_PASS;

    // Qt 合成模式不使用 GstVideoOverlay，也没有独立视频窗口句柄。
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

/** @brief 记录当前有效源到达视频 sink 的单调毫秒，并按源代次过滤排队通知。 */
GstPadProbeReturn GstPlayerWidget::videoPadProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !self->pipelineFramesEnabled_.load() ||
            self->pipelineFrameRevision_.load()!=self->pipelineRevision_.load() || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    const auto revision=self->pipelineFrameRevision_.load();
    const qint64 now = MonotonicClock::nowMs();
    self->lastVideoFrameMs_.storeRelease(now);

    if (self->sourceSizeKnown_.loadAcquire() == 0) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        GstVideoInfo videoInfo;
        if (caps && gst_video_info_from_caps(&videoInfo, caps)) {
            const int width = GST_VIDEO_INFO_WIDTH(&videoInfo);
            const int height = GST_VIDEO_INFO_HEIGHT(&videoInfo);
            if (width > 0 && height > 0 && self->sourceSizeKnown_.testAndSetOrdered(0, 1)) {
                QMetaObject::invokeMethod(self, [self, width, height,revision]() {
                    if(revision==self->pipelineRevision_.load() && !self->shuttingDown_.load()) self->setSourceVideoSize(width, height);
                }, Qt::QueuedConnection);
            }
        }
        if (caps) gst_caps_unref(caps);
    }

    const qint64 lastSignal = self->lastVideoSignalMs_.loadAcquire();
    if (lastSignal == 0 || now - lastSignal >= 200) {
        self->lastVideoSignalMs_.storeRelease(now);
        QMetaObject::invokeMethod(self, [self, now,revision]() {
            if (self->shuttingDown_.load() || revision != self->pipelineRevision_.load()) return;
            if (!self->useQtCompositedVideo() && self->firstOutputFrameRevision_ != revision) {
                self->firstOutputFrameRevision_ = revision;
                emit self->firstVideoFrameSubmitted();
            }
            emit self->videoFrameArrived(now);
        }, Qt::QueuedConnection);
    }
    return GST_PAD_PROBE_OK;
}

/** @brief 当前有效源到达 appsink 输入 pad 时沿原路径发出音频帧通知。
 * @param[in] info 借用的探针信息；只处理缓冲事件。
 * @param[in] userData 借用的播放器实例；旧代次不发通知。
 * @return GST_PAD_PROBE_OK，保持原有音频缓冲流。
 */
GstPadProbeReturn GstPlayerWidget::audioPadProbe(GstPad *, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !self->pipelineFramesEnabled_.load() ||
            self->pipelineFrameRevision_.load()!=self->pipelineRevision_.load() || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    const auto revision=self->pipelineFrameRevision_.load();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    self->lastAudioFrameMs_.storeRelease(now);
    const qint64 lastSignal = self->lastAudioSignalMs_.loadAcquire();
    if (now - lastSignal >= 500) {
        self->lastAudioSignalMs_.storeRelease(now);
        QMetaObject::invokeMethod(self, [self, now,revision]() {
            if (!self->shuttingDown_.load() && revision==self->pipelineRevision_.load()) emit self->audioFrameArrived(now);
        }, Qt::QueuedConnection);
    }
    return GST_PAD_PROBE_OK;
}

/** @brief 统计压缩视频输入，并检查直播解码积压。
 * @param[in] pad 解码器输入 pad。
 * @param[in] info 当前探针事件，用于读取压缩缓冲区。
 * @param[in] userData 绑定该探针的播放器。
 * @return 允许缓冲区通过，或在等待关键帧及解码积压时丢弃缓冲区。
 * @note 积压确认后将恢复信号投递给 GUI，按媒体地址过滤重复告警。
 */
GstPadProbeReturn GstPlayerWidget::decoderInputPadProbe(
        GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() ||
        !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    const bool liveRtsp = self->liveRtspMode_.loadAcquire() != 0;
    const bool deltaUnit = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    if (liveRtsp) {
        self->liveAuReceived_.fetchAndAddOrdered(1);

        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps)) {
            const GstStructure *structure = gst_caps_get_structure(caps, 0);
            const gchar *alignment = structure
                    ? gst_structure_get_string(structure, "alignment") : nullptr;
            const bool auAligned = alignment && std::strcmp(alignment, "au") == 0;
            self->liveDecoderAuAligned_.storeRelease(auAligned ? 1 : 0);
            if (self->liveDecoderCapsLogged_.testAndSetOrdered(0, 1)) {
                gchar *capsText = gst_caps_to_string(caps);
                qInfo().noquote() << "[GStreamer-LIVE-CAPS] decoderSink="
                                  << (capsText ? QString::fromUtf8(capsText)
                                               : QStringLiteral("unknown"))
                                  << "auAligned=" << auAligned;
                if (capsText) g_free(capsText);
            }
        }
        if (caps) gst_caps_unref(caps);

        const bool auAligned = self->liveDecoderAuAligned_.loadAcquire() == 1;
        if (auAligned && self->liveWaitingForKeyframe_.loadAcquire() != 0) {
            if (deltaUnit) {
                self->liveAuDropped_.fetchAndAddOrdered(1);
                return GST_PAD_PROBE_DROP;
            }
            self->liveWaitingForKeyframe_.storeRelease(0);
            qInfo() << "[GStreamer-LIVE-CREDIT] first IDR accepted; live decoder input opened";
        }

        if (auAligned && self->liveCreditRecoveryPending_.loadAcquire() != 0) {
            self->liveAuDropped_.fetchAndAddOrdered(1);
            return GST_PAD_PROBE_DROP;
        }

        if (auAligned && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) &&
            GST_BUFFER_PTS(buffer) <=
                    static_cast<GstClockTime>(std::numeric_limits<qint64>::max())) {
            const qint64 inputPtsNs = static_cast<qint64>(GST_BUFFER_PTS(buffer));
            self->liveLatestInputPtsNs_.storeRelease(inputPtsNs);
            self->liveLatestInputPtsValid_.storeRelease(1);

            if (self->liveLatestOutputPtsValid_.loadAcquire() != 0) {
                const qint64 outputPtsNs = self->liveLatestOutputPtsNs_.loadAcquire();
                const qint64 lagNs = inputPtsNs >= outputPtsNs
                        ? inputPtsNs - outputPtsNs : 0;
                self->liveDecoderLagNs_.storeRelease(lagNs);
                updateMaximumAbsolute(&self->liveMaximumDecoderLagNs_, lagNs);

                const qint64 inFlight = self->liveInFlightAus_.loadAcquire();
                const bool creditOverLimit =
                        lagNs >= kLiveDecoderCreditLagNs &&
                        inFlight >= kLiveDecoderMaximumInFlightAus;
                const int consecutive = creditOverLimit
                        ? self->liveCreditOverLimitConsecutiveAus_.fetchAndAddOrdered(1) + 1
                        : 0;
                if (!creditOverLimit) {
                    self->liveCreditOverLimitConsecutiveAus_.storeRelease(0);
                }

                if (consecutive >= kLiveDecoderCreditConfirmAus &&
                    self->liveCreditRecoveryPending_.testAndSetOrdered(0, 1)) {
                    self->liveAuDropped_.fetchAndAddOrdered(1);
                    const QString reason = QStringLiteral(
                            "live MPP credit overflow: lagMs=%1 inFlightAu=%2 "
                            "limitAu=%3 alignment=au")
                            .arg(static_cast<double>(lagNs) / 1000000.0, 0, 'f', 3)
                            .arg(inFlight)
                            .arg(kLiveDecoderMaximumInFlightAus);
                    const auto revision=self->pipelineFrameRevision_.load();
                    QPointer<GstPlayerWidget> safeSelf(self);
                    const bool queued = QMetaObject::invokeMethod(self,
                            [safeSelf, reason,revision]() {
                        if (!safeSelf || safeSelf->shuttingDown_.load() || revision!=safeSelf->pipelineRevision_.load() ||
                            safeSelf->liveRtspMode_.loadAcquire() == 0) {
                            return;
                        }
                        if (!safeSelf->liveCreditWarningLogged_) {
                            safeSelf->liveCreditWarningLogged_ = true;
                            qWarning().noquote() << "[GStreamer-LIVE-CREDIT]" << reason;
                        }
                        emit safeSelf->liveTimelineFaultDetected(reason);
                    }, Qt::QueuedConnection);
                    if (!queued) {
                        self->liveCreditRecoveryPending_.storeRelease(0);
                    }
                    return GST_PAD_PROBE_DROP;
                }
            }
        }

        if (auAligned) {
            self->liveAuAccepted_.fetchAndAddOrdered(1);
            const qint64 inFlight = self->liveInFlightAus_.fetchAndAddOrdered(1) + 1;
            updateMaximumAbsolute(&self->liveMaximumInFlightAus_, inFlight);
        }
    }

    self->decoderInputFrames_.fetchAndAddOrdered(1);
    if (!deltaUnit) {
        self->decoderInputKeyFrames_.fetchAndAddOrdered(1);
    }
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED)) {
        self->decoderInputCorruptedFrames_.fetchAndAddOrdered(1);
    }
    updateMaximumBufferGap(&self->decoderInputLastBufferMs_,
                           &self->decoderInputMaxGapMs_,
                           MonotonicClock::nowMs());

    qint64 inputOffsetNs = 0;
    if (bufferPtsClockOffsetNs(pad, buffer, &inputOffsetNs)) {
        self->decoderInputPtsOffsetNs_.storeRelease(inputOffsetNs);
        updateMaximumAbsolute(&self->decoderInputMaxAbsPtsOffsetNs_,
                              inputOffsetNs);
        self->decoderInputPtsOffsetValid_.storeRelease(1);
    }
    return GST_PAD_PROBE_OK;
}

/** @brief 统计视频解码器输出帧及其相对管线时钟的 PTS 偏移。 */
GstPadProbeReturn GstPlayerWidget::decoderOutputPadProbe(
        GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() ||
        !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;
    if (self->liveRtspMode_.loadAcquire() != 0 &&
        self->liveDecoderAuAligned_.loadAcquire() == 1) {
        qint64 inFlight = self->liveInFlightAus_.loadAcquire();
        while (inFlight > 0 &&
               !self->liveInFlightAus_.testAndSetOrdered(inFlight, inFlight - 1)) {
            inFlight = self->liveInFlightAus_.loadAcquire();
        }
        if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) &&
            GST_BUFFER_PTS(buffer) <=
                    static_cast<GstClockTime>(std::numeric_limits<qint64>::max())) {
            const qint64 outputPtsNs = static_cast<qint64>(GST_BUFFER_PTS(buffer));
            self->liveLatestOutputPtsNs_.storeRelease(outputPtsNs);
            self->liveLatestOutputPtsValid_.storeRelease(1);
            if (self->liveLatestInputPtsValid_.loadAcquire() != 0) {
                const qint64 inputPtsNs = self->liveLatestInputPtsNs_.loadAcquire();
                const qint64 lagNs = inputPtsNs >= outputPtsNs
                        ? inputPtsNs - outputPtsNs : 0;
                self->liveDecoderLagNs_.storeRelease(lagNs);
                updateMaximumAbsolute(&self->liveMaximumDecoderLagNs_, lagNs);
            }
        }
    }
    self->decoderOutputFrames_.fetchAndAddOrdered(1);
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED)) {
        self->decoderOutputCorruptedFrames_.fetchAndAddOrdered(1);
    }
    updateMaximumBufferGap(&self->decoderOutputLastBufferMs_,
                           &self->decoderOutputMaxGapMs_,
                           QDateTime::currentMSecsSinceEpoch());

    qint64 offsetNs = 0;
    if (bufferPtsClockOffsetNs(pad, buffer, &offsetNs)) {
        self->decoderOutputPtsOffsetNs_.storeRelease(offsetNs);
        updateMaximumAbsolute(&self->decoderOutputMaxAbsPtsOffsetNs_, offsetNs);
        self->decoderOutputPtsOffsetValid_.storeRelease(1);
    }
    return GST_PAD_PROBE_OK;
}

/** @brief 统计进入 jitterbuffer 前的原始视频 RTP 时钟推进和本地到达推进。 */
GstPadProbeReturn GstPlayerWidget::rtpInputPadProbe(
        GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !pad ||
        !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    int mediaTag = GPOINTER_TO_INT(
                g_object_get_data(G_OBJECT(pad), "ycest-rtp-media-tag"));
    if (mediaTag == 0) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps)) {
            const GstStructure *structure = gst_caps_get_structure(caps, 0);
            const gchar *media = structure
                    ? gst_structure_get_string(structure, "media") : nullptr;
            if (media && g_ascii_strcasecmp(media, "video") == 0) {
                mediaTag = 1;
                gint clockRate = 0;
                if (gst_structure_get_int(structure, "clock-rate", &clockRate) &&
                    clockRate > 0) {
                    self->rtpVideoClockRate_.storeRelease(clockRate);
                }
            } else if (media && *media) {
                mediaTag = 2;
            }
            if (mediaTag != 0) {
                g_object_set_data(G_OBJECT(pad), "ycest-rtp-media-tag",
                                  GINT_TO_POINTER(mediaTag));
            }
        }
        if (caps) gst_caps_unref(caps);
    }
    if (mediaTag != 1) return GST_PAD_PROBE_OK;

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!buffer || !gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
        return GST_PAD_PROBE_OK;
    }

    const quint32 timestamp = gst_rtp_buffer_get_timestamp(&rtp);
    const quint32 ssrc = gst_rtp_buffer_get_ssrc(&rtp);
    const quint32 sequence = gst_rtp_buffer_get_seq(&rtp);
    const quint32 payloadType = gst_rtp_buffer_get_payload_type(&rtp);
    gst_rtp_buffer_unmap(&rtp);

    const qint64 arrivalUs = g_get_monotonic_time();
    self->rtpVideoRawPackets_.fetchAndAddOrdered(1);
    self->rtpVideoLastPayloadType_.storeRelease(payloadType);

    if (self->rtpVideoPreviousPacketValid_.testAndSetOrdered(0, 1)) {
        self->rtpVideoLastSsrc_.storeRelease(ssrc);
        self->rtpVideoLastSequence_.storeRelease(sequence);
        self->rtpVideoLastTimestamp_.storeRelease(timestamp);
        self->rtpVideoLastTimestampArrivalUs_.storeRelease(arrivalUs);
        return GST_PAD_PROBE_OK;
    }

    const quint32 previousSsrc = self->rtpVideoLastSsrc_.fetchAndStoreOrdered(ssrc);
    const quint32 previousSequence =
            self->rtpVideoLastSequence_.fetchAndStoreOrdered(sequence);
    const quint32 previousTimestamp =
            self->rtpVideoLastTimestamp_.fetchAndStoreOrdered(timestamp);

    if (previousSsrc != ssrc) {
        self->rtpVideoSsrcChanges_.fetchAndAddOrdered(1);
        self->rtpVideoLastTimestampArrivalUs_.storeRelease(arrivalUs);
        return GST_PAD_PROBE_OK;
    }

    const quint16 sequenceDelta = static_cast<quint16>(
                static_cast<quint16>(sequence) -
                static_cast<quint16>(previousSequence));
    if (sequenceDelta > 1 && sequenceDelta < 0x8000u) {
        self->rtpVideoSequenceGaps_.fetchAndAddOrdered(sequenceDelta - 1);
    } else if (sequenceDelta == 0 || sequenceDelta >= 0x8000u) {
        self->rtpVideoSequenceReorders_.fetchAndAddOrdered(1);
    }

    if (timestamp != previousTimestamp) {
        const quint32 modularDelta = timestamp - previousTimestamp;
        const qint64 timestampDelta = modularDelta <= 0x7fffffffu
                ? static_cast<qint64>(modularDelta)
                : -static_cast<qint64>(0x100000000ULL - modularDelta);
        const qint64 previousArrivalUs =
                self->rtpVideoLastTimestampArrivalUs_.fetchAndStoreOrdered(arrivalUs);
        const qint64 clockRate = self->rtpVideoClockRate_.loadAcquire();
        const qint64 maximumExpectedStep = clockRate > 0
                ? clockRate * 10 : 900000;

        if (timestampDelta > 0 && timestampDelta <= maximumExpectedStep) {
            self->rtpVideoTimestampChanges_.fetchAndAddOrdered(1);
            self->rtpVideoTimestampAdvanceTicks_.fetchAndAddOrdered(
                        static_cast<quint64>(timestampDelta));
            const qint64 arrivalDeltaUs = arrivalUs - previousArrivalUs;
            if (previousArrivalUs > 0 && arrivalDeltaUs > 0 &&
                arrivalDeltaUs <= 10000000) {
                self->rtpVideoArrivalAdvanceUs_.fetchAndAddOrdered(
                            static_cast<quint64>(arrivalDeltaUs));
            }
        } else if (timestampDelta < 0) {
            self->rtpVideoTimestampBackwards_.fetchAndAddOrdered(1);
        } else if (timestampDelta > maximumExpectedStep) {
            self->rtpVideoTimestampJumps_.fetchAndAddOrdered(1);
        }
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
    const auto revision = pipelineRevision_.load();
    if (firstOutputFrameRevision_ != revision) {
        firstOutputFrameRevision_ = revision;
        emit firstVideoFrameSubmitted();
    }
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

/** @brief 拉取当前 PCM 并交共享混音器；停止与旧代次沿原路径拒绝。
 * @param[in] sink 借用的音频 appsink，只有当前有效代次才拉取样本。
 * @param[in] userData 借用的播放器，析构前由管线停止屏障保护。
 * @return 原有 GstFlowReturn；共享输出瞬时失败仍对 playbin 返回 OK，避免终止后续解码。
 * @note 只在当前有效代次拉取样本；失效铃声被丢弃，提示音沿原有重试路径提交。
 */
GstFlowReturn GstPlayerWidget::sharedAudioAppSinkNewSample(GstElement *sink, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self) return GST_FLOW_FLUSHING;
    if (self->shuttingDown_.load() || !self->pipelineFramesEnabled_.load() || self->pipelineFrameRevision_.load()!=self->pipelineRevision_.load() || self->preloadMode_) {
        return GST_FLOW_FLUSHING;
    }

    const auto revision=self->pipelineFrameRevision_.load();
    const auto ringtoneToken=self->ringtoneToken_.load();
    GstSample *sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_EOS;
    if (ringtoneToken != 0 && revision != self->pipelineRevision_.load()) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    const bool promptSource = self->promptAudioMode_.loadAcquire() != 0;
    GstFlowReturn flow = GST_FLOW_ERROR;
    const int attempts = promptSource ? 12 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        flow = SharedAudioMixer::instance().pushSample(self,sample,promptSource,ringtoneToken,false);
        if (flow != GST_FLOW_FLUSHING) break;
        // 提示音首批真实 PCM 保留在当前 sample 中等待分支恢复，不能像
        // 普通连续媒体那样消费后直接丢弃。
        g_usleep(10 * 1000);
    }
    gst_sample_unref(sample);
    if (promptSource && flow != GST_FLOW_OK) {
        qWarning() << "[GStreamer-AUDIO] prompt PCM delivery failed flow=" << flow;
        QMetaObject::invokeMethod(self, [self, flow,revision]() {
            if (!self->shuttingDown_.load() && revision==self->pipelineRevision_.load()) {
                emit self->errorOccured(
                            QStringLiteral("提示音 PCM 写入失败: %1")
                            .arg(static_cast<int>(flow)));
            }
        }, Qt::QueuedConnection);
    }

    // 共享输出管线首次启动时可能仍处于异步状态，appsrc 会短暂返回
    // GST_FLOW_FLUSHING。这里已经成功消费了 playbin 的样本，不能把共享
    // 管线的瞬时状态反向传播给 appsink，否则 playbin 会永久停止音频流，
    // 表现为视频画面正常但全程无声。后续样本会在共享管线就绪后继续送入。
    return GST_FLOW_OK;
}

/** @brief 获焦后的文件播报预热，重用 preroll 的格式，不提前消费首块真实 PCM。
 * @param[in] revision 当前播放代次；换源后不再继续准备。
 * @return 成功送入静音返回 OK；等待样本或失焦返回 FLUSHING；输出错误返回 ERROR。
 */
GstFlowReturn GstPlayerWidget::primeManagedPrompt(quint64 revision)
{
    GstSample *sample = nullptr;
    {
        QMutexLocker lock(&gstMutex_);
        if (promptPrerollSample_) sample = gst_sample_ref(promptPrerollSample_);
    }
    if (!sample) return GST_FLOW_FLUSHING;

    GstFlowReturn flow = GST_FLOW_FLUSHING;
    for (int attempt = 0; attempt < 20 && revision == pipelineRevision_.load(); ++attempt) {
        bool submitted = false;
        flow = SharedAudioMixer::instance().pushSilentPreroll(this, sample, 200 * GST_MSECOND, 0, &submitted);
        if (flow == GST_FLOW_OK && submitted) {
            promptAudioPrimed_.storeRelease(1);
            promptLeadInQueued_.storeRelease(1);
            break;
        }
        if (flow == GST_FLOW_OK) {
            gst_sample_unref(sample);
            return GST_FLOW_FLUSHING;
        }
        if (flow != GST_FLOW_FLUSHING) break;
        g_usleep(10 * 1000);
    }
    gst_sample_unref(sample);
    return flow == GST_FLOW_FLUSHING && revision == pipelineRevision_.load() ? GST_FLOW_ERROR : flow;
}

/** @brief 受管播报缓存预卷样本，铃声在原回调内预热；旧代次不能唤醒新源。 */
GstFlowReturn GstPlayerWidget::sharedAudioAppSinkNewPreroll(GstElement *sink,
                                                            gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !self->pipelineFramesEnabled_.load() ||
            self->pipelineFrameRevision_.load()!=self->pipelineRevision_.load() ||
            self->promptAudioMode_.loadAcquire() == 0) {
        return GST_FLOW_OK;
    }

    const auto revision=self->pipelineFrameRevision_.load();
    const auto ringtoneToken=self->ringtoneToken_.load();
    GstSample *sample = nullptr;
    g_signal_emit_by_name(sink, "pull-preroll", &sample);
    if (!sample) return GST_FLOW_OK;
    if (ringtoneToken != 0 && revision != self->pipelineRevision_.load()) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    if (self->priorityManaged()) {
        {
            QMutexLocker lock(&self->gstMutex_);
            if (revision == self->pipelineRevision_.load()) {
                if (self->promptPrerollSample_) gst_sample_unref(self->promptPrerollSample_);
                self->promptPrerollSample_ = gst_sample_ref(sample);
            }
        }
        gst_sample_unref(sample);
        QMetaObject::invokeMethod(self, [self,revision] {
            if (!self->shuttingDown_.load() && revision == self->pipelineRevision_.load() && self->playRequested_)
                self->play();
        }, Qt::QueuedConnection);
        return GST_FLOW_OK;
    }

    GstFlowReturn flow = GST_FLOW_ERROR;
    // 第一次把动态 appsrc 分支同步到共享 PLAYING 管线时，可能短暂处于
    // flushing。完整的 200ms 前导静音在 GStreamer preroll 线程准备，
    // Qt 主线程不再直接调用 appsrc push-buffer。
    for (int attempt = 0; attempt < 20; ++attempt) {
        flow = SharedAudioMixer::instance().pushSilentPreroll(
                    self, sample, 200 * GST_MSECOND, ringtoneToken);
        if (flow != GST_FLOW_FLUSHING) break;
        g_usleep(10 * 1000);
    }
    if (flow != GST_FLOW_OK) {
        qWarning() << "[GStreamer-AUDIO] prompt silent preroll failed flow=" << flow;
        QMetaObject::invokeMethod(self, [self, flow,revision]() {
            if (!self->shuttingDown_.load() && revision==self->pipelineRevision_.load()) {
                emit self->errorOccured(
                            QStringLiteral("提示音分支准备失败: %1")
                            .arg(static_cast<int>(flow)));
            }
        }, Qt::QueuedConnection);
    } else {
        {
            QMutexLocker locker(&self->gstMutex_);
            if (self->promptPrerollSample_) {
                gst_sample_unref(self->promptPrerollSample_);
            }
            self->promptPrerollSample_ = gst_sample_ref(sample);
        }
        self->promptLeadInQueued_.storeRelease(1);
        self->promptAudioPrimed_.storeRelease(1);
        qInfo() << "[GStreamer-AUDIO] prompt branch ready; lead-in silence accepted";

        QMetaObject::invokeMethod(self, [self,revision]() {
            if (!self->shuttingDown_.load() && revision==self->pipelineRevision_.load() && self->playRequested_) {
                self->play();
            }
        }, Qt::QueuedConnection);
    }
    gst_sample_unref(sample);
    // preroll 是准备动作，不能把共享输出的瞬时状态反向传播给 playbin。
    return GST_FLOW_OK;
}

/** @brief 接收当前源样本，固定入口代次后经RGA写邮箱；切源期间的旧样本不得占用新代次槽。 */
GstFlowReturn GstPlayerWidget::appSinkNewSample(GstElement *sink, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || self->shuttingDown_.load() || !self->pipelineFramesEnabled_.load() || self->pipelineFrameRevision_.load()!=self->pipelineRevision_.load() || self->preloadMode_) {
        return GST_FLOW_FLUSHING;
    }

    const int generation=self->mediaGeneration_.loadAcquire();
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

    if (buffer) {
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        qint64 offsetNs = 0;
        if (sinkPad && bufferPtsClockOffsetNs(sinkPad, buffer, &offsetNs)) {
            self->appSinkPtsOffsetNs_.storeRelease(offsetNs);
            updateMaximumAbsolute(&self->appSinkMaxAbsPtsOffsetNs_, offsetNs);
            self->appSinkPtsOffsetValid_.storeRelease(1);
        }
        if (sinkPad) gst_object_unref(sinkPad);
    }

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

    if(generation!=self->mediaGeneration_.loadAcquire() || !self->pipelineFramesEnabled_.load() ||
       self->pipelineFrameRevision_.load()!=self->pipelineRevision_.load()) { gst_sample_unref(sample);return GST_FLOW_OK; }
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

    const char *factoryName = elementFactoryName(source);
    if (!factoryName || std::strcmp(factoryName, "rtspsrc") != 0) return;

    // rtspsrc 的属性在 source-setup 阶段配置。延迟可通过环境变量覆盖，
    // 默认 1000 ms 为长时间播放吸收网络抖动，同时用 drop-on-latency 限制延迟上界。
    bool ok = false;
    const int envLatency = QString::fromLocal8Bit(qgetenv("QT_YCEST_RTSP_LATENCY_MS")).toInt(&ok);
    const int latency = ok ? std::max(0, envLatency) : 1000;
    setIntegerProperty(G_OBJECT(source), "latency", latency);
    setBooleanProperty(G_OBJECT(source), "drop-on-latency", TRUE);
    // 对照测试结束后恢复 auto；解码器输入探针只观测原始时间戳，不再改写 PTS/DTS。
    // GstRTSPSrcBufferMode 的 auto 枚举值为 3。
    const bool bufferModeSupported = hasProperty(G_OBJECT(source), "buffer-mode");
    setIntegerProperty(G_OBJECT(source), "buffer-mode", 3);
    gint configuredBufferMode = -1;
    if (bufferModeSupported) {
        g_object_get(source, "buffer-mode", &configuredBufferMode, nullptr);
    }
    const QString configuredBufferModeText = !bufferModeSupported
            ? QStringLiteral("unsupported")
            : rtspBufferModeName(configuredBufferMode);
    // 当前组播服务端把 RTCP 回传端口发布为 0，无法接收 NACK/重传请求。
    // 保留普通 RTCP 支持以接收发送端时钟报告，仅关闭无效的重传请求。
    setBooleanProperty(G_OBJECT(source), "do-retransmission", FALSE);
    // 新版 GStreamer 在 RTSP/TCP 模式下可用接收时间戳抑制长时发送端时钟漂移；
    // 老版本没有该属性时保持原行为。
    setBooleanProperty(G_OBJECT(source), "tcp-timestamp", TRUE);

    qInfo() << "[GStreamer-RTSP] source configured"
            << "latencyMs=" << latency
            << "bufferMode=" << configuredBufferModeText
            << "dropOnLatency=true"
            << "retransmission=false"
            << "tcpTimestampSupported=" << hasProperty(G_OBJECT(source), "tcp-timestamp");
}

/** @brief 在 element 创建时调整解码器和解析器属性。 */
void GstPlayerWidget::elementSetup(GstElement *, GstElement *element, gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || !element || self->shuttingDown_.load()) return;

    const char *factoryName = elementFactoryName(element);
    if (!factoryName) return;

    if (std::strcmp(factoryName, "rtpjitterbuffer") == 0) {
        // 只开启丢包消息统计，不改变 jitterbuffer 的排队或丢帧策略。
        setBooleanProperty(G_OBJECT(element), "post-drop-messages", TRUE);
        setIntegerProperty(G_OBJECT(element), "drop-messages-interval", 1000);
        if (!g_object_get_data(G_OBJECT(element), "ycest-raw-rtp-diag-hooked")) {
            g_object_set_data(G_OBJECT(element),
                              "ycest-raw-rtp-diag-hooked",
                              GINT_TO_POINTER(1));
            GstPad *sinkPad = gst_element_get_static_pad(element, "sink");
            if (sinkPad) {
                gst_pad_add_probe(sinkPad,
                                  GST_PAD_PROBE_TYPE_BUFFER,
                                  &GstPlayerWidget::rtpInputPadProbe,
                                  self,
                                  nullptr);
                gst_object_unref(sinkPad);
            }
        }
    }

    const QString factory = QString::fromLatin1(factoryName);
    if (isVideoDecoderFactory(factory) &&
        !g_object_get_data(G_OBJECT(element), "ycest-decoder-diag-hooked")) {
        g_object_set_data(G_OBJECT(element),
                          "ycest-decoder-diag-hooked",
                          GINT_TO_POINTER(1));
        GstPad *sinkPad = gst_element_get_static_pad(element, "sink");
        GstPad *srcPad = gst_element_get_static_pad(element, "src");
        const bool sinkPadAttached = sinkPad != nullptr;
        const bool srcPadAttached = srcPad != nullptr;
        if (sinkPad) {
            gst_pad_add_probe(sinkPad,
                              GST_PAD_PROBE_TYPE_BUFFER,
                              &GstPlayerWidget::decoderInputPadProbe,
                              self,
                              nullptr);
            gst_object_unref(sinkPad);
        }
        if (srcPad) {
            gst_pad_add_probe(srcPad,
                              GST_PAD_PROBE_TYPE_BUFFER,
                              &GstPlayerWidget::decoderOutputPadProbe,
                              self,
                              nullptr);
            gst_object_unref(srcPad);
        }
        qInfo() << "[GStreamer-DIAG] decoder probes attached"
                << "factory=" << factory
                << "sinkPad=" << sinkPadAttached
                << "srcPad=" << srcPadAttached;
        if (self->liveRtspMode_.loadAcquire() != 0) {
            qInfo().noquote() << QStringLiteral(
                    "[GStreamer-DECODER] factory=%1 plugin=%2 fastMode=%3 ignoreError=%4")
                    .arg(factory,
                         elementPluginVersion(element),
                         booleanPropertyText(G_OBJECT(element), "fast-mode"),
                         booleanPropertyText(G_OBJECT(element), "ignore-error"));
        }
    }

    if (self->liveRtspMode_.loadAcquire() != 0 &&
        std::strcmp(factoryName, "rtph264depay") == 0) {
        const bool waitSupported = hasProperty(G_OBJECT(element), "wait-for-keyframe");
        const bool requestSupported = hasProperty(G_OBJECT(element), "request-keyframe");
        setBooleanProperty(G_OBJECT(element), "wait-for-keyframe", TRUE);
        setBooleanProperty(G_OBJECT(element), "request-keyframe", TRUE);
        qInfo() << "[GStreamer-LIVE] H.264 depay keyframe recovery"
                << "waitForKeyframe=" << waitSupported
                << "requestKeyframe=" << requestSupported;
    }

    if (self->h264AuAlignmentEnabled_.loadAcquire() == 0) return;

    if (std::strcmp(factoryName, "h264parse") != 0 ||
        g_object_get_data(G_OBJECT(element), "ycest-h264-au-query-hooked")) {
        return;
    }

    // 不修改 decodebin 已经创建的内部 capsfilter。开始推流后再改
    // capsfilter 会触发重新协商，并可能让 hlsdemux 报 not-negotiated。
    // 在 h264parse 首次协商输出格式时限制为完整 AU，使解析器先完成
    // NAL -> AU 组装，再把 byte-stream/AU 数据交给 Rockchip MPP。
    setIntegerProperty(G_OBJECT(element), "config-interval", -1);
    g_object_set_data(G_OBJECT(element),
                      "ycest-h264-au-query-hooked",
                      GINT_TO_POINTER(1));
    GstPad *srcPad = gst_element_get_static_pad(element, "src");
    if (!srcPad) return;
    gst_pad_add_probe(srcPad,
                      static_cast<GstPadProbeType>(
                              GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM |
                              GST_PAD_PROBE_TYPE_PUSH),
                      &GstPlayerWidget::h264ParserQueryProbe,
                      self,
                      nullptr);
    gst_object_unref(srcPad);

    qInfo() << "[GStreamer-H264] parser negotiation restricted"
            << "liveRtsp=" << (self->liveRtspMode_.loadAcquire() != 0)
            << "stream-format=byte-stream alignment=au";
}

/** @brief 为 HLS/RTSP H.264 caps 查询补充 AU 对齐要求。 */
GstPadProbeReturn GstPlayerWidget::h264ParserQueryProbe(
        GstPad *pad,
        GstPadProbeInfo *info,
        gpointer userData)
{
    auto *self = static_cast<GstPlayerWidget *>(userData);
    if (!self || !pad || !info || self->shuttingDown_.load() ||
        self->h264AuAlignmentEnabled_.loadAcquire() == 0) {
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

SnapshotVideoFrame GstPlayerWidget::snapshotVideoFrame() const
{
    SnapshotVideoFrame result;
    if(!useQtCompositedVideo() || !frameMailboxMutex_.tryLock()) return result;
    const FrameMailboxSlot *latest=nullptr;const auto generation=mediaGeneration_.loadAcquire();
    for(const auto &slot:frameMailboxSlots_) {
        if(slot.state==FrameSlotWriting || slot.generation!=generation || !slot.sequence) continue;
        if(!latest || slot.sequence>latest->sequence) latest=&slot;
    }
    if(latest) { result.nv12=latest->nv12Frame;result.size=QSize(latest->width,latest->height); }
    frameMailboxMutex_.unlock();return result;
}
QWidget *GstPlayerWidget::snapshotVideoWidget() const { return useQtCompositedVideo() ? videoCanvas() : nullptr; }
