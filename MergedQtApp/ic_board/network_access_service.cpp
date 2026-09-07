/**
 * @file network_access_service.cpp
 * @brief online_v1 网络人员本地权限判定实现。
 */
#include "network_access_service.h"

#include "common/sql/dbstore.h"
#include "ic_board/rs485_floor_frame_builder.h"
#include "platform/rk3566_platform.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSettings>
#include <QStringList>
#include <QTime>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kAccessCountExhaustedCode = 4200;
constexpr int kAccessBalanceInsufficientCode = 4201;
constexpr double kAmountEpsilon = 0.000001;

QString accessRuleFailureReason(int code)
{
    switch (code) {
    case kAccessCountExhaustedCode:
        return QStringLiteral("通行次数已用完，请联系管理员充值");
    case kAccessBalanceInsufficientCode:
        return QStringLiteral("通行余额不足，请联系管理员充值");
    default:
        return QStringLiteral("通行权限校验失败");
    }
}

double normalizedAmount(double value)
{
    return std::round(value * 1000000.0) / 1000000.0;
}

NetworkAccessResult denied(const QString &reason,
                           const QString &personId = QString(),
                           const QString &credential = QString(),
                           const QString &credentialType = QString(),
                           int code = 0)
{
    NetworkAccessResult result;
    result.code = code;
    result.reason = reason;
    result.personId = personId;
    result.credential = credential;
    result.credentialType = credentialType;
    return result;
}

QString currentDeviceId()
{
    QSettings ini(Rk3566Platform::netConfigPath(), QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    return ini.value(QStringLiteral("mqtt/client_id")).toString().trimmed();
}

QDateTime parseServerDateTime(const QString &value)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) return {};
    QDateTime dt = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!dt.isValid()) dt = QDateTime::fromString(text, Qt::ISODate);
    if (!dt.isValid()) {
        dt = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    return dt;
}

QStringList cardCandidates(const QString &readerValue)
{
    const QString raw = readerValue.trimmed().toUpper();
    QStringList values;
    if (raw.isEmpty()) return values;
    values.append(raw);

    bool hexOk = false;
    const qulonglong hexValue = raw.toULongLong(&hexOk, 16);
    if (hexOk && hexValue <= 0xFFFFFFFFULL) {
        const QString decimal = QString::number(hexValue);
        if (!values.contains(decimal)) values.append(decimal);

        // 读卡器帧按字节显示为大端十六进制，而服务器卡号使用四字节
        // 小端反序后的无符号十进制。例如 5EF4603F -> 3F60F45E -> 1063318622。
        if (raw.size() == 8) {
            QByteArray bytes = QByteArray::fromHex(raw.toLatin1());
            if (bytes.size() == 4) {
                std::reverse(bytes.begin(), bytes.end());
                const QString reversedHex = QString::fromLatin1(bytes.toHex()).toUpper();
                if (!values.contains(reversedHex)) values.append(reversedHex);

                bool reversedOk = false;
                const qulonglong reversedValue = reversedHex.toULongLong(&reversedOk, 16);
                if (reversedOk) {
                    const QString reversedDecimal = QString::number(reversedValue);
                    if (!values.contains(reversedDecimal)) values.append(reversedDecimal);
                }
            }
        }
    }

    bool decimalOk = false;
    const qulonglong decimalValue = raw.toULongLong(&decimalOk, 10);
    if (decimalOk && decimalValue <= 0xFFFFFFFFULL) {
        const QString hex = QStringLiteral("%1")
                .arg(decimalValue, 8, 16, QLatin1Char('0')).toUpper();
        if (!values.contains(hex)) values.append(hex);
    }
    return values;
}

