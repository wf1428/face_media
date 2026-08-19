/**
 * @file network_personnel_store.cpp
 * @brief MQTT 网络人员聚合数据的 SQLite 持久化与哈希幂等处理实现。
 */

#include "network_personnel_store.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPair>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <cmath>

#include "common/sql/dbstore.h"
#include "platform/rk3566_platform.h"

namespace {

QString compactJson(const QJsonObject &value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QString compactJson(const QJsonArray &value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QString compactJson(const QJsonValue &value)
{
    if (value.isArray()) {
        return compactJson(value.toArray());
    }
    if (value.isObject()) {
        return compactJson(value.toObject());
    }
    return {};
}

QJsonArray floorNumbers(const QJsonValue &value)
{
    if (value.isArray()) {
        return value.toArray();
    }
    if (!value.isString()) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8(), &error);
    return error.error == QJsonParseError::NoError && doc.isArray()
            ? doc.array()
            : QJsonArray();
}

QString requestId(const QJsonObject &envelope)
{
    return envelope.value(QStringLiteral("id")).toString().trimmed();
}

QString requestDeviceId(const QJsonObject &envelope)
{
    return envelope.value(QStringLiteral("deviceId")).toString().trimmed();
}

QString requestMethod(const QJsonObject &envelope)
{
    return envelope.value(QStringLiteral("method")).toString().trimmed();
}

QStringList faceDeleteKeys(const QString &item)
{
    QStringList keys;
    const QString value = item.trimmed();
    if (value.isEmpty()) {
        return keys;
    }

    // 兼容服务端直接下发 faceHash。
    keys.append(value);

    // 服务端也可能在 FACE 删除消息的 items 中下发 data URL。
    // network_face 保存的是图片原始字节的 SHA-256，因此解码后计算 faceHash。
    if (value.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
        const int comma = value.indexOf(QLatin1Char(','));
        if (comma > 0
                && value.left(comma).contains(QStringLiteral(";base64"),
                                              Qt::CaseInsensitive)) {
            const QByteArray imageBytes =
                    QByteArray::fromBase64(value.mid(comma + 1).toLatin1());
            if (!imageBytes.isEmpty()) {
                const QString faceHash = QString::fromLatin1(
                            QCryptographicHash::hash(
                                imageBytes, QCryptographicHash::Sha256).toHex());
                if (!keys.contains(faceHash)) {
                    keys.append(faceHash);
                }
            }
        }
    }
    return keys;
}

} // namespace

bool NetworkPersonnelStore::initialize()
{
    if (initialized_) {
        return true;
    }
    if (!DbStore::bootstrap(Rk3566Platform::databasePath())) {
        return false;
    }

    const QStringList schema = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_person ("
            "person_id TEXT PRIMARY KEY,"
            "person_hash TEXT NOT NULL DEFAULT '',"
            "name TEXT,"
            "phone TEXT,"
            "id_card TEXT,"
            "employee_no TEXT,"
            "department TEXT,"
            "position TEXT,"
            "hire_date_ms INTEGER,"
            "person_type TEXT,"
            "status INTEGER NOT NULL DEFAULT 1,"
            "remark TEXT,"
            "active INTEGER NOT NULL DEFAULT 1,"
            "deleted INTEGER NOT NULL DEFAULT 0,"
            "sync_state TEXT NOT NULL DEFAULT 'READY',"
            "server_time TEXT,"
            "person_json TEXT,"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_face ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "person_id TEXT NOT NULL,"
            "face_hash TEXT NOT NULL,"
            "name TEXT,"
            "bind_time TEXT,"
            "image_path TEXT,"
            "face_base64 TEXT,"
            "feature_blob BLOB,"
            "model_version TEXT,"
            "status TEXT NOT NULL DEFAULT 'METADATA',"
            "UNIQUE(person_id, face_hash),"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_ic_card ("
            "card_id TEXT PRIMARY KEY,"
            "person_id TEXT NOT NULL,"
            "name TEXT,"
            "bind_time TEXT,"
            "active INTEGER NOT NULL DEFAULT 1,"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_qr_code ("
            "qr_unique TEXT PRIMARY KEY,"
            "person_id TEXT NOT NULL,"
            "qr_name TEXT,"
            "qr_type TEXT,"
            "purpose TEXT,"
            "valid_from TEXT,"
            "valid_to TEXT,"
            "max_use_count INTEGER,"
            "used_count INTEGER NOT NULL DEFAULT 0,"
            "visitor_name TEXT,"
            "visitor_phone TEXT,"
            "bind_time TEXT,"
            "raw_json TEXT,"
            "active INTEGER NOT NULL DEFAULT 1,"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_access_rule ("
            "person_id TEXT PRIMARY KEY,"
            "time_enabled INTEGER NOT NULL DEFAULT 0,"
            "count_enabled INTEGER NOT NULL DEFAULT 0,"
            "relay_enabled INTEGER NOT NULL DEFAULT 0,"
            "amount_enabled INTEGER NOT NULL DEFAULT 0,"
            "duration_enabled INTEGER NOT NULL DEFAULT 0,"
            "remote_call_enabled INTEGER NOT NULL DEFAULT 0,"
            "remote_call_current_day_count INTEGER,"
            "remote_call_every_day_count INTEGER,"
            "remote_call_used_count INTEGER NOT NULL DEFAULT 0,"
            "card_password_enabled INTEGER NOT NULL DEFAULT 0,"
            "card_password TEXT,"
            "control_elevator INTEGER NOT NULL DEFAULT 0,"
            "count_total INTEGER,"
            "count_add_count INTEGER,"
            "amount_total REAL,"
            "amount_add_amount REAL,"
            "amount_unit_price REAL,"
            "duration_start TEXT,"
            "duration_end TEXT,"
            "elevator_value_type INTEGER,"
            "raw_json TEXT,"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_timing_rule ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "person_id TEXT NOT NULL,"
            "rule_order INTEGER NOT NULL,"
            "days_json TEXT,"
            "start_time TEXT,"
            "end_time TEXT,"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_floor ("
            "person_id TEXT NOT NULL,"
            "device_id TEXT NOT NULL,"
            "floor_no INTEGER NOT NULL,"
            "PRIMARY KEY(person_id, device_id, floor_no),"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_person_tombstone ("
            "person_id TEXT PRIMARY KEY,"
            "delete_type TEXT NOT NULL,"
            "server_time TEXT,"
            "message_id TEXT,"
            "deleted_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_access_usage ("
            "person_id TEXT PRIMARY KEY,"
            "remaining_count INTEGER,"
            "access_count INTEGER NOT NULL DEFAULT 0,"
            "updated_at TEXT NOT NULL,"
            "FOREIGN KEY(person_id) REFERENCES network_person(person_id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_access_deduct_result ("
            "result_id TEXT PRIMARY KEY,"
            "method TEXT NOT NULL,"
            "person_id TEXT NOT NULL,"
            "code INTEGER NOT NULL,"
            "deducted INTEGER NOT NULL,"
            "message TEXT,"
            "remaining_count INTEGER,"
            "used_count INTEGER,"
            "processed_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_remote_call_transaction ("
            "request_id TEXT PRIMARY KEY,"
            "person_id TEXT NOT NULL,"
            "floor INTEGER NOT NULL,"
            "source TEXT NOT NULL DEFAULT 'platform',"
            "rs485_success INTEGER,"
            "rs485_reason TEXT,"
            "access_result_id TEXT,"
            "access_result_success INTEGER,"
            "access_result_sent INTEGER NOT NULL DEFAULT 0,"
            "local_deducted INTEGER NOT NULL DEFAULT 0,"
            "reserved_remaining_count INTEGER,"
            "deduct_result_id TEXT,"
            "deduct_code INTEGER,"
            "deducted INTEGER,"
            "deduct_message TEXT,"
            "state TEXT NOT NULL,"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_qr_access_cache ("
            "qr_unique TEXT PRIMARY KEY,"
            "person_id TEXT,"
            "source_type TEXT NOT NULL DEFAULT '',"
            "invitation_id TEXT,"
            "floor_hex TEXT NOT NULL,"
            "floors_json TEXT NOT NULL,"
            "deduct_count INTEGER NOT NULL DEFAULT 0,"
            "last_deduct_at TEXT,"
            "updated_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_qr_transaction ("
            "scan_id TEXT PRIMARY KEY,"
            "device_id TEXT NOT NULL,"
            "qr_unique TEXT NOT NULL,"
            "person_id TEXT,"
            "source_type TEXT,"
            "invitation_id TEXT,"
            "scan_result_id TEXT,"
            "scan_code INTEGER,"
            "scan_success INTEGER,"
            "scan_reason TEXT,"
            "scan_data_json TEXT,"
            "floor_control_id TEXT,"
            "floor_hex TEXT,"
            "floors_json TEXT,"
            "rs485_success INTEGER,"
            "rs485_reason TEXT,"
            "access_result_id TEXT,"
            "access_result_success INTEGER,"
            "access_result_sent INTEGER NOT NULL DEFAULT 0,"
            "deduct_result_id TEXT,"
            "deduct_code INTEGER,"
            "deducted INTEGER,"
            "deduct_message TEXT,"
            "state TEXT NOT NULL,"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS network_qr_visitor_usage ("
            "qr_unique TEXT PRIMARY KEY,"
            "remaining_count REAL,"
            "used_count REAL NOT NULL DEFAULT 0,"
            "last_result_id TEXT,"
            "updated_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sync_inbox ("
            "device_id TEXT NOT NULL,"
            "message_id TEXT NOT NULL,"
            "method TEXT NOT NULL,"
            "person_id TEXT,"
            "payload_hash TEXT NOT NULL,"
            "result_json TEXT NOT NULL,"
            "processed_at TEXT NOT NULL,"
            "PRIMARY KEY(device_id, message_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_face_person ON network_face(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_ic_card_person ON network_ic_card(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_qr_person ON network_qr_code(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_floor_person ON network_floor(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_qr_cache_person ON network_qr_access_cache(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_qr_tx_qr_state ON network_qr_transaction(qr_unique,state)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_network_qr_tx_floor_msg ON network_qr_transaction(floor_control_id) WHERE floor_control_id IS NOT NULL AND floor_control_id<>''"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_network_qr_tx_deduct_msg ON network_qr_transaction(deduct_result_id) WHERE deduct_result_id IS NOT NULL AND deduct_result_id<>''"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_network_remote_call_pending ON network_remote_call_transaction(person_id,floor,state)"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_network_remote_call_deduct_msg ON network_remote_call_transaction(deduct_result_id) WHERE deduct_result_id IS NOT NULL AND deduct_result_id<>''")
    };

    for (const QString &sql : schema) {
        if (!DbStore::exec(sql)) {
            return false;
        }
    }

    QSet<QString> faceColumnNames;
    const QList<QVariantMap> faceColumns =
            DbStore::query(QStringLiteral("PRAGMA table_info(network_face)"));
    for (const QVariantMap &column : faceColumns) {
        faceColumnNames.insert(column.value(QStringLiteral("name"))
                               .toString().trimmed().toLower());
    }

    const QList<QPair<QString, QString>> requiredFaceColumns = {
        {QStringLiteral("face_base64"), QStringLiteral("TEXT")},
        {QStringLiteral("feature_blob"), QStringLiteral("BLOB")},
        {QStringLiteral("model_version"), QStringLiteral("TEXT")}
    };
    for (const auto &column : requiredFaceColumns) {
        if (!faceColumnNames.contains(column.first)
                && !DbStore::exec(QStringLiteral(
                                      "ALTER TABLE network_face ADD COLUMN %1 %2")
                                  .arg(column.first, column.second))) {
            return false;
        }
    }

    QSet<QString> accessRuleColumnNames;
    const QList<QVariantMap> accessRuleColumns =
            DbStore::query(QStringLiteral("PRAGMA table_info(network_access_rule)"));
    for (const QVariantMap &column : accessRuleColumns) {
        accessRuleColumnNames.insert(column.value(QStringLiteral("name"))
                                     .toString().trimmed().toLower());
    }
    const QList<QPair<QString, QString>> requiredAccessRuleColumns = {
        {QStringLiteral("remote_call_current_day_count"), QStringLiteral("INTEGER")},
        {QStringLiteral("remote_call_every_day_count"), QStringLiteral("INTEGER")},
        {QStringLiteral("remote_call_used_count"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("card_password"), QStringLiteral("TEXT")}
    };
    for (const auto &column : requiredAccessRuleColumns) {
        if (!accessRuleColumnNames.contains(column.first)
                && !DbStore::exec(QStringLiteral(
                                      "ALTER TABLE network_access_rule ADD COLUMN %1 %2")
                                  .arg(column.first, column.second))) {
            return false;
        }
    }

    initialized_ = true;
    return true;
}

QString NetworkPersonnelStore::payloadHash(const QJsonObject &envelope) const
{
    const QByteArray bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool NetworkPersonnelStore::loadCachedResult(const QJsonObject &envelope,
                                             const QString &hash,
                                             NetworkPersonnelSyncResult *result) const
{
    if (!result) {
        return false;
    }
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT payload_hash, result_json FROM sync_inbox "
                               "WHERE device_id=? AND message_id=? LIMIT 1"),
                {requestDeviceId(envelope), requestId(envelope)});
    if (rows.isEmpty()) {
        return false;
    }

    if (rows.first().value(QStringLiteral("payload_hash")).toString() != hash) {
        *result = makeResult(false,
                             QStringLiteral("MESSAGE_ID_CONFLICT"),
                             QStringLiteral("相同消息 ID 对应了不同的消息内容"));
        result->duplicate = true;
        return true;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(
                rows.first().value(QStringLiteral("result_json")).toByteArray(), &error);
    result->ok = error.error == QJsonParseError::NoError && doc.isObject()
            && doc.object().value(QStringLiteral("status")).toString() == QStringLiteral("SUCCESS");
    result->data = doc.isObject() ? doc.object() : QJsonObject();
    result->data.insert(QStringLiteral("duplicate"), true);
    result->duplicate = true;
    return true;
}

bool NetworkPersonnelStore::recordResult(const QJsonObject &envelope,
                                         const QString &personId,
                                         const QString &hash,
                                         const NetworkPersonnelSyncResult &result) const
{
    return DbStore::execute(
                QStringLiteral("INSERT INTO sync_inbox("
                               "device_id,message_id,method,person_id,payload_hash,result_json,processed_at"
                               ") VALUES(?,?,?,?,?,?,?)"),
                {requestDeviceId(envelope),
                 requestId(envelope),
                 requestMethod(envelope),
                 personId,
                 hash,
                 compactJson(result.data),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))});
}

NetworkPersonnelSyncResult NetworkPersonnelStore::makeResult(
        bool ok,
        const QString &code,
        const QString &message,
        const QString &personId) const
{
    NetworkPersonnelSyncResult result;
    result.ok = ok;
    result.data.insert(QStringLiteral("status"), ok ? QStringLiteral("SUCCESS")
                                                    : QStringLiteral("ERROR"));
    result.data.insert(QStringLiteral("code"), code);
    result.data.insert(QStringLiteral("message"), message);
    if (!personId.isEmpty()) {
        result.data.insert(QStringLiteral("personId"), personId);
    }
    return result;
}

QJsonArray NetworkPersonnelStore::networkPersonnelList() const
{
    QJsonArray people;
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral(
                    "SELECT p.person_id,p.employee_no,p.name,p.status,p.active,"
                    "p.deleted,p.person_hash,p.updated_at,"
                    "COUNT(f.id) AS face_count,"
                    "COALESCE(SUM(CASE WHEN COALESCE(f.image_path,'')<>'' "
                    "AND COALESCE(f.face_base64,'')<>'' THEN 1 ELSE 0 END),0) "
                    "AS image_count "
                    "FROM network_person p "
                    "LEFT JOIN network_face f ON f.person_id=p.person_id "
                    "GROUP BY p.person_id,p.employee_no,p.name,p.status,p.active,"
                    "p.deleted,p.person_hash,p.updated_at "
                    "ORDER BY p.updated_at DESC,p.person_id"));
    for (const QVariantMap &row : rows) {
        QJsonObject person;
        person.insert(QStringLiteral("personId"),
                      row.value(QStringLiteral("person_id")).toString());
        person.insert(QStringLiteral("employeeNo"),
                      row.value(QStringLiteral("employee_no")).toString());
        person.insert(QStringLiteral("name"),
                      row.value(QStringLiteral("name")).toString());
        person.insert(QStringLiteral("status"),
                      row.value(QStringLiteral("status")).toInt());
        person.insert(QStringLiteral("active"),
                      row.value(QStringLiteral("active")).toInt() != 0);
        person.insert(QStringLiteral("deleted"),
                      row.value(QStringLiteral("deleted")).toInt() != 0);
        person.insert(QStringLiteral("personHash"),
                      row.value(QStringLiteral("person_hash")).toString());
        person.insert(QStringLiteral("updatedAt"),
                      row.value(QStringLiteral("updated_at")).toString());
        person.insert(QStringLiteral("faceCount"),
                      row.value(QStringLiteral("face_count")).toInt());
        person.insert(QStringLiteral("imageCount"),
                      row.value(QStringLiteral("image_count")).toInt());
        QJsonArray faces;
        const QList<QVariantMap> faceRows = DbStore::query(
                    QStringLiteral(
                        "SELECT face_hash,image_path,status "
                        "FROM network_face WHERE person_id=? ORDER BY id"),
                    {person.value(QStringLiteral("personId")).toString()});
        for (const QVariantMap &faceRow : faceRows) {
            QJsonObject face;
            face.insert(QStringLiteral("faceHash"),
                        faceRow.value(QStringLiteral("face_hash")).toString());
            face.insert(QStringLiteral("imagePath"),
                        faceRow.value(QStringLiteral("image_path")).toString());
            face.insert(QStringLiteral("status"),
                        faceRow.value(QStringLiteral("status")).toString());
            faces.append(face);
        }
        person.insert(QStringLiteral("faces"), faces);
        people.append(person);
    }
    return people;
}

NetworkPersonnelImportResult NetworkPersonnelStore::importSpreadsheetPersonnel(
        const QVector<QJsonObject> &records)
{
    NetworkPersonnelImportResult summary;
    if (!initialized_ && !initialize()) {
        summary.error = DbStore::lastError();
        return summary;
    }

    auto jsonObject = [](const QString &text) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
        return error.error == QJsonParseError::NoError && document.isObject()
                ? document.object() : QJsonObject();
    };
    auto stringSet = [](const QList<QVariantMap> &rows, const QString &column) {
        QSet<QString> values;
        for (const QVariantMap &row : rows) {
            const QString value = row.value(column).toString().trimmed();
            if (!value.isEmpty()) values.insert(value);
        }
        return values;
    };
    auto jsonStringSet = [](const QJsonArray &items, const QString &key) {
        QSet<QString> values;
        for (const QJsonValue &value : items) {
            const QString item = value.toObject().value(key).toString().trimmed();
            if (!item.isEmpty()) values.insert(item);
        }
        return values;
    };
    auto floorSet = [](const QJsonArray &floors) {
        QSet<QString> values;
        for (const QJsonValue &value : floors) {
            const QJsonObject item = value.toObject();
            const QString deviceId = item.value(QStringLiteral("deviceId")).toString().trimmed();
            const QJsonArray numbers = floorNumbers(item.value(QStringLiteral("floor")));
            for (const QJsonValue &number : numbers) {
                if (number.isDouble()) {
                    values.insert(deviceId + QLatin1Char('\n') + QString::number(number.toInt()));
                }
            }
        }
        return values;
    };

    for (const QJsonObject &record : records) {
        const QJsonObject person = record.value(QStringLiteral("person")).toObject();
        const QString name = person.value(QStringLiteral("name")).toString().trimmed();
        const QString account = person.value(QStringLiteral("account")).toString().trimmed();
        if (name.isEmpty() || account.isEmpty()) {
            ++summary.failed;
            summary.failedNames.append(name.isEmpty() ? QStringLiteral("未命名人员") : name);
            continue;
        }

        QSet<QString> candidateIds;
        const QList<QVariantMap> people = DbStore::query(
                    QStringLiteral("SELECT person_id,phone,id_card,employee_no,person_json "
                                   "FROM network_person"));
        for (const QVariantMap &row : people) {
            const QString personId = row.value(QStringLiteral("person_id")).toString();
            const QJsonObject storedPerson = jsonObject(
                        row.value(QStringLiteral("person_json")).toString());
            const QString storedAccount = storedPerson.value(
                        QStringLiteral("account")).toString().trimmed();
            const bool accountMatch = !storedAccount.isEmpty()
                    && storedAccount.compare(account, Qt::CaseInsensitive) == 0;
            const auto sameNonEmpty = [](const QString &left, const QString &right) {
                return !left.trimmed().isEmpty() && !right.trimmed().isEmpty()
                        && left.trimmed() == right.trimmed();
            };
            if (accountMatch || personId.compare(account, Qt::CaseInsensitive) == 0
                    || sameNonEmpty(row.value(QStringLiteral("phone")).toString(),
                                    person.value(QStringLiteral("phone")).toString())
                    || sameNonEmpty(row.value(QStringLiteral("id_card")).toString(),
                                    person.value(QStringLiteral("idCard")).toString())
                    || sameNonEmpty(row.value(QStringLiteral("employee_no")).toString(),
                                    person.value(QStringLiteral("employeeNo")).toString())) {
                candidateIds.insert(personId);
            }
        }

        const QJsonArray cards = record.value(QStringLiteral("icCards")).toArray();
        for (const QString &cardId : jsonStringSet(cards, QStringLiteral("cardId"))) {
            const QList<QVariantMap> owners = DbStore::query(
                        QStringLiteral("SELECT person_id FROM network_ic_card WHERE card_id=?"),
                        {cardId});
            if (!owners.isEmpty()) candidateIds.insert(
                        owners.first().value(QStringLiteral("person_id")).toString());
        }
        const QJsonArray qrCodes = record.value(QStringLiteral("qrCodes")).toArray();
        for (const QString &qrUnique : jsonStringSet(qrCodes, QStringLiteral("qrUnique"))) {
            const QList<QVariantMap> owners = DbStore::query(
                        QStringLiteral("SELECT person_id FROM network_qr_code WHERE qr_unique=?"),
                        {qrUnique});
            if (!owners.isEmpty()) candidateIds.insert(
                        owners.first().value(QStringLiteral("person_id")).toString());
        }

        if (candidateIds.size() > 1) {
            ++summary.failed;
            summary.failedNames.append(name + QStringLiteral("（多个既有人员匹配）"));
            continue;
        }

        const bool exists = candidateIds.size() == 1;
        const QString personId = exists ? *candidateIds.constBegin()
                                        : QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject rules = record.value(QStringLiteral("rules")).toObject();
        const QJsonArray floors = record.value(QStringLiteral("floors")).toArray();

        bool unchanged = false;
        if (exists) {
            const QList<QVariantMap> current = DbStore::query(
                        QStringLiteral("SELECT name,phone,id_card,employee_no,department,position,"
                                       "hire_date_ms,person_type,status,active,deleted,person_json "
                                       "FROM network_person WHERE person_id=? LIMIT 1"),
                        {personId});
            const QSet<QString> currentCards = stringSet(DbStore::query(
                        QStringLiteral("SELECT card_id FROM network_ic_card WHERE person_id=?"),
                        {personId}), QStringLiteral("card_id"));
            const QSet<QString> currentQrs = stringSet(DbStore::query(
                        QStringLiteral("SELECT qr_unique FROM network_qr_code WHERE person_id=?"),
                        {personId}), QStringLiteral("qr_unique"));
            QSet<QString> currentFloors;
            const QList<QVariantMap> floorRows = DbStore::query(
                        QStringLiteral("SELECT device_id,floor_no FROM network_floor WHERE person_id=?"),
                        {personId});
            for (const QVariantMap &floor : floorRows) {
                currentFloors.insert(floor.value(QStringLiteral("device_id")).toString()
                                     + QLatin1Char('\n')
                                     + QString::number(floor.value(QStringLiteral("floor_no")).toInt()));
            }
            const QList<QVariantMap> ruleRows = DbStore::query(
                        QStringLiteral("SELECT raw_json FROM network_access_rule WHERE person_id=?"),
                        {personId});
            const QString currentRules = ruleRows.isEmpty()
                    ? QString() : compactJson(jsonObject(ruleRows.first().value(
                                                            QStringLiteral("raw_json")).toString()));
            const QVariant hireDate = person.value(QStringLiteral("hireDate")).toVariant();
            const QVariantMap currentPerson = current.value(0);
            unchanged = !current.isEmpty()
                    && currentPerson.value(QStringLiteral("name")).toString() == person.value(QStringLiteral("name")).toString()
                    && currentPerson.value(QStringLiteral("phone")).toString() == person.value(QStringLiteral("phone")).toString()
                    && currentPerson.value(QStringLiteral("id_card")).toString() == person.value(QStringLiteral("idCard")).toString()
                    && currentPerson.value(QStringLiteral("employee_no")).toString() == person.value(QStringLiteral("employeeNo")).toString()
                    && currentPerson.value(QStringLiteral("department")).toString() == person.value(QStringLiteral("department")).toString()
                    && currentPerson.value(QStringLiteral("position")).toString() == person.value(QStringLiteral("position")).toString()
                    && currentPerson.value(QStringLiteral("hire_date_ms")) == hireDate
                    && currentPerson.value(QStringLiteral("person_type")).toString() == person.value(QStringLiteral("personType")).toString()
                    && currentPerson.value(QStringLiteral("status")).toInt() == person.value(QStringLiteral("status")).toInt(1)
                    && currentPerson.value(QStringLiteral("active")).toInt() == 1
                    && currentPerson.value(QStringLiteral("deleted")).toInt() == 0
                    && compactJson(jsonObject(currentPerson.value(QStringLiteral("person_json")).toString())) == compactJson(person)
                    && currentCards == jsonStringSet(cards, QStringLiteral("cardId"))
                    && currentQrs == jsonStringSet(qrCodes, QStringLiteral("qrUnique"))
                    && currentFloors == floorSet(floors)
                    && currentRules == compactJson(rules);
        }
        if (unchanged) {
            ++summary.unchanged;
            summary.unchangedNames.append(name);
            continue;
        }

        if (!DbStore::transactionBegin()) {
            ++summary.failed;
            summary.failedNames.append(name);
            summary.error = DbStore::lastError();
            continue;
        }
        auto rollback = [&]() {
            summary.error = DbStore::lastError();
            DbStore::transactionRollback();
            ++summary.failed;
            summary.failedNames.append(name);
        };
        const QString now = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        bool writeOk = true;
        if (exists) {
            writeOk = DbStore::execute(
                        QStringLiteral("UPDATE network_person SET name=?,phone=?,id_card=?,employee_no=?,"
                                       "department=?,position=?,hire_date_ms=?,person_type=?,status=?,remark=?,"
                                       "active=1,deleted=0,sync_state='READY',person_json=?,updated_at=? "
                                       "WHERE person_id=?"),
                        {person.value(QStringLiteral("name")).toString(),
                         person.value(QStringLiteral("phone")).toString(),
                         person.value(QStringLiteral("idCard")).toString(),
                         person.value(QStringLiteral("employeeNo")).toString(),
                         person.value(QStringLiteral("department")).toString(),
                         person.value(QStringLiteral("position")).toString(),
                         person.value(QStringLiteral("hireDate")).toVariant(),
                         person.value(QStringLiteral("personType")).toString(),
                         person.value(QStringLiteral("status")).toInt(1),
                         person.value(QStringLiteral("remark")).toString(),
                         compactJson(person), now, personId});
        } else {
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO network_person("
                                       "person_id,person_hash,name,phone,id_card,employee_no,department,position,"
                                       "hire_date_ms,person_type,status,remark,active,deleted,sync_state,server_time,"
                                       "person_json,created_at,updated_at) "
                                       "VALUES(?,'',?,?,?,?,?,?,?,?,?,?,1,0,'READY','',?,?,?)"),
                        {personId,
                         person.value(QStringLiteral("name")).toString(),
                         person.value(QStringLiteral("phone")).toString(),
                         person.value(QStringLiteral("idCard")).toString(),
                         person.value(QStringLiteral("employeeNo")).toString(),
                         person.value(QStringLiteral("department")).toString(),
                         person.value(QStringLiteral("position")).toString(),
                         person.value(QStringLiteral("hireDate")).toVariant(),
                         person.value(QStringLiteral("personType")).toString(),
                         person.value(QStringLiteral("status")).toInt(1),
                         person.value(QStringLiteral("remark")).toString(),
                         compactJson(person), now, now});
        }
        if (!writeOk) {
            rollback();
            continue;
        }

        const QSet<QString> importedCards = jsonStringSet(cards, QStringLiteral("cardId"));
        const QList<QVariantMap> storedCardRows = DbStore::query(
                    QStringLiteral("SELECT card_id FROM network_ic_card WHERE person_id=?"), {personId});
        for (const QVariantMap &storedCard : storedCardRows) {
            const QString cardId = storedCard.value(QStringLiteral("card_id")).toString();
            if (!importedCards.contains(cardId)
                    && !DbStore::execute(QStringLiteral("DELETE FROM network_ic_card WHERE card_id=?"), {cardId})) {
                writeOk = false;
                break;
            }
        }
        for (const QJsonValue &value : cards) {
            if (!writeOk) break;
            const QJsonObject card = value.toObject();
            const QString cardId = card.value(QStringLiteral("cardId")).toString().trimmed();
            if (cardId.isEmpty()) continue;
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO network_ic_card(card_id,person_id,name,bind_time,active) "
                                       "VALUES(?,?,?,?,1) ON CONFLICT(card_id) DO UPDATE SET "
                                       "person_id=excluded.person_id,active=1"),
                        {cardId, personId,
                         card.value(QStringLiteral("name")).toString(),
                         card.value(QStringLiteral("bindTime")).toString()});
        }
        if (!writeOk) {
            rollback();
            continue;
        }

        const QSet<QString> importedQrs = jsonStringSet(qrCodes, QStringLiteral("qrUnique"));
        const QList<QVariantMap> storedQrRows = DbStore::query(
                    QStringLiteral("SELECT qr_unique FROM network_qr_code WHERE person_id=?"), {personId});
        for (const QVariantMap &storedQr : storedQrRows) {
            const QString qrUnique = storedQr.value(QStringLiteral("qr_unique")).toString();
            if (!importedQrs.contains(qrUnique)
                    && (!DbStore::execute(QStringLiteral("DELETE FROM network_qr_access_cache WHERE qr_unique=?"), {qrUnique})
                        || !DbStore::execute(QStringLiteral("DELETE FROM network_qr_code WHERE qr_unique=?"), {qrUnique}))) {
                writeOk = false;
                break;
            }
        }
        for (const QJsonValue &value : qrCodes) {
            if (!writeOk) break;
            const QJsonObject qr = value.toObject();
            const QString qrUnique = qr.value(QStringLiteral("qrUnique")).toString().trimmed();
            if (qrUnique.isEmpty()) continue;
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO network_qr_code("
                                       "qr_unique,person_id,qr_name,qr_type,purpose,valid_from,valid_to,max_use_count,"
                                       "visitor_name,visitor_phone,bind_time,raw_json,active) "
                                       "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1) ON CONFLICT(qr_unique) DO UPDATE SET "
                                       "person_id=excluded.person_id,active=1"),
                        {qrUnique, personId,
                         qr.value(QStringLiteral("qrName")).toString(),
                         qr.value(QStringLiteral("qrType")).toString(),
                         qr.value(QStringLiteral("purpose")).toString(),
                         qr.value(QStringLiteral("validFrom")).toString(),
                         qr.value(QStringLiteral("validTo")).toString(),
                         qr.value(QStringLiteral("maxUseCount")).toVariant(),
                         qr.value(QStringLiteral("visitorName")).toString(),
                         qr.value(QStringLiteral("visitorPhone")).toString(),
                         qr.value(QStringLiteral("bindTime")).toString(), compactJson(qr)});
        }
        if (!writeOk) {
            rollback();
            continue;
        }

        writeOk = DbStore::execute(QStringLiteral("DELETE FROM network_timing_rule WHERE person_id=?"), {personId})
                && DbStore::execute(QStringLiteral("DELETE FROM network_access_rule WHERE person_id=?"), {personId});
        if (writeOk) {
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO network_access_rule("
                                       "person_id,time_enabled,count_enabled,relay_enabled,amount_enabled,duration_enabled,"
                                       "remote_call_enabled,remote_call_current_day_count,remote_call_every_day_count,"
                                       "remote_call_used_count,card_password_enabled,card_password,control_elevator,count_total,count_add_count,"
                                       "amount_total,amount_add_amount,amount_unit_price,duration_start,duration_end,"
                                       "elevator_value_type,raw_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
                        {personId,
                         rules.value(QStringLiteral("time")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("count")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("relay")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("amount")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("duration")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("remoteCall")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("remoteCallDataCurrentDayCallCount")).toVariant(),
                         rules.value(QStringLiteral("remoteCallDataEveryDayUpdateCallCount")).toVariant(),
                         0,
                         rules.value(QStringLiteral("cardPassword")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("cardPasswordDataPassword")).toString(),
                         rules.value(QStringLiteral("controlElevator")).toBool() ? 1 : 0,
                         rules.value(QStringLiteral("countDataTotal")).toVariant(),
                         rules.value(QStringLiteral("countDataAddCount")).toVariant(),
                         rules.value(QStringLiteral("amountDataTotal")).toVariant(),
                         rules.value(QStringLiteral("amountDataAddAmount")).toVariant(),
                         rules.value(QStringLiteral("amountDataUnitPrice")).toVariant(),
                         rules.value(QStringLiteral("durationDataStartTime")).toString(),
                         rules.value(QStringLiteral("durationDataEndTime")).toString(),
                         rules.value(QStringLiteral("controlElevatorDataValueType")).toVariant(), compactJson(rules)});
        }
        const QJsonArray timingRules = rules.value(QStringLiteral("timingRules")).toArray();
        for (int i = 0; writeOk && i < timingRules.size(); ++i) {
            const QJsonObject timing = timingRules.at(i).toObject();
            const QJsonArray range = timing.value(QStringLiteral("timeRange")).toArray();
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO network_timing_rule("
                                       "person_id,rule_order,days_json,start_time,end_time) VALUES(?,?,?,?,?)"),
                        {personId, i, compactJson(timing.value(QStringLiteral("days"))),
                         range.size() > 0 ? range.at(0).toString() : QString(),
                         range.size() > 1 ? range.at(1).toString() : QString()});
        }
        if (writeOk) {
            if (rules.value(QStringLiteral("count")).toBool()) {
                writeOk = DbStore::execute(
                            QStringLiteral("INSERT INTO network_access_usage(person_id,remaining_count,access_count,updated_at) "
                                           "VALUES(?,?,0,?) ON CONFLICT(person_id) DO UPDATE SET "
                                           "remaining_count=excluded.remaining_count,updated_at=excluded.updated_at"),
                            {personId, rules.value(QStringLiteral("countDataRemaining")).toVariant(), now});
            } else {
                writeOk = DbStore::execute(QStringLiteral("DELETE FROM network_access_usage WHERE person_id=?"), {personId});
            }
        }
        if (!writeOk) {
            rollback();
            continue;
        }

        writeOk = DbStore::execute(QStringLiteral("DELETE FROM network_floor WHERE person_id=?"), {personId});
        for (const QJsonValue &value : floors) {
            if (!writeOk) break;
            const QJsonObject floor = value.toObject();
            const QString deviceId = floor.value(QStringLiteral("deviceId")).toString().trimmed();
            for (const QJsonValue &number : floorNumbers(floor.value(QStringLiteral("floor")))) {
                if (!number.isDouble()) continue;
                writeOk = DbStore::execute(
                            QStringLiteral("INSERT OR IGNORE INTO network_floor(person_id,device_id,floor_no) VALUES(?,?,?)"),
                            {personId, deviceId, number.toInt()});
                if (!writeOk) break;
            }
        }
        if (writeOk) {
            writeOk = DbStore::execute(QStringLiteral("DELETE FROM network_person_tombstone WHERE person_id=?"), {personId});
        }
        if (!writeOk || !DbStore::transactionCommit()) {
            rollback();
            continue;
        }

        if (exists) ++summary.updated;
        else ++summary.inserted;
    }

