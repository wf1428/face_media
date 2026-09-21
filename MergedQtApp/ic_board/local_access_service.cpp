#include "local_access_service.h"

#include "common/sql/dbstore.h"
#include "ic_board/rs485_floor_frame_builder.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QTime>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace {

NetworkAccessResult denied(const QString &reason, const QString &personId = QString(),
                           const QString &credential = QString(),
                           const QString &credentialType = QString(), int code = 0)
{
    NetworkAccessResult result;
    result.reason = reason;
    result.personId = personId;
    result.credential = credential;
    result.credentialType = credentialType;
    result.code = code;
    return result;
}

QDateTime parseDateTime(const QString &text)
{
    QDateTime value = QDateTime::fromString(text.trimmed(), Qt::ISODateWithMs);
    if (!value.isValid()) value = QDateTime::fromString(text.trimmed(), Qt::ISODate);
    if (!value.isValid()) value = QDateTime::fromString(text.trimmed(), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    return value;
}

QStringList cardCandidates(const QString &input)
{
    const QString raw = input.trimmed().toUpper();
    QStringList values;
    if (raw.isEmpty()) return values;
    values.append(raw);
    bool ok = false;
    const qulonglong hexValue = raw.toULongLong(&ok, 16);
    if (ok && hexValue <= 0xffffffffULL) {
        const QString decimal = QString::number(hexValue);
        if (!values.contains(decimal)) values.append(decimal);
        if (raw.size() == 8) {
            QByteArray bytes = QByteArray::fromHex(raw.toLatin1());
            std::reverse(bytes.begin(), bytes.end());
            const QString reverseHex = QString::fromLatin1(bytes.toHex()).toUpper();
            if (!values.contains(reverseHex)) values.append(reverseHex);
            const QString reverseDecimal = QString::number(reverseHex.toULongLong(&ok, 16));
            if (ok && !values.contains(reverseDecimal)) values.append(reverseDecimal);
        }
    }
    const qulonglong decimalValue = raw.toULongLong(&ok, 10);
    if (ok && decimalValue <= 0xffffffffULL) {
        const QString hex = QStringLiteral("%1").arg(decimalValue, 8, 16, QLatin1Char('0')).toUpper();
        if (!values.contains(hex)) values.append(hex);
        QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
        if (bytes.size() == 4) {
            std::reverse(bytes.begin(), bytes.end());
            const QString reverseHex = QString::fromLatin1(bytes.toHex()).toUpper();
            if (!values.contains(reverseHex)) values.append(reverseHex);
            const QString reverseDecimal = QString::number(reverseHex.toULongLong(&ok, 16));
            if (ok && !values.contains(reverseDecimal)) values.append(reverseDecimal);
        }
    }
    return values;
}

bool timeMatches(const QVariantMap &row, const QDateTime &now)
{
    const QJsonDocument document = QJsonDocument::fromJson(
                row.value(QStringLiteral("days_json")).toString().toUtf8());
    if (!document.isArray()) return false;
    bool dayOk = false;
    for (const QJsonValue &day : document.array()) {
        if (day.toInt(-1) == now.date().dayOfWeek()) { dayOk = true; break; }
    }
    if (!dayOk) return false;
    const QTime start = QTime::fromString(row.value(QStringLiteral("start_time")).toString(), QStringLiteral("HH:mm"));
    const QTime end = QTime::fromString(row.value(QStringLiteral("end_time")).toString(), QStringLiteral("HH:mm"));
    if (!start.isValid() || !end.isValid()) return false;
    return start <= end ? now.time() >= start && now.time() <= end
                        : now.time() >= start || now.time() <= end;
}

} // namespace

bool LocalAccessService::initialize()
{
    return personnelStore_.initialize();
}

NetworkAccessResult LocalAccessService::checkCardFrame(const QByteArray &frame, const QDateTime &now)
{
    if (frame.size() < 43) return denied(QStringLiteral("刷卡数据长度不足"));
    const QString readerCard = QString::fromLatin1(frame.mid(35, 8)).trimmed().toUpper();
    return checkCardId(readerCard, now);
}

bool LocalAccessService::hasCardId(const QString &cardId)
{
    return !cardHolders(cardId).isEmpty();
}

