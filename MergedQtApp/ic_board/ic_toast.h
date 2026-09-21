/**
 * @file ic_toast.h
 * @brief IC 验证结果覆盖提示、边缘动画和语音焦点控制。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QVector>
#include <QHash>
#include <QPointer>
#include <QImage>
#include <QRegion>

class AmpVolumeController;
class GstPlayerWidget;
class QPainter;
class QVariantAnimation;

/**
 * @brief IC 刷卡、二维码结果的全屏覆盖提示单例。
 *
 * 成功时绘制绿色呼吸边框，失败时绘制红色边框和居中文本；可按配置播放提示音。
 * 播音期间临时静音正在播放的视频并提升 RK809 播放音量，播放结束或兜底超时后恢复。
 */
class IcToast : public QWidget {
    Q_OBJECT
public:
    /**
     * @brief 获取单例，并在宿主窗口变化时重新挂接父对象和尺寸事件。
     * @param mainWindow 覆盖层所属主窗口。
     * @return 进程内唯一 IcToast 实例。
     */
    static IcToast* instance(QWidget* mainWindow);

    /** @brief 显示成功主题的全屏绿色呼吸边框并播放通过提示音。 */
    void showPass(int ms = 1200);

    /** @brief 本地化失败原因，显示红色提示并播放失败提示音。 */
    void showFail(const QString& reason, int ms = 5000, int resultCode = 0);

    /** @brief 显示普通居中文本提示，并在指定毫秒数后隐藏。 */
    void showText(const QString& text, int ms = 5000);

protected:
    /** @brief 宿主窗口尺寸或状态变化时同步覆盖层几何位置。 */
    bool eventFilter(QObject* watched, QEvent* event) override;

    /** @brief 绘制边缘主题以及失败或普通文本框。 */
    void paintEvent(QPaintEvent* e) override;

private:
    /** @brief 初始化透明覆盖层、动画定时器和无视频输出的语音播放器。 */
    explicit IcToast(QWidget* mainWindow);

    /** @brief 根据当前字体、内边距和最大宽度计算文本框尺寸。 */
    void resizeToFitText();

    /** @brief 将文本框中心映射到主屏幕中心对应的宿主坐标。 */
    void moveBoxToScreenCenter();

    /** @brief 重启自动隐藏定时器。 */
    void armHide(int ms);

    /** @brief 启动固定周期和次数的边缘呼吸动画。 */
    void startPassEdgeFlash();

    /** @brief 按正弦缓动更新当前边缘亮度并请求重绘。 */
    void updatePassEdgeFlash();

    /** @brief 确保当前尺寸和主题的边缘光效纹理已经预渲染。 */
    void ensurePassEdgeTexture();

    /** @brief 将固定最大亮度的边缘渐变绘制进缓存纹理。 */
    void renderPassEdgeTexture(QPainter& p);

    /** @return 本次动画只需要刷新的四周边缘区域。 */
    QRegion passEdgeDirtyRegion() const;

    /** @brief 将 common/audio 中的 Qt 资源复制到 GStreamer 可访问的临时文件。 */
    QString materializeResourceToTemp(const QString& resourcePath);

    /** @brief 延迟创建功放控制器并恢复持久化音量。 */
    void ensureAmpReady();

    /** @brief 在配置允许时播放本地提示音，并建立临时语音焦点。 */
    void playVoiceFile(const QString& localFilePath);

    /** @brief 播放通过提示音。 */
    void playPassVoice();

    /** @brief 按 MQTT 结果码播放对应失败提示音，未配置时使用通用失败音。 */
    void playFailVoice(int resultCode);

    /** @return 提示音使用的 RK809 固定原始音量 230。 */
    int boostedToastRawVolume() const;

    /** @brief 保存并静音正在播放的视频，同时临时提升 RK809 播放音量。 */
    void beginVoiceFocus();

    /** @brief 恢复所有被压低的播放器和 RK809 播放音量。 */
    void endVoiceFocus();

    /** @brief 在硬件音量恢复完成后平滑恢复视频软件音量。 */
    void startVideoVolumeRestore(qreal startFactor);

    /** @return 同类提示距上次触发达到冷却时间时返回 true，并重启计时。 */
    bool allowTriggerNow(bool isFail);

    /** @return 配置项 DisplayDirRect/MusicPlayEn 是否启用提示音。 */
    bool isVoicePromptEnabled() const;

private:
    /** @brief 覆盖层四边光效主题。 */
    enum EdgeTheme {
        EdgeNone,
        EdgeSuccessGreen,
        EdgeFailRed
    };

