/**
 * @file MultimediaModuleAdapter.cpp
 * @brief 将原多媒体 MainWindow 适配为应用外壳统一生命周期接口的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "MultimediaModuleAdapter.h"

#include "mainwindow.h"
#include "components/multimedia/multimediademo.h"

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
    multimediaPage_ = window_->findChild<MultimediaDemo *>();
    if (!multimediaPage_) {
        qCritical() << "[MULTIMEDIA-ADAPTER] MultimediaDemo page was not found";
        delete window_;
        window_ = nullptr;
        return false;
    }
    connect(multimediaPage_, &MultimediaDemo::adminLongPressRequested,
            this, &MultimediaModuleAdapter::adminLongPressRequested);
    connect(multimediaPage_, &MultimediaDemo::recognitionResultDisplayStarted,
            this, &MultimediaModuleAdapter::recognitionResultDisplayStarted);
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

void MultimediaModuleAdapter::beginEmbeddedFaceRecognition()
{
    if (multimediaPage_) multimediaPage_->beginEmbeddedFaceRecognition();
}

void MultimediaModuleAdapter::endEmbeddedFaceRecognition()
{
    if (multimediaPage_) multimediaPage_->endEmbeddedFaceRecognition();
}

void MultimediaModuleAdapter::setEmbeddedFaceFrame(const QImage &image)
{
    if (multimediaPage_) multimediaPage_->setEmbeddedFaceFrame(image);
}

void MultimediaModuleAdapter::setEmbeddedFaceFaces(const QVector<DetectedFace> &faces)
{
    if (multimediaPage_) multimediaPage_->setEmbeddedFaceFaces(faces);
}

void MultimediaModuleAdapter::setEmbeddedFaceState(VerifyState state,
                                                    const QString &message)
{
    if (multimediaPage_) multimediaPage_->setEmbeddedFaceState(state, message);
}

void MultimediaModuleAdapter::setEmbeddedFaceUnavailable(const QString &message)
{
    if (multimediaPage_) multimediaPage_->setEmbeddedFaceUnavailable(message);
}

void MultimediaModuleAdapter::showRecognitionImage(const QImage &image,
                                                    const QString &name,
                                                    int durationMs)
{
    if (multimediaPage_) multimediaPage_->showRecognitionImage(image, name, durationMs);
}
