/**
 * @file rs485_driver_port.h
 * @brief RK3566 RS485 串口收发封装。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef RS485_DRIVER_PORT_H
#define RS485_DRIVER_PORT_H

#include <QObject>
#include <QSerialPort>
#include <QFile>
#include <QTimer>
#include <QByteArray>

#include "platform/rk3566_platform.h"

/**
 * @brief RK3566 RS485 串口收发封装。
 *
 * 优先请求内核 TIOCSRS485 自动方向；配置了兼容方向设备时使用 0/1 外部控制。
 * 接收数据以“连续字节间空闲超时”聚合成帧，协议边界由上层解释。
 */
class Rs485DriverPort : public QObject {
    Q_OBJECT
public:
    /** @brief 初始化串口读取和接收空闲定时器的信号连接。 */
    explicit Rs485DriverPort(QObject* parent = nullptr);

    /**
     * @brief 打开 RS485 串口并配置收发方向控制。
     *
     * RK3566 优先使用内核 TIOCSRS485；仅当 dirDev 存在时沿用旧硬件的
     * 0/1 外部方向控制。内核 ioctl 不可用时继续依赖驱动或收发器自动方向。
     */
    bool open(const QString& ttyDev = Rk3566Platform::rs485Device(),
              int baud = 9600,
              const QString& dirDev = Rk3566Platform::rs485DirectionDevice(),
              QSerialPort::DataBits db = QSerialPort::Data8,
              QSerialPort::Parity par = QSerialPort::NoParity,
              QSerialPort::StopBits sb = QSerialPort::OneStop);

    /** @brief 停止接收定时器、恢复接收方向并关闭设备。 */
    void close();

    /**
     * @brief 完整发送一帧，并在外部方向模式下等待发送结束后切回接收。
     * @param frame 待发送数据；空帧视为成功。
     * @param writeTimeoutMs 每次等待串口发送完成的超时，单位 ms。
     */
    bool sendFrame(const QByteArray& frame, int writeTimeoutMs = 1000);

    /** @brief 设置接收字节间空闲成帧时间，最小 1 ms。 */
    void setInterByteTimeoutMs(int ms);

signals:
    /** @brief 连续数据静默达到配置时长后输出聚合帧。 */
    void frameReceived(const QByteArray& frame);

    /** @brief 输出串口、方向设备或发送错误。 */
    void errorOccured(const QString& err);

private slots:
    /** @brief 追加当前可读字节并重启空闲成帧定时器。 */
    void onReadyRead();

    /** @brief 空闲超时后输出并清空接收缓冲区。 */
    void onRxIdleTimeout();

private:
    /** @brief 对串口文件描述符应用 TIOCSRS485 自动方向配置。 */
    bool configureKernelRs485();

    /** @brief 在外部方向设备上写入 1，切换为发送。 */
    bool setDirTx();

    /** @brief 在外部方向设备上写入 0，切换为接收。 */
    bool setDirRx();

private:
    QSerialPort serial_;              /**< RS485 数据串口。 */
    QFile dirDev_;                    /**< 可选的旧硬件方向控制字符设备。 */
    QTimer rxIdleTimer_;              /**< 接收字节间空闲成帧单次定时器。 */
    QByteArray rxBuf_;                /**< 当前尚未因空闲超时输出的数据。 */
    int interByteTimeoutMs_ = 10;     /**< 字节间空闲阈值，单位 ms。 */
    bool externalDirection_ = false;  /**< 是否使用 dirDev_ 控制 DE/RE。 */
    bool kernelDirection_ = false;    /**< TIOCSRS485 配置是否成功。 */
};
#endif // RS485_DRIVER_PORT_H
