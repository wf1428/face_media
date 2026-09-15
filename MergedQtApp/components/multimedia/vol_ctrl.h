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
#include <QTimer>
#include <QStringList>

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

signals:
    /** @brief 音量实际应用到音频路径后发出。 */
    void volumeApplied(int percent);

public slots:
    /** @brief 将用户音量限制到 0~100，并防抖应用和保存。 */
    void setVolume(int value);

    /** @brief 立即设置 0~200 RK809 原始值，用于提示音等临时增益。 */
    void setRawVolumeTemporary(int rawValue);

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
};

#endif // VOL_CTRL_H
