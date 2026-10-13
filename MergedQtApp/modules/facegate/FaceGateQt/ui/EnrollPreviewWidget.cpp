/**
 * @file EnrollPreviewWidget.cpp
 * @brief 人脸录入页面的等比预览和检测框绘制控件的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "EnrollPreviewWidget.h"

#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOption>
#include <QtMath>

/** @brief 创建空录入预览控件。 */
EnrollPreviewWidget::EnrollPreviewWidget(QWidget *parent)
    : OpenGlImageWidget(parent)
{
    setMinimumSize(220, 170);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setObjectName(QStringLiteral("enrollPreview"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

/** @return 适合管理面板布局的推荐尺寸。 */
QSize EnrollPreviewWidget::sizeHint() const
{
    return QSize(512, 384);
}

/** @brief 更新预览图、原始坐标尺寸和人脸检测结果。 */
void EnrollPreviewWidget::setFrame(const QImage &previewImage,
                                   const QSize &sourceSize,
                                   const QVector<DetectedFace> &faces)
{
    previewImage_ = previewImage;
    if (!previewImage_.isNull()) {
        cameraUnavailableMessage_.clear();
    }
    setOpenGlImage(previewImage_);
    sourceSize_ = sourceSize;
    faces_ = faces;
    update();
}

/** @brief 清除预览和检测框。 */
void EnrollPreviewWidget::clearFrame()
{
    previewImage_ = QImage();
    cameraUnavailableMessage_.clear();
    clearOpenGlImage();
    sourceSize_ = QSize();
    faces_.clear();
    update();
}

/** @brief 清除旧画面并在录入预览中央显示摄像头异常提示。 */
void EnrollPreviewWidget::setCameraUnavailableMessage(const QString &message)
{
    clearFrame();
    cameraUnavailableMessage_ = message;
    update();
}

/** @brief 用 OpenGL 纹理绘制等比图像，并叠加检测框。 */
void EnrollPreviewWidget::paintGL()
{
    clearOpenGlSurface(palette().color(QPalette::Window));
    {
        QPainter backgroundPainter(this);
        QStyleOption option;
        option.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &backgroundPainter, this);

        if (previewImage_.isNull()) {
            backgroundPainter.setRenderHint(QPainter::TextAntialiasing, true);
            const bool cameraUnavailable = !cameraUnavailableMessage_.isEmpty();
            backgroundPainter.setPen(cameraUnavailable ? Qt::white : QColor(113, 133, 144));
            QFont messageFont = backgroundPainter.font();
            messageFont.setPointSize(cameraUnavailable ? 16 : 15);
            messageFont.setBold(cameraUnavailable);
            backgroundPainter.setFont(messageFont);
            backgroundPainter.drawText(
                rect().adjusted(24, 24, -24, -24),
                Qt::AlignCenter | Qt::TextWordWrap,
                cameraUnavailable ? cameraUnavailableMessage_
                                  : QStringLiteral("等待摄像头画面"));
            return;
        }
    }

    QSize drawSize = previewImage_.size();
    if (drawSize.width() > width() || drawSize.height() > height()) {
        drawSize.scale(size(), Qt::KeepAspectRatio);
    }
    const QRect imageRect(QPoint((width() - drawSize.width()) / 2,
                                 (height() - drawSize.height()) / 2),
                          drawSize);

    drawOpenGlImage(imageRect);

    if (!sourceSize_.isValid()) {
        return;
    }

    // 纹理绘制完成后使用新的 QPainter 会话，避免检测文字继承原生 GL 状态。
    QPainter painter(this);
    QPen pen(QColor(48, 230, 122));
    pen.setWidth(2);
    painter.setPen(pen);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const qreal scaleX = static_cast<qreal>(imageRect.width()) / sourceSize_.width();
    const qreal scaleY = static_cast<qreal>(imageRect.height()) / sourceSize_.height();
    for (const DetectedFace &face : faces_) {
        QRect mapped(imageRect.left() + qRound(face.rect.left() * scaleX),
                     imageRect.top() + qRound(face.rect.top() * scaleY),
                     qRound(face.rect.width() * scaleX),
                     qRound(face.rect.height() * scaleY));
        mapped = mapped.intersected(imageRect);
        if (mapped.isEmpty()) {
            continue;
        }

        painter.drawRect(mapped);
        QString label = QStringLiteral("检测 %1").arg(face.detConfidence, 0, 'f', 2);
        if (face.qualityValid) {
            label += QStringLiteral("  q %1").arg(face.quality, 0, 'f', 2);
        }
        const QPoint textPoint(mapped.left() + 4, qMax(imageRect.top() + 14, mapped.top() - 4));
        painter.drawText(textPoint, label);
    }
}