QList<CardHolderInfo> LocalAccessService::cardHolders(const QString &cardId)
{
    QList<CardHolderInfo> holders;
    if (!initialize()) return holders;
    const QStringList candidates = cardCandidates(cardId);
    if (candidates.isEmpty()) return holders;

    QStringList marks;
    QList<QVariant> binds;
    for (const QString &candidate : candidates) {
        marks.append(QStringLiteral("?"));
        binds.append(candidate);
    }
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT DISTINCT c.person_id,p.name FROM local_ic_card c "
                               "JOIN person p ON p.id=c.person_id "
                               "WHERE UPPER(c.card_id) IN (%1) ORDER BY c.person_id")
                    .arg(marks.join(QLatin1Char(','))), binds);
    for (const QVariantMap &row : rows) {
        CardHolderInfo holder;
        holder.personId = row.value(QStringLiteral("person_id")).toString();
        holder.name = row.value(QStringLiteral("name")).toString().trimmed();
        if (holder.name.isEmpty()) holder.name = holder.personId;
        holders.append(holder);
    }
    return holders;
}

NetworkAccessResult LocalAccessService::checkCardId(const QString &cardId, const QDateTime &now)
{
    if (!initialize()) return denied(QStringLiteral("本地权限数据库初始化失败"));
    const QString readerCard = cardId.trimmed().toUpper();
    const QStringList candidates = cardCandidates(readerCard);
    QStringList marks;
    QList<QVariant> binds;
    for (const QString &candidate : candidates) { marks.append(QStringLiteral("?")); binds.append(candidate); }
    if (marks.isEmpty()) return denied(QStringLiteral("未解析到卡号"));
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT c.person_id,c.card_id FROM local_ic_card c JOIN person p ON p.id=c.person_id "
                               "WHERE UPPER(c.card_id) IN (%1) AND c.active=1 AND p.enabled=1 AND p.deleted=0 LIMIT 1")
                .arg(marks.join(QLatin1Char(','))), binds);
    if (rows.isEmpty()) return denied(QStringLiteral("卡号未登记"), QString(), readerCard, QStringLiteral("IC_CARD"));
    return evaluatePerson(rows.first().value(QStringLiteral("person_id")).toLongLong(),
                          rows.first().value(QStringLiteral("card_id")).toString(), QStringLiteral("IC_CARD"), now);
}

NetworkAccessResult LocalAccessService::checkQrCode(const QString &qrCode, const QDateTime &now)
{
    if (!initialize()) return denied(QStringLiteral("本地权限数据库初始化失败"));
    const QString value = qrCode.trimmed();
    if (value.isEmpty()) return denied(QStringLiteral("二维码为空"));
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT q.person_id,q.valid_from,q.valid_to,q.max_use_count,q.used_count "
                               "FROM local_qr_code q JOIN person p ON p.id=q.person_id "
                               "WHERE q.qr_unique=? AND q.active=1 AND p.enabled=1 AND p.deleted=0 LIMIT 1"), {value});
    if (rows.isEmpty()) return denied(QStringLiteral("二维码未登记"), QString(), value, QStringLiteral("QR_CODE"));
    const QVariantMap row = rows.first();
    const QString id = row.value(QStringLiteral("person_id")).toString();
    const QDateTime from = parseDateTime(row.value(QStringLiteral("valid_from")).toString());
    const QDateTime to = parseDateTime(row.value(QStringLiteral("valid_to")).toString());
    if (from.isValid() && now < from) return denied(QStringLiteral("二维码尚未生效"), id, value, QStringLiteral("QR_CODE"));
    if (to.isValid() && now > to) return denied(QStringLiteral("二维码已过期"), id, value, QStringLiteral("QR_CODE"));
    if (!row.value(QStringLiteral("max_use_count")).isNull()
            && row.value(QStringLiteral("max_use_count")).toInt() > 0
            && row.value(QStringLiteral("used_count")).toInt() >= row.value(QStringLiteral("max_use_count")).toInt()) {
        return denied(QStringLiteral("二维码使用次数已用完"), id, value, QStringLiteral("QR_CODE"));
    }
    return evaluatePerson(id.toLongLong(), value, QStringLiteral("QR_CODE"), now);
}

