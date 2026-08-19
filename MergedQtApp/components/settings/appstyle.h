/**
 * @file appstyle.h
 * @brief 1024x768 设置页面的统一缩放、QSS 和滚动区域工厂。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef APPSTYLE_H
#define APPSTYLE_H

#include <QString>
#include <QScreen>
#include <QScrollBar>

class QWidget;
class QScrollArea;

/** @brief 1024x768 设置页面的统一缩放、QSS 和滚动区域工厂。 */
class AppStyle
{
public:
    /** @return 相对设计分辨率计算的统一 UI 缩放系数。 */
    static double uiScaleForScreen();

    /** @return 设计像素乘以当前屏幕缩放后的整数像素值。 */
    static int px(double v);

    /** @return 设置页分组、输入、按钮、标签和滚动条的完整 QSS。 */
    static QString pageStyleSheet();

    /** @brief 将完整设置页样式应用到指定窗口。 */
    static void applyTo(QWidget* w);

    /** @return 已配置触摸滚动和统一样式的 QScrollArea。 */
    static QScrollArea* createScrollArea(QWidget* parent);

    /** @brief 只应用滚动条样式，不改变字体、按钮和输入框。 */
    static void applyScrollBarOnly(QWidget* w);
};


#endif // APPSTYLE_H
