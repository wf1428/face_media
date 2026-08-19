/**
 * @file ic_offline.cpp
 * @brief 刷卡器串口收发及 103/115 字节卡片帧拆分器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_offline.h"

#include <QDebug>
#include "common/debug/probe_log.h"

static constexpr int kLenVisitor = 103;
static constexpr int kLenMulti   = 115;


/** @brief 建立串口读取、错误和 115 字节等待定时器的内部连接。 */
CardSerial2Reader::CardSerial2Reader(QObject *parent)
    : QObject(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &CardSerial2Reader::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &CardSerial2Reader::onPortError);

    m_wait115Timer.setSingleShot(true);
    connect(&m_wait115Timer, &QTimer::timeout, this, &CardSerial2Reader::onWait115Timeout);
}

/** @brief 115 字节等待超时后，将当前 103 字节前缀按访客卡帧处理。 */
void CardSerial2Reader::onWait115Timeout()
{
    // 只有已积累 103 字节且仍在等待 115 字节补全时，超时才有判定意义。
    if (!m_waitingFor115) return;

    m_waitingFor115 = false;

    // 超时后仍只有 103~114 字节，按协议把前 103 字节判为访客卡帧。
    if (m_buf.size() >= kLenVisitor && m_buf.size() < kLenMulti) {
        // 再次确认头部位置，避免前面又混入脏字节
        int idx = findHeader();

        if (idx < 0) {


            if (m_buf.size() > 1) m_buf = m_buf.right(1);


            return;
        }
        if (idx > 0) m_buf.remove(0, idx);


        if (m_buf.size() < kLenVisitor) return;


        QByteArray frame = m_buf.left(kLenVisitor);
        m_buf.remove(0, kLenVisitor);

        CardType type = classifyFrame(frame); // 这里确保能识别 103
        if (type == CardType::Unknown) {

            // 可以直接 emit Visitor
            emit errorOccured("[CardSerial2] got 103 bytes but frame invalid");
        } else {

            emit cardFrame(type, frame);
        }

        // 吐完继续解析缓冲区里后续数据
        parseBuffer();
    }


}

/** @brief 销毁前关闭串口。 */
CardSerial2Reader::~CardSerial2Reader()
{
    stop();
}

/** @brief 以 8N1、无流控方式打开刷卡器读写串口。 */
bool CardSerial2Reader::start(const QString& portName, int baudRate)
{
    stop();

    m_port.setPortName(portName);
    m_port.setBaudRate(baudRate);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        emit errorOccured(QString("[CardSerial2] open failed: %1").arg(m_port.errorString()));
        return false;
    }

    reset();
    return true;
}

/** @brief 关闭已打开的刷卡器串口。 */
void CardSerial2Reader::stop()
{
    if (m_port.isOpen()) {
        m_port.close();
    }
}

/** @return 刷卡器串口已打开时返回 true。 */
bool CardSerial2Reader::isRunning() const
{
    return m_port.isOpen();
}

/** @brief 清空尚未解析的串口接收缓存。 */
void CardSerial2Reader::reset()
{
    m_buf.clear();
}

/** @brief 读取串口数据并追加到解析缓冲区。 */
void CardSerial2Reader::onReadyRead()
{
    const QByteArray data = m_port.readAll();
    if (data.isEmpty()) {
//        PROBEQ("CARD-RX onReadyRead got empty data");
        return;
    }

//    PROBEQ(QString("CARD-RX raw len=%1 hex=%2")
//               .arg(data.size())
//               .arg(QString::fromLatin1(data.toHex(' '))));

    m_buf.append(data);

//    PROBEQ(QString("CARD-RX buffer-appended total=%1 hex=%2")
//               .arg(m_buf.size())
//               .arg(QString::fromLatin1(m_buf.toHex(' '))));

    parseBuffer();
}

/** @brief 忽略 NoError，其余串口错误统一上报。 */
void CardSerial2Reader::onPortError(QSerialPort::SerialPortError err)
{
    if (err == QSerialPort::NoError) return;

    // 有些平台会频繁抛 ResourceError
    emit errorOccured(QString("[CardSerial2] serial error: %1").arg(m_port.errorString()));
}

