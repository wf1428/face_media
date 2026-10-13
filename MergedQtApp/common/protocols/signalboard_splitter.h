/**
 * @file signalboard_splitter.h
 * @brief 信号板固定长度协议拆帧器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SIGNALBOARD_V1_H
#define SIGNALBOARD_V1_H

#include <QByteArray>
#include <QList>

/**
 * @brief 信号板固定长度协议拆帧器。
 *
 * 缓存跨串口读取到达的数据，以 0x02 为帧头、0x03 为帧尾提取 12 字节帧；
 * 发现错位时逐字节重新同步。
 */
class SignalBoardSplitter {
public:
    /**
     * @brief 追加一段字节流并返回本次能够提取的全部完整帧。
     * @param chunk 新收到的串口数据。
     * @return 按到达顺序排列的 12 字节完整帧列表。
     */
    QList<QByteArray> push(const QByteArray& chunk);

private:
    QByteArray buf;                       /**< 尚未组成完整帧的跨调用缓冲区。 */
    static constexpr int kFrameLen = 12; /**< 固定协议帧长度，单位字节。 */
    static constexpr char kSTX = 0x02;   /**< 帧头控制字符。 */
    static constexpr char kETX = 0x03;   /**< 帧尾控制字符。 */
};
#endif // SIGNALBOARD_V1_H