    summary.ok = summary.failed == 0;
    if (!summary.ok && summary.error.isEmpty()) {
        summary.error = QStringLiteral("部分人员因资料冲突未导入");
    }
    return summary;
}

QJsonArray NetworkPersonnelStore::pendingFaceImageRequests() const
{
    QJsonArray requests;
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral(
                    "SELECT p.person_id,f.face_hash,f.image_path,f.face_base64 "
                    "FROM network_person p "
                    "JOIN network_face f ON f.person_id=p.person_id "
                    "WHERE p.active=1 AND p.deleted=0 "
                    "ORDER BY p.person_id,f.id"));

    QString currentPersonId;
    QJsonArray hashes;
    auto appendCurrent = [&]() {
        if (currentPersonId.isEmpty() || hashes.isEmpty()) {
            return;
        }
        QJsonObject request;
        request.insert(QStringLiteral("personId"), currentPersonId);
        request.insert(QStringLiteral("faceHashes"), hashes);
        requests.append(request);
    };

    for (const QVariantMap &row : rows) {
        const QString personId =
                row.value(QStringLiteral("person_id")).toString().trimmed();
        const QString faceHash =
                row.value(QStringLiteral("face_hash")).toString().trimmed();
        const QString imagePath =
                row.value(QStringLiteral("image_path")).toString().trimmed();
        const bool imageMissing =
                row.value(QStringLiteral("face_base64")).toString().isEmpty()
                || imagePath.isEmpty()
                || !QFileInfo::exists(imagePath);
        if (personId.isEmpty() || faceHash.isEmpty()) {
            continue;
        }
        if (!imageMissing) {
            continue;
        }
        if (!currentPersonId.isEmpty() && currentPersonId != personId) {
            appendCurrent();
            hashes = QJsonArray();
        }
        currentPersonId = personId;
        hashes.append(faceHash);
    }
    appendCurrent();
    return requests;
}

