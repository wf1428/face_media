/**
 * @file ic_event_bridge.cpp
 * @brief IC 本地链路、UI 与 MQTT 网关之间的进程内事件总线的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_event_bridge.h"

/** @return 进程生命周期内唯一的事件桥；故意不随业务窗口销毁。 */
IcEventBridge *IcEventBridge::instance()
{
/** 进程级事件桥；采用静态指针保证串口与页面共享同一信号中心。 */
static IcEventBridge *s = new IcEventBridge();
    return s;
}

/** @brief 发布卡片通过信息给 MQTT 网关等订阅者。 */
void IcEventBridge::emitCardPassed(const QString &cardId, const QString &floor, const QByteArray &rawFrame)
{
    emit cardPassed(cardId, floor, rawFrame);
}

/** @brief 发布在线模式识别出的二维码内容。 */
void IcEventBridge::emitOnlineQrScanned(const QString &qrCode)
{
    emit onlineQrScanned(qrCode);
}

/** @brief 把 online_v1 扫码值交给 ycLinux 二维码协议状态机。 */
void IcEventBridge::emitOnlineV1QrScanned(const QString &qrCode)
{
    emit onlineV1QrScanned(qrCode);
}

/** @brief 把人脸识别通过的人员ID投递给本地楼层权限链路。 */
void IcEventBridge::emitFaceRecognized(const QString &personId,
                                       qint64 localPersonId,
                                       const QString &personName,
                                       const QString &faceHash)
{
    emit faceRecognized(personId, localPersonId, personName, faceHash);
}

void IcEventBridge::emitPasswordAccessRequested(const QString &password)
{
    emit passwordAccessRequested(password);
}

void IcEventBridge::emitPasswordAccessFinished(const QString &personId,
                                                const QString &floors,
                                                const QByteArray &rs485Frame,
                                                bool success,
                                                const QString &reason)
{
    emit passwordAccessFinished(personId, floors, rs485Frame, success, reason);
}

/** @brief 发布人脸楼层权限判定及 RS485 实际发送结果。 */
void IcEventBridge::emitFaceAccessFinished(const QString &personId,
                                           const QString &faceHash,
                                           const QString &floors,
                                           const QByteArray &rs485Frame,
                                           bool success,
                                           const QString &reason)
{
    emit faceAccessFinished(personId, faceHash, floors, rs485Frame,
                            success, reason);
}

/** @brief 把人脸通行抓拍上传任务投递给 MQTT 协调器。 */
void IcEventBridge::emitFaceUploadRequested(const QString &personId,
                                            const QString &faceHash,
                                            const QString &snapshotPath,
                                            bool success)
{
    emit faceUploadRequested(personId, faceHash, snapshotPath, success);
}

/** @brief 发布刷卡楼层权限判定及 RS485 实际发送结果。 */
void IcEventBridge::emitCardAccessFinished(const QString &personId,
                                           const QString &cardId,
                                           const QString &floors,
                                           const QByteArray &rs485Frame,
                                           bool success,
                                           const QString &reason)
{
    emit cardAccessFinished(personId, cardId, floors, rs485Frame, success, reason);
}

/** @brief 发布 RS485 接收到的按空闲间隔聚合帧。 */
void IcEventBridge::emitRs485FrameReceived(const QByteArray &frame)
{
    emit rs485FrameReceived(frame);
}

/**
 * @brief 异步请求 RS485 发送。
 * @return 请求信号已发出时返回 true，不代表硬件发送已经成功。
 */
bool IcEventBridge::requestRs485Send(const QByteArray &frame, const QString &sourceTag)
{
    emit rs485SendRequested(frame, sourceTag);
    return true;
}

/** @brief 发布 RS485 异步发送结果。 */
void IcEventBridge::emitRs485SendFinished(const QString &sourceTag, bool ok, const QString &reason)
{
    emit rs485SendFinished(sourceTag, ok, reason);
}

/** @brief 请求主线程显示验证通过提示。 */
void IcEventBridge::emitToastPassRequested()
{
    emit toastPassRequested();
}

/** @brief 请求主线程显示带原因的验证失败提示。 */
void IcEventBridge::emitToastFailRequested(const QString &reason)
{
    emit toastFailRequested(reason);
}


/**
 * @brief 异步请求通过刷卡器串口下发命令。
 * @return 请求信号已发出时返回 true，结果由 cardReaderCommandFinished() 返回。
 */
bool IcEventBridge::requestCardReaderCommand(const QByteArray &command, const QString &sourceTag)
{
    emit cardReaderCommandRequested(command, sourceTag);
    return true;
}

/** @brief 发布刷卡器命令异步发送结果。 */
void IcEventBridge::emitCardReaderCommandFinished(const QString &sourceTag, bool ok, const QString &reason)
{
    emit cardReaderCommandFinished(sourceTag, ok, reason);
}
