/**
 * @file broadcast_live.h
 * @brief 单向直播的有界接收与独立 ALSA 播放。
 * @author Du lin
 * @note 最近修改：2026-11-10；修改人：Du lin；声明直播工作线程的声卡交接状态，供控制线程延后释放焦点。
 * @date 2026-11-05
 */
#pragma once
#include <QJsonObject>
#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

/** @brief 一条直播的媒体入口和播放线程，不拥有采集设备。
 * @details IPC线程只校验并交接有限帧，播放线程按20ms出队。停止、暂停或换会话时
 *          取消旧代次的写入，由播放线程关闭 ALSA 并交还扬声器。析构等待该线程退出。
 */
class BroadcastLive {
public:
    /** @brief 创建空闲播放线程，配置播放前不占用声卡。 */
    BroadcastLive();
    /** @brief 停止并等待播放线程，释放播放设备和扬声器。 */
    ~BroadcastLive();
    /** @brief 更新本地接收代次并丢弃旧会话的媒体。
     * @param[in] topic 精确直播音频主题，空串关闭入口。
     * @param[in] frameMs 平台帧间隔，20或40ms。
     * @param[in] playing true允许出队，false暂停并清空。
     * @param[in] epoch 当前IPC媒体代次，暂停时为空。
     */
    void configure(const QString &topic, int frameMs, bool playing, const QString &epoch);
    /** @brief 设置当前直播软件增益。
     * @param[in] percent 0..100，超限钳制。
     */
    void setVolume(int percent);
    /** @return 播放阶段无有效帧的本地单调毫秒。 */
    qint64 silenceMs() const;
    /** @return 播放输出是否报告错误，重新配置播放时重置。 */
    bool failed() const;

    /** @brief 查询直播工作线程是否仍在播放或归还共享输出。
     * @return true 表示声卡交接尚未完成，false 表示可以释放直播焦点。
     */
    bool outputPending() const;
    /** @brief 当前媒体代次的诊断计数，区分 IPC 帧、20ms 播放块和完整写入 ALSA 的块。 */
    struct Statistics {
        quint64 ipcFrames = 0; /**< 观察到的广播 IPC 帧数，含拒绝帧。 */
        quint64 acceptedFrames = 0; /**< 至少一个块被抗抖缓冲接受的 IPC 帧数，含满时淘汰。 */
        quint64 alsaBlocks = 0; /**< 完整写入 ALSA 的20ms块数。 */
        quint64 alsaXruns = 0; /**< ALSA 进入欠载或挂起恢复的次数。 */
        qint64 writeMaxUs = 0; /**< 取帧到 ALSA 写入返回的最长耗时，单位微秒。 */
        quint64 silenceBlocks = 0; /**< 缓冲未准备或欠载时送入的20ms补静音块。 */
        quint64 jitterDrops = 0; /**< 通话同款抗抖缓冲累计淘汰、过期及拒绝块。 */
        quint64 underruns = 0; /**< 已预缓冲后取不到有效帧的次数。 */
        quint64 nonzeroBlocks = 0; /**< 写入 ALSA 且含非零样本的块数。 */
        int peak = 0; /**< 送播增益后的最大绝对样本值，0..32768。 */
    };
    /** @return 主线程读取的当前媒体代次统计，停止或换代后清零。 */
    Statistics statistics() const;
    /** @return 可在IPC线程直接调用的值回调，持有共享队列而不访问播放设备。 */
    std::function<void(const QJsonObject &)> receiver() const;

private:
    struct State;
    /** @brief 按媒体代次打开和关闭 ALSA，不持队列锁访问声卡。 */
    void run();
    std::shared_ptr<State> state_; /**< 主线程与IPC/播放线程共享队列。 */
    std::thread worker_; /**< 独立播放线程，停止时由条件变量唤醒。 */
};
