/**
 * @file ic_mqtt_gateway.cpp
 * @brief MQTT 在线协议与本地 IC/二维码/RS485 事件之间的双向网关的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_mqtt_gateway.h"

#include <QDebug>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QtGlobal>

#include "ic_board/ic_offline_checker.h"
#include "ic_board/ic_event_bridge.h"
#include "ic_board/core_migration_audit/core_migration_sync.h"
#include "ic_board/rs485_floor_frame_builder.h"
#include "platform/rk3566_platform.h"

namespace {
/** @brief 按兼容字段优先级返回第一个非空字符串值。 */
static QString firstNonEmpty(const QJsonObject &obj, const QStringList &keys)
{
    for (const QString &k : keys) {
        const QJsonValue v = obj.value(k);
        if (v.isString()) {
            const QString s = v.toString().trimmed();
            if (!s.isEmpty()) return s;
        } else if (v.isDouble()) {
            return QString::number(v.toInt());
        }
    }
    return {};
}

/** @return JSON 对象的单行紧凑文本，供协议日志使用。 */
static QString jsonCompact(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

/** @brief 将业务楼层号映射为控制板位索引。 */
static int floorIndexFromNo(int floorNo)
{
    if (floorNo >= -8 && floorNo <= -1) {
        return floorNo + 8; // -8 -> 0, -1 -> 7
    }
    if (floorNo >= 1 && floorNo <= 120) {
        return floorNo + 7;     // 1 -> 8, 120 -> 127
    }
    return -1;
}


// aa:对应位清0,开放; bb:对应位置1,限行. OH:打开;CL:关闭
static QString normalizeTrafficCmd(const QString &raw, bool isOpen)
{
    const QString s = raw.trimmed();
    if (s.isEmpty()) return {};

    if (s.startsWith("OH", Qt::CaseInsensitive) ||
        s.startsWith("CL", Qt::CaseInsensitive) ||
        s.startsWith("aa", Qt::CaseInsensitive) ||
        s.startsWith("bb", Qt::CaseInsensitive)) {
        return s;
    }

    // 32位hex位图
    QRegularExpression reHex("^[0-9A-Fa-f]{32}$");
    if (reHex.match(s).hasMatch()) {
        return QString(isOpen ? "OH%1" : "CL%1").arg(s.toUpper());
    }

    // 单层楼层号：001 / -01 / 120
    bool ok = false;
    const int floorNo = s.toInt(&ok);
    if (ok && floorNo >= -8 && floorNo <= 120 && floorNo != 0) {
        QString floor3;
        if (floorNo < 0) {
            floor3 = QString("-%1").arg(qAbs(floorNo), 2, 10, QLatin1Char('0'));
        } else {
            floor3 = QString("%1").arg(floorNo, 3, 10, QLatin1Char('0'));
        }
        return QString(isOpen ? "aa%1" : "bb%1").arg(floor3);
    }

    return s;
}



/** @brief 解析并校验服务端开楼层参数。 */
static bool parseOpenFloorNo(const QString &raw, int *floorNo, QString *err)
{
    QString s = raw.trimmed();
    if (s.startsWith("#@")) {
        s = s.mid(2);
    }

    bool ok = false;
    const int n = s.toInt(&ok);
    if (!ok) {
        if (err) *err = QString("openFloor floor 非法: %1").arg(raw);
        return false;
    }
    if (n == 0 || n < -8 || n > 120) {
        if (err) *err = QString("openFloor floor 越界: %1").arg(n);
        return false;
    }
    if (floorNo) *floorNo = n;
    return true;
}

/** @brief 将交通梯控制命令映射为 STM32 兼容 RS485 载荷。 */
static bool applyTrafficCmdLikeStm32(const QString &cmd, QByteArray *rs485Payload, QString *err)
{
    QByteArray bits = DeviceConfigSync::floorFlags128Bits();
    const QByteArray buf = cmd.trimmed().toLatin1();

    if (buf.size() >= 5 &&
        ((buf[0] == 'a' && buf[1] == 'a') || (buf[0] == 'b' && buf[1] == 'b'))) {

        bool okNo = false;
        const int floorNo = QString::fromLatin1(buf.mid(2, 3)).toInt(&okNo);
        const int idx = floorIndexFromNo(floorNo);

        if (!okNo || idx < 0 || idx >= 128) {
            if (err) *err = QString("单层楼层号非法: %1").arg(QString::fromLatin1(buf.mid(2, 3)));
            return false;
        }

        // aa -> 清0, bb -> 置1
        bits[idx] = (buf[0] == 'b' && buf[1] == 'b') ? '1' : '0';

        if (!DeviceConfigSync::saveFloorFlags128Bits(bits)) {
            if (err) *err = "保存楼层开关位图失败";
            return false;
        }

        QByteArray out = buf.left(5);
        out.append('<');
        out.append('>');
        if (rs485Payload) *rs485Payload = out;
        return true;
    }

    if (buf.size() >= 34 &&
        ((buf[0] == 'C' && buf[1] == 'L') || (buf[0] == 'O' && buf[1] == 'H'))) {

        const bool isCL = (buf[0] == 'C' && buf[1] == 'L');
        const QByteArray hex = buf.mid(2, 32).toUpper();

        if (hex.size() != 32) {
            if (err) *err = "批量楼层控制hex长度不是32";
            return false;
        }

        QByteArray incomingBits;
        incomingBits.reserve(128);

        for (char c : hex) {
            switch (c) {
            case '0': incomingBits += "0000"; break;
            case '1': incomingBits += "0001"; break;
            case '2': incomingBits += "0010"; break;
            case '3': incomingBits += "0011"; break;
            case '4': incomingBits += "0100"; break;
            case '5': incomingBits += "0101"; break;
            case '6': incomingBits += "0110"; break;
            case '7': incomingBits += "0111"; break;
            case '8': incomingBits += "1000"; break;
            case '9': incomingBits += "1001"; break;
            case 'A': incomingBits += "1010"; break;
            case 'B': incomingBits += "1011"; break;
            case 'C': incomingBits += "1100"; break;
            case 'D': incomingBits += "1101"; break;
            case 'E': incomingBits += "1110"; break;
            case 'F': incomingBits += "1111"; break;
            default:
                if (err) *err = QString("hex字符非法: %1").arg(QChar(c));
                return false;
            }
        }

        // process_control_packet_binary_string()
        for (int i = 0; i < 128; ++i) {
            if (isCL) {
                if (incomingBits[i] == '1' && incomingBits[i] != bits[i]) {
                    bits[i] = '1';
                }
            } else { // OH
                if (incomingBits[i] == '1' && incomingBits[i] == bits[i]) {
                    bits[i] = '0';
                }
            }
        }

        if (!DeviceConfigSync::saveFloorFlags128Bits(bits)) {
            if (err) *err = "保存楼层开关位图失败";
            return false;
        }

        // 发给分层板"OPEN" + 128位 '0'/'1'
        QByteArray out("OPEN");
        out += bits;
        if (rs485Payload) *rs485Payload = out;
        return true;
    }

    if (err) *err = QString("未知楼层限行控制格式: %1").arg(QString::fromLatin1(buf));
    return false;
}


// 楼层回包识别
static bool parseLegacyFloorPressFrame(const QByteArray &frame, QString *floorOut)
{
    if (frame.size() != 36) {
        return false;
    }

    if (static_cast<unsigned char>(frame[0]) != 0x24 ||
        static_cast<unsigned char>(frame[1]) != 0x40 ||
        static_cast<unsigned char>(frame[34]) != 0x23 ||
        static_cast<unsigned char>(frame[35]) != 0x21) {
        return false;
    }

    if (floorOut) {
        *floorOut = QString::fromLatin1(frame.constData() + 2, 2);
    }
    return true;
}


}


