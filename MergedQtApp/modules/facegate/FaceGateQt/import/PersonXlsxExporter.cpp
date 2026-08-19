/**
 * @file PersonXlsxExporter.cpp
 * @brief 人员数据库到固定31列 XLSX 模板的映射及无外部命令写入实现。
 */

#include "PersonXlsxExporter.h"

#include "common/sql/dbstore.h"
#include "common/sql/network_personnel_store.h"
#include "platform/rk3566_platform.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSaveFile>
#include <QStringList>
#include <QTime>
#include <QtGlobal>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <limits>
#include <zlib.h>

namespace {

struct ExportRows
{
    bool ok = false;
    QVector<QStringList> rows;
    QString error;
};

QJsonObject jsonObject(const QString &text)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
            ? document.object() : QJsonObject();
}

QJsonArray jsonArray(const QString &text)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isArray()
            ? document.array() : QJsonArray();
}

QString jsonText(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 15);
    if (value.isBool()) return value.toBool() ? QStringLiteral("true")
                                              : QStringLiteral("false");
    return QString();
}

QString compactNumber(double value)
{
    QString text = QString::number(value, 'f', 2);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return text;
}

bool jsonNumber(const QJsonObject &object, const QString &key, double *value)
{
    const QJsonValue item = object.value(key);
    if (item.isDouble()) {
        if (value) *value = item.toDouble();
        return true;
    }
    if (item.isString()) {
        bool ok = false;
        const double number = item.toString().toDouble(&ok);
        if (ok && value) *value = number;
        return ok;
    }
    return false;
}

bool mapNumber(const QVariantMap &row, const QString &key, double *value)
{
    const QVariant item = row.value(key);
    if (!item.isValid() || item.isNull()) return false;
    bool ok = false;
    const double number = item.toDouble(&ok);
    if (ok && value) *value = number;
    return ok;
}

QString headersRow(int index)
{
    static const QStringList headers = {
        QStringLiteral("序号"), QStringLiteral("姓名"), QStringLiteral("工号"),
        QStringLiteral("部门"), QStringLiteral("职位"), QStringLiteral("手机号"),
        QStringLiteral("身份证号"), QStringLiteral("入职日期"), QStringLiteral("人员类型"),
        QStringLiteral("账号"), QStringLiteral("初始密码"), QStringLiteral("分组名称"),
        QStringLiteral("IC卡号"), QStringLiteral("二维码"), QStringLiteral("是否有人脸"),
        QStringLiteral("通行模板名称"), QStringLiteral("规则模板名称"), QStringLiteral("设备及楼层"),
        QStringLiteral("启用功能"), QStringLiteral("定时规则"), QStringLiteral("剩余次数"),
        QStringLiteral("剩余金额"), QStringLiteral("单价"), QStringLiteral("有效期限"),
        QStringLiteral("远程呼梯"), QStringLiteral("控梯类型"), QStringLiteral("继电器"),
        QStringLiteral("密码通行"), QStringLiteral("人脸文件列表"), QStringLiteral("增量次数"),
        QStringLiteral("增量金额")
    };
    return headers.value(index);
}

QStringList headerRow()
{
    QStringList row;
    for (int i = 0; i < 31; ++i) row.append(headersRow(i));
    return row;
}

QStringList instructionRow()
{
    return QStringList{
        QStringLiteral("自动生成"), QStringLiteral("必填，人员真实姓名"),
        QStringLiteral("选填，人员工号"), QStringLiteral("选填，所属部门"),
        QStringLiteral("选填，人员职位"), QStringLiteral("必填，11位手机号"),
        QStringLiteral("选填，18位含末位X"), QStringLiteral("选填，yyyy-MM-dd"),
        QStringLiteral("必填，填：员工 或 访客"), QStringLiteral("必填，本物业唯一"),
        QStringLiteral("必填，初始登录密码"), QStringLiteral("选填，不存在则创建"),
        QStringLiteral("选填，多个用英文逗号分隔"), QStringLiteral("选填，多个用英文逗号分隔"),
        QStringLiteral("选填，是/否，默认否"), QStringLiteral("选填，已存在的模板名称"),
        QStringLiteral("选填，已存在的模板名称"), QStringLiteral("选填，格式：设备名:楼层1,楼层2"),
        QStringLiteral("选填，导出只读"), QStringLiteral("选填，导出只读"),
        QStringLiteral("选填，导出只读"), QStringLiteral("选填，导出只读"),
        QStringLiteral("选填，导出只读"), QStringLiteral("选填，导出只读"),
        QStringLiteral("选填，导出只读"), QStringLiteral("选填，导出只读"),
        QStringLiteral("选填，导出只读"), QStringLiteral("选填，导出只读"),
        QStringLiteral("导出列，导入时忽略"),
        QStringLiteral("选填，导出只读；模板应用时记录的增量次数"),
        QStringLiteral("选填，导出只读；模板应用时记录的增量金额")
    };
}

