/**
 * @file rs485_floor_frame_builder.h
 * @brief 旧分层板协议的公共楼层位图帧构造器。
 */
#ifndef RS485_FLOOR_FRAME_BUILDER_H
#define RS485_FLOOR_FRAME_BUILDER_H

#include <QByteArray>
#include <QList>
#include <QString>

namespace Rs485FloorFrameBuilder {
QByteArray buildSingleFloor(int floorNo, QString *error = nullptr);
QByteArray buildFloors(const QList<int> &floors, QString *error = nullptr);
}

#endif