/** @brief 创建保活、旧卡片兜底和在线二维码响应定时器。 */
IcMqttGateway::IcMqttGateway(QObject *parent)
    : QObject(parent)
{
    keepAliveTimer_ = new QTimer(this);
    keepAliveTimer_->setInterval(60000);
    connect(keepAliveTimer_, &QTimer::timeout, this, &IcMqttGateway::onKeepAliveTimeout);

    legacyCardFallbackTimer_ = new QTimer(this);
    legacyCardFallbackTimer_->setSingleShot(true);
    legacyCardFallbackTimer_->setInterval(10);   // send_user_use_mqtt() 的 10ms 节奏
    connect(legacyCardFallbackTimer_, &QTimer::timeout,
            this, &IcMqttGateway::publishPendingLegacyCardFallback);

    onlineQrReplyTimer_.setSingleShot(true);
    connect(&onlineQrReplyTimer_, &QTimer::timeout,
            this, &IcMqttGateway::onOnlineQrReplyTimeout);
}

/** @brief 替换 MQTT/硬件配置并按协议模式启停网关。 */
void IcMqttGateway::applyConfig(const MqttConfig &cfg)
{
    cfg_ = cfg;

    emit logMessage(QString("IcMqttGateway::applyConfig protocolMode=%1 publishTopic=%2 subscribeTopic=%3")
                    .arg(cfg_.protocolMode, cfg_.publishTopic, cfg_.subscribeTopic));

    if (!isEnabled()) {
        pendingLegacyCardId_.clear();
        if (legacyCardFallbackTimer_) {
            legacyCardFallbackTimer_->stop();
        }
        qWarning() << "[IC-GW] disabled by protocolMode=" << cfg_.protocolMode;
        keepAliveTimer_->stop();
        return;
    }

    ensureBridgeConnected();

    if (!keepAliveTimer_->isActive()) {
        keepAliveTimer_->start();
    }
}


