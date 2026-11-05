/**
 * @file AlsaAudioDevice.cpp
 * @brief 实现非阻塞 ALSA PCM、有限等待及 RK809 通话路由配置。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；采集打开前保存硬件增益，默认仅降低超过240的通道，原环境变量仍可覆盖；关闭和打开失败按当前值校验恢复，控制命令只在后台有界执行，DSP及PCM规则不变。
 */
#include "AlsaAudioDevice.h"
#include "AudioProcessing.h"
#include <alsa/asoundlib.h>
#include <QProcess>
#include <QElapsedTimer>
#include <QtEndian>
#include <cerrno>
#include <QDebug>

namespace {
/** @brief 当前线程CPU累计微秒，仅诊断已有时间查询，不读/proc或等待。
 * @return Linux CLOCK_THREAD_CPUTIME_ID结果；其他平台/失败为-1。
 */
int64_t queryThreadCpuUs() {
#ifdef Q_OS_LINUX
    timespec time{};if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&time)==0) return int64_t(time.tv_sec)*1000000+time.tv_nsec/1000;
#endif
    return -1;
}
/** @brief 克隆ALSA配置并只替换本地plughw的转换规则，不改系统或全局树。
 * @param[out] local 返回调用方拥有的完整配置；后续任何失败也由调用方delete清理。
 * @return 非负为成功；负值为配置读取、复制、查找、解析或合并失败。
 * @note 仅后台打开采集时执行。override替换整个ttable，清除原有0→1等映射；使用旧版ALSA也提供的buffer输入接口，始终关闭临时输入。
 */
int channelZeroConfig(snd_config_t **local)
{
    snd_config_t *global=nullptr;
    int rc=snd_config_update_ref(&global);
    if(rc<0) return rc;
    snd_config_t *copy=nullptr;
    rc=snd_config_copy(&copy,global);
    snd_config_unref(global);
    if(rc<0) return rc;
    *local=copy; // copy失败时不发布可能不完整的输出指针。
    snd_config_t *plug=nullptr;
    rc=snd_config_search(*local,"pcm.plughw",&plug);
    if(rc<0) return rc;
    const char rules[]="route_policy copy\nttable { 0 { 0 1.0 } }\n";
    snd_input_t *input=nullptr;
    rc=snd_input_buffer_open(&input,rules,sizeof(rules)-1);
    if(rc<0) return rc;
    rc=snd_config_load_override(plug,input);
    const int closeRc=snd_input_close(input);
    return rc<0 ? rc : closeRc;
}
/** @brief 后台执行单条amixer参数数组，无shell；读取/设置失败均有限等待并终止子进程。
 * @param[in] args 原amixer参数，不拼接shell命令。
 * @param[out] output 可空，成功时标准输出。
 * @param[out] error 启动/超时/退出错误，成功时不使用。
 * @return true正常退出0；false为失败，不重试。
 */
bool mixerCommand(const QStringList &args,QString *output,QString &error) {
    QProcess process;process.start(QStringLiteral("amixer"),args);
    if(!process.waitForStarted(1000) || !process.waitForFinished(1500) || process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
        process.kill();process.waitForFinished(500);
        error=QStringLiteral("amixer %1: %2").arg(args.join(' '),QString::fromLocal8Bit(process.readAllStandardError()));return false;
    }
    if(output) *output=QString::fromLocal8Bit(process.readAllStandardOutput());
    return true;
}
/** @brief 解析Capture Volume的values行，拒绝异常格式或超出0..255的硬件值。
 * @param[in] output amixer cget标准输出。
 * @param[out] values 成功时返回每通道整数，失败不发布部分值。
 * @return true至少一个合法通道；false不能可靠保存原增益。
 */
bool captureValues(const QString &output,QStringList &values) {
    for(const auto &line:output.split('\n')) {
        const auto text=line.trimmed();if(!text.startsWith(QLatin1String(": values="))) continue;
        const auto fields=text.mid(9).split(',');QStringList parsed;
        for(const auto &field:fields) { bool ok=false;const int n=field.trimmed().toInt(&ok);if(!ok || n<0 || n>255) return false;parsed<<QString::number(n); }
        if(parsed.isEmpty()) return false;
        values=parsed;return true;
    }
    return false;
}

}