NetworkPersonnelSyncResult NetworkPersonnelStore::applyFaceImages(
        const QJsonObject &envelope)
{
    if (!initialized_ && !initialize()) {
        return makeResult(false,
                          QStringLiteral("DATABASE_NOT_READY"),
                          DbStore::lastError());
    }

    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    const QString personId =
            data.value(QStringLiteral("personId")).toString().trimmed();
    const QJsonArray faces = data.value(QStringLiteral("faces")).toArray();
    if (personId.isEmpty() || faces.isEmpty()) {
        return makeResult(false,
                          QStringLiteral("INVALID_FACE_RESPONSE"),
                          QStringLiteral("face.responseImages 缺少 personId 或 faces"),
                          personId);
    }

    const QList<QVariantMap> personRows = DbStore::query(
                QStringLiteral(
                    "SELECT person_id FROM network_person "
                    "WHERE person_id=? AND active=1 AND deleted=0 LIMIT 1"),
                {personId});
    if (personRows.isEmpty()) {
        return makeResult(false,
                          QStringLiteral("PERSON_NOT_FOUND"),
                          QStringLiteral("网络人员不存在或已删除"),
                          personId);
    }

    struct DecodedFace {
        QString name;
        QString faceHash;
        QString base64;
        QByteArray bytes;
        QString extension;
    };
    QList<DecodedFace> decodedFaces;
    QJsonArray failedFaces;
    auto appendFailedFace = [&failedFaces](const QString &name,
                                           const QString &faceHash,
                                           const QString &message,
                                           const QString &imagePath = QString()) {
        QJsonObject failed;
        failed.insert(QStringLiteral("name"), name);
        failed.insert(QStringLiteral("faceHash"), faceHash);
        failed.insert(QStringLiteral("message"), message);
        if (!imagePath.isEmpty()) {
            failed.insert(QStringLiteral("imagePath"), imagePath);
        }
        failedFaces.append(failed);
    };
    const QRegularExpression safeHash(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    for (const QJsonValue &faceValue : faces) {
        const QJsonObject face = faceValue.toObject();
        QString faceName = face.value(QStringLiteral("name"))
                .toString().trimmed();
        const QString faceHash =
                face.value(QStringLiteral("faceHash")).toString().trimmed();
        const QString base64 =
                face.value(QStringLiteral("faceBase64")).toString().trimmed();
        if (!safeHash.match(faceHash).hasMatch()) {
            appendFailedFace(
                        faceName, faceHash,
                        QStringLiteral("人脸图像数据无效，请重新录入人脸。"));
            continue;
        }

        const QList<QVariantMap> faceRows = DbStore::query(
                    QStringLiteral(
                        "SELECT id,name FROM network_face "
                        "WHERE person_id=? AND face_hash=? LIMIT 1"),
                    {personId, faceHash});
        if (faceRows.isEmpty()) {
            appendFailedFace(
                        faceName, faceHash,
                        QStringLiteral("人脸图像记录不存在，请重新同步人员信息。"));
            continue;
        }
        if (faceName.isEmpty()) {
            faceName = faceRows.first().value(QStringLiteral("name"))
                    .toString().trimmed();
        }
        if (base64.isEmpty()) {
            appendFailedFace(
                        faceName, faceHash,
                        QStringLiteral("人脸图像数据无效，请重新录入人脸。"));
            continue;
        }

        const int comma = base64.indexOf(QLatin1Char(','));
        const QString encoded =
                base64.startsWith(QStringLiteral("data:"),
                                  Qt::CaseInsensitive)
                && comma >= 0
                ? base64.mid(comma + 1)
                : base64;
        const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
        // faceHash is the server-side business key. It is not guaranteed to
        // be SHA-256(image bytes), so only validate the decoded image format.
        QImage image;
        if (bytes.isEmpty() || !image.loadFromData(bytes) || image.isNull()) {
            appendFailedFace(
                        faceName, faceHash,
                        QStringLiteral("人脸图像无法识别，请重新录入人脸。"));
            continue;
        }

        QString extension;
        const QString dataHeader =
                comma >= 0 ? base64.left(comma).toLower() : QString();
        if (dataHeader.contains(QStringLiteral("image/jpeg"))
                || dataHeader.contains(QStringLiteral("image/jpg"))
                || (bytes.size() >= 2
                    && static_cast<unsigned char>(bytes.at(0)) == 0xff
                    && static_cast<unsigned char>(bytes.at(1)) == 0xd8)) {
            extension = QStringLiteral("jpg");
        } else if (dataHeader.contains(QStringLiteral("image/webp"))
                   || (bytes.size() >= 12
                       && bytes.left(4) == QByteArrayLiteral("RIFF")
                       && bytes.mid(8, 4) == QByteArrayLiteral("WEBP"))) {
            extension = QStringLiteral("webp");
        } else if (dataHeader.contains(QStringLiteral("image/bmp"))
                   || bytes.left(2) == QByteArrayLiteral("BM")) {
            extension = QStringLiteral("bmp");
        } else {
            extension = QStringLiteral("png");
        }

        DecodedFace decoded;
        decoded.name = faceName;
        decoded.faceHash = faceHash;
        decoded.base64 = base64;
        decoded.bytes = bytes;
        decoded.extension = extension;
        decodedFaces.append(decoded);
    }

    QString safePersonId = personId;
    safePersonId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                         QStringLiteral("_"));
    const QString imageDirPath =
            QDir(QFileInfo(Rk3566Platform::databasePath()).absolutePath())
            .filePath(QStringLiteral("network_faces/%1").arg(safePersonId));
    QDir imageDir;
    if (!imageDir.mkpath(imageDirPath)) {
        for (const DecodedFace &decoded : decodedFaces) {
            appendFailedFace(
                        decoded.name, decoded.faceHash,
                        QStringLiteral("人脸图像保存失败，请重新录入人脸。"));
        }
        NetworkPersonnelSyncResult result = makeResult(
                    true,
                    QStringLiteral("FACE_IMAGES_NEED_VALIDATION"),
                    QStringLiteral("人脸图像保存失败，请重新录入人脸。"),
                    personId);
        result.data.insert(QStringLiteral("faceItems"), QJsonArray());
        result.data.insert(QStringLiteral("failedFaces"), failedFaces);
        return result;
    }

    if (!DbStore::transactionBegin()) {
        return makeResult(false,
                          QStringLiteral("TRANSACTION_FAILED"),
                          DbStore::lastError(),
                          personId);
    }

    QStringList writtenPaths;
    auto removeWrittenFiles = [&writtenPaths]() {
        for (const QString &path : writtenPaths) {
            QFile::remove(path);
        }
    };

    int storedCount = 0;
    QJsonArray storedItems;
    for (const DecodedFace &decoded : decodedFaces) {
        const QString imagePath =
                QDir(imageDirPath).filePath(
                    decoded.faceHash + QLatin1Char('.') + decoded.extension);
        QSaveFile file(imagePath);
        file.setDirectWriteFallback(true);
        if (!file.open(QIODevice::WriteOnly)) {
            appendFailedFace(
                        decoded.name, decoded.faceHash,
                        QStringLiteral("人脸图像保存失败，请重新录入人脸。"),
                        imagePath);
            continue;
        }
        if (file.write(decoded.bytes) != decoded.bytes.size()) {
            file.cancelWriting();
            appendFailedFace(
                        decoded.name, decoded.faceHash,
                        QStringLiteral("人脸图像保存失败，请重新录入人脸。"),
                        imagePath);
            continue;
        }
        if (!file.commit()) {
            appendFailedFace(
                        decoded.name, decoded.faceHash,
                        QStringLiteral("人脸图像保存失败，请重新录入人脸。"),
                        imagePath);
            continue;
        }

        const QFileInfo savedFile(imagePath);
        writtenPaths.append(imagePath);
        if (!savedFile.exists()
                || !savedFile.isFile()
                || savedFile.size() != decoded.bytes.size()) {
            appendFailedFace(
                        decoded.name, decoded.faceHash,
                        QStringLiteral("人脸图像保存失败，请重新录入人脸。"),
                        imagePath);
            continue;
        }

        if (!DbStore::execute(
                    QStringLiteral(
                        "UPDATE network_face SET face_base64=?,image_path=?,"
                        "feature_blob=NULL,model_version=NULL,"
                        "status='IMAGE_PENDING_VALIDATION' "
                        "WHERE person_id=? AND face_hash=?"),
                    {decoded.base64, imagePath, personId, decoded.faceHash})) {
            const QString error = DbStore::lastError();
            DbStore::transactionRollback();
            removeWrittenFiles();
            return makeResult(false,
                              QStringLiteral("FACE_IMAGE_UPDATE_FAILED"),
                              error,
                              personId);
        }
        ++storedCount;
        QJsonObject stored;
        stored.insert(QStringLiteral("name"), decoded.name);
        stored.insert(QStringLiteral("faceHash"), decoded.faceHash);
        stored.insert(QStringLiteral("imagePath"), imagePath);
        storedItems.append(stored);
    }

    if (!DbStore::execute(
                QStringLiteral(
                    "UPDATE network_person SET sync_state=?,"
                    "updated_at=? WHERE person_id=?"),
                {storedCount > 0
                     ? QStringLiteral("FACE_VALIDATING")
                     : QStringLiteral("FACE_PARTIAL_FAILED"),
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 personId})) {
        const QString error = DbStore::lastError();
        DbStore::transactionRollback();
        removeWrittenFiles();
        return makeResult(false,
                          QStringLiteral("PERSON_FACE_STATE_FAILED"),
                          error,
                          personId);
    }

    if (!DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        removeWrittenFiles();
        return makeResult(false,
                          QStringLiteral("DATABASE_COMMIT_FAILED"),
                          DbStore::lastError(),
                          personId);
    }

    NetworkPersonnelSyncResult result =
            makeResult(true,
                       QStringLiteral("FACE_IMAGES_PENDING_VALIDATION"),
                       QStringLiteral("人员人脸图像已保存，等待人脸质量校验: %1")
                           .arg(imageDirPath),
                       personId);
    result.data.insert(QStringLiteral("storedFaceCount"), storedCount);
    result.data.insert(QStringLiteral("faceItems"), storedItems);
    result.data.insert(QStringLiteral("failedFaces"), failedFaces);
    return result;
}

