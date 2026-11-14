/**
 * @file vol_ctrl.h
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-12；修改人：Du lin；广播文件音频持有焦点期间丢弃系统音量请求，并让播报增益等待旧异步写入退出，结束后恢复原用户音量。
 */

#ifndef VOL_CTRL_H
#define VOL_CTRL_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

class QProcess;
class SerialWorkQueue;
class GstPlayerWidget;

/**
 * @brief RK3566 功放开关、ALSA 混音器和音量持久化控制器。
 *
 * 系统音量 0% 为 raw0 静音，1~100% 线性映射到 RK809 Playback Volume 的 100~230；
 * 语音播报音量独立使用 0~100，并线性映射到原始值 200~250。
 * 两个百分比共同保存在 volume.dat 中，临时硬件调整不改变持久化值。
 * 所有接口在 UI 线程调用；通话持有共享硬件控制权时，其他实例只保存逻辑音量；
 * 广播文件音频持有焦点期间，外部系统音量请求直接丢弃。已启动的旧异步命令必须退出后，
 * 通话或播报增益才可就绪。
 */
class AmpVolumeController : public QObject
{
    Q_OBJECT
public:
    /** @brief 初始化音量队列和路径；非通话持有者在保护期间跳过硬件初始化。
     * @param[in] parent UI 线程所有者，负责释放本控制器及其进程/定时器。
     */
    explicit AmpVolumeController(QObject *parent = nullptr);

    /** @brief 读取系统/播报配置，并按当前系统百分比归一化功放标志。
     * @return 系统百分比，0~100；读不到文件时默认50%，播报默认60%（raw230）。
     * @note 系统0%标记为关闭，非零为开启；旧标志不一致时沿用原写入路径更新。
     */
    int loadVolumeFromFile();

    /** @return 当前系统0%为false、非零为true的逻辑标志，不反映通话/播报临时功放状态。 */
    bool loadAmpFlagFromFile();

    /** @return 当前保存的用户音量，范围 0~100。 */
    int currentVolume() const { return m_volume; }

    /** @return 用户 0% 对应 raw0；1~100% 对应 raw100~230，不反映通话/播报临时覆盖。 */
    int currentRawVolume() const { return volumePercentToRaw(m_volume); }

    /** @return 当前保存的语音播报音量，范围 0~100。 */
    int currentPromptVolume() const { return m_promptVolume; }

    /** @return 语音播报百分比映射的 200~250 RK809 原始值。 */
    int currentPromptRawVolume() const { return promptVolumePercentToRaw(m_promptVolume); }

    /** @return DisplayDirRect/MusicPlayEn；缺少配置时按启用处理。 */
    static bool isPromptPlaybackEnabled();

    /** @brief 将统一播报开关写入 DisplayDirRect/MusicPlayEn。 */
    static bool setPromptPlaybackEnabled(bool enabled);

signals:
    /** @brief 后台音量保存尝试结束，通知最新代次的原回执/UI链路；保存失败记录日志，沿用原完成行为。
     * @param[in] percent 0~100 的逻辑系统音量，不代表通话临时硬件增益。
     */
    void volumeApplied(int percent);

    /** @brief 当前硬件代次的异步请求结束；被通话取代的旧请求不再发出此通知。
     * @param[in] rawValue 本次提交的 RK809 原始值，0~255。
     * @param[in] success true 表示 amixer 正常退出；false 表示启动/执行失败。
     */
    void rawVolumeTemporaryApplied(int rawValue, bool success);

public slots:
    /** @brief 更新 0~100 用户音量，重复值不再写硬件；播报焦点期间直接丢弃。
     * @param[in] value 用户系统音量百分比，限制到 0~100。
     * @note 播报期间不更新缓存、文件或硬件，也不在结束后补写；通话期间仍更新恢复值。
     */
    void setVolume(int value);

    /** @return true 表示本控制器的用户音量请求会被当前播报焦点丢弃；仅 UI 线程调用。 */
    bool userVolumeBlocked() const;

