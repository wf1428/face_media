/**
 * @file company_play_info.cpp
 * @brief 高清UI像素/视频值与普通截图共用后台合成编码，按容量异步提交上传。
 * @author Du lin
 * @note 最近修改：2026-11-04；修改人：Du lin；高清直接接收原DPR的UI像素及NV12值，取消UI命令回放约定；后台视频合成/JPEG/Base64/JSON及有界上传流程保持。
 */
#include "company_play_info.h"
#include "common/snapshot_work.h"
#include <QBuffer>
#include <QDateTime>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QUuid>
namespace {
constexpr int kMaxJpegBytes=1000000,kMaxRequests=4; /**< 原JPEG字节上限/总请求数上限，包含活动请求。 */
/** @brief 构造原协议信封；正常编码在后台，满队列的小型失败回执在GUI生成。
 * @param[in] packet 原请求状态信封值，保留ID和主题。
 * @param[in] image 完整data URI；失败时为空。
 * @param[in] reason 可空，非空追加既有deviceLog字段。
 * @return 紧凑JSON加换行，沿用原IPC协议。
 */
QByteArray responseLine(QJsonObject packet, const QString &image, const QString &reason)
{
    auto payload = packet.value("payload").toObject();
    auto data = payload.value("data").toObject();
    auto info = data.value("info").toObject();
    info["base64Image"] = image;
    if (!reason.isEmpty()) {
        const QString previous = info.value("deviceLog").toString();
        info["deviceLog"] = previous.isEmpty() ? reason : previous + QLatin1Char('\n') + reason;
    }
    data["info"] = info;
    payload["data"] = data;
    payload["time"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    packet["payload"] = payload;
    return QJsonDocument(packet).toJson(QJsonDocument::Compact) + '\n';
}

/** @brief 后台原尺寸合成/JPEG/Base64/JSON；只操作值，取消在编码尝试间检查。
 * @param[in] packet 保留原ID/主题的状态信封。
 * @param[in] ui 只读UI/场景像素值，空图返回失败回执。
 * @param[in] video 可空的兼容路径视频像素值。
 * @param[in] rect video在ui中的像素目标区域，视频非空时必须有效。
 * @param[in] initialReason 已知失败原因，非空时不执行图像编码。
 * @param[in] cancelled 对象析构时置true，在编码尝试之间取消。
 * @return 原协议成功/失败信封的换行JSON。
 */
QByteArray encodeCompany(const QJsonObject &packet,const QImage &ui,const QImage &video,const QRect &rect,
                         const QString &initialReason,const std::shared_ptr<std::atomic_bool> &cancelled) {
        QString reason = initialReason;
        QImage image = ui;
        image.setDevicePixelRatio(1.0);
        if (reason.isEmpty() && image.isNull())
            reason = QStringLiteral("高清截图失败：UI 图像为空");
        if (reason.isEmpty() && !video.isNull()) {
            if (!rect.isValid()) {
                reason = QStringLiteral("高清截图失败：视频矩形无效");
            } else {
                QImage composite(image.size(), QImage::Format_ARGB32_Premultiplied);
                if (composite.isNull()) {
                    reason = QStringLiteral("高清截图失败：合成图像分配失败");
                } else {
                    composite.fill(Qt::transparent);
                    QPainter painter(&composite);
                    painter.drawImage(rect, video);
                    painter.drawImage(0, 0, image);
                    painter.end();
                    image = composite;
                }
            }
        }

        QByteArray jpeg;
        int quality = 95;
        if (reason.isEmpty()) {
            for (;;) {
                if (cancelled->load()) {
                    reason = QStringLiteral("高清截图已取消");
                    break;
                }
                jpeg.clear();
                QBuffer buffer(&jpeg);
                if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPG", quality)) {
                    reason = QStringLiteral("高清截图失败：JPEG 编码失败");
                    break;
                }
                if (jpeg.size() <= kMaxJpegBytes)
                    break;
                if (quality == 1) {
                    reason = QStringLiteral("高清截图失败：原尺寸 JPEG 在最低质量下仍超过 1 MB");
                    break;
                }
                quality = quality > 5 ? quality - 5 : 1;
            }
        }
        const QString base64 = reason.isEmpty()
                ? QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64())
                : QString();
        return responseLine(packet, base64, reason);
}
}
CompanyPlayInfo::CompanyPlayInfo(QObject *parent):QObject(parent) {
    captureTimer_.setSingleShot(true);captureTimer_.setInterval(3000);
    connect(&captureTimer_,&QTimer::timeout,this,[this]{submitFrame(active_.token,QImage(),QImage(),QRect(),QStringLiteral("高清截图失败：抓取超时"));});
    uploadTimer_.setSingleShot(true);uploadTimer_.setInterval(50);connect(&uploadTimer_,&QTimer::timeout,this,&CompanyPlayInfo::tryPublish);
}
CompanyPlayInfo::~CompanyPlayInfo(){cancelled_->store(true);captureTimer_.stop();uploadTimer_.stop();}
void CompanyPlayInfo::enqueue(const QJsonObject &packet) {
    if(pending_.size()+int(!active_.token.isEmpty())>=kMaxRequests){const auto line=responseLine(packet,QString(),QStringLiteral("高清截图失败：请求队列已满"));emit overflowReplyReady(line);emit publishReady(line);return;}
    pending_.enqueue({QUuid::createUuid().toString(QUuid::WithoutBraces),packet});startNext();
}
void CompanyPlayInfo::startNext(){if(!active_.token.isEmpty() || pending_.isEmpty())return;active_=pending_.dequeue();captureTimer_.start();emit captureRequested(active_.token);}
void CompanyPlayInfo::submitFrame(const QString &token,const QImage &uiImage,const QImage &videoImage,const QRect &videoRect,const QString &reason) {
    queueFrame(token,SnapshotScene(),false,uiImage,videoImage,videoRect,reason);
}
void CompanyPlayInfo::submitScene(const QString &token,const SnapshotScene &scene) {
    queueFrame(token,scene,true,QImage(),QImage(),QRect(),scene.valid()?QString():scene.error());
}
void CompanyPlayInfo::queueFrame(const QString &token,const SnapshotScene &scene,bool useScene,const QImage &uiImage,
                                 const QImage &videoImage,const QRect &videoRect,const QString &reason) {
    if(token.isEmpty() || token!=active_.token || processing_ || !readyLine_.isEmpty())return;
    captureTimer_.stop();processing_=true;
    const auto packet=active_.packet;const auto cancelled=cancelled_;const QPointer<CompanyPlayInfo> alive(this);auto result=std::make_shared<QByteArray>();
    SnapshotWork::queue().enqueue([=] {
        SerialWorkQueue::Task task;task.reportSlow=false;
        task.run=[sceneValue=scene,uiValue=uiImage,videoValue=videoImage,packet,useScene,reason,videoRect,cancelled,result]() mutable {
            // 后台移走UI像素/视频大值，run结束前销毁，避免完成回调在UI释放在途像素。
            auto ownedScene=std::move(sceneValue);auto ui=std::move(uiValue);auto video=std::move(videoValue);
            if(cancelled->load())return QString();
            QString error=reason;const QImage image=useScene?ownedScene.rasterize({},&error):ui;
            *result=encodeCompany(packet,image,video,videoRect,error,cancelled);return QString();};
        task.done=[alive,token,result,packet](const QString &error) {if(!alive || alive->active_.token!=token)return;alive->processing_=false;alive->readyLine_=error.isEmpty()?*result:responseLine(packet,QString(),error);alive->tryPublish();};return task;
    },QString(),[alive,cancelled]{return !alive || cancelled->load();});
}
void CompanyPlayInfo::tryPublish() {
    if(readyLine_.isEmpty())return;
    if(publisher_ && !publisher_(readyLine_)){uploadTimer_.start();return;}
    const auto line=std::move(readyLine_);readyLine_.clear();const QPointer<CompanyPlayInfo> alive(this);emit publishReady(line);if(!alive)return;active_=Request();startNext();
}