bool NetworkPersonnelStore::finalizeFaceImageValidation(
        const QString &personId,
        const QJsonArray &validatedFaces,
        const QJsonArray &failedFaces,
        bool *allFacesReady,
        QString *error)
{
    if (allFacesReady) *allFacesReady = false;
    if (error) error->clear();
    const QString normalizedPersonId = personId.trimmed();
    if (normalizedPersonId.isEmpty()
            || (validatedFaces.isEmpty() && failedFaces.isEmpty())) {
        if (error) *error = QStringLiteral("人员ID或人脸校验结果为空");
        return false;
    }
    if (!initialized_ && !initialize()) {
        if (error) *error = DbStore::lastError();
        return false;
    }
    if (!DbStore::transactionBegin()) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    auto rollback = [error]() {
        const QString message = DbStore::lastError();
        DbStore::transactionRollback();
        if (error) *error = message;
        return false;
    };

    for (const QJsonValue &value : validatedFaces) {
        const QJsonObject face = value.toObject();
        const QString faceHash = face.value(QStringLiteral("faceHash"))
                .toString().trimmed();
        const QByteArray feature = QByteArray::fromBase64(
                    face.value(QStringLiteral("featureBase64"))
                    .toString().toLatin1());
        const QString modelVersion = face.value(QStringLiteral("modelVersion"))
                .toString().trimmed();
        if (faceHash.isEmpty() || feature.isEmpty() || modelVersion.isEmpty()) {
            DbStore::transactionRollback();
            if (error) *error = QStringLiteral("通过的人脸校验结果缺少hash或特征");
            return false;
        }
        if (!DbStore::execute(
                    QStringLiteral(
                        "UPDATE network_face SET feature_blob=?,model_version=?,"
                        "status='IMAGE_READY' WHERE person_id=? AND face_hash=? "
                        "AND status='IMAGE_PENDING_VALIDATION'"),
                    {feature, modelVersion, normalizedPersonId, faceHash})) {
            return rollback();
        }
    }

    QStringList failedImagePaths;
    for (const QJsonValue &value : failedFaces) {
        const QJsonObject face = value.toObject();
        const QString faceHash = face.value(QStringLiteral("faceHash"))
                .toString().trimmed();
        const QString reportedImagePath = face.value(QStringLiteral("imagePath"))
                .toString().trimmed();
        if (!reportedImagePath.isEmpty()
                && !failedImagePaths.contains(reportedImagePath)) {
            failedImagePaths.append(reportedImagePath);
        }
        if (faceHash.isEmpty()) {
            continue;
        }
        const QList<QVariantMap> rows = DbStore::query(
                    QStringLiteral(
                        "SELECT image_path FROM network_face "
                        "WHERE person_id=? AND face_hash=? LIMIT 1"),
                    {normalizedPersonId, faceHash});
        if (!rows.isEmpty()) {
            const QString imagePath = rows.first()
                    .value(QStringLiteral("image_path")).toString().trimmed();
            if (!imagePath.isEmpty() && !failedImagePaths.contains(imagePath)) {
                failedImagePaths.append(imagePath);
            }
            if (!DbStore::execute(
                        QStringLiteral(
                            "UPDATE network_face SET face_base64=NULL,image_path=NULL,"
                            "feature_blob=NULL,model_version=NULL,status='IMAGE_FAILED' "
                            "WHERE person_id=? AND face_hash=?"),
                        {normalizedPersonId, faceHash})) {
                return rollback();
            }
        }
    }

    const QList<QVariantMap> incompleteRows = DbStore::query(
                QStringLiteral(
                    "SELECT COUNT(*) AS incomplete_count FROM network_face "
                    "WHERE person_id=? "
                    "AND COALESCE(status,'')<>'IMAGE_READY'"),
                {normalizedPersonId});
    if (incompleteRows.isEmpty()) {
        return rollback();
    }
    const bool ready = incompleteRows.first()
            .value(QStringLiteral("incomplete_count")).toInt() == 0;
    const QString personState = ready
            ? QStringLiteral("READY")
            : QStringLiteral("FACE_PARTIAL_FAILED");
    if (!DbStore::execute(
                QStringLiteral(
                    "UPDATE network_person SET sync_state=?,updated_at=? "
                    "WHERE person_id=?"),
                {personState,
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 normalizedPersonId})) {
        return rollback();
    }
    if (!DbStore::transactionCommit()) {
        return rollback();
    }

    if (allFacesReady) *allFacesReady = ready;

    QString safePersonId = normalizedPersonId;
    safePersonId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                         QStringLiteral("_"));
    const QString expectedDir = QDir(
                QFileInfo(Rk3566Platform::databasePath()).absolutePath())
            .filePath(QStringLiteral("network_faces/%1").arg(safePersonId));
    const QString expectedPrefix = QDir(expectedDir).absolutePath()
            + QDir::separator();
    for (const QString &path : failedImagePaths) {
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        if (absolutePath.startsWith(expectedPrefix)
                && QFileInfo::exists(absolutePath)
                && !QFile::remove(absolutePath)) {
            if (error) {
                *error = QStringLiteral("失败人脸图像文件清理失败: %1")
                        .arg(absolutePath);
            }
            return false;
        }
    }
    QDir().rmdir(expectedDir);
    return true;
}

