/**
 * @file RgaImageProcessor.h
 * @brief 封装 librga 图像缓冲区及拷贝、缩放和像素格式转换。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef RGA_IMAGE_PROCESSOR_H
#define RGA_IMAGE_PROCESSOR_H

#include <QString>

namespace RgaImageProcessor {

/** @brief RGA 支持的标准化像素格式。 */
enum class PixelFormat {
    Nv12,
    Nv21,
    Yuyv422,
    Rgb888
};

/** @brief 可由 DMA 文件描述符或虚拟地址描述的 RGA 图像缓冲区。 */
struct Buffer {
    int dmaFd = -1;                  /**< DMA-BUF 文件描述符；-1 表示不使用。 */
    void *virtualAddress = nullptr;  /**< 虚拟地址；DMA 模式下可为空。 */
    int width = 0;                   /**< 有效图像宽度，单位 px。 */
    int height = 0;                  /**< 有效图像高度，单位 px。 */
    int widthStride = 0;             /**< 内存行跨度对应的像素宽度。 */
    int heightStride = 0;            /**< 分配缓冲区高度。 */
    PixelFormat format = PixelFormat::Rgb888; /**< 像素格式。 */

    /** @return 使用 DMA-BUF 描述的 Buffer。 */
    static Buffer fromDmaFd(int fd,
                            int width,
                            int height,
                            int widthStride,
                            int heightStride,
                            PixelFormat format);

    /** @return 使用进程虚拟地址描述的 Buffer。 */
    static Buffer fromVirtualAddress(void *address,
                                     int width,
                                     int height,
                                     int widthStride,
                                     int heightStride,
                                     PixelFormat format);
};

/**
 * @brief 同步执行 RGA 拷贝、缩放或格式转换。
 *
 * 源和目标格式相同时执行拷贝/缩放；不同格式走支持的颜色转换路径。
 */
bool process(const Buffer &source, const Buffer &destination, QString *error = nullptr);

} // namespace RgaImageProcessor

#endif
