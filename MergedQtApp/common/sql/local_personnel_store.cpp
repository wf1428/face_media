#include "local_personnel_store.h"

#include "common/sql/dbstore.h"
#include "common/storage_policy.h"
#include "platform/rk3566_platform.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QVariantMap>
#include <algorithm>

namespace {

QString compactJson(const QJsonValue &value)
{
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject())
                                 .toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray())
                                 .toJson(QJsonDocument::Compact));
    }
    return QString();
}

QJsonObject parsedObject(const QString &text)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
            ? document.object() : QJsonObject();
}

bool tableHasColumn(const QString &table, const QString &column)
{
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("PRAGMA table_info(%1)").arg(table));
    for (const QVariantMap &row : rows) {
        if (row.value(QStringLiteral("name")).toString() == column) return true;
    }
    return false;
}

bool tableExists(const QString &table)
{
    return !DbStore::query(
                QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name=? LIMIT 1"),
                {table}).isEmpty();
}

bool ensureTableColumn(const QString &table, const QString &name,
                       const QString &definition)
{
    return tableHasColumn(table, name)
            || DbStore::exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                             .arg(table, name, definition));
}

bool ensureColumn(const QString &name, const QString &definition)
{
    return ensureTableColumn(QStringLiteral("person"), name, definition);
}

QString safeFileStem(const QString &value)
{
    QString result;
    for (const QChar ch : value) {
        result.append(ch.isLetterOrNumber() || ch == QLatin1Char('_')
                      || ch == QLatin1Char('-') ? ch : QLatin1Char('_'));
    }
    return result.isEmpty() ? QStringLiteral("person") : result;
}

/** @return 与刷卡鉴权一致的原值、十进制及四字节大小端卡号候选。 */
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
            if (bytes.size() == 4) {
                std::reverse(bytes.begin(), bytes.end());
                const QString reversedHex = QString::fromLatin1(bytes.toHex()).toUpper();
                if (!values.contains(reversedHex)) values.append(reversedHex);
                const qulonglong reversedValue = reversedHex.toULongLong(&ok, 16);
                if (ok) {
                    const QString reversedDecimal = QString::number(reversedValue);
                    if (!values.contains(reversedDecimal)) values.append(reversedDecimal);
                }
            }
        }
    }

    const qulonglong decimalValue = raw.toULongLong(&ok, 10);
    if (ok && decimalValue <= 0xffffffffULL) {
        const QString hex = QStringLiteral("%1")
                .arg(decimalValue, 8, 16, QLatin1Char('0')).toUpper();
        if (!values.contains(hex)) values.append(hex);
        QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
        if (bytes.size() == 4) {
            std::reverse(bytes.begin(), bytes.end());
            const QString reversedHex = QString::fromLatin1(bytes.toHex()).toUpper();
            if (!values.contains(reversedHex)) values.append(reversedHex);
            const qulonglong reversedValue = reversedHex.toULongLong(&ok, 16);
            const QString reversedDecimal = QString::number(reversedValue);
            if (ok && !values.contains(reversedDecimal)) values.append(reversedDecimal);
        }
    }
    return values;
}

struct StoredCardOwner
{
    QString key;
    QString name;
};

QList<StoredCardOwner> conflictingCardOwners(const QString &cardId,
                                              qint64 excludedLocalPersonId = 0,
                                              const QString &excludedNetworkPersonId = QString())
{
    QList<StoredCardOwner> owners;
    const QStringList candidates = cardCandidates(cardId);
    if (candidates.isEmpty()) return owners;

    QStringList marks;
    QList<QVariant> cardBinds;
    for (const QString &candidate : candidates) {
        marks.append(QStringLiteral("?"));
        cardBinds.append(candidate);
    }
    const QString inClause = marks.join(QLatin1Char(','));

    if (tableExists(QStringLiteral("network_ic_card"))
            && tableExists(QStringLiteral("network_person"))) {
        QList<QVariant> binds = cardBinds;
        QString exclusion;
        if (!excludedNetworkPersonId.isEmpty()) {
            exclusion = QStringLiteral(" AND c.person_id<>?");
            binds.append(excludedNetworkPersonId);
        }
        const QList<QVariantMap> rows = DbStore::query(
                    QStringLiteral("SELECT DISTINCT c.person_id,p.name FROM network_ic_card c "
                                   "JOIN network_person p ON p.person_id=c.person_id "
                                   "WHERE UPPER(c.card_id) IN (%1)%2 ORDER BY c.person_id")
                    .arg(inClause, exclusion), binds);
        for (const QVariantMap &row : rows) {
            StoredCardOwner owner;
            owner.key = QStringLiteral("network:")
                    + row.value(QStringLiteral("person_id")).toString();
            owner.name = row.value(QStringLiteral("name")).toString().trimmed();
            if (owner.name.isEmpty()) {
                owner.name = row.value(QStringLiteral("person_id")).toString();
            }
            owners.append(owner);
        }
    }

    if (tableExists(QStringLiteral("local_ic_card"))
            && tableExists(QStringLiteral("person"))) {
        QList<QVariant> binds = cardBinds;
        QString exclusion;
        if (excludedLocalPersonId > 0) {
            exclusion = QStringLiteral(" AND c.person_id<>?");
            binds.append(excludedLocalPersonId);
        }
        const QList<QVariantMap> rows = DbStore::query(
                    QStringLiteral("SELECT DISTINCT c.person_id,p.name FROM local_ic_card c "
                                   "JOIN person p ON p.id=c.person_id "
                                   "WHERE UPPER(c.card_id) IN (%1)%2 ORDER BY c.person_id")
                    .arg(inClause, exclusion), binds);
        for (const QVariantMap &row : rows) {
            StoredCardOwner owner;
            owner.key = QStringLiteral("local:")
                    + row.value(QStringLiteral("person_id")).toString();
            owner.name = row.value(QStringLiteral("name")).toString().trimmed();
            if (owner.name.isEmpty()) {
                owner.name = row.value(QStringLiteral("person_id")).toString();
            }
            owners.append(owner);
        }
    }
    return owners;
}

