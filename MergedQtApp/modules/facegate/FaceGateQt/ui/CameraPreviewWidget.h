/**
 * @file CameraPreviewWidget.h
 * @brief 门禁主界面的摄像头预览、状态和人脸框绘制控件。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef CAMERA_PREVIEW_WIDGET_H
#define CAMERA_PREVIEW_WIDGET_H

#include <QImage>

#include "OpenGlImageWidget.h"
#include "VerificationTypes.h"

class QResizeEvent;

/** @brief 门禁主界面的摄像头预览、状态和人脸框绘制控件。 */
class CameraPreviewWidget : public OpenGlImageWidget {
    Q_OBJECT

public:
    /** @brief 创建空预览并初始化静态背景缓存。 */
    explicit CameraPreviewWidget(QWidget *parent = nullptr);

    /** @brief 更新当前摄像头图像。 */
    void setFrame(const QImage &image);

    /** @brief 清除动态图像和相关缓存。 */
    void clearFrame();

    /** @brief 在无摄像头画面时显示居中的设备异常提示。 */
    void setCameraUnavailableMessage(const QString &message);

    /** @brief 更新需要叠加绘制的人脸列表。 */
    void setFaces(const QVector<DetectedFace> &faces);

    /** @brief 更新状态面板文本。 */
    void setStateText(const QString &text);

    /** @brief 更新状态机阶段及对应视觉主题。 */
    void setState(VerifyState state);

    /** @brief 设置左右信息栏占用宽度，避免视频被覆盖。 */
    void setSideOverlayWidth(int width);

signals:
    /** @brief 用户点击预览画面时发出，用于管理员入口计数。 */
    void clicked();

protected:
    /** @brief 用 OpenGL 纹理绘制视频，再叠加人脸框和状态面板。 */
    void paintGL() override;

    /** @brief 转换鼠标按下为 clicked() 信号。 */
    void mousePressEvent(QMouseEvent *event) override;

    /** @brief 尺寸变化时重建静态背景缓存。 */
    void resizeEvent(QResizeEvent *event) override;

private:
    /** @brief 基于最近背景源帧和当前画布重建模糊/填充背景。 */
    void rebuildStaticBackground();

    /** @return sourceSize 在可用视频区内保持比例的目标矩形。 */
    QRect videoTargetRect(const QSize &sourceSize) const;

    /** @return 人脸覆盖变化需要刷新的区域。 */
    QRect faceOverlayUpdateRect() const;

    /** @return 状态文本面板的绘制区域。 */
    QRect statusPanelRect() const;

    QImage frame_;                       /**< 当前前景视频帧。 */
    QImage backgroundSourceFrame_;       /**< 生成静态背景的源帧。 */
    QImage staticBackground_;            /**< 与画布尺寸匹配的缓存背景。 */
    QSize staticBackgroundCanvasSize_;   /**< staticBackground_ 对应的控件尺寸。 */
    QVector<DetectedFace> faces_;        /**< 当前人脸叠加数据。 */
    QString stateText_ = "等待中";       /**< 当前状态提示。 */
    QString cameraUnavailableMessage_;   /**< 无画面时居中显示的摄像头异常提示。 */
    VerifyState state_ = VerifyState::Idle; /**< 当前验证状态。 */
    int sideOverlayWidth_ = 0;           /**< 单侧信息覆盖区宽度，单位 px。 */
};

#endif
