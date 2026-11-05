/**
 * @file SchedulingDiagnostics.h
 * @brief 后台有限读取线程schedstat/wchan与CPU配额统计，区分排队迟到和阻塞线索。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStringList>
#include <array>
#include "AudioProcessing.h"

namespace VoiceCall {
/** @brief 后台线程独占的/proc与cgroup观察器；不得从UI或录放实时循环调用。
 * @details 每250ms最多一轮文件读取；普通日志3秒一次，异常详情限频。
 * schedstat是累计区间证据，wchan是采样瞬间，均不能冒充具体慢函数内的精确等待轨迹。
 * 读取失败/内核未启用记录为未知；从不修改优先级、亲和性、CPU配额或内核开关。
 */
class SchedulingDiagnostics {
public:
    /** @brief 创建路径观察器，不读取文件。
     * @param[in] procRoot 生产/proc；测试可传隔离目录。
     * @param[in] cgroupRoot 生产/sys/fs/cgroup；测试可传隔离目录。
     */
    explicit SchedulingDiagnostics(QString procRoot=QStringLiteral("/proc"),QString cgroupRoot=QStringLiteral("/sys/fs/cgroup"))
        :procRoot_(std::move(procRoot)),cgroupRoot_(std::move(cgroupRoot)) {}
    /** @brief 有限读取三个相关线程及已解析cgroup，耗时只落在诊断线程。
     * @param[in] nowUs 单调微秒。
     * @param[in] tids UI、播放、采集的内核TID，0表示尚未就绪/不支持。
     * @param[in] slowObserved 最近音频批次有慢段，下一次采样附加详情；不突破250ms读取节拍。
     * @return 可低频打印的纯诊断文本，不含音频和业务正文。
     */
    QStringList poll(int64_t nowUs,const std::array<int64_t,3> &tids,bool slowObserved=false) {
        pendingSlow_|=slowObserved;QStringList lines;
        if(nowUs<nextSampleUs_) return lines;
        nextSampleUs_=nowUs+250000;
        const auto started=monotonicUs();
        if(!initialized_) {
            initialized_=true;resolveCgroups();
            const auto stats=read(procRoot_+QStringLiteral("/sys/kernel/sched_schedstats")).trimmed();
            schedstatsEnabled_=stats.isEmpty() ? -1 : stats.toInt();
            lines.append(QStringLiteral("[call-sched] config schedstatsEnabled=%1 rtRuntimeUs=%2 rtPeriodUs=%3 cgroupPaths=%4 samplePeriodMs=250")
                .arg(schedstatsEnabled_).arg(read(procRoot_+"/sys/kernel/sched_rt_runtime_us").trimmed()).arg(read(procRoot_+"/sys/kernel/sched_rt_period_us").trimmed()).arg(cgroups_.size()));
        }
        const bool periodic=nowUs>=nextReportUs_;bool anomaly=pendingSlow_;QStringList details;
        static const char *roles[]={"ui","playback","capture"};
        for(size_t i=0;i<tids.size();++i) {
            if(tids[i]<=0) continue;
            const auto path=procRoot_+"/self/task/"+QString::number(tids[i]);
            const auto sched=read(path+"/schedstat").simplified().split(' ');const auto stat=read(path+"/stat");
            const auto status=read(path+"/status");const auto wchan=read(path+"/wchan").trimmed();
            bool valid=sched.size()>=3;uint64_t run=0,wait=0,slices=0;
            if(valid) { bool a=false,b=false,c=false;run=sched[0].toULongLong(&a);wait=sched[1].toULongLong(&b);slices=sched[2].toULongLong(&c);valid=a&&b&&c; }
            const auto suffix=stat.mid(stat.lastIndexOf(')')+1).simplified().split(' ');
            const QString state=suffix.isEmpty() || suffix[0].isEmpty()?QStringLiteral("?"):suffix[0];
            const auto field=[&](int at) { return suffix.size()>at ? suffix[at] : QStringLiteral("?"); };
            auto &old=threads_[i];const bool comparable=valid && old.valid && old.tid==tids[i] && run>=old.run && wait>=old.wait && slices>=old.slices;
            const auto elapsed=old.tid==tids[i] && old.stamp ? nowUs-old.stamp : -1;
            const int64_t runUs=comparable ? int64_t((run-old.run)/1000) : -1,waitUs=comparable ? int64_t((wait-old.wait)/1000) : -1;
            const auto voluntary=count(status,"voluntary_ctxt_switches:"),involuntary=count(status,"nonvoluntary_ctxt_switches:");
            const auto vd=old.tid==tids[i] && voluntary>=old.voluntary && old.voluntary>=0 ? voluntary-old.voluntary : -1;
            const auto ivd=old.tid==tids[i] && involuntary>=old.involuntary && old.involuntary>=0 ? involuntary-old.involuntary : -1;
            anomaly|=waitUs>=5000 || state=="D";
            details.append(QStringLiteral("[call-sched] role=%1 tid=%2 beginMonoUs=%3 endMonoUs=%4 intervalUs=%5 schedstatAvailable=%6 schedstatsEnabled=%7 runUs=%8 runqueueWaitUs=%9 slices=%10")
                .arg(QString::fromLatin1(roles[i])).arg(tids[i]).arg(old.tid==tids[i]?old.stamp:0).arg(nowUs).arg(elapsed).arg(valid).arg(schedstatsEnabled_).arg(runUs).arg(waitUs).arg(comparable?int64_t(slices-old.slices):-1)
                +QStringLiteral(" state=%1 wchan=%2 vCtx=%3 ivCtx=%4 nice=%5 priority=%6 processor=%7 policy=%8 rtPriority=%9")
                .arg(state,wchan.isEmpty()?QStringLiteral("unknown"):wchan).arg(vd).arg(ivd).arg(field(16),field(15),field(36),field(38),field(37)));
            old={tids[i],nowUs,run,wait,slices,voluntary,involuntary,valid};
        }
        for(auto &group:cgroups_) {
            const auto text=read(group.path+"/cpu.stat");
            const auto throttled=count(text,"nr_throttled"),periods=count(text,"nr_periods");
            auto throttleUs=count(text,"throttled_usec");if(throttleUs<0) { const auto ns=count(text,"throttled_time");if(ns>=0) throttleUs=ns/1000; }
            const auto numberDelta=group.throttled>=0 && throttled>=group.throttled ? throttled-group.throttled : -1;
            const auto timeDelta=group.timeUs>=0 && throttleUs>=group.timeUs ? throttleUs-group.timeUs : -1;
            anomaly|=numberDelta>0;
            details.append(QStringLiteral("[call-sched] cgroup=%1 beginMonoUs=%2 endMonoUs=%3 quota=%4 nrPeriods=%5 nrThrottled=%6 throttleCountDelta=%7 throttledUsDelta=%8")
                .arg(group.path).arg(group.stamp).arg(nowUs).arg(group.quota).arg(periods).arg(throttled).arg(numberDelta).arg(timeDelta));
            group.throttled=throttled;group.timeUs=throttleUs;group.stamp=nowUs;
        }
        if(periodic || (anomaly && (reports_<8 || nowUs-lastDetailUs_>=1000000))) {
            lines+=details;nextReportUs_=nowUs+3000000;lastDetailUs_=nowUs;++reports_;
            lines.append(QStringLiteral("[call-sched] sampleMonoUs=%1 sampleCostUs=%2 audioSlowObserved=%3").arg(nowUs).arg(monotonicUs()-started).arg(pendingSlow_));
        }
        pendingSlow_=false;return lines;
    }
private:
    /** @brief 读取最多16KiB；不用atEnd判断/proc大小为0的伪文件，不在外部持有文件句柄。 */
    static QString read(const QString &path) { QFile file(path);return file.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(file.read(16384)) : QString(); }
    /** @brief 解析以键开头的累计整数，兼容status冒号和cpu.stat空白；未找到/非法为-1。 */
    static int64_t count(const QString &text,const QString &key) {
        for(const auto &line:text.split('\n')) { const auto fields=line.simplified().split(' ');if(fields.size()<2 || fields[0]!=key) continue;bool ok=false;const auto value=fields[1].toLongLong(&ok);return ok?value:-1; }return -1;
    }
    /** @brief 解析本进程v1/v2 CPU控制组及最多8层祖先，读取实际挂载中的已有目录。 */
    void resolveCgroups() {
        QString leaf,mount=cgroupRoot_;
        for(const auto &line:read(procRoot_+"/self/cgroup").split('\n')) {
            const auto first=line.indexOf(':'),second=line.indexOf(':',first+1);if(first<0 || second<0) continue;
            const auto controls=line.mid(first+1,second-first-1).split(',');const auto path=line.mid(second+1).trimmed();
            if(!path.startsWith('/') || path.split('/').contains("..")) continue;
            if(first==1 && line[0]=='0' && second==2) { leaf=cgroupRoot_+path;mount=cgroupRoot_;break; }
            if(controls.contains("cpu")) {
                for(const auto &name:{QStringLiteral("cpu,cpuacct"),QStringLiteral("cpu"),QStringLiteral("cpuacct,cpu")}) {
                    const auto candidate=cgroupRoot_+'/'+name;if(QFileInfo::exists(candidate+path+"/cpu.stat")) { mount=candidate;leaf=candidate+path;break; }
                }
            }
        }
        leaf=QDir::cleanPath(leaf);mount=QDir::cleanPath(mount);
        for(int depth=0;depth<8 && !leaf.isEmpty() && (leaf==mount || leaf.startsWith(mount+'/'));++depth) {
            if(QFileInfo::exists(leaf+"/cpu.stat")) {
                Group group;group.path=leaf;group.quota=read(leaf+"/cpu.max").simplified();
                if(group.quota.isEmpty()) group.quota=read(leaf+"/cpu.cfs_quota_us").trimmed()+"/"+read(leaf+"/cpu.cfs_period_us").trimmed();
                cgroups_.append(group);
            }
            if(leaf==mount) break;
            leaf=QFileInfo(leaf).path();
        }
    }
    /** @brief 每个角色最近一次采样；TID变化时不跨线程实例计算差值。 */
    struct Thread { int64_t tid=0,stamp=0;uint64_t run=0,wait=0,slices=0;int64_t voluntary=-1,involuntary=-1;bool valid=false; };
    /** @brief 控制组累计节流值；单位转换后统一微秒，-1为未知。 */
    struct Group { QString path,quota;int64_t throttled=-1,timeUs=-1,stamp=0; };
    QString procRoot_,cgroupRoot_;std::array<Thread,3> threads_{};QList<Group> cgroups_; /**< 仅后台诊断线程访问的路径和固定角色基线。 */
    int64_t nextSampleUs_=0,nextReportUs_=0,lastDetailUs_=0;unsigned reports_=0;int schedstatsEnabled_=-1;bool initialized_=false,pendingSlow_=false; /**< 读取/日志节拍及未知能力状态，不控制音频。 */
};
}
