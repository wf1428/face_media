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

/**
 * @brief 保存由 Qt 生成的受控上传任务和文件路径。
 * @param task 持久化上传任务记录。
 */
void create(const QVariantMap &task);

/**
 * @brief 读取记录并校验其存在性及账号归属。
 * @param id 目标记录或任务的标识。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @return 任务行；必须属于指定用户，owner=0 仅用于内部恢复。
 */
QVariantMap get(const QString &id, int owner = 0);

/**
 * @brief 记录状态、实际接收大小和安全错误说明。
 * @param id 目标记录或任务的标识。
 * @param state 实际执行状态。
 * @param received 已实际接收的字节数。
 * @param error 接收失败原因；允许为空时不写入错误文本。
 */
void setState(const QString &id,
              const QString &state,
              qint64 received = 0,
              const QString &error = QString());

/**
 * @brief 读取需要启动恢复核对的未完成上传任务。
 * @return 尚未终止的上传任务，供服务重启处理。
 */
QList<QVariantMap> incomplete();

/**
 * @brief 统计指定类型未完成上传的空间预留。
 * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
 * @return 所有在途任务未写入部分的配额预留字节数。
 */
qint64 reserved(const QString &kind);
} // namespace WebUploads