AlsaAudioDevice::~AlsaAudioDevice() { close(); }

bool AlsaAudioDevice::configureRoutes(QString &error)
{
    QList<QStringList> controls={
        {"-c","0","cset","name=Playback Path","SPK_HP"},
        {"-c","0","cset","name=Capture MIC Path","Main Mic"}
    };
    const QByteArray configuredGain=qgetenv("QT_YCEST_CALL_CAPTURE_VOLUME");
    if (!configuredGain.isEmpty()) {
        bool valid=false;const int gain=configuredGain.toInt(&valid);
        if (!valid || gain<0 || gain>255) { error=QStringLiteral("QT_YCEST_CALL_CAPTURE_VOLUME must be 0..255");return false; }
        qInfo()<<"[call-alsa] capture gain override="<<gain;
    } else qInfo()<<"[call-alsa] capture gain ceiling=240; lower settings preserved";
    for(const auto &args:controls) if(!mixerCommand(args,nullptr,error)) return false;
    return true;
}

/** @brief 录音线程在打开PCM前保存实际增益；默认只降低高于240的通道，不在PCM上补数字衰减。 */
bool AlsaAudioDevice::prepareCaptureGain(QString &error)
{
    QString text;QStringList values;
    if(!mixerCommand({"-c","0","cget","name=Capture Volume"},&text,error)) return false;
    if(!captureValues(text,values)) { error=QStringLiteral("无法解析Capture Volume原值");return false; }
    const auto original=values.join(',');const auto configured=qgetenv("QT_YCEST_CALL_CAPTURE_VOLUME");
    bool valid=true;const int overrideGain=configured.isEmpty() ? -1 : configured.toInt(&valid);
    if(!valid || overrideGain>255 || (!configured.isEmpty() && overrideGain<0)) { error=QStringLiteral("QT_YCEST_CALL_CAPTURE_VOLUME must be 0..255");return false; }
    for(auto &value:values) value=QString::number(configured.isEmpty() ? qMin(value.toInt(),240) : overrideGain);
    const auto applied=values.join(',');if(applied==original) return true;
    originalCaptureGain_=original;appliedCaptureGain_=applied;
    return mixerCommand({"-c","0","cset","name=Capture Volume",applied},nullptr,error);
}
/** @brief 后台释放增益租约；不覆盖通话期间管理员另行设置的值。 */
void AlsaAudioDevice::restoreCaptureGain()
{
    if(originalCaptureGain_.isEmpty()) return;
    const auto original=originalCaptureGain_,applied=appliedCaptureGain_;originalCaptureGain_.clear();appliedCaptureGain_.clear();
    QString text,error;QStringList values;
    if(mixerCommand({"-c","0","cget","name=Capture Volume"},&text,error) && captureValues(text,values) && values.join(',')==applied)
        mixerCommand({"-c","0","cset","name=Capture Volume",original},nullptr,error);
}

