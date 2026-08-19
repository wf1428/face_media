/**
 * @file ic_offline.h
 * @brief 刷卡器串口收发及 103/115 字节卡片帧拆分器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_OFFLINE_H
#define IC_OFFLINE_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QTimer>
#include <QThread>

/**
 * @brief 刷卡器串口收发及 103/115 字节卡片帧拆分器。
 *
 * 由于 103 字节访客卡帧是 115 字节多层卡帧的长度前缀，收到 103~114 字节时
 * 短暂等待 40 ms，避免串口分包导致把 115 字节帧误判为访客卡帧。
 */
class CardSerial2Reader : public QObject
{
    Q_OBJECT
public:
    /** @brief 支持的卡片原始帧类型。 */
    enum class CardType {
        Unknown = 0,
        Visitor103,
        Multi115
    };
    Q_ENUM(CardType)

    /** @brief 建立串口读取、错误和 115 字节等待定时器的内部连接。 */
    explicit CardSerial2Reader(QObject *parent = nullptr);

    /** @brief 销毁前关闭串口。 */
    ~CardSerial2Reader();

    /** @brief 以 8N1、无流控方式打开刷卡器读写串口。 */
    bool start(const QString& portName, int baudRate = 9600);

    /** @brief 关闭已打开的刷卡器串口。 */
    void stop();

    /** @return 刷卡器串口已打开时返回 true。 */
    bool isRunning() const;

    /** @brief 清空尚未解析的串口接收缓存。 */
    void reset();

    /**
     * @brief 将完整命令写入刷卡器串口，每批写入等待最多 500 ms。
     * @param command 待发送的非空命令。
     * @param err 可选错误原因输出。
     * @return 全部字节发送完成时返回 true。
     */
    bool sendCommand(const QByteArray &command, QString *err = nullptr);

signals:
    /** @brief 输出以 "#@" 开头的 103 或 115 字节完整卡片帧。 */
    void cardFrame(CardSerial2Reader::CardType type, QByteArray rawFrame);

    /** @brief 输出拆帧或串口错误。 */
    void errorOccured(QString err);

private slots:
    /** @brief 读取串口数据并追加到解析缓冲区。 */
    void onReadyRead();

    /** @brief 忽略 NoError，其余串口错误统一上报。 */
    void onPortError(QSerialPort::SerialPortError err);

    /** @brief 115 字节等待超时后，将当前 103 字节前缀按访客卡帧处理。 */
    void onWait115Timeout();

private:
    QSerialPort m_port; /**< 刷卡器读写串口。 */
    QByteArray  m_buf;  /**< 跨 readyRead 保存的未消费字节。 */

    QTimer m_wait115Timer;             /**< 区分 103/115 字节帧的单次 40 ms 定时器。 */
    bool   m_waitingFor115 = false;    /**< 是否已有 103 字节并等待剩余 12 字节。 */

    /** @brief 从缓冲区循环提取完整帧或进入 115 字节等待状态。 */
    void parseBuffer();

    /** @return 缓冲区内 "#@" 帧头位置，不存在时返回 -1。 */
    int findHeader() const;

    /** @return 根据帧头和精确长度判断的 CardType。 */
    static CardType classifyFrame(const QByteArray& frame);

public:
    /** @brief 预留的 36 字节 RS485 发送接口；当前实现不执行硬件操作。 */
    void sendRs485Placeholder(const QByteArray& fcbbuf36);
};

#endif // IC_OFFLINE_H
