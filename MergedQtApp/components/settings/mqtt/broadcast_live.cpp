/**
 * @file broadcast_live.cpp
 * @brief 校验直播二进制帧、拆分20ms采样块并按单调时钟播放。
 * @author Du lin
 * @note 最近修改：2026-11-10；修改人：Du lin；关闭独立 ALSA 并完成共享输出恢复后才结束交接，记录关闭和恢复结果。
 * @date 2026-11-05
 */
#include "broadcast_live.h"
#include "components/multimedia/gst_player_widget.h"
#include "src/call/AlsaAudioDevice.h"
#include "src/call/ConcurrentJitterBuffer.h"
#include <QDebug>
#include "src/call/RealtimeAudio.h"
#include <QtEndian>
#include <chrono>

namespace {
using Clock = std::chrono::steady_clock;
/** @return 本进程单调时钟的毫秒。 */
qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
/** @return 与播放节拍及 condition_variable 共用的本地单调微秒。 */
qint64 nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

/** @brief 20ms 直播块的本地标记，通话缓冲返回的补静音帧默认 seq 为0。 */
constexpr uint8_t LiveFrame = 1;

}

/** @brief 主线程配置、IPC生产、播放线程消费的有界状态。 */
struct BroadcastLive::State {
    /** @brief 当前媒体代次，生产和消费各一个线程，停播后在途调用仅持有旧代次。 */
    struct Media {
        std::atomic_bool stop{false}; /**< 换代、析构或通话抢占时取消声卡写入。 */
        VoiceCall::ConcurrentJitterBuffer jitter; /**< 通话同款10槽缓冲，本直播独享。 */
        std::atomic<qint64> lastArrival{nowMs()}; /**< 本地最近有效帧或开流时刻，单位毫秒。 */
        std::atomic<quint64> acceptedFrames{0}; /**< 抗抖队列接受的 MQTT 线上帧数。 */
    };
    std::mutex mutex; /**< 保护主题、媒体代次、播放门和输出统计。 */
    std::condition_variable wake; /**< 配置改变/退出时唤醒播放线程。 */
    std::shared_ptr<Media> media; /**< 当前收播代次，暂停或停止时为空。 */
    QString topic; /**< 非空时接收该设备的精确广播主题。 */
    QString epoch; /**< 本地IPC媒体代次，忽略暂停/恢复之前的积压帧。 */
    int frameMs = 20; /**< 线上帧间隔20或40ms。 */
    bool outputPending = false; /**< mutex 保护；工作线程持有代次直至完成输出归还。 */
    unsigned generation = 0; /**< 切换状态后退役旧分支。 */
    bool closing = false; /**< 析构时通知播放线程退出。 */
    std::atomic<quint64> ipcFrames{0}; /**< IPC线程计数，采样与换代边界允许一帧竞争。 */
    Statistics stats; /**< 队列锁保护有效帧和送播计数。 */
    std::atomic_int volume{80}; /**< 软件音量0..100。 */
    std::atomic_bool failed{false}; /**< 本代次 ALSA 输出失败。 */
};

BroadcastLive::BroadcastLive()
    : state_(std::make_shared<State>()), worker_([this] { run(); })
{
}

BroadcastLive::~BroadcastLive()
{
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closing = true;
        if (state_->media) state_->media->stop = true;
        state_->media.reset();
    }
    state_->wake.notify_one();
    worker_.join();
}

void BroadcastLive::configure(const QString &topic, int frameMs, bool playing, const QString &epoch)
{
    auto media = playing ? std::make_shared<State::Media>() : nullptr;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->topic = topic;
    state_->epoch = epoch;
    state_->frameMs = frameMs;
    if (state_->media) state_->media->stop = true;
    state_->media = std::move(media);
    state_->ipcFrames.store(0, std::memory_order_relaxed);
    state_->stats = {};
    state_->failed = false;
    ++state_->generation;
    state_->wake.notify_one();
}

void BroadcastLive::setVolume(int percent) { state_->volume = qBound(0, percent, 100); }
qint64 BroadcastLive::silenceMs() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->media ? nowMs() - state_->media->lastArrival.load(std::memory_order_relaxed) : 0;
}
bool BroadcastLive::failed() const { return state_->failed.load(); }

bool BroadcastLive::outputPending() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->outputPending;
}

BroadcastLive::Statistics BroadcastLive::statistics() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    Statistics stats = state_->stats;
    stats.ipcFrames = state_->ipcFrames.load(std::memory_order_relaxed);
    if (state_->media) {
        stats.acceptedFrames = state_->media->acceptedFrames.load(std::memory_order_relaxed);
        stats.jitterDrops = state_->media->jitter.dropped.load(std::memory_order_relaxed);
        stats.underruns = state_->media->jitter.underruns.load(std::memory_order_relaxed);
    }
    return stats;
}

