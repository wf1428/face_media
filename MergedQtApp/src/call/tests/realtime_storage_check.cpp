/**
 * @file realtime_storage_check.cpp
 * @brief 单独替换C++分配入口，验证音频固定队列与补写节拍稳态循环不申请堆内存。
 * @author Du lin
 * @date 2026-11-03
 * @note 最近修改：2026-11-04；修改人：Du lin；将无互斥下行槽/PCM切换加入零分配守卫。
 * @note 只测试项目队列/节拍，不覆盖ALSA/Qt/系统库内部的分配；独立可执行文件，禁止链接进应用。
 */
#include "../CallProtocol.h"
#include "../RealtimeAudio.h"
#include "../ConcurrentJitterBuffer.h"
#include "../CaptureContinuity.h"
#include <cstdlib>
#include <cstdio>
#include <new>

namespace {
bool watching=false;size_t allocations=0; /**< 仅main线程的测试开关和new/new[]调用数。 */
}
/** @brief 测试专用new入口；观察期开启时累计，失败遵守bad_alloc约定。
 * @param[in] size 申请字节数，0按1字节申请。
 * @return malloc分配、由下方delete归还的指针。
 * @throws std::bad_alloc malloc失败。
 */
void *operator new(size_t size) {
    if(watching) ++allocations;
    if(void *p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
/** @brief 数组分配同样经过计数入口。
 * @param[in] size 字节数。
 * @return 已分配存储，失败抛bad_alloc。
 */
void *operator new[](size_t size) { return ::operator new(size); }
/** @brief 释放测试分配；空指针允许。
 * @param[in] p 要释放的malloc存储。
 */
void operator delete(void *p) noexcept { std::free(p); }
/** @brief 数组释放；与测试new[]配对。
 * @param[in] p 允许为空的已分配存储。
 */
void operator delete[](void *p) noexcept { std::free(p); }
/** @brief 有大小的C++释放重载，大小不影响malloc存储释放。
 * @param[in] p 允许为空的已分配存储。
 * @param[in] size 编译器传入的大小，本测试不使用。
 */
void operator delete(void *p,size_t size) noexcept { (void)size;std::free(p); }
/** @brief 有大小的数组释放重载。
 * @param[in] p 允许为空的已分配存储。
 * @param[in] size 编译器传入的数组大小，本测试不使用。
 */
void operator delete[](void *p,size_t size) noexcept { (void)size;std::free(p); }
/** @brief 执行覆盖插入/溢出/出队/清空/回绕的2万周期，检查new次数。
 * @return 0全部约束满足；1出现堆分配或固定队列行为失败。
 */
int main() {
    VoiceCall::JitterBuffer incoming;VoiceCall::FixedQueue<VoiceCall::Frame,5> outgoing;
    VoiceCall::SpscQueue<VoiceCall::Frame,7> transfer;VoiceCall::PlaybackPacer pacer(0);
    VoiceCall::ConcurrentJitterBuffer downlink;VoiceCall::CaptureContinuity continuity;
    watching=true;bool correct=true;
    for(uint32_t n=0;n<20000;++n) {
        VoiceCall::Frame f;f.timestamp=n*20;f.localMs=int64_t(n)*20;f.seq=uint8_t(n);f.pcm[0]=int16_t(n%32768);
        correct=incoming.push(f) && correct;downlink.push(f);if(n%3==0) downlink.take(f.localMs);
        VoiceCall::Pcm fallback;continuity.prepare(f.pcm,fallback);continuity.finish(f.pcm,n%5!=0,&fallback);
        if(n%3==0) incoming.take(f.localMs);
        if(outgoing.size()==5) outgoing.pop_front();
        correct=outgoing.push_back(f) && correct;
        correct=transfer.push(f) && correct;VoiceCall::Frame result;
        correct=transfer.pop(result) && result.timestamp==f.timestamp && correct;
        pacer.advance(int64_t(n)*20000+(n%50==0 ? 45000 : 0),n%1000==0);
        if(n%97==0) { incoming.clear();outgoing.clear(); }
    }
    watching=false;
    std::printf("realtime storage: cycles=20000 heapAllocations=%zu correct=%d\n",allocations,int(correct));
    return allocations==0 && correct ? 0 : 1;
}
