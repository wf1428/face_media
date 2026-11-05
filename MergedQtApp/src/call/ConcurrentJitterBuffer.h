/**
 * @file ConcurrentJitterBuffer.h
 * @brief 固定10槽下行抖动缓存；单接收者/单播放者通过槽所有权交接，不持互斥、不分配。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include "CallProtocol.h"
#include <atomic>
namespace VoiceCall {
/** @brief 保留10帧、4帧/80ms预缓冲、200ms过期与4次欠载再缓冲的并发队列。
 * @details 仅一个push生产者、一个take消费者；诊断只能读原子计数。每槽带版本和
 *          Empty/Writing/Ready/Reading所有权，PCM仅由取得槽者访问，避免覆盖与播放复制竞争；通话epoch隔离迟到生产者。
 *          满时丢时间戳最旧帧（新帧更旧则丢新帧但仍返回已接受），按32位模差排序。
 *          扫描/CAS均有固定上限，不等待暂停的另一线程；reset只在消费者停止时调用；已在途的旧生产者只能发布旧epoch，不能进入新通话。
 */
class ConcurrentJitterBuffer {
    static_assert(std::atomic<uint64_t>::is_always_lock_free,"64-bit slot tags must be lock free");
    enum State:uint64_t { Empty=0, /**< 未占用。 */ Writing=1, /**< 生产者独占写入。 */ Ready=2, /**< 已发布等消费。 */ Reading=3 /**< 消费者独占复制。 */ }; /**< 低2位所有权，高62位为发布版本，防止ABA；只在本类内部使用。 */
    /** @brief 固定槽；tag发布后metadata原子可窥视，PCM只能在独占状态下复制。 */
    struct Slot {
        std::atomic<uint64_t> tag{Empty},metadata{0}; /**< 版本/状态及(seq<<32)|timestamp，release/acquire发布。 */
        std::atomic<uint32_t> epoch{0}; /**< PCM所属通话，tag稳定时读取；旧epoch的Ready可安全回收。 */
        std::atomic<uint64_t> order{0}; /**< 单生产者插入顺序；相同时间戳保持原缓存的稳定排序。 */
        Frame frame{}; /**< Ready时不可写，Reading只归消费者，Writing只归生产者。 */
    };
public:
    /** @brief 消费者停止后推进通话epoch及清计数；不改写仍可能在途的旧生产者槽。 */
    void reset() {
        epoch_.fetch_add(1,std::memory_order_acq_rel);
        played_.store(0);dropped=0;underruns=0;highWater=0;contentionDrops=0;pushRetries=0;takeRetries=0;takeExhausted=0;primed_=false;consecutive_=0;
    }
    /** @brief 插入一帧；固定槽中去重并在满时淘汰最旧，不等播放线程。
     * @param[in] frame 已校验PCM，localMs为本地接收毫秒。
     * @param[in] expectedEpoch 接收入口取得的通话代次，0表示当前代次；旧值直接拒绝。
     * @return false重复/已播放旧帧/过时代次；true已接受，含溢出淘汰。极端CAS争用也计入dropped/contentionDrops。
     */
    bool push(const Frame &frame,uint32_t expectedEpoch=0) {
        const auto generation=epoch_.load(std::memory_order_acquire);
        if(expectedEpoch && expectedEpoch!=generation) return false;
        const auto baseline=played_.load(std::memory_order_acquire);
        if((baseline>>32) && delta(frame.timestamp,uint32_t(baseline))<=0) { ++dropped;return false; }
        const uint64_t key=(uint64_t(frame.seq)<<32)|frame.timestamp;
        for(unsigned retry=0;retry<12;++retry) {
            Slot *free=nullptr,*oldest=nullptr;uint64_t freeTag=0,oldTag=0,oldKey=0,oldOrder=0;size_t depth=0;
            for(auto &slot:slots_) {
                const auto tag=slot.tag.load(std::memory_order_acquire),state=tag&3;
                if(state==Empty || (state==Ready && slot.epoch.load(std::memory_order_acquire)!=generation)) { if(!free) { free=&slot;freeTag=tag; }continue; }
                if(state!=Ready && state!=Reading) continue;
                const auto stored=slot.metadata.load(std::memory_order_acquire),order=slot.order.load(std::memory_order_acquire);
                if(slot.tag.load(std::memory_order_acquire)!=tag) continue;
                if(stored==key) { ++dropped;return false; }
                if(state==Ready) {
                    ++depth;
                    if(!oldest || delta(uint32_t(stored),uint32_t(oldKey))<0 || (uint32_t(stored)==uint32_t(oldKey) && order<oldOrder)) { oldest=&slot;oldTag=tag;oldKey=stored;oldOrder=order; }
                }
            }
            Slot *target=free ? free : oldest;auto expected=free ? freeTag : oldTag;
            if(!target) continue;
            if(!free && delta(frame.timestamp,uint32_t(oldKey))<0) { ++dropped;return true; }
            if(!target->tag.compare_exchange_strong(expected,(expected&~uint64_t(3))|Writing,std::memory_order_acq_rel)) { ++pushRetries;continue; }
            if(!free) ++dropped;
            target->frame=frame;target->order.store(++serial_,std::memory_order_release);target->metadata.store(key,std::memory_order_release);target->epoch.store(generation,std::memory_order_release);
            target->tag.store(((expected&~uint64_t(3))+4)|Ready,std::memory_order_release);
            const auto peak=std::min<size_t>(10,depth+(free ? 1 : 0));
            if(peak>highWater.load()) highWater.store(peak);
            return epoch_.load(std::memory_order_acquire)==generation;
        }
        ++dropped;++contentionDrops;return true;
    }
    /** @brief 每20ms取最旧帧，预缓冲/欠载输出静音；只在播放线程调用。
     * @param[in] nowMs 本地单调毫秒。
     * @return 当前PCM或静音，不重放旧PCM；未就绪的Writing槽留待下一周期。
     */
    Frame take(int64_t nowMs) {
        const auto generation=epoch_.load(std::memory_order_acquire);bool exhausted=true;
        for(unsigned retry=0;retry<24;++retry) {
            Slot *oldest=nullptr;uint64_t oldTag=0,oldKey=0,oldOrder=0;size_t depth=0;
            for(auto &slot:slots_) {
                const auto tag=slot.tag.load(std::memory_order_acquire);
                if((tag&3)!=Ready || slot.epoch.load(std::memory_order_acquire)!=generation) continue;
                const auto key=slot.metadata.load(std::memory_order_acquire),order=slot.order.load(std::memory_order_acquire);
                if(slot.tag.load(std::memory_order_acquire)!=tag) continue;
                ++depth;
                if(!oldest || delta(uint32_t(key),uint32_t(oldKey))<0 || (uint32_t(key)==uint32_t(oldKey) && order<oldOrder)) { oldest=&slot;oldTag=tag;oldKey=key;oldOrder=order; }
            }
            if(!oldest) { exhausted=false;break; }
            auto expected=oldTag;
            if(!oldest->tag.compare_exchange_strong(expected,(oldTag&~uint64_t(3))|Reading,std::memory_order_acq_rel)) { ++takeRetries;continue; }
            const auto frame=oldest->frame;
            if(nowMs-frame.localMs>200) {
                oldest->tag.store(oldTag&~uint64_t(3),std::memory_order_release);++dropped;continue;
            }
            if(!primed_ && (depth>=4 || nowMs-frame.localMs>=80)) primed_=true;
            if(!primed_) { oldest->tag.store(oldTag,std::memory_order_release);exhausted=false;break; }
            played_.store((uint64_t(1)<<32)|frame.timestamp,std::memory_order_release);
            oldest->tag.store(oldTag&~uint64_t(3),std::memory_order_release);consecutive_=0;return frame;
        }
        if(exhausted) ++takeExhausted;
        if(primed_) { ++underruns;if(++consecutive_>=4) primed_=false; }
        Frame silence;silence.localMs=nowMs;return silence;
    }
    /** @return 近似待播放深度，0..10；并发到达可使相邻快照不同，不消费槽。 */
    size_t pendingFrames() const { size_t count=0;const auto e=epoch_.load();for(const auto &s:slots_) count+=((s.tag.load(std::memory_order_acquire)&3)==Ready && s.epoch.load()==e);return count; }
    /** @return 当前通话代次；接收函数应在解析之前保存，push时提交以排除迟到旧调用。 */
    uint32_t generation() const { return epoch_.load(std::memory_order_acquire); }
    std::atomic<uint64_t> dropped{0},underruns{0},contentionDrops{0}; /**< 丢弃、已预缓冲欠载与达到CAS扫描上限次数；非PCM内存竞态。 */
    std::atomic<uint64_t> pushRetries{0},takeRetries{0},takeExhausted{0}; /**< CAS碰撞及消费扫描上限次数；不把无互斥误写为无调度迟到。 */
    std::atomic<size_t> highWater{0}; /**< 生产者扫描估计峰值，最多10帧。 */
private:
    /** @brief 32位时间戳有符号模差，要求真实差小于2^31毫秒。
     * @param[in] a 待比较时间戳。
     * @param[in] b 基准时间戳。
     * @return 正值a更新，负值a更旧。
     */
    static int64_t delta(uint32_t a,uint32_t b) { const uint32_t d=a-b;return d<=0x7fffffffU ? int64_t(d) : int64_t(d)-0x100000000LL; }
    uint64_t serial_=0; /**< 仅生产者递增的插入顺序，reset不改写，避免与在途生产者竞争。 */
    std::array<Slot,10> slots_{}; /**< 固定总容量；Reading槽仍占槽，生产者不能覆盖正在复制的PCM。 */
    std::atomic<uint32_t> epoch_{1}; /**< reset推进通话代次，生产/消费只接受对应槽，自然32位回绕。 */
    std::atomic<uint64_t> played_{0}; /**< bit32为有效标志，低32位最后播放时间戳；消费者写、生产者读。 */
    bool primed_=false;int consecutive_=0; /**< 仅播放线程访问的预缓冲/连续欠载状态。 */
};
}
