/**
 * @file fake_audio_device.cpp
 * @brief 可控音频边界，测试真实引擎的线程调度、停止和失败收尾，不访问硬件。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-05；修改人：Du lin；移除本轮排查与整改新增的测试代码，保留原有测试基线。
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
}
AlsaAudioDevice::~AlsaAudioDevice() { close(); }
bool AlsaAudioDevice::configureRoutes(QString &) { QThread::msleep(25); return true; }
bool AlsaAudioDevice::open(bool capture,QString &error) {
    if (VoiceTest::failOpen.load()) { error=QStringLiteral("injected ALSA open failure"); return false; }
    capture_=capture; pcm_=reinterpret_cast<snd_pcm_t *>(1); ++VoiceTest::activeDevices;
    nextFrameEndUs=VoiceCall::monotonicUs()+20000;return true;
}
bool AlsaAudioDevice::transfer(VoiceCall::Pcm &pcm,const std::atomic_bool &stop) {
    // 模拟硬件持续采样：调度迟到时读出积存块，而非把迟到加入下一块采样周期。
    const auto frameEndUs=nextFrameEndUs;nextFrameEndUs+=20000;
    while(!stop.load() && VoiceCall::monotonicUs()<frameEndUs) QThread::usleep(1000);
    if (capture_) for (int i=0;i<VoiceCall::Samples;++i) pcm[i]=int16_t(i-160);
    if (!capture_ && pcm[0]!=0) ++VoiceTest::playedNonzero;
    if (timingEnabled_) {
        lastTiming_={frameEndUs-(capture_ ? 60000 : 20000),true,20000};
        if (!capture_) lastPlaybackPcm_=pcm;
    }
    return !stop.load();
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
