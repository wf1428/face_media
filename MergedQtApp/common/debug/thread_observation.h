/**
 * @file thread_observation.h
 * @brief 只读观察本线程CPU、切换、页错误和CPU迁移，不读取/proc、不调整调度。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include <cstdint>
#include <algorithm>
#ifdef __linux__
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sched.h>
#endif
namespace ThreadDiagnostics {
/** @brief 当前线程的累计资源快照；未知字段为-1，不能用0代替不支持。 */
struct Sample {
    int64_t cpuUs=-1,voluntary=-1,involuntary=-1,minorFaults=-1,majorFaults=-1; /**< CPU微秒、主动/非主动切换及缺页累计次数。 */
    int cpu=-1; /**< 本次采样的逻辑CPU编号；不是线程亲和性配置。 */
};
/** @brief 同一线程两个快照的差值，仅诊断，不参与音频或业务判定。 */
struct Delta {
    int64_t cpuUs=-1,offCpuUs=-1,voluntary=-1,involuntary=-1,minorFaults=-1,majorFaults=-1; /**< CPU/非CPU微秒及切换/缺页次数；-1未知。 */
    int cpuBegin=-1,cpuEnd=-1; /**< 两端逻辑CPU；不同只证明观测到迁移，相同不保证途中无迁移。 */
};
/** @return Linux内核TID；其他平台为0。只查询本线程，不打开文件或等待其他线程。 */
inline int64_t nativeTid() {
#ifdef __linux__
    return ::syscall(SYS_gettid);
#else
    return 0;
#endif
}
/** @return 本线程CPU时钟和getrusage(RUSAGE_THREAD)；不支持或失败保留-1。 */
inline Sample sample() {
    Sample result;
#ifdef __linux__
    timespec time{};
    if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&time)==0) result.cpuUs=int64_t(time.tv_sec)*1000000+time.tv_nsec/1000;
    rusage usage{};
    if(::getrusage(RUSAGE_THREAD,&usage)==0) {
        result.voluntary=usage.ru_nvcsw;result.involuntary=usage.ru_nivcsw;
        result.minorFaults=usage.ru_minflt;result.majorFaults=usage.ru_majflt;
    }
    result.cpu=::sched_getcpu();
#endif
    return result;
}
/** @brief 累计值差；任一未知或倒退时返回-1。
 * @param[in] begin 开始累计值，-1未知。
 * @param[in] end 结束累计值，-1未知。
 * @return 非负差值或-1。
 */
inline int64_t difference(int64_t begin,int64_t end) { return begin>=0 && end>=begin ? end-begin : -1; }
/** @brief 计算同一线程、同一作用域的资源差。
 * @param[in] begin 开始快照。
 * @param[in] end 结束快照。
 * @param[in] wallUs 同作用域的单调墙钟微秒。
 * @return 非CPU包含主动阻塞、睡眠和调度等；不能单凭它认定被抢占或锁争用。
 */
inline Delta delta(const Sample &begin,const Sample &end,int64_t wallUs) {
    Delta result;result.cpuUs=difference(begin.cpuUs,end.cpuUs);
    if(result.cpuUs>=0) result.offCpuUs=std::max<int64_t>(0,wallUs-result.cpuUs);
    result.voluntary=difference(begin.voluntary,end.voluntary);result.involuntary=difference(begin.involuntary,end.involuntary);
    result.minorFaults=difference(begin.minorFaults,end.minorFaults);result.majorFaults=difference(begin.majorFaults,end.majorFaults);
    result.cpuBegin=begin.cpu;result.cpuEnd=end.cpu;return result;
}
}
