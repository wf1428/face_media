/**
 * @file CallRingtone.cpp
 * @brief 将内置外呼 MP3 接入既有 GStreamer 播报与共享混音链路。
 * @author Du lin
 * @date 2026-11-05
 */
#include "CallRingtone.h"
#include "common/configuration_work.h"
#include "components/multimedia/gst_player_widget.h"

#include <QDebug>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>

/** @brief 后台生成的铃声文件，临时目录随最后一个持有者销毁。 */
struct CallRingtone::Asset {
    std::unique_ptr<QTemporaryDir> directory; /**< 播放器释放后自动清理。 */
    QString path; /**< 完成回调成功时可用的本地文件路径。 */
};

CallRingtone::CallRingtone(QObject *parent)
    : QObject(parent), asset_(std::make_shared<Asset>())
{
    const QPointer<CallRingtone> alive(this);
    ConfigurationWork::queue().enqueue([alive, asset=asset_] {
        return SerialWorkQueue::Task{[asset]() -> QString {
            QFile source(QStringLiteral(":/common/audio/phone_ringing.mp3"));
            if (!source.open(QIODevice::ReadOnly)) {
                return QStringLiteral("外呼铃声资源无法打开");
            }
            asset->directory = std::make_unique<QTemporaryDir>();
            if (!asset->directory->isValid()) {
                return QStringLiteral("无法创建外呼铃声临时目录");
            }
            const auto data = source.readAll();
            const auto path = asset->directory->filePath(QStringLiteral("phone_ringing.mp3"));
            QFile file(path);
            if (data.isEmpty() || !file.open(QIODevice::WriteOnly) ||
                    file.write(data) != data.size() || !file.flush()) {
                return QStringLiteral("外呼铃声临时文件写入失败");
            }
            asset->path = path;
            return {};
        }, [alive](const QString &error) {
            if (!alive) return;
            if (!error.isEmpty()) {
                qWarning().noquote() << error;
                return;
            }
            alive->assetReady_ = true;
            alive->playIfReady();
        }};
    });
}

CallRingtone::~CallRingtone()
{
    setPhase(CallManager::RingtonePhase::Off);
}

void CallRingtone::ensurePlayer()
{
    if (player_) return;
    player_ = std::make_unique<GstPlayerWidget>();
    player_->setPromptAudioMode(true);
    connect(player_.get(), &GstPlayerWidget::videoFinished, this, [this]() {
        if (!playing_) return;
        playing_ = false;
        playIfReady();
    });
    connect(player_.get(), &GstPlayerWidget::errorOccured, this,
            [this](const QString &error) {
        if (!playing_) return;
        qWarning().noquote() << QStringLiteral("外呼铃声播放失败：") + error;
        stopPlayback();
    });
}

void CallRingtone::setPhase(CallManager::RingtonePhase phase)
{
    using Phase = CallManager::RingtonePhase;
    if (phase_ == phase) return;
    phase_ = phase;

    switch (phase) {
    case Phase::Preparing:
        ensurePlayer();
        player_->setCallRingtonePriority(true);
        break;
    case Phase::Playing:
        playIfReady();
        break;
    case Phase::HandingOff:
        stopPlayback();
        // 换代后旧铃声分支的缓存失效。新代次不播放，直到 AudioEngine 独占输出。
        player_->setCallRingtonePriority(true);
        break;
    case Phase::Off:
        player_->setCallRingtonePriority(false);
        stopPlayback();
        break;
    }
}

void CallRingtone::playIfReady()
{
    if (phase_ != CallManager::RingtonePhase::Playing || !assetReady_ || playing_) return;
    playing_ = true;
    player_->setMedia(asset_->path);
    player_->play();
}

void CallRingtone::stopPlayback()
{
    if (!playing_) return;
    playing_ = false;
    player_->stop();
}
