/**
 * @file web_import_repository.cpp
 * @brief Web 批次状态与图片清单；不涉及 network_* 写入。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_import_repository.h"
#include "web_sql.h"
#include <QUuid>
#include <QDateTime>

/** @brief 保存新的人员导入批次和文件清单。 */
QString WebImports::create(int owner, const QJsonArray &manifest)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    WebSql::execute(QStringLiteral("INSERT INTO "
                                   "web_person_import_batches(id,owner_id,state,result_json,"
                                   "created_at,updated_at) VALUES(?,?,'OPEN',?,?,?)"),
                    {id, owner, WebSql::json({{"manifest", manifest}}), now, now});
    return id;
}

/** @brief 按批次编号与账号归属查询导入状态。 */
QJsonObject WebImports::get(const QString &id, int owner)
{
    auto rows =
        WebSql::query(QStringLiteral("SELECT * FROM web_person_import_batches WHERE id=?"), {id});
    if (rows.isEmpty() || (owner && rows.first().value("owner_id").toInt() != owner))
        throw WebError(404, QStringLiteral("导入批次不存在"));
    QJsonObject result = WebSql::object(rows.first());
    result.insert(
        "result",
        QJsonDocument::fromJson(rows.first().value("result_json").toByteArray()).object());
    result.remove("result_json");
    result.remove("owner_id");
    QJsonArray files;
    for (const auto &asset : assets(id))
        files.append(QJsonObject{{"name", asset.value("original_name").toString()},
                                 {"kind", asset.value("kind").toString()}});
    result.insert("assets", files);
    return result;
}

/** @brief 更新批次状态和可展示结果。 */
void WebImports::setState(const QString &id, const QString &state, const QJsonObject &result)
{
    WebSql::execute(
        QStringLiteral(
            "UPDATE web_person_import_batches SET state=?,result_json=?,updated_at=? WHERE id=?"),
        {state, WebSql::json(result), QDateTime::currentMSecsSinceEpoch(), id});
}

/** @brief 记录已接收的批次文件。 */
void WebImports::addAsset(const QVariantMap &task)
{
    WebSql::execute(
        QStringLiteral(
            "INSERT OR IGNORE INTO web_person_import_assets(id,batch_id,kind,original_name,path) "
            "VALUES(?,?,?,?,?)"),
        {task.value("id"),
         task.value("batch_id"),
         task.value("kind"),
         task.value("original_name"),
         task.value("final_path")});
}

/** @brief 读取批次中已上传的文件记录。 */
QList<QVariantMap> WebImports::assets(const QString &id)
{
    return WebSql::query(QStringLiteral("SELECT * FROM web_person_import_assets WHERE batch_id=? "
                                        "ORDER BY original_name COLLATE NOCASE"),
                         {id});
}

/** @brief 标记重启前未完成的导入批次。 */
void WebImports::recover()
{
    WebSql::execute(
        QStringLiteral("UPDATE web_person_import_batches SET "
                       "state='RECOVERY_REQUIRED',result_json='{\"message\":"
                       "\"导入时服务中断，请核对已入库人员；禁止自动重放本批次\"}' WHERE state IN "
                       "('IMPORTING','VALIDATING_FACES','SAVING_FACES')"));
}