NetworkPersonnelSyncResult NetworkPersonnelStore::applyFullPersonnel(
        const QJsonObject &envelope)
{
    if (!initialize()) {
        return makeResult(false, QStringLiteral("DATABASE_INIT_FAILED"), DbStore::lastError());
    }

    const QString hash = payloadHash(envelope);
    NetworkPersonnelSyncResult cached;
    if (loadCachedResult(envelope, hash, &cached)) {
        return cached;
    }

    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    const QString personId = data.value(QStringLiteral("personId")).toString().trimmed();
    const QString personHash = data.value(QStringLiteral("personHash")).toString().trimmed();
    if (requestId(envelope).isEmpty() || personId.isEmpty() || personHash.isEmpty()) {
        return makeResult(false,
                          QStringLiteral("INVALID_PAYLOAD"),
                          QStringLiteral("id、data.personId 或 data.personHash 为空"),
                          personId);
    }

    const QString serverTime = envelope.value(QStringLiteral("time")).toString().trimmed();
    const QList<QVariantMap> tombstones = DbStore::query(
                QStringLiteral("SELECT server_time FROM network_person_tombstone WHERE person_id=?"),
                {personId});
    if (!tombstones.isEmpty() && !serverTime.isEmpty()) {
        const QString deletedServerTime =
                tombstones.first().value(QStringLiteral("server_time")).toString();
        if (!deletedServerTime.isEmpty() && deletedServerTime > serverTime) {
            return makeResult(false,
                              QStringLiteral("STALE_PERSON_VERSION"),
                              QStringLiteral("同步消息早于本地已处理的删除消息"),
                              personId);
        }
    }

    const QList<QVariantMap> existing = DbStore::query(
                QStringLiteral("SELECT person_hash,active,deleted,sync_state "
                               "FROM network_person WHERE person_id=?"),
                {personId});

    // personHash 相同也不能单独作为跳过依据：服务器 faces 中的 hash 集合
    // 可能与本地元数据不一致。只有人员聚合 hash 和人脸 hash 集合都一致，
    // 才允许跳过本次完整快照写入；缺失的图像文件由后续自动拉取补齐。
    const QJsonArray faces = data.value(QStringLiteral("faces")).toArray();
    QSet<QString> incomingFaceHashes;
    for (const QJsonValue &value : faces) {
        const QString faceHash = value.toObject()
                .value(QStringLiteral("faceHash")).toString().trimmed();
        if (!faceHash.isEmpty()) {
            incomingFaceHashes.insert(faceHash);
        }
    }
    QSet<QString> localFaceHashes;
    const QList<QVariantMap> localFaceRows = DbStore::query(
                QStringLiteral("SELECT face_hash FROM network_face WHERE person_id=?"),
                {personId});
    for (const QVariantMap &row : localFaceRows) {
        const QString faceHash = row.value(QStringLiteral("face_hash"))
                .toString().trimmed();
        if (!faceHash.isEmpty()) {
            localFaceHashes.insert(faceHash);
        }
    }
    const bool faceHashesUnchanged = incomingFaceHashes == localFaceHashes;
    const bool unchanged = !existing.isEmpty()
            && existing.first().value(QStringLiteral("person_hash")).toString() == personHash
            && existing.first().value(QStringLiteral("active")).toInt() == 1
            && existing.first().value(QStringLiteral("deleted")).toInt() == 0
            && existing.first().value(QStringLiteral("sync_state")).toString()
                    == QStringLiteral("READY")
            && faceHashesUnchanged;

    if (!DbStore::transactionBegin()) {
        return makeResult(false, QStringLiteral("TRANSACTION_FAILED"), DbStore::lastError(), personId);
    }

    if (unchanged) {
        NetworkPersonnelSyncResult result =
                makeResult(true,
                           QStringLiteral("HASH_UNCHANGED"),
                           QStringLiteral("personHash 未变化，已跳过更新"),
                           personId);
        result.data.insert(QStringLiteral("personHash"), personHash);
        result.data.insert(QStringLiteral("changed"), false);
        if (!recordResult(envelope, personId, hash, result)
                || !DbStore::transactionCommit()) {
            DbStore::transactionRollback();
            return makeResult(false, QStringLiteral("DATABASE_WRITE_FAILED"), DbStore::lastError(), personId);
        }
        return result;
    }

    auto rollbackError = [&](const QString &code) {
        const QString error = DbStore::lastError();
        DbStore::transactionRollback();
        return makeResult(false, code, error, personId);
    };

    const QJsonObject person = data.value(QStringLiteral("person")).toObject();
    const QString now = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!DbStore::execute(
                QStringLiteral(
                    "INSERT INTO network_person("
                    "person_id,person_hash,name,phone,id_card,employee_no,department,position,"
                    "hire_date_ms,person_type,status,remark,active,deleted,sync_state,server_time,"
                    "person_json,created_at,updated_at"
                    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1,0,'READY',?,?,?,?) "
                    "ON CONFLICT(person_id) DO UPDATE SET "
                    "person_hash=excluded.person_hash,name=excluded.name,phone=excluded.phone,"
                    "id_card=excluded.id_card,employee_no=excluded.employee_no,"
                    "department=excluded.department,position=excluded.position,"
                    "hire_date_ms=excluded.hire_date_ms,person_type=excluded.person_type,"
                    "status=excluded.status,remark=excluded.remark,active=1,deleted=0,"
                    "sync_state='READY',server_time=excluded.server_time,"
                    "person_json=excluded.person_json,updated_at=excluded.updated_at"),
                {personId,
                 personHash,
                 person.value(QStringLiteral("name")).toString(),
                 person.value(QStringLiteral("phone")).toString(),
                 person.value(QStringLiteral("idCard")).toString(),
                 person.value(QStringLiteral("employeeNo")).toString(),
                 person.value(QStringLiteral("department")).toString(),
                 person.value(QStringLiteral("position")).toString(),
                 person.value(QStringLiteral("hireDate")).toVariant(),
                 person.value(QStringLiteral("personType")).toString(),
                 person.value(QStringLiteral("status")).toInt(1),
                 person.value(QStringLiteral("remark")).toString(),
                 serverTime,
                 compactJson(person),
                 now,
                 now})) {
        return rollbackError(QStringLiteral("PERSON_WRITE_FAILED"));
    }

    // 人脸采用 hash 差量替换：相同 hash 保留后续可能生成的图片路径和特征，不同 hash 删除。
    for (const QJsonValue &value : faces) {
        const QJsonObject face = value.toObject();
        const QString faceHash = face.value(QStringLiteral("faceHash")).toString().trimmed();
        if (faceHash.isEmpty()) {
            continue;
        }
        incomingFaceHashes.insert(faceHash);
        if (!DbStore::execute(
                    QStringLiteral(
                        "INSERT INTO network_face(person_id,face_hash,name,bind_time,status) "
                        "VALUES(?,?,?,?,'METADATA') "
                        "ON CONFLICT(person_id,face_hash) DO UPDATE SET "
                        "name=excluded.name,bind_time=excluded.bind_time"),
                    {personId,
                     faceHash,
                     face.value(QStringLiteral("name")).toString(),
                     face.value(QStringLiteral("bindTime")).toString()})) {
            return rollbackError(QStringLiteral("FACE_WRITE_FAILED"));
        }
    }
    const QList<QVariantMap> storedFaces = DbStore::query(
                QStringLiteral("SELECT face_hash FROM network_face WHERE person_id=?"),
                {personId});
    for (const QVariantMap &stored : storedFaces) {
        const QString storedHash = stored.value(QStringLiteral("face_hash")).toString();
        if (!incomingFaceHashes.contains(storedHash)
                && !DbStore::execute(
                    QStringLiteral("DELETE FROM network_face WHERE person_id=? AND face_hash=?"),
                    {personId, storedHash})) {
            return rollbackError(QStringLiteral("FACE_DELETE_FAILED"));
        }
    }

    // fullPersonnel 中的卡片、二维码、规则和楼层均按完整快照替换。
    if (!DbStore::execute(QStringLiteral("DELETE FROM network_ic_card WHERE person_id=?"), {personId})) {
        return rollbackError(QStringLiteral("CARD_DELETE_FAILED"));
    }
    const QJsonArray cards = data.value(QStringLiteral("icCards")).toArray();
    for (const QJsonValue &value : cards) {
        const QJsonObject card = value.toObject();
        const QString cardId = card.value(QStringLiteral("cardId")).toString().trimmed();
        if (cardId.isEmpty()) {
            continue;
        }
        if (!DbStore::execute(
                    QStringLiteral(
                        "INSERT INTO network_ic_card(card_id,person_id,name,bind_time,active) "
                        "VALUES(?,?,?,?,1) "
                        "ON CONFLICT(card_id) DO UPDATE SET person_id=excluded.person_id,"
                        "name=excluded.name,bind_time=excluded.bind_time,active=1"),
                    {cardId,
                     personId,
                     card.value(QStringLiteral("name")).toString(),
                     card.value(QStringLiteral("bindTime")).toString()})) {
            return rollbackError(QStringLiteral("CARD_WRITE_FAILED"));
        }
    }

    if (!DbStore::execute(
                QStringLiteral("DELETE FROM network_qr_access_cache WHERE person_id=?"),
                {personId})
            || !DbStore::execute(
                QStringLiteral("DELETE FROM network_qr_code WHERE person_id=?"),
                {personId})) {
        return rollbackError(QStringLiteral("QR_DELETE_FAILED"));
    }
    const QJsonArray qrCodes = data.value(QStringLiteral("qrCodes")).toArray();
    for (const QJsonValue &value : qrCodes) {
        const QJsonObject qr = value.toObject();
        const QString qrUnique = qr.value(QStringLiteral("qrUnique")).toString().trimmed();
        if (qrUnique.isEmpty()) {
            continue;
        }
        if (!DbStore::execute(
                    QStringLiteral(
                        "INSERT INTO network_qr_code("
                        "qr_unique,person_id,qr_name,qr_type,purpose,valid_from,valid_to,"
                        "max_use_count,visitor_name,visitor_phone,bind_time,raw_json,active"
                        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1) "
                        "ON CONFLICT(qr_unique) DO UPDATE SET "
                        "person_id=excluded.person_id,qr_name=excluded.qr_name,"
                        "qr_type=excluded.qr_type,purpose=excluded.purpose,"
                        "valid_from=excluded.valid_from,valid_to=excluded.valid_to,"
                        "max_use_count=excluded.max_use_count,visitor_name=excluded.visitor_name,"
                        "visitor_phone=excluded.visitor_phone,bind_time=excluded.bind_time,"
                        "raw_json=excluded.raw_json,active=1"),
                    {qrUnique,
                     personId,
                     qr.value(QStringLiteral("qrName")).toString(),
                     qr.value(QStringLiteral("qrType")).toString(),
                     qr.value(QStringLiteral("purpose")).toString(),
                     qr.value(QStringLiteral("validFrom")).toString(),
                     qr.value(QStringLiteral("validTo")).toString(),
                     qr.value(QStringLiteral("maxUseCount")).toVariant(),
                     qr.value(QStringLiteral("visitorName")).toString(),
                     qr.value(QStringLiteral("visitorPhone")).toString(),
                     qr.value(QStringLiteral("bindTime")).toString(),
                     compactJson(qr)})) {
            return rollbackError(QStringLiteral("QR_WRITE_FAILED"));
        }
    }

    if (!DbStore::execute(QStringLiteral("DELETE FROM network_timing_rule WHERE person_id=?"), {personId})
            || !DbStore::execute(QStringLiteral("DELETE FROM network_access_rule WHERE person_id=?"), {personId})
            || !DbStore::execute(QStringLiteral("DELETE FROM network_access_usage WHERE person_id=?"), {personId})) {
        return rollbackError(QStringLiteral("RULE_DELETE_FAILED"));
    }
    const QJsonObject rules = data.value(QStringLiteral("rules")).toObject();
    if (!rules.isEmpty()) {
        if (!DbStore::execute(
                    QStringLiteral(
                        "INSERT INTO network_access_rule("
                        "person_id,time_enabled,count_enabled,relay_enabled,amount_enabled,"
                        "duration_enabled,remote_call_enabled,remote_call_current_day_count,"
                        "remote_call_every_day_count,remote_call_used_count,card_password_enabled,card_password,control_elevator,"
                        "count_total,count_add_count,amount_total,amount_add_amount,amount_unit_price,"
                        "duration_start,duration_end,elevator_value_type,raw_json"
                        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
                    {personId,
                     rules.value(QStringLiteral("time")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("count")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("relay")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("amount")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("duration")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("remoteCall")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("remoteCallDataCurrentDayCallCount")).toVariant(),
                     rules.value(QStringLiteral("remoteCallDataEveryDayUpdateCallCount")).toVariant(),
                     0,
                     rules.value(QStringLiteral("cardPassword")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("cardPasswordDataPassword")).toString(),
                     rules.value(QStringLiteral("controlElevator")).toBool() ? 1 : 0,
                     rules.value(QStringLiteral("countDataTotal")).toVariant(),
                     rules.value(QStringLiteral("countDataAddCount")).toVariant(),
                     rules.value(QStringLiteral("amountDataTotal")).toVariant(),
                     rules.value(QStringLiteral("amountDataAddAmount")).toVariant(),
                     rules.value(QStringLiteral("amountDataUnitPrice")).toVariant(),
                     rules.value(QStringLiteral("durationDataStartTime")).toString(),
                     rules.value(QStringLiteral("durationDataEndTime")).toString(),
                     rules.value(QStringLiteral("controlElevatorDataValueType")).toVariant(),
                     compactJson(rules)})) {
            return rollbackError(QStringLiteral("RULE_WRITE_FAILED"));
        }

        if (rules.value(QStringLiteral("count")).toBool()
                && !DbStore::execute(
                    QStringLiteral("INSERT INTO network_access_usage("
                                   "person_id,remaining_count,access_count,updated_at) "
                                   "VALUES(?,?,0,?)"),
                    {personId,
                     rules.value(QStringLiteral("countDataTotal")).toVariant(),
                     now})) {
            return rollbackError(QStringLiteral("USAGE_WRITE_FAILED"));
        }

        const QJsonArray timingRules = rules.value(QStringLiteral("timingRules")).toArray();
        for (int i = 0; i < timingRules.size(); ++i) {
            const QJsonObject timing = timingRules.at(i).toObject();
            const QJsonArray range = timing.value(QStringLiteral("timeRange")).toArray();
            if (!DbStore::execute(
                        QStringLiteral(
                            "INSERT INTO network_timing_rule("
                            "person_id,rule_order,days_json,start_time,end_time"
                            ") VALUES(?,?,?,?,?)"),
                        {personId,
                         i,
                         compactJson(timing.value(QStringLiteral("days"))),
                         range.size() > 0 ? range.at(0).toString() : QString(),
                         range.size() > 1 ? range.at(1).toString() : QString()})) {
                return rollbackError(QStringLiteral("TIMING_RULE_WRITE_FAILED"));
            }
        }
    }

    if (!DbStore::execute(QStringLiteral("DELETE FROM network_floor WHERE person_id=?"), {personId})) {
        return rollbackError(QStringLiteral("FLOOR_DELETE_FAILED"));
    }
    const QJsonArray floors = data.value(QStringLiteral("floors")).toArray();
    for (const QJsonValue &value : floors) {
        const QJsonObject floor = value.toObject();
        const QString floorDeviceId = floor.value(QStringLiteral("deviceId")).toString().trimmed();
        const QJsonArray numbers = floorNumbers(floor.value(QStringLiteral("floor")));
        for (const QJsonValue &number : numbers) {
            if (!number.isDouble()) {
                continue;
            }
            if (!DbStore::execute(
                        QStringLiteral(
                            "INSERT OR IGNORE INTO network_floor(person_id,device_id,floor_no) "
                            "VALUES(?,?,?)"),
                        {personId, floorDeviceId, number.toInt()})) {
                return rollbackError(QStringLiteral("FLOOR_WRITE_FAILED"));
            }
        }
    }

    if (!DbStore::execute(QStringLiteral("DELETE FROM network_person_tombstone WHERE person_id=?"),
                          {personId})) {
        return rollbackError(QStringLiteral("TOMBSTONE_DELETE_FAILED"));
    }

    NetworkPersonnelSyncResult result =
            makeResult(true,
                       QStringLiteral("APPLIED"),
                       QStringLiteral("网络人员数据已保存"),
                       personId);
    result.data.insert(QStringLiteral("personHash"), personHash);
    result.data.insert(QStringLiteral("changed"), true);
    result.data.insert(QStringLiteral("faceCount"), faces.size());
    result.data.insert(QStringLiteral("icCardCount"), cards.size());
    result.data.insert(QStringLiteral("qrCodeCount"), qrCodes.size());
    if (!recordResult(envelope, personId, hash, result)
            || !DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        return makeResult(false, QStringLiteral("DATABASE_COMMIT_FAILED"), DbStore::lastError(), personId);
    }
    return result;
}

bool NetworkPersonnelStore::beginQrAccessTransaction(const QString &scanId,
                                                      const QString &deviceId,
                                                      const QString &qrCode,
                                                      QString *error)
{
    if (error) error->clear();
    if (!initialized_ && !initialize()) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    const QString normalizedScanId = scanId.trimmed();
    const QString normalizedQrCode = qrCode.trimmed();
    if (normalizedScanId.isEmpty() || deviceId.trimmed().isEmpty()
            || normalizedQrCode.isEmpty()) {
        if (error) *error = QStringLiteral("扫码事务参数为空");
        return false;
    }

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const bool ok = DbStore::execute(
                QStringLiteral(
                    "INSERT INTO network_qr_transaction("
                    "scan_id,device_id,qr_unique,state,created_at,updated_at"
                    ") VALUES(?,?,?,'SCAN_CREATED',?,?)"),
                {normalizedScanId, deviceId.trimmed(), normalizedQrCode, now, now});
    if (!ok && error) *error = DbStore::lastError();
    return ok;
}

bool NetworkPersonnelStore::recordQrScanResult(
        const QString &scanId,
        const QString &messageId,
        int code,
        bool success,
        const QString &reason,
        const QJsonObject &authorizationData,
        bool *duplicate,
        QString *error)
{
    if (duplicate) *duplicate = false;
    if (error) error->clear();
    if (!initialized_ && !initialize()) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT scan_result_id FROM network_qr_transaction "
                               "WHERE scan_id=? LIMIT 1"),
                {scanId.trimmed()});
    if (rows.isEmpty()) {
        if (error) *error = QStringLiteral("未找到对应扫码事务");
        return false;
    }

    const QString oldMessageId = rows.first()
            .value(QStringLiteral("scan_result_id")).toString().trimmed();
    if (!oldMessageId.isEmpty()) {
        if (oldMessageId == messageId.trimmed()) {
            if (duplicate) *duplicate = true;
            return true;
        }
        if (error) *error = QStringLiteral("同一扫码事务已存在其他 scanResult");
        return false;
    }

    // 访客楼层只参与本次 RS485 控制，不把 scanResult 中的楼层授权留在数据库。
    QJsonObject storedAuthorizationData = authorizationData;
    if (storedAuthorizationData.value(QStringLiteral("sourceType"))
            .toString().trimmed().compare(QStringLiteral("VISITOR"),
                                          Qt::CaseInsensitive) == 0) {
        storedAuthorizationData.remove(QStringLiteral("floorHex"));
        storedAuthorizationData.remove(QStringLiteral("floors"));
    }

    const bool ok = DbStore::execute(
                QStringLiteral(
                    "UPDATE network_qr_transaction SET "
                    "scan_result_id=?,scan_code=?,scan_success=?,scan_reason=?,"
                    "scan_data_json=?,state=CASE WHEN state='SCAN_CREATED' THEN ? ELSE state END,"
                    "updated_at=? WHERE scan_id=?"),
                {messageId.trimmed(),
                 code,
                 success ? 1 : 0,
                 reason,
                  compactJson(storedAuthorizationData),
                 success ? QStringLiteral("SCAN_ACCEPTED")
                         : QStringLiteral("SCAN_REJECTED"),
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 scanId.trimmed()});
    if (!ok && error) *error = DbStore::lastError();
    return ok;
}

