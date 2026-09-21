/**
 * @file vol_ctrl.h
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef VOL_CTRL_H
#define VOL_CTRL_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QStringList>
#include <QVector>

class QProcess;
class GstPlayerWidget;

/**
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器。
 *
 * 用户音量使用 0~100，并线性映射到 RK809 Playback Volume 的 60~200；
 * 普通 setVolume() 经防抖后持久化，临时硬件调整不改变保存的用户音量。
 */
class AmpVolumeController : public QObject
{
    Q_OBJECT
public:
    /** @brief 解析平台路径并初始化音量防抖定时器。 */
    explicit AmpVolumeController(QObject *parent = nullptr);

    /** @return 从持久化文件读取的 0~100 音量；文件不存在时创建为 50%。 */
    int loadVolumeFromFile();

    /** @return 持久化文件记录的功放启用标志。 */
    bool loadAmpFlagFromFile();

    /** @return 当前保存的用户音量，范围 0~100。 */
    int currentVolume() const { return m_volume; }

    /** @return 当前用户音量映射的 60~200 RK809 原始值，不反映临时覆盖。 */
    int currentRawVolume() const { return volumePercentToRaw(m_volume); }

    /** @return DisplayDirRect/MusicPlayEn；缺少配置时按启用处理。 */
    static bool isPromptPlaybackEnabled();

    /** @brief 将统一播报开关写入 DisplayDirRect/MusicPlayEn。 */
    static bool setPromptPlaybackEnabled(bool enabled);

signals:
    /** @brief 音量实际应用到音频路径后发出。 */
    void volumeApplied(int percent);

    /** @brief 异步临时原始音量请求完成；success 表示 amixer 已正常退出。 */
    void rawVolumeTemporaryApplied(int rawValue, bool success);

public slots:
    /** @brief 将用户音量限制到 0~100，并防抖应用和保存。 */
    void setVolume(int value);

    /** @brief 立即设置 0~255 RK809 原始值，用于提示音等临时增益。 */
    void setRawVolumeTemporary(int rawValue);

    /** @brief 异步设置临时原始音量，不阻塞 Qt 界面线程。 */
    void setRawVolumeTemporaryAsync(int rawValue);

private:
    /** @return 0~100 线性映射并限制到 60~200 的 RK809 原始值。 */
    static int volumePercentToRaw(int value);

    /** @brief 打开功放控制设备。 */
    bool amplifierOpen();

    /** @brief 关闭功放控制设备。 */
    bool amplifierClose();

    /** @brief 使用 numid=3 设置 RK809 左右声道原始播放音量。 */
    bool setPlaybackVolumeRaw(int rawValue);

    /** @brief 在配置声卡上执行一次 amixer 命令。 */
    bool runAmixer(const QStringList &arguments, QString *output = nullptr) const;

    /** @brief 启动队列中最新一次异步原始音量请求。 */
    void startPendingRawVolumeRequest();

    /** @brief 完成当前异步原始音量请求并继续处理最新等待值。 */
    void finishRawVolumeRequest(bool success, const QString &detail = QString());

    /** @brief 按音量决定功放开关和 RK809 播放音量，并发出结果信号。 */
    void applyNow(int value);

    /** @brief 将用户音量和功放标志写入持久化文件。 */
    void writeVolumeFile(int volume, bool ampFlag);

    /** @brief 首次使用时初始化功放和 ALSA 音频路径。 */
    void initAudioPathOnce();

private:
    QString dataFile_;
    QString ampDevice_;
    QString alsaCard_;
    int m_volume = 50;
    bool m_ampFlag = true;
    QTimer m_debounce;
    int m_pendingVolume = -1;
    QProcess *rawVolumeProcess_ = nullptr;
    QTimer rawVolumeTimeout_;          /**< 异步 amixer 超时保护。 */
    int pendingRawVolume_ = -1;
    int activeRawVolume_ = -1;
    bool rawVolumeRequestActive_ = false;
};

/**
 * @brief 统一管理人脸、刷卡和二维码播报期间的视频静音及 RK809 音量。
 *
 * 多个播报重叠时只执行一次压低和硬件增益；最后一个播报结束后才恢复，
 * 避免各入口分别保存/恢复音量造成永久静音或相互覆盖。
 */
class AudioPromptFocusManager final : public QObject
{
    Q_OBJECT
public:
    static AudioPromptFocusManager &instance();

    /** @brief 获取播报焦点；硬件音量稳定后发出 focusReady。 */
    void acquire(QObject *owner);
    /** @brief 释放播报焦点；最后一个持有者释放后恢复视频和用户音量。 */
    void release(QObject *owner);

signals:
    /** @brief 指定持有者可以开始播放提示音。 */
    void focusReady(QObject *owner);

private:
    explicit AudioPromptFocusManager(QObject *parent = nullptr);
    ~AudioPromptFocusManager() override;
    AudioPromptFocusManager(const AudioPromptFocusManager &) = delete;
    AudioPromptFocusManager &operator=(const AudioPromptFocusManager &) = delete;

    bool hasDuckedPlayer(GstPlayerWidget *player) const;
    void duckNewActiveVideoPlayers();
    void restoreVideoPlayers();
    void requestBoost();

    QSet<QObject *> owners_;
    QSet<QObject *> pendingReadyOwners_;
    QVector<QPointer<GstPlayerWidget>> duckedPlayers_;
    AmpVolumeController *volumeController_ = nullptr;
    QTimer boostDelayTimer_;
    int savedAmpRawVolume_ = -1;
    bool boostReady_ = false;
};

#endif // VOL_CTRL_H