QStringList exampleRow()
{
    return QStringList{
        QStringLiteral("1"), QStringLiteral("张三"), QStringLiteral("EMP001"),
        QStringLiteral("技术部"), QStringLiteral("工程师"), QStringLiteral("13800138000"),
        QStringLiteral("110101199001011234"), QStringLiteral("2024-01-15"),
        QStringLiteral("员工"), QStringLiteral("zhangsan"), QStringLiteral("123456"),
        QStringLiteral("研发一组"), QStringLiteral("10001,10002"), QStringLiteral("QR-001"),
        QStringLiteral("否"), QStringLiteral("标准通行模板"), QStringLiteral("规则模板A"),
        QStringLiteral("测试1:3,8"), QStringLiteral("定时,远程呼梯"),
        QStringLiteral("周三18:00-18:07"), QStringLiteral("80/100"),
        QStringLiteral("¥50.00/¥100.00"), QStringLiteral("¥0.50"),
        QStringLiteral("2026-01-01 ~ 2026-12-31"), QStringLiteral("日15次/每日更新10次"),
        QStringLiteral("直梯"), QStringLiteral("通道1,2 导通5s"), QStringLiteral("否"),
        QStringLiteral("zhangsan.jpg"), QStringLiteral("8"), QStringLiteral("7")
    };
}

QString personTypeText(QString value)
{
    value = value.trimmed();
    if (value.compare(QStringLiteral("EMPLOYEE"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("员工");
    }
    if (value.compare(QStringLiteral("VISITOR"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("访客");
    }
    return value;
}

QString dateText(const QVariantMap &row, const QJsonObject &person)
{
    QString text = jsonText(person, QStringLiteral("hireDateText")).trimmed();
    if (!text.isEmpty()) return text;
    const QJsonValue jsonDate = person.value(QStringLiteral("hireDate"));
    if (jsonDate.isString()) {
        const QDate date = QDate::fromString(jsonDate.toString().left(10), Qt::ISODate);
        if (date.isValid()) return date.toString(QStringLiteral("yyyy-MM-dd"));
    }
    qint64 milliseconds = 0;
    bool valid = false;
    if (jsonDate.isDouble()) {
        milliseconds = qint64(jsonDate.toDouble());
        valid = true;
    } else if (!row.value(QStringLiteral("hire_date_ms")).isNull()) {
        milliseconds = row.value(QStringLiteral("hire_date_ms")).toLongLong(&valid);
    }
    return valid ? QDateTime::fromMSecsSinceEpoch(milliseconds).date()
                   .toString(QStringLiteral("yyyy-MM-dd")) : QString();
}

QString joinedColumn(const QString &sql,
                     const QList<QVariant> &binds,
                     const QString &column)
{
    QStringList values;
    const QList<QVariantMap> rows = DbStore::query(sql, binds);
    for (const QVariantMap &row : rows) {
        const QString value = row.value(column).toString().trimmed();
        if (!value.isEmpty()) values.append(value);
    }
    return values.join(QLatin1Char(','));
}

QString faceFileList(const QString &personId)
{
    QStringList values;
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT name,image_path FROM network_face "
                               "WHERE person_id=? ORDER BY id"), {personId});
    for (const QVariantMap &row : rows) {
        QString value = row.value(QStringLiteral("name")).toString().trimmed();
        if (value.isEmpty()) {
            value = QFileInfo(row.value(QStringLiteral("image_path")).toString()).fileName();
        }
        if (!value.isEmpty()) values.append(value);
    }
    return values.join(QLatin1Char(','));
}

QString floorText(const QString &personId)
{
    QMap<QString, QStringList> devices;
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT device_id,floor_no FROM network_floor "
                               "WHERE person_id=? ORDER BY device_id,floor_no"), {personId});
    for (const QVariantMap &row : rows) {
        devices[row.value(QStringLiteral("device_id")).toString()]
                .append(QString::number(row.value(QStringLiteral("floor_no")).toInt()));
    }
    QStringList parts;
    for (auto it = devices.constBegin(); it != devices.constEnd(); ++it) {
        if (!it.key().trimmed().isEmpty() && !it.value().isEmpty()) {
            parts.append(it.key() + QLatin1Char(':') + it.value().join(QLatin1Char(',')));
        }
    }
    return parts.join(QStringLiteral("; "));
}

