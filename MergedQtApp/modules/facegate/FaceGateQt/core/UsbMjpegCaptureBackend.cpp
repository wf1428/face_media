/**
 * @file UsbMjpegCaptureBackend.cpp
 * @brief 实现 USB MJPEG 摄像头的 GStreamer/MPP 硬件解码采集链路。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "UsbMjpegCaptureBackend.h"

#include <QByteArray>
#include <QDebug>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cstring>
#include <limits>
#include <memory>

#ifdef Q_OS_LINUX
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace {

/** @brief 进程已初始化 GStreamer 时直接复用，否则执行一次兼容初始化。 */
bool ensureGStreamer(QString *error)
{
    if (gst_is_initialized()) {
        return true;
    }

    GError *gstError = nullptr;
    const bool ok = gst_init_check(nullptr, nullptr, &gstError);
    if (!ok && error) {
        *error = gstError && gstError->message
                     ? QStringLiteral("GStreamer initialization failed: %1")
                           .arg(QString::fromUtf8(gstError->message))
                     : QStringLiteral("GStreamer initialization failed");
    }
    if (gstError) {
        g_error_free(gstError);
    }
    return ok;
}

/** @brief 同步进入 NULL，确保 MPP 上下文已在另一条解码链路启动前释放。 */
bool setNullStateAndWait(GstElement *element)
{
    if (!element) {
        return true;
    }
    if (gst_element_set_state(element, GST_STATE_NULL) ==
        GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    const GstStateChangeReturn result =
        gst_element_get_state(element, nullptr, nullptr, 2 * GST_SECOND);
    return result != GST_STATE_CHANGE_FAILURE &&
           result != GST_STATE_CHANGE_ASYNC;
}

/** @brief 解析并弹出管线总线上的一个错误或 EOS。 */
bool takeBusFailure(GstBus *bus, QString *error)
{
    if (!bus) {
        return false;
    }

    GstMessage *message = gst_bus_pop_filtered(
        bus,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!message) {
        return false;
    }

    QString text;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *gstError = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &gstError, &debug);
        text = gstError && gstError->message
                   ? QString::fromUtf8(gstError->message)
                   : QStringLiteral("unknown GStreamer error");
        if (debug && *debug) {
            text += QStringLiteral(" (%1)").arg(QString::fromUtf8(debug));
        }
        if (gstError) {
            g_error_free(gstError);
        }
        g_free(debug);
    } else {
        text = QStringLiteral("USB MJPEG pipeline reached EOS");
    }
    gst_message_unref(message);

    if (error) {
        *error = text;
    }
    return true;
}

/** @brief USB MJPEG -> mppjpegdec -> NV12 的独立采集后端。 */
class UsbMjpegCaptureBackend final : public ICameraCaptureBackend {
public:
    /** @brief 停止管线并释放全部 GStreamer/MPP 资源。 */
    ~UsbMjpegCaptureBackend() override
    {
        close();
    }