/** @return 已启用相同通行密码的网络及其他本地人员。 */
QList<StoredCardOwner> conflictingPasswordOwners(const QString &password,
                                                  qint64 excludedLocalPersonId = 0)
{
    QList<StoredCardOwner> owners;
    const QString value = password.trimmed();
    if (value.isEmpty()) return owners;

    if (tableExists(QStringLiteral("network_access_rule"))
            && tableExists(QStringLiteral("network_person"))) {
        const QList<QVariantMap> rows = DbStore::query(
                    QStringLiteral("SELECT DISTINCT r.person_id,p.name "
                                   "FROM network_access_rule r "
                                   "JOIN network_person p ON p.person_id=r.person_id "
                                   "WHERE r.card_password_enabled=1 AND r.card_password=? "
                                   "AND p.active=1 AND p.deleted=0 ORDER BY r.person_id"),
                    {value});
        for (const QVariantMap &row : rows) {
            StoredCardOwner owner;
            owner.key = QStringLiteral("network:")
                    + row.value(QStringLiteral("person_id")).toString();
            owner.name = row.value(QStringLiteral("name")).toString().trimmed();
            if (owner.name.isEmpty()) {
                owner.name = row.value(QStringLiteral("person_id")).toString();
            }
            owners.append(owner);
        }
    }

    if (tableExists(QStringLiteral("local_access_rule"))
            && tableExists(QStringLiteral("person"))) {
        QList<QVariant> binds{value};
        QString exclusion;
        if (excludedLocalPersonId > 0) {
            exclusion = QStringLiteral(" AND r.person_id<>?");
            binds.append(excludedLocalPersonId);
        }
        const QList<QVariantMap> rows = DbStore::query(
                    QStringLiteral("SELECT DISTINCT r.person_id,p.name "
                                   "FROM local_access_rule r "
                                   "JOIN person p ON p.id=r.person_id "
                                   "WHERE r.card_password_enabled=1 AND r.card_password=? "
                                   "AND p.deleted=0%1 ORDER BY r.person_id")
                    .arg(exclusion), binds);
        for (const QVariantMap &row : rows) {
            StoredCardOwner owner;
            owner.key = QStringLiteral("local:")
                    + row.value(QStringLiteral("person_id")).toString();
            owner.name = row.value(QStringLiteral("name")).toString().trimmed();
            if (owner.name.isEmpty()) {
                owner.name = row.value(QStringLiteral("person_id")).toString();
            }
            owners.append(owner);
        }
    }
    return owners;
}

} // namespace