QString timingText(const QString &personId, const QJsonObject &rules)
{
    const QString stored = jsonText(rules, QStringLiteral("timingRuleText")).trimmed();
    if (!stored.isEmpty()) return stored;
    const QStringList dayNames = {
        QString(), QStringLiteral("周一"), QStringLiteral("周二"), QStringLiteral("周三"),
        QStringLiteral("周四"), QStringLiteral("周五"), QStringLiteral("周六"), QStringLiteral("周日")
    };
    QStringList result;
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral("SELECT days_json,start_time,end_time FROM network_timing_rule "
                               "WHERE person_id=? ORDER BY rule_order"), {personId});
    for (const QVariantMap &row : rows) {
        QStringList days;
        for (const QJsonValue &value : jsonArray(row.value(QStringLiteral("days_json")).toString())) {
            const int day = value.toInt();
            if (day > 0 && day < dayNames.size()) days.append(dayNames.at(day));
        }
        const QString start = row.value(QStringLiteral("start_time")).toString().left(5);
        const QString end = row.value(QStringLiteral("end_time")).toString().left(5);
        if (!days.isEmpty() && !start.isEmpty() && !end.isEmpty()) {
            result.append(days.join(QLatin1Char(',')) + start + QLatin1Char('-') + end);
        }
    }
    return result.join(QStringLiteral("; "));
}

QString enabledFeatures(const QJsonObject &person,
                        const QJsonObject &rules,
                        const QVariantMap &ruleRow)
{
    const QString stored = jsonText(person, QStringLiteral("enabledFeaturesText")).trimmed();
    if (!stored.isEmpty()) return stored;
    const QList<QPair<QString, QString>> features = {
        {QStringLiteral("定时"), QStringLiteral("time")},
        {QStringLiteral("次数"), QStringLiteral("count")},
        {QStringLiteral("金额"), QStringLiteral("amount")},
        {QStringLiteral("期限"), QStringLiteral("duration")},
        {QStringLiteral("继电器"), QStringLiteral("relay")},
        {QStringLiteral("控梯"), QStringLiteral("controlElevator")},
        {QStringLiteral("远程呼梯"), QStringLiteral("remoteCall")},
        {QStringLiteral("密码通行"), QStringLiteral("cardPassword")}
    };
    const QHash<QString, QString> columns = {
        {QStringLiteral("time"), QStringLiteral("time_enabled")},
        {QStringLiteral("count"), QStringLiteral("count_enabled")},
        {QStringLiteral("amount"), QStringLiteral("amount_enabled")},
        {QStringLiteral("duration"), QStringLiteral("duration_enabled")},
        {QStringLiteral("relay"), QStringLiteral("relay_enabled")},
        {QStringLiteral("controlElevator"), QStringLiteral("control_elevator")},
        {QStringLiteral("remoteCall"), QStringLiteral("remote_call_enabled")},
        {QStringLiteral("cardPassword"), QStringLiteral("card_password_enabled")}
    };
    QStringList values;
    for (const auto &feature : features) {
        const bool enabled = rules.contains(feature.second)
                ? rules.value(feature.second).toBool()
                : ruleRow.value(columns.value(feature.second)).toInt() != 0;
        if (enabled) values.append(feature.first);
    }
    return values.join(QLatin1Char(','));
}