    /**
     * @brief 创建并启动 v4l2src -> jpegparse -> mppjpegdec -> appsink 管线。
     *
     * appsink 只保留最新一帧，避免推理较慢时在采集链路中累计历史帧。
     */
    bool open(const CameraProfile &profile, QString *error) override
    {
        close();
        warnings_.clear();

        const QString pixelFormat = profile.pixelFormat.trimmed().toLower();
        if (pixelFormat != QStringLiteral("mjpeg") &&
            pixelFormat != QStringLiteral("mjpg")) {
            return fail(QStringLiteral(
                            "USB MJPEG backend requires pixel_format=mjpeg"),
                        error);
        }
        if (profile.devicePath.trimmed().isEmpty() ||
            profile.width <= 0 || profile.height <= 0 || profile.fps <= 0 ||
            (profile.width & 1) != 0 || (profile.height & 1) != 0) {
            return fail(QStringLiteral("USB MJPEG profile is invalid"), error);
        }
        if (!ensureGStreamer(error)) {
            return false;
        }

        devicePath_ = profile.devicePath.trimmed();
        width_ = profile.width;
        height_ = profile.height;
        fps_ = profile.fps;

        pipeline_ = gst_pipeline_new("facegate-usb-mjpeg-pipeline");
        GstElement *source = gst_element_factory_make("v4l2src", "camera-source");
        GstElement *jpegCapsFilter =
            gst_element_factory_make("capsfilter", "jpeg-caps");
        GstElement *jpegParser = gst_element_factory_make("jpegparse", "jpeg-parser");
        GstElement *decoder = gst_element_factory_make("mppjpegdec", "mjpeg-decoder");
        GstElement *rawCapsFilter =
            gst_element_factory_make("capsfilter", "nv12-caps");
        appSink_ = gst_element_factory_make("appsink", "camera-sink");

        if (!pipeline_ || !source || !jpegCapsFilter || !jpegParser ||
            !decoder || !rawCapsFilter || !appSink_) {
            if (pipeline_) {
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
            }
            unrefUnparented(source);
            unrefUnparented(jpegCapsFilter);
            unrefUnparented(jpegParser);
            unrefUnparented(decoder);
            unrefUnparented(rawCapsFilter);
            unrefUnparented(appSink_);
            appSink_ = nullptr;
            return fail(QStringLiteral(
                            "Unable to create USB MJPEG GStreamer elements; "
                            "v4l2src/jpegparse/mppjpegdec/appsink are required"),
                        error);
        }

        const QByteArray deviceBytes = devicePath_.toLocal8Bit();
        g_object_set(source,
                     "device", deviceBytes.constData(),
                     "do-timestamp", TRUE,
                     nullptr);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "io-mode")) {
            gst_util_set_object_arg(G_OBJECT(source), "io-mode", "mmap");
        }
        gst_util_set_object_arg(G_OBJECT(decoder), "format", "NV12");
        g_object_set(appSink_,
                     "emit-signals", FALSE,
                     "sync", FALSE,
                     "max-buffers", 1u,
                     "drop", TRUE,
                     "enable-last-sample", FALSE,
                     "wait-on-eos", FALSE,
                     nullptr);

        GstCaps *jpegCaps = gst_caps_new_simple(
            "image/jpeg",
            "width", G_TYPE_INT, width_,
            "height", G_TYPE_INT, height_,
            "framerate", GST_TYPE_FRACTION, fps_, 1,
            nullptr);
        GstCaps *rawCaps = gst_caps_new_simple(
            "video/x-raw",
            "format", G_TYPE_STRING, "NV12",
            nullptr);
        g_object_set(jpegCapsFilter, "caps", jpegCaps, nullptr);
        g_object_set(rawCapsFilter, "caps", rawCaps, nullptr);
        g_object_set(appSink_, "caps", rawCaps, nullptr);
        gst_caps_unref(jpegCaps);
        gst_caps_unref(rawCaps);

        gst_bin_add_many(GST_BIN(pipeline_),
                         source,
                         jpegCapsFilter,
                         jpegParser,
                         decoder,
                         rawCapsFilter,
                         appSink_,
                         nullptr);
        if (!gst_element_link_many(source,
                                   jpegCapsFilter,
                                   jpegParser,
                                   decoder,
                                   rawCapsFilter,
                                   appSink_,
                                   nullptr)) {
            return fail(QStringLiteral("Unable to link USB MJPEG GStreamer pipeline"),
                        error);
        }

        bus_ = gst_element_get_bus(pipeline_);
        const GstStateChangeReturn setResult =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (setResult == GST_STATE_CHANGE_FAILURE) {
            QString busError;
            takeBusFailure(bus_, &busError);
            return fail(busError.isEmpty()
                            ? QStringLiteral("USB MJPEG pipeline failed to start")
                            : busError,
                        error);
        }

        const GstStateChangeReturn waitResult =
            gst_element_get_state(pipeline_, nullptr, nullptr, 3 * GST_SECOND);
        if (waitResult == GST_STATE_CHANGE_FAILURE ||
            waitResult == GST_STATE_CHANGE_ASYNC) {
            QString busError;
            takeBusFailure(bus_, &busError);
            return fail(busError.isEmpty()
                            ? QStringLiteral(
                                  "USB MJPEG pipeline did not reach PLAYING")
                            : busError,
                        error);
        }

        streaming_ = true;
        return true;
    }

    /** @brief 在超时内从 appsink 拉取一帧，管线错误和 EOS 均作为失败返回。 */
    CameraWaitResult waitForFrame(int timeoutMs, QString *error) override
    {
        if (!streaming_ || !appSink_) {
            if (error) {
                *error = QStringLiteral("USB MJPEG pipeline is not running");
            }
            return CameraWaitResult::Error;
        }
        if (sample_) {
            return CameraWaitResult::FrameReady;
        }

        sample_ = gst_app_sink_try_pull_sample(
            GST_APP_SINK(appSink_),
            static_cast<GstClockTime>(qMax(0, timeoutMs)) * GST_MSECOND);
        if (sample_) {
            return CameraWaitResult::FrameReady;
        }
        if (takeBusFailure(bus_, error) ||
            gst_app_sink_is_eos(GST_APP_SINK(appSink_))) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("USB MJPEG appsink reached EOS");
            }
            return CameraWaitResult::Error;
        }
        return CameraWaitResult::Timeout;
    }

    /**
     * @brief 映射解码后的 NV12 平面，并按有效宽度去除每行 stride 填充。
     *
     * 输出复制到连续缓冲区，确保后续 RGA 处理不依赖 GStreamer 平面布局。
     */
    bool dequeue(CapturedCameraBuffer *captured, QString *error) override
    {
        if (!captured || !sample_ || frameMapped_) {
            if (error) {
                *error = QStringLiteral("USB MJPEG dequeue state is invalid");
            }
            return false;
        }

        GstCaps *caps = gst_sample_get_caps(sample_);
        GstBuffer *buffer = gst_sample_get_buffer(sample_);
        GstVideoInfo info;
        gst_video_info_init(&info);
        if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
            GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
            if (error) {
                *error = QStringLiteral(
                    "USB MJPEG appsink did not provide a valid NV12 frame");
            }
            return false;
        }
        if (!gst_video_frame_map(&videoFrame_, &info, buffer, GST_MAP_READ)) {
            if (error) {
                *error = QStringLiteral("Unable to map decoded USB NV12 frame");
            }
            return false;
        }
        frameMapped_ = true;

        const int decodedWidth = GST_VIDEO_FRAME_WIDTH(&videoFrame_);
        const int decodedHeight = GST_VIDEO_FRAME_HEIGHT(&videoFrame_);
        const int yStride = GST_VIDEO_FRAME_PLANE_STRIDE(&videoFrame_, 0);
        const int uvStride = GST_VIDEO_FRAME_PLANE_STRIDE(&videoFrame_, 1);
        const auto *yData = static_cast<const uchar *>(
            GST_VIDEO_FRAME_PLANE_DATA(&videoFrame_, 0));
        const auto *uvData = static_cast<const uchar *>(
            GST_VIDEO_FRAME_PLANE_DATA(&videoFrame_, 1));
        if (!yData || !uvData || decodedWidth <= 0 || decodedHeight <= 0 ||
            (decodedWidth & 1) != 0 || (decodedHeight & 1) != 0 ||
            yStride < decodedWidth || uvStride < decodedWidth) {
            if (error) {
                *error = QStringLiteral("Decoded USB NV12 plane layout is invalid");
            }
            releaseCurrentSample();
            return false;
        }

        width_ = decodedWidth;
        height_ = decodedHeight;
        const qint64 yBytes64 = static_cast<qint64>(width_) * height_;
        const qint64 totalBytes64 = yBytes64 + yBytes64 / 2;
        if (totalBytes64 <= 0 ||
            totalBytes64 > std::numeric_limits<int>::max()) {
            if (error) {
                *error = QStringLiteral("Decoded USB NV12 frame is too large");
            }
            releaseCurrentSample();
            return false;
        }
        const int yBytes = static_cast<int>(yBytes64);
        const int totalBytes = static_cast<int>(totalBytes64);
        nv12Frame_.resize(totalBytes);
        auto *destination = reinterpret_cast<uchar *>(nv12Frame_.data());
        for (int row = 0; row < height_; ++row) {
            std::memcpy(destination + static_cast<qint64>(row) * width_,
                        yData + static_cast<qint64>(row) * yStride,
                        static_cast<std::size_t>(width_));
        }
        uchar *destinationUv = destination + yBytes;
        for (int row = 0; row < height_ / 2; ++row) {
            std::memcpy(destinationUv + static_cast<qint64>(row) * width_,
                        uvData + static_cast<qint64>(row) * uvStride,
                        static_cast<std::size_t>(width_));
        }

        captured->index = frameIndex_++;
        captured->bytesUsed = gst_buffer_get_size(buffer);
        captured->source = RgaImageProcessor::Buffer::fromVirtualAddress(
            nv12Frame_.data(),
            width_,
            height_,
            width_,
            height_,
            RgaImageProcessor::PixelFormat::Nv12);
        return true;
    }

    /** @brief 释放当前映射帧和样本，使 appsink 可以继续提供下一帧。 */
    bool requeue(const CapturedCameraBuffer &captured, QString *error) override
    {
        Q_UNUSED(captured)
        if (!sample_) {
            if (error) {
                *error = QStringLiteral("USB MJPEG sample is not outstanding");
            }
            return false;
        }
        releaseCurrentSample();
        return true;
    }

    /**
     * @brief 先释放未归还样本，再等待管线进入 NULL 状态并解除引用。
     *
     * NULL 状态是 MPP 解码器释放硬件资源的关键边界；超时会记录非致命警告。
     */
    void close() override
    {
        streaming_ = false;
        releaseCurrentSample();

        if (pipeline_ && !setNullStateAndWait(pipeline_)) {
            const QString warning = QStringLiteral(
                "USB MJPEG pipeline did not reach NULL; MPP release is uncertain");
            warnings_.append(warning);
            qWarning().noquote() << "[CAMERA-GST]" << warning;
        }
        if (bus_) {
            gst_bus_set_flushing(bus_, TRUE);
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (pipeline_) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        appSink_ = nullptr;
        nv12Frame_.clear();
        width_ = 0;
        height_ = 0;
        fps_ = 0;
        frameIndex_ = 0;
    }

    /** @return 最近一帧解码后的实际尺寸。 */
    QSize captureSize() const override
    {
        return QSize(width_, height_);
    }

    /** @return 后端提供给 RGA 的连续 NV12 格式。 */
    RgaImageProcessor::PixelFormat normalizedOutputFormat() const override
    {
        return RgaImageProcessor::PixelFormat::Nv12;
    }

    /** @return 摄像头输入压缩格式名称 MJPEG。 */
    QString inputFormatName() const override
    {
        return QStringLiteral("MJPEG");
    }

    /** @return 解码后帧的内存来源说明。 */
    QString inputMemoryName() const override
    {
        return QStringLiteral("GStreamer mapped NV12");
    }

    /** @return 当前 USB 节点和解码链路摘要。 */
    QString statusMessage() const override
    {
        return QStringLiteral(
                   "USB MJPEG camera started (%1, MJPEG -> MPP -> NV12)")
            .arg(devicePath_);
    }

    /** @return 管线关闭等阶段产生的非致命警告。 */
    QStringList warnings() const override
    {
        return warnings_;
    }

private:
    /** @brief 仅释放尚未加入管线、因创建失败而遗留的元素。 */
    static void unrefUnparented(GstElement *element)
    {
        if (element && !GST_OBJECT_PARENT(element)) {
            gst_object_unref(element);
        }
    }

    /** @brief 写入错误、关闭半初始化管线并统一返回 false。 */
    bool fail(const QString &message, QString *error)
    {
        if (error) {
            *error = message;
        }
        close();
        return false;
    }

    /** @brief 按先解除帧映射、后释放样本的顺序归还当前帧。 */
    void releaseCurrentSample()
    {
        if (frameMapped_) {
            gst_video_frame_unmap(&videoFrame_);
            frameMapped_ = false;
        }
        if (sample_) {
            gst_sample_unref(sample_);
            sample_ = nullptr;
        }
    }

    QString devicePath_;
    QStringList warnings_;
    GstElement *pipeline_ = nullptr;
    GstElement *appSink_ = nullptr; // 由 pipeline_ 持有。
    GstBus *bus_ = nullptr;
    GstSample *sample_ = nullptr;
    GstVideoFrame videoFrame_{};
    bool frameMapped_ = false;
    bool streaming_ = false;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int frameIndex_ = 0;
    QByteArray nv12Frame_;
};

} // namespace
#endif

/**
 * @brief 创建使用 GStreamer/MPP 解码 MJPEG 的 USB 采集后端。
 * @return 非 Linux 平台返回空指针。
 */
std::unique_ptr<ICameraCaptureBackend> createUsbMjpegCaptureBackend()
{
#ifdef Q_OS_LINUX
    return std::make_unique<UsbMjpegCaptureBackend>();
#else
    return std::unique_ptr<ICameraCaptureBackend>();
#endif
}
