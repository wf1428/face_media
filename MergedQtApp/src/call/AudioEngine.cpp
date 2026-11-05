/**
 * @file AudioEngine.cpp
 * @brief 实现有限等待的全双工音频线程、静音补帧和音频设备异步交接。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；上行使用固定5槽无等待交接，统计改采集try_lock发布与退出补齐，发送线程不碰统计互斥；保留满丢最旧、100ms过期、DSP及录放行为。
 */
#include "AudioEngine.h"
#include "AlsaAudioDevice.h"
#include "AudioOutputFocus.h"
#include "PcmDiagnosticRecorder.h"
#include "SchedulingDiagnostics.h"
#include "common/debug/thread_observation.h"
#include <QFileInfo>
#include "platform/rk3566_platform.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <QStringList>
#include <QCoreApplication>
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
/** @brief 格式化与同一作用域配对的切换/缺页/CPU证据，只有诊断线程调用。 */
QString phaseResources(const char *stage,const ThreadDiagnostics::Delta &r) {
    const auto prefix=QString::fromLatin1(stage);
    return QStringLiteral(" %1VCtx=%2 %1IVCtx=%3 %1MinFault=%4 %1MajFault=%5 %1CpuBegin=%6 %1CpuEnd=%7")
        .arg(prefix).arg(r.voluntary).arg(r.involuntary).arg(r.minorFaults).arg(r.majorFaults).arg(r.cpuBegin).arg(r.cpuEnd);
}
/** @brief 播放线程自身的累计资源计数；不支持或查询失败时cpuUs=-1。 */
struct PlaybackCpu {
    int64_t cpuUs=-1,voluntary=0,involuntary=0,minorFaults=0,majorFaults=0; /**< 微秒、切换次数及页错误次数；均为线程累计。 */
};
/** @brief 读取本线程资源快照，不读取/proc、不查询其他线程、不等待UI。
 * @return Linux getrusage(RUSAGE_THREAD)结果；其他平台或失败返回未知CPU快照。
 */
