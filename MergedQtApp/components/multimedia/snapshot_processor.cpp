/**
 * @file snapshot_processor.cpp
 * @brief 普通截图后台视频转换/合成/JPEG编码，复用高清单一执行线程。
 * @author Du lin
 * @note 最近修改：2026-11-04；修改人：Du lin；普通截图继续在共享后台转换视频/合成/编码，输入改为UI已绘制的小图像素；同步取消命令回放的文档约定，编码参数及信号保持。
 */
#include "snapshot_processor.h"
#include "common/snapshot_work.h"
#include <QPointer>
#include <memory>
namespace {
/** @brief 后台JPEG/Base64编码，保留原普通截图尺寸和质量。
 * @param[in] sourceImage 只读源图像，空图失败。
 * @param[out] base64String 可空，成功写完整data URI，失败保持原值。
 * @param[out] reason 可空，失败写具体原因。
 * @return true完整编码，false空图/缩放/缓冲打开/JPEG失败。
 */
static bool encodeSnapshotToBase64(const QImage &sourceImage, QString *base64String, QString *reason)
{
    if (sourceImage.isNull()) {
        if (reason) *reason = QStringLiteral("sourceImage is null");
        return false;
    }

    QImage uploadImage = sourceImage.scaled(
        560, 360,
        Qt::KeepAspectRatio,
        Qt::FastTransformation
    );

    if (uploadImage.isNull()) {
        if (reason) *reason = QStringLiteral("uploadImage scaled failed");
        return false;
    }

    QByteArray imageBytes;
    QBuffer buffer(&imageBytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (reason) *reason = QStringLiteral("QBuffer open failed");
        return false;
    }

    if (!uploadImage.save(&buffer, "JPG", 35)) {
        if (reason) *reason = QStringLiteral("save JPG failed");
        return false;
    }

    if (base64String) {
        *base64String = QStringLiteral("data:image/jpeg;base64,") +
                        QString::fromLatin1(imageBytes.toBase64());
    }

    return true;
}


/** @brief GUI提交一个只操作值的后台动作，完成/失败回到仍存活的处理器。
 * @param[in] owner GUI所有的处理器，排队及完成时检查寿命。
 * @param[in] create 后台生成图像，空图走原编码失败，不访问QWidget。
 */
void submit(SnapshotProcessor *owner,std::function<QImage()> create) {
    auto result=std::make_shared<QString>();const QPointer<SnapshotProcessor> alive(owner);
    SnapshotWork::queue().enqueue([alive,result,create] {
        SerialWorkQueue::Task task;task.reportSlow=false;
        task.run=[result,create]() mutable {auto work=std::move(create);QString error;encodeSnapshotToBase64(work(),result.get(),&error);work={};return error;};
        task.done=[alive,result](const QString &error) {if(!alive)return;if(error.isEmpty())emit alive->finished(*result);else emit alive->failed(error);};return task;
    },QStringLiteral("normal-%1").arg(quintptr(owner)),[alive]{return !alive;});
}
}
void SnapshotProcessor::processUiOnlySnapshot(const QImage &uiImage) { submit(this,[uiImage]{return uiImage;}); }
void SnapshotProcessor::processScene(const SnapshotScene &scene) { submit(this,[scene]{return scene.rasterize(QSize(560,360));}); }
void SnapshotProcessor::processSnapshot(const QImage &uiImage,const QImage &videoImage,const QRect &videoRect) {
    submit(this,[uiImage,videoImage,videoRect] {
        if(uiImage.isNull() || videoImage.isNull() || !videoRect.isValid())return QImage();
        QImage image(uiImage.size(),QImage::Format_ARGB32_Premultiplied);if(image.isNull())return QImage();image.fill(Qt::transparent);
        {QPainter p(&image);p.drawImage(videoRect,videoImage);p.drawImage(0,0,uiImage);}return image;
    });
}
