/**
 * @file CameraService.cpp
 * @brief 摄像头采集线程服务的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-05；修改人：Du lin；按独立消费者需求复用RGB帧，截图不消费识别邮箱；正常启停异步协调，原后端采集/识别处理保持。
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
#include <QDateTime>
#include <QTimer>

#include <memory>
#include <utility>

namespace {

/** @brief 按消费者需求生成自有RGB，以及可选识别YUV/录入预览；不保留驱动借用内存。
 * @param[in] sourceImage 当前出队帧，调用期间有效，完成后可重新入队。
 * @param[in] outputFormat 后端标准化NV12/NV21格式。
 * @param[in] frameIndex 当前采集序号。
 * @param[in] recognition true沿用原三路转换，false仅生成截图RGB。
 * @param[out] frame 成功时替换完整帧；失败保持原值。
 * @param[out] error 可为空；失败时写入参数、分配或RGA错误。
 * @return true转换成功；false输入无效、分配失败或RGA失败。
 */
bool buildRgaFrame(const RgaImageProcessor::Buffer &sourceImage,
                   RgaImageProcessor::PixelFormat outputFormat,
                   int frameIndex,
                   bool recognition,
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
    if (recognition) {
        result.yuv420sp.resize(width * height * 3 / 2);
        const Buffer yuvImage = Buffer::fromVirtualAddress(
            result.yuv420sp.data(), width, height, width, height, outputFormat);

        // This normalized YUV copy remains valid after the V4L2 buffer is returned.
        if (!RgaImageProcessor::process(sourceImage, yuvImage, error)) {
            return false;
        }
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

    if (recognition) {
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

/** @brief 按完整配置启动线程；硬件打开错误由采集线程异步报告。 */
bool CameraService::start(const CameraProfile &profile)
{
    if (running_) {
        return true;
    }

    // 采集线程可能因设备不存在或初始化失败而自行退出。此时 running_ 已经
    // 变为 false，但 std::thread 在 join() 前仍然是 joinable 的，直接向
    // captureThread_ 赋予新线程会触发 std::terminate()。
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    desiredRunning_ = true;
    profile_ = profile;
    profile_.pixelFormat = profile_.pixelFormat.trimmed().toLower();
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_ = CameraFrame();
        frameDeliveryPending_ = false;
    }
    running_ = true;
    captureExited_ = false;
    captureThread_ = std::thread(&CameraService::captureLoop, this);
    return true;
}

/** @brief 请求采集线程停止并等待退出。 */
void CameraService::stop()
{
    desiredRunning_ = false;
    setScreenshotEnabled(false);
    running_ = false;
    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrame_ = CameraFrame();
    frameDeliveryPending_ = false;
}

/** @brief 仅设置需求，已退出线程才在对象线程回收，不等待 V4L2/MPP 释放。 */
void CameraService::requestRunning(const CameraProfile &profile,bool wanted,bool recognition)
{
    desiredProfile_=profile; desiredRunning_=wanted; recognitionEnabled_=recognition;
    reconcileCapture();
}

/** @brief 以唯一异步检查串行化停止与再次启动，避免快速交叠时 join 正在退出的线程。 */
void CameraService::reconcileCapture()
{
    if(!desiredRunning_) { running_=false; return; }
    if(running_)return;
    if(captureThread_.joinable() && !captureExited_) {
        if(!reconcilePending_) {
            reconcilePending_=true;
            QTimer::singleShot(10,this,[this]{reconcilePending_=false;reconcileCapture();});
        }
        return;
    }
    start(desiredProfile_);
}

/** @brief 仅切换截图消费者，清空旧截图/失败结果不改变原识别邮箱。 */
void CameraService::setScreenshotEnabled(bool enabled)
{
    if(screenshotEnabled_.exchange(enabled)==enabled)return;
    screenshotRequested_=false;
    std::lock_guard<std::mutex> lock(screenshotMutex_);
    screenshotImage_=QImage(); screenshotTs_=0;
}

/** @brief try_lock 领取自有截图；无帧时请求采集线程在后续有效帧准备一次。 */
bool CameraService::takeScreenshot(QImage *image,qint64 *ts)
{
    if(!image || !ts || !screenshotEnabled_)return false;
    std::unique_lock<std::mutex> lock(screenshotMutex_,std::try_to_lock);
    if(!lock.owns_lock())return false;
    if(screenshotImage_.isNull() && screenshotTs_==0) { screenshotRequested_=true; return false; }
    *image=std::move(screenshotImage_); screenshotImage_=QImage();
    *ts=screenshotTs_; screenshotTs_=0; return true;
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
    /** @brief 在后端和局部资源释放后发布退出完成；所有错误/非Linux返回路径均覆盖。 */
    struct ExitGuard {
        std::atomic_bool &exited; /**< 借用服务成员，服务析构在 join 后才释放。 */
        /** @brief 后端已释放后标记退出完成，不访问UI或执行设备操作。 */
        ~ExitGuard() { exited=true; }
    } exitGuard{captureExited_};
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

        const bool wantScreenshot=screenshotEnabled_ && screenshotRequested_.exchange(false);
        const qint64 capturedTs=wantScreenshot?QDateTime::currentMSecsSinceEpoch():0;
        bool shouldConvert = false;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            if (recognitionEnabled_ && !frameDeliveryPending_) {
                frameDeliveryPending_ = true;
                shouldConvert = true;
            }
        }

        if (shouldConvert || wantScreenshot) {
            CameraFrame frame;
            QString rgaError;
            if (buildRgaFrame(captured.source,
                              backend->normalizedOutputFormat(),
                              frameIndex,
                              shouldConvert,
                              &frame,
                              &rgaError)) {
                if(wantScreenshot && screenshotEnabled_) {
                    std::unique_lock<std::mutex> lock(screenshotMutex_,std::try_to_lock);
                    if(lock.owns_lock() && screenshotEnabled_) {
                        screenshotImage_=frame.image; screenshotTs_=capturedTs;
                    } else if(screenshotEnabled_) screenshotRequested_=true;
                }
                if(shouldConvert) {
                    std::lock_guard<std::mutex> lock(frameMutex_);
                    latestFrame_ = std::move(frame);
                }
                if(shouldConvert) emit frameAvailable();
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
            } else if(shouldConvert) {
                // 原识别转换失败仍走原故障路径；截图自身的失败不能停掉共享识别相机。
                {
                    std::lock_guard<std::mutex> lock(frameMutex_);
                    frameDeliveryPending_ = false;
                }
                emit cameraError(rgaError);
                running_ = false;
            } else if(wantScreenshot && screenshotEnabled_) {
                std::unique_lock<std::mutex> lock(screenshotMutex_,std::try_to_lock);
                if(lock.owns_lock() && screenshotEnabled_) {
                    screenshotImage_=QImage();screenshotTs_=capturedTs;
                }
            }
        } else if(recognitionEnabled_) {
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
