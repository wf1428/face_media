/**
 * @file broadcast_service.h
 * @brief 处理6.15文件广播、排程与单向直播控制。
 * @author Du lin
 * @note 最近修改：2026-11-12；修改人：Du lin；为文件未缓存告警增加按计划和音频去重的状态，避免定时重试期间重复刷屏；播放接口与排程规则不变。
 *       说明下行 FTP 主机的字段、优先级和非法主机的下载失败行为。
 * @date 2026-11-05
 */
#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

class BroadcastLive;
class QDateTime;
class GstPlayerWidget;
class QProcess;
class SerialWorkQueue;

/** @brief 独立广播业务，使用已有 MQTT 和共享音频焦点。
 * @details 路由、排程和回执在主线程串行执行。FTP进程异步，文件哈希与清单写入
 *          由串行后台完成。直播媒体进入独立缓冲，不参与业务消息队列。
 */
class BroadcastService final : public QObject {
public:
    using Publisher = std::function<bool(const QJsonObject &)>; /**< 发布本机event信封。 */
    /** @brief 创建广播控制器与独立直播输入。
     * @param[in] publisher 主线程无阻塞发布回调。
     * @param[in] parent 可空的主线程所有者。
     */
    BroadcastService(Publisher publisher, QObject *parent);
    /** @brief 释放音频焦点并在最终退出等待后台任务。 */
    ~BroadcastService() override;
    /** @brief 更新路由和下载基址，设备变更时加载该设备的持久化计划。
     * @param[in] device 精确ycLinux设备ID，空值禁用。
     * @param[in] ftpHost 下行未提供 FTP 主机时使用的设备默认主机，可为空。
     */
    void configure(const QString &device, const QString &ftpHost);
    /** @brief 切换联网状态，上线主动上报，离线终止实时直播。
     * @param[in] online 当前 MQTT 在线状态。
     */
    void setOnline(bool online);
    /** @brief 更新本地定时播放授权；关闭时停止当前计划并释放焦点。
     * @param[in] enabled true 启用排程及计划控制；false 忽略计划控制。
     */
    void setScheduledEnabled(bool enabled);
    /** @brief 识别本服务的广播方法，无需实例或业务数据。
     * @param[in] method 下行方法名。
     * @return true 为广播文件、排程、点播或直播控制。
     */
    static bool handlesMethod(const QString &method);
    /** @brief 处理设备精确request主题上的广播指令。
     * @param[in] topic 实际消息主题。
     * @param[in] payload 已解析的协议正文。broadcastSet 的 data.ftp.host 与 broadcastAudioSync ADD
     *                    的 data.ftpHost 仅用于本次下载任务。
     * @return true 广播方法已处理或校验拒绝。
     */
    bool handle(const QString &topic, const QJsonObject &payload);
    /** @return 线程安全媒体入口，可在IPC线程直接调用。 */
    std::function<void(const QJsonObject &)> mediaReceiver() const;

private:
    /** @brief 已校验的远端文件描述与下载来源。 */
    struct Download {
        QString request, id, md5, path, user, password; /**< 原请求、文件身份、FTP 路径及本次凭据。 */
        QString host; /**< 本次 FTP 主机，IPv6 带方括号。为空时仅可复用已校验的本地缓存。 */
        int port = 8969; /**< FTP TCP端口。 */
        qint64 bytes = 0, version = 0; /**< 文件长度和请求配置版本。 */
        quint64 generation = 0; /**< 所属设备/全量清单代次。 */
    };
    /** @brief 独立文件播放器的可恢复播放状态。 */
    struct FilePlayback {
        GstPlayerWidget *player = nullptr; /**< 由服务最终释放。 */
        QJsonObject plan; /**< 当前计划或标识符请求的值快照。 */
        QString request; /**< 标识符START尚待确认时的原请求ID。 */
        QString occurrence; /**< 按时段开始日生成的去重键。 */
        qint64 dueMs = -1; /**< 自动 LOOP 的固定开播点，epoch 毫秒；其他播放为 -1。 */
        bool automatic = false; /**< true 表示由定时循环调度，首次获焦时建立本时段的间隔锚点。 */
        bool started = false; /**< 本轮已获焦并请求开播；区间结束时允许仍在播放的一轮收尾。 */
        bool playing = false, manualPause = false, focusPaused = false; /**< 状态及独立暂停原因。 */
        int position = 0, duration = 0; /**< GStreamer已上报位置和时长，毫秒。 */
    };
    /** @brief 构造设备上行信封，空请求ID时生成独立ID。
     * @param[in] method 上行方法。
     * @param[in] id 原请求ID，可空。
     * @param[in] data 业务状态或回执。
     */
    void send(const QString &method, const QString &id, const QJsonObject &data);
    /** @brief 发布广播内存状态。
     * @param[in] trigger 上报原因。
     * @param[in] id 查询原ID，可空。
     */
    void state(const QString &trigger, const QString &id = {});
    /** @brief 校验并替换完整计划，入队缺失的音频。
     * @param[in] data 完整配置。data.ftp 的主机和凭据只用于本批下载，不写入计划快照。
     * @param[in] id 回执关联ID。
     * @param[in] loading true表示启动加载持久化配置，不发回执。
     * @param[in] committed true表示后台已原子写入本版本。
     */
    void apply(const QJsonObject &data, const QString &id, bool loading = false, bool committed = false);
    /** @brief 原子写入平台简写停用指令，清空定时计划并保留音频目录。
     * @param[in] data 省略两个清单的显式停用指令；数字 cfgVersion=0 保留已应用版本。
     * @param[in] id 回执关联ID。
     */
    void clearPlans(const QJsonObject &data, const QString &id);
    /** @brief 序列化计划与现有音频目录，凭据不进入快照。
     * @param[in] version 将写入的配置版本。
     * @param[in] enabled 文件广播开关。
     * @param[in] plans 将写入的定时计划。
     * @param[in] keepOnce 是否保留单次时段记录。
     * @return 可供后台原子写入的 JSON 字节。
     */
    QByteArray configBytes(qint64 version, bool enabled, const QJsonArray &plans, bool keepOnce) const;
    /** @brief 后台保存队列执行时的已应用计划与缓存描述，FTP密码不落盘。 */
    void persist();
    /** @brief 排入音频校验和下载任务。
     * @param[in] audio 文件描述。
     * @param[in] ftp 本批 host、port、user 和 pwd。host 字符串接受主机名或 IP，未提供非空字符串时使用设备默认主机。
     * @param[in] id 回执请求ID，可空。
     * @note 非空 host 字符串格式无效时，缓存未命中则按下载失败回报。
     */
    void queueAudio(const QJsonObject &audio, const QJsonObject &ftp, const QString &id);
    /** @brief 驱动单任务缓存检查、FTP和安装。 */
    void nextDownload();
    /** @brief 完成本次FTP并在后台核对大小与MD5。
     * @param[in] ok 进程是否成功退出。
     */
    void completeTransfer(bool ok);
    /** @brief 清理本次任务并上报结果。
     * @param[in] ok 文件是否已通过校验。
     */
    void finishDownload(bool ok);
    /** @brief 上报一条音频下载状态。
     * @param[in] job 本次下载关联。
     * @param[in] status NONE/RUNNING/DONE/FAILED。
     * @param[in] progress 0..100。
     */
    void progress(const Download &job, const QString &status, int progress);
    /** @brief 执行时段、直播超时与状态心跳，并在文件活动期间每约五秒汇总一次播放状态。 */
    void tick();
    /** @brief 在有效时段首次立即开播，此后按首次开播锚点和平台间隔处理轮次。
     * @param[in] now 设备本地日期时间，用于判断时段和到点轮次。
     * @param[in] inCall 当前调度是否应按通话占用处理。
     */
    void schedule(const QDateTime &now, bool inCall);
    /** @brief 持久化 ONCE 时段已播放或已跳过的记录。
     * @param[in] plan 当前计划，非 ONCE 忽略。
     * @param[in] occurrence 当前时段键。
     */
    void markOnceHandled(const QJsonObject &plan, const QString &occurrence);
    /** @brief 申请文件播放焦点并保存可恢复状态。
     * @param[in,out] slot 定时或标识符播放器。
     * @param[in] plan 播放描述。
     * @param[in] occurrence 时段键，手动播放可空。
     * @param[in] dueMs 自动循环的固定开播点，epoch 毫秒；-1 不限制获焦迟到。
     * @param[in] automatic true 表示自动循环轮次；首次获焦时记录实际开播时刻。
     * @return true 接受播放意图，false 文件不可用。
     * @note 同一计划和音频连续不可用时仅告警一次；可用后清除该告警标记。
     */
    bool startFile(FilePlayback &slot, const QJsonObject &plan, const QString &occurrence,
                   qint64 dueMs = -1, bool automatic = false);
    /** @brief 结束文件播放并归还焦点。
     * @param[in,out] slot 待清除的文件播放器。
     */
    void stopFile(FilePlayback &slot);
    /** @brief 创建文件播放器并绑定完成、错误、暂停和恢复。
     * @param[in,out] slot 声明周期覆盖播放器。
     */
    void preparePlayer(FilePlayback &slot);
    /** @brief 执行直播开始或控制。
     * @param[in] method 开始/控制方法。
     * @param[in] id 原请求ID。
     * @param[in] data 已解析控制参数。
     */
    void liveCommand(const QString &method, const QString &id, const QJsonObject &data);
    /** @brief 停止直播并归还焦点。
     * @param[in] reason 状态上报原因。
     */
    void stopLive(const QString &reason);
    /** @brief 申请直播焦点，若旧输出正在归还则延后申请。 */
    void acquireLiveFocus();
    /** @brief 在后台声卡交接完成后释放直播焦点。 */
    void releaseLiveFocus();
    /** @brief 在服务线程轮询交接，完成后处理尚有效的恢复请求。 */
    void finishLiveFocusHandoff();
    /** @brief 同步本地及mqttd的直播接收门并推进媒体代次。
     * @param[in] enabled true开始接收新帧，false丢弃积压。
     * @return true已提交本地控制，false IPC未受理。
     */
    bool setLiveMedia(bool enabled);
    /** @brief 输出首帧、首次送播或关闭媒体前的统计。
     * @param[in] final true始终输出本代次汇总，false只记录首次变化。
     */
    void logLiveMedia(bool final);
    /** @brief 发布当前直播状态。
     * @param[in] state PLAYING/PAUSED/ENDED。
     * @param[in] reason 可空的暂停或结束原因。
     */
    void liveState(const QString &state, const QString &reason = {});

