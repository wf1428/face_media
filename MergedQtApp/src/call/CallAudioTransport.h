/**
 * @file CallAudioTransport.h
 * @brief 复用现有 mqttd IPC，在有界信用窗口内转发通话音频与 QoS2 信令。
 * @author Du lin
 * @date 2026-11-03
 * @note 最近修改：2026-11-04；修改人：Du lin；缓存转义后的媒体JSON前缀，标准音频及整数确认使用固定2048B栈行与IPC固定槽，避免逐帧分配/共享锁；异常身份保留兼容回退，原信用及协议不变。
 * @details 由MqttCallAdapter持有的独立音频线程拥有；仅使用IPC代理的线程安全提交/快照，不操作套接字。
 */
#pragma once
#include <QObject>
#include <QTimer>
#include "common/debug/thread_observation.h"
#include <QJsonObject>
#include "AudioEngine.h"
class MqttIpcClient;

namespace VoiceCall {
/** @brief 单线程使用的采样等待统计；只计数，不参与发送判定。
 * @details observe(true)表示本次检查遇到等待，连续命中归为一段；时长从首次命中到
 *          首次非等待观测，包含音频事件循环迟到，不能等同于设备/网络实际阻塞时长。
 *          不同等待统计不能相加推算丢帧数；整体赋空值可重置本媒体代次。
 */
struct TransmitWaitStats {
    quint64 ticks=0,count=0; /**< 等待命中的检查次数、连续等待段数；均不是丢帧数。 */
    int64_t sinceMs=-1,totalMs=0,maxMs=0; /**< 单调毫秒起点（-1未等待）、已结束累计时长、已结束最长段。 */
    /** @brief 记录一次等待检查，退出等待时结算时长，不阻塞调用线程。
     * @param[in] waiting true命中等待条件，false关闭当前等待段。
     * @param[in] nowMs 本地单调毫秒，不得混用墙钟。
     */
    void observe(bool waiting,int64_t nowMs) {
        if (waiting) { ++ticks;if (sinceMs<0) { sinceMs=nowMs;++count; } }
        else if (sinceMs>=0) { const auto duration=qMax<int64_t>(0,nowMs-sinceMs);totalMs+=duration;maxMs=qMax(maxMs,duration);sinceMs=-1; }
    }
    /** @brief 读取包括未结束段的累计等待，不改变统计。
     * @param[in] nowMs 当前单调毫秒。
     * @return 当前媒体代次累计等待毫秒。
     */
    int64_t totalAt(int64_t nowMs) const { return totalMs+(sinceMs<0 ? 0 : qMax<int64_t>(0,nowMs-sinceMs)); }
    /** @brief 读取包括未结束段的最长等待，不改变统计。
     * @param[in] nowMs 当前单调毫秒。
     * @return 最长一段等待毫秒。
     */
    int64_t maxAt(int64_t nowMs) const { return qMax(maxMs,sinceMs<0 ? int64_t(0) : qMax<int64_t>(0,nowMs-sinceMs)); }
};
}

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
    /** @brief 消费音频、信用和媒体就绪 IPC 帧；有效就绪/信用ACK同时结束相应诊断等待段。
     * @param[in] obj 已解析 IPC JSON 对象。
     * @return true 为本模块消息；false 交由原业务分派。
     */
    bool handleIpc(const QJsonObject &obj);
    /** @return 当前精确 request 主题，未配置时为空。 */
    QString requestTopic() const;
    /** @return 已配置的设备标识；仅来自当前工程 MQTT 路由。 */
    QString deviceId() const { return deviceId_; }
signals:
    /** @brief 每3秒、媒体结束/信用超时或首个拒绝时输出小型诊断。
     * @param[in] text 只含计数、单调时长、校验原因和头部，不输出 PCM 正文。
     */
    void diagnostics(const QString &text);
    /** @brief 当前代次媒体门已就绪。
     * @param[in] revision 媒体代次，UI代理丢弃旧值，禁止旧ready复活新会话。
     */
    void mediaReady(quint64 revision);
    /** @brief 原信用超时、拒绝或IPC断开通知。
     * @param[in] revision 当前媒体代次，由UI代理过滤迟到通知。
     */
    void stalled(quint64 revision);
