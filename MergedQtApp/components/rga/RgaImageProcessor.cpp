/**
 * @file RgaImageProcessor.cpp
 * @brief 封装 librga 图像缓冲区及拷贝、缩放和像素格式转换。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "RgaImageProcessor.h"

#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <rga/im2d.hpp>
#include <rga/rga.h>
#endif

namespace RgaImageProcessor {

/** @return 使用 DMA-BUF 描述的 Buffer。 */
Buffer Buffer::fromDmaFd(int fd,
                         int width,
                         int height,
                         int widthStride,
                         int heightStride,
                         PixelFormat format)
{
    Buffer buffer;
    buffer.dmaFd = fd;
    buffer.width = width;
    buffer.height = height;
    buffer.widthStride = widthStride;
    buffer.heightStride = heightStride;
    buffer.format = format;
    return buffer;
}

/** @return 使用进程虚拟地址描述的 Buffer。 */
Buffer Buffer::fromVirtualAddress(void *address,
                                  int width,
                                  int height,
                                  int widthStride,
                                  int heightStride,
                                  PixelFormat format)
{
    Buffer buffer;
    buffer.virtualAddress = address;
    buffer.width = width;
    buffer.height = height;
    buffer.widthStride = widthStride;
    buffer.heightStride = heightStride;
    buffer.format = format;
    return buffer;
}

#ifdef Q_OS_LINUX
namespace {

/** @brief 将项目像素格式映射为 librga 格式常量。 */
int toRgaFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::Nv12:
        return RK_FORMAT_YCbCr_420_SP;
    case PixelFormat::Nv21:
        return RK_FORMAT_YCrCb_420_SP;
    case PixelFormat::Yuyv422:
        return RK_FORMAT_YUYV_422;
    case PixelFormat::Rgb888:
        return RK_FORMAT_RGB_888;
    }
    return -1;
}

/** @brief 校验 RGA 缓冲区尺寸、步长和地址描述是否可用。 */
bool isValid(const Buffer &buffer)
{
    return (buffer.dmaFd >= 0 || buffer.virtualAddress) &&
           buffer.width > 0 &&
           buffer.height > 0 &&
           buffer.widthStride >= buffer.width &&
           buffer.heightStride >= buffer.height;
}

/** @brief 根据 DMA 文件描述符或虚拟地址创建 librga 缓冲区包装对象。 */
rga_buffer_t wrap(const Buffer &buffer, int rgaFormat)
{
    if (buffer.dmaFd >= 0) {
        return wrapbuffer_fd_t(buffer.dmaFd,
                               buffer.width,
                               buffer.height,
                               buffer.widthStride,
                               buffer.heightStride,
                               rgaFormat);
    }
    return wrapbuffer_virtualaddr_t(buffer.virtualAddress,
                                    buffer.width,
                                    buffer.height,
                                    buffer.widthStride,
                                    buffer.heightStride,
                                    rgaFormat);
}

/** @brief 仅在调用方提供错误输出指针时写入 RGA 失败原因。 */
void setError(QString *error, const QString &operation, IM_STATUS status)
{
    if (error) {
        *error = QStringLiteral("RGA %1 failed: %2")
                     .arg(operation, QString::fromLocal8Bit(imStrError(status)));
    }
}

} // namespace
#endif

/**
 * @brief 同步执行 RGA 拷贝、缩放或格式转换。
 *
 * 源和目标格式相同时执行拷贝/缩放；不同格式走支持的颜色转换路径。
 */
bool process(const Buffer &source, const Buffer &destination, QString *error)
{
#ifndef Q_OS_LINUX
    Q_UNUSED(source)
    Q_UNUSED(destination)
    if (error) {
        *error = QStringLiteral("RGA image processing requires Linux");
    }
    return false;
#else
    if (!isValid(source) || !isValid(destination)) {
        if (error) {
            *error = QStringLiteral("RGA buffer parameters are invalid");
        }
        return false;
    }

    const int sourceFormat = toRgaFormat(source.format);
    const int destinationFormat = toRgaFormat(destination.format);
    if (sourceFormat < 0 || destinationFormat < 0) {
        if (error) {
            *error = QStringLiteral("RGA pixel format is unsupported");
        }
        return false;
    }

    const rga_buffer_t sourceImage = wrap(source, sourceFormat);
    const rga_buffer_t destinationImage = wrap(destination, destinationFormat);
    const im_rect emptyRect = {};
    IM_STATUS status = imcheck(sourceImage, destinationImage, emptyRect, emptyRect);
    if (status != IM_STATUS_NOERROR) {
        setError(error, QStringLiteral("imcheck"), status);
        return false;
    }

    if (source.format == destination.format) {
        status = imresize(sourceImage, destinationImage);
        if (status != IM_STATUS_SUCCESS) {
            setError(error, QStringLiteral("copy/resize"), status);
            return false;
        }
        return true;
    }

    const bool sourceIsYuv =
        source.format == PixelFormat::Nv12 ||
        source.format == PixelFormat::Nv21 ||
        source.format == PixelFormat::Yuyv422;
    const bool destinationIsRgb =
        destination.format == PixelFormat::Rgb888;
    const bool yuyvToYuv420 =
        source.format == PixelFormat::Yuyv422 &&
        (destination.format == PixelFormat::Nv12 ||
         destination.format == PixelFormat::Nv21);
    if (!(sourceIsYuv && destinationIsRgb) && !yuyvToYuv420) {
        if (error) {
            *error = QStringLiteral("RGA conversion path is unsupported");
        }
        return false;
    }

    status = imcvtcolor(sourceImage,
                        destinationImage,
                        sourceFormat,
                        destinationFormat,
                        destinationIsRgb ? IM_YUV_TO_RGB_BT601_FULL : 0,
                        1);
    if (status != IM_STATUS_SUCCESS) {
        setError(error, QStringLiteral("color conversion"), status);
        return false;
    }
    return true;
#endif
}

} // namespace RgaImageProcessor
