/**
 * @file AudioProcessing.cpp
 * @brief 读取本通话DSP参数并按AEC→预处理串行运行，失步/缺库安全直通。
 * @author Du lin
 * @date 2026-10-31
 * @note 最近修改：2026-11-04；修改人：Du lin；保留原DSP过程与参数，旁路/主路/后备均记录阶段幅度、削顶、峰顶平坦和AGC只读状态；可选复制中间PCM供有限短录音，不修改样本。
 */
#include "AudioProcessing.h"
#include "SpeexDsp.h"
#include "CaptureContinuity.h"
#include <QSettings>
#include <QStringList>
#include <exception>

VoiceCall::ProcessingConfig VoiceCall::ProcessingConfig::load(const QString &path,QString *warnings)
{
    ProcessingConfig config;QStringList notes;if (warnings) warnings->clear();
    if (path.isEmpty()) return config;
    QSettings settings(path,QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
    settings.setIniCodec("UTF-8");
#endif
    settings.sync(); // 下一通话显式刷新外部编辑后的文件；本读取器不设置任何键。
    settings.beginGroup(QStringLiteral("VoiceCallDsp"));
    // 参数只读；不补写默认值，不触及MQTT/网络凭据，也不在通话中热重建状态。
    const auto text=[&](const char *key,const QString &fallback) {
        return settings.value(QString::fromLatin1(key),fallback).toString().section(';',0,0).trimmed();
    };
    const auto flag=[&](const char *key,bool fallback) {
        const QString value=text(key,fallback ? QStringLiteral("true") : QStringLiteral("false")).toLower();
        if (value=="true" || value=="1" || value=="yes" || value=="on") return true;
        if (value=="false" || value=="0" || value=="no" || value=="off") return false;
        notes<<QStringLiteral("invalid boolean %1; default used").arg(QString::fromLatin1(key));return fallback;
    };
    const auto number=[&](const char *key,int fallback,int low,int high) {
        bool ok=false;const int value=text(key,QString::number(fallback)).toInt(&ok);
        if (!ok) { notes<<QStringLiteral("invalid integer %1; default used").arg(QString::fromLatin1(key));return fallback; }
        const int bounded=qBound(low,value,high);
        if (bounded!=value) notes<<QStringLiteral("clamped %1 to %2").arg(QString::fromLatin1(key)).arg(bounded);
        return bounded;
    };
    config.aecEnabled=flag("aecEnabled",config.aecEnabled);
    config.preprocessEnabled=flag("preprocessEnabled",config.preprocessEnabled);
    config.denoiseEnabled=flag("denoiseEnabled",config.denoiseEnabled);
    config.agcEnabled=flag("agcEnabled",config.agcEnabled);
    config.filterLengthMs=number("filterLengthMs",config.filterLengthMs,40,500);
    config.referenceDelayMs=number("referenceDelayMs",config.referenceDelayMs,-100,300);
    config.alignmentToleranceMs=number("alignmentToleranceMs",config.alignmentToleranceMs,1,20);
    config.referenceHistoryFrames=number("referenceHistoryFrames",config.referenceHistoryFrames,8,64);
    config.noiseSuppressDb=number("noiseSuppressDb",config.noiseSuppressDb,-30,0);
    config.echoSuppressDb=number("echoSuppressDb",config.echoSuppressDb,-40,0);
    config.echoSuppressActiveDb=number("echoSuppressActiveDb",config.echoSuppressActiveDb,-12,0);
    if (config.echoSuppressActiveDb<config.echoSuppressDb) {
        config.echoSuppressActiveDb=config.echoSuppressDb;notes<<QStringLiteral("near-end echo suppression limited to preserve speech");
    }
    config.agcTarget=number("agcTarget",config.agcTarget,1000,16000);
    config.agcMaxGainDb=number("agcMaxGainDb",config.agcMaxGainDb,0,12);
    config.agcIncrementDbPerSec=number("agcIncrementDbPerSec",config.agcIncrementDbPerSec,1,12);
    config.agcDecrementDbPerSec=number("agcDecrementDbPerSec",config.agcDecrementDbPerSec,-30,-1);
    // 库路径不再是INI配置项；旧文件中的libraryPath忽略，生产使用系统搜索。
    if (settings.status()!=QSettings::NoError) notes<<QStringLiteral("INI read error; available values/defaults used");
    if (warnings) *warnings=notes.join(QStringLiteral("; "));
    return config;
}

/** @brief 采集线程独占的实现；声明顺序确保预处理器先于关联AEC析构。 */
struct VoiceCall::AudioProcessing::Impl {
    std::unique_ptr<SpeexAec> aec; /**< 拥有回声状态，先创建、后销毁。 */
    std::unique_ptr<SpeexPreprocessor> preprocess; /**< 可选关联同一AEC，先销毁。 */
    std::unique_ptr<SpeexPreprocessor> fallback; /**< 独立后备，不关联旧残余回声估计；仅AEC+预处理开启时创建。 */
    CaptureContinuity continuity; /**< 同一采集帧切换平滑状态，不缓存历史语音。 */
    ProcessingStats counters;
    ProcessingObservation observed;ProcessingPcm observedPcm;bool pcmObservation=false; /**< 最近处理的固定诊断；PCM复制需显式启用。 */
    PcmShapeTracker rawShape,aecShape,preShape,outShape; /**< 单线程跨块边界观察，与Speex状态独立。 */
    bool aligned=false; /**< true表示上帧已使用有效参考；失步只重置一次。 */
    uint64_t generation=0; /**< 最近使用的连续播放历史代次。 */
    QString notice; /**< 初始化/重同步等低频事件，读取后清空。 */
    /** @brief 处理器初始化/重置异常时降级，不将半处理帧送出。
     * @param[in] error 不含录音正文的错误说明。
     */
    void degrade(const QString &error) {
        fallback.reset();preprocess.reset();aec.reset();aligned=false;generation=0;
        ++counters.failures;notice=QStringLiteral("[call-dsp] bypass V1: ")+error;
    }
};
VoiceCall::AudioProcessing::AudioProcessing(const ProcessingConfig &config,QString &information):impl_(new Impl)
{
    information=QStringLiteral("[call-dsp] config aec=%1 preprocess=%2 denoise=%3 agc=%4 tailMs=%5 refDelayMs=%6 toleranceMs=%7 history=%8 noiseDb=%9 echoDb=%10 activeDb=%11")
        .arg(config.aecEnabled).arg(config.preprocessEnabled).arg(config.denoiseEnabled).arg(config.agcEnabled)
        .arg(config.filterLengthMs).arg(config.referenceDelayMs).arg(config.alignmentToleranceMs).arg(config.referenceHistoryFrames)
        .arg(config.noiseSuppressDb).arg(config.echoSuppressDb).arg(config.echoSuppressActiveDb);
    try {
        QString error;
        if (config.aecEnabled) {
            impl_->aec=SpeexAec::create(config,error);
            if (!impl_->aec) { impl_->degrade(error);information+=QStringLiteral("; ")+takeNotice();return; }
        }
        if (config.preprocessEnabled) {
            impl_->preprocess=SpeexPreprocessor::create(config,impl_->aec.get(),error);
            if (!impl_->preprocess) { impl_->degrade(error);information+=QStringLiteral("; ")+takeNotice();return; }
        }
        if(impl_->aec && impl_->preprocess) {
            impl_->fallback=SpeexPreprocessor::create(config,nullptr,error);
            if(!impl_->fallback) information+=QStringLiteral("; fallback preprocessor unavailable (DC-only transition fallback)");
        }
        if (needsReference()) information+=QStringLiteral("; SpeexDSP ready (AEC waits for aligned ALSA reference)");
        else if (active()) information+=QStringLiteral("; SpeexDSP ready (preprocessor only)");
        else information+=QStringLiteral("; V1 direct (all processing disabled)");
    } catch (const std::exception &error) { impl_->degrade(QString::fromLocal8Bit(error.what()));information+=QStringLiteral("; ")+takeNotice(); }
}
VoiceCall::AudioProcessing::~AudioProcessing()=default;
bool VoiceCall::AudioProcessing::active() const { return bool(impl_->aec) || bool(impl_->preprocess); }
bool VoiceCall::AudioProcessing::needsReference() const { return bool(impl_->aec); }
void VoiceCall::AudioProcessing::reset(const QString &reason)
{
    if (!active()) return;
    const auto started=monotonicUs();
    impl_->continuity.reset();
    impl_->rawShape.reset();impl_->aecShape.reset();impl_->preShape.reset();impl_->outShape.reset();
    try {
        if (impl_->aec) impl_->aec->reset();
        if (impl_->preprocess) {
            impl_->preprocess->reset();
            if (!impl_->preprocess->valid()) { impl_->degrade(QStringLiteral("preprocess reset failed"));return; }
        }
        impl_->aligned=false;impl_->generation=0;++impl_->counters.resets;
        impl_->notice=QStringLiteral("[call-dsp] reset: ")+reason;
    } catch (const std::exception &error) { impl_->degrade(QString::fromLocal8Bit(error.what())); }
    impl_->counters.maxProcessingUs=std::max(impl_->counters.maxProcessingUs,monotonicUs()-started);
}
void VoiceCall::AudioProcessing::process(Pcm &mic,const Pcm *reference,uint64_t generation)
{
    impl_->observed={};impl_->observedPcm.valid=0;
    impl_->observed.raw=impl_->rawShape.observe(mic);impl_->observed.valid=1;
    /** @brief 所有提前返回/异常均只读记录最终PCM，不干预原处理或原错误回退。 */
    struct OutputObservation { Impl *impl;Pcm &pcm;~OutputObservation() { impl->observed.output=impl->outShape.observe(pcm);impl->observed.valid|=8; } } output{impl_.get(),mic};
    if (!active()) { ++impl_->counters.bypassFrames;return; }
    const auto started=monotonicUs();
    const Pcm original=mic;Pcm fallbackPcm{};
    if(impl_->aec) {
        impl_->continuity.prepare(original,fallbackPcm);
        if(impl_->fallback) {
            const auto dcFallback=fallbackPcm;fallbackPcm=original;
            try { impl_->fallback->process(fallbackPcm.data(),Samples); }
            catch(const std::exception &) { impl_->fallback.reset();fallbackPcm=dcFallback; }
        }
    }
    if (impl_->aec && !reference) {
        ++impl_->counters.referenceMisses;++impl_->counters.bypassFrames;
        if(impl_->aligned) {
            try { impl_->aec->reset();impl_->aligned=false;impl_->generation=0;++impl_->counters.resets;
                impl_->notice=QStringLiteral("[call-dsp] reset AEC only: playback reference unavailable; preprocess preserved");
            } catch(const std::exception &error) { impl_->degrade(QString::fromLocal8Bit(error.what()));mic=original;return; }
        }
        mic=fallbackPcm;++impl_->counters.fallbackFrames;impl_->observed.fallback=true;
        if(impl_->fallback) {
            impl_->observed.preprocess=impl_->preShape.observe(mic);impl_->observed.valid|=4;impl_->observed.pre=impl_->fallback->observation();
            if(impl_->pcmObservation) { impl_->observedPcm.preprocess=mic;impl_->observedPcm.valid|=4; }
        }
        if(impl_->fallback) ++impl_->counters.preprocessFrames;
        impl_->counters.transitionFrames+=impl_->continuity.finish(mic,false,nullptr);
        impl_->counters.maxProcessingUs=std::max(impl_->counters.maxProcessingUs,monotonicUs()-started);
        return;
    }
    try {
        if (impl_->aec) {
            if (impl_->generation && impl_->generation!=generation) {
                impl_->aec->reset();impl_->aligned=false;impl_->generation=0;++impl_->counters.resets;
                impl_->notice=QStringLiteral("[call-dsp] reset AEC only: playback timeline changed; preprocess preserved");
            }
            if (!active()) { ++impl_->counters.bypassFrames;return; }
            impl_->aec->process(mic.data(),reference->data(),mic.data(),Samples);
            impl_->observed.aec=impl_->aecShape.observe(mic);impl_->observed.valid|=2;
            if(impl_->pcmObservation) { impl_->observedPcm.aec=mic;impl_->observedPcm.valid|=2; }
            impl_->aligned=true;impl_->generation=generation;++impl_->counters.aecFrames;
        }
        if (impl_->preprocess) {
            impl_->preprocess->process(mic.data(),Samples);++impl_->counters.preprocessFrames;
            impl_->observed.preprocess=impl_->preShape.observe(mic);impl_->observed.valid|=4;impl_->observed.pre=impl_->preprocess->observation();
            if(impl_->pcmObservation) { impl_->observedPcm.preprocess=mic;impl_->observedPcm.valid|=4; }
        }
        if(impl_->aec) impl_->counters.transitionFrames+=impl_->continuity.finish(mic,true,&fallbackPcm);
    } catch (const std::exception &error) {
        mic=original;impl_->degrade(QString::fromLocal8Bit(error.what()));++impl_->counters.bypassFrames;
    }
    impl_->counters.maxProcessingUs=std::max(impl_->counters.maxProcessingUs,monotonicUs()-started);
}
VoiceCall::ProcessingStats VoiceCall::AudioProcessing::stats() const { return impl_->counters; }
QString VoiceCall::AudioProcessing::takeNotice() { auto value=impl_->notice;impl_->notice.clear();return value; }

/** @brief 返回本帧纯数值副本；调用不改变Speex或边界观察状态。 */
VoiceCall::ProcessingObservation VoiceCall::AudioProcessing::observation() const { return impl_->observed; }
/** @brief 只切换中间PCM副本开关，不重置或修改处理器。 */
void VoiceCall::AudioProcessing::enablePcmObservation(bool enabled) { impl_->pcmObservation=enabled;if(!enabled) impl_->observedPcm.valid=0; }
/** @brief 仅采集线程读取可选中间PCM；valid=0表示未复制，不能把零填充当实际输出。 */
VoiceCall::ProcessingPcm VoiceCall::AudioProcessing::pcmObservation() const { return impl_->observedPcm; }
