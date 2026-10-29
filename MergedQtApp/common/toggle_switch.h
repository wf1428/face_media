/**
 * @file toggle_switch.h
 * @brief 复用工程管理员页面的动画滑动开关，供 QWidget 页面保持一致外观。
 * @author Du lin
 * @date 2026-10-27
 */
#pragma once
#include <QCheckBox>
#include <QPainter>
#include <QVariantAnimation>
#include <QEasingCurve>

/**
 * @brief 工程共用的圆角轨道、白色滑块动画开关，勾选时滑块位于右侧。
 * @note 仅在 GUI 线程使用；原生点击/空格切换语义保持 QCheckBox 行为。
 */
class ToggleSwitch : public QCheckBox {
public:

    /** @brief 初始化 190ms OutCubic 动画和 66×38 逻辑像素轨道。
     *  @param parent Qt 父控件，按父子关系管理控件生命周期；动画由成员对象持有。 */
    explicit ToggleSwitch(QWidget *parent = nullptr) : QCheckBox(parent), animation_(this)
    {
        setText(QString());
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFixedSize(66, 38);

        animation_.setDuration(190);
        animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            position_ = value.toReal();
            update();
        });
        connect(this, &QCheckBox::toggled, this, [this](bool checked) {
            animation_.stop();
            animation_.setStartValue(position_);
            animation_.setEndValue(checked ? 1.0 : 0.0);
            animation_.start();
        });
    }

protected:

    /** @brief 整个轨道均可响应点击。
     *  @param position 控件局部坐标，单位为逻辑像素。
     *  @return 坐标位于控件矩形内时为 true。 */
    bool hitButton(const QPoint &position) const override
    {
        return rect().contains(position);
    }

    /** @brief 绘制插值后的轨道和滑块；非动画回显直接采用 checked 对应端点。 */
    void paintEvent(QPaintEvent *) override
    {
        if (animation_.state() != QAbstractAnimation::Running) {
            position_ = isChecked() ? 1.0 : 0.0;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF trackRect(1.0, 2.0, width() - 2.0, height() - 4.0);
        painter.setPen(Qt::NoPen);
        const QColor offColor = isEnabled() ? QColor("#566477") : QColor("#3f4855");
        const QColor onColor = isEnabled() ? QColor("#36d4c7") : QColor("#4a6770");
        const auto blend = [this](int off, int on) {
            return qRound(off + (on - off) * position_);
        };
        painter.setBrush(QColor(blend(offColor.red(), onColor.red()),
                                blend(offColor.green(), onColor.green()),
                                blend(offColor.blue(), onColor.blue())));
        painter.drawRoundedRect(trackRect, trackRect.height() / 2.0, trackRect.height() / 2.0);

        const qreal knobDiameter = trackRect.height() - 6.0;
        const qreal knobStart = trackRect.left() + 3.0;
        const qreal knobEnd = trackRect.right() - knobDiameter - 3.0;
        const qreal knobX = knobStart + (knobEnd - knobStart) * position_;
        const QRectF knobRect(knobX, trackRect.top() + 3.0, knobDiameter, knobDiameter);
        painter.setBrush(QColor("#ffffff"));
        painter.drawEllipse(knobRect);
    }

private:

    QVariantAnimation animation_; /**< GUI 线程的滑块/颜色插值动画，持续 190ms。 */
    qreal position_ = 0.0;        /**< 归一化滑块位置；0 为左侧，1 为右侧，由动画更新。 */
};