bool LocalPersonnelStore::initialize()
{
    if (!DbStore::bootstrap(Rk3566Platform::databasePath())) return false;

    const QStringList schema = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS person ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,person_no TEXT NOT NULL UNIQUE,"
            "name TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 1,"
            "deleted INTEGER NOT NULL DEFAULT 0,person_source TEXT NOT NULL DEFAULT 'manual',"
            "created_at TEXT NOT NULL,updated_at TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS face_feature ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,person_id INTEGER NOT NULL,image_path TEXT NOT NULL,"
            "feature_blob BLOB NOT NULL,feature_size INTEGER NOT NULL,model_version TEXT,quality REAL,"
            "created_at TEXT NOT NULL,FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_ic_card ("
            "card_id TEXT PRIMARY KEY,person_id INTEGER NOT NULL,name TEXT,bind_time TEXT,"
            "active INTEGER NOT NULL DEFAULT 1,"
            "FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_qr_code ("
            "qr_unique TEXT PRIMARY KEY,person_id INTEGER NOT NULL,qr_name TEXT,qr_type TEXT,"
            "purpose TEXT,valid_from TEXT,valid_to TEXT,max_use_count INTEGER,"
            "used_count INTEGER NOT NULL DEFAULT 0,bind_time TEXT,active INTEGER NOT NULL DEFAULT 1,"
            "FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_access_rule ("
            "person_id INTEGER PRIMARY KEY,time_enabled INTEGER NOT NULL DEFAULT 0,"
            "count_enabled INTEGER NOT NULL DEFAULT 0,relay_enabled INTEGER NOT NULL DEFAULT 0,"
            "amount_enabled INTEGER NOT NULL DEFAULT 0,duration_enabled INTEGER NOT NULL DEFAULT 0,"
            "remote_call_enabled INTEGER NOT NULL DEFAULT 0,card_password_enabled INTEGER NOT NULL DEFAULT 0,"
            "card_password TEXT,control_elevator INTEGER NOT NULL DEFAULT 0,"
            "remote_call_current_day_count INTEGER,remote_call_every_day_count INTEGER,"
            "remote_call_used_count INTEGER NOT NULL DEFAULT 0,remote_call_refresh_date TEXT,"
            "count_total INTEGER,count_add_count INTEGER,amount_total REAL,amount_add_amount REAL,"
            "amount_unit_price REAL,duration_start TEXT,duration_end TEXT,elevator_value_type INTEGER,"
            "raw_json TEXT,FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_timing_rule ("
            "person_id INTEGER NOT NULL,rule_order INTEGER NOT NULL,days_json TEXT,start_time TEXT,end_time TEXT,"
            "PRIMARY KEY(person_id,rule_order),"
            "FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_floor ("
            "person_id INTEGER NOT NULL,device_id TEXT NOT NULL,floor_no INTEGER NOT NULL,"
            "PRIMARY KEY(person_id,device_id,floor_no),"
            "FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_access_usage ("
            "person_id INTEGER PRIMARY KEY,remaining_count INTEGER,access_count INTEGER NOT NULL DEFAULT 0,"
            "remaining_amount REAL,used_amount REAL NOT NULL DEFAULT 0,updated_at TEXT NOT NULL,"
            "FOREIGN KEY(person_id) REFERENCES person(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_ic_person ON local_ic_card(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_qr_person ON local_qr_code(person_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_floor_person ON local_floor(person_id)")
    };
    for (const QString &sql : schema) {
        if (!DbStore::exec(sql)) return false;
    }

    const QList<QPair<QString, QString>> columns = {
        {QStringLiteral("employee_no"), QStringLiteral("TEXT")},
        {QStringLiteral("department"), QStringLiteral("TEXT")},
        {QStringLiteral("position"), QStringLiteral("TEXT")},
        {QStringLiteral("phone"), QStringLiteral("TEXT")},
        {QStringLiteral("id_card"), QStringLiteral("TEXT")},
        {QStringLiteral("hire_date_ms"), QStringLiteral("INTEGER")},
        {QStringLiteral("person_type"), QStringLiteral("TEXT")},
        {QStringLiteral("account"), QStringLiteral("TEXT")},
        {QStringLiteral("initial_password"), QStringLiteral("TEXT")},
        {QStringLiteral("group_name"), QStringLiteral("TEXT")},
        {QStringLiteral("has_face"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("access_template_name"), QStringLiteral("TEXT")},
        {QStringLiteral("rule_template_name"), QStringLiteral("TEXT")},
        {QStringLiteral("enabled_features_text"), QStringLiteral("TEXT")},
        {QStringLiteral("control_type_text"), QStringLiteral("TEXT")},
        {QStringLiteral("relay_text"), QStringLiteral("TEXT")},
        {QStringLiteral("remote_call_text"), QStringLiteral("TEXT")},
        {QStringLiteral("face_file_list"), QStringLiteral("TEXT")},
        {QStringLiteral("person_json"), QStringLiteral("TEXT")},
        {QStringLiteral("person_source"), QStringLiteral("TEXT NOT NULL DEFAULT 'manual'")}
    };
    for (const auto &column : columns) {
        if (!ensureColumn(column.first, column.second)) return false;
    }
    // 兼容升级前已经由Excel导入的本地人员。手工录入人员没有person_json，
    // 因而不会被误标；后续导入会显式写入excel来源。
    if (!DbStore::execute(
                QStringLiteral("UPDATE person SET person_source='excel' "
                               "WHERE COALESCE(TRIM(person_json),'')<>'' "
                               "AND COALESCE(TRIM(person_source),'manual')<>'excel'"))) {
        return false;
    }
    const QList<QPair<QString, QString>> localRuleColumns = {
        {QStringLiteral("remote_call_current_day_count"), QStringLiteral("INTEGER")},
        {QStringLiteral("remote_call_every_day_count"), QStringLiteral("INTEGER")},
        {QStringLiteral("remote_call_used_count"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("remote_call_refresh_date"), QStringLiteral("TEXT")}
    };
    for (const auto &column : localRuleColumns) {
        if (!ensureTableColumn(QStringLiteral("local_access_rule"),
                               column.first, column.second)) return false;
    }
    return true;
}

LocalPersonnelImportResult LocalPersonnelStore::importSpreadsheetPersonnel(
        const QVector<QJsonObject> &records)
{
    LocalPersonnelImportResult summary;
    if (!initialize()) {
        summary.error = DbStore::lastError();
        return summary;
    }

    const QList<QVariantMap> networkRows = tableExists(QStringLiteral("network_person"))
            ? DbStore::query(QStringLiteral("SELECT person_id,name,phone,person_json FROM network_person "
                                            "WHERE active=1 AND deleted=0"))
            : QList<QVariantMap>();
    const QString now = QDateTime::currentDateTime().toString(
                QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    for (const QJsonObject &record : records) {
        const QJsonObject person = record.value(QStringLiteral("person")).toObject();
        const QString name = person.value(QStringLiteral("name")).toString().trimmed();
        const QString account = person.value(QStringLiteral("account")).toString().trimmed();
        const QString phone = person.value(QStringLiteral("phone")).toString().trimmed();
        const QJsonArray cards = record.value(QStringLiteral("icCards")).toArray();
        const QJsonObject rules = record.value(QStringLiteral("rules")).toObject();
        if (name.isEmpty() || account.isEmpty()) {
            ++summary.failed;
            summary.failedNames.append((name.isEmpty() ? QStringLiteral("未命名人员") : name)
                                       + QStringLiteral("（资料不完整）"));
            continue;
        }

        const QList<QVariantMap> existing = DbStore::query(
                    QStringLiteral("SELECT id FROM person WHERE "
                                   "LOWER(COALESCE(NULLIF(account,''),person_no))=LOWER(?) "
                                   "OR person_no=? ORDER BY id LIMIT 1"),
                    {account, account});
        const bool exists = !existing.isEmpty();
        qint64 personId = exists
                ? existing.first().value(QStringLiteral("id")).toLongLong() : 0;

        QStringList ownerKeys;
        QStringList ownerNames;
        for (const QJsonValue &value : cards) {
            const QString cardId = value.toObject().value(
                        QStringLiteral("cardId")).toString().trimmed();
            if (cardId.isEmpty()) continue;
            const QList<StoredCardOwner> owners = conflictingCardOwners(cardId, personId);
            for (const StoredCardOwner &owner : owners) {
                if (!ownerKeys.contains(owner.key)) {
                    ownerKeys.append(owner.key);
                    ownerNames.append(owner.name);
                }
            }
        }
        if (!ownerNames.isEmpty()) {
            QStringList names;
            names.append(name + QStringLiteral("人员"));
            for (const QString &ownerName : ownerNames) {
                names.append(ownerName + QStringLiteral("人员"));
            }
            const QString reason = QStringLiteral("卡号冲突：%1")
                    .arg(names.join(QStringLiteral("和")));
            ++summary.failed;
            summary.failedNames.append(reason);
            if (summary.error.isEmpty()) summary.error = reason;
            continue;
        }

        if (rules.value(QStringLiteral("cardPassword")).toBool()) {
            const QString password = rules.value(
                        QStringLiteral("cardPasswordDataPassword")).toString().trimmed();
            const QList<StoredCardOwner> passwordOwners =
                    conflictingPasswordOwners(password, personId);
            if (!passwordOwners.isEmpty()) {
                QStringList names;
                names.append(name + QStringLiteral("人员"));
                QStringList ownerKeys;
                for (const StoredCardOwner &owner : passwordOwners) {
                    if (ownerKeys.contains(owner.key)) continue;
                    ownerKeys.append(owner.key);
                    names.append(owner.name + QStringLiteral("人员"));
                }
                const QString reason = QStringLiteral("密码冲突：%1")
                        .arg(names.join(QStringLiteral("和")));
                ++summary.failed;
                summary.failedNames.append(reason);
                if (summary.error.isEmpty()) summary.error = reason;
                continue;
            }
        }

        bool networkDuplicate = false;
        for (const QVariantMap &row : networkRows) {
            const QJsonObject stored = parsedObject(
                        row.value(QStringLiteral("person_json")).toString());
            QString storedAccount = stored.value(
                        QStringLiteral("account")).toString().trimmed();
            if (storedAccount.isEmpty()) {
                storedAccount = row.value(QStringLiteral("person_id")).toString().trimmed();
            }
            if (storedAccount.compare(account, Qt::CaseInsensitive) == 0
                    && row.value(QStringLiteral("name")).toString().trimmed() == name
                    && row.value(QStringLiteral("phone")).toString().trimmed() == phone) {
                networkDuplicate = true;
                break;
            }
        }
        if (networkDuplicate) {
            ++summary.failed;
            summary.failedNames.append(name + QStringLiteral("（重复人员）"));
            continue;
        }

        if (!DbStore::transactionBegin()) {
            ++summary.failed;
            summary.failedNames.append(name + QStringLiteral("（保存失败）"));
            summary.error = DbStore::lastError();
            continue;
        }
        bool writeOk = false;
        const QVariant hireDate = person.value(QStringLiteral("hireDate")).toVariant();
        const QList<QVariant> values = {
            account, name, person.value(QStringLiteral("employeeNo")).toString(),
            person.value(QStringLiteral("department")).toString(),
            person.value(QStringLiteral("position")).toString(), phone,
            person.value(QStringLiteral("idCard")).toString(), hireDate,
            person.value(QStringLiteral("personType")).toString(), account,
            person.value(QStringLiteral("initialPassword")).toString(),
            person.value(QStringLiteral("groupName")).toString(),
            person.value(QStringLiteral("accessTemplateName")).toString(),
            person.value(QStringLiteral("ruleTemplateName")).toString(),
            person.value(QStringLiteral("enabledFeaturesText")).toString(),
            person.value(QStringLiteral("controlTypeText")).toString(),
            person.value(QStringLiteral("relayText")).toString(),
            person.value(QStringLiteral("remoteCallText")).toString(),
            compactJson(person), now
        };
        if (exists) {
            QList<QVariant> binds = values;
            binds.append(personId);
            writeOk = DbStore::execute(
                        QStringLiteral("UPDATE person SET person_no=?,name=?,employee_no=?,department=?,position=?,"
                                       "phone=?,id_card=?,hire_date_ms=?,person_type=?,account=?,initial_password=?,"
                                       "group_name=?,access_template_name=?,rule_template_name=?,enabled_features_text=?,"
                                       "control_type_text=?,relay_text=?,remote_call_text=?,person_json=?,"
                                       "person_source='excel',enabled=1,deleted=0,updated_at=? WHERE id=?"), binds);
        } else {
            QList<QVariant> binds = values;
            binds.append(now);
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO person(person_no,name,employee_no,department,position,phone,id_card,"
                                       "hire_date_ms,person_type,account,initial_password,group_name,access_template_name,"
                                       "rule_template_name,enabled_features_text,control_type_text,relay_text,remote_call_text,"
                                       "person_json,updated_at,created_at,enabled,deleted,has_face,face_file_list,person_source) "
                                       "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,0,0,'','excel')"), binds);
            if (writeOk) {
                const QList<QVariantMap> ids = DbStore::query(
                            QStringLiteral("SELECT id FROM person WHERE person_no=? LIMIT 1"),
                            {account});
                personId = ids.isEmpty() ? 0
                                         : ids.first().value(QStringLiteral("id")).toLongLong();
                writeOk = personId > 0;
            }
        }

        const QJsonArray qrs = record.value(QStringLiteral("qrCodes")).toArray();
        const QJsonArray floors = record.value(QStringLiteral("floors")).toArray();
        const QJsonArray timings = rules.value(QStringLiteral("timingRules")).toArray();
        const auto configured = [&](const QString &key, bool fallback) {
            return rules.contains(key) ? rules.value(key).toBool() : fallback;
        };
        const bool timeConfigured = configured(
                    QStringLiteral("timeConfigured"), !timings.isEmpty());
        const bool countConfigured = configured(
                    QStringLiteral("countConfigured"),
                    rules.value(QStringLiteral("countDataRemaining")).isDouble()
                    && rules.value(QStringLiteral("countDataTotal")).isDouble());
        const bool amountConfigured = configured(
                    QStringLiteral("amountConfigured"),
                    rules.value(QStringLiteral("amountDataRemaining")).isDouble()
                    && rules.value(QStringLiteral("amountDataTotal")).isDouble()
                    && rules.value(QStringLiteral("amountDataUnitPrice")).isDouble()
                    && rules.value(QStringLiteral("amountDataUnitPrice")).toDouble() > 0.0);
        const bool durationConfigured = configured(
                    QStringLiteral("durationConfigured"),
                    !rules.value(QStringLiteral("durationDataStartTime")).toString().trimmed().isEmpty()
                    && !rules.value(QStringLiteral("durationDataEndTime")).toString().trimmed().isEmpty());
        const bool relayConfigured = configured(
                    QStringLiteral("relayConfigured"),
                    !rules.value(QStringLiteral("relayText")).toString().trimmed().isEmpty());
        const bool remoteCallConfigured = configured(
                    QStringLiteral("remoteCallConfigured"),
                    rules.value(QStringLiteral("remoteCallDataCurrentDayCallCount")).isDouble()
                    && rules.value(QStringLiteral("remoteCallDataEveryDayUpdateCallCount")).isDouble());
        const bool controlElevatorConfigured = configured(
                    QStringLiteral("controlElevatorConfigured"),
                    rules.value(QStringLiteral("controlElevatorDataValueType")).isDouble());
        const QString cardPassword = rules.value(
                    QStringLiteral("cardPasswordDataPassword")).toString().trimmed();
        const bool timeEnabled = rules.value(QStringLiteral("time")).toBool() && timeConfigured;
        const bool countEnabled = rules.value(QStringLiteral("count")).toBool() && countConfigured;
        const bool relayEnabled = rules.value(QStringLiteral("relay")).toBool() && relayConfigured;
        const bool amountEnabled = rules.value(QStringLiteral("amount")).toBool() && amountConfigured;
        const bool durationEnabled = rules.value(QStringLiteral("duration")).toBool() && durationConfigured;
        const bool remoteCallEnabled = rules.value(QStringLiteral("remoteCall")).toBool()
                && remoteCallConfigured;
        const bool cardPasswordEnabled = rules.value(QStringLiteral("cardPassword")).toBool()
                && !cardPassword.isEmpty();
        const bool controlElevatorEnabled = rules.value(QStringLiteral("controlElevator")).toBool()
                && controlElevatorConfigured;
        const qlonglong remoteCallRemaining = rules.value(
                    QStringLiteral("remoteCallDataCurrentDayCallCount")).toVariant().toLongLong();
        const qlonglong remoteCallDailyTotal = rules.value(
                    QStringLiteral("remoteCallDataEveryDayUpdateCallCount")).toVariant().toLongLong();
        const qlonglong remoteCallUsed = remoteCallEnabled
                ? qMax<qlonglong>(0, remoteCallDailyTotal - remoteCallRemaining) : 0;

        QVariant remainingCount = countEnabled
                ? rules.value(QStringLiteral("countDataRemaining")).toVariant() : QVariant();
        QVariant remainingAmount = amountEnabled
                ? rules.value(QStringLiteral("amountDataRemaining")).toVariant() : QVariant();
        if (exists && countEnabled
                && rules.value(QStringLiteral("countDataAddCount")).isDouble()) {
            remainingCount = remainingCount.toLongLong()
                    + rules.value(QStringLiteral("countDataAddCount")).toVariant().toLongLong();
        }
        if (exists && amountEnabled
                && rules.value(QStringLiteral("amountDataAddAmount")).isDouble()) {
            remainingAmount = remainingAmount.toDouble()
                    + rules.value(QStringLiteral("amountDataAddAmount")).toDouble();
        }
        if (writeOk) {
            writeOk = DbStore::execute(QStringLiteral("DELETE FROM local_ic_card WHERE person_id=?"), {personId})
                    && DbStore::execute(QStringLiteral("DELETE FROM local_qr_code WHERE person_id=?"), {personId})
                    && DbStore::execute(QStringLiteral("DELETE FROM local_timing_rule WHERE person_id=?"), {personId})
                    && DbStore::execute(QStringLiteral("DELETE FROM local_access_rule WHERE person_id=?"), {personId})
                    && DbStore::execute(QStringLiteral("DELETE FROM local_floor WHERE person_id=?"), {personId})
                    && DbStore::execute(QStringLiteral("DELETE FROM local_access_usage WHERE person_id=?"), {personId});
        }
        for (const QJsonValue &value : cards) {
            if (!writeOk) break;
            const QJsonObject card = value.toObject();
            const QString cardId = card.value(QStringLiteral("cardId")).toString().trimmed();
            if (cardId.isEmpty()) continue;
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO local_ic_card(card_id,person_id,name,bind_time,active) "
                                       "VALUES(?,?,?,?,1) ON CONFLICT(card_id) DO UPDATE SET "
                                       "person_id=excluded.person_id,name=excluded.name,bind_time=excluded.bind_time,active=1"),
                        {cardId, personId, card.value(QStringLiteral("name")).toString(),
                         card.value(QStringLiteral("bindTime")).toString()});
        }
        for (const QJsonValue &value : qrs) {
            if (!writeOk) break;
            const QJsonObject qr = value.toObject();
            const QString qrId = qr.value(QStringLiteral("qrUnique")).toString().trimmed();
            if (qrId.isEmpty()) continue;
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO local_qr_code(qr_unique,person_id,qr_name,qr_type,purpose,"
                                       "valid_from,valid_to,max_use_count,used_count,bind_time,active) "
                                       "VALUES(?,?,?,?,?,?,?,?,0,?,1) ON CONFLICT(qr_unique) DO UPDATE SET "
                                       "person_id=excluded.person_id,qr_name=excluded.qr_name,qr_type=excluded.qr_type,"
                                       "purpose=excluded.purpose,valid_from=excluded.valid_from,valid_to=excluded.valid_to,"
                                       "max_use_count=excluded.max_use_count,bind_time=excluded.bind_time,active=1"),
                        {qrId, personId, qr.value(QStringLiteral("qrName")).toString(),
                         qr.value(QStringLiteral("qrType")).toString(), qr.value(QStringLiteral("purpose")).toString(),
                         qr.value(QStringLiteral("validFrom")).toString(), qr.value(QStringLiteral("validTo")).toString(),
                         qr.value(QStringLiteral("maxUseCount")).toVariant(),
                         qr.value(QStringLiteral("bindTime")).toString()});
        }
        if (writeOk) {
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO local_access_rule(person_id,time_enabled,count_enabled,relay_enabled,"
                                       "amount_enabled,duration_enabled,remote_call_enabled,remote_call_current_day_count,"
                                       "remote_call_every_day_count,remote_call_used_count,remote_call_refresh_date,"
                                       "card_password_enabled,card_password,control_elevator,count_total,count_add_count,"
                                       "amount_total,amount_add_amount,amount_unit_price,"
                                       "duration_start,duration_end,elevator_value_type,raw_json) "
                                       "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
                        {personId, timeEnabled ? 1 : 0,
                         countEnabled ? 1 : 0,
                         relayEnabled ? 1 : 0,
                         amountEnabled ? 1 : 0,
                         durationEnabled ? 1 : 0,
                         remoteCallEnabled ? 1 : 0,
                         remoteCallEnabled ? QVariant(remoteCallRemaining) : QVariant(),
                         remoteCallEnabled ? QVariant(remoteCallDailyTotal) : QVariant(),
                         remoteCallUsed,
                         remoteCallEnabled ? QDate::currentDate().toString(Qt::ISODate) : QString(),
                         cardPasswordEnabled ? 1 : 0,
                         cardPasswordEnabled ? cardPassword : QString(),
                         controlElevatorEnabled ? 1 : 0,
                         rules.value(QStringLiteral("countDataTotal")).toVariant(),
                         rules.value(QStringLiteral("countDataAddCount")).toVariant(),
                         rules.value(QStringLiteral("amountDataTotal")).toVariant(),
                         rules.value(QStringLiteral("amountDataAddAmount")).toVariant(),
                         rules.value(QStringLiteral("amountDataUnitPrice")).toVariant(),
                         rules.value(QStringLiteral("durationDataStartTime")).toString(),
                         rules.value(QStringLiteral("durationDataEndTime")).toString(),
                         rules.value(QStringLiteral("controlElevatorDataValueType")).toVariant(), compactJson(rules)});
        }
        for (int index = 0; writeOk && index < timings.size(); ++index) {
            const QJsonObject timing = timings.at(index).toObject();
            const QJsonArray range = timing.value(QStringLiteral("timeRange")).toArray();
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO local_timing_rule(person_id,rule_order,days_json,start_time,end_time) "
                                       "VALUES(?,?,?,?,?)"),
                        {personId, index, compactJson(timing.value(QStringLiteral("days"))),
                         range.size() > 0 ? range.at(0).toString() : QString(),
                         range.size() > 1 ? range.at(1).toString() : QString()});
        }
        for (const QJsonValue &value : floors) {
            if (!writeOk) break;
            const QJsonObject floor = value.toObject();
            const QString deviceId = floor.value(QStringLiteral("deviceId")).toString().trimmed();
            for (const QJsonValue &number : floor.value(QStringLiteral("floor")).toArray()) {
                if (!number.isDouble()) continue;
                writeOk = DbStore::execute(
                            QStringLiteral("INSERT OR IGNORE INTO local_floor(person_id,device_id,floor_no) VALUES(?,?,?)"),
                            {personId, deviceId, number.toInt()});
                if (!writeOk) break;
            }
        }
        if (writeOk && (countEnabled || amountEnabled)) {
            writeOk = DbStore::execute(
                        QStringLiteral("INSERT INTO local_access_usage(person_id,remaining_count,access_count,"
                                       "remaining_amount,used_amount,updated_at) VALUES(?,?,0,?,0,?)"),
                        {personId, remainingCount, remainingAmount, now});
        }

        if (!writeOk || !DbStore::transactionCommit()) {
            DbStore::transactionRollback();
            ++summary.failed;
            summary.failedNames.append(name + QStringLiteral("（保存失败）"));
            summary.error = DbStore::lastError();
            continue;
        }
        LocalImportedPerson imported;
        imported.personId = personId;
        imported.personNo = account;
        imported.name = name;
        imported.phone = phone;
        summary.importedPeople.append(imported);
        if (exists) ++summary.updated;
        else ++summary.inserted;
    }

    summary.ok = summary.failed == 0;
    if (!summary.ok && summary.error.isEmpty()) {
        summary.error = QStringLiteral("部分人员未成功导入");
    }
    return summary;
}

