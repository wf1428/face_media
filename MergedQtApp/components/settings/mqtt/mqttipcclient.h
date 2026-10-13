/**
 * @file mqttipcclient.h
 * @brief 与本机 mqttd 通过 QLocalSocket 交换换行分隔 JSON 帧的客户端。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <QQueue>

/** @brief 与本机 mqttd 通过 QLocalSocket 交换换行分隔 JSON 帧的客户端。 */
class MqttIpcClient : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建本地套接字并连接状态、读取和错误信号。 */
    explicit MqttIpcClient(QObject *parent = nullptr);

    /** @brief 销毁前断开本地套接字。 */
    ~MqttIpcClient();

    /** @brief 连接指定 Unix 域套接字路径。 */
    void connectToServer(const QString &path);

    /** @brief 主动断开 mqttd。 */
    void disconnectFromServer();

    /** @return 套接字处于 ConnectedState 时返回 true。 */
    bool isConnected() const;

    /** @return 当前 QLocalSocket 状态。 */
    QLocalSocket::LocalSocketState state() const;

    /** @brief 将 JSON 紧凑编码并按单行帧发送。 */
    bool sendJsonLine(const QJsonObject &obj);

    /** @brief 发送原始帧并确保以换行结束。 */
    bool sendRawLine(const QByteArray &line);

signals:
    /** @brief 本地 IPC 已连接。 */
    void connected();
    /** @brief 本地 IPC 已断开。 */
    void disconnected();
    /** @brief 收到一个已去除换行符的完整 JSON 帧。 */
    void frameReceived(const QByteArray &frame);
    /** @brief 套接字错误转换后的文本。 */
    void errorOccurred(const QString &err);
    /** @brief IPC 连接和收发诊断日志。 */
    void logMessage(const QString &msg);

private slots:
    /** @brief 转发连接成功状态。 */
    void onSocketConnected();
    /** @brief 转发断开状态。 */
    void onSocketDisconnected();
    /** @brief 累积读取数据并逐行提取完整帧。 */
    void onSocketReadyRead();
    /** @brief 分批向上层投递已组装帧，避免一次突发长期占用事件循环。 */
    void processBufferedFrames();
    /** @brief 将 QLocalSocket 错误转换为业务错误文本。 */
    void onSocketError(QLocalSocket::LocalSocketError err);

private:
    /** @brief 从 buf 的 offset 位置提取一帧，只推进游标而不反复搬移缓冲区。 */
    static bool extractOneFrame(const QByteArray &buf,
                                int &offset,
                                QByteArray &outFrame);

private:
    QLocalSocket *socket = nullptr; /**< mqttd 本地套接字。 */
    QByteArray readBuffer;         /**< 跨 readyRead 保存的未完整行。 */
    QQueue<QByteArray> readyFrames; /**< 已完整组装、等待分批投递的帧。 */
    QString serverPath;            /**< 当前连接路径。 */
    bool frameDrainScheduled = false; /**< 是否已有下一轮分批投递任务。 */
    int maxFramesPerDrain = 4;      /**< 每轮最多同步投递的帧数。 */
};
