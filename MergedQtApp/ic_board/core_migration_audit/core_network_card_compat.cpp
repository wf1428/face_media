/**
 * @file core_network_card_compat.cpp
 * @brief 复现旧 Core 在线卡片兼容判定的命名空间的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */


#include "core_network_card_compat.h"
#include "core_migration_sync.h"

namespace {

/** @return 单个十六进制 ASCII 字符的数值；非法时返回 -1。 */
int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

/** @return 从指定偏移读取的两位十六进制 ASCII 数值；越界或非法时返回 -1。 */
int parseAsciiHex2At(const QByteArray &f, int idx)
{
    if (idx < 0 || idx + 1 >= f.size()) return -1;
    const int h = hexNibble(f[idx]);
    const int l = hexNibble(f[idx + 1]);
    if (h < 0 || l < 0) return -1;
    return h * 16 + l;
}

/** @return 从指定偏移读取的两位十进制 ASCII 数值；越界或非法时返回 -1。 */
int parseAsciiDec2At(const QByteArray &f, int idx)
{
    if (idx < 0 || idx + 1 >= f.size()) return -1;
    const char a = f[idx];
    const char b = f[idx + 1];
    if (a < '0' || a > '9' || b < '0' || b > '9') return -1;
    return (a - '0') * 10 + (b - '0');
}

/** @brief 复现旧 Core 对星期半字节字符先减 '0'、大于 9 再减 7 的转换。 */
int parseWeekNibbleLikeCore(char c)
{
    int v = c - '0';
    if (v > 9) v -= 7; // Core: 'A'..'F' 再减7
    return v;
}

/** @brief 按旧协议分散在两个半字节中的星期权限判断指定星期。 */
bool weekPermitLikeCore(int dayOfWeek1to7, int w0, int w1)
{
    switch (dayOfWeek1to7) {
    case 1: return ((w1 >> 2) & 0x01) != 0; // Mon
    case 2: return ((w1 >> 1) & 0x01) != 0; // Tue
    case 3: return ((w1 >> 0) & 0x01) != 0; // Wed
    case 4: return ((w0 >> 3) & 0x01) != 0; // Thu
    case 5: return ((w0 >> 2) & 0x01) != 0; // Fri
    case 6: return ((w0 >> 1) & 0x01) != 0; // Sat
    case 7: return ((w0 >> 0) & 0x01) != 0; // Sun
    default: return false;
    }
}


/** @brief 判断当前时间是否位于普通或跨午夜的闭区间内。 */
bool timePermitLikeCore(int nowH, int nowM, int sh, int sm, int eh, int em)
{
//    if ((nowH > sh) && (nowH < eh)) return true;
//    if (nowH == sh) return (nowM >= sm) && (nowM <= em);
//    return false;

    // 转成分钟再比较
    const int now = nowH * 60 + nowM;
    const int start = sh * 60 + sm;
    const int end = eh * 60 + em;

    // 00:00 ~ 00:00 为全天开放
//    if (start == 0 && end == 0) {
//        return true;
//    }

    // 同一天区间：如 09:00 ~ 18:30
    if (start <= end) {
        return now >= start && now <= end;
    }

    // 跨天区间：如 23:00 ~ 02:00
    return now >= start || now <= end;

}




/** @return "#@"、源帧 [2..33] 和 "#!" 组成的固定 36 字节控制帧。 */
QByteArray buildFcbbuf36(const QByteArray &frame)
{
    QByteArray out(36, '0');
    out[0] = '#';
    out[1] = '@';
    out[34] = '#';
    out[35] = '!';
    if (frame.size() >= 34) {
        for (int i = 0; i < 32; ++i) out[2 + i] = frame[2 + i];
    }
    return out;
}

} // namespace

