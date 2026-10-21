#ifndef WAYLAND_DISPLAY_MANAGER_H
#define WAYLAND_DISPLAY_MANAGER_H

#include <QObject>
#include <QString>
#include <memory>

class QScreen;
class QWidget;

/** Select the fixed Wayland source output and keep a fullscreen window on it. */
class WaylandDisplayManager final : public QObject
{
public:
    explicit WaylandDisplayManager(QObject *parent = nullptr);
    ~WaylandDisplayManager() override;

    bool hasFixedOutput() const { return !outputName_.isEmpty(); }
    QScreen *selectStartupScreen();
    void placeWindow(QWidget *window, QScreen *screen);
    bool trackFullscreenWindow(QWidget *window, QScreen *screen);
    QScreen *findScreen(const QString &outputName, int waitMs = 0);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    QString outputName_;
};

#endif