// 只在 online 模式下启用
bool IcMqttGateway::isEnabled() const
{
    QString mode = cfg_.protocolMode.trimmed().toLower();

    // 兼容旧值
    if (mode == "legacy") {
        mode = "online";
    } else if (mode == "ic") {
        mode = "offline";
    }

    return mode == "online";
}

/** @brief 仅一次连接 IcEventBridge 的硬件事件与回执信号。 */
void IcMqttGateway::ensureBridgeConnected()
{
    if (bridgeConnected_) return;

    auto bridge = IcEventBridge::instance();

    connect(bridge, &IcEventBridge::cardPassed,
            this, &IcMqttGateway::onCardPassed, Qt::UniqueConnection);

    connect(bridge, &IcEventBridge::onlineQrScanned,
            this, &IcMqttGateway::onOnlineQrScanned, Qt::UniqueConnection);

    connect(bridge, &IcEventBridge::rs485FrameReceived,
            this, &IcMqttGateway::onRs485FrameReceived, Qt::UniqueConnection);

    QObject::connect(IcEventBridge::instance(), &IcEventBridge::rs485SendFinished,
                     this,
                     [this](const QString &sourceTag, bool ok, const QString &reason) {
        if (!sourceTag.startsWith(QStringLiteral("onlineQr:"))) {
            return;
        }

        if (ok) {
            emit logMessage(QString("onlineQr RS485发送完成，sourceTag=%1").arg(sourceTag));
            IcEventBridge::instance()->emitToastPassRequested();
        } else {
            emit logMessage(QString("onlineQr RS485发送失败，sourceTag=%1 reason=%2")
                            .arg(sourceTag, reason));
            IcEventBridge::instance()->emitToastFailRequested(
                        QStringLiteral("%1").arg(reason));
        }
    }, Qt::UniqueConnection);

    bridgeConnected_ = true;
    emit logMessage("IC MQTT bridge connected");
}