QStringList networkPersonRow(const QVariantMap &row, int sequence)
{
    const QString personId = row.value(QStringLiteral("person_id")).toString();
    const QJsonObject person = jsonObject(row.value(QStringLiteral("person_json")).toString());
    const QList<QVariantMap> ruleRows = DbStore::query(
                QStringLiteral("SELECT * FROM network_access_rule WHERE person_id=? LIMIT 1"),
                {personId});
    const QVariantMap ruleRow = ruleRows.value(0);
    const QJsonObject rules = jsonObject(ruleRow.value(QStringLiteral("raw_json")).toString());
    const QList<QVariantMap> usageRows = DbStore::query(
                QStringLiteral("SELECT remaining_count FROM network_access_usage "
                               "WHERE person_id=? LIMIT 1"), {personId});

    const QString cards = joinedColumn(
                QStringLiteral("SELECT card_id FROM network_ic_card "
                               "WHERE person_id=? AND active=1 ORDER BY card_id"),
                {personId}, QStringLiteral("card_id"));
    const QString qrCodes = joinedColumn(
                QStringLiteral("SELECT qr_unique FROM network_qr_code "
                               "WHERE person_id=? AND active=1 ORDER BY qr_unique"),
                {personId}, QStringLiteral("qr_unique"));

    double countRemaining = 0;
    double countTotal = 0;
    const bool hasCountRemaining = (!usageRows.isEmpty()
                                    && mapNumber(usageRows.first(),
                                                 QStringLiteral("remaining_count"),
                                                 &countRemaining))
            || jsonNumber(rules, QStringLiteral("countDataRemaining"), &countRemaining);
    const bool hasCountTotal = jsonNumber(rules, QStringLiteral("countDataTotal"), &countTotal)
            || mapNumber(ruleRow, QStringLiteral("count_total"), &countTotal);
    const QString counts = hasCountRemaining || hasCountTotal
            ? compactNumber(countRemaining) + QLatin1Char('/') + compactNumber(countTotal)
            : QString();

    double amountRemaining = 0;
    double amountTotal = 0;
    const bool hasAmountRemaining = jsonNumber(rules, QStringLiteral("amountDataRemaining"), &amountRemaining);
    const bool hasAmountTotal = jsonNumber(rules, QStringLiteral("amountDataTotal"), &amountTotal)
            || mapNumber(ruleRow, QStringLiteral("amount_total"), &amountTotal);
    const QString amounts = hasAmountRemaining || hasAmountTotal
            ? QStringLiteral("¥") + compactNumber(amountRemaining)
              + QStringLiteral("/¥") + compactNumber(amountTotal)
            : QString();

    double unitPrice = 0;
    const bool hasUnitPrice = jsonNumber(rules, QStringLiteral("amountDataUnitPrice"), &unitPrice)
            || mapNumber(ruleRow, QStringLiteral("amount_unit_price"), &unitPrice);
    double countAdd = 0;
    const bool hasCountAdd = jsonNumber(rules, QStringLiteral("countDataAddCount"), &countAdd)
            || mapNumber(ruleRow, QStringLiteral("count_add_count"), &countAdd);
    double amountAdd = 0;
    const bool hasAmountAdd = jsonNumber(rules, QStringLiteral("amountDataAddAmount"), &amountAdd)
            || mapNumber(ruleRow, QStringLiteral("amount_add_amount"), &amountAdd);

    QString durationStart = jsonText(rules, QStringLiteral("durationDataStartTime"));
    QString durationEnd = jsonText(rules, QStringLiteral("durationDataEndTime"));
    if (durationStart.isEmpty()) durationStart = ruleRow.value(QStringLiteral("duration_start")).toString();
    if (durationEnd.isEmpty()) durationEnd = ruleRow.value(QStringLiteral("duration_end")).toString();
    const QString duration = durationStart.isEmpty() && durationEnd.isEmpty()
            ? QString() : durationStart + QStringLiteral(" ~ ") + durationEnd;

    QString controlType = jsonText(person, QStringLiteral("controlTypeText"));
    if (controlType.isEmpty() && (rules.value(QStringLiteral("controlElevator")).toBool()
                                  || ruleRow.value(QStringLiteral("control_elevator")).toInt() != 0)) {
        const int valueType = rules.contains(QStringLiteral("controlElevatorDataValueType"))
                ? rules.value(QStringLiteral("controlElevatorDataValueType")).toInt()
                : ruleRow.value(QStringLiteral("elevator_value_type")).toInt();
        controlType = valueType == 1 ? QStringLiteral("直梯") : QStringLiteral("门禁");
    }
    QString password = QStringLiteral("否");
    if (rules.value(QStringLiteral("cardPassword")).toBool()
            || ruleRow.value(QStringLiteral("card_password_enabled")).toInt() != 0) {
        password = QStringLiteral("是");
    }

    const QList<QVariantMap> faceCountRows = DbStore::query(
                QStringLiteral("SELECT COUNT(*) AS count FROM network_face WHERE person_id=?"),
                {personId});
    const bool hasFace = (!faceCountRows.isEmpty()
                          && faceCountRows.first().value(QStringLiteral("count")).toInt() > 0)
            || person.value(QStringLiteral("hasFace")).toBool();

    QString type = jsonText(person, QStringLiteral("personType"));
    if (type.isEmpty()) type = row.value(QStringLiteral("person_type")).toString();
    QString name = jsonText(person, QStringLiteral("name"));
    if (name.isEmpty()) name = row.value(QStringLiteral("name")).toString();

    return QStringList{
        QString::number(sequence), name,
        jsonText(person, QStringLiteral("employeeNo")).isEmpty()
            ? row.value(QStringLiteral("employee_no")).toString()
            : jsonText(person, QStringLiteral("employeeNo")),
        jsonText(person, QStringLiteral("department")).isEmpty()
            ? row.value(QStringLiteral("department")).toString()
            : jsonText(person, QStringLiteral("department")),
        jsonText(person, QStringLiteral("position")).isEmpty()
            ? row.value(QStringLiteral("position")).toString()
            : jsonText(person, QStringLiteral("position")),
        jsonText(person, QStringLiteral("phone")).isEmpty()
            ? row.value(QStringLiteral("phone")).toString()
            : jsonText(person, QStringLiteral("phone")),
        jsonText(person, QStringLiteral("idCard")).isEmpty()
            ? row.value(QStringLiteral("id_card")).toString()
            : jsonText(person, QStringLiteral("idCard")),
        dateText(row, person), personTypeText(type),
        jsonText(person, QStringLiteral("account")),
        jsonText(person, QStringLiteral("initialPassword")),
        jsonText(person, QStringLiteral("groupName")),
        cards, qrCodes, hasFace ? QStringLiteral("是") : QStringLiteral("否"),
        jsonText(person, QStringLiteral("accessTemplateName")),
        jsonText(person, QStringLiteral("ruleTemplateName")),
        floorText(personId), enabledFeatures(person, rules, ruleRow),
        timingText(personId, rules), counts, amounts,
        hasUnitPrice ? QStringLiteral("¥") + compactNumber(unitPrice) : QString(),
        duration,
        !jsonText(person, QStringLiteral("remoteCallText")).isEmpty()
            ? jsonText(person, QStringLiteral("remoteCallText"))
            : jsonText(rules, QStringLiteral("remoteCallText")),
        controlType,
        !jsonText(person, QStringLiteral("relayText")).isEmpty()
            ? jsonText(person, QStringLiteral("relayText"))
            : jsonText(rules, QStringLiteral("relayText")),
        password, faceFileList(personId),
        hasCountAdd ? compactNumber(countAdd) : QString(),
        hasAmountAdd ? compactNumber(amountAdd) : QString()
    };
}