NetworkAccessResult LocalAccessService::checkPassword(const QString &password, const QDateTime &now)
{
    if (!initialize()) return denied(QStringLiteral("本地权限数据库初始化失败"));
    const QString value = password.trimmed();
    if (value.isEmpty()) {
        return denied(QStringLiteral("通行密码不能为空"), QString(), QString(),
                      QStringLiteral("PASSWORD"));
    }
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT r.person_id FROM local_access_rule r JOIN person p ON p.id=r.person_id "
                               "WHERE r.card_password_enabled=1 AND r.card_password=? AND p.enabled=1 AND p.deleted=0 LIMIT 2"),
                {value});
    if (rows.isEmpty()) return denied(QStringLiteral("通行密码错误"), QString(), QString(), QStringLiteral("PASSWORD"));
    if (rows.size() > 1) return denied(QStringLiteral("通行密码对应多人"), QString(), QString(), QStringLiteral("PASSWORD"));
    return evaluatePerson(rows.first().value(QStringLiteral("person_id")).toLongLong(),
                          QString(), QStringLiteral("PASSWORD"), now);
}

NetworkAccessResult LocalAccessService::checkFacePerson(qint64 personId, const QDateTime &now)
{
    if (!initialize()) return denied(QStringLiteral("本地权限数据库初始化失败"));
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT id FROM person WHERE id=? AND enabled=1 AND deleted=0 LIMIT 1"), {personId});
    if (rows.isEmpty()) return denied(QStringLiteral("本地人脸人员无效"), QString::number(personId),
                                      QString::number(personId), QStringLiteral("FACE"));
    return evaluatePerson(personId, QString::number(personId), QStringLiteral("FACE"), now);
}

NetworkAccessResult LocalAccessService::evaluatePerson(qint64 personId, const QString &credential,
                                                        const QString &credentialType, const QDateTime &now)
{
    const QList<QVariantMap> people = DbStore::query(
                QStringLiteral("SELECT person_no FROM person WHERE id=? LIMIT 1"), {personId});
    const QString id = people.isEmpty() ? QString::number(personId)
                                        : people.first().value(QStringLiteral("person_no")).toString();
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT * FROM local_access_rule WHERE person_id=? LIMIT 1"), {personId});
    if (rows.isEmpty()) return denied(QStringLiteral("人员权限规则不存在"), id, credential, credentialType);
    const QVariantMap rule = rows.first();
    if (rule.value(QStringLiteral("control_elevator")).toInt() != 1)
        return denied(QStringLiteral("人员未启用控梯权限"), id, credential, credentialType);
    if (rule.value(QStringLiteral("duration_enabled")).toInt() == 1) {
        const QString startText = rule.value(QStringLiteral("duration_start")).toString().trimmed();
        const QString endText = rule.value(QStringLiteral("duration_end")).toString().trimmed();
        // 开关开启但期限为空或格式不完整，表示尚未配置，不参与限制。
        if (!startText.isEmpty() && !endText.isEmpty()) {
            const QDateTime start = parseDateTime(startText);
            const QDateTime end = parseDateTime(endText);
            if (start.isValid() && end.isValid() && start <= end
                    && (now < start || now > end)) {
                return denied(QStringLiteral("人员通行期限不允许"), id, credential, credentialType);
            }
        }
    }
    if (rule.value(QStringLiteral("time_enabled")).toInt() == 1) {
        const QList<QVariantMap> timing = DbStore::query(
                    QStringLiteral("SELECT days_json,start_time,end_time FROM local_timing_rule WHERE person_id=? ORDER BY rule_order"),
                    {personId});
        bool matched = timing.isEmpty();
        for (const QVariantMap &item : timing) if (timeMatches(item, now)) { matched = true; break; }
        if (!matched) return denied(QStringLiteral("当前时间不在允许时段"), id, credential, credentialType);
    }
    const QList<QVariantMap> usageRows = DbStore::query(
                QStringLiteral("SELECT remaining_count,remaining_amount FROM local_access_usage WHERE person_id=? LIMIT 1"),
                {personId});
    const QVariantMap usage = usageRows.value(0);
    const QVariant remainingCount = usage.value(QStringLiteral("remaining_count"));
    if (rule.value(QStringLiteral("count_enabled")).toInt() == 1
            && remainingCount.isValid() && !remainingCount.isNull()
            && remainingCount.toLongLong() <= 0) {
        return denied(QStringLiteral("通行次数已用完"), id, credential, credentialType, 4200);
    }
    if (rule.value(QStringLiteral("amount_enabled")).toInt() == 1) {
        const QVariant priceValue = rule.value(QStringLiteral("amount_unit_price"));
        const QVariant remainingValue = usage.value(QStringLiteral("remaining_amount"));
        if (priceValue.isValid() && !priceValue.isNull()
                && remainingValue.isValid() && !remainingValue.isNull()) {
            const double price = priceValue.toDouble();
            const double remaining = remainingValue.toDouble();
            if (std::isfinite(price) && price > 0.0
                    && remaining + 0.000001 < price) {
                return denied(QStringLiteral("通行余额不足"), id, credential, credentialType, 4201);
            }
        }
    }
    // 本地导入表中的设备字段是人员楼层分组名称，不是 MQTT client_id。
    // 本地通行只按人员汇总全部楼层；网络人员仍在 NetworkAccessService 中按设备ID匹配。
    const QList<QVariantMap> floorRows = DbStore::query(
                QStringLiteral("SELECT DISTINCT floor_no FROM local_floor "
                               "WHERE person_id=? ORDER BY floor_no"),
                {personId});
    if (floorRows.isEmpty()) return denied(QStringLiteral("人员没有可用楼层"), id, credential, credentialType);
    QList<int> floors;
    for (const QVariantMap &floor : floorRows) floors.append(floor.value(QStringLiteral("floor_no")).toInt());
    std::sort(floors.begin(), floors.end());
    if (Rs485FloorFrameBuilder::shouldRejectModeOccupiedFloors(floors))
        return denied(QStringLiteral("楼层已被电梯模式占用"), id, credential, credentialType);
    QString error;
    const QByteArray frame = Rs485FloorFrameBuilder::buildFloors(floors, &error);
    if (frame.isEmpty()) return denied(error, id, credential, credentialType);
    NetworkAccessResult result;
    result.pass = true; result.personId = id; result.credential = credential;
    result.localPersonId = personId;
    result.credentialType = credentialType; result.floors = floors; result.rs485Frame = frame;
    return result;
}

