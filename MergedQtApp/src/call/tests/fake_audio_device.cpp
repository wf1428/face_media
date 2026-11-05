/**
 * @file fake_audio_device.cpp
 * @brief 可控音频边界，测试真实引擎的线程调度、停止和失败收尾，不访问硬件。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；增加采集时间查询失效原因的单次注入，保留原硬件时钟/默认PCM，验证无XRUN时精确参考失效日志与后备恢复。
 */
#include "src/call/AlsaAudioDevice.h"
#include "src/call/AudioOutputFocus.h"
#include "src/call/AudioProcessing.h"
#include <QThread>
#include <QTimer>
#include <atomic>
#include "platform/rk3566_platform.h"

namespace {
/** @brief 每个模拟音频线程独占一个声卡时钟；同线程一次仅打开一个替身设备。 */
thread_local int64_t nextFrameEndUs=0; /**< 下次20ms采样块结束时刻；open初始化，不随调度抖动回拨。 */
}

namespace Rk3566Platform {
/** @brief 键盘测试不读取生产 UI 配置；空路径使响应式配置使用默认值。
 * @return 空字符串，不依赖设备上的 /home/cat 配置。
 */
QString uiConfigPath() { return {}; }
/** @return 测试显式创建的隔离DSP配置，不读取生产网络配置。 */
QString netConfigPath() { return qEnvironmentVariable("VOICE_CALL_TEST_INI"); }
}