ExportRows loadNetworkRows()
{
    ExportRows result;
    const QList<QVariantMap> people = DbStore::query(
                QStringLiteral("SELECT person_id,name,phone,id_card,employee_no,department,position,"
                               "hire_date_ms,person_type,person_json FROM network_person "
                               "WHERE active=1 AND deleted=0 ORDER BY created_at,person_id"));
    int sequence = 1;
    for (const QVariantMap &person : people) {
        result.rows.append(networkPersonRow(person, sequence++));
        if (!DbStore::lastError().isEmpty()) {
            result.error = DbStore::lastError();
            return result;
        }
    }
    if (!DbStore::lastError().isEmpty()) {
        result.error = DbStore::lastError();
        return result;
    }
    result.ok = true;
    return result;
}

ExportRows loadLocalRows()
{
    ExportRows result;
    const QList<QVariantMap> people = DbStore::query(
                QStringLiteral("SELECT p.id,p.person_no,p.name,p.enabled,COUNT(f.id) AS face_count "
                               "FROM person p LEFT JOIN face_feature f ON f.person_id=p.id "
                               "WHERE COALESCE(p.deleted,0)=0 "
                               "GROUP BY p.id,p.person_no,p.name,p.enabled ORDER BY p.id"));
    if (!DbStore::lastError().isEmpty()) {
        result.error = DbStore::lastError();
        return result;
    }
    int sequence = 1;
    for (const QVariantMap &person : people) {
        QStringList row;
        row.reserve(31);
        for (int column = 0; column < 31; ++column) row.append(QString());
        row[0] = QString::number(sequence++);
        row[1] = person.value(QStringLiteral("name")).toString();
        row[2] = person.value(QStringLiteral("person_no")).toString();
        row[9] = person.value(QStringLiteral("person_no")).toString();
        row[14] = person.value(QStringLiteral("face_count")).toInt() > 0
                ? QStringLiteral("是") : QStringLiteral("否");
        result.rows.append(row);
    }
    result.ok = true;
    return result;
}