bool LocalAccessService::recordSuccessfulAccess(const NetworkAccessResult &result)
{
    if (!result.pass || result.personId.isEmpty() || !DbStore::transactionBegin()) return false;
    const qint64 personId = result.localPersonId;
    if (personId <= 0) { DbStore::transactionRollback(); return false; }
    const QList<QVariantMap> rules = DbStore::query(
                QStringLiteral("SELECT r.count_enabled,r.amount_enabled,r.amount_unit_price,"
                               "u.remaining_count,u.remaining_amount FROM local_access_rule r "
                               "LEFT JOIN local_access_usage u ON u.person_id=r.person_id WHERE r.person_id=?"),
                {personId});
    bool ok = true;
    if (!rules.isEmpty()) {
        const QVariantMap rule = rules.first();
        QStringList assignments;
        QList<QVariant> binds;
        const QVariant remainingCount = rule.value(QStringLiteral("remaining_count"));
        if (rule.value(QStringLiteral("count_enabled")).toInt() == 1
                && remainingCount.isValid() && !remainingCount.isNull()) {
            assignments.append(QStringLiteral("remaining_count=remaining_count-1,access_count=access_count+1"));
        }
        if (rule.value(QStringLiteral("amount_enabled")).toInt() == 1) {
            const QVariant unitPriceValue = rule.value(QStringLiteral("amount_unit_price"));
            const QVariant remainingAmount = rule.value(QStringLiteral("remaining_amount"));
            const double unitPrice = unitPriceValue.toDouble();
            if (unitPriceValue.isValid() && !unitPriceValue.isNull()
                    && remainingAmount.isValid() && !remainingAmount.isNull()
                    && std::isfinite(unitPrice) && unitPrice > 0.0) {
                assignments.append(QStringLiteral("remaining_amount=remaining_amount-?,used_amount=used_amount+?"));
                binds.append(unitPrice);
                binds.append(unitPrice);
            }
        }
        if (!assignments.isEmpty()) {
            assignments.append(QStringLiteral("updated_at=?"));
            binds.append(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            binds.append(personId);
            ok = DbStore::execute(QStringLiteral("UPDATE local_access_usage SET %1 WHERE person_id=?")
                                  .arg(assignments.join(QLatin1Char(','))),
                                  binds);
        }
    }
    if (ok && result.credentialType == QStringLiteral("QR_CODE"))
        ok = DbStore::execute(QStringLiteral("UPDATE local_qr_code SET used_count=used_count+1 WHERE person_id=? AND qr_unique=?"),
                              {personId, result.credential});
    if (ok) ok = DbStore::transactionCommit();
    if (!ok) DbStore::transactionRollback();
    return ok;
}
