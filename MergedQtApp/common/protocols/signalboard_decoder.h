/**
 * @file signalboard_decoder.h
 * @brief 解析信号板固定帧中的楼层、方向和运行状态。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SIGNALBOARD_DECODER_H
#define SIGNALBOARD_DECODER_H

#include <QByteArray>
#include <QString>

#include <QMetaType>

/** @brief 电梯运行方向。 */
enum class RunDirection { Stop, Up, Down, Unknown };

/** @brief 信号板上报的电梯工作模式。 */
enum class WorkMode { Normal, Fire, Repair, Full, Overload, Unknown };

/**
 * @brief 12 字节信号板协议帧的解析结果。
 *
 * 同时保留语义化字段和 UI 兼容字段，避免界面层再次按固定偏移解析 raw。
 */
struct SignalBoardState {
    bool ok = false;                    /**< 帧格式有效且楼层可显示时为 true。 */
    int floor = 0;                      /**< 数字楼层值；字母楼层不使用该字段。 */

    bool floorValid = false;            /**< 数字楼层或地下楼层解析成功标志。 */
    int  floorNumber = 0;               /**< 地下楼层为负数，普通楼层为非负数。 */
    QString floorText;                  /**< 面向 UI 的楼层文本，如 "05"、"B1" 或 "A"。 */

    RunDirection dir = RunDirection::Unknown; /**< 由协议方向字符映射得到。 */
    WorkMode mode = WorkMode::Unknown;        /**< 由协议模式字符映射得到。 */
    QByteArray raw;                         /**< 原始 12 字节帧，供诊断追溯。 */

    QChar floorPrefix; /**< 地下楼层前缀 'B' 或 '-'；其他情况为空字符。 */
    QChar floorLetter; /**< A-Z 单字母楼层；非字母楼层为空字符。 */
    int tenDigit = 0;  /**< 数字楼层十位，范围 0~9。 */
    int oneDigit = 0;  /**< 数字楼层个位，范围 0~9。 */

    unsigned char dirAscii  = 0x30;  /**< 原始方向 ASCII：'0'、'1' 或 '3'。 */
    unsigned char modeAscii = 0x38;  /**< 原始模式 ASCII：'8'、'1'、'2'、'3' 或 '4'。 */

    unsigned char floorTenOrPrefix = 0; /**< UI 兼容值：十位数或 'B'/'-' ASCII。 */
    unsigned char floorOneDigit    = 0; /**< UI 兼容值：个位数字 0~9。 */
};

/** @brief 将固定 12 字节信号板协议帧解析为 SignalBoardState。 */
class SignalBoardDecoder {
public:
    /**
     * @brief 校验帧头、帧尾和长度，并解析方向、模式及楼层。
     * @param frame 信号板完整帧，格式为 STX + 10 字节数据 + ETX。
     * @return 解析状态；格式或楼层无效时 ok 为 false。
     */
    static SignalBoardState decode(const QByteArray& frame);

};

Q_DECLARE_METATYPE(SignalBoardState)

#endif // SIGNALBOARD_DECODER_H
