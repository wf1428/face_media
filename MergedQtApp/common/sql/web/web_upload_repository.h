/**
 * @file web_upload_repository.h
 * @brief Web 上传任务持久化，状态转换与恢复查询。
 * @author Du lin
 * @date 2026-10-09
 */
#pragma once
#include <QVariantMap>
#include <QList>
#include <QString>
namespace WebUploads {
/** @brief 保存由 Qt 生成的受控上传任务和文件路径。 */
void create(const QVariantMap &task);
/** @return 任务行；必须属于指定用户，owner=0 仅用于内部恢复。 */
QVariantMap get(const QString &id, int owner = 0);
/** @brief 记录状态、实际接收大小和安全错误说明。 */
void setState(const QString &id, const QString &state, qint64 received = 0, const QString &error = QString());
/** @return 尚未终止的上传任务，供服务重启处理。 */
QList<QVariantMap> incomplete();
/** @return 所有在途任务未写入部分的配额预留字节数。 */
qint64 reserved(const QString &kind);
}
