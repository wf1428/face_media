/**
 * @file CameraCaptureBackend.h
 * @brief 实现 V4L2、USB 和平台摄像头采集后端及兼容回退。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef CAMERA_CAPTURE_BACKEND_H
#define CAMERA_CAPTURE_BACKEND_H

#include "CameraProfile.h"
#include "RgaImageProcessor.h"

#include <QSize>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <memory>

/** @brief 等待摄像头帧的三态结果。 */
enum class CameraWaitResult {
    FrameReady,
    Timeout,
    Error
};

/** @brief 从后端出队、等待处理后必须重新入队的摄像头缓冲区。 */
struct CapturedCameraBuffer {
    RgaImageProcessor::Buffer source; /**< 可供 RGA 读取的源图像描述。 */
    int index = -1;                   /**< V4L2 缓冲区索引；-1 表示无效。 */
    std::size_t bytesUsed = 0;        /**< 驱动报告的有效数据字节数。 */
};

/** @brief MIPI/USB 摄像头采集后端统一接口。 */
class ICameraCaptureBackend {
public:
    /** @brief 允许通过接口指针释放具体采集后端。 */
    virtual ~ICameraCaptureBackend() = default;

    /** @brief 按 profile 打开并初始化流式采集。 */
    virtual bool open(const CameraProfile &profile, QString *error) = 0;

    /** @brief 等待一帧就绪、超时或错误。 */
    virtual CameraWaitResult waitForFrame(int timeoutMs, QString *error) = 0;

    /** @brief 将一个已就绪缓冲区从驱动队列取出。 */
    virtual bool dequeue(CapturedCameraBuffer *buffer, QString *error) = 0;

    /** @brief 将处理完成的缓冲区归还驱动队列。 */
    virtual bool requeue(const CapturedCameraBuffer &buffer, QString *error) = 0;

    /** @brief 停止流并释放所有 V4L2/RGA 资源。 */
    virtual void close() = 0;

    /** @return 后端协商后的实际采集尺寸。 */
    virtual QSize captureSize() const = 0;

    /** @return 后端提供给后续处理的标准化像素格式。 */
    virtual RgaImageProcessor::PixelFormat normalizedOutputFormat() const = 0;

    /** @return 实际输入像素格式的诊断名称。 */
    virtual QString inputFormatName() const = 0;

    /** @return MMAP、DMABUF 等输入内存模式名称。 */
    virtual QString inputMemoryName() const = 0;

    /** @return 后端初始化后的状态摘要。 */
    virtual QString statusMessage() const = 0;

    /** @return 非致命兼容或回退警告列表。 */
    virtual QStringList warnings() const = 0;
};

/** @return 与摄像头来源匹配的新采集后端实例。 */
std::unique_ptr<ICameraCaptureBackend>
createCameraCaptureBackend(CameraSourceType source);

#endif
