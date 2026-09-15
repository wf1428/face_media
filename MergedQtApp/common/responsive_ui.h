#ifndef RESPONSIVE_UI_H
#define RESPONSIVE_UI_H

#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include "platform/rk3566_platform.h"

/**
 * 后台管理界面的统一尺寸策略。
 *
 * 多媒体播放画布仍使用用户配置的坐标和尺寸；这里仅服务于普通 Qt
 * 管理页面。测试阶段优先使用 [backRect] 中的背景尺寸判断横竖屏；
 * 配置缺失或无效时，保留原有实际宿主/屏幕几何逻辑作为回退。
 * 支持的最小逻辑尺寸为 600x600。
 */
namespace ResponsiveUi {

constexpr int kMinimumScreenExtent = 600;

/** @return 当前是否运行在 Wayland QPA 插件上。 */
inline bool isWaylandPlatform()
{
    return QGuiApplication::platformName().trimmed().toLower().startsWith(
                QStringLiteral("wayland"));
}

/**
 * @return 适合承载应用内覆盖层的长期根控件。
 *
 * Wayland 不允许客户端决定顶层窗口的全局坐标。键盘、设置页和提示框需要
 * 挂在已有根窗口内部，所有几何位置都使用该宿主的局部坐标。
 */
inline QWidget *overlayHost(QWidget *parent)
{
    if (!parent) {
        return nullptr;
    }
    QWidget *host = parent->window();
    return host ? host : parent;
}

/**
 * @return Wayland 使用应用根控件，其他平台保留调用方传入的原始父控件。
 */
inline QWidget *dialogParent(QWidget *parent)
{
    return isWaylandPlatform() ? overlayHost(parent) : parent;
}

inline bool usesTopNavigation(const QSize &size)
{
    return size.width() < 760 || size.height() > size.width();
}

inline qreal scaleForSize(const QSize &size)
{
    if (!size.isValid()) {
        return 1.0;
    }

    const QSize design = size.height() > size.width()
            ? QSize(768, 1024)
            : QSize(1024, 768);
    const qreal sx = qreal(size.width()) / qreal(design.width());
    const qreal sy = qreal(size.height()) / qreal(design.height());
    return qBound<qreal>(0.78, qMin(sx, sy), 1.25);
}

inline QRect hostRect(QWidget *parent)
{
    if (parent) {
        QWidget *host = overlayHost(parent);
        if (host && host->width() > 0 && host->height() > 0) {
            // 返回宿主局部坐标。该矩形可直接用于子覆盖层的 setGeometry()，
            // 不再依赖 Wayland 下不可控的顶层窗口全局位置。
            return host->rect();
        }
    }

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        return QRect(QPoint(0, 0), screen->availableGeometry().size());
    }
    return QRect(0, 0, 1024, 768);
}

/**
 * @return UI 配置中的背景尺寸；字段缺失或低于支持下限时返回无效 QSize。
 *
 * 配置与多媒体主界面在同一次进程启动中保持一致，因此只读取一次；
 * 修改测试分辨率后需重启应用。
 */
inline QSize configuredScreenSize()
{
    static const QSize configured = []() {
        QSettings settings(Rk3566Platform::uiConfigPath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("backRect"));
        bool widthOk = false;
        bool heightOk = false;
        const int width = settings.value(QStringLiteral("backPic_x_Size")).toInt(&widthOk);
        const int height = settings.value(QStringLiteral("backPic_y_Size")).toInt(&heightOk);
        settings.endGroup();

        if (widthOk && heightOk &&
                width >= kMinimumScreenExtent &&
                height >= kMinimumScreenExtent) {
            return QSize(width, height);
        }
        return QSize();
    }();
    return configured;
}

/** @return 配置优先的布局参考分辨率，配置无效时回退到实际宿主尺寸。 */
inline QSize layoutReferenceSize(QWidget *parent = nullptr)
{
    const QSize configured = configuredScreenSize();
    return configured.isValid() ? configured : hostRect(parent).size();
}

inline QSize dialogSize(QWidget *parent,
                        const QSize &preferred,
                        const QSize &minimum = QSize(360, 360),
                        int margin = 24)
{
    const QSize rawAvailable = hostRect(parent).size() - QSize(margin, margin);
    const QSize available(qMax(1, rawAvailable.width()),
                          qMax(1, rawAvailable.height()));
    return QSize(qBound(qMin(minimum.width(), available.width()),
                        preferred.width(), available.width()),
                 qBound(qMin(minimum.height(), available.height()),
                        preferred.height(), available.height()));
}

inline int keyboardHeight(int availableHeight, int preferredHeight)
{
    if (availableHeight <= 0) {
        return preferredHeight;
    }
    return qMin(preferredHeight, qMax(240, availableHeight * 46 / 100));
}

} // namespace ResponsiveUi

#endif // RESPONSIVE_UI_H
