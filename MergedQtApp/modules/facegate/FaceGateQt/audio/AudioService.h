/**
 * @file AudioService.h
 * @brief 使用外部 aplay 进程播放门禁语音提示。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <QObject>
#include <QHash>

#include "AppConfig.h"

/**
 * @brief 使用外部 aplay 进程播放门禁语音提示。
 *
 * 负责提示开关、文件解析、同类提示冷却和系统混音器音量设置。
 * 播放使用 startDetached，不持有 aplay 进程句柄，因此不同提示可能并行播放。
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
        EnrollSuccess,
        EnrollFailed
    };

    /** @brief 创建尚未配置的音频服务。 */
    explicit AudioService(QObject *parent = nullptr);

    /** @brief 替换完整运行配置，并应用音量设置。 */
    void configure(const AppConfig &config);

    /** @brief 启用或关闭语音提示总开关。 */
    void setEnabled(bool enabled);

    /** @brief 设置 0~100 的逻辑音量并尝试同步系统混音器。 */
    void setVolume(int volume);

    /** @brief 在提示类型启用且冷却允许时播放对应 WAV。 */
    void playPrompt(Prompt prompt);

    /** @brief 按配置键测试播放指定提示，忽略业务冷却。 */
    void testPlay(const QString &promptKey);

    /** @brief 兼容预留接口；当前 detached 播放模式下不执行停止操作。 */
    void stopCurrentPlayback();

    /** @return Prompt 对应的稳定配置键。 */
    static QString keyForPrompt(Prompt prompt);

signals:
    /** @brief 上报播放开始、结束或配置状态。 */
    void audioStatus(const QString &message);

    /** @brief 上报文件、进程或音量设置错误。 */
    void audioError(const QString &message);

private:
    /** @return 指定提示的独立开关是否启用。 */
    bool promptEnabled(Prompt prompt) const;

    /** @return 指定提示配置的文件名。 */
    QString promptFile(Prompt prompt) const;

    /** @return 相对 audioDir 解析后的音频绝对路径。 */
    QString absoluteAudioPath(const QString &fileName) const;

    /** @return 距同类提示上次播放达到 audioCooldownMs 时返回 true。 */
    bool cooldownAllowed(Prompt prompt) const;

    /** @brief 校验文件并启动 aplay 进程。 */
    bool startAplay(const QString &path, bool ignoreCooldown, Prompt prompt);

    /** @brief 使用系统混音器工具设置配置的播放控制项。 */
    bool applySystemVolume(int volume);

private:
    AppConfig config_;                       /**< 音频开关、设备、目录和提示文件配置。 */
    mutable QHash<int, qint64> lastPromptMs_; /**< 各提示类型最近播放时间，单位 ms。 */
};

#endif
