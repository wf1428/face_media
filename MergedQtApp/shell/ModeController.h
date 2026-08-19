#ifndef MODECONTROLLER_H
#define MODECONTROLLER_H

#include <QObject>
#include <QString>

class CursorOverlay;
class IApplicationModule;
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
        SwitchingToMultimedia,
        ShuttingDown
    };
    Q_ENUM(AppModeState)

    explicit ModeController(QStackedWidget *stack,
                            IApplicationModule *multimediaModule,
                            IApplicationModule *faceGateModule,
                            CursorOverlay *cursor,
                            int recognitionReturnDelayMs,
                            QObject *parent = nullptr);

    bool start();
    void shutdown();
    AppModeState state() const;

public slots:
    void handlePresenceDetected();
    void handlePresenceLost();
    void handleRecognitionSucceeded();
    void reconcilePresenceLevel(bool valid, bool present);

signals:
    void stateChanged(ModeController::AppModeState state);
    void switchFailed(const QString &message);

private:
    void setState(AppModeState state);
    void raiseCursor();
    void switchToMultimedia(const char *reason, bool waitForPresenceReset);

    QStackedWidget *stack_ = nullptr;
    IApplicationModule *multimediaModule_ = nullptr;
    IApplicationModule *faceGateModule_ = nullptr;
    CursorOverlay *cursor_ = nullptr;
    QTimer *recognitionReturnTimer_ = nullptr;
    AppModeState state_ = AppModeState::SwitchingToMultimedia;
    bool started_ = false;
    bool recognitionResultPending_ = false;
    bool waitForPresenceReset_ = false;
};

#endif // MODECONTROLLER_H