bool AlsaAudioDevice::open(bool capture,QString &error)
{
    close(); capture_=capture;
    QByteArray name=qgetenv(capture ? "QT_YCEST_CALL_CAPTURE" : "QT_YCEST_CALL_PLAYBACK");
    if (name.isEmpty()) name="plughw:0,0";
    int rc=0;
    if(capture) {
        if(!prepareCaptureGain(error)) { close();return false; }
        rc=channelZeroConfig(&captureConfig_);
        if(rc<0) { error=QStringLiteral("Channel 0 copy配置失败: %1").arg(QString::fromLocal8Bit(snd_strerror(rc)));close();return false; }
        rc=snd_pcm_open_lconf(&pcm_,name.constData(),SND_PCM_STREAM_CAPTURE,SND_PCM_NONBLOCK,captureConfig_);
    } else rc=snd_pcm_open(&pcm_,name.constData(),SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK);
    if (rc>=0) rc=snd_pcm_set_params(pcm_,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,
                                    capture ? 1 : 2,16000,1,60000);
    if (rc<0) { error=QString::fromLocal8Bit(snd_strerror(rc)); close(); return false; }
    snd_pcm_uframes_t buffer=0,period=0;
    geometry_.paramsRc=snd_pcm_get_params(pcm_,&buffer,&period);
    if(geometry_.paramsRc>=0) { geometry_.bufferFrames=int64_t(buffer);geometry_.periodFrames=int64_t(period); }
    snd_pcm_sw_params_t *software=nullptr;
    geometry_.swRc=snd_pcm_sw_params_malloc(&software);
    if(geometry_.swRc>=0) {
        geometry_.swRc=snd_pcm_sw_params_current(pcm_,software);
        snd_pcm_uframes_t value=0;
        if(geometry_.swRc>=0) {
            if(snd_pcm_sw_params_get_start_threshold(software,&value)>=0) geometry_.startThreshold=int64_t(value);
            if(snd_pcm_sw_params_get_avail_min(software,&value)>=0) geometry_.availMin=int64_t(value);
        }
        snd_pcm_sw_params_free(software);
    }
    qInfo()<<"[call-alsa]"<<(capture ? "capture" : "playback")<<name<<"rate=16000 channels="<<(capture ? 1 : 2)
           <<(capture ? "route_policy=copy ttable=0->0 only" : "identical L/R");
    qInfo()<<"[call-alsa] geometry"<<(capture ? "capture" : "playback")<<"appRate=16000 requestedLatencyUs=60000 bufferFrames="<<geometry_.bufferFrames
           <<"periodFrames="<<geometry_.periodFrames<<"bufferUs="<<(geometry_.bufferFrames<0 ? -1 : geometry_.bufferFrames*1000000/16000)
           <<"startThreshold="<<geometry_.startThreshold<<"availMin="<<geometry_.availMin<<"paramsRc="<<geometry_.paramsRc<<"swRc="<<geometry_.swRc;
    return true;
}

