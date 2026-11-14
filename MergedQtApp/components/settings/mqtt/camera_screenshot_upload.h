/**
 * @file camera_screenshot_upload.h
 * @brief 摄像头单次/订阅截图控制、有界后台编码与可取消上传。
 * @author Du lin
 * @note 最近修改：2026-11-10；修改人：Du lin；声明只读方法分类，便于 MQTT 在摄像头禁用时跳过业务解析。
 * @date 2026-11-05
 */
#pragma once
#include <QObject>
#include <QImage>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QQueue>
#include <QSettings>
#include <QTimer>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

class SerialWorkQueue;

/**
 * @brief UI线程维护截图控制状态；原相机采集和共享media.snapshot线程负责图像工作。
 * @details 最多4笔单次请求、1笔编码；周期拥塞跳帧，不缓存已编码图片、不补历史。
 * 本地上传总开关默认关闭；订阅本地存储使用独立单任务后台队列。停止取消未开写的 IPC 图片
 * 及本地任务。没有60秒续期/自动开订阅。
 */
class CameraScreenshotUpload final : public QObject {
    Q_OBJECT
public:
    using Cancel = std::shared_ptr<std::atomic_bool>; /**< 会话取消令牌，跨编码/IPC线程只读。 */
    using Publisher = std::function<bool(const QByteArray&,const Cancel&)>; /**< 主线程有界提交，不等待网络。 */
    /** @brief 创建25ms调度器，未启用时不启动计时或采集。
     * @param[in] allowed 本地INI启动快照，缺项/读取失败由configEnabled返回false。
     * @param[in] parent QObject所有者，可为空。
     */
    explicit CameraScreenshotUpload(bool allowed,QObject *parent=nullptr);
    /** @brief 取消截图需求，最终析构等待独立本地存储线程退出。 */
    ~CameraScreenshotUpload() override;
    /** @brief 严格读取cameraScreenshot/enabled，不写盘，不热更新。
     * @param[in] settings 已打开的运行配置；仅true/1启用，缺项、其他值及读失败关闭。
     * @return 是否允许摄像头截图上传。
     */
    static bool configEnabled(const QSettings &settings);
    /** @brief 注入现有单IPC的空闲快照与可取消提交；仅主线程调用。
     * @param[in] ready 无普通业务/图片待发且原大帧预算到期时true，仅本地容量。
     * @param[in] publisher false表示拒绝；订阅直接跳帧，单次保留有界请求重试至超时。
     */
    void setTransport(std::function<bool()> ready,Publisher publisher);
    /** @brief 更换精确ycLinux event主题时停止旧会话；不接受通配符或额外路径。
     * @param[in] eventTopic 当前设备的event主题；非法值禁用协议路由。
     */
    void configure(const QString &eventTopic);
    /** @brief 同步Broker和IPC可用性；掉线取消订阅，重连不回放旧图。
     * @param[in] online true表示两者可用，不自动重新启用订阅。
     */
    void setOnline(bool online);
    /** @brief 识别摄像头下行方法，无需实例或业务数据。
     * @param[in] method 下行方法名。
     * @return true 为单次或订阅截图协议。
     */
    static bool handlesMethod(const QString &method);
    /** @brief 校验并消费getScreenshot/setScreenshotConfig，关闭时完全静默。
     * @param[in] topic 必须精确匹配当前设备request主题。
     * @param[in] payload 含id/deviceId/data的已解析信封。单次/普通订阅缺省 PERC:15；普通订阅
     *                   优先有效的 storagePolicy.cloudQuality，CALL 上传固定 ORIG；订阅可配置本地存储。
     * @return true为摄像头协议（包括忽略/失败）；false为其他method。
     */
    bool handle(const QString &topic,const QJsonObject &payload);
    /** @brief 通话结束/显式关闭立即取消任务、定时器和未开写的上传/本地帧，释放截图占用。
     * @note 已开始的完整JSON行和Broker/网络中的帧无法撤回；不关闭识别仍需要的相机。
     */
    void stop();
private:
    /** @brief 一笔纯元数据请求；不拥有相机或UI指针。 */
    struct Request {
        QString id,quality; /**< 上行关联ID与实际请求质量；周期帧使用新ID。 */
        qint64 queuedMs=0,captureAfter=0; /**< 单调受理毫秒及允许采集的最早epoch毫秒。 */
        int seq=0; /**< 0为单次；订阅从1递增，INT_MAX后回绕到1。 */
    };
    /** @brief 当前订阅的本地存储快照，队列使用值拷贝。 */
    struct LocalPolicy {
        QString directory,quality; /**< 设备专用目录及本地 JPEG 编码档。 */
        qint64 maxBytes=0; /**< 容量上限，字节。 */
        Cancel cancel; /**< 策略变更或停订阅时取消旧任务；空表示关闭。 */
    };
    struct LocalStore;
    /** @brief 根据可选子对象更新本地存储策略，并在后台回收超额文件。
     * @param[in] policy 平台传入的 storagePolicy；空对象有效，未传则保持旧协议行为。
     */
    void configureLocal(const QJsonValue &policy);
    /** @brief 向独立后台队列提交最多一张图或一次配额检查，忙时跳过。
     * @param[in] image 需单独编码时传入自有图像，空表示复用 JPEG 或仅回收。
     * @param[in] jpeg 已编码 JPEG；空且 image 为空表示仅回收。
     * @param[in] ts 采集时刻 epoch 毫秒。
     * @param[in] id 周期截图 ID，用于唯一文件名。
     */
    void storeLocal(const QImage &image,const QByteArray &jpeg,qint64 ts,const QString &id);
    /** @brief 原订阅上传本轮忙时，尝试保存独立本地帧，不等待磁盘或占用单次请求。 */
    void captureLocalOnly();
    /** @brief 周期调度：忙时跳帧；无帧等待最多3秒；不在此编码大图。 */
    void poll();
    /** @brief 同步有界请求/订阅对相机的占用和调度器，不操作PIC。 */
    void updateDemand();
    /** @brief 后台执行一次JPEG/Base64/JSON；完成仅提交未取消代次。
     * @param[in] image 自有RGB像素，空图时生成失败包。
     * @param[in] ts 实际采集epoch毫秒；失败时为检测失败时刻。
     * @param[in] reason 非空为失败说明，此时不编码图片。
     */
    void encode(const QImage &image,qint64 ts,const QString &reason);
    /** @brief 构造小型失败信封并尝试有界提交；关闭/取消时不发送。
     * @param[in] request 保留原单次ID或订阅序号。
     * @param[in] reason 不包含图片内容的失败说明。
     */
    void fail(const Request &request,const QString &reason);
    bool allowed_=false,online_=false,subscribed_=false,encoding_=false; /**< 主线程总开关/在线/订阅/唯一编码在途。 */
    QString eventTopic_,deviceId_,quality_=QStringLiteral("PERC:15"); /**< 精确路由身份及本轮订阅的有效 JPEG 质量。 */
    int intervalMs_=1000,seq_=0; /**< 订阅毫秒间隔200..60000及序号；重发配置不重置序号。 */
    qint64 nextMs_=0; /**< 单调时钟下的下次周期起点；不追赶遗漏周期。 */
    LocalPolicy local_; /**< 主线程当前策略，后台只使用快照和取消令牌。 */
    qint64 localCaptureAfter_=0; /**< 本轮允许的最早采集 epoch 毫秒；0 无独立取帧。 */
    bool localQuotaPending_=false; /**< 策略启用或变更后待执行一次容量检查。 */
    std::shared_ptr<LocalStore> localStore_; /**< 独立队列独占文件索引；与工作任务共享寿命。 */
    std::unique_ptr<SerialWorkQueue> localWork_; /**< 按需创建，忙时跳过，无照片积压。 */
    QElapsedTimer clock_;QTimer timer_; /**< 主线程单调调度器，只在有需求时运行。 */
    QQueue<Request> pending_;Request active_; /**< 单次元数据与活动请求合计最多4笔，不包含周期帧。 */
    QStringList seen_; /**< 最近64个已受理指令ID，拒绝QoS重复导致停用后重开。 */
    Cancel cancelled_=std::make_shared<std::atomic_bool>(false); /**< stop置true，后续新指令建立新代次。 */
    std::function<bool()> ready_;Publisher publisher_; /**< 主线程调用的非阻塞单IPC接口；不由后台调用。 */
};
