/**
 * @file web_local_runtime.cpp
 * @brief Qt 主线程的真实状态采集、设备控制和异步截图桥。
 * @author Du lin
 * @date 2026-10-16
 */

#include "web_local_runtime.h"
#include "common/media_content_policy.h"
#include "platform/rk3566_platform.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QBuffer>
#include <QDebug>
#include <memory>

namespace {
class Work final : public QRunnable {
public:

    std::function<void()> action;

    /**
     * @brief 保存在线程池中执行的截图编码任务。
     * @param fn 在线程池中执行的任务，不直接访问界面控件。
     */
    explicit Work(std::function<void()> fn) : action(std::move(fn))
    {
    }

    /**
     * @brief 在工作线程执行已捕获的截图编码与落盘任务。
     */
    void run() override
    {
        action();
    }
};
} // namespace

/**
 * @brief 返回由应用主线程持有的本地设备运行桥实例。
 * @return 应用生命周期内共享的桥对象指针。
 */
WebLocalRuntime *WebLocalRuntime::instance()
{
    static auto *runtime = new WebLocalRuntime(QCoreApplication::instance());
    return runtime;
}

/**
 * @brief 在应用主线程建立请求队列和真实播放器状态采样定时器。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
WebLocalRuntime::WebLocalRuntime(QObject *parent) : QObject(parent)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    connect(this,
            &WebLocalRuntime::requested,
            this,
            &WebLocalRuntime::execute,
            Qt::QueuedConnection);
    // 发布通知只在主线程排队，绝不在 Web 工作线程操作 GStreamer 控件。
    connect(
        this,
        &WebLocalRuntime::videoUploaded,
        this,
        [this](const QString &name) {
            pendingVideo_ = name;
        },
        Qt::QueuedConnection);
    connect(this,
            &WebLocalRuntime::finished,
            this,
            [](const QString &token, const QJsonObject &, int code, const QString &message) {
                if (token.startsWith("web-upload:") && code != 200)
                    qWarning() << "[WEB] uploaded video playback failed:" << message;
            });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() {
        refreshMedia();
        // 仅在既有播放器允许启动且上一轮切换结束后播放新发布文件。
        if (!pendingVideo_.isEmpty() && mediaOwner_ && control_ && !mediaBusy_ &&
            media().value("canStartLocalPlayback").toBool(true)) {
            const auto name = pendingVideo_;
            pendingVideo_.clear();
            execute("web-upload:" + name,
                    "videoControl",
                    {{"videoType", "RECORDED"}, {"videoPath", QFileInfo(name).fileName()},
                     {"_mediaPath", QFileInfo(name).isAbsolute() ? name : QDir(Rk3566Platform::videoDir()).filePath(name)}});
        }
    });
    timer->start(1000);
}

/**
 * @brief 绑定实际播放器的读取和控制回调，并刷新初始快照。
 * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
 * @param read 在主线程读取实际播放器状态的回调。
 * @param control 在主线程应用播放器设置的回调。
 * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
 */
void WebLocalRuntime::bindMedia(QObject *owner,
                                std::function<QJsonObject()> read,
                                std::function<void(const QJsonObject &)> control)
{
    mediaOwner_ = owner;
    read_ = std::move(read);
    control_ = std::move(control);
    refreshMedia();
}

/**
 * @brief 绑定当前活动界面的截图入口及其对象生命周期。
 * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
 * @param capture 在主线程发起截图的回调，参数为任务标识。
 * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
 */
void WebLocalRuntime::bindScreen(QObject *owner, std::function<void(const QString &)> capture)
{
    screenOwner_ = owner;
    capture_ = std::move(capture);
}

/**
 * @brief 注册异步有线配置回调，只在 Qt 主线程调用后台页面入口。
 * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
 * @param apply 在主线程提交对应异步操作的回调。
 * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
 */
void WebLocalRuntime::bindNetwork(QObject *owner,
                                  std::function<void(const QString &, const QJsonObject &)> apply)
{
    networkOwner_ = owner;
    networkApply_ = std::move(apply);
}

/**
 * @brief 在主线程采集播放器实际状态，再以互斥保护发布快照。
 */
void WebLocalRuntime::refreshMedia()
{
    QJsonObject value;
    if (mediaOwner_ && read_)
        value = read_();
    value.insert("available", !value.isEmpty());
    value.insert("observedAt", QDateTime::currentDateTime().toString(Qt::ISODate));
    {
        QMutexLocker lock(&mutex_);
        media_ = value;
    }
}

/**
 * @brief 返回线程安全的最近一次真实播放器快照。
 * @return 加锁复制的最近一次实际播放器快照。
 */
QJsonObject WebLocalRuntime::media() const
{
    QMutexLocker lock(&mutex_);
    return media_;
}

