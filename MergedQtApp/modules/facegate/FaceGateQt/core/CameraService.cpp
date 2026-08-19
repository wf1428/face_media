/**
 * @file CameraService.cpp
 * @brief 摄像头采集线程服务的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

/*
 * V4L2 capture service for the FaceGate module.
 * Capture backends normalize camera-specific input through RGA while the
 * downstream CameraFrame contract remains unchanged.
 */

#include "CameraService.h"

#include "CameraCaptureBackend.h"
#include "RgaImageProcessor.h"

#include <QDebug>

#include <memory>
#include <utility>

namespace {

/** @brief 把采集缓冲区通过 RGA 转换为人脸算法需要的连续 RGB 帧。 */
bool buildRgaFrame(const RgaImageProcessor::Buffer &sourceImage,
                   RgaImageProcessor::PixelFormat outputFormat,
                   int frameIndex,
                   CameraFrame *frame,
                   QString *error)
{
    if (!frame ||
        sourceImage.width <= 0 ||
        sourceImage.height <= 0 ||
        sourceImage.widthStride < sourceImage.width ||
        (sourceImage.dmaFd < 0 && !sourceImage.virtualAddress)) {
        if (error) {
            *error = QStringLiteral("RGA input parameters are invalid");
        }
        return false;
    }

    /** @brief 缩短归一化过程中 RGA 缓冲区类型的限定名。 */
    using RgaImageProcessor::Buffer;
    /** @brief 缩短归一化过程中 RGA 像素格式类型的限定名。 */
    using RgaImageProcessor::PixelFormat;

    if (outputFormat != PixelFormat::Nv12 &&
        outputFormat != PixelFormat::Nv21) {
        if (error) {
            *error = QStringLiteral("Camera normalized output must be NV12/NV21");
        }
        return false;
    }

    const int width = sourceImage.width;
    const int height = sourceImage.height;
    CameraFrame result;
    result.yuv420sp.resize(width * height * 3 / 2);
    const Buffer yuvImage = Buffer::fromVirtualAddress(
        result.yuv420sp.data(), width, height, width, height, outputFormat);

    // This normalized YUV copy remains valid after the V4L2 buffer is returned.
    if (!RgaImageProcessor::process(sourceImage, yuvImage, error)) {
        return false;
    }

    result.image = QImage(width, height, QImage::Format_RGB888);
    if (result.image.isNull()) {
        if (error) {
            *error = QStringLiteral("Unable to allocate the RGB preview buffer");
        }
        return false;
    }

    const Buffer rgbImage = Buffer::fromVirtualAddress(
        result.image.bits(), width, height, width, height, PixelFormat::Rgb888);
    if (!RgaImageProcessor::process(sourceImage, rgbImage, error)) {
        return false;
    }

    constexpr int kEnrollPreviewWidth = 512;
    constexpr int kEnrollPreviewHeight = 384;
    result.enrollmentPreview =
        QImage(kEnrollPreviewWidth, kEnrollPreviewHeight, QImage::Format_RGB888);
    if (result.enrollmentPreview.isNull()) {
        if (error) {
            *error = QStringLiteral("Unable to allocate the enrollment preview buffer");
        }
        return false;
    }

    const Buffer enrollmentPreview = Buffer::fromVirtualAddress(
        result.enrollmentPreview.bits(),
        kEnrollPreviewWidth,
        kEnrollPreviewHeight,
        kEnrollPreviewWidth,
        kEnrollPreviewHeight,
        PixelFormat::Rgb888);
    if (!RgaImageProcessor::process(rgbImage, enrollmentPreview, error)) {
        return false;
    }

    const bool nv21 = outputFormat == PixelFormat::Nv21;
    result.pixelFormat = nv21 ? QStringLiteral("nv21") : QStringLiteral("nv12");
    result.width = width;
    result.height = height;
    result.frameIndex = frameIndex;
    result.nv21 = nv21;
    *frame = std::move(result);
    return true;
}

} // namespace

/** @brief 创建尚未启动的采集服务。 */
CameraService::CameraService(QObject *parent)
    : QObject(parent)
{
}

/** @brief 销毁前停止并等待采集线程退出。 */
CameraService::~CameraService()
{
    stop();
}

/** @brief 使用兼容参数构造 MIPI 配置并启动采集。 */
bool CameraService::start(const QString &devicePath,
                          int width,
                          int height,
                          const QString &pixelFormat)
{
    CameraProfile profile;
    profile.source = CameraSourceType::Mipi;
    profile.devicePath = devicePath;
    profile.width = width;
    profile.height = height;
    profile.pixelFormat = pixelFormat;
    return start(profile);
}

