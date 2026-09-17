#ifndef MODECONTROLLER_H
#define MODECONTROLLER_H

#include <QObject>
#include <QString>

class FaceGateModuleAdapter;
class MultimediaModuleAdapter;
class QStackedWidget;
class QTimer;

class ModeController final : public QObject
{
    Q_OBJECT

public:
    enum class AppModeState {
        MultimediaActive,
        SwitchingToFaceGate,
        FaceGateActive,
        AdminActive,
        SwitchingToMultimedia,
        ShuttingDown
    };
    Q_ENUM(AppModeState)

    explicit ModeController(QStackedWidget *stack,
                            MultimediaModuleAdapter *multimediaModule,
                            FaceGateModuleAdapter *faceGateModule,
                            int recognitionReturnDelayMs,
                            QObject *parent = nullptr);

    bool start();
    void shutdown();
    AppModeState state() const;

public slots:
    void handlePresenceDetected();
    void handlePresenceLost();
    void handleRecognitionFinished();
    void handleRecognitionVerified();
    void handleRecognitionSucceeded();
    void handleRecognitionDisplayStarted();
    void handleFacePresenceChanged(bool present);
    void handleCameraUnavailable(const QString &message);
    void reconcilePresenceLevel(bool valid, bool present);
    void handleAdminRequested();
    void handleAdminSessionStarted();
    void handleAdminSessionFinished();

signals:
    void stateChanged(ModeController::AppModeState state);
    void switchFailed(const QString &message);

private:
    void setState(AppModeState state);
    void leaveForegroundRecognition(const char *reason, bool cameraFailure);

    QStackedWidget *stack_ = nullptr;
    MultimediaModuleAdapter *multimediaModule_ = nullptr;
    FaceGateModuleAdapter *faceGateModule_ = nullptr;
    QTimer *foregroundRecognitionTimer_ = nullptr;
    int foregroundRecognitionMs_ = 20000;
    AppModeState state_ = AppModeState::SwitchingToMultimedia;
    bool started_ = false;
    bool waitForPresenceReset_ = false;
    bool presenceLevelValid_ = false;
    bool presencePresent_ = false;
    bool facePresent_ = false;
    bool foregroundCameraUnavailable_ = false;
    /** 前台20秒已到，但需等待当前PIR高电平结束后再恢复PIC。 */
    bool foregroundExitPendingForPresenceLost_ = false;
};

#endif // MODECONTROLLER_H
