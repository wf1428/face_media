#include "ModeController.h"

#include "IApplicationModule.h"

#include <QDebug>
#include <QStackedWidget>
#include <QTimer>

ModeController::ModeController(QStackedWidget *stack,
                               IApplicationModule *multimediaModule,
                               IApplicationModule *faceGateModule,
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
        if (presenceLevelValid_ && presencePresent_) {
            qInfo() << "[MODE] recognition result hold expired; presence remains high";
            emit recognitionResumed();
            return;
        }
        switchToMultimedia("recognition result timeout with no presence", false);
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
    multimediaModule_->deactivate();
    stack_->setCurrentWidget(faceGateModule_->rootWidget());
    faceGateModule_->activate();

    if (!faceGateModule_->isActive()) {
        qWarning() << "[MODE] facegate activation failed; rolling back";
        faceGateModule_->deactivate();
        stack_->setCurrentWidget(multimediaModule_->rootWidget());
        multimediaModule_->activate();
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

    recognitionReturnTimer_->stop();
    recognitionResultPending_ = false;
    facePresent_ = false;
    waitForPresenceReset_ = waitForPresenceReset;
    setState(AppModeState::MultimediaActive);
    qInfo() << "[MODE] multimedia mode active";
}

void ModeController::setState(AppModeState state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}
