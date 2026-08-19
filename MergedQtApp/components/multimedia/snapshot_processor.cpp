/**
 * @file snapshot_processor.cpp
 * @brief 在工作线程中合成并编码媒体抓拍的处理器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "snapshot_processor.h"


namespace {

/** @brief 将图像编码为 JPEG Base64，失败时返回具体原因。 */
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

} // namespace


/** @brief 将视频图像绘制到 UI 指定区域后编码为 Base64。 */
void SnapshotProcessor::processSnapshot(const QImage &uiImage,
                     const QImage &videoImage,
                     const QRect &videoRect)
{
    if (uiImage.isNull()) {
        emit failed(QStringLiteral("uiImage is null"));
        return;
    }

    if (videoImage.isNull()) {
        emit failed(QStringLiteral("videoImage is null"));
        return;
    }

    if (!videoRect.isValid()) {
        emit failed(QStringLiteral("videoRect is invalid"));
        return;
    }

    // 1) 合成：把视频帧贴回整屏截图中的视频区域
    QImage finalImage(uiImage.size(), QImage::Format_ARGB32_Premultiplied);
    finalImage.fill(Qt::transparent);
    if (finalImage.isNull()) {
        emit failed(QStringLiteral("finalImage copy failed"));
        return;
    }

    {
        QPainter painter(&finalImage);

        // 1. 先画视频到底层
        painter.drawImage(videoRect, videoImage);
        // 2. 再画 UI 到上层
        painter.drawImage(0, 0, uiImage);
        painter.end();
    }

    QString base64String;
    QString reason;
    if (!encodeSnapshotToBase64(finalImage, &base64String, &reason)) {
        emit failed(reason);
        return;
    }

    emit finished(base64String);
}

/** @brief 仅编码完整 UI 图像为 Base64。 */
void SnapshotProcessor::processUiOnlySnapshot(const QImage &uiImage)
{
    QString base64String;
    QString reason;

    if (!encodeSnapshotToBase64(uiImage, &base64String, &reason)) {
        emit failed(reason);
        return;
    }

    emit finished(base64String);
}
