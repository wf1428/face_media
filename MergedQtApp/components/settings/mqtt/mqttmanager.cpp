/**
 * @file mqttmanager.cpp
 * @brief 协调 mqttd IPC、业务服务、消息路由和 IC 在线网关。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "mqttmanager.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QTime>
#include <QUuid>

#include <algorithm>
#include <cmath>

#include "common/face_image_sync_bridge.h"
#include "common/sql/dbstore.h"
#include "common/storage_policy.h"
#include "ic_mqtt_gateway.h"
#include "ic_board/ic_event_bridge.h"
#include "ic_board/rs485_floor_frame_builder.h"
#include "platform/rk3566_platform.h"
#include <unistd.h>

namespace {

QString accessSnapshotRoot()
{
    return QDir(Rk3566Platform::applicationRoot()).filePath(
                QStringLiteral("data/snapshots"));
}

QString accessPersonName(const QString &personId)
{
    const QString normalized = personId.trimmed();
    if (normalized.isEmpty()) {
        return QStringLiteral("未登记人员");
    }
    QList<QVariantMap> rows = DbStore::query(
                QStringLiteral(
                    "SELECT name FROM network_person WHERE person_id=? LIMIT 1"),
                {normalized});
    if (!rows.isEmpty()) {
        const QString name = rows.first().value(QStringLiteral("name"))
                .toString().trimmed();
        if (!name.isEmpty()) return name;
    }
    rows = DbStore::query(
                QStringLiteral(
                    "SELECT name FROM person WHERE person_no=? LIMIT 1"),
                {normalized});
    if (!rows.isEmpty()) {
        const QString name = rows.first().value(QStringLiteral("name"))
                .toString().trimmed();
        if (!name.isEmpty()) return name;
    }
    return QStringLiteral("未登记人员");
}

void appendCredentialAccessRecord(const QString &type,
                                  const QString &personId,
                                  const QString &credential,
                                  bool success,
                                  const QString &reason)
{
    QJsonObject record;
    record.insert(QStringLiteral("type"), type);
    record.insert(QStringLiteral("name"), accessPersonName(personId));
    record.insert(QStringLiteral("credential"), credential);
    record.insert(QStringLiteral("success"), success);
    if (!reason.trimmed().isEmpty()) {
        record.insert(QStringLiteral("reason"), reason.trimmed());
    }
    QString error;
    if (!StoragePolicy::appendAccessRecord(
                accessSnapshotRoot(), record, &error)) {
        qWarning().noquote() << error;
    }
}

QJsonArray failedFacesFromPayload(const QJsonObject &payload,
                                  const QString &message)
{
    QJsonArray failedFaces;
    const QJsonArray faces = payload.value(QStringLiteral("data"))
            .toObject().value(QStringLiteral("faces")).toArray();
    for (const QJsonValue &value : faces) {
        const QJsonObject input = value.toObject();
        QJsonObject failed;
        failed.insert(QStringLiteral("name"),
                      input.value(QStringLiteral("name")).toString());
        failed.insert(QStringLiteral("faceHash"),
                      input.value(QStringLiteral("faceHash")).toString());
        failed.insert(QStringLiteral("message"), message);
        failedFaces.append(failed);
    }
    return failedFaces;
}

QJsonArray mergeFailedFaces(const QJsonArray &first,
                            const QJsonArray &second)
{
    QJsonArray merged;
    QSet<QString> keys;
    auto appendUnique = [&merged, &keys](const QJsonArray &faces) {
        for (const QJsonValue &value : faces) {
            const QJsonObject face = value.toObject();
            const QString faceHash = face.value(QStringLiteral("faceHash"))
                    .toString().trimmed();
            const QString key = faceHash.isEmpty()
                    ? QStringLiteral("\n")
                        + face.value(QStringLiteral("name"))
                          .toString().trimmed()
                    : faceHash;
            if (!keys.contains(key)) {
                keys.insert(key);
                merged.append(face);
            }
        }
    };
    appendUnique(first);
    appendUnique(second);
    return merged;
}

QString firstFailedFaceMessage(const QJsonArray &failedFaces,
                               const QString &fallback)
{
    for (const QJsonValue &value : failedFaces) {
        const QString message = value.toObject()
                .value(QStringLiteral("message")).toString().trimmed();
        if (!message.isEmpty()) return message;
    }
    return fallback;
}

QString onlineQrFailureReason(int code, const QString &serverReason = QString())
{
    switch (code) {
    case 4000: return QStringLiteral("二维码参数错误");
    case 4001: return QStringLiteral("二维码无效");
    case 4002: return QStringLiteral("二维码格式错误");
    case 4003: return QStringLiteral("二维码记录不存在");
    case 4100: return QStringLiteral("该二维码已被停用");
    case 4101: return QStringLiteral("二维码尚未生效，请等待生效时间");
    case 4102: return QStringLiteral("二维码已过期");
    case 4200: return QStringLiteral("通行次数已用完，请联系管理员充值");
    case 4201: return QStringLiteral("通行余额不足，请联系管理员充值");
    case 4202: return QStringLiteral("二维码使用次数已用完");
    case 4300: return QStringLiteral("此设备无通行权限");
    case 4301: return QStringLiteral("未配置可通行楼层");
    case 4400: return QStringLiteral("访客邀请已失效");
    case 4401: return QStringLiteral("访客邀请未配置设备");
    case 4402: return QStringLiteral("此设备不在访客可通行列表中");
    case 4403: return QStringLiteral("访客邀请未配置楼层");
    case 4500: return QStringLiteral("设备开启失败");
    case 4501: return QStringLiteral("次数扣减失败");
    default:
        return serverReason.trimmed().isEmpty()
                ? QStringLiteral("二维码处理失败") : serverReason.trimmed();
    }
}

QString qrCodeFromObject(const QJsonObject &object)
{
    return object.value(QStringLiteral("qrCode")).toString().trimmed();
}

bool parseFloorControl(const QJsonObject &data,
                       NetworkQrAuthorization *authorization,
                       QByteArray *rs485Frame,
                       QString *error)
{
    if (error) error->clear();
    if (!authorization || !rs485Frame) {
        if (error) *error = QStringLiteral("二维码参数错误");
        return false;
    }

    NetworkQrAuthorization value;
    value.qrCode = qrCodeFromObject(data);
    value.personId = data.value(QStringLiteral("personId")).toString().trimmed();
    value.sourceType = data.value(QStringLiteral("sourceType"))
            .toString().trimmed().toUpper();
    value.invitationId = data.value(QStringLiteral("invitationId"))
            .toString().trimmed();
    value.floorHex = data.value(QStringLiteral("floorHex")).toString().trimmed();
    value.floorHex.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (value.floorHex.size() == 36
            && value.floorHex.startsWith(QStringLiteral("#@"))
            && value.floorHex.endsWith(QStringLiteral("#!"))) {
        value.floorHex = value.floorHex.mid(2, 32);
    }
    value.floorHex = value.floorHex.toUpper();

    if (value.qrCode.isEmpty()
            || (value.sourceType != QStringLiteral("CREDENTIAL")
                && value.sourceType != QStringLiteral("VISITOR"))) {
        if (error) *error = QStringLiteral("二维码参数错误");
        return false;
    }
    if (value.sourceType == QStringLiteral("CREDENTIAL") && value.personId.isEmpty()) {
        if (error) *error = QStringLiteral("二维码参数错误");
        return false;
    }
    if (value.sourceType == QStringLiteral("VISITOR") && !value.personId.isEmpty()) {
        if (error) *error = QStringLiteral("访客二维码不应携带personId");
        return false;
    }
    static const QRegularExpression floorHexPattern(QStringLiteral("^[0-9A-F]{32}$"));
    if (!floorHexPattern.match(value.floorHex).hasMatch()) {
        if (error) *error = QStringLiteral("楼层控制数据格式错误");
        return false;
    }

    QList<int> floorNumbers;
    const QJsonArray floors = data.value(QStringLiteral("floors")).toArray();
    QSet<int> seen;
    for (const QJsonValue &floorValue : floors) {
        bool ok = false;
        const int floor = floorValue.isDouble()
                ? floorValue.toInt()
                : floorValue.toString().trimmed().toInt(&ok);
        if (floorValue.isDouble()) ok = true;
        if (!ok || floor == 0 || floor < -8 || floor > 120) {
            if (error) *error = QStringLiteral("楼层控制数据格式错误");
            return false;
        }
        if (!seen.contains(floor)) {
            seen.insert(floor);
            floorNumbers.append(floor);
        }
    }
    if (floorNumbers.isEmpty()) {
        if (error) *error = QStringLiteral("未配置可通行楼层");
        return false;
    }
    std::sort(floorNumbers.begin(), floorNumbers.end());

    QString buildError;
    const QByteArray frame = Rs485FloorFrameBuilder::buildFloors(floorNumbers, &buildError);
    if (frame.size() != 36) {
        if (error) *error = buildError.isEmpty()
                ? QStringLiteral("楼层控制数据格式错误") : buildError;
        return false;
    }
    const QString generatedFloorHex = QString::fromLatin1(frame.mid(2, 32)).toUpper();
    if (generatedFloorHex != value.floorHex) {
        if (error) *error = QStringLiteral("楼层控制信息不一致");
        return false;
    }

    for (int floor : floorNumbers) {
        value.floors.append(QString::number(floor));
    }
    *authorization = value;
    *rs485Frame = frame;
    return true;
}

QString faceImageRequestKey(const QJsonObject &payload)
{
    const QString messageId = payload.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString deviceId = payload.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (!messageId.isEmpty()) {
        return QStringLiteral("%1\n%2").arg(deviceId, messageId);
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(payload).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

QString faceImageContentKey(const QJsonObject &payload)
{
    const QJsonObject data = payload.value(QStringLiteral("data")).toObject();
    QByteArray content = data.value(QStringLiteral("personId"))
            .toString().trimmed().toUtf8();
    const QJsonArray faces = data.value(QStringLiteral("faces")).toArray();
    for (const QJsonValue &value : faces) {
        const QJsonObject face = value.toObject();
        content.append('\n');
        content.append(face.value(QStringLiteral("faceHash"))
                       .toString().trimmed().toUtf8());
        content.append('\n');
        content.append(face.value(QStringLiteral("name"))
                       .toString().trimmed().toUtf8());
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        content, QCryptographicHash::Sha256).toHex());
}

struct FloorLimitAction
{
    int floor = 0;
    bool open = false;
};

int onlineV1FloorBit(int floor)
{
    if (floor >= 1 && floor <= 120) return floor - 1;
    if (floor >= -8 && floor <= -1) return floor + 128;
    return -1;
}

QByteArray onlineV1FloorLimitBits()
{
    const QString stored = DbStore::getConfig(
                QStringLiteral("online_v1_floor_limit_bits"),
                QString(128, QLatin1Char('0'))).toString().trimmed();
    const QByteArray bits = stored.toLatin1();
    if (bits.size() != 128) return QByteArray(128, '0');
    for (char bit : bits) {
        if (bit != '0' && bit != '1') return QByteArray(128, '0');
    }
    return bits;
}

bool saveOnlineV1FloorLimitBits(const QByteArray &bits,
                                QString *error)
{
    if (bits.size() != 128) {
        if (error) *error = QStringLiteral("楼层状态长度错误");
        return false;
    }

    if (!DbStore::setConfig(QStringLiteral("online_v1_floor_limit_bits"),
                            QString::fromLatin1(bits))) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    DbStore::checkpoint();
    return true;
}

QJsonArray onlineV1FloorSchedules()
{
    const QByteArray raw = DbStore::getConfig(
                QStringLiteral("online_v1_floor_schedules"),
                QStringLiteral("[]")).toString().trimmed().toUtf8();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        return QJsonArray();
    }
    return document.array();
}

QByteArray onlineV1FloorScheduleBits()
{
    const QString stored = DbStore::getConfig(
                QStringLiteral("online_v1_floor_schedule_bits"),
                QString(128, QLatin1Char('0'))).toString().trimmed();
    const QByteArray bits = stored.toLatin1();
    if (bits.size() != 128) return QByteArray(128, '0');
    for (char bit : bits) {
        if (bit != '0' && bit != '1') return QByteArray(128, '0');
    }
    return bits;
}

QSet<int> onlineV1FloorScheduleFloors(const QJsonArray &schedules)
{
    QSet<int> floors;
    for (const QJsonValue &scheduleValue : schedules) {
        const QJsonArray values = scheduleValue.toObject()
                .value(QStringLiteral("floors")).toArray();
        for (const QJsonValue &floorValue : values) {
            if (floorValue.isDouble()) floors.insert(floorValue.toInt());
        }
    }
    return floors;
}

int onlineV1FloorOpenStatus(const QByteArray &openBits, int floor)
{
    const int bit = onlineV1FloorBit(floor);
    if (bit < 0 || bit >= openBits.size()) return 0;
    return openBits.at(bit) == '1' ? 1 : 0;
}

QByteArray onlineV1FloorLimitBatchFrame(const QByteArray &openBits)
{
    // 完全沿用 online_v2 批量交通梯帧："OPEN" + 128 个限行状态字符。
    // 分层板位序为 -8~-1、1~120；状态字符 0=开放、1=限行。
    QByteArray restrictionBits(128, '1');
    for (int floor = -8; floor <= -1; ++floor) {
        const int stateBit = onlineV1FloorBit(floor);
        const int boardBit = floor + 8;
        restrictionBits[boardBit] = openBits.at(stateBit) == '1' ? '0' : '1';
    }
    for (int floor = 1; floor <= 120; ++floor) {
        const int stateBit = onlineV1FloorBit(floor);
        const int boardBit = floor + 7;
        restrictionBits[boardBit] = openBits.at(stateBit) == '1' ? '0' : '1';
    }

    QByteArray frame("OPEN");
    frame.append(restrictionBits);
    return frame;
}

QString onlineV1FloorOpenHex(const QByteArray &openBits)
{
    QByteArray bytes(16, '\0');
    for (int bit = 0; bit < openBits.size() && bit < 128; ++bit) {
        if (openBits.at(bit) != '1') continue;
        const int byteIndex = 15 - bit / 8;
        const int oldValue = static_cast<unsigned char>(bytes.at(byteIndex));
        bytes[byteIndex] = static_cast<char>(oldValue | (1 << (bit % 8)));
    }
    return QStringLiteral("0x")
            + QString::fromLatin1(bytes.toHex().toUpper());
}

QJsonObject floorLimitResponseData(const QByteArray &openBits,
                                   const QList<int> &floors,
                                   int code)
{
    QJsonArray statuses;
    for (int floor : floors) {
        QJsonObject item;
        item.insert(QStringLiteral("floor"), floor);
        item.insert(QStringLiteral("status"),
                    onlineV1FloorOpenStatus(openBits, floor));
        statuses.append(item);
    }

    QJsonObject data;
    data.insert(QStringLiteral("code"), code);
    data.insert(QStringLiteral("floors"), statuses);
    data.insert(QStringLiteral("floorHex"), onlineV1FloorOpenHex(openBits));
    return data;
}

QList<int> allTrafficFloors()
{
    QList<int> floors;
    floors.reserve(128);
    for (int floor = 1; floor <= 120; ++floor) floors.append(floor);
    for (int floor = -8; floor <= -1; ++floor) floors.append(floor);
    return floors;
}

bool jsonIntegerInRange(const QJsonValue &value,
                        int minimum,
                        int maximum,
                        int *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (std::floor(number) != number
            || number < static_cast<double>(minimum)
            || number > static_cast<double>(maximum)) {
        return false;
    }
    if (result) *result = static_cast<int>(number);
    return true;
}

bool normalizeOnlineV1FloorSchedules(const QJsonObject &data,
                                     QJsonArray *normalizedSchedules,
                                     QString *error)
{
    if (!normalizedSchedules) return false;
    *normalizedSchedules = QJsonArray();

    const QJsonValue schedulesValue = data.value(QStringLiteral("schedules"));
    if (!schedulesValue.isArray()) {
        if (error) *error = QStringLiteral("schedules 必须是数组");
        return false;
    }

    static const QRegularExpression timePattern(
                QStringLiteral("^(?:[01][0-9]|2[0-3]):[0-5][0-9]$"));
    QHash<QString, int> actionBySlot;
    QSet<QString> scheduleHashes;
    const QJsonArray schedules = schedulesValue.toArray();
    for (const QJsonValue &scheduleValue : schedules) {
        if (!scheduleValue.isObject()) {
            if (error) *error = QStringLiteral("schedules 元素必须是对象");
            return false;
        }

        const QJsonObject input = scheduleValue.toObject();
        const QJsonValue hashValue = input.value(QStringLiteral("hash"));
        const QString hash = hashValue.toString().trimmed();
        if (!hashValue.isString() || hash.isEmpty()) {
            if (error) *error = QStringLiteral("hash 必须是非空字符串");
            return false;
        }
        if (scheduleHashes.contains(hash)) {
            if (error) *error = QStringLiteral("schedules 中存在重复 hash");
            return false;
        }
        scheduleHashes.insert(hash);

        int action = -1;
        if (!jsonIntegerInRange(input.value(QStringLiteral("action")),
                                0, 1, &action)) {
            if (error) *error = QStringLiteral("action 只能为 0 或 1");
            return false;
        }

        const QString time = input.value(QStringLiteral("time"))
                .toString().trimmed();
        if (!timePattern.match(time).hasMatch()
                || !QTime::fromString(time, QStringLiteral("HH:mm")).isValid()) {
            if (error) *error = QStringLiteral("time 格式必须为 HH:mm");
            return false;
        }

        const QJsonValue weekDaysValue = input.value(QStringLiteral("weekDays"));
        const QJsonValue floorsValue = input.value(QStringLiteral("floors"));
        if (!weekDaysValue.isArray() || weekDaysValue.toArray().isEmpty()) {
            if (error) *error = QStringLiteral("weekDays 必须是非空数组");
            return false;
        }
        if (!floorsValue.isArray() || floorsValue.toArray().isEmpty()) {
            if (error) *error = QStringLiteral("floors 必须是非空数组");
            return false;
        }

        QSet<int> weekDaySet;
        for (const QJsonValue &weekDayValue : weekDaysValue.toArray()) {
            int weekDay = 0;
            if (!jsonIntegerInRange(weekDayValue, 1, 7, &weekDay)) {
                if (error) *error = QStringLiteral("weekDays 只能为 1~7");
                return false;
            }
            weekDaySet.insert(weekDay);
        }

        QSet<int> floorSet;
        for (const QJsonValue &floorValue : floorsValue.toArray()) {
            int floor = 0;
            if (!jsonIntegerInRange(floorValue, -8, 120, &floor)
                    || floor == 0) {
                if (error) {
                    *error = QStringLiteral(
                                "floors 楼层范围为 -8~-1、1~120");
                }
                return false;
            }
            floorSet.insert(floor);
        }

        QList<int> weekDays = weekDaySet.values();
        QList<int> floors = floorSet.values();
        std::sort(weekDays.begin(), weekDays.end());
        std::sort(floors.begin(), floors.end());

        for (int weekDay : weekDays) {
            for (int floor : floors) {
                const QString slot = QStringLiteral("%1|%2|%3")
                        .arg(time).arg(weekDay).arg(floor);
                if (actionBySlot.contains(slot)
                        && actionBySlot.value(slot) != action) {
                    if (error) {
                        *error = QStringLiteral(
                                    "同一楼层在相同星期和时间存在冲突 action");
                    }
                    return false;
                }
                actionBySlot.insert(slot, action);
            }
        }

        QJsonArray normalizedWeekDays;
        for (int weekDay : weekDays) normalizedWeekDays.append(weekDay);
        QJsonArray normalizedFloors;
        for (int floor : floors) normalizedFloors.append(floor);

        QJsonObject normalized;
        normalized.insert(QStringLiteral("action"), action);
        normalized.insert(QStringLiteral("time"), time);
        normalized.insert(QStringLiteral("weekDays"), normalizedWeekDays);
        normalized.insert(QStringLiteral("floors"), normalizedFloors);
        normalized.insert(QStringLiteral("hash"), hash);
        normalizedSchedules->append(normalized);
    }
    return true;
}

bool saveOnlineV1FloorSchedules(const QJsonArray &schedules,
                                const QByteArray &scheduleBits,
                                bool resetExecutionMinute,
                                QString *error)
{
    if (scheduleBits.size() != 128) {
        if (error) *error = QStringLiteral("定时楼层状态长度错误");
        return false;
    }
    if (!DbStore::transactionBegin()) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    const QString schedulesJson = QString::fromUtf8(
                QJsonDocument(schedules).toJson(QJsonDocument::Compact));
    const bool schedulesSaved = DbStore::setConfig(
                QStringLiteral("online_v1_floor_schedules"), schedulesJson);
    const bool stateSaved = schedulesSaved && DbStore::setConfig(
                QStringLiteral("online_v1_floor_schedule_bits"),
                QString::fromLatin1(scheduleBits));
    // config.value 为 NOT NULL；QString() 会被 Qt SQL 绑定为 NULL。
    const bool markerSaved = !resetExecutionMinute
            ? stateSaved
            : stateSaved && DbStore::setConfig(
                  QStringLiteral("online_v1_floor_schedule_last_minute"),
                  QStringLiteral(""));
    if (!markerSaved || !DbStore::transactionCommit()) {
        const QString dbError = DbStore::lastError();
        DbStore::transactionRollback();
        if (error) *error = dbError;
        return false;
    }
    DbStore::checkpoint();
    return true;
}

bool saveOnlineV1FloorScheduleExecution(const QByteArray &scheduleBits,
                                        const QByteArray &currentFloorBits,
                                        const QString &minuteKey,
                                        QString *error)
{
    if (scheduleBits.size() != 128
            || currentFloorBits.size() != 128
            || minuteKey.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("定时执行状态错误");
        return false;
    }
    if (!DbStore::transactionBegin()) {
        if (error) *error = DbStore::lastError();
        return false;
    }
    const bool stateSaved = DbStore::setConfig(
                QStringLiteral("online_v1_floor_schedule_bits"),
                QString::fromLatin1(scheduleBits));
    const bool currentStateSaved = stateSaved && DbStore::setConfig(
                QStringLiteral("online_v1_floor_limit_bits"),
                QString::fromLatin1(currentFloorBits));
    const bool markerSaved = currentStateSaved && DbStore::setConfig(
                QStringLiteral("online_v1_floor_schedule_last_minute"),
                minuteKey);
    if (!markerSaved || !DbStore::transactionCommit()) {
        const QString dbError = DbStore::lastError();
        DbStore::transactionRollback();
        if (error) *error = dbError;
        return false;
    }
    DbStore::checkpoint();
    return true;
}

} // namespace

/** @brief 从下载请求关键字段生成任务去重键。 */
static QString recordedDownloadTaskKey(const QJsonObject &dataObj)
{
    const QString host = dataObj.value("ftpPath").toString().trimmed().toLower();
    const QJsonValue portValue = dataObj.value("ftpPort");
    const QString port = portValue.isDouble()
            ? QString::number(portValue.toInt())
            : portValue.toString().trimmed();
    const QString user = dataObj.value("ftpUser").toString().trimmed();
    const QString fileName = QFileInfo(dataObj.value("videoPath").toString().trimmed()).fileName();
    return QString("%1|%2|%3|%4")
            .arg(host)
            .arg(port)
            .arg(user)
            .arg(fileName);
}

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


/**
 * @brief 从标准 mqttd msg 对象中提取 topic 与 payload JSON 对象。
 *
 * 支持以下两种 payload 形式：
 * - payload 本身就是 JSON object；
 * - payload 为字符串，且字符串内容为 JSON object。
 *
 * @param obj        mqttd 标准消息对象。
 * @param topicOut   输出参数，返回消息 topic。
 * @param payloadOut 输出参数，返回解析后的 payload JSON 对象。
 * @return true  成功提取 payload 对象。
 * @return false payload 不存在、格式不合法或解析失败。
 */
