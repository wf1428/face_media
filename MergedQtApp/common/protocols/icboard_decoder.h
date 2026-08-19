/**
 * @file icboard_decoder.h
 * @brief IC 卡板原始帧的解析结果。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ICBOARD_DECODER_H
#define ICBOARD_DECODER_H

#include <QString>
#include <QByteArray>

/**
 * @brief IC 卡板原始帧的解析结果。
 *
 * 字段解析失败时保留默认值，调用方应先检查 looksValid，再使用业务字段。
 */
struct IcCardEvent {
    /** @brief 由帧长度区分的卡数据类型。 */
    enum class CardType { Unknown, Visitor103, Multi115 };

    CardType type = CardType::Unknown; /**< Unknown、103 字节访客卡或 115 字节多层卡。 */
    QByteArray raw;                    /**< 未修改的原始协议帧。 */

    QString cardId;        /**< raw[35..42] 的 8 字节 ASCII 卡号。 */
    int relayNum = -1;     /**< 继电器编号，两位十进制 ASCII；-1 表示未解析。 */
    int relayTimes = -1;   /**< 继电器次数，两位十六进制 ASCII；-1 表示未解析。 */

    bool looksValid = false; /**< 帧头为 "#@" 且长度为 103 或 115 字节时为 true。 */
};

/** @brief 将 IC 卡板的 103/115 字节原始帧解析为 IcCardEvent。 */
class IcBoardDecoder {
public:
    /**
     * @brief 校验帧头和长度，并解析卡号及继电器字段。
     * @param frame 待解析的完整原始帧。
     * @return 解析结果；格式不匹配时 type 为 Unknown、looksValid 为 false。
     */
    static IcCardEvent decode(const QByteArray &frame);

private:
    /** @brief 将不可打印字节替换为 '.'，便于安全显示 ASCII 字段。 */
    static QString bytesToAscii(const QByteArray &b);

    /** @return 两位十进制 ASCII 的数值；格式错误时返回 -1。 */
    static int parseAsciiInt2(const QByteArray &b2);

    /** @return 两位十六进制 ASCII 的数值；格式错误时返回 -1。 */
    static int parseAsciiHex2(const QByteArray &b2);
};

#endif // ICBOARD_DECODER_H