/**
 * @brief 根据缓存快照和串口就绪标记返回运行能力。
 * @param owner 回调所属对象；QPointer 在对象销毁后阻止使用失效回调。
 * @param apply 在主线程提交对应异步操作的回调。
 * @note 在应用主线程绑定；Web 工作线程通过排队请求调用。
 */
void WebLocalRuntime::bindFloorSchedule(QObject *owner,
                                       std::function<void(const QString &, const QJsonObject &)> apply)
{
    floorScheduleOwner_ = owner;
    floorScheduleApply_ = std::move(apply);
    QMutexLocker lock(&mutex_);
    floorScheduleReady_ = owner && bool(floorScheduleApply_);
}

/**
 * @brief 读取当前模块支持能力及真实就绪状态。
 * @return 当前已接入功能与运行就绪状态。
 */
QJsonObject WebLocalRuntime::capabilities() const
{
    // Read readiness from the thread-safe source snapshot, never from MQTT.
    const bool ready = media().value("available").toBool();
    QMutexLocker lock(&mutex_);
    return {{"playback", ready}, {"screenshot", ready}, {"elevatorWrite", deviceReady_},
            {"floorScheduleWrite", floorScheduleReady_}};
}

/**
 * @brief 线程安全地更新本地串口控制的就绪状态。
 * @param ready 实际硬件或能力是否就绪。
 */
void WebLocalRuntime::setDeviceReady(bool ready)
{
    QMutexLocker lock(&mutex_);
    deviceReady_ = ready;
}

/**
 * @brief 发出本地任务完成信号，交由 Web 工作线程持久化结果。
 * @param token 贯穿请求与异步结果的任务标识。
 * @param data 本次操作的数据对象。
 * @param code 业务状态码，成功通常为 200。
 * @param message 可返回调用方的结果或错误说明。
 */
void WebLocalRuntime::complete(const QString &token,
                               const QJsonObject &data,
                               int code,
                               const QString &message)
{
    emit finished(token, data, code, message);
}

/**
 * @brief 在主线程分派截图、播放器切换和设备 IO 请求。
 * @param token 贯穿请求与异步结果的任务标识。
 * @param operation 白名单业务操作名。
 * @param args 操作对应的结构化参数。
 */
void WebLocalRuntime::execute(const QString &token,
                              const QString &operation,
                              const QJsonObject &args)
{

    // 截图采集和后台编码共用一个占用标记，超时清理后允许后续请求。
    if (operation == "screenshot") {
        if (!screenOwner_ || !capture_) {
            complete(token, {}, 503, "当前界面未就绪");
            return;
        }
        if (!captureToken_.isEmpty()) {
            complete(token, {}, 409, "截图正在采集，请稍后重试");
            return;
        }
        captureToken_ = token;
        output_.insert(token, args.value("output").toString());
        capture_(token);
        QTimer::singleShot(8000, this, [this, token]() {
            if (output_.remove(token)) {
                if (captureToken_ == token)
                    captureToken_.clear();
                complete(token, {}, 504, "当前界面截图超时");
            }
        });
        return;
    }

    // 切换后轮询真实播放器状态，达到目标才完成，不能以请求参数充当回执。
    if (operation == "videoControl") {
        if (!mediaOwner_ || !control_) {
            complete(token, {}, 503, "播放器未就绪");
            return;
        }
        if (mediaBusy_) {
            complete(token, {}, 409, "播放器正在切换，请稍后重试");
            return;
        }
        // 在GUI线程串行写入，与MQTT共用同一INI；省略字段保留当前选择。
        const QString iniPath = Rk3566Platform::netConfigPath();
        if (args.contains("contentType") || args.contains("smallImageMode")) {
            const QString content = args.contains("contentType") ? args.value("contentType").toString()
                                                                : MediaContentPolicy::contentType(iniPath);
            const bool small = args.contains("smallImageMode") ? MediaContentPolicy::smallImageMode(args.value("smallImageMode"))
                                                               : MediaContentPolicy::smallImageMode(iniPath);
            QString error;
            if (!MediaContentPolicy::saveSelection(iniPath, content, small, &error)) {
                complete(token, {}, 503, error);
                return;
            }
        }
        mediaBusy_ = true;
        control_(args);
        auto *timer = new QTimer(this);
        timer->setInterval(200);
        auto attempts = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [this, token, args, timer, attempts]() {
            refreshMedia();
            const auto state = media();
            const auto type = args.value("videoType").toString();
            const bool selectionRequest = args.contains("contentType") || args.contains("smallImageMode") || args.contains("_mediaPath");
            const bool modeOk = type.isEmpty() || state.value("videoType") == type ||
                    (type == "RECORDED" && selectionRequest && state.value("videoType") == "NONE" &&
                     state.value("localPlaylistEmpty").toBool() && state.value("canStartLocalPlayback").toBool());
            const bool volumeOk = !args.contains("videoVolume") ||
                                  state.value("videoVolume") == args.value("videoVolume");
            const bool liveOk =
                type != "LIVE" || (state.value("playbackState") == "PLAYING" &&
                                   state.value("videoUrl") == args.value("videoUrl"));
            const QString requestedFile = args.value("videoPath").toString();
            const QString internalPath = args.value("_mediaPath").toString();
            const QString iniPath = Rk3566Platform::netConfigPath();
            const bool independentPic = !internalPath.isEmpty() && MediaContentPolicy::isInDirectory(
                        internalPath, MediaContentPolicy::pictureDir(Rk3566Platform::staticRoot()));
            const bool filteredFile = !requestedFile.isEmpty() && !MediaContentPolicy::allows(
                        MediaContentPolicy::contentType(iniPath), requestedFile);
            const bool selectionOk = (!args.contains("contentType") || MediaContentPolicy::contentType(iniPath) ==
                                      MediaContentPolicy::normalizeContentType(args.value("contentType").toString())) &&
                    (!args.contains("smallImageMode") || MediaContentPolicy::smallImageMode(iniPath) ==
                     MediaContentPolicy::smallImageMode(args.value("smallImageMode")));
            const bool fileOk = requestedFile.isEmpty() || independentPic || filteredFile ||
                                (state.value("videoPath") == args.value("videoPath") &&
                                 state.value("playbackState") == "PLAYING");
            if ((modeOk && volumeOk && liveOk && fileOk && selectionOk) || ++*attempts >= 40) {
                timer->stop();
                timer->deleteLater();
                mediaBusy_ = false;
                complete(token,
                         state,
                         modeOk && volumeOk && liveOk && fileOk && selectionOk ? 200 : 504,
                         "播放器未在期限内达到请求状态，请核对当前实际状态");
            }
        });
        timer->start();
        return;
    }
    if (operation == "floorSchedule") {
        if (!floorScheduleOwner_ || !floorScheduleApply_)
            complete(token, {}, 503, QStringLiteral("定时梯控执行器尚未就绪"));
        else floorScheduleApply_(token, args);
        return;
    }
    if (operation == "networkConfig") {
        if (!networkOwner_ || !networkApply_)
            complete(token, {}, 503, QStringLiteral("本地网络配置入口尚未就绪"));
        else
            networkApply_(token, args);
        return;
    }
    emit deviceRequested(token, operation, args);
}