    /** @brief 保存 0~100 语音播报音量；不立即改变正常媒体硬件音量。 */
    void setPromptVolume(int value);

    /** @brief 同步设置 0~255 原始值；通话保护期间非持有者请求被丢弃，不回放。
     * @param[in] rawValue 临时增益，限制到 0~255；可能等待外部命令，仅用于原同步兜底。
     */
    void setRawVolumeTemporary(int rawValue);

    /** @brief 排队异步临时增益；通话或广播文件音频保护期间非持有者请求不操作硬件。
     * @param[in] rawValue 限制到 0~255；旧硬件代次的等待请求自动丢弃。
     * @note 焦点持有者须等其他实例已经启动的 amixer 退出后才启动增益命令。
     */
    void setRawVolumeTemporaryAsync(int rawValue);

private:
    friend class AudioPromptFocusManager;
    /** @brief 获取/更新本实例的通话硬件控制权，废弃旧代次，保留已有逻辑音量修改。 */
    void claimCallOutput();
    /** @brief 读取通话结束应恢复的最新系统原始音量，含尚在防抖等待的用户修改。
     * @return 0% 为 raw0，非零百分比为 raw100~230。
     */
    int callRestoreRawVolume();
    /** @brief 恢复完成后放开本实例持有的硬件控制权，并使旧等待请求失效。 */
    void releaseCallOutput();
    /** @brief 占有广播文件音频的硬件写入权；非通话场景废弃此前排队的外部增益请求。 */
    void claimPromptOutput();
    /** @brief 用户音量恢复完成后释放广播文件音频写入权；不重放期间丢弃的请求。 */
    void releasePromptOutput();
    /** @return true 表示另一实例正保护通话或广播文件音频硬件，本实例不能修改音量/功放。 */
    bool callBlocksHardware() const;
    /** @brief 结束一个异步硬件写者并唤醒等待中的通话或广播持有者；仅 UI 线程调用。
     * @param[in] process 已退出或已析构的进程指针，仅作为集合键，不解引用。
     */
    static void completeHardwareWrite(QProcess *process);

    /** @brief 映射视频/系统百分比；0% 静音，非零值以 1%→100、100%→230 线性递增。
     * @param[in] value 系统百分比，先限制到 0~100。
     * @return 0 或四舍五入后的 raw100~230。
     */
    static int volumePercentToRaw(int value);

    /** @return 0~100 线性映射并限制到 200~250 的 RK809 原始值。 */
    static int promptVolumePercentToRaw(int value);

    /** @brief 打开功放；其他实例持有通话控制权时保持当前状态。
     * @return true 为成功或由通话保护保持现状；false 为设备打开/写入失败。
     */
    bool amplifierOpen();

    /** @brief 关闭功放；其他实例持有通话控制权时不执行关闭。
     * @return true 为成功或由通话保护保持现状；false 为设备打开/写入失败。
     */
    bool amplifierClose();

    /** @brief 使用 numid=3 设置左右声道；非持有者不能覆盖通话增益。
     * @param[in] rawValue 原始值，限制到 0~255。
     * @return true 为命令成功；false 为通话保护禁止或命令失败。
     */
    bool setPlaybackVolumeRaw(int rawValue);

    /** @brief 在配置声卡上执行一次 amixer 命令。 */
    bool runAmixer(const QStringList &arguments, QString *output = nullptr) const;

    /** @brief 启动队列中最新一次异步原始音量请求。 */
    void startPendingRawVolumeRequest();

    /** @brief 完成当前异步原始音量请求并继续处理最新等待值。 */
    void finishRawVolumeRequest(bool success, const QString &detail = QString());

    /** @brief 保存用户音量并通知原回执；无通话保护时才提交硬件请求。
     * @param[in] value 限制到 0~100 的逻辑系统百分比。
     */
    void applyNow(int value);

