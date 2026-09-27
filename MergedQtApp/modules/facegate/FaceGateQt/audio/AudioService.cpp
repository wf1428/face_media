/**
 * @file AudioService.cpp
 * @brief 使用进程内 GStreamer 播放门禁语音提示的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AudioService.h"

#include "components/multimedia/gst_player_widget.h"
#include "components/multimedia/vol_ctrl.h"

#include <QDateTime>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariantAnimation>
#include <QEasingCurve>

/** @brief 创建尚未配置的音频服务。 */
AudioService::AudioService(QObject *parent)
    : QObject(parent), config_(AppConfig::defaults())
{
    volumeController_ = new AmpVolumeController(this);
    // AmpVolumeController 构造时已经读取（或首次创建）volume.dat，并完成
    // RK809 输出路径及硬件音量初始化，这里只同步界面配置值。
    config_.audioVolume = volumeController_->currentPromptVolume();
    player_ = new GstPlayerWidget;
    player_->setVideoOutput(nullptr);
    player_->setVolume(1.0f);
    connect(player_, &GstPlayerWidget::videoFinished, this, [this]() {
        endVoiceFocus();
        emit audioStatus(QStringLiteral("GStreamer 提示音播放完成"));
    });
    connect(player_, &GstPlayerWidget::errorOccured, this, [this](const QString &error) {
        endVoiceFocus();
        emit audioError(QStringLiteral("GStreamer 提示音播放失败：%1").arg(error));
    });
    connect(&AudioPromptFocusManager::instance(),
            &AudioPromptFocusManager::focusReady,
            this, [this](QObject *owner) {
        if (owner == this && voiceFocusActive_ && !pendingFileName_.isEmpty()) {
            playbackStartTimer_.start();
        }
    });

    playbackStartTimer_.setSingleShot(true);
    // 硬件音量切到独立播报音量后短暂稳定；提示音链路已通过 preroll 静音准备。
    playbackStartTimer_.setInterval(15);
    connect(&playbackStartTimer_, &QTimer::timeout, this, [this]() {
        if (!player_ || pendingFileName_.isEmpty()) {
            endVoiceFocus();
            return;
        }
        player_->play();
        voiceFocusRestoreTimer_.start(15000);
        emit audioStatus(QStringLiteral("GStreamer 正在播放：%1")
                         .arg(pendingFileName_));
    });

    voiceFocusRestoreTimer_.setSingleShot(true);
    connect(&voiceFocusRestoreTimer_, &QTimer::timeout,
            this, &AudioService::endVoiceFocus);
}

AudioService::~AudioService()
{
    restoreVoiceFocusImmediately();
    if (player_) {
        player_->stop();
        delete player_;
        player_ = nullptr;
    }
}

/** @brief 替换完整运行配置，并应用音量设置。 */
void AudioService::configure(const AppConfig &config)
{
    config_ = config;
    config_.audioVolume = volumeController_
            ? volumeController_->currentPromptVolume()
            : qBound(0, config_.audioVolume, 100);
}

/** @brief 启用或关闭语音提示总开关。 */
void AudioService::setEnabled(bool enabled)
{
    config_.audioEnabled = enabled;
    AmpVolumeController::setPromptPlaybackEnabled(enabled);
}

/** @brief 设置并持久化 0~100 语音播报音量。 */
void AudioService::setVolume(int volume)
{
    config_.audioVolume = qBound(0, volume, 100);
    if (volumeController_) {
        volumeController_->setPromptVolume(config_.audioVolume);
    }
}

int AudioService::volume() const
{
    return volumeController_
            ? volumeController_->currentPromptVolume()
            : qBound(0, config_.audioVolume, 100);
}

/** @brief 在提示类型启用且冷却允许时播放对应音频。 */
void AudioService::playPrompt(Prompt prompt)
{
    if (!AmpVolumeController::isPromptPlaybackEnabled()) {
        return;
    }
    if (!promptEnabled(prompt)) {
        return;
    }
    startGstreamer(absoluteAudioPath(promptFile(prompt)), false, prompt);
}

/** @brief 按统一通行结果码播放专用失败音，未配置的结果使用通用失败音。 */
void AudioService::playAccessFailure(int resultCode)
{
    if (!AmpVolumeController::isPromptPlaybackEnabled() ||
            !config_.promptVerifyFailedEnabled) {
        return;
    }

    QString fileName = QStringLiteral("fail.wav");
    switch (resultCode) {
    case 4000:
    case 4001:
    case 4003:
    case 4100:
    case 4101:
    case 4102:
    case 4200:
    case 4201:
    case 4202:
    case 4300:
    case 4301:
    case 4400:
    case 4401:
    case 4402:
    case 4403:
    case 4501:
        fileName = QStringLiteral("code_%1.mp3").arg(resultCode);
        break;
    default:
        break;
    }
    startGstreamer(absoluteAudioPath(fileName), false, Prompt::VerifyFailed);
}

