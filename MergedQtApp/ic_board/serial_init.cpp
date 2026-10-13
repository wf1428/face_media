/**
 * @file serial_init.cpp
 * @brief 接收并拆分二维码、刷卡器和在线二维码串口报文。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "serial_init.h"
#include <QTextCodec>

/** @brief 建立串口 readyRead 到内部解析槽的连接。 */
QrSerialReceiver::QrSerialReceiver(QObject* parent) : QObject(parent) {
    connect(&serial_, &QSerialPort::readyRead, this, &QrSerialReceiver::onReadyRead);
}

/** @brief 按指定串口参数以只读方式打开二维码设备。 */
bool QrSerialReceiver::open(const QString& dev, int baud,
                            QSerialPort::DataBits db,
                            QSerialPort::Parity par,
                            QSerialPort::StopBits sb) {
    serial_.setPortName(dev);
    serial_.setBaudRate(baud);
    serial_.setDataBits(db);
    serial_.setParity(par);
    serial_.setStopBits(sb);
    serial_.setFlowControl(QSerialPort::NoFlowControl);
    return serial_.open(QIODevice::ReadOnly);
}

/** @brief 关闭串口并清空未解析缓冲区。 */
void QrSerialReceiver::close() {
    if (serial_.isOpen()) serial_.close();
    buf_.clear();
}

/** @brief 仅清空未解析缓冲区，不改变串口状态。 */
void QrSerialReceiver::clearBuffer() {
    buf_.clear();
}

/** @brief 读取数据、限制缓存大小并循环提取最早出现的支持报文。 */
void QrSerialReceiver::onReadyRead() {
    QByteArray bytes = serial_.readAll();
    emit rawBytesReceived(bytes);

    buf_.append(bytes);
    if (buf_.size() > kMaxBuf) {
        // 丢弃前面部分，避免异常撑爆
        buf_ = buf_.right(kMaxBuf / 2);
    }

    // 用字符串做匹配（二维码内容是ASCII/UTF-8）
    QString s = QString::fromUtf8(buf_);

    // 1) 楼层二维码：32hex#32hex#8digits#
    static QRegularExpression reFloor(R"(([0-9a-fA-F]{32})#([0-9a-fA-F]{32})#(\d{8})#)");
    // 2) 时间二维码：先简单匹配到'#'结束
    static QRegularExpression reTime(R"((time:[^#]*#))");
    // 3) 在线二维码 JSON：{"qrCode":"..."}
    static QRegularExpression reOnlineJson(
                R"qrjson((\{\s*"qrCode"\s*:\s*"[^"]+"\s*\}))qrjson");

    while (true) {
        int posFloor = s.indexOf(reFloor);
        int posTime  = s.indexOf(reTime);
        int posJson  = s.indexOf(reOnlineJson);

        int pos = -1;
        int kind = 0; // 1 floor, 2 time, 3 json
        if (posFloor >= 0) { pos = posFloor; kind = 1; }
        if (posTime >= 0 && (pos < 0 || posTime < pos)) { pos = posTime; kind = 2; }
        if (posJson >= 0 && (pos < 0 || posJson < pos)) { pos = posJson; kind = 3; }

        if (pos < 0) break;

        // 丢掉匹配前噪声
        if (pos > 0) {
            s.remove(0, pos);
        }

        QRegularExpressionMatch m;
        if (kind == 1) m = reFloor.match(s);
        else if (kind == 2) m = reTime.match(s);
        else m = reOnlineJson.match(s);
        if (!m.hasMatch()) break;

        QString frame = m.captured(0);
        emit qrFrameReceived(frame);
        s.remove(0, m.capturedLength(0));
    }

    buf_ = s.toUtf8();
}
