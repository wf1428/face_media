/**
 * @file mqttpacketparser.cpp
 * @brief 在后台兼容解析mqttd标准信封、字符串payload及旧格式消息。
 * @author Du lin
 * @date 2026-11-03
 */
#include "mqttpacketparser.h"
#include <QJsonDocument>
#include <QJsonParseError>
namespace {
/**
 * @brief 从格式损坏的 mqttd 消息中尝试提取 topic 与 payload 对象。
 *
 * 该函数用于处理类似如下“坏格式”消息：
 * @code
 * {"type":"msg",...,"payload":"{...}"}
 * @endcode
 *
 * 其中 payload 本应是标准 JSON 对象，但实际上被包装成了字符串，
 * 因此需要通过字符串扫描和大括号配对方式手动抠出 JSON 对象部分。
 *
 * @param raw        原始输入字节流。
 * @param topicOut   输出参数，返回提取到的 topic。
 * @param payloadOut 输出参数，返回提取到的 payload JSON 对象。
 * @param errOut     输出参数，返回失败原因。
 * @return true  提取并解析成功。
 * @return false 提取失败或 payload 解析失败。
 *
 * @note 该函数属于容错 fallback 逻辑，仅用于处理非标准格式输入。
 */
static bool extractPayloadObjectFromBrokenMsg(const QByteArray& raw,
                                              QString& topicOut,
                                              QJsonObject& payloadOut,
                                              QString& errOut)
{
    topicOut.clear();
    payloadOut = QJsonObject{};
    errOut.clear();

    // 1) 粗略提取 topic：查找 "topic":"..."
    const QByteArray key = "\"topic\":\"";
    int t0 = raw.indexOf(key);
    if (t0 >= 0) {
        t0 += key.size();
        int t1 = raw.indexOf('"', t0);
        if (t1 > t0) {
            topicOut = QString::fromUtf8(raw.mid(t0, t1 - t0));
        }
    }

    // 2) 定位 payload 字段起始位置："payload":"{...}"
    const QByteArray pkey = "\"payload\":\"";
    int p0 = raw.indexOf(pkey);
    if (p0 < 0) {
        errOut = "找不到 payload 字段";
        return false;
    }
    p0 += pkey.size();

    int lb = raw.indexOf('{', p0);
    if (lb < 0) {
        errOut = "payload 里找不到 '{'";
        return false;
    }

    // 3) 从 payload 内部第一个 '{' 开始进行 brace matching
    bool inString = false;
    bool escape = false;
    int depth = 0;
    int endPos = -1;

    for (int i = lb; i < raw.size(); ++i) {
        char c = raw[i];

        if (escape) { escape = false; continue; }
        if (c == '\\') { if (inString) escape = true; continue; }
        if (c == '"') { inString = !inString; continue; }
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
        errOut = "payload JSON 未闭合（找不到配对的 '}'）";
        return false;
    }

    QByteArray payloadJson = raw.mid(lb, endPos - lb + 1);

    // 4) 对截取出的 payload JSON 进行标准解析
    QJsonParseError e2;
    QJsonDocument d2 = QJsonDocument::fromJson(payloadJson, &e2);
    if (e2.error != QJsonParseError::NoError || !d2.isObject()) {
        errOut = QString("payload JSON 解析失败：%1").arg(e2.errorString());
        return false;
    }

    payloadOut = d2.object();
    return true;
}


/**
 * @brief 解析“topic + JSON”格式的一行消息。
 *
 * 输入格式示例：
 * @code
 * device/xxx/request {"method":"getPlayInfo",...}
 * @endcode
 *
 * 该函数会从原始字节流中提取 topic 部分和后续 JSON payload。
 *
 * @param raw        原始输入字节流。
 * @param topicOut   输出参数，返回解析出的 topic。
 * @param payloadOut 输出参数，返回解析出的 JSON 对象。
 * @return true  成功解析出 topic 与 payload。
 * @return false 输入格式不合法或 JSON 解析失败。
 */
static bool parseTopicJsonLine(const QByteArray &raw,
                               QString &topicOut,
                               QJsonObject &payloadOut)
{
    topicOut.clear();
    payloadOut = QJsonObject{};

    int lb = raw.indexOf('{');
    int rb = raw.lastIndexOf('}');
    if (lb < 0 || rb <= lb) {
        return false;
    }

    topicOut = QString::fromUtf8(raw.left(lb).trimmed());
    if (topicOut.isEmpty()) {
        return false;
    }

    const QByteArray payloadJson = raw.mid(lb, rb - lb + 1).trimmed();

    QJsonParseError e;
    QJsonDocument doc = QJsonDocument::fromJson(payloadJson, &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    payloadOut = doc.object();
    return true;
}



}
MqttParsedPacket parseMqttPacket(const QByteArray &input)
{
    MqttParsedPacket result;const auto raw=input.trimmed();
    result.display=raw.size()>16384 ? QStringLiteral("RX: large MQTT frame, bytes=%1").arg(raw.size()) : QStringLiteral("RX: %1").arg(QString::fromUtf8(raw));
    if(raw.isEmpty() || raw.startsWith('[')) return result;
    QString topic,error;QJsonObject payload;
    if(!raw.startsWith('{')) {
        if(!parseTopicJsonLine(raw,topic,payload)) { result.error=QStringLiteral("不是有效的 topic+JSON 行");return result; }
        result.object={{"type","msg"},{"topic",topic},{"payload",payload}};return result;
    }
    QJsonParseError failure;const auto document=QJsonDocument::fromJson(raw,&failure);
    if(failure.error!=QJsonParseError::NoError || !document.isObject()) {
        if(extractPayloadObjectFromBrokenMsg(raw,topic,payload,error)) result.object={{"type","msg"},{"topic",topic},{"payload",payload}};
        else result.error=QStringLiteral("JSON 解析失败：%1；fallback也失败：%2").arg(failure.errorString(),error);
        return result;
    }
    result.object=document.object();
    if(result.object.value("type").toString()!=QStringLiteral("msg")) return result;
    const auto value=result.object.value("payload");
    if(value.isObject()) return result;
    if(!value.isString()) { result.error=QStringLiteral("msg payload 不是 object/string，已忽略");result.object={};return result; }
    const auto text=value.toString().trimmed();
    if(!text.startsWith('{') || !text.endsWith('}')) { result.error=QStringLiteral("msg payload 不是 JSON object 字符串，已忽略");result.object={};return result; }
    const auto nested=QJsonDocument::fromJson(text.toUtf8(),&failure);
    if(failure.error!=QJsonParseError::NoError || !nested.isObject()) {
        result.error=QStringLiteral("msg payload 二次解析失败：%1").arg(failure.errorString());result.object={};return result;
    }
    result.object.insert("payload",nested.object());return result;
}