namespace CoreNetworkCardCompat {


/**
 * @brief 执行旧 Core 在线兼容判定。
 *
 * 访客卡检查有效期，多层卡检查两个星期时段，随后统一检查本地白名单；
 * 按兼容要求不检查密钥和楼层位图。
 */
Result check(const QByteArray &frame, const QDateTime &now)
{
    Result r;

    if (frame.size() < 2 || frame[0] != '#' || frame[1] != '@') {
        r.reason = "frame_invalid";
        return r;
    }

    const int len = frame.size();
    if (len != 103 && len != 115) {
        r.reason = "frame_len_unsupported";
        return r;
    }

    r.cardId = CoreMigrationSync::normalizeCardId(QString::fromLatin1(frame.mid(35, 8)));
    if (r.cardId.isEmpty()) {
        r.reason = "card_id_invalid";
        return r;
    }

    const int nowYY = now.date().year() % 100;
    const int nowMM = now.date().month();
    const int nowDD = now.date().day();
    const int nowHH = now.time().hour();
    const int nowMI = now.time().minute();

    bool permit = false;
    if (len == 103) {
        // Core swipe_card_task: 仅做“次数卡有效期”判断 + 注册卡判断
        const int fy = parseAsciiHex2At(frame, 44);
        const int fm = parseAsciiHex2At(frame, 46);
        const int fd = parseAsciiHex2At(frame, 48);
        const int fh = parseAsciiHex2At(frame, 50);
        const int fmin = parseAsciiHex2At(frame, 52);
        if (fy < 0 || fm < 0 || fd < 0 || fh < 0 || fmin < 0) {
            r.reason = "visitor_time_parse_fail";
            return r;
        }

        if (nowYY < fy) permit = true;
        else if (nowYY == fy) {
            if (nowMM < fm) permit = true;
            else if (nowMM == fm) {
                if (nowDD < fd) permit = true;
                else if (nowDD == fd) {
                    if (nowHH < fh) permit = true;
                    else if (nowHH == fh) {
                        if (nowMI <= fmin) permit = true;
                    }
                }
            }
        }

        r.relayTimes = parseAsciiHex2At(frame, 99);
        r.relayNum = parseAsciiDec2At(frame, 101);
    } else {
        // Core swipe_card_task: 多层卡按周/时段判断 + 注册卡判断，不做密钥/楼层位校验
        const int w1_0 = parseWeekNibbleLikeCore(frame[45]);
        const int w1_1 = parseWeekNibbleLikeCore(frame[47]);
        const int w2_0 = parseWeekNibbleLikeCore(frame[57]);
        const int w2_1 = parseWeekNibbleLikeCore(frame[59]);

        const int t1_sh = parseAsciiHex2At(frame, 48);
        const int t1_sm = parseAsciiHex2At(frame, 50);
        const int t1_eh = parseAsciiHex2At(frame, 52);
        const int t1_em = parseAsciiHex2At(frame, 54);

        const int t2_sh = parseAsciiHex2At(frame, 60);
        const int t2_sm = parseAsciiHex2At(frame, 62);
        const int t2_eh = parseAsciiHex2At(frame, 64);
        const int t2_em = parseAsciiHex2At(frame, 66);

        if (t1_sh < 0 || t1_sm < 0 || t1_eh < 0 || t1_em < 0 ||
            t2_sh < 0 || t2_sm < 0 || t2_eh < 0 || t2_em < 0) {
            r.reason = "multi_time_parse_fail";
            return r;
        }

        const int dow = now.date().dayOfWeek(); // 1..7
        const bool pass1 = weekPermitLikeCore(dow, w1_0, w1_1) &&
                           timePermitLikeCore(nowHH, nowMI, t1_sh, t1_sm, t1_eh, t1_em);
        const bool pass2 = weekPermitLikeCore(dow, w2_0, w2_1) &&
                           timePermitLikeCore(nowHH, nowMI, t2_sh, t2_sm, t2_eh, t2_em);
        permit = pass1 || pass2;

        // Core: 4个星期位均为'0'，视为“无时间限制”
        if (frame[45] == '0' && frame[47] == '0' &&
            frame[57] == '0' && frame[59] == '0') {
            permit = true;
        }

        r.relayTimes = parseAsciiHex2At(frame, 111);
        r.relayNum = parseAsciiDec2At(frame, 113);
    }

    if (!permit) {
        r.reason = "time_deny";
        return r;
    }

    // 白名单检查
    if (!CoreMigrationSync::isRegisteredCard(r.cardId)) {
        r.reason = "card_unregistered";
        return r;
    }

    r.fcbbuf36 = buildFcbbuf36(frame);
    r.pass = true;
    r.reason = "pass";
    return r;
}

} // namespace CoreNetworkCardCompat