/** @brief 生成协议要求的当前时间字符串。 */
QString IcMqttGateway::currentTimeString() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

/** @brief 生成当前进程内递增且可追踪的消息 ID。 */
QString IcMqttGateway::nextMessageId() const
{
    const qlonglong current = DbStore::getConfig("message_id", 0).toLongLong();
    const qlonglong next = current + 1;
    DbStore::setConfig("message_id", next);
    return QString("%1").arg(next, 32, 10, QLatin1Char('0'));
}

/** @brief 兼容扁平和嵌套 envelope，归一化服务端消息。 */
IcMqttGateway::NormalizedMsg IcMqttGateway::normalize(const QJsonObject &obj) const
{
    NormalizedMsg m;
    m.raw = obj;
    m.method = firstNonEmpty(obj, {"method", "methon"});
    m.id = firstNonEmpty(obj, {"id", "reqId"});
    m.time = firstNonEmpty(obj, {"time", "Time"});
    if (obj.value("data").isObject()) {
        m.data = obj.value("data").toObject();
    } else {
        m.data = obj;
    }
    return m;
}

/** @brief 把业务 payload 封装为 mqttd publish packet。 */
QJsonObject IcMqttGateway::buildPublishPacket(const QJsonObject &payload) const
{
    QJsonObject root;
    root["cmd"] = "publish";
    root["topic"] = cfg_.publishTopic.trimmed();
    root["payload"] = payload;
    return root;
}

/** @brief 创建含公共设备、时间和 method 字段的 payload。 */
QJsonObject IcMqttGateway::buildBasePayload(const QString &methon) const
{
    QJsonObject payload;
    payload["methon"] = methon;
    payload["id"] = nextMessageId();
    payload["Time"] = currentTimeString();
    return payload;
}

/** @brief 发布无请求上下文的硬件事件。 */
void IcMqttGateway::publishEvent(const QString &methon, const QJsonObject &extra, const QString &tag)
{
    if (cfg_.publishTopic.trimmed().isEmpty()) return;
    QJsonObject payload = buildBasePayload(methon);
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        payload.insert(it.key(), it.value());
    }
    emit publishPacket(buildPublishPacket(payload), tag);
}

/** @brief 对指定请求 ID 发布成功响应。 */
void IcMqttGateway::publishSuccess(const QString &methon, const QString &reqId, const QJsonObject &extra, const QString &tag)
{
    if (cfg_.publishTopic.trimmed().isEmpty()) return;
    QJsonObject payload;
    payload["methon"] = methon;
    payload["id"] = reqId;
    payload["Time"] = currentTimeString();
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        payload.insert(it.key(), it.value());
    }
    emit publishPacket(buildPublishPacket(payload), tag);
}

/** @brief 发布交通梯当前楼层状态响应。 */
void IcMqttGateway::publishTrafficFloorState(const QString &reqId)
{
    QJsonObject extra;
    extra["faceDevice"] = cfg_.deviceName.trimmed().isEmpty() ? deviceCfg_.deviceName : cfg_.deviceName.trimmed();
    extra["Floor"] = CoreMigrationSync::floorFlags128String();
    publishSuccess("controlFloor", reqId, extra, "ic/getTrafficFloor");
}

/** @brief 从服务端 data 的兼容字段中查找卡号。 */
QString IcMqttGateway::findCardValue(const QJsonObject &data) const
{
    return firstNonEmpty(data, {"CardNo", "CardID", "cardNo", "cardId"});
}


/** @brief 用随后到达的楼层补全旧协议刷卡事件并发布。 */
void IcMqttGateway::publishPendingLegacyCardWithFloor(const QString &floor)
{
    if (pendingLegacyCardId_.isEmpty()) {
        return;
    }

    if (legacyCardFallbackTimer_) {
        legacyCardFallbackTimer_->stop();
    }

    QJsonObject extra;
    extra["CardID"] = pendingLegacyCardId_;
    extra["Floor"] = floor;
    publishEvent("card", extra, "ic/card");

    emit logMessage(QString("IC刷卡上报: card=%1 floor=%2")
                    .arg(pendingLegacyCardId_, floor));

    pendingLegacyCardId_.clear();
}


