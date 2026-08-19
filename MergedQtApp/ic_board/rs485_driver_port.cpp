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
#include <QFileInfo>

#include "common/debug/probe_log.h"

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
 * RK3566 优先使用内核 TIOCSRS485；仅当 dirDev 存在时沿用旧硬件的
 * 0/1 外部方向控制。内核 ioctl 不可用时继续依赖驱动或收发器自动方向。
 */
bool Rs485DriverPort::open(const QString& ttyDev, int baud, const QString& dirDev,
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

    // 兼容旧 T113 外部方向设备；RK3566 通常由 UART 驱动自动切换 DE/RE。
    externalDirection_ = !dirDev.trimmed().isEmpty() && QFileInfo::exists(dirDev);
    if (externalDirection_) {
        dirDev_.setFileName(dirDev);
        if (!dirDev_.open(QIODevice::WriteOnly)) {
            emit errorOccured(QStringLiteral("打开 RS485 方向设备 %1 失败: %2")
                              .arg(dirDev, dirDev_.errorString()));
            serial_.close();
            externalDirection_ = false;
            return false;
        }
        if (!setDirRx()) {
            emit errorOccured(QStringLiteral("RS485 方向设备切换到接收失败"));
            close();
            return false;
        }
    } else {
        kernelDirection_ = configureKernelRs485();
        if (!kernelDirection_) {
            // 部分 RK3566 BSP 在设备树中已固定为自动方向，TIOCSRS485 可能返回
            // ENOTTY/EINVAL。此时继续工作，由驱动或收发器硬件自行控制方向。
            qWarning() << "[RS485] TIOCSRS485 unavailable; continue with hardware/driver auto direction"
                       << ttyDev << strerror(errno);
        }
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

/** @brief 停止接收定时器、恢复接收方向并关闭设备。 */
void Rs485DriverPort::close()
{
    rxIdleTimer_.stop();
    if (dirDev_.isOpen()) {
        setDirRx();
        dirDev_.close();
    }
    if (serial_.isOpen()) serial_.close();

    externalDirection_ = false;
    kernelDirection_ = false;
    rxBuf_.clear();
}

/** @brief 设置接收字节间空闲成帧时间，最小 1 ms。 */
void Rs485DriverPort::setInterByteTimeoutMs(int ms)
{
    interByteTimeoutMs_ = qMax(1, ms);
}

/** @brief 在外部方向设备上写入 1，切换为发送。 */
bool Rs485DriverPort::setDirTx()
{
    if (!externalDirection_) return true;
    if (!dirDev_.isOpen()) return false;

    const char value = 1;
    const qint64 written = dirDev_.write(&value, 1);
    dirDev_.flush();
    ::fsync(dirDev_.handle());
    return written == 1;
}

/** @brief 在外部方向设备上写入 0，切换为接收。 */
bool Rs485DriverPort::setDirRx()
{
    if (!externalDirection_) return true;
    if (!dirDev_.isOpen()) return false;

    const char value = 0;
    const qint64 written = dirDev_.write(&value, 1);
    dirDev_.flush();
    return written == 1;
}

/**
 * @brief 完整发送一帧，并在外部方向模式下等待发送结束后切回接收。
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

    if (!setDirTx()) {
        emit errorOccured(QStringLiteral("RS485 切换发送方向失败"));
        return false;
    }

    qint64 total = 0;
    while (total < frame.size()) {
        const qint64 written = serial_.write(frame.constData() + total, frame.size() - total);
        if (written < 0) {
            emit errorOccured(QStringLiteral("RS485 写入失败: %1").arg(serial_.errorString()));
            setDirRx();
            return false;
        }
        total += written;
        if (!serial_.waitForBytesWritten(writeTimeoutMs)) {
            emit errorOccured(QStringLiteral("RS485 等待发送完成超时: %1")
                              .arg(serial_.errorString()));
            setDirRx();
            return false;
        }
    }

    const qintptr handle = serial_.handle();
    if (handle >= 0) {
        ::tcdrain(static_cast<int>(handle));
    }

    // 仅外部 GPIO/字符设备方向控制需要额外等待最后一个停止位。
    if (externalDirection_) {
        ::usleep(200);
        if (!setDirRx()) {
            emit errorOccured(QStringLiteral("RS485 切换接收方向失败"));
            return false;
        }
    }

    PROBEQ(QStringLiteral("RS485 TX len=%1 hex=%2")
           .arg(frame.size())
           .arg(QString::fromLatin1(frame.toHex(' '))));
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
