/**
 * @file mqttipcclient.cpp
 * @brief 与本机 mqttd 通过 QLocalSocket 交换换行分隔 JSON 帧的客户端的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "mqttipcclient.h"
#include <QJsonDocument>

/**
 * @brief 构造一个本地 IPC 客户端对象。
 *
 * 初始化 QLocalSocket，并建立底层 socket 信号与本类槽函数之间的连接。
 *
 * @param parent 父对象指针。
 */
MqttIpcClient::MqttIpcClient(QObject *parent)
    : QObject(parent)
{
    // 创建本地 IPC socket，由 QObject 父子关系自动管理生命周期
    socket = new QLocalSocket(this);

    // 底层 socket 连接成功
    connect(socket, &QLocalSocket::connected,
            this, &MqttIpcClient::onSocketConnected);

    // 底层 socket 断开连接
    connect(socket, &QLocalSocket::disconnected,
            this, &MqttIpcClient::onSocketDisconnected);

    // 底层 socket 有新数据可读
    connect(socket, &QLocalSocket::readyRead,
            this, &MqttIpcClient::onSocketReadyRead);

    // 底层 socket 发生错误
    connect(socket,
            QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
            this,
            &MqttIpcClient::onSocketError);
}

/**
 * @brief 析构函数。
 *
 * 若当前 socket 仍处于连接状态，则在对象销毁前尝试主动断开连接。
 */
MqttIpcClient::~MqttIpcClient()
{
    if (socket && socket->state() != QLocalSocket::UnconnectedState) {
        socket->disconnectFromServer();
        socket->waitForDisconnected(200);
    }
}

/**
 * @brief 连接到指定的本地 IPC 服务端。
 *
 * 连接前会对路径做去首尾空白处理，并根据当前 socket 状态决定是否需要发起新连接。
 *
 * @param path 本地 socket 路径或服务名。
 */
void MqttIpcClient::connectToServer(const QString &path)
{
    serverPath = path.trimmed();

    if (serverPath.isEmpty()) {
        emit errorOccurred(QStringLiteral("socket 路径为空"));
        return;
    }

    const auto st = socket->state();

    if (st == QLocalSocket::ConnectedState) {
        emit connected();
        return;
    }

    if (st == QLocalSocket::ConnectingState) {
        emit logMessage(QStringLiteral("IPC 正在连接中…"));
        return;
    }

    socket->abort();
    emit logMessage(QStringLiteral("连接 mqttd IPC：%1").arg(serverPath));
    socket->connectToServer(serverPath);
}

/**
 * @brief 主动断开与服务端的连接。
 */
void MqttIpcClient::disconnectFromServer()
{
    if (!socket) return;

    emit logMessage(QStringLiteral("断开 mqttd IPC"));
    socket->disconnectFromServer();
}

/**
 * @brief 判断当前是否已连接到服务端。
 *
 * @return true 当前 socket 处于已连接状态。
 * @return false 当前未连接。
 */
bool MqttIpcClient::isConnected() const
{
    return socket && socket->state() == QLocalSocket::ConnectedState;
}

/**
 * @brief 获取当前 socket 状态。
 *
 * @return 当前 QLocalSocket 状态；若 socket 无效，则返回 UnconnectedState。
 */
QLocalSocket::LocalSocketState MqttIpcClient::state() const
{
    return socket ? socket->state() : QLocalSocket::UnconnectedState;
}

/**
 * @brief 发送一条 JSON 格式消息。
 *
 * 该函数会将 JSON 对象序列化为紧凑格式，并在末尾追加换行符后发送。
 *
 * @param obj 待发送的 JSON 对象。
 * @return true 发送成功。
 * @return false 发送失败。
 */
bool MqttIpcClient::sendJsonLine(const QJsonObject &obj)
{
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    line.append('\n');
    return sendRawLine(line);
}

/**
 * @brief 发送原始数据行。
 *
 * @param line 待发送的原始字节流。
 * @return true 写入成功。
 * @return false 当前未连接或写入失败。
 *
 * @note 本函数仅保证数据写入到底层 socket 缓冲区，不保证对端已完成处理。
 */
bool MqttIpcClient::sendRawLine(const QByteArray &line)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) {
        emit errorOccurred(QStringLiteral("IPC 未连接"));
        return false;
    }

    qint64 n = socket->write(line);
    socket->flush();

    if (n < 0) {
        emit errorOccurred(QStringLiteral("IPC write 失败：%1").arg(socket->errorString()));
        return false;
    }

    return true;
}

/**
 * @brief 底层 socket 连接成功时的槽函数。
 *
 * 连接成功后向上层转发 connected() 信号。
 */
void MqttIpcClient::onSocketConnected()
{
    emit connected();
}

/**
 * @brief 底层 socket 断开连接时的槽函数。
 *
 * 断开连接后向上层转发 disconnected() 信号。
 */
void MqttIpcClient::onSocketDisconnected()
{
    emit disconnected();
}

