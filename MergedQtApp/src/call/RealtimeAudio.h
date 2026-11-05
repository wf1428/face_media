/**
 * @file RealtimeAudio.h
 * @brief 提供固定容量音频队列、无等待单生产者交接及有界播放追赶节拍。
 * @author Du lin
 * @date 2026-11-03
 */
#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace VoiceCall {
/** @brief 外部同步的固定容量FIFO；入队失败不覆盖，调用者显式决定丢帧策略。
 * @tparam T 自有值类型，无动态成员时队列操作不分配内存。
 * @tparam Capacity 可用槽数，必须大于0。
 */
template<class T,size_t Capacity> class FixedQueue {
    static_assert(Capacity>0,"non-empty storage required");
public:
    /** @brief 丢弃全部值；调用方保证没有并发访问。 */
    void clear() { head_=count_=0; }
    /** @return 当前帧数，外部同步。 */
    size_t size() const { return count_; }
    /** @return true表示无待消费值。 */
    bool empty() const { return count_==0; }
    /** @return 队首借用；仅在非空且外部同步期间有效。 */
    const T &front() const { return data_[head_]; }
    /** @brief 丢弃队首；前提为非空并已外部同步。 */
    void pop_front() { head_=(head_+1)%Capacity;--count_; }
    /** @brief 拷贝值到下一个空槽。
     * @param[in] value 入队值。
     * @return false表示已满且队列未改变；true表示入队成功。
     */
    bool push_back(const T &value) {
        if(count_==Capacity) return false;
        data_[(head_+count_)%Capacity]=value;++count_;return true;
    }
private:
    std::array<T,Capacity> data_{}; /**< 内嵌存储，生命周期内不增减分配。 */
    size_t head_=0,count_=0; /**< 环形队首及有效槽数，调用方同步。 */
};

/** @brief 固定容量SPSC交接；满时拒绝新值，生产者绝不等待消费者或改写其槽。
 * @tparam T 可拷贝的自有值；音频路径只使用无动态成员类型。
 * @tparam Capacity 可用槽数；额外一个槽区分满与空。
 * @note 同一实例只能各有一个生产者和消费者；reset仅可在两者停止时调用。
 *       release/acquire发布完整值，32位原子须由目标平台无锁实现。
 */
template<class T,size_t Capacity> class SpscQueue {
    static_assert(Capacity>0 && Capacity<0xffffffffU,"valid ring capacity required");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,"audio indices must be lock free");
public:
    /** @brief 停止状态清空索引，不清写PCM内容；不得与push/pop并发。 */
    void reset() { write_.store(0);read_.store(0); }
    /** @brief 单生产者发布一份完整快照。
     * @param[in] value 要复制的自有值。
     * @return true成功；false已满，旧值保持不变。
     */
    bool push(const T &value) {
        const auto w=write_.load(std::memory_order_relaxed),next=uint32_t((w+1)%(Capacity+1));
        if(next==read_.load(std::memory_order_acquire)) return false;
        data_[w]=value;write_.store(next,std::memory_order_release);return true;
    }
    /** @brief 单消费者取下一份快照。
     * @param[out] value 成功时复制完整值；空队列时不变。
     * @return true成功；false为空。
     */
    bool pop(T &value) {
        const auto r=read_.load(std::memory_order_relaxed);
        if(r==write_.load(std::memory_order_acquire)) return false;
        value=data_[r];read_.store(uint32_t((r+1)%(Capacity+1)),std::memory_order_release);return true;
    }
private:
    std::array<T,Capacity+1> data_{}; /**< 创建时一次性固定存储；不使用堆队列。 */
    alignas(64) std::atomic<uint32_t> write_{0}; /**< 只由生产者写，消费者acquire读取。 */
    alignas(64) std::atomic<uint32_t> read_{0}; /**< 只由消费者写，生产者acquire读取。 */
};

/** @brief 20ms绝对节拍；迟到或XRUN后最多连续补写两帧，不无限追赶陈旧截止时间。
 * @details 不生成或重复PCM；每次循环仍从原抖动队列取下一帧，ALSA仍负责容量/部分写入。
 *          XRUN后允许当前帧以外再补两帧恢复最多60ms储备；超长停顿最多两次立即执行后重建截止时间。
 */
class PlaybackPacer {
public:
    /** @brief 以首个播放循环入口建立时间线。
     * @param[in] nowUs 同一本地单调时钟微秒。
     */
    explicit PlaybackPacer(int64_t nowUs):deadlineUs_(nowUs) {}
    /** @brief 完成一帧后计算下次循环截止时刻。
     * @param[in] nowUs 本帧完成后的单调微秒。
     * @param[in] recovered 本帧是否发生ALSA XRUN/挂起恢复。
     * @return 单调微秒截止时刻，等于nowUs表示立即继续，绝不返回过去时间。
     */
    int64_t advance(int64_t nowUs,bool recovered) {
        if(recovered) { deadlineUs_=nowUs-40000;immediate_=0; }
        deadlineUs_+=20000;
        if(deadlineUs_<=nowUs) {
            if(immediate_<2) { ++immediate_;++catchups;return nowUs; }
            deadlineUs_=nowUs+20000;immediate_=0;++rebases;
        } else immediate_=0;
        return deadlineUs_;
    }
    uint64_t catchups=0,rebases=0; /**< 立即补写和债务丢弃次数，非网络丢帧；仅播放线程访问。 */
private:
    int64_t deadlineUs_=0; /**< 下一截止时间线，单调微秒。 */
    unsigned immediate_=0; /**< 已连续立即执行次数，最多2次。 */
};
}
