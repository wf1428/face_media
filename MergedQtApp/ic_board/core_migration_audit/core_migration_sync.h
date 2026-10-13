/**
 * @file core_migration_sync.h
 * @brief 旧 Core 数据模型与当前 Qt 配置存储之间的兼容工具。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef CORE_MIGRATION_SYNC_H
#define CORE_MIGRATION_SYNC_H

#include <QByteArray>
#include <QString>

/**
 * @brief 旧 Core 数据模型与当前 Qt 配置存储之间的兼容工具。
 *
 * 提供卡片白名单键、注册计数兼容字段、楼层位图文本和 RS485 诊断文本转换。
 */
class CoreMigrationSync {
public:
    /** @return 去除首尾空白后的卡号。 */
    static QString normalizeCardId(const QString &cardId);

    /** @return 标准化卡号非空且对应白名单配置键存在时返回 true。 */
    static bool isRegisteredCard(const QString &cardId);

    /** @brief 注册卡号，并在首次注册时同步两个历史计数字段。 */
    static bool registerCard(const QString &cardId);

    /** @brief 删除卡号，并在卡号原本存在时递减兼容计数且不低于 0。 */
    static bool deleteCard(const QString &cardId);

    /** @return 当前 128 位楼层权限的 '0'/'1' 字符串。 */
    static QString floorFlags128String();

    /** @return 可打印 RS485 数据的文本；含二进制字节时返回十六进制字符串。 */
    static QString sanitizeRs485Text(const QByteArray &frame);
};

#endif // CORE_MIGRATION_SYNC_H
