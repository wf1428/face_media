/**
 * @file CameraCaptureBackend.cpp
 * @brief 实现 V4L2、USB 和平台摄像头采集后端及兼容回退。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "CameraCaptureBackend.h"
#include "UsbMjpegCaptureBackend.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <vector>
#include <memory>

#ifdef Q_OS_LINUX
namespace {

/** @brief 一个 V4L2 映射缓冲区及其可选 DMA-BUF 导出句柄。 */
struct MappedBuffer {
    void *address = nullptr;   /**< mmap 得到的 CPU 虚拟地址。 */
    std::size_t length = 0;    /**< 映射长度，单位字节。 */
    int dmaFd = -1;            /**< 导出的 DMA-BUF fd；-1 表示未导出。 */
};

/** @brief 在 ioctl 被信号中断时重试，保留其他系统错误。 */
int xioctl(int fd, unsigned long request, void *arg)
{
    int result = 0;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);
    return result;
}

/** @brief 把当前 errno 转换为包含上下文的可读错误信息。 */
QString errnoText(const QString &operation)
{
    return QStringLiteral("%1 failed (errno=%2)").arg(operation).arg(errno);
}

/** @brief 解除 V4L2 映射并关闭全部 DMA-BUF，避免采集重启时泄漏。 */
void releaseBuffers(std::vector<MappedBuffer> *buffers)
{
    if (!buffers) {
        return;
    }
    for (MappedBuffer &buffer : *buffers) {
        if (buffer.dmaFd >= 0) {
            ::close(buffer.dmaFd);
            buffer.dmaFd = -1;
        }
        if (buffer.address) {
            ::munmap(buffer.address, buffer.length);
            buffer.address = nullptr;
        }
    }
    buffers->clear();
}