/** @brief 使用兼容参数构造 MIPI 配置并启动采集。 */
bool CameraService::start(const CameraProfile &profile)
{
    if (running_) {
        return true;
    }

    profile_ = profile;
    profile_.pixelFormat = profile_.pixelFormat.trimmed().toLower();
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_ = CameraFrame();
        frameDeliveryPending_ = false;
    }
    running_ = true;
    captureThread_ = std::thread(&CameraService::captureLoop, this);
    return true;
}

/** @brief 请求采集线程停止并等待退出。 */
void CameraService::stop()
{
    running_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrame_ = CameraFrame();
    frameDeliveryPending_ = false;
}

/** @return 采集线程运行标志。 */
bool CameraService::isRunning() const
{
    return running_;
}

/**
 * @brief 取出最近一帧的副本并清除待投递标志。
 * @return 当前存在尚未领取的最新帧时返回 true。
 */
bool CameraService::takeLatestFrame(CameraFrame *frame)
{
    if (!frame) {
        return false;
    }

    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!frameDeliveryPending_) {
        return false;
    }

    *frame = std::move(latestFrame_);
    latestFrame_ = CameraFrame();
    frameDeliveryPending_ = false;
    return true;
}

/** @brief 采集线程主循环，确保每个出队缓冲区最终重新入队。 */
void CameraService::captureLoop()
{
#ifndef Q_OS_LINUX
    emit cameraError(QStringLiteral("CameraService requires embedded Linux V4L2"));
    running_ = false;
#else
    std::unique_ptr<ICameraCaptureBackend> backend =
        createCameraCaptureBackend(profile_.source);
    if (!backend) {
        emit cameraError(QStringLiteral("Unable to create the camera capture backend"));
        running_ = false;
        return;
    }

    QString backendError;
    if (!backend->open(profile_, &backendError)) {
        emit cameraError(backendError);
        running_ = false;
        return;
    }

    for (const QString &warning : backend->warnings()) {
        qWarning() << "[CAMERA-BACKEND]" << warning;
    }
    emit cameraStatus(backend->statusMessage());

    int frameIndex = 0;
    quint64 busyDropCount = 0;
    bool firstRgaFrame = true;
    while (running_) {
        backendError.clear();
        const CameraWaitResult waitResult =
            backend->waitForFrame(200, &backendError);
        if (waitResult == CameraWaitResult::Timeout) {
            continue;
        }
        if (waitResult == CameraWaitResult::Error) {
            emit cameraError(backendError);
            running_ = false;
            break;
        }

        CapturedCameraBuffer captured;
        backendError.clear();
        if (!backend->dequeue(&captured, &backendError)) {
            if (!backendError.isEmpty()) {
                emit cameraError(backendError);
                running_ = false;
            }
            continue;
        }

        bool shouldConvert = false;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            if (!frameDeliveryPending_) {
                frameDeliveryPending_ = true;
                shouldConvert = true;
            }
        }

        if (shouldConvert) {
            CameraFrame frame;
            QString rgaError;
            if (buildRgaFrame(captured.source,
                              backend->normalizedOutputFormat(),
                              frameIndex,
                              &frame,
                              &rgaError)) {
                {
                    std::lock_guard<std::mutex> lock(frameMutex_);
                    latestFrame_ = std::move(frame);
                }
                emit frameAvailable();
                if (firstRgaFrame) {
                    firstRgaFrame = false;
                    qInfo() << "[CAMERA-RGA] first frame ready"
                            << "input=" << backend->inputFormatName()
                            << "memory=" << backend->inputMemoryName()
                            << "size=" << backend->captureSize()
                            << "stride=" << captured.source.widthStride
                            << "output="
                            << (backend->normalizedOutputFormat() ==
                                        RgaImageProcessor::PixelFormat::Nv21
                                    ? "NV21"
                                    : "NV12")
                            << "CPU fallback=disabled";
                }
            } else {
                {
                    std::lock_guard<std::mutex> lock(frameMutex_);
                    frameDeliveryPending_ = false;
                }
                emit cameraError(rgaError);
                running_ = false;
            }
        } else {
            ++busyDropCount;
            if ((busyDropCount % 120) == 0) {
                qInfo() << "[CAMERA-MAILBOX] dropped busy frames=" << busyDropCount;
            }
        }

        backendError.clear();
        if (!backend->requeue(captured, &backendError)) {
            emit cameraError(backendError);
            running_ = false;
        }
        ++frameIndex;
    }

    backend->close();
    emit cameraStatus(QStringLiteral("camera stopped"));
#endif
}
