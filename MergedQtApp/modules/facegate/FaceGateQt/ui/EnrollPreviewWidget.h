/**
 * @file EnrollPreviewWidget.h
 * @brief 人脸录入页面的等比预览和检测框绘制控件。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ENROLL_PREVIEW_WIDGET_H
#define ENROLL_PREVIEW_WIDGET_H

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include "OpenGlImageWidget.h"
#include "VerificationTypes.h"

/** @brief 人脸录入页面的等比预览和检测框绘制控件。 */
class EnrollPreviewWidget : public OpenGlImageWidget {
    Q_OBJECT

public:
    /** @brief 创建空录入预览控件。 */
    explicit EnrollPreviewWidget(QWidget *parent = nullptr);

    /** @return 适合管理面板布局的推荐尺寸。 */
    QSize sizeHint() const override;

    /** @brief 更新预览图、原始坐标尺寸和人脸检测结果。 */
    void setFrame(const QImage &previewImage,
                  const QSize &sourceSize,
                  const QVector<DetectedFace> &faces);

    /** @brief 清除预览和检测框。 */
    void clearFrame();

    /** @brief 清除旧画面并显示摄像头不可用提示。 */
    void setCameraUnavailableMessage(const QString &message);

protected:
    /** @brief 用 OpenGL 纹理绘制等比图像，并叠加检测框。 */
    void paintGL() override;

private:
    QImage previewImage_;            /**< 当前录入预览图。 */
    QSize sourceSize_;               /**< 人脸坐标所基于的原始图像尺寸。 */
    QVector<DetectedFace> faces_;    /**< 当前检测框和状态。 */
    QString cameraUnavailableMessage_; /**< 无画面时显示的摄像头异常提示。 */
};

#endif
