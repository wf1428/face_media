/**
 * @file key_service.cpp
 * @brief 轮询 audio_key 驱动并把稳定的按下/释放序列转换为 Qt 信号的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "key_service.h"


#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

namespace {
constexpr int kNormalPollIntervalMs = 80;
constexpr int kMissingDeviceRetryMs = 2000;
}

/** @brief 创建按键轮询定时器并解析平台设备路径。 */
KeyService::KeyService(QObject *parent)
    : QObject(parent)
{
    m_pollTimer.setInterval(kNormalPollIntervalMs);   // 设备存在时 80ms 轮询
    connect(&m_pollTimer, &QTimer::timeout,
            this, &KeyService::pollDevice);
}

/** @brief 销毁前停止轮询并关闭设备文件描述符。 */
KeyService::~KeyService()
{
    stop();
}

/** @brief 启动按键设备轮询。 */
void KeyService::start()
{
    if (!m_pollTimer.isActive()) {
        m_waitRelease = false;
        m_lastValue = 3;
        m_pollTimer.start();
    }
}

/** @brief 停止轮询并关闭设备。 */
void KeyService::stop()
{
    m_pollTimer.stop();
    closeDevice();
}

/** @return 设备已打开或本次打开成功时返回 true。 */
bool KeyService::openDeviceIfNeeded()
{
    if (m_fd >= 0) {
        return true;
    }

    m_fd = ::open(m_devPath.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (m_fd < 0) {
        const int openErrno = errno;

        // 当前设备尚未安装按键驱动时，/dev 节点不存在是预期状态。
        // 不再每 80ms 刷屏，同时把重试周期降低到 2 秒，避免无意义的 open 系统调用。
        if (openErrno == ENOENT || openErrno == ENODEV) {
            if (m_pollTimer.interval() != kMissingDeviceRetryMs) {
                m_pollTimer.setInterval(kMissingDeviceRetryMs);
            }
            return false;
        }

        emit logMessage(QString("打开按键设备失败: %1, err=%2")
                        .arg(m_devPath, QString::fromLocal8Bit(strerror(openErrno))));
        return false;
    }

    if (m_pollTimer.interval() != kNormalPollIntervalMs) {
        m_pollTimer.setInterval(kNormalPollIntervalMs);
    }
    emit logMessage(QString("按键设备已打开: %1").arg(m_devPath));
    return true;
}

/** @brief 关闭有效文件描述符并恢复未打开状态。 */
void KeyService::closeDevice()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

/** @brief 非阻塞读取 audio_key 原始值并处理按下/释放边沿。 */
void KeyService::pollDevice()
{
    if (!openDeviceIfNeeded()) {
        return;
    }

    int value = -1;
    errno = 0;
    const ssize_t n = ::read(m_fd, &value, sizeof(value));

    // 兼容当前驱动 bug：
    // 驱动成功 copy_to_user 后返回 0，而不是 sizeof(value)
    if (n < 0) {
        emit logMessage(QString("读取按键设备失败: err=%1")
                        .arg(QString::fromLocal8Bit(strerror(errno))));
        closeDevice();
        return;
    }

    if (value < 0 || value > 3) {
        return;
    }

    if (value != m_lastValue) {
        emit logMessage(QString("audio_key value=%1").arg(value));
        m_lastValue = value;
    }

    handleKeyValue(value);
}

/** @brief 将驱动位值转换为 KEY3/KEY4 单次按下信号。 */
void KeyService::handleKeyValue(int value)
{
    // 空闲态：重新允许下一次触发
    if (value == 3) {
        m_waitRelease = false;
        return;
    }

    // 未释放前不重复触发
    if (m_waitRelease) {
        return;
    }

    m_waitRelease = true;

    // KEY1_IO -> 逻辑 KEY3   查询卡号注册
    // KEY2_IO -> 逻辑 KEY4   主板重启
    if (value == 2) {
        emit logMessage("按键触发: KEY3");
        emit key3Pressed();
        return;
    }

    if (value == 1) {
        emit logMessage("按键触发: KEY4");
        emit key4Pressed();
        return;
    }

    // value == 0，两个都按下，先忽略
    emit logMessage("硬按键触发: value=0，忽略");
}
