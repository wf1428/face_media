/**
 * @file rs485_driver_port.cpp
 * @brief RK3566 RS485 串口收发封装的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "rs485_driver_port.h"

#include <errno.h>
#include <linux/serial.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <QDebug>

#include "common/debug/probe_log.h"
#include "ic_board/rs485_floor_frame_builder.h"

/** @brief 初始化串口读取和接收空闲定时器的信号连接。 */
Rs485DriverPort::Rs485DriverPort(QObject* parent) : QObject(parent)
{
    connect(&serial_, &QSerialPort::readyRead, this, &Rs485DriverPort::onReadyRead);

    rxIdleTimer_.setSingleShot(true);
    connect(&rxIdleTimer_, &QTimer::timeout, this, &Rs485DriverPort::onRxIdleTimeout);
}

/**
 * @brief 打开 RS485 串口并配置收发方向控制。
 *
 * RK3566 使用内核 TIOCSRS485 配置 UART 自动方向；内核 ioctl 不可用时
 * 继续依赖设备树中已经启用的驱动或收发器自动方向。
 */
bool Rs485DriverPort::open(const QString& ttyDev, int baud,
                           QSerialPort::DataBits db, QSerialPort::Parity par,
                           QSerialPort::StopBits sb)
{
    close();

    serial_.setPortName(ttyDev);
    serial_.setBaudRate(baud);
    serial_.setDataBits(db);
    serial_.setParity(par);
    serial_.setStopBits(sb);
    serial_.setFlowControl(QSerialPort::NoFlowControl);

    if (!serial_.open(QIODevice::ReadWrite)) {
        emit errorOccured(QStringLiteral("打开 RS485 串口 %1 失败: %2")
                          .arg(ttyDev, serial_.errorString()));
        return false;
    }

    if (!configureKernelRs485()) {
        // 部分 RK3566 BSP 在设备树中已固定为自动方向，TIOCSRS485 可能返回
        // ENOTTY/EINVAL。此时继续工作，由驱动或收发器硬件自行控制方向。
        qWarning() << "[RS485] TIOCSRS485 unavailable; continue with hardware/driver auto direction"
                   << ttyDev << strerror(errno);
    }

    rxBuf_.clear();
    return true;
}

/** @brief 对串口文件描述符应用 TIOCSRS485 自动方向配置。 */
bool Rs485DriverPort::configureKernelRs485()
{
    const qintptr handle = serial_.handle();
    if (handle < 0) return false;

    serial_rs485 config{};
    config.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
#ifdef SER_RS485_RX_DURING_TX
    config.flags &= ~SER_RS485_RX_DURING_TX;
#endif
    config.delay_rts_before_send = 0;
    config.delay_rts_after_send = 0;

    return ::ioctl(static_cast<int>(handle), TIOCSRS485, &config) == 0;
}

/** @brief 停止接收定时器并关闭串口。 */
void Rs485DriverPort::close()
{
    rxIdleTimer_.stop();
    if (serial_.isOpen()) serial_.close();

    rxBuf_.clear();
}

/** @brief 设置接收字节间空闲成帧时间，最小 1 ms。 */
void Rs485DriverPort::setInterByteTimeoutMs(int ms)
{
    interByteTimeoutMs_ = qMax(1, ms);
}

/**
 * @brief 完整发送一帧，收发方向由内核 UART 驱动自动控制。
 * @param frame 待发送数据；空帧视为成功。
 * @param writeTimeoutMs 每次等待串口发送完成的超时，单位 ms。
 */
bool Rs485DriverPort::sendFrame(const QByteArray& frame, int writeTimeoutMs)
{
    if (!serial_.isOpen()) {
        emit errorOccured(QStringLiteral("RS485 串口未打开"));
        return false;
    }
    if (frame.isEmpty()) return true;

    // 所有业务最终都在串口出口执行一次模式位保护，覆盖事件桥直发、
    // 人脸/密码/刷卡直发、访客二维码、呼梯和定时控梯等路径。
    const QByteArray wireFrame =
            Rs485FloorFrameBuilder::applyElevatorModeOverlay(frame);

    qint64 total = 0;
    while (total < wireFrame.size()) {
        const qint64 written = serial_.write(wireFrame.constData() + total,
                                             wireFrame.size() - total);
        if (written < 0) {
            emit errorOccured(QStringLiteral("RS485 写入失败: %1").arg(serial_.errorString()));
            return false;
        }
        total += written;
        if (!serial_.waitForBytesWritten(writeTimeoutMs)) {
            emit errorOccured(QStringLiteral("RS485 等待发送完成超时: %1")
                              .arg(serial_.errorString()));
            return false;
        }
    }

    const qintptr handle = serial_.handle();
    if (handle >= 0) {
        ::tcdrain(static_cast<int>(handle));
    }

    PROBEQ(QStringLiteral("RS485 TX len=%1 hex=%2")
           .arg(wireFrame.size())
           .arg(QString::fromLatin1(wireFrame.toHex(' '))));
    return true;
}

/** @brief 追加当前可读字节并重启空闲成帧定时器。 */
void Rs485DriverPort::onReadyRead()
{
    const QByteArray bytes = serial_.readAll();
    if (bytes.isEmpty()) return;

    rxBuf_.append(bytes);
    rxIdleTimer_.start(interByteTimeoutMs_);
}

/** @brief 空闲超时后输出并清空接收缓冲区。 */
void Rs485DriverPort::onRxIdleTimeout()
{
    if (rxBuf_.isEmpty()) return;
    const QByteArray frame = rxBuf_;
    rxBuf_.clear();
    emit frameReceived(frame);
}
