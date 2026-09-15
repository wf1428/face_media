#include "ModeController.h"

#include "modules/facegate/FaceGateModuleAdapter.h"
#include "modules/multimedia/MultimediaModuleAdapter.h"

#include <QDebug>
#include <QStackedWidget>
#include <QTimer>

ModeController::ModeController(QStackedWidget *stack,
                               MultimediaModuleAdapter *multimediaModule,
                               FaceGateModuleAdapter *faceGateModule,
                               int recognitionReturnDelayMs,
                               QObject *parent)
    : QObject(parent),
      stack_(stack),
      multimediaModule_(multimediaModule),
      faceGateModule_(faceGateModule)
{
    recognitionReturnTimer_ = new QTimer(this);
    recognitionReturnTimer_->setSingleShot(true);
    recognitionReturnTimer_->setInterval(qMax(0, recognitionReturnDelayMs));
    connect(recognitionReturnTimer_, &QTimer::timeout, this, [this]() {
        recognitionResultPending_ = false;
        if (!started_ || state_ != AppModeState::FaceGateActive) {
            return;
        }
        if (!faceGateModule_->allowsPresenceSwitch()) {
            qInfo() << "[MODE] recognition return suppressed by password/admin UI";
            return;
        }
#ifdef FACEGATE_LEGACY_PAGE_MODE
        if (presenceLevelValid_ && presencePresent_) {
            qInfo() << "[MODE] recognition result hold expired; presence remains high";
            emit recognitionResumed();
            return;
        }
        switchToMultimedia("recognition result timeout with no presence", false);
#else
        // 新模式必须回到播放列表，且在 SR505 先回低之前不重新触发，避免
        // 200ms 电平校准在人体仍停留时立即再次打开摄像头。
        switchToMultimedia("recognition result hold expired", true);
#endif
    });
}

bool ModeController::start()
{
    if (started_) {
        return true;
    }
    if (!stack_ || !multimediaModule_ || !faceGateModule_ ||
        !multimediaModule_->rootWidget() || !faceGateModule_->rootWidget()) {
        emit switchFailed(QStringLiteral("mode controller dependencies are incomplete"));
        return false;
    }

    faceGateModule_->deactivate();
    stack_->setCurrentWidget(multimediaModule_->rootWidget());
    multimediaModule_->activate();
    if (!multimediaModule_->isActive()) {
        emit switchFailed(QStringLiteral("multimedia module activation failed"));
        return false;
    }

    started_ = true;
    setState(AppModeState::MultimediaActive);
    qInfo() << "[MODE] initial multimedia mode active";
    return true;
}

void ModeController::shutdown()
{
    if (state_ == AppModeState::ShuttingDown) {
        return;
    }
    setState(AppModeState::ShuttingDown);
    recognitionReturnTimer_->stop();
    recognitionResultPending_ = false;
    waitForPresenceReset_ = false;
    presenceLevelValid_ = false;
    presencePresent_ = false;
    facePresent_ = false;
    if (multimediaModule_) {
        multimediaModule_->deactivate();
    }
    if (faceGateModule_) {
        faceGateModule_->deactivate();
    }
    if (multimediaModule_) {
        multimediaModule_->shutdown();
    }
    if (faceGateModule_) {
        faceGateModule_->shutdown();
    }
    started_ = false;
    qInfo() << "[MODE] modules shut down";
}

ModeController::AppModeState ModeController::state() const
{
    return state_;
}

