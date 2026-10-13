#include "Sr505PresenceSensor.h"

#include <QDebug>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

Sr505PresenceSensor::Sr505PresenceSensor(const QString &devicePath,
                                         bool activeHigh,
                                         int reconnectIntervalMs,
                                         QObject *parent)
    : IPresenceSensor(parent),
      devicePath_(devicePath),
      activeHigh_(activeHigh)
{
    reconnectTimer_.setInterval(qMax(200, reconnectIntervalMs));
    connect(&reconnectTimer_, &QTimer::timeout,
            this, &Sr505PresenceSensor::openDeviceIfNeeded);
}

Sr505PresenceSensor::~Sr505PresenceSensor()
{
    stop();
}

bool Sr505PresenceSensor::start()
{
    if (running_) {
        return true;
    }
    if (devicePath_.trimmed().isEmpty()) {
        emit sensorFault(QStringLiteral("SR505 device path is empty"));
        return false;
    }

    running_ = true;
    levelValid_ = false;
    faultReported_ = false;
    reconnectTimer_.start();
    openDeviceIfNeeded();
    qInfo() << "[SR505] application adapter started"
            << "device=" << devicePath_
            << "activeHigh=" << activeHigh_;
    return true;
}

void Sr505PresenceSensor::stop()
{
    if (!running_ && fd_ < 0) {
        return;
    }
    running_ = false;
    reconnectTimer_.stop();
    closeDevice();
    levelValid_ = false;
    qInfo() << "[SR505] application adapter stopped";
}

bool Sr505PresenceSensor::isRunning() const
{
    return running_;
}

bool Sr505PresenceSensor::isPresent() const
{
    return levelValid_ && stableHigh_;
}

bool Sr505PresenceSensor::hasValidLevel() const
{
    return levelValid_;
}

void Sr505PresenceSensor::openDeviceIfNeeded()
{
    if (!running_ || fd_ >= 0) {
        return;
    }

    fd_ = ::open(devicePath_.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        const int savedErrno = errno;
        reportFaultOnce(QStringLiteral("cannot open SR505 device %1: %2")
                        .arg(devicePath_, QString::fromLocal8Bit(std::strerror(savedErrno))));
        return;
    }

    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated,
            this, &Sr505PresenceSensor::readAvailableEvents);
    reconnectTimer_.stop();
    faultReported_ = false;
    qInfo() << "[SR505] device opened" << devicePath_;

    // The kernel driver sets event_pending during module initialization, so
    // this read publishes the startup level immediately (including HIGH).
    readAvailableEvents();
}

void Sr505PresenceSensor::readAvailableEvents()
{
    if (!running_ || fd_ < 0) {
        return;
    }

    for (;;) {
        unsigned char state = 0xff;
        errno = 0;
        const ssize_t count = ::read(fd_, &state, sizeof(state));
        if (count == 1) {
            if (state > 1) {
                reportFaultOnce(QStringLiteral("invalid SR505 byte %1 from %2")
                                .arg(static_cast<int>(state)).arg(devicePath_));
                continue;
            }
            faultReported_ = false;
            const bool rawHigh = state == 1;
            publishLevel(activeHigh_ ? rawHigh : !rawHigh);
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }

        const int savedErrno = errno;
        reportFaultOnce(QStringLiteral("cannot read SR505 device %1: %2")
                        .arg(devicePath_, QString::fromLocal8Bit(std::strerror(savedErrno))));
        closeDevice();
        if (running_) {
            reconnectTimer_.start();
        }
        return;
    }
}

void Sr505PresenceSensor::closeDevice()
{
    if (notifier_) {
        notifier_->setEnabled(false);
        delete notifier_;
        notifier_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Sr505PresenceSensor::publishLevel(bool high)
{
    const bool initialSample = !levelValid_;
    if (!initialSample && stableHigh_ == high) {
        return;
    }

    levelValid_ = true;
    stableHigh_ = high;
    qInfo() << "[SR505] level" << (high ? "high" : "low")
            << "initial=" << initialSample;

    if (high) {
        emit presenceDetected();
    } else if (!initialSample) {
        emit presenceLost();
    }
}

void Sr505PresenceSensor::reportFaultOnce(const QString &message)
{
    if (faultReported_) {
        return;
    }
    faultReported_ = true;
    emit sensorFault(message);
}