bool NetworkPersonnelStore::saveQrFloorControl(
        const QString &scanId,
        const QString &messageId,
        const NetworkQrAuthorization &authorization,
        bool *duplicate,
        bool *changed,
        QString *error)
{
    if (duplicate) *duplicate = false;
    if (changed) *changed = false;
    if (error) error->clear();
    if (!initialized_ && !initialize()) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    const QString normalizedMessageId = messageId.trimmed();
    const QList<QVariantMap> duplicateRows = DbStore::query(
                QStringLiteral("SELECT scan_id FROM network_qr_transaction "
                               "WHERE floor_control_id=? LIMIT 1"),
                {normalizedMessageId});
    if (!duplicateRows.isEmpty()) {
        if (duplicate) *duplicate = true;
        return true;
    }

    const QList<QVariantMap> transactions = DbStore::query(
                QStringLiteral("SELECT qr_unique FROM network_qr_transaction "
                               "WHERE scan_id=? LIMIT 1"),
                {scanId.trimmed()});
    if (transactions.isEmpty()
            || transactions.first().value(QStringLiteral("qr_unique"))
                .toString() != authorization.qrCode.trimmed()) {
        if (error) *error = QStringLiteral("楼层控制与扫码事务不匹配");
        return false;
    }

    QJsonArray floorArray;
    for (const QString &floor : authorization.floors) {
        floorArray.append(floor);
    }
    const QString floorsJson = compactJson(floorArray);

    const QString sourceType = authorization.sourceType.trimmed().toUpper();
    const bool persistFloorAuthorization = sourceType == QStringLiteral("CREDENTIAL");
    bool cacheChanged = false;
    if (persistFloorAuthorization) {
        cacheChanged = true;
        const QList<QVariantMap> cacheRows = DbStore::query(
                    QStringLiteral(
                        "SELECT person_id,source_type,invitation_id,floor_hex,floors_json "
                        "FROM network_qr_access_cache WHERE qr_unique=? LIMIT 1"),
                    {authorization.qrCode.trimmed()});
        if (!cacheRows.isEmpty()) {
            const QVariantMap old = cacheRows.first();
            cacheChanged = old.value(QStringLiteral("person_id")).toString()
                        != authorization.personId.trimmed()
                    || old.value(QStringLiteral("source_type")).toString()
                        != sourceType
                    || old.value(QStringLiteral("invitation_id")).toString()
                        != authorization.invitationId.trimmed()
                    || old.value(QStringLiteral("floor_hex")).toString()
                        .compare(authorization.floorHex.trimmed(), Qt::CaseInsensitive) != 0
                    || old.value(QStringLiteral("floors_json")).toString() != floorsJson;
        }
    }

    if (!DbStore::transactionBegin()) {
        if (error) *error = DbStore::lastError();
        return false;
    }
    auto rollback = [&](const QString &why) {
        DbStore::transactionRollback();
        if (error) *error = why.isEmpty() ? DbStore::lastError() : why;
        return false;
    };

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (persistFloorAuthorization && cacheChanged && !DbStore::execute(
                QStringLiteral(
                    "INSERT INTO network_qr_access_cache("
                    "qr_unique,person_id,source_type,invitation_id,floor_hex,floors_json,updated_at"
                    ") VALUES(?,?,?,?,?,?,?) "
                    "ON CONFLICT(qr_unique) DO UPDATE SET "
                    "person_id=excluded.person_id,source_type=excluded.source_type,"
                    "invitation_id=excluded.invitation_id,floor_hex=excluded.floor_hex,"
                    "floors_json=excluded.floors_json,updated_at=excluded.updated_at"),
                {authorization.qrCode.trimmed(),
                 authorization.personId.trimmed(),
                 sourceType,
                 authorization.invitationId.trimmed(),
                 authorization.floorHex.trimmed().toUpper(),
                 floorsJson,
                 now})) {
        return rollback(DbStore::lastError());
    }
    if (!persistFloorAuthorization && !DbStore::execute(
                QStringLiteral("DELETE FROM network_qr_access_cache WHERE qr_unique=?"),
                {authorization.qrCode.trimmed()})) {
        return rollback(DbStore::lastError());
    }

    // VISITOR 的楼层只用于本次 RS485 下发，不写入授权缓存和事务楼层字段。
    const QVariant storedFloorHex = persistFloorAuthorization
            ? QVariant(authorization.floorHex.trimmed().toUpper()) : QVariant();
    const QVariant storedFloorsJson = persistFloorAuthorization
            ? QVariant(floorsJson) : QVariant();

    if (!DbStore::execute(
                QStringLiteral(
                    "UPDATE network_qr_transaction SET "
                    "person_id=?,source_type=?,invitation_id=?,floor_control_id=?,"
                    "floor_hex=?,floors_json=?,state='FLOOR_CONTROL_RECEIVED',updated_at=? "
                    "WHERE scan_id=?"),
                {authorization.personId.trimmed(),
                 sourceType,
                 authorization.invitationId.trimmed(),
                 normalizedMessageId,
                 storedFloorHex,
                 storedFloorsJson,
                 now,
                 scanId.trimmed()})) {
        return rollback(DbStore::lastError());
    }

    if (!DbStore::transactionCommit()) {
        return rollback(DbStore::lastError());
    }
    if (changed) *changed = persistFloorAuthorization && cacheChanged;
    return true;
}

bool NetworkPersonnelStore::recordQrRs485Result(const QString &scanId,
                                                 bool success,
                                                 const QString &reason,
                                                 QString *error)
{
    if (error) error->clear();
    const bool ok = DbStore::execute(
                QStringLiteral(
                    "UPDATE network_qr_transaction SET rs485_success=?,rs485_reason=?,"
                    "state=?,updated_at=? WHERE scan_id=?"),
                {success ? 1 : 0,
                 reason,
                 success ? QStringLiteral("RS485_SUCCEEDED")
                         : QStringLiteral("RS485_FAILED"),
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 scanId.trimmed()});
    if (!ok && error) *error = DbStore::lastError();
    return ok;
}

bool NetworkPersonnelStore::recordQrAccessResult(const QString &scanId,
                                                  const QString &messageId,
                                                  bool success,
                                                  bool submitted,
                                                  QString *error)
{
    if (error) error->clear();
    const bool ok = DbStore::execute(
                QStringLiteral(
                    "UPDATE network_qr_transaction SET access_result_id=?,"
                    "access_result_success=?,access_result_sent=?,state=?,updated_at=? "
                    "WHERE scan_id=?"),
                {messageId.trimmed(),
                 success ? 1 : 0,
                 submitted ? 1 : 0,
                 submitted ? QStringLiteral("ACCESS_REPORTED")
                           : QStringLiteral("ACCESS_REPORT_FAILED"),
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 scanId.trimmed()});
    if (!ok && error) *error = DbStore::lastError();
    return ok;
}

NetworkRemoteCallContext NetworkPersonnelStore::beginRemoteCallTransaction(
        const QString &requestIdValue,
        const QString &personId,
        int floor,
        const QString &source)
{
    NetworkRemoteCallContext result;
    if (!initialized_ && !initialize()) {
        result.error = DbStore::lastError();
        return result;
    }

    result.requestId = requestIdValue.trimmed();
    result.personId = personId.trimmed();
    result.floor = floor;
    result.source = source.trimmed().isEmpty()
            ? QStringLiteral("platform") : source.trimmed();
    if (result.requestId.isEmpty() || result.personId.isEmpty() || floor == 0) {
        result.error = QStringLiteral("remoteCall 缺少 requestId、personId 或有效楼层");
        return result;
    }

    const QList<QVariantMap> existing = DbStore::query(
                QStringLiteral("SELECT person_id,floor,source,local_deducted,"
                               "reserved_remaining_count FROM network_remote_call_transaction "
                               "WHERE request_id=? LIMIT 1"),
                {result.requestId});
    if (!existing.isEmpty()) {
        const QVariantMap row = existing.first();
        result.ok = true;
        result.duplicate = true;
        result.personId = row.value(QStringLiteral("person_id")).toString();
        result.floor = row.value(QStringLiteral("floor")).toInt();
        result.source = row.value(QStringLiteral("source")).toString();
        result.localDeducted = row.value(QStringLiteral("local_deducted")).toBool();
        result.remainingCount = row.value(
                    QStringLiteral("reserved_remaining_count")).isNull()
                ? -1 : row.value(QStringLiteral("reserved_remaining_count")).toLongLong();
        return result;
    }

    const QList<QVariantMap> pending = DbStore::query(
                QStringLiteral("SELECT request_id FROM network_remote_call_transaction "
                               "WHERE person_id=? AND floor=? AND state IN "
                               "('CREATED','RS485_SUCCEEDED','ACCESS_REPORTED') LIMIT 1"),
                {result.personId, floor});
    if (!pending.isEmpty()) {
        result.error = QStringLiteral("同一人员同一楼层已有待确认的远程呼梯事务");
        return result;
    }

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!DbStore::execute(
                QStringLiteral("INSERT INTO network_remote_call_transaction("
                               "request_id,person_id,floor,source,state,created_at,updated_at) "
                               "VALUES(?,?,?,?,?,?,?)"),
                {result.requestId, result.personId, floor, result.source,
                 QStringLiteral("CREATED"), now, now})) {
        result.error = DbStore::lastError();
        return result;
    }
    result.ok = true;
    return result;
}

NetworkRemoteCallContext NetworkPersonnelStore::recordRemoteCallRs485Result(
        const QString &requestIdValue,
        bool success,
        const QString &reason)
{
    NetworkRemoteCallContext result;
    if (!initialized_ && !initialize()) {
        result.error = DbStore::lastError();
        return result;
    }
    result.requestId = requestIdValue.trimmed();
    if (result.requestId.isEmpty()) {
        result.error = QStringLiteral("remoteCall requestId 为空");
        return result;
    }
    if (!DbStore::transactionBegin()) {
        result.error = DbStore::lastError();
        return result;
    }
    auto rollback = [&](const QString &why) {
        DbStore::transactionRollback();
        result.error = why.isEmpty() ? DbStore::lastError() : why;
        return result;
    };

    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT person_id,floor,source,rs485_success,local_deducted,"
                               "reserved_remaining_count FROM network_remote_call_transaction "
                               "WHERE request_id=? LIMIT 1"),
                {result.requestId});
    if (rows.isEmpty()) {
        return rollback(QStringLiteral("未找到远程呼梯事务"));
    }
    const QVariantMap row = rows.first();
    result.personId = row.value(QStringLiteral("person_id")).toString();
    result.floor = row.value(QStringLiteral("floor")).toInt();
    result.source = row.value(QStringLiteral("source")).toString();
    if (!row.value(QStringLiteral("rs485_success")).isNull()) {
        result.ok = true;
        result.duplicate = true;
        result.localDeducted = row.value(QStringLiteral("local_deducted")).toBool();
        result.remainingCount = row.value(
                    QStringLiteral("reserved_remaining_count")).isNull()
                ? -1 : row.value(QStringLiteral("reserved_remaining_count")).toLongLong();
        DbStore::transactionCommit();
        return result;
    }

    if (success) {
        const QList<QVariantMap> ruleRows = DbStore::query(
                    QStringLiteral("SELECT remote_call_enabled,remote_call_current_day_count "
                                   "FROM network_access_rule WHERE person_id=? LIMIT 1"),
                    {result.personId});
        if (!ruleRows.isEmpty()
                && ruleRows.first().value(QStringLiteral("remote_call_enabled")).toBool()) {
            bool countOk = false;
            const qlonglong count = ruleRows.first().value(
                        QStringLiteral("remote_call_current_day_count")).toLongLong(&countOk);
            if (countOk && count > 0) {
                result.remainingCount = count - 1;
                if (!DbStore::execute(
                            QStringLiteral("UPDATE network_access_rule SET "
                                           "remote_call_current_day_count=?,"
                                           "remote_call_used_count=remote_call_used_count+1 "
                                           "WHERE person_id=?"),
                            {result.remainingCount, result.personId})) {
                    return rollback(DbStore::lastError());
                }
                result.localDeducted = true;
            }
        }
    }

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!DbStore::execute(
                QStringLiteral("UPDATE network_remote_call_transaction SET "
                               "rs485_success=?,rs485_reason=?,local_deducted=?,"
                               "reserved_remaining_count=?,state=?,updated_at=? WHERE request_id=?"),
                {success ? 1 : 0, reason, result.localDeducted ? 1 : 0,
                 result.localDeducted ? QVariant(result.remainingCount) : QVariant(),
                 success ? QStringLiteral("RS485_SUCCEEDED")
                         : QStringLiteral("RS485_FAILED"),
                 now, result.requestId})) {
        return rollback(DbStore::lastError());
    }
    if (!DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        result.error = DbStore::lastError();
        return result;
    }
    result.ok = true;
    return result;
}

bool NetworkPersonnelStore::recordRemoteCallAccessResult(
        const QString &requestIdValue,
        const QString &accessResultId,
        bool success,
        bool submitted,
        QString *error)
{
    if (!initialized_ && !initialize()) {
        if (error) *error = DbStore::lastError();
        return false;
    }
    const QString requestId = requestIdValue.trimmed();
    if (!DbStore::transactionBegin()) {
        if (error) *error = DbStore::lastError();
        return false;
    }
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT local_deducted "
                               "FROM network_remote_call_transaction WHERE request_id=? LIMIT 1"),
                {requestId});
    if (rows.isEmpty()) {
        DbStore::transactionRollback();
        if (error) *error = QStringLiteral("未找到远程呼梯事务");
        return false;
    }
    const QVariantMap row = rows.first();
    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!DbStore::execute(
                QStringLiteral("UPDATE network_remote_call_transaction SET "
                               "access_result_id=?,access_result_success=?,access_result_sent=?,"
                               "local_deducted=?,state=?,updated_at=? WHERE request_id=?"),
                {accessResultId.trimmed(), success ? 1 : 0, submitted ? 1 : 0,
                 submitted ? row.value(QStringLiteral("local_deducted")) : QVariant(0),
                 submitted ? QStringLiteral("ACCESS_REPORTED")
                           : QStringLiteral("ACCESS_REPORT_FAILED"),
                 now, requestId})
            || !DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        if (error) *error = DbStore::lastError();
        return false;
    }
    return true;
}

