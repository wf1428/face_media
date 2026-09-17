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
    foregroundRecognitionMs_ = qMax(1000, recognitionReturnDelayMs);
    foregroundRecognitionTimer_ = new QTimer(this);
    foregroundRecognitionTimer_->setSingleShot(true);
    foregroundRecognitionTimer_->setInterval(foregroundRecognitionMs_);
    connect(foregroundRecognitionTimer_, &QTimer::timeout, this, [this]() {
        if (!started_ || state_ != AppModeState::FaceGateActive) {
            return;
        }
        if (foregroundCameraUnavailable_ || !faceGateModule_->isActive()) {
            leaveForegroundRecognition(
                        "20s foreground camera failure timeout expired", true);
            return;
        }
        if (!faceGateModule_->allowsPresenceSwitch()) {
            qInfo() << "[MODE] foreground timeout delayed by protected face operation";
            foregroundRecognitionTimer_->start(500);
            return;
        }
        if (presenceLevelValid_ && presencePresent_) {
            foregroundExitPendingForPresenceLost_ = true;
            qInfo() << "[MODE] foreground timeout expired while PIR remains high;"
                       " keeping camera visible until PIR returns low";
            return;
        }
        leaveForegroundRecognition(
                    "20s foreground recognition inactivity expired", false);
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
    foregroundRecognitionTimer_->stop();
    foregroundExitPendingForPresenceLost_ = false;
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
    foregroundRecognitionTimer_->stop();
    waitForPresenceReset_ = false;
    presenceLevelValid_ = false;
    presencePresent_ = false;
    facePresent_ = false;
    foregroundCameraUnavailable_ = false;
    foregroundExitPendingForPresenceLost_ = false;
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
    foregroundRecognitionTimer_->stop();
    facePresent_ = false;
    foregroundCameraUnavailable_ = false;
    foregroundExitPendingForPresenceLost_ = false;
    setState(AppModeState::SwitchingToFaceGate);
#ifdef FACEGATE_LEGACY_PAGE_MODE
    multimediaModule_->deactivate();
    stack_->setCurrentWidget(faceGateModule_->rootWidget());
#else
    // 视频始终保持播放；这里只把 pictureRect 从 PIC 切换为摄像头预览。
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
        // 摄像头未连接时避免当前持续高电平每 200 ms 立即重复拉起失败链路。
        waitForPresenceReset_ = presenceLevelValid_ && presencePresent_;
        setState(AppModeState::MultimediaActive);
        foregroundCameraUnavailable_ = false;
        emit switchFailed(QStringLiteral("facegate activation failed; multimedia restored"));
        return;
    }

    faceGateModule_->restartRecognition(QStringLiteral("正在识别"));
    setState(AppModeState::FaceGateActive);
    foregroundRecognitionTimer_->start(foregroundRecognitionMs_);
    qInfo() << "[MODE] facegate mode active; foreground inactivity timer started";
}

void ModeController::handlePresenceLost()
{
    presenceLevelValid_ = true;
    presencePresent_ = false;
    if (foregroundExitPendingForPresenceLost_
            && state_ == AppModeState::FaceGateActive) {
        foregroundExitPendingForPresenceLost_ = false;
        leaveForegroundRecognition(
                    "PIR returned low after foreground timeout", false);
        return;
    }
    if (waitForPresenceReset_ && state_ == AppModeState::MultimediaActive) {
        waitForPresenceReset_ = false;
        qInfo() << "[MODE] SR505 reset to low; next high level is armed";
        return;
    }
    if (!started_) {
        qInfo() << "[MODE] presenceLost ignored"
                << "state=" << static_cast<int>(state_);
        return;
    }
    // 前台 20 秒周期内，PIR 电平只记录状态，不触发页面切换或刷新计时。
    qInfo() << "[MODE] presenceLost recorded without mode transition"
            << "state=" << static_cast<int>(state_);
}

void ModeController::handleRecognitionFinished()
{
    // 4 秒窗口失败不刷新前台倒计时；识别通过由 handleRecognitionVerified() 刷新。
}

/** @brief 只有 4 秒窗口内身份验证通过，才刷新前台 20 秒倒计时。 */
void ModeController::handleRecognitionVerified()
{
    if (started_ && state_ == AppModeState::FaceGateActive
            && !foregroundCameraUnavailable_) {
        foregroundRecognitionTimer_->start(foregroundRecognitionMs_);
        qInfo() << "[MODE] verified face refreshed foreground inactivity timer";
    }
}

