/**
 * @file appstyle.cpp
 * @brief 1024x768 设置页面的统一缩放、QSS 和滚动区域工厂的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "appstyle.h"

#include <QGuiApplication>
#include <QScreen>
#include <QtMath>
#include <QWidget>
#include <QScrollArea>
#include <QScrollBar>

static double g_scale = 1.0;

/** @return 相对设计分辨率计算的统一 UI 缩放系数。 */
double AppStyle::uiScaleForScreen()
{
    QScreen* s = QGuiApplication::primaryScreen();
    if (!s) return 1.0;

    const QRect g = s->availableGeometry();
    const double sx = double(g.width())  / 1024.0;
    const double sy = double(g.height()) / 768.0;

    double f = qMin(sx, sy);
    f = qBound(0.78, f, 1.20);
    return f;
}


/** @return 设计像素乘以当前屏幕缩放后的整数像素值。 */
int AppStyle::px(double v)
{
    return int(qRound(v * g_scale));
}

/** @return 设置页分组、输入、按钮、标签和滚动条的完整 QSS。 */
QString AppStyle::pageStyleSheet()
{
    // 统一参数
    const int gbRadius     = px(12);
    const int gbTitleSize  = px(15);
    const int gbMarginTop  = px(22);
    const int gbPadTop     = px(12);
    const int gbPadLR      = px(12);

    const int fieldMinH    = qMax(px(34), px(30));
    const int fieldFont    = px(14);
    const int fieldPadV    = px(6);
    const int fieldPadH    = px(10);

    const int plainFont    = px(13);
    const int plainMinH    = px(70);

    // GroupBox
    const QString groupStyle = QString(R"(
QGroupBox {
    border: 1px solid rgba(0,0,0,0.14);
    border-radius: %1px;
    background: rgba(0,0,0,0.02);
    margin-top: %2px;
    padding: %3px %4px %5px %4px;
    font-size: %6px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    top: %7px;
    left: %8px;
    padding: %9px %10px;
    border-radius: %11px;
    background-color: rgba(255,255,255,0.98);
    color: rgba(0,0,0,0.75);
}
)")
    .arg(gbRadius)
    .arg(gbMarginTop)
    .arg(gbPadTop).arg(gbPadLR).arg(px(10))
    .arg(gbTitleSize)
    .arg(px(6)).arg(px(12))
    .arg(px(2)).arg(px(8))
    .arg(px(10));

    // Fields + PlainText
    const QString fieldStyle = QString(R"(
QLineEdit, QSpinBox {
    background: rgba(255,255,255,0.98);
    border: 1px solid rgba(0,0,0,0.16);
    border-radius: %1px;
    min-height: %2px;
    padding: %3px %4px;
    font-size: %5px;
    color: rgba(0,0,0,0.86);
}
QLineEdit:hover, QSpinBox:hover { border: 1px solid rgba(0,0,0,0.28); }
QLineEdit:focus, QSpinBox:focus { border: 1px solid rgba(66,133,244,0.95); background: white; }
QLineEdit[readOnly="true"] { background: rgba(0,0,0,0.03); color: rgba(0,0,0,0.70); }

QSpinBox { padding-right: %6px; }
QSpinBox::up-button, QSpinBox::down-button {
    subcontrol-origin: border;
    width: %7px;
    border: none;
    background: transparent;
}
QSpinBox::up-button { subcontrol-position: top right; height: 50%; border-top-right-radius: %8px; }
QSpinBox::down-button { subcontrol-position: bottom right; height: 50%; border-bottom-right-radius: %8px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: rgba(0,0,0,0.05); }

QPlainTextEdit {
    background: rgba(255,255,255,0.98);
    border: 1px solid rgba(0,0,0,0.14);
    border-radius: %9px;
    padding: %10px %11px;
    font-size: %12px;
    color: rgba(0,0,0,0.85);
    min-height: %13px;
}
QPlainTextEdit:focus { border: 1px solid rgba(66,133,244,0.85); }

/* 推荐：统一 QLabel 字体（你之前没控 label） */
QLabel { font-size: %14px; color: rgba(0,0,0,0.80); }
)")
    .arg(px(10))
    .arg(fieldMinH)
    .arg(fieldPadV).arg(fieldPadH)
    .arg(fieldFont)
    .arg(px(32))
    .arg(px(20))
    .arg(px(10))
    .arg(px(12))
    .arg(px(8)).arg(px(10))
    .arg(plainFont)
    .arg(plainMinH)
    .arg(px(14));  // label font

    // ScrollBar
    const QString scrollBarStyle = QString(R"(
QScrollBar:vertical {
    background: transparent;
    width: %1px;
    margin: %2px %3px %2px 0px;
}
QScrollBar::handle:vertical {
    background: rgba(0,0,0,0.22);
    min-height: %4px;
    border-radius: %5px;
}
QScrollBar::handle:vertical:hover { background: rgba(0,0,0,0.32); }
QScrollBar::handle:vertical:pressed { background: rgba(0,0,0,0.42); }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: transparent; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: rgba(0,0,0,0.04); border-radius: %6px; }

QScrollBar:horizontal {
    background: transparent;
    height: %1px;
    margin: 0px %2px %3px %2px;
}
QScrollBar::handle:horizontal {
    background: rgba(0,0,0,0.22);
    min-width: %4px;
    border-radius: %5px;
}
QScrollBar::handle:horizontal:hover { background: rgba(0,0,0,0.32); }
QScrollBar::handle:horizontal:pressed { background: rgba(0,0,0,0.42); }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: transparent; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: rgba(0,0,0,0.04); border-radius: %6px; }
)")
    .arg(px(10))
    .arg(px(6))
    .arg(px(4))
    .arg(px(40))
    .arg(px(10))
    .arg(px(8));

    return groupStyle + fieldStyle + scrollBarStyle;
}