bool MqttManager::extractPayloadObjectFromMsgObject(const QJsonObject &obj,
                                                    QString &topicOut,
                                                    QJsonObject &payloadOut)
{
    topicOut.clear();
    payloadOut = QJsonObject{};

    topicOut = obj.value("topic").toString().trimmed();
    const QJsonValue pv = obj.value("payload");

    // payload 已经是对象，直接返回
    if (pv.isObject()) {
        payloadOut = pv.toObject();
        return true;
    }

    // payload 是字符串时，尝试二次解析 JSON 对象
    if (pv.isString()) {
        const QString payloadStr = pv.toString().trimmed();
        if (!payloadStr.startsWith('{') || !payloadStr.endsWith('}')) {
            emit logMessage("msg payload 不是 JSON object 字符串，已忽略");
            return false;
        }

        QJsonParseError e;
        QJsonDocument doc = QJsonDocument::fromJson(payloadStr.toUtf8(), &e);
        if (e.error != QJsonParseError::NoError || !doc.isObject()) {
            emit logMessage(QString("msg payload 二次解析失败：%1").arg(e.errorString()));
            return false;
        }

        payloadOut = doc.object();
        return true;
    }

    emit logMessage("msg payload 不是 object/string，已忽略");
    return false;
}

/** @brief 去重并用配置文件约定的分隔符连接主题列表。 */
static QString joinTopicsForIni(QStringList topics)
{
    QStringList out;
    for (QString t : topics) {
        t = t.trimmed();
        if (!t.isEmpty()) out << t;
    }
    out.removeDuplicates();
    return out.join(" ; ");
}

/** @brief 将当前 MQTT 路由字段持久化到网络 INI。 */
bool MqttManager::persistCurrentMqttRouteToIni(QString *err) const
{
    if (err) err->clear();

    QDir dir(Rk3566Platform::netConfigDir());
    if (!dir.exists() && !dir.mkpath(".")) {
        if (err) *err = "无法创建平台配置目录";
        return false;
    }

    const QString cfgPath = Rk3566Platform::netConfigPath();

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    ini.beginGroup("mqtt");

    // 公共配置
    ini.setValue("use_config", true);

    if (!cfg_.ipcPath.trimmed().isEmpty())
        ini.setValue("ipc_path", cfg_.ipcPath.trimmed());

    if (!cfg_.clientId.trimmed().isEmpty())
        ini.setValue("client_id", cfg_.clientId.trimmed());

    if (!cfg_.protocolMode.trimmed().isEmpty())
        ini.setValue("protocol_mode", cfg_.protocolMode.trimmed());

    ini.setValue("port", cfg_.port);
    ini.setValue("keepalive", cfg_.keepalive);
    ini.setValue("qos", cfg_.qos);
    ini.setValue("tls", cfg_.tls);

    const QString host = cfg_.host.trimmed();
    const QString username = cfg_.username.trimmed();
    const QString password = cfg_.password;
    const QString subTopicsText = joinTopicsForIni(cfg_.subTopics);
    const QString subscribeTopic = cfg_.subscribeTopic.trimmed();
    const QString publishTopic = cfg_.publishTopic.trimmed();
    QString streamUrlTopic = cfg_.streamUrlTopic.trimmed();

    // online_v1：如果 streamUrlTopic 为空，兜底用 subscribeTopic
    if (streamUrlTopic.isEmpty() && publishTopic.isEmpty()) {
        streamUrlTopic = subscribeTopic;
    }

    // 清理旧公共键，避免老逻辑串入
    ini.remove("host");
    ini.remove("sub_topics");
    ini.remove("publish_topic");
    ini.remove("subscribe_topic");
    ini.remove("stream_url_topic");
    ini.remove("username");
    ini.remove("password");
//    ini.remove("publish_topic_online_v1");

    // 按 topic 规则判断当前是哪条链路：
    // online_v1:
    //   sub  : device/ycLinux/<id>/request
    //   pub  : device/ycLinux/<id>/event
    // online_v2:
    //   sub  : device/yc/<id>/responses
    //   pub  : device/yc/<id>/event（旧 IC 上行保持不变）
    const bool isOnlineV1 = publishTopic.contains(QStringLiteral("device/ycLinux/"));
    const bool isOnlineV2 = publishTopic.contains(QStringLiteral("device/yc/"));

    if (isOnlineV1) {
        if (!subTopicsText.isEmpty())
            ini.setValue("sub_topics_online_v1", subTopicsText);

        if (!publishTopic.isEmpty())
            ini.setValue("publish_topic_online_v1", publishTopic);

        if (!username.isEmpty())
            ini.setValue("username_online_v1", username);

        ini.setValue("password_online_v1", password);

    } else if (isOnlineV2) {
        if (!subTopicsText.isEmpty())
            ini.setValue("sub_topics_online_v2", subTopicsText);

        if (!publishTopic.isEmpty())
            ini.setValue("publish_topic_online_v2", publishTopic);

        if (!username.isEmpty())
            ini.setValue("username_online_v2", username);

        ini.setValue("password_online_v2", password);
    } else {
        if (err) *err = "当前 cfg_ 无法判断 online_v1 / online_v2 的 topic 链路";
        ini.endGroup();
        return false;
    }

    ini.endGroup();
    ini.sync();

    if (ini.status() != QSettings::NoError) {
        if (err) *err = QString("写入 %1 失败").arg(cfgPath);
        return false;
    }

    return true;
}



/**
 * @brief 构造 MqttManager 对象。
 *
 * 创建 IPC 客户端与业务服务对象，并建立各类状态、日志及业务信号转发关系。
 *
 * @param parent 父对象指针。
 */
MqttManager::MqttManager(QObject *parent)
    : QObject(parent)
{
    // 创建 IPC 通信对象与 MQTT 业务服务对象
    ipc_ = new MqttIpcClient(this);
    service_ = new MqttService(this);
    icGateway_ = new IcMqttGateway(this);
    messageRouter_ = new MqttMessageRouter(this);
    registerBuiltinMessageHandlers();
    const bool networkStoreReady = networkPersonnelStore_.initialize();
    if (!networkStoreReady) {
        qWarning() << "[PERSONNEL-SYNC] unified database initialization failed";
    } else {
        // 等待融合应用的 FaceGate 模块完成信号连接后，装载已有网络人员图库。
        QTimer::singleShot(0, this, []() {
            FaceImageSyncBridge::instance()->requestNetworkFaceGalleryRefresh();
        });
    }

    personnelSyncThread_ = new QThread(this);
    personnelSyncWorker_ = new QObject();
    personnelSyncStore_ = new NetworkPersonnelStore();
    personnelSyncWorker_->moveToThread(personnelSyncThread_);
    connect(personnelSyncThread_, &QThread::finished,
            personnelSyncWorker_, &QObject::deleteLater);
    personnelSyncThread_->start();

    networkFaceRefreshDebounceTimer_ = new QTimer(this);
    networkFaceRefreshDebounceTimer_->setSingleShot(true);
    networkFaceRefreshDebounceTimer_->setInterval(
                networkFaceRefreshDebounceMs_);
    connect(networkFaceRefreshDebounceTimer_, &QTimer::timeout,
            this, &MqttManager::flushNetworkFaceRefresh);

    networkFaceRefreshMaxTimer_ = new QTimer(this);
    networkFaceRefreshMaxTimer_->setSingleShot(true);
    networkFaceRefreshMaxTimer_->setInterval(networkFaceRefreshMaxMs_);
    connect(networkFaceRefreshMaxTimer_, &QTimer::timeout,
            this, &MqttManager::flushNetworkFaceRefresh);

    faceUploadThread_ = new QThread(this);
    faceUploadWorker_ = new QObject();
    faceUploadWorker_->moveToThread(faceUploadThread_);
    connect(faceUploadThread_, &QThread::finished,
            faceUploadWorker_, &QObject::deleteLater);
    faceUploadThread_->start();

    onlineQrV1Timer_ = new QTimer(this);
    onlineQrV1Timer_->setSingleShot(true);
    onlineQrV1Timer_->setInterval(onlineQrV1TimeoutMs_);
    connect(onlineQrV1Timer_, &QTimer::timeout,
            this, &MqttManager::onOnlineV1QrTimeout);

    floorScheduleTimer_ = new QTimer(this);
    floorScheduleTimer_->setInterval(floorScheduleCheckIntervalMs_);
    connect(floorScheduleTimer_, &QTimer::timeout,
            this, &MqttManager::onOnlineV1FloorScheduleTick);
    floorScheduleTimer_->start();

    faceImageReconnectResponseTimer_ = new QTimer(this);
    faceImageReconnectResponseTimer_->setSingleShot(true);
    faceImageReconnectResponseTimer_->setInterval(
                faceImageReconnectResponseTimeoutMs_);
    connect(faceImageReconnectResponseTimer_, &QTimer::timeout,
            this, [this]() {
        const QStringList personIds =
                faceImageReconnectAwaitingPersons_.values();
        faceImageReconnectAwaitingPersons_.clear();
        for (const QString &personId : personIds) {
            const auto it = pendingFaceImageRequests_.constFind(personId);
            if (it == pendingFaceImageRequests_.cend()
                    || !it.value().awaitingResponse) {
                continue;
            }
            const QJsonObject requestPayload = it.value().requestPayload;
            const QString message = QStringLiteral(
                        "重连补发人脸图像请求后10秒内未收到响应，图像同步失败。");
            const bool submitted = publishFaceImagesResult(
                        requestPayload,
                        personId,
                        false,
                        message,
                        QJsonArray());
            emit logMessage(QStringLiteral(
                                "重连补发人脸图像响应超时：personId=%1 "
                                "Imagesresult=%2")
                            .arg(personId,
                                 submitted ? QStringLiteral("submitted")
                                           : QStringLiteral("failed")));
        }
    });

    IcEventBridge *icBridge = IcEventBridge::instance();
    mqttReconnectRequiredAfterNetworkLoss_ = !icBridge->networkAvailable();
    connect(icBridge, &IcEventBridge::networkAvailabilityChanged,
            this, [this](bool available) {
        if (!available) {
            mqttReconnectRequiredAfterNetworkLoss_ = true;
            emit logMessage(QStringLiteral(
                                "网络已断开，后续成功通行将计入离线批量记录"));
            return;
        }

        emit logMessage(QStringLiteral(
                            "网络已恢复，等待MQTT重新确认连接后补报离线通行"));
        if (ipc_ && ipc_->isConnected()) {
            ipc_->sendRawLine("{\"cmd\":\"status\"}\n");
        }
    }, Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::onlineV1QrScanned,
            this, &MqttManager::onOnlineV1QrScanned,
            Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::faceAccessFinished,
            this, &MqttManager::onOnlineV1FaceAccessFinished,
            Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::passwordAccessFinished,
            this, &MqttManager::onOnlineV1PasswordAccessFinished,
            Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::faceUploadRequested,
            this, &MqttManager::onOnlineV1FaceUploadRequested,
            Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::cardAccessFinished,
            this, &MqttManager::onOnlineV1CardAccessFinished,
            Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::rs485SendFinished,
            this, &MqttManager::onOnlineV1QrRs485Finished,
            Qt::QueuedConnection);
    connect(icBridge, &IcEventBridge::rs485SendFinished,
            this, &MqttManager::onOnlineV1RemoteCallRs485Finished,
            Qt::QueuedConnection);

    // IPC 自动重连
    ipcReconnectTimer_ = new QTimer(this);
    ipcReconnectTimer_->setInterval(ipcReconnectIntervalMs_);
    ipcReconnectTimer_->setSingleShot(false);

    connect(ipcReconnectTimer_, &QTimer::timeout, this, [this]() {
        if (!ipcAutoReconnect_) return;
        if (!ipc_) return;

        if (ipc_->state() == QLocalSocket::ConnectedState) {
            stopIpcReconnect();
            return;
        }

        emit logMessage(QString("IPC 自动重连中：%1").arg(cfg_.ipcPath));
        connectAndApply();
    });

    // 转发 IPC 连接状态与接收事件
    connect(ipc_, &MqttIpcClient::connected,
            this, &MqttManager::onIpcConnected);
    connect(ipc_, &MqttIpcClient::disconnected,
            this, &MqttManager::onIpcDisconnected);
    connect(ipc_, &MqttIpcClient::frameReceived,
            this, &MqttManager::onIpcFrameReceived);
    connect(ipc_, &MqttIpcClient::errorOccurred,
            this, &MqttManager::onIpcError);
    connect(ipc_, &MqttIpcClient::logMessage,
            this, &MqttManager::logMessage);

    // 转发业务服务日志
    connect(service_, &MqttService::logMessage,
            this, &MqttManager::logMessage);
    connect(icGateway_, &IcMqttGateway::logMessage,
            this, &MqttManager::logMessage);
    connect(icGateway_, &IcMqttGateway::publishPacket,
            this, [this](const QJsonObject &packet, const QString &tag) {
        publishJsonPacket(packet, tag);
    });

    // 转发业务服务产生的业务事件
    connect(service_, &MqttService::mqttVolumeReceived,
            this, &MqttManager::mqttVolumeReceived);
    connect(service_, &MqttService::stopLiveRequested,
            this, &MqttManager::stopLiveRequested);
    connect(service_, &MqttService::liveStreamReady,
            this, &MqttManager::liveStreamReady);
    connect(service_, &MqttService::ftpDownloadTaskReady,
            this, &MqttManager::ftpDownloadTaskReady);
    connect(service_, &MqttService::stopAllPlayRequested,
            this, &MqttManager::stopAllPlayRequested);
    connect(service_, &MqttService::volumeControlResultReady,
            this, &MqttManager::onVolumeControlResultReady);
    connect(service_, &MqttService::resumeRecordedPlaybackRequested,
            this, &MqttManager::resumeRecordedPlaybackRequested);

    FaceImageSyncBridge *faceSyncBridge = FaceImageSyncBridge::instance();
    connect(faceSyncBridge, &FaceImageSyncBridge::personnelListRequested,
            this, [this, faceSyncBridge]() {
        if (!networkPersonnelStore_.initialize()) {
            faceSyncBridge->reportSyncStatus(
                        false,
                        QStringLiteral("网络人员数据库未就绪"));
            return;
        }
        faceSyncBridge->reportPersonnelList(
                    networkPersonnelStore_.networkPersonnelList());
    });
    connect(faceSyncBridge,
            &FaceImageSyncBridge::storedFaceValidationFinished,
            this,
            &MqttManager::onStoredFaceValidationFinished,
            Qt::QueuedConnection);
}

MqttManager::~MqttManager()
{
    if (personnelSyncThread_) {
        personnelSyncThread_->quit();
        personnelSyncThread_->wait();
    }
    personnelSyncWorker_ = nullptr;
    delete personnelSyncStore_;
    personnelSyncStore_ = nullptr;
    if (faceUploadThread_) {
        faceUploadThread_->quit();
        faceUploadThread_->wait();
    }
    faceUploadWorker_ = nullptr;
}

/** @brief 注册一个可处理一组 method 的扩展处理器。 */
bool MqttManager::registerMessageHandler(IMqttMessageHandler *handler)
{
    if (!messageRouter_) {
        return false;
    }

    const bool registered = messageRouter_->registerHandler(handler);
    if (!registered) {
        emit logMessage(QString("MQTT handler registration failed: group=%1 methods=%2")
                        .arg(handler ? handler->group() : QStringLiteral("<null>"),
                             handler ? handler->methods().join(",") : QString()));
    }
    return registered;
}

/** @brief 注册媒体服务和 IC 网关等内置处理器。 */
void MqttManager::registerBuiltinMessageHandlers()
{
    if (!messageRouter_) {
        return;
    }

    // Preserve the original priority: IC methods are offered to the IC gateway first.
    messageRouter_->registerInterceptor(
                QStringLiteral("ic"),
                this,
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &) {
        return icGateway_
                && icGateway_->isEnabled()
                && icGateway_->handleIncoming(topic, payload);
    });

    auto *multimediaHandler =
            new MqttCallbackMessageHandler(QStringLiteral("multimedia"),
                                           messageRouter_);
    multimediaHandler->addMethod(
                QStringLiteral("getPlayFileList"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleGetPlayFileList(topic, payload, method);
    });

    multimediaHandler->addMethod(
                QStringLiteral("getPlayInfo"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleGetPlayInfo(topic, payload, method);
    });

    multimediaHandler->addMethod(
                QStringLiteral("deletePlayFile"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleDeletePlayFile(topic, payload, method);
    });

    multimediaHandler->addMethod(
                QStringLiteral("videoControl"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &) {
        return handleVideoControl(topic, payload);
    });
    registerMessageHandler(multimediaHandler);

    auto *systemHandler =
            new MqttCallbackMessageHandler(QStringLiteral("system"),
                                           messageRouter_);
    systemHandler->addMethod(
                QStringLiteral("linuxReboot"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleLinuxReboot(topic, payload, method);
    });
    registerMessageHandler(systemHandler);

    auto *personnelSyncHandler =
            new MqttCallbackMessageHandler(QStringLiteral("personnel_sync"),
                                           messageRouter_);
    const QStringList syncMethods = {
        QStringLiteral("sync.fullPersonnel"),
        QStringLiteral("sync.checkAllPersonHash"),
        QStringLiteral("sync.deletePersonnel")
    };
    for (const QString &syncMethod : syncMethods) {
        personnelSyncHandler->addMethod(
                    syncMethod,
                    [this](const QString &topic,
                           const QJsonObject &payload,
                           const QString &method) {
            return handlePersonnelSync(topic, payload, method);
        });
    }
    registerMessageHandler(personnelSyncHandler);

    auto *faceImageHandler =
            new MqttCallbackMessageHandler(QStringLiteral("face_images"),
                                           messageRouter_);
    faceImageHandler->addMethod(
                QStringLiteral("face.responseImages"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleFaceImages(topic, payload, method);
    });
    registerMessageHandler(faceImageHandler);

    auto *remoteCallHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_remote_call"),
                                           messageRouter_);
    remoteCallHandler->addMethod(
                QStringLiteral("remoteCall"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleOnlineV1RemoteCall(topic, payload, method);
    });
    remoteCallHandler->addMethod(
                QStringLiteral("remoteCall.deductResult"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleOnlineV1RemoteCallDeductResult(topic, payload, method);
    });
    registerMessageHandler(remoteCallHandler);

    auto *floorLimitHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_floor_limit"),
                                           messageRouter_);
    const QStringList floorLimitMethods = {
        QStringLiteral("floorLimit"),
        QStringLiteral("floorLimit.query")
    };
    for (const QString &floorLimitMethod : floorLimitMethods) {
        floorLimitHandler->addMethod(
                    floorLimitMethod,
                    [this](const QString &topic,
                           const QJsonObject &payload,
                           const QString &method) {
            return handleOnlineV1FloorLimit(topic, payload, method);
        });
    }
    registerMessageHandler(floorLimitHandler);

    auto *floorScheduleHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_floor_schedule"),
                                           messageRouter_);
    const QStringList floorScheduleMethods = {
        QStringLiteral("floorSchedule"),
        QStringLiteral("floorSchedule.query"),
        QStringLiteral("floorSchedule.delete")
    };
    for (const QString &floorScheduleMethod : floorScheduleMethods) {
        floorScheduleHandler->addMethod(
                    floorScheduleMethod,
                    [this](const QString &topic,
                           const QJsonObject &payload,
                           const QString &method) {
            return handleOnlineV1FloorSchedule(topic, payload, method);
        });
    }
    registerMessageHandler(floorScheduleHandler);

    auto *elevatorModeHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_elevator_mode"),
                                           messageRouter_);
    const QStringList elevatorModeMethods = {
        QStringLiteral("elevatorMode"),
        QStringLiteral("elevatorMode.query")
    };
    for (const QString &elevatorModeMethod : elevatorModeMethods) {
        elevatorModeHandler->addMethod(
                    elevatorModeMethod,
                    [this](const QString &topic,
                           const QJsonObject &payload,
                           const QString &method) {
            return handleOnlineV1ElevatorMode(topic, payload, method);
        });
    }
    registerMessageHandler(elevatorModeHandler);

    auto *heartbeatHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_heartbeat"),
                                           messageRouter_);
    heartbeatHandler->addMethod(
                QStringLiteral("heartbeat"),
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &method) {
        return handleOnlineV1Heartbeat(topic, payload, method);
    });
    registerMessageHandler(heartbeatHandler);

    auto *onlineQrHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_qr"),
                                           messageRouter_);
    const QStringList onlineQrMethods = {
        QStringLiteral("qr.scanResult"),
        QStringLiteral("qr.floorControl"),
        QStringLiteral("qr.deductResult")
    };
    for (const QString &onlineQrMethod : onlineQrMethods) {
        onlineQrHandler->addMethod(
                    onlineQrMethod,
                    [this](const QString &topic,
                           const QJsonObject &payload,
                           const QString &method) {
            return handleOnlineV1QrMessage(topic, payload, method);
        });
    }
    registerMessageHandler(onlineQrHandler);

    auto *accessDeductHandler =
            new MqttCallbackMessageHandler(QStringLiteral("online_v1_access_deduct"),
                                           messageRouter_);
    const QStringList accessDeductMethods = {
        QStringLiteral("card.deductResult"),
        QStringLiteral("ic.deductResult"),
        QStringLiteral("face.deductResult"),
        QStringLiteral("access.deductResult"),
        QStringLiteral("deductResult")
    };
    for (const QString &deductMethod : accessDeductMethods) {
        accessDeductHandler->addMethod(
                    deductMethod,
                    [this](const QString &topic,
                           const QJsonObject &payload,
                           const QString &method) {
            return handleOnlineV1AccessDeductResult(topic, payload, method);
        });
    }
    registerMessageHandler(accessDeductHandler);

    // Stream URL messages are identified by topic and may not contain a method.
    messageRouter_->registerFallbackHandler(
                QStringLiteral("multimedia"),
                this,
                [this](const QString &topic,
                       const QJsonObject &payload,
                       const QString &) {
        return handleStreamUrlMessage(topic, payload);
    });
}

