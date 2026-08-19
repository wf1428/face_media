#ifndef APPSHELL_H
#define APPSHELL_H

#include <QWidget>

class CursorOverlay;
class FaceGateModuleAdapter;
class ModeController;
class MultimediaModuleAdapter;
class QResizeEvent;
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

public slots:
    void raiseCursorOverlay();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QString shellConfigPath() const;
    QString faceGateConfigPath() const;

    QStackedWidget *stack_ = nullptr;
    CursorOverlay *cursor_ = nullptr;
    MultimediaModuleAdapter *multimediaModule_ = nullptr;
    FaceGateModuleAdapter *faceGateModule_ = nullptr;
    ModeController *modeController_ = nullptr;
    Sr505PresenceSensor *presenceSensor_ = nullptr;
    QTimer *presenceReconcileTimer_ = nullptr;
    bool initialized_ = false;
    bool shutdown_ = false;
};

#endif // APPSHELL_H