    Publisher publish_; /**< 已注入的本机发布入口。 */
    QString device_, ftpHost_, directory_; /**< 当前设备、下行缺省时的 FTP 主机与设备隔离缓存目录。 */
    bool scheduledEnabled_ = true; /**< 控制器缺省允许；设备启动后由本地配置覆盖。 */
    bool online_ = false, enabled_ = false; /**< MQTT在线及文件广播配置开关。 */
    qint64 version_ = 0; /**< 已应用的全量计划版本。 */
    quint64 routeGeneration_ = 0; /**< 设备切换代次，隔离旧设备的异步配置回调。 */
    quint64 generation_ = 0; /**< 设备/全量切换代次，旧下载不提交。 */
    QJsonArray plans_; /**< 完整计划的权威快照。 */
    QHash<QString, QJsonObject> audio_; /**< 目录描述，以规范化数字ID索引。 */
    QHash<QString, QString> local_; /**< 仅已校验的可播放本地路径。 */
    QSet<QString> unavailableLogged_; /**< 已报告文件不可用的计划/音频键；文件恢复或配置切换时清除。 */
    QHash<QString, QString> once_; /**< ONCE 计划最后已处理的时段键，包含通话跳过。 */
    QHash<QString, qint64> loopAnchor_; /**< 每时段首次自动循环实际请求开播的 epoch 毫秒，后续轮次不重置。 */
    QHash<QString, qint64> lastLoop_; /**< 每时段最后处理的固定开播点，epoch 毫秒。 */
    QSet<QString> stopped_; /**< 手动STOP的本次时段键。 */
    QQueue<Download> downloads_; /**< 单工作进程串行消费的文件请求。 */
    Download activeDownload_; /**< 当前缓存检查或传输任务。 */
    QString temporary_, destination_; /**< 本任务临时及正式路径。 */
    QProcess *process_ = nullptr; /**< 本任务异步FTP进程。 */
    int lastProgress_ = -1; /**< 去重后的进度百分比。 */
    qint64 downloadStarted_ = 0; /**< FTP启动单调毫秒，限制挂起时间。 */
    std::unique_ptr<SerialWorkQueue> files_; /**< 哈希和持久化的后台串行队列。 */
    FilePlayback scheduled_, tagged_; /**< 独立定时与标识符播放器；通话打断自动排程后丢弃本轮。 */
    QStringList seen_; /**< 最近64个业务指令键，防止重复副作用。 */
    QHash<QString,QPair<QString,QJsonObject>> replies_; /**< 已有业务回执，便于重复指令重发。 */
    QTimer timer_; /**< 主线程低频调度与状态上报。 */
    QElapsedTimer clock_; /**< 下载、直播生命周期和心跳的单调时间。 */
    qint64 lastFileDiagnosticMs_ = 0; /**< 文件播报最近一次诊断日志的单调毫秒，仅限正在播放时节流。 */
    qint64 lastState_ = 0, lastLiveState_ = 0; /**< 最近心跳单调毫秒。 */
    bool liveReleasePending_ = false; /**< 服务线程维护，等待直播后台归还输出。 */
    bool liveAcquirePending_ = false; /**< 旧输出归还后重新申请焦点。 */
    GstPlayerWidget *liveInput_; /**< 服务独有的直播混音输入。 */
    std::unique_ptr<BroadcastLive> live_; /**< 播放线程，先于输入析构。 */
    QString liveId_, liveStartId_; /**< 当前会话与尚待授予焦点的启动请求。 */
    int frameMs_ = 20, liveVolume_ = 80; /**< 线上间隔毫秒和软件音量百分比。 */
    bool manualLivePause_ = false, callLivePause_ = false, resumeAfterCall_ = true; /**< 独立暂停原因与恢复策略。 */
    bool firstLiveFrameLogged_ = false, firstLivePcmLogged_ = false; /**< 当前媒体代次已输出的首帧/首次送播诊断。 */
    bool livePlaying_ = false; /**< 焦点获准且已开启接收门。 */
    qint64 liveStarted_ = 0, liveDeadline_ = 0; /**< 会话开始与到期单调毫秒。 */
    qint64 liveFocusDeadline_ = 0; /**< 本次启动或恢复等待焦点的单调毫秒截止时间；0表示未计时。 */
};
