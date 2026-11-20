/**
 * @file ic_event_bridge.h
 * @brief IC 本地链路、UI 与 MQTT 网关之间的进程内事件总线。
 *
 * @author Du lin
 * @note 最近修改：2026-11-20；修改人：Du lin；新增消防业务状态通知，使 GPIO 消防状态
 *       可排队传递到楼层状态界面；原信号板状态、提示和硬件请求事件保持不变。
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
                                    const QString &reason,
                                    int resultCode = 0);

    /** @brief 发布人脸楼层权限判定及 RS485 实际发送结果。 */
    void emitFaceAccessFinished(const QString &personId, const QString &faceHash,
                                const QString &floors,
                                const QByteArray &rs485Frame, bool success,
                                const QString &reason,
                                int resultCode = 0);

    /** @brief 发布本地人员人脸通行结果，该事件不供 MQTT 订阅。 */
    void emitLocalFaceAccessFinished(const QString &personId, const QString &faceHash,
                                     const QString &floors, const QByteArray &rs485Frame,
                                     bool success, const QString &reason, int resultCode = 0);

    /** @brief 发布本地人员密码通行结果，不上报 MQTT。 */
    void emitLocalPasswordAccessFinished(const QString &personId, const QString &floors,
                                         const QByteArray &rs485Frame, bool success,
                                         const QString &reason, int resultCode = 0);

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
    void emitToastFailRequested(const QString &reason, int resultCode = 0);

    /** @brief 发布刷卡通过结果，标记来源以应用 IC 声音开关。 */
    void emitCardToastPassRequested();
    /** @brief 发布刷卡失败结果，标记来源以应用 IC 声音开关。
     * @param[in] reason 失败原因码或文本。
     * @param[in] resultCode 失败提示音结果码，0 使用通用提示。
     */
    void emitCardToastFailRequested(const QString &reason, int resultCode = 0);

    /** @brief 发布 RS485 异步发送结果。 */
    void emitRs485SendFinished(const QString &sourceTag, bool ok, const QString &reason);

    /** @brief 发布刷卡器命令异步发送结果。 */
    void emitCardReaderCommandFinished(const QString &sourceTag, bool ok, const QString &reason);

    /** @return 当前网络链路和外网探测均可用时返回 true。 */
    bool networkAvailable() const;

    /** @brief 更新进程级网络可用状态；相同状态不会重复发出变化信号。 */
    void updateNetworkAvailability(bool available);

    /** @brief 请求多媒体页底部显示读头心跳异常。 */
    void emitCardReaderHeartbeatWarning(const QString &message);

    /**
     * @brief 向界面发布已确认的消防业务状态。
     * @param[in] active true 表示消防已进入，false 表示恢复帧已发送并退出消防。
     * @note 调用线程可以是 IC 工作线程；界面接收者必须使用排队连接。
     */
    void emitFireEmergencyChanged(bool active);

signals:
    /** @brief 网络可用状态实际发生变化。 */
    void networkAvailabilityChanged(bool available);

    /** @brief 读头心跳超时，需要在多媒体页底部显示提示。 */
    void cardReaderHeartbeatWarning(const QString &message);

    /**
     * @brief 已确认消防状态发生变化。
     * @param[in] active true 时显示消防状态，false 时恢复原信号板状态判断。
     */
    void fireEmergencyChanged(bool active);

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
                                const QString &reason,
                                int resultCode);

    /** @brief 人脸权限判定及楼层帧发送完成事件。 */
    void faceAccessFinished(const QString &personId, const QString &faceHash,
                            const QString &floors,
                            const QByteArray &rs485Frame, bool success,
                            const QString &reason,
                            int resultCode);

    void localFaceAccessFinished(const QString &personId, const QString &faceHash,
                                 const QString &floors, const QByteArray &rs485Frame,
                                 bool success, const QString &reason, int resultCode);

    void localPasswordAccessFinished(const QString &personId, const QString &floors,
                                     const QByteArray &rs485Frame, bool success,
                                     const QString &reason, int resultCode);

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

    /** @brief UI 通过提示请求。
     * @param[in] fromCard true 表示刷卡来源，false 表示普通提示。
     */
    void toastPassRequested(bool fromCard);

    /** @brief UI 失败提示请求。
     * @param[in] reason 失败原因。
     * @param[in] resultCode 失败提示音结果码。
     * @param[in] fromCard true 表示刷卡来源，false 表示普通提示。
     */
    void toastFailRequested(const QString &reason, int resultCode, bool fromCard);

    /** @brief RS485 发送完成回执。 */
    void rs485SendFinished(const QString &sourceTag, bool ok, const QString &reason);

    /** @brief 刷卡器命令发送完成回执。 */
    void cardReaderCommandFinished(const QString &sourceTag, bool ok, const QString &reason);

private:
    bool networkAvailable_ = true;
};

#endif // IC_EVENT_BRIDGE_H
