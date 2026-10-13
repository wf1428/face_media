/**
 * @file signalboard_worker.h
 * @brief 信号板串口采集 worker。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SIGNALBOARD_WORKER_H
#define SIGNALBOARD_WORKER_H

#include <QObject>
#include <QSerialPort>
#include <QTimer>

#include "common/protocols/signalboard_splitter.h"
#include "common/protocols/signalboard_decoder.h"

/**
 * @brief 信号板串口采集 worker。
 *
 * 在所属线程中打开只读串口、拆分并解码协议帧，同时通过周期检查判断信号板是否在线。
 */
class SignalBoardWorker : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 保存串口参数并初始化串口、在线检测定时器。
     * @param portName 串口设备名称。
     * @param baud 串口波特率，单位 bit/s。
     * @param parent Qt 父对象。
     */
    explicit SignalBoardWorker(const QString& portName, int baud, QObject *parent=nullptr);

signals:
    /** @brief 输出一帧协议数据解码得到的信号板状态。 */
    void stateReceived(SignalBoardState state);

    /** @brief 仅在打开结果或在线状态变化时输出状态及原因。 */
    void onlineChanged(bool online, QString reason);

public slots:
    /** @brief 以只读方式打开串口并启动在线检测定时器。 */
    void start();

    /** @brief 停止在线检测、关闭串口并在必要时通知离线。 */
    void stop();

private slots:
    /** @brief 读取当前串口数据，完成拆帧、解码和状态分发。 */
    void onReadyRead();

    /** @brief 将串口运行错误转换为离线状态通知。 */
    void onError(QSerialPort::SerialPortError e);

    /** @brief 根据最近有效帧时间戳检测在线状态变化。 */
    void onOnlineCheck();

private:
    QString portName; /**< 信号板串口设备名称。 */
    int baud = 9600;  /**< 串口波特率，单位 bit/s。 */

    QSerialPort serial;               /**< 只读信号板串口。 */
    SignalBoardSplitter splitter;     /**< 12 字节固定帧拆分器。 */

    QTimer onlineTimer;               /**< 每 500 ms 执行一次在线状态检查。 */
    qint64 lastFrameMs = 0;           /**< 最近完整帧到达时间，单位 ms。 */
    bool online = false;              /**< 最近一次已对外公布的在线状态。 */
};

#endif // SIGNALBOARD_WORKER_H
