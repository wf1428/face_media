/**
 * @file icboard_worker.cpp
 * @brief IC 卡板串口接收与协议解码 worker的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "icboard_worker.h"

/** @brief 建立客户端帧、错误信号到 worker 槽的内部连接。 */
IcBoardWorker::IcBoardWorker(QObject *parent)
    : QObject(parent)
{
    connect(&m_client, &IcBoardClient::rawFrameReceived, this, &IcBoardWorker::onFrame);
    connect(&m_client, &IcBoardClient::portError, this, &IcBoardWorker::onErr);
}

/**
 * @brief 使用指定设备和波特率启动 IC 卡板串口。
 * @param portName 串口设备名称。
 * @param baudRate 波特率，默认 115200 bit/s。
 */
void IcBoardWorker::start(const QString &portName, int baudRate) {
    m_client.open(portName, baudRate);
}

/** @brief 关闭 IC 卡板串口。 */
void IcBoardWorker::stop() {
    m_client.close();
}

/** @brief 转发原始帧，并将同一帧解码为卡片事件。 */
void IcBoardWorker::onFrame(const QByteArray &frame) {
    emit rawFrame(frame);

    IcCardEvent ev = IcBoardDecoder::decode(frame);
    emit cardEvent(ev);
}

/** @brief 将底层错误不加改写地转发给上层。 */
void IcBoardWorker::onErr(const QString &err) {
    emit error(err);
}
