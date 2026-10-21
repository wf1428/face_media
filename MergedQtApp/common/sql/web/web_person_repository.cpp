/**
 * @file web_person_repository.cpp
 * @brief 原 person 表的独立只读 Web 投影；完整写入仍复用 Excel 入库方法。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_person_repository.h"
#include "web_sql.h"
#include <QSet>

namespace {

const QString selection = QStringLiteral(
    "SELECT id,name,person_no AS account,employee_no AS employeeNo,department,phone,enabled FROM "
    "person WHERE deleted=0"); /**< 基本公开字段。 */
}

/** @brief 读取可在 Web 中查看的本地人员列表。 */
QJsonObject WebPeople::list()
{
    return {
        {"persons",
         WebSql::array(WebSql::query(selection + QStringLiteral(" ORDER BY id DESC LIMIT 5000")))}};
}

/** @brief 读取指定本地人员详情。 */
QJsonObject WebPeople::get(const QString &id)
{
    bool valid = false;
    const qint64 value = id.toLongLong(&valid);
    if (!valid || value <= 0)
        throw WebError(400, QStringLiteral("人员编号无效"));
    const auto rows = WebSql::query(selection + QStringLiteral(" AND id=?"), {value});
    if (rows.isEmpty())
        throw WebError(404, QStringLiteral("本地人员不存在"));
    return {{"person", WebSql::object(rows.first())}};
}

/** @brief 检查本地与网络人员防重所需的数据源。 */
void WebPeople::verifyConflictSources()
{
    WebSql::query(
        QStringLiteral("SELECT id,person_no,account,name,phone,deleted FROM person LIMIT 0"));
    WebSql::query(QStringLiteral("SELECT person_id,card_id FROM local_ic_card LIMIT 0"));
    WebSql::query(QStringLiteral(
        "SELECT person_id,card_password_enabled,card_password FROM local_access_rule LIMIT 0"));
    const auto rows = WebSql::query(
        QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name IN "
                       "('network_person','network_ic_card','network_access_rule')"));
    QSet<QString> tables;
    for (const auto &row : rows)
        tables.insert(row.value("name").toString());
    if (!tables.isEmpty() && !tables.contains("network_person"))
        throw WebError(503, QStringLiteral("网络人员冲突数据结构未就绪"));
    if (tables.contains("network_person"))
        WebSql::query(QStringLiteral(
            "SELECT person_id,name,phone,person_json,active,deleted FROM network_person LIMIT 0"));
    if (tables.contains("network_ic_card"))
        WebSql::query(QStringLiteral("SELECT c.person_id,c.card_id,p.name FROM network_ic_card c "
                                     "JOIN network_person p ON p.person_id=c.person_id LIMIT 0"));
    if (tables.contains("network_access_rule"))
        WebSql::query(QStringLiteral(
            "SELECT r.person_id,r.card_password_enabled,r.card_password,p.name,p.active,p.deleted "
            "FROM network_access_rule r JOIN network_person p ON p.person_id=r.person_id LIMIT 0"));
}