ExportRows loadAllRows()
{
    ExportRows result = loadNetworkRows();
    if (!result.ok) return result;

    const ExportRows local = loadLocalRows();
    if (!local.ok) return local;

    result.rows += local.rows;
    for (int index = 0; index < result.rows.size(); ++index) {
        result.rows[index][0] = QString::number(index + 1);
    }
    return result;
}

QString xmlEscape(QString value)
{
    QString clean;
    clean.reserve(value.size());
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code == 0x09 || code == 0x0A || code == 0x0D || code >= 0x20) {
            clean.append(character);
        }
    }
    clean.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    clean.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    clean.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    clean.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    clean.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
    return clean;
}

QString columnName(int zeroBased)
{
    QString result;
    int value = zeroBased + 1;
    while (value > 0) {
        const int remainder = (value - 1) % 26;
        result.prepend(QChar(QLatin1Char('A').unicode() + remainder));
        value = (value - 1) / 26;
    }
    return result;
}

QByteArray worksheetXml(const QVector<QStringList> &personRows)
{
    QVector<QStringList> rows;
    rows.reserve(personRows.size() + 3);
    rows.append(headerRow());
    rows.append(instructionRow());
    rows.append(exampleRow());
    rows += personRows;

    static const int widths[] = {
        8, 16, 14, 14, 14, 16, 22, 14, 12, 18, 14, 18, 24, 24, 12, 20,
        20, 50, 28, 36, 14, 18, 12, 28, 28, 14, 24, 14, 28, 14, 14
    };
    QString xml = QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                "<dimension ref=\"A1:AE%1\"/>"
                "<sheetViews><sheetView workbookViewId=\"0\" zoomScale=\"70\">"
                "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
                "</sheetView></sheetViews><sheetFormatPr defaultRowHeight=\"22\"/><cols>")
            .arg(rows.size());
    for (int i = 0; i < 31; ++i) {
        xml += QStringLiteral("<col min=\"%1\" max=\"%1\" width=\"%2\" customWidth=\"1\"/>")
                .arg(i + 1).arg(widths[i]);
    }
    xml += QStringLiteral("</cols><sheetData>");
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const int excelRow = rowIndex + 1;
        const int style = rowIndex == 0 ? 1 : (rowIndex == 1 ? 2 : 3);
        const int height = rowIndex == 0 ? 28 : (rowIndex == 1 ? 48 : 26);
        xml += QStringLiteral("<row r=\"%1\" ht=\"%2\" customHeight=\"1\">")
                .arg(excelRow).arg(height);
        const QStringList &row = rows.at(rowIndex);
        for (int column = 0; column < 31; ++column) {
            const QString value = row.value(column);
            const QString reference = columnName(column) + QString::number(excelRow);
            xml += QStringLiteral("<c r=\"%1\" s=\"%2\" t=\"inlineStr\"><is><t xml:space=\"preserve\">%3</t></is></c>")
                    .arg(reference).arg(style).arg(xmlEscape(value));
        }
        xml += QStringLiteral("</row>");
    }
    xml += QStringLiteral("</sheetData><autoFilter ref=\"A1:AE%1\"/>"
                          "<pageMargins left=\"0.3\" right=\"0.3\" top=\"0.5\" bottom=\"0.5\" "
                          "header=\"0.2\" footer=\"0.2\"/></worksheet>").arg(rows.size());
    return xml.toUtf8();
}