/**
 * @brief 底层 socket 数据可读时的槽函数。
 *
 * 将新到达的数据追加到接收缓冲区，并循环提取完整帧。
 * 提取成功后，通过 frameReceived() 信号将帧数据交给上层处理。
 *
 * 为防止异常输入导致死循环或缓冲区无限增长，本函数包含以下保护措施：
 * - 单次调用最多提取 1000 帧；
 * - 当 readBuffer 超过 16 MB 时主动清空，以容纳 Base64 人脸消息。
 */
void MqttIpcClient::onSocketReadyRead()
{
    // 将本次读取到的所有数据追加到缓冲区，用于处理半包/粘包
    readBuffer.append(socket->readAll());

    QByteArray frame;
    int guard = 0;

    // 反复提取完整帧，直到缓冲区中不再存在可解析的完整数据
    while (extractOneFrame(readBuffer, frame)) {
        if (!frame.isEmpty()) {
             emit frameReceived(frame);
        }

        // 防御性保护：避免异常输入导致无限循环
        if (++guard > 1000) {
            emit errorOccurred("提取帧次数过多，输入流可能异常");
            break;
        }
    }

    // 防御性保护：避免长时间无法完成组帧导致缓冲区持续膨胀
    // FACE 删除消息可能携带 Base64 图片，分包未收完整时需要更大的缓冲区。
    if (readBuffer.size() > 16 * 1024 * 1024) {
        emit errorOccurred("readBuffer 过大，已清空");
        readBuffer.clear();
    }
}

/**
 * @brief 底层 socket 错误处理槽函数。
 *
 * @param err 底层 socket 错误码。
 */
void MqttIpcClient::onSocketError(QLocalSocket::LocalSocketError err)
{
    Q_UNUSED(err);
    emit errorOccurred(socket->errorString());
}

/**
 * @brief 从接收缓冲区中提取一条完整帧。
 *
 * 帧定义如下：
 * - 若当前缓冲区以 '[' 开头，则按日志行处理，以 '\n' 为结束；
 * - 否则查找第一个 '{'，并基于大括号配对规则提取完整 JSON；
 * - JSON 前允许存在 topic 或其他前缀，提取结果包含此前缀。
 *
 * @param buf 输入输出缓冲区。成功提取后，会移除对应帧数据。
 * @param outFrame 输出参数，返回提取到的完整帧内容。
 * @return true 成功提取到一条完整帧。
 * @return false 当前数据不足以构成完整帧，需要等待更多输入。
 *
 * @note 该函数会正确处理 JSON 字符串中的转义字符以及字符串内部的大括号。
 */
bool MqttIpcClient::extractOneFrame(QByteArray& buf, QByteArray& outFrame)
{
    outFrame.clear();
    if (buf.isEmpty()) return false;

    // 跳过前导空行，避免影响后续帧识别
    while (!buf.isEmpty() && (buf[0] == '\n' || buf[0] == '\r')) {
        buf.remove(0, 1);
    }
    if (buf.isEmpty()) return false;

    // 1) 日志行：以 '\n' 作为一帧结束标记
    if (buf[0] == '[') {
        int nl = buf.indexOf('\n');
        if (nl < 0) return false; // 当前日志行尚未接收完整

        outFrame = buf.left(nl);
        buf.remove(0, nl + 1);

        // 去除可能存在的 '\r'
        if (!outFrame.isEmpty() && outFrame.endsWith('\r')) outFrame.chop(1);
        return true;
    }

    // 2) 非日志数据：查找 JSON 起始位置
    int lb = buf.indexOf('{');
    if (lb < 0) {
        // 尚未出现 JSON 起始符，等待更多数据
        return false;
    }

    // 从首个 '{' 开始进行大括号配对，支持字符串与转义处理
    bool inString = false;
    bool escape = false;
    int depth = 0;
    int endPos = -1;

    for (int i = lb; i < buf.size(); ++i) {
        char c = buf[i];

        if (escape) {
            // 若上一字符为转义符，则当前字符不参与语义判断
            escape = false;
            continue;
        }

        if (c == '\\') {
            // 仅字符串内部的反斜杠才表示转义
            if (inString) escape = true;
            continue;
        }

        if (c == '"') {
            // 双引号用于切换字符串状态
            inString = !inString;
            continue;
        }

        if (inString) continue;

        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                endPos = i;
                break;
            }
        }
    }

    if (endPos < 0) {
        // JSON 尚未接收完整
        return false;
    }

    // 从缓冲区起始位置到 JSON 结束位置整体作为一帧，保留可能存在的前缀
    outFrame = buf.left(endPos + 1);
    buf.remove(0, endPos + 1);

    // 去除帧尾部可能残留的换行符
    while (!outFrame.isEmpty() && (outFrame.endsWith('\r') || outFrame.endsWith('\n'))) {
        outFrame.chop(1);
    }
    return true;
}
