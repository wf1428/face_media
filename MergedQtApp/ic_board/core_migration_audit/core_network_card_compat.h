/**
 * @file core_network_card_compat.h
 * @brief 复现旧 Core 在线卡片兼容判定的命名空间。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef CORE_NETWORK_CARD_COMPAT_H
#define CORE_NETWORK_CARD_COMPAT_H

#include <QByteArray>
#include <QDateTime>
#include <QString>

/**
 * @brief 复现旧 Core 在线卡片兼容判定的命名空间。
 *
 * 该兼容路径按卡类型校验有效期或周/时段，再检查本地注册白名单；
 * 按旧实现约定，不执行密钥和楼层位图校验。
 */
namespace CoreNetworkCardCompat {

/** @brief 在线兼容判定结果及放行所需的 RS485 控制参数。 */
struct Result {
    bool pass = false;       /**< 所有兼容校验通过时为 true。 */
    QString reason;          /**< "pass" 或稳定的英文失败原因码。 */
    QString cardId;          /**< frame[35..42] 解析并去除首尾空白后的卡号。 */
    QByteArray fcbbuf36;     /**< 放行时生成的 36 字节 "#@"..."#!" 控制帧。 */
    int relayNum = -1;       /**< 两位十进制 ASCII 继电器编号；解析失败为 -1。 */
    int relayTimes = -1;     /**< 两位十六进制 ASCII 继电器次数；解析失败为 -1。 */
};

/**
 * @brief 按旧 Core 在线兼容规则检查 103/115 字节卡片帧。
 * @param frame 以 "#@" 开头的完整卡片帧。
 * @param now 当前本地日期时间，用于有效期、星期和时段判断。
 * @return 判定结果；失败时 pass 为 false 并给出 reason。
 */
Result check(const QByteArray &frame, const QDateTime &now);

} // namespace CoreNetworkCardCompat

#endif // CORE_NETWORK_CARD_COMPAT_H