NetworkRemoteCallDeductApplyResult NetworkPersonnelStore::applyRemoteCallDeductResult(
        const QString &messageId,
        const QString &personId,
        int floor,
        int code,
        bool deducted,
        const QString &message,
        const QVariant &remainingCount,
        const QVariant &usedCount,
        const QVariant &passRemainingCount,
        const QVariant &passUsedCount)
{
    NetworkRemoteCallDeductApplyResult result;
    if (!initialized_ && !initialize()) {
        result.error = DbStore::lastError();
        return result;
    }
    const QString resultId = messageId.trimmed();
    const QString normalizedPersonId = personId.trimmed();
    if (resultId.isEmpty() || normalizedPersonId.isEmpty() || floor == 0) {
        result.error = QStringLiteral("remoteCall.deductResult 参数不完整");
        return result;
    }

    auto parseNonNegativeInteger = [](const QVariant &value, qlonglong *number) {
        if (!value.isValid() || value.isNull() || !number) return false;
        bool ok = false;
        const double raw = value.toDouble(&ok);
        if (!ok || !std::isfinite(raw) || raw < 0.0 || std::floor(raw) != raw) {
            return false;
        }
        *number = static_cast<qlonglong>(raw);
        return true;
    };

    const bool shouldApply = code == 200 && deducted;
    if (shouldApply
            && (!parseNonNegativeInteger(remainingCount, &result.remainingCount)
                || !parseNonNegativeInteger(usedCount, &result.usedCount))) {
        result.error = QStringLiteral("远程呼梯扣次成功结果缺少有效 remainingCount 或 usedCount");
        return result;
    }
    const bool hasPassRemaining = passRemainingCount.isValid() && !passRemainingCount.isNull();
    const bool hasPassUsed = passUsedCount.isValid() && !passUsedCount.isNull();
    if (shouldApply && hasPassRemaining != hasPassUsed) {
        result.error = QStringLiteral("通行次数结果必须同时提供 passRemainingCount 和 passUsedCount");
        return result;
    }
    if (shouldApply && hasPassRemaining
            && (!parseNonNegativeInteger(passRemainingCount, &result.passRemainingCount)
                || !parseNonNegativeInteger(passUsedCount, &result.passUsedCount))) {
        result.error = QStringLiteral("通行次数结果不是有效非负整数");
        return result;
    }

    if (!DbStore::transactionBegin()) {
        result.error = DbStore::lastError();
        return result;
    }
    auto rollback = [&](const QString &why) {
        DbStore::transactionRollback();
        result.error = why.isEmpty() ? DbStore::lastError() : why;
        return result;
    };
    const QList<QVariantMap> duplicateRows = DbStore::query(
                QStringLiteral("SELECT request_id FROM network_remote_call_transaction "
                               "WHERE deduct_result_id=? LIMIT 1"), {resultId});
    if (!duplicateRows.isEmpty()) {
        result.ok = true;
        result.duplicate = true;
        result.requestId = duplicateRows.first().value(
                    QStringLiteral("request_id")).toString();
        DbStore::transactionCommit();
        return result;
    }
    const QList<QVariantMap> txRows = DbStore::query(
                QStringLiteral("SELECT request_id,local_deducted,reserved_remaining_count "
                               "FROM network_remote_call_transaction WHERE person_id=? AND floor=? "
                               "AND state='ACCESS_REPORTED' ORDER BY created_at LIMIT 1"),
                {normalizedPersonId, floor});
    if (txRows.isEmpty()) {
        return rollback(QStringLiteral("未找到匹配的待确认远程呼梯事务"));
    }
    const QVariantMap tx = txRows.first();
    result.requestId = tx.value(QStringLiteral("request_id")).toString();
    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    if (shouldApply) {
        const QList<QVariantMap> ruleRows = DbStore::query(
                    QStringLiteral("SELECT person_id FROM network_access_rule WHERE person_id=? LIMIT 1"),
                    {normalizedPersonId});
        if (ruleRows.isEmpty()) {
            return rollback(QStringLiteral("远程呼梯人员规则不存在，无法同步平台次数"));
        }
        if (!DbStore::execute(
                    QStringLiteral("UPDATE network_access_rule SET "
                                   "remote_call_current_day_count=?,remote_call_used_count=? "
                                   "WHERE person_id=?"),
                    {result.remainingCount, result.usedCount, normalizedPersonId})) {
            return rollback(DbStore::lastError());
        }
        result.remoteCountApplied = true;
        if (hasPassRemaining) {
            if (!DbStore::execute(
                        QStringLiteral("INSERT INTO network_access_usage("
                                       "person_id,remaining_count,access_count,updated_at) VALUES(?,?,?,?) "
                                       "ON CONFLICT(person_id) DO UPDATE SET "
                                       "remaining_count=excluded.remaining_count,"
                                       "access_count=excluded.access_count,updated_at=excluded.updated_at"),
                        {normalizedPersonId, result.passRemainingCount,
                         result.passUsedCount, now})) {
                return rollback(DbStore::lastError());
            }
            result.passCountApplied = true;
        }
    } else {
        result.skipped = code == 200;
    }

    const QString state = shouldApply ? QStringLiteral("DEDUCT_CONFIRMED")
            : (code == 200 ? QStringLiteral("DEDUCT_SKIPPED")
                           : QStringLiteral("DEDUCT_FAILED"));
    if (!DbStore::execute(
                QStringLiteral("UPDATE network_remote_call_transaction SET "
                               "deduct_result_id=?,deduct_code=?,deducted=?,deduct_message=?,"
                               "local_deducted=0,state=?,updated_at=? WHERE request_id=?"),
                {resultId, code, deducted ? 1 : 0, message, state, now, result.requestId})) {
        return rollback(DbStore::lastError());
    }
    if (!DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        result.error = DbStore::lastError();
        return result;
    }
    result.ok = true;
    return result;
}

NetworkAccessDeductApplyResult NetworkPersonnelStore::applyAccessDeductResult(
        const QString &messageId,
        const QString &method,
        const QString &personId,
        int code,
        bool deducted,
        const QString &message,
        const QVariant &remainingCount,
        const QVariant &usedCount)
{
    NetworkAccessDeductApplyResult result;
    if (!initialized_ && !initialize()) {
        result.error = DbStore::lastError();
        return result;
    }

    const QString normalizedId = messageId.trimmed();
    const QString normalizedMethod = method.trimmed();
    const QString normalizedPersonId = personId.trimmed();
    if (normalizedId.isEmpty() || normalizedMethod.isEmpty()
            || normalizedPersonId.isEmpty()) {
        result.error = QStringLiteral("deductResult 缺少 id、method 或 personId");
        return result;
    }

    const QList<QVariantMap> duplicateRows = DbStore::query(
                QStringLiteral("SELECT result_id FROM network_access_deduct_result "
                               "WHERE result_id=? LIMIT 1"),
                {normalizedId});
    if (!duplicateRows.isEmpty()) {
        result.ok = true;
        result.duplicate = true;
        return result;
    }

    const QList<QVariantMap> personRows = DbStore::query(
                QStringLiteral("SELECT person_id FROM network_person "
                               "WHERE person_id=? LIMIT 1"),
                {normalizedPersonId});
    if (personRows.isEmpty()) {
        result.error = QStringLiteral("deductResult 对应人员不存在");
        return result;
    }

    const bool shouldApply = code == 200 && deducted;
    qlonglong serverRemaining = -1;
    qlonglong serverUsed = -1;
    if (shouldApply) {
        bool remainingOk = false;
        bool usedOk = false;
        serverRemaining = remainingCount.toLongLong(&remainingOk);
        serverUsed = usedCount.toLongLong(&usedOk);
        if (!remainingCount.isValid() || remainingCount.isNull()
                || !usedCount.isValid() || usedCount.isNull()
                || !remainingOk || !usedOk
                || serverRemaining < 0 || serverUsed < 0) {
            result.error = QStringLiteral(
                        "扣次成功结果缺少有效 remainingCount 或 usedCount");
            return result;
        }
    }

    if (!DbStore::transactionBegin()) {
        result.error = DbStore::lastError();
        return result;
    }
    auto rollback = [&](const QString &why) {
        DbStore::transactionRollback();
        result.error = why.isEmpty() ? DbStore::lastError() : why;
        return result;
    };

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (shouldApply) {
        const QList<QVariantMap> currentUsage = DbStore::query(
                    QStringLiteral("SELECT remaining_count,access_count "
                                   "FROM network_access_usage WHERE person_id=? LIMIT 1"),
                    {normalizedPersonId});
        const qlonglong currentUsed = currentUsage.isEmpty()
                ? -1 : currentUsage.first().value(QStringLiteral("access_count")).toLongLong();
        if (currentUsed > serverUsed) {
            // 避免较晚到达的旧回执把本地权威计数倒退。
            result.skipped = true;
        } else {
            if (!DbStore::execute(
                        QStringLiteral(
                            "INSERT INTO network_access_usage("
                            "person_id,remaining_count,access_count,updated_at) "
                            "VALUES(?,?,?,?) "
                            "ON CONFLICT(person_id) DO UPDATE SET "
                            "remaining_count=excluded.remaining_count,"
                            "access_count=excluded.access_count,"
                            "updated_at=excluded.updated_at"),
                        {normalizedPersonId, serverRemaining, serverUsed, now})) {
                return rollback(DbStore::lastError());
            }
            result.countApplied = true;
            result.remainingCount = serverRemaining;
            result.usedCount = serverUsed;
        }
    } else if (code == 200) {
        result.skipped = true;
    }

    if (!DbStore::execute(
                QStringLiteral(
                    "INSERT INTO network_access_deduct_result("
                    "result_id,method,person_id,code,deducted,message,"
                    "remaining_count,used_count,processed_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?)"),
                {normalizedId,
                 normalizedMethod,
                 normalizedPersonId,
                 code,
                 deducted ? 1 : 0,
                 message,
                 shouldApply ? QVariant(serverRemaining) : QVariant(),
                 shouldApply ? QVariant(serverUsed) : QVariant(),
                 now})) {
        return rollback(DbStore::lastError());
    }

    if (!DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        result.error = DbStore::lastError();
        return result;
    }
    result.ok = true;
    return result;
}

NetworkQrDeductApplyResult NetworkPersonnelStore::applyQrDeductResult(
        const QString &messageId,
        const QString &qrCode,
        const QString &sourceType,
        const QString &personId,
        int code,
        bool deducted,
        const QString &message,
        const QVariant &remainingCount,
        const QVariant &usedCount)
{
    NetworkQrDeductApplyResult result;
    if (!initialized_ && !initialize()) {
        result.error = DbStore::lastError();
        return result;
    }

    const QString normalizedId = messageId.trimmed();
    const QString normalizedQrCode = qrCode.trimmed();
    const QString normalizedSourceType = sourceType.trimmed().toUpper();
    const QString normalizedPersonId = personId.trimmed();
    if (normalizedId.isEmpty() || normalizedQrCode.isEmpty()) {
        result.error = QStringLiteral("deductResult.id 或 qrCode 为空");
        return result;
    }
    if (code == 200
            && normalizedSourceType != QStringLiteral("CREDENTIAL")
            && normalizedSourceType != QStringLiteral("VISITOR")) {
        result.error = QStringLiteral("成功扣次结果缺少有效sourceType");
        return result;
    }
    if (normalizedSourceType == QStringLiteral("CREDENTIAL")
            && normalizedPersonId.isEmpty()) {
        result.error = QStringLiteral("CREDENTIAL扣次结果缺少personId");
        return result;
    }
    if (normalizedSourceType == QStringLiteral("VISITOR")
            && !normalizedPersonId.isEmpty()) {
        result.error = QStringLiteral("VISITOR扣次结果不应携带personId");
        return result;
    }
    result.sourceType = normalizedSourceType;

    const QList<QVariantMap> duplicateRows = DbStore::query(
                QStringLiteral("SELECT scan_id,source_type FROM network_qr_transaction "
                               "WHERE deduct_result_id=? LIMIT 1"),
                {normalizedId});
    if (!duplicateRows.isEmpty()) {
        result.ok = true;
        result.duplicate = true;
        result.scanId = duplicateRows.first()
                .value(QStringLiteral("scan_id")).toString();
        result.sourceType = duplicateRows.first()
                .value(QStringLiteral("source_type")).toString();
        return result;
    }

    const QList<QVariantMap> transactions = DbStore::query(
                QStringLiteral(
                    "SELECT scan_id,person_id,source_type,rs485_success "
                    "FROM network_qr_transaction "
                    "WHERE qr_unique=? AND (deduct_result_id IS NULL OR deduct_result_id='') "
                    "ORDER BY created_at DESC"),
                {normalizedQrCode});
    QVariantMap transaction;
    for (const QVariantMap &candidate : transactions) {
        const QString candidateSource = candidate.value(QStringLiteral("source_type"))
                .toString().trimmed().toUpper();
        const QString candidatePerson = candidate.value(QStringLiteral("person_id"))
                .toString().trimmed();
        if (!normalizedSourceType.isEmpty()
                && candidateSource != normalizedSourceType) {
            continue;
        }
        if (normalizedSourceType == QStringLiteral("CREDENTIAL")
                && candidatePerson != normalizedPersonId) {
            continue;
        }
        if (normalizedSourceType == QStringLiteral("VISITOR")
                && !candidatePerson.isEmpty()) {
            continue;
        }
        transaction = candidate;
        break;
    }
    if (transaction.isEmpty()) {
        result.error = QStringLiteral(
                    "未找到与qrCode/sourceType/personId匹配的待扣次事务");
        return result;
    }

    result.scanId = transaction.value(QStringLiteral("scan_id")).toString();
    const QString transactionPersonId = transaction.value(QStringLiteral("person_id"))
            .toString().trimmed();
    const QString transactionSourceType = transaction.value(QStringLiteral("source_type"))
            .toString().trimmed().toUpper();
    result.sourceType = transactionSourceType;
    const bool rs485Success = transaction.value(QStringLiteral("rs485_success")).toInt() == 1;

    bool countEnabled = false;
    if (transactionSourceType == QStringLiteral("CREDENTIAL")) {
        const QList<QVariantMap> rules = DbStore::query(
                    QStringLiteral("SELECT count_enabled FROM network_access_rule "
                                   "WHERE person_id=? LIMIT 1"),
                    {transactionPersonId});
        if (rules.isEmpty()) {
            result.error = QStringLiteral("未找到CREDENTIAL对应的人员规则");
            return result;
        }
        countEnabled = rules.first().value(QStringLiteral("count_enabled")).toInt() == 1;
        if (countEnabled) {
            const QList<QVariantMap> usageRows = DbStore::query(
                        QStringLiteral("SELECT person_id FROM network_access_usage "
                                       "WHERE person_id=? LIMIT 1"),
                        {transactionPersonId});
            if (usageRows.isEmpty()) {
                result.error = QStringLiteral("人员次数规则已启用但本地次数记录不存在");
                return result;
            }
        }
    }

    const bool credentialCountApplied = code == 200 && deducted && rs485Success
            && transactionSourceType == QStringLiteral("CREDENTIAL") && countEnabled;
    const bool visitorCountApplied = code == 200 && deducted && rs485Success
            && transactionSourceType == QStringLiteral("VISITOR");

    bool remainingOk = false;
    bool usedOk = false;
    const double visitorRemaining = remainingCount.toDouble(&remainingOk);
    const double visitorUsed = usedCount.toDouble(&usedOk);
    if (transactionSourceType == QStringLiteral("VISITOR")
            && code == 200 && (visitorCountApplied || (!deducted && rs485Success))
            && (!remainingCount.isValid() || remainingCount.isNull()
                || !usedCount.isValid() || usedCount.isNull()
                || !remainingOk || !usedOk
                || visitorRemaining < 0.0 || visitorUsed < 0.0)) {
        result.error = QStringLiteral("VISITOR扣次结果缺少有效remainingCount或usedCount");
        return result;
    }

    if (!DbStore::transactionBegin()) {
        result.error = DbStore::lastError();
        return result;
    }
    auto rollback = [&](const QString &why) {
        DbStore::transactionRollback();
        result.error = why.isEmpty() ? DbStore::lastError() : why;
        return result;
    };

    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QString state = QStringLiteral("DEDUCT_REJECTED");
    if (code == 200) {
        if (!rs485Success) {
            state = QStringLiteral("DEDUCT_IGNORED");
            result.error = QStringLiteral("收到平台扣次结果，但本地RS485未成功");
        } else if (!deducted
                || (transactionSourceType == QStringLiteral("CREDENTIAL")
                    && !countEnabled)) {
            state = QStringLiteral("DEDUCT_SKIPPED");
            result.skipped = true;
        } else {
            state = QStringLiteral("DEDUCTED");
        }
    }
    if (!DbStore::execute(
                QStringLiteral(
                    "UPDATE network_qr_transaction SET deduct_result_id=?,deduct_code=?,"
                    "deducted=?,deduct_message=?,state=?,updated_at=? WHERE scan_id=?"),
                {normalizedId,
                 code,
                 deducted ? 1 : 0,
                 message,
                 state,
                 now,
                 result.scanId})) {
        return rollback(DbStore::lastError());
    }

    if (credentialCountApplied) {
        if (!DbStore::execute(
                    QStringLiteral(
                        "UPDATE network_access_usage SET "
                        "remaining_count=MAX(COALESCE(remaining_count,0)-1,0),"
                        "access_count=access_count+1,updated_at=? WHERE person_id=?"),
                    {now, transactionPersonId})) {
            return rollback(DbStore::lastError());
        }
        if (!DbStore::execute(
                    QStringLiteral(
                        "UPDATE network_qr_access_cache SET "
                        "deduct_count=deduct_count+1,last_deduct_at=?,updated_at=? "
                        "WHERE qr_unique=?"),
                    {now, now, normalizedQrCode})) {
            return rollback(DbStore::lastError());
        }
        result.countApplied = true;
    } else if (visitorCountApplied) {
        if (!DbStore::execute(
                    QStringLiteral(
                        "INSERT INTO network_qr_visitor_usage("
                        "qr_unique,remaining_count,used_count,last_result_id,updated_at"
                        ") VALUES(?,?,?,?,?) "
                        "ON CONFLICT(qr_unique) DO UPDATE SET "
                        "remaining_count=excluded.remaining_count,"
                        "used_count=excluded.used_count,"
                        "last_result_id=excluded.last_result_id,"
                        "updated_at=excluded.updated_at"),
                    {normalizedQrCode,
                     visitorRemaining,
                     visitorUsed,
                     normalizedId,
                     now})) {
            return rollback(DbStore::lastError());
        }
        result.countApplied = true;
    } else if (transactionSourceType == QStringLiteral("VISITOR")
               && code == 200 && !deducted && rs485Success) {
        // 未实际扣减时仍保存平台返回的访客次数快照，不把它视为失败。
        if (!DbStore::execute(
                    QStringLiteral(
                        "INSERT INTO network_qr_visitor_usage("
                        "qr_unique,remaining_count,used_count,last_result_id,updated_at"
                        ") VALUES(?,?,?,?,?) "
                        "ON CONFLICT(qr_unique) DO UPDATE SET "
                        "remaining_count=excluded.remaining_count,"
                        "used_count=excluded.used_count,"
                        "last_result_id=excluded.last_result_id,"
                        "updated_at=excluded.updated_at"),
                    {normalizedQrCode,
                     visitorRemaining,
                     visitorUsed,
                     normalizedId,
                     now})) {
            return rollback(DbStore::lastError());
        }
    } else if (code == 200 && deducted) {
        if (transactionSourceType == QStringLiteral("CREDENTIAL")
                   && !countEnabled) {
            result.skipped = true;
        }
    }

    if (!DbStore::transactionCommit()) {
        return rollback(DbStore::lastError());
    }
    result.ok = true;
    return result;
}