void ModeController::handlePresenceDetected()
{
    presenceLevelValid_ = true;
    presencePresent_ = true;
    if (waitForPresenceReset_) {
        qInfo() << "[MODE] presenceDetected ignored until SR505 returns low";
        return;
    }
    if (!started_ || state_ != AppModeState::MultimediaActive) {
        qInfo() << "[MODE] presenceDetected ignored"
                << "state=" << static_cast<int>(state_);
        return;
    }
    if (!multimediaModule_->allowsPresenceSwitch()) {
        qInfo() << "[MODE] presenceDetected ignored outside MultimediaDemo main screen";
        return;
    }

    qInfo() << "[MODE] switching multimedia -> facegate";
    recognitionReturnTimer_->stop();
    recognitionResultPending_ = false;
    facePresent_ = false;
    setState(AppModeState::SwitchingToFaceGate);
#ifdef FACEGATE_LEGACY_PAGE_MODE
    multimediaModule_->deactivate();
    stack_->setCurrentWidget(faceGateModule_->rootWidget());
#else
    // 顺序不可反转：先让 GStreamer/MPP 和图片混播完整停止，再启动摄像头。
    multimediaModule_->beginEmbeddedFaceRecognition();
#endif
    faceGateModule_->activate();

    if (!faceGateModule_->isActive()) {
        qWarning() << "[MODE] facegate activation failed; rolling back";
        faceGateModule_->deactivate();
#ifdef FACEGATE_LEGACY_PAGE_MODE
        stack_->setCurrentWidget(multimediaModule_->rootWidget());
        multimediaModule_->activate();
#else
        multimediaModule_->endEmbeddedFaceRecognition();
#endif
        setState(AppModeState::MultimediaActive);
        emit switchFailed(QStringLiteral("facegate activation failed; multimedia restored"));
        return;
    }

    setState(AppModeState::FaceGateActive);
    qInfo() << "[MODE] facegate mode active";
}

void ModeController::handlePresenceLost()
{
    presenceLevelValid_ = true;
    presencePresent_ = false;
    if (waitForPresenceReset_ && state_ == AppModeState::MultimediaActive) {
        waitForPresenceReset_ = false;
        qInfo() << "[MODE] SR505 reset to low; next high level is armed";
        return;
    }
    if (!started_ || state_ != AppModeState::FaceGateActive) {
        qInfo() << "[MODE] presenceLost ignored"
                << "state=" << static_cast<int>(state_);
        return;
    }
    if (recognitionResultPending_) {
        qInfo() << "[MODE] presenceLost ignored while recognition result is held";
        return;
    }
    if (facePresent_) {
        qInfo() << "[MODE] presenceLost ignored while a face is detected";
        return;
    }
    if (!faceGateModule_->allowsPresenceSwitch()) {
        qInfo() << "[MODE] presenceLost ignored during password/admin UI";
        return;
    }

    switchToMultimedia("SR505 low level", false);
}

void ModeController::handleRecognitionFinished()
{
    if (!started_ || state_ != AppModeState::FaceGateActive ||
        !faceGateModule_->allowsPresenceSwitch()) {
        qInfo() << "[MODE] recognition result return ignored in protected/non-face state";
        return;
    }

    recognitionResultPending_ = true;
    recognitionReturnTimer_->start();
    qInfo() << "[MODE] recognition result hold started"
            << "durationMs=" << recognitionReturnTimer_->interval();
}

/** @brief 等待图片开始新一批显示时重新计算完整的结果保持时间。 */
void ModeController::handleRecognitionDisplayStarted()
{
#ifndef FACEGATE_LEGACY_PAGE_MODE
    if (!started_ || state_ != AppModeState::FaceGateActive) {
        return;
    }
    recognitionResultPending_ = true;
    recognitionReturnTimer_->start();
#endif
}

void ModeController::handleFacePresenceChanged(bool present)
{
    facePresent_ = present;
    if (present || !started_ || state_ != AppModeState::FaceGateActive ||
        recognitionResultPending_ || !presenceLevelValid_ || presencePresent_) {
        return;
    }
    if (!faceGateModule_->allowsPresenceSwitch()) {
        return;
    }

    switchToMultimedia("no face while presence is low", false);
}