bool MqttManager::handleOnlineV1FloorLimit(const QString &topic,
                                           const QJsonObject &payloadObj,
                                           const QString &method)
{
    if (method != QStringLiteral("floorLimit")
            && method != QStringLiteral("floorLimit.query")) {
        return false;
    }

    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (messageDeviceId.isEmpty()
            || (!expectedDeviceId.isEmpty() && messageDeviceId != expectedDeviceId)) {
        emit logMessage(QStringLiteral(
                            "floorLimit 下行 deviceId 不匹配: method=%1 deviceId=%2")
                        .arg(method, messageDeviceId));
        return true;
    }
    if (messageId.isEmpty()) {
        emit logMessage(QStringLiteral("floorLimit 下行参数错误: method=%1")
                        .arg(method));
        return true;
    }

    if (method == QStringLiteral("floorLimit.query")) {
        const QByteArray state = onlineV1FloorLimitBits();
        publishOnlineV1Event(QStringLiteral("floorLimit"),
                             floorLimitResponseData(state,
                                                    allTrafficFloors(),
                                                    0),
                             messageId,
                             QStringLiteral("floorLimit.query"));
        emit logMessage(QStringLiteral("floorLimit.query 已回报当前128位楼层状态"));
        return true;
    }

    if (!payloadObj.value(QStringLiteral("data")).isObject()) {
        emit logMessage(QStringLiteral("floorLimit 下行参数错误: method=%1")
                        .arg(method));
        return true;
    }

    const QJsonObject requestData = payloadObj.value(QStringLiteral("data")).toObject();
    const QJsonValue floorsValue = requestData.value(QStringLiteral("floors"));
    const QJsonArray floorsArray = floorsValue.toArray();
    QList<FloorLimitAction> actions;
    QList<int> responseFloors;
    QHash<int, bool> actionsByFloor;
    QString validationError;

    if (!floorsValue.isArray() || floorsArray.isEmpty()) {
        validationError = QStringLiteral("floors 必须是非空数组");
    }
    for (const QJsonValue &value : floorsArray) {
        if (!validationError.isEmpty()) break;
        if (!value.isObject()) {
            validationError = QStringLiteral("floors 元素必须是对象");
            break;
        }
        const QJsonObject item = value.toObject();
        const QJsonValue floorValue = item.value(QStringLiteral("floor"));
        const QJsonValue actionValue = item.value(QStringLiteral("action"));
        const double floorNumber = floorValue.toDouble();
        const double actionNumber = actionValue.toDouble();
        if (!floorValue.isDouble() || std::floor(floorNumber) != floorNumber
                || floorNumber < -8.0 || floorNumber > 120.0
                || floorNumber == 0.0) {
            validationError = QStringLiteral("floor 超出范围（允许 -8~-1、1~120）");
            break;
        }
        if (!actionValue.isDouble() || std::floor(actionNumber) != actionNumber
                || (actionNumber != 0.0 && actionNumber != 1.0)) {
            validationError = QStringLiteral("action 只能为 0 或 1");
            break;
        }

        const int floor = static_cast<int>(floorNumber);
        const bool open = static_cast<int>(actionNumber) == 1;
        if (actionsByFloor.contains(floor)) {
            if (actionsByFloor.value(floor) != open) {
                validationError = QStringLiteral("同一楼层存在冲突 action");
            }
            continue;
        }
        actionsByFloor.insert(floor, open);
        FloorLimitAction action;
        action.floor = floor;
        action.open = open;
        actions.append(action);
        responseFloors.append(floor);
    }

    if (!validationError.isEmpty()) {
        const QByteArray state = onlineV1FloorLimitBits();
        publishOnlineV1Event(QStringLiteral("floorLimit"),
                             floorLimitResponseData(state, responseFloors, -1),
                             messageId,
                             QStringLiteral("floorLimit"));
        emit logMessage(QStringLiteral("floorLimit 下行参数错误: %1")
                        .arg(validationError));
        return true;
    }

    QByteArray state = onlineV1FloorLimitBits();
    for (const FloorLimitAction &action : actions) {
        const int bit = onlineV1FloorBit(action.floor);
        state[bit] = action.open ? '1' : '0';
    }

    int responseCode = 0;
    QString saveError;
    if (!saveOnlineV1FloorLimitBits(state, &saveError)) {
        responseCode = -1;
        state = onlineV1FloorLimitBits();
        emit logMessage(QStringLiteral("floorLimit 状态保存失败: %1")
                        .arg(saveError));
    }

    if (responseCode == 0) {
        const QByteArray rs485Payload = onlineV1FloorLimitBatchFrame(
                    state);
        const QString sourceTag = QStringLiteral("onlineV1FloorLimit:%1:batch")
                .arg(messageId);
        IcEventBridge::instance()->requestRs485Send(rs485Payload, sourceTag);
        emit logMessage(QStringLiteral(
                            "floorLimit -> RS485全量帧: floors=%1 len=%2 frame=%3")
                        .arg(actions.size())
                        .arg(rs485Payload.size())
                        .arg(QString::fromLatin1(rs485Payload.toHex(' '))));
    }

    publishOnlineV1Event(QStringLiteral("floorLimit"),
                         floorLimitResponseData(state,
                                                responseFloors,
                                                responseCode),
                         messageId,
                         QStringLiteral("floorLimit"));
    return true;
}

bool MqttManager::handleOnlineV1FloorSchedule(const QString &topic,
                                              const QJsonObject &payloadObj,
                                              const QString &method)
{
    if (method != QStringLiteral("floorSchedule")
            && method != QStringLiteral("floorSchedule.query")
            && method != QStringLiteral("floorSchedule.delete")) {
        return false;
    }

    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (messageDeviceId.isEmpty()
            || (!expectedDeviceId.isEmpty() && messageDeviceId != expectedDeviceId)) {
        emit logMessage(QStringLiteral(
                            "floorSchedule 下行 deviceId 不匹配: method=%1 deviceId=%2")
                        .arg(method, messageDeviceId));
        return true;
    }
    if (messageId.isEmpty()) {
        emit logMessage(QStringLiteral("floorSchedule 下行缺少 id"));
        return true;
    }

    auto publishSchedules = [this, &messageId](int code,
                                               const QJsonArray &schedules,
                                               const QString &message) {
        QJsonObject data;
        data.insert(QStringLiteral("code"), code);
        data.insert(QStringLiteral("schedules"), schedules);
        if (!message.trimmed().isEmpty()) {
            data.insert(QStringLiteral("message"), message.trimmed());
        }
        publishOnlineV1Event(QStringLiteral("floorSchedule"),
                             data,
                             messageId,
                             QStringLiteral("floorSchedule"));
    };

    if (method == QStringLiteral("floorSchedule.query")) {
        publishSchedules(200, onlineV1FloorSchedules(), QString());
        emit logMessage(QStringLiteral("floorSchedule.query 已回报当前定时规则"));
        return true;
    }

    if (method == QStringLiteral("floorSchedule.delete")) {
        const QJsonValue dataValue = payloadObj.value(QStringLiteral("data"));
        if (!dataValue.isObject()) {
            const QString error = QStringLiteral(
                        "floorSchedule.delete data 必须是对象");
            publishSchedules(400, onlineV1FloorSchedules(), error);
            emit logMessage(error);
            return true;
        }

        const QJsonValue hashListValue = dataValue.toObject().value(
                    QStringLiteral("hashList"));
        if (!hashListValue.isArray() || hashListValue.toArray().isEmpty()) {
            const QString error = QStringLiteral("hashList 必须是非空数组");
            publishSchedules(400, onlineV1FloorSchedules(), error);
            emit logMessage(QStringLiteral("floorSchedule.delete 参数错误: %1")
                            .arg(error));
            return true;
        }

        QSet<QString> requestedHashes;
        for (const QJsonValue &hashValue : hashListValue.toArray()) {
            const QString hash = hashValue.toString().trimmed();
            if (!hashValue.isString() || hash.isEmpty()) {
                const QString error = QStringLiteral(
                            "hashList 元素必须是非空字符串");
                publishSchedules(400, onlineV1FloorSchedules(), error);
                emit logMessage(QStringLiteral(
                                    "floorSchedule.delete 参数错误: %1")
                                .arg(error));
                return true;
            }
            requestedHashes.insert(hash);
        }

        const QJsonArray oldSchedules = onlineV1FloorSchedules();
        QJsonArray remainingSchedules;
        QJsonArray deletedHashes;
        QSet<QString> deletedHashSet;
        for (const QJsonValue &scheduleValue : oldSchedules) {
            const QString hash = scheduleValue.toObject()
                    .value(QStringLiteral("hash")).toString().trimmed();
            if (!hash.isEmpty() && requestedHashes.contains(hash)) {
                if (!deletedHashSet.contains(hash)) {
                    deletedHashSet.insert(hash);
                    deletedHashes.append(hash);
                }
                continue;
            }
            remainingSchedules.append(scheduleValue);
        }

        if (deletedHashes.isEmpty()) {
            publishSchedules(200, oldSchedules, QString());
            emit logMessage(QStringLiteral(
                                "floorSchedule.delete 未找到匹配hash，按幂等成功处理"));
            return true;
        }

        const QByteArray oldScheduleBits = onlineV1FloorScheduleBits();
        QByteArray remainingScheduleBits(128, '0');
        const QSet<int> remainingFloors = onlineV1FloorScheduleFloors(
                    remainingSchedules);
        for (int floor : remainingFloors) {
            const int bit = onlineV1FloorBit(floor);
            remainingScheduleBits[bit] = oldScheduleBits.at(bit);
        }

        QString saveError;
        if (!saveOnlineV1FloorSchedules(remainingSchedules,
                                        remainingScheduleBits,
                                        false,
                                        &saveError)) {
            const QString error = saveError.trimmed().isEmpty()
                    ? QStringLiteral("定时规则删除保存失败") : saveError;
            publishSchedules(500, onlineV1FloorSchedules(), error);
            emit logMessage(QStringLiteral("floorSchedule.delete 保存失败: %1")
                            .arg(error));
            return true;
        }

        publishSchedules(200, remainingSchedules, QString());
        emit logMessage(QStringLiteral(
                            "floorSchedule.delete 删除完成: deleted=%1 remaining=%2")
                        .arg(deletedHashes.size())
                        .arg(remainingSchedules.size()));
        return true;
    }

    if (!payloadObj.value(QStringLiteral("data")).isObject()) {
        const QString error = QStringLiteral("floorSchedule data 必须是对象");
        publishSchedules(400, onlineV1FloorSchedules(), error);
        emit logMessage(error);
        return true;
    }

    QJsonArray normalizedSchedules;
    QString validationError;
    if (!normalizeOnlineV1FloorSchedules(
                payloadObj.value(QStringLiteral("data")).toObject(),
                &normalizedSchedules,
                &validationError)) {
        publishSchedules(400, onlineV1FloorSchedules(), validationError);
        emit logMessage(QStringLiteral("floorSchedule 参数错误: %1")
                        .arg(validationError));
        return true;
    }

    const QJsonArray oldSchedules = onlineV1FloorSchedules();
    const QByteArray normalizedJson = QJsonDocument(normalizedSchedules)
            .toJson(QJsonDocument::Compact);
    const QByteArray oldJson = QJsonDocument(oldSchedules)
            .toJson(QJsonDocument::Compact);
    if (normalizedJson == oldJson) {
        publishSchedules(200, oldSchedules, QString());
        emit logMessage(QStringLiteral(
                            "floorSchedule 内容未变化，保留当前执行分钟标记"));
        return true;
    }

    const QSet<int> newScheduleFloors = onlineV1FloorScheduleFloors(
                normalizedSchedules);
    const QByteArray currentFloorBits = onlineV1FloorLimitBits();
    QByteArray newScheduleBits(128, '0');
    for (int floor : newScheduleFloors) {
        const int bit = onlineV1FloorBit(floor);
        // 保存规则不改变硬件；定时状态从设备当前状态开始，等待下一个命中时间覆盖。
        newScheduleBits[bit] = currentFloorBits.at(bit);
    }

    QString saveError;
    if (!saveOnlineV1FloorSchedules(normalizedSchedules,
                                    newScheduleBits,
                                    true,
                                    &saveError)) {
        const QString error = saveError.trimmed().isEmpty()
                ? QStringLiteral("定时规则保存失败") : saveError;
        publishSchedules(500, onlineV1FloorSchedules(), error);
        emit logMessage(QStringLiteral("floorSchedule 保存失败: %1").arg(error));
        return true;
    }

    publishSchedules(200, normalizedSchedules, QString());
    emit logMessage(QStringLiteral("floorSchedule 已全量覆盖: rules=%1 floors=%2")
                    .arg(normalizedSchedules.size())
                    .arg(newScheduleFloors.size()));

    // 新规则若恰好命中当前分钟，应立即执行，不等待下一个5秒周期。
    onOnlineV1FloorScheduleTick();
    return true;
}

bool MqttManager::handleOnlineV1ElevatorMode(const QString &topic,
                                             const QJsonObject &payloadObj,
                                             const QString &method)
{
    if (method != QStringLiteral("elevatorMode")
            && method != QStringLiteral("elevatorMode.query")) {
        return false;
    }

    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (messageDeviceId.isEmpty()
            || (!expectedDeviceId.isEmpty() && messageDeviceId != expectedDeviceId)) {
        emit logMessage(QStringLiteral(
                            "elevatorMode 下行 deviceId 不匹配: method=%1 deviceId=%2")
                        .arg(method, messageDeviceId));
        return true;
    }
    if (messageId.isEmpty()) {
        emit logMessage(QStringLiteral("elevatorMode 下行缺少 id"));
        return true;
    }

    auto publishState = [this, &messageId](
            int code,
            const Rs485FloorFrameBuilder::ElevatorModeState &state,
            const QString &message) {
        QJsonObject data;
        data.insert(QStringLiteral("code"), code);
        data.insert(QStringLiteral("masterSwitch"), state.masterSwitch ? 1 : 0);
        data.insert(QStringLiteral("delay"), state.delay ? 1 : 0);
        data.insert(QStringLiteral("parking"), state.parking ? 1 : 0);
        data.insert(QStringLiteral("driver"), state.driver ? 1 : 0);
        data.insert(QStringLiteral("independent"), state.independent ? 1 : 0);
        data.insert(QStringLiteral("ext1"), state.ext1 ? 1 : 0);
        data.insert(QStringLiteral("ext2"), state.ext2 ? 1 : 0);
        data.insert(QStringLiteral("ext3"), state.ext3 ? 1 : 0);
        data.insert(QStringLiteral("ext4"), state.ext4 ? 1 : 0);
        if (!message.trimmed().isEmpty()) {
            data.insert(QStringLiteral("message"), message.trimmed());
        }
        publishOnlineV1Event(QStringLiteral("elevatorMode"),
                             data,
                             messageId,
                             QStringLiteral("elevatorMode"));
    };

    if (method == QStringLiteral("elevatorMode.query")) {
        publishState(200, Rs485FloorFrameBuilder::elevatorModeState(), QString());
        emit logMessage(QStringLiteral("elevatorMode.query 已回报当前模式状态"));
        return true;
    }

    const QJsonValue dataValue = payloadObj.value(QStringLiteral("data"));
    if (!dataValue.isObject()) {
        const QString error = QStringLiteral("elevatorMode data 必须是对象");
        publishState(400, Rs485FloorFrameBuilder::elevatorModeState(), error);
        emit logMessage(error);
        return true;
    }

    const QJsonObject data = dataValue.toObject();
    const QStringList fields = {
        QStringLiteral("masterSwitch"),
        QStringLiteral("delay"),
        QStringLiteral("parking"),
        QStringLiteral("driver"),
        QStringLiteral("independent"),
        QStringLiteral("ext1"),
        QStringLiteral("ext2"),
        QStringLiteral("ext3"),
        QStringLiteral("ext4")
    };
    QHash<QString, int> switches;
    QString validationError;
    for (const QString &field : fields) {
        int value = 0;
        if (!jsonIntegerInRange(data.value(field), 0, 1, &value)) {
            validationError = QStringLiteral("%1 只能为整数0或1").arg(field);
            break;
        }
        switches.insert(field, value);
    }
    if (!validationError.isEmpty()) {
        publishState(400, Rs485FloorFrameBuilder::elevatorModeState(),
                     validationError);
        emit logMessage(QStringLiteral("elevatorMode 参数错误: %1")
                        .arg(validationError));
        return true;
    }

    Rs485FloorFrameBuilder::ElevatorModeState state;
    state.masterSwitch = switches.value(QStringLiteral("masterSwitch")) == 1;
    state.delay = switches.value(QStringLiteral("delay")) == 1;
    state.parking = switches.value(QStringLiteral("parking")) == 1;
    state.driver = switches.value(QStringLiteral("driver")) == 1;
    state.independent = switches.value(QStringLiteral("independent")) == 1;
    state.ext1 = switches.value(QStringLiteral("ext1")) == 1;
    state.ext2 = switches.value(QStringLiteral("ext2")) == 1;
    state.ext3 = switches.value(QStringLiteral("ext3")) == 1;
    state.ext4 = switches.value(QStringLiteral("ext4")) == 1;

    QString saveError;
    if (!Rs485FloorFrameBuilder::saveElevatorModeState(state, &saveError)) {
        const QString error = saveError.trimmed().isEmpty()
                ? QStringLiteral("电梯模式保存失败") : saveError;
        publishState(500, Rs485FloorFrameBuilder::elevatorModeState(), error);
        emit logMessage(QStringLiteral("elevatorMode 保存失败: %1").arg(error));
        return true;
    }

    // 保存后立即下发完整当前状态；串口最终出口会在总开关开启时覆盖高8位，
    // 总开关关闭时则原样恢复113～120层的楼层权限。
    const QByteArray frame = onlineV1FloorLimitBatchFrame(
                onlineV1FloorLimitBits());
    const QString sourceTag = QStringLiteral("onlineV1ElevatorMode:%1")
            .arg(messageId);
    IcEventBridge::instance()->requestRs485Send(frame, sourceTag);
    emit logMessage(QStringLiteral(
                        "elevatorMode -> RS485: masterSwitch=%1 frame=%2")
                    .arg(state.masterSwitch ? 1 : 0)
                    .arg(QString::fromLatin1(frame.toHex(' '))));

    publishState(200, state, QString());
    return true;
}

void MqttManager::onOnlineV1FloorScheduleTick()
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) return;

    const QJsonArray schedules = onlineV1FloorSchedules();
    if (schedules.isEmpty()) return;

    const QDateTime now = QDateTime::currentDateTime();
    const QString minuteKey = now.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QString lastMinute = DbStore::getConfig(
                QStringLiteral("online_v1_floor_schedule_last_minute"),
                QString()).toString().trimmed();
    if (lastMinute == minuteKey) return;

    const QString currentTime = now.time().toString(QStringLiteral("HH:mm"));
    const int currentWeekDay = now.date().dayOfWeek();
    QHash<int, bool> actionsByFloor;
    for (const QJsonValue &scheduleValue : schedules) {
        const QJsonObject schedule = scheduleValue.toObject();
        if (schedule.value(QStringLiteral("time")).toString() != currentTime) {
            continue;
        }

        bool weekDayMatched = false;
        for (const QJsonValue &weekDayValue :
             schedule.value(QStringLiteral("weekDays")).toArray()) {
            if (weekDayValue.toInt() == currentWeekDay) {
                weekDayMatched = true;
                break;
            }
        }
        if (!weekDayMatched) continue;

        const bool open = schedule.value(QStringLiteral("action")).toInt() == 1;
        for (const QJsonValue &floorValue :
             schedule.value(QStringLiteral("floors")).toArray()) {
            actionsByFloor.insert(floorValue.toInt(), open);
        }
    }
    if (actionsByFloor.isEmpty()) return;

    QList<int> floors = actionsByFloor.keys();
    std::sort(floors.begin(), floors.end());
    QByteArray scheduleBits = onlineV1FloorScheduleBits();
    QByteArray currentFloorBits = onlineV1FloorLimitBits();
    for (int floor : floors) {
        const int bit = onlineV1FloorBit(floor);
        const char state = actionsByFloor.value(floor) ? '1' : '0';
        scheduleBits[bit] = state;
        // 到达定时时间后，定时动作成为该楼层最新的设备当前状态。
        currentFloorBits[bit] = state;
    }

    QString saveError;
    if (!saveOnlineV1FloorScheduleExecution(scheduleBits,
                                            currentFloorBits,
                                            minuteKey,
                                            &saveError)) {
        emit logMessage(QStringLiteral("floorSchedule 到时状态保存失败: %1")
                        .arg(saveError));
        return;
    }

    const QByteArray frame = onlineV1FloorLimitBatchFrame(currentFloorBits);
    const QString sourceTag = QStringLiteral("onlineV1FloorSchedule:%1:batch")
            .arg(minuteKey);
    IcEventBridge::instance()->requestRs485Send(frame, sourceTag);
    emit logMessage(QStringLiteral(
                        "floorSchedule 到时执行全量帧: minute=%1 floors=%2 len=%3 frame=%4")
                    .arg(minuteKey)
                    .arg(floors.size())
                    .arg(frame.size())
                    .arg(QString::fromLatin1(frame.toHex(' '))));
}

bool MqttManager::handleOnlineV1Heartbeat(const QString &topic,
                                          const QJsonObject &payloadObj,
                                          const QString &method)
{
    if (method != QStringLiteral("heartbeat")) return false;

    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (messageDeviceId.isEmpty()
            || (!expectedDeviceId.isEmpty() && messageDeviceId != expectedDeviceId)) {
        emit logMessage(QStringLiteral("heartbeat 下行 deviceId 不匹配: %1")
                        .arg(messageDeviceId));
        return true;
    }
    if (messageId.isEmpty() || !payloadObj.value(QStringLiteral("data")).isObject()) {
        emit logMessage(QStringLiteral("heartbeat 下行参数错误"));
        return true;
    }

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QJsonObject data;
    data.insert(QStringLiteral("code"), 200);
    data.insert(QStringLiteral("time"), now);
    publishOnlineV1Event(QStringLiteral("heartbeat"),
                         data,
                         messageId,
                         QStringLiteral("heartbeat"),
                         now);
    return true;
}

/**
 * @brief 处理 online_v1 的单楼层远程呼梯指令。
 *
 * remoteCall.floor 是 JSON 整数楼层号，不兼容 online_v2 openFloor 的
 * "#@003" 字符串格式。校验完成后仍复用公共的 36 字节楼层位图构造器。
 */
bool MqttManager::handleOnlineV1RemoteCall(const QString &topic,
                                           const QJsonObject &payloadObj,
                                           const QString &method)
{
    if (method != QStringLiteral("remoteCall")) return false;

    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    const QJsonObject data = payloadObj.value(QStringLiteral("data")).toObject();
    const QString personId = data.value(QStringLiteral("personId"))
            .toString().trimmed();
    const QString source = data.value(QStringLiteral("source"))
            .toString().trimmed().isEmpty()
            ? QStringLiteral("platform")
            : data.value(QStringLiteral("source")).toString().trimmed();

    if (messageId.isEmpty() || messageDeviceId.isEmpty()
            || (!expectedDeviceId.isEmpty() && messageDeviceId != expectedDeviceId)
            || data.isEmpty() || personId.isEmpty()) {
        emit logMessage(QStringLiteral(
                            "remoteCall 参数错误，未下发RS485: id=%1 deviceId=%2 personId=%3")
                        .arg(messageId, messageDeviceId, personId));
        return true;
    }

    const QJsonValue floorValue = data.value(QStringLiteral("floor"));
    if (!floorValue.isDouble()) {
        emit logMessage(QStringLiteral(
                            "remoteCall.floor 必须是整数，未下发RS485: id=%1")
                        .arg(messageId));
        return true;
    }

    const double floorNumber = floorValue.toDouble();
    if (!std::isfinite(floorNumber)
            || std::floor(floorNumber) != floorNumber
            || floorNumber == 0.0
            || floorNumber < -8.0
            || floorNumber > 120.0) {
        emit logMessage(QStringLiteral(
                            "remoteCall.floor 越界或不是整数，未下发RS485: id=%1 floor=%2")
                        .arg(messageId)
                        .arg(floorNumber, 0, 'g', 15));
        return true;
    }

    const int floor = static_cast<int>(floorNumber);
    const bool modeFloorOccupied =
            Rs485FloorFrameBuilder::shouldRejectModeOccupiedFloors(
                QList<int>{floor});
    QString buildError;
    const QByteArray frame = Rs485FloorFrameBuilder::buildSingleFloor(
                floor, &buildError);
    if (frame.isEmpty()) {
        emit logMessage(QStringLiteral(
                            "remoteCall 楼层帧构造失败，未下发RS485: id=%1 floor=%2 reason=%3")
                        .arg(messageId)
                        .arg(floor)
                        .arg(buildError));
        return true;
    }

    const NetworkRemoteCallContext transaction =
            networkPersonnelStore_.beginRemoteCallTransaction(
                messageId, personId, floor, source);
    if (!transaction.ok) {
        emit logMessage(QStringLiteral(
                            "remoteCall 事务创建失败，未下发RS485: id=%1 reason=%2")
                        .arg(messageId, transaction.error));
        return true;
    }
    if (transaction.duplicate) {
        emit logMessage(QStringLiteral(
                            "重复 remoteCall 已忽略，不重复下发RS485: id=%1")
                        .arg(messageId));
        return true;
    }

    const QString sourceTag = QStringLiteral("onlineV1RemoteCall:%1")
            .arg(messageId);
    if (modeFloorOccupied) {
        const QString reason = QStringLiteral("楼层已被电梯模式占用");
        emit logMessage(QStringLiteral(
                            "remoteCall 已拒绝，未下发RS485: id=%1 floor=%2 reason=%3")
                        .arg(messageId)
                        .arg(floor)
                        .arg(reason));
        onOnlineV1RemoteCallRs485Finished(sourceTag, false, reason);
        return true;
    }

    IcEventBridge::instance()->requestRs485Send(frame, sourceTag);
    emit logMessage(QStringLiteral(
                        "remoteCall -> RS485请求: id=%1 personId=%2 floor=%3 frame=%4")
                    .arg(messageId, personId)
                    .arg(floor)
                    .arg(QString::fromLatin1(frame.toHex(' '))));
    return true;
}