/**
 * @brief 接收已有界面合成信号，并解码当前等待中的截图。
 * @param base64 现有截图信号提供的 Base64 图像数据。
 */
void WebLocalRuntime::encodedImage(const QString &base64)
{
    if (captureToken_.isEmpty())
        return;
    auto bytes = base64.toLatin1();
    if (bytes.startsWith("data:"))
        bytes = bytes.mid(bytes.indexOf(',') + 1);
    image(captureToken_, QImage::fromData(QByteArray::fromBase64(bytes)));
}

/**
 * @brief 校验当前图像并交给线程池编码落盘，随后返回小型结果元数据。
 * @param token 贯穿请求与异步结果的任务标识。
 * @param image 待编码的界面图像。
 */
void WebLocalRuntime::image(const QString &token, const QImage &image)
{
    if (!output_.contains(token))
        return;
    const QString path = output_.value(token);
    if (path.isEmpty())
        return; // This capture is already being encoded.
    if (image.isNull() || path.isEmpty() || qint64(image.width()) * image.height() > 16777216) {
        if (captureToken_ == token)
            captureToken_.clear();
        output_.remove(token);
        complete(token, {}, 503, "当前界面图像不可用");
        return;
    }
    output_[token].clear();
    const QPointer<WebLocalRuntime> self(this);

    // 主线程仅持有图像快照；JPEG 编码和原子写盘交给线程池。
    QThreadPool::globalInstance()->start(new Work([self, token, path, image]() {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        const bool encoded = image.save(&buffer, "JPG", 80) && bytes.size() <= 6 * 1024 * 1024;
        QSaveFile file(path);
        const bool saved = encoded && file.open(QIODevice::WriteOnly) &&
                           file.write(bytes) == bytes.size() && file.commit();
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, token, path, saved]() {
                    if (!self)
                        return;
                    if (!self->output_.remove(token)) {
                        QFile::remove(path);
                        return;
                    }
                    if (self->captureToken_ == token)
                        self->captureToken_.clear();
                    if (!saved) {
                        self->complete(token, {}, 503, "截图编码或保存失败");
                        return;
                    }
                    self->complete(
                        token,
                        {{"assetPath", path},
                         {"timestamp", QDateTime::currentDateTime().toString(Qt::ISODate)}},
                        saved ? 200 : 503,
                        "截图编码或保存失败");
                },
                Qt::QueuedConnection);
    }));
}
