#ifndef APPSHELL_H
#define APPSHELL_H

#include <QWidget>

class FaceGateModuleAdapter;
class LedFillLight;
class ModeController;
class MultimediaModuleAdapter;
class QStackedWidget;
class QTimer;
class Sr505PresenceSensor;

class AppShell final : public QWidget
{
    Q_OBJECT

public:
    explicit AppShell(QWidget *parent = nullptr);
    ~AppShell() override;

    bool initialize();
    void shutdown();
    QWidget *multimediaRootWidget() const;
    QWidget *activeRootWidget() const;

private:
    QString shellConfigPath() const;
    QString faceGateConfigPath() const;

    QStackedWidget *stack_ = nullptr;
    MultimediaModuleAdapter *multimediaModule_ = nullptr;
    FaceGateModuleAdapter *faceGateModule_ = nullptr;
    ModeController *modeController_ = nullptr;
    Sr505PresenceSensor *presenceSensor_ = nullptr;
    LedFillLight *fillLight_ = nullptr;
    QTimer *presenceReconcileTimer_ = nullptr;
    bool initialized_ = false;
    bool shutdown_ = false;
};

#endif // APPSHELL_H