    quint64 volumePersistenceRevision_=0; /**< UI递增的系统音量保存代次；旧完成不通知最新请求。 */
    /** @brief 后台串行读改写系统音量/功放标志，保留文件播报值；最新完成回UI通知。
     * @param[in] volume 系统音量，限制到0～100。
     * @param[in] ampFlag 原逻辑功放标志；不表示当前通话硬件开关。
     */
    void writeVolumeFile(int volume, bool ampFlag);

    /** @brief UI更新播报缓存，后台串行读改写文件，保留系统音量/功放标志。
     * @param[in] promptVolume 播报百分比，限制到0～100。
     */
    void writePromptVolumeFile(int promptVolume);

    /** @brief 首次初始化路由和功放；通话保护期间新建的非持有者跳过硬件操作。 */
    void initAudioPathOnce();

private:
    QString dataFile_;
    QString ampDevice_;
    QString alsaCard_;
    int m_volume = 50;
    int m_promptVolume = 60;       /**< 60% 对应旧版固定原始播报音量 230。 */
    bool m_ampFlag = true;
    QTimer m_debounce;
    int m_pendingVolume = -1;
    QProcess *rawVolumeProcess_ = nullptr;
    QTimer rawVolumeTimeout_;          /**< 异步 amixer 超时保护。 */
    int pendingRawVolume_ = -1;
    int activeRawVolume_ = -1;
    bool rawVolumeRequestActive_ = false;
    quint64 pendingRawGeneration_ = 0; /**< 等待请求所属硬件代次，UI 线程读写。 */
    quint64 activeRawGeneration_ = 0; /**< 已启动请求代次；旧代次完成不关闭功放或回调就绪。 */
};

/**
 * @brief 统一管理通话与人脸、刷卡、二维码提示的静音所有权和 RK809 临时音量。
 *
 * 每次仅授予一个播放者，同级保留先到者。高优先级挂起低优先级，
 * 释放后恢复仍在申请的最高级；全部结束后恢复视频与用户音量。
 * 有通话或广播文件音频时跨控制器保护硬件，等待旧写者退出；最后一个持有者释放后，
 * 按原恢复规则异步恢复，再解除保护。所有方法与回调都在 UI 线程串行执行。
 */
class AudioPromptFocusManager final : public QObject
{
    Q_OBJECT
public:
    static AudioPromptFocusManager &instance();

    /** @brief 焦点优先级由低至高，视频为无持有者时的默认输出。 */
    enum class Priority {
        Prompt, /**< 人脸、刷卡及普通播报。 */
        Tagged, /**< 平台标识符播报。 */
        Schedule, /**< 精细化定时广播。 */
        Live, /**< 单向实时直播。 */
        Call /**< 双向通话和其铃声。 */
    };
    /** @brief 申请焦点；获准并完成增益准备后发出 focusReady。
     * @param[in] owner 非空主线程业务持有者，析构自动归还。
     * @param[in] player 借用的输出分支身份，寿命应覆盖申请。
     * @param[in] priority 音频来源优先级；通话用 acquireCall 申请。
     */
    void acquire(QObject *owner, GstPlayerWidget *player, Priority priority = Priority::Prompt);
    /** @return 通话是否持有仲裁焦点，包括铃声和释放声卡之前。 */
    bool callOwnsFocus() const;
    /** @brief 检查当前输出是否高于候选优先级。
     * @param[in] priority 候选请求级别。
     * @return true 表示候选不能立即播放。
     */
    bool blocks(Priority priority) const;
    /** @brief 获取通话焦点并保护全进程 RK809/功放；等待旧异步命令退出后才就绪。
     * @param[in] owner 非空 UI 对象，通常为 CallManager；销毁时由原释放路径归还。
     * @note 保持到全部重叠焦点结束且最新系统音量恢复完成，不阻止视频切源/回执。
     */
    void acquireCall(QObject *owner);
    /** @brief 释放焦点；最后一个持有者退出后异步恢复系统音量并解除通话保护。
     * @param[in] owner 之前申请的持有者；重复或空指针释放忽略。
     * @note 通话保护期间使用最新用户音量；普通提示仍沿用原进入快照恢复规则。
     */
    void release(QObject *owner);

signals:
    /** @brief 指定持有者的当前硬件准备请求已收口，可以继续提示/通话准备。
     * @param[in] owner 仍持有焦点的 UI 对象；失败场景沿用日志并继续的原策略。
     */
    void focusReady(QObject *owner);
    /** @brief 更高优先级抢占后通知原持有者暂停。
     * @param[in] owner 被挂起的主线程业务对象，申请仍保留。
     */
    void focusSuspended(QObject *owner);
    /** @brief 通话焦点变更时同步通知，普通音源切换不发送。
     * @param[in] active true 通话占用焦点，false 通话释放焦点（含暂停时释放）。
     */
    void callFocusChanged(bool active);

private:
    /** @brief 在应用初始化阶段创建硬件控制器并绑定增益/恢复完成回调。
     * @param[in] parent 可空 UI 线程所有者；单例按原静态生命周期析构。
     */
    explicit AudioPromptFocusManager(QObject *parent = nullptr);
    /** @brief 退出阶段解除保护，沿用原同步兜底恢复并销毁控制器；可能等待进程。 */
    ~AudioPromptFocusManager() override;
    AudioPromptFocusManager(const AudioPromptFocusManager &) = delete;
    AudioPromptFocusManager &operator=(const AudioPromptFocusManager &) = delete;

