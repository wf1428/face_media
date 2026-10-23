/**
 * @file web_sql.h
 * @brief Web 仓储共用的 SQL 错误、绑定和 JSON 转换；不持有跨线程连接。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include "common/sql/dbstore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

/** @brief 可安全返回客户端的错误；内部 SQL 细节仅写入日志。 */
struct WebError : std::runtime_error {
    int status;      /**< HTTP 对应状态码。 */
    QString message; /**< 中文错误说明。 */

    /**
     * @brief 构造业务错误。
     * @param code 映射到 HTTP 的错误状态码。
     * @param text 可返回 Web 调用方的业务错误说明。
     */
    WebError(int code, const QString &text)
        : std::runtime_error(text.toStdString()), status(code), message(text)
    {
    }
};

namespace WebSql {

/**
 * @brief 执行绑定 SQL，失败立即中断当前 Web 操作。
 * @param sql 带占位符的 SQL 语句。
 * @param values 按占位符顺序绑定的参数。
 * @note 数据库连接由当前 Web 工作线程持有；SQL 失败转换为 503 WebError。
 */
inline void execute(const QString &sql, const QList<QVariant> &values = {})
{
    if (!DbStore::execute(sql, values))
        throw WebError(503, QStringLiteral("数据库操作失败，请稍后重试"));
}

/**
 * @brief 执行带绑定参数的 SQL 查询并立即检查错误。
 * @param sql 带占位符的 SQL 语句。
 * @param values 按占位符顺序绑定的参数。
 * @return 查询结果；每次查询清空旧错误，避免误报。
 * @note 数据库连接由当前 Web 工作线程持有；SQL 失败转换为 503 WebError。
 */
inline QList<QVariantMap> query(const QString &sql, const QList<QVariant> &values = {})
{
    DbStore::m_lastError.clear();
    auto rows = DbStore::query(sql, values);
    if (!DbStore::lastError().isEmpty())
        throw WebError(503, QStringLiteral("数据库查询失败，请稍后重试"));
    return rows;
}

/**
 * @brief 将业务对象编码为紧凑 JSON 文本。
 * @param value 待序列化的 JSON 对象。
 * @return JSON 的紧凑 UTF-8 文本。
 */
inline QString json(const QJsonObject &value)
{
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

/**
 * @brief 将单行数据库字段映射为 JSON 对象。
 * @param row 数据库单行字段。
 * @return SQLite 行转换的 JSON；64 位数量使用 Qt JSON 可精确的设备配额范围。
 */
inline QJsonObject object(const QVariantMap &row)
{
    return QJsonObject::fromVariantMap(row);
}

/**
 * @brief 按原顺序将多行数据库结果转换为 JSON 数组。
 * @param rows 数据库查询结果行。
 * @return 行列表转换的 JSON 数组。
 */
inline QJsonArray array(const QList<QVariantMap> &rows)
{
    QJsonArray result;
    for (const auto &row : rows)
        result.append(object(row));
    return result;
}
} // namespace WebSql