/** @brief 以 RS485 驱动的实际完成结果判定远程呼梯状态，不产生 MQTT 回包。 */
void MqttManager::onOnlineV1RemoteCallRs485Finished(const QString &sourceTag,
                                                    bool success,
                                                    const QString &reason)
{
    const QString prefix = QStringLiteral("onlineV1RemoteCall:");
    if (!sourceTag.startsWith(prefix)) return;

    const QString messageId = sourceTag.mid(prefix.size());
    {
        const NetworkRemoteCallContext context =
                networkPersonnelStore_.recordRemoteCallRs485Result(
                    messageId, success, reason.trimmed());
        if (!context.ok) {
            emit logMessage(QStringLiteral(
                                "remoteCall RS485结果保存失败: id=%1 reason=%2")
                            .arg(messageId, context.error));
            return;
        }
        if (context.duplicate) {
            emit logMessage(QStringLiteral(
                                "重复 remoteCall RS485完成通知已忽略: id=%1")
                            .arg(messageId));
            return;
        }

        QJsonObject eventData;
        eventData.insert(QStringLiteral("personId"), context.personId);
        eventData.insert(QStringLiteral("floor"), context.floor);
        eventData.insert(QStringLiteral("success"), success);
        eventData.insert(QStringLiteral("source"), context.source);
        if (!success && !reason.trimmed().isEmpty()) {
            eventData.insert(QStringLiteral("reason"), reason.trimmed());
        }
        const QString accessResultId = QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
        const bool published = publishOnlineV1Event(
                    QStringLiteral("remoteCall.accessResult"), eventData,
                    accessResultId, QStringLiteral("remote-call-access-result"));

        QString dbError;
        if (!networkPersonnelStore_.recordRemoteCallAccessResult(
                    messageId, accessResultId, success, published, &dbError)) {
            emit logMessage(QStringLiteral(
                                "remoteCall.accessResult 状态保存失败: requestId=%1 reason=%2")
                            .arg(messageId, dbError));
        }
        emit logMessage(QStringLiteral(
                            "remoteCall.accessResult %1: requestId=%2 eventId=%3 "
                            "personId=%4 floor=%5 success=%6 localDeducted=%7 remainingCount=%8 reason=%9")
                        .arg(published ? QStringLiteral("已提交") : QStringLiteral("提交失败"),
                             messageId, accessResultId, context.personId)
                        .arg(context.floor)
                        .arg(success ? QStringLiteral("true") : QStringLiteral("false"),
                             context.localDeducted ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(context.remainingCount)
                        .arg(reason.trimmed()));
        return;
    }
#if 0
    if (success) {
        emit logMessage(QStringLiteral(
                            "remoteCall RS485发送完成，呼梯成功: id=%1")
                        .arg(messageId));
        return;
    }

    emit logMessage(QStringLiteral(
                        "remoteCall RS485发送失败: id=%1 reason=%2")
                    .arg(messageId,
                         reason.trimmed().isEmpty()
                         ? QStringLiteral("未知错误") : reason.trimmed()));
}

/** @brief 处理网络人员同步并把结果发往 ycLinux request 对应的 event 主题。 */
#endif
}
bool MqttManager::handleOnlineV1RemoteCallDeductResult(
        const QString &topic,
        const QJsonObject &payloadObj,
        const QString &method)
{
    if (method != QStringLiteral("remoteCall.deductResult")) return false;
    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (!messageDeviceId.isEmpty() && !expectedDeviceId.isEmpty()
            && messageDeviceId != expectedDeviceId) {
        emit logMessage(QStringLiteral(
                            "remoteCall.deductResult 设备不匹配，已忽略: expected=%1 actual=%2")
                        .arg(expectedDeviceId, messageDeviceId));
        return true;
    }

    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QJsonObject data = payloadObj.value(QStringLiteral("data")).toObject();
    const QString personId = data.value(QStringLiteral("personId"))
            .toString().trimmed();
    const QJsonValue floorValue = data.value(QStringLiteral("floor"));
    const double floorNumber = floorValue.toDouble(0.0);
    if (messageId.isEmpty() || personId.isEmpty() || !floorValue.isDouble()
            || !std::isfinite(floorNumber) || std::floor(floorNumber) != floorNumber
            || floorNumber == 0.0 || floorNumber < -8.0 || floorNumber > 120.0) {
        emit logMessage(QStringLiteral(
                            "remoteCall.deductResult 参数错误: id=%1 personId=%2 floor=%3")
                        .arg(messageId, personId)
                        .arg(floorNumber, 0, 'g', 15));
        return true;
    }

    const int code = data.value(QStringLiteral("code")).toInt(-1);
    const bool deducted = data.value(QStringLiteral("deducted")).toBool(false);
    const QString message = data.value(QStringLiteral("message"))
            .toString().trimmed();
    auto numericVariant = [&data](const QString &key) {
        const QJsonValue value = data.value(key);
        return value.isDouble() ? value.toVariant() : QVariant();
    };
    const NetworkRemoteCallDeductApplyResult result =
            networkPersonnelStore_.applyRemoteCallDeductResult(
                messageId, personId, static_cast<int>(floorNumber), code,
                deducted, message,
                numericVariant(QStringLiteral("remainingCount")),
                numericVariant(QStringLiteral("usedCount")),
                numericVariant(QStringLiteral("passRemainingCount")),
                numericVariant(QStringLiteral("passUsedCount")));
    if (!result.ok) {
        emit logMessage(QStringLiteral(
                            "remoteCall.deductResult 处理失败: id=%1 personId=%2 floor=%3 reason=%4")
                        .arg(messageId, personId)
                        .arg(static_cast<int>(floorNumber))
                        .arg(result.error));
        return true;
    }
    if (result.duplicate) {
        emit logMessage(QStringLiteral(
                            "重复 remoteCall.deductResult 已忽略: id=%1 requestId=%2")
                        .arg(messageId, result.requestId));
    } else if (result.remoteCountApplied) {
        emit logMessage(QStringLiteral(
                            "remoteCall.deductResult 已确认并同步本地次数: "
                            "requestId=%1 personId=%2 floor=%3 remainingCount=%4 usedCount=%5 "
                            "passRemainingCount=%6 passUsedCount=%7")
                        .arg(result.requestId, personId)
                        .arg(static_cast<int>(floorNumber))
                        .arg(result.remainingCount)
                        .arg(result.usedCount)
                        .arg(result.passRemainingCount)
                        .arg(result.passUsedCount));
    } else {
        emit logMessage(QStringLiteral(
                            "remoteCall.deductResult 平台未确认扣减，已清除待确认标志并保留本地次数: "
                            "requestId=%1 code=%2 deducted=%3 localCountPreserved=true message=%4")
                        .arg(result.requestId)
                        .arg(code)
                        .arg(deducted ? QStringLiteral("true") : QStringLiteral("false"),
                             message));
    }
    return true;
}

bool MqttManager::handlePersonnelSync(const QString &topic,
                                      const QJsonObject &payloadObj,
                                      const QString &method)
{
    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active =
            cfg_.publishTopic.trimmed().startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest =
            requestTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) {
        emit logMessage(QStringLiteral(
                            "忽略非 online_v1 的人员同步消息: method=%1 topic=%2 publishTopic=%3")
                        .arg(method, requestTopic, cfg_.publishTopic));
        return false;
    }

    emit logMessage(QStringLiteral(
                        "人员同步消息开始处理: method=%1 id=%2 topic=%3")
                    .arg(method,
                         payloadObj.value(QStringLiteral("id")).toString(),
                         requestTopic));

    if (method != QStringLiteral("sync.fullPersonnel")
            && method != QStringLiteral("sync.checkAllPersonHash")
            && method != QStringLiteral("sync.deletePersonnel")) {
        return false;
    }

    if (!personnelSyncWorker_ || !personnelSyncThread_
            || !personnelSyncThread_->isRunning() || !personnelSyncStore_) {
        NetworkPersonnelSyncResult result;
        if (method == QStringLiteral("sync.fullPersonnel")) {
            result = networkPersonnelStore_.applyFullPersonnel(payloadObj);
        } else if (method == QStringLiteral("sync.checkAllPersonHash")) {
            result = networkPersonnelStore_.checkAllPersonHashes(payloadObj);
        } else {
            result = networkPersonnelStore_.deletePersonnel(payloadObj);
        }
        completePersonnelSync(requestTopic, payloadObj, method, result);
        return true;
    }

    NetworkPersonnelStore *workerStore = personnelSyncStore_;
    QMetaObject::invokeMethod(
                personnelSyncWorker_,
                [this, workerStore, requestTopic, payloadObj, method]() {
        NetworkPersonnelSyncResult result;
        if (method == QStringLiteral("sync.fullPersonnel")) {
            result = workerStore->applyFullPersonnel(payloadObj);
        } else if (method == QStringLiteral("sync.checkAllPersonHash")) {
            result = workerStore->checkAllPersonHashes(payloadObj);
        } else {
            result = workerStore->deletePersonnel(payloadObj);
        }
        QMetaObject::invokeMethod(
                    this,
                    [this, requestTopic, payloadObj, method, result]() {
            completePersonnelSync(requestTopic, payloadObj, method, result);
        },
        Qt::QueuedConnection);
    },
    Qt::QueuedConnection);
    return true;
}

void MqttManager::completePersonnelSync(
        const QString &requestTopic,
        const QJsonObject &payloadObj,
        const QString &method,
        const NetworkPersonnelSyncResult &result)
{

    QString responseTopic = requestTopic;
    if (responseTopic.endsWith(QStringLiteral("/request"))) {
        responseTopic.chop(QStringLiteral("/request").size());
        responseTopic += QStringLiteral("/event");
    } else {
        responseTopic += QStringLiteral("/event");
    }

    QJsonObject responseData = result.data;
    responseData.insert(
                QStringLiteral("resultcode"),
                static_cast<int>(result.ok
                                 ? PersonnelSyncResultCode::Success
                                 : PersonnelSyncResultCode::Failure));

    QJsonObject responsePayload;
    responsePayload.insert(QStringLiteral("method"), method);
    responsePayload.insert(QStringLiteral("id"),
                           payloadObj.value(QStringLiteral("id")).toString());
    responsePayload.insert(QStringLiteral("deviceId"),
                           payloadObj.value(QStringLiteral("deviceId")).toString());
    responsePayload.insert(QStringLiteral("data"), responseData);
    responsePayload.insert(QStringLiteral("time"),
                           QDateTime::currentDateTime().toString(
                               QStringLiteral("yyyy-MM-dd HH:mm:ss")));

    QJsonObject packet;
    packet.insert(QStringLiteral("cmd"), QStringLiteral("publish"));
    packet.insert(QStringLiteral("topic"), responseTopic);
    packet.insert(QStringLiteral("payload"), responsePayload);

    const bool published = publishJsonPacket(packet, QStringLiteral("personnel-sync"));
    emit logMessage(QStringLiteral(
                        "人员同步处理完成: method=%1 id=%2 status=%3 code=%4 duplicate=%5 ack=%6")
                    .arg(method,
                         payloadObj.value(QStringLiteral("id")).toString(),
                         result.data.value(QStringLiteral("status")).toString(),
                         result.data.value(QStringLiteral("code")).toString(),
                         result.duplicate ? QStringLiteral("true") : QStringLiteral("false"),
                         published ? QStringLiteral("submitted") : QStringLiteral("failed")));

    // fullPersonnel 回执先提交，再针对该人员自动补拉缺失的人脸原图。
    // HASH_UNCHANGED 也会检查图片文件，便于上次拉取失败后由下一次全量同步重试。
    bool waitingForFaceImages = false;
    if (result.ok && method == QStringLiteral("sync.fullPersonnel")) {
        QString personId = result.data.value(QStringLiteral("personId"))
                .toString().trimmed();
        if (personId.isEmpty()) {
            personId = payloadObj.value(QStringLiteral("data")).toObject()
                    .value(QStringLiteral("personId")).toString().trimmed();
        }
        if (!personId.isEmpty()) {
            waitingForFaceImages = !networkPersonnelStore_
                    .pendingFaceImageRequest(personId).isEmpty();
            bool faceWorkInProgress = faceImageWorkActive_
                    && currentFaceImageWork_.personId == personId;
            if (!faceWorkInProgress) {
                for (const PendingFaceImageWork &work : pendingFaceImageWork_) {
                    if (work.personId == personId) {
                        faceWorkInProgress = true;
                        break;
                    }
                }
            }
            if (waitingForFaceImages && faceWorkInProgress) {
                emit logMessage(QStringLiteral(
                                    "重复fullPersonnel已回执，该人员图片正在处理，"
                                    "不重复发送face.requestImages: personId=%1")
                                .arg(personId));
            } else {
                requestFaceImagesForPerson(personId);
            }
        }
    }
    if (result.ok) {
        if (method == QStringLiteral("sync.deletePersonnel")) {
            flushNetworkFaceRefresh();
        } else if (method == QStringLiteral("sync.fullPersonnel")
                   && !waitingForFaceImages) {
            scheduleNetworkFaceRefresh();
        }
    }
}

void MqttManager::scheduleNetworkFaceRefresh()
{
    if (!networkFaceRefreshDebounceTimer_ || !networkFaceRefreshMaxTimer_) {
        flushNetworkFaceRefresh();
        return;
    }
    networkFaceRefreshDebounceTimer_->start(networkFaceRefreshDebounceMs_);
    if (!networkFaceRefreshMaxTimer_->isActive()) {
        networkFaceRefreshMaxTimer_->start(networkFaceRefreshMaxMs_);
    }
}

void MqttManager::flushNetworkFaceRefresh()
{
    if (networkFaceRefreshDebounceTimer_) {
        networkFaceRefreshDebounceTimer_->stop();
    }
    if (networkFaceRefreshMaxTimer_) {
        networkFaceRefreshMaxTimer_->stop();
    }
    FaceImageSyncBridge *bridge = FaceImageSyncBridge::instance();
    bridge->reportPersonnelList(networkPersonnelStore_.networkPersonnelList());
    bridge->requestNetworkFaceGalleryRefresh();
    emit logMessage(QStringLiteral("网络人员批次刷新已执行"));
}

bool MqttManager::publishOnlineV1QrEvent(const QString &method,
                                         const QJsonObject &data,
                                         const QString &messageId,
                                         const QString &tag)
{
    return publishOnlineV1Event(method, data, messageId, tag);
}

bool MqttManager::publishOnlineV1Event(const QString &method,
                                       const QJsonObject &data,
                                       const QString &messageId,
                                       const QString &tag,
                                       const QString &eventTime)
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active || messageId.trimmed().isEmpty()) {
        emit logMessage(QStringLiteral("online_v1 event 发布被忽略: method=%1 topic=%2")
                        .arg(method, eventTopic));
        return false;
    }

    const QString deviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    QJsonObject payload;
    payload.insert(QStringLiteral("method"), method);
    payload.insert(QStringLiteral("id"), messageId.trimmed());
    payload.insert(QStringLiteral("deviceId"), deviceId);
    payload.insert(QStringLiteral("data"), data);
    const QString payloadTime = eventTime.trimmed().isEmpty()
            ? QDateTime::currentDateTime().toString(
                  QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : eventTime.trimmed();
    payload.insert(QStringLiteral("time"), payloadTime);

    QJsonObject packet;
    packet.insert(QStringLiteral("cmd"), QStringLiteral("publish"));
    packet.insert(QStringLiteral("topic"), eventTopic);
    packet.insert(QStringLiteral("payload"), payload);
    return publishJsonPacket(packet, tag);
}

bool MqttManager::recordOfflineAccessResult(const QString &method,
                                            const QString &personId)
{
    QString dbError;
    const bool recorded = networkPersonnelStore_.recordOfflineAccessResult(
                method, personId, true, &dbError);
    emit logMessage(QStringLiteral(
                        "MQTT断线通行%1: method=%2 personId=%3 reason=%4")
                    .arg(recorded ? QStringLiteral("已累计")
                                  : QStringLiteral("累计失败"),
                         method,
                         personId.trimmed(),
                         dbError));
    return recorded;
}

bool MqttManager::shouldRecordOfflineAccess() const
{
    return !mqttBrokerConnected_
            || mqttReconnectRequiredAfterNetworkLoss_
            || !IcEventBridge::instance()->networkAvailable();
}

void MqttManager::flushOfflineAccessResults()
{
    if (shouldRecordOfflineAccess()) return;

    const QStringList methods = {
        QStringLiteral("card.accessResult"),
        QStringLiteral("qr.accessResult"),
        QStringLiteral("face.accessResult"),
        QStringLiteral("password.accessResult")
    };
    for (const QString &method : methods) {
        QString dbError;
        const QJsonArray records =
                networkPersonnelStore_.pendingOfflineAccessResults(method, &dbError);
        if (!dbError.isEmpty()) {
            emit logMessage(QStringLiteral("读取离线通行记录失败: method=%1 reason=%2")
                            .arg(method, dbError));
            continue;
        }
        if (records.isEmpty()) continue;

        QJsonObject data;
        data.insert(QStringLiteral("records"), records);
        const QString messageId = QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
        const bool submitted = publishOnlineV1Event(
                    method,
                    data,
                    messageId,
                    QStringLiteral("offline-access-result"));
        if (!submitted) {
            emit logMessage(QStringLiteral(
                                "离线通行批量补报提交失败，保留本地记录: "
                                "method=%1 id=%2 records=%3")
                            .arg(method, messageId)
                            .arg(records.size()));
            continue;
        }

        if (!networkPersonnelStore_.consumeOfflineAccessResults(
                    method, records, &dbError)) {
            emit logMessage(QStringLiteral(
                                "离线通行批量补报已提交，但消费本地快照失败: "
                                "method=%1 id=%2 reason=%3")
                            .arg(method, messageId, dbError));
            continue;
        }
        emit logMessage(QStringLiteral(
                            "离线通行批量补报已提交: method=%1 id=%2 records=%3")
                        .arg(method, messageId)
                        .arg(records.size()));
    }
}

void MqttManager::onOnlineV1FaceAccessFinished(
        const QString &personId,
        const QString &faceHash,
        const QString &floors,
        const QByteArray &rs485Frame,
        bool success,
        const QString &reason)
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) {
        return;
    }

    if (shouldRecordOfflineAccess()) {
        if (success && !personId.trimmed().isEmpty()) {
            // 人脸链路已在发出完成信号前原子扣减本地次数/金额，避免断线分支重复扣减。
            recordOfflineAccessResult(
                        QStringLiteral("face.accessResult"), personId);
        }
        return;
    }

    Q_UNUSED(rs485Frame)

    QJsonObject data;
    data.insert(QStringLiteral("personId"), personId);
    data.insert(QStringLiteral("faceHash"), faceHash);
    data.insert(QStringLiteral("success"), success);
    if (!reason.trimmed().isEmpty()) {
        data.insert(QStringLiteral("reason"), reason.trimmed());
    }

    const QString messageId = QUuid::createUuid().toString(
                QUuid::WithoutBraces);
    const bool published = publishOnlineV1Event(
                QStringLiteral("face.accessResult"), data, messageId,
                QStringLiteral("face-access-result"));
    emit logMessage(QStringLiteral(
                        "人脸控梯事件%1: method=face.accessResult personId=%2 floors=%3 result=%4 reason=%5")
                    .arg(published ? QStringLiteral("已提交") : QStringLiteral("提交失败"),
                         personId, floors,
                         success ? QStringLiteral("true") : QStringLiteral("false"),
                         reason));
}

void MqttManager::onOnlineV1PasswordAccessFinished(
        const QString &personId,
        const QString &floors,
        const QByteArray &rs485Frame,
        bool success,
        const QString &reason)
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active || personId.trimmed().isEmpty()) {
        return;
    }

    if (shouldRecordOfflineAccess()) {
        if (success) {
            // 密码链路已在发出完成信号前扣减本地次数/金额，此处只累计补报次数。
            recordOfflineAccessResult(
                        QStringLiteral("password.accessResult"), personId);
        }
        return;
    }

    Q_UNUSED(rs485Frame)

    QJsonObject data;
    data.insert(QStringLiteral("personId"), personId.trimmed());
    data.insert(QStringLiteral("success"), success);
    if (!success && !reason.trimmed().isEmpty()) {
        data.insert(QStringLiteral("reason"), reason.trimmed());
    }

    const QString messageId = QUuid::createUuid().toString(
                QUuid::WithoutBraces);
    const bool published = publishOnlineV1Event(
                QStringLiteral("password.accessResult"), data, messageId,
                QStringLiteral("password-access-result"));
    emit logMessage(QStringLiteral(
                        "密码控梯事件%1: method=password.accessResult personId=%2 "
                        "floors=%3 result=%4 reason=%5")
                    .arg(published ? QStringLiteral("已提交") : QStringLiteral("提交失败"),
                         personId.trimmed(), floors,
                         success ? QStringLiteral("true") : QStringLiteral("false"),
                         reason));
}

void MqttManager::onOnlineV1FaceUploadRequested(
        const QString &personId,
        const QString &faceHash,
        const QString &snapshotPath,
        bool success)
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) {
        return;
    }

    if (!faceUploadWorker_ || !faceUploadThread_
            || !faceUploadThread_->isRunning()) {
        publishOnlineV1FaceUpload(
                    personId, faceHash, QString(), success,
                    QStringLiteral("人脸抓拍编码线程未就绪"));
        return;
    }

    const QString imagePath = snapshotPath.trimmed();
    QMetaObject::invokeMethod(
                faceUploadWorker_,
                [this, personId, faceHash, imagePath, success]() {
        QString faceImage;
        QString imageError;
        if (imagePath.isEmpty()) {
            imageError = QStringLiteral("抓拍路径为空");
        } else {
            QFile imageFile(imagePath);
            const qint64 maximumImageBytes = 2 * 1024 * 1024;
            const qint64 imageSize = imageFile.size();
            if (imageSize < 0) {
                imageError = QStringLiteral("无法读取抓拍文件信息: %1")
                        .arg(imagePath);
            } else if (imageSize > maximumImageBytes) {
                imageError = QStringLiteral("抓拍文件超过2MiB限制: %1")
                        .arg(imagePath);
            } else if (!imageFile.open(QIODevice::ReadOnly)) {
                imageError = QStringLiteral("打开抓拍文件失败: %1")
                        .arg(imagePath);
            } else {
                const QByteArray imageBytes = imageFile.readAll();
                if (imageBytes.size() != imageSize) {
                    imageError = QStringLiteral("抓拍文件读取不完整: %1")
                            .arg(imagePath);
                } else {
                    faceImage = QString::fromLatin1(imageBytes.toBase64());
                }
            }
        }

        QMetaObject::invokeMethod(
                    this,
                    [this, personId, faceHash, faceImage,
                     success, imageError]() {
            publishOnlineV1FaceUpload(
                        personId, faceHash, faceImage,
                        success, imageError);
        },
        Qt::QueuedConnection);
    },
    Qt::QueuedConnection);
}