bool AlsaAudioDevice::transfer(VoiceCall::Pcm &pcm,const std::atomic_bool &stop)
{
    const auto previous=transferTrace_;transferTrace_={};auto &trace=transferTrace_;
    trace.sequence=++transferSequence_;trace.beginUs=VoiceCall::monotonicUs();trace.xrunsBefore=xruns_;
    trace.entryGapUs=previous.beginUs ? trace.beginUs-previous.beginUs : 0;trace.lastWriteBeforeUs=lastIoSuccessUs_;
    trace.previousDelayFrames=previous.queryRc==0 ? previous.delayFrames : -1;
    trace.previousQueryUs=previous.queryEndUs;
    // 每个既有返回分支只补快照，不增加重试、等待或设备恢复动作。
    const auto finish=[&](bool ok) { trace.endUs=VoiceCall::monotonicUs();trace.xrunsAfter=xruns_;trace.completed=ok;return ok; };
    lastTiming_={};lastTiming_.reason=timingEnabled_ ? VoiceCall::TimingReason::Incomplete : VoiceCall::TimingReason::Disabled;
    if (!pcm_) return finish(false);
    std::array<uint16_t,VoiceCall::Samples*2> wire{};
    if (!capture_) for (int i=0;i<VoiceCall::Samples;++i) {
        wire[2*i]=wire[2*i+1]=qToLittleEndian(uint16_t(pcm[i]));
    }
    int offset=0;
    snd_pcm_sframes_t lastIo=0;int lastWait=0;
    QElapsedTimer deadline; deadline.start();
    while (!stop.load() && offset<VoiceCall::Samples && deadline.elapsed()<100) {
        if(capture_ && snd_pcm_state(pcm_)==SND_PCM_STATE_PREPARED) {
            const int rc=snd_pcm_start(pcm_);
            if(rc<0) { qWarning()<<"[call-alsa] capture start failed"<<rc<<snd_strerror(rc);return finish(false); }
        }
        const int channels=capture_ ? 1 : 2;
        const auto ioStart=VoiceCall::monotonicUs();
        snd_pcm_sframes_t n=capture_
            ? snd_pcm_readi(pcm_,wire.data()+offset*channels,VoiceCall::Samples-offset)
            : snd_pcm_writei(pcm_,wire.data()+offset*channels,VoiceCall::Samples-offset);
        lastIo=n;
        const auto ioEnd=VoiceCall::monotonicUs(),ioSpent=ioEnd-ioStart;
        ++trace.ioCalls;trace.ioUs+=ioSpent;trace.ioMaxUs=qMax(trace.ioMaxUs,ioSpent);
        int source=1;
        if (n>0) { if(n<VoiceCall::Samples-offset) ++trace.partial;offset+=int(n);trace.frames=offset;lastIoSuccessUs_=ioEnd;continue; }
        if (n==0 || n==-EAGAIN || n==-EINTR) {
            if(!n) ++trace.zero;else if(n==-EAGAIN) ++trace.again;else ++trace.interrupts;
            const auto waitStart=VoiceCall::monotonicUs();
            lastWait=snd_pcm_wait(pcm_,10);
            const auto spent=VoiceCall::monotonicUs()-waitStart;
            ++trace.waitCalls;trace.waitUs+=spent;trace.waitMaxUs=qMax(trace.waitMaxUs,spent);if(!lastWait) ++trace.waitTimeouts;
            if(lastWait>=0 || lastWait==-EINTR) continue;
            n=lastWait;source=2; // wait也可能报告XRUN/拔除，不能忽略后反复等待。
        }
        if(!trace.faultCode) {
            trace.faultCode=int(n);trace.faultSource=source;trace.faultUs=VoiceCall::monotonicUs();
            trace.faultState=int(snd_pcm_state(pcm_));trace.offsetAtFault=offset;
            trace.faultSinceWriteUs=lastIoSuccessUs_ ? trace.faultUs-lastIoSuccessUs_ : -1;
        }
        // snd_pcm_recover 对 ESTRPIPE 可能阻塞；非阻塞 resume/prepare 替代。
        if (n==-EPIPE || n==-ESTRPIPE) {
            ++xruns_;
            // 恢复期间不做QString分配/串口日志；调用方通过transferTrace在诊断线程输出。
            // recoveryLogUs保留兼容旧日志字段，当前实现为0。
            const auto recoverStart=VoiceCall::monotonicUs();trace.recoverRc=snd_pcm_prepare(pcm_);
            trace.recoverUs+=VoiceCall::monotonicUs()-recoverStart;
            if (trace.recoverRc<0) return finish(false);
            offset=0;trace.frames=0;
            continue;
        }
        qWarning()<<"[call-alsa]"<<(capture_ ? "capture" : "playback")<<"transfer failed"<<n<<snd_strerror(int(n));
        return finish(false);
    }
    if(stop.load()) return finish(false);
    if(offset!=VoiceCall::Samples) {
        qWarning()<<"[call-alsa]"<<(capture_ ? "capture" : "playback")<<"timeout frames="<<offset
                  <<"/"<<VoiceCall::Samples<<"elapsedMs="<<deadline.elapsed()<<"state="<<int(snd_pcm_state(pcm_))
                  <<"lastIo="<<lastIo<<"lastWait="<<lastWait;
        return finish(false);
    }
    if (capture_) for (int i=0;i<VoiceCall::Samples;++i) {
        const uint16_t value=qFromLittleEndian(wire[size_t(i)]);
        pcm[i]=value<32768 ? int16_t(value) : int16_t(int(value)-65536);
    }
    if (timingEnabled_) {
        // 每帧最多两次同步查询；重查使用新夹取时间和新延迟，绝不沿用首查估计。
        for(unsigned attempt=0;attempt<2;++attempt) {
            lastTiming_.valid=false;lastTiming_.firstSampleUs=0;lastTiming_.queueDelayUs=-1;
            lastTiming_.queryCpuUs=-1;lastTiming_.queryOffCpuUs=-1;
            const auto cpuBefore=queryThreadCpuUs();
            const auto before=VoiceCall::monotonicUs();
            snd_pcm_sframes_t available=0,delay=0;
            const int status=snd_pcm_avail_delay(pcm_,&available,&delay);
            const auto state=snd_pcm_state(pcm_);const bool running=state==SND_PCM_STATE_RUNNING;
            const auto after=VoiceCall::monotonicUs();
            const auto cpuAfter=queryThreadCpuUs();
            lastTiming_.xruns=xruns_;lastTiming_.querySpanUs=after-before;lastTiming_.queryAttempts=attempt+1;
            if(cpuBefore>=0 && cpuAfter>=cpuBefore) { lastTiming_.queryCpuUs=cpuAfter-cpuBefore;lastTiming_.queryOffCpuUs=std::max<int64_t>(0,after-before-lastTiming_.queryCpuUs); }
            trace.queryRc=status;trace.timingUs+=after-before;trace.queryEndUs=after;trace.available=status>=0 ? int64_t(available) : -1;
            trace.delayFrames=status>=0 ? int64_t(delay) : -1;trace.state=int(state);trace.queryRunning=running;
            if (status>=0 && delay>=0) lastTiming_.queueDelayUs=int64_t(delay)*1000000/16000;
            using Reason=VoiceCall::TimingReason;
            if(status<0) lastTiming_.reason=Reason::QueryError;
            else if(!running) lastTiming_.reason=Reason::NotRunning;
            else if(available<0) lastTiming_.reason=Reason::NegativeAvailable;
            else if(delay<0) lastTiming_.reason=Reason::NegativeDelay;
            else if(delay>16000) lastTiming_.reason=Reason::ExcessDelay;
            else if(after-before>2000) lastTiming_.reason=Reason::SlowQuery;
            else {
                const int64_t queuedUs=int64_t(delay)*1000000/16000;
                lastTiming_.firstSampleUs=before+(after-before)/2+(capture_ ? -queuedUs : queuedUs)-20000;
                lastTiming_.valid=lastTiming_.firstSampleUs>0;
                lastTiming_.reason=lastTiming_.valid ? Reason::Valid : Reason::InvalidSampleTime;
            }

            if(attempt==0) {
                lastTiming_.firstQuerySpanUs=lastTiming_.querySpanUs;
                lastTiming_.firstQueryCpuUs=lastTiming_.queryCpuUs;lastTiming_.firstQueryOffCpuUs=lastTiming_.queryOffCpuUs;
            }
            // 只对跨度超限重查；驱动错误/未RUNNING等失败不重试、不放宽2ms阈值。
            if(lastTiming_.reason!=VoiceCall::TimingReason::SlowQuery || attempt!=0 || stop.load()) break;
        }
        lastTiming_.queryTotalUs=trace.timingUs;
        trace.queryAttempts=lastTiming_.queryAttempts;trace.firstQueryUs=lastTiming_.firstQuerySpanUs;
        if (!capture_ && lastTiming_.valid) for (int i=0;i<VoiceCall::Samples;++i) {
            const auto value=qFromLittleEndian(wire[size_t(i)*2]);
            lastPlaybackPcm_[size_t(i)]=value<32768 ? int16_t(value) : int16_t(int(value)-65536);
        }
    }
    return finish(true);
}

int AlsaAudioDevice::delayMs() const
{
    snd_pcm_sframes_t frames=0;
    return pcm_ && snd_pcm_delay(pcm_,&frames)>=0 ? int(qMax<snd_pcm_sframes_t>(0,frames)/16) : 0;
}

void AlsaAudioDevice::close()
{
    lastTiming_={};
    if (pcm_) { snd_pcm_drop(pcm_); snd_pcm_close(pcm_); pcm_=nullptr; }
    restoreCaptureGain();
    if(captureConfig_) { snd_config_delete(captureConfig_);captureConfig_=nullptr; }
    geometry_={};transferTrace_={};transferSequence_=0;lastIoSuccessUs_=0;
}
