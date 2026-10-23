/**
 * @file web_upload_repository.cpp
 * @brief 上传表读写；流式传输期间不占用数据库事务。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-23；修改人：Du lin；人员图片归档纳入原人员上传预留容量统计，保留原资源包独立预算。
 */

#include "web_upload_repository.h"
#include "web_sql.h"
#include <QDateTime>
#include <QFileInfo>

/**
 * @brief 保存新上传任务及受控路径。
 * @param task 持久化上传任务记录。
 */
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

/**
 * @brief 按任务编号与账号归属读取上传状态。
 * @param id 目标记录或任务的标识。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @return 所属账号的上传任务；owner=0 用于内部恢复，异常时抛出 WebError。
 */
QVariantMap WebUploads::get(const QString &id, int owner)
{
    const auto rows =
        WebSql::query(QStringLiteral("SELECT * FROM web_upload_tasks WHERE id=?"), {id});
    if (rows.isEmpty() || (owner && rows.first().value("owner_id").toInt() != owner))
        throw WebError(404, QStringLiteral("上传任务不存在"));
    return rows.first();
}

/**
 * @brief 更新上传任务状态和已接收字节数。
 * @param id 目标记录或任务的标识。
 * @param state 实际执行状态。
 * @param received 已实际接收的字节数。
 * @param error 接收失败原因；允许为空时不写入错误文本。
 */
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

/**
 * @brief 查询尚未完成或取消的上传任务。
 * @return 尚未进入终态的上传记录列表。
 */
QList<QVariantMap> WebUploads::incomplete()
{
    return WebSql::query(QStringLiteral(
        "SELECT * FROM web_upload_tasks WHERE state NOT IN ('READY','FAILED','CANCELLED')"));
}

/**
 * @brief 统计未完成上传占用的预留容量。
 * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
 * @return 该资源类型尚未完成任务的预留字节数。
 */
qint64 WebUploads::reserved(const QString &kind)
{
    qint64 bytes = 0;
    for (const auto &task : incomplete()) {
        const QString taskKind = task.value("kind").toString();
        // 资源包使用独立的容量预算，不挤占原有人员 Excel/图片配额。
        if (kind == "video" ? (taskKind != "video" && taskKind != "ad_image")
                            : (taskKind != "person_excel" && taskKind != "person_face" && taskKind != "person_face_archive"))
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
