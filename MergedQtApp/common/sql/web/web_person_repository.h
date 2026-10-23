/**
 * @file web_person_repository.h
 * @brief 网页本地人员资料只读仓储，不提供 JSON 人员写入或凭证控制。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include <QJsonObject>

namespace WebPeople {

/**
 * @brief 读取现有本地人员的公开字段。
 * @return 本地人员基本清单；不读取密码或私有凭证。
 */
QJsonObject list();

/**
 * @brief 校验人员主键并读取有效的本地人员资料。
 * @param id 目标记录或任务的标识。
 * @return 指定本地人员基本资料。
 */
QJsonObject get(const QString &id);

/**
 * @brief 校验原冲突查询依赖列可读；异常结构报错，不能把 SQL 错误当作无重复。
 */
void verifyConflictSources();
} // namespace WebPeople
