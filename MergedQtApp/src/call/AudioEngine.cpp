/**
 * @file AudioEngine.cpp
 * @brief 实现有限等待的全双工音频线程、静音补帧和音频设备异步交接。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-05；修改人：Du lin；移除逐帧诊断、PCM转储及调度采样；保留原周期和结束汇总、XRUN、无锁交接、参考对齐和播放节拍。
 */
#include "AudioEngine.h"
#include "AlsaAudioDevice.h"
#include "AudioOutputFocus.h"
#include "platform/rk3566_platform.h"
#include <chrono>
#include <thread>
#include <condition_variable>
#include <ctime>
#include <QStringList>

namespace {
/** @brief 成功提交 ALSA 的参考帧；序号缺口或无效时间先清理采集端旧时间线。 */
struct ReferenceEvent {
    VoiceCall::Pcm pcm{}; /**< 实际写入的单声道 PCM，仅 valid 时使用。 */
    int64_t firstUs=0; /**< 播放首样本的单调微秒。 */
    uint64_t sequence=0; /**< 包含失效事件的播放序号，用于检测 SPSC 缺口。 */
    bool valid=false,reset=false; /**< valid 允许加入历史；reset 清理旧时间线。 */
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
    if (isRunning()) return;
    incoming_.reset();rxStatsEpoch_=0;rxFrames_=0;lastReceivedMs_=0;
    { std::lock_guard<std::mutex> lock(txMutex_);
      VoiceCall::Frame discarded;while(outgoing_.pop(discarded)) {}
      reference_=VoiceCall::PlaybackReference{};dspStats_={};captureDelayUs_=-1;
      referenceDepth_=0;referenceGeneration_=1;referencePhaseUs_=0;
      txDrops_=txFrames_=0;txHighWater_=0;
      micLevel_={};captureGapMaxMs_=0; }
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
        if(now-candidate.localMs>100) { ++txDrops_;continue; }
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
    std::atomic_bool captureReady{false},captureFailed{false},needReference{false};
    uint64_t captureXruns=0,completedFrames=0;VoiceCall::LevelStats completedLevel;
    int64_t completedDelayUs=-1;
    auto referenceEvents=std::make_unique<VoiceCall::SpscQueue<ReferenceEvent,64>>();