bool timeWindowMatches(const QVariantMap &rule, const QDateTime &now)
{
    QJsonParseError error;
    const QJsonDocument daysDoc = QJsonDocument::fromJson(
                rule.value(QStringLiteral("days_json")).toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !daysDoc.isArray()) return false;

    bool dayMatches = false;
    for (const QJsonValue &day : daysDoc.array()) {
        if (day.toInt(-1) == now.date().dayOfWeek()) {
            dayMatches = true;
            break;
        }
    }
    if (!dayMatches) return false;

    const QTime start = QTime::fromString(
                rule.value(QStringLiteral("start_time")).toString(), QStringLiteral("HH:mm"));
    const QTime end = QTime::fromString(
                rule.value(QStringLiteral("end_time")).toString(), QStringLiteral("HH:mm"));
    if (!start.isValid() || !end.isValid()) return false;

    const QTime current = now.time();
    if (start <= end) return current >= start && current <= end;
    return current >= start || current <= end;
}

} // namespace

bool NetworkAccessService::initialize()
{
    if (!personnelStore_.initialize()) return false;
    return DbStore::exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS network_access_usage ("
        "person_id TEXT PRIMARY KEY,"
        "remaining_count INTEGER,"
        "access_count INTEGER NOT NULL DEFAULT 0,"
        "remaining_amount REAL,"
        "used_amount REAL NOT NULL DEFAULT 0,"
        "updated_at TEXT NOT NULL,"
        "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"));
}

NetworkAccessResult NetworkAccessService::checkCardFrame(const QByteArray &cardFrame,
                                                         const QDateTime &now)
{
    if (!initialize()) return denied(QStringLiteral("网络权限数据库初始化失败"));
    if (cardFrame.size() < 43) return denied(QStringLiteral("刷卡数据长度不足"));

    const QString readerCard = QString::fromLatin1(cardFrame.mid(35, 8)).trimmed().toUpper();
    const QStringList candidates = cardCandidates(readerCard);
    if (candidates.isEmpty()) return denied(QStringLiteral("未解析到卡号"));

    QStringList placeholders;
    QList<QVariant> binds;
    for (const QString &candidate : candidates) {
        placeholders.append(QStringLiteral("?"));
        binds.append(candidate);
    }
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT c.person_id,c.card_id FROM network_ic_card c "
                               "JOIN network_person p ON p.person_id=c.person_id "
                               "WHERE UPPER(c.card_id) IN (%1) AND c.active=1 "
                               "AND p.active=1 AND p.deleted=0 AND p.status=1 LIMIT 1")
                    .arg(placeholders.join(QLatin1Char(','))), binds);
    if (rows.isEmpty()) {
        return denied(QStringLiteral("卡号未登记"), QString(), readerCard,
                      QStringLiteral("IC_CARD"));
    }

    return evaluatePerson(rows.first().value(QStringLiteral("person_id")).toString(),
                          rows.first().value(QStringLiteral("card_id")).toString(),
                          QStringLiteral("IC_CARD"), now);
}

NetworkAccessResult NetworkAccessService::checkQrCode(const QString &qrCode,
                                                       const QDateTime &now)
{
    if (!initialize()) return denied(QStringLiteral("网络权限数据库初始化失败"));
    const QString value = qrCode.trimmed();
    if (value.isEmpty()) return denied(QStringLiteral("二维码为空"));

    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT q.person_id,q.valid_from,q.valid_to,"
                               "q.max_use_count,q.used_count FROM network_qr_code q "
                               "JOIN network_person p ON p.person_id=q.person_id "
                               "WHERE q.qr_unique=? AND q.active=1 "
                               "AND p.active=1 AND p.deleted=0 AND p.status=1 LIMIT 1"),
                {value});
    if (rows.isEmpty()) {
        return denied(QStringLiteral("二维码未登记"), QString(), value,
                      QStringLiteral("QR_CODE"));
    }

    const QVariantMap row = rows.first();
    const QString personId = row.value(QStringLiteral("person_id")).toString();
    const QDateTime validFrom = parseServerDateTime(
                row.value(QStringLiteral("valid_from")).toString());
    const QDateTime validTo = parseServerDateTime(
                row.value(QStringLiteral("valid_to")).toString());
    if (validFrom.isValid() && now < validFrom) {
        return denied(QStringLiteral("二维码尚未生效"), personId, value,
                      QStringLiteral("QR_CODE"));
    }
    if (validTo.isValid() && now > validTo) {
        return denied(QStringLiteral("二维码已过期"), personId, value,
                      QStringLiteral("QR_CODE"));
    }
    const int maxUseCount = row.value(QStringLiteral("max_use_count")).toInt();
    if (!row.value(QStringLiteral("max_use_count")).isNull()
            && maxUseCount > 0
            && row.value(QStringLiteral("used_count")).toInt() >= maxUseCount) {
        return denied(QStringLiteral("二维码使用次数已用完"), personId, value,
                      QStringLiteral("QR_CODE"));
    }
    return evaluatePerson(personId, value, QStringLiteral("QR_CODE"), now);
}

