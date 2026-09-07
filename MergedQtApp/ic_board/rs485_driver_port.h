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
#include <QTimer>
#include <QByteArray>

#include "platform/rk3566_platform.h"

/**
 * @brief RK3566 RS485 串口收发封装。
 *
 * 通过 TIOCSRS485 请求内核自动控制 UART RTS/DE 方向。
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
     * RK3566 使用内核 TIOCSRS485 配置 UART 自动方向；内核 ioctl 不可用时
     * 继续依赖设备树中已经启用的驱动或收发器自动方向。
     */
    bool open(const QString& ttyDev = Rk3566Platform::rs485Device(),
              int baud = 9600,
              QSerialPort::DataBits db = QSerialPort::Data8,
              QSerialPort::Parity par = QSerialPort::NoParity,
              QSerialPort::StopBits sb = QSerialPort::OneStop);

    /** @brief 停止接收定时器并关闭串口。 */
    void close();

    /**
     * @brief 完整发送一帧，收发方向由内核 UART 驱动自动控制。
     * @param frame 待发送数据；空帧视为成功。
     * @param writeTimeoutMs 每次等待串口发送完成的超时，单位 ms。
     */
    bool sendFrame(const QByteArray& frame, int writeTimeoutMs = 1000);

    /** @brief 设置接收字节间空闲成帧时间，最小 1 ms。 */
    void setInterByteTimeoutMs(int ms);

signals:
    /** @brief 连续数据静默达到配置时长后输出聚合帧。 */
    void frameReceived(const QByteArray& frame);

    /** @brief 输出串口或发送错误。 */
    void errorOccured(const QString& err);

private slots:
    /** @brief 追加当前可读字节并重启空闲成帧定时器。 */
    void onReadyRead();

    /** @brief 空闲超时后输出并清空接收缓冲区。 */
    void onRxIdleTimeout();

private:
    /** @brief 对串口文件描述符应用 TIOCSRS485 自动方向配置。 */
    bool configureKernelRs485();

    QSerialPort serial_;              /**< RS485 数据串口。 */
    QTimer rxIdleTimer_;              /**< 接收字节间空闲成帧单次定时器。 */
    QByteArray rxBuf_;                /**< 当前尚未因空闲超时输出的数据。 */
    int interByteTimeoutMs_ = 10;     /**< 字节间空闲阈值，单位 ms。 */
};
#endif // RS485_DRIVER_PORT_H