void MqttManager::publishOnlineV1FaceUpload(
        const QString &personId,
        const QString &faceHash,
        const QString &faceImage,
        bool success,
        const QString &imageError)
{
    QJsonObject data;
    data.insert(QStringLiteral("personId"), personId);
    data.insert(QStringLiteral("faceHash"), faceHash);
    data.insert(QStringLiteral("faceImage"), faceImage);
    data.insert(QStringLiteral("success"), success);

    const QString messageId = QUuid::createUuid().toString(
                QUuid::WithoutBraces);
    const bool published = publishOnlineV1Event(
                QStringLiteral("face.upload"), data, messageId,
                QStringLiteral("face-upload"));
    emit logMessage(QStringLiteral(
                        "人脸通行抓拍%1: method=face.upload personId=%2 "
                        "faceHash=%3 success=%4 imageBase64Bytes=%5 imageError=%6")
                    .arg(published ? QStringLiteral("已提交")
                                   : QStringLiteral("提交失败"),
                         personId,
                         faceHash,
                         success ? QStringLiteral("true")
                                 : QStringLiteral("false"),
                         QString::number(faceImage.size()),
                         imageError));
}

void MqttManager::onOnlineV1CardAccessFinished(
        const QString &personId,
        const QString &cardId,
        const QString &floors,
        const QByteArray &rs485Frame,
        bool success,
        const QString &reason)
{
    appendCredentialAccessRecord(QStringLiteral("card"), personId,
                                 cardId, success, reason);

    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) {
        return;
    }

    if (shouldRecordOfflineAccess()) {
        if (success && !personId.trimmed().isEmpty()) {
            // 刷卡链路已在发出完成信号前原子扣减本地次数/金额，避免断线分支重复扣减。
            recordOfflineAccessResult(
                        QStringLiteral("card.accessResult"), personId);
        }
        return;
    }

    Q_UNUSED(rs485Frame)

    QJsonObject data;
    data.insert(QStringLiteral("cardId"), cardId);
    data.insert(QStringLiteral("success"), success);
    if (!reason.trimmed().isEmpty()) {
        data.insert(QStringLiteral("reason"), reason.trimmed());
    }

    const QString messageId = QUuid::createUuid().toString(
                QUuid::WithoutBraces);
    const bool published = publishOnlineV1Event(
                QStringLiteral("card.accessResult"), data, messageId,
                QStringLiteral("card-access-result"));
    emit logMessage(QStringLiteral(
                        "刷卡控梯事件%1: method=card.accessResult personId=%2 "
                        "cardId=%3 floors=%4 result=%5 reason=%6")
                    .arg(published ? QStringLiteral("已提交") : QStringLiteral("提交失败"),
                         personId, cardId, floors,
                         success ? QStringLiteral("true") : QStringLiteral("false"),
                         reason));
}

bool MqttManager::handleOnlineV1AccessDeductResult(
        const QString &topic,
        const QJsonObject &payloadObj,
        const QString &method)
{
    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) {
        return false;
    }

    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QString messageDeviceId = payloadObj.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    if (!messageDeviceId.isEmpty() && !expectedDeviceId.isEmpty()
            && messageDeviceId != expectedDeviceId) {
        emit logMessage(QStringLiteral(
                            "%1 设备不匹配，已忽略: expected=%2 actual=%3")
                        .arg(method, expectedDeviceId, messageDeviceId));
        return true;
    }

    const QJsonObject data = payloadObj.value(QStringLiteral("data")).toObject();
    const QString personId = data.value(QStringLiteral("personId"))
            .toString().trimmed();
    const int code = data.value(QStringLiteral("code")).toInt(-1);
    const bool deducted = data.value(QStringLiteral("deducted")).toBool(false);
    QString message = data.value(QStringLiteral("message")).toString().trimmed();
    if (message.isEmpty()) {
        message = data.value(QStringLiteral("reason")).toString().trimmed();
    }
    const QVariant remainingCount =
            data.value(QStringLiteral("remainingCount")).isDouble()
            ? data.value(QStringLiteral("remainingCount")).toVariant()
            : QVariant();
    const QVariant usedCount = data.value(QStringLiteral("usedCount")).isDouble()
            ? data.value(QStringLiteral("usedCount")).toVariant()
            : QVariant();
    const QVariant remainingAmount =
            data.value(QStringLiteral("remainingAmount")).isDouble()
            ? data.value(QStringLiteral("remainingAmount")).toVariant()
            : (data.value(QStringLiteral("passRemainingAmount")).isDouble()
               ? data.value(QStringLiteral("passRemainingAmount")).toVariant()
               : QVariant());
    const QVariant usedAmount = data.value(QStringLiteral("usedAmount")).isDouble()
            ? data.value(QStringLiteral("usedAmount")).toVariant()
            : (data.value(QStringLiteral("passUsedAmount")).isDouble()
               ? data.value(QStringLiteral("passUsedAmount")).toVariant()
               : QVariant());

    const NetworkAccessDeductApplyResult applyResult =
            networkPersonnelStore_.applyAccessDeductResult(
                messageId, method, personId, code, deducted, message,
                remainingCount, usedCount, remainingAmount, usedAmount);
    if (!applyResult.ok) {
        emit logMessage(QStringLiteral("处理 %1 失败: %2")
                        .arg(method, applyResult.error));
        return true;
    }
    if (applyResult.duplicate) {
        emit logMessage(QStringLiteral("重复 %1 已忽略: id=%2")
                        .arg(method, messageId));
    } else if (applyResult.countApplied || applyResult.amountApplied) {
        QStringList synced;
        if (applyResult.countApplied) {
            synced.append(QStringLiteral("次数 remainingCount=%1 usedCount=%2")
                          .arg(applyResult.remainingCount)
                          .arg(applyResult.usedCount));
        }
        if (applyResult.amountApplied) {
            synced.append(QStringLiteral("金额 remainingAmount=%1 usedAmount=%2")
                          .arg(applyResult.remainingAmount, 0, 'f', 6)
                          .arg(applyResult.usedAmount, 0, 'f', 6));
        }
        emit logMessage(QStringLiteral(
                            "%1 已确认，扣减完成并同步本地状态: personId=%2 %3")
                        .arg(method, personId, synced.join(QLatin1Char(' '))));
    } else if (applyResult.skipped && code == 200) {
        emit logMessage(QStringLiteral(
                            "%1 已确认但未重复扣次: personId=%2 deducted=%3 message=%4")
                        .arg(method, personId,
                             deducted ? QStringLiteral("true") : QStringLiteral("false"),
                             message));
    } else {
        emit logMessage(QStringLiteral("%1 未同步本地次数/金额: code=%2 reason=%3")
                        .arg(method)
                        .arg(code)
                        .arg(message));
    }
    return true;
}

void MqttManager::onOnlineV1QrScanned(const QString &qrCode)
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active = eventTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) {
        emit logMessage(QStringLiteral("忽略非 online_v1 扫码事件"));
        return;
    }

    const QString value = qrCode.trimmed();
    if (value.isEmpty()) {
        IcEventBridge::instance()->emitToastFailRequested(
                    QStringLiteral("二维码格式错误"));
        return;
    }
    if (pendingOnlineQr_.active || pendingOfflineQr_.active) {
        IcEventBridge::instance()->emitToastFailRequested(
                    QStringLiteral("二维码正在处理中，请稍候"));
        emit logMessage(QStringLiteral("忽略重复扫码，已有二维码任务正在处理"));
        return;
    }

    if (shouldRecordOfflineAccess()) {
        const NetworkAccessResult access = offlineAccessService_.checkQrCode(
                    value, QDateTime::currentDateTime());
        if (!access.pass) {
            const QString failureReason = access.reason.trimmed().isEmpty()
                    ? QStringLiteral("二维码离线权限校验失败")
                    : access.reason.trimmed();
            emit logMessage(QStringLiteral(
                                "MQTT断线二维码通行拒绝: qrCode=%1 reason=%2")
                            .arg(value, failureReason));
            appendCredentialAccessRecord(QStringLiteral("qr"),
                                         access.personId, value,
                                         false, failureReason);
            IcEventBridge::instance()->emitToastFailRequested(failureReason);
            return;
        }

        pendingOfflineQr_ = PendingOfflineQrAccess{};
        pendingOfflineQr_.active = true;
        pendingOfflineQr_.sourceTag = QStringLiteral("offlineMqttQr:")
                + QUuid::createUuid().toString(QUuid::WithoutBraces);
        pendingOfflineQr_.access = access;
        IcEventBridge::instance()->requestRs485Send(
                    access.rs485Frame, pendingOfflineQr_.sourceTag);
        emit logMessage(QStringLiteral(
                            "MQTT断线二维码已通过本地校验并下发RS485: personId=%1")
                        .arg(access.personId));
        return;
    }

    const QString scanId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString deviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    QString dbError;
    if (!networkPersonnelStore_.beginQrAccessTransaction(
                scanId, deviceId, value, &dbError)) {
        emit logMessage(QStringLiteral("创建二维码扫码事务失败: %1").arg(dbError));
        IcEventBridge::instance()->emitToastFailRequested(
                    QStringLiteral("二维码处理失败"));
        return;
    }

    pendingOnlineQr_ = PendingOnlineQrAccess{};
    pendingOnlineQr_.active = true;
    pendingOnlineQr_.scanId = scanId;
    pendingOnlineQr_.qrCode = value;

    QJsonObject data;
    data.insert(QStringLiteral("qrCode"), value);
    if (!publishOnlineV1QrEvent(QStringLiteral("qr.scan"),
                                data,
                                scanId,
                                QStringLiteral("qr.scan"))) {
        networkPersonnelStore_.finishQrAccessTransaction(
                    scanId,
                    QStringLiteral("SCAN_PUBLISH_FAILED"),
                    QStringLiteral("二维码上报失败"));
        IcEventBridge::instance()->emitToastFailRequested(
                    QStringLiteral("二维码上报失败"));
        clearPendingOnlineV1Qr();
        return;
    }

    onlineQrV1Timer_->start(onlineQrV1TimeoutMs_);
    emit logMessage(QStringLiteral("qr.scan 已提交: scanId=%1 qrCode=%2")
                    .arg(scanId, value));
}

bool MqttManager::handleOnlineV1QrMessage(const QString &topic,
                                          const QJsonObject &payloadObj,
                                          const QString &method)
{
    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    const bool ycLinuxRequest = requestTopic.startsWith(
                QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (!onlineV1Active || !ycLinuxRequest) return false;

    const QString expectedDeviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    const QString messageDeviceId = payloadObj.value(
                QStringLiteral("deviceId")).toString().trimmed();
    if (messageDeviceId.isEmpty()
            || (!expectedDeviceId.isEmpty() && messageDeviceId != expectedDeviceId)) {
        emit logMessage(QStringLiteral("二维码下行 deviceId 不匹配: method=%1 deviceId=%2")
                        .arg(method, messageDeviceId));
        return true;
    }

    const QString messageId = payloadObj.value(QStringLiteral("id"))
            .toString().trimmed();
    const QJsonObject data = payloadObj.value(QStringLiteral("data")).toObject();
    if (messageId.isEmpty() || data.isEmpty()) {
        emit logMessage(QStringLiteral("二维码下行参数错误: method=%1")
                        .arg(method));
        return true;
    }

    if (method == QStringLiteral("qr.scanResult")) {
        if (!pendingOnlineQr_.active) {
            emit logMessage(QStringLiteral("qr.scanResult 无待处理扫码，忽略 id=%1")
                            .arg(messageId));
            return true;
        }

        const int code = data.value(QStringLiteral("code")).toInt(-1);
        const bool success = data.value(QStringLiteral("success")).toBool(false);
        const QString serverReason = data.value(QStringLiteral("reason"))
                .toString().trimmed();
        const QJsonObject authorizationData = data.value(
                    QStringLiteral("data")).toObject();
        const bool accepted = code == 200 && success;
        const QString reason = accepted ? QStringLiteral("通行成功")
                                        : onlineQrFailureReason(code, serverReason);

        bool duplicate = false;
        QString dbError;
        if (!networkPersonnelStore_.recordQrScanResult(
                    pendingOnlineQr_.scanId,
                    messageId,
                    code,
                    accepted,
                    reason,
                    authorizationData,
                    &duplicate,
                    &dbError)) {
            emit logMessage(QStringLiteral("保存 qr.scanResult 失败: %1").arg(dbError));
            return true;
        }
        if (duplicate) {
            emit logMessage(QStringLiteral("重复 qr.scanResult 已忽略: id=%1")
                            .arg(messageId));
            return true;
        }

        if (!accepted) {
            if (pendingOnlineQr_.rs485Requested) {
                emit logMessage(QStringLiteral(
                                    "协议异常：楼层已请求发送后收到拒绝 scanResult，code=%1")
                                .arg(code));
            } else {
                IcEventBridge::instance()->emitToastFailRequested(reason);
                clearPendingOnlineV1Qr();
            }
            return true;
        }

        pendingOnlineQr_.scanAccepted = true;
        emit logMessage(QStringLiteral("qr.scanResult 通行成功，等待/核对 floorControl"));
        return true;
    }

    if (method == QStringLiteral("qr.floorControl")) {
        if (!pendingOnlineQr_.active) {
            emit logMessage(QStringLiteral("qr.floorControl 无待处理扫码，忽略 id=%1")
                            .arg(messageId));
            return true;
        }
        const QString commandQrCode = qrCodeFromObject(data);
        if (commandQrCode != pendingOnlineQr_.qrCode) {
            emit logMessage(QStringLiteral(
                                "qr.floorControl 与当前扫码不匹配，忽略 id=%1")
                            .arg(messageId));
            return true;
        }
        if (pendingOnlineQr_.rs485Requested
                || pendingOnlineQr_.accessResultReported
                || pendingOnlineQr_.floorControlId == messageId) {
            emit logMessage(QStringLiteral("重复 qr.floorControl 已忽略: id=%1")
                            .arg(messageId));
            return true;
        }
        pendingOnlineQr_.floorControlId = messageId;

        NetworkQrAuthorization authorization;
        QByteArray rs485Frame;
        QString validationError;
        if (!parseFloorControl(data, &authorization, &rs485Frame, &validationError)) {
            emit logMessage(QStringLiteral("qr.floorControl 校验失败: %1")
                            .arg(validationError));
            publishOnlineV1QrAccessResult(false, validationError);
            return true;
        }
        pendingOnlineQr_.personId = authorization.personId;

        QList<int> authorizedFloors;
        for (const QString &floor : authorization.floors) {
            authorizedFloors.append(floor.toInt());
        }
        if (Rs485FloorFrameBuilder::shouldRejectModeOccupiedFloors(
                    authorizedFloors)) {
            const QString reason = QStringLiteral("楼层已被电梯模式占用");
            emit logMessage(QStringLiteral(
                                "qr.floorControl 已拒绝，未下发RS485: id=%1 reason=%2")
                            .arg(messageId, reason));
            publishOnlineV1QrAccessResult(false, reason);
            return true;
        }

        bool duplicate = false;
        bool changed = false;
        QString dbError;
        if (!networkPersonnelStore_.saveQrFloorControl(
                    pendingOnlineQr_.scanId,
                    messageId,
                    authorization,
                    &duplicate,
                    &changed,
                    &dbError)) {
            emit logMessage(QStringLiteral("保存二维码楼层授权失败: %1").arg(dbError));
            publishOnlineV1QrAccessResult(false,
                                          QStringLiteral("二维码处理失败"));
            return true;
        }
        if (duplicate) {
            emit logMessage(QStringLiteral("数据库已处理 qr.floorControl: id=%1")
                            .arg(messageId));
            return true;
        }

        pendingOnlineQr_.rs485Requested = true;
        onlineQrV1Timer_->start(onlineQrV1TimeoutMs_);
        const QString sourceTag = QStringLiteral("onlineV1Qr:%1")
                .arg(pendingOnlineQr_.scanId);
        IcEventBridge::instance()->requestRs485Send(rs485Frame, sourceTag);
        emit logMessage(QStringLiteral(
                            "qr.floorControl -> RS485，cacheChanged=%1 frame=%2")
                        .arg(changed ? QStringLiteral("true") : QStringLiteral("false"),
                             QString::fromLatin1(rs485Frame.toHex(' '))));
        return true;
    }

    if (method == QStringLiteral("qr.deductResult")) {
        const QString deductQrCode = qrCodeFromObject(data);
        const int code = data.value(QStringLiteral("code")).toInt(-1);
        const bool deducted = data.value(QStringLiteral("deducted")).toBool(false);
        const QString message = data.value(QStringLiteral("message"))
                .toString().trimmed();
        const QString sourceType = data.value(QStringLiteral("sourceType"))
                .toString().trimmed().toUpper();
        const QString personId = data.value(QStringLiteral("personId"))
                .toString().trimmed();
        const QVariant remainingCount =
                data.value(QStringLiteral("remainingCount")).isDouble()
                ? data.value(QStringLiteral("remainingCount")).toVariant()
                : QVariant();
        const QVariant usedCount = data.value(QStringLiteral("usedCount")).isDouble()
                ? data.value(QStringLiteral("usedCount")).toVariant()
                : QVariant();
        if (deductQrCode.isEmpty()) {
            emit logMessage(QStringLiteral(
                                "qr.deductResult 缺少 qrCode，暂不修改本地次数 id=%1")
                            .arg(messageId));
            return true;
        }

        const NetworkQrDeductApplyResult applyResult =
                networkPersonnelStore_.applyQrDeductResult(
                    messageId,
                    deductQrCode,
                    sourceType,
                    personId,
                    code,
                    deducted,
                    message,
                    remainingCount,
                    usedCount);
        if (!applyResult.ok) {
            emit logMessage(QStringLiteral("处理 qr.deductResult 失败: %1")
                            .arg(applyResult.error));
            return true;
        }
        if (applyResult.duplicate) {
            emit logMessage(QStringLiteral("重复 qr.deductResult 已忽略: id=%1")
                            .arg(messageId));
        } else if (applyResult.countApplied) {
            const QString target = applyResult.sourceType == QStringLiteral("VISITOR")
                    ? QStringLiteral("访客次数") : QStringLiteral("人员规则次数");
            emit logMessage(QStringLiteral(
                                "qr.deductResult 已确认，%1已更新，qrCode=%2 sourceType=%3")
                            .arg(target, deductQrCode, applyResult.sourceType));
        } else if (applyResult.skipped && code == 200) {
            emit logMessage(QStringLiteral(
                                "qr.deductResult 成功处理并跳过扣次：qrCode=%1 "
                                "sourceType=%2 deducted=%3 message=%4")
                            .arg(deductQrCode,
                                 applyResult.sourceType,
                                 deducted ? QStringLiteral("true")
                                          : QStringLiteral("false"),
                                 message));
        } else {
            const QString localReason = applyResult.error.trimmed().isEmpty()
                    ? onlineQrFailureReason(code, message)
                    : applyResult.error.trimmed();
            emit logMessage(QStringLiteral("qr.deductResult 未扣本地次数: code=%1 reason=%2")
                            .arg(code)
                            .arg(localReason));
        }

        if (pendingOnlineQr_.active
                && pendingOnlineQr_.qrCode == deductQrCode) {
            clearPendingOnlineV1Qr();
        }
        return true;
    }

    return false;
}

bool MqttManager::publishOnlineV1QrAccessResult(bool success,
                                                const QString &failureReason)
{
    if (!pendingOnlineQr_.active) return false;
    if (pendingOnlineQr_.accessResultReported) return false;
    pendingOnlineQr_.accessResultReported = true;

    appendCredentialAccessRecord(QStringLiteral("qr"),
                                 pendingOnlineQr_.personId,
                                 pendingOnlineQr_.qrCode,
                                 success, failureReason);

    const QString messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject data;
    data.insert(QStringLiteral("qrCode"), pendingOnlineQr_.qrCode);
    data.insert(QStringLiteral("success"), success);
    if (!success && !failureReason.trimmed().isEmpty()) {
        data.insert(QStringLiteral("reason"), failureReason.trimmed());
    }
    const bool submitted = publishOnlineV1QrEvent(
                QStringLiteral("qr.accessResult"),
                data,
                messageId,
                QStringLiteral("qr.accessResult"));

    QString dbError;
    if (!networkPersonnelStore_.recordQrAccessResult(
                pendingOnlineQr_.scanId,
                messageId,
                success,
                submitted,
                &dbError)) {
        emit logMessage(QStringLiteral("保存 qr.accessResult 状态失败: %1")
                        .arg(dbError));
    }

    if (success) {
        IcEventBridge::instance()->emitToastPassRequested();
    } else {
        IcEventBridge::instance()->emitToastFailRequested(
                    failureReason.trimmed().isEmpty()
                    ? QStringLiteral("设备开启失败") : failureReason.trimmed());
    }
    onlineQrV1Timer_->start(onlineQrV1TimeoutMs_);
    return submitted;
}

void MqttManager::onOnlineV1QrRs485Finished(const QString &sourceTag,
                                            bool success,
                                            const QString &reason)
{
    const QString offlinePrefix = QStringLiteral("offlineMqttQr:");
    if (sourceTag.startsWith(offlinePrefix)) {
        if (!pendingOfflineQr_.active
                || pendingOfflineQr_.sourceTag != sourceTag) {
            emit logMessage(QStringLiteral("收到过期的 MQTT 断线 QR RS485 结果: %1")
                            .arg(sourceTag));
            return;
        }

        const NetworkAccessResult access = pendingOfflineQr_.access;
        pendingOfflineQr_ = PendingOfflineQrAccess{};
        if (!success) {
            const QString failureReason = reason.trimmed().isEmpty()
                    ? QStringLiteral("RS485发送失败") : reason.trimmed();
            emit logMessage(QStringLiteral(
                                "MQTT断线二维码RS485失败: personId=%1 reason=%2")
                            .arg(access.personId, failureReason));
            appendCredentialAccessRecord(QStringLiteral("qr"),
                                         access.personId,
                                         access.credential,
                                         false, failureReason);
            IcEventBridge::instance()->emitToastFailRequested(failureReason);
            return;
        }

        if (!offlineAccessService_.recordSuccessfulAccess(access)) {
            emit logMessage(QStringLiteral(
                                "MQTT断线二维码通行本地次数/金额扣减失败: personId=%1")
                            .arg(access.personId));
        }
        appendCredentialAccessRecord(QStringLiteral("qr"),
                                     access.personId,
                                     access.credential,
                                     true, QString());
        recordOfflineAccessResult(QStringLiteral("qr.accessResult"),
                                  access.personId);
        IcEventBridge::instance()->emitToastPassRequested();
        if (!shouldRecordOfflineAccess()) {
            flushOfflineAccessResults();
        }
        return;
    }

    const QString prefix = QStringLiteral("onlineV1Qr:");
    if (!sourceTag.startsWith(prefix)) return;

    const QString scanId = sourceTag.mid(prefix.size());
    if (!pendingOnlineQr_.active || pendingOnlineQr_.scanId != scanId) {
        emit logMessage(QStringLiteral("收到过期的 online_v1 QR RS485 结果: %1")
                        .arg(sourceTag));
        return;
    }

    pendingOnlineQr_.rs485Finished = true;
    pendingOnlineQr_.rs485Success = success;
    QString dbError;
    if (!networkPersonnelStore_.recordQrRs485Result(
                scanId, success, reason, &dbError)) {
        emit logMessage(QStringLiteral("保存二维码 RS485 结果失败: %1").arg(dbError));
    }

    if (shouldRecordOfflineAccess()) {
        if (success && !pendingOnlineQr_.personId.trimmed().isEmpty()) {
            NetworkAccessResult access;
            access.pass = true;
            access.personId = pendingOnlineQr_.personId.trimmed();
            access.credential = pendingOnlineQr_.qrCode;
            access.credentialType = QStringLiteral("QR_CODE");
            if (!offlineAccessService_.initialize()
                    || !offlineAccessService_.recordSuccessfulAccess(access)) {
                emit logMessage(QStringLiteral(
                                    "MQTT断线二维码通行本地次数/金额扣减失败: personId=%1")
                                .arg(access.personId));
            }
            recordOfflineAccessResult(QStringLiteral("qr.accessResult"),
                                      access.personId);
            IcEventBridge::instance()->emitToastPassRequested();
        } else if (!success) {
            IcEventBridge::instance()->emitToastFailRequested(
                        reason.trimmed().isEmpty()
                        ? QStringLiteral("设备开启失败") : reason.trimmed());
        }
        networkPersonnelStore_.finishQrAccessTransaction(
                    scanId,
                    success ? QStringLiteral("OFFLINE_RECORDED")
                            : QStringLiteral("RS485_FAILED"),
                    reason);
        clearPendingOnlineV1Qr();
        return;
    }
    publishOnlineV1QrAccessResult(
                success,
                reason.trimmed().isEmpty()
                ? QStringLiteral("设备开启失败") : reason.trimmed());
}

void MqttManager::onOnlineV1QrTimeout()
{
    if (!pendingOnlineQr_.active) return;

    QString state;
    QString reason;
    if (pendingOnlineQr_.rs485Requested && !pendingOnlineQr_.rs485Finished) {
        state = QStringLiteral("RS485_TIMEOUT");
        reason = QStringLiteral("RS485发送超时");
        networkPersonnelStore_.finishQrAccessTransaction(
                    pendingOnlineQr_.scanId, state, reason);
        publishOnlineV1QrAccessResult(false, reason);
        clearPendingOnlineV1Qr();
        return;
    }
    if (pendingOnlineQr_.accessResultReported && !pendingOnlineQr_.rs485Finished) {
        state = QStringLiteral("ACCESS_RESULT_WAIT_TIMEOUT");
        reason = QStringLiteral("等待平台扣次结果超时");
        emit logMessage(QStringLiteral("%1: qrCode=%2")
                        .arg(reason, pendingOnlineQr_.qrCode));
        networkPersonnelStore_.finishQrAccessTransaction(
                    pendingOnlineQr_.scanId, state, reason);
        clearPendingOnlineV1Qr();
        return;
    }
    if (pendingOnlineQr_.rs485Finished) {
        state = QStringLiteral("DEDUCT_TIMEOUT");
        reason = QStringLiteral("等待平台扣次结果超时");
        emit logMessage(QStringLiteral("%1: qrCode=%2")
                        .arg(reason, pendingOnlineQr_.qrCode));
    } else {
        state = QStringLiteral("AUTH_TIMEOUT");
        reason = QStringLiteral("二维码验证超时");
        IcEventBridge::instance()->emitToastFailRequested(reason);
    }
    networkPersonnelStore_.finishQrAccessTransaction(
                pendingOnlineQr_.scanId, state, reason);
    clearPendingOnlineV1Qr();
}

void MqttManager::clearPendingOnlineV1Qr()
{
    if (onlineQrV1Timer_) onlineQrV1Timer_->stop();
    pendingOnlineQr_ = PendingOnlineQrAccess{};
}

bool MqttManager::requestFaceImagesForPerson(const QString &personId,
                                             bool reconnectRetry)
{
    const QString normalizedPersonId = personId.trimmed();
    if (normalizedPersonId.isEmpty()) {
        emit logMessage(QStringLiteral("自动人脸图像请求被忽略：personId为空"));
        return false;
    }

    const QString eventTopic = cfg_.publishTopic.trimmed();
    const bool onlineV1Active =
            eventTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && eventTopic.endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) {
        emit logMessage(QStringLiteral(
                            "自动人脸图像请求被忽略：当前不是online_v1，personId=%1")
                        .arg(normalizedPersonId));
        return false;
    }
    if (!ipc_ || !ipc_->isConnected()) {
        emit logMessage(QStringLiteral(
                            "自动人脸图像请求失败：MQTT IPC未连接，personId=%1")
                        .arg(normalizedPersonId));
        return false;
    }
    QJsonObject requestData;
    if (reconnectRetry) {
        requestData = pendingFaceImageRequests_.value(
                    normalizedPersonId).requestData;
        if (requestData.isEmpty()) {
            emit logMessage(QStringLiteral(
                                "人脸图像断线补发被忽略：待处理请求已完成，personId=%1")
                            .arg(normalizedPersonId));
            return false;
        }
    } else {
        if (!networkPersonnelStore_.initialize()) {
            emit logMessage(QStringLiteral(
                                "自动人脸图像请求失败：数据库未就绪，personId=%1")
                            .arg(normalizedPersonId));
            return false;
        }

        requestData = networkPersonnelStore_.pendingFaceImageRequest(
                    normalizedPersonId);
    }
    const QJsonArray hashes = requestData.value(QStringLiteral("faceHashes")).toArray();
    if (requestData.isEmpty() || hashes.isEmpty()) {
        emit logMessage(QStringLiteral(
                            "fullPersonnel人脸图像已完整，无需拉取：personId=%1")
                        .arg(normalizedPersonId));
        return true;
    }

    const QString deviceId = cfg_.clientId.trimmed().isEmpty()
            ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    QJsonObject payload;
    payload.insert(QStringLiteral("method"), QStringLiteral("face.requestImages"));
    payload.insert(QStringLiteral("id"),
                   QUuid::createUuid().toString(QUuid::WithoutBraces)
                       .remove(QLatin1Char('-')));
    payload.insert(QStringLiteral("deviceId"), deviceId);
    payload.insert(QStringLiteral("data"), requestData);
    payload.insert(QStringLiteral("time"),
                   QDateTime::currentDateTime().toString(
                       QStringLiteral("yyyy-MM-dd HH:mm:ss")));

    QJsonObject packet;
    packet.insert(QStringLiteral("cmd"), QStringLiteral("publish"));
    packet.insert(QStringLiteral("topic"), eventTopic);
    packet.insert(QStringLiteral("payload"), payload);
    const bool submitted = publishJsonPacket(
                packet,
                reconnectRetry
                    ? QStringLiteral("face-request-images-reconnect-retry")
                    : QStringLiteral("face-request-images-auto"));
    if (submitted) {
        PendingFaceImageRequest pending =
                pendingFaceImageRequests_.value(normalizedPersonId);
        pending.requestData = requestData;
        pending.requestPayload = payload;
        pending.awaitingResponse = true;
        pendingFaceImageRequests_.insert(normalizedPersonId, pending);
    }
    emit logMessage(QStringLiteral(
                        "%1人脸图像请求：personId=%2 count=%3 result=%4")
                    .arg(reconnectRetry
                         ? QStringLiteral("MQTT断线重连后补发")
                         : QStringLiteral("fullPersonnel自动"))
                    .arg(normalizedPersonId)
                    .arg(hashes.size())
                    .arg(submitted ? QStringLiteral("submitted")
                                   : QStringLiteral("failed")));
    return submitted;
}

