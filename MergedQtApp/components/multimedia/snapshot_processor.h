/**
 * @file snapshot_processor.h
 * @brief 在工作线程中合成并编码媒体抓拍的处理器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SNAPSHOTPROCESSOR_H
#define SNAPSHOTPROCESSOR_H

#include <QObject>
#include <QImage>
#include <QRect>
#include <QBuffer>
#include <QPainter>
#include <QString>

/** @brief 在工作线程中合成并编码媒体抓拍的处理器。 */
class SnapshotProcessor : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建无状态抓拍处理器。 */
    explicit SnapshotProcessor(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    /** @brief 将视频图像绘制到 UI 指定区域后编码为 Base64。 */
    void processSnapshot(const QImage &uiImage, const QImage &videoImage, const QRect &videoRect);

    /** @brief 仅编码完整 UI 图像为 Base64。 */
    void processUiOnlySnapshot(const QImage &uiImage);


signals:
    /** @brief 输出编码完成的 Base64 图片文本。 */
    void finished(const QString &base64);

    /** @brief 输出图像为空或编码失败原因。 */
    void failed(const QString &reason);
};

#endif // SNAPSHOTPROCESSOR_H
