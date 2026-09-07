/**
 * @file ic_event_bridge.h
 * @brief IC 本地链路、UI 与 MQTT 网关之间的进程内事件总线。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_EVENT_BRIDGE_H
#define IC_EVENT_BRIDGE_H

#include <QObject>
#include <QByteArray>
#include <QString>

/**
 * @brief IC 本地链路、UI 与 MQTT 网关之间的进程内事件总线。
 *
 * 单例只负责转发 Qt 信号，不持有串口或业务状态。请求方法表示事件已投递，
 * 实际硬件结果通过对应 Finished 信号异步返回。
 */
class IcEventBridge : public QObject
{
    Q_OBJECT
public:
    /** @return 进程生命周期内唯一的事件桥对象。 */
    static IcEventBridge* instance();

    /** @brief 发布在线兼容模式下通过的卡片信息。 */
    void emitCardPassed(const QString &cardId, const QString &floor, const QByteArray &rawFrame);

    /** @brief 发布在线模式识别出的二维码内容。 */
    void emitOnlineQrScanned(const QString &qrCode);

    /** @brief 发布 online_v1 扫描到的二维码值，交给新平台协议处理。 */
    void emitOnlineV1QrScanned(const QString &qrCode);

    /** @brief 把人脸识别通过的人员ID投递给本地楼层权限链路。 */
    void emitFaceRecognized(const QString &personId, qint64 localPersonId,
                            const QString &personName, const QString &faceHash);

    /** @brief 把人脸界面输入的人员通行密码投递给本地权限链路。 */
    void emitPasswordAccessRequested(const QString &password);

    /** @brief 发布密码权限判定及楼层帧实际发送结果。 */
    void emitPasswordAccessFinished(const QString &personId,
                                    const QString &floors,
                                    const QByteArray &rs485Frame,
                                    bool success,
                                    const QString &reason);

    /** @brief 发布人脸楼层权限判定及 RS485 实际发送结果。 */
    void emitFaceAccessFinished(const QString &personId, const QString &faceHash,
                                const QString &floors,
                                const QByteArray &rs485Frame, bool success,
                                const QString &reason);

    /** @brief 发布一条需要异步编码并上传的人脸通行抓拍记录。 */
    void emitFaceUploadRequested(const QString &personId,
                                 const QString &faceHash,
                                 const QString &snapshotPath,
                                 bool success);

    /** @brief 发布刷卡楼层权限判定及 RS485 实际发送结果。 */
    void emitCardAccessFinished(const QString &personId, const QString &cardId,
                                const QString &floors, const QByteArray &rs485Frame,
                                bool success, const QString &reason);

    /** @brief 发布 RS485 接收到的按空闲间隔聚合帧。 */
    void emitRs485FrameReceived(const QByteArray &frame);

    /**
     * @brief 异步请求 RS485 发送。
     * @return 请求信号已发出时返回 true，不代表硬件发送已经成功。
     */
    bool requestRs485Send(const QByteArray &frame, const QString &sourceTag = QString());

    /**
     * @brief 异步请求通过刷卡器串口下发命令。
     * @return 请求信号已发出时返回 true，结果由 cardReaderCommandFinished() 返回。
     */
    bool requestCardReaderCommand(const QByteArray &command, const QString &sourceTag = QString());

    /** @brief 请求主线程显示验证通过提示。 */
    void emitToastPassRequested();

    /** @brief 请求主线程显示带原因的验证失败提示。 */
    void emitToastFailRequested(const QString &reason);

    /** @brief 发布 RS485 异步发送结果。 */
    void emitRs485SendFinished(const QString &sourceTag, bool ok, const QString &reason);

    /** @brief 发布刷卡器命令异步发送结果。 */
    void emitCardReaderCommandFinished(const QString &sourceTag, bool ok, const QString &reason);

    /** @return 当前网络链路和外网探测均可用时返回 true。 */
    bool networkAvailable() const;

    /** @brief 更新进程级网络可用状态；相同状态不会重复发出变化信号。 */
    void updateNetworkAvailability(bool available);

signals:
    /** @brief 网络可用状态实际发生变化。 */
    void networkAvailabilityChanged(bool available);

    /** @brief 在线模式卡片通过事件。 */
    void cardPassed(const QString &cardId, const QString &floor, const QByteArray &rawFrame);

    /** @brief 在线模式二维码扫描事件。 */
    void onlineQrScanned(const QString &qrCode);

    /** @brief online_v1 二维码扫码事件。 */
    void onlineV1QrScanned(const QString &qrCode);

    /** @brief 人脸识别通过后发起本地人员权限校验。 */
    void faceRecognized(const QString &personId, qint64 localPersonId,
                        const QString &personName, const QString &faceHash);

    /** @brief 人脸界面提交了人员通行密码。 */
    void passwordAccessRequested(const QString &password);

    /** @brief 密码权限判定及楼层帧发送完成。 */
    void passwordAccessFinished(const QString &personId,
                                const QString &floors,
                                const QByteArray &rs485Frame,
                                bool success,
                                const QString &reason);

    /** @brief 人脸权限判定及楼层帧发送完成事件。 */
    void faceAccessFinished(const QString &personId, const QString &faceHash,
                            const QString &floors,
                            const QByteArray &rs485Frame, bool success,
                            const QString &reason);

    /** @brief 人脸识别结果需要上传抓拍记录。 */
    void faceUploadRequested(const QString &personId,
                             const QString &faceHash,
                             const QString &snapshotPath,
                             bool success);

    /** @brief online_v1 刷卡权限判定及楼层帧发送完成事件。 */
    void cardAccessFinished(const QString &personId, const QString &cardId,
                            const QString &floors, const QByteArray &rs485Frame,
                            bool success, const QString &reason);

    /** @brief RS485 接收事件。 */
    void rs485FrameReceived(const QByteArray &frame);

    /** @brief 投递给 IC worker 的 RS485 发送请求。 */
    void rs485SendRequested(const QByteArray &frame, const QString &sourceTag);

    /** @brief 投递给 IcWorker、由刷卡器串口执行的命令请求。 */
    void cardReaderCommandRequested(const QByteArray &command, const QString &sourceTag);

    /** @brief UI 通过提示请求。 */
    void toastPassRequested();

    /** @brief UI 失败提示请求。 */
    void toastFailRequested(const QString &reason);

    /** @brief RS485 发送完成回执。 */
    void rs485SendFinished(const QString &sourceTag, bool ok, const QString &reason);

    /** @brief 刷卡器命令发送完成回执。 */
    void cardReaderCommandFinished(const QString &sourceTag, bool ok, const QString &reason);

private:
    bool networkAvailable_ = true;
};

#endif // IC_EVENT_BRIDGE_H