/** @brief 优先返回 device_caps，兼容未提供该字段的旧 V4L2 驱动。 */
__u32 effectiveCapabilities(const v4l2_capability &capability)
{
    return (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
               ? capability.device_caps
               : capability.capabilities;
}

/**
 * @brief RK3566 MIPI 摄像头采集后端。
 *
 * 使用 V4L2 多平面采集和 DMA-BUF，把驱动缓冲区直接交给 RGA，减少 CPU 拷贝。
 */
class MipiCaptureBackend final : public ICameraCaptureBackend {
public:
    /** @brief 确保停止采集并释放映射缓冲区和设备句柄。 */
    ~MipiCaptureBackend() override
    {
        close();
    }

    /** @brief 按配置协商 MIPI 格式、申请 DMA 缓冲并启动视频流。 */
    bool open(const CameraProfile &profile, QString *error) override
    {
        close();
        devicePath_ = profile.devicePath;
        const QByteArray device = devicePath_.toLocal8Bit();
        fd_ = ::open(device.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
        if (fd_ < 0) {
            return fail(QStringLiteral("Unable to open MIPI camera: %1").arg(devicePath_), error);
        }

        const bool requestNv21 = profile.pixelFormat.toLower() != QStringLiteral("nv12");
        v4l2_format format{};
        format.type = bufferType_;
        format.fmt.pix_mp.width = profile.width;
        format.fmt.pix_mp.height = profile.height;
        format.fmt.pix_mp.pixelformat =
            requestNv21 ? V4L2_PIX_FMT_NV21 : V4L2_PIX_FMT_NV12;
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
            return fail(errnoText(QStringLiteral("MIPI VIDIOC_S_FMT")), error);
        }

        if (format.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV21) {
            inputFormat_ = RgaImageProcessor::PixelFormat::Nv21;
            inputFormatName_ = QStringLiteral("NV21");
        } else if (format.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12) {
            inputFormat_ = RgaImageProcessor::PixelFormat::Nv12;
            inputFormatName_ = QStringLiteral("NV12");
        } else {
            return fail(QStringLiteral("MIPI camera did not negotiate NV12/NV21"), error);
        }

        width_ = static_cast<int>(format.fmt.pix_mp.width);
        height_ = static_cast<int>(format.fmt.pix_mp.height);
        numPlanes_ = qMax(1, static_cast<int>(format.fmt.pix_mp.num_planes));
        stride_ = qMax(width_,
                       static_cast<int>(format.fmt.pix_mp.plane_fmt[0].bytesperline));
        if (numPlanes_ != 1) {
            return fail(QStringLiteral(
                            "MIPI DMA-BUF/RGA path requires single-plane NV12/NV21; "
                            "driver returned %1 planes")
                            .arg(numPlanes_),
                        error);
        }

        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = bufferType_;
        request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) {
            return fail(errnoText(QStringLiteral("MIPI VIDIOC_REQBUFS")), error);
        }

        buffers_.resize(request.count);
        for (uint index = 0; index < request.count; ++index) {
            v4l2_plane planes[VIDEO_MAX_PLANES]{};
            v4l2_buffer buffer{};
            buffer.type = bufferType_;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            buffer.length = numPlanes_;
            buffer.m.planes = planes;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
                return fail(errnoText(QStringLiteral("MIPI VIDIOC_QUERYBUF")), error);
            }

            MappedBuffer &mapped = buffers_[index];
            mapped.length = planes[0].length;
            mapped.address = ::mmap(nullptr,
                                    mapped.length,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED,
                                    fd_,
                                    planes[0].m.mem_offset);
            if (mapped.address == MAP_FAILED) {
                mapped.address = nullptr;
                return fail(errnoText(QStringLiteral("MIPI buffer mmap")), error);
            }

            v4l2_exportbuffer exportBuffer{};
            exportBuffer.type = bufferType_;
            exportBuffer.index = index;
            exportBuffer.plane = 0;
            exportBuffer.flags = O_CLOEXEC;
            if (xioctl(fd_, VIDIOC_EXPBUF, &exportBuffer) < 0) {
                return fail(QStringLiteral(
                                "MIPI VIDIOC_EXPBUF failed; RGA DMA-BUF input is unavailable"),
                            error);
            }
            mapped.dmaFd = exportBuffer.fd;
        }

        for (uint index = 0; index < request.count; ++index) {
            v4l2_plane planes[VIDEO_MAX_PLANES]{};
            v4l2_buffer buffer{};
            buffer.type = bufferType_;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            buffer.length = numPlanes_;
            buffer.m.planes = planes;
            if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
                return fail(errnoText(QStringLiteral("MIPI initial VIDIOC_QBUF")), error);
            }
        }

        if (xioctl(fd_, VIDIOC_STREAMON, &bufferType_) < 0) {
            return fail(errnoText(QStringLiteral("MIPI VIDIOC_STREAMON")), error);
        }
        streaming_ = true;
        return true;
    }

    /** @brief 使用 poll 等待 MIPI 帧，区分帧就绪、超时和系统错误。 */
    CameraWaitResult waitForFrame(int timeoutMs, QString *error) override
    {
        pollfd descriptor{};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        const int result = ::poll(&descriptor, 1, timeoutMs);
        if (result == 0 || (result < 0 && errno == EINTR)) {
            return CameraWaitResult::Timeout;
        }
        if (result < 0) {
            if (error) {
                *error = errnoText(QStringLiteral("MIPI poll"));
            }
            return CameraWaitResult::Error;
        }
        return CameraWaitResult::FrameReady;
    }

    /** @brief 从 V4L2 队列取出一帧并导出可供 RGA 使用的 DMA 描述。 */
    bool dequeue(CapturedCameraBuffer *captured, QString *error) override
    {
        if (!captured) {
            if (error) {
                *error = QStringLiteral("MIPI dequeue output is null");
            }
            return false;
        }

        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buffer{};
        buffer.type = bufferType_;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = numPlanes_;
        buffer.m.planes = planes;
        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                return false;
            }
            if (error) {
                *error = errnoText(QStringLiteral("MIPI VIDIOC_DQBUF"));
            }
            return false;
        }
        if (buffer.index >= buffers_.size()) {
            if (error) {
                *error = QStringLiteral("MIPI driver returned an invalid buffer index");
            }
            return false;
        }

        const MappedBuffer &mapped = buffers_[buffer.index];
        captured->index = static_cast<int>(buffer.index);
        captured->bytesUsed = planes[0].bytesused;
        captured->source = RgaImageProcessor::Buffer::fromDmaFd(
            mapped.dmaFd, width_, height_, stride_, height_, inputFormat_);
        return true;
    }

    /** @brief 将算法处理完成的 MIPI 缓冲区归还 V4L2 队列。 */
    bool requeue(const CapturedCameraBuffer &captured, QString *error) override
    {
        if (captured.index < 0 ||
            static_cast<std::size_t>(captured.index) >= buffers_.size()) {
            if (error) {
                *error = QStringLiteral("MIPI requeue buffer index is invalid");
            }
            return false;
        }
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buffer{};
        buffer.type = bufferType_;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<__u32>(captured.index);
        buffer.length = numPlanes_;
        buffer.m.planes = planes;
        if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            if (error) {
                *error = errnoText(QStringLiteral("MIPI VIDIOC_QBUF return"));
            }
            return false;
        }
        return true;
    }

    /** @brief 先停止视频流，再解除映射并关闭设备，避免驱动继续访问缓冲区。 */
    void close() override
    {
        if (streaming_ && fd_ >= 0) {
            xioctl(fd_, VIDIOC_STREAMOFF, &bufferType_);
        }
        streaming_ = false;
        releaseBuffers(&buffers_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /** @return 驱动最终协商的 MIPI 采集尺寸。 */
    QSize captureSize() const override { return QSize(width_, height_); }
    /** @return MIPI 后端交给后续处理的标准化像素格式。 */
    RgaImageProcessor::PixelFormat normalizedOutputFormat() const override
    {
        return inputFormat_;
    }
    /** @return 驱动实际输入像素格式名称。 */
    QString inputFormatName() const override { return inputFormatName_; }
    /** @return MIPI 后端使用的输入内存模式。 */
    QString inputMemoryName() const override { return QStringLiteral("DMA-BUF"); }
    /** @return 包含节点、格式和尺寸的初始化状态摘要。 */
    QString statusMessage() const override
    {
        return QStringLiteral("MIPI camera started (DMA-BUF -> RGA)");
    }
    /** @return 初始化期间记录的非致命回退警告。 */
    QStringList warnings() const override { return warnings_; }

private:
    /** @brief 保存错误文本、关闭半初始化资源并向调用方返回失败。 */
    bool fail(const QString &message, QString *error)
    {
        if (error) {
            *error = message;
        }
        close();
        return false;
    }

    QString devicePath_;
    int fd_ = -1;
    bool streaming_ = false;
    v4l2_buf_type bufferType_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int numPlanes_ = 1;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    RgaImageProcessor::PixelFormat inputFormat_ =
        RgaImageProcessor::PixelFormat::Nv12;
    QString inputFormatName_;
    QStringList warnings_;
    std::vector<MappedBuffer> buffers_;
};

/** @brief 检查视频节点能力和驱动信息，确认其是否为可用 USB 摄像头。 */
bool queryUsbVideoNode(const QString &path, bool requireUvc, int *fd)
{
    if (!fd) {
        return false;
    }
    const QByteArray pathBytes = path.toLocal8Bit();
    const int candidate = ::open(pathBytes.constData(),
                                 O_RDWR | O_NONBLOCK | O_CLOEXEC,
                                 0);
    if (candidate < 0) {
        return false;
    }

    v4l2_capability capability{};
    if (xioctl(candidate, VIDIOC_QUERYCAP, &capability) < 0) {
        ::close(candidate);
        return false;
    }
    const __u32 caps = effectiveCapabilities(capability);
    const bool isCapture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
    const bool isStreaming = (caps & V4L2_CAP_STREAMING) != 0;
    const bool isUvc =
        QString::fromLatin1(reinterpret_cast<const char *>(capability.driver))
            .trimmed() == QStringLiteral("uvcvideo");
    if (!isCapture || !isStreaming || (requireUvc && !isUvc)) {
        ::close(candidate);
        return false;
    }
    *fd = candidate;
    return true;
}

/** @brief 扫描视频节点并返回第一个满足采集要求的 USB 摄像头。 */
QString findUsbVideoNode(int *fd)
{
    QDir dev(QStringLiteral("/dev"));
    const QStringList entries =
        dev.entryList(QStringList() << QStringLiteral("video*"),
                      QDir::System | QDir::Files | QDir::NoDotAndDotDot,
                      QDir::Name);
    for (const QString &entry : entries) {
        const QString path = dev.absoluteFilePath(entry);
        if (queryUsbVideoNode(path, true, fd)) {
            return path;
        }
    }
    return QString();
}

/** @brief 自动探测阶段记录一个 UVC 视频节点支持的原始像素格式。 */
struct UsbCameraCandidate {
    QString devicePath;
    bool supportsYuyv = false;
    bool supportsMjpeg = false;
};

/** @return 全部具有 Video Capture/Streaming 能力的 UVC 节点及其格式。 */
std::vector<UsbCameraCandidate> probeUsbCameraCandidates()
{
    std::vector<UsbCameraCandidate> candidates;
    QDir dev(QStringLiteral("/dev"));
    const QStringList entries =
        dev.entryList(QStringList() << QStringLiteral("video*"),
                      QDir::System | QDir::Files | QDir::NoDotAndDotDot,
                      QDir::Name);
    for (const QString &entry : entries) {
        const QString path = dev.absoluteFilePath(entry);
        int fd = -1;
        if (!queryUsbVideoNode(path, true, &fd)) {
            continue;
        }

        UsbCameraCandidate candidate;
        candidate.devicePath = path;
        for (__u32 index = 0;; ++index) {
            v4l2_fmtdesc format{};
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            format.index = index;
            if (xioctl(fd, VIDIOC_ENUM_FMT, &format) < 0) {
                break;
            }
            candidate.supportsYuyv =
                candidate.supportsYuyv ||
                format.pixelformat == V4L2_PIX_FMT_YUYV;
            candidate.supportsMjpeg =
                candidate.supportsMjpeg ||
                format.pixelformat == V4L2_PIX_FMT_MJPEG;
        }
        ::close(fd);

        if (candidate.supportsYuyv || candidate.supportsMjpeg) {
            candidates.push_back(candidate);
        }
    }
    return candidates;
}

/** @brief 记录摄像头控制项不支持等非致命兼容警告。 */
void appendControlWarning(QStringList *warnings,
                          const QString &name,
                          const QString &reason)
{
    if (warnings) {
        warnings->append(QStringLiteral("USB control %1: %2").arg(name, reason));
    }
}

/** @brief 设置 V4L2 摄像头控制项，并把不支持情况降级为警告。 */
bool setUsbControl(int fd,
                   __u32 id,
                   int requestedValue,
                   const QString &name,
                   QStringList *warnings)
{
    v4l2_queryctrl query{};
    query.id = id;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &query) < 0 ||
        (query.flags & V4L2_CTRL_FLAG_DISABLED)) {
        appendControlWarning(warnings, name, QStringLiteral("unsupported"));
        return false;
    }

    int value = qBound(static_cast<int>(query.minimum),
                       requestedValue,
                       static_cast<int>(query.maximum));
    if (query.step > 1) {
        value = static_cast<int>(query.minimum) +
                ((value - static_cast<int>(query.minimum)) /
                 static_cast<int>(query.step)) *
                    static_cast<int>(query.step);
    }

    v4l2_control control{};
    control.id = id;
    control.value = value;
    if (xioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
        appendControlWarning(
            warnings,
            name,
            QStringLiteral("set %1 failed (errno=%2)").arg(value).arg(errno));
        return false;
    }
    return true;
}

