/**
 * @file CameraProfile.h
 * @brief 摄像头硬件来源。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef CAMERA_PROFILE_H
#define CAMERA_PROFILE_H

#include <QString>

/** @brief 摄像头硬件来源。 */
enum class CameraSourceType {
    Mipi,
    Usb
};

/** @brief USB UVC 摄像头控制参数。 */
struct UsbCameraControlConfig {
    bool autoWhiteBalance = true;       /**< 是否由摄像头自动调节白平衡。 */
    int whiteBalanceTemperature = 4600; /**< 手动白平衡色温，单位 K。 */
    bool autoExposure = true;           /**< 是否使用自动曝光。 */
    int exposureAbsolute = 166;         /**< UVC 曝光绝对值，单位由设备驱动定义。 */
    bool exposureAutoPriority = false;  /**< 是否允许自动曝光优先调整帧率。 */
    int powerLineFrequency = 1;         /**< UVC 电源频率枚举值，用于抑制灯光闪烁。 */
};

/** @brief 启动一次摄像头采集所需的完整参数。 */
struct CameraProfile {
    CameraSourceType source = CameraSourceType::Mipi; /**< MIPI 或 USB 后端。 */
    QString devicePath = QStringLiteral("/dev/video0"); /**< V4L2 设备节点。 */
    int width = 640;                         /**< 请求采集宽度，单位 px。 */
    int height = 480;                        /**< 请求采集高度，单位 px。 */
    int fps = 0;                             /**< 请求帧率；0 表示沿用后端默认值。 */
    QString pixelFormat = QStringLiteral("nv12"); /**< 请求的 V4L2 像素格式名。 */
    UsbCameraControlConfig usbControls;      /**< 仅 USB 来源使用的 UVC 控制项。 */
};

#endif
