/**
 * @file ui_performance.cpp
 * @brief 有界记录通话期间UI长事件、媒体操作及日志写入耗时并低频汇总。
 * @author Du lin
 * @note 最近修改：2026-11-04；修改人：Du lin；UI操作与事件新增配对CPU/切换/缺页、嵌套事件计账和自身耗时，分段定位进度查询/通知及业务/截图；保持原notify和业务执行顺序。
 * @date 2026-11-03
 */
#include "ui_performance.h"
#include <QDebug>
#include <QEvent>
#include <QTimer>
#include <QTimerEvent>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>

namespace {
constexpr int64_t SlowUs=10000; /**< 长事件/慢操作阈值10ms，仅用于统计。 */
constexpr size_t EventCapacity=16; /**< 一个窗口最多保留16组长事件，输出耗时最大的3组。 */
thread_local bool uiThread=false; /**< 仅诊断应用构造线程置true，不反查QObject跨线程状态。 */
thread_local bool reporting=false; /**< 诊断输出重入保护；不忽略其他线程的普通日志。 */
thread_local int eventDepth=0; /**< notify嵌套深度；耗时为包含子事件的墙钟时间，不可相加。 */
thread_local uint64_t eventSerial=0; /**< 已观察notify入口序号，用于识别长事件期间仍有嵌套分发的情况。 */
/** @brief 当前notify嵌套链，仅UI线程访问；子事件完整耗时只计入父事件一次。 */
struct ActiveEvent { ActiveEvent *parent=nullptr;int64_t childWall=0,childCpu=0;bool childCpuKnown=true; };
thread_local ActiveEvent *activeEvent=nullptr;
thread_local uint64_t completedNotifyCpuUnknown=0; /**< 子事件CPU读取失败累计，用于传播未知而非减去不完整CPU。 */
thread_local int64_t completedNotifyWall=0,completedNotifyCpu=0; /**< 累加各notify自身时长，祖孙不重复计账。 */
std::atomic_bool capturing{false}; /**< release/acquire发布诊断启停，不控制任何音频功能。 */
std::atomic<uint64_t> generation{0}; /**< 当前诊断代次，跨线程日志样本用于排除已结束代次。 */

/** @brief 原子日志累计，消息处理器不申请诊断锁也不输出诊断。 */
struct AtomicLogStats {
    std::atomic<uint64_t> count{0},formatUs{0},writeUs{0},formatMaxUs{0},writeMaxUs{0}; /**< 消息数、累计及最大耗时，单位微秒。 */
    std::atomic<uint64_t> slowFormat{0},slowWrite{0},bytes{0},errors{0},shortWrites{0}; /**< 慢格式化/写入次数、请求字节、失败和短写次数。 */
};
std::array<AtomicLogStats,2> logStats; /**< 0为UI，1为其他线程；仅字段原子，窗口快照不保证跨字段原子性。 */

/** @brief UI线程中的操作累计，记录最长样本的身份和时刻。 */
struct Metric {
    uint64_t count=0,slow=0,totalUs=0,maxUs=0; /**< 次数、达到10ms次数、累计及最长耗时微秒。 */
    int64_t maxStartedUs=0,maxEndedUs=0; /**< 最长样本单调微秒，用于与通话日志对应。 */
    ThreadDiagnostics::Delta resources; /**< 与最长墙钟相同作用域的CPU/切换/缺页差值。 */
    int64_t nestedWallUs=0,nestedCpuUs=0,result=-1;uint64_t nested=0;char detail[48]{}; /**< 最长样本内嵌套事件时长/次数及阶段/返回值，不输出正文。 */
    UiPerformance::ObjectIdentity owner; /**< 最长样本入口身份，不保留对象指针所有权。 */
};
/** @brief 一组长事件，以接收地址、类/名称、类型、定时器ID归组，身份保存在最长样本中。 */
struct EventMetric {
    Metric metric; /**< 仅包含达到10ms的事件。 */
    int type=0,timerId=-1,depth=0; /**< QEvent类型、定时器ID（非Timer为-1）、最长样本嵌套深度。 */
    uint64_t nestedEvents=0; /**< 最长样本内再次分发的notify次数；模态事件循环可能使父事件很长而UI仍有进展。 */
};
/** @brief 固定大小的一个窗口，只有UI线程修改，不持有设备或业务资源。 */
struct Window {
    uint64_t events=0,slowEvents=0,eventMaxUs=0,detailOmitted=0; /**< 总事件数、长事件数、最大微秒、未保留详情样本数。 */
    std::array<EventMetric,EventCapacity> longEvents{}; /**< 长事件详情有界，不形成日志队列。 */
    std::array<Metric,size_t(UiPerformance::Operation::Count)> operations{}; /**< 固定媒体操作累计。 */
};
/** @brief 进程生命周期内的UI诊断状态；定时器由应用所有，销毁时显式停止。 */
struct Recorder {
    QTimer *timer=nullptr; /**< 借用应用所有的3秒汇总定时器，应用析构前置空。 */
    QString callId; /**< 诊断用通话标签，不修改信令。 */
    Window window; /**< 当前3秒窗口累计。 */
    int64_t windowStartedUs=0,previousReportUs=0; /**< 当前窗口起点、上一轮汇总墙钟耗时，微秒。 */
    bool finishPending=false; /**< 已请求结束，等待当前UI事件返回后汇总。 */
};

/** @brief 延长状态至进程结束，避免Qt销毁期间的消息处理器访问已析构对象。
 * @return 仅UI线程访问的固定诊断状态；日志线程只访问独立原子字段。
 */
Recorder &recorder() { static auto *value=new Recorder;return *value; }

/** @brief 复制入口身份；调用者保证此时对象仍有效。
 * @param[in] object 当前对象，可为空。
 * @return 不拥有QObject、可在对象销毁后格式化的身份副本。
 */
UiPerformance::ObjectIdentity identityOf(const QObject *object)
{
    UiPerformance::ObjectIdentity result;
    if (!object) return result;
    result.address=reinterpret_cast<quintptr>(object);
    std::strncpy(result.className,object->metaObject()->className(),sizeof(result.className)-1);
    result.objectName=object->objectName().left(64);
    if (const auto *parent=object->parent()) {
        std::strncpy(result.parentClass,parent->metaObject()->className(),sizeof(result.parentClass)-1);
        result.parentName=parent->objectName().left(64);
    }
    return result;
}
/** @brief 将名称转为单行、带引号的诊断字段，不读取控件内容。
 * @param[in] value objectName或上下文标签。
 * @return 转义后的单行字符串。
 */
QString quoted(QString value)
{
    value.replace('\\',QStringLiteral("\\\\"));value.replace('"',QStringLiteral("\\\""));
    value.replace('\r',QStringLiteral("\\r"));value.replace('\n',QStringLiteral("\\n"));
    return '"'+value+'"';
}
/** @brief 累加一项墙钟耗时并保留最大样本身份，不执行日志或业务调用。
 * @param[in,out] metric 当前窗口累计。
 * @param[in] startedUs 入口单调微秒。
 * @param[in] endedUs 退出单调微秒。
 * @param[in] owner 入口身份副本。
 */
bool addMetric(Metric &metric,int64_t startedUs,int64_t endedUs,const UiPerformance::ObjectIdentity &owner)
{
    const auto duration=uint64_t(std::max<int64_t>(0,endedUs-startedUs));
    ++metric.count;metric.totalUs+=duration;if (duration>=uint64_t(SlowUs)) ++metric.slow;
    if (metric.count==1 || duration>metric.maxUs) {
        metric.maxUs=duration;metric.maxStartedUs=startedUs;metric.maxEndedUs=endedUs;metric.owner=owner;return true;
    }
    return false;
}
/** @brief 无诊断互斥的最大值更新。
 * @param[in,out] target 原子峰值。
 * @param[in] value 新样本。
 */
void atomicMax(std::atomic<uint64_t> &target,uint64_t value)
{
    auto old=target.load(std::memory_order_relaxed);
    while (value>old && !target.compare_exchange_weak(old,value,std::memory_order_relaxed)) {}
}
/** @brief 给常见事件附加可读名，其余保留数值便于查Qt枚举。
 * @param[in] type QEvent类型值。
 * @return 静态名字，无资源所有权转移。
 */
const char *eventName(int type)
{
    switch(type) {
    case QEvent::Timer:return "Timer";case QEvent::MetaCall:return "MetaCall";
    case QEvent::Paint:return "Paint";case QEvent::UpdateRequest:return "UpdateRequest";
    case QEvent::Expose:return "Expose";case QEvent::Resize:return "Resize";
    case QEvent::SockAct:return "SockAct";case QEvent::DeferredDelete:return "DeferredDelete";
    case QEvent::MouseButtonPress:return "MouseButtonPress";case QEvent::MouseButtonRelease:return "MouseButtonRelease";
    case QEvent::User:return "User";default:return "Other";
    }
}
/** @brief 固定操作的日志字段名。
 * @param[in] operation 有效固定操作类别。
 * @return 静态名字；无效值为unknown。
 */
const char *operationName(UiPerformance::Operation operation)
{
    switch(operation) {
    case UiPerformance::Operation::VideoPaint:return "videoPaint";
    case UiPerformance::Operation::VideoUpload:return "videoUpload";
    case UiPerformance::Operation::PlaybackPosition:return "playbackPosition";
    case UiPerformance::Operation::MediaSet:return "mediaSet";
    case UiPerformance::Operation::MediaStop:return "mediaStop";
    case UiPerformance::Operation::PipelineNull:return "pipelineNull";
    case UiPerformance::Operation::PipelineRelease:return "pipelineRelease";
    case UiPerformance::Operation::VideoSnapshot:return "videoSnapshot";
    case UiPerformance::Operation::PositionQuery:return "positionQuery";
    case UiPerformance::Operation::DurationQuery:return "durationQuery";
    case UiPerformance::Operation::PositionNotify:return "positionNotify";
    case UiPerformance::Operation::MqttBusiness:return "mqttBusiness";
    case UiPerformance::Operation::GetPlayInfo:return "getPlayInfo";
    case UiPerformance::Operation::PlayInfoBuild:return "playInfoBuild";
    case UiPerformance::Operation::PlayInfoSubmit:return "playInfoSubmit";
    case UiPerformance::Operation::FileList:return "fileList";
    case UiPerformance::Operation::SnapshotRequest:return "snapshotRequest";
    case UiPerformance::Operation::SnapshotReady:return "snapshotReady";
    case UiPerformance::Operation::UiGrab:return "uiGrab";
    case UiPerformance::Operation::PixmapToImage:return "pixmapToImage";
    case UiPerformance::Operation::WebMediaRead:return "webMediaRead";
    case UiPerformance::Operation::WebMediaPublish:return "webMediaPublish";
    case UiPerformance::Operation::SettingsRead:return "settingsRead";
    case UiPerformance::Operation::NetworkProbe:return "networkProbe";
    case UiPerformance::Operation::ProcessStart:return "processStart";
    default:return "unknown";
    }
}
/** @brief 格式化同一最长样本的资源与嵌套证据，不额外查询设备。 */
QString resourceFields(const Metric &metric) {
    const auto &r=metric.resources;
    const auto selfUs=std::max<int64_t>(0,int64_t(metric.maxUs)-metric.nestedWallUs);
    const auto selfCpu=r.cpuUs<0 || metric.nestedCpuUs<0 ? -1 : std::max<int64_t>(0,r.cpuUs-metric.nestedCpuUs);
    return QStringLiteral(" cpuUs=%1 offCpuUs=%2 vCtx=%3 ivCtx=%4 minFault=%5 majFault=%6 cpuBegin=%7 cpuEnd=%8")
        .arg(r.cpuUs).arg(r.offCpuUs).arg(r.voluntary).arg(r.involuntary).arg(r.minorFaults).arg(r.majorFaults).arg(r.cpuBegin).arg(r.cpuEnd)
        +QStringLiteral(" nestedNotifyUs=%1 selfUs=%2 selfCpuUs=%3 selfOffCpuUs=%4 nestedNotifyCount=%5 detail=%6 result=%7")
        .arg(metric.nestedWallUs).arg(selfUs).arg(selfCpu).arg(selfCpu<0 ? -1 : std::max<int64_t>(0,selfUs-selfCpu))
        .arg(metric.nested).arg(quoted(QString::fromLatin1(metric.detail))).arg(metric.result);
}
/** @brief 格式化已复制身份，历史地址从不解引用。
 * @param[in] identity 入口身份快照。
 * @return 类、名称、地址及父对象身份字段。
 */
QString identityFields(const UiPerformance::ObjectIdentity &identity)
{
    return QStringLiteral(" receiver=%1 name=%2 object=0x%3 parent=%4 parentName=%5")
        .arg(QString::fromLatin1(identity.className),quoted(identity.objectName))
        .arg(qulonglong(identity.address),0,16).arg(QString::fromLatin1(identity.parentClass),quoted(identity.parentName));
}
/** @brief 单个notify的观察作用域，保证提前返回、对象销毁和嵌套分发均不改变分发语义。 */
class EventScope final {
public:
    /** @brief 入口复制身份及事件元数据。
     * @param[in] receiver 当前有效接收对象。
     * @param[in] event 当前事件；仅入口读取类型和定时器ID。
     */
    EventScope(QObject *receiver,QEvent *event):generation_(generation.load(std::memory_order_relaxed)),
        identity_(identityOf(receiver)),type_(int(event->type())),
        timerId_(event->type()==QEvent::Timer ? static_cast<QTimerEvent *>(event)->timerId() : -1),depth_(++eventDepth),
        serial_(++eventSerial),startedUs_(UiPerformance::monotonicUs()),resources_(ThreadDiagnostics::sample()) {
        event_.parent=activeEvent;activeEvent=&event_;
    }
    /** @brief 累计事件，最长16组详情只保留较慢分组；不在此处打印日志。 */
    ~EventScope() {
        const auto endedUs=UiPerformance::monotonicUs();const auto usage=ThreadDiagnostics::delta(resources_,ThreadDiagnostics::sample(),endedUs-startedUs_);--eventDepth;
        activeEvent=event_.parent;
        completedNotifyWall+=std::max<int64_t>(0,endedUs-startedUs_-event_.childWall);
        if(usage.cpuUs>=0 && event_.childCpuKnown) completedNotifyCpu+=std::max<int64_t>(0,usage.cpuUs-event_.childCpu);
        else ++completedNotifyCpuUnknown;
        if(event_.parent) { event_.parent->childWall+=std::max<int64_t>(0,endedUs-startedUs_);
            if(usage.cpuUs>=0) event_.parent->childCpu+=usage.cpuUs;else event_.parent->childCpuKnown=false; }
        if (!capturing.load(std::memory_order_relaxed) || generation_!=generation.load(std::memory_order_relaxed)) return;
        auto &window=recorder().window;const auto duration=uint64_t(std::max<int64_t>(0,endedUs-startedUs_));
        ++window.events;window.eventMaxUs=std::max(window.eventMaxUs,duration);
        if (duration<uint64_t(SlowUs)) return;
        ++window.slowEvents;
        EventMetric *slot=nullptr;
        for(auto &entry:window.longEvents) if(entry.metric.count && entry.metric.owner.address==identity_.address &&
            entry.metric.owner.objectName==identity_.objectName && std::strcmp(entry.metric.owner.className,identity_.className)==0 &&
            entry.type==type_ && entry.timerId==timerId_) { slot=&entry;break; }
        if(!slot) for(auto &entry:window.longEvents) if(!entry.metric.count) { slot=&entry;break; }
        if(!slot) {
            slot=&*std::min_element(window.longEvents.begin(),window.longEvents.end(),[](const EventMetric &a,const EventMetric &b) { return a.metric.maxUs<b.metric.maxUs; });
            if(duration<=slot->metric.maxUs) { ++window.detailOmitted;return; }
            window.detailOmitted+=slot->metric.count;*slot={};
        }
        const bool newMaximum=!slot->metric.count || duration>slot->metric.maxUs;
        slot->type=type_;slot->timerId=timerId_;
        addMetric(slot->metric,startedUs_,endedUs,identity_);
        if(newMaximum) { slot->depth=depth_;slot->nestedEvents=eventSerial-serial_;
            slot->metric.resources=usage;slot->metric.nestedWallUs=event_.childWall;slot->metric.nestedCpuUs=event_.childCpuKnown ? event_.childCpu : -1;slot->metric.nested=eventSerial-serial_; }
    }
private:
    uint64_t generation_; /**< 入口诊断代次，切换后不混入下一代。 */
    UiPerformance::ObjectIdentity identity_; /**< 接收对象销毁后仍安全的入口身份。 */
    int type_,timerId_,depth_; /**< 入口事件类型、定时器及嵌套深度。 */
    uint64_t serial_; /**< 本次入口序号，退出差值仅用于观察嵌套分发，不参与业务。 */
    int64_t startedUs_; /**< 实际调用原notify前的单调微秒。 */
    ThreadDiagnostics::Sample resources_;ActiveEvent event_; /**< 本次资源和栈内嵌套计账，生命周期覆盖原notify。 */
};
}

