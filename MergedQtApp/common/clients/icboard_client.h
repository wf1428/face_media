/**
 * @file icboard_client.h
 * @brief IC 卡板只读串口客户端。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ICBOARD_CLIENT_H
#define ICBOARD_CLIENT_H

#include <QObject>
#include <QSerialPort>

#include "common/protocols/icboard_splitter.h"

/**
 * @brief IC 卡板只读串口客户端。
 *
 * 负责配置串口、接收原始字节流并交给 IcBoardSplitter 拆帧。客户端只读取设备数据，
 * 完整帧和串口错误通过 Qt 信号交给上层处理。
 */
class IcBoardClient : public QObject {
    Q_OBJECT
public:
    /** @brief 创建客户端并建立串口、拆帧器的内部信号连接。 */
    explicit IcBoardClient(QObject *parent = nullptr);

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
    void open(const QString &portName,
              int baudRate = 9600,
              QSerialPort::DataBits dataBits = QSerialPort::Data8,
              QSerialPort::Parity parity = QSerialPort::NoParity,
              QSerialPort::StopBits stopBits = QSerialPort::OneStop);

    /** @brief 关闭已打开的串口；未打开时不执行操作。 */
    void close();

    /** @return 串口当前已打开时返回 true。 */
    bool isOpen() const;

signals:
    /** @brief 转发拆帧器产生的完整 IC 卡板协议帧。 */
    void rawFrameReceived(const QByteArray &frame);

    /** @brief 上报串口打开或运行期间发生的错误。 */
    void portError(const QString &err);

private slots:
    /** @brief 读取当前可用串口数据并送入拆帧器。 */
    void onReadyRead();

    /** @brief 忽略 NoError，其余串口错误统一转换为 portError()。 */
    void onSerialError(QSerialPort::SerialPortError err);

private:
    QSerialPort m_port;       /**< IC 卡板只读串口。 */
    IcBoardSplitter m_splitter; /**< 将连续串口字节流切分为完整协议帧。 */
};

#endif // ICBOARD_CLIENT_H
