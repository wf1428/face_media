/**
 * @file snapshot_scene.cpp
 * @brief UI直接绘制指定尺寸的截图像素，后台合成复用的NV12视频帧。
 * @author Du lin
 * @date 2026-11-04
 * @note 最近修改：2026-11-04；修改人：Du lin；恢复一次根render并移除分段绘制/局部grab；可见视频在截图栈内禁用更新并恢复，以同层临时透明孔保留透明容器下的合成，隐藏视频走纯UI，后台及尺寸保持。
 */
#include "snapshot_scene.h"
#include <QWidget>
#include <QPointer>
#include <QPainter>
#include <QRegion>
#include <QThread>

/** @brief 已完成GUI绘制的不可变像素值；不保存控件、画笔或绘图命令。 */
struct SnapshotSceneData {
    QImage uiImage; /**< GUI按目标尺寸直接绘制的自有像素；视频区域透明，前景覆盖控件保留。 */
    QSize logical; /**< 根控件逻辑像素尺寸；用于后台换算视频位置，完成抓取后不变。 */
    SnapshotVideoFrame video; /**< 按需共享的只读NV12值，后续解码写入按Qt写时复制规则分离。 */
    QRect videoArea;QRegion videoClip; /**< 根逻辑坐标中的视频画布/祖先累计裁剪；空区域表示未包含视频。 */
    QString error; /**< 抓取失败原因；有效图为空，后台不再访问GUI查询错误。 */
};
namespace {
/** @brief 按现有GL的BT.601有限范围系数转换NV12，仅后台调用。
 * @param[in] frame 自有只读NV12值，紧密Y/UV布局。
 * @return 自有RGB32图，输入非法或分配失败为空。
 */
QImage nv12Image(const SnapshotVideoFrame &frame) {
    if(!frame.valid()) return {};
    const int w=frame.size.width(),h=frame.size.height();QImage out(w,h,QImage::Format_RGB32);if(out.isNull())return {};
    const auto *y=reinterpret_cast<const unsigned char*>(frame.nv12.constData()),*uv=y+qint64(w)*h;
    for(int row=0;row<h;++row) { auto *dst=reinterpret_cast<QRgb*>(out.scanLine(row));
        for(int col=0;col<w;++col) {const int offset=(row/2)*w+(col&~1);const double yy=1.16438356*(int(y[row*w+col])-16),u=int(uv[offset])-128,v=int(uv[offset+1])-128;
            dst[col]=qRgb(qBound(0,qRound(yy+1.59602678*v),255),qBound(0,qRound(yy-0.39176229*u-0.81296764*v),255),qBound(0,qRound(yy+2.01723214*u),255));
        }
    }return out;
}

/** @brief 仅在截图同步调用栈内暂停视频子控件更新；不拥有控件，不改变可见性/布局。
 * @details 原先已禁用时不改动；本保护禁用的控件在正常退出、提前返回和异常展开时恢复。
 * QWidget恢复更新会安排正常重绘，不停止解码/播放；禁止在保护期间主动运行事件循环。
 */
class VideoUpdatesGuard final {
public:
    /** @brief 保存并临时禁用可见视频子控件的更新。
     * @param[in] widget 可空，必须属于GUI线程；由原UI所有者管理寿命。
     */
    explicit VideoUpdatesGuard(QWidget *widget):widget_(widget),restore_(widget && widget->updatesEnabled()) {
        if(restore_) widget_->setUpdatesEnabled(false);
    }
    /** @brief 仅恢复本保护改变的状态；对象已销毁时不访问，原先禁用的控件保持禁用。 */
    ~VideoUpdatesGuard() {
        if(restore_ && widget_) widget_->setUpdatesEnabled(true);
    }
    /** @brief 禁止复制，防止同一更新状态被重复恢复。 */
    VideoUpdatesGuard(const VideoUpdatesGuard &)=delete;
    /** @brief 禁止赋值，保持恢复责任只属于本保护。 */
    VideoUpdatesGuard &operator=(const VideoUpdatesGuard &)=delete;
private:
    QPointer<QWidget> widget_; /**< GUI借用指针；控件提前销毁后自动置空。 */
    bool restore_=false; /**< true表示本保护从启用改为禁用，离开时需恢复；false不改原状态。 */
};

/** @brief 截图栈内的视频透明孔，仅作为视频的临时兄弟控件参与同一次根render。
 * @details Qt在透明祖先下不一定剪掉所有底层兄弟像素；禁用GL更新后，此控件在原视频堆叠位置清透明，
 * 随后仍由Qt绘制上层字幕/效果。无需另一次render或grab，也不修改原控件几何、mask或相对层级。
 * 构造/销毁必须在GUI同一截图调用栈，期间禁止处理事件循环；不加入任何布局，退出前移除，不留在实际显示帧中。
 */
class SnapshotVideoHole final:public QWidget {
public:
    /** @brief 保存原视频位置/遮罩，在其上方、原上层控件下方插入一次性透明孔。
     * @param[in] video 非空的可见视频子控件；父控件和视频须覆盖本保护寿命。
     */
    explicit SnapshotVideoHole(QWidget *video):QWidget(video->parentWidget()) {
        setAttribute(Qt::WA_OpaquePaintEvent,true);setAttribute(Qt::WA_NoSystemBackground,true);
        setAttribute(Qt::WA_TransparentForMouseEvents,true);setFocusPolicy(Qt::NoFocus);
        setGeometry(video->geometry());if(!video->mask().isEmpty())setMask(video->mask());
        const auto siblings=parentWidget()->children();const int index=siblings.indexOf(video);
        QWidget *next=nullptr;
        for(int i=index+1;i<siblings.size();++i) {
            auto *candidate=qobject_cast<QWidget*>(siblings.at(i));
            if(candidate && candidate!=this && !candidate->isWindow()) {next=candidate;break;}
        }
        if(next)stackUnder(next);else raise();
        show();
    }
protected:
    /** @brief 仅在同步截图的Qt递归中把视频区域清透明；裁剪和前景堆叠仍由Qt决定。
     * @param[in] event Qt绘制通知；区域已由Qt画笔系统裁剪，不另行扩大。
     */
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);QPainter painter(this);painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(),Qt::transparent);
    }
};

