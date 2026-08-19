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

class AmpVolumeController;
class GstPlayerWidget;
class QPainter;

/**
 * @brief IC 刷卡、二维码结果的全屏覆盖提示单例。
 *
 * 成功时绘制绿色呼吸边框，失败时绘制红色边框和居中文本；可按配置播放提示音。
 * 播音期间临时压低业务播放器音量并提升功放 DAC，播放结束或兜底超时后恢复。
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
    void showFail(const QString& reason, int ms = 5000);

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

    /** @brief 绘制当前成功绿色或失败红色的四边光晕。 */
    void paintPassEdgeFlash(QPainter& p);

    /** @brief 将 Qt 资源中的音频复制到 GStreamer 可访问的临时文件。 */
    QString materializeResourceToTemp(const QString& resourcePath);

    /** @brief 延迟创建功放控制器并恢复持久化音量。 */
    void ensureAmpReady();

    /** @brief 在配置允许时播放本地提示音，并建立临时语音焦点。 */
    void playVoiceFile(const QString& localFilePath);

    /** @brief 播放通过提示音。 */
    void playPassVoice();

    /** @brief 播放失败提示音。 */
    void playFailVoice();

    /** @return 提示音使用的临时 DAC 值，上限为 55。 */
    int boostedToastDac() const;

    /** @brief 保存并压低其他播放器音量，同时临时提升功放 DAC。 */
    void beginVoiceFocus();

    /** @brief 恢复所有被压低的播放器和功放 DAC。 */
    void endVoiceFocus();

    /** @return 同类提示距上次触发达到冷却时间时返回 true，并重启计时。 */
    bool allowTriggerNow(bool isFail);

    /** @return 配置项 DisplayDirRect/MusicPlayEn 是否启用提示音。 */
    bool isVoicePromptEnabled() const;

    /** @return 外置静态资源或 qrc 中首个存在的提示配置文件。 */
    QString resolveToastConfigPath() const;

    /** @brief 兼容 0/1、true/false、yes/no 和带分号注释的 INI 布尔值。 */
    static bool parseIniBool(const QVariant& v, bool def = false);

private:
    /** @brief 覆盖层四边光效主题。 */
    enum EdgeTheme {
        EdgeNone,
        EdgeSuccessGreen,
        EdgeFailRed
    };

    /** @brief 语音焦点期间被压低的播放器及其原始音量。 */
    struct DuckedPlayerState {
        GstPlayerWidget* player = nullptr; /**< 由主窗口对象树拥有的播放器。 */
        float volume = 0.0f;               /**< 压低前的归一化音量。 */
    };

    static IcToast* self_;            /**< 进程内单例指针。 */
    QWidget* main_ = nullptr;         /**< 当前覆盖的宿主窗口。 */
    QString text_;                    /**< 失败或普通提示文本。 */
    QTimer hideTimer_;                /**< 提示自动隐藏单次定时器。 */

    QTimer passAnimTimer_;            /**< 约 60 fps 的边缘动画定时器。 */
    QElapsedTimer passAnimClock_;     /**< 当前动画周期累计时间。 */
    qreal passFlashStrength_ = 0.0;   /**< 当前边缘亮度，范围 0.0~1.0。 */
    int passFlashTotalMs_ = 2000;     /**< 本次边缘动画总时长，单位 ms。 */
    int passPulsePeriodMs_ = 2000;    /**< 单次呼吸周期，单位 ms。 */
    int passPulseCount_ = 2;          /**< 一次提示包含的呼吸周期数。 */
    int passRingWidthPx_ = 40;        /**< 边缘实心光环目标宽度，单位 px。 */
    EdgeTheme edgeTheme_ = EdgeNone;  /**< 当前边缘颜色主题。 */

    GstPlayerWidget* voicePlayer_ = nullptr; /**< 专用无视频提示音播放器。 */
    QTimer voiceFocusRestoreTimer_;          /**< 2.2 秒兜底恢复语音焦点。 */
    QString passVoiceFile_;                  /**< 临时目录中的通过提示音。 */
    QString failVoiceFile_;                  /**< 临时目录中的失败提示音。 */
    AmpVolumeController* ampCtrl_ = nullptr; /**< 延迟创建的功放音量控制器。 */
    bool voiceFocusActive_ = false;          /**< 是否已保存并调整业务音量。 */
    int savedAmpDac_ = -1;                   /**< 语音焦点前的原始 DAC；-1 表示未保存。 */
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
