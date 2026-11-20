/**
 * @file fire_gpio_monitor.cpp
 * @brief 实现消防 GPIO 的非阻塞轮询和边沿通知。
 * @author Du lin
 * @date 2026-11-19
 */

#include "fire_gpio_monitor.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <QDateTime>
#include <QByteArray>
#include <QDebug>
#include <QTimer>

namespace {
const char kFireGpioPath[] = "/dev/fire_gpio";
constexpr int kPollIntervalMs = 20;
constexpr qint64 kOpenFailureLogIntervalMs = 5000;

/**
 * @brief 解析驱动返回的消防输入值。
 * @param[in] data 驱动读取缓冲区。
 * @param[in] size 缓冲区有效字节数。
 * @param[out] active 成功时写入高低电平状态。
 * @return true 表示识别出文本或二进制 0/1；false 表示数据格式无效。
 */
bool parseLevel(const char *data, ssize_t size, bool *active)
{
    if (!data || size <= 0 || !active) return false;
    for (ssize_t i = 0; i < size; ++i) {
        const unsigned char value = static_cast<unsigned char>(data[i]);
        if (value == 0 || value == '0') {
            *active = false;
            return true;
        }
        if (value == 1 || value == '1') {
            *active = true;
            return true;
        }
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
            continue;
        }
        return false;
    }
    return false;
}
} // namespace

FireGpioMonitor::FireGpioMonitor(QObject *parent)
    : QObject(parent)
{
}

FireGpioMonitor::~FireGpioMonitor()
{
    stop();
}

void FireGpioMonitor::start()
{
    if (!pollTimer_) {
        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(kPollIntervalMs);
        pollTimer_->setTimerType(Qt::CoarseTimer);
        connect(pollTimer_, &QTimer::timeout, this, &FireGpioMonitor::poll);
    }
    if (pollTimer_->isActive()) return;

    pollTimer_->start();
    poll();
}

void FireGpioMonitor::stop()
{
    if (pollTimer_) pollTimer_->stop();
    closeDevice();
    hasSample_ = false;
}

bool FireGpioMonitor::ensureOpen()
{
    if (fd_ >= 0) return true;

    fd_ = ::open(kFireGpioPath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ >= 0) {
        if (!openLogged_) {
            qInfo() << "[FIRE] GPIO monitor started:" << kFireGpioPath;
            openLogged_ = true;
        }
        return true;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs >= nextOpenLogMs_) {
        qWarning() << "[FIRE] open GPIO failed:" << kFireGpioPath
                   << "errno=" << errno;
        nextOpenLogMs_ = nowMs + kOpenFailureLogIntervalMs;
    }
    return false;
}

void FireGpioMonitor::closeDevice()
{
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
}

void FireGpioMonitor::poll()
{
    if (!ensureOpen()) return;

    // sysfs风格驱动通常要求每次从偏移0读取；不支持seek的字符设备会返回ESPIPE，
    // 此时继续使用当前描述符，若读到EOF则关闭并在下个周期重新打开。
    if (::lseek(fd_, 0, SEEK_SET) < 0 && errno != ESPIPE) {
        qWarning() << "[FIRE] GPIO seek failed, reopen on next poll, errno=" << errno;
        closeDevice();
        return;
    }

    char data[16] = {};
    const ssize_t size = ::read(fd_, data, sizeof(data));
    if (size == 0) {
        closeDevice();
        return;
    }
    if (size < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        qWarning() << "[FIRE] GPIO read failed, reopen on next poll, errno=" << errno;
        closeDevice();
        return;
    }

    bool active = false;
    if (!parseLevel(data, size, &active)) {
        qWarning() << "[FIRE] invalid GPIO payload, bytes="
                   << QByteArray(data, static_cast<int>(size)).toHex(' ');
        return;
    }
    if (hasSample_ && active == lastActive_) return;

    hasSample_ = true;
    lastActive_ = active;
    emit fireStateChanged(active);
}
