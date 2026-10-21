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

    /** @brief 返回应用主线程创建并随应用退出销毁的本地运行桥。 */
    static WebLocalRuntime *instance(); // create on the application thread

    /** @brief 绑定实际播放器的读取和控制回调，并刷新初始快照。 */
    void bindMedia(QObject *owner,
                   std::function<QJsonObject()> read,
                   std::function<void(const QJsonObject &)> control);

    /** @brief 绑定当前活动界面的截图入口及其对象生命周期。 */
    void bindScreen(QObject *owner, std::function<void(const QString &)> capture);

    /** @brief 绑定后台网络页的异步配置入口，网卡操作沿用共享网络线程。 */
    void bindNetwork(QObject *owner,
                     std::function<void(const QString &, const QJsonObject &)> apply);

    /** @brief 返回线程安全的最近一次真实播放器快照。 */
    QJsonObject media() const;

    /** @brief 根据缓存快照和串口就绪标记返回运行能力。 */
    QJsonObject capabilities() const;

    /** @brief 线程安全地更新本地串口控制的就绪状态。 */
    void setDeviceReady(bool ready);

    /** @brief 校验当前图像并交给线程池编码落盘，随后返回小型结果元数据。 */
    void image(const QString &token, const QImage &image);

    /** @brief 接收已有界面合成信号，并解码当前等待中的截图。 */
    void encodedImage(const QString &base64);

    /** @brief 发出本地任务完成信号，交由 Web 工作线程持久化结果。 */
    void complete(const QString &token,
                  const QJsonObject &data,
                  int code = 200,
                  const QString &message = {});

signals:

    /** @brief 发布手机视频上传的实际接收、校验和发布状态。 */
    void
    uploadProgress(const QString &token, const QString &name, int percent, const QString &state);

    /** @brief 视频成功发布后排队切换真实播放器；仅接收设备端生成的文件名。 */
    void videoUploaded(const QString &name);

    /** @brief 复用 U 盘覆盖层显示 Web 资源接收、解压与目录更新进度。 */
    void resourceProgress(const QString &token, const QString &message, int percent, bool finished);

    /** @brief 跨线程提交带任务标识的本地执行请求。 */
    void requested(const QString &token, const QString &operation, const QJsonObject &args);

    /** @brief 返回任务结果、业务状态码和可显示的错误说明。 */
    void finished(const QString &token, const QJsonObject &data, int code, const QString &message);

    /** @brief 将串口或重启任务交给应用主线程绑定的实际平台入口。 */
    void deviceRequested(const QString &token, const QString &operation, const QJsonObject &args);

private:

    /** @brief 在应用主线程建立请求队列和真实播放器状态采样定时器。 */
    explicit WebLocalRuntime(QObject *parent);

    /** @brief 在主线程分派截图、播放器切换和设备 IO 请求。 */
    void execute(const QString &token, const QString &operation, const QJsonObject &args);

    /** @brief 在主线程采集播放器实际状态，再以互斥保护发布快照。 */
    void refreshMedia();

    // 快照和就绪位允许 Web 工作线程读取，控件与回调只在主线程使用。
    mutable QMutex mutex_;
    QJsonObject media_;
    QPointer<QObject> mediaOwner_, screenOwner_;
    QPointer<QObject> networkOwner_;
    std::function<void(const QString &, const QJsonObject &)> networkApply_;
    std::function<QJsonObject()> read_;
    std::function<void(const QJsonObject &)> control_;
    std::function<void(const QString &)> capture_;

    // 截图令牌覆盖采集到编码的完整生命周期，防止并发重复启动。
    QString captureToken_;
    QHash<QString, QString> output_;
    bool mediaBusy_ = false;
    QString pendingVideo_; /**< 连续上传时保留最近完成的视频，等待当前切换结束。 */
    bool deviceReady_ = false;
};