PlaybackCpu playbackCpu()
{
    PlaybackCpu result;
#ifdef Q_OS_LINUX
    rusage usage{};
    if(::getrusage(RUSAGE_THREAD,&usage)==0) {
        result.cpuUs=int64_t(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000+usage.ru_utime.tv_usec+usage.ru_stime.tv_usec;
        result.voluntary=usage.ru_nvcsw;result.involuntary=usage.ru_nivcsw;
        result.minorFaults=usage.ru_minflt;result.majorFaults=usage.ru_majflt;
    }
#endif
    return result;
}
/** @brief 单个播放循环的纯数值快照；ring只保存元数据，不保存音频。 */
struct PlaybackCycle {
    uint64_t sequence=0,rx=0,underruns=0,referenceGeneration=0; /**< 循环序号及队列/参考累计计数。 */
    int64_t beginUs=0,entryGapUs=0,cpuUs=-1,offCpuUs=-1,vCtx=0,ivCtx=0,minorFaults=0,majorFaults=0; /**< 单调微秒及与前次循环入口间的线程资源差值；offCpu包含正常睡眠/I/O，不能直接等同被抢占。 */
    int64_t queueCpuUs=-1,queueOffCpuUs=-1,alsaCpuUs=-1,alsaOffCpuUs=-1,workCpuUs=-1,workOffCpuUs=-1; /**< 本循环各工作段CPU/非CPU微秒，不含节拍睡眠；-1未知。 */
    int64_t queueLockUs=0,queueUs=0,alsaUs=0,refLockUs=0,referenceUs=0,statsUs=0,workUs=0; /**< 各阶段耗时，均为微秒；referenceUs含refLockUs。 */
    int64_t sleepRequestedUs=0,sleepUs=0,wakeLateUs=0,scheduleSlipUs=0,diagnosticUs=0,rxAgeMs=-1; /**< 请求/实际睡眠、超期及诊断耗时；接收新鲜度为毫秒。 */
    size_t queuedBefore=0,queuedAfter=0; /**< 同一队列锁内，take前后的待播放帧数。 */
    bool scheduleReset=false,completed=false,referenceValid=false,lowBuffer=false; /**< 节拍债务重置、完整传输、参考有效及低余量观测；lowBuffer只诊断，不单独触发补写。 */
    VoiceCall::LevelStats playedLevel;uint64_t playedFrames=0; /**< 播放线程累计数值副本，诊断线程发布，不共享播放写锁。 */
    uint64_t catchups=0,rebases=0; /**< 补写/舍弃超长节拍债务计数。 */
    ThreadDiagnostics::Delta queueResources,alsaResources,workResources,sleepResources; /**< 与取帧/ALSA/完整工作/节拍等待段配对的资源差；不读文件。 */
    VoiceCall::AlsaTransferTrace alsa; /**< 与本次循环对应的设备快照，不重新查询设备。 */
};
/** @brief 诊断线程独占的固定16槽历史和累计极值；播放线程只交接纯数值，不格式化。 */
class PlaybackRecorder {
public:
    /** @brief 写入一条完成或失败的循环快照并累计极值。
     * @param[in] value 自有数值快照；不保存外部指针。
     */
    void add(const PlaybackCycle &value) {
        history_[cursor_]=value;cursor_=(cursor_+1)%history_.size();count_=std::min(count_+1,history_.size());++cycles_;
        gapMax_=std::max(gapMax_,value.entryGapUs);if(value.workUs>workMax_) { workMax_=value.workUs;workAtMaxCpu_=value.workCpuUs;workAtMaxOffCpu_=value.workOffCpuUs; }
        queryRetries_+=value.alsa.queryAttempts==2;queryRecovered_+=value.alsa.queryAttempts==2 && value.referenceValid;wakeMax_=std::max(wakeMax_,value.wakeLateUs);
        ioMax_=std::max(ioMax_,value.alsa.ioMaxUs);waitMax_=std::max(waitMax_,value.alsa.waitMaxUs);queryMax_=std::max(queryMax_,value.alsa.timingUs);
        lockMax_=std::max(lockMax_,std::max(value.queueLockUs,value.refLockUs));statsMax_=std::max(statsMax_,value.statsUs);
        resets_+=value.scheduleReset;lowBufferTicks_+=value.lowBuffer;
        if(value.alsa.queryRc==0 && value.alsa.queryRunning && value.alsa.delayFrames>=0) {
            const auto delay=value.alsa.delayFrames*1000000/16000;
            delayMin_=delayMin_<0 ? delay : std::min(delayMin_,delay);
        }
    }
    /** @brief 将本次诊断自身耗时附加到刚写入记录；不改变任何播放判定。
     * @param[in] us 格式化/发信号的微秒耗时，不包括UI接收者稍后执行的日志输出。
     */
    void diagnosticCost(int64_t us) { if(count_) history_[(cursor_+history_.size()-1)%history_.size()].diagnosticUs=us;diagMax_=std::max(diagMax_,us); }
    /** @brief 判断异常详情限频；XRUN/失败前三批保留，慢循环/低余量前两批保留，此后各最多10秒一批。
     * @param[in] xrun true为XRUN/失败类，false为慢循环/低余量类。
     * @param[in] nowUs 单调微秒。
     * @return true输出本批；false只累加省略计数，正常周期仍输出极值。
     */
    bool allow(bool xrun,int64_t nowUs) {
        auto &last=xrun ? lastXrun_ : lastSlow_;auto &number=xrun ? xrunReports_ : slowReports_;
        if(number<(xrun ? 3U : 2U) || nowUs-last>=10000000) { ++number;last=nowUs;return true; }
        ++omitted_;return false;
    }
    /** @brief 生成累计极值摘要；只有诊断线程/诊断退出后调用，无锁。
     * @param[in] phase tick或final。
     * @return 小型摘要，不包含PCM。
     */
    QString summary(const QString &phase) const {
        const VoiceCall::AlsaTransferTrace latest=count_ ? history_[(cursor_+history_.size()-1)%history_.size()].alsa : VoiceCall::AlsaTransferTrace{};
        return QStringLiteral("[call-play] phase=%1 cycles=%2 loopGapMaxUs=%3 workMaxUs=%4 wakeLateMaxUs=%5 ioMaxUs=%6 waitMaxUs=%7 queryMaxUs=%8")
            .arg(phase).arg(cycles_).arg(gapMax_).arg(workMax_).arg(wakeMax_).arg(ioMax_).arg(waitMax_).arg(queryMax_)
            +QStringLiteral(" lockWaitMaxUs=%1 statsMaxUs=%2 diagnosticMaxUs=%3 scheduleResets=%4 delayMinUs=%5 xrunReports=%6 slowReports=%7 detailOmitted=%8")
            .arg(lockMax_).arg(statsMax_).arg(diagMax_).arg(resets_).arg(delayMin_).arg(xrunReports_).arg(slowReports_).arg(omitted_)
            +QStringLiteral(" lastDelayUs=%1 lastAvailFrames=%2 lastQueryRc=%3 lastState=%4 xruns=%5 lastBeginMonoUs=%6 lowBufferTicks=%7")
            .arg(latest.queryRc==0 && latest.delayFrames>=0 ? latest.delayFrames*1000000/16000 : -1)
            .arg(latest.available).arg(latest.queryRc).arg(latest.state).arg(latest.xrunsAfter).arg(latest.beginUs).arg(lowBufferTicks_)
            +QStringLiteral(" workAtMaxCpuUs=%1 workAtMaxOffCpuUs=%2 queryRetries=%3 queryRetryRecovered=%4").arg(workAtMaxCpu_).arg(workAtMaxOffCpu_).arg(queryRetries_).arg(queryRecovered_)
            +QStringLiteral(" catchupFrames=%1 debtRebases=%2").arg(count_ ? history_[(cursor_+history_.size()-1)%history_.size()].catchups : 0).arg(count_ ? history_[(cursor_+history_.size()-1)%history_.size()].rebases : 0);
    }
    /** @brief 格式化最多指定条数的最近历史，按旧到新排列，仅异常时调用。
     * @param[in] reason xrun、post-xrun、slow或failed。
     * @param[in] limit 最多输出条数，内部受固定16槽限制。
     * @return 数值日志列表；当前批日志自身diagnosticUs尚未知，后续历史中才可见。
     */
    QStringList dump(const QString &reason,size_t limit) const {
        QStringList lines;const auto count=std::min(count_,limit);
        for(size_t i=count;i>0;--i) {
            const auto &c=history_[(cursor_+history_.size()-i)%history_.size()];const auto &a=c.alsa;
            lines.append(QStringLiteral("[call-play] trace=%1 seq=%2 beginMonoUs=%3 gapUs=%4 cpuUs=%5 offCpuUs=%6 vCtx=%7 ivCtx=%8 minFault=%9 majFault=%10")
                .arg(reason).arg(c.sequence).arg(c.beginUs).arg(c.entryGapUs).arg(c.cpuUs).arg(c.offCpuUs).arg(c.vCtx).arg(c.ivCtx).arg(c.minorFaults).arg(c.majorFaults)
                +QStringLiteral(" qBefore=%1 qAfter=%2 rx=%3 rxAgeMs=%4 underrun=%5 qLockUs=%6 qWorkUs=%7 alsaUs=%8 ioUs=%9 ioMaxUs=%10 ioCalls=%11")
                .arg(qulonglong(c.queuedBefore)).arg(qulonglong(c.queuedAfter)).arg(c.rx).arg(c.rxAgeMs).arg(c.underruns).arg(c.queueLockUs).arg(c.queueUs).arg(c.alsaUs).arg(a.ioUs).arg(a.ioMaxUs).arg(a.ioCalls)
                +QStringLiteral(" waitUs=%1 waitMaxUs=%2 waits=%3 waitTimeouts=%4 again=%5 partial=%6 queryUs=%7 queryRc=%8 state=%9 avail=%10 delayFrames=%11")
                .arg(a.waitUs).arg(a.waitMaxUs).arg(a.waitCalls).arg(a.waitTimeouts).arg(a.again).arg(a.partial).arg(a.timingUs).arg(a.queryRc).arg(a.state).arg(a.available).arg(a.delayFrames)
                +QStringLiteral(" refLockUs=%1 refUs=%2 refValid=%3 refGen=%4 statsUs=%5 workUs=%6 sleepReqUs=%7 sleepUs=%8 wakeLateUs=%9 reset=%10 slipUs=%11 diagUs=%12 ok=%13 lowBuffer=%14")
                .arg(c.refLockUs).arg(c.referenceUs).arg(c.referenceValid).arg(c.referenceGeneration).arg(c.statsUs).arg(c.workUs).arg(c.sleepRequestedUs).arg(c.sleepUs).arg(c.wakeLateUs).arg(c.scheduleReset).arg(c.scheduleSlipUs).arg(c.diagnosticUs).arg(c.completed).arg(c.lowBuffer)
                +QStringLiteral(" qCpuUs=%1 qOffCpuUs=%2 alsaCpuUs=%3 alsaOffCpuUs=%4 workCpuUs=%5 workOffCpuUs=%6 queryAttempts=%7 firstQueryUs=%8")
                .arg(c.queueCpuUs).arg(c.queueOffCpuUs).arg(c.alsaCpuUs).arg(c.alsaOffCpuUs).arg(c.workCpuUs).arg(c.workOffCpuUs).arg(a.queryAttempts).arg(a.firstQueryUs)
                +phaseResources("queue",c.queueResources)+phaseResources("alsa",c.alsaResources)+phaseResources("work",c.workResources)+phaseResources("sleep",c.sleepResources));
        }
        return lines;
    }
private:
    std::array<PlaybackCycle,16> history_{};size_t cursor_=0,count_=0; /**< 线程独占环形元数据历史，满时覆盖最旧。 */
    uint64_t cycles_=0,resets_=0,omitted_=0,lowBufferTicks_=0;unsigned xrunReports_=0,slowReports_=0; /**< 本次run累计次数，非丢帧数；lowBufferTicks为低余量观测。 */
    uint64_t queryRetries_=0,queryRecovered_=0;int64_t workAtMaxCpu_=-1,workAtMaxOffCpu_=-1; /**< 重查/恢复次数及最大工作墙钟对应CPU证据。 */
    int64_t gapMax_=0,workMax_=0,wakeMax_=0,ioMax_=0,waitMax_=0,queryMax_=0,lockMax_=0,statsMax_=0,diagMax_=0,delayMin_=-1; /**< 累计微秒极值；delayMin=-1未知。 */
    int64_t lastXrun_=0,lastSlow_=0; /**< 两类详情最近输出单调微秒。 */
};
/** @brief 播放线程发布的实际ALSA参考事件；序号缺口/失效使采集端先清时间线。 */
struct ReferenceEvent {
    VoiceCall::Pcm pcm{};int64_t firstUs=0;uint64_t sequence=0; /**< 成功提交PCM、单调首样本时间和事件序号，含失效事件。 */
    bool valid=false,reset=false,xrun=false; /**< valid允许push；reset先clear；xrun标记播放恢复原因。 */
    VoiceCall::AudioTiming timing;int queryRc=1,state=-1; /**< 实际播放时间判定证据，不另查ALSA。 */
};
/** @brief 单次采集和处理的纯数值诊断；不保存PCM，也不修改处理结果。 */
struct CaptureCycle {
    uint64_t index=0,xruns=0,generation=0; /**< 本地采集序号、设备恢复次数及参考代次。 */
    int64_t beginUs=0,endUs=0,gapUs=0,ioUs=0,waitUs=0,dspUs=0; /**< 读帧起止、连续读完间隔及各阶段单调微秒。 */
    int64_t readCpuUs=-1,readOffCpuUs=-1,gapCpuUs=-1,gapOffCpuUs=-1,dspCpuUs=-1,dspOffCpuUs=-1; /**< 录音读帧、连续完成间隔和处理段CPU/非CPU微秒；间隔包含上一帧处理及本次等待。 */
    ThreadDiagnostics::Delta readResources,dspResources,txResources;int64_t txLockWaitUs=0,txWorkUs=0; /**< 读帧/DSP/发送交接区段资源；txLockWaitUs保留兼容字段，固定槽/统计try_lock路径无等待，恒为0。 */
    VoiceCall::ProcessingObservation observation; /**< 四阶段纯数值波形证据，不保存音频。 */
    int rawRms=0,rawPeak=0,rawDc=0,outRms=0,outPeak=0,outDc=0; /**< 一帧处理前后幅度统计；满幅32768。 */
    uint64_t rawClip=0,outClip=0;int rawStep=0,outStep=0; /**< 近满幅采样数及跨相邻采集块边界的幅度差，不自动判为爆音。 */
    int faultCode=0,faultSource=0;int64_t faultUs=0,recoverUs=0; /**< ALSA首个异常来源及prepare耗时，仅数值。 */
    unsigned clearFlags=0;uint64_t playSequence=0;VoiceCall::AudioTiming playTiming;int playQueryRc=1,playState=-1; /**< 最近参考清理原因位及实际播放时间证据，见call-ref字段。 */
    bool referenceRequired=false;VoiceCall::AudioTiming captureTiming;VoiceCall::ReferenceReadTrace reference;
    int timingRc=1,timingState=-1;int64_t timingAvail=-1,timingDelay=-1; /**< 本帧既有ALSA查询证据，未知为-1/1，无新增查询。 */
    unsigned mode=0;bool aligned=false,failed=false; /**< bit0实际AEC，bit1实际预处理，bit2原PCM旁路；参考可用及传输失败。 */
};
/** @brief 仅诊断线程累计波形证据；大输入/削顶/平坦/突变详情限频，不据指标单独断言失真。 */
class ShapeRecorder {
public:
    /** @brief 累计本帧各有效阶段，并对大输入或异常波形线索最多3秒一批。 */
    QStringList add(const VoiceCall::ProcessingObservation &o,uint64_t index,int64_t now) {
        const std::array<VoiceCall::PcmShape,4> shapes{{o.raw,o.aec,o.preprocess,o.output}};
        unsigned reason=o.raw.peak>=12000 ? 1U:0U;
        for(size_t i=0;i<shapes.size();++i) if(o.valid&(1U<<i)) {
            const auto &v=shapes[i];auto &total=totals_[i];++total.frames;total.clipped+=v.clipped;total.rails+=v.rails;
            total.flatSamples+=v.flatPeak;total.flatRun=std::max(total.flatRun,v.flatRun);total.peak=std::max(total.peak,v.peak);
            total.step=std::max(total.step,v.adjacentStep);total.boundary=std::max(total.boundary,v.boundaryStep);total.last=v;
            if(v.clipped) reason|=2U<<i;
            if(v.flatRun>=4) reason|=32U;
        }
        if(o.output.adjacentStep>16000 || o.output.boundaryStep>12000) reason|=64U;
        latest_=o;latestIndex_=index;
        if(!reason || (reports_>=3 && now-lastUs_<3000000)) return {};
        ++reports_;lastUs_=now;QStringList lines;
        for(size_t i=0;i<shapes.size();++i) if(o.valid&(1U<<i)) lines.append(format("event",i,shapes[i],index,now,reason,o));
        return lines;
    }
    /** @return 全通话累计阶段峰值/削顶/平坦/突变和最后hash；无PCM正文。 */
    QStringList summary(const QString &phase) const {
        QStringList lines;static const char *names[]={"raw","aec","preprocess","upload"};
        for(size_t i=0;i<totals_.size();++i) {
            const auto &t=totals_[i];if(!t.frames) continue;
            lines.append(QStringLiteral("[call-shape] phase=%1 stage=%2 frames=%3 peakMax=%4 clipSamples=%5 railSamples=%6 flatPeakSamples=%7 flatRunMax=%8 adjacentStepMax=%9 boundaryStepMax=%10 lastIndex=%11 lastHash=%12")
                .arg(phase).arg(QString::fromLatin1(names[i])).arg(t.frames).arg(t.peak).arg(t.clipped).arg(t.rails).arg(t.flatSamples).arg(t.flatRun).arg(t.step).arg(t.boundary).arg(latestIndex_).arg(t.last.hash));
        }
        return lines;
    }
private:
    /** @brief 格式化同帧阶段证据，阶段名称固定，不输出原始样本。 */
    static QString format(const char *phase,size_t stage,const VoiceCall::PcmShape &v,uint64_t index,int64_t now,unsigned reason,const VoiceCall::ProcessingObservation &o) {
        static const char *names[]={"raw","aec","preprocess","upload"};
        return QStringLiteral("[call-shape] phase=%1 stage=%2 index=%3 captureDoneMonoUs=%4 reasonFlags=%5 rms=%6 acRms=%7 dc=%8 peak=%9 min=%10 max=%11 clip=%12 rails=%13")
            .arg(QString::fromLatin1(phase)).arg(QString::fromLatin1(names[stage])).arg(index).arg(now).arg(reason).arg(v.rms).arg(v.acRms).arg(v.dc).arg(v.peak).arg(v.minimum).arg(v.maximum).arg(v.clipped).arg(v.rails)
            +QStringLiteral(" flatPeak=%1 flatRun=%2 adjacentStep=%3 boundaryStep=%4 hash=%5 valid=%6 fallback=%7 agcGainValid=%8 agcGainDb=%9 loudness=%10 speechProb=%11")
            .arg(v.flatPeak).arg(v.flatRun).arg(v.adjacentStep).arg(v.boundaryStep).arg(v.hash).arg(o.valid).arg(o.fallback).arg(o.pre.gainValid).arg(o.pre.gainDb).arg(o.pre.loudness).arg(o.pre.speechProbability);
    }
    /** @brief 单阶段全通话累计；flat只表示候选线索，不代表已确认削顶。 */
    struct Total { uint64_t frames=0,clipped=0,rails=0,flatSamples=0;int peak=0,flatRun=0,step=0,boundary=0;VoiceCall::PcmShape last; };
    std::array<Total,4> totals_{};VoiceCall::ProcessingObservation latest_;uint64_t latestIndex_=0;int64_t lastUs_=0;unsigned reports_=0; /**< 有界累计和详情限频，全部诊断线程独占。 */
};
/** @brief 诊断线程独占的采集前后16帧历史；切换/异常限频保留前8后4帧。 */
class CaptureRecorder {
public:
    /** @brief 保存纯数值并生成限频异常批次；只在诊断线程调用。
     * @param[in] c 本次采集摘要。
     * @return 零或多行小型诊断文本，无PCM。
     */
    QStringList add(const CaptureCycle &c) {
        auto referenceLines=referenceLog(c);referenceLines+=shapes_.add(c.observation,c.index,c.endUs);
        history_[cursor_]=c;cursor_=(cursor_+1)%history_.size();count_=std::min(count_+1,history_.size());++frames_;
        const bool change=havePrevious_ && c.mode!=previousMode_;const bool xrun=c.xruns>previousXruns_;
        changes_+=change;previousMode_=c.mode;previousXruns_=c.xruns;havePrevious_=true;
        gapMax_=std::max(gapMax_,c.gapUs);if(c.dspUs>dspMax_) { dspMax_=c.dspUs;dspAtMaxCpu_=c.dspCpuUs;dspAtMaxOffCpu_=c.dspOffCpuUs; }
        queryRetries_+=c.captureTiming.queryAttempts==2;queryRecovered_+=c.captureTiming.queryAttempts==2 && c.captureTiming.valid;
        rawStepMax_=std::max(rawStepMax_,c.rawStep);outStepMax_=std::max(outStepMax_,c.outStep);
        QStringList lines=referenceLines;const bool trigger=change || xrun || c.failed || c.gapUs>30000 || c.dspUs>15000 || c.outStep>12000;
        if(trigger && (reports_<4 || c.endUs-lastReportUs_>=5000000)) {
            ++reports_;lastReportUs_=c.endUs;post_=4;
            lines+=dump(c.failed ? QStringLiteral("failed") : xrun ? QStringLiteral("capture-xrun") : change ? QStringLiteral("dsp-mode") : c.gapUs>30000 ? QStringLiteral("capture-gap") : c.dspUs>15000 ? QStringLiteral("slow-dsp") : QStringLiteral("pcm-step"),9);return lines;
        }
        if(trigger) ++omitted_;
        if(post_>0 && --post_==0) lines+=dump(QStringLiteral("post-transition"),4);
        return lines;
    }
    /** @brief 独立于慢采集详情限频的参考失败/恢复证据；每个原因首次必留，后续最多5秒一批。
     * @param[in] c 当前帧数值快照。
     * @return 原因变化/首次/恢复日志及计数，不输出PCM。
     */
    QStringList referenceLog(const CaptureCycle &c) {
        QStringList lines;if(!c.referenceRequired) return lines;
        if(c.captureTiming.queryAttempts==2 && (retryReports_<3 || c.endUs-lastRetryUs_>=5000000)) {
            ++retryReports_;lastRetryUs_=c.endUs;
            lines.append(QStringLiteral("[call-ref] event=query-retry index=%1 attempts=2 firstSpanUs=%2 firstCpuUs=%3 firstOffCpuUs=%4 finalSpanUs=%5 finalReason=%6 valid=%7")
                .arg(c.index).arg(c.captureTiming.firstQuerySpanUs).arg(c.captureTiming.firstQueryCpuUs).arg(c.captureTiming.firstQueryOffCpuUs)
                .arg(c.captureTiming.querySpanUs).arg(QString::fromLatin1(VoiceCall::timingReasonName(c.captureTiming.reason))).arg(c.captureTiming.valid));
        }
        if(c.aligned) {
            if(referenceLost_ && referenceEpisodeLogged_) lines.append(QStringLiteral("[call-ref] event=recovered index=%1 lostFrames=%2 captureFirstUs=%3 refGen=%4")
                .arg(c.index).arg(lostRun_).arg(c.captureTiming.firstSampleUs).arg(c.generation));
            referenceLost_=false;referenceEpisodeLogged_=false;lostRun_=0;return lines;
        }
        ++lostRun_;++referenceMisses_;const unsigned key=c.reference.reason==VoiceCall::ReferenceReason::CaptureTiming ? 8+unsigned(c.captureTiming.reason) : unsigned(c.reference.reason);
        const auto bit=uint64_t(1)<<key;const bool first=!(referenceSeen_&bit);referenceSeen_|=bit;
        if(key<referenceReasons_.size()) ++referenceReasons_[key];
        if(first || (!referenceLost_ && referenceReports_<3) || c.endUs-lastReferenceLogUs_>=5000000) {
            ++referenceReports_;referenceEpisodeLogged_=true;
            // 参考事件单独计数，普通capture-gap再多也不能挤掉首个具体失效原因。
            lines.append(QStringLiteral("[call-ref] event=lost index=%1 reason=%2 timingReason=%3 queryRc=%4 state=%5 avail=%6 delayFrames=%7 querySpanUs=%8 captureFirstUs=%9")
                .arg(c.index).arg(QString::fromLatin1(VoiceCall::referenceReasonName(c.reference.reason)))
                .arg(QString::fromLatin1(VoiceCall::timingReasonName(c.captureTiming.reason)))
                .arg(c.timingRc).arg(c.timingState).arg(c.timingAvail).arg(c.timingDelay).arg(c.captureTiming.querySpanUs).arg(c.captureTiming.firstSampleUs)
                +QStringLiteral(" requestedUs=%1 requestedEndUs=%2 oldestUs=%3 newestEndUs=%4 depth=%5 failedSample=%6 refGen=%7 lostRun=%8")
                .arg(c.reference.requestedUs).arg(c.reference.requestedEndUs).arg(c.reference.oldestUs).arg(c.reference.newestEndUs)
                .arg(qulonglong(c.reference.depth)).arg(c.reference.failedSample).arg(c.generation).arg(lostRun_)
                +QStringLiteral(" clearFlags=%1 playSeq=%2 playTimingReason=%3 playQueryRc=%4 playState=%5 playQuerySpanUs=%6 playDelayUs=%7")
                .arg(c.clearFlags).arg(c.playSequence).arg(QString::fromLatin1(VoiceCall::timingReasonName(c.playTiming.reason)))
                .arg(c.playQueryRc).arg(c.playState).arg(c.playTiming.querySpanUs).arg(c.playTiming.queueDelayUs)
                +QStringLiteral(" captureQueryCpuUs=%1 captureQueryOffCpuUs=%2 playQueryCpuUs=%3 playQueryOffCpuUs=%4")
                .arg(c.captureTiming.queryCpuUs).arg(c.captureTiming.queryOffCpuUs).arg(c.playTiming.queryCpuUs).arg(c.playTiming.queryOffCpuUs)
                +QStringLiteral(" captureQueryAttempts=%1 captureFirstQueryUs=%2 playQueryAttempts=%3 playFirstQueryUs=%4")
                .arg(c.captureTiming.queryAttempts).arg(c.captureTiming.firstQuerySpanUs).arg(c.playTiming.queryAttempts).arg(c.playTiming.firstQuerySpanUs));lastReferenceLogUs_=c.endUs;
        }
        referenceLost_=true;return lines;
    }
    /** @brief 参考原因累计；原因名与事件字段一致，不因详情限频丢失计数。
     * @param[in] phase tick或final。
     * @return 仅含有发生记录的原因及帧数。
     */
    QString referenceSummary(const QString &phase) const {
        QString text=QStringLiteral("[call-ref] phase=%1 misses=%2").arg(phase).arg(referenceMisses_);
        for(size_t i=0;i<referenceReasons_.size();++i) if(referenceReasons_[i]) {
            const char *reason=i>=8 ? VoiceCall::timingReasonName(VoiceCall::TimingReason(i-8)) : VoiceCall::referenceReasonName(VoiceCall::ReferenceReason(i));
            text+=QStringLiteral(" %1=%2").arg(QString::fromLatin1(reason)).arg(referenceReasons_[i]);
        }
        return text;
    }
    /** @brief 格式化最近历史；不含PCM正文。
     * @param[in] reason 触发原因。
     * @param[in] limit 最大条数，受16槽限制。
     * @return 旧到新诊断行。
     */
    QStringList dump(const QString &reason,size_t limit) const {
        QStringList lines;
        for(size_t n=std::min(count_,limit);n>0;--n) {
            const auto &c=history_[(cursor_+history_.size()-n)%history_.size()];
            lines.append(QStringLiteral("[call-capture] trace=%1 index=%2 beginMonoUs=%3 endMonoUs=%4 gapUs=%5 ioUs=%6 waitUs=%7 dspUs=%8 mode=%9 aligned=%10 refGen=%11 xruns=%12 failed=%13")
                .arg(reason).arg(c.index).arg(c.beginUs).arg(c.endUs).arg(c.gapUs).arg(c.ioUs).arg(c.waitUs).arg(c.dspUs).arg(c.mode).arg(c.aligned).arg(c.generation).arg(c.xruns).arg(c.failed)
                +QStringLiteral(" rawRms=%1 rawPeak=%2 rawDc=%3 rawClip=%4 outRms=%5 outPeak=%6 outDc=%7 outClip=%8 rawStep=%9 outStep=%10")
                .arg(c.rawRms).arg(c.rawPeak).arg(c.rawDc).arg(c.rawClip).arg(c.outRms).arg(c.outPeak).arg(c.outDc).arg(c.outClip).arg(c.rawStep).arg(c.outStep)
                +QStringLiteral(" readCpuUs=%1 readOffCpuUs=%2 gapCpuUs=%3 gapOffCpuUs=%4 dspCpuUs=%5 dspOffCpuUs=%6")
                .arg(c.readCpuUs).arg(c.readOffCpuUs).arg(c.gapCpuUs).arg(c.gapOffCpuUs).arg(c.dspCpuUs).arg(c.dspOffCpuUs)
                +QStringLiteral(" txLockWaitUs=%1 txWorkUs=%2").arg(c.txLockWaitUs).arg(c.txWorkUs)
                +phaseResources("read",c.readResources)+phaseResources("dsp",c.dspResources)+phaseResources("tx",c.txResources)
                +QStringLiteral(" code=%1 source=%2 faultMonoUs=%3 recoverUs=%4").arg(c.faultCode).arg(c.faultSource).arg(c.faultUs).arg(c.recoverUs));
        }
        return lines;
    }
    /** @brief 产生周期或结束汇总。
     * @param[in] phase tick或final。
     * @return 幅度边界/采集连续性极值，不代表平台播放器状态。
     */
    QString summary(const QString &phase) const {
        return QStringLiteral("[call-capture] phase=%1 frames=%2 gapMaxUs=%3 dspMaxUs=%4 rawStepMax=%5 outStepMax=%6 modeChanges=%7 detailOmitted=%8")
            .arg(phase).arg(frames_).arg(gapMax_).arg(dspMax_).arg(rawStepMax_).arg(outStepMax_).arg(changes_).arg(omitted_)
            +QStringLiteral(" dspAtMaxCpuUs=%1 dspAtMaxOffCpuUs=%2 queryRetries=%3 queryRetryRecovered=%4").arg(dspAtMaxCpu_).arg(dspAtMaxOffCpu_).arg(queryRetries_).arg(queryRecovered_);
    }
    /** @return 各有效PCM阶段的全通话累计，供低频tick/final输出。 */
    QStringList shapeSummary(const QString &phase) const { return shapes_.summary(phase); }
private:
    ShapeRecorder shapes_; /**< 诊断线程独占的阶段波形累计。 */
    std::array<uint64_t,18> referenceReasons_{};uint64_t referenceSeen_=0,referenceMisses_=0,lostRun_=0;bool referenceLost_=false,referenceEpisodeLogged_=false;unsigned referenceReports_=0;int64_t lastReferenceLogUs_=0; /**< 诊断线程独占的参考原因计数/去重，日志与普通慢帧限频独立。 */
    std::array<CaptureCycle,16> history_{};size_t cursor_=0,count_=0; /**< 固定元数据历史，只有诊断线程访问。 */
    uint64_t frames_=0,changes_=0,omitted_=0,previousXruns_=0;unsigned reports_=0,previousMode_=0;int post_=0;bool havePrevious_=false; /**< 本通话计数及后续详情剩余帧。 */
    uint64_t queryRetries_=0,queryRecovered_=0;int64_t dspAtMaxCpu_=-1,dspAtMaxOffCpu_=-1,lastRetryUs_=0;unsigned retryReports_=0; /**< 采集查询重查/恢复计数、限频及最大DSP墙钟的配对CPU证据。 */
    int64_t gapMax_=0,dspMax_=0,lastReportUs_=0;int rawStepMax_=0,outStepMax_=0; /**< 单调微秒及幅度差极值。 */
};
}