int64_t UiPerformance::monotonicUs()
{
#ifdef Q_OS_LINUX
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC,&time)==0) return int64_t(time.tv_sec)*1000000+time.tv_nsec/1000;
#endif
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void UiPerformance::setCallContext(const QString &callId)
{
    if (!uiThread || !recorder().timer) return;
    auto &state=recorder();
    if (callId.isEmpty()) {
        if (!capturing.load() || state.finishPending) return;
        state.finishPending=true;const auto current=generation.load();
        QTimer::singleShot(0,state.timer,[current]() {
            auto &currentState=recorder();
            if (!currentState.finishPending || generation.load()!=current) return;
            reportNow(QStringLiteral("final"));capturing.store(false,std::memory_order_release);
            currentState.timer->stop();currentState.finishPending=false;
        });
        return;
    }
    if (capturing.load() && !state.finishPending) { state.callId=callId;return; }
    if (capturing.load()) reportNow(QStringLiteral("final"));
    state.finishPending=false;state.window={};state.callId=callId;state.windowStartedUs=monotonicUs();state.previousReportUs=0;
    for (auto &log:logStats) {
        log.count.exchange(0);log.formatUs.exchange(0);log.writeUs.exchange(0);log.formatMaxUs.exchange(0);log.writeMaxUs.exchange(0);
        log.slowFormat.exchange(0);log.slowWrite.exchange(0);log.bytes.exchange(0);log.errors.exchange(0);log.shortWrites.exchange(0);
    }
    generation.fetch_add(1);capturing.store(true,std::memory_order_release);state.timer->start();
}