void ModeController::reconcilePresenceLevel(bool valid, bool present)
{
    presenceLevelValid_ = valid;
    presencePresent_ = valid && present;
    if (!valid || !started_) {
        return;
    }
    if (waitForPresenceReset_) {
        if (!present) {
            waitForPresenceReset_ = false;
            qInfo() << "[MODE] SR505 reset to low; next high level is armed";
        }
        return;
    }
    if (state_ == AppModeState::MultimediaActive && present) {
        if (multimediaModule_->allowsPresenceSwitch()) {
            handlePresenceDetected();
        }
    } else if (state_ == AppModeState::FaceGateActive && !present) {
        if (!recognitionResultPending_ && !facePresent_ &&
            faceGateModule_->allowsPresenceSwitch()) {
            handlePresenceLost();
        }
    }
}

void ModeController::switchToMultimedia(const char *reason, bool waitForPresenceReset)
{
    if (!started_ || state_ != AppModeState::FaceGateActive) {
        return;
    }

    qInfo() << "[MODE] switching facegate -> multimedia" << "reason=" << reason;
    setState(AppModeState::SwitchingToMultimedia);
    faceGateModule_->deactivate();
#ifdef FACEGATE_LEGACY_PAGE_MODE
    stack_->setCurrentWidget(multimediaModule_->rootWidget());
    multimediaModule_->activate();

    if (!multimediaModule_->isActive()) {
        qWarning() << "[MODE] multimedia activation failed; restoring facegate";
        stack_->setCurrentWidget(faceGateModule_->rootWidget());
        faceGateModule_->activate();
        setState(faceGateModule_->isActive()
                 ? AppModeState::FaceGateActive
                 : AppModeState::SwitchingToMultimedia);
        emit switchFailed(QStringLiteral("multimedia activation failed"));
        return;
    }
#else
    stack_->setCurrentWidget(multimediaModule_->rootWidget());
    multimediaModule_->endEmbeddedFaceRecognition();
#endif

    recognitionReturnTimer_->stop();
    recognitionResultPending_ = false;
    facePresent_ = false;
    waitForPresenceReset_ = waitForPresenceReset;
    setState(AppModeState::MultimediaActive);
    qInfo() << "[MODE] multimedia mode active";
}

/** @brief 多媒体页长按后切到原 FaceGate 根窗口并打开原管理认证流程。 */
void ModeController::handleAdminRequested()
{
#ifdef FACEGATE_LEGACY_PAGE_MODE
    // 旧模式保留原人脸主页五击入口，不改变现有交互。
    return;
#else
    if (!started_ || state_ == AppModeState::ShuttingDown
            || state_ == AppModeState::AdminActive) {
        return;
    }
    if (state_ != AppModeState::MultimediaActive
            && state_ != AppModeState::FaceGateActive) {
        return;
    }

    recognitionReturnTimer_->stop();
    recognitionResultPending_ = false;
    if (state_ == AppModeState::MultimediaActive) {
        multimediaModule_->beginEmbeddedFaceRecognition();
        faceGateModule_->activate();
        if (!faceGateModule_->isActive()) {
            multimediaModule_->endEmbeddedFaceRecognition();
            emit switchFailed(QStringLiteral("facegate activation failed; multimedia restored"));
            return;
        }
    }

    stack_->setCurrentWidget(faceGateModule_->rootWidget());
    setState(AppModeState::AdminActive);
    faceGateModule_->requestOpenAdminManagement();
#endif
}

/** @brief 管理登录取消或后台关闭后回到多媒体并等待人体传感器复位。 */
void ModeController::handleAdminSessionFinished()
{
#ifndef FACEGATE_LEGACY_PAGE_MODE
    if (!started_ || state_ != AppModeState::AdminActive) {
        return;
    }
    faceGateModule_->deactivate();
    stack_->setCurrentWidget(multimediaModule_->rootWidget());
    multimediaModule_->endEmbeddedFaceRecognition();
    recognitionResultPending_ = false;
    facePresent_ = false;
    waitForPresenceReset_ = presenceLevelValid_ && presencePresent_;
    setState(AppModeState::MultimediaActive);
#endif
}

void ModeController::setState(AppModeState state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}
