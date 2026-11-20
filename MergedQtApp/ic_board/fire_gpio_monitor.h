/**
 * @file fire_gpio_monitor.h
 * @brief 在非 UI 线程监视单路消防 GPIO 设备节点。
 * @details 设备节点固定为 /dev/fire_gpio；高电平进入消防、低电平退出消防。
 *          监视器只产生稳定的电平变化通知，不直接发送 RS485 或修改数据库。
 * @author Du lin
 * @date 2026-11-19
 */

#ifndef FIRE_GPIO_MONITOR_H
#define FIRE_GPIO_MONITOR_H

#include <QObject>
#include <QtGlobal>

class QTimer;

/**
 * @brief 以非阻塞轮询方式读取消防 GPIO。
 * @details 对象必须创建并运行在 IC 工作线程。驱动可返回文本 0/1 或单字节
 *          0/1；设备暂不可用时按间隔重新打开，不阻塞 UI 线程。
 */
class FireGpioMonitor final : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建监视器；定时器在 start() 时才启动。 */
    explicit FireGpioMonitor(QObject *parent = nullptr);

    /** @brief 停止轮询并关闭设备文件描述符。 */
    ~FireGpioMonitor() override;

    /** @brief 打开设备并开始以 20 ms 周期读取消防电平。 */
    void start();

    /** @brief 停止轮询并释放设备文件描述符。 */
    void stop();

signals:
    /**
     * @brief 首次有效采样或输入电平改变时发出。
     * @param[in] active true 为高电平消防触发，false 为低电平恢复正常。
     */
    void fireStateChanged(bool active);

private slots:
    /** @brief 在对象所属工作线程中执行一次非阻塞采样。 */
    void poll();

private:
    /** @return 设备已经打开或本次打开成功时返回 true。 */
    bool ensureOpen();

    /** @brief 关闭当前设备文件描述符；下次采样会重新打开。 */
    void closeDevice();

    QTimer *pollTimer_ = nullptr; /**< 工作线程轮询定时器，周期单位为毫秒。 */
    int fd_ = -1;                /**< /dev/fire_gpio 的非阻塞只读描述符。 */
    bool hasSample_ = false;     /**< 是否已经取得过可解析的有效电平。 */
    bool lastActive_ = false;    /**< 最近一次有效采样；true 表示高电平。 */
    bool openLogged_ = false;    /**< 防止无 seek 字符设备反复打开时重复打印启动日志。 */
    qint64 nextOpenLogMs_ = 0;   /**< 下一次允许记录打开失败日志的系统纪元毫秒值。 */
};

#endif // FIRE_GPIO_MONITOR_H
