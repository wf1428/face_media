/**
 * @file UsbMjpegCaptureBackend.h
 * @brief 创建使用 Rockchip MPP 解码 MJPEG 的 USB 摄像头采集后端。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef USB_MJPEG_CAPTURE_BACKEND_H
#define USB_MJPEG_CAPTURE_BACKEND_H

#include "CameraCaptureBackend.h"

#include <memory>

/**
 * @return 新建的 USB MJPEG 采集后端。
 *
 * 后端通过 GStreamer v4l2src 获取 MJPEG，使用 mppjpegdec 硬件解码为
 * NV12，再按 ICameraCaptureBackend 的统一接口交给现有 CameraService。
 */
std::unique_ptr<ICameraCaptureBackend> createUsbMjpegCaptureBackend();

#endif