bool LocalPersonnelStore::replaceValidatedFaces(const QJsonArray &validatedFaces,
                                                 int *savedCount,
                                                 QStringList *failedPeople,
                                                 QString *error)
{
    if (savedCount) *savedCount = 0;
    if (!initialize()) {
        if (error) *error = DbStore::lastError();
        return false;
    }

    QHash<qint64, QJsonArray> facesByPerson;
    for (const QJsonValue &value : validatedFaces) {
        const QJsonObject face = value.toObject();
        const qint64 personId = face.value(QStringLiteral("localPersonId"))
                .toString().toLongLong();
        if (personId > 0) facesByPerson[personId].append(face);
    }

    bool allOk = true;
    QDir enrollmentDir(Rk3566Platform::applicationRoot());
    enrollmentDir.mkpath(QStringLiteral("enrollments"));
    for (auto it = facesByPerson.constBegin(); it != facesByPerson.constEnd(); ++it) {
        const qint64 personId = it.key();
        const QList<QVariantMap> people = DbStore::query(
                    QStringLiteral("SELECT person_no,name FROM person WHERE id=? LIMIT 1"), {personId});
        if (people.isEmpty()) continue;
        const QString personNo = people.first().value(QStringLiteral("person_no")).toString();
        const QString personName = people.first().value(QStringLiteral("name")).toString();
        QStringList newPaths;
        QList<QPair<QJsonObject, QByteArray>> prepared;
        int sequence = 0;
        for (const QJsonValue &value : it.value()) {
            const QJsonObject face = value.toObject();
            QFile source(face.value(QStringLiteral("imagePath")).toString());
            if (!source.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = source.readAll();
            const QString suffix = QFileInfo(source.fileName()).suffix().toLower();
            const QString relative = QStringLiteral("enrollments/%1_usb_%2_%3.%4")
                    .arg(safeFileStem(personNo),
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz")),
                         QString::number(++sequence), suffix.isEmpty() ? QStringLiteral("jpg") : suffix);
            const QString target = enrollmentDir.absoluteFilePath(relative);
            QString storageError;
            if (!StoragePolicy::writeRegistrationPhoto(target, bytes, &storageError)) {
                allOk = false;
                if (failedPeople) failedPeople->append(personName + QStringLiteral("（人脸图片保存失败）"));
                if (error && error->isEmpty()) *error = storageError;
                continue;
            }
            QJsonObject storedFace = face;
            storedFace.insert(QStringLiteral("storedPath"), target);
            prepared.append(qMakePair(storedFace,
                                      QByteArray::fromBase64(face.value(
                                          QStringLiteral("featureBase64")).toString().toLatin1())));
            newPaths.append(target);
        }
        if (prepared.isEmpty()) continue;

        const QList<QVariantMap> oldFaces = DbStore::query(
                    QStringLiteral("SELECT image_path FROM face_feature WHERE person_id=?"), {personId});
        if (!DbStore::transactionBegin()
                || !DbStore::execute(QStringLiteral("DELETE FROM face_feature WHERE person_id=?"), {personId})) {
            for (const QString &path : newPaths) QFile::remove(path);
            DbStore::transactionRollback();
            allOk = false;
            if (failedPeople) failedPeople->append(personName + QStringLiteral("（人脸入库失败）"));
            if (error && error->isEmpty()) *error = DbStore::lastError();
            continue;
        }
        bool personOk = true;
        for (const auto &entry : prepared) {
            const QJsonObject face = entry.first;
            const QByteArray feature = entry.second;
            personOk = !feature.isEmpty() && DbStore::execute(
                        QStringLiteral("INSERT INTO face_feature(person_id,image_path,feature_blob,feature_size,"
                                       "model_version,quality,created_at) VALUES(?,?,?,?,?,?,?)"),
                        {personId, face.value(QStringLiteral("storedPath")).toString(), feature,
                         feature.size(), face.value(QStringLiteral("modelVersion")).toString(),
                         face.value(QStringLiteral("faceQuality")).toDouble(),
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))});
            if (!personOk) break;
        }
        if (personOk) {
            personOk = DbStore::execute(
                        QStringLiteral("UPDATE person SET has_face=1,face_file_list=?,updated_at=? WHERE id=?"),
                        {newPaths.join(QLatin1Char(',')),
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), personId});
        }
        if (!personOk || !DbStore::transactionCommit()) {
            DbStore::transactionRollback();
            for (const QString &path : newPaths) QFile::remove(path);
            allOk = false;
            if (failedPeople) failedPeople->append(personName + QStringLiteral("（人脸入库失败）"));
            if (error && error->isEmpty()) *error = DbStore::lastError();
            continue;
        }
        for (const QVariantMap &oldFace : oldFaces) {
            const QString oldPath = oldFace.value(QStringLiteral("image_path")).toString();
            if (!oldPath.isEmpty() && !newPaths.contains(oldPath)) QFile::remove(oldPath);
        }
        if (savedCount) *savedCount += prepared.size();
    }
    return allOk;
}