// 10ms 内没收到楼层上报(空楼层)
void IcMqttGateway::publishPendingLegacyCardFallback()
{
    if (pendingLegacyCardId_.isEmpty()) {
        return;
    }

    QJsonObject extra;
    extra["CardID"] = pendingLegacyCardId_;
    extra["Floor"] = "";
    publishEvent("card", extra, "ic/card");

    emit logMessage(QString("IC刷卡兜底上报(空楼层): card=%1").arg(pendingLegacyCardId_));

    pendingLegacyCardId_.clear();
}



/** @brief 解析服务端时间并同步系统与 RTC。 */
bool IcMqttGateway::setSystemTime(const QString &t, QString *err) const
{
    const QString trimmed = t.trimmed();
    if (trimmed.isEmpty()) {
        if (err) *err = "时间为空";
        return false;
    }
    const QDateTime dt = QDateTime::fromString(trimmed, "yyyy-MM-dd HH:mm:ss");
    if (!dt.isValid()) {
        if (err) *err = QString("时间格式非法: %1").arg(trimmed);
        return false;
    }
    if (!Rk3566Platform::setSystemDateTime(dt, err)) {
        return false;
    }
    QString rtcError;
    if (!Rk3566Platform::syncRtcFromSystem(&rtcError)) {
        qWarning() << "[time] system time set, RTC sync failed:" << rtcError;
    }
    return true;
}


// OpenFloor 远程开梯
bool IcMqttGateway::executeOpenFloor(const QString &floor, QString *err)
{
    const QString f = floor.trimmed();
    if (f.isEmpty()) {
        if (err) *err = "floor 为空";
        return false;
    }

    QByteArray frame;
    if (f.size() == 34) {
        frame = f.toLatin1();
        frame.append('#');
        frame.append('!');
    } else if (f.size() == 36 && f.startsWith("#@") && f.endsWith("#!")) {
        frame = f.toLatin1();
    } else {
        int floorNo = 0;
        if (!parseOpenFloorNo(f, &floorNo, err)) {
            return false;
        }
        frame = Rs485FloorFrameBuilder::buildSingleFloor(floorNo, err);
        if (frame.isEmpty()) return false;
    }

    IcEventBridge::instance()->requestRs485Send(frame);
    emit logMessage(QString("openFloor -> RS485 len=%1 hex=%2").arg(frame.size())
                    .arg(QString::fromLatin1(frame.toHex(' '))));
    return true;
}


// SendFloorQr
bool IcMqttGateway::executeSendFloorQr(const QString &rawData, const QString &sourceTag, QString *err)
{
    QString s = rawData.trimmed();
    if (s.isEmpty()) {
        if (err) *err = QStringLiteral("data为空");
        return false;
    }

    QByteArray frame = s.toLatin1();

    // 服务端回 34 字节正文时，末尾补 # !
    if (frame.size() == 34) {
        frame.append('#');
        frame.append('!');
    }

    if (frame.size() != 36) {
        if (err) {
            *err = QStringLiteral("sendFloorQr长度非法: %1").arg(frame.size());
        }
        return false;
    }

    IcEventBridge::instance()->requestRs485Send(frame, sourceTag);

    emit logMessage(QString("sendFloorQr -> RS485 req, sourceTag=%1 hex=%2")
                    .arg(sourceTag, QString::fromLatin1(frame.toHex(' '))));
    return true;
}

/** @brief 执行继电器控制命令。 */
bool IcMqttGateway::executeSetJdq(const QString &cmd, QString *err)
{
    const QByteArray frame = cmd.toLatin1();
    if (frame.isEmpty()) {
        if (err) *err = "setjdq 指令为空";
        return false;
    }

    IcEventBridge::instance()->requestRs485Send(frame);
    emit logMessage(QString("setjdq -> RS485 cmd=%1").arg(QString::fromLatin1(frame)));
    return true;
}


