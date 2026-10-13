#ifndef SR505PRESENCESENSOR_H
#define SR505PRESENCESENSOR_H

#include "IPresenceSensor.h"

#include <QString>
#include <QTimer>

class QSocketNotifier;

/** Application adapter for the pollable /dev/sr505 kernel-driver node. */
class Sr505PresenceSensor final : public IPresenceSensor
{
    Q_OBJECT

public:
    explicit Sr505PresenceSensor(const QString &devicePath,
                                 bool activeHigh,
                                 int reconnectIntervalMs,
                                 QObject *parent = nullptr);
    ~Sr505PresenceSensor() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

    bool isPresent() const;
    bool hasValidLevel() const;

private slots:
    void openDeviceIfNeeded();
    void readAvailableEvents();

private:
    void closeDevice();
    void publishLevel(bool high);
    void reportFaultOnce(const QString &message);

    QString devicePath_;
    QTimer reconnectTimer_;
    QSocketNotifier *notifier_ = nullptr;
    int fd_ = -1;
    bool activeHigh_ = true;
    bool running_ = false;
    bool levelValid_ = false;
    bool stableHigh_ = false;
    bool faultReported_ = false;
};

#endif // SR505PRESENCESENSOR_H
