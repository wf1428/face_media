/**
 * @file key_service.h
 * @brief 轮询 audio_key 驱动并把稳定的按下/释放序列转换为 Qt 信号。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef KEY_SERVICE_H
#define KEY_SERVICE_H

#include <QObject>
#include <QTimer>
#include <QString>
#include "platform/rk3566_platform.h"

/**
 * @brief 轮询 audio_key 驱动并把稳定的按下/释放序列转换为 Qt 信号。
 *
 * m_waitRelease 防止按键保持低电平期间重复触发，设备临时不可用时由后续轮询重开。
 */
class KeyService : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建按键轮询定时器并解析平台设备路径。 */
    explicit KeyService(QObject *parent = nullptr);

    /** @brief 销毁前停止轮询并关闭设备文件描述符。 */
    ~KeyService();

    /** @brief 启动按键设备轮询。 */
    void start();

    /** @brief 停止轮询并关闭设备。 */
    void stop();

signals:
    /** @brief KEY3 产生一次完整按下边沿。 */
    void key3Pressed();
    /** @brief KEY4 产生一次完整按下边沿。 */
    void key4Pressed();
    /** @brief 输出设备打开或读取诊断日志。 */
    void logMessage(const QString &msg);

private slots:
    /** @brief 非阻塞读取 audio_key 原始值并处理按下/释放边沿。 */
    void pollDevice();

private:
    /** @return 设备已打开或本次打开成功时返回 true。 */
    bool openDeviceIfNeeded();

    /** @brief 关闭有效文件描述符并恢复未打开状态。 */
    void closeDevice();

    /** @brief 将驱动位值转换为 KEY3/KEY4 单次按下信号。 */
    void handleKeyValue(int value);

private:
    QTimer m_pollTimer;
    int m_fd = -1;

    // audio_key 驱动读出来的原始值：
    // value = gpio(KEY1_IO) + gpio(KEY2_IO) * 2
    // 常见情况：
    // 3 = 1 + 2 = 空闲（两个输入都为高）
    // 2 = KEY1_IO 被拉低
    // 1 = KEY2_IO 被拉低
    // 0 = 两个都低（同时按下/异常）
    int m_lastValue = 3;
    bool m_waitRelease = false;

    QString m_devPath = Rk3566Platform::keyDevice();
};

#endif // KEY_SERVICE_H