// 网络版刷卡上报
void IcMqttGateway::onCardPassed(const QString &cardId, const QString &floor, const QByteArray &rawFrame)
{
    Q_UNUSED(rawFrame);

    if (!isEnabled()) {
        qWarning() << "[IC-GW] onCardPassed ignored because disabled";
        return;
    }

    // 白名单保护
    if (!CoreMigrationSync::isRegisteredCard(cardId)) {
        emit logMessage(QString("IC card blocked: card=%1 unregistered").arg(cardId));
        return;
    }

    // 带楼层路径上报
    if (!floor.isEmpty()) {
        QJsonObject extra;
        extra["CardID"] = cardId;
        extra["Floor"] = floor;
        publishEvent("card", extra, "ic/card");

        emit logMessage(QString("IC刷卡上报: card=%1 floor=%2").arg(cardId, floor));
        return;
    }

    // 先挂起 cardId，等待分层板楼层回包；
    // 若 10ms 内仍无回包，则空楼层兜底上报。
    pendingLegacyCardId_ = cardId;

    if (legacyCardFallbackTimer_) {
        legacyCardFallbackTimer_->stop();
        legacyCardFallbackTimer_->start();
    }

    emit logMessage(QString("刷卡通过，等待楼层回包/兜底上报: card=%1").arg(cardId));
}


// 网络版二维码上报
void IcMqttGateway::onOnlineQrScanned(const QString &qrCode)
{
    if (!isEnabled()) {
        emit logMessage(QString("onlineQrScanned ignored, protocolMode=%1").arg(cfg_.protocolMode));
        return;
    }

    if (qrCode.trimmed().isEmpty()) {
        emit logMessage("onlineQrScanned ignored, qrCode empty");
        return;
    }

    // 等上一个二维码回包
    if (onlineQrReplyPending_) {
        emit logMessage(QString("在线二维码请求尚未完成，忽略重复扫码，pendingQr=%1")
                        .arg(pendingOnlineQrCode_));
        return;
    }

    QJsonObject extra;
    extra.insert("qrCode", qrCode);

    publishEvent(QStringLiteral("sendQrCode"), extra, QStringLiteral("ic/sendQrCode"));

    // 上报成功后开始等待服务端 sendFloorQr
    armOnlineQrReplyTimeout(qrCode);
}


// 保活消息发布
// RS485 回包上报（对齐 Core: methon=getjdq, jdq=回包文本）
void IcMqttGateway::onRs485FrameReceived(const QByteArray &frame)
{
    if (!isEnabled()) return;

    // 是否是旧式楼层回包帧
    QString pressedFloor;
    if (parseLegacyFloorPressFrame(frame, &pressedFloor) && !pendingLegacyCardId_.isEmpty()) {
        publishPendingLegacyCardWithFloor(pressedFloor);
    }

    const QString jdq = CoreMigrationSync::sanitizeRs485Text(frame);
    if (jdq.isEmpty()) return;

    QJsonObject extra;
    extra["jdq"] = jdq;
    publishEvent("getjdq", extra, "ic/getjdq/rx");
}

// 保活消息发布
void IcMqttGateway::onKeepAliveTimeout()
{
    if (!isEnabled()) return;
    QJsonObject extra;
    extra["Connection_status"] = "YC-ZKB is alive";
    publishEvent("keep", extra, "ic/keepalive");
}