AudioEngine::AudioEngine(QObject *parent):QThread(parent) {}
AudioEngine::~AudioEngine() { stopAsync(); wait(); }
int64_t AudioEngine::nowMs() {
#ifdef Q_OS_LINUX
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC,&time)==0) return int64_t(time.tv_sec)*1000+time.tv_nsec/1000000;
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void AudioEngine::begin() {
    uiNativeTid_.store(ThreadDiagnostics::nativeTid());
    if (isRunning()) return;
    incoming_.reset();rxStatsEpoch_=0;rxFrames_=0;lastReceivedMs_=0;
    { std::lock_guard<std::mutex> lock(txMutex_);
      VoiceCall::Frame discarded;while(outgoing_.pop(discarded)) {}
      reference_=VoiceCall::PlaybackReference{};dspStats_={};captureDelayUs_=-1;
      referenceDepth_=0;referenceGeneration_=1;referencePhaseUs_=0;
      txDrops_=txDropFull_=txDropExpired_=txFrames_=0;txHighWater_=0;
      micLevel_={};lastCaptureMs_=captureGapMaxMs_=0; }
    for(auto &point:receivedLevels_) point.store(0);
    playedFrames_=0;playLevel_={};playbackDelayUs_=-1;statsStartMs_=0;
    talking_=false;stop_=false;start();
}
void AudioEngine::stopAsync() { talking_=false; stop_=true; }
void AudioEngine::setTalking(bool on) {
    if (on) statsStartMs_=nowMs();
    talking_=on;
}
void AudioEngine::receive(const QByteArray &packet) {
    const auto receiveGeneration=incoming_.generation();
    if (!talking_.load()) return;
    VoiceCall::Frame frame;
    if (!VoiceCall::decode(reinterpret_cast<const uint8_t *>(packet.constData()),packet.size(),nowMs(),frame)) return;
    VoiceCall::LevelStats level;level.add(frame.pcm); // 320点遍历与sqrt均在队列锁外。
    const auto rms=level.rms();
    if (!talking_.load() || !incoming_.push(frame,receiveGeneration)) return;
    {
      if(incoming_.generation()!=receiveGeneration || !talking_.load()) return;
      if(rxLevelEpoch_!=receiveGeneration) {
          rxLevelEpoch_=receiveGeneration;rxFramesLocal_=0;rxLevel_={};receivedLevelCursor_=0;
          for(auto &point:receivedLevels_) point.store(0);
      }
      rxFrames_=++rxFramesLocal_;lastReceivedMs_=frame.localMs;
      rxLevel_.samples+=level.samples;rxLevel_.clipped+=level.clipped;rxLevel_.sum+=level.sum;
      rxLevel_.squares+=level.squares;rxLevel_.peak=std::max(rxLevel_.peak,level.peak);
      receivedLevels_[receivedLevelCursor_].store((uint64_t(uint32_t(frame.localMs))<<32)|(uint64_t(level.peak)<<16)|uint64_t(rms),std::memory_order_release);
      receivedLevelCursor_=(receivedLevelCursor_+1)%receivedLevels_.size();
      rxTotals_.publish(rxLevel_);rxStatsEpoch_.store(receiveGeneration,std::memory_order_release); }
}
VoiceCall::ReceivedAudioLevel AudioEngine::receivedAudioLevel() const
{
    VoiceCall::ReceivedAudioLevel result;
    const auto now=nowMs();double squares=0;int count=0;
    const auto last=lastReceivedMs_.load();if(!talking_.load() || !last || rxStatsEpoch_.load(std::memory_order_acquire)!=incoming_.generation()) return result;
    result.frames=rxFrames_.load();result.ageMs=qMax<int64_t>(0,now-last);
    for(const auto &point:receivedLevels_) {
        const auto value=point.load(std::memory_order_acquire);if(!value) continue;
        const auto age=uint32_t(now)-uint32_t(value>>32);if(age>200) continue;
        const int peak=int((value>>16)&65535),rms=int(value&65535);
        result.peak=qMax(result.peak,peak);squares+=double(rms)*rms;++count;
    }
    if (count) result.rms=int(std::sqrt(squares/count));
    return result;
}