    bool hasDuckedPlayer(GstPlayerWidget *player) const;
    void duckNewActiveVideoPlayers();
    void restoreVideoPlayers();
    /** @brief 非通话播报后台清除旧 PCM；确认清空后即使共享输出恢复失败，也先写本轮增益再授予文件播报焦点。
     * @note 清空未确认时沿用当前音量；此处不修复共享输出重开失败或保证 PCM 已送达声卡。
     */
    void beginPromptHandoff();
    /** @brief 使旧后台交接结果失效，不阻塞主线程。 */
    void cancelPromptHandoff();
    void requestBoost();

    QSet<QObject *> owners_;
    /** @brief 选择当前唯一输出，发送暂停/恢复通知并保留硬件恢复快照。 */
    void selectOwner();
    /** @brief 本次请求的播放器身份及优先级，均由 UI 线程访问。 */
    struct Request {
        QPointer<GstPlayerWidget> player; /**< 借用播放器，销毁后失效。 */
        Priority priority; /**< 决定抢占顺序。 */
        QMetaObject::Connection destroyed; /**< 请求释放时解除本次生命周期连接。 */
    };
    QHash<QObject *, Request> requests_; /**< 活跃及挂起的有序请求索引。 */
    QList<QObject *> order_; /**< 先到先服务顺序，释放时移除。 */
    QObject *activeOwner_ = nullptr; /**< 当前拥有输出的请求键，不解引用。 */
    QVector<QPointer<GstPlayerWidget>> duckedPlayers_;
    AmpVolumeController *volumeController_ = nullptr;
    QTimer boostDelayTimer_; /**< 通话和独立直播沿用的硬件准备延时。 */
    std::unique_ptr<SerialWorkQueue> promptHandoff_; /**< 按需创建的普通播报音频交接队列。 */
    std::atomic<quint64> promptHandoffGeneration_{0}; /**< 主线程推进，后台检查是否取消。 */
    bool promptHandoffPending_ = false; /**< 同一批普通播报共用一个清除操作。 */
    bool requirePromptRestore_ = false; /**< 普通播报或直播要求确认功放恢复；失败时保持视频静音。 */
    bool restoreRetried_ = false; /**< 本次普通播报功放恢复至多额外重试一次。 */
    int savedAmpRawVolume_ = -1;
    int requestedBoostRaw_ = -1;   /**< 本轮根据播报百分比映射出的 200~250 原始值。 */
    bool boostReady_ = false;
    bool callOutputHeld_ = false; /**< true 时保护通话/重叠焦点及异步恢复阶段的硬件，不允许媒体实例覆盖。 */
};

#endif // VOL_CTRL_H
