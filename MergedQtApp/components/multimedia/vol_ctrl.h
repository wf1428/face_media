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

#include "volumepopup.h"

/**
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器。
 *
 * 用户音量使用 0~100，硬件 DAC 使用 0~63；普通 setVolume() 经防抖后持久化，
 * setRawDacTemporary() 只做临时硬件调整，不改变保存的用户音量。
 */
class AmpVolumeController : public QObject
{
    Q_OBJECT
public:
    /** @brief 解析平台路径并初始化音量防抖定时器。 */
    explicit AmpVolumeController(QObject *parent = nullptr);

    /** @return 从持久化文件加载并应用的 0~100 音量。 */
    int loadVolumeFromFile();

    /** @return 持久化文件记录的功放启用标志。 */
    bool loadAmpFlagFromFile();

    /** @return 当前保存的用户音量，范围 0~100。 */
    int currentVolume() const { return m_volume; }

    /** @return 当前用户音量映射的 0~63 DAC 值，不反映临时 DAC 覆盖。 */
    int currentRawDac() const { return vol100ToDac63(m_volume); }

signals:
    /** @brief 音量实际应用到音频路径后发出。 */
    void volumeApplied(int percent);

public slots:
    /** @brief 将用户音量限制到 0~100，并防抖应用和保存。 */
    void setVolume(int value);

    /** @brief 立即设置 0~63 DAC，用于提示音等临时增益。 */
    void setRawDacTemporary(int dac);

private:
    /** @return 0~100 线性映射并限制到 0~63 的 DAC 值。 */
    static int vol100ToDac63(int value);

    /** @return 0~63 线性映射并限制到 0~100 的用户音量。 */
    static int dac63ToVol100(int dac);

    /** @brief 打开功放控制设备。 */
    bool amplifierOpen();

    /** @brief 关闭功放控制设备。 */
    bool amplifierClose();

    /** @brief 设置 ALSA 混音器百分比和静音状态。 */
    bool setMixerPercent(int percent, bool unmute);

    /** @return 配置或自动检测到的混音器控制项。 */
    QString detectMixerControl();

    /** @brief 在配置声卡上执行一次 amixer 命令。 */
    bool runAmixer(const QStringList &arguments, QString *output = nullptr) const;

    /** @brief 按音量决定功放开关、DAC 和混音器，并发出结果信号。 */
    void applyNow(int value);

    /** @brief 将用户音量和功放标志写入持久化文件。 */
    void writeVolumeFile(int volume, bool ampFlag);

    /** @brief 首次使用时初始化功放和 ALSA 音频路径。 */
    void initAudioPathOnce();

private:
    QString dataFile_;
    QString ampDevice_;
    QString alsaCard_;
    QString mixerControl_;

    int m_volume = 50;
    bool m_ampFlag = true;
    QTimer m_debounce;
    int m_pendingVolume = -1;
};

#endif // VOL_CTRL_H
