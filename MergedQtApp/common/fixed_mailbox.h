/**
 * @file fixed_mailbox.h
 * @brief 用固定槽所有权交接自有值，避免在提交/消费线程等待互斥或分配队列节点。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

/** @brief 固定容量多生产者邮箱；每槽版本防止ABA，操作仅有界扫描和CAS。
 * @tparam T 可移动自有值，移动赋值/条件/析构不得抛出或阻塞；成本由调用者负责；音频必须使用无动态成员值。
 * @tparam Capacity 槽数，包含正在读写的槽。
 * @details 同一生产者的已完成提交保持顺序；并发生产者按发布序号择取。生产者暂停在Writing不阻止其他槽。
 * 普通模式满时拒绝；replaceOldest仅供单生产者音频，淘汰最旧Ready，不覆盖正在Reading的值。
 * 正常通路使用单消费者；退出清理也必须通过pop认领槽，不得直接改payload。
 * 不依赖Qt事件或外部锁。析构前必须停止全部访问者；不提供并发重置索引的接口。
 */
template<class T,size_t Capacity> class FixedMailbox {
    static_assert(Capacity>0,"fixed storage must not be empty");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,"slot ownership must not use library locks");
    /** @brief tag低2位所有权，其余位为发布版本。 */
    enum :uint64_t {
        Empty=0,   /**< 无访问者，生产者可CAS取得。 */
        Writing=1, /**< 生产者独占payload；其他线程不等待此槽。 */
        Ready=2,   /**< 只读发布，消费者或满时覆盖者可CAS取得。 */
        Reading=3  /**< 消费者独占，生产者不得覆盖。 */
    };
    /** @brief payload只能由Writing或Reading所有者访问；窥视排序只读取原子字段。 */
    struct Slot {
        std::atomic<uint64_t> tag{Empty},order{0}; /**< 版本/所有权及发布顺序；仅原子读取，不窥视payload。 */
        T value{}; /**< 仅Writing/Reading所有者访问；发布后消费者负责移出和释放。 */
    };
public:
    /** @brief 有界提交；移入槽后消费者拥有值。
     * @param[in] value 自有待发送值，失败时由本次调用释放，不改变已发布旧值。
     * @param[in] replaceOldest true只用于单生产者：无空槽时淘汰最旧未读值。
     * @param[out] replaced 可空，成功且覆盖旧Ready时为true，其余false。
     * @return true发布成功；false容量/有界竞争拒绝，无等待或隐式重试任务。
     */
    bool push(T value,bool replaceOldest=false,bool *replaced=nullptr) {
        if(replaced) *replaced=false;
        for(size_t attempt=0;attempt<Capacity+2;++attempt) {
            Slot *target=nullptr,*oldest=nullptr;uint64_t targetTag=0,oldTag=0,oldOrder=0;
            for(auto &slot:slots_) {
                const auto tag=slot.tag.load(std::memory_order_acquire);
                if((tag&3)==Empty) { target=&slot;targetTag=tag;break; }
                if(replaceOldest && (tag&3)==Ready) {
                    const auto order=slot.order.load(std::memory_order_relaxed);
                    if(slot.tag.load(std::memory_order_acquire)!=tag) continue;
                    if(!oldest || order<oldOrder) { oldest=&slot;oldTag=tag;oldOrder=order; }
                }
            }
            const bool overwrite=!target;
            if(!target) { if(!replaceOldest || !oldest) return false;target=oldest;targetTag=oldTag; }
            auto expected=targetTag;
            if(!target->tag.compare_exchange_strong(expected,(targetTag&~uint64_t(3))|Writing,std::memory_order_acq_rel)) continue;
            target->value=std::move(value);
            target->order.store(serial_.fetch_add(1,std::memory_order_relaxed)+1,std::memory_order_relaxed);
            target->tag.store(((targetTag&~uint64_t(3))+4)|Ready,std::memory_order_release);
            if(replaced) *replaced=overwrite;
            return true;
        }
        return false;
    }
    /** @brief 消费最早发布的值；空或有界竞争失败不改输出。
     * @param[out] value 成功时移出自有值；消费者负责释放，槽内移入的旧输出也在本消费者清理，生产者不回收大正文。
     * @return true取得值，false暂时没有可取得的Ready槽。
     */
    bool pop(T &value) { return popIf(value,[](const T &){return true;}); }
    /** @brief 取得所有权后条件消费，条件不满足保持原序号/值。
     * @tparam Predicate 不抛异常、不阻塞且不保存payload引用的条件。
     * @param[out] value 成功移出的自有值。
     * @param[in] accept 在Reading所有权内检查，只允许常量时间判断。
     * @return true消费，false空/竞争/条件拒绝；拒绝不修改输出。
     */
    template<class Predicate> bool popIf(T &value,Predicate accept) {
        for(size_t attempt=0;attempt<Capacity+2;++attempt) {
            Slot *oldest=nullptr;uint64_t oldTag=0,oldOrder=0;
            for(auto &slot:slots_) {
                const auto tag=slot.tag.load(std::memory_order_acquire);if((tag&3)!=Ready) continue;
                const auto order=slot.order.load(std::memory_order_relaxed);if(slot.tag.load(std::memory_order_acquire)!=tag) continue;
                if(!oldest || order<oldOrder) { oldest=&slot;oldTag=tag;oldOrder=order; }
            }
            if(!oldest) return false;
            auto expected=oldTag;
            if(!oldest->tag.compare_exchange_strong(expected,(oldTag&~uint64_t(3))|Reading,std::memory_order_acq_rel)) continue;
            if(!accept(oldest->value)) { oldest->tag.store(oldTag,std::memory_order_release);return false; }
            value=std::move(oldest->value);
            // Qt隐式共享对象的移动赋值可能交换旧值；在消费者清空，避免下个生产者释放大正文。
            oldest->value=T{};
            oldest->tag.store(oldTag&~uint64_t(3),std::memory_order_release);return true;
        }
        return false;
    }
    /** @return Ready槽近似数，范围0..Capacity；并发快照不代表线性化队列长度。 */
    size_t size() const { size_t count=0;for(const auto &slot:slots_) count+=(slot.tag.load(std::memory_order_acquire)&3)==Ready;return count; }
    /** @return 当前没有Ready槽；Writing中的值可能随后发布。 */
    bool empty() const { return size()==0; }
private:
    std::array<Slot,Capacity> slots_{}; /**< 固定拥有payload及槽状态，不分配节点。 */
    std::atomic<uint64_t> serial_{0}; /**< 发布排序，仅诊断/取最旧使用，实际运行不得跨2^64次发布回绕。 */
};
