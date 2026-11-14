/**
 * @file camera_screenshot_upload.cpp
 * @brief 实现摄像头协议、按需新帧采样、单次后台编码和拥塞跳帧。
 * @author Du lin
 * @note 最近修改：2026-11-10；修改人：Du lin；复用统一设备开关解析，提取摄像头方法分类以便禁用时静默忽略。
 * @date 2026-11-05
 */
#include "camera_screenshot_upload.h"
#include "common/camera_screenshot_source.h"
#include "common/device_feature_switches.h"
#include "common/snapshot_work.h"
#include "platform/rk3566_platform.h"
#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QUuid>
#include <limits>
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif
namespace {
constexpr qint64 MiB=1024*1024; /**< 本地策略中的 1 MB 按 1 MiB 计。 */
constexpr qint64 MinFreeBytes=64*MiB; /**< 本模块停止新增文件的卷剩余空间下限。 */
constexpr int MaxBytes=1000000,TimeoutMs=3000; /**< 最终IPC信封字节上限及单次取帧等待毫秒。 */
/** @brief 校验并规范化设备支持的 JPEG 质量标识，不改变像素尺寸。
 * @param[in] value 质量字符串，允许首尾空白和大小写差异。
 * @return ORIG 或 PERC:1..100；缺省或无效时为空。
 */
QString normalizedQuality(const QJsonValue &value) {
    const QString text=value.toString().trimmed().toUpper();
    if(text=="ORIG")return text;
    if(!text.startsWith("PERC:"))return {};
    bool ok=false;const int number=text.mid(5).toInt(&ok);
    return ok && number>=1 && number<=100 ? QStringLiteral("PERC:%1").arg(number) : QString();
}
/** @brief 解析单次和普通订阅的上报质量。
 * @param[in] value 顶层 quality，省略、null 或空白时使用物业缺省。
 * @return 缺省 PERC:15，有效值规范化，无效非空值回退 ORIG。
 */
QString qualityName(const QJsonValue &value) {
    if(value.isUndefined() || value.isNull() || (value.isString() && value.toString().trimmed().isEmpty()))
        return QStringLiteral("PERC:15");
    const auto quality=normalizedQuality(value);
    return quality.isEmpty() ? QStringLiteral("ORIG") : quality;
}
/** @brief 选择订阅上传质量；存储开关及容量字段由原存储主体处理。
 * @param[in] data setScreenshotConfig 的订阅参数。
 * @return CALL 固定 ORIG，其他订阅优先有效的 cloudQuality，否则使用顶层 quality。
 */
QString subscriptionQuality(const QJsonObject &data) {
    if(data.value("event").toString()=="CALL")return QStringLiteral("ORIG");
    const auto policy=data.value("storagePolicy").toObject();
    const auto cloudQuality=normalizedQuality(policy.value("cloudQuality"));
    return cloudQuality.isEmpty() ? qualityName(data.value("quality")) : cloudQuality;
}
/** @brief 使用指定质量将相机原尺寸图像编码为 JPEG。
 * @param[in] image 有效的自有像素。
 * @param[in] quality 已规范化的 ORIG 或 PERC:1..100。
 * @return JPEG 字节；失败为空。
 */
QByteArray encodeJpeg(const QImage &image,const QString &quality) {
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    const int level=quality=="ORIG" ? 95 : quality.mid(5).toInt();
    if(!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer,"JPG",level))return {};
    return jpeg;
}
/** @brief 构造摄像头专用信封；成功时base64Image为JPEG data URI，失败省略该字段。
 * @param[in] topic 精确设备event主题。
 * @param[in] device 精确设备ID。
 * @param[in] id 单次原请求ID或周期唯一ID。
 * @param[in] quality 实际编码质量标识。
 * @param[in] ts 实际采集/失败时刻epoch毫秒。
 * @param[in] seq 0省略，正数为订阅序号。
 * @param[in] jpeg 自有JPEG字节，失败为空。
 * @param[in] reason 失败说明，空为成功。
 * @return 完整紧凑IPC JSON行；不意味着已写入mqttd或平台。
 */
