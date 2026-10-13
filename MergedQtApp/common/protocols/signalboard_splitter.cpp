/**
 * @file signalboard_splitter.cpp
 * @brief 信号板固定长度协议拆帧器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "signalboard_splitter.h"


/**
 * @brief 追加一段字节流并返回本次能够提取的全部完整帧。
 * @param chunk 新收到的串口数据。
 * @return 按到达顺序排列的 12 字节完整帧列表。
 */
QList<QByteArray> SignalBoardSplitter::push(const QByteArray& chunk)
{
    QList<QByteArray> out;
    buf += chunk;       //追加数据到缓冲

    while (true) {
        // 找帧头 0x02
        int stxPos = buf.indexOf(kSTX);
        if (stxPos < 0) {
            // 没有帧头，丢弃过长垃圾，防止无限涨
            if (buf.size() > 1024) buf.clear();
            break;
        }
        if (stxPos > 0) {
            buf.remove(0, stxPos); // 找到帧头,丢掉帧头前垃圾
        }

        if (buf.size() < kFrameLen) break; // 不够一帧，等下次

        // 固定长度取 12 字节
        QByteArray frame = buf.left(kFrameLen);

        // 检查帧尾
        if (static_cast<unsigned char>(frame.at(kFrameLen - 1)) != static_cast<unsigned char>(kETX)) {
            // 帧尾不对，说明错位/丢字节：丢掉第一个字节再重新找
            buf.remove(0, 1);
            continue;
        }

        // 成功拿到一帧
        out.push_back(frame);
        buf.remove(0, kFrameLen);
    }

    return out;
}
