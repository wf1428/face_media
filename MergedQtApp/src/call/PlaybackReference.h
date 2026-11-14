/**
 * @file PlaybackReference.h
 * @brief 缓存已成功提交 ALSA 的最终 PCM，并按单调样本时间重建采集参考。
 * @author Du lin
 * @date 2026-10-31
 * @note 最近修改：2026-11-05；修改人：Du lin；删除仅供日志的参考失败详情，保留原覆盖边界、插值及时间线代次。
 */
#pragma once
#include "CallProtocol.h"
#include <array>
#include <cmath>

namespace VoiceCall {
/** @brief 一个已完成 ALSA 传输的时间估计；只在设备所属线程读写。 */
struct AudioTiming {
    int64_t firstSampleUs=0; /**< 首样本的本地 CLOCK_MONOTONIC 微秒估计；采集为过去、播放可在未来。 */
    bool valid=false; /**< true 仅在 RUNNING、延迟非负且取样跨度足够小时成立。 */
    int64_t queueDelayUs=-1; /**< 同步查询得到的应用采样率排队延迟；-1为未取得。 */
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
     * @return true 整帧有实际播放历史覆盖；false 为尚未就绪、过期或有时间缺口。
     */
    bool read(int64_t captureUs,int64_t delayUs,Pcm &pcm) const {
        if (!count_) return false;
        const double start=double(captureUs-delayUs);size_t block=0;
        if (start<double(at(0).firstUs)) return false;
        for (int i=0;i<Samples;++i) {
            const double target=start+double(i)*62.5;
            while (block+1<count_ && target>=double(at(block+1).firstUs)) ++block;
            const auto &current=at(block);
            const double duration=block+1<count_ ? double(at(block+1).firstUs-current.firstUs) : 20000.0;
            if (duration<=0 || target<double(current.firstUs)) return false;
            if(target>=double(current.firstUs)+duration) return false;
            const double position=(target-double(current.firstUs))*Samples/duration;
            const int index=int(position);const double fraction=position-index;
            if (index<0 || index>=Samples) return false;
            const int a=current.pcm[size_t(index)];
            int b=a;
            if (index+1<Samples) b=current.pcm[size_t(index+1)];
            else if (block+1<count_) b=at(block+1).pcm[0];
            else if (fraction>0.0001) return false;
            pcm[size_t(i)]=int16_t(std::max(-32768,std::min(32767,int(std::lround(a+(b-a)*fraction)))));
        }
        return true;
    }
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
