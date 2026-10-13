/**
 * @file icboard_client.cpp
 * @brief IC 卡板只读串口客户端的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "icboard_client.h"

/** @brief 创建客户端并建立串口、拆帧器的内部信号连接。 */
IcBoardClient::IcBoardClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &IcBoardClient::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &IcBoardClient::onSerialError);

    connect(&m_splitter, &IcBoardSplitter::frameReady, this, &IcBoardClient::rawFrameReceived);
}

/**
 * @brief 按指定串口参数打开 IC 卡板。
 *
 * 若串口已经打开，会先关闭旧连接再应用新参数；打开失败时通过 portError() 上报。
 *
 * @param portName 串口设备名称。
 * @param baudRate 波特率，默认 9600 bit/s。
 * @param dataBits 数据位数。
 * @param parity 校验方式。
 * @param stopBits 停止位数。
 */
void IcBoardClient::open(const QString &portName,
                         int baudRate,
                         QSerialPort::DataBits dataBits,
                         QSerialPort::Parity parity,
                         QSerialPort::StopBits stopBits)
{
    if (m_port.isOpen()) m_port.close();

    m_port.setPortName(portName);
    m_port.setBaudRate(baudRate);
    m_port.setDataBits(dataBits);
    m_port.setParity(parity);
    m_port.setStopBits(stopBits);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadOnly)) {
        emit portError(QString("ICBoard open failed: %1").arg(m_port.errorString()));
        return;
    }
}

/** @brief 关闭已打开的串口；未打开时不执行操作。 */
void IcBoardClient::close() {
    if (m_port.isOpen()) m_port.close();
}

/** @return 串口当前已打开时返回 true。 */
bool IcBoardClient::isOpen() const {
    return m_port.isOpen();
}

/** @brief 读取当前可用串口数据并送入拆帧器。 */
void IcBoardClient::onReadyRead() {
    const QByteArray data = m_port.readAll();
    /* 调试 */

    if (!data.isEmpty()) {
        m_splitter.feed(data);
    }
}

/** @brief 忽略 NoError，其余串口错误统一转换为 portError()。 */
void IcBoardClient::onSerialError(QSerialPort::SerialPortError err) {
    if (err == QSerialPort::NoError) return;
    emit portError(QString("ICBoard serial error: %1").arg(m_port.errorString()));
}