private:
    /** @brief 每 5ms 检查一次有界信用，按绝对 20ms 时序发送；短延迟允许有限追赶。
     * @note 只处理一帧，不等待网络；采集时钟控制生产速率，窗口最多5帧。
     *       诊断仅在当前音频调度线程累加/采样；沿用该定时器，不输出逐帧日志。
     */
    void tick();
    /** @brief 在音频调度线程格式化累计诊断，未结束等待按now计入；不重置统计。
     * @param[in] phase tick、final或stalled；stalled覆盖原超时路径active先关闭的情况。
     * @param[in] now 本地单调毫秒。
     */
    void reportTransport(const QString &phase,int64_t now);
    /** @brief 汇总一帧成功交IPC的PCM，不输出录音正文，不推断平台接收/播放。
     * @param[in] frame 被编码并提交成功的本地帧。
     * @param[in] now 本地提交单调毫秒。
     */
    void observeSubmitted(const VoiceCall::Frame &frame,int64_t now);
    int64_t takeMaxUs_=0,submitMaxUs_=0,takeMaxBeginUs_=0,submitMaxBeginUs_=0; /**< 本次媒体取帧/提交最大墙钟及单调起点，微秒。 */
    ThreadDiagnostics::Delta takeAtMax_,submitAtMax_;uint32_t pcmHash_=0,wireHash_=0;uint64_t wireHashMismatches_=0; /**< 配对资源及最近编码提交尝试的PCM/编码字节摘要，均本线程独占。 */
    uint64_t lastCaptureIndex_=0,txCaptureSkips_=0,txModeChanges_=0,txClip_=0; /**< 已成功提交的序号及跳过采集帧数、模式变化、近满幅采样数；媒体开启清零。 */
    int lastPcmTail_=0,txStepMax_=0,txRms_=0,txPcmPeak_=0,txDc_=0;unsigned lastProcessingMode_=0; /**< 最后PCM尾部及跨提交帧边界极值/最近一帧电平；只在音频调度线程更新。 */
    int64_t txPcmLastLogMs_=0;unsigned txPcmReports_=0; /**< 实际提交异常详情前3批，此后最多5s一批；不向UI投50fps日志。 */
    int64_t txSubmitAt_=0,txSubmitGapMaxMs_=0,txAgeMaxMs_=0; /**< 最近成功提交时刻、最大提交间隔与采集年龄，单调毫秒；0未提交。 */
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
    int64_t pendingSince_=0; /**< 等待 ready/sent 的本地单调时钟起点，单位 ms。 */
    VoiceCall::TransmitWaitStats readyWait_,creditWait_,ipcWait_; /**< 仅音频调度线程读写；分别为媒体就绪、就绪后无信用、到发送时刻且有信用但IPC>2048B；media(true)清零。 */
    bool diagnosticReady_=false; /**< 仅区分启动等待与信用耗尽，不参与原收发判定。 */
    int diagnosticCreditMin_=-1; /**< 就绪后的最低剩余信用，含提交扣减后的值；-1表示未就绪。 */
    int64_t lastTickAt_=0,tickGapMaxMs_=0; /**< 媒体启用至首tick及相邻有效tick的最大服务间隔，单调毫秒；不含停用期。 */
    int64_t tickGapStartMs_=0,tickGapEndMs_=0; /**< 最大服务间隔的起止单调毫秒；仅更大间隔出现时更新，media(true)清零，供关联ui-perf。 */
    qint64 ipcPendingPeakBytes_=0; /**< 实际待写/当前帧及紧急队列估计字节峰值；不含延后普通大帧、内核/mqttd/Broker积压。 */
    quint64 ipcDisconnectedTicks_=0,txSendFailures_=0; /**< 到期且有信用时IPC断开次数、实时/兼容提交失败次数；不是帧丢弃分类。 */
};
