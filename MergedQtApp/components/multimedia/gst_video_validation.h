/**
 * @file gst_video_validation.h
 * @brief 向 Qt Web 上传提供与原播放器共用 GStreamer 环境的视频内容检查接口。
 * @author Du lin
 * @date 2026-10-09
 */
#ifndef GST_VIDEO_VALIDATION_H
#define GST_VIDEO_VALIDATION_H

#include <QString>

/** @brief 区分文件无效与板端 GStreamer 组件缺失，供上传接口映射状态码。 */
enum class GstVideoValidationStatus {
    Valid,
    Invalid,
    Unavailable
};

/** @brief 本地视频内容检查结果；不修改原播放器的任何状态。 */
struct GstVideoValidationResult {
    GstVideoValidationStatus status; /**< 内容有效、无效或检测环境不可用。 */
    QString message; /**< 可直接展示给上传用户的简短原因。 */
};

/**
 * @brief 在 Web 工作线程探测完整的本地文件，要求容器可解析且包含视频流。
 * @param path 已接收完整、仍未发布的受控本地文件绝对路径。
 * @param timeoutSeconds 单文件探测上限，限制异常媒体阻塞 Web 工作线程。
 * @return 验证结果；仅检查内容，不播放、转码或改变既有 GstPlayerWidget。
 */
GstVideoValidationResult validateVideoWithGStreamer(const QString &path, int timeoutSeconds = 15);

#endif // GST_VIDEO_VALIDATION_H
