/**
 * @file rs485_floor_frame_builder.cpp
 * @brief 旧分层板协议的公共楼层位图帧构造器实现。
 */
#include "rs485_floor_frame_builder.h"

#include <QSet>

namespace {

bool floorPosition(int floorNo, int *frameIndex, int *mask, QString *error)
{
    if (floorNo == 0 || floorNo < -8 || floorNo > 120) {
        if (error) {
            *error = QStringLiteral("楼层越界: %1（允许 -8~-1、1~120）").arg(floorNo);
        }
        return false;
    }

    const int bitIndex = floorNo < 0 ? floorNo + 8 : floorNo + 7;
    if (frameIndex) *frameIndex = 2 + bitIndex / 4;
    static const int kMasks[4] = {0x8, 0x4, 0x2, 0x1};
    if (mask) *mask = kMasks[bitIndex % 4];
    return true;
}

char upperHexDigit(int value)
{
    static const char kHex[] = "0123456789ABCDEF";
    return kHex[value & 0x0F];
}

int hexDigitValue(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    return -1;
}

} // namespace

namespace Rs485FloorFrameBuilder {

QByteArray buildFloors(const QList<int> &floors, QString *error)
{
    if (error) error->clear();
    if (floors.isEmpty()) {
        if (error) *error = QStringLiteral("没有可下发的楼层");
        return {};
    }

    QByteArray frame(36, '0');
    frame[0] = '#';
    frame[1] = '@';
    frame[34] = '#';
    frame[35] = '!';

    QSet<int> applied;
    for (int floorNo : floors) {
        if (applied.contains(floorNo)) continue;

        int index = -1;
        int mask = 0;
        if (!floorPosition(floorNo, &index, &mask, error)) return {};

        const int oldValue = hexDigitValue(frame.at(index));
        if (oldValue < 0) {
            if (error) *error = QStringLiteral("楼层位图内部状态非法");
            return {};
        }
        frame[index] = upperHexDigit(oldValue | mask);
        applied.insert(floorNo);
    }
    return frame;
}

QByteArray buildSingleFloor(int floorNo, QString *error)
{
    return buildFloors(QList<int>{floorNo}, error);
}

} // namespace Rs485FloorFrameBuilder