UiPerformance::OperationScope::OperationScope(Operation operation,const QObject *owner,const char *detail):operation_(operation)
{
    if (!uiThread || reporting || size_t(operation)>=size_t(Operation::Count) || !capturing.load(std::memory_order_acquire)) return;
    generation_=generation.load();identity_=identityOf(owner);startedUs_=monotonicUs();resources_=ThreadDiagnostics::sample();
    eventSerial_=eventSerial;notifyWall_=completedNotifyWall;notifyCpu_=completedNotifyCpu;notifyCpuUnknown_=completedNotifyCpuUnknown;
    if(detail) std::strncpy(detail_,detail,sizeof(detail_)-1);
}
UiPerformance::OperationScope::~OperationScope()
{
    if (!generation_ || !capturing.load() || generation_!=generation.load()) return;
    const auto ended=monotonicUs();const auto usage=ThreadDiagnostics::delta(resources_,ThreadDiagnostics::sample(),ended-startedUs_);
    auto &metric=recorder().window.operations[size_t(operation_)];
    if(addMetric(metric,startedUs_,ended,identity_)) {
        metric.resources=usage;metric.nested=eventSerial-eventSerial_;
        metric.nestedWallUs=completedNotifyWall-notifyWall_;metric.nestedCpuUs=completedNotifyCpuUnknown==notifyCpuUnknown_ ? completedNotifyCpu-notifyCpu_ : -1;
        metric.result=result_;std::memcpy(metric.detail,detail_,sizeof(metric.detail));
    }
}

