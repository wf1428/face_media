/**
 * @file PlaybackReference.h
 * @brief 缓存已成功提交 ALSA 的最终 PCM，并按单调样本时间重建采集参考。
 * @author Du lin
 * @date 2026-10-31
 * @note 最近修改：2026-11-04；修改人：Du lin；补充有界时间重查的次数、首次/最终跨度与CPU证据；参考来源、2ms有效阈值、历史插值及平滑行为不变。
 */
#pragma once
#include "CallProtocol.h"
#include <array>
#include <cmath>

namespace VoiceCall {
/** @brief ALSA首样本时间判定结果；每个无效分支有唯一原因，数值不属于MQTT协议。 */
enum class TimingReason {
    Disabled, /**< 本帧未启用时间查询。 */
    Incomplete, /**< PCM未完整传输。 */
    Valid, /**< 全部时间判定通过。 */
    QueryError, /**< avail_delay返回负错误码。 */
    NotRunning, /**< PCM状态不是RUNNING。 */
    NegativeAvailable, /**< 可用帧数为负。 */
    NegativeDelay, /**< 排队延迟为负。 */
    ExcessDelay, /**< 排队超过16000采样。 */
    SlowQuery, /**< 查询夹取跨度超过2000us。 */
    InvalidSampleTime /**< 推算首样本时间不大于0。 */
};
/** @brief 返回静态原因名，无分配。
 * @param[in] reason 时间判定枚举。
 * @return 生命周期为整个进程的原因字符串。
 */
inline const char *timingReasonName(TimingReason reason) {
    static const char *const names[]={"disabled","transfer-incomplete","ok","query-error","not-running","negative-avail","negative-delay","excess-delay","query-slow","invalid-sample-time"};
    const auto n=size_t(reason);return n<sizeof(names)/sizeof(*names) ? names[n] : "unknown";
}
/** @brief 参考覆盖判定结果，只用于本地诊断。 */
enum class ReferenceReason {
    Valid, /**< 整帧可读取。 */
    CaptureTiming, /**< 采集时间无效，未尝试读取。 */
    Empty, /**< 尚无历史。 */
    BeforeHistory, /**< 请求早于最旧历史。 */
    AfterHistory, /**< 请求达到/晚于历史覆盖末端。 */
    InvalidTimeline, /**< 历史时长非正或存在覆盖间隙。 */
    InvalidIndex, /**< 插值样本索引越界。 */
    NextSample /**< 末点插值需要尚未到达的下一真实样本。 */
};
/** @brief 参考读取的固定大小证据，不保存PCM。 */
struct ReferenceReadTrace {
    ReferenceReason reason=ReferenceReason::Empty; /**< 失败路径或完整成功。 */
    int failedSample=-1;size_t depth=0; /**< 第一个失败采样索引、历史帧数。 */
    int64_t requestedUs=0,requestedEndUs=0,oldestUs=0,newestEndUs=0; /**< 请求区间及历史边界，单调微秒；requestedEndUs为末样本向上取整，newestEndUs为开区间末端。 */
};
/** @brief 返回静态参考失败名。
 * @param[in] reason 参考读取结果。
 * @return 无分配的静态字符串。
 */
inline const char *referenceReasonName(ReferenceReason reason) {
    static const char *const names[]={"ok","capture-timing","history-empty","before-history","after-history","timeline-gap","sample-index","next-sample"};
    const auto n=size_t(reason);return n<sizeof(names)/sizeof(*names) ? names[n] : "unknown";
}
/** @brief 一个已完成 ALSA 传输的时间估计；只在设备所属线程读写。 */
struct AudioTiming {
    int64_t firstSampleUs=0; /**< 首样本的本地 CLOCK_MONOTONIC 微秒估计；采集为过去、播放可在未来。 */
    bool valid=false; /**< true 仅在 RUNNING、延迟非负且取样跨度足够小时成立。 */
    uint64_t xruns=0; /**< 设备累计恢复次数，用于检测跨帧时序不连续。 */
    int64_t queueDelayUs=-1; /**< 同步查询得到的应用采样率排队延迟；-1为未取得。 */
    TimingReason reason=TimingReason::Disabled; /**< 最近时间查询判定；valid为true时为Valid，禁用为Disabled。 */
    int64_t queryCpuUs=-1,queryOffCpuUs=-1; /**< Linux查询段CPU/非CPU时间估计，微秒；-1未知，非CPU含阻塞/调度，不能直接等同抢占。 */
    unsigned queryAttempts=0; /**< 本帧实际查询次数0/1/2；仅首查SlowQuery允许第二次。 */
    int64_t firstQuerySpanUs=0,firstQueryCpuUs=-1,firstQueryOffCpuUs=-1; /**< 首查跨度/CPU/非CPU微秒，用于确认重查触发原因。 */
    int64_t queryTotalUs=0; /**< 本帧全部查询段累计微秒；不用于有效性阈值。 */
    int64_t querySpanUs=0; /**< 最终一次单调时钟夹取查询耗时，用于原2000us有效性判定。 */
};
/** @brief 固定64帧容量的实际播放历史；引擎中归采集线程独占，跨线程共享时须外部同步。
 * @details 只接收设备写入完成后的 PCM 和时间，不接收 MQTT 原始帧；不等待新参考。
 *          近邻锚点做1/32相位平滑，插值处理采放相位与小幅时钟漂移，超限失步清历史。
 */
class PlaybackReference {
public:
    /** @brief 清空历史并推进代次；重复空历史失效不反复推进。 */
    void clear() { if (count_) { count_=0;cursor_=0;++generation_; } }
    /** @brief 加入真实播放帧；明显跳时先清空，不沿用旧时间线。
     * @param[in] pcm ALSA 已完整接受的最终单声道 PCM，含欠载填充的静音。
     * @param[in] firstUs 首样本单调微秒估计。
     * @param[in] toleranceUs 相邻锚点允许偏差，单位微秒。
     * @param[in] capacity 实际历史上限，4~64帧；超限覆盖最旧。
     */
    void push(const Pcm &pcm,int64_t firstUs,int64_t toleranceUs,size_t capacity) {
        capacity=std::max<size_t>(4,std::min<size_t>(64,capacity));lastPhaseErrorUs_=0;
        if (count_) {
            const auto &previous=at(count_-1);
            const int64_t expected=previous.firstUs+20000,error=firstUs-expected;lastPhaseErrorUs_=error;
            if (std::abs(error)>toleranceUs) clear();
            else firstUs=expected+error/32;
        }
        blocks_[cursor_]={pcm,firstUs};cursor_=(cursor_+1)%blocks_.size();
        count_=std::min(count_+1,capacity);
    }
    /** @brief 按采集首样本时间和标定偏移构造320点参考；不足整帧则旁路，绝不补错位参考。
     * @param[in] captureUs 采集帧首样本单调微秒估计。
     * @param[in] delayUs 正值读取更早的播放样本，用于现场校准声学/额外路径延迟。
     * @param[out] pcm 成功写入320点；失败时调用者不得使用部分内容。
     * @param[out] trace 非空时写入精确失败点和请求/覆盖边界；只复制数值，无新增等待。
     * @return true 整帧有实际播放历史覆盖；false 为尚未就绪、过期或有时间缺口。
     */
    bool read(int64_t captureUs,int64_t delayUs,Pcm &pcm,ReferenceReadTrace *trace=nullptr) const {
        ReferenceReadTrace details;describe(details);details.requestedUs=captureUs-delayUs;details.requestedEndUs=details.requestedUs+19938;
        const auto fail=[&](ReferenceReason reason,int sample) { details.reason=reason;details.failedSample=sample;if(trace) *trace=details;return false; };
        if (!count_) return fail(ReferenceReason::Empty,0);
        const double start=double(captureUs-delayUs);size_t block=0;
        if (start<double(at(0).firstUs)) return fail(ReferenceReason::BeforeHistory,0);
        for (int i=0;i<Samples;++i) {
            const double target=start+double(i)*62.5;
            while (block+1<count_ && target>=double(at(block+1).firstUs)) ++block;
            const auto &current=at(block);
            const double duration=block+1<count_ ? double(at(block+1).firstUs-current.firstUs) : 20000.0;
            if (duration<=0 || target<double(current.firstUs)) return fail(ReferenceReason::InvalidTimeline,i);
            if(target>=double(current.firstUs)+duration) return fail(ReferenceReason::AfterHistory,i);
            const double position=(target-double(current.firstUs))*Samples/duration;
            const int index=int(position);const double fraction=position-index;
            if (index<0 || index>=Samples) return fail(ReferenceReason::InvalidIndex,i);
            const int a=current.pcm[size_t(index)];
            int b=a;
            if (index+1<Samples) b=current.pcm[size_t(index+1)];
            else if (block+1<count_) b=at(block+1).pcm[0];
            else if (fraction>0.0001) return fail(ReferenceReason::NextSample,i);
            pcm[size_t(i)]=int16_t(std::max(-32768,std::min(32767,int(std::lround(a+(b-a)*fraction)))));
        }
        details.reason=ReferenceReason::Valid;if(trace) *trace=details;return true;
    }
    /** @brief 复制当前历史边界；不等待播放、不消费数据。
     * @param[out] trace 覆盖depth/oldestUs/newestEndUs，其余字段保留。
     */
    void describe(ReferenceReadTrace &trace) const { trace.depth=count_;trace.oldestUs=count_ ? at(0).firstUs : 0;trace.newestEndUs=count_ ? at(count_-1).firstUs+20000 : 0; }
    /** @return 当前可用帧数，最多64。 */
    size_t size() const { return count_; }
    /** @return 当前连续播放时间线代次；自然无符号回绕。 */
    uint64_t generation() const { return generation_; }
    /** @return 最近播放锚点相对连续时间线的误差，微秒；不代表已测得的声学延迟。 */
    int64_t phaseErrorUs() const { return lastPhaseErrorUs_; }
private:
    /** @brief 实际提交播放的一个有时间锚点的帧。 */
    struct Block { Pcm pcm{};int64_t firstUs=0; }; /**< PCM归本缓存拥有；时间单位微秒。 */
    /** @return 第index个从旧到新排列的块；调用方保证index<count_。 */
    const Block &at(size_t index) const { return blocks_[(cursor_+blocks_.size()-count_+index)%blocks_.size()]; }
    std::array<Block,64> blocks_{}; /**< 固定存储，正常逐帧路径不分配内存。 */
    size_t cursor_=0,count_=0; /**< 下一个槽及当前帧数。 */
    int64_t lastPhaseErrorUs_=0; /**< 最近锚点偏差，供低频诊断。 */
    uint64_t generation_=1; /**< 不连续时推进，通知采集线程重置DSP。 */
};
}
