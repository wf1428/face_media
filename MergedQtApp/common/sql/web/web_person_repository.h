/**
 * @file web_person_repository.h
 * @brief 网页本地人员资料只读仓储，不提供 JSON 人员写入或凭证控制。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include <QJsonObject>

namespace WebPeople {

/** @return 本地人员基本清单；不读取密码或私有凭证。 */
QJsonObject list();

/** @return 指定本地人员基本资料。 */
QJsonObject get(const QString &id);

/** @brief 校验原冲突查询依赖列可读；异常结构报错，不能把 SQL 错误当作无重复。 */
void verifyConflictSources();
} // namespace WebPeople