/** @brief 按屏幕缩放系数生成统一滚动条样式。 */
static QString buildScrollBarQss(double scale)
{
    auto px = [&](double v){ return int(qRound(v * scale)); };
    return QString(R"(
    QScrollBar:vertical {
        background: transparent;
        width: %1px;
        margin: %2px %3px %2px 0px;
    }
    QScrollBar::handle:vertical {
        background: rgba(0,0,0,0.22);
        min-height: %4px;
        border-radius: %5px;
    }
    QScrollBar::handle:vertical:hover { background: rgba(0,0,0,0.32); }
    QScrollBar::handle:vertical:pressed { background: rgba(0,0,0,0.42); }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: transparent; }
    QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: rgba(0,0,0,0.04); border-radius: %6px; }

    QScrollBar:horizontal {
        background: transparent;
        height: %1px;
        margin: 0px %2px %3px %2px;
    }
    QScrollBar::handle:horizontal {
        background: rgba(0,0,0,0.22);
        min-width: %4px;
        border-radius: %5px;
    }
    QScrollBar::handle:horizontal:hover { background: rgba(0,0,0,0.32); }
    QScrollBar::handle:horizontal:pressed { background: rgba(0,0,0,0.42); }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: transparent; }
    QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: rgba(0,0,0,0.04); border-radius: %6px; }
    )")
    .arg(px(10)).arg(px(6)).arg(px(4)).arg(px(40)).arg(px(10)).arg(px(8));
}

/** @brief 只应用滚动条样式，不改变字体、按钮和输入框。 */
void AppStyle::applyScrollBarOnly(QWidget* w)
{
    if (!w) return;
    const double s = uiScaleForScreen();
    const QString sb = buildScrollBarQss(s);

    // 给窗口下所有滚动条一个统一风格（不改字体）
    w->setStyleSheet(w->styleSheet() + sb);
}

/** @brief 将完整设置页样式应用到指定窗口。 */
void AppStyle::applyTo(QWidget* w)
{
    if (!w) return;
    g_scale = uiScaleForScreen();
    w->setStyleSheet(w->styleSheet() + pageStyleSheet());
}

/** @return 已配置触摸滚动和统一样式的 QScrollArea。 */
QScrollArea* AppStyle::createScrollArea(QWidget* parent)
{
    g_scale = uiScaleForScreen();

    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // 强制 Qt 绘制
    scroll->setStyleSheet("QScrollArea { background: transparent; }");
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground, true);

    // 强行把滚动条样式打到对象上
    const QString sb = pageStyleSheet();
    scroll->verticalScrollBar()->setStyleSheet(sb);
    scroll->horizontalScrollBar()->setStyleSheet(sb);

    return scroll;
}