/** @return 缓冲区内 "#@" 帧头位置，不存在时返回 -1。 */
int CardSerial2Reader::findHeader() const
{
    return m_buf.indexOf("#@"); // 0x23 0x40
}

/** @return 根据帧头和精确长度判断的 CardType。 */
CardSerial2Reader::CardType CardSerial2Reader::classifyFrame(const QByteArray& frame)
{
    if (frame.size() == kLenMulti && frame.size() >= 2 && frame[0] == '#' && frame[1] == '@')
        return CardType::Multi115;
    if (frame.size() == kLenVisitor && frame.size() >= 2 && frame[0] == '#' && frame[1] == '@')
        return CardType::Visitor103;
    return CardType::Unknown;
}

/** @brief 从缓冲区循环提取完整帧或进入 115 字节等待状态。 */
void CardSerial2Reader::parseBuffer()
{
    while (true) {
        int idx = findHeader();
        if (idx < 0) {
            if (m_buf.size() > 1) m_buf = m_buf.right(1);
            return;
        }
        if (idx > 0) m_buf.remove(0, idx);

        if (m_buf.size() < 2) return;

        // 1) 115：（多层卡）
        if (m_buf.size() >= kLenMulti) {
            // 已经够 115 了，说明无需再等 115
            if (m_waitingFor115) {
                m_waitingFor115 = false;
                m_wait115Timer.stop();
            }

            QByteArray frame = m_buf.left(kLenMulti);
            m_buf.remove(0, kLenMulti);

            CardType type = classifyFrame(frame);
            if (type != CardType::Unknown) emit cardFrame(type, frame);
            else emit errorOccured("[CardSerial2] got 115 bytes but header invalid");

            continue;
        }

        // 103 字节也可能只是 115 字节帧的首个串口分包，短暂等待剩余 12 字节。
        if (m_buf.size() >= kLenVisitor && m_buf.size() < kLenMulti) {


            if (!m_waitingFor115) {
                m_waitingFor115 = true;
                // 等 30~50ms, 避免受串口分包延迟
                m_wait115Timer.start(40);
            }
            return; // 先退出，等待更多数据/或超时再来解析
        }

        // 3) 还不够103：继续等
        return;
    }
}

/** @brief 预留的 36 字节 RS485 发送接口；当前实现不执行硬件操作。 */
void CardSerial2Reader::sendRs485Placeholder(const QByteArray& fcbbuf36)
{
    // 预留：后续 RS485 串口对象，在这里 write(fcbbuf36)
    Q_UNUSED(fcbbuf36);
    // qDebug() << "[RS485] placeholder send:" << fcbbuf36.toHex(' ');
}

/**
 * @brief 将完整命令写入刷卡器串口，每批写入等待最多 500 ms。
 * @param command 待发送的非空命令。
 * @param err 可选错误原因输出。
 * @return 全部字节发送完成时返回 true。
 */
bool CardSerial2Reader::sendCommand(const QByteArray &command, QString *err)
{
    if (err) err->clear();

    if (!m_port.isOpen()) {
        if (err) *err = QStringLiteral("刷卡串口未打开");
        return false;
    }

    if (command.isEmpty()) {
        if (err) *err = QStringLiteral("刷卡器命令为空");
        return false;
    }

    // 写入串口
    qint64 totalWritten = 0;
    while (totalWritten < command.size()) {
        const qint64 written = m_port.write(command.constData() + totalWritten,
                                            command.size() - totalWritten);
        if (written < 0) {
            if (err) {
                *err = QStringLiteral("串口写入失败: %1").arg(m_port.errorString());
            }
            return false;
        }

        if (written == 0) {
            if (!m_port.waitForBytesWritten(500)) {
                if (err) {
                    *err = QStringLiteral("串口等待发送完成超时: %1").arg(m_port.errorString());
                }
                return false;
            }
            continue;
        }

        totalWritten += written;

        if (!m_port.waitForBytesWritten(500)) {
            if (err) {
                *err = QStringLiteral("串口等待发送完成超时: %1").arg(m_port.errorString());
            }
            return false;
        }
    }

    return true;
}
