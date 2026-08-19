/**
 * @file CameraPreviewWidget.cpp
 * @brief 门禁主界面的摄像头预览、状态和人脸框绘制控件的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "CameraPreviewWidget.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSizePolicy>

/** @brief 创建空预览并初始化静态背景缓存。 */
CameraPreviewWidget::CameraPreviewWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

/** @brief 更新当前摄像头图像。 */
void CameraPreviewWidget::setFrame(const QImage &image)
{
    frame_ = image;
    if (frame_.isNull()) {
        backgroundSourceFrame_ = QImage();
        staticBackground_ = QImage();
        staticBackgroundCanvasSize_ = QSize();
        update();
        return;
    }

    if (backgroundSourceFrame_.isNull()) {
        backgroundSourceFrame_ = frame_;
        rebuildStaticBackground();
        update();
        return;
    }

    update(videoTargetRect(frame_.size()));
}

/** @brief 清除动态图像和相关缓存。 */
void CameraPreviewWidget::clearFrame()
{
    frame_ = QImage();
    backgroundSourceFrame_ = QImage();
    staticBackground_ = QImage();
    staticBackgroundCanvasSize_ = QSize();
    faces_.clear();
    update();
}

/** @return sourceSize 在可用视频区内保持比例的目标矩形。 */
QRect CameraPreviewWidget::videoTargetRect(const QSize &sourceSize) const
{
    if (sourceSize.isEmpty() || size().isEmpty()) {
        return QRect();
    }

    const int centerInset = qMin(qMax(sideOverlayWidth_, 0), width() / 3);
    QRect clearArea = rect().adjusted(centerInset, 0, -centerInset, 0);
    if (clearArea.width() < 80) {
        clearArea = rect();
    }

    const QSize scaled = sourceSize.scaled(clearArea.size(), Qt::KeepAspectRatio);
    QRect target(
        QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2),
        scaled);
    target.moveCenter(clearArea.center());
    return target;
}

/** @return 人脸覆盖变化需要刷新的区域。 */
QRect CameraPreviewWidget::faceOverlayUpdateRect() const
{
    if (frame_.isNull()) {
        return QRect();
    }
    return videoTargetRect(frame_.size()).adjusted(-140, -34, 140, 4).intersected(rect());
}

/** @return 状态文本面板的绘制区域。 */
QRect CameraPreviewWidget::statusPanelRect() const
{
    const int panelWidth = qMin(560, qMax(260, width() - 420));
    return QRect((width() - panelWidth) / 2, height() - 82, panelWidth, 52);
}