bool NetworkPersonnelStore::finishQrAccessTransaction(const QString &scanId,
                                                       const QString &state,
                                                       const QString &reason,
                                                       QString *error)
{
    if (error) error->clear();
    const bool ok = DbStore::execute(
                QStringLiteral(
                    "UPDATE network_qr_transaction SET state=?,scan_reason=?,updated_at=? "
                    "WHERE scan_id=?"),
                {state.trimmed(),
                 reason,
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                 scanId.trimmed()});
    if (!ok && error) *error = DbStore::lastError();
    return ok;
}

NetworkPersonnelSyncResult NetworkPersonnelStore::checkAllPersonHashes(
        const QJsonObject &envelope)
{
    if (!initialize()) {
        return makeResult(false, QStringLiteral("DATABASE_INIT_FAILED"), DbStore::lastError());
    }
    const QString hash = payloadHash(envelope);
    NetworkPersonnelSyncResult cached;
    if (loadCachedResult(envelope, hash, &cached)) {
        return cached;
    }

    const QJsonValue expectedValue = envelope.value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("personIdHashes"));
    const QJsonObject expected = expectedValue.toObject();
    if (requestId(envelope).isEmpty() || !expectedValue.isObject()) {
        return makeResult(false,
                          QStringLiteral("INVALID_PAYLOAD"),
                          QStringLiteral("id 为空或 data.personIdHashes 不是对象"));
    }

    QHash<QString, QString> local;
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT person_id,person_hash FROM network_person "
                               "WHERE active=1 AND deleted=0"));
    for (const QVariantMap &row : rows) {
        local.insert(row.value(QStringLiteral("person_id")).toString(),
                     row.value(QStringLiteral("person_hash")).toString());
    }

    QJsonArray missing;
    QJsonArray changed;
    QJsonArray matched;
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        if (!local.contains(it.key())) {
            missing.append(it.key());
        } else if (local.value(it.key()) != it.value().toString()) {
            changed.append(it.key());
        } else {
            matched.append(it.key());
        }
    }
    QJsonArray extra;
    for (auto it = local.begin(); it != local.end(); ++it) {
        if (!expected.contains(it.key())) {
            extra.append(it.key());
        }
    }

    NetworkPersonnelSyncResult result =
            makeResult(true,
                       QStringLiteral("HASH_CHECKED"),
                       QStringLiteral("人员哈希清单比对完成"));
    result.data.insert(QStringLiteral("missing"), missing);
    result.data.insert(QStringLiteral("changed"), changed);
    result.data.insert(QStringLiteral("matched"), matched);
    result.data.insert(QStringLiteral("extra"), extra);
    result.data.insert(QStringLiteral("inferredDeletedCount"), extra.size());

    if (!DbStore::transactionBegin()) {
        return makeResult(false,
                          QStringLiteral("TRANSACTION_FAILED"),
                          DbStore::lastError());
    }

    const QString deletedAt =
            QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    for (const QJsonValue &personIdValue : extra) {
        const QString personId = personIdValue.toString().trimmed();
        if (personId.isEmpty()) {
            continue;
        }
        if (!DbStore::execute(
                    QStringLiteral(
                        "UPDATE network_person SET active=0,deleted=1,"
                        "sync_state='DELETED',updated_at=? "
                        "WHERE person_id=? AND active=1 AND deleted=0"),
                    {deletedAt, personId})) {
            DbStore::transactionRollback();
            return makeResult(false,
                              QStringLiteral("INFERRED_DELETE_FAILED"),
                              DbStore::lastError(),
                              personId);
        }
    }

    if (!recordResult(envelope, QString(), hash, result)
            || !DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        return makeResult(false, QStringLiteral("DATABASE_WRITE_FAILED"), DbStore::lastError());
    }
    return result;
}

NetworkPersonnelSyncResult NetworkPersonnelStore::deletePersonnel(
        const QJsonObject &envelope)
{
    if (!initialize()) {
        return makeResult(false, QStringLiteral("DATABASE_INIT_FAILED"), DbStore::lastError());
    }
    const QString hash = payloadHash(envelope);
    NetworkPersonnelSyncResult cached;
    if (loadCachedResult(envelope, hash, &cached)) {
        return cached;
    }

    const QJsonObject data = envelope.value(QStringLiteral("data")).toObject();
    const QString personId = data.value(QStringLiteral("personId")).toString().trimmed();
    const QString deleteType = data.value(QStringLiteral("deleteType")).toString().trimmed().toUpper();
    const QString personHash = data.value(QStringLiteral("personHash")).toString().trimmed();
    const QString storedPersonHash = personHash.isEmpty()
            ? QStringLiteral("")
            : personHash;
    const QJsonArray items = data.value(QStringLiteral("items")).toArray();
    if (requestId(envelope).isEmpty() || personId.isEmpty() || deleteType.isEmpty()) {
        return makeResult(false,
                          QStringLiteral("INVALID_PAYLOAD"),
                          QStringLiteral("id、personId 或 deleteType 为空"),
                          personId);
    }
    if (deleteType != QStringLiteral("ALL")
            && deleteType != QStringLiteral("PERSON")
            && deleteType != QStringLiteral("IC_CARD")
            && deleteType != QStringLiteral("FACE")
            && deleteType != QStringLiteral("QR_CODE")) {
        return makeResult(false,
                          QStringLiteral("UNSUPPORTED_DELETE_TYPE"),
                          QStringLiteral("不支持的 deleteType: %1").arg(deleteType),
                          personId);
    }
    const bool deleteWholePerson = deleteType == QStringLiteral("ALL")
            || deleteType == QStringLiteral("PERSON");
    if (!deleteWholePerson && items.isEmpty()) {
        return makeResult(false,
                          QStringLiteral("INVALID_ITEMS"),
                          QStringLiteral("凭证删除消息的 data.items 为空"),
                          personId);
    }

    if (!DbStore::transactionBegin()) {
        return makeResult(false, QStringLiteral("TRANSACTION_FAILED"), DbStore::lastError(), personId);
    }
    auto rollbackError = [&](const QString &code) {
        const QString error = DbStore::lastError();
        DbStore::transactionRollback();
        return makeResult(false, code, error, personId);
    };

    int removed = 0;
    if (deleteWholePerson) {
        const QStringList childDeletes = {
            QStringLiteral("DELETE FROM network_face WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_ic_card WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_qr_access_cache WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_qr_code WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_timing_rule WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_access_rule WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_access_usage WHERE person_id=?"),
            QStringLiteral("DELETE FROM network_floor WHERE person_id=?")
        };
        for (const QString &sql : childDeletes) {
            if (!DbStore::execute(sql, {personId})) {
                return rollbackError(QStringLiteral("PERSON_DELETE_FAILED"));
            }
        }
        if (!DbStore::execute(
                    QStringLiteral("UPDATE network_person SET active=0,deleted=1,"
                                   "person_hash='',sync_state='DELETED',updated_at=? "
                                   "WHERE person_id=?"),
                    {QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                     personId})) {
            return rollbackError(QStringLiteral("PERSON_DELETE_FAILED"));
        }
        removed = 1;
    } else {
        QString table;
        QString key;
        if (deleteType == QStringLiteral("IC_CARD")) {
            table = QStringLiteral("network_ic_card");
            key = QStringLiteral("card_id");
        } else if (deleteType == QStringLiteral("FACE")) {
            table = QStringLiteral("network_face");
            key = QStringLiteral("face_hash");
        } else {
            table = QStringLiteral("network_qr_code");
            key = QStringLiteral("qr_unique");
        }

        const QString sql = QStringLiteral("DELETE FROM %1 WHERE person_id=? AND %2=?")
                .arg(table, key);
        for (const QJsonValue &item : items) {
            const QString value = item.toString().trimmed();
            if (value.isEmpty()) {
                continue;
            }
            const QStringList deleteKeys = deleteType == QStringLiteral("FACE")
                    ? faceDeleteKeys(value)
                    : QStringList{value};
            for (const QString &deleteKey : deleteKeys) {
                if (!DbStore::execute(sql, {personId, deleteKey})) {
                    return rollbackError(QStringLiteral("ITEM_DELETE_FAILED"));
                }
                if (deleteType == QStringLiteral("QR_CODE")
                        && !DbStore::execute(
                            QStringLiteral(
                                "DELETE FROM network_qr_access_cache "
                                "WHERE person_id=? AND qr_unique=?"),
                            {personId, deleteKey})) {
                    return rollbackError(QStringLiteral("ITEM_DELETE_FAILED"));
                }
            }
            ++removed;
        }

        // 新协议会在部分删除后携带新的聚合 personHash，直接保存；
        // 兼容旧消息：未携带时清空，等待 checkAll/fullPersonnel 重新确立。
        if (!DbStore::execute(
                    QStringLiteral("UPDATE network_person "
                                   "SET person_hash=COALESCE(?,''),updated_at=? "
                                   "WHERE person_id=?"),
                    {storedPersonHash,
                     QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                     personId})) {
            return rollbackError(QStringLiteral("PERSON_HASH_CLEAR_FAILED"));
        }
    }

    const QString serverTime = envelope.value(QStringLiteral("time")).toString();
    if (!DbStore::execute(
                QStringLiteral(
                    "INSERT INTO network_person_tombstone("
                    "person_id,delete_type,server_time,message_id,deleted_at"
                    ") VALUES(?,?,?,?,?) "
                    "ON CONFLICT(person_id) DO UPDATE SET "
                    "delete_type=excluded.delete_type,server_time=excluded.server_time,"
                    "message_id=excluded.message_id,deleted_at=excluded.deleted_at"),
                {personId,
                 deleteType,
                 serverTime,
                 requestId(envelope),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))})) {
        return rollbackError(QStringLiteral("TOMBSTONE_WRITE_FAILED"));
    }

    NetworkPersonnelSyncResult result =
            makeResult(true,
                       QStringLiteral("DELETED"),
                       QStringLiteral("删除消息已处理"),
                       personId);
    result.data.insert(QStringLiteral("deleteType"), deleteType);
    result.data.insert(QStringLiteral("processedItemCount"), removed);
    if (!personHash.isEmpty() && !deleteWholePerson) {
        result.data.insert(QStringLiteral("personHash"), personHash);
    }
    if (!recordResult(envelope, personId, hash, result)
            || !DbStore::transactionCommit()) {
        DbStore::transactionRollback();
        return makeResult(false, QStringLiteral("DATABASE_COMMIT_FAILED"), DbStore::lastError(), personId);
    }
    return result;
}
