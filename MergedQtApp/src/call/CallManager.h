/**
 * @file CallManager.h
 * @brief 单设备单通话的唯一业务入口，覆盖双向呼叫、排队及异步收尾。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-03；修改人：Du lin；明确退出时先结束音频调度代理再释放其借用的引擎；保留UI状态机、物理键入口和原异步挂断/诊断通知。
 */
#pragma once
#include "MqttCallAdapter.h"
#include <QSet>
#include <QStringList>

/** @brief UI 线程串行运行的通话状态机；所有音频资源操作由 AudioEngine 后台完成。 */
class CallManager : public QObject {
    Q_OBJECT
public:
    /** @brief 可见状态；Starting/Ending 是设备本地异步资源交接状态。 */
    enum State {
        Idle, /**< 空闲，无通话资源。 */
        Requesting, /**< 等待平台分配 callId。 */
        Queued, /**< 平台排队，尚未接听。 */
        Ringing, /**< 远端坐席响铃；平台来电也短暂经过此状态后自动准备音频。 */
        Starting, /**< 后台正在打开录放资源，禁止媒体。 */
        Talking, /**< 允许双向音频。 */
        Ending /**< 禁止媒体，等待后台恢复视频音频后才能再次呼叫。 */
    };
    Q_ENUM(State)
    /** @brief 绑定已有 IPC；在初始化阶段准备原音量焦点，创建音频引擎、适配器及本地超时。
     * @param[in] ipc 借用的现有 MQTT IPC 客户端。
     * @param[in] parent QObject 所有者。
     */
    explicit CallManager(MqttIpcClient *ipc,QObject *parent=nullptr);
    /** @brief 仅销毁时先停止音频调度代理，再停止并释放借用引擎，归还原音量/功放焦点。
     * @note 普通挂断不等待；退出必须先停止持有引擎指针的调度线程，防止悬空访问。
     */
    ~CallManager() override;
    /** @brief 更新工程当前 MQTT 身份；路由改变先挂断当前通话。
     * @param[in] deviceId ycLinux event 主题中的精确设备 ID；空串关闭通话。
     */
    void configure(const QString &deviceId);
    /** @brief 同步 MQTT/IPC 可用状态，掉线立即禁止音频并异步收尾。
     * @param[in] online true 表示 Broker 和 IPC 都可用。
     */
    void setOnline(bool online);
    /** @brief 分流已解析的通话 IPC 数据。
     * @param[in] obj 原 IPC JSON。
     * @return true 表示已消费通话专用消息。
     */
    bool handleIpc(const QJsonObject &obj);
    /** @brief 处理精确 request 主题上的 call.* JSON。
     * @note 有效 call.start 自动异步准备音频，就绪后才上报 call.answer；不等待本地按钮。
     * @param[in] topic 完整 MQTT 主题。
     * @param[in] payload JSON 信封，校验 id/deviceId/data/callId。
     * @return true 表示属于 call.*，包括已丢弃的无效、重复和过期消息。
     */
    bool handleSignal(const QString &topic,const QJsonObject &payload);
    /** @return 当前本地状态。 */
    State state() const { return state_; }
    /** @return 当前是否是设备主动呼叫。 */
    bool outgoing() const { return outgoing_; }
    /** @return 可发起呼叫的在线且有路由状态。 */
    bool available() const { return online_ && !adapter_->deviceId().isEmpty(); }
    /** @return 空闲、在线且没有等待清理的未受理请求时返回 true。 */
    bool canRequestCall() const { return available() && state_==Idle && !cancelledRequest_; }
    /** @return 物理键可执行拨号或取消/挂断时为true；Ending及空闲等待迟到ACK时为false。 */
    bool canUseCallButton() const { return canRequestCall() || (state_!=Idle && state_!=Ending); }
    /** @return 当前精确设备标识；空串表示未启用通话路由。 */
    QString deviceId() const { return adapter_->deviceId(); }
    /** @return 已校验下行 PCM 的最近电平/帧数；空闲、未收到时为空，不消费音频。 */
    VoiceCall::ReceivedAudioLevel receivedAudioLevel() const { return engine_->receivedAudioLevel(); }
    /** @return 当前 callId；Requesting 阶段可为空。 */
    QString callId() const { return callId_; }
public slots:
    /** @brief 空闲在线时向物业发起 call.request，30s 内未受理则退出。 */
    void requestCall();
    /** @brief UI线程处理一次物理按下：空闲拨号，Requesting/Queued/Ringing/Starting/Talking取消或挂断。
     * @note Ending忽略；复用hangup的迟到ACK保护和后台收尾，不等待声卡。
     *       键盘仍使用requestCall，避免旧键盘请求被解释为挂断。
     */
    void pressCallButton();
    /** @brief 兼容已有调用的被叫准备入口；有效 call.start 已自动执行，无需 UI 调用。
     * @note 仅被叫 Ringing 有效；Starting/Talking 重复调用不重复打开音频。
     */
    void answer();
    /** @brief 挂断/拒绝当前通话，异步释放声卡后恢复 Idle。 */
    void hangup();
signals:
    /** @brief 状态/可用性改变，UI 不参与音频处理。
     * @param[in] text 当前状态的可读中文描述。
     */
    void changed(const QString &text);
    /** @brief 含 callId 的状态、音频和失败诊断。
     * @param[in] text 不含 PCM 的诊断信息。
     */
    void logMessage(const QString &text);
private:
    /** @brief 更新状态并通知界面；相同状态仍可刷新可用性，并同步UI诊断标签。
     * @param[in] state 新的本地业务状态。
     * @note 仅UI线程调用；Idle仅请求诊断在下轮事件循环汇总，不等待诊断或音频线程。
     */
    void change(State state);
    /** @brief 申请可取消的异步音量/功放焦点，就绪后才启动后台音频；重复接听不重复申请。 */
    void startAudio();
    /** @brief 禁止媒体、取消计时并异步结束；可重复调用。
     * @note 若还在等待音量，则取消就绪回调；若声卡已启动，则在后台退出后再恢复用户音量。
     * @param[in] reason 上行挂断原因。
     * @param[in] notify true 上报 call.hangup，false 用于平台 stop/掉线。
     */
    void finish(const QString &reason,bool notify);
    /** @brief 发送包含当前 callId 的事件。
     * @param[in] method call.ringing/answer/hangup。
     * @param[in] reason 可选结束原因。
     * @return true 已提交；false 为无 ID 或发送失败。
     */
    bool report(const QString &method,const QString &reason=QString());
    /** @brief 保存已结束 ID 的有限缓存，拒绝近期重复呼叫。 */
    void retire();
    State state_=Idle; /**< 仅 UI 线程读写的当前状态。 */
    bool outgoing_=false,online_=false; /**< 呼叫方向和 Broker/IPC 可用性。 */
    QString callId_; /**< 当前会话；必须精确匹配后才接受 answer/stop。 */
    QStringList retired_; /**< 最近 32 个结束 callId，防止 QoS 重复恢复旧通话。 */
    QStringList seenStarts_; /**< 最近 32 个 start 消息 ID，拦截无 callId 的重复呼叫。 */
    bool enginePending_=false; /**< begin 到 finished 槽处理完之间为 true，防止旧 finished 结束新通话。 */
    bool cancelledRequest_=false; /**< 未获 callId 前取消；迟到 ACK 须补发 hangup。 */
    quint64 cancellationGeneration_=0; /**< 取消上下文代次，防止旧超时清除后续取消标志。 */
    AudioEngine *engine_; /**< 子对象；正常收尾由 finished 异步通知。 */
    MqttCallAdapter *adapter_; /**< 子对象；只复用既有 IPC。 */
    QTimer timeout_; /**< 本地单次超时，毫秒；排队到响铃不延长累计 30s。 */
};