    // 保留原 3 秒汇总，格式化和统计锁仍留在后台报告线程。
    // 播放只投递汇总所需的数值；报告线程迟到时可跳过一次快照，不影响出声。
    struct PlaybackTotals {
        VoiceCall::LevelStats level; /**< 已写入的播放电平累计。 */
        uint64_t frames=0; /**< 已完整写入的帧数，含补静音。 */
        int64_t delayUs=-1; /**< 最近已取得的排队延迟，-1 表示未知。 */
    };
    VoiceCall::SpscQueue<PlaybackTotals,4> reportQueue;
    std::mutex reportMutex;std::condition_variable reportWake;bool reportStop=false;
    std::thread reporter;
    try {
        reporter=std::thread([&]() {
            std::unique_lock<std::mutex> lock(reportMutex);
            while(!reportWake.wait_for(lock,std::chrono::milliseconds(100),[&]{return reportStop;})) {
                lock.unlock();
                PlaybackTotals totals;bool updated=false;
                while(reportQueue.pop(totals)) {
                    playedFrames_=totals.frames;playLevel_=totals.level;playbackDelayUs_=totals.delayUs;
                    updated=true;
                }
                if(updated) reportStatistics(QStringLiteral("tick"));
                lock.lock();
            }
        });
    } catch(const std::exception &) {
        // 汇总线程创建失败仍可录放，退出后继续输出原有 final。
    }
    if (!stop_.load() && AlsaAudioDevice::configureRoutes(error) && playback.open(false,error)) {
        std::thread capture;
        try { capture=std::thread([&]() {
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
            captureReady=true;
            uint8_t seq=0;bool wasTalking=false;uint64_t previousXruns=device.xruns(),lastReferenceSequence=0;
            while (!stop_.load()) {
                VoiceCall::Frame frame;
                if (!device.transfer(frame.pcm,stop_)) {
                    if (!stop_.exchange(true)) emit failed(QStringLiteral("麦克风读取失败或超时"));
                    break;
                }
                frame.localMs=nowMs()-20; frame.seq=seq++;
                if (!talking_.load()) { wasTalking=false;previousXruns=device.xruns();continue; }
                if (!wasTalking || previousXruns!=device.xruns()) {
                    if (processing) processing->reset(wasTalking ? QStringLiteral("capture XRUN") : QStringLiteral("TALKING started"));
                    if (needReference.load()) reference_.clear();
                    wasTalking=true;
                }
                previousXruns=device.xruns();
                // 参考历史只有采集线程持有；先按播放序号排空交接，缺口必须清旧时间线。
                ReferenceEvent event;
                for(int n=0;n<64 && referenceEvents->pop(event);++n) {
                    const bool gap=lastReferenceSequence && event.sequence!=lastReferenceSequence+1;
                    if(event.reset || gap) reference_.clear();
                    lastReferenceSequence=event.sequence;
                    if(event.valid) reference_.push(event.pcm,event.firstUs,int64_t(processingConfig.alignmentToleranceMs)*1000,size_t(processingConfig.referenceHistoryFrames));
                }
                const auto rawMic=frame.pcm;
                VoiceCall::Pcm ref{};bool aligned=false;uint64_t referenceGeneration=0;
                if (needReference.load()) {
                    const auto timing=device.timing();
                    if (timing.valid) aligned=reference_.read(timing.firstSampleUs,int64_t(processingConfig.referenceDelayMs)*1000,ref);
                    referenceGeneration=reference_.generation();
                }
                if (processing) {
                    processing->process(frame.pcm,aligned ? &ref : nullptr,referenceGeneration);
                    needReference=processing->needsReference();device.enableTiming(needReference.load());
                    const auto notice=processing->takeNotice();if (!notice.isEmpty()) emit statistics(notice);
                }
                if(talking_.load()) {
                    captureTotals.add(rawMic);
                    if(captureLastMs) captureGapMs=std::max(captureGapMs,frame.localMs-captureLastMs);
                    captureLastMs=frame.localMs;
                    bool replaced=false;
                    if(!outgoing_.push(frame,true,&replaced) || replaced) ++txDrops_;
                    const auto depth=outgoing_.size();if(depth>txHighWater_.load()) txHighWater_=depth;
                    // 诊断若被调度延迟不能拖住采集；下帧发布累计值，最终退出再完整补齐。
                    std::unique_lock<std::mutex> lock(txMutex_,std::try_to_lock);
                    if(lock.owns_lock()) {
                        dspStats_=processing ? processing->stats() : VoiceCall::ProcessingStats{};micLevel_=captureTotals;captureGapMaxMs_=captureGapMs;
                        referenceDepth_=reference_.size();referenceGeneration_=reference_.generation();referencePhaseUs_=reference_.phaseErrorUs();
                        captureDelayUs_=device.timing().queueDelayUs;
                    }
                }
            }
            { std::lock_guard<std::mutex> lock(txMutex_);micLevel_=captureTotals;captureGapMaxMs_=captureGapMs;
              if(processing) dspStats_=processing->stats();
              referenceDepth_=reference_.size();referenceGeneration_=reference_.generation();referencePhaseUs_=reference_.phaseErrorUs();captureDelayUs_=device.timing().queueDelayUs; }
            captureXruns=device.xruns();
        }); } catch (const std::exception &failure) {
            stop_=true; emit failed(QStringLiteral("无法启动采集线程: %1").arg(QString::fromLocal8Bit(failure.what())));
        }
        while (!stop_.load() && !captureReady.load()) msleep(2);
        if (!stop_.load() && !captureFailed.load()) emit ready();
        VoiceCall::PlaybackPacer pacer(VoiceCall::monotonicUs());
        uint64_t previousPlaybackXruns=playback.xruns(),referenceSequence=0;
        auto nextReport=nowMs()+3000;
        while (!stop_.load()) {
            playback.enableTiming(needReference.load());
            VoiceCall::Frame frame;
            if (talking_.load()) frame=incoming_.take(nowMs());
            if (!playback.transfer(frame.pcm,stop_)) {
                if(!stop_.exchange(true)) emit failed(QStringLiteral("喇叭写入失败或超时"));
                break;
            }
            VoiceCall::Pcm actualPlayback{};VoiceCall::AudioTiming playbackTiming;
            const bool validReference=needReference.load() && playback.playbackReference(actualPlayback,playbackTiming);
            ReferenceEvent event;event.sequence=++referenceSequence;event.valid=validReference;
            event.reset=previousPlaybackXruns!=playback.xruns() || !validReference;
            if(validReference) { event.pcm=actualPlayback;event.firstUs=playbackTiming.firstSampleUs; }
            if(needReference.load()) referenceEvents->push(event);
            ++completedFrames;completedLevel.add(frame.pcm);
            completedDelayUs=playback.timing().queueDelayUs;
            const auto now=nowMs();
            if(now>=nextReport) {
                reportQueue.push({completedLevel,completedFrames,completedDelayUs});
                nextReport=now+3000;
            }
            const auto sleepStart=VoiceCall::monotonicUs();
            const auto nextUs=pacer.advance(sleepStart,previousPlaybackXruns!=playback.xruns());
            previousPlaybackXruns=playback.xruns();
            if(nextUs>sleepStart) std::this_thread::sleep_for(std::chrono::microseconds(nextUs-sleepStart));
        }
        stop_=true; if (capture.joinable()) capture.join();
    } else if (!stop_.load()) emit failed(error);
    { std::lock_guard<std::mutex> lock(reportMutex);reportStop=true; }
    reportWake.notify_one();if(reporter.joinable()) reporter.join();
    playedFrames_=completedFrames;playLevel_=completedLevel;playbackDelayUs_=completedDelayUs;
    talking_=false;
    const auto playbackXruns=playback.xruns();
    playback.close();
    if (!VoiceCall::setMediaAudioExclusive(false)) emit statistics(QStringLiteral("媒体音频恢复失败"));
    reportStatistics(QStringLiteral("final"));
    emit statistics(QStringLiteral("xrun capture=%1 playback=%2").arg(captureXruns).arg(playbackXruns));
    // 下行槽保留至下一次begin逻辑换代，不能在迟到接收仍可能返回时物理重置槽。
    VoiceCall::Frame discarded;while(outgoing_.pop(discarded)) {}
    reference_.clear();

}

void AudioEngine::reportStatistics(const QString &phase)
{
    VoiceCall::LevelStats mic,rx,played;
    uint64_t txFrames,rxFrames,txDrops,rxDrops,underruns,playFrames;
    size_t txPeak,rxPeak,referenceDepth;int64_t elapsed,gap,captureDelay,playbackDelay,referencePhase;
    VoiceCall::ProcessingStats dsp;uint64_t referenceGeneration;
    {
        std::lock_guard<std::mutex> lock(txMutex_);
        mic=micLevel_;txFrames=txFrames_;txDrops=txDrops_;
        txPeak=txHighWater_;gap=captureGapMaxMs_;dsp=dspStats_;captureDelay=captureDelayUs_;
        referenceDepth=referenceDepth_;referenceGeneration=referenceGeneration_;referencePhase=referencePhaseUs_;
    }
    {
        const bool current=rxStatsEpoch_.load(std::memory_order_acquire)==incoming_.generation();
        rx=current ? rxTotals_.snapshot() : VoiceCall::LevelStats{};rxFrames=current ? rxFrames_.load() : 0;rxDrops=incoming_.dropped.load();underruns=incoming_.underruns.load();rxPeak=incoming_.highWater.load();
    }
    // 周期播放快照来自本报告线程；结束时在其退出后使用最终累计值。
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
        +QStringLiteral(" captureDelayUs=%1 playbackDelayUs=%2 refPhaseUs=%3").arg(captureDelay).arg(playbackDelay).arg(referencePhase));
}
