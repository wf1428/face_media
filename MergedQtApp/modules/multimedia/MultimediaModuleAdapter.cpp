/**
 * @file MultimediaModuleAdapter.cpp
 * @brief 将原多媒体 MainWindow 适配为应用外壳统一生命周期接口的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "MultimediaModuleAdapter.h"

#include "mainwindow.h"

#include <QDebug>

/** @brief 保存窗口父对象，实际多媒体窗口在 initialize() 中创建。 */
MultimediaModuleAdapter::MultimediaModuleAdapter(QWidget *widgetParent, QObject *parent)
    : QObject(parent),
      widgetParent_(widgetParent)
{
}

/** @return 多媒体主窗口；尚未初始化时返回 nullptr。 */
QWidget *MultimediaModuleAdapter::rootWidget()
{
    return window_;
}

/** @brief 创建多媒体主窗口并建立初始活动状态。 */
bool MultimediaModuleAdapter::initialize()
{
    if (window_) {
        return true;
    }
    if (!widgetParent_) {
        qCritical() << "[MULTIMEDIA-ADAPTER] missing widget parent";
        return false;
    }
    window_ = new MainWindow(widgetParent_);
    window_->setWindowFlags(Qt::Widget);
    shutdown_ = false;
    qInfo() << "[MULTIMEDIA-ADAPTER] initialized";
    return true;
}

/** @brief 恢复多媒体模块前台播放。 */
void MultimediaModuleAdapter::activate()
{
    if (!window_ || shutdown_) {
        active_ = false;
        return;
    }
    window_->activateModule();
    active_ = window_->isModuleActive();
    qInfo() << "[MULTIMEDIA-ADAPTER] activate result=" << active_;
}

/** @brief 停止多媒体播放并标记模块停用。 */
void MultimediaModuleAdapter::deactivate()
{
    if (window_) {
        window_->deactivateModule();
    }
    active_ = false;
    qInfo() << "[MULTIMEDIA-ADAPTER] deactivated";
}

/** @brief 关闭多媒体模块；重复调用保持幂等。 */
void MultimediaModuleAdapter::shutdown()
{
    if (shutdown_) {
        return;
    }
    deactivate();
    shutdown_ = true;
    qInfo() << "[MULTIMEDIA-ADAPTER] shutdown complete";
}

/** @return 多媒体模块当前活动时返回 true。 */
bool MultimediaModuleAdapter::isActive() const
{
    return active_ && window_ && window_->isModuleActive();
}

/** @return 仅在多媒体主页面处于可切换状态时返回 true。 */
bool MultimediaModuleAdapter::allowsPresenceSwitch() const
{
    return isActive() && window_->allowsPresenceSwitch();
}
