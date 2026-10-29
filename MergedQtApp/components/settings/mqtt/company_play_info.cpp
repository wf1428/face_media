/**
 * @file company_play_info.cpp
 * @brief 独立编码原始尺寸 JPEG、限制一 MB 上限并关联高清截图回执。
 * @author Du lin
 * @date 2026-10-24
 * @note 最近修改：2026-10-24；修改人：Du lin；移除高清编码摘要、满队列和完成时的临时日志打印，保留 JPEG 上限、原图尺寸、后台处理和失败回执语义。
 */
#include "company_play_info.h"

#include <QBuffer>
#include <QDateTime>
#include <QJsonDocument>
#include <QPainter>
#include <QThread>
#include <QUuid>

namespace {
constexpr int kMaxJpegBytes = 1000000; /**< JPEG 二进制上限，1 MB；Base64 另增加约三分之一。 */
constexpr int kMaxRequests = 4;        /**< 活动抓取/编码与等待队列合计上限。 */

/**
 * @brief 仅改写本次信封中的图像、完成时间和失败说明，不污染普通播放状态。
 * @param[in] packet 原始 publish 信封的值副本。
 * @param[in] image 含 data:image/jpeg;base64, 前缀的高清图片；失败时为空。
 * @param[in] reason 失败原因；非空时追加到本次 deviceLog。
 * @return 紧凑 JSON 加换行；普通成功回执保留所有原播放字段和原请求 id。
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
} // namespace

/**
 * @brief 拥有单笔图像值副本的后台任务，不访问 GUI、MQTT 服务或套接字。
 * @details 原尺寸 JPEG 从质量 95 开始，每次降低 5，最后尝试 1；达到上限即结束。
 *          质量 1 仍超限时失败，不缩小尺寸、不填充数据凑体积。中断在编码步骤间检查。
 */
class CompanySnapshotTask final : public QThread {
public:
    /**
     * @brief 按值持有只读请求和图像，图像由 Qt 隐式共享。
     * @param packet 含原始协议标识的状态快照。
     * @param ui 完整 UI 原始像素图。
     * @param video 可为空的视频帧。
     * @param rect 视频帧在 UI 上的实际像素矩形。
     * @param reason 非空时仅构造抓取失败回执。
     * @param parent 拥有线程对象的控制器，只能在线程结束后删除。
     */
    CompanySnapshotTask(QJsonObject packet, QImage ui, QImage video, QRect rect,
                        QString reason, QObject *parent)
        : QThread(parent), packet_(packet), ui_(ui), video_(video), rect_(rect), reason_(reason) {}

    QByteArray line; /**< run 完成后只由对象线程读取；finished 信号提供完成通知。 */

protected:
    /** @brief 后台执行合成、限次 JPEG 编码、Base64 和 JSON；任何失败都生成单笔失败回执。 */
    void run() override
    {
        QString reason = reason_;
        QImage image = ui_;
        image.setDevicePixelRatio(1.0);
        if (reason.isEmpty() && image.isNull())
            reason = QStringLiteral("高清截图失败：UI 图像为空");
        if (reason.isEmpty() && !video_.isNull()) {
            if (!rect_.isValid()) {
                reason = QStringLiteral("高清截图失败：视频矩形无效");
            } else {
                QImage composite(image.size(), QImage::Format_ARGB32_Premultiplied);
                if (composite.isNull()) {
                    reason = QStringLiteral("高清截图失败：合成图像分配失败");
                } else {
                    composite.fill(Qt::transparent);
                    QPainter painter(&composite);
                    painter.drawImage(rect_, video_);
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
                if (isInterruptionRequested()) {
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
        line = responseLine(packet_, base64, reason);
    }

private:
    QJsonObject packet_; /**< 独立的请求状态快照，不引用 service 的可变播放状态。 */
    QImage ui_;          /**< 原始 UI 像素，run 读取，不访问 QWidget。 */
    QImage video_;       /**< 可选视频帧，run 读取。 */
    QRect rect_;         /**< UI 实际像素坐标中的视频区域。 */
    QString reason_;     /**< GUI 抓取失败原因，非空时跳过编码。 */
};

CompanyPlayInfo::CompanyPlayInfo(QObject *parent) : QObject(parent)
{
    captureTimer_.setSingleShot(true);
    captureTimer_.setInterval(3000);
    connect(&captureTimer_, &QTimer::timeout, this, [this]() {
        submitFrame(active_.token, QImage(), QImage(), QRect(), QStringLiteral("高清截图失败：抓取超时"));
    });
}

CompanyPlayInfo::~CompanyPlayInfo()
{
    captureTimer_.stop();
    if (task_) {
        disconnect(task_, nullptr, this, nullptr);
        task_->requestInterruption();
        task_->wait();
    }
}

void CompanyPlayInfo::enqueue(const QJsonObject &packet)
{
    if (pending_.size() + int(!active_.token.isEmpty()) >= kMaxRequests) {
        emit publishReady(responseLine(packet, QString(), QStringLiteral("高清截图失败：请求队列已满")));
        return;
    }
    pending_.enqueue({QUuid::createUuid().toString(QUuid::WithoutBraces), packet});
    startNext();
}

void CompanyPlayInfo::startNext()
{
    if (!active_.token.isEmpty() || pending_.isEmpty())
        return;
    active_ = pending_.dequeue();
    captureTimer_.start();
    emit captureRequested(active_.token);
}

void CompanyPlayInfo::submitFrame(const QString &token, const QImage &uiImage,
                                const QImage &videoImage, const QRect &videoRect, const QString &reason)
{
    if (token.isEmpty() || token != active_.token || task_)
        return;
    captureTimer_.stop();
    task_ = new CompanySnapshotTask(active_.packet, uiImage, videoImage, videoRect, reason, this);
    connect(task_, &QThread::finished, this, [this]() {
        CompanySnapshotTask *completed = task_;
        task_ = nullptr;
        emit publishReady(completed->line);
        completed->deleteLater();
        active_ = Request();
        startNext();
    });
    task_->start(QThread::LowPriority);
}