/**
 * @brief USB UVC 摄像头采集后端。
 *
 * 使用 V4L2 单平面 MMAP 缓冲，输出 YUYV 并在启动时尽力应用亮度、曝光等控制项。
 */
class UsbCaptureBackend final : public ICameraCaptureBackend {
public:
    /** @brief 确保停止采集并释放 USB 摄像头映射与句柄。 */
    ~UsbCaptureBackend() override
    {
        close();
    }

    /** @brief 查找 UVC 节点、协商 YUYV、申请 MMAP 缓冲并启动采集。 */
    bool open(const CameraProfile &profile, QString *error) override
    {
        close();
        warnings_.clear();
        dmaBufForAll_ = true;

        if (profile.pixelFormat.trimmed().toLower() != QStringLiteral("yuyv")) {
            return fail(QStringLiteral(
                            "USB camera backend currently requires pixel_format=yuyv"),
                        error);
        }

        const QString requestedDevice = profile.devicePath.trimmed();
        if (requestedDevice.isEmpty() ||
            requestedDevice.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
            devicePath_ = findUsbVideoNode(&fd_);
            if (fd_ < 0) {
                return fail(QStringLiteral(
                                "No UVC video-capture node was found under /dev/video*"),
                            error);
            }
        } else {
            devicePath_ = requestedDevice;
            if (!queryUsbVideoNode(devicePath_, false, &fd_)) {
                return fail(QStringLiteral(
                                "USB camera node is not a streaming video-capture device: %1")
                                .arg(devicePath_),
                            error);
            }
        }

        v4l2_format format{};
        format.type = bufferType_;
        format.fmt.pix.width = profile.width;
        format.fmt.pix.height = profile.height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
            return fail(errnoText(QStringLiteral("USB VIDIOC_S_FMT")), error);
        }
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            return fail(QStringLiteral("USB camera did not negotiate YUYV"), error);
        }

        width_ = static_cast<int>(format.fmt.pix.width);
        height_ = static_cast<int>(format.fmt.pix.height);
        strideBytes_ =
            qMax(width_ * 2, static_cast<int>(format.fmt.pix.bytesperline));
        stridePixels_ = strideBytes_ / 2;

        if (profile.fps > 0) {
            v4l2_streamparm parameters{};
            parameters.type = bufferType_;
            parameters.parm.capture.timeperframe.numerator = 1;
            parameters.parm.capture.timeperframe.denominator =
                static_cast<__u32>(profile.fps);
            if (xioctl(fd_, VIDIOC_S_PARM, &parameters) < 0) {
                warnings_.append(errnoText(QStringLiteral("USB VIDIOC_S_PARM")));
            }
        }

        applyControls(profile.usbControls);

        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = bufferType_;
        request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) {
            return fail(errnoText(QStringLiteral("USB VIDIOC_REQBUFS")), error);
        }

        buffers_.resize(request.count);
        for (uint index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = bufferType_;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
                return fail(errnoText(QStringLiteral("USB VIDIOC_QUERYBUF")), error);
            }

            MappedBuffer &mapped = buffers_[index];
            mapped.length = buffer.length;
            mapped.address = ::mmap(nullptr,
                                    mapped.length,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED,
                                    fd_,
                                    buffer.m.offset);
            if (mapped.address == MAP_FAILED) {
                mapped.address = nullptr;
                return fail(errnoText(QStringLiteral("USB buffer mmap")), error);
            }

            v4l2_exportbuffer exportBuffer{};
            exportBuffer.type = bufferType_;
            exportBuffer.index = index;
            exportBuffer.flags = O_CLOEXEC;
            if (xioctl(fd_, VIDIOC_EXPBUF, &exportBuffer) == 0) {
                mapped.dmaFd = exportBuffer.fd;
            } else {
                dmaBufForAll_ = false;
            }
        }
        if (!dmaBufForAll_) {
            warnings_.append(QStringLiteral(
                "USB VIDIOC_EXPBUF is unavailable; RGA uses mapped V4L2 buffers"));
        }

        for (uint index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type = bufferType_;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
                return fail(errnoText(QStringLiteral("USB initial VIDIOC_QBUF")), error);
            }
        }

        if (xioctl(fd_, VIDIOC_STREAMON, &bufferType_) < 0) {
            return fail(errnoText(QStringLiteral("USB VIDIOC_STREAMON")), error);
        }
        streaming_ = true;
        return true;
    }

    /** @brief 使用 poll 等待 USB 帧，区分帧就绪、超时和错误。 */
    CameraWaitResult waitForFrame(int timeoutMs, QString *error) override
    {
        pollfd descriptor{};
        descriptor.fd = fd_;
        descriptor.events = POLLIN;
        const int result = ::poll(&descriptor, 1, timeoutMs);
        if (result == 0 || (result < 0 && errno == EINTR)) {
            return CameraWaitResult::Timeout;
        }
        if (result < 0) {
            if (error) {
                *error = errnoText(QStringLiteral("USB poll"));
            }
            return CameraWaitResult::Error;
        }
        return CameraWaitResult::FrameReady;
    }

    /** @brief 从 USB 驱动队列取出一帧并返回 MMAP 虚拟地址描述。 */
    bool dequeue(CapturedCameraBuffer *captured, QString *error) override
    {
        if (!captured) {
            if (error) {
                *error = QStringLiteral("USB dequeue output is null");
            }
            return false;
        }
        v4l2_buffer buffer{};
        buffer.type = bufferType_;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                return false;
            }
            if (error) {
                *error = errnoText(QStringLiteral("USB VIDIOC_DQBUF"));
            }
            return false;
        }
        if (buffer.index >= buffers_.size()) {
            if (error) {
                *error = QStringLiteral("USB driver returned an invalid buffer index");
            }
            return false;
        }

        const MappedBuffer &mapped = buffers_[buffer.index];
        captured->index = static_cast<int>(buffer.index);
        captured->bytesUsed = buffer.bytesused;
        if (mapped.dmaFd >= 0) {
            captured->source = RgaImageProcessor::Buffer::fromDmaFd(
                mapped.dmaFd,
                width_,
                height_,
                stridePixels_,
                height_,
                RgaImageProcessor::PixelFormat::Yuyv422);
        } else {
            captured->source =
                RgaImageProcessor::Buffer::fromVirtualAddress(
                    mapped.address,
                    width_,
                    height_,
                    stridePixels_,
                    height_,
                    RgaImageProcessor::PixelFormat::Yuyv422);
        }
        return true;
    }

    /** @brief 将处理完成的 USB 缓冲区重新加入 V4L2 队列。 */
    bool requeue(const CapturedCameraBuffer &captured, QString *error) override
    {
        if (captured.index < 0 ||
            static_cast<std::size_t>(captured.index) >= buffers_.size()) {
            if (error) {
                *error = QStringLiteral("USB requeue buffer index is invalid");
            }
            return false;
        }
        v4l2_buffer buffer{};
        buffer.type = bufferType_;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<__u32>(captured.index);
        if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
            if (error) {
                *error = errnoText(QStringLiteral("USB VIDIOC_QBUF return"));
            }
            return false;
        }
        return true;
    }

    /** @brief 停止 USB 视频流并按映射、缓冲、设备顺序释放资源。 */
    void close() override
    {
        if (streaming_ && fd_ >= 0) {
            xioctl(fd_, VIDIOC_STREAMOFF, &bufferType_);
        }
        streaming_ = false;
        releaseBuffers(&buffers_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /** @return 驱动最终协商的 USB 采集尺寸。 */
    QSize captureSize() const override { return QSize(width_, height_); }
    /** @return USB 后端提供给 RGA 的标准化输入格式。 */
    RgaImageProcessor::PixelFormat normalizedOutputFormat() const override
    {
        return RgaImageProcessor::PixelFormat::Nv12;
    }
    /** @return USB 后端固定协商的 YUYV 格式名称。 */
    QString inputFormatName() const override { return QStringLiteral("YUYV"); }
    /** @return USB 后端使用的 V4L2 内存模式。 */
    QString inputMemoryName() const override
    {
        return dmaBufForAll_ ? QStringLiteral("DMA-BUF")
                            : QStringLiteral("MMAP virtual");
    }
    /** @return 包含 USB 节点、格式、内存模式和尺寸的状态摘要。 */
    QString statusMessage() const override
    {
        return QStringLiteral("USB camera started (%1, YUYV -> RGA -> NV12)")
            .arg(devicePath_);
    }
    /** @return 不支持控制项等非致命兼容警告。 */
    QStringList warnings() const override { return warnings_; }

private:
    /** @brief 保存错误文本、关闭半初始化资源并向调用方返回失败。 */
    bool fail(const QString &message, QString *error)
    {
        if (error) {
            *error = message;
        }
        close();
        return false;
    }

    /** @brief 尽力应用 USB 摄像头控制项，不支持的项仅记录警告。 */
    void applyControls(const UsbCameraControlConfig &controls)
    {
        setUsbControl(fd_,
                      V4L2_CID_POWER_LINE_FREQUENCY,
                      controls.powerLineFrequency,
                      QStringLiteral("power_line_frequency"),
                      &warnings_);
        setUsbControl(fd_,
                      V4L2_CID_AUTO_WHITE_BALANCE,
                      controls.autoWhiteBalance ? 1 : 0,
                      QStringLiteral("auto_white_balance"),
                      &warnings_);
        if (!controls.autoWhiteBalance) {
            setUsbControl(fd_,
                          V4L2_CID_WHITE_BALANCE_TEMPERATURE,
                          controls.whiteBalanceTemperature,
                          QStringLiteral("white_balance_temperature"),
                          &warnings_);
        }

        setUsbControl(fd_,
                      V4L2_CID_EXPOSURE_AUTO,
                      controls.autoExposure ? V4L2_EXPOSURE_APERTURE_PRIORITY
                                            : V4L2_EXPOSURE_MANUAL,
                      QStringLiteral("exposure_auto"),
                      &warnings_);
        setUsbControl(fd_,
                      V4L2_CID_EXPOSURE_AUTO_PRIORITY,
                      controls.exposureAutoPriority ? 1 : 0,
                      QStringLiteral("exposure_auto_priority"),
                      &warnings_);
        if (!controls.autoExposure) {
            setUsbControl(fd_,
                          V4L2_CID_EXPOSURE_ABSOLUTE,
                          controls.exposureAbsolute,
                          QStringLiteral("exposure_absolute"),
                          &warnings_);
        }
    }

    QString devicePath_;
    int fd_ = -1;
    bool streaming_ = false;
    bool dmaBufForAll_ = true;
    v4l2_buf_type bufferType_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int width_ = 0;
    int height_ = 0;
    int strideBytes_ = 0;
    int stridePixels_ = 0;
    QStringList warnings_;
    std::vector<MappedBuffer> buffers_;
};

/**
 * @brief 自动选择摄像头的代理后端。
 *
 * 按 UVC 节点逐个选择采集链路：双格式节点优先使用 MJPEG/MPP，失败后
 * 再回退 YUYV；单格式节点沿用对应后端。没有可用 USB 摄像头时把原配置
 * 交给 MIPI 后端。
 */
class AutoCaptureBackend final : public ICameraCaptureBackend {
public:
    /** @brief 关闭当前选中的实际后端。 */
    ~AutoCaptureBackend() override
    {
        close();
    }

    /**
     * @brief 双格式 USB 优先 MJPEG/MPP，失败后回退 YUYV，最后回退 MIPI。
     *
     * 前序候选的失败信息会保留为兼容警告，便于诊断为何发生后端回退。
     */
    bool open(const CameraProfile &profile, QString *error) override
    {
        close();
        selectionWarnings_.clear();

        const std::vector<UsbCameraCandidate> candidates =
            probeUsbCameraCandidates();

        const auto tryYuyv =
            [this, &profile](const UsbCameraCandidate &candidate) -> bool {
            CameraProfile usbProfile = usbProfileFor(
                profile, candidate.devicePath, QStringLiteral("yuyv"));
            auto backend = std::make_unique<UsbCaptureBackend>();
            QString backendError;
            if (backend->open(usbProfile, &backendError)) {
                selected_ = std::move(backend);
                selectedKind_ = QStringLiteral("USB YUYV");
                return true;
            }
            selectionWarnings_.append(
                QStringLiteral("Auto USB YUYV %1 failed: %2")
                    .arg(candidate.devicePath, backendError));
            return false;
        };

        const auto tryMjpeg =
            [this, &profile](const UsbCameraCandidate &candidate) -> bool {
            CameraProfile usbProfile = usbProfileFor(
                profile, candidate.devicePath, QStringLiteral("mjpeg"));
            auto backend = createUsbMjpegCaptureBackend();
            if (!backend) {
                selectionWarnings_.append(QStringLiteral(
                    "USB MJPEG backend is unavailable on this platform"));
                return false;
            }
            QString backendError;
            if (backend->open(usbProfile, &backendError)) {
                selected_ = std::move(backend);
                selectedKind_ = QStringLiteral("USB MJPEG/MPP");
                return true;
            }
            selectionWarnings_.append(
                QStringLiteral("Auto USB MJPEG %1 failed: %2")
                    .arg(candidate.devicePath, backendError));
            return false;
        };

        for (const UsbCameraCandidate &candidate : candidates) {
            if (candidate.supportsYuyv && candidate.supportsMjpeg) {
                qInfo().noquote()
                    << "[CAMERA-AUTO] dual-format USB camera; preferring MJPEG/MPP"
                    << candidate.devicePath;
                if (tryMjpeg(candidate) || tryYuyv(candidate)) {
                    return true;
                }
                continue;
            }
            if (candidate.supportsYuyv && tryYuyv(candidate)) {
                return true;
            }
            if (candidate.supportsMjpeg && tryMjpeg(candidate)) {
                return true;
            }
        }

        CameraProfile mipiProfile = profile;
        mipiProfile.source = CameraSourceType::Mipi;
        auto mipiBackend = std::make_unique<MipiCaptureBackend>();
        QString mipiError;
        if (mipiBackend->open(mipiProfile, &mipiError)) {
            selected_ = std::move(mipiBackend);
            selectedKind_ = QStringLiteral("MIPI");
            return true;
        }

        if (error) {
            QStringList details = selectionWarnings_;
            details.append(QStringLiteral("MIPI fallback failed: %1").arg(mipiError));
            *error = details.join(QStringLiteral("; "));
        }
        close();
        return false;
    }

    /** @brief 把帧等待请求转发给已选后端。 */
    CameraWaitResult waitForFrame(int timeoutMs, QString *error) override
    {
        if (!selected_) {
            if (error) {
                *error = QStringLiteral("Auto camera backend is not open");
            }
            return CameraWaitResult::Error;
        }
        return selected_->waitForFrame(timeoutMs, error);
    }

    /** @brief 从已选后端取出一帧。 */
    bool dequeue(CapturedCameraBuffer *buffer, QString *error) override
    {
        if (!selected_) {
            if (error) {
                *error = QStringLiteral("Auto camera backend is not open");
            }
            return false;
        }
        return selected_->dequeue(buffer, error);
    }

    /** @brief 把已处理帧归还给已选后端。 */
    bool requeue(const CapturedCameraBuffer &buffer, QString *error) override
    {
        if (!selected_) {
            if (error) {
                *error = QStringLiteral("Auto camera backend is not open");
            }
            return false;
        }
        return selected_->requeue(buffer, error);
    }

    /** @brief 关闭并释放已选后端，清空选择状态。 */
    void close() override
    {
        if (selected_) {
            selected_->close();
            selected_.reset();
        }
        selectedKind_.clear();
    }

    /** @return 已选后端的实际采集尺寸。 */
    QSize captureSize() const override
    {
        return selected_ ? selected_->captureSize() : QSize();
    }

    /** @return 已选后端输出格式；未选择时返回 NV12 默认值。 */
    RgaImageProcessor::PixelFormat normalizedOutputFormat() const override
    {
        return selected_ ? selected_->normalizedOutputFormat()
                         : RgaImageProcessor::PixelFormat::Nv12;
    }

    /** @return 已选后端的输入格式诊断名称。 */
    QString inputFormatName() const override
    {
        return selected_ ? selected_->inputFormatName() : QString();
    }

    /** @return 已选后端的输入内存模式名称。 */
    QString inputMemoryName() const override
    {
        return selected_ ? selected_->inputMemoryName() : QString();
    }

    /** @return 自动选择结果与实际后端状态摘要。 */
    QString statusMessage() const override
    {
        return selected_
                   ? QStringLiteral("Auto-selected %1: %2")
                         .arg(selectedKind_, selected_->statusMessage())
                   : QStringLiteral("Auto camera is not running");
    }

    /** @return 自动选择失败记录与已选后端警告的合并列表。 */
    QStringList warnings() const override
    {
        QStringList result = selectionWarnings_;
        if (selected_) {
            result.append(selected_->warnings());
        }
        return result;
    }

private:
    /** @brief 从基础配置生成指定 USB 节点和像素格式的候选配置。 */
    static CameraProfile usbProfileFor(const CameraProfile &base,
                                       const QString &devicePath,
                                       const QString &pixelFormat)
    {
        CameraProfile result = base;
        result.source = CameraSourceType::Usb;
        result.devicePath = devicePath;
        result.width = base.width > 0 ? base.width : 640;
        result.height = base.height > 0 ? base.height : 480;
        result.fps = base.fps > 0 ? base.fps : 30;
        result.pixelFormat = pixelFormat;
        return result;
    }

    std::unique_ptr<ICameraCaptureBackend> selected_; /**< 当前实际工作的采集后端。 */
    QString selectedKind_;                           /**< 自动选择结果名称。 */
    QStringList selectionWarnings_;                  /**< 前序候选的失败诊断。 */
};

} // namespace
#endif

/**
 * @brief 按摄像头来源创建采集后端。
 * @return 非 Linux 平台返回空指针；Linux 平台返回自动、USB 或 MIPI 后端。
 */
std::unique_ptr<ICameraCaptureBackend>
createCameraCaptureBackend(CameraSourceType source)
{
#ifndef Q_OS_LINUX
    Q_UNUSED(source)
    return std::unique_ptr<ICameraCaptureBackend>();
#else
    if (source == CameraSourceType::Auto) {
        return std::unique_ptr<ICameraCaptureBackend>(new AutoCaptureBackend);
    }
    if (source == CameraSourceType::Usb) {
        return std::unique_ptr<ICameraCaptureBackend>(new UsbCaptureBackend);
    }
    return std::unique_ptr<ICameraCaptureBackend>(new MipiCaptureBackend);
#endif
}