NetworkAccessResult NetworkAccessService::checkFacePerson(const QString &personId,
                                                           const QDateTime &now)
{
    if (!initialize()) {
        return denied(QStringLiteral("网络权限数据库初始化失败"));
    }

    const QString value = personId.trimmed();
    if (value.isEmpty()) {
        return denied(QStringLiteral("人脸人员ID为空"), QString(), value,
                      QStringLiteral("FACE"));
    }

    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT person_id FROM network_person "
                               "WHERE person_id=? AND active=1 AND deleted=0 "
                               "AND status=1 LIMIT 1"),
                {value});
    if (rows.isEmpty()) {
        return denied(QStringLiteral("人脸人员ID未匹配到有效人员"), value, value,
                      QStringLiteral("FACE"));
    }

    return evaluatePerson(value, value, QStringLiteral("FACE"), now);
}

NetworkAccessResult NetworkAccessService::checkPassword(const QString &password,
                                                         const QDateTime &now)
{
    if (!initialize()) {
        return denied(QStringLiteral("网络权限数据库初始化失败"));
    }

    const QString value = password.trimmed();
    if (value.isEmpty()) {
        return denied(QStringLiteral("通行密码不能为空"), QString(), QString(),
                      QStringLiteral("PASSWORD"));
    }

    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT r.person_id FROM network_access_rule r "
                               "JOIN network_person p ON p.person_id=r.person_id "
                               "WHERE r.card_password_enabled=1 AND r.card_password=? "
                               "AND p.active=1 AND p.deleted=0 AND p.status=1 "
                               "ORDER BY r.person_id LIMIT 2"),
                {value});
    if (rows.isEmpty()) {
        return denied(QStringLiteral("通行密码错误"), QString(), QString(),
                      QStringLiteral("PASSWORD"));
    }
    if (rows.size() > 1) {
        return denied(QStringLiteral("通行密码对应多个人员，无法确定楼层权限"),
                      QString(), QString(), QStringLiteral("PASSWORD"));
    }

    const QString personId = rows.first().value(
                QStringLiteral("person_id")).toString().trimmed();
    return evaluatePerson(personId, QString(), QStringLiteral("PASSWORD"), now);
}