void MqttManager::resendPendingFaceImageRequestsAfterReconnect()
{
    if (!mqttBrokerConnected_ || faceImageReconnectRetryPersons_.isEmpty()) {
        return;
    }

    const QStringList personIds = faceImageReconnectRetryPersons_.values();
    faceImageReconnectRetryPersons_.clear();
    for (const QString &personId : personIds) {
        const auto it = pendingFaceImageRequests_.constFind(personId);
        if (it == pendingFaceImageRequests_.cend()
                || !it.value().awaitingResponse) {
            continue;
        }

        const bool submitted = requestFaceImagesForPerson(personId, true);
        const auto updated = pendingFaceImageRequests_.constFind(personId);
        if (updated != pendingFaceImageRequests_.cend()
                && updated.value().awaitingResponse) {
            faceImageReconnectAwaitingPersons_.insert(personId);
        }
        emit logMessage(QStringLiteral(
                            "MQTT重连后补发一次人脸图像请求：personId=%1 "
                            "result=%2")
                        .arg(personId,
                             submitted ? QStringLiteral("submitted")
                                       : QStringLiteral("failed")));
    }

    if (!faceImageReconnectAwaitingPersons_.isEmpty()
            && faceImageReconnectResponseTimer_) {
        faceImageReconnectResponseTimer_->start(
                    faceImageReconnectResponseTimeoutMs_);
        emit logMessage(QStringLiteral(
                            "已补发%1个人脸图像请求，等待响应超时=%2秒")
                        .arg(faceImageReconnectAwaitingPersons_.size())
                        .arg(faceImageReconnectResponseTimeoutMs_ / 1000));
    }
}

