/**
 * @file ImageUtils.h
 * @brief 将连续 YUV420SP 数据转换为 QImage::Format_RGB888。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IMAGE_UTILS_H
#define IMAGE_UTILS_H

#include <QImage>
#include <QVector>
#include <QtGlobal>

#include <cstring>

namespace ImageUtils {

/** @return 将整数饱和限制到 0~255 后得到的字节值。 */
inline uchar clampInt(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uchar>(v);
}

/**
 * @brief 将连续 YUV420SP 数据转换为 QImage::Format_RGB888。
 * @param yuv 输入缓冲区，至少 width*height*3/2 字节。
 * @param width 图像宽度，单位 px。
 * @param height 图像高度，单位 px。
 * @param nv21 true 表示 VU 排列，false 表示 NV12 的 UV 排列。
 * @return RGB 图像；参数无效时返回空图像。
 */
inline QImage yuv420spToRgb888(const uchar *yuv, int width, int height, bool nv21)
{
    if (!yuv || width <= 0 || height <= 0) {
        return QImage();
    }

    QImage image(width, height, QImage::Format_RGB888);
    const int frameSize = width * height;

    // 逐像素把 YUV420SP 转换为 RGB888，供 Qt 预览显示使用。
    for (int y = 0; y < height; ++y) {
        uchar *dst = image.scanLine(y);
        const int uvRow = (y >> 1) * width;

        for (int x = 0; x < width; ++x) {
            const int yy = static_cast<int>(yuv[y * width + x]) - 16;
            const int uvIndex = frameSize + uvRow + (x & ~1);
            const int first = yuv[uvIndex] - 128;
            const int second = yuv[uvIndex + 1] - 128;
            const int v = nv21 ? first : second;
            const int u = nv21 ? second : first;

            const int c = qMax(0, yy);
            const int r = (298 * c + 409 * v + 128) >> 8;
            const int g = (298 * c - 100 * u - 208 * v + 128) >> 8;
            const int b = (298 * c + 516 * u + 128) >> 8;

            dst[x * 3 + 0] = clampInt(r);
            dst[x * 3 + 1] = clampInt(g);
            dst[x * 3 + 2] = clampInt(b);
        }
    }

    return image;
}

/**
 * @brief 将任意 QImage 转为无行填充的连续 RGB888 字节数组。
 * @return 大小为 width*height*3 的数据。
 */
inline QByteArray imageToRgb888Bytes(const QImage &image)
{
    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    QByteArray data;
    data.resize(rgb.width() * rgb.height() * 3);

    // 逐行复制 RGB888 数据，避免 QImage 行跨度影响输出连续性。
    for (int y = 0; y < rgb.height(); ++y) {
        memcpy(data.data() + y * rgb.width() * 3, rgb.constScanLine(y), rgb.width() * 3);
    }

    return data;
}

}

#endif
