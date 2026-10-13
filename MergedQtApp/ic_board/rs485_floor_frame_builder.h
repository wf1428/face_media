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

/** @brief 分层板最高8位的自定义电梯模式配置。 */
struct ElevatorModeState
{
    bool masterSwitch = false;
    bool delay = false;
    bool parking = false;
    bool driver = false;
    bool independent = false;
    bool ext1 = false;
    bool ext2 = false;
    bool ext3 = false;
    bool ext4 = false;
};

QByteArray buildSingleFloor(int floorNo, QString *error = nullptr);
QByteArray buildFloors(const QList<int> &floors, QString *error = nullptr);

/** @return 数据库中保存的电梯模式；无配置或配置损坏时返回总开关关闭。 */
ElevatorModeState elevatorModeState();

/**
 * @brief 判断一次通行授权是否只能使用已被电梯模式占用的113～120楼。
 *
 * 仅当总开关开启、楼层列表非空且全部楼层均位于113～120时返回true；
 * 列表中只要包含任一其他楼层就返回false。
 */
bool shouldRejectModeOccupiedFloors(const QList<int> &floors);

/** @brief 原子保存电梯模式配置。 */
bool saveElevatorModeState(const ElevatorModeState &state,
                           QString *error = nullptr);

/**
 * @brief 在RS485最终发送前保护板端120～127位。
 *
 * 总开关关闭时原样返回；开启时，OPEN+128字符使用持久化模式状态覆盖
 * 最高8位，#@+32HEX+#! 的一次性通行帧将最高8位清零，并把可能修改
 * 高8位的aa/bb单层帧转换为受保护的全量OPEN帧。
 */
QByteArray applyElevatorModeOverlay(const QByteArray &frame);
}

#endif