// 属性上报 发布
bool IcMqttGateway::handleIncoming(const QString &topic, const QJsonObject &payloadObj)
{
    Q_UNUSED(topic);
    if (!isEnabled()) return false;

    const NormalizedMsg msg = normalize(payloadObj);
    if (msg.method.isEmpty()) {
        return false;
    }

    // 只对真正 IC 方法打印 IC下行
    static const QSet<QString> kIcMethods = {
        "reboot",
        "setTime",
        "registerCardOne",
        "deleteCardOne",
        "openFloor",
        "openTrafficFloor",
        "closeTrafficFloor",
        "getTrafficFloor",
        "sendFloorQr",
        "setjdq",
        "getjdq",
        "ResponseAlive"
    };

    if (!kIcMethods.contains(msg.method)) {
        return false;
    }

    emit logMessage(QString("IC下行: method=%1 payload=%2")
                       .arg(msg.method, jsonCompact(payloadObj)));

    if (msg.method == "reboot") {
        publishSuccess("rebootSuccess", msg.id, QJsonObject{}, "ic/reboot");
        Rk3566Platform::reboot();
        return true;
    }

    if (msg.method == "setTime") {
        QString err;
        const QString t = firstNonEmpty(msg.data, {"time", "Time"}).isEmpty() ? msg.time : firstNonEmpty(msg.data, {"time", "Time"});
        if (!setSystemTime(t, &err)) {
            emit logMessage(QString("setTime 失败: %1").arg(err));
        }
        return true;
    }

    if (msg.method == "registerCardOne") {
        const QString card = CoreMigrationSync::normalizeCardId(findCardValue(msg.data));
        if (!card.isEmpty() && CoreMigrationSync::registerCard(card)) {
            QJsonObject extra; extra["CardID"] = card;
            publishSuccess("registerSuccess", msg.id, extra, "ic/registerCardOne");
        }
        return true;
    }

    if (msg.method == "deleteCardOne") {
        const QString card = CoreMigrationSync::normalizeCardId(findCardValue(msg.data));
        if (!card.isEmpty() && CoreMigrationSync::deleteCard(card)) {
            QJsonObject extra; extra["CardID"] = card;
            publishSuccess("deleteSuccess", msg.id, extra, "ic/deleteCardOne");
        }
        return true;
    }

    if (msg.method == "openFloor") {
        QString err;
        const QString floor = firstNonEmpty(msg.data, {"floor", "Floor", "openFloor"});
        if (executeOpenFloor(floor, &err)) {
            publishSuccess("openFloorSuccess", msg.id, QJsonObject{}, "ic/openFloor");
        } else {
            emit logMessage(QString("openFloor 失败: %1").arg(err));
        }
        return true;
    }

    if (msg.method == "openTrafficFloor") {
        QString err;
        QString raw = firstNonEmpty(msg.data, {"openTrafficFloor", "Floor", "data"});
        if (raw.isEmpty() && msg.raw.value("data").isString()) {
            raw = msg.raw.value("data").toString().trimmed();
        }

        const QString cmd = normalizeTrafficCmd(raw, true);
        QByteArray rs485Payload;

        if (applyTrafficCmdLikeStm32(cmd, &rs485Payload, &err)) {
            IcEventBridge::instance()->requestRs485Send(rs485Payload);
            emit logMessage(QString("openTrafficFloor -> RS485 len=%1 hex=%2")
                            .arg(rs485Payload.size())
                            .arg(QString::fromLatin1(rs485Payload.toHex(' '))));
            publishSuccess("openTrafficFloorSuccess", msg.id, QJsonObject{}, "ic/openTrafficFloor");
        } else {
            emit logMessage(QString("openTrafficFloor 失败: %1").arg(err));
        }
        return true;
    }

    if (msg.method == "closeTrafficFloor") {
        QString err;
        QString raw = firstNonEmpty(msg.data, {"closeTrafficFloor", "Floor", "data"});
        if (raw.isEmpty() && msg.raw.value("data").isString()) {
            raw = msg.raw.value("data").toString().trimmed();
        }

        const QString cmd = normalizeTrafficCmd(raw, false);
        QByteArray rs485Payload;

        if (applyTrafficCmdLikeStm32(cmd, &rs485Payload, &err)) {
            IcEventBridge::instance()->requestRs485Send(rs485Payload);
            emit logMessage(QString("closeTrafficFloor -> RS485 len=%1 hex=%2")
                            .arg(rs485Payload.size())
                            .arg(QString::fromLatin1(rs485Payload.toHex(' '))));
            publishSuccess("closeTrafficFloorSuccess", msg.id, QJsonObject{}, "ic/closeTrafficFloor");
        } else {
            emit logMessage(QString("closeTrafficFloor 失败: %1").arg(err));
        }
        return true;
    }

    if (msg.method == "getTrafficFloor") {
        publishTrafficFloorState(msg.id);
        return true;
    }

    if (msg.method == "sendFloorQr") {
        // 如果已经超时，直接忽略
        if (!onlineQrReplyPending_) {
            emit logMessage(QString("sendFloorQr 收到，但当前没有待处理在线二维码，忽略。id=%1")
                            .arg(msg.id));
            return true;
        }

        // 收到服务端回包，先停止“未回包超时”计时
        clearOnlineQrReplyTimeout();

        QString err;
        QString dataStr;
        if (msg.raw.value("data").isString()) {
            dataStr = msg.raw.value("data").toString().trimmed();
        }
        if (dataStr.isEmpty()) {
            dataStr = firstNonEmpty(msg.data, {"data", "payload", "floorData", "sendFloorQr"});
        }

        const QString sourceTag = QStringLiteral("onlineQr:%1").arg(msg.id);

        if (!executeSendFloorQr(dataStr, sourceTag, &err)) {
            emit logMessage(QString("sendFloorQr 失败: %1").arg(err));
            IcEventBridge::instance()->emitToastFailRequested(
                        QStringLiteral("在线二维码失败：%1").arg(err));
        }

        return true;
    }

    if (msg.method == "setjdq") {
        QString err;
        const QString cmd = firstNonEmpty(msg.data, {"setjdq", "cmd", "data"});
        if (!executeSetJdq(cmd, &err)) {
            emit logMessage(QString("setjdq 失败: %1").arg(err));
        }
        return true;
    }

    if (msg.method == "getjdq") {
        IcEventBridge::instance()->requestRs485Send(QByteArray("getjdq"));
        emit logMessage("getjdq -> RS485 cmd=getjdq");
        return true;
    }

    if (msg.method == "ResponseAlive") {
        emit logMessage(QString("服务端存活响应：code=%1 msg=%2")
                        .arg(msg.data.value("code").toInt())
                        .arg(msg.data.value("msg").toString()));
        return true;
    }

    return false;
}