/** @brief 基于最近背景源帧和当前画布重建模糊/填充背景。 */
void CameraPreviewWidget::rebuildStaticBackground()
{
    if (backgroundSourceFrame_.isNull() || size().isEmpty()) {
        staticBackground_ = QImage();
        staticBackgroundCanvasSize_ = size();
        return;
    }

    staticBackground_ = QImage(size(), QImage::Format_RGB32);
    if (staticBackground_.isNull()) {
        staticBackgroundCanvasSize_ = QSize();
        return;
    }
    staticBackground_.fill(Qt::black);

    QPainter backgroundPainter(&staticBackground_);
    const QSize coverSize =
        backgroundSourceFrame_.size().scaled(size(), Qt::KeepAspectRatioByExpanding);
    const QRect coverTarget(
        (width() - coverSize.width()) / 2,
        (height() - coverSize.height()) / 2,
        coverSize.width(),
        coverSize.height());
    const QSize blurSmallSize(qMax(24, width() / 18), qMax(24, height() / 18));
    const QImage blurred =
        backgroundSourceFrame_
            .scaled(blurSmallSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
            .scaled(coverTarget.size(),
                    Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation);
    backgroundPainter.drawImage(coverTarget, blurred);
    backgroundPainter.fillRect(staticBackground_.rect(), QColor(0, 0, 0, 88));

    const QRect target = videoTargetRect(backgroundSourceFrame_.size());
    if (!target.isEmpty()) {
        const int feather = qMin(72, qMax(24, target.width() / 9));
        QLinearGradient leftFade(target.left() - feather, 0, target.left(), 0);
        leftFade.setColorAt(0.0, QColor(0, 0, 0, 0));
        leftFade.setColorAt(1.0, QColor(0, 0, 0, 86));
        backgroundPainter.fillRect(
            QRect(target.left() - feather, 0, feather, height()), leftFade);

        QLinearGradient rightFade(target.right() + 1, 0, target.right() + 1 + feather, 0);
        rightFade.setColorAt(0.0, QColor(0, 0, 0, 86));
        rightFade.setColorAt(1.0, QColor(0, 0, 0, 0));
        backgroundPainter.fillRect(
            QRect(target.right() + 1, 0, feather, height()), rightFade);
    }

    staticBackgroundCanvasSize_ = size();
}

/**
 * @brief 更新识别链路检测到的人脸框。
 *
 * 人脸框使用源图坐标，并在 paintEvent 中缩放，
 * 使叠加层与摄像头画面的等比居中预览区域保持一致。
 */
void CameraPreviewWidget::setFaces(const QVector<DetectedFace> &faces)
{
    faces_ = faces;
    const QRect dirty = faceOverlayUpdateRect();
    if (dirty.isEmpty()) {
        update();
    } else {
        update(dirty);
    }
}

/** @brief 更新状态面板文本。 */
void CameraPreviewWidget::setStateText(const QString &text)
{
    stateText_ = text;
    update(statusPanelRect().adjusted(-2, -2, 2, 2));
}

/** @brief 更新状态机阶段及对应视觉主题。 */
void CameraPreviewWidget::setState(VerifyState state)
{
    state_ = state;
    update(statusPanelRect().adjusted(-2, -2, 2, 2));
}

/** @brief 设置左右信息栏占用宽度，避免视频被覆盖。 */
void CameraPreviewWidget::setSideOverlayWidth(int width)
{
    sideOverlayWidth_ = qMax(0, width);
    rebuildStaticBackground();
    update();
}

/** @brief 绘制静态背景、等比视频、人脸框和状态面板。 */
void CameraPreviewWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!backgroundSourceFrame_.isNull() &&
        (staticBackground_.isNull() || staticBackgroundCanvasSize_ != size())) {
        rebuildStaticBackground();
    }
    if (!staticBackground_.isNull()) {
        p.drawImage(QPoint(0, 0), staticBackground_);
    } else {
        p.fillRect(rect(), QColor(0, 0, 0));
    }

    QRect target;
    if (!frame_.isNull()) {
        /*
         * 先绘制一份全窗口模糊副本。侧边信息面板位于
         * 该层之上，而清晰摄像头画面限制在
         * 中央观看区域内。
         */
        target = videoTargetRect(frame_.size());
        p.drawImage(target, frame_);

        const qreal sx = static_cast<qreal>(target.width()) / static_cast<qreal>(frame_.width());
        const qreal sy = static_cast<qreal>(target.height()) / static_cast<qreal>(frame_.height());
        for (const DetectedFace &face : faces_) {
            QRect box(
                target.left() + qRound(face.rect.left() * sx),
                target.top() + qRound(face.rect.top() * sy),
                qRound(face.rect.width() * sx),
                qRound(face.rect.height() * sy)
            );

            QColor boxColor = face.matched ? QColor(40, 210, 120) : QColor(245, 175, 45);
            if (face.overlayStatus == QStringLiteral("passed")) {
                boxColor = QColor(40, 210, 120);
            } else if (face.overlayStatus == QStringLiteral("failed") ||
                       face.overlayStatus == QStringLiteral("liveness_failed")) {
                boxColor = QColor(230, 80, 70);
            } else if (face.overlayStatus == QStringLiteral("stranger")) {
                boxColor = QColor(245, 175, 45);
            } else if (face.qualityValid && face.quality < 0.35f) {
                boxColor = QColor(230, 80, 70);
            }

            QPen pen(boxColor, 3);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(box);

            QString label = face.overlayText;
            if (label.isEmpty()) {
                label = face.matched ? face.person.name : QStringLiteral("检测中");
            }
            if (label.isEmpty()) {
                label = face.matched ? face.person.personNo : QStringLiteral("检测中");
            }
            const QRect labelRect(box.left(), qMax(target.top(), box.top() - 30), qMax(120, box.width()), 26);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 165));
            p.drawRoundedRect(labelRect, 5, 5);
            p.setPen(boxColor);
            QFont labelFont = p.font();
            labelFont.setPointSize(12);
            labelFont.setBold(true);
            p.setFont(labelFont);
            p.drawText(labelRect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        }
    }

    QColor accent(70, 170, 255);
    if (state_ == VerifyState::Passed) accent = QColor(40, 190, 110);
    if (state_ == VerifyState::Failed) accent = QColor(220, 70, 70);
    if (state_ == VerifyState::LivenessChecking || state_ == VerifyState::CollectingFrames) accent = QColor(240, 170, 50);

    const QRect panel = statusPanelRect();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawRoundedRect(panel, 8, 8);
    p.setBrush(accent);
    p.drawRoundedRect(QRect(panel.left() + 14, panel.top() + 14, 10, 26), 5, 5);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPointSize(18);
    f.setBold(true);
    p.setFont(f);
    p.drawText(panel.adjusted(38, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft, stateText_);
}

/** @brief 转换鼠标按下为 clicked() 信号。 */
void CameraPreviewWidget::mousePressEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    emit clicked();
}

/** @brief 尺寸变化时重建静态背景缓存。 */
void CameraPreviewWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rebuildStaticBackground();
}
