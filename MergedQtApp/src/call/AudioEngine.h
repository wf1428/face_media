/**
 * @file AudioEngine.h
 * @brief 管理后台全双工录放、限长音频队列和实际播放参考。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；采集帧改为固定5槽所有权交接，发送取帧不再等待统计锁；采集统计仅try_lock发布，完整累计在退出后补齐，100ms过期和原协议不变。
 */
#pragma once
#include "CallProtocol.h"
#include "AudioProcessing.h"
#include "PlaybackReference.h"
#include "RealtimeAudio.h"
#include "ConcurrentJitterBuffer.h"
#include "common/fixed_mailbox.h"
#include <QThread>
#include <mutex>
#include <memory>

/** @brief 每通话启动一次的播放后台；采集和数值诊断各自在线程运行。
 * @details 控制器只调用非阻塞 start/stop/队列接口；ALSA、amixer 和共享混音器
 *          状态切换均在后台。正常挂断通过 finished 异步收尾，UI 不 join。
 */
class AudioEngine : public QThread {
    Q_OBJECT
public:
    /** @brief 创建空闲引擎，不打开音频设备。
     * @param[in] parent QObject 所有者，通常为 CallManager。
     */
    explicit AudioEngine(QObject *parent=nullptr);
    /** @brief 应用退出时请求停止并等待后台释放声卡；正常挂断不调用此析构。 */
    ~AudioEngine() override;
    /** @brief 新通话前清空缓冲/计数并启动后台线程。
     * @pre 上一次 finished 已送达，不允许并发启动。
     */
    void begin();
    /** @brief 原子禁止音频并请求异步停止；可重复调用，无等待。 */
    void stopAsync();
    /** @brief 切换 TALKING 媒体门；false 立即使入队音频无效。
     * @param[in] on true 允许收发，false 禁止。
     */
    void setTalking(bool on);
    /** @brief 校验并无等待投递下行固定槽；电平摘要独立发布，不锁播放队列。
     * @note 仅音频调度线程作为单生产者调用；播放线程为唯一消费者。
     * @param[in] packet MQTT 原始 650 字节帧。
     */
    void receive(const QByteArray &packet);
    /** @brief 取最旧且不超过 100ms 的待发送帧，丢弃过期帧并单独累计过期计数。
     * @param[out] frame 成功时写入采集 PCM。
     * @return true 有新帧；false 禁用或暂无可取得有效槽；不等待采集或统计线程。
     */
    bool takeCaptured(VoiceCall::Frame &frame);
    /** @brief 读取最近接收到的有效 PCM 电平，不访问声卡、不消费播放队列。
     * @return 最近 200ms 电平、帧数和新鲜度；非 TALKING 返回空快照。
     * @note UI 可按 100ms 周期读取；只读取最多10个原子电平点，不等待接收线程，不传递录音正文。
     */
    VoiceCall::ReceivedAudioLevel receivedAudioLevel() const;
    /** @return Linux 显式 CLOCK_MONOTONIC 毫秒，与 mqttd 同源；其他平台使用 steady_clock。不用于墙钟协议时间。 */
    static int64_t nowMs();
signals:
    /** @brief 声卡与共享音频交接就绪；控制器确认当前状态后才开启媒体。 */
    void ready();
    /** @brief 后台发生不可恢复错误，控制器需挂断。
     * @param[in] error 不含原始 PCM 的诊断文本。
     */
    void failed(const QString &error);
    /** @brief 周期、异常限频及结束时报告队列、PCM连续性、DSP模式与XRUN统计。
     * @param[in] text 统计字段及数值，或恢复失败信息。
     */
    void statistics(const QString &text);
protected:
    /** @brief 后台完成路由/声卡初始化、播放循环、采集及诊断线程退出与混音器恢复。 */
    void run() override;
private:
    /** @brief 复制小型计数后在锁外格式化周期/结束诊断，txDrop为满队列与过期丢帧之和。
     * @param[in] phase tick 或 final，用于区分运行与结束快照。
     * @note tick仅诊断线程调用，final在采集/诊断join后调用；tx域短锁复制、下行电平原子读取，不嵌套持锁。
     */
    void reportStatistics(const QString &phase);
    VoiceCall::LevelStats micLevel_,rxLevel_,playLevel_; /**< 原始采集由txMutex_保护，rxLevel_仅接收线程写；播放电平仅诊断线程/退出后读取。 */
    uint64_t playedFrames_=0; /**< ALSA 完成写入帧数，含静音。 */
    std::atomic<int64_t> statsStartMs_{0}; /**< TALKING起点，单调毫秒；0为未开始。 */
    int64_t lastCaptureMs_=0,captureGapMaxMs_=0; /**< 本地单调时钟时间及最大采集间隔，单位毫秒。 */
    std::atomic_bool stop_{true}; /**< true使采集/播放线程在ALSA有限等待后退出；诊断队列在后台收尾。 */
    std::atomic_bool talking_{false}; /**< 仅 TALKING 才允许媒体进出队列。 */
    mutable std::mutex txMutex_; /**< 只保护低频统计快照；采集循环仅try_lock，发送/播放不获取，退出后可等待补齐最终值。 */
    /** @brief 单接收者发布累计电平；原子数值快照不要求各字段来自同一帧，统计不影响PCM。 */
    struct ReceiveTotals {
        static_assert(std::atomic<double>::is_always_lock_free,"receive snapshots must not lock");
        std::atomic<uint64_t> samples{0},clipped{0};std::atomic<int64_t> sum{0};std::atomic<double> squares{0};std::atomic<int> peak{0}; /**< 采样累计及幅度，字段独立原子发布。 */
        /** @brief 接收线程发布数值，不持锁、不做PCM遍历。
         * @param[in] value 接收线程已累计的电平。
         */
        void publish(const VoiceCall::LevelStats &value) { samples=value.samples;clipped=value.clipped;sum=value.sum;squares=value.squares;peak=value.peak; }
        /** @return 低频近似电平快照，调用方通过rxStatsEpoch_确认所属通话。 */
        VoiceCall::LevelStats snapshot() const { VoiceCall::LevelStats value;value.samples=samples.load();value.clipped=clipped.load();value.sum=sum.load();value.squares=squares.load();value.peak=peak.load();return value; }
    };
    ReceiveTotals rxTotals_;std::atomic<uint32_t> rxStatsEpoch_{0}; /**< 接收电平发布值及通话代次；UI/播放不等待诊断/接收线程。 */
    uint32_t rxLevelEpoch_=0;uint64_t rxFramesLocal_=0; /**< 仅接收线程使用的累计值代次/帧数，换代时由它自行清零。 */
    std::array<std::atomic<uint64_t>,10> receivedLevels_{}; /**< 固定原子电平点：高32位接收单调毫秒低位，低32位为peak16|rms16；UI无锁读取。 */
    size_t receivedLevelCursor_=0; /**< 单接收者下一写入槽，首次接收新epoch时初始化。 */
    std::atomic<int64_t> lastReceivedMs_{0}; /**< 当前通话最后有效帧接收时刻；0 表示尚无帧。 */
    std::atomic<int64_t> uiNativeTid_{0}; /**< begin在UI记录本线程内核TID，后台只读用于/proc采样；不支持为0。 */
    VoiceCall::ConcurrentJitterBuffer incoming_; /**< 10 帧/200ms 抖动缓冲。 */
    FixedMailbox<VoiceCall::Frame,5> outgoing_; /**< 固定5槽，采集发布/发送消费；满时替换最旧Ready，绝不覆盖Reading。 */
    VoiceCall::PlaybackReference reference_; /**< 仅采集线程访问的固定64槽历史；播放通过SPSC送入实际写入结果，停止后begin可重置。 */
    size_t referenceDepth_=0;uint64_t referenceGeneration_=1;int64_t referencePhaseUs_=0; /**< txMutex_保护的采集端参考快照，诊断不访问插值对象。 */
    int64_t captureDelayUs_=-1,playbackDelayUs_=-1; /**< 最近传输查询的排队延迟，微秒；采集值由txMutex_保护，播放值只由诊断线程发布，-1为未查询。 */
    VoiceCall::ProcessingStats dspStats_; /**< 采集线程复制的DSP计数，txMutex_保护，UI只读低频日志。 */
    std::atomic<uint64_t> txDrops_{0},txFrames_{0}; /**< 独立原子上行累计；丢弃为容量/有界竞争及100ms过期之和，不保护PCM。 */
    std::atomic<uint64_t> rxFrames_{0}; /**< 接收线程发布，播放只读；rxStatsEpoch_标明所属通话。 */
    std::atomic<uint64_t> txDropFull_{0},txDropExpired_{0}; /**< 固定槽满/有界竞争拒绝与年龄严格大于100ms的直接丢弃帧数，begin在停止状态清零。 */
    std::atomic<size_t> txHighWater_{0}; /**< 单采集者发布Ready槽近似峰值，最多5帧。 */
};
