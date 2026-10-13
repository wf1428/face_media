/**
 * @file serial_init.h
 * @brief 接收并拆分二维码、刷卡器和在线二维码串口报文。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SERIAL_INIT_H
#define SERIAL_INIT_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QRegularExpression>
#include "platform/rk3566_platform.h"


/**
 * @brief 二维码扫描器串口报文接收器。
 *
 * 在连续 UTF-8/ASCII 字节流中按正则识别楼层码、时间码和在线 JSON 码，
 * 丢弃匹配前噪声，并保留尚未匹配的尾部供下次读取继续解析。
 */
class QrSerialReceiver : public QObject {
    Q_OBJECT
public:
    /** @brief 建立串口 readyRead 到内部解析槽的连接。 */
    explicit QrSerialReceiver(QObject* parent=nullptr);

    /** @brief 按指定串口参数以只读方式打开二维码设备。 */
    bool open(const QString& dev = Rk3566Platform::qrDevice(),
              int baud = 9600,
              QSerialPort::DataBits db = QSerialPort::Data8,
              QSerialPort::Parity par = QSerialPort::NoParity,
              QSerialPort::StopBits sb = QSerialPort::OneStop);

    /** @brief 关闭串口并清空未解析缓冲区。 */
    void close();

    /** @brief 仅清空未解析缓冲区，不改变串口状态。 */
    void clearBuffer();

signals:
    /** @brief 输出一条完整楼层码、时间码或在线 JSON 码。 */
    void qrFrameReceived(const QString& frame);

    /** @brief 在解析前原样输出本次串口读取的字节。 */
    void rawBytesReceived(const QByteArray& bytes);

private slots:
    /** @brief 读取数据、限制缓存大小并循环提取最早出现的支持报文。 */
    void onReadyRead();

private:
    QSerialPort serial_;             /**< 二维码扫描器只读串口。 */
    QByteArray buf_;                 /**< 跨 readyRead 保留的未匹配 UTF-8/ASCII 数据。 */
    const int kMaxBuf = 2048;        /**< 异常无匹配数据的最大缓存，单位字节。 */
};

#endif // SERIAL_INIT_H