/** @brief 记录待响应二维码并启动 500 ms 超时。 */
void IcMqttGateway::armOnlineQrReplyTimeout(const QString &qrCode)
{
    onlineQrReplyPending_ = true;
    pendingOnlineQrCode_ = qrCode;
    onlineQrReplyTimer_.start(onlineQrReplyTimeoutMs_);

    emit logMessage(QString("在线二维码已上报，等待 sendFloorQr 回包，timeout=%1ms qrCode=%2")
                    .arg(onlineQrReplyTimeoutMs_)
                    .arg(qrCode));
}

/** @brief 收到匹配响应后清除二维码等待状态。 */
void IcMqttGateway::clearOnlineQrReplyTimeout()
{
    onlineQrReplyTimer_.stop();
    onlineQrReplyPending_ = false;
    pendingOnlineQrCode_.clear();
}

/** @brief 服务端未及时响应时结束等待并记录超时。 */
void IcMqttGateway::onOnlineQrReplyTimeout()
{
    if (!onlineQrReplyPending_) {
        return;
    }

    const QString qrCode = pendingOnlineQrCode_;
    clearOnlineQrReplyTimeout();

    emit logMessage(QString("在线二维码等待 sendFloorQr 回包超时，qrCode=%1").arg(qrCode));
    IcEventBridge::instance()->emitToastFailRequested(
                QStringLiteral("无效二维码"));
}
