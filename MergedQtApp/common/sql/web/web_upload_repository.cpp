/**
 * @file web_upload_repository.cpp
 * @brief 上传表读写；流式传输期间不占用数据库事务。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_upload_repository.h"
#include "web_sql.h"
#include <QDateTime>
#include <QFileInfo>

/** @brief 保存新上传任务及受控路径。 */
void WebUploads::create(const QVariantMap &task)
{
    WebSql::execute(
        QStringLiteral(
            "INSERT INTO "
            "web_upload_tasks(id,owner_id,kind,batch_id,original_name,expected_bytes,state,part_"
            "path,final_path,updated_at) VALUES(?,?,?,?,?,?,'CREATED',?,?,?)"),
        {task.value("id"),
         task.value("owner_id"),
         task.value("kind"),
         task.value("batch_id"),
         task.value("original_name"),
         task.value("expected_bytes"),
         task.value("part_path"),
         task.value("final_path"),
         QDateTime::currentMSecsSinceEpoch()});
}

/** @brief 按任务编号与账号归属读取上传状态。 */
QVariantMap WebUploads::get(const QString &id, int owner)
{
    const auto rows =
        WebSql::query(QStringLiteral("SELECT * FROM web_upload_tasks WHERE id=?"), {id});
    if (rows.isEmpty() || (owner && rows.first().value("owner_id").toInt() != owner))
        throw WebError(404, QStringLiteral("上传任务不存在"));
    return rows.first();
}

/** @brief 更新上传任务状态和已接收字节数。 */
void WebUploads::setState(const QString &id,
                          const QString &state,
                          qint64 received,
                          const QString &error)
{
    WebSql::execute(
        QStringLiteral(
            "UPDATE web_upload_tasks SET state=?,received_bytes=?,error=?,updated_at=? WHERE id=?"),
        {state,
         received,
         error.isNull() ? QStringLiteral("") : error,
         QDateTime::currentMSecsSinceEpoch(),
         id});
}

/** @brief 查询尚未完成或取消的上传任务。 */
QList<QVariantMap> WebUploads::incomplete()
{
    return WebSql::query(QStringLiteral(
        "SELECT * FROM web_upload_tasks WHERE state NOT IN ('READY','FAILED','CANCELLED')"));
}

/** @brief 统计未完成上传占用的预留容量。 */
qint64 WebUploads::reserved(const QString &kind)
{
    qint64 bytes = 0;
    for (const auto &task : incomplete()) {
        const QString taskKind = task.value("kind").toString();
        // 资源包使用独立的容量预算，不挤占原有人员 Excel/图片配额。
        if (kind == "video" ? taskKind != "video"
                            : (taskKind != "person_excel" && taskKind != "person_face"))
            continue;
        const QFileInfo part(task.value("part_path").toString()),
            final(task.value("final_path").toString());
        const qint64 written =
            part.exists()
                ? part.size()
                : (task.value("state") == "COMMITTING" && final.exists() ? final.size() : 0);
        bytes += qMax<qint64>(0, task.value("expected_bytes").toLongLong() - written);
    }
    return bytes;
}
