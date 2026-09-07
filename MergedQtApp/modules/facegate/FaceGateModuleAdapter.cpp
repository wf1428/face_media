/**
 * @file FaceGateModuleAdapter.cpp
 * @brief 将人脸门禁主窗口适配为应用外壳统一生命周期接口的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "FaceGateModuleAdapter.h"

#include "modules/facegate/FaceGateQt/config/AppConfig.h"
#include "modules/facegate/FaceGateQt/ui/MainWindow.h"

#include <QDebug>
#include <QFileInfo>

/** @brief 保存窗口父对象和配置路径，实际窗口在 initialize() 中创建。 */
FaceGateModuleAdapter::FaceGateModuleAdapter(QWidget *widgetParent,
                                             const QString &configPath,
                                             QObject *parent)
    : QObject(parent),
      widgetParent_(widgetParent),
      configPath_(configPath)
{
}

/** @return 门禁主窗口；尚未初始化时返回 nullptr。 */
QWidget *FaceGateModuleAdapter::rootWidget()
{
    return window_;
}

/** @brief 创建并初始化门禁主窗口。 */
bool FaceGateModuleAdapter::initialize()
{
    if (window_) {
        return true;
    }
    if (!widgetParent_) {
        qCritical() << "[FACEGATE-ADAPTER] missing widget parent";
        return false;
    }

    if (!QFileInfo::exists(configPath_)) {
        qWarning() << "[FACEGATE-ADAPTER] config does not exist; safe defaults will be used"
                   << configPath_;
    }
    const AppConfig config = AppConfig::load(configPath_);
    window_ = new FaceGateMainWindow(config, widgetParent_);
    window_->setWindowFlags(Qt::Widget);
    connect(window_, &FaceGateMainWindow::recognitionSucceeded,
            this, &FaceGateModuleAdapter::recognitionSucceeded);
    connect(window_, &FaceGateMainWindow::recognitionFinished,
            this, &FaceGateModuleAdapter::recognitionFinished);
    connect(window_, &FaceGateMainWindow::facePresenceChanged,
            this, &FaceGateModuleAdapter::facePresenceChanged);
    shutdown_ = false;
    qInfo() << "[FACEGATE-ADAPTER] initialized"
            << "config=" << configPath_;
    return true;
}

/** @brief 激活摄像头、推理和门禁界面。 */
void FaceGateModuleAdapter::activate()
{
    active_ = window_ && !shutdown_ && window_->activateModule();
    qInfo() << "[FACEGATE-ADAPTER] activate result=" << active_;
}

/** @brief 停止门禁前台活动并保留可恢复资源。 */
void FaceGateModuleAdapter::deactivate()
{
    if (window_) {
        window_->deactivateModule();
    }
    active_ = false;
    qInfo() << "[FACEGATE-ADAPTER] deactivated";
}

/** @brief 关闭门禁模块；重复调用保持幂等。 */
void FaceGateModuleAdapter::shutdown()
{
    if (shutdown_) {
        return;
    }
    active_ = false;
    if (window_) {
        window_->shutdownModule();
    }
    shutdown_ = true;
    qInfo() << "[FACEGATE-ADAPTER] shutdown complete";
}

/** @return 模块已经激活且未关闭时返回 true。 */
bool FaceGateModuleAdapter::isActive() const
{
    return active_ && window_ && window_->isModuleActive();
}

/** @return 仅在门禁识别主页处于可切换状态时返回 true。 */
bool FaceGateModuleAdapter::allowsPresenceSwitch() const
{
    return isActive() && window_->allowsPresenceSwitch();
}