bool MqttManager::handleFaceImages(const QString &topic,
                                   const QJsonObject &payloadObj,
                                   const QString &method)
{
    const QString requestTopic = topic.trimmed();
    const bool onlineV1Active =
            cfg_.publishTopic.trimmed().startsWith(
                QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(
                QStringLiteral("/event"));
    const bool ycLinuxRequest =
            requestTopic.startsWith(QStringLiteral("device/ycLinux/"))
            && requestTopic.endsWith(QStringLiteral("/request"));
    if (method != QStringLiteral("face.responseImages")
            || !onlineV1Active
            || !ycLinuxRequest) {
        return false;
    }

    const QString personId = payloadObj.value(QStringLiteral("data"))
            .toObject().value(QStringLiteral("personId"))
            .toString().trimmed();
    auto pendingRequest = pendingFaceImageRequests_.find(personId);
    if (pendingRequest != pendingFaceImageRequests_.end()
            && pendingRequest.value().awaitingResponse) {
        pendingRequest.value().awaitingResponse = false;
        faceImageReconnectRetryPersons_.remove(personId);
        faceImageReconnectAwaitingPersons_.remove(personId);
        if (faceImageReconnectAwaitingPersons_.isEmpty()
                && faceImageReconnectResponseTimer_) {
            faceImageReconnectResponseTimer_->stop();
        }
        emit logMessage(QStringLiteral(
                            "已收到face.responseImages，取消该人员的断线补发等待：personId=%1")
                        .arg(personId));
    }
    const QString requestKey = faceImageRequestKey(payloadObj);
    const QString contentKey = faceImageContentKey(payloadObj);
    if (activeFaceImageRequestKeys_.contains(requestKey)
            || activeFaceImageContentKeys_.contains(contentKey)) {
        emit logMessage(QStringLiteral(
                            "重复face.responseImages处于排队或处理中，已合并: "
                            "personId=%1 id=%2")
                        .arg(personId,
                             payloadObj.value(QStringLiteral("id")).toString()));
        return true;
    }

    PendingFaceImageWork work;
    work.requestKey = requestKey;
    work.contentKey = contentKey;
    work.personId = personId;
    work.responsePayload = payloadObj;
    activeFaceImageRequestKeys_.insert(requestKey);
    activeFaceImageContentKeys_.insert(contentKey);
    pendingFaceImageWork_.enqueue(work);
    emit logMessage(QStringLiteral(
                        "face.responseImages已进入串行队列: personId=%1 pending=%2")
                    .arg(personId)
                    .arg(pendingFaceImageWork_.size()));
    QTimer::singleShot(0, this, &MqttManager::processNextFaceImageWork);
    return true;
}

void MqttManager::processNextFaceImageWork()
{
    if (faceImageWorkActive_ || pendingFaceImageWork_.isEmpty()) {
        return;
    }

    faceImageWorkActive_ = true;
    currentFaceImageWork_ = pendingFaceImageWork_.dequeue();

    const QJsonObject payload = currentFaceImageWork_.responsePayload;
    if (!personnelSyncWorker_ || !personnelSyncThread_
            || !personnelSyncThread_->isRunning() || !personnelSyncStore_) {
        onFaceImagesPrepared(networkPersonnelStore_.applyFaceImages(payload));
        return;
    }

    NetworkPersonnelStore *workerStore = personnelSyncStore_;
    QMetaObject::invokeMethod(
                personnelSyncWorker_,
                [this, workerStore, payload]() {
        const NetworkPersonnelSyncResult result =
                workerStore->applyFaceImages(payload);
        QMetaObject::invokeMethod(
                    this,
                    [this, result]() { onFaceImagesPrepared(result); },
                    Qt::QueuedConnection);
    },
    Qt::QueuedConnection);
}

void MqttManager::onFaceImagesPrepared(
        const NetworkPersonnelSyncResult &result)
{
    if (!faceImageWorkActive_) {
        return;
    }
    const QJsonObject payloadObj = currentFaceImageWork_.responsePayload;
    const QString personId = currentFaceImageWork_.personId;
    FaceImageSyncBridge *bridge = FaceImageSyncBridge::instance();
    QString message = result.data.value(QStringLiteral("message"))
            .toString().trimmed();
    if (!result.ok) {
        if (message.isEmpty()) {
            message = QStringLiteral("人脸图像保存失败，请重新录入人脸。");
        }
        publishFaceImagesResult(
                    payloadObj, personId, false, message,
                    failedFacesFromPayload(payloadObj, message));
        bridge->reportSyncStatus(false, message);
        emit logMessage(QStringLiteral(
                            "人员图像保存失败，未删除人员聚合数据: "
                            "personId=%1 code=%2 message=%3")
                        .arg(personId,
                             result.data.value(QStringLiteral("code")).toString(),
                             message));
        finishCurrentFaceImageWork();
        return;
    }

    const QJsonArray faceItems = result.data.value(
                QStringLiteral("faceItems")).toArray();
    const QJsonArray initialFailedFaces = result.data.value(
                QStringLiteral("failedFaces")).toArray();
    if (personId.isEmpty()
            || (faceItems.isEmpty() && initialFailedFaces.isEmpty())) {
        message = QStringLiteral("人脸图像校验参数不完整，请重新录入人脸。");
        publishFaceImagesResult(
                    payloadObj, personId, false, message,
                    failedFacesFromPayload(payloadObj, message));
        bridge->reportSyncStatus(false, message);
        finishCurrentFaceImageWork();
        return;
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingFaceImageValidation pending;
    pending.personId = personId;
    pending.responsePayload = payloadObj;
    pending.requestedFaces = faceItems;
    pending.initialFailedFaces = initialFailedFaces;
    pendingFaceValidations_.insert(token, pending);
    faceValidationTokenByPerson_.insert(personId, token);

    if (faceItems.isEmpty()) {
        onStoredFaceValidationFinished(
                    token, personId, false,
                    firstFailedFaceMessage(
                        initialFailedFaces,
                        QStringLiteral("人脸图像校验失败，请重新录入人脸。")),
                    QJsonArray(), QJsonArray());
        return;
    }

    bridge->reportSyncStatus(
                true, QStringLiteral("人脸图像已保存，正在检测人脸和质量。"));
    bridge->requestStoredFaceValidation(token, personId, faceItems);
    QTimer::singleShot(faceValidationTimeoutMs_, this, [this, token]() {
        if (!pendingFaceValidations_.contains(token)) return;
        const PendingFaceImageValidation pending =
                pendingFaceValidations_.value(token);
        const QString message = QStringLiteral(
                    "人脸图像校验超时，请重新录入人脸。");
        QJsonArray timedOutFaces;
        for (const QJsonValue &value : pending.requestedFaces) {
            QJsonObject failed = value.toObject();
            failed.insert(QStringLiteral("message"), message);
            timedOutFaces.append(failed);
        }
        onStoredFaceValidationFinished(
                    token, pending.personId, false,
                    message, QJsonArray(), timedOutFaces);
    });

    emit logMessage(QStringLiteral(
                        "人员图像已落盘并提交质量校验: personId=%1 count=%2 token=%3")
                    .arg(personId)
                    .arg(faceItems.size())
                    .arg(token));
}

void MqttManager::finishCurrentFaceImageWork()
{
    if (!faceImageWorkActive_) {
        return;
    }
    activeFaceImageRequestKeys_.remove(currentFaceImageWork_.requestKey);
    activeFaceImageContentKeys_.remove(currentFaceImageWork_.contentKey);
    currentFaceImageWork_ = PendingFaceImageWork();
    faceImageWorkActive_ = false;
    QTimer::singleShot(0, this, &MqttManager::processNextFaceImageWork);
}

void MqttManager::onStoredFaceValidationFinished(
        const QString &token,
        const QString &personId,
        bool ok,
        const QString &message,
        const QJsonArray &validatedFaces,
        const QJsonArray &failedFaces)
{
    if (!pendingFaceValidations_.contains(token)) {
        emit logMessage(QStringLiteral(
                            "忽略过期的人脸图像校验结果: token=%1 personId=%2")
                        .arg(token, personId));
        return;
    }

    const PendingFaceImageValidation pending =
            pendingFaceValidations_.take(token);
    if (faceValidationTokenByPerson_.value(pending.personId) == token) {
        faceValidationTokenByPerson_.remove(pending.personId);
    }

    const bool personMatches = personId == pending.personId;
    QJsonArray finalValidatedFaces = validatedFaces;
    QJsonArray finalFailedFaces = mergeFailedFaces(
                pending.initialFailedFaces, failedFaces);
    QString finalMessage = message.trimmed();
    if (!personMatches) {
        finalMessage = QStringLiteral("人脸校验人员信息不一致，请重新录入人脸。");
        QJsonArray mismatchedFaces;
        for (const QJsonValue &value : pending.requestedFaces) {
            QJsonObject failed = value.toObject();
            failed.insert(QStringLiteral("message"), finalMessage);
            mismatchedFaces.append(failed);
        }
        finalFailedFaces = mergeFailedFaces(
                    finalFailedFaces, mismatchedFaces);
        finalValidatedFaces = QJsonArray();
    } else if (!ok && failedFaces.isEmpty()) {
        QJsonArray unclassifiedFaces;
        for (const QJsonValue &value : pending.requestedFaces) {
            QJsonObject failed = value.toObject();
            failed.insert(QStringLiteral("message"), finalMessage);
            unclassifiedFaces.append(failed);
        }
        finalFailedFaces = mergeFailedFaces(
                    finalFailedFaces, unclassifiedFaces);
    }

    if (!finalFailedFaces.isEmpty()) {
        finalMessage = firstFailedFaceMessage(
                    finalFailedFaces,
                    QStringLiteral("人脸图像校验失败，请重新录入人脸。"));
    }

    if (!personnelSyncWorker_ || !personnelSyncThread_
            || !personnelSyncThread_->isRunning() || !personnelSyncStore_) {
        bool allFacesReady = false;
        QString finalizeError;
        QJsonArray commitFailedFaces;
        QJsonArray commitFaceNotices;
        const bool finalized = networkPersonnelStore_.finalizeFaceImageValidation(
                    pending.personId,
                    finalValidatedFaces,
                    finalFailedFaces,
                    &allFacesReady,
                    &finalizeError,
                    &commitFailedFaces,
                    &commitFaceNotices);
        completeStoredFaceValidation(
                    pending, personMatches, finalMessage, finalFailedFaces,
                    finalized, allFacesReady, finalizeError,
                    commitFailedFaces, commitFaceNotices);
        return;
    }

    NetworkPersonnelStore *workerStore = personnelSyncStore_;
    QMetaObject::invokeMethod(
                personnelSyncWorker_,
                [this, workerStore, pending, personMatches, finalMessage,
                 finalValidatedFaces, finalFailedFaces]() {
        bool allFacesReady = false;
        QString finalizeError;
        QJsonArray commitFailedFaces;
        QJsonArray commitFaceNotices;
        const bool finalized = workerStore->finalizeFaceImageValidation(
                    pending.personId,
                    finalValidatedFaces,
                    finalFailedFaces,
                    &allFacesReady,
                    &finalizeError,
                    &commitFailedFaces,
                    &commitFaceNotices);
        QMetaObject::invokeMethod(
                    this,
                    [this, pending, personMatches, finalMessage,
                     finalFailedFaces, finalized, allFacesReady,
                     finalizeError, commitFailedFaces, commitFaceNotices]() {
            completeStoredFaceValidation(
                        pending, personMatches, finalMessage, finalFailedFaces,
                        finalized, allFacesReady, finalizeError,
                        commitFailedFaces, commitFaceNotices);
        },
        Qt::QueuedConnection);
    },
    Qt::QueuedConnection);
}

void MqttManager::completeStoredFaceValidation(
        const PendingFaceImageValidation &pending,
        bool personMatches,
        const QString &validationMessage,
        const QJsonArray &validationFailedFaces,
        bool finalized,
        bool allFacesReady,
        const QString &finalizeError,
        const QJsonArray &commitFailedFaces,
        const QJsonArray &commitFaceNotices)
{
    QJsonArray finalFailedFaces = validationFailedFaces;
    QString finalMessage = validationMessage;
    if (!commitFailedFaces.isEmpty()) {
        finalFailedFaces = mergeFailedFaces(
                    finalFailedFaces, commitFailedFaces);
        finalMessage = firstFailedFaceMessage(
                    finalFailedFaces,
                    QStringLiteral("人脸图像校验失败，请重新录入人脸。"));
    }
    const bool finalResult = finalized
            && personMatches
            && finalFailedFaces.isEmpty()
            && allFacesReady;
    if (!finalized) {
        if (finalFailedFaces.isEmpty()) {
            finalMessage = QStringLiteral("人脸特征保存失败，请重新录入人脸。");
        }
        if (!finalizeError.isEmpty()) {
            emit logMessage(QStringLiteral(
                                "确认人脸图像失败: personId=%1 error=%2")
                            .arg(pending.personId, finalizeError));
        }
    } else if (finalResult) {
        finalMessage = QStringLiteral("人脸图像同步成功。");
    } else if (!finalizeError.isEmpty()) {
        finalMessage = finalizeError;
    } else if (finalMessage.isEmpty()) {
        finalMessage = QStringLiteral("人脸图像校验失败，请重新录入人脸。");
    }

    publishFaceImagesResult(pending.responsePayload,
                            pending.personId,
                            finalResult,
                            finalMessage,
                            finalFailedFaces,
                            commitFaceNotices);
    FaceImageSyncBridge *bridge = FaceImageSyncBridge::instance();
    bridge->reportSyncStatus(finalResult, finalMessage);
    scheduleNetworkFaceRefresh();
    emit logMessage(QStringLiteral(
                        "人脸图像校验完成: personId=%1 result=%2 message=%3")
                    .arg(pending.personId,
                         finalResult ? QStringLiteral("true")
                                      : QStringLiteral("false"),
                          finalMessage));
    finishCurrentFaceImageWork();
}

bool MqttManager::publishFaceImagesResult(
        const QJsonObject &responsePayload,
        const QString &personId,
        bool result,
        const QString &message,
        const QJsonArray &failedFaces,
        const QJsonArray &faceNotices)
{
    const QString eventTopic = cfg_.publishTopic.trimmed();
    if (eventTopic.isEmpty()) {
        emit logMessage(QStringLiteral("Imagesresult 发布失败：publishTopic为空"));
        return false;
    }
    if (!mqttBrokerConnected_) {
        emit logMessage(QStringLiteral(
                            "Imagesresult暂缓提交：MQTT Broker未连接，personId=%1")
                        .arg(personId));
        return false;
    }

    QString messageId = responsePayload.value(QStringLiteral("id"))
            .toString().trimmed();
    if (messageId.isEmpty()) {
        messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    QString deviceId = responsePayload.value(QStringLiteral("deviceId"))
            .toString().trimmed();
    if (deviceId.isEmpty()) {
        deviceId = cfg_.clientId.trimmed().isEmpty()
                ? cfg_.deviceName.trimmed() : cfg_.clientId.trimmed();
    }

    QJsonArray responseFaces = responsePayload.value(QStringLiteral("data"))
            .toObject().value(QStringLiteral("faces")).toArray();
    if (responseFaces.isEmpty()) {
        responseFaces = failedFaces;
    }
    if (responseFaces.isEmpty()) {
        responseFaces.append(QJsonObject());
    }

    bool submitted = true;
    int submittedCount = 0;
    for (const QJsonValue &value : responseFaces) {
        const QJsonObject responseFaceSource = value.toObject();
        const QString faceName = responseFaceSource.value(
                    QStringLiteral("name")).toString().trimmed();
        const QString faceHash = responseFaceSource.value(
                    QStringLiteral("faceHash")).toString().trimmed();

        QJsonObject matchedFailure;
        for (const QJsonValue &failedValue : failedFaces) {
            const QJsonObject failed = failedValue.toObject();
            const QString failedHash = failed.value(QStringLiteral("faceHash"))
                    .toString().trimmed();
            const QString failedName = failed.value(QStringLiteral("name"))
                    .toString().trimmed();
            const bool sameFace = !faceHash.isEmpty()
                    ? failedHash == faceHash
                    : (!faceName.isEmpty() && failedHash.isEmpty()
                       && failedName == faceName);
            if (sameFace) {
                matchedFailure = failed;
                break;
            }
        }

        QJsonObject matchedNotice;
        for (const QJsonValue &noticeValue : faceNotices) {
            const QJsonObject notice = noticeValue.toObject();
            const QString noticeHash = notice.value(QStringLiteral("faceHash"))
                    .toString().trimmed();
            const QString noticeName = notice.value(QStringLiteral("name"))
                    .toString().trimmed();
            const bool sameFace = !faceHash.isEmpty()
                    ? noticeHash == faceHash
                    : (!faceName.isEmpty() && noticeHash.isEmpty()
                       && noticeName == faceName);
            if (sameFace) {
                matchedNotice = notice;
                break;
            }
        }

        const bool faceResult = matchedFailure.isEmpty()
                ? (failedFaces.isEmpty() ? result : true)
                : false;
        QString faceMessage = matchedFailure.value(QStringLiteral("message"))
                .toString().trimmed();
        if (faceMessage.isEmpty()) {
            faceMessage = matchedNotice.value(QStringLiteral("message"))
                    .toString().trimmed();
        }
        if (faceMessage.isEmpty()) {
            faceMessage = faceResult && !result
                    ? QStringLiteral("人脸图像同步成功。")
                    : message;
        }

        QJsonObject data;
        data.insert(QStringLiteral("personId"), personId);
        data.insert(QStringLiteral("name"), faceName);
        data.insert(QStringLiteral("faceHash"), faceHash);
        data.insert(QStringLiteral("result"), faceResult);
        data.insert(QStringLiteral("msg"), faceMessage);
        if (!faceResult && (!faceName.isEmpty() || !faceHash.isEmpty())) {
            QJsonObject failedFace;
            failedFace.insert(QStringLiteral("name"), faceName);
            failedFace.insert(QStringLiteral("faceHash"), faceHash);
            QJsonArray responseFailedFaces;
            responseFailedFaces.append(failedFace);
            data.insert(QStringLiteral("failed_faces"), responseFailedFaces);
        }

        QJsonObject payload;
        payload.insert(QStringLiteral("method"), QStringLiteral("Imagesresult"));
        payload.insert(QStringLiteral("id"), messageId);
        payload.insert(QStringLiteral("deviceId"), deviceId);
        payload.insert(QStringLiteral("data"), data);
        payload.insert(QStringLiteral("time"),
                       QDateTime::currentDateTime().toString(
                           QStringLiteral("yyyy-MM-dd HH:mm:ss")));

        QJsonObject packet;
        packet.insert(QStringLiteral("cmd"), QStringLiteral("publish"));
        packet.insert(QStringLiteral("topic"), eventTopic);
        packet.insert(QStringLiteral("payload"), payload);
        const bool faceSubmitted = publishJsonPacket(
                    packet, QStringLiteral("Imagesresult"));
        submitted = submitted && faceSubmitted;
        if (faceSubmitted) {
            ++submittedCount;
        }
    }

    const QString normalizedPersonId = personId.trimmed();
    if (submitted && !normalizedPersonId.isEmpty()) {
        const bool wasPending =
                pendingFaceImageRequests_.remove(normalizedPersonId) > 0;
        faceImageReconnectRetryPersons_.remove(normalizedPersonId);
        faceImageReconnectAwaitingPersons_.remove(normalizedPersonId);
        if (faceImageReconnectAwaitingPersons_.isEmpty()
                && faceImageReconnectResponseTimer_) {
            faceImageReconnectResponseTimer_->stop();
        }
        if (wasPending) {
            emit logMessage(QStringLiteral(
                                "Imagesresult已提交，停止人脸图像断线补发：personId=%1")
                            .arg(normalizedPersonId));
        }
    }
    emit logMessage(QStringLiteral(
                        "Imagesresult逐图提交完成: personId=%1 submitted=%2 total=%3")
                    .arg(normalizedPersonId)
                    .arg(submittedCount)
                    .arg(responseFaces.size()));
    return submitted;
}


/**
 * @brief 连接到 mqttd IPC 并在连接成功后下发配置。
 *
 * 行为说明：
 * - 若 IPC 路径为空，则仅记录日志；
 * - 若已连接，则直接发送配置并立即应用；
 * - 若正在连接，则不重复发起连接；
 * - 若未连接，则发起新的 IPC 连接。
 */
void MqttManager::setConfig(const MqttConfig &cfg)
{
    cfg_ = cfg;
    const bool onlineV1Active = cfg_.publishTopic.trimmed()
            .startsWith(QStringLiteral("device/ycLinux/"))
            && cfg_.publishTopic.trimmed().endsWith(QStringLiteral("/event"));
    if (!onlineV1Active) {
        if (faceImageReconnectResponseTimer_) {
            faceImageReconnectResponseTimer_->stop();
        }
        pendingFaceImageRequests_.clear();
        faceImageReconnectRetryPersons_.clear();
        faceImageReconnectAwaitingPersons_.clear();
    }
    if (!onlineV1Active && pendingOnlineQr_.active) {
        networkPersonnelStore_.finishQrAccessTransaction(
                    pendingOnlineQr_.scanId,
                    QStringLiteral("MODE_CHANGED"),
                    QStringLiteral("运行模式已切换"));
        clearPendingOnlineV1Qr();
    }
    emit logMessage(QString("MqttManager::setConfig protocolMode=%1 publishTopic=%2 subscribeTopic=%3 ipcPath=%4")
                    .arg(cfg_.protocolMode, cfg_.publishTopic, cfg_.subscribeTopic, cfg_.ipcPath));

    if (icGateway_) {
        icGateway_->applyConfig(cfg_);
    }
}

/** @brief 通过 IPC 发布已组装的 JSON packet。 */
bool MqttManager::publishJsonPacket(const QJsonObject &packet, const QString &tag)
{
    if (!ipc_ || !ipc_->isConnected()) {
        emit logMessage(QString("IC publish失败：IPC未连接，tag=%1").arg(tag));
        return false;
    }
    const bool ok = ipc_->sendJsonLine(packet);
    if (ok) {
        if (tag == QStringLiteral("face-upload")) {
            const QJsonObject payload = packet.value(
                        QStringLiteral("payload")).toObject();
            const QJsonObject data = payload.value(
                        QStringLiteral("data")).toObject();
            emit logMessage(QStringLiteral(
                                "TX publish[face-upload]: method=%1 id=%2 "
                                "success=%3 imageBase64Bytes=%4")
                            .arg(payload.value(QStringLiteral("method")).toString(),
                                 payload.value(QStringLiteral("id")).toString(),
                                 data.value(QStringLiteral("success")).toBool()
                                 ? QStringLiteral("true")
                                 : QStringLiteral("false"),
                                 QString::number(data.value(
                                     QStringLiteral("faceImage"))
                                     .toString().size())));
        } else {
            emit logMessage(QString("TX publish[%1]: %2")
                            .arg(tag,
                                 QString::fromUtf8(QJsonDocument(packet).toJson(
                                     QJsonDocument::Compact))));
        }
    }
    return ok;
}


/**
 * @brief 连接到 mqttd IPC 并在连接成功后下发配置。
 *
 * 行为说明：
 * - 若 IPC 路径为空，则仅记录日志；
 * - 若已连接，则直接发送配置并立即应用；
 * - 若正在连接，则不重复发起连接；
 * - 若未连接，则发起新的 IPC 连接。
 */
void MqttManager::connectAndApply()
{
    const QString path = cfg_.ipcPath.trimmed();
    if (path.isEmpty()) {
        emit logMessage("请输入 mqttd 的 socket 路径，例如 /tmp/mqttd.sock");
        return;
    }

    const auto st = ipc_->state();

    // 已经连上 IPC：直接下发配置并应用
    if (st == QLocalSocket::ConnectedState) {
        sendConfigToMqttd(true);
        return;
    }

    // 正在连接中：不重复发起
    if (st == QLocalSocket::ConnectingState) {
        emit logMessage("IPC 正在连接中…");
        return;
    }

    // 未连接：发起 IPC 连接
    ipc_->connectToServer(path);
}


/**
 * @brief 主动断开 IPC 连接。
 */
void MqttManager::disconnectIpc()
{
    ipc_->disconnectFromServer();
}


/**
 * @brief 向 mqttd 发送测试连接命令。
 *
 * 仅在 IPC 已建立连接时有效。
 */
void MqttManager::testConnection()
{
    if (!ipc_->isConnected()) {
        emit logMessage("IPC 未连接 mqttd，无法测试");
        return;
    }

    ipc_->sendRawLine("{\"cmd\":\"test\"}\n");
    emit logMessage("已发送测试连接命令：{cmd:test}");
}


/**
 * @brief IPC 连接建立后的槽函数。
 *
 * 连接建立后会：
 * - 通知界面 IPC 已连接；
 * - 主动请求一次 mqttd status；
 * - 根据返回状态决定是否需要重新应用配置，避免IPC重连连带重建Broker连接。
 */
void MqttManager::onIpcConnected()
{
    stopIpcReconnect();

    emit ipcStateChanged(true);
    emit logMessage("IPC 已连接：等待 mqttd hello/status 推送…");

    emit mqttConnMarkChanged(0);

    // 主动请求一次 status
    ipcConfigApplyPending_ = true;
    const int configDecisionGeneration = ++ipcConfigDecisionGeneration_;
    ipc_->sendRawLine("{\"cmd\":\"status\"}\n");
    const auto retryStatus = [this, configDecisionGeneration]() {
        if (ipcConfigDecisionGeneration_ == configDecisionGeneration
                && ipcConfigApplyPending_ && ipc_ && ipc_->isConnected()) {
            ipc_->sendRawLine("{\"cmd\":\"status\"}\n");
        }
    };
    QTimer::singleShot(3000, this, retryStatus);
    QTimer::singleShot(10000, this, retryStatus);
    QTimer::singleShot(30000, this, [this, configDecisionGeneration]() {
        if (ipcConfigDecisionGeneration_ != configDecisionGeneration
                || !ipcConfigApplyPending_ || !ipc_ || !ipc_->isConnected()) {
            return;
        }
        ipcConfigApplyPending_ = false;
        emit logMessage(QStringLiteral(
                            "IPC连接30秒未取得mqttd状态，执行一次配置apply兜底"));
        sendConfigToMqttd(true);
    });
}


/**
 * @brief IPC 断开连接后的槽函数。
 *
 * 断开后重置界面上的 MQTT 状态展示。
 */
void MqttManager::onIpcDisconnected()
{
    emit ipcStateChanged(false);
    emit mqttStateTextChanged("未知");
    emit subTopicsTextChanged("");

    emit mqttConnMarkChanged(1);
    emit logMessage("IPC 已断开");


    startIpcReconnect();
}


/**
 * @brief IPC 错误处理槽函数。
 *
 * @param err 错误描述文本。
 */
void MqttManager::onIpcError(const QString &err)
{
    emit ipcStateChanged(false);
    emit mqttStateTextChanged("未知");
    emit subTopicsTextChanged("");

    emit mqttConnMarkChanged(1);
    emit logMessage(QString("IPC 错误：%1").arg(err));

    startIpcReconnect();
}

/**
 * @brief IPC 收到完整帧后的槽函数。
 *
 * @param frame 已完成组帧的一条原始消息。
 */
void MqttManager::onIpcFrameReceived(const QByteArray &frame)
{
    handleIpcLine(frame);
}

/**
 * @brief 通过 mqttd IPC 下发 MQTT 配置及订阅主题。
 *
 * 该函数会构造如下信息并发送给 mqttd：
 * - 基础 MQTT 连接参数；
 * - 订阅 topic 列表；
 * - 可选的 apply 指令。
 *
 * @param applyNow 是否在发送配置后立即发送 apply 命令。
 */
void MqttManager::sendConfigToMqttd(bool applyNow)
{
    if (!ipc_ || !ipc_->isConnected()) {
        emit logMessage("未连接 mqttd（IPC），无法下发配置");
        return;
    }

    // 组装 MQTT 基础配置对象
    QJsonObject mqtt;
    mqtt["host"] = cfg_.host.trimmed();
    mqtt["port"] = cfg_.port;
    mqtt["client_id"] = cfg_.clientId.trimmed();
    mqtt["keepalive"] = cfg_.keepalive;
    mqtt["qos"] = cfg_.qos;
    mqtt["tls"] = cfg_.tls;
    mqtt["username"] = cfg_.username;
    mqtt["password"] = cfg_.password;

    // 组装订阅 topic 列表，至少保留一个默认 topic
    QStringList topics = cfg_.subTopics;
    if (topics.isEmpty()) topics << "test";

    // 确保 streamUrlTopic 也被纳入订阅集合
//    if (!cfg_.streamUrlTopic.isEmpty() && !topics.contains(cfg_.streamUrlTopic))
//        topics << cfg_.streamUrlTopic;

    topics.removeDuplicates();

    QJsonArray subs;
    for (const auto& tp : topics) subs.append(tp);

    emit logMessage(QString("订阅topics：%1").arg(topics.join(";")));

    // 组装 set_config 命令
    QJsonObject root;
    root["cmd"] = "set_config";
    root["mqtt"] = mqtt;
    root["subs"] = subs;

    ipc_->sendJsonLine(root);
    emit logMessage("TX: set_config 已发送");

    if (applyNow) {
        ipc_->sendRawLine("{\"cmd\":\"apply\"}\n");
        emit logMessage("TX: apply 已发送");
    }
}


/**
 * @brief 统一分发一条已归一化的 payload 消息。
 *
 * 输入参数应当已经被解析为统一形式：
 * - topic
 * - payload JSON 对象
 *
 * 本函数负责：
 * - 输出统一的接收日志；
 * - 提取 method；
 * - 按业务处理顺序调用各类 handler。
 *
 * @param topic      消息 topic。
 * @param payloadObj 消息 payload 对象。
 * @return true  消息已被某个 handler 处理。
 * @return false 没有任何 handler 处理该消息。
 */
bool MqttManager::dispatchPayloadMessage(const QString &topic, const QJsonObject &payloadObj)
{
    if (payloadObj.isEmpty()) {
        emit logMessage(QString("消息 payload 为空：topic=%1").arg(topic));
        return false;
    }

    const QString method = payloadObj.value("method").toString().trimmed().isEmpty()
            ? payloadObj.value("methon").toString().trimmed()
            : payloadObj.value("method").toString().trimmed();

    if (method == QStringLiteral("face.responseImages")) {
        const QJsonObject data = payloadObj.value(QStringLiteral("data")).toObject();
        const QJsonArray faces = data.value(QStringLiteral("faces")).toArray();
        QStringList faceHashes;
        qint64 base64Chars = 0;
        for (const QJsonValue &value : faces) {
            const QJsonObject face = value.toObject();
            const QString faceHash = face.value(QStringLiteral("faceHash"))
                    .toString().trimmed();
            if (!faceHash.isEmpty()) faceHashes.append(faceHash);

            const QString encoded = face.value(QStringLiteral("faceBase64")).toString();
            const int comma = encoded.indexOf(QLatin1Char(','));
            base64Chars += comma >= 0 ? encoded.size() - comma - 1 : encoded.size();
        }
        emit logMessage(QStringLiteral(
                            "消息摘要：topic=%1 method=%2 personId=%3 "
                            "faceCount=%4 base64Chars=%5 faceHashes=%6")
                        .arg(topic,
                             method,
                             data.value(QStringLiteral("personId")).toString().trimmed())
                        .arg(faces.size())
                        .arg(base64Chars)
                        .arg(faceHashes.join(QLatin1Char(','))));
    } else {
        const QByteArray compactPayload = QJsonDocument(payloadObj)
                .toJson(QJsonDocument::Compact);
        if (compactPayload.size() > 16384) {
            emit logMessage(QStringLiteral(
                                "消息摘要：topic=%1 method=%2 payloadBytes=%3")
                            .arg(topic, method)
                            .arg(compactPayload.size()));
        } else {
            emit logMessage(QString("消息：topic=%1 payload=%2")
                            .arg(topic, QString::fromUtf8(compactPayload)));
        }
    }

    return messageRouter_
            && messageRouter_->dispatch(topic, payloadObj, method);
}


/**
 * @brief 处理 getPlayFileList 请求。
 *
 * 仅当 payload 中的 method 为 "getPlayFileList" 时生效。
 * 若命中该方法，则构造播放文件列表响应并通过 mqttd IPC 回发。
 *
 * @param topic      请求 topic。
 * @param payloadObj 请求 payload 对象。
 * @param method     已提取的 method 字段。
 * @return true  已识别并处理该请求，或处理失败但已消费该请求。
 * @return false 当前消息不是 getPlayFileList 请求。
 */
bool MqttManager::handleGetPlayFileList(const QString &topic, const QJsonObject &payloadObj, const QString &method)
{
    if (method != "getPlayFileList") {
        return false;
    }

    if (!service_ || !ipc_) {
        emit logMessage("处理 getPlayFileList 失败：内部对象未初始化");
        return true;
    }

    const QString reqId = payloadObj.value("id").toString().trimmed();
    const QString deviceId = payloadObj.value("deviceId").toString().trimmed();

    emit logMessage(QString("收到 getPlayFileList 请求：topic=%1 id=%2 deviceId=%3")
                    .arg(topic, reqId, deviceId));

    QString respTopic;
    QJsonObject packet =
        service_->buildPlayFileListPublishPacket(topic, reqId, deviceId, &respTopic);

    if (ipc_->sendJsonLine(packet)) {
        emit logMessage(QString("sendPlayFileList 已提交到 mqttd，respTopic=%1").arg(respTopic));
    } else {
        emit logMessage("发送播放文件列表失败：IPC write 失败");
    }

    return true;
}


/**
 * @brief 处理 getPlayInfo 请求。
 *
 * 仅当 payload 中的 method 为 "getPlayInfo" 时生效。
 * 命中后向上层发出 getPlayInfoRequested 信号，由其他组件组织响应内容。
 *
 * @param topic      请求 topic。
 * @param payloadObj 请求 payload 对象。
 * @param method     已提取的 method 字段。
 * @return true  已识别并处理该请求，或处理失败但已消费该请求。
 * @return false 当前消息不是 getPlayInfo 请求。
 */
bool MqttManager::handleGetPlayInfo(const QString &topic, const QJsonObject &payloadObj, const QString &method)
{
    if (method != "getPlayInfo") {
        return false;
    }

    if (!service_ || !ipc_) {
        emit logMessage("处理 getPlayInfo 失败：内部对象未初始化");
        return true;
    }

    const QString reqId = payloadObj.value("id").toString().trimmed();
    const QString deviceId = payloadObj.value("deviceId").toString().trimmed();

    emit logMessage(QString("收到 getPlayInfo 请求：topic=%1 id=%2 deviceId=%3")
                    .arg(topic, reqId, deviceId));

    emit getPlayInfoRequested(topic, reqId, deviceId);
    return true;
}


/**
 * @brief 处理 deletePlayFile 请求。
 *
 * 仅当 payload 中的 method 为 "deletePlayFile" 时生效。
 *
 * @param topic      请求 topic。
 * @param payloadObj 请求 payload 对象。
 * @param method     已提取的 method 字段。
 * @return true  已识别并处理该请求，或处理失败但已消费该请求。
 * @return false 当前消息不是 deletePlayFile 请求。
 */
bool MqttManager::handleDeletePlayFile(const QString &topic,
                                       const QJsonObject &payloadObj,
                                       const QString &method)
{
    if (method != "deletePlayFile") {
        return false;
    }

    if (!service_) {
        emit logMessage("处理 deletePlayFile 失败：service 未初始化");
        return true;
    }

    service_->handleDeletePlayFileMessage(topic, payloadObj);
    return true;
}


/** 确认 linuxReboot 请求后延迟执行，确保 MQTT 回执有机会先发送完成。 */
bool MqttManager::handleLinuxReboot(const QString &topic,
                                    const QJsonObject &payloadObj,
                                    const QString &method)
{
    if (method != "linuxReboot") {
        return false;
    }

    static bool rebootPending = false;

    if (rebootPending) {
        emit logMessage("linuxReboot 已在等待重启中，忽略重复重启请求");
        return true;
    }

    if (!service_ || !ipc_) {
        emit logMessage("处理 linuxReboot 失败：内部对象未初始化");
        return true;
    }

    const QString reqId = payloadObj.value("id").toString().trimmed();
    const QString deviceId = payloadObj.value("deviceId").toString().trimmed();

    emit logMessage(QString("收到 linuxReboot 请求：topic=%1 id=%2 deviceId=%3")
                    .arg(topic, reqId, deviceId));

    if (!ipc_->isConnected()) {
        emit logMessage("处理 linuxReboot 失败：IPC 未连接，无法发送回执");
        return true;
    }

    QString respTopic;
    QJsonObject packet = service_->buildLinuxRebootAckPublishPacket(topic, payloadObj, &respTopic);

    rebootPending = true;

    if (ipc_->sendJsonLine(packet)) {
        emit logMessage(QString("linuxReboot 回执已提交到 mqttd，respTopic=%1 status=ok")
                        .arg(respTopic));
    } else {
        emit logMessage(QString("linuxReboot 回执发送失败：IPC write 失败，respTopic=%1")
                        .arg(respTopic));
        return true;
    }

    emit logMessage("linuxReboot 回执已发送，准备重启 RK3566 主板");

    QTimer::singleShot(1000, this, []() {
        // 刷新日志和文件系统缓存
        ::sync();
        Rk3566Platform::reboot();

    });

    return true;
}



/**
 * @brief 处理视频控制类消息。
 *
 * 实际处理逻辑委托给 MqttService::handleVideoControlMessage()。
 *
 * @param topic      消息 topic。
 * @param payloadObj 消息 payload 对象。
 * @return true  已由 service 层识别并处理。
 * @return false 未识别为视频控制消息，或 service 不可用。
 */
bool MqttManager::handleVideoControl(const QString &topic,
                                     const QJsonObject &payloadObj)
{
    const QString method = payloadObj.value("method").toString().trimmed();
    if (method != "videoControl") {
        return false;
    }

    // 如果是回执消息，不再重复处理
    const QJsonObject dataObj = payloadObj.value("data").toObject();
    const QString status = dataObj.value("status").toString().trimmed().toLower();
    if (status == "ok" || status == "error") {
        emit logMessage(QString("忽略 videoControl 回执消息：topic=%1 status=%2")
                        .arg(topic, status));
        return true;
    }

    if (!service_) {
        emit logMessage("处理 videoControl 失败：service 未初始化");
        return true;
    }

    // 关键：基于原始请求，提前判断这次是不是“音量类统一回执”
    const QString videoType = dataObj.value("videoType").toString().trimmed().toUpper();

    bool hasVideoVolume = false;
    int requestedVideoVolume = -1;
    const QJsonValue vv = dataObj.value("videoVolume");
    if (!vv.isUndefined() && !vv.isNull()) {
        if (vv.isDouble()) {
            hasVideoVolume = true;
            requestedVideoVolume = vv.toInt();
        } else if (vv.isString()) {
            bool numOk = false;
            requestedVideoVolume = vv.toString().trimmed().toInt(&numOk);
            hasVideoVolume = numOk;
        }
    }

    QString videoPath = dataObj.value("videoPath").toString().trimmed();
    videoPath = QFileInfo(videoPath).fileName().trimmed();

    const bool isRecordedDownloadRequest =
            (videoType == "RECORDED" && !videoPath.isEmpty());
    const bool isRecordedNoVideoPathRequest =
            (videoType == "RECORDED" && videoPath.isEmpty());

    if (videoType == "LIVE" && recordedDownloadTaskActive_) {
        const QString reason = QStringLiteral("RECORDED 下载进行中，暂不能切换到 LIVE");
        emit logMessage(reason);
        emit videoControlNotice(reason);
        sendVideoControlAckNow(topic, payloadObj, false, false, true, false,
                               QStringLiteral("download_busy"), reason);
        return true;
    }

    QString downloadTaskKey;
    if (isRecordedDownloadRequest) {
        downloadTaskKey = recordedDownloadTaskKey(dataObj);

        if (recordedDownloadTaskActive_) {
            if (recordedDownloadTaskKey_ == downloadTaskKey) {
                pendingVideoCtrlAck_.active = true;
                pendingVideoCtrlAck_.reqTopic = topic;
                pendingVideoCtrlAck_.reqPayload = payloadObj;

                if (hasVideoVolume && requestedVideoVolume >= 0) {
                    requestedVideoVolume = qBound(0, requestedVideoVolume, 100);
                    service_->updateCurrentPlayVolume(requestedVideoVolume);
                    emit mqttVolumeReceived(requestedVideoVolume);
                }

                emit logMessage(QStringLiteral("RECORDED 相同下载任务仍在预检查或下载中，合并重复消息"));
                return true;
            }

            const QString reason = QStringLiteral("已有其他 RECORDED 下载任务正在执行");
            emit logMessage(reason);
            emit videoControlNotice(reason);
            sendVideoControlAckNow(topic, payloadObj, false, false, true, true,
                                   QStringLiteral("download_busy"), reason);
            return true;
        }

        recordedDownloadTaskActive_ = true;
        recordedDownloadTaskKey_ = downloadTaskKey;
        pendingVideoCtrlAck_.active = true;
        pendingVideoCtrlAck_.reqTopic = topic;
        pendingVideoCtrlAck_.reqPayload = payloadObj;
    }

    bool ok = false;
    const bool handled = service_->handleVideoControlMessage(topic, payloadObj, &ok);
    if (!handled) {
        if (isRecordedDownloadRequest) {
            recordedDownloadTaskActive_ = false;
            recordedDownloadTaskKey_.clear();
            pendingVideoCtrlAck_ = PendingVideoControlAck{};
        }
        return false;
    }

    const PlayInfoState info = service_->currentPlayInfo();

    if (isRecordedDownloadRequest) {
        if (!recordedDownloadTaskActive_) {
            return true;
        }

        if (info.status.compare("downloading", Qt::CaseInsensitive) == 0) {
            emit logMessage("RECORDED 下载任务已进入预检查，等待 FTP 最终结果统一发送 videoControl 回执");
            return true;
        }

        notifyRecordedDownloadResult(false, false,
                                     QStringLiteral("setup_error"),
                                     QStringLiteral("RECORDED 下载任务初始化失败"));
        return true;
    }

    // 2) 有挂起的音量回执，等 notifyVolumeSetFinished() 后再统一发
    if (hasVideoVolume && !isRecordedDownloadRequest) {
        emit logMessage("当前请求包含 videoVolume，videoControl 回执由音量设置完成链路统一发送");
        return true;
    }

//    const QString videoType = dataObj.value("videoType").toString().trimmed().toUpper();

//    // RECORDED：不立刻回执，只记录请求，等 FTP 最终结果
//    if (videoType == "RECORDED") {
//        pendingVideoCtrlAck_.active = true;
//        pendingVideoCtrlAck_.reqTopic = topic;
//        pendingVideoCtrlAck_.reqPayload = payloadObj;

//        emit logMessage("RECORDED 已触发 FTP 下载，等待下载结果后发送 videoControl 回执");
//        return true;
//    }

    // 3) RECORDED 但没有 videoPath：

    if (isRecordedNoVideoPathRequest) {
        sendVideoControlAckNow(topic, payloadObj, ok, false, false, false);  // 不带 ftpDownloadstatus
        return true;
    }

    // 4) LIVE / NONE：仍然立即回执
    sendVideoControlAckNow(topic, payloadObj, ok, false);
    return true;
}


/** @brief 立即构建并发布 videoControl 确认包。 */
void MqttManager::sendVideoControlAckNow(const QString &reqTopic, const QJsonObject &reqPayload, bool ok,
                                         bool ftpDownloaded, bool includeStatus, bool includeFtpDownload,
                                         const QString &downloadResult, const QString &reason)
{
    if (!ipc_ || !ipc_->isConnected() || !service_) {
        emit logMessage("无法发送 videoControl 回执：IPC 未连接或 service 为空");
        return;
    }

    QString respTopic;
    QJsonObject packet =
        service_->buildVideoControlAckPublishPacket(reqTopic, reqPayload, ok, ftpDownloaded, &respTopic,
                                                    includeStatus, includeFtpDownload, downloadResult, reason);

    if (ipc_->sendJsonLine(packet)) {
        emit logMessage(QString("videoControl 回执已提交到 mqttd，respTopic=%1 status=%2 ftpDownload=%3 result=%4")
                        .arg(respTopic)
                        .arg(includeStatus ? (ok ? "ok" : "error") : "omitted")
                        .arg(includeFtpDownload ? (ftpDownloaded ? "100%" : "0%") : "omitted")
                        .arg(downloadResult.isEmpty() ? "omitted" : downloadResult));
    } else {
        emit logMessage(QString("videoControl 回执发送失败：IPC write 失败，respTopic=%1")
                        .arg(respTopic));
    }
}


/** @brief 完成待处理的录播下载控制确认。 */
void MqttManager::notifyRecordedDownloadResult(bool statusOk, bool ftpDownloaded,
                                                const QString &downloadResult,
                                                const QString &reason)
{
    if (!pendingVideoCtrlAck_.active) {
        emit logMessage("收到 FTP 下载结果，但当前没有待回执的 RECORDED 请求");
        recordedDownloadTaskActive_ = false;
        recordedDownloadTaskKey_.clear();
        return;
    }

    if (service_) {
        service_->updateRecordedDownloadStatus(statusOk, reason);
    }

    sendVideoControlAckNow(pendingVideoCtrlAck_.reqTopic,
                           pendingVideoCtrlAck_.reqPayload,
                           statusOk, ftpDownloaded, true, true,
                           downloadResult, reason);

    pendingVideoCtrlAck_.active = false;
    pendingVideoCtrlAck_.reqTopic.clear();
    pendingVideoCtrlAck_.reqPayload = QJsonObject{};
    recordedDownloadTaskActive_ = false;
    recordedDownloadTaskKey_.clear();
}


/**
 * @brief 处理视频控制类消息。
 *
 * 实际处理逻辑委托给 MqttService::handleVideoControlMessage()。
 *
 * @param topic      消息 topic。
 * @param payloadObj 消息 payload 对象。
 * @return true  已由 service 层识别并处理。
 * @return false 未识别为视频控制消息，或 service 不可用。
 */
bool MqttManager::handleStreamUrlMessage(const QString &topic,
                                         const QJsonObject &payloadObj)
{
    if (topic != cfg_.streamUrlTopic) {
        return false;
    }


    QString url = payloadObj.value("data").toObject().value("videoUrl").toString().trimmed();
    if (url.isEmpty()) {
        url = payloadObj.value("url").toString().trimmed();
    }

    if (url.isEmpty()) {
        return false;
    }

    if (waitingStreamUrl_) {
        waitingStreamUrl_ = false;
    }

    emit logMessage(QString("识别到Stream URL：%1").arg(url));
    emit streamUrlReceived(url);
    return true;
}


/**
 * @brief 解析并处理来自 mqttd IPC 的一条完整消息。
 *
 * 支持以下几类输入：
 * - mqttd 普通日志行；
 * - topic + JSON 形式的消息；
 * - 标准 mqttd JSON 协议消息；
 * - 损坏格式的 fallback JSON 消息。
 *
 * @param line 原始输入数据。
 */
void MqttManager::handleIpcLine(const QByteArray &line)
{
    QByteArray raw = line.trimmed();
    if (raw.size() > 16384) {
        emit logMessage(QStringLiteral("RX: large MQTT frame, bytes=%1")
                        .arg(raw.size()));
    } else {
        emit logMessage(QString("RX: %1").arg(QString::fromUtf8(raw)));
    }

    if (raw.isEmpty()) return;
    if (raw.startsWith('[')) return;

    // 1) topic + JSON
    if (!raw.startsWith('{')) {
        QString topic;
        QJsonObject payloadObj;
        if (parseTopicJsonLine(raw, topic, payloadObj)) {
            if (!dispatchPayloadMessage(topic, payloadObj)) {
                emit logMessage(QString("未处理的 payload 消息：topic=%1").arg(topic));
            }
        } else {
            emit logMessage("不是有效的 topic+JSON 行");
        }
        return;
    }

    // 2) 标准 JSON
    QJsonParseError e;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &e);

    if (e.error != QJsonParseError::NoError || !doc.isObject()) {
        QString topic;
        QJsonObject payloadObj;
        QString err2;
        if (extractPayloadObjectFromBrokenMsg(raw, topic, payloadObj, err2)) {
            if (!dispatchPayloadMessage(topic, payloadObj)) {
                emit logMessage(QString("未处理的 payload 消息：topic=%1").arg(topic));
            }
        } else {
            emit logMessage(QString("JSON 解析失败：%1；fallback也失败：%2")
                            .arg(e.errorString(), err2));
        }
        return;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();

    if (type == "hello" || type == "status" || type == "test_result") {
        handleStatusLikeMessage(obj);
        return;
    }

    if (type == "conn") {
        handleConnMessage(obj);
        return;
    }

    if (type == "ack") {
        handleAckMessage(obj);
        return;
    }

    if (type == "msg") {
        QString topic;
        QJsonObject payloadObj;
        if (extractPayloadObjectFromMsgObject(obj, topic, payloadObj)) {
            if (!dispatchPayloadMessage(topic, payloadObj)) {
                emit logMessage(QString("未处理的 payload 消息：topic=%1").arg(topic));
            }
        }
        return;
    }

    if (type == "error") {
        emit logMessage(QString("mqttd error：%1").arg(obj.value("msg").toString()));
        return;
    }

    emit logMessage(QString("未知类型 type=%1").arg(type));
}


/**
 * @brief 处理 hello / status / test_result 类型的状态类消息。
 *
 * 该函数负责更新：
 * - MQTT 连接状态文本；
 * - 当前订阅 topic 文本；
 * - 从 status 中同步回来的 MQTT 配置。
 *
 * 若类型为 test_result，还会输出一次连接测试结果日志。
 *
 * @param obj mqttd 状态类 JSON 对象。
 */
void MqttManager::handleStatusLikeMessage(const QJsonObject &obj)
{
    const QString type = obj.value("type").toString();
    const bool connected = obj.value("connected").toBool(false);
    const int lastRc = obj.value("last_rc").toInt(-1);
    mqttBrokerConnected_ = connected;
    if (connected) {
        if (IcEventBridge::instance()->networkAvailable()) {
            mqttReconnectRequiredAfterNetworkLoss_ = false;
        }
        flushOfflineAccessResults();
        resendPendingFaceImageRequestsAfterReconnect();
    } else if (faceImageReconnectResponseTimer_) {
        faceImageReconnectResponseTimer_->stop();
    }

    emit mqttStateTextChanged(
        connected ? QString("已连接（rc=%1）").arg(lastRc)
                  : QString("未连接（rc=%1）").arg(lastRc)
    );

    emit mqttConnMarkChanged(connected ? 2 : 3);
    emit subTopicsTextChanged(obj.value("sub_topics").toString());

    const QJsonObject mqtt = obj.value("mqtt").toObject();
    emit mqttConfigFromStatus(mqtt);

    if (ipcConfigApplyPending_
            && (type == QStringLiteral("hello")
                || type == QStringLiteral("status"))) {
        ipcConfigApplyPending_ = false;
        QStringList configuredTopics = cfg_.subTopics;
        if (configuredTopics.isEmpty()) configuredTopics.append(QStringLiteral("test"));
        configuredTopics.removeDuplicates();
        configuredTopics.sort();

        QString runningTopicsText = obj.value(QStringLiteral("sub_topics"))
                .toString();
        runningTopicsText.replace(QLatin1Char(';'), QLatin1Char(','));
        QStringList runningTopics = runningTopicsText.split(QLatin1Char(','));
        runningTopics.removeAll(QString());
        for (QString &topic : runningTopics) topic = topic.trimmed();
        runningTopics.removeAll(QString());
        runningTopics.removeDuplicates();
        runningTopics.sort();

        const bool sameRunningConfig = connected
                && mqtt.value(QStringLiteral("host")).toString().trimmed()
                    == cfg_.host.trimmed()
                && mqtt.value(QStringLiteral("port")).toInt() == cfg_.port
                && mqtt.value(QStringLiteral("client_id")).toString().trimmed()
                    == cfg_.clientId.trimmed()
                && mqtt.value(QStringLiteral("qos")).toInt() == cfg_.qos
                && mqtt.value(QStringLiteral("tls")).toBool() == cfg_.tls
                && runningTopics == configuredTopics;
        if (sameRunningConfig) {
            emit logMessage(QStringLiteral(
                                "IPC重连后mqttd配置和Broker连接仍有效，跳过apply"));
        } else {
            emit logMessage(QStringLiteral(
                                "IPC连接后的mqttd状态或配置不一致，执行一次apply"));
            sendConfigToMqttd(true);
        }
    }

    if (connected) {
        if (!mqttRoutePersisted_) {
            QString iniErr;
            if (persistCurrentMqttRouteToIni(&iniErr)) {
                mqttRoutePersisted_ = true;
                emit logMessage("MQTT 已连接，当前 topic 路由已写入 net_cfg.ini");
            } else {
                emit logMessage(QString("MQTT 已连接，但写入 topic 路由到 net_cfg.ini 失败：%1").arg(iniErr));
            }
        }
    } else {
        mqttRoutePersisted_ = false;
    }

    if (type == "test_result") {
        const QString host = mqtt.value("host").toString();
        const int port = mqtt.value("port").toInt();
        const QString clientId = mqtt.value("client_id").toString();
        const int qos = mqtt.value("qos").toInt(0);
        const bool tls = mqtt.value("tls").toBool(false);

        emit logMessage(
            connected
            ? QString("测试连接结果：连接正常 Broker=%1:%2 ClientId=%3 QoS=%4 TLS=%5")
                  .arg(host).arg(port).arg(clientId).arg(qos).arg(tls ? "ON" : "OFF")
            : QString("测试连接结果：连接异常 rc=%1 Broker=%2:%3")
                  .arg(lastRc).arg(host).arg(port)
        );
    }
}


/**
 * @brief 处理 conn 类型的连接状态消息。
 *
 * 支持如下状态：
 * - connected
 * - disconnected
 * - failed
 *
 * @param obj mqttd 连接状态 JSON 对象。
 */
void MqttManager::handleConnMessage(const QJsonObject &obj)
{
    const QString state = obj.value("state").toString();

    if (state == "connected") {
        mqttBrokerConnected_ = true;
        if (IcEventBridge::instance()->networkAvailable()) {
            mqttReconnectRequiredAfterNetworkLoss_ = false;
        }
        emit mqttStateTextChanged("已连接");
        emit mqttConnMarkChanged(2);

        if (!mqttRoutePersisted_) {
            QString iniErr;
            if (persistCurrentMqttRouteToIni(&iniErr)) {
                mqttRoutePersisted_ = true;
                emit logMessage("MQTT broker 已连接，当前 topic 路由已写入 net_cfg.ini");
            } else {
                emit logMessage(QString("MQTT broker 已连接，但写入 topic 路由失败：%1").arg(iniErr));
            }
        }
        flushOfflineAccessResults();
        resendPendingFaceImageRequestsAfterReconnect();
        return;
    }

    if (state == "disconnected") {
        mqttBrokerConnected_ = false;
        if (faceImageReconnectResponseTimer_) {
            faceImageReconnectResponseTimer_->stop();
        }
        faceImageReconnectAwaitingPersons_.clear();
        const int rc = obj.value(QStringLiteral("rc")).toInt(-1);
        const QString desc = obj.value(QStringLiteral("desc"))
                .toString().trimmed();
        for (auto it = pendingFaceImageRequests_.cbegin();
             it != pendingFaceImageRequests_.cend(); ++it) {
            if (it.value().awaitingResponse) {
                faceImageReconnectRetryPersons_.insert(it.key());
            }
        }
        if (!faceImageReconnectRetryPersons_.isEmpty()) {
            emit logMessage(QStringLiteral(
                                "MQTT连接断开，已标记%1个尚未收到responseImages的请求，重连后补发一次：rc=%2 desc=%3")
                            .arg(faceImageReconnectRetryPersons_.size())
                            .arg(rc)
                            .arg(desc.isEmpty() ? QStringLiteral("<empty>") : desc));
        }
        mqttRoutePersisted_ = false;
        emit mqttStateTextChanged("已断开");
        emit mqttConnMarkChanged(3);
        return;
    }

    if (state == "failed") {
        mqttBrokerConnected_ = false;
        if (faceImageReconnectResponseTimer_) {
            faceImageReconnectResponseTimer_->stop();
        }
        faceImageReconnectAwaitingPersons_.clear();
        mqttRoutePersisted_ = false;
        const int rc = obj.value("rc").toInt(-1);
        emit mqttStateTextChanged(QString("连接失败（rc=%1）").arg(rc));
        emit mqttConnMarkChanged(3);
        return;
    }
}


/**
 * @brief 处理 ack 类型的命令应答消息。
 *
 * @param obj mqttd ack JSON 对象。
 */
void MqttManager::handleAckMessage(const QJsonObject &obj)
{
    const QString cmd = obj.value("cmd").toString();
    const bool ok = obj.value("ok").toBool(false);
    const QString msg = obj.value("msg").toString();

    emit logMessage(QString("ACK: cmd=%1 ok=%2 msg=%3")
                    .arg(cmd)
                    .arg(ok ? "true" : "false")
                    .arg(msg));
}

/**
 * @brief 设置当前录制文件路径。
 *
 * @param fullPath 当前录制文件完整路径。
 */
void MqttManager::setCurrentRecordedFile(const QString &fullPath)
{

    if (service_) service_->setCurrentRecordedFile(fullPath);
}


/**
 * @brief 清空当前录制文件记录。
 */
void MqttManager::clearCurrentRecordedFile()
{
    if (service_) service_->clearCurrentRecordedFile();
}


/**
 * @brief 更新当前播放画面的 base64 图像数据。
 *
 * @param base64Image base64 编码图像字符串。
 */
void MqttManager::updateCurrentPlayImage(const QString &base64Image)
{
    if (service_) service_->updateCurrentPlayImage(base64Image);
}


/**
 * @brief 立即发送一次播放信息响应消息。
 *
 * 该接口通常用于收到 getPlayInfo 请求后，主动构造并回发当前播放信息。
 *
 * @param reqTopic 请求 topic。
 * @param reqId    请求 ID。
 * @param deviceId 设备 ID。
 */
void MqttManager::sendPlayInfoNow(const QString &reqTopic,
                                  const QString &reqId,
                                  const QString &deviceId)
{
    emit logMessage(QString("[DBG] sendPlayInfoNow enter topic=%1 id=%2 deviceId=%3")
                    .arg(reqTopic, reqId, deviceId));

    if (!ipc_ || !ipc_->isConnected() || !service_) {
        emit logMessage("发送 sendPlayInfo 失败：IPC未连接或service为空");
        return;
    }

    QString respTopic;
    QJsonObject packet = service_->buildPlayInfoPublishPacket(reqTopic, reqId, deviceId, &respTopic);

    if (ipc_->sendJsonLine(packet)) {
        emit logMessage(QString("sendPlayInfo 已提交到 mqttd，respTopic=%1").arg(respTopic));
    } else {
        emit logMessage("发送 sendPlayInfo 失败：IPC write 失败");
    }
}


/**
 * @brief 更新当前播放音量信息。
 *
 * @param volume 当前音量值。
 */
void MqttManager::updateCurrentPlayVolume(int volume)
{
    if (service_) {
        service_->updateCurrentPlayVolume(volume);
    }
}


/** @brief 更新当前设备日志文本。 */
void MqttManager::updateCurrentDeviceLog(const QString &deviceLog)
{
    if (service_) {
        service_->updateCurrentDeviceLog(deviceLog);
    }
}

/** @brief 清除当前设备日志。 */
void MqttManager::clearCurrentDeviceLog()
{
    if (service_) {
        service_->clearCurrentDeviceLog();
    }
}


/** @brief 在允许自动重连时启动固定间隔定时器。 */
void MqttManager::startIpcReconnect()
{
    if (!ipcAutoReconnect_) return;
    if (!ipcReconnectTimer_) return;

    if (!ipcReconnectTimer_->isActive()) {
        emit logMessage(QString("启动 IPC 自动重连，间隔=%1ms").arg(ipcReconnectIntervalMs_));
        ipcReconnectTimer_->start();
    }
}

/** @brief 停止 IPC 重连定时器。 */
void MqttManager::stopIpcReconnect()
{
    if (ipcReconnectTimer_ && ipcReconnectTimer_->isActive()) {
        ipcReconnectTimer_->stop();
        emit logMessage("停止 IPC 自动重连");
    }
}

/** @brief 接收音量控制执行结果并发布确认。 */
void MqttManager::onVolumeControlResultReady(bool ok,
                                             const QString &reqTopic,
                                             const QJsonObject &reqPayload)
{
    const QJsonObject dataObj = reqPayload.value("data").toObject();
        const QString videoType = dataObj.value("videoType").toString().trimmed().toUpper();

        QString videoPath = dataObj.value("videoPath").toString().trimmed();
        videoPath = QFileInfo(videoPath).fileName().trimmed();

        const bool isRecordedNoVideoPathRequest =
                (videoType == "RECORDED" && videoPath.isEmpty());

        //RECORDED 不带 videoPath 的音量调整：
        if (isRecordedNoVideoPathRequest) {
            sendVideoControlAckNow(reqTopic,
                                   reqPayload,
                                   ok,
                                   false,
                                   true,   // includeStatus
                                   false);  // includeFtpDownload

            emit logMessage("RECORDED 无 videoPath 音量设置完成，已发送 videoControl 回执，不带 status 和 ftpDownload");
            return;
        }

        /*
         * 其他 videoControl 音量请求保持原逻辑。
         */
        sendVideoControlAckNow(reqTopic,
                               reqPayload,
                               ok,
                               false,
                               true,
                               true);

        emit logMessage(QString("音量设置完成，已发送 videoControl 回执 status=%1")
                        .arg(ok ? "ok" : "error"));
}


/** @brief 发布一次本地音量变更状态。 */
void MqttManager::publishLocalVideoControlStatus(int volume, bool ok, const QString &reason)
{
    if (!ipc_ || !ipc_->isConnected() || !service_) {
        emit logMessage("无法发送本地音量同步：IPC 未连接或 service 为空");
        return;
    }

    QString respTopic;
    QJsonObject packet =
        service_->buildLocalVideoControlStatusPublishPacket(
            cfg_.streamUrlTopic,   // request topic 对应的设备 topic
            cfg_.clientId,         // 现在 clientId 基本就是设备ID来源
            volume,
            ok,
            &respTopic
        );

    if (ipc_->sendJsonLine(packet)) {
        emit logMessage(QString("本地音量同步已提交到 mqttd，respTopic=%1 status=%2 volume=%3")
                        .arg(respTopic, ok ? "ok" : "error")
                        .arg(volume));
    } else {
        emit logMessage(QString("本地音量同步发送失败：IPC write 失败，respTopic=%1").arg(respTopic));
    }
}



/** @brief 通知业务服务本地音量设置完成。 */
void MqttManager::notifyVolumeSetFinished(bool ok, const QString &reason)
{
    if (service_) {
        service_->notifyVolumeSetFinished(ok, reason);
    }
}
