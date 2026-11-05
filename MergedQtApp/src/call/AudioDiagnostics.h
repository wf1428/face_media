/**
 * @file AudioDiagnostics.h
 * @brief 音频后台独占的ALSA只读参数与单次传输时序快照，不保存PCM或改变控制流程。
 * @author Du lin
 * @date 2026-11-03
 * @note 最近修改：2026-11-04；修改人：Du lin；新增直接线程CPU时钟差值工具及有界时间重查计数；非CPU时间包含主动等待/锁/调度，未知返回-1；保持纯数值后台交接。
 */
#pragma once
#include <cstdint>
#ifdef __linux__
#include <time.h>
#endif

namespace VoiceCall {
/** @return 本线程累计CPU微秒；Linux直接读取线程CPU时钟，其他平台或失败为-1，不读/proc、不等待。 */
inline int64_t threadCpuUs() {
#ifdef __linux__
    timespec value{};if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)==0) return int64_t(value.tv_sec)*1000000+value.tv_nsec/1000;
#endif
    return -1;
}
/** @brief 计算线程CPU快照差值。
 * @param[in] before 起始CPU微秒，-1表示未知。
 * @param[in] after 结束CPU微秒，-1表示未知。
 * @return 非负差值；任一未知或倒退为-1，不能将未知当成0。
 */
inline int64_t cpuElapsedUs(int64_t before,int64_t after) { return before>=0 && after>=before ? after-before : -1; }
/** @brief 估计非CPU时长，包括主动等待/锁/调度，不能直接等同被抢占。
 * @param[in] wall 对应工作段墙钟微秒。
 * @param[in] cpu 同一段CPU微秒，-1为未知。
 * @return cpu未知为-1，否则返回max(0,wall-cpu)，仅诊断不参与控制。
 */
inline int64_t offCpuUs(int64_t wall,int64_t cpu) { return cpu<0 ? -1 : wall>cpu ? wall-cpu : 0; }

/** @brief 应用PCM打开后的实际缓冲参数；plughw数值不等同于底层硬件DMA几何。
 * @note 仅读取一次；-1表示查询失败，不影响原有录放启动。
 */
struct AlsaGeometry {
    int64_t bufferFrames=-1,periodFrames=-1,startThreshold=-1,availMin=-1; /**< 应用采样率下的音频帧数，非字节数。 */
    int paramsRc=1,swRc=1; /**< 0成功、负值ALSA错误、1未查询。 */
};
/** @brief 最近一次transfer的固定大小诊断；只由设备所属线程读写。
 * @details 时间均为CLOCK_MONOTONIC微秒或其时长；不输出PCM，不因诊断增加逐帧ALSA状态查询。
 */
struct AlsaTransferTrace {
    uint64_t sequence=0,xrunsBefore=0,xrunsAfter=0; /**< 本次open内调用序号及实例累计错误恢复计数。 */
    int64_t beginUs=0,endUs=0,entryGapUs=0,lastWriteBeforeUs=0; /**< 调用起止、与上次入口间隔、入口前最近成功I/O时刻；0表示未知。 */
    int64_t ioUs=0,ioMaxUs=0,waitUs=0,waitMaxUs=0,timingUs=0,recoverUs=0,recoveryLogUs=0; /**< 对应调用总耗时及单次最大耗时，微秒；recoveryLogUs保留兼容，恢复不再现场输出文本，当前为0。 */
    int ioCalls=0,waitCalls=0,waitTimeouts=0,again=0,interrupts=0,zero=0,partial=0; /**< I/O、等待及各返回条件次数。 */
    int frames=0; /**< 最终已传输帧数；恢复按原逻辑归零，非跨恢复累计。 */
    int64_t available=-1,delayFrames=-1,previousDelayFrames=-1,queryEndUs=0,previousQueryUs=0; /**< 当前/前次既有时间查询的帧数和查询结束时刻；-1/0表示未知。 */
    unsigned queryAttempts=0;int64_t firstQueryUs=0; /**< 本帧查询0/1/2次与首查跨度微秒，关闭计时均为0。 */
    int queryRc=1,state=-1; /**< 既有时间查询结果及状态；1/-1表示未查询。 */
    int faultCode=0,faultSource=0,faultState=-1,recoverRc=0,offsetAtFault=0; /**< 首个错误码、来源(1=I/O,2=wait)、当时状态、最近prepare结果及首个错误前已传帧数。 */
    int64_t faultUs=0,faultSinceWriteUs=-1; /**< 首个错误的单调时刻及距最近成功I/O时长；-1未知。 */
    bool completed=false; /**< true仅完整320采样传输完成；取消/超时/失败为false。 */
    bool queryRunning=false; /**< 既有时间查询时PCM处于RUNNING；未查询为false，仅诊断使用。 */
};
}