/** @brief 权限下发成功后，从该时刻重新计算完整的前台 20 秒周期。 */
void ModeController::handleRecognitionSucceeded()
{
    if (started_ && state_ == AppModeState::FaceGateActive
            && !foregroundCameraUnavailable_) {
        foregroundRecognitionTimer_->start(foregroundRecognitionMs_);
        qInfo() << "[MODE] successful access refreshed foreground cycle";
    }
}

/** @brief 等待图片开始新一批显示时重新计算完整的结果保持时间。 */
void ModeController::handleRecognitionDisplayStarted()
{
#ifndef FACEGATE_LEGACY_PAGE_MODE
    if (!started_ || state_ != AppModeState::FaceGateActive
            || foregroundCameraUnavailable_) {
        return;
    }
    foregroundRecognitionTimer_->start(foregroundRecognitionMs_);
#endif
}

void ModeController::handleFacePresenceChanged(bool present)
{
    facePresent_ = present;
    // 检测到新的人脸意味着新的 4 秒窗口即将开始，从此刻重新计算前台 20 秒。
    if (present && started_ && state_ == AppModeState::FaceGateActive
            && !foregroundCameraUnavailable_) {
        foregroundRecognitionTimer_->start(foregroundRecognitionMs_);
        qInfo() << "[MODE] face detection started a new recognition window;"
                   " foreground inactivity timer refreshed";
    }
}

/** @brief 摄像头运行中断后保证错误画面最多保持一个前台周期。 */
void ModeController::handleCameraUnavailable(const QString &message)
{
    if (!started_ || state_ != AppModeState::FaceGateActive) {
        return;
    }
    if (!foregroundCameraUnavailable_) {
        foregroundCameraUnavailable_ = true;
        foregroundRecognitionTimer_->start(foregroundRecognitionMs_);
    }
    qWarning().noquote() << "[MODE] foreground camera unavailable;"
                           " forced recovery timer started:" << message;
}

void ModeController::reconcilePresenceLevel(bool valid, bool present)
{
    presenceLevelValid_ = valid;
    presencePresent_ = valid && present;
    if (!valid || !started_) {
        return;
    }
    if (foregroundExitPendingForPresenceLost_
            && state_ == AppModeState::FaceGateActive && !present) {
        foregroundExitPendingForPresenceLost_ = false;
        leaveForegroundRecognition(
                    "PIR low level reconciled after foreground timeout", false);
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
    }
}

/** @brief 前台超时后关闭摄像头和推理、恢复 PIC，并重新等待一次有效 PIR 触发。 */
void ModeController::leaveForegroundRecognition(const char *reason,
                                                bool cameraFailure)
{
    if (!started_ || state_ != AppModeState::FaceGateActive) {
        return;
    }

    if (cameraFailure) {
        qWarning() << "[MODE] leaving failed foreground camera"
                   << "reason=" << reason;
    } else {
        qInfo() << "[MODE] foreground recognition finished"
                << "reason=" << reason;
    }
    foregroundRecognitionTimer_->stop();
    faceGateModule_->deactivate();
#ifdef FACEGATE_LEGACY_PAGE_MODE
    stack_->setCurrentWidget(multimediaModule_->rootWidget());
    multimediaModule_->activate();
#else
    stack_->setCurrentWidget(multimediaModule_->rootWidget());
    multimediaModule_->endEmbeddedFaceRecognition();
#endif
    facePresent_ = false;
    foregroundCameraUnavailable_ = false;
    foregroundExitPendingForPresenceLost_ = false;
    waitForPresenceReset_ = presenceLevelValid_ && presencePresent_;
    setState(AppModeState::MultimediaActive);
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

    foregroundRecognitionTimer_->stop();
    foregroundExitPendingForPresenceLost_ = false;
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

/** @brief 管理认证成功后暂停播放器；普通前台识别不经过此路径。 */
void ModeController::handleAdminSessionStarted()
{
#ifndef FACEGATE_LEGACY_PAGE_MODE
    if (!started_ || state_ != AppModeState::AdminActive) {
        return;
    }
    multimediaModule_->deactivate();
    qInfo() << "[MODE] face admin panel entered; multimedia playback stopped";
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
    // 密码成功进入后台时多媒体模块已经停用；重新激活会按原有约定
    // 从本地播放列表第一项开始，并恢复仍然有效的 LIVE 探测目标。
    multimediaModule_->activate();
    foregroundRecognitionTimer_->stop();
    facePresent_ = false;
    foregroundExitPendingForPresenceLost_ = false;
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
