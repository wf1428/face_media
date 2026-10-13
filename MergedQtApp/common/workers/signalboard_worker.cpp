/**
 * @file signalboard_worker.cpp
 * @brief 信号板串口采集 worker的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include <QObject>
#include <QSerialPort>
#include <QTimer>

#include "signalboard_worker.h"
#include <QDateTime>

/** @return 当前毫秒时间戳，用于计算距离最近协议帧的时间。 */
static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

/**
 * @brief 保存串口参数并初始化串口、在线检测定时器。
 * @param portName 串口设备名称。
 * @param baud 串口波特率，单位 bit/s。
 * @param parent Qt 父对象。
 */
SignalBoardWorker::SignalBoardWorker(const QString& portName, int baud, QObject *parent)
    : QObject(parent), portName(portName), baud(baud)
{
    //配置串口 + 连接信号槽 + 配置定时器

    serial.setParent(this);
    onlineTimer.setParent(this);

    serial.setPortName(portName);
    serial.setBaudRate(baud);
    serial.setDataBits(QSerialPort::Data8);
    serial.setParity(QSerialPort::NoParity);
    serial.setStopBits(QSerialPort::OneStop);
    serial.setFlowControl(QSerialPort::NoFlowControl);

    connect(&serial, &QSerialPort::readyRead, this, &SignalBoardWorker::onReadyRead);
    connect(&serial, &QSerialPort::errorOccurred, this, &SignalBoardWorker::onError);

    onlineTimer.setInterval(500);
    connect(&onlineTimer, &QTimer::timeout, this, &SignalBoardWorker::onOnlineCheck);
}


// 打开串口并标记在线
void SignalBoardWorker::start()
{
    if (serial.isOpen()) return;

    // 只读打开失败就报错，并通知外部离线
    if (!serial.open(QIODevice::ReadOnly)) {
        emit onlineChanged(false, QString("open failed: %1").arg(serial.errorString()));
        return;
    }

    lastFrameMs = nowMs();
    onlineTimer.start();
    // 串口打开成功
    emit onlineChanged(true, "opened");
    online = true;
}


/** @brief 停止在线检测、关闭串口并在必要时通知离线。 */
void SignalBoardWorker::stop()
{
    //停止定时器、关闭串口、通知下线
    onlineTimer.stop();
    if (serial.isOpen()) serial.close();
    if (online) emit onlineChanged(false, "stopped");
    online = false;
}

//读取数据 -> 分帧 -> 解码 -> 发状态
void SignalBoardWorker::onReadyRead()
{
    QByteArray chunk = serial.readAll();

    auto frames = splitter.push(chunk);
    for (const auto& f : frames) {
        //每收到一帧就刷新 lastFrameMs
        lastFrameMs = nowMs();

        //解码并发出状态
        auto st = SignalBoardDecoder::decode(f);
        emit stateReceived(st);
    }
}

/** @brief 根据最近有效帧时间戳检测在线状态变化。 */
void SignalBoardWorker::onOnlineCheck()
{
    // 定时器每 500 ms 检查一次；连续 3000 ms 未收到完整帧才判离线，容忍短时串口抖动。
    const qint64 diff = nowMs() - lastFrameMs;
    bool nowOnline = (diff <= 3000);

    //只有状态发生变化才 emit，避免刷屏
    if (nowOnline != online) {
        online = nowOnline;
        emit onlineChanged(online, online ? "alive" : "timeout");
    }
}

// 串口报错处理
void SignalBoardWorker::onError(QSerialPort::SerialPortError e)
{
    if (e == QSerialPort::NoError) return;
    emit onlineChanged(false, QString("serial error: %1").arg(serial.errorString()));
}
