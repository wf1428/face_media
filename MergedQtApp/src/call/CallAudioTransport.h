/**
 * @file CallAudioTransport.h
 * @brief 复用现有 mqttd IPC，在有界信用窗口内转发通话音频与 QoS2 信令。
 * @author Du lin
 * @date 2026-11-03
 * @note 最近修改：2026-11-10；修改人：Du lin；声明仅由音频调度线程维护的有效下行活动时间和超时收口。
 * @details 由MqttCallAdapter持有的独立音频线程拥有；仅使用IPC代理的线程安全提交/快照，不操作套接字。
 */
#pragma once
#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include "AudioEngine.h"
class MqttIpcClient;

/** @brief 音频调度线程中的轻量通话适配器；不打开声卡、不等待网络、不创建 MQTT 客户端。 */
class CallAudioTransport : public QObject {
    Q_OBJECT
public:
    /** @brief 绑定既有 IPC 和音频队列，并准备5ms轮询及20ms帧时序；start只在所属线程调用。
     * @param[in] ipc 借用可跨线程提交的IPC代理，生命周期须覆盖本对象。
     * @param[in] engine 借用的后台音频引擎。
     * @param[in] parent QObject 所有者。
     */
    CallAudioTransport(MqttIpcClient *ipc,AudioEngine *engine,QObject *parent=nullptr);
    /** @brief 设置主题身份；更换时先停媒体。
     * @param[in] deviceId 精确设备标识，空串表示此模式不支持通话。
     */
    void configure(const QString &deviceId);
    /** @brief 音频线程启动后启用原5ms检查，不在构造线程启动定时器。 */
    void start();
    /** @brief 应用退出时在音频线程停表、关闭媒体；不得由UI直接调用。 */
    void shutdown();
    /** @brief IPC断线立即关闭后台媒体并发出原stalled通知，不等待UI；空闲时忽略。 */
    void connectionLost();
    /** @brief 设置媒体门，清除信用和代次；开启时清零诊断，关闭时汇总当前代次。
     * @note 旧代次ACK不可恢复新通话信用；所有调用及诊断状态访问均在所属音频调度线程。
     * @param[in] on true开启当前媒体，false停止。
     * @param[in] revision 代理递增代次，旧代次ready/stalled不得作用于新通话。
     * @param[in] reservation 开启时IPC预留代次；关闭时忽略，使用本对象保存的当前代次。
     */
    void media(bool on,quint64 revision,quint64 reservation=0);
    /** @brief 消费音频、信用和媒体就绪 IPC 帧；有效就绪/信用ACK更新发送信用。
     * @param[in] obj 已解析 IPC JSON 对象。
     * @return true 为本模块消息；false 交由原业务分派。
     */
    bool handleIpc(const QJsonObject &obj);
    /** @return 当前精确 request 主题，未配置时为空。 */
    QString requestTopic() const;
    /** @return 已配置的设备标识；仅来自当前工程 MQTT 路由。 */
    QString deviceId() const { return deviceId_; }
signals:
    /** @brief 周期/结束汇总及传输拒绝、就绪、信用超时等原有事件。
     * @param[in] text 只含计数、校验原因和帧头，不输出 PCM 正文。
     */
    void diagnostics(const QString &text);
    /** @brief 当前代次媒体门已就绪。
     * @param[in] revision 媒体代次，UI代理丢弃旧值，禁止旧ready复活新会话。
     */
    void mediaReady(quint64 revision);
    /** @brief 信用超时、有效下行超时、拒绝或 IPC 断开通知。
     * @param[in] revision 当前媒体代次，由UI代理过滤迟到通知。
     */
    void stalled(quint64 revision);
private:
    /** @brief 每 5ms 检查一次有界信用，按绝对 20ms 时序发送；短延迟允许有限追赶。
     * @note 只处理一帧，不等待网络；采集时钟控制生产速率，窗口最多5帧。
     *       沿用当前定时器输出低频收发汇总。
     */
    void tick();
    /** @brief 在音频调度线程格式化原有的收发计数。
     * @param[in] final true 为结束汇总；false 为周期汇总并附剩余信用。
     */
    void reportTransport(bool final);
    quint64 revision_=0; /**< 仅音频线程读写的媒体控制代次。 */
    quint64 reservation_=0; /**< IPC媒体优先预留代次；0尚未预留，关闭后清零，旧值不能解除新预留。 */
    MqttIpcClient *ipc_; /**< 借用原IPC的线程安全代理；本对象只在音频线程调用它的提交/快照接口。 */
    AudioEngine *engine_; /**< 借用的有界 PCM 队列。 */
    QTimer timer_; /**< 5ms 检查一次；绝对帧时序保持平均 50fps，不再逐帧停等确认。 */
    QByteArray audioPrefix_,ackPrefix_; /**< 仅media切换分配的已转义JSON前缀；本音频线程独占，逐帧不修改。 */
    QString deviceId_,epoch_; /**< 当前身份及媒体代次，代次只在本地 IPC 使用。 */
    bool active_=false; /**< true 表示当前媒体代次允许收发。 */
    int credits_=0,window_=1; /**< 剩余 IPC 信用和窗口上限；新 mqttd 为 5，旧版回退为 1。 */
    int64_t nextSendAt_=0,lastReportAt_=0; /**< 绝对发送时序和诊断时刻，单调时钟毫秒。 */
    quint64 rxIpc_=0,rxForwarded_=0,rxRejected_=0,txSubmitted_=0; /**< 当前媒体代次计数。 */
    bool firstRejectReported_=false; /**< 每通话只输出第一条拒绝细节，防止日志洪泛。 */
    int64_t lastDownlinkAt_=-1; /**< 负值表示未就绪；就绪后记录有效下行活动的单调毫秒。 */
    int64_t pendingSince_=0; /**< 等待 ready/sent 的本地单调时钟起点，单位 ms。 */
};