NetworkAccessResult NetworkAccessService::evaluatePerson(const QString &personId,
                                                          const QString &credential,
                                                          const QString &credentialType,
                                                          const QDateTime &now)
{
    const QList<QVariantMap> rules = DbStore::query(
                QStringLiteral("SELECT * FROM network_access_rule WHERE person_id=?"), {personId});
    if (rules.isEmpty()) {
        return denied(QStringLiteral("人员权限规则不存在"), personId, credential, credentialType);
    }
    const QVariantMap rule = rules.first();
    if (rule.value(QStringLiteral("control_elevator")).toInt() != 1) {
        return denied(QStringLiteral("人员未启用控梯权限"), personId, credential, credentialType);
    }

    // rules.duration controls the overall permission validity period. It is
    // independent from rules.time, which controls recurring timingRules slots.
    if (rule.value(QStringLiteral("duration_enabled")).toInt() == 1) {
        const QDateTime start = parseServerDateTime(
                    rule.value(QStringLiteral("duration_start")).toString());
        const QDateTime end = parseServerDateTime(
                    rule.value(QStringLiteral("duration_end")).toString());
        if (!start.isValid() || !end.isValid() || start > end) {
            return denied(QStringLiteral("人员通行期限配置无效"), personId,
                          credential, credentialType);
        }
        if (now < start) {
            return denied(QStringLiteral("人员通行权限尚未生效"), personId,
                          credential, credentialType);
        }
        if (now > end) {
            return denied(QStringLiteral("人员通行权限已过期"), personId,
                          credential, credentialType);
        }
    }

    const QList<QVariantMap> timingRules = DbStore::query(
                QStringLiteral("SELECT days_json,start_time,end_time "
                               "FROM network_timing_rule WHERE person_id=? ORDER BY rule_order"),
                {personId});
    if (rule.value(QStringLiteral("time_enabled")).toInt() == 1
            && !timingRules.isEmpty()) {
        bool matched = false;
        for (const QVariantMap &timing : timingRules) {
            if (timeWindowMatches(timing, now)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return denied(QStringLiteral("当前时间不在定时规则允许时段"), personId,
                          credential, credentialType);
        }
    }

    const bool countEnabled = rule.value(QStringLiteral("count_enabled")).toInt() == 1;
    const bool amountEnabled = rule.value(QStringLiteral("amount_enabled")).toInt() == 1;
    if (countEnabled || amountEnabled) {
        QList<QVariantMap> usage = DbStore::query(
                    QStringLiteral("SELECT remaining_count,access_count,"
                                   "remaining_amount,used_amount "
                                   "FROM network_access_usage WHERE person_id=?"),
                    {personId});
        if (usage.isEmpty()) {
            const QVariant initialCount = countEnabled
                    ? rule.value(QStringLiteral("count_total")) : QVariant();
            const QVariant initialAmount = amountEnabled
                    ? rule.value(QStringLiteral("amount_total")) : QVariant();
            if (!DbStore::execute(
                        QStringLiteral("INSERT INTO network_access_usage("
                                       "person_id,remaining_count,access_count,"
                                       "remaining_amount,used_amount,updated_at) "
                                       "VALUES(?,?,0,?,0,?)"),
                        {personId, initialCount, initialAmount,
                         now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))})) {
                return denied(QStringLiteral("通行次数/金额状态初始化失败"), personId,
                              credential, credentialType);
            }
            QVariantMap initialUsage;
            initialUsage.insert(QStringLiteral("remaining_count"), initialCount);
            initialUsage.insert(QStringLiteral("access_count"), 0);
            initialUsage.insert(QStringLiteral("remaining_amount"), initialAmount);
            initialUsage.insert(QStringLiteral("used_amount"), 0.0);
            usage.append(initialUsage);
        }

        QVariantMap usageState = usage.first();
        if (countEnabled
                && usageState.value(QStringLiteral("remaining_count")).isNull()) {
            const QVariant initialCount = rule.value(QStringLiteral("count_total"));
            if (!DbStore::execute(
                        QStringLiteral("UPDATE network_access_usage SET "
                                       "remaining_count=?,updated_at=? WHERE person_id=?"),
                        {initialCount,
                         now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                         personId})) {
                return denied(QStringLiteral("权限次数状态初始化失败"), personId,
                              credential, credentialType);
            }
            usageState.insert(QStringLiteral("remaining_count"), initialCount);
        }
        if (amountEnabled
                && usageState.value(QStringLiteral("remaining_amount")).isNull()) {
            const QVariant initialAmount = rule.value(QStringLiteral("amount_total"));
            if (!DbStore::execute(
                        QStringLiteral("UPDATE network_access_usage SET "
                                       "remaining_amount=?,updated_at=? WHERE person_id=?"),
                        {initialAmount,
                         now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                         personId})) {
                return denied(QStringLiteral("权限金额状态初始化失败"), personId,
                              credential, credentialType);
            }
            usageState.insert(QStringLiteral("remaining_amount"), initialAmount);
        }

        if (countEnabled
                && usageState.value(QStringLiteral("remaining_count")).toLongLong() <= 0) {
            return denied(accessRuleFailureReason(kAccessCountExhaustedCode), personId,
                          credential, credentialType, kAccessCountExhaustedCode);
        }

        if (amountEnabled) {
            bool unitPriceOk = false;
            bool remainingAmountOk = false;
            const double unitPrice = rule.value(QStringLiteral("amount_unit_price"))
                    .toDouble(&unitPriceOk);
            const double remainingAmount = usageState
                    .value(QStringLiteral("remaining_amount"))
                    .toDouble(&remainingAmountOk);
            if (!unitPriceOk || !std::isfinite(unitPrice) || unitPrice <= 0.0) {
                return denied(QStringLiteral("通行金额单价配置无效"), personId,
                              credential, credentialType);
            }
            if (!remainingAmountOk || !std::isfinite(remainingAmount)
                    || remainingAmount <= 0.0
                    || remainingAmount + kAmountEpsilon < unitPrice) {
                return denied(accessRuleFailureReason(kAccessBalanceInsufficientCode),
                              personId, credential, credentialType,
                              kAccessBalanceInsufficientCode);
            }
        }
    }

    const QString deviceId = currentDeviceId();
    QList<QVariantMap> floorRows;
    if (deviceId.isEmpty()) {
        floorRows = DbStore::query(
                    QStringLiteral("SELECT floor_no FROM network_floor "
                                   "WHERE person_id=? ORDER BY floor_no"), {personId});
    } else {
        floorRows = DbStore::query(
                    QStringLiteral("SELECT floor_no FROM network_floor "
                                   "WHERE person_id=? AND device_id=? ORDER BY floor_no"),
                    {personId, deviceId});
    }
    if (floorRows.isEmpty()) {
        return denied(QStringLiteral("当前设备没有可用楼层"), personId,
                      credential, credentialType);
    }

    QList<int> floors;
    for (const QVariantMap &floor : floorRows) {
        floors.append(floor.value(QStringLiteral("floor_no")).toInt());
    }
    std::sort(floors.begin(), floors.end());

    if (Rs485FloorFrameBuilder::shouldRejectModeOccupiedFloors(floors)) {
        return denied(QStringLiteral("楼层已被电梯模式占用"), personId,
                      credential, credentialType);
    }

    QString frameError;
    const QByteArray frame = Rs485FloorFrameBuilder::buildFloors(floors, &frameError);
    if (frame.isEmpty()) {
        return denied(frameError, personId, credential, credentialType);
    }

    NetworkAccessResult result;
    result.pass = true;
    result.personId = personId;
    result.credential = credential;
    result.credentialType = credentialType;
    result.floors = floors;
    result.rs485Frame = frame;
    return result;
}