QByteArray stylesXml()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"4\">"
        "<font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font>"
        "<font><b/><sz val=\"14\"/><name val=\"宋体\"/></font>"
        "<font><color rgb=\"FF666666\"/><sz val=\"10\"/><name val=\"宋体\"/></font>"
        "<font><sz val=\"11\"/><name val=\"宋体\"/></font>"
        "</fonts>"
        "<fills count=\"4\"><fill><patternFill patternType=\"none\"/></fill>"
        "<fill><patternFill patternType=\"gray125\"/></fill>"
        "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFD9E1F2\"/><bgColor indexed=\"64\"/></patternFill></fill>"
        "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FFF2F2F2\"/><bgColor indexed=\"64\"/></patternFill></fill>"
        "</fills>"
        "<borders count=\"2\"><border><left/><right/><top/><bottom/><diagonal/></border>"
        "<border><left style=\"thin\"><color rgb=\"FFB7B7B7\"/></left>"
        "<right style=\"thin\"><color rgb=\"FFB7B7B7\"/></right>"
        "<top style=\"thin\"><color rgb=\"FFB7B7B7\"/></top>"
        "<bottom style=\"thin\"><color rgb=\"FFB7B7B7\"/></bottom><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"4\">"
        "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\" wrapText=\"1\"/></xf>"
        "<xf numFmtId=\"0\" fontId=\"2\" fillId=\"3\" borderId=\"1\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyBorder=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\" wrapText=\"1\"/></xf>"
        "<xf numFmtId=\"49\" fontId=\"3\" fillId=\"0\" borderId=\"1\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\" applyBorder=\"1\" applyAlignment=\"1\"><alignment vertical=\"center\" wrapText=\"1\"/></xf>"
        "</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
        "</styleSheet>");
}

void append16(QByteArray *data, quint16 value)
{
    data->append(char(value & 0xFFu));
    data->append(char((value >> 8) & 0xFFu));
}

void append32(QByteArray *data, quint32 value)
{
    append16(data, quint16(value & 0xFFFFu));
    append16(data, quint16((value >> 16) & 0xFFFFu));
}

struct ZipEntry
{
    QByteArray name;
    QByteArray data;
    quint32 crc = 0;
    quint32 offset = 0;
};