namespace VoiceTest {
std::atomic_bool failOpen{false}; /**< 下次设备打开失败，用于错误恢复测试。 */
std::atomic_int volumeDelayMs{0}; /**< 模拟音量异步就绪延迟，单位毫秒。 */
std::atomic_int activeDevices{0}; /**< 仍未关闭的模拟句柄数量。 */
std::atomic_int playedNonzero{0}; /**< 实际播放非静音 PCM 的帧数，验证下行经过真实引擎。 */
std::atomic_int exclusive{0}; /**< 当前通话是否持有共享输出独占。 */
std::atomic_int restoreDelayMs{0}; /**< 模拟恢复音频设备的后台延迟，单位毫秒。 */
std::atomic_int captureTimingFault{0}; /**< 下次采集时间判定失败的TimingReason整数，0不注入，不伪造XRUN。 */
std::atomic_int capturePauseMs{0}; /**< 下次采集额外暂停毫秒，0不注入；不伪造XRUN。 */
std::atomic_int captureGain{1}; /**< 采集测试样本倍数，默认1；限幅到int16有效范围。 */
std::atomic_int playbackXrunPauseMs{0}; /**< 正值使下次播放暂停该毫秒并恢复一次XRUN；单原子命令避免暂停与错误注入错帧。 */
std::atomic_int playbackDiagnosticDelay{-1}; /**< 仅诊断快照的延迟帧数注入；-1关闭，不改变模拟AEC时间或播放样本。 */
}
AlsaAudioDevice::~AlsaAudioDevice() { close(); }
bool AlsaAudioDevice::configureRoutes(QString &) { QThread::msleep(25); return true; }
bool AlsaAudioDevice::open(bool capture,QString &error) {
    if (VoiceTest::failOpen.load()) { error=QStringLiteral("injected ALSA open failure"); return false; }
    capture_=capture; pcm_=reinterpret_cast<snd_pcm_t *>(1); ++VoiceTest::activeDevices;
    transferTrace_={};transferSequence_=0;lastIoSuccessUs_=0;xruns_=0;
    nextFrameEndUs=VoiceCall::monotonicUs()+20000;return true;
}
bool AlsaAudioDevice::transfer(VoiceCall::Pcm &pcm,const std::atomic_bool &stop) {
    const auto previous=transferTrace_;transferTrace_={};auto &trace=transferTrace_;
    trace.sequence=++transferSequence_;trace.beginUs=VoiceCall::monotonicUs();trace.xrunsBefore=xruns_;
    trace.entryGapUs=previous.beginUs ? trace.beginUs-previous.beginUs : 0;trace.lastWriteBeforeUs=lastIoSuccessUs_;
    if(capture_) { const int pause=VoiceTest::capturePauseMs.exchange(0);if(pause>0) QThread::msleep(unsigned(pause)); }
    if(!capture_) {
        const int pause=VoiceTest::playbackXrunPauseMs.exchange(0);
        if(pause>0) {
            QThread::msleep(unsigned(pause));
            ++xruns_;trace.faultCode=-32;trace.faultSource=1;trace.faultUs=VoiceCall::monotonicUs();
            trace.faultSinceWriteUs=lastIoSuccessUs_ ? trace.faultUs-lastIoSuccessUs_ : -1;
        }
    }
    // 模拟硬件持续采样：调度迟到时读出积存块，而非把迟到加入下一块采样周期。
    const auto frameEndUs=nextFrameEndUs;nextFrameEndUs+=20000;
    while(!stop.load() && VoiceCall::monotonicUs()<frameEndUs) QThread::usleep(1000);
    if (capture_) { const int gain=VoiceTest::captureGain.load();for (int i=0;i<VoiceCall::Samples;++i) pcm[i]=int16_t(std::max(-32768,std::min(32767,(i-160)*gain))); }
    if (!capture_ && pcm[0]!=0) ++VoiceTest::playedNonzero;
    if (timingEnabled_) {
        lastTiming_={frameEndUs-(capture_ ? 60000 : 20000),true,xruns_};lastTiming_.reason=VoiceCall::TimingReason::Valid;
        if (!capture_) lastPlaybackPcm_=pcm;
    }
    trace.endUs=VoiceCall::monotonicUs();trace.ioCalls=1;trace.ioUs=trace.ioMaxUs=trace.endUs-trace.beginUs;
    trace.completed=!stop.load();trace.frames=trace.completed ? VoiceCall::Samples : 0;trace.xrunsAfter=xruns_;lastIoSuccessUs_=trace.endUs;
    const int diagnosticDelay=VoiceTest::playbackDiagnosticDelay.load();
    if(!capture_ && diagnosticDelay>=0) {
        geometry_.bufferFrames=960;trace.queryRc=0;trace.queryRunning=true;trace.delayFrames=diagnosticDelay;trace.available=960-diagnosticDelay;
    }
    if(capture_ && timingEnabled_) { const int reason=VoiceTest::captureTimingFault.exchange(0);if(reason>0) { lastTiming_.valid=false;lastTiming_.reason=VoiceCall::TimingReason(reason);lastTiming_.querySpanUs=reason==int(VoiceCall::TimingReason::SlowQuery) ? 6000 : 80;trace.queryRc=reason==int(VoiceCall::TimingReason::QueryError) ? -5 : 0;trace.state=3;trace.available=640;trace.delayFrames=40; } }
    return trace.completed;
}
int AlsaAudioDevice::delayMs() const { return 20; }
void AlsaAudioDevice::close() { if (pcm_) { pcm_=nullptr; --VoiceTest::activeDevices; } }
bool VoiceCall::setMediaAudioExclusive(bool exclusive) {
    if (!exclusive) QThread::msleep(unsigned(VoiceTest::restoreDelayMs.load()));
    VoiceTest::exclusive=exclusive ? 1 : 0; return true;
}

void VoiceCall::initializeOutputVolume() {}
void VoiceCall::acquireOutputVolume(QObject *owner,std::function<void()> ready) {
    auto *ticket=new QObject(owner);ticket->setObjectName("voiceCallVolumeLease");
    QTimer::singleShot(VoiceTest::volumeDelayMs.load(),ticket,[ready]() { ready(); });
}
void VoiceCall::releaseOutputVolume(QObject *owner) {
    delete owner->findChild<QObject *>(QStringLiteral("voiceCallVolumeLease"),Qt::FindDirectChildrenOnly);
}