bool AudioEngine::takeCaptured(VoiceCall::Frame &frame) {
    const auto now=nowMs();VoiceCall::Frame candidate;
    // 关闭媒体后仍按原规则结算过期帧，但保留尚未过期的帧至结束清理。
    for(unsigned n=0;n<5 && outgoing_.popIf(candidate,[&](const VoiceCall::Frame &value) { return now-value.localMs>100 || talking_.load(); });++n) {
        if(now-candidate.localMs>100) { ++txDrops_;++txDropExpired_;continue; }
        if(!talking_.load()) return false;
        frame=candidate;++txFrames_;return true;
    }
    return false;
}

void AudioEngine::run()
{
    QString error;
    // 阻塞的状态切换只发生在本后台；不得持有音频队列锁。
    if (!VoiceCall::setMediaAudioExclusive(true)) {
        VoiceCall::setMediaAudioExclusive(false);
        emit failed(QStringLiteral("无法释放媒体音频设备")); return;
    }
    QString configWarnings;
    const auto processingConfig=VoiceCall::ProcessingConfig::load(Rk3566Platform::netConfigPath(),&configWarnings);
    if (!configWarnings.isEmpty()) emit statistics(QStringLiteral("[call-dsp] ")+configWarnings);
    AlsaAudioDevice playback;
    PlaybackRecorder playbackLog;int postXrunFrames=0;
    // recorder只归独立诊断线程；退出join后本线程才读取final。
    const auto recordPlayback=[&](const PlaybackCycle &cycle,bool periodic,bool failure) {
        const auto started=VoiceCall::monotonicUs();playbackLog.add(cycle);
        const auto &a=cycle.alsa;const bool xrun=a.xrunsAfter!=a.xrunsBefore;
        if((xrun || failure) && playbackLog.allow(true,started)) {
            emit statistics(QStringLiteral("[call-play] fault seq=%1 code=%2 source=%3 faultMonoUs=%4 faultState=%5 offset=%6 sinceIoUs=%7 lastIoBeforeMonoUs=%8 prevQueryMonoUs=%9 prevDelayFrames=%10")
                .arg(cycle.sequence).arg(a.faultCode).arg(a.faultSource).arg(a.faultUs).arg(a.faultState).arg(a.offsetAtFault).arg(a.faultSinceWriteUs).arg(a.lastWriteBeforeUs).arg(a.previousQueryUs).arg(a.previousDelayFrames)
                +QStringLiteral(" recoverRc=%1 recoverUs=%2 recoverLogUs=%3 xrunBefore=%4 xrunAfter=%5")
                .arg(a.recoverRc).arg(a.recoverUs).arg(a.recoveryLogUs).arg(a.xrunsBefore).arg(a.xrunsAfter));
            for(const auto &line:playbackLog.dump(xrun ? QStringLiteral("xrun") : QStringLiteral("failed"),9)) emit statistics(line);
            postXrunFrames=xrun && cycle.completed ? 4 : 0;
        } else if(!xrun && (cycle.entryGapUs>40000 || cycle.workUs>40000 || cycle.wakeLateUs>15000 || cycle.lowBuffer) && playbackLog.allow(false,started)) {
            const bool slow=cycle.entryGapUs>40000 || cycle.workUs>40000 || cycle.wakeLateUs>15000;
            for(const auto &line:playbackLog.dump(slow ? QStringLiteral("slow") : QStringLiteral("low-buffer"),8)) emit statistics(line);
        }
        if(!xrun && postXrunFrames>0 && --postXrunFrames==0)
            for(const auto &line:playbackLog.dump(QStringLiteral("post-xrun"),4)) emit statistics(line);
        if(periodic) emit statistics(playbackLog.summary(QStringLiteral("tick")));
        playbackLog.diagnosticCost(VoiceCall::monotonicUs()-started);
    };
    std::atomic_bool captureReady{false},captureFailed{false},needReference{false};
    uint64_t captureXruns=0,completedFrames=0;VoiceCall::LevelStats completedLevel;
    // 创建/预触固定存储在进入实时循环前完成；消费者迟到只能丢诊断，绝不等待UI。
    auto playbackTraces=std::make_unique<VoiceCall::SpscQueue<PlaybackCycle,256>>();
    auto captureTraces=std::make_unique<VoiceCall::SpscQueue<CaptureCycle,256>>();
    auto referenceEvents=std::make_unique<VoiceCall::SpscQueue<ReferenceEvent,64>>();
    const bool pcmEnabled=QFileInfo::exists(QStringLiteral("/tmp/face_media_voice_pcm_diag.enable"));
    std::unique_ptr<VoiceCall::SpscQueue<VoiceCall::PcmDiagnosticFrame,64>> pcmFrames;
    if(pcmEnabled) pcmFrames.reset(new VoiceCall::SpscQueue<VoiceCall::PcmDiagnosticFrame,64>);
    std::atomic_bool pcmActive{pcmEnabled};std::atomic<uint64_t> pcmDrops{0};
    std::atomic<int64_t> captureTid{0},playbackTid{ThreadDiagnostics::nativeTid()};
    const auto pcmDirectory=QStringLiteral("/tmp/voice_pcm_diag_%1_%2").arg(QCoreApplication::applicationPid()).arg(nowMs());
    std::atomic_bool diagnosticsStop{false};std::atomic<uint64_t> playTraceDrops{0},captureTraceDrops{0},referenceDrops{0},referenceGenerationPublished{1};
    CaptureRecorder captureLog;std::thread diagnostic;
    try { diagnostic=std::thread([&]() {
        auto nextReport=nowMs()+3000;VoiceCall::SchedulingDiagnostics scheduler;
        std::unique_ptr<VoiceCall::PcmDiagnosticRecorder> pcmRecorder;
        if(pcmFrames) { pcmRecorder.reset(new VoiceCall::PcmDiagnosticRecorder(pcmDirectory));emit statistics(QStringLiteral("[call-pcm] armed directory=%1 triggerPeak=12000 maxFrames=500").arg(pcmDirectory)); }
        int64_t nextManualCheck=0;bool manualTrigger=false;
        for(;;) {
            bool progress=false,slowObserved=false;PlaybackCycle cycle;CaptureCycle captured;
            for(int n=0;n<64 && playbackTraces->pop(cycle);++n) {
                slowObserved|=cycle.entryGapUs>30000 || cycle.workUs>15000 || cycle.wakeLateUs>10000;
                progress=true;playedFrames_=cycle.playedFrames;playLevel_=cycle.playedLevel;
                playbackDelayUs_=cycle.alsa.queryRc==0 && cycle.alsa.delayFrames>=0 ? cycle.alsa.delayFrames*1000000/16000 : -1;
                const bool periodic=nowMs()>=nextReport;
                recordPlayback(cycle,periodic,!cycle.completed && cycle.alsa.faultCode!=0);
                if(periodic) { reportStatistics(QStringLiteral("tick"));emit statistics(captureLog.summary(QStringLiteral("tick")));emit statistics(captureLog.referenceSummary(QStringLiteral("tick")));for(const auto &line:captureLog.shapeSummary(QStringLiteral("tick"))) emit statistics(line);nextReport=nowMs()+3000; }
            }
            for(int n=0;n<64 && captureTraces->pop(captured);++n) {
                slowObserved|=captured.gapUs>30000 || captured.dspUs>15000 || captured.txLockWaitUs>2000;
                progress=true;for(const auto &line:captureLog.add(captured)) emit statistics(line);
            }
            if(pcmRecorder) {
                if(nowMs()>=nextManualCheck && !pcmRecorder->done()) {
                    nextManualCheck=nowMs()+200;
                    const auto marker=QStringLiteral("/tmp/face_media_voice_pcm_diag.trigger");
                    if(QFileInfo::exists(marker)) { manualTrigger=true;QFile::remove(marker); }
                }
                VoiceCall::PcmDiagnosticFrame pcmFrame;
                for(int n=0;n<64 && pcmFrames->pop(pcmFrame);++n) {
                    progress=true;pcmRecorder->consume(pcmFrame,manualTrigger);manualTrigger=false;
                    if(pcmRecorder->done()) pcmActive=false;
                }
                const auto notice=pcmRecorder->takeNotice();if(!notice.isEmpty()) emit statistics(notice);
            }
            for(const auto &line:scheduler.poll(VoiceCall::monotonicUs(),{{uiNativeTid_.load(),playbackTid.load(),captureTid.load()}},slowObserved)) emit statistics(line);
            if(diagnosticsStop.load() && !progress) break;
            if(!progress) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(pcmRecorder) { pcmRecorder->finish();const auto notice=pcmRecorder->takeNotice();if(!notice.isEmpty()) emit statistics(notice); }
    }); } catch(const std::exception &failure) {
        stop_=true;emit failed(QStringLiteral("无法启动音频诊断线程: %1").arg(QString::fromLocal8Bit(failure.what())));
    }
    if (!stop_.load() && AlsaAudioDevice::configureRoutes(error) && playback.open(false,error)) {
        std::thread capture;
        try { capture=std::thread([&]() {
            captureTid=ThreadDiagnostics::nativeTid();
            VoiceCall::LevelStats captureTotals;int64_t captureLastMs=0,captureGapMs=0;
            AlsaAudioDevice device;
            QString captureError;
            if (!device.open(true,captureError)) {
                captureFailed=true; stop_=true; emit failed(captureError); return;
            }
            std::unique_ptr<VoiceCall::AudioProcessing> processing;
            try {
                QString information;processing.reset(new VoiceCall::AudioProcessing(processingConfig,information));
                emit statistics(information);
            } catch (const std::exception &failure) {
                emit statistics(QStringLiteral("[call-dsp] initialization exception; V1 direct: %1").arg(QString::fromLocal8Bit(failure.what())));
            }
            needReference=processing && processing->needsReference();device.enableTiming(needReference.load());
            if(processing) processing->enablePcmObservation(pcmEnabled);
            std::unique_ptr<VoiceCall::PcmDiagnosticFrame> observedPcm;if(pcmEnabled) observedPcm.reset(new VoiceCall::PcmDiagnosticFrame);
            VoiceCall::PcmShapeTracker directRawShape,directOutShape;
            captureReady=true;
            uint8_t seq=0;bool wasTalking=false;uint64_t previousXruns=device.xruns(),captureIndex=0,lastReferenceSequence=0;
            unsigned lastReferenceClear=0;ReferenceEvent lastReferenceEvent;
            int64_t lastCaptureEndUs=0,lastCaptureCpuUs=-1;int previousRaw=0,previousOut=0;bool haveBoundary=false;
            while (!stop_.load()) {
                VoiceCall::Frame frame;CaptureCycle captured;captured.beginUs=VoiceCall::monotonicUs();captured.index=++captureIndex;
                const auto readResourceStart=ThreadDiagnostics::sample();const auto readCpuStart=readResourceStart.cpuUs;
                const bool capturedOk=device.transfer(frame.pcm,stop_);
                const auto readResourceEnd=ThreadDiagnostics::sample();const auto readCpuEnd=readResourceEnd.cpuUs;
                captured.endUs=VoiceCall::monotonicUs();
                captured.readResources=ThreadDiagnostics::delta(readResourceStart,readResourceEnd,captured.endUs-captured.beginUs);
                captured.readCpuUs=VoiceCall::cpuElapsedUs(readCpuStart,readCpuEnd);captured.readOffCpuUs=VoiceCall::offCpuUs(captured.endUs-captured.beginUs,captured.readCpuUs);
                captured.gapCpuUs=lastCaptureEndUs ? VoiceCall::cpuElapsedUs(lastCaptureCpuUs,readCpuEnd) : -1;
                captured.gapUs=lastCaptureEndUs ? captured.endUs-lastCaptureEndUs : 0;lastCaptureEndUs=captured.endUs;lastCaptureCpuUs=readCpuEnd;
                captured.gapOffCpuUs=VoiceCall::offCpuUs(captured.gapUs,captured.gapCpuUs);
                captured.ioUs=device.transferTrace().ioUs;captured.waitUs=device.transferTrace().waitUs;captured.xruns=device.xruns();
                captured.faultCode=device.transferTrace().faultCode;captured.faultSource=device.transferTrace().faultSource;
                captured.faultUs=device.transferTrace().faultUs;captured.recoverUs=device.transferTrace().recoverUs;
                if (!capturedOk) {
                    if(!stop_.load()) { captured.failed=true;if(!captureTraces->push(captured)) ++captureTraceDrops; }
                    if (!stop_.exchange(true)) emit failed(QStringLiteral("麦克风读取失败或超时"));
                    break;
                }
                frame.localMs=nowMs()-20; frame.seq=seq++;frame.captureIndex=captureIndex;
                if (!talking_.load()) { wasTalking=false;previousXruns=device.xruns();continue; }
                if (!wasTalking || previousXruns!=device.xruns()) {
                    if (processing) processing->reset(wasTalking ? QStringLiteral("capture XRUN") : QStringLiteral("TALKING started"));
                    if (needReference.load()) { reference_.clear();lastReferenceClear=wasTalking ? 2U : 1U; }
                    wasTalking=true;
                }
                previousXruns=device.xruns();
                // 参考历史只有采集线程持有；先按播放序号排空交接，缺口必须清旧时间线。
                ReferenceEvent event;
                for(int n=0;n<64 && referenceEvents->pop(event);++n) {
                    const bool gap=lastReferenceSequence && event.sequence!=lastReferenceSequence+1;
                    if(event.reset || gap) { reference_.clear();lastReferenceClear=(event.xrun ? 4U : 0U)|(!event.valid ? 8U : 0U)|(gap ? 16U : 0U); }
                    const auto beforeGeneration=reference_.generation();lastReferenceEvent=event;
                    lastReferenceSequence=event.sequence;
                    if(event.valid) reference_.push(event.pcm,event.firstUs,int64_t(processingConfig.alignmentToleranceMs)*1000,size_t(processingConfig.referenceHistoryFrames));
                    if(reference_.generation()!=beforeGeneration) lastReferenceClear=32U;
                }
                const auto rawMic=frame.pcm;
                VoiceCall::Pcm ref{};bool aligned=false;uint64_t referenceGeneration=0;
                captured.clearFlags=lastReferenceClear;captured.playSequence=lastReferenceEvent.sequence;captured.playTiming=lastReferenceEvent.timing;
                captured.playQueryRc=lastReferenceEvent.queryRc;captured.playState=lastReferenceEvent.state;
                captured.referenceRequired=needReference.load();captured.captureTiming=device.timing();reference_.describe(captured.reference);
                captured.timingRc=device.transferTrace().queryRc;captured.timingState=device.transferTrace().state;
                captured.timingAvail=device.transferTrace().available;captured.timingDelay=device.transferTrace().delayFrames;
                if (needReference.load()) {
                    const auto timing=device.timing();
                    if (timing.valid) aligned=reference_.read(timing.firstSampleUs,int64_t(processingConfig.referenceDelayMs)*1000,ref,&captured.reference);
                    else captured.reference.reason=VoiceCall::ReferenceReason::CaptureTiming;
                    referenceGeneration=reference_.generation();
                }
                const auto beforeProcessing=processing ? processing->stats() : VoiceCall::ProcessingStats{};
                const auto dspResourceStart=ThreadDiagnostics::sample();const auto dspCpuStart=dspResourceStart.cpuUs;
                const auto processingStart=VoiceCall::monotonicUs();
                if (processing) {
                    processing->process(frame.pcm,aligned ? &ref : nullptr,referenceGeneration);
                    needReference=processing->needsReference();device.enableTiming(needReference.load());
                    const auto notice=processing->takeNotice();if (!notice.isEmpty()) emit statistics(notice);
                }
                captured.dspUs=VoiceCall::monotonicUs()-processingStart;
                const auto dspResourceEnd=ThreadDiagnostics::sample();captured.dspResources=ThreadDiagnostics::delta(dspResourceStart,dspResourceEnd,captured.dspUs);
                captured.dspCpuUs=VoiceCall::cpuElapsedUs(dspCpuStart,dspResourceEnd.cpuUs);captured.dspOffCpuUs=VoiceCall::offCpuUs(captured.dspUs,captured.dspCpuUs);
                captured.observation=processing ? processing->observation() : VoiceCall::ProcessingObservation{};
                if(!processing) { captured.observation.raw=directRawShape.observe(rawMic);captured.observation.output=directOutShape.observe(frame.pcm);captured.observation.valid=9; }
                if(pcmFrames && pcmActive.load()) {
                    const auto middle=processing ? processing->pcmObservation() : VoiceCall::ProcessingPcm{};
                    observedPcm->index=captureIndex;observedPcm->captureUs=device.timing().valid ? device.timing().firstSampleUs : 0;
                    observedPcm->valid=captured.observation.valid|(aligned ? 16U:0U);observedPcm->raw=rawMic;observedPcm->output=frame.pcm;
                    observedPcm->aec=middle.aec;observedPcm->preprocess=middle.preprocess;observedPcm->reference=ref;observedPcm->observation=captured.observation;
                    if(!pcmFrames->push(*observedPcm)) ++pcmDrops;
                } else if(processing && pcmEnabled) processing->enablePcmObservation(false);
                const auto afterProcessing=processing ? processing->stats() : VoiceCall::ProcessingStats{};
                frame.processingMode=uint8_t((afterProcessing.aecFrames>beforeProcessing.aecFrames ? 1 : 0) |
                    (afterProcessing.preprocessFrames>beforeProcessing.preprocessFrames ? 2 : 0) |
                    (!processing || afterProcessing.bypassFrames>beforeProcessing.bypassFrames ? 4 : 0));
                VoiceCall::LevelStats rawLevel,outLevel;rawLevel.add(rawMic);outLevel.add(frame.pcm);
                captured.mode=frame.processingMode;captured.aligned=aligned;captured.generation=reference_.generation();
                captured.rawRms=rawLevel.rms();captured.rawPeak=rawLevel.peak;captured.rawDc=rawLevel.dc();captured.rawClip=rawLevel.clipped;
                captured.outRms=outLevel.rms();captured.outPeak=outLevel.peak;captured.outDc=outLevel.dc();captured.outClip=outLevel.clipped;
                if(haveBoundary) { captured.rawStep=std::abs(int(rawMic.front())-previousRaw);captured.outStep=std::abs(int(frame.pcm.front())-previousOut); }
                previousRaw=rawMic.back();previousOut=frame.pcm.back();haveBoundary=true;
                referenceGenerationPublished.store(reference_.generation());
                const auto txResources=ThreadDiagnostics::sample();const auto txStart=VoiceCall::monotonicUs();
                if(talking_.load()) {
                    captureTotals.samples+=rawLevel.samples;captureTotals.clipped+=rawLevel.clipped;captureTotals.sum+=rawLevel.sum;
                    captureTotals.squares+=rawLevel.squares;captureTotals.peak=std::max(captureTotals.peak,rawLevel.peak);
                    if(captureLastMs) captureGapMs=std::max(captureGapMs,frame.localMs-captureLastMs);
                    captureLastMs=frame.localMs;
                    bool replaced=false;
                    if(!outgoing_.push(frame,true,&replaced) || replaced) { ++txDrops_;++txDropFull_; }
                    const auto depth=outgoing_.size();if(depth>txHighWater_.load()) txHighWater_=depth;
                    // 诊断若被调度延迟不能拖住采集；下帧发布累计值，最终退出再完整补齐。
                    std::unique_lock<std::mutex> lock(txMutex_,std::try_to_lock);
                    if(lock.owns_lock()) {
                        dspStats_=afterProcessing;micLevel_=captureTotals;lastCaptureMs_=captureLastMs;captureGapMaxMs_=captureGapMs;
                        referenceDepth_=reference_.size();referenceGeneration_=reference_.generation();referencePhaseUs_=reference_.phaseErrorUs();
                        captureDelayUs_=device.timing().queueDelayUs;
                    }
                }
                captured.txLockWaitUs=0;
                captured.txWorkUs=VoiceCall::monotonicUs()-txStart;captured.txResources=ThreadDiagnostics::delta(txResources,ThreadDiagnostics::sample(),captured.txWorkUs);
                if(!captureTraces->push(captured)) ++captureTraceDrops;
            }
            { std::lock_guard<std::mutex> lock(txMutex_);micLevel_=captureTotals;lastCaptureMs_=captureLastMs;captureGapMaxMs_=captureGapMs;
              if(processing) dspStats_=processing->stats();
              referenceDepth_=reference_.size();referenceGeneration_=reference_.generation();referencePhaseUs_=reference_.phaseErrorUs();captureDelayUs_=device.timing().queueDelayUs; }
            captureXruns=device.xruns();
        }); } catch (const std::exception &failure) {
            stop_=true; emit failed(QStringLiteral("无法启动采集线程: %1").arg(QString::fromLocal8Bit(failure.what())));
        }
        while (!stop_.load() && !captureReady.load()) msleep(2);
        if (!stop_.load() && !captureFailed.load()) emit ready();
        int64_t nativeTid=-1;
#ifdef Q_OS_LINUX
        nativeTid=int64_t(::syscall(SYS_gettid));
#endif
        emit statistics(QStringLiteral("[call-play] start thread=%1 tid=%2 pid=%3")
            .arg(quintptr(QThread::currentThreadId()),0,16).arg(nativeTid).arg(QCoreApplication::applicationPid()));
        VoiceCall::PlaybackPacer pacer(VoiceCall::monotonicUs());
        uint64_t previousPlaybackXruns=playback.xruns(),referenceSequence=0;
        int64_t previousLoopUs=0;PlaybackCpu previousCpu;uint64_t loopSequence=0;
        while (!stop_.load()) {
            PlaybackCycle trace;trace.playedFrames=completedFrames;trace.playedLevel=completedLevel;trace.beginUs=VoiceCall::monotonicUs();trace.sequence=++loopSequence;
            trace.entryGapUs=previousLoopUs ? trace.beginUs-previousLoopUs : 0;
            const auto cpu=playbackCpu();
            if(previousLoopUs && cpu.cpuUs>=0 && previousCpu.cpuUs>=0) {
                trace.cpuUs=cpu.cpuUs-previousCpu.cpuUs;trace.offCpuUs=std::max<int64_t>(0,trace.entryGapUs-trace.cpuUs);
                trace.vCtx=cpu.voluntary-previousCpu.voluntary;trace.ivCtx=cpu.involuntary-previousCpu.involuntary;
                trace.minorFaults=cpu.minorFaults-previousCpu.minorFaults;trace.majorFaults=cpu.majorFaults-previousCpu.majorFaults;
            }
            previousLoopUs=trace.beginUs;previousCpu=cpu;const auto workResourceStart=ThreadDiagnostics::sample();const auto workCpuStart=workResourceStart.cpuUs;
            playback.enableTiming(needReference.load());
            VoiceCall::Frame frame;
            const auto queueResourceStart=ThreadDiagnostics::sample();const auto queueCpuStart=queueResourceStart.cpuUs;
            const auto queueStart=VoiceCall::monotonicUs();int64_t acquired=0;
            {
              acquired=queueStart;trace.queueLockUs=0;
              trace.queuedBefore=incoming_.pendingFrames();
              if (talking_.load()) frame=incoming_.take(nowMs());
              trace.queuedAfter=incoming_.pendingFrames();trace.rx=rxStatsEpoch_.load()==incoming_.generation() ? rxFrames_.load() : 0;trace.underruns=incoming_.underruns.load();
              const auto receivedAt=lastReceivedMs_.load();trace.rxAgeMs=receivedAt ? nowMs()-receivedAt : -1; }
            trace.queueUs=VoiceCall::monotonicUs()-acquired;
            const auto queueResourceEnd=ThreadDiagnostics::sample();trace.queueResources=ThreadDiagnostics::delta(queueResourceStart,queueResourceEnd,trace.queueUs);
            trace.queueCpuUs=VoiceCall::cpuElapsedUs(queueCpuStart,queueResourceEnd.cpuUs);trace.queueOffCpuUs=VoiceCall::offCpuUs(trace.queueUs,trace.queueCpuUs);
            const auto alsaResourceStart=ThreadDiagnostics::sample();const auto alsaCpuStart=alsaResourceStart.cpuUs;
            const auto alsaStart=VoiceCall::monotonicUs();
            const bool transferred=playback.transfer(frame.pcm,stop_);
            trace.alsaUs=VoiceCall::monotonicUs()-alsaStart;trace.alsa=playback.transferTrace();trace.completed=transferred;
            const auto alsaResourceEnd=ThreadDiagnostics::sample();trace.alsaResources=ThreadDiagnostics::delta(alsaResourceStart,alsaResourceEnd,trace.alsaUs);
            trace.alsaCpuUs=VoiceCall::cpuElapsedUs(alsaCpuStart,alsaResourceEnd.cpuUs);trace.alsaOffCpuUs=VoiceCall::offCpuUs(trace.alsaUs,trace.alsaCpuUs);
            // 低于40ms和缓冲容量2/3两者较小值仅作观测；补写只依据绝对节拍/XRUN，不更改ALSA参数。
            const auto lowLimit=std::min<int64_t>(640,playback.geometry().bufferFrames*2/3);
            trace.lowBuffer=lowLimit>0 && trace.alsa.queryRc==0 && trace.alsa.queryRunning && trace.alsa.delayFrames>=0 && trace.alsa.delayFrames<lowLimit;
            if (!transferred) {
                const bool failure=!stop_.exchange(true);
                if(failure) emit failed(QStringLiteral("喇叭写入失败或超时"));
                trace.workUs=VoiceCall::monotonicUs()-trace.beginUs;
                if(!playbackTraces->push(trace)) ++playTraceDrops;
                break;
            }
            const auto referenceStart=VoiceCall::monotonicUs();
            VoiceCall::Pcm actualPlayback{};VoiceCall::AudioTiming playbackTiming;
            const bool validReference=needReference.load() && playback.playbackReference(actualPlayback,playbackTiming);
            ReferenceEvent event;event.sequence=++referenceSequence;event.valid=validReference;
            event.xrun=previousPlaybackXruns!=playback.xruns();event.reset=event.xrun || !validReference;
            event.timing=playback.timing();event.queryRc=playback.transferTrace().queryRc;event.state=playback.transferTrace().state;
            if(validReference) { event.pcm=actualPlayback;event.firstUs=playbackTiming.firstSampleUs; }
            if(needReference.load() && !referenceEvents->push(event)) ++referenceDrops;
            ++completedFrames;completedLevel.add(frame.pcm);trace.playedFrames=completedFrames;trace.playedLevel=completedLevel;
            trace.referenceGeneration=referenceGenerationPublished.load();
            trace.referenceUs=VoiceCall::monotonicUs()-referenceStart;trace.referenceValid=validReference;
            const auto sleepStart=VoiceCall::monotonicUs();trace.workUs=sleepStart-trace.beginUs;
            const auto workResourceEnd=ThreadDiagnostics::sample();trace.workResources=ThreadDiagnostics::delta(workResourceStart,workResourceEnd,trace.workUs);
            trace.workCpuUs=VoiceCall::cpuElapsedUs(workCpuStart,workResourceEnd.cpuUs);trace.workOffCpuUs=VoiceCall::offCpuUs(trace.workUs,trace.workCpuUs);
            const auto oldRebases=pacer.rebases;
            const auto nextUs=pacer.advance(sleepStart,previousPlaybackXruns!=playback.xruns());
            trace.catchups=pacer.catchups;trace.rebases=pacer.rebases;trace.scheduleReset=pacer.rebases!=oldRebases;
            previousPlaybackXruns=playback.xruns();
            trace.sleepRequestedUs=std::max<int64_t>(0,nextUs-sleepStart);
            const auto sleepResources=ThreadDiagnostics::sample();
            if(trace.sleepRequestedUs>0) std::this_thread::sleep_for(std::chrono::microseconds(trace.sleepRequestedUs));
            trace.sleepUs=VoiceCall::monotonicUs()-sleepStart;trace.wakeLateUs=std::max<int64_t>(0,trace.sleepUs-trace.sleepRequestedUs);
            trace.sleepResources=ThreadDiagnostics::delta(sleepResources,ThreadDiagnostics::sample(),trace.sleepUs);
            if(!playbackTraces->push(trace)) ++playTraceDrops;

        }
        stop_=true; if (capture.joinable()) capture.join();
    } else if (!stop_.load()) emit failed(error);
    diagnosticsStop=true;if(diagnostic.joinable()) diagnostic.join();
    playedFrames_=completedFrames;playLevel_=completedLevel;
    if(postXrunFrames>0) for(const auto &line:playbackLog.dump(QStringLiteral("post-xrun-ending"),size_t(4-postXrunFrames))) emit statistics(line);
    talking_=false;
    const auto playbackXruns=playback.xruns();
    playback.close();
    if (!VoiceCall::setMediaAudioExclusive(false)) emit statistics(QStringLiteral("媒体音频恢复失败"));
    reportStatistics(QStringLiteral("final"));
    emit statistics(QStringLiteral("xrun capture=%1 playback=%2").arg(captureXruns).arg(playbackXruns));
    emit statistics(playbackLog.summary(QStringLiteral("final")));
    emit statistics(captureLog.summary(QStringLiteral("final")));
    emit statistics(captureLog.referenceSummary(QStringLiteral("final")));
    for(const auto &line:captureLog.shapeSummary(QStringLiteral("final"))) emit statistics(line);
    emit statistics(QStringLiteral("[call-diag] playbackDropped=%1 captureDropped=%2 referenceDropped=%3 pcmEnabled=%4 pcmDropped=%5 platformPlayback=unknown")
        .arg(playTraceDrops.load()).arg(captureTraceDrops.load()).arg(referenceDrops.load()).arg(pcmEnabled).arg(pcmDrops.load()));
    // 下行槽保留至下一次begin逻辑换代，不能在迟到接收仍可能返回时物理重置槽。
    VoiceCall::Frame discarded;while(outgoing_.pop(discarded)) {}
    reference_.clear();

}

void AudioEngine::reportStatistics(const QString &phase)
{
    VoiceCall::LevelStats mic,rx,played;
    uint64_t txFrames,rxFrames,txDrops,txDropFull,txDropExpired,rxDrops,underruns,playFrames;
    size_t txPeak,rxPeak,referenceDepth;int64_t elapsed,gap,captureDelay,playbackDelay,referencePhase;
    VoiceCall::ProcessingStats dsp;uint64_t referenceGeneration;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic=micLevel_;txFrames=txFrames_;txDrops=txDrops_;txDropFull=txDropFull_;txDropExpired=txDropExpired_;
        txPeak=txHighWater_;gap=captureGapMaxMs_;dsp=dspStats_;captureDelay=captureDelayUs_;
        referenceDepth=referenceDepth_;referenceGeneration=referenceGeneration_;referencePhase=referencePhaseUs_;
    }
    {
        const bool current=rxStatsEpoch_.load(std::memory_order_acquire)==incoming_.generation();
        rx=current ? rxTotals_.snapshot() : VoiceCall::LevelStats{};rxFrames=current ? rxFrames_.load() : 0;rxDrops=incoming_.dropped.load();underruns=incoming_.underruns.load();rxPeak=incoming_.highWater.load();
    }
    // 播放累计值只由本诊断线程写；最终汇总在join后读取，不与播放线程共享锁。
    played=playLevel_;playFrames=playedFrames_;playbackDelay=playbackDelayUs_;
    const auto started=statsStartMs_.load();elapsed=started ? qMax<int64_t>(1,nowMs()-started) : 1;

    emit statistics(QStringLiteral("%1 tx=%2 rx=%3 txDrop=%4 rxDrop=%5 txPeak=%6 rxPeak=%7 underrun=%8 played=%9")
        .arg(phase).arg(txFrames).arg(rxFrames).arg(txDrops).arg(rxDrops).arg(txPeak).arg(rxPeak).arg(underruns).arg(playFrames)
        +QStringLiteral(" txFps=%1 rxFps=%2 micRms=%3 micPeak=%4 micDc=%5 micClip=%6 rxRms=%7 rxPcmPeak=%8 playRms=%9 captureGapMaxMs=%10")
        .arg(double(txFrames)*1000/elapsed,0,'f',1).arg(double(rxFrames)*1000/elapsed,0,'f',1)
        .arg(mic.rms()).arg(mic.peak).arg(mic.dc()).arg(mic.clipped).arg(rx.rms()).arg(rx.peak).arg(played.rms()).arg(gap)
        +QStringLiteral(" aecFrames=%1 ppFrames=%2 dspBypass=%3 refMiss=%4 dspReset=%5 dspFailure=%6 dspMaxUs=%7 refDepth=%8 refGeneration=%9")
        .arg(dsp.aecFrames).arg(dsp.preprocessFrames).arg(dsp.bypassFrames).arg(dsp.referenceMisses)
        .arg(dsp.resets).arg(dsp.failures).arg(dsp.maxProcessingUs).arg(qulonglong(referenceDepth)).arg(referenceGeneration)
        +QStringLiteral(" fallbackFrames=%1 transitionFrames=%2").arg(dsp.fallbackFrames).arg(dsp.transitionFrames)
        +QStringLiteral(" captureDelayUs=%1 playbackDelayUs=%2 refPhaseUs=%3").arg(captureDelay).arg(playbackDelay).arg(referencePhase)
        +QStringLiteral(" txDropFull=%1 txDropExpired=%2 rxContentionDrop=%3 rxPushRetry=%4 rxTakeRetry=%5 rxTakeExhausted=%6").arg(txDropFull).arg(txDropExpired).arg(incoming_.contentionDrops.load()).arg(incoming_.pushRetries.load()).arg(incoming_.takeRetries.load()).arg(incoming_.takeExhausted.load()));
}
