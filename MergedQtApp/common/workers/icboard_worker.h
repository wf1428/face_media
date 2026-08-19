/**
 * @file icboard_worker.h
 * @brief IC 卡板串口接收与协议解码 worker。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ICBOARD_WORKER_H
#define ICBOARD_WORKER_H

#include <QObject>
#include "common/clients/icboard_client.h"
#include "common/protocols/icboard_decoder.h"

/**
 * @brief IC 卡板串口接收与协议解码 worker。
 *
 * 封装 IcBoardClient，将完整原始帧继续转发，并同时输出 IcBoardDecoder 的解析事件。
 */
class IcBoardWorker : public QObject {
    Q_OBJECT
public:
    /** @brief 建立客户端帧、错误信号到 worker 槽的内部连接。 */
    explicit IcBoardWorker(QObject *parent = nullptr);

    /**
     * @brief 使用指定设备和波特率启动 IC 卡板串口。
     * @param portName 串口设备名称。
     * @param baudRate 波特率，默认 115200 bit/s。
     */
    void start(const QString &portName, int baudRate = 115200);

    /** @brief 关闭 IC 卡板串口。 */
    void stop();

signals:
    /** @brief 输出由完整帧解析得到的卡片事件。 */
    void cardEvent(const IcCardEvent &event);

    /** @brief 原样输出完整帧，供日志、转发或故障分析使用。 */
    void rawFrame(const QByteArray &frame);

    /** @brief 转发客户端串口错误。 */
    void error(const QString &err);

private slots:
    /** @brief 转发原始帧，并将同一帧解码为卡片事件。 */
    void onFrame(const QByteArray &frame);

    /** @brief 将底层错误不加改写地转发给上层。 */
    void onErr(const QString &err);

private:
    IcBoardClient m_client; /**< 负责串口读取和基础拆帧的客户端。 */
};

#endif // ICBOARD_WORKER_H
