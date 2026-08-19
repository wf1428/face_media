/**
 * @file ic_offline_checker.cpp
 * @brief IC 卡片离线权限校验器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_offline_checker.h"
#include "common/debug/probe_log.h"

#include <QDebug>

static constexpr int kLenMulti = 115;       //离线多卡数据帧固定长度, 且前两个字符为 "#@"
static constexpr int kLenVisitor = 103;     // 离线访客二维码固定帧长度。

/* =========================
 *  基础字符解析工具函数
 * ========================= */

/**
 * @brief 将单个 ASCII 十六进制字符转换为 0~15 的数值
 * @param c 输入字符：'0'~'9' 'A'~'F' 'a'~'f'
 * @return 成功：0~15；失败：-1
 * @note 仅负责单个 nibble（4bit）解析，不负责校验其它范围
 */
int OfflineChecker::hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

/**
 * @brief 解析两个 ASCII 十六进制字符为 1 字节数值
 * @param a 高半字节字符
 * @param b 低半字节字符
 * @return 成功：0~255；失败：-1
 */
int OfflineChecker::parseAsciiHex2(char a, char b)
{
    int hi = hexNibble(a);
    int lo = hexNibble(b);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/**
 * @brief 解析两个 ASCII 十进制字符为 0~99
 * @param a 十位字符 '0'~'9'
 * @param b 个位字符 '0'~'9'
 * @return 成功：0~99；失败：-1
 * @note 该函数严格要求两个字符均为数字，不接受空格/其它符号
 */
int OfflineChecker::parseAsciiDec2(char a, char b)
{
    if (a < '0' || a > '9') return -1;
    if (b < '0' || b > '9') return -1;
    return (a - '0') * 10 + (b - '0');
}

/**
 * @brief 从 QByteArray 指定位置解析两个 ASCII 十六进制字符
 * @param f 数据帧
 * @param idx 第一个字符索引（会读取 idx 和 idx+1）
 * @return 成功：0~255；失败：-1（越界或非 hex 字符）
 */
int OfflineChecker::parseAsciiHex2At(const QByteArray& f, int idx)
{
    if (idx + 1 >= f.size()) return -1;
    return parseAsciiHex2(f[idx], f[idx + 1]);
}

/**
 * @brief 从 QByteArray 指定位置解析两个 ASCII 十进制字符
 * @param f 数据帧
 * @param idx 第一个字符索引（会读取 idx 和 idx+1）
 * @return 成功：0~99；失败：-1（越界或非数字字符）
 */
int OfflineChecker::parseAsciiDec2At(const QByteArray& f, int idx)
{
    if (idx + 1 >= f.size()) return -1;
    return parseAsciiDec2(f[idx], f[idx + 1]);
}

/**
 * @brief 将“特殊编码字符”归一化为可按位运算的数值字符
 * @param c 输入字符
 * @return 归一化后的字符
 */
char OfflineChecker::normalizeHexCharLikeStm(char c)
{
    // STM32: if (buf[x] > '9') buf[x] -= 7;
    if (c > '9') return static_cast<char>(c - 7);       // 'A'以上减7
    return c;
}

/* =========================
 *  权限判断：楼层/星期/时间
 * ========================= */

/**
 * @brief 多卡协议楼层权限判断
 * @param f 完整数据帧（长度 kLenMulti）
 * @param floorNum 楼层号（从 1 开始）
 * @return true 允许；false 禁止（含越界/参数非法）
 *
 * @details
 * 协议约定楼层权限位图从索引 78 开始，每个字符承载 4 个楼层的权限位：
 *   - floorNum=1..4  -> f[78]
 *   - floorNum=5..8  -> f[79]
 *   - ...
 *
 * 在一个字节/字符内，四个楼层对应 bit3~bit0：
 *   mod=0 -> bit3 (0x08)  对应该组第1个楼层
 *   mod=1 -> bit2 (0x04)  对应该组第2个楼层
 *   mod=2 -> bit1 (0x02)  对应该组第3个楼层
 *   mod=3 -> bit0 (0x01)  对应该组第4个楼层
 */
bool OfflineChecker::floorPermit(const QByteArray& f, int floorNum, int baseIdx)
{
    if (floorNum <= 0) return false;

    // floor_bit：决定楼层分组（每 4 层一组），例如 1..4 -> 0, 5..8 -> 1
    int floor_bit = (floorNum - 1) / 4;

    // 协议：multi 楼层权限位图起始索引baseIdx为 78
    int idx = baseIdx + floor_bit;

    if (idx < 0 || idx >= f.size()) return false;

    // v：当前 4 楼一组的权限位
    char v = normalizeHexCharLikeStm(f[idx]);

    // mod：该组内部的第几个楼层（0~3）
    int mod = (floorNum - 1) % 4;

    switch (mod) {
        case 0: return (v & 0x08) != 0; // 该组第1层（1楼/5楼/...）
        case 1: return (v & 0x04) != 0; // 该组第2层
        case 2: return (v & 0x02) != 0; // 该组第3层
        case 3: return (v & 0x01) != 0; // 该组第4层
        default: return false;
    }
}

/**
 * @brief 星期权限判断（两字节/两半字节位图）
 * @param dayOfWeek_1to7 Qt 规范：1=Mon ... 7=Sun
 * @param w0 周四~周日的位图（四五六日 8421）
 * @param w1 周一~周三的位图（一二三 0421）
 * @return true/false
 *
 * @details
 *   周一 -> (w1 >> 2)
 *   周二 -> (w1 >> 1)
 *   周三 -> (w1 >> 0)
 *   周四 -> (w0 >> 3)
 *   周五 -> (w0 >> 2)
 *   周六 -> (w0 >> 1)
 *   周日 -> (w0 >> 0)
 */
bool OfflineChecker::weekPermit(int dayOfWeek_1to7, int w0, int w1)
{
    // Qt: 1=Mon ... 7=Sun
    switch (dayOfWeek_1to7) {
        case 1: return ((w1 >> 2) & 0x01) != 0; // 周一
        case 2: return ((w1 >> 1) & 0x01) != 0; // 周二
        case 3: return ((w1 >> 0) & 0x01) != 0; // 周三
        case 4: return ((w0 >> 3) & 0x01) != 0; // 周四
        case 5: return ((w0 >> 2) & 0x01) != 0; // 周五
        case 6: return ((w0 >> 1) & 0x01) != 0; // 周六
        case 7: return ((w0 >> 0) & 0x01) != 0; // 周日
        default: return false;
    }
}

/**
 * @brief 时间段判断
 * @param nowH 当前小时 0~23
 * @param nowM 当前分钟 0~59
 * @param sh 起始小时
 * @param sm 起始分钟
 * @param eh 结束小时
 * @param em 结束分钟
 * @return true 在允许时间段内；false 不在
 *
 * 普通区间和跨午夜区间都包含起止边界；当起止时间相同时，
 * 按当前实现只允许该分钟，不代表全天开放。
 */
bool OfflineChecker::timePermit(int nowH, int nowM, int sh, int sm, int eh, int em)
{
    // follow the STM32 comparisons
//    if (nowH > sh && nowH < eh) return true;

//    if (nowH == sh) {
//        if (nowM >= sm && nowM <= em) return true;
//    }
//    return false;

    ///20260309 dl

    // 转成分钟再比较
    int now = nowH * 60 + nowM;
    int start = sh * 60 + sm;
    int end = eh * 60 + em;

    // 00:00 ~ 00:00 为全天开放
//    if (start == 0 && end == 0) {
//        return true;
//    }

    // 普通区间：如 09:00 ~ 18:30
    if (start <= end) {
        return now >= start && now <= end;
    }

    // 跨天区间：如 23:00 ~ 02:00
    return now >= start || now <= end;
}
/**
 * @brief 按旧 STM32 trans() 兼容路径解析两个 ASCII 十六进制字符。
 * @param f 帧
 * @param idx 起始索引（读取 idx 和 idx+1）
 * @return 0~255；失败返回 -1
 */
int OfflineChecker::trans2digits(const QByteArray& f, int idx)
{
    // 沿用 STM32 trans() 的命名，但实际字段是两位 ASCII 十六进制数。
    return parseAsciiHex2At(f, idx);
}

/* =========================
 *  主校验函数
 * ========================= */

/*
 * 停用说明：
 * 下方整段 // 代码是早期 115 字节多层卡校验草稿，仅保留作协议迁移对照，
 * 不参与编译，也不是 visitorNotExpired() 的接口注释。实际入口为 check()，
 * 当前多层卡实现见 checkMulti115()。
 *
 * 草稿所记录的校验流程：
 *  0) 基础校验：长度、头部 "#@"
 *  1) 密钥校验：frame[69..76] 与 secret8 完全一致
 *  2) 楼层权限：依据 frame[78..] 的位图判断目标楼层
 *  3) 时间权限：
 *     - 若四个星期字段 frame[45], [47], [57], [59] 全为 '0' -> 表示不限制时间
 *     - 否则：校验两个时间段 slot1/slot2，只要任意一个满足（星期允许 且 时间允许）即可
 *  4) 通过后拼接 fcbbuf36： "#@" + frame[2..33] + "#!"
 *  5) 读取继电器参数：
 *     - relayTimes：frame[111..112] 两位 ASCII HEX
 *     - relayNum  ：frame[113..114] 两位 ASCII DEC
 */

//OfflineChecker::Result OfflineChecker::check(const QByteArray& frame,
//                                                               int floorNum,
//                                                               const QByteArray& secret8,
//                                                               const QDateTime& now)
//{
//    Result r;

//    /* -------- 0) 基础校验 -------- */
//    if (frame.size() != kLenMulti || frame.size() < 2 || frame[0] != '#' || frame[1] != '@') {
//        r.reason = "frame_invalid";
//        return r;
//    }

//    /* -------- 1) 密钥校验：frame[69..76] 共8字节 -------- */
//    if (secret8.size() != 8 || frame.mid(69, 8) != secret8) {
//        r.reason = "secret_fail";
//        return r;
//    }

//    /* -------- 2) 楼层权限校验 -------- */
//    if (!floorPermitMulti(frame, floorNum)) {
//        r.reason = "floor_deny";
//        return r;
//    }

//    /* -------- 3) 时间段校验：slot1/slot2 或不限制 --------
//     *
//     * 索引换算：
//     *   Based on your STM32 indexing: [43+2,45+2,46+2..53+2,55+2,57+2,58+2..65+2]
//     *   -> week1 at [45],[47], time1 at [48..55]
//     *   -> week2 at [57],[59], time2 at [60..67]
//     *
//     * 这里保持当前实现：
//     *   slot1: week bitmap at [45],[47]
//     *         start/end: [48,50,52,54] 各两位（HH,MM,HH,MM）
//     *   slot2: week bitmap at [57],[59]
//     *         start/end: [60,62,64,66]
//     */

//    // slot1 week bitmap
//    // w1_0：周四/五/六/日（8421），w1_1：周一/二/三（0421）
//    int w1_0 = frame[45] - '0'; // 四五六日 8421
//    if (w1_0 > 9) w1_0 -= 7;    // STM32: if >9, -=7（将 A..F 等映射到连续数值）
//    int w1_1 = frame[47] - '0'; // 一二三 0421

//    // slot1 start/end（每项两位 ASCII 十进制）
//    int s1_sh = trans2digits(frame, 48); // start hour
//    int s1_sm = trans2digits(frame, 50); // start minute
//    int s1_eh = trans2digits(frame, 52); // end hour
//    int s1_em = trans2digits(frame, 54); // end minute

//    // slot2 week bitmap
//    int w2_0 = frame[57] - '0';
//    if (w2_0 > 9) w2_0 -= 7;
//    int w2_1 = frame[59] - '0';

//    // slot2 start/end
//    int s2_sh = trans2digits(frame, 60);
//    int s2_sm = trans2digits(frame, 62);
//    int s2_eh = trans2digits(frame, 64);
//    int s2_em = trans2digits(frame, 66);

//    // 当前时间
//    const int dow  = now.date().dayOfWeek(); // Qt: 1..7（Mon..Sun）
//    const int nowH = now.time().hour();
//    const int nowM = now.time().minute();

//    /**
//     * @brief 不限制时间判定
//     * 协议约定：若两段的星期字段都为 '0'，表示不做时间/星期限制
//     *
//     * NOTE:
//     * 这里判断的是“原始字符是否为 '0'”，而不是解析后的 w*_*
//     * 也就是说：若字段可能出现 '00'/'0' 以外的表示（例如空格），需要协议进一步明确。
//     */
//    const bool noTimeLimit =
//        (frame[45] == '0' && frame[47] == '0' && frame[57] == '0' && frame[59] == '0');

//    qDebug() << "dow:" << dow
//             << "nowH:" << nowH
//             << "nowM:" << nowM;

//    bool allow = false;
//    if (noTimeLimit) {
//        allow = true;
//    } else {
//        // slot 满足条件：星期允许 AND 时间允许
//        const bool slot1_ok =
//            weekPermit(dow, w1_0, w1_1) &&
//            timePermit(nowH, nowM, s1_sh, s1_sm, s1_eh, s1_em);

//        const bool slot2_ok =
//            weekPermit(dow, w2_0, w2_1) &&
//            timePermit(nowH, nowM, s2_sh, s2_sm, s2_eh, s2_em);

//        // 任意一个时间段满足即可放行
//        allow = slot1_ok || slot2_ok;
//    }

//    if (!allow) {
//        r.reason = "time_deny";
//        return r;
//    }

//    /* -------- 4) 通过后构造 fcbbuf36 --------
//     *
//     * 格式： "#@" + frame[2..33] + "#!"
//     * 总长度 36：
//     *   [0..1]   "#@"
//     *   [2..33]  拷贝 frame 中 32 字节（索引 2~33）
//     *   [34..35] "#!"
//     */
//    QByteArray fc(36, 0);
//    fc[0] = '#'; fc[1] = '@';
//    for (int i = 0; i <= 31; ++i) fc[i + 2] = frame[i + 2];
//    fc[34] = '#'; fc[35] = '!';
//    r.fcbbuf36 = fc;

//    /* -------- 5) 继电器参数解析 --------
//     *
//     * relayTimes: frame[111..112] -> 两位 ASCII HEX
//     * relayNum  : frame[113..114] -> 两位 ASCII DEC
//     *
//     * NOTE:
//     * 若 parse 失败会得到 -1。
//     */
//    r.relayTimes = parseAsciiHex2At(frame, 111);
//    r.relayNum   = parseAsciiDec2At(frame, 113);

//    /* -------- 6) 成功返回 -------- */
//    r.pass = true;
//    r.reason = "ok";
//    return r;
//}


// 访客卡有效期判断
bool OfflineChecker::visitorNotExpired(const QByteArray& frame, const QDateTime& now)
{
    // STM32 下标：year [42+2..43+2] => [44..45]
    int yy = trans2digits(frame, 44);
    int mm = trans2digits(frame, 46);
    int dd = trans2digits(frame, 48);
    int hh = trans2digits(frame, 50);
    int mi = trans2digits(frame, 52);

    if (yy < 0 || mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59)
        return false;

    int nowYY = now.date().year() % 100;
    int nowMM = now.date().month();
    int nowDD = now.date().day();
    int nowHH = now.time().hour();
    int nowMI = now.time().minute();


    // 允许：当前时间 <= 截止时间
    if (nowYY < yy) return true;
    if (nowYY > yy) return false;

    if (nowMM < mm) return true;
    if (nowMM > mm) return false;

    if (nowDD < dd) return true;
    if (nowDD > dd) return false;

    if (nowHH < hh) return true;
    if (nowHH > hh) return false;

    return nowMI <= mi;
}


// 判断多层卡/访客卡
OfflineChecker::Result OfflineChecker::check(const QByteArray& frame, int floorNum, const QByteArray& secret8, const QDateTime& now)
{
    if (frame.size() == kLenMulti && frame.size() >= 2 && frame[0] == '#' && frame[1] == '@')
        return checkMulti115(frame, floorNum, secret8, now);

    if (frame.size() == kLenVisitor && frame.size() >= 2 && frame[0] == '#' && frame[1] == '@')
        return checkVisitor103(frame, floorNum, secret8, now);

    Result r;
    r.type = CardType::Unknown;
    r.pass = false;
    r.reason = "frame_invalid";
    return r;
}


// 多层卡（115 字节）离线校验逻辑
OfflineChecker::Result OfflineChecker::checkMulti115(const QByteArray& frame, int floorNum, const QByteArray& secret8, const QDateTime& now)
{
    Result r;
    r.type = CardType::Multi115;

    // basic check
    // - 长度必须是 115 前两个字节必须是 `#@`
    if (frame.size() != kLenMulti || frame.size() < 2 || frame[0] != '#' || frame[1] != '@') {
        r.reason = "frame_invalid";
        return r;
    }

    // 1) 密钥检查: [69..76]
//    qDebug() <<  "size:" << secret8.size()
//             << "frame.mid" << frame.mid(69,8)
//             << "secret8" << secret8;

    if (secret8.size() != 8 || frame.mid(69, 8) != secret8) {
        r.reason = "secret_fail";
        return r;
    }

    // 2) 楼层权限检查: base=78,从索引78开始,一个字符内部按 `8/4/2/1` 映射四层
    if (!floorPermit(frame, floorNum, 78)) {
        r.reason = "floor_deny";
        return r;
    }

    // 3) 星期 + 时间段检查
    // week1 at [45],[47], time1 at [48..55]
    int w1_0 = frame[45] - '0';
    if (w1_0 > 9) w1_0 -= 7;
    int w1_1 = frame[47] - '0';

    int s1_sh = trans2digits(frame, 48);
    int s1_sm = trans2digits(frame, 50);
    int s1_eh = trans2digits(frame, 52);
    int s1_em = trans2digits(frame, 54);

    // week2 at [57],[59], time2 at [60..67]
    int w2_0 = frame[57] - '0';
    if (w2_0 > 9) w2_0 -= 7;
    int w2_1 = frame[59] - '0';

    int s2_sh = trans2digits(frame, 60);
    int s2_sm = trans2digits(frame, 62);
    int s2_eh = trans2digits(frame, 64);
    int s2_em = trans2digits(frame, 66);

    const int dow  = now.date().dayOfWeek();
    const int nowH = now.time().hour();
    const int nowM = now.time().minute();

    const bool noTimeLimit = (frame[45] == '0' && frame[47] == '0' && frame[57] == '0' && frame[59] == '0');

    bool allow = false;
    if (noTimeLimit) {
        allow = true;
    } else {
        const bool slot1_ok = weekPermit(dow, w1_0, w1_1) && timePermit(nowH, nowM, s1_sh, s1_sm, s1_eh, s1_em);
        const bool slot2_ok = weekPermit(dow, w2_0, w2_1) && timePermit(nowH, nowM, s2_sh, s2_sm, s2_eh, s2_em);
        allow = slot1_ok || slot2_ok;
    }

    if (!allow) {
        r.reason = "time_deny";
        return r;
    }

    // 通过后构造控制帧 fcbbuf36
    QByteArray fc(36, 0);
    fc[0] = '#'; fc[1] = '@';
    for (int i = 0; i <= 31; ++i) fc[i + 2] = frame[i + 2];
    fc[34] = '#'; fc[35] = '!';
    r.fcbbuf36 = fc;

    // 继电器参数提取: times [111..112] hex, num [113..114] dec
    r.relayTimes = parseAsciiHex2At(frame, 111);
    r.relayNum   = parseAsciiDec2At(frame, 113);

    r.pass = true;
    r.reason = "ok";
    return r;
}


// 访客卡（103 字节）离线校验逻辑
OfflineChecker::Result OfflineChecker::checkVisitor103(const QByteArray& frame,
                                                                         int floorNum,
                                                                         const QByteArray& secret8,
                                                                         const QDateTime& now)
{
    Result r;
    r.type = CardType::Visitor103;

    // basic check
    if (frame.size() != kLenVisitor || frame.size() < 2 || frame[0] != '#' || frame[1] != '@') {
        r.reason = "frame_invalid";
        return r;
    }

    // 1) 有效期检查, 当前时间 <= 截止时间
    if (!visitorNotExpired(frame, now)) {
        r.reason = "expired";
//        PROBEQ(QString("OFFLINE-CHECK visitor103 deny reason=%1 deadlineRaw=%2")
//                       .arg(r.reason)
//                       .arg(QString::fromLatin1(frame.mid(45, 9))));
        return r;
    }

    // 2) 密钥检查: visitor [57..64]
    if (secret8.size() != 8 || frame.mid(57, 8) != secret8) {
        r.reason = "secret_fail";
        return r;
    }

    // 3) 次数检查: [54]=='0' && [55]=='0'
    if (frame[54] == '0' && frame[55] == '0') {
        r.reason = "no_times";
//        PROBEQ(QString("OFFLINE-CHECK visitor103 deny reason=%1 times=%2")
//                       .arg(r.reason)
//                       .arg(QString::fromLatin1(frame.mid(54, 2))));
        return r;
    }

    // 4) 楼层权限检查: visitor base=66
    if (!floorPermit(frame, floorNum, 66)) {
        r.reason = "floor_deny";
        return r;
    }

    // 通过后构造 RS485 控制帧 fcbbuf36
    QByteArray fc(36, 0);
    fc[0] = '#'; fc[1] = '@';
    for (int i = 0; i <= 31; ++i) fc[i + 2] = frame[i + 2];
    fc[34] = '#'; fc[35] = '!';
    r.fcbbuf36 = fc;

    // relay fields: visitor times [99..100] hex, num [101..102] dec
    r.relayTimes = parseAsciiHex2At(frame, 99);
    r.relayNum   = parseAsciiDec2At(frame, 101);

    r.pass = true;
    r.reason = "ok";
    return r;
}
