/**
 * @file web_local_runtime.h
 * @brief 本地界面与硬件运行桥；跨线程只传状态、资源句柄和执行结果。
 * @author Du lin
 * @date 2026-10-16
 */

#pragma once
#include <QObject>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QHash>
#include <functional>
class QImage;

/** @brief 在 Qt 主线程连接真实界面、播放器和平台设备入口。 */
class WebLocalRuntime final : public QObject {
    Q_OBJECT

public:

    /**
     * @brief 返回应用主线程创建并随应用退出销毁的本地运行桥。
     * @return 应用生命周期内共享的桥对象指针。
     */
    static WebLocalRuntime *instance(); // create on the application thread

    /**
     * @brief 绑定实际播放器的读取和控制回调，并刷新初始快照。
     * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
     * @param read 在主线程读取实际播放器状态的回调。
     * @param control 在主线程应用播放器设置的回调。
     * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
     */
    void bindMedia(QObject *owner,
                   std::function<QJsonObject()> read,
                   std::function<void(const QJsonObject &)> control);

    /**
     * @brief 绑定当前活动界面的截图入口及其对象生命周期。
     * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
     * @param capture 在主线程发起截图的回调，参数为任务标识。
     * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
     */
    void bindScreen(QObject *owner, std::function<void(const QString &)> capture);

    /**
     * @brief 绑定后台网络页的异步配置入口，网卡操作沿用共享网络线程。
     * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
     * @param apply 在主线程提交对应异步操作的回调。
     * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
     */
    void bindNetwork(QObject *owner,
                     std::function<void(const QString &, const QJsonObject &)> apply);
    /**
     * @brief 绑定Web独立定时梯控入口，不依赖MQTT管理器。
     * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
     * @param apply 在主线程提交对应异步操作的回调。
     * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
     */
    void bindFloorSchedule(QObject *owner,
                           std::function<void(const QString &, const QJsonObject &)> apply);

    /**
     * @brief 返回线程安全的最近一次真实播放器快照。
     * @return 加锁复制的最近一次实际播放器快照。
     */
    QJsonObject media() const;

    /**
     * @brief 根据缓存快照和串口就绪标记返回运行能力。
     * @return 当前已接入功能与运行就绪状态。
     */
    QJsonObject capabilities() const;

    /**
     * @brief 线程安全地更新本地串口控制的就绪状态。
     * @param ready 实际硬件或能力是否就绪。
     */
    void setDeviceReady(bool ready);

    /**
     * @brief 校验当前图像并交给线程池编码落盘，随后返回小型结果元数据。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param image 待编码的界面图像。
     */
    void image(const QString &token, const QImage &image);

    /**
     * @brief 接收已有界面合成信号，并解码当前等待中的截图。
     * @param base64 现有截图信号提供的 Base64 图像数据。
     */
    void encodedImage(const QString &base64);

    /**
     * @brief 发出本地任务完成信号，交由 Web 工作线程持久化结果。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param data 本次操作的数据对象。
     * @param code 业务状态码，成功通常为 200。
     * @param message 可返回调用方的结果或错误说明。
     */
    void complete(const QString &token,
                  const QJsonObject &data,
                  int code = 200,
                  const QString &message = {});

signals:

    /**
     * @brief 发布手机视频上传的实际接收、校验和发布状态。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param name 待处理的名称。
     * @param percent 实际处理进度百分比，范围 0~100。
     * @param state 实际执行状态。
     */
    void
    uploadProgress(const QString &token, const QString &name, int percent, const QString &state);

    /**
     * @brief 视频成功发布后排队切换真实播放器；仅接收设备端生成的文件名。
     * @param name 待处理的名称。
     */
    void videoUploaded(const QString &name);
    /**
     * @brief 通知播放器清除Web删除的媒体及图片缓存。
     * @param fullPath 已删除媒体的完整路径。
     */
    void mediaFileDeleted(const QString &fullPath);

    /**
     * @brief 复用 U 盘覆盖层显示 Web 资源接收、解压与目录更新进度。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param message 可返回调用方的结果或错误说明。
     * @param percent 实际处理进度百分比，范围 0~100。
     * @param finished 是否已经完成本阶段或任务。
     */
    void resourceProgress(const QString &token, const QString &message, int percent, bool finished);

    /**
     * @brief 跨线程提交带任务标识的本地执行请求。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     */
    void requested(const QString &token, const QString &operation, const QJsonObject &args);

    /**
     * @brief 返回任务结果、业务状态码和可显示的错误说明。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param data 本次操作的数据对象。
     * @param code 业务状态码，成功通常为 200。
     * @param message 可返回调用方的结果或错误说明。
     */
    void finished(const QString &token, const QJsonObject &data, int code, const QString &message);

    /**
     * @brief 将串口或重启任务交给应用主线程绑定的实际平台入口。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     */
    void deviceRequested(const QString &token, const QString &operation, const QJsonObject &args);

private:

    /**
     * @brief 在应用主线程建立请求队列和真实播放器状态采样定时器。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    explicit WebLocalRuntime(QObject *parent);

    /**
     * @brief 在主线程分派截图、播放器切换和设备 IO 请求。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     */
    void execute(const QString &token, const QString &operation, const QJsonObject &args);

    /**
     * @brief 在主线程采集播放器实际状态，再以互斥保护发布快照。
     */
    void refreshMedia();

    // 快照和就绪位允许 Web 工作线程读取，控件与回调只在主线程使用。
    mutable QMutex mutex_; /**< 保护跨线程读取的状态快照和就绪位。 */
    QJsonObject media_; /**< 最近一次在主线程采集的播放器真实状态。 */
    QPointer<QObject> mediaOwner_, screenOwner_; /**< 播放和截图回调的生命周期守卫。 */
    QPointer<QObject> networkOwner_; /**< 网络页生命周期守卫。 */
    std::function<void(const QString &, const QJsonObject &)> networkApply_; /**< 主线程网络配置受理入口。 */
    QPointer<QObject> floorScheduleOwner_; /**< 独立定时执行器生命周期守卫。 */
    std::function<void(const QString &, const QJsonObject &)> floorScheduleApply_; /**< 主线程规则操作受理入口。 */
    bool floorScheduleReady_ = false; /**< 独立定时规则写入口是否已绑定。 */
    std::function<QJsonObject()> read_; /**< 仅主线程调用的播放器状态读取函数。 */
    std::function<void(const QJsonObject &)> control_; /**< 仅主线程调用的播放设置函数。 */
    std::function<void(const QString &)> capture_; /**< 仅主线程调用的界面截图函数。 */

    // 截图令牌覆盖采集到编码的完整生命周期，防止并发重复启动。
    QString captureToken_; /**< 覆盖采集和编码全流程的唯一截图任务标识。 */
    QHash<QString, QString> output_; /**< 截图任务标识到受控输出文件的映射。 */
    bool mediaBusy_ = false; /**< 播放器切换尚未返回结果，后续上传暂缓。 */
    QString pendingVideo_; /**< 连续上传时保留最近完成的视频，等待当前切换结束。 */
    bool deviceReady_ = false; /**< 由平台入口发布的串口控制就绪状态。 */
};
