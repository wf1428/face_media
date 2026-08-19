/**
 * @file core_migration_sync.cpp
 * @brief 旧 Core 数据模型与当前 Qt 配置存储之间的兼容工具的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "core_migration_sync.h"

#include <QChar>
#include <QtGlobal>

#include "common/sql/dbstore.h"
#include "ic_board/device_config_sync.h"

namespace {
/** @return 卡号在 config 表中的白名单键名。 */
QString keyForCard(const QString &cardId)
{
    return QString("card_allow_%1").arg(cardId);
}

/** @brief 同步旧版与新版使用的两个注册卡计数字段。 */
void syncRegisterCountCompat(int count)
{
    DbStore::setConfig("register_times", count);
    DbStore::setConfig("registered_card_count", count);
}
} // namespace


/** @return 去除首尾空白后的卡号；不改变卡号内部字符。 */
QString CoreMigrationSync::normalizeCardId(const QString &cardId)
{
    return cardId.trimmed();
}


/** @return 标准化卡号非空且白名单键存在时返回 true。 */
bool CoreMigrationSync::isRegisteredCard(const QString &cardId)
{
    const QString normalized = normalizeCardId(cardId);
    if (normalized.isEmpty()) return false;
    if (DbStore::hasConfig(keyForCard(normalized))) {
        return true;
    }

    // 网络同步卡片与离线兼容白名单共用统一 SQLite，但分表保存。
    // 只允许仍处于有效状态的网络人员卡片通过。
    const QList<QVariantMap> rows = DbStore::query(
                QStringLiteral(
                    "SELECT 1 FROM network_ic_card c "
                    "JOIN network_person p ON p.person_id=c.person_id "
                    "WHERE c.card_id=? AND c.active=1 AND p.active=1 "
                    "AND p.deleted=0 AND p.status=1 LIMIT 1"),
                {normalized});
    return !rows.isEmpty();
}


/** @brief 写入白名单键；仅首次出现该键时增加兼容注册计数。 */
bool CoreMigrationSync::registerCard(const QString &cardId)
{
    const QString normalized = normalizeCardId(cardId);
    if (normalized.isEmpty()) return false;

    const QString key = keyForCard(normalized);
    if (!DbStore::hasConfig(key)) {
        const int oldCount = DbStore::getConfig("register_times", 0).toInt();
        syncRegisterCountCompat(oldCount + 1);
    }
    return DbStore::setConfig(key, 1);
}


/** @brief 删除白名单键；仅键原本存在时减少兼容注册计数。 */
bool CoreMigrationSync::deleteCard(const QString &cardId)
{
    const QString normalized = normalizeCardId(cardId);
    if (normalized.isEmpty()) return false;

    const QString key = keyForCard(normalized);
    if (DbStore::hasConfig(key)) {
        const int oldCount = DbStore::getConfig("register_times", 0).toInt();
        syncRegisterCountCompat(qMax(0, oldCount - 1));
    }
    return DbStore::deleteConfig(key);
}

/** @return 当前 128 位楼层权限的 '0'/'1' 字符串。 */
QString CoreMigrationSync::floorFlags128String()
{
    return QString::fromLatin1(DeviceConfigSync::floorFlags128Bits());
}

/** @return 可打印 RS485 数据的文本；含二进制字节时返回十六进制字符串。 */
QString CoreMigrationSync::sanitizeRs485Text(const QByteArray &frame)
{
    if (frame.isEmpty()) return {};

    QByteArray raw = frame;
    const int nulPos = raw.indexOf('\0');
    if (nulPos >= 0) {
        raw = raw.left(nulPos);
    }

    while (!raw.isEmpty() && (raw.endsWith('\r') || raw.endsWith('\n'))) {
        raw.chop(1);
    }

    if (raw.isEmpty()) return {};

    bool printable = true;
    for (char ch : raw) {
        const uchar uc = static_cast<uchar>(ch);
        if (uc == '\t') continue;
        if (uc < 0x20 || uc >= 0x7F) {
            printable = false;
            break;
        }
    }

    if (printable) {
        return QString::fromLatin1(raw).trimmed();
    }
    return QString::fromLatin1(raw.toHex());
}