bool writeXlsx(const QString &filePath,
               const QVector<QStringList> &rows,
               QString *error)
{
    QVector<ZipEntry> entries;
    auto add = [&](const char *name, const QByteArray &data) {
        ZipEntry entry;
        entry.name = QByteArray(name);
        entry.data = data;
        entry.crc = quint32(crc32(0L,
                                  reinterpret_cast<const Bytef *>(data.constData()),
                                  uInt(data.size())));
        entries.append(entry);
    };

    add("[Content_Types].xml", QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>"));
    add("_rels/.rels", QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>"));
    add("xl/workbook.xml", QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<bookViews><workbookView/></bookViews><sheets><sheet name=\"人员列表\" sheetId=\"1\" r:id=\"rId1\"/></sheets>"
        "</workbook>"));
    add("xl/_rels/workbook.xml.rels", QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "</Relationships>"));
    add("xl/styles.xml", stylesXml());
    add("xl/worksheets/sheet1.xml", worksheetXml(rows));

    quint64 totalSize = 0;
    for (const ZipEntry &entry : entries) totalSize += entry.data.size() + entry.name.size() + 80;
    if (totalSize > quint64(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("导出人员表过大，超过设备端内存限制");
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const quint16 dosTime = quint16((now.time().hour() << 11)
                                    | (now.time().minute() << 5)
                                    | (now.time().second() / 2));
    const int year = qMax(1980, now.date().year());
    const quint16 dosDate = quint16(((year - 1980) << 9)
                                    | (now.date().month() << 5)
                                    | now.date().day());
    QByteArray archive;
    archive.reserve(int(totalSize));
    for (ZipEntry &entry : entries) {
        entry.offset = quint32(archive.size());
        append32(&archive, 0x04034B50u);
        append16(&archive, 20);
        append16(&archive, 0x0800u);
        append16(&archive, 0);
        append16(&archive, dosTime);
        append16(&archive, dosDate);
        append32(&archive, entry.crc);
        append32(&archive, quint32(entry.data.size()));
        append32(&archive, quint32(entry.data.size()));
        append16(&archive, quint16(entry.name.size()));
        append16(&archive, 0);
        archive.append(entry.name);
        archive.append(entry.data);
    }
    const quint32 centralOffset = quint32(archive.size());
    for (const ZipEntry &entry : entries) {
        append32(&archive, 0x02014B50u);
        append16(&archive, 20);
        append16(&archive, 20);
        append16(&archive, 0x0800u);
        append16(&archive, 0);
        append16(&archive, dosTime);
        append16(&archive, dosDate);
        append32(&archive, entry.crc);
        append32(&archive, quint32(entry.data.size()));
        append32(&archive, quint32(entry.data.size()));
        append16(&archive, quint16(entry.name.size()));
        append16(&archive, 0);
        append16(&archive, 0);
        append16(&archive, 0);
        append16(&archive, 0);
        append32(&archive, 0);
        append32(&archive, entry.offset);
        archive.append(entry.name);
    }
    const quint32 centralSize = quint32(archive.size()) - centralOffset;
    append32(&archive, 0x06054B50u);
    append16(&archive, 0);
    append16(&archive, 0);
    append16(&archive, quint16(entries.size()));
    append16(&archive, quint16(entries.size()));
    append32(&archive, centralSize);
    append32(&archive, centralOffset);
    append16(&archive, 0);

    QSaveFile file(filePath);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法创建导出文件：%1").arg(file.errorString());
        return false;
    }
    if (file.write(archive) != archive.size()) {
        if (error) *error = QStringLiteral("写入导出文件失败：%1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = QStringLiteral("保存导出文件失败：%1").arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace

PersonXlsxExportResult PersonXlsxExporter::exportToFile(PersonExportKind kind,
                                                        const QString &filePath)
{
    PersonXlsxExportResult result;
    if (!DbStore::bootstrap(Rk3566Platform::databasePath())) {
        result.error = DbStore::lastError();
        return result;
    }
    if (kind == PersonExportKind::Network || kind == PersonExportKind::All) {
        NetworkPersonnelStore store;
        if (!store.initialize()) {
            result.error = DbStore::lastError();
            DbStore::close();
            return result;
        }
    }
    ExportRows data;
    switch (kind) {
    case PersonExportKind::Network:
        data = loadNetworkRows();
        break;
    case PersonExportKind::All:
        data = loadAllRows();
        break;
    case PersonExportKind::Local:
    default:
        data = loadLocalRows();
        break;
    }
    if (!data.ok) {
        result.error = data.error;
        DbStore::close();
        return result;
    }
    result.personCount = data.rows.size();
    result.ok = writeXlsx(filePath, data.rows, &result.error);
    DbStore::close();
    return result;
}

QString PersonXlsxExporter::kindName(PersonExportKind kind)
{
    switch (kind) {
    case PersonExportKind::Network: return QStringLiteral("网络人员");
    case PersonExportKind::All: return QStringLiteral("全部人员");
    case PersonExportKind::Local:
    default: return QStringLiteral("本地人员");
    }
}