UiPerformance::LogMeasurement::LogMeasurement()
{
    if (reporting || !capturing.load(std::memory_order_acquire)) return;
    generation_=generation.load();uiThread_=uiThread;startedUs_=monotonicUs();
}
void UiPerformance::LogMeasurement::beforeWrite() { if(generation_) writeStartedUs_=monotonicUs(); }
void UiPerformance::LogMeasurement::finish(qint64 requestedBytes,qint64 writtenBytes)
{
    if (!generation_) return;
    const auto current=generation_;generation_=0;const auto ended=monotonicUs();
    if (!capturing.load() || current!=generation.load() || !writeStartedUs_) return;
    auto &stats=logStats[uiThread_ ? 0 : 1];
    const auto format=uint64_t(std::max<int64_t>(0,writeStartedUs_-startedUs_));
    const auto write=uint64_t(std::max<int64_t>(0,ended-writeStartedUs_));
    stats.count.fetch_add(1,std::memory_order_relaxed);stats.formatUs.fetch_add(format,std::memory_order_relaxed);
    stats.writeUs.fetch_add(write,std::memory_order_relaxed);atomicMax(stats.formatMaxUs,format);atomicMax(stats.writeMaxUs,write);
    if(format>=uint64_t(SlowUs)) stats.slowFormat.fetch_add(1,std::memory_order_relaxed);
    if(write>=uint64_t(SlowUs)) stats.slowWrite.fetch_add(1,std::memory_order_relaxed);
    stats.bytes.fetch_add(uint64_t(std::max<qint64>(0,requestedBytes)),std::memory_order_relaxed);
    if(writtenBytes<0) stats.errors.fetch_add(1,std::memory_order_relaxed);
    else if(writtenBytes<requestedBytes) stats.shortWrites.fetch_add(1,std::memory_order_relaxed);
}

