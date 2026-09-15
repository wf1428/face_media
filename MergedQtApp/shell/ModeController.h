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
    void handleRecognitionDisplayStarted();
    void handleFacePresenceChanged(bool present);
    void reconcilePresenceLevel(bool valid, bool present);
    void handleAdminRequested();
    void handleAdminSessionFinished();

signals:
    void stateChanged(ModeController::AppModeState state);
    void switchFailed(const QString &message);
    /** @brief 结果保持结束且人体仍在时通知外壳恢复识别附属设备。 */
    void recognitionResumed();

private:
    void setState(AppModeState state);
    void switchToMultimedia(const char *reason, bool waitForPresenceReset);

    QStackedWidget *stack_ = nullptr;
    MultimediaModuleAdapter *multimediaModule_ = nullptr;
    FaceGateModuleAdapter *faceGateModule_ = nullptr;
    QTimer *recognitionReturnTimer_ = nullptr;
    AppModeState state_ = AppModeState::SwitchingToMultimedia;
    bool started_ = false;
    bool recognitionResultPending_ = false;
    bool waitForPresenceReset_ = false;
    bool presenceLevelValid_ = false;
    bool presencePresent_ = false;
    bool facePresent_ = false;
};

#endif // MODECONTROLLER_H
