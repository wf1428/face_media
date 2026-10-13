/**
 * @file icboard_decoder.cpp
 * @brief IC 卡板原始帧的解析结果的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "icboard_decoder.h"
#include <QChar>

static constexpr int kLenVisitor = 103;
static constexpr int kLenMulti   = 115;

/** @brief 将不可打印字节替换为 '.'，便于安全显示 ASCII 字段。 */
QString IcBoardDecoder::bytesToAscii(const QByteArray &b) {
    // 只保留可打印字符，避免日志乱码
    QString s;
    s.reserve(b.size());
    for (char c : b) {
        if (c >= 0x20 && c <= 0x7E) s.append(QChar(c));
        else s.append('.');
    }
    return s;
}

/** @return 两位十进制 ASCII 的数值；格式错误时返回 -1。 */
int IcBoardDecoder::parseAsciiInt2(const QByteArray &b2) {
    if (b2.size() != 2) return -1;
    if (b2[0] < '0' || b2[0] > '9') return -1;
    if (b2[1] < '0' || b2[1] > '9') return -1;
    return (b2[0] - '0') * 10 + (b2[1] - '0');
}

/** @return 单个十六进制 ASCII 字符的数值；非法字符返回 -1。 */
static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

/** @return 两位十六进制 ASCII 的数值；格式错误时返回 -1。 */
int IcBoardDecoder::parseAsciiHex2(const QByteArray &b2) {
    if (b2.size() != 2) return -1;
    int hi = hexVal(b2[0]);
    int lo = hexVal(b2[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/**
 * @brief 校验帧头和长度，并解析卡号及继电器字段。
 * @param frame 待解析的完整原始帧。
 * @return 解析结果；格式不匹配时 type 为 Unknown、looksValid 为 false。
 */
IcCardEvent IcBoardDecoder::decode(const QByteArray &frame) {
    IcCardEvent ev;
    ev.raw = frame;

    if (frame.size() != kLenVisitor && frame.size() != kLenMulti) {
        ev.type = IcCardEvent::CardType::Unknown;
        ev.looksValid = false;
        return ev;
    }
    if (frame.size() < 2 || frame[0] != '#' || frame[1] != '@') {
        ev.type = IcCardEvent::CardType::Unknown;
        ev.looksValid = false;
        return ev;
    }

    ev.looksValid = true;
    ev.type = (frame.size() == kLenVisitor) ? IcCardEvent::CardType::Visitor103
                                            : IcCardEvent::CardType::Multi115;

    // 尝试解析卡号：网络版里 cardid = buf[35..42]（8字节ASCII）
    if (frame.size() >= 43) {
        QByteArray id = frame.mid(35, 8);
        ev.cardId = bytesToAscii(id).trimmed();
    }

    // 继电器字段：
    if (ev.type == IcCardEvent::CardType::Visitor103) {
        // relay_times: [99..100] hex; relay_num: [101..102] decimal
        if (frame.size() >= 103) {
            ev.relayTimes = parseAsciiHex2(frame.mid(99, 2));
            ev.relayNum   = parseAsciiInt2(frame.mid(101, 2));
        }
    } else if (ev.type == IcCardEvent::CardType::Multi115) {
        // relay_times: [111..112] hex; relay_num: [113..114] decimal
        if (frame.size() >= 115) {
            ev.relayTimes = parseAsciiHex2(frame.mid(111, 2));
            ev.relayNum   = parseAsciiInt2(frame.mid(113, 2));
        }
    }

    return ev;
}