    /** @brief 语音焦点期间被压低的播放器及其原始音量。 */
    struct DuckedPlayerState {
        QPointer<GstPlayerWidget> player;  /**< 正在播放且带视频画布的播放器。 */
        float volume = 0.0f;               /**< 压低前的归一化音量。 */
    };

    static IcToast* self_;            /**< 进程内单例指针。 */
    QWidget* main_ = nullptr;         /**< 当前覆盖的宿主窗口。 */
    QString text_;                    /**< 失败或普通提示文本。 */
    QTimer hideTimer_;                /**< 提示自动隐藏单次定时器。 */

    QTimer passAnimTimer_;            /**< 约 30 fps 的边缘动画定时器。 */
    QElapsedTimer passAnimClock_;     /**< 当前动画周期累计时间。 */
    qreal passFlashStrength_ = 0.0;   /**< 当前边缘亮度，范围 0.0~1.0。 */
    int passFlashTotalMs_ = 2000;     /**< 本次边缘动画总时长，单位 ms。 */
    int passPulsePeriodMs_ = 2000;    /**< 单次呼吸周期，单位 ms。 */
    int passPulseCount_ = 2;          /**< 一次提示包含的呼吸周期数。 */
    int passRingWidthPx_ = 40;        /**< 边缘实心光环目标宽度，单位 px。 */
    EdgeTheme edgeTheme_ = EdgeNone;  /**< 当前边缘颜色主题。 */
    QImage passEdgeTexture_;          /**< 当前尺寸和主题的最大亮度预渲染纹理。 */
    QSize passEdgeTextureSize_;       /**< 缓存纹理对应的控件尺寸。 */
    EdgeTheme passEdgeTextureTheme_ = EdgeNone; /**< 缓存纹理对应的颜色主题。 */

    GstPlayerWidget* voicePlayer_ = nullptr; /**< 专用无视频提示音播放器。 */
    QVariantAnimation* videoDuckAnimation_ = nullptr; /**< 播报前平滑压低视频软件音量。 */
    QVariantAnimation* videoRestoreAnimation_ = nullptr; /**< 播报后平滑恢复视频软件音量。 */
    QTimer voiceBoostTimer_;                  /**< 视频静音排空后再提升 RK809 音量。 */
    QTimer voiceStartTimer_;                  /**< RK809 音量稳定后再开始播放的短延时。 */
    QTimer voiceFocusRestoreTimer_;          /**< 15 秒异常兜底恢复语音焦点。 */
    QString pendingVoiceFile_;                /**< 等待短延时后播放的本地文件。 */
    QString passVoiceFile_;                  /**< common/audio/code_200.mp3 的临时播放路径。 */
    QString failVoiceFile_;                  /**< common/audio/fail.wav 的临时播放路径。 */
    AmpVolumeController* ampCtrl_ = nullptr; /**< 延迟创建的功放音量控制器。 */
    bool voiceFocusActive_ = false;          /**< 是否已保存并调整业务音量。 */
    bool volumeBoostPending_ = false;        /**< 是否等待异步应用提示音原始音量 230。 */
    int savedAmpRawVolume_ = -1;             /**< 语音焦点前 RK809 原始音量；-1 表示未保存。 */
    int pendingRestoreRaw_ = -1;             /**< 等待异步恢复的 RK809 原始音量。 */
    qreal pendingRestoreFactor_ = 0.0;        /**< 硬件恢复后视频淡入的起始比例。 */
    QVector<DuckedPlayerState> duckedPlayers_; /**< 本次被压低的业务播放器列表。 */

    QFont baseFont_;                /**< 创建时保存的应用基础字体。 */
    int maxWidth_ = 520;            /**< 文本布局最大宽度，单位 px。 */
    int padX_ = 100;                /**< 文本框水平内边距，单位 px。 */
    int padY_ = 50;                 /**< 文本框垂直内边距，单位 px。 */

    /** @brief 当前提示绘制类型。 */
    enum ToastKind {
        Neutral,
        PassEdgeFlash,
        Fail
    };
    ToastKind kind_ = Neutral;      /**< 当前提示类型。 */

    QRect boxRect_;                 /**< 仅失败和普通文本提示使用的绘制区域。 */

    QElapsedTimer passTriggerClock_; /**< 成功提示独立节流计时器。 */
    QElapsedTimer failTriggerClock_; /**< 失败提示独立节流计时器。 */
    int triggerCooldownMs_ = 2000;   /**< 同类提示最小触发间隔，单位 ms。 */

    /** @return 已知英文原因码的中文提示；未知内容原样保留。 */
    QString localizeFailReason(const QString& reason) const;

    /** @return 文本包含 CJK 基本汉字时返回 true。 */
    bool looksLikeChinese(const QString& text) const;
};
