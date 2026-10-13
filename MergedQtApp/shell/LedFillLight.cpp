/**
 * @file LedFillLight.cpp
 * @brief /dev/led 人脸识别补光灯控制模块的实现。
 */

#include "LedFillLight.h"

#include <QDebug>
#include <QFile>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

LedFillLight::LedFillLight(const QString &devicePath, QObject *parent)
    : QObject(parent),
      devicePath_(devicePath.trimmed().isEmpty()
                  ? QStringLiteral("/dev/led")
                  : devicePath.trimmed())
{
    // 应用启动时主动写低电平，和驱动的默认关闭状态保持一致。
    turnOff();
}

LedFillLight::~LedFillLight()
{
    // 无论应用从哪个状态退出，都尽力保证补光灯关闭。
    turnOff();
}

bool LedFillLight::turnOn()
{
    return applyState(true);
}

bool LedFillLight::turnOff()
{
    return applyState(false);
}

bool LedFillLight::isOn() const
{
    return stateKnown_ && on_;
}

bool LedFillLight::applyState(bool on)
{
    if (stateKnown_ && on_ == on) {
        return true;
    }

    const QByteArray nativePath = QFile::encodeName(devicePath_);
    int fd = -1;
    do {
        fd = ::open(nativePath.constData(), O_WRONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        const int savedErrno = errno;
        qWarning() << "[FILL-LIGHT] cannot open LED device"
                   << devicePath_
                   << QString::fromLocal8Bit(std::strerror(savedErrno));
        return false;
    }

    const char value = on ? '1' : '0';
    ssize_t written = -1;
    do {
        written = ::write(fd, &value, 1);
    } while (written < 0 && errno == EINTR);
    const int writeErrno = written < 0 ? errno : EIO;
    ::close(fd);

    if (written != 1) {
        qWarning() << "[FILL-LIGHT] cannot write LED state"
                   << devicePath_
                   << QString::fromLocal8Bit(std::strerror(writeErrno));
        return false;
    }

    on_ = on;
    stateKnown_ = true;
    qInfo() << "[FILL-LIGHT]" << (on ? "ON" : "OFF")
            << "device=" << devicePath_;
    return true;
}
