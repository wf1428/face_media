/** @file web_floor_schedule.cpp
 *  @brief Web独立规则校验、事务存储和定时执行；共享规则兼容原协议，MQTT源码保持独立。
 */
#include "web_floor_schedule.h"
#include "common/sql/dbstore.h"
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
/**
 * @brief 将业务楼层转换为共享状态中的位索引。
 * @param floor 业务楼层号，支持 -8~-1 和 1~120，0 无效。
 * @return 正楼层对应 0~119，负楼层对应 120~127；无效楼层返回 -1。
 */
int floorBit(int floor)
{
    if (floor >= 1 && floor <= 120) return floor - 1;
    if (floor >= -8 && floor <= -1) return floor + 128;
    return -1;
}

/**
 * @brief 读取共享楼层开放位，损坏时回退为全 0。
 * @return 恰好 128 字节的 0/1 状态。
 */
QByteArray readCurrentBits()
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

/**
 * @brief 读取与 MQTT 共用的规则 JSON，格式无效时返回空数组。
 * @return 已保存规则数组；缺失或解析失败时为空。
 */
QJsonArray readSchedules()
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

/**
 * @brief 读取定时规则开放位，损坏时回退为全 0。
 * @return 恰好 128 字节的 0/1 定时状态。
 */
QByteArray readScheduleBits()
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

/**
 * @brief 收集规则涉及的楼层并去重。
 * @param schedules 按约定字段保存的定时规则数组。
 * @return 规则涉及的唯一楼层集合。
 */
QSet<int> scheduleFloors(const QJsonArray &schedules)
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

/**
 * @brief 将业务开放位转换为分层板持久控制帧。
 * @param openBits 128 字节业务开放位，1=开放；顺序为 1~120、-8~-1。
 * @return 132 字节 OPEN 帧；板端位序 -8~-1、1~120，0=开放、1=限行。
 * @note 输入至少包含 128 字节；特殊模式叠加由公共 RS485 出口处理。
 */
QByteArray batchFrame(const QByteArray &openBits)
{
    // 完全沿用 online_v2 批量交通梯帧："OPEN" + 128 个限行状态字符。
    // 分层板位序为 -8~-1、1~120；状态字符 0=开放、1=限行。
    QByteArray restrictionBits(128, '1');
    for (int floor = -8; floor <= -1; ++floor) {
        const int stateBit = floorBit(floor);
        const int boardBit = floor + 8;
        restrictionBits[boardBit] = openBits.at(stateBit) == '1' ? '0' : '1';
    }
    for (int floor = 1; floor <= 120; ++floor) {
        const int stateBit = floorBit(floor);
        const int boardBit = floor + 7;
        restrictionBits[boardBit] = openBits.at(stateBit) == '1' ? '0' : '1';
    }

    QByteArray frame("OPEN");
    frame.append(restrictionBits);
    return frame;
}

/**
 * @brief 校验 JSON 数值为范围内的整数，拒绝字符串和小数。
 * @param value 待检查的 JSON 数值。
 * @param minimum 允许的整数下界，含边界。
 * @param maximum 允许的整数上界，含边界。
 * @param[out] result 可选整数输出，仅校验成功时写入。
 * @return 值是指定闭区间的整数时为 true，失败不写输出。
 */
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

/**
 * @brief 校验规则字段并规范化，拒绝同一楼层、星期和时刻的冲突动作。
 * @param data 本次操作的数据对象。
 * @param[out] normalizedSchedules 接收通过校验且去重、排序后的规则数组，不可为空。
 * @param[out] error 可选输出，接收失败说明。
 * @return 规则全部合法时为 true；否则为 false，并提供错误原因。
 */