void UiPerformance::reportNow(const QString &phase)
{
    if (!uiThread || reporting || !capturing.load()) return;
    reporting=true;
    auto &state=recorder();const auto started=monotonicUs();
    Window window=std::move(state.window);state.window={};
    const auto prefix=QStringLiteral("[ui-perf] callId=%1 phase=%2").arg(quoted(state.callId),quoted(phase));
    qInfo().noquote()<<prefix+QStringLiteral(" windowStartMonoMs=%1 windowEndMonoMs=%2 slowThresholdUs=%3 events=%4 slowEvents=%5 eventMaxUs=%6 detailOmitted=%7 previousReportUs=%8")
        .arg(state.windowStartedUs/1000).arg(started/1000).arg(SlowUs).arg(window.events).arg(window.slowEvents)
        .arg(window.eventMaxUs).arg(window.detailOmitted).arg(state.previousReportUs);
    std::sort(window.longEvents.begin(),window.longEvents.end(),[](const EventMetric &a,const EventMetric &b) { return a.metric.maxUs>b.metric.maxUs; });
    for(size_t i=0;i<3 && window.longEvents[i].metric.count;++i) {
        const auto &entry=window.longEvents[i];const auto &metric=entry.metric;
        qInfo().noquote()<<prefix+QStringLiteral(" eventType=%1(%2) timerId=%3 depth=%4 slowCount=%5 totalSlowUs=%6 maxUs=%7 maxStartMonoMs=%8 maxEndMonoMs=%9 nestedEvents=%10")
            .arg(QString::fromLatin1(eventName(entry.type))).arg(entry.type).arg(entry.timerId).arg(entry.depth)
            .arg(metric.count).arg(metric.totalUs).arg(metric.maxUs).arg(metric.maxStartedUs/1000).arg(metric.maxEndedUs/1000)
            .arg(entry.nestedEvents)+identityFields(metric.owner)+resourceFields(metric);
    }
    for(size_t i=0;i<window.operations.size();++i) {
        const auto &metric=window.operations[i];if (!metric.count) continue;
        qInfo().noquote()<<prefix+QStringLiteral(" op=%1 count=%2 slowCount=%3 totalUs=%4 maxUs=%5 maxStartMonoMs=%6 maxEndMonoMs=%7")
            .arg(QString::fromLatin1(operationName(Operation(i)))).arg(metric.count).arg(metric.slow).arg(metric.totalUs)
            .arg(metric.maxUs).arg(metric.maxStartedUs/1000).arg(metric.maxEndedUs/1000)+identityFields(metric.owner)+resourceFields(metric);
    }
    for(size_t i=0;i<logStats.size();++i) {
        auto &stats=logStats[i];const auto count=stats.count.exchange(0);
        const auto format=stats.formatUs.exchange(0),write=stats.writeUs.exchange(0);
        const auto formatMax=stats.formatMaxUs.exchange(0),writeMax=stats.writeMaxUs.exchange(0);
        const auto slowFormat=stats.slowFormat.exchange(0),slowWrite=stats.slowWrite.exchange(0);
        const auto bytes=stats.bytes.exchange(0),errors=stats.errors.exchange(0),shortWrites=stats.shortWrites.exchange(0);
        qInfo().noquote()<<prefix+QStringLiteral(" op=qtLog thread=%1 count=%2 formatTotalUs=%3 formatMaxUs=%4 slowFormat=%5 writeTotalUs=%6 writeMaxUs=%7 slowWrite=%8 requestedBytes=%9 writeErrors=%10 shortWrites=%11")
            .arg(i==0 ? QStringLiteral("ui") : QStringLiteral("worker")).arg(count).arg(format).arg(formatMax).arg(slowFormat)
            .arg(write).arg(writeMax).arg(slowWrite).arg(bytes).arg(errors).arg(shortWrites);
    }
    state.previousReportUs=monotonicUs()-started;state.windowStartedUs=monotonicUs();reporting=false;
}

UiDiagnosticApplication::UiDiagnosticApplication(int &argc,char **argv):QApplication(argc,argv)
{
    uiThread=true;auto &state=recorder();state.timer=new QTimer(this);
    state.timer->setObjectName(QStringLiteral("ui.perf.report"));state.timer->setInterval(3000);
    connect(state.timer,&QTimer::timeout,this,[]() { UiPerformance::reportNow(QStringLiteral("tick")); });
}
UiDiagnosticApplication::~UiDiagnosticApplication()
{
    capturing.store(false,std::memory_order_release);recorder().timer->stop();recorder().timer=nullptr;uiThread=false;
}
bool UiDiagnosticApplication::notify(QObject *receiver,QEvent *event)
{
    if (!uiThread || reporting || !capturing.load(std::memory_order_acquire) || !receiver || !event || receiver==recorder().timer)
        return QApplication::notify(receiver,event);
    EventScope scope(receiver,event);return QApplication::notify(receiver,event);
}