/** @brief 按配置键测试播放指定提示，忽略业务冷却。 */
void AudioService::testPlay(const QString &promptKey)
{
    if (!AmpVolumeController::isPromptPlaybackEnabled()) {
        emit audioStatus(QStringLiteral("语音播报已关闭"));
        return;
    }
    const QString key = promptKey.trimmed().toLower();
    Prompt prompt = Prompt::VerifyPassed;
    if (key == QStringLiteral("verify_passed")) {
        prompt = Prompt::VerifyPassed;
    } else if (key == QStringLiteral("verify_failed")) {
        prompt = Prompt::VerifyFailed;
    } else if (key == QStringLiteral("stranger")) {
        prompt = Prompt::Stranger;
    } else if (key == QStringLiteral("liveness_failed")) {
        prompt = Prompt::LivenessFailed;
    } else if (key == QStringLiteral("face_access_timeout")) {
        prompt = Prompt::FaceAccessTimeout;
    } else if (key == QStringLiteral("face_liveness_unavailable")) {
        prompt = Prompt::FaceLivenessUnavailable;
    } else if (key == QStringLiteral("enroll_success")) {
        prompt = Prompt::EnrollSuccess;
    } else if (key == QStringLiteral("enroll_failed")) {
        prompt = Prompt::EnrollFailed;
    } else {
        emit audioError(QStringLiteral("未知音频事件：%1").arg(promptKey));
        return;
    }
    startGstreamer(absoluteAudioPath(promptFile(prompt)), true, prompt);
}

/** @brief 停止当前 GStreamer 提示音。 */
void AudioService::stopCurrentPlayback()
{
    volumeBoostTimer_.stop();
    playbackStartTimer_.stop();
    if (player_) {
        player_->stop();
    }
    endVoiceFocus();
}

/** @return Prompt 对应的稳定配置键。 */
QString AudioService::keyForPrompt(Prompt prompt)
{
    switch (prompt) {
    case Prompt::VerifyPassed: return QStringLiteral("verify_passed");
    case Prompt::VerifyFailed: return QStringLiteral("verify_failed");
    case Prompt::Stranger: return QStringLiteral("stranger");
    case Prompt::LivenessFailed: return QStringLiteral("liveness_failed");
    case Prompt::FaceAccessTimeout: return QStringLiteral("face_access_timeout");
    case Prompt::FaceLivenessUnavailable: return QStringLiteral("face_liveness_unavailable");
    case Prompt::EnrollSuccess: return QStringLiteral("enroll_success");
    case Prompt::EnrollFailed: return QStringLiteral("enroll_failed");
    }
    return QStringLiteral("unknown");
}

/** @return 指定提示的独立开关是否启用。 */
bool AudioService::promptEnabled(Prompt prompt) const
{
    switch (prompt) {
    case Prompt::VerifyPassed: return config_.promptVerifyPassedEnabled;
    case Prompt::VerifyFailed: return config_.promptVerifyFailedEnabled;
    case Prompt::Stranger: return config_.promptStrangerEnabled;
    case Prompt::LivenessFailed: return config_.promptLivenessFailedEnabled;
    case Prompt::FaceAccessTimeout: return config_.promptVerifyFailedEnabled;
    case Prompt::FaceLivenessUnavailable: return config_.promptLivenessFailedEnabled;
    case Prompt::EnrollSuccess: return config_.promptEnrollSuccessEnabled;
    case Prompt::EnrollFailed: return config_.promptEnrollFailedEnabled;
    }
    return false;
}

/** @return 指定提示配置的文件名。 */
QString AudioService::promptFile(Prompt prompt) const
{
    switch (prompt) {
    case Prompt::VerifyPassed: return config_.promptVerifyPassedFile;
    case Prompt::VerifyFailed: return config_.promptVerifyFailedFile;
    case Prompt::Stranger: return config_.promptStrangerFile;
    case Prompt::LivenessFailed: return config_.promptLivenessFailedFile;
    case Prompt::FaceAccessTimeout: return QStringLiteral("face_access_timeout.mp3");
    case Prompt::FaceLivenessUnavailable: return QStringLiteral("face_liveness_unavailable.mp3");
    case Prompt::EnrollSuccess: return config_.promptEnrollSuccessFile;
    case Prompt::EnrollFailed: return config_.promptEnrollFailedFile;
    }
    return QString();
}