bool normalizeSchedules(const QJsonObject &data,
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

/**
 * @brief 事务保存共享规则及定时状态，必要时重置执行分钟标记。
 * @param schedules 按约定字段保存的定时规则数组。
 * @param scheduleBits 128 字节定时开放状态，1=开放。
 * @param resetExecutionMinute 规则变化时是否清除共享的已执行分钟标记。
 * @param[out] error 可选输出，接收失败说明。
 * @return 事务提交成功时为 true，失败回滚并返回 false。
 */
bool saveSchedules(const QJsonArray &schedules,
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
    const bool localSaved = markerSaved && DbStore::setConfig(
                QStringLiteral("floor_schedule_local_enabled"), QStringLiteral("1"));
    if (!localSaved || !DbStore::transactionCommit()) {
        const QString dbError = DbStore::lastError();
        DbStore::transactionRollback();
        if (error) *error = dbError;
        return false;
    }
    DbStore::checkpoint();
    return true;
}

/**
 * @brief 事务保存定时与当前楼层状态及共享分钟去重标记。
 * @param scheduleBits 128 字节定时开放状态，1=开放。
 * @param currentFloorBits 128 字节当前楼层开放状态，1=开放。
 * @param minuteKey 设备本地时间的 yyyy-MM-dd HH:mm 去重标记。
 * @param[out] error 可选输出，接收失败说明。
 * @return 状态及分钟标记提交成功时为 true，失败回滚并返回 false。
 */
bool saveExecution(const QByteArray &scheduleBits,
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

/**
 * @brief 建立独立 Web 定时执行器，并每 5 秒检查一次到期规则。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 * @param sendFrame 公共 RS485 发送回调，返回是否受理本次发送。
 */
WebFloorSchedule::WebFloorSchedule(QObject *parent, SendFrame sendFrame)
    : QObject(parent), sendFrame_(std::move(sendFrame))
{
    auto *timer = new QTimer(this);
    timer->setInterval(5000);
    // MQTT原定时器与此定时器都在GUI线程；共享分钟标记使同一分钟只发送一次。
    connect(timer, &QTimer::timeout, this, [this]() { checkDue(); });
    timer->start();
}

/**
 * @brief 本地入口与MQTT使用相同规范、状态位和分钟去重标记。
 * @param action 规则操作：set、delete 或 resend。
 * @param data 本次操作的数据对象。
 * @param[out] status 接收本次操作的业务状态码。
 * @param[out] error 可选输出，接收失败说明。
 * @return 成功时返回已保存规则及可写状态；失败返回空对象，并设置 status 和 error。
 * @note 在对象所属线程调用；baseSchedules 不匹配当前快照时拒绝覆盖并返回 409。
 */
QJsonObject WebFloorSchedule::apply(const QString &action, const QJsonObject &data,
                                               int *status, QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (error) error->clear();
    if (status) *status = 200;
    const auto fail = [&](int code, const QString &message) {
        if (status) *status = code;
        if (error) *error = message;
        return QJsonObject();
    };
    const QJsonArray old = readSchedules();
    if (data.contains("baseSchedules") &&
        (!data.value("baseSchedules").isArray() || data.value("baseSchedules").toArray() != old))
        return fail(409, QStringLiteral("规则已被其他入口更新，请刷新后再保存"));

    QJsonArray next = old;
    QString validation;
    if (action == "set") {
        if (!normalizeSchedules(data, &next, &validation)) return fail(400, validation);
    } else if (action == "delete") {
        const auto hashes = data.value("hashList");
        if (!hashes.isArray() || hashes.toArray().isEmpty())
            return fail(400, QStringLiteral("hashList 必须是非空数组"));
        QSet<QString> remove;
        for (const auto &value : hashes.toArray()) {
            if (!value.isString() || value.toString().trimmed().isEmpty())
                return fail(400, QStringLiteral("hashList 元素必须是非空字符串"));
            remove.insert(value.toString().trimmed());
        }
        next = QJsonArray();
        for (const auto &value : old)
            if (!remove.contains(value.toObject().value("hash").toString().trimmed())) next.append(value);
    } else if (action != "resend") {
        return fail(400, QStringLiteral("定时规则操作无效"));
    }
    const bool changed = next != old;
    QByteArray bits = readScheduleBits();
    if (changed) {
        const QByteArray source = action == "delete" ? bits : readCurrentBits();
        bits = QByteArray(128, '0');
        for (int floor : scheduleFloors(next)) {
            const int bit = floorBit(floor);
            bits[bit] = source.at(bit);
        }
    }
    // 规则、状态和本地执行开关一起提交；相同规则不清除已执行分钟。
    if (!saveSchedules(next, bits, changed && action == "set", &validation))
        return fail(503, validation);
    if (action != "delete") checkDue();
    qInfo() << (QStringLiteral("Web 定时梯控已保存: action=%1 rules=%2").arg(action).arg(next.size()));
    return {{"deviceSchedules", next}, {"writable", true}, {"saved", true}};
}

/**
 * @brief 按设备本地时间检查并执行到期楼层规则。
 * @param now 设备本地时间；默认取当前时间。
 * @note 每 5 秒检查设备本地分钟；先事务保存状态及共享分钟标记，再调用公共串口出口，避免两套入口重复执行。
 */
void WebFloorSchedule::checkDue(const QDateTime &now)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!sendFrame_ || !DbStore::getConfig(
                QStringLiteral("floor_schedule_local_enabled"), false).toBool()) return;

    const QJsonArray schedules = readSchedules();
    if (schedules.isEmpty()) return;

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
    QByteArray scheduleBits = readScheduleBits();
    QByteArray currentFloorBits = readCurrentBits();
    for (int floor : floors) {
        const int bit = floorBit(floor);
        const char state = actionsByFloor.value(floor) ? '1' : '0';
        scheduleBits[bit] = state;
        // 到达定时时间后，定时动作成为该楼层最新的设备当前状态。
        currentFloorBits[bit] = state;
    }

    QString saveError;
    if (!saveExecution(scheduleBits,
                                            currentFloorBits,
                                            minuteKey,
                                            &saveError)) {
        qInfo() << (QStringLiteral("floorSchedule 到时状态保存失败: %1")
                        .arg(saveError));
        return;
    }

    const QByteArray frame = batchFrame(currentFloorBits);
    const QString sourceTag = QStringLiteral("webFloorSchedule:%1:batch")
            .arg(minuteKey);
    if (!sendFrame_(frame, sourceTag)) qWarning() << "[Web schedule] RS485 request rejected" << sourceTag;
    qInfo() << (QStringLiteral(
                        "floorSchedule 到时执行全量帧: minute=%1 floors=%2 len=%3 frame=%4")
                    .arg(minuteKey)
                    .arg(floors.size())
                    .arg(frame.size())
                    .arg(QString::fromLatin1(frame.toHex(' '))));
}
