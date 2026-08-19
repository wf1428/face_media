/**
 * @file volumepopup.cpp
 * @brief 从右侧滑入的触摸音量调节面板的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "volumepopup.h"
#include "vol_ctrl.h"
#include <QSlider>
#include <QLabel>
#include <QVBoxLayout>
#include <QPropertyAnimation>
#include <QMouseEvent>
#include <QDebug>
#include "platform/rk3566_platform.h"

/** @brief 绑定音量控制器并创建滑块、标签和动画。 */
VolumePanel::VolumePanel(AmpVolumeController *ctrl, QWidget *parent)
    : QWidget(parent), m_ctrl(ctrl)
{
    // 固定作为右侧面板
    setAttribute(Qt::WA_StyledBackground, true);
    //setStyleSheet("background: rgba(0,0,0,180); border-left: 2px solid rgba(255,255,255,60);");

    m_label = new QLabel("音量", this);
    m_label->setStyleSheet("color:white; font-size:18px;");

    m_slider = new QSlider(Qt::Vertical, this);
    m_slider->setRange(0, 100);

    // 离散档位：每次 1 格，保证档位完整
    m_slider->setSingleStep(1);
    m_slider->setPageStep(1);
    m_slider->setTickInterval(1);
    // 显示刻度；样式里把 tick-mark 设透明了，所以视觉上仍是“看不见”
    // m_slider->setTickPosition(QSlider::TicksBothSides);

    // 让“上面更大、下面更小”（符合直觉）
    m_slider->setInvertedAppearance(true);  // 顶部=7，底部=0
    m_slider->setInvertedControls(true);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(10);
    lay->addWidget(m_label, 0, Qt::AlignHCenter);
    lay->addWidget(m_slider, 1, Qt::AlignHCenter);

    connect(m_slider, &QSlider::valueChanged, this, [this](int v){
        m_label->setText(QString("音量 %1").arg(v));
        if (m_ctrl) m_ctrl->setVolume(v); // 0关PA，>0开PA并设置
    });

    connect(m_slider, &QSlider::sliderReleased, this, [this](){
        emit manualVolumeChanged(m_slider->value(),QStringLiteral("Local manual volume adjustment：%1").arg(m_slider->value())); // 松开时发送最终值
    });

    setStyleSheet(R"(
    VolumePanel {
        background: rgba(22, 22, 22, 210);
        border: 1px solid rgba(255, 255, 255, 40);

        border-radius: 16px;
    }

    QLabel {
        color: rgba(255, 255, 255, 220);
        font-size: 18px;
        font-weight: 500;
    }

    QSlider::groove:vertical {
        width: 8px;
        border-radius: 4px;
        background: rgba(255, 255, 255, 22);
        margin: 10px 0;
    }

    QSlider::sub-page:vertical {
        border-radius: 4px;
        background: rgba(255, 255, 255, 185);
    }
    QSlider::add-page:vertical {
        border-radius: 4px;
        background: rgba(255, 255, 255, 28);
    }

    QSlider::handle:vertical {
        height: 18px;
        width: 18px;
        margin: -6px -5px;
        border-radius: 9px;
        background: rgba(255, 255, 255, 235);
        border: 1px solid rgba(0, 0, 0, 70);
    }

    QSlider::handle:vertical:hover {
        background: rgba(255, 255, 255, 255);
    }
    QSlider::handle:vertical:pressed {
        background: rgba(255, 255, 255, 255);
    }

    QSlider::tick-mark:vertical {
        background: transparent;
    }
    )");

    applyGeometry();

    // 初始隐藏在屏幕外（最右边），y 用面板的目标 y
    move(m_screenW, m_panelY);
    hide();
}

/** @brief 根据屏幕尺寸计算显示和隐藏几何位置。 */
void VolumePanel::applyGeometry()
{
    // 默认值
    m_screenW = 1024;
    m_screenH = 768;

    // 外部资源根目录
    const QString configPath = Rk3566Platform::uiConfigPath();
    qDebug() << "VolumePanel 开始读取磁盘配置文件: " << configPath;

    if (QFile::exists(configPath)) {
        QSettings settings(configPath, QSettings::IniFormat);

        bool okW = false;
        bool okH = false;

        settings.beginGroup("backRect");
        int w = settings.value("backPic_x_Size", 1024).toInt(&okW);
        int h = settings.value("backPic_y_Size", 768).toInt(&okH);

        if (okW && w > 0) {
            m_screenW = w;
        }
        if (okH && h > 0) {
            m_screenH = h;
        }

        qDebug() << "VolumePanel 读取配置成功: " << configPath
                  << ", backPic_x_Size=" << m_screenW
                  << ", backPic_y_Size=" << m_screenH;
    } else {
        qDebug() << "VolumePanel 配置文件不存在，使用默认分辨率: 1024x768";
    }

    // 面板w,h
    m_panelW = 120;
    m_panelH = m_screenH / 3;

    setFixedSize(m_panelW, m_panelH);

    // 垂直居中
    m_panelY = (m_screenH - m_panelH) / 2;

    // 贴底：m_panelY = m_screenH - m_panelH;
    // 贴顶：m_panelY = 0;
}

/** @brief 将外部音量同步到滑块和标签。 */
void VolumePanel::setCurrentVolume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    m_slider->blockSignals(true);
    m_slider->setValue(v);
    m_label->setText(QString("音量 %1").arg(v));
    m_slider->blockSignals(false);
}

/** @brief 从右侧屏幕外滑入。 */
void VolumePanel::showSlideIn()
{
    if (m_shown) return;
    m_shown = true;

    applyGeometry();
    show();
    raise();

    // 从 x=800（屏幕外）滑到 x=800-panelW，y 保持 m_panelY
    auto *anim = new QPropertyAnimation(this, "pos");
    anim->setDuration(180);
    anim->setStartValue(QPoint(m_screenW, m_panelY));
    anim->setEndValue(QPoint(m_screenW - width(), m_panelY));
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

/** @brief 滑出到右侧后隐藏。 */
void VolumePanel::hideSlideOut()
{
    if (!m_shown) return;
    m_shown = false;

    auto *anim = new QPropertyAnimation(this, "pos");
    anim->setDuration(160);
    anim->setStartValue(pos());
    anim->setEndValue(QPoint(m_screenW, m_panelY));
    anim->setEasingCurve(QEasingCurve::InCubic);
    connect(anim, &QPropertyAnimation::finished, this, [this](){
        hide();
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

/** @brief 消费面板内点击，避免事件穿透到底层播放器。 */
void VolumePanel::mousePressEvent(QMouseEvent *e)
{
    e->accept();
}
