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
#include <QVector>
#include <QWidget>

#include "VerificationTypes.h"

/** @brief 人脸录入页面的等比预览和检测框绘制控件。 */
class EnrollPreviewWidget : public QWidget {
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

protected:
    /** @brief 绘制等比图像，并将原始人脸坐标映射到预览区域。 */
    void paintEvent(QPaintEvent *event) override;

private:
    QImage previewImage_;            /**< 当前录入预览图。 */
    QSize sourceSize_;               /**< 人脸坐标所基于的原始图像尺寸。 */
    QVector<DetectedFace> faces_;    /**< 当前检测框和状态。 */
};

#endif
