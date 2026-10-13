/**
 * @file AudioService.h
 * @brief 使用进程内 GStreamer 播放门禁语音提示。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include "AppConfig.h"

class AmpVolumeController;
class GstPlayerWidget;
class QVariantAnimation;

/**
 * @brief 使用进程内 GStreamer 播放门禁语音提示。
 *
 * 负责提示开关、文件解析、同类提示冷却和统一音量持久化；播放通过
 * GstPlayerWidget 进入进程内共享音频输出，避免与视频争抢 ALSA 设备。
 */
class AudioService : public QObject {
    Q_OBJECT

public:
    /** @brief 门禁业务提示类型。 */
    enum class Prompt {
        VerifyPassed,
        VerifyFailed,
        Stranger,
        LivenessFailed,
        FaceAccessTimeout,
        FaceLivenessUnavailable,
        EnrollSuccess,
        EnrollFailed
    };

    /** @brief 创建尚未配置的音频服务。 */
    explicit AudioService(QObject *parent = nullptr);
    /** @brief 停止并释放专用 GStreamer 提示音播放器。 */
    ~AudioService() override;

    /** @brief 替换完整运行配置，并应用音量设置。 */
    void configure(const AppConfig &config);

    /** @brief 启用或关闭语音提示总开关。 */
    void setEnabled(bool enabled);

    /** @brief 设置并持久化 0~100 语音播报音量，播放时映射到 RK809 200~250。 */
    void setVolume(int volume);

    /** @return volume.dat 中当前语音播报音量百分比。 */
    int volume() const;

    /** @brief 在提示类型启用且冷却允许时播放对应音频。 */
    void playPrompt(Prompt prompt);

    /** @brief 按统一通行结果码播放对应失败音，未知结果使用 fail.wav。 */
    void playAccessFailure(int resultCode);

    /** @brief 按配置键测试播放指定提示，忽略业务冷却。 */
    void testPlay(const QString &promptKey);

    /** @brief 停止当前 GStreamer 提示音。 */
    void stopCurrentPlayback();

    /** @return Prompt 对应的稳定配置键。 */
    static QString keyForPrompt(Prompt prompt);

signals:
    /** @brief 上报播放开始、结束或配置状态。 */
    void audioStatus(const QString &message);

    /** @brief 上报文件、GStreamer 或音量设置错误。 */
    void audioError(const QString &message);

private:
    /** @brief 保存并静音正在播放的视频，同时应用独立的语音播报音量。 */
    void beginVoiceFocus();

    /** @brief 恢复提示音开始前的视频音量和硬件音量。 */
    void endVoiceFocus();

    /** @brief 停止过渡动画并立即恢复全部音量，用于对象销毁等不可等待场景。 */
    void restoreVoiceFocusImmediately();

    /** @brief 在硬件原始音量恢复完成后平滑恢复视频软件音量。 */
    void startVideoVolumeRestore(qreal startFactor);

    /** @return 指定提示的独立开关是否启用。 */
    bool promptEnabled(Prompt prompt) const;

    /** @return 指定提示配置的文件名。 */
    QString promptFile(Prompt prompt) const;

    /** @return common/audio 资源物化后的临时文件绝对路径。 */
    QString absoluteAudioPath(const QString &fileName);

    /** @return 距同类提示上次播放达到 audioCooldownMs 时返回 true。 */
    bool cooldownAllowed(Prompt prompt) const;

    /** @brief 校验文件并通过 GStreamer 启动播放。 */
    bool startGstreamer(const QString &path, bool ignoreCooldown, Prompt prompt);

private:
    struct MutedVideoState {
        QPointer<GstPlayerWidget> player;
        float volume = 0.0f;
    };

    AppConfig config_;                       /**< 音频开关、设备、目录和提示文件配置。 */
    mutable QHash<int, qint64> lastPromptMs_; /**< 各提示类型最近播放时间，单位 ms。 */
    GstPlayerWidget *player_ = nullptr;       /**< 无视频输出的 GStreamer 提示音播放器。 */
    AmpVolumeController *volumeController_ = nullptr; /**< RK809 音量和持久化控制器。 */
    QVariantAnimation *videoDuckAnimation_ = nullptr; /**< 播报前平滑压低视频软件音量。 */
    QVariantAnimation *videoRestoreAnimation_ = nullptr; /**< 播报后平滑恢复视频软件音量。 */
    QTimer volumeBoostTimer_;                 /**< 视频静音排空后再提升 RK809 音量。 */
    QTimer playbackStartTimer_;               /**< RK809 音量稳定后再开始播放的短延时。 */
    QTimer voiceFocusRestoreTimer_;           /**< 播放异常时恢复音量的兜底定时器。 */
    QVector<MutedVideoState> mutedVideos_;    /**< 本次播报临时压低的视频播放器。 */
    int savedRawVolume_ = -1;                 /**< 播报前的 RK809 原始音量。 */
    bool voiceFocusActive_ = false;           /**< 当前是否持有语音焦点。 */
    bool volumeBoostPending_ = false;         /**< 是否等待异步应用提示音原始音量。 */
    int pendingRestoreRaw_ = -1;              /**< 等待异步恢复的 RK809 原始音量。 */
    qreal pendingRestoreFactor_ = 0.0;        /**< 硬件恢复后视频淡入的起始比例。 */
    QString pendingFileName_;                 /**< 延时启动后用于状态日志的文件名。 */
};

#endif