/** @brief 取得视频在根控件中的祖先矩形/mask裁剪；只遍历祖先，不绘制子树。
 * @param[in] widget root中的视频后代，必须非空。
 * @param[in] root GUI截图宿主，必须是widget的祖先。
 * @return 根逻辑坐标中的有效视频区域；完全被裁剪时为空。
 */
QRegion videoRegion(QWidget *widget,QWidget *root)
{
    QRegion clip(root->rect());
    for(QWidget *ancestor=widget;ancestor;ancestor=ancestor->parentWidget()) {
        const QPoint origin=ancestor==root?QPoint():ancestor->mapTo(root,QPoint());
        clip&=QRegion(ancestor->rect()).translated(origin);
        if(!ancestor->mask().isEmpty()) clip&=ancestor->mask().translated(origin);
        if(ancestor==root || clip.isEmpty()) break;
    }
    return clip;
}
}
SnapshotScene SnapshotScene::capture(QWidget *root,QWidget *videoWidget,const SnapshotVideoFrame &video,
                                     const QSize &limit)
{
    SnapshotScene result;
    auto data=std::make_shared<SnapshotSceneData>();
    result.data_=data;
    if(!root || root->size().isEmpty()) {
        data->error=QStringLiteral("截图根控件无效");return result;
    }
    Q_ASSERT(QThread::currentThread()==root->thread());
    data->logical=root->size();
    const qreal dpr=root->devicePixelRatioF();
    QSize pixels(qRound(root->width()*dpr),qRound(root->height()*dpr));
    if(!limit.isEmpty()) pixels.scale(limit,Qt::KeepAspectRatio);
    data->uiImage=QImage(pixels,QImage::Format_ARGB32_Premultiplied);
    if(data->uiImage.isNull()) {
        data->error=QStringLiteral("截图像素分配失败");return result;
    }
    data->uiImage.setDotsPerMeterX(qRound(root->logicalDpiX()/0.0254));
    data->uiImage.setDotsPerMeterY(qRound(root->logicalDpiY()/0.0254));
    data->uiImage.fill(Qt::transparent);
    QPointer<QWidget> visibleVideo;
    if(videoWidget && videoWidget->isVisible()) {
        if(videoWidget==root) {
            data->error=QStringLiteral("截图根控件不能同时作为视频画布");return result;
        }
        if(root->isAncestorOf(videoWidget) && !videoRegion(videoWidget,root).isEmpty()) {
            // 当前VideoHoleWidget已声明不透明。保留此遮挡信息，Qt会跳过其背后的像素。
            if(!videoWidget->testAttribute(Qt::WA_OpaquePaintEvent)) {
                data->error=QStringLiteral("截图视频画布未声明不透明绘制");return result;
            }
            visibleVideo=videoWidget;
        }
    }
    {
        VideoUpdatesGuard restoreVideo(visibleVideo.data());
        // 先结束画笔，再移除临时透明孔，最后恢复视频更新；整个范围不运行事件循环。
        std::unique_ptr<SnapshotVideoHole> hole;
        if(visibleVideo) hole.reset(new SnapshotVideoHole(visibleVideo.data()));
        QPainter painter(&data->uiImage);
        if(!painter.isActive()) {data->error=QStringLiteral("截图画笔初始化失败");return result;}
        painter.scale(qreal(pixels.width())/root->width(),qreal(pixels.height())/root->height());
        // 一次完整递归保留Qt的兄弟遮挡、透明度和图形效果上下文。
        // 视频仍可见/不透明，但updatesEnabled=false使Qt在子控件绘制入口跳过GL读回。
        root->render(&painter,QPoint(),QRegion(),QWidget::DrawWindowBackground|QWidget::DrawChildren);
    }
    if(visibleVideo && visibleVideo->isVisible()) {
        // render可能同步处理待决几何事件；按实际绘制后的逻辑坐标交接后台。
        data->videoArea=QRect(visibleVideo->mapTo(root,QPoint()),visibleVideo->size());
        data->videoClip=videoRegion(visibleVideo,root);
        data->video=video;
    }
    return result;
}
bool SnapshotScene::valid()const {return data_ && data_->error.isEmpty() && !data_->uiImage.isNull();}
QString SnapshotScene::error()const {return data_?data_->error:QStringLiteral("截图像素为空");}
QImage SnapshotScene::rasterize(const QSize &limit,QString *error)const
{
    if(error) error->clear();
    if(!valid()) {if(error)*error=this->error();return {};}
    QSize pixels=data_->uiImage.size();
    if(!limit.isEmpty()) pixels.scale(limit,Qt::KeepAspectRatio);
    if(data_->videoArea.isEmpty() || data_->videoClip.isEmpty()) {
        const QImage image=pixels==data_->uiImage.size()?data_->uiImage:
                    data_->uiImage.scaled(pixels,Qt::IgnoreAspectRatio,Qt::FastTransformation);
        if(image.isNull() && error) *error=QStringLiteral("截图缩放图像分配失败");
        return image;
    }
    QImage image(pixels,QImage::Format_ARGB32_Premultiplied);
    if(image.isNull()) {if(error)*error=QStringLiteral("截图合成图像分配失败");return {};}
    image.setDotsPerMeterX(data_->uiImage.dotsPerMeterX());
    image.setDotsPerMeterY(data_->uiImage.dotsPerMeterY());
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.scale(qreal(pixels.width())/data_->logical.width(),qreal(pixels.height())/data_->logical.height());
        painter.setClipRegion(data_->videoClip);
        painter.fillRect(data_->videoArea,Qt::black);
        if(data_->video.valid()) {
            const QImage videoImage=nv12Image(data_->video);
            if(videoImage.isNull()) {if(error)*error=QStringLiteral("截图视频转换图像分配失败");return {};}
            const QSize fitted=data_->video.size.scaled(data_->videoArea.size(),Qt::KeepAspectRatio);
            const QPoint offset((data_->videoArea.width()-fitted.width())/2,(data_->videoArea.height()-fitted.height())/2);
            painter.setRenderHint(QPainter::SmoothPixmapTransform,true);
            painter.drawImage(QRect(data_->videoArea.topLeft()+offset,fitted),videoImage);
        }
        painter.setClipping(false);
        painter.resetTransform();
        painter.drawImage(QRect(QPoint(),pixels),data_->uiImage);
    }
    return image;
}