QByteArray packet(const QString &topic,const QString &device,const QString &id,const QString &quality,
                  qint64 ts,int seq,const QByteArray &jpeg,const QString &reason) {
    QJsonObject info{{"mime","image/jpeg"},{"quality",quality},{"ts",ts}};
    if(seq>0)info.insert("seq",seq);
    if (reason.isEmpty()) {
        info.insert("base64Image", QStringLiteral("data:image/jpeg;base64,") +
                                  QString::fromLatin1(jpeg.toBase64()));
    } else {
        info.insert("deviceLog", reason);
    }
    const QJsonObject payload{{"method","screenshot.push"},{"id",id},{"deviceId",device},
        {"data",QJsonObject{{"info",info}}},{"time",QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")}};
    return QJsonDocument(QJsonObject{{"cmd","publish"},{"topic",topic},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';
}
}
/** @brief 本地队列独占的索引，按采集时间从旧到新淘汰。
 * @details 仅管理本模块命名的 JPEG。首次或新策略扫描，正常逐帧写盘使用内存容量。
 */
struct CameraScreenshotUpload::LocalStore {
    Cancel indexed; /**< 完成扫描的策略代次；扫描失败后下一次可重试。 */
    QByteArray indexedDevice; /**< 本次索引所属文件系统的设备标识。 */
    QMap<QPair<qint64,QString>,qint64> files; /**< 采集毫秒及文件名到已落盘字节数。 */
    qint64 used=0; /**< files 的容量合计，字节。 */
    qint64 lastErrorMs=0; /**< 上次错误日志时刻，用于限频。 */

    /** @brief 恢复已落盘截图的容量回收顺序。
     * @param[in] file 本地目录中的普通文件。
     * @return 旧名的采集毫秒；新名用分钟及该分钟内的文件修改时间；非截图返回 -1。
     */
    static qint64 savedTime(const QFileInfo &file) {
        static const QRegularExpression pattern(QStringLiteral("^([0-9]{12}|[0-9]{16})-[a-f0-9]{32}\\.jpg$"));
        const auto match=pattern.match(file.fileName());
        if(!match.hasMatch())return -1;
        const auto prefix=match.captured(1);
        if(prefix.size()==16)return prefix.toLongLong();
        const auto minute=QDateTime::fromString(prefix,QStringLiteral("yyyyMMddHHmm"));
        if(!minute.isValid())return -1;
        const auto start=minute.toMSecsSinceEpoch();
        const auto modified=file.lastModified();
        return modified.isValid() ? qBound(start,modified.toMSecsSinceEpoch(),start+59999) : start;
    }

    /** @brief 在后台检查目录、按配额删除旧图并原子写入当前图。
     * @param[in] policy 当前目录、限制和取消令牌。
     * @param[in] image 需本地编码的像素，或空。
     * @param[in] sharedJpeg 可复用的上传 JPEG，或空。
     * @param[in] ts 采集时刻 epoch 毫秒。
     * @param[in] id 帧 UUID，空表示只检查配额。
     * @return 空表示完成或取消，非空表示本地错误。
     */
    QString write(const LocalPolicy &policy,const QImage &image,const QByteArray &sharedJpeg,
                  qint64 ts,const QString &id) {
        if(policy.cancel->load())return {};
        const QString root=Rk3566Platform::storageRoot();
        if(!QFileInfo(root).isDir()) {
            indexed.reset();
            return QStringLiteral("截图存储根目录不可用");
        }
        QStorageInfo volume(root);
        if(!volume.isValid() || !volume.isReady() || volume.isReadOnly()) {
            indexed.reset();
            return QStringLiteral("截图存储卷不可写");
        }
        const QDir dir(policy.directory);
        const QFileInfo directory(dir.absolutePath());
        if(directory.isSymLink() || QFileInfo(QDir(root).filePath(QStringLiteral("camera_screenshots"))).isSymLink())
            return QStringLiteral("截图目录不允许使用符号链接");
        if(!directory.isDir()) {
            indexed.reset();
            if(!QDir(root).mkpath(dir.absolutePath()))return QStringLiteral("无法创建截图目录");
        }

        if(indexed!=policy.cancel || indexedDevice!=volume.device()) {
            files.clear();used=0;
            QDirIterator entries(dir.absolutePath(),QDir::Files|QDir::NoSymLinks);
            while(entries.hasNext()) {
                if(policy.cancel->load())return {};
                entries.next();
                const auto file=entries.fileInfo();
                const auto captured=savedTime(file);
                if(captured<0)continue;
                const auto bytes=file.size();
                files.insert(qMakePair(captured,file.fileName()),bytes);used+=bytes;
            }
            indexed=policy.cancel;indexedDevice=volume.device();
        }

        QByteArray jpeg=sharedJpeg;
        if(!image.isNull() && jpeg.isEmpty())jpeg=encodeJpeg(image,policy.quality);
        if(policy.cancel->load())return {};
        if(!id.isEmpty() && jpeg.isEmpty())return QStringLiteral("本地 JPEG 编码失败");
        const auto minute=QDateTime::fromMSecsSinceEpoch(ts).toString(QStringLiteral("yyyyMMddHHmm"));
        const auto name=QStringLiteral("%1-%2.jpg").arg(minute,id);
        const auto key=qMakePair(ts,name);
        const bool save=!id.isEmpty() && jpeg.size()<=policy.maxBytes;
        while(!files.isEmpty() && (used>policy.maxBytes ||
              (save && used+jpeg.size()>policy.maxBytes && files.firstKey()<key))) {
            if(policy.cancel->load())return {};
            auto oldest=files.begin();
            const auto path=dir.filePath(oldest.key().second);
            if(!QFile::remove(path) && QFileInfo::exists(path))return QStringLiteral("无法回收最旧截图");
            used-=oldest.value();files.erase(oldest);
        }
        if(!save || used+jpeg.size()>policy.maxBytes || policy.cancel->load())return {};
        if(volume.bytesAvailable()<MinFreeBytes+jpeg.size()) {
            volume.refresh();
            if(volume.bytesAvailable()<MinFreeBytes+jpeg.size())return QStringLiteral("截图卷可用空间不足");
        }
        QSaveFile file(dir.filePath(name));
        if(!file.open(QIODevice::WriteOnly) || file.write(jpeg)!=jpeg.size())
            return QStringLiteral("本地截图写入失败");
        if(policy.cancel->load())return {};
        if(!file.commit())return QStringLiteral("本地截图提交失败");
        used+=jpeg.size();files.insert(key,jpeg.size());
        return {};
    }
};

CameraScreenshotUpload::CameraScreenshotUpload(bool allowed,QObject *parent):QObject(parent),allowed_(allowed) {
    clock_.start();timer_.setInterval(25);
    connect(&timer_,&QTimer::timeout,this,&CameraScreenshotUpload::poll);
}
CameraScreenshotUpload::~CameraScreenshotUpload() {stop();}
bool CameraScreenshotUpload::configEnabled(const QSettings &settings) {
    return DeviceFeatureSwitches::readFlag(settings,QStringLiteral("cameraScreenshot/enabled"));
}
void CameraScreenshotUpload::setTransport(std::function<bool()> ready,Publisher publisher) {
    ready_=std::move(ready);publisher_=std::move(publisher);
}
void CameraScreenshotUpload::configure(const QString &eventTopic) {
    if(eventTopic_==eventTopic)return;
    stop();eventTopic_.clear();deviceId_.clear();seen_.clear();
    const auto parts=eventTopic.split('/');
    if(parts.size()!=4 || parts[0]!="device" || parts[1]!="ycLinux" || parts[3]!="event" ||
       parts[2].isEmpty() || parts[2].contains('+') || parts[2].contains('#'))return;
    eventTopic_=eventTopic;deviceId_=parts[2];
}
void CameraScreenshotUpload::setOnline(bool online) {online_=online;if(!online)stop();}
bool CameraScreenshotUpload::handlesMethod(const QString &method) {
    return method=="getScreenshot" || method=="setScreenshotConfig";
}
bool CameraScreenshotUpload::handle(const QString &topic,const QJsonObject &payload) {
    const QString method=payload.value("method").toString();
    if(!handlesMethod(method))return false;
    if(!allowed_ || !online_ || deviceId_.isEmpty() || topic!=QStringLiteral("device/ycLinux/%1/request").arg(deviceId_) ||
       payload.value("deviceId").toString()!=deviceId_ || !payload.value("data").isObject())return true;
    const QString id=payload.value("id").toString();
    if(id.isEmpty() || id.size()>128)return true;
    const auto data=payload.value("data").toObject();
    // 补发关闭必须幂等执行，即使平台沿用此前启用ID，也不能被去重拦截。
    if(method=="setScreenshotConfig" && !data.value("enabled").toBool(false)) {stop();return true;}
    if(seen_.contains(id))return true;
    seen_.append(id);if(seen_.size()>64)seen_.removeFirst();
    if(cancelled_->load())cancelled_=std::make_shared<std::atomic_bool>(false);
    if(method=="getScreenshot") {
        const Request request{id,qualityName(data.value("quality")),clock_.elapsed(),QDateTime::currentMSecsSinceEpoch(),0};
        if(pending_.size()+int(!active_.id.isEmpty() && active_.seq==0)>=4)fail(request,QStringLiteral("摄像头截图失败：请求繁忙"));
        else pending_.enqueue(request);
    } else {
        intervalMs_=qBound(200,data.value("intervalMs").toInt(1000),60000);
        quality_=subscriptionQuality(data);
        configureLocal(data.value("storagePolicy"));
        if(!subscribed_) {subscribed_=true;seq_=0;nextMs_=clock_.elapsed();}
    }
    updateDemand();poll();return true;
}
void CameraScreenshotUpload::stop() {
    cancelled_->store(true);timer_.stop();subscribed_=false;pending_.clear();active_=Request();seq_=0;
    if(local_.cancel)local_.cancel->store(true);
    local_=LocalPolicy();localCaptureAfter_=0;localQuotaPending_=false;
    CameraScreenshotSource::instance().setDemand(false);
}
void CameraScreenshotUpload::configureLocal(const QJsonValue &policy) {
    const auto object=policy.toObject();
    const auto enabled=object.value("localEnabled");
    static const QRegularExpression safeDevice(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    LocalPolicy next;
    // 旧平台省略整个策略时继续只上传。新策略省略 localEnabled 时默认启用。
    if(policy.isObject() && (enabled.isUndefined() || enabled.toBool(false)) &&
       safeDevice.match(deviceId_).hasMatch()) {
        next.directory=QDir(Rk3566Platform::storageRoot()).filePath(QStringLiteral("camera_screenshots/")+deviceId_);
        next.maxBytes=qBound(10,object.value("localMaxSizeMB").toInt(512),10240)*MiB;
        next.quality=normalizedQuality(object.value("localQuality"));
        if(next.quality.isEmpty())next.quality=quality_;
    }
    if(next.directory==local_.directory && next.maxBytes==local_.maxBytes && next.quality==local_.quality)return;
    if(local_.cancel)local_.cancel->store(true);
    if(!next.directory.isEmpty())next.cancel=std::make_shared<std::atomic_bool>(false);
    local_=std::move(next);
    localCaptureAfter_=0;
    localQuotaPending_=bool(local_.cancel);
}

void CameraScreenshotUpload::storeLocal(const QImage &image,const QByteArray &jpeg,qint64 ts,const QString &id) {
    if(!local_.cancel || (localWork_ && localWork_->busy()))return;
    if(!localWork_) {
        localStore_=std::make_shared<LocalStore>();
        localWork_.reset(new SerialWorkQueue(QStringLiteral("camera.local-storage")));
    }
    const auto policy=local_;const auto store=localStore_;
    localQuotaPending_=false;
    localWork_->enqueue([policy,store,image,jpeg,ts,id] {
        return SerialWorkQueue::Task{[policy,store,image,jpeg,ts,id] {
#ifdef Q_OS_LINUX
            // Linux nice 为线程属性，仅调低本地编码/写盘工作线程。
            static thread_local const int priorityResult=setpriority(PRIO_PROCESS,0,10);
            Q_UNUSED(priorityResult);
#endif
            const auto error=store->write(policy,image,jpeg,ts,id);
            const auto now=QDateTime::currentMSecsSinceEpoch();
            if(!error.isEmpty() && !policy.cancel->load() && now-store->lastErrorMs>=30000) {
                store->lastErrorMs=now;
                qWarning() << "[camera-storage]" << error << policy.directory;
            }
            return QString();
        },{}};
    });
}

void CameraScreenshotUpload::captureLocalOnly() {
    if(!localCaptureAfter_ || !local_.cancel || (localWork_ && localWork_->busy()))return;
    QImage image;qint64 ts=0;
    if(!CameraScreenshotSource::instance().take(&image,&ts) || ts<localCaptureAfter_)return;
    localCaptureAfter_=0;
    if(!image.isNull())storeLocal(image,{},ts,QUuid::createUuid().toString(QUuid::Id128));
}

void CameraScreenshotUpload::updateDemand() {
    const bool wanted=allowed_ && online_ && (subscribed_ || !pending_.isEmpty() || !active_.id.isEmpty());
    CameraScreenshotSource::instance().setDemand(wanted);
    if(wanted && !timer_.isActive())timer_.start();
    if(!wanted)timer_.stop();
}
void CameraScreenshotUpload::fail(const Request &request,const QString &reason) {
    if(!allowed_ || !online_ || cancelled_->load() || !publisher_)return;
    publisher_(packet(eventTopic_,deviceId_,request.id,request.quality,QDateTime::currentMSecsSinceEpoch(),request.seq,{},reason),cancelled_);
}
void CameraScreenshotUpload::poll() {
    if(!allowed_ || !online_ || cancelled_->load() || encoding_)return;
    if(localQuotaPending_)storeLocal({},{},0,{});
    const qint64 now=clock_.elapsed();
    if(active_.id.isEmpty()) {
        if(!pending_.isEmpty())active_=pending_.dequeue();
        else if(subscribed_ && now>=nextMs_) {
            nextMs_=now+intervalMs_;
            const bool canUpload=ready_ && ready_() && !SnapshotWork::queue().busy();
            if(!canUpload && !local_.cancel)return;
            const auto captureAfter=QDateTime::currentMSecsSinceEpoch();
            localCaptureAfter_=local_.cancel ? captureAfter : 0;
            if(!canUpload) {
                captureLocalOnly();
                return;
            }
            seq_=seq_==std::numeric_limits<int>::max()?1:seq_+1;
            active_={QUuid::createUuid().toString(QUuid::WithoutBraces),quality_,now,captureAfter,seq_};
        } else {captureLocalOnly();updateDemand();return;}
    }
    if(now-active_.queuedMs>=TimeoutMs) {
        fail(active_,QStringLiteral("摄像头截图失败：取帧或上传繁忙超时"));active_=Request();updateDemand();return;
    }
    if(!ready_ || !ready_() || SnapshotWork::queue().busy()) {
        if(active_.seq>0) {active_=Request();captureLocalOnly();updateDemand();}
        return;
    }
    QImage image;qint64 ts=0;
    if(!CameraScreenshotSource::instance().take(&image,&ts))return;
    if(ts<active_.captureAfter)return; // 不复用此前订阅或请求遗留的旧帧。
    encode(image,ts,image.isNull()?QStringLiteral("摄像头截图失败：帧转换失败"):QString());
}
void CameraScreenshotUpload::encode(const QImage &image,qint64 ts,const QString &reason) {
    encoding_=true;
    const auto request=active_;const auto cancel=cancelled_;const auto topic=eventTopic_;const auto device=deviceId_;
    auto result=std::make_shared<QByteArray>();const QPointer<CameraScreenshotUpload> alive(this);
    Cancel localCancel;
    if(request.seq>0 && localCaptureAfter_) {
        localCaptureAfter_=0;
        if(local_.cancel && !image.isNull() && (!localWork_ || !localWork_->busy())) {
            if(request.quality==local_.quality)localCancel=local_.cancel;
            else storeLocal(image,{},ts,QString(request.id).remove(QLatin1Char('-')));
        }
    }
    const auto localJpeg=localCancel ? std::make_shared<QByteArray>() : nullptr;
    SnapshotWork::queue().enqueue([=] {
        SerialWorkQueue::Task task;
        task.run=[owned=image,request,cancel,topic,device,ts,reason,result,localJpeg]() mutable {
            QImage pixels=std::move(owned);if(cancel->load())return QString();
            QString error=reason;QByteArray jpeg;
            if(error.isEmpty()) {
                jpeg=encodeJpeg(pixels,request.quality);
                if(jpeg.isEmpty())error=QStringLiteral("摄像头截图失败：JPEG编码失败");
                else if(jpeg.size()>749000)error=QStringLiteral("摄像头截图失败：原尺寸图片超过上传上限");
            }
            if(cancel->load())return QString();
            *result=packet(topic,device,request.id,request.quality,ts,request.seq,jpeg,error);
            if(result->size()>MaxBytes)*result=packet(topic,device,request.id,request.quality,ts,request.seq,{},QStringLiteral("摄像头截图失败：消息超过1MB"));
            if(localJpeg)*localJpeg=std::move(jpeg);
            return QString();
        };
        task.done=[alive,cancel,result,request,ts,localCancel,localJpeg](const QString &error) {
            if(!alive)return;
            alive->encoding_=false;
            if(cancel==alive->cancelled_ && !cancel->load()) {
                if(error.isEmpty() && alive->publisher_ && !result->isEmpty()) {
                    const bool accepted=alive->publisher_(*result,cancel);
                    if(!accepted && request.seq==0)alive->pending_.prepend(request);
                } else if(!error.isEmpty())alive->fail(request,error);
                if(localJpeg && !localJpeg->isEmpty() && localCancel==alive->local_.cancel && !localCancel->load())
                    alive->storeLocal({},*localJpeg,ts,QString(request.id).remove(QLatin1Char('-')));
                alive->active_=Request();
            }
            alive->updateDemand();
        };
        return task;
    });
}
