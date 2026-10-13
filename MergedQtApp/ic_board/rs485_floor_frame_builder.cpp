/**
 * @file rs485_floor_frame_builder.cpp
 * @brief 旧分层板协议的公共楼层位图帧构造器实现。
 */
#include "rs485_floor_frame_builder.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include "common/sql/dbstore.h"

namespace {

const QString kElevatorModeConfigKey = QStringLiteral("online_v1_elevator_mode");

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

bool jsonSwitch(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isBool()) return value.toBool();
    return value.isDouble() && value.toInt() == 1;
}

quint8 elevatorModeByte(const Rs485FloorFrameBuilder::ElevatorModeState &state)
{
    quint8 value = 0;
    if (state.delay) value |= 0x80;
    if (state.parking) value |= 0x40;
    if (state.driver) value |= 0x20;
    if (state.independent) value |= 0x10;
    if (state.ext1) value |= 0x08;
    if (state.ext2) value |= 0x04;
    if (state.ext3) value |= 0x02;
    if (state.ext4) value |= 0x01;
    return value;
}

void applyModeByteToOpenFrame(
        QByteArray *frame,
        const Rs485FloorFrameBuilder::ElevatorModeState &state)
{
    if (!frame || frame->size() != 132 || !frame->startsWith("OPEN")) return;
    const quint8 mode = elevatorModeByte(state);
    for (int i = 0; i < 8; ++i) {
        (*frame)[4 + 120 + i] = (mode & (0x80 >> i)) ? '1' : '0';
    }
}

QByteArray protectedOpenFrame(
        const Rs485FloorFrameBuilder::ElevatorModeState &state)
{
    // online_v1在数据库中按业务楼层保存开放状态：1=开放，0=限行。
    const QByteArray openBits = DbStore::getConfig(
                QStringLiteral("online_v1_floor_limit_bits"),
                QString(128, QLatin1Char('0'))).toString().trimmed().toLatin1();
    bool bitsValid = openBits.size() == 128;
    for (char bit : openBits) {
        if (bit != '0' && bit != '1') {
            bitsValid = false;
            break;
        }
    }
    const QByteArray validOpenBits = bitsValid
            ? openBits : QByteArray(128, '0');
    QByteArray restrictionBits(128, '1');
    for (int floor = -8; floor <= -1; ++floor) {
        const int stateBit = floor + 128;
        const int boardBit = floor + 8;
        restrictionBits[boardBit] = validOpenBits.at(stateBit) == '1' ? '0' : '1';
    }
    for (int floor = 1; floor <= 120; ++floor) {
        const int stateBit = floor - 1;
        const int boardBit = floor + 7;
        restrictionBits[boardBit] = validOpenBits.at(stateBit) == '1' ? '0' : '1';
    }

    QByteArray frame("OPEN");
    frame.append(restrictionBits);
    applyModeByteToOpenFrame(&frame, state);
    return frame;
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

ElevatorModeState elevatorModeState()
{
    const QByteArray raw = DbStore::getConfig(kElevatorModeConfigKey,
                                               QStringLiteral("{}"))
            .toString().trimmed().toUtf8();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return ElevatorModeState();
    }

    const QJsonObject object = document.object();
    ElevatorModeState state;
    state.masterSwitch = jsonSwitch(object, QStringLiteral("masterSwitch"));
    state.delay = jsonSwitch(object, QStringLiteral("delay"));
    state.parking = jsonSwitch(object, QStringLiteral("parking"));
    state.driver = jsonSwitch(object, QStringLiteral("driver"));
    state.independent = jsonSwitch(object, QStringLiteral("independent"));
    state.ext1 = jsonSwitch(object, QStringLiteral("ext1"));
    state.ext2 = jsonSwitch(object, QStringLiteral("ext2"));
    state.ext3 = jsonSwitch(object, QStringLiteral("ext3"));
    state.ext4 = jsonSwitch(object, QStringLiteral("ext4"));
    return state;
}

bool shouldRejectModeOccupiedFloors(const QList<int> &floors)
{
    if (floors.isEmpty() || !elevatorModeState().masterSwitch) return false;

    for (int floor : floors) {
        if (floor < 113 || floor > 120) return false;
    }
    return true;
}

bool saveElevatorModeState(const ElevatorModeState &state, QString *error)
{
    QJsonObject object;
    object.insert(QStringLiteral("masterSwitch"), state.masterSwitch ? 1 : 0);
    object.insert(QStringLiteral("delay"), state.delay ? 1 : 0);
    object.insert(QStringLiteral("parking"), state.parking ? 1 : 0);
    object.insert(QStringLiteral("driver"), state.driver ? 1 : 0);
    object.insert(QStringLiteral("independent"), state.independent ? 1 : 0);
    object.insert(QStringLiteral("ext1"), state.ext1 ? 1 : 0);
    object.insert(QStringLiteral("ext2"), state.ext2 ? 1 : 0);
    object.insert(QStringLiteral("ext3"), state.ext3 ? 1 : 0);
    object.insert(QStringLiteral("ext4"), state.ext4 ? 1 : 0);
    const QString json = QString::fromUtf8(
                QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!DbStore::setConfig(kElevatorModeConfigKey, json)) {
        if (error) *error = DbStore::lastError();
        return false;
    }
    DbStore::checkpoint();
    return true;
}

QByteArray applyElevatorModeOverlay(const QByteArray &frame)
{
    const ElevatorModeState state = elevatorModeState();
    if (!state.masterSwitch || frame.isEmpty()) return frame;

    if (frame.size() == 132 && frame.startsWith("OPEN")) {
        QByteArray protectedFrame = frame;
        applyModeByteToOpenFrame(&protectedFrame, state);
        return protectedFrame;
    }

    if (frame.size() == 36 && frame.startsWith("#@") && frame.endsWith("#!")) {
        QByteArray protectedFrame = frame;
        // 36字节帧是一次性呼梯/通行继电器选择，不是持续状态设置。
        // 模式开启后最高8位已被模式占用，这里必须清零，不能把数据库
        // 模式值写入帧中，否则每次通行都会同时触发已启用的模式通道。
        protectedFrame[32] = '0';
        protectedFrame[33] = '0';
        return protectedFrame;
    }

    if (frame.size() >= 5
            && ((frame.at(0) == 'a' && frame.at(1) == 'a')
                || (frame.at(0) == 'b' && frame.at(1) == 'b'))) {
        bool ok = false;
        const int floor = QString::fromLatin1(frame.mid(2, 3)).toInt(&ok);
        if (ok && floor >= 113 && floor <= 120) {
            // 高8位已被模式征用，不能再让单层aa/bb直接改写对应位。
            return protectedOpenFrame(state);
        }
    }

    return frame;
}

} // namespace Rs485FloorFrameBuilder