/** @return common/audio 资源物化后的临时文件绝对路径。 */
QString AudioService::absoluteAudioPath(const QString &fileName)
{
    const QString baseName = QFileInfo(fileName.trimmed()).fileName();
    if (baseName.isEmpty()) {
        emit audioError(QStringLiteral("音频文件名为空"));
        return QString();
    }

    const QString resourcePath = QStringLiteral(":/common/audio/%1").arg(baseName);
    QFile source(resourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        const QString error = QStringLiteral("音频资源不存在：%1").arg(resourcePath);
        qWarning().noquote() << error;
        emit audioError(error);
        return QString();
    }
    const QByteArray data = source.readAll();

    QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempRoot.trimmed().isEmpty()) {
        tempRoot = QStringLiteral("/tmp");
    }
    const QString tempDir = QDir(tempRoot).filePath(QStringLiteral("voice"));
    if (!QDir().mkpath(tempDir)) {
        const QString error = QStringLiteral("无法创建提示音临时目录：%1").arg(tempDir);
        qWarning().noquote() << error;
        emit audioError(error);
        return QString();
    }

    const QString outputPath = QDir(tempDir).filePath(baseName);
    QFile existing(outputPath);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data) {
        return outputPath;
    }
    existing.close();

    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)
            || output.write(data) != data.size()
            || !output.commit()) {
        const QString error = QStringLiteral("无法生成提示音临时文件：%1").arg(outputPath);
        qWarning().noquote() << error;
        emit audioError(error);
        return QString();
    }
    return outputPath;
}

/** @return 距同类提示上次播放达到 audioCooldownMs 时返回 true。 */
bool AudioService::cooldownAllowed(Prompt prompt) const
{
    const int cooldownMs = qMax(0, config_.audioCooldownMs);
    if (cooldownMs <= 0) {
        return true;
    }

    const int key = static_cast<int>(prompt);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastMs = lastPromptMs_.value(key, 0);
    if (lastMs > 0 && nowMs - lastMs < cooldownMs) {
        return false;
    }
    lastPromptMs_.insert(key, nowMs);
    return true;
}

/** @brief 校验文件并通过 GStreamer 启动播放。 */
bool AudioService::startGstreamer(const QString &path, bool ignoreCooldown, Prompt prompt)
{
    if (path.trimmed().isEmpty()) {
        emit audioError(QStringLiteral("音频文件路径为空"));
        return false;
    }
    if (!ignoreCooldown && !cooldownAllowed(prompt)) {
        return false;
    }

    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        const QString message = QStringLiteral("音频文件不存在：%1").arg(path);
        qWarning().noquote() << message;
        emit audioError(message);
        return false;
    }

    if (!player_) {
        emit audioError(QStringLiteral("GStreamer 提示音播放器不可用"));
        return false;
    }

    player_->stop();
    endVoiceFocus();
    player_->setVideoOutput(nullptr);
    player_->setVolume(1.0f);
    player_->setMedia(info.absoluteFilePath());
    if (!player_->isAvailable()) return false;
    pendingFileName_ = info.fileName();
    beginVoiceFocus();
    return true;
}

/** @brief 平滑压低正在播放的视频，并保存播报前的硬件音量。 */
void AudioService::beginVoiceFocus()
{
    voiceFocusRestoreTimer_.stop();
    if (voiceFocusActive_) {
        voiceFocusRestoreTimer_.start(15000);
        return;
    }
    voiceFocusActive_ = true;
    voiceFocusRestoreTimer_.start(15000);
    AudioPromptFocusManager::instance().acquire(this);
}

/** @brief 恢复播报前的视频音量与 RK809 原始音量。 */
void AudioService::endVoiceFocus()
{
    volumeBoostTimer_.stop();
    playbackStartTimer_.stop();
    voiceFocusRestoreTimer_.stop();
    if (!voiceFocusActive_) return;
    voiceFocusActive_ = false;
    volumeBoostPending_ = false;
    pendingFileName_.clear();
    AudioPromptFocusManager::instance().release(this);
}

/** @brief 硬件音量恢复后平滑淡入视频输入。 */
void AudioService::startVideoVolumeRestore(qreal startFactor)
{
    if (!mutedVideos_.isEmpty() && videoRestoreAnimation_) {
        videoRestoreAnimation_->stop();
        videoRestoreAnimation_->setStartValue(startFactor);
        videoRestoreAnimation_->setEndValue(1.0);
        videoRestoreAnimation_->start();
        return;
    }
    for (const MutedVideoState &state : mutedVideos_) {
        if (state.player) state.player->clearAudioDucking(this);
    }
    mutedVideos_.clear();
}

/** @brief 不依赖事件循环，立即恢复播报前保存的全部音量。 */
void AudioService::restoreVoiceFocusImmediately()
{
    if (videoDuckAnimation_) videoDuckAnimation_->stop();
    if (videoRestoreAnimation_) videoRestoreAnimation_->stop();
    volumeBoostTimer_.stop();
    playbackStartTimer_.stop();
    voiceFocusRestoreTimer_.stop();

    if (voiceFocusActive_) {
        AudioPromptFocusManager::instance().release(this);
    }
    for (const MutedVideoState &state : mutedVideos_) {
        if (state.player) state.player->clearAudioDucking(this);
    }

    mutedVideos_.clear();
    savedRawVolume_ = -1;
    voiceFocusActive_ = false;
    volumeBoostPending_ = false;
    pendingRestoreRaw_ = -1;
    pendingFileName_.clear();
}