bool NetworkAccessService::recordSuccessfulAccess(const NetworkAccessResult &result)
{
    if (!result.pass || result.personId.isEmpty()) return false;
    if (!DbStore::transactionBegin()) return false;

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QList<QVariantMap> rules = DbStore::query(
                QStringLiteral("SELECT count_enabled,amount_enabled,count_total,"
                               "amount_total,amount_unit_price "
                               "FROM network_access_rule WHERE person_id=?"),
                {result.personId});
    if (!rules.isEmpty()) {
        const QVariantMap rule = rules.first();
        const bool countEnabled = rule.value(QStringLiteral("count_enabled")).toInt() == 1;
        const bool amountEnabled = rule.value(QStringLiteral("amount_enabled")).toInt() == 1;
        if (countEnabled || amountEnabled) {
            QList<QVariantMap> usages = DbStore::query(
                        QStringLiteral("SELECT remaining_count,access_count,"
                                       "remaining_amount,used_amount "
                                       "FROM network_access_usage WHERE person_id=? LIMIT 1"),
                        {result.personId});
            if (usages.isEmpty()) {
                const QVariant initialCount = countEnabled
                        ? rule.value(QStringLiteral("count_total")) : QVariant();
                const QVariant initialAmount = amountEnabled
                        ? rule.value(QStringLiteral("amount_total")) : QVariant();
                if (!DbStore::execute(
                            QStringLiteral("INSERT INTO network_access_usage("
                                           "person_id,remaining_count,access_count,"
                                           "remaining_amount,used_amount,updated_at) "
                                           "VALUES(?,?,0,?,0,?)"),
                            {result.personId, initialCount, initialAmount, now})) {
                    DbStore::transactionRollback();
                    return false;
                }
                QVariantMap initialUsage;
                initialUsage.insert(QStringLiteral("remaining_count"), initialCount);
                initialUsage.insert(QStringLiteral("access_count"), 0);
                initialUsage.insert(QStringLiteral("remaining_amount"), initialAmount);
                initialUsage.insert(QStringLiteral("used_amount"), 0.0);
                usages.append(initialUsage);
            }

            const QVariantMap usage = usages.first();
            QStringList assignments;
            QList<QVariant> binds;
            if (countEnabled) {
                const qlonglong remainingCount = usage
                        .value(QStringLiteral("remaining_count")).toLongLong();
                if (remainingCount <= 0) {
                    DbStore::transactionRollback();
                    return false;
                }
                assignments.append(QStringLiteral("remaining_count=?"));
                binds.append(remainingCount - 1);
                assignments.append(QStringLiteral("access_count=?"));
                binds.append(usage.value(QStringLiteral("access_count")).toLongLong() + 1);
            }
            if (amountEnabled) {
                bool unitPriceOk = false;
                bool remainingAmountOk = false;
                const double unitPrice = rule.value(QStringLiteral("amount_unit_price"))
                        .toDouble(&unitPriceOk);
                const double remainingAmount = usage
                        .value(QStringLiteral("remaining_amount"))
                        .toDouble(&remainingAmountOk);
                if (!unitPriceOk || !remainingAmountOk
                        || !std::isfinite(unitPrice) || !std::isfinite(remainingAmount)
                        || unitPrice <= 0.0 || remainingAmount <= 0.0
                        || remainingAmount + kAmountEpsilon < unitPrice) {
                    DbStore::transactionRollback();
                    return false;
                }
                assignments.append(QStringLiteral("remaining_amount=?"));
                binds.append(normalizedAmount(std::max(0.0, remainingAmount - unitPrice)));
                assignments.append(QStringLiteral("used_amount=?"));
                binds.append(normalizedAmount(
                                 usage.value(QStringLiteral("used_amount")).toDouble()
                                 + unitPrice));
            }
            assignments.append(QStringLiteral("updated_at=?"));
            binds.append(now);
            binds.append(result.personId);
            if (!DbStore::execute(
                        QStringLiteral("UPDATE network_access_usage SET %1 WHERE person_id=?")
                        .arg(assignments.join(QLatin1Char(','))), binds)) {
                DbStore::transactionRollback();
                return false;
            }
        }
    }

    if (result.credentialType == QStringLiteral("QR_CODE")) {
        if (!DbStore::execute(
                    QStringLiteral("UPDATE network_qr_code SET used_count=used_count+1 "
                                   "WHERE person_id=? AND qr_unique=?"),
                    {result.personId, result.credential})) {
            DbStore::transactionRollback();
            return false;
        }
    }
    if (!DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        return false;
    }
    return true;
}