std::function<void(const QJsonObject &)> BroadcastLive::receiver() const
{
    return [state = state_](const QJsonObject &message) {
        if (message.value("type").toString() != QStringLiteral("broadcast_audio")) return;
        state->ipcFrames.fetch_add(1, std::memory_order_relaxed);
        const QString topic = message.value("topic").toString();
        const QString epoch = message.value("epoch").toString();
        const QString hex = message.value("hex").toString();
        if (hex.size() != 1300 && hex.size() != 2580) return;
        const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
        if (bytes.size() * 2 != hex.size() || !bytes.startsWith(QByteArray::fromHex("a1c0010001"))) return;
        const auto *wire = reinterpret_cast<const uchar *>(bytes.constData());
        const auto stamp = qFromBigEndian<quint32>(wire + 6);
        const auto now = nowMs();
        const double arrivalValue = message.value("arrivalMs").toDouble(-1);
        if (arrivalValue < 0 || arrivalValue > double(now) || double(now)-arrivalValue > 200) return;
        std::shared_ptr<State::Media> media;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closing || !state->media || state->media->stop.load() || state->topic != topic || state->epoch != epoch ||
                    bytes.size() != 10 + state->frameMs * 32) return;
            media = state->media;
        }

        bool accepted = false;
        const int payloadBytes = bytes.size() - 10;
        for (int offset = 0; offset < payloadBytes; offset += VoiceCall::Samples * 2) {
            VoiceCall::Frame frame;
            frame.seq = LiveFrame;
            frame.timestamp = stamp + quint32(offset / 32);
            frame.localMs = qint64(arrivalValue);
            const auto *pcm = wire + 10 + offset;
            for (int i = 0; i < VoiceCall::Samples; ++i)
                frame.pcm[i] = qFromLittleEndian<qint16>(pcm + i * 2);
            if (media->jitter.push(frame)) accepted = true;
        }
        if (accepted) {
            media->lastArrival.store(now, std::memory_order_relaxed);
            media->acceptedFrames.fetch_add(1, std::memory_order_relaxed);
        }
    };
}

/** @brief 播放线程持有本代次设备及声卡租约，关闭句柄后才归还。 */
void BroadcastLive::run()
{
    std::unique_lock<std::mutex> lock(state_->mutex);
    while (!state_->closing) {
        state_->wake.wait(lock, [this] {
            return state_->closing || (state_->media && !state_->media->stop.load());
        });
        if (state_->closing) break;
        const auto media = state_->media;
        const auto generation = state_->generation;
        state_->outputPending = true;
        lock.unlock();

        const bool ownsOutput = GstPlayerWidget::acquireBroadcastAudioOutput(media->stop);
        AlsaAudioDevice playback;
        QString error;
        // 只打开播放设备。通话的采集路由和 AEC 由原通话引擎独立管理。
        const bool opened = ownsOutput && !media->stop.load() && playback.open(false, error);
        auto deadlineUs = nowUs();
        VoiceCall::PlaybackPacer pacer(deadlineUs);
        for (bool ok = opened; ok && !media->stop.load();) {
            lock.lock();
            state_->wake.wait_until(lock, Clock::time_point(std::chrono::microseconds(deadlineUs)),
                                    [&media] { return media->stop.load(); });
            lock.unlock();
            if (media->stop.load()) break;

            const auto tick = nowUs();
            auto frame = media->jitter.take(tick / 1000);
            const int volume = state_->volume;
            int peak = 0;
            for (auto &sample : frame.pcm) {
                sample = qint16(sample * volume / 100);
                peak = qMax(peak, qAbs(int(sample)));
            }
            const auto xrunsBefore = playback.xruns();
            ok = playback.transfer(frame.pcm, media->stop);
            const auto writtenUs = nowUs();
            const auto xruns = playback.xruns();
            deadlineUs = pacer.advance(writtenUs, xruns != xrunsBefore);
            lock.lock();
            if (generation == state_->generation) {
                state_->stats.alsaXruns = xruns;
                state_->stats.writeMaxUs = qMax(state_->stats.writeMaxUs, writtenUs - tick);
                if (ok) {
                    ++state_->stats.alsaBlocks;
                    if (frame.seq != LiveFrame) ++state_->stats.silenceBlocks;
                    if (peak > 0) ++state_->stats.nonzeroBlocks;
                    state_->stats.peak = qMax(state_->stats.peak, peak);
                }
            }
            lock.unlock();
        }

        // 先 drop 并关闭 PCM，再允许通话打开声卡或恢复视频的音频 sink。
        playback.close();
        if (ownsOutput) {
            qInfo() << "[broadcast-live] ALSA closed" << "opened=" << opened;
            const bool restored = GstPlayerWidget::releaseBroadcastAudioOutput();
            qInfo() << "[broadcast-live] shared output handoff" << "ready=" << restored;
        }
        lock.lock();
        state_->outputPending = false;
        if (generation == state_->generation && !media->stop.exchange(true)) {
            state_->failed = true;
            qWarning() << "[broadcast-live] ALSA playback failed"
                       << (opened ? QStringLiteral("write failed") :
                                  ownsOutput ? error : QStringLiteral("audio output unavailable"));
        }
    }
}
