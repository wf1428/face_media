/**
 * @file snapshot_scene.h
 * @brief GUI直接绘制截图像素，后台合成只读视频帧的值接口。
 * @author Du lin
 * @date 2026-11-04
 * @note 最近修改：2026-11-04；修改人：Du lin；截图改为一次根控件render，隐藏/非后代视频按纯UI处理；可见不透明视频子控件由栈内保护禁用更新并恢复，去掉分段render/局部grab约定，尺寸和后台接口保持。
 */
#pragma once
#include <QImage>
#include <QByteArray>
#include <QRect>
#include <QSize>
#include <QString>
#include <QMetaType>
#include <memory>
class QWidget;

/** @brief 一次按需取得的NV12只读帧；值持有数据，解码器后续写入通过QByteArray分离。
 * @details 只在请求截图时借用一次现有邮箱帧，不持有槽锁/解码器指针，不修改正常播放租约。
 * 字节持有期间仅首次复用该邮箱缓冲会按Qt规则分离，不引入逐帧常驻复制。
 */
struct SnapshotVideoFrame {
    QByteArray nv12;QSize size; /**< 紧密NV12，Y后接交错UV；尺寸均为正偶数，字节至少width*height*3/2。 */
    /** @return true为完整有效NV12值，false按无视频画面处理。 */
    bool valid() const { return size.width()>0 && size.height()>0 && !(size.width()&1) && !(size.height()&1) && qint64(nv12.size())>=qint64(size.width())*size.height()*3/2; }
};
struct SnapshotSceneData;
/** @brief 已完成GUI绘制的不可变UI像素及视频值；保留名称以兼容既有异步接口。
 * @details 普通UI在GUI直接绘制560x360以内小图，高清绘制原DPR尺寸；后台仅转换视频、合成和编码。
 * 不保存绘图命令、QWidget或GL上下文，统一截图队列只访问自有值。
 */
class SnapshotScene {
public:
    /** @brief 创建空截图值，不访问GUI或申请图像像素。 */
    SnapshotScene()=default;
    /** @brief GUI一次根控件render直接绘制目标尺寸QImage，临时排除可见视频子控件的GL绘制/读回。
     * @param[in] root GUI根控件，空/尺寸无效失败。
     * @param[in] videoWidget 可空的不透明视频子控件，只在抓取时使用；隐藏/非后代按无视频处理，等于root或可见但非不透明时失败。
     * @param[in] video 自有只读NV12值；仅可见视频使用，无效时该区域在后台补黑色。
     * @param[in] limit 普通传560x360并保持比例；空尺寸按原始DPR像素绘制高清图。
     * @return 自有UI像素及视频值；失败时valid=false并携带error。
     * @note 仅GUI调用；可见视频只在本次同步绘制栈内临时禁用更新，退出恢复原状态，原先禁用则保持；恢复会安排正常重绘。
     * 不隐藏/修改原控件mask/布局/相对层级/播放状态，不主动运行事件循环、不局部grab。
     * 可见视频上方仅在截图栈内放置不入布局的临时透明孔，退出前移除；遮挡/透明/效果由Qt一次递归处理。
     */
    static SnapshotScene capture(QWidget *root,QWidget *videoWidget=nullptr,const SnapshotVideoFrame &video={},
                                 const QSize &limit={});
    /** @brief 后台将现成UI像素与NV12合成；沿用方法名以兼容调用，不回放UI绘图命令。
     * @param[in] limit 可选输出限制，保持比例；空尺寸保留capture时的像素尺寸。
     * @param[out] error 可空，失败写原因，成功清空。
     * @return 自有/隐式共享ARGB32像素图；无效输入或分配失败为空，不访问GUI。
     */
    QImage rasterize(const QSize &limit={},QString *error=nullptr) const;
    /** @return 已成功绘制非空UI像素时true。 */
    bool valid() const;
    /** @return 抓取失败原因；有效截图为空。 */
    QString error() const;
private:
    std::shared_ptr<const SnapshotSceneData> data_; /**< 不可变UI像素/视频引用及区域；由引用计数管理寿命，后台完成合成后释放在途值。 */
};
Q_DECLARE_METATYPE(SnapshotScene)
