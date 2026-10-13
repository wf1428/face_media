/**
 * @file ic_offline_checker.h
 * @brief IC 卡片离线权限校验器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_OFFLINE_CHECKER_H
#define IC_OFFLINE_CHECKER_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QCryptographicHash>

/**
 * @brief IC 卡片离线权限校验器。
 *
 * 根据 103/115 字节协议分别检查有效期、密钥、次数、楼层位图以及星期时段，
 * 成功时生成 36 字节 RS485 控制帧。所有失败路径只返回原因码，不执行硬件动作。
 */
class OfflineChecker
{
public:

    /** @brief 支持的离线卡片帧类型。 */
    enum class CardType {
        Unknown = 0,
        Visitor103,
        Multi115
    };

    /** @brief 离线校验结果和放行控制参数。 */
    struct Result {
        CardType type = CardType::Unknown; /**< 由帧长度识别的卡类型。 */
        bool pass = false;                 /**< 全部校验通过时为 true。 */
        QString reason;                    /**< "ok" 或稳定的英文失败原因码。 */
        QByteArray fcbbuf36;               /**< 放行时的 "#@" + frame[2..33] + "#!"。 */
        int relayNum = -1;                 /**< 两位十进制 ASCII 继电器编号。 */
        int relayTimes = -1;               /**< 两位十六进制 ASCII 继电器次数。 */
    };

    /** @brief 按帧长度分派到访客卡或多层卡离线校验。 */
    static Result check(const QByteArray& frame,
                        int floorNum,
                        const QByteArray& secret8,
                        const QDateTime& now);

    /** @brief 校验 115 字节多层卡的密钥、楼层和两个星期时段。 */
    static Result checkMulti115(const QByteArray& frame,
                                int floorNum,
                                const QByteArray& secret8,
                                const QDateTime& now);

    /** @brief 校验 103 字节访客卡的有效期、密钥、次数和楼层权限。 */
    static Result checkVisitor103(const QByteArray& frame,
                                  int floorNum,
                                  const QByteArray& secret8,
                                  const QDateTime& now);

private:
    /** @return 单个十六进制 ASCII 字符的数值；非法时返回 -1。 */
    static int hexNibble(char c);

    /** @return 两位十六进制 ASCII 的数值；非法时返回 -1。 */
    static int parseAsciiHex2(char a, char b);

    /** @return 两位十进制 ASCII 的数值；非法时返回 -1。 */
    static int parseAsciiDec2(char a, char b);

    /** @return 从 idx 开始的两位十六进制 ASCII 数值；越界时返回 -1。 */
    static int parseAsciiHex2At(const QByteArray& f, int idx);

    /** @return 从 idx 开始的两位十进制 ASCII 数值；越界时返回 -1。 */
    static int parseAsciiDec2At(const QByteArray& f, int idx);

    /** @brief 按旧 STM32 算法将 'A' 以上字符减 7，供低位权限位运算使用。 */
    static char normalizeHexCharLikeStm(char c);

    /** @brief 从 baseIdx 起始的每字符四楼层位图中判断目标楼层权限。 */
    static bool floorPermit(const QByteArray& f, int floorNum, int baseIdx);

    /** @brief 按协议的两半字节星期位图判断当前星期是否允许。 */
    static bool weekPermit(int dayOfWeek_1to7, int w0, int w1);

    /** @brief 判断当前时间是否落在普通或跨午夜的闭区间内。 */
    static bool timePermit(int nowH, int nowM, int sh, int sm, int eh, int em);

    /**
     * @return 按旧协议将两个 ASCII 十六进制字符解析为数值；失败返回 -1。
     * @note 名称沿用旧 Core 的 trans()，实际实现调用十六进制解析。
     */
    static int  trans2digits(const QByteArray& f, int idx);

    /** @brief 按两位年/月/日/时/分逐字段判断访客卡是否尚未过期。 */
    static bool visitorNotExpired(const QByteArray& frame, const QDateTime& now);


};

#endif // IC_OFFLINE_CHECKER_H
