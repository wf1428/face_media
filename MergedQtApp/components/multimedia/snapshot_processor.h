/**
 * @file snapshot_processor.h
 * @brief 普通截图的GUI异步门面，将现成UI像素及视频值交共享后台处理。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-04；修改人：Du lin；截图输入改为直接绘制的小图像素及NV12值；更新后台职责说明，不再回放UI命令，普通尺寸/质量和异步信号保持。
 */

#ifndef SNAPSHOTPROCESSOR_H
#define SNAPSHOTPROCESSOR_H

#include <QObject>
#include <QImage>
#include <QRect>
#include <QBuffer>
#include <QPainter>
#include <QString>
#include "common/snapshot_scene.h"

/** @brief GUI所有的异步截图门面；合成/编码在普通与高清共用后台队列，信号回GUI。 */
class SnapshotProcessor : public QObject
{
    Q_OBJECT
public:
    /** @brief 在GUI创建截图门面，不创建独立线程。
     * @param[in] parent GUI所有者，可空；销毁后忽略后台完成值。
     */
    explicit SnapshotProcessor(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    /** @brief GUI排队自有图像值，后台合成/编码，成功/失败信号回GUI；不等待。
     * @param[in] uiImage 旧图像接口的UI底图，空图异步失败。
     * @param[in] videoImage 非空视频后备帧，空图异步失败。
     * @param[in] videoRect 视频实际像素区域，非法矩形异步失败。
     * @note 普通入口一次在途由MultimediaDemo门控；同门面尚未执行的任务合并。
     */
    void processSnapshot(const QImage &uiImage, const QImage &videoImage, const QRect &videoRect);

    /** @brief GUI排队UI图像，后台按原560x360/JPEG35编码，销毁后忽略结果。
     * @param[in] uiImage 自有图像值，空图异步发failed。
     */
    void processUiOnlySnapshot(const QImage &uiImage);
    /** @brief GUI提交现成UI像素及视频值，NV12转换/合成/编码在共享队列；无效输入发failed。
     * @param[in] scene 已直接绘制的小图像素及NV12值，不带控件指针或绘图命令。
     */
    void processScene(const SnapshotScene &scene);


signals:
    /** @brief 在GUI输出原data:image/jpeg;base64图片文本。
     * @param[in] base64 成功编码的完整data URI。
     */
    void finished(const QString &base64);

    /** @brief 在GUI输出图像为空或编码失败原因。
     * @param[in] reason 原编码错误/后台异常描述，不含图像正文。
     */
    void failed(const QString &reason);
};

#endif // SNAPSHOTPROCESSOR_H
