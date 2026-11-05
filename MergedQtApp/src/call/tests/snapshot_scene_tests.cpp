/**
 * @file snapshot_scene_tests.cpp
 * @brief 验证GUI直接绘制截图像素及后台视频合成，覆盖尺寸、层级和GL跳过。
 * @author Du lin
 * @date 2026-11-04
 * @note 最近修改：2026-11-04；修改人：Du lin；验证一次根render、隐藏视频纯UI路径、可见视频更新状态恢复及被视频遮住的背景不重绘；视频替身使用真实不透明属性，保留尺寸/DPR/透明/阴影回归。
 */
#include <QtTest>
#include <QtWidgets>
#include <functional>
#include "common/snapshot_scene.h"
#include "common/snapshot_work.h"
/** @brief 可统计GUI绘制次数的测试控件，视频替身绘制应被完全跳过。 */
class PaintedWidget:public QWidget {
public:
    int paints=0;QColor color=Qt::red;using QWidget::QWidget;
    std::function<void()> observer; /**< 测试绘制期间观察更新状态；空值时不执行，不修改生产行为。 */
protected:
    /** @brief 绘制可比对纯色，计数仅测试使用。 */
    void paintEvent(QPaintEvent*)override{++paints;if(observer)observer();QPainter p(this);p.fillRect(rect(),color);}
};
/** @brief 与当前VideoHoleWidget相同不透明属性的非GL视频替身；paint计数用于验证入口被跳过。 */
class OpaqueVideoWidget final:public PaintedWidget {
public:
    /** @brief 初始化不透明/无系统背景属性，保留默认更新启用状态。
     * @param[in] parent 测试GUI父控件，按Qt规则拥有本对象。
     */
    explicit OpaqueVideoWidget(QWidget *parent):PaintedWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent,true);setAttribute(Qt::WA_NoSystemBackground,true);
    }
};
/** @brief 建立真实可见层级并清除首次显示的计数；事件处理仅在测试准备阶段。
 * @param[in,out] host 测试根窗口。
 * @param[in,out] video 需要观察截图绘制次数的视频替身。
 */
static void exposeScene(QWidget &host,PaintedWidget &video) {
    host.show();QCoreApplication::processEvents();video.paints=0;
}
/** @brief 实际GUI一次根绘制和后台视频合成回归。 */
class SnapshotSceneTests:public QObject{
    Q_OBJECT
private slots:
    /** @brief 验证图片/文字/透明控件/剪裁与Qt直接绘制逐像素一致。 */
    void widgetParity(){
        QWidget host;host.resize(320,180);host.setStyleSheet("background:#305070;");
        QLabel label("Text 123",&host);label.setGeometry(9,15,150,32);label.setStyleSheet("color:white;background:rgba(200,30,10,180);border:2px solid yellow;");label.show();
        QLabel image(&host);image.setGeometry(100,80,180,80);QPixmap pm(180,80);pm.fill(Qt::green);image.setPixmap(pm);image.show();
        QImage expected(host.size(),QImage::Format_ARGB32_Premultiplied);expected.setDotsPerMeterX(qRound(host.logicalDpiX()/0.0254));expected.setDotsPerMeterY(qRound(host.logicalDpiY()/0.0254));expected.fill(Qt::transparent);{QPainter p(&expected);host.render(&p);}
        const auto scene=SnapshotScene::capture(&host);QVERIFY2(scene.valid(),qPrintable(scene.error()));const auto actual=scene.rasterize();QCOMPARE(actual,expected);
    }
    /** @brief 普通UI在GUI阶段已绘成目标小图，高清保留原像素，后台调用不再次绘制控件。 */
    void captureDrawsTargetPixelsOnlyOnce(){
        PaintedWidget host;host.resize(1920,1080);host.color=Qt::blue;
        const auto ordinary=SnapshotScene::capture(&host,nullptr,SnapshotVideoFrame(),QSize(560,360));
        QVERIFY(ordinary.valid());QCOMPARE(host.paints,1);
        const auto hd=SnapshotScene::capture(&host);QVERIFY(hd.valid());QCOMPARE(host.paints,2);
        host.color=Qt::red;
        QCOMPARE(ordinary.rasterize().size(),QSize(560,315));
        QCOMPARE(ordinary.rasterize().pixelColor(20,20),QColor(Qt::blue));
        QCOMPARE(hd.rasterize().size(),QSize(1920,1080));
        QCOMPARE(host.paints,2);
    }
    /** @brief 透明视频孔只覆盖原视频mask，前置底图和后置半透明控件保留原合成结果。 */
    void videoMaskAndTranslucentOverlay(){
        PaintedWidget host;host.resize(200,120);host.color=Qt::green;
        OpaqueVideoWidget video(&host);video.setGeometry(20,20,80,80);video.color=Qt::white;
        video.setMask(QRegion(0,0,80,80,QRegion::Ellipse));video.show();
        PaintedWidget overlay(&host);overlay.setGeometry(50,10,60,60);overlay.color=QColor(255,0,0,128);
        overlay.setAttribute(Qt::WA_TranslucentBackground);overlay.show();overlay.raise();
        exposeScene(host,video);
        QImage expected(host.size(),QImage::Format_ARGB32_Premultiplied);expected.fill(Qt::transparent);
        {QPainter p(&expected);host.render(&p);}
        video.paints=0;SnapshotVideoFrame frame;frame.size=QSize(2,2);frame.nv12=QByteArray(6,char(128));
        for(int i=0;i<4;++i)frame.nv12[i]=char(235);
        const auto image=SnapshotScene::capture(&host,&video,frame).rasterize();
        QCOMPARE(video.paints,0);QCOMPARE(image,expected);
    }
    /** @brief 隐藏视频不暂停更新、不合成旧NV12；截图仍是当前普通UI像素。 */
    void hiddenVideoUsesPlainUi(){
        PaintedWidget host;host.resize(200,120);host.color=Qt::blue;
        OpaqueVideoWidget video(&host);video.setGeometry(20,20,80,80);video.show();
        exposeScene(host,video);video.hide();QCoreApplication::processEvents();host.paints=0;video.paints=0;
        bool unchangedDuringPaint=false;host.observer=[&]{unchangedDuringPaint=video.updatesEnabled();};
        SnapshotVideoFrame frame;frame.size=QSize(2,2);frame.nv12=QByteArray(6,char(128));for(int i=0;i<4;++i)frame.nv12[i]=char(235);
        const auto scene=SnapshotScene::capture(&host,&video,frame);
        QVERIFY(scene.valid());QCOMPARE(host.paints,1);QCOMPARE(video.paints,0);QVERIFY(unchangedDuringPaint);
        QVERIFY(video.isHidden());QVERIFY(video.updatesEnabled());QCOMPARE(scene.rasterize().pixelColor(40,40),QColor(Qt::blue));
        host.observer={};
    }
    /** @brief 可见视频只在绘制期间禁用更新；原启用/原禁用状态均正确保留。 */
    void visibleVideoUpdatesAreRestored(){
        for(bool enabled:{false,true}) {
            PaintedWidget host;host.resize(200,120);host.color=Qt::blue;
            OpaqueVideoWidget video(&host);video.setGeometry(20,20,80,80);video.show();exposeScene(host,video);
            video.setUpdatesEnabled(enabled);const auto children=host.children();bool sawDisabled=false;host.observer=[&]{sawDisabled=!video.updatesEnabled();};
            SnapshotVideoFrame frame;frame.size=QSize(2,2);frame.nv12=QByteArray(6,char(128));for(int i=0;i<4;++i)frame.nv12[i]=char(235);
            const auto scene=SnapshotScene::capture(&host,&video,frame);
            QVERIFY(scene.valid());QVERIFY(sawDisabled);QCOMPARE(video.paints,0);QCOMPARE(video.updatesEnabled(),enabled);
            QVERIFY(video.isVisible());QCOMPARE(video.geometry(),QRect(20,20,80,80));QCOMPARE(host.children(),children);
            QCOMPARE(scene.rasterize().pixelColor(50,50),QColor(Qt::white));host.observer={};
        }
    }
    /** @brief Qt保留视频遮挡裁剪，不先绘制被完全挡住的背景再擦除。 */
    void coveredBackgroundIsNotRepainted(){
        PaintedWidget host;host.resize(200,120);host.color=Qt::blue;
        PaintedWidget behind(&host);behind.setGeometry(20,20,100,60);behind.color=Qt::red;behind.show();
        OpaqueVideoWidget video(&host);video.setGeometry(20,20,100,60);video.show();exposeScene(host,video);behind.paints=0;
        const auto scene=SnapshotScene::capture(&host,&video,SnapshotVideoFrame());
        QVERIFY(scene.valid());QCOMPARE(behind.paints,0);QCOMPARE(video.paints,0);QVERIFY(video.updatesEnabled());
        QCOMPARE(scene.rasterize().pixelColor(50,50),QColor(Qt::black));
    }
    /** @brief 无效根控件及不支持的视频根输入提前失败，不改变原更新开关。 */
    void invalidCapturePreservesUpdates(){
        PaintedWidget host;host.resize(200,120);OpaqueVideoWidget video(&host);video.setGeometry(20,20,80,80);video.show();exposeScene(host,video);
        for(bool enabled:{false,true}) {
            video.setUpdatesEnabled(enabled);
            QVERIFY(!SnapshotScene::capture(nullptr,&video).valid());QCOMPARE(video.updatesEnabled(),enabled);
            QVERIFY(!SnapshotScene::capture(&video,&video).valid());QCOMPARE(video.updatesEnabled(),enabled);
        }
    }
    /** @brief 真正2倍DPR维持高清像素尺寸，普通仍按上传尺寸限制。 */
    void highDpiDimensions(){
        /** @brief 模拟2倍DPR根控件，其余设备计量沿用Qt。 */
        class HiDpi:public PaintedWidget{protected:
        /** @brief 提供测试DPR计量。
         * @param[in] m Qt设备计量项。
         * @return DPR项为2或其固定倍率，其余继承QWidget值。
         */
        int metric(PaintDeviceMetric m)const override{if(m==PdmDevicePixelRatio)return 2;if(m==PdmDevicePixelRatioScaled)return int(2*devicePixelRatioFScale());return QWidget::metric(m);}} host;
        host.resize(320,180);host.color=Qt::blue;PaintedWidget child(&host);child.setGeometry(10,20,30,40);child.show();const auto scene=SnapshotScene::capture(&host);QVERIFY(scene.valid());QCOMPARE(scene.rasterize().size(),QSize(640,360));QCOMPARE(scene.rasterize().pixelColor(30,50),QColor(Qt::red));QCOMPARE(scene.rasterize().pixelColor(90,50),QColor(Qt::blue));QCOMPARE(scene.rasterize(QSize(560,360)).size(),QSize(560,315));
    }
    /** @brief 高清2倍DPR下仍按逻辑坐标合成视频、黑边和前景，截图后恢复更新。 */
    void highDpiVideoAndOverlay(){
        /** @brief 模拟2倍DPR根控件，其余设备计量沿用Qt。 */
        class HiDpi:public PaintedWidget{protected:
        /** @brief 提供测试DPR计量。
         * @param[in] m Qt设备计量项。
         * @return DPR项为2或其固定倍率，其余继承QWidget值。
         */
        int metric(PaintDeviceMetric m)const override{if(m==PdmDevicePixelRatio)return 2;if(m==PdmDevicePixelRatioScaled)return int(2*devicePixelRatioFScale());return QWidget::metric(m);}} host;
        host.resize(160,100);host.color=Qt::blue;
        OpaqueVideoWidget video(&host);video.setGeometry(20,20,80,60);video.show();
        PaintedWidget overlay(&host);overlay.setGeometry(40,35,20,20);overlay.color=Qt::green;overlay.show();overlay.raise();exposeScene(host,video);
        SnapshotVideoFrame frame;frame.size=QSize(4,2);frame.nv12=QByteArray(12,char(128));for(int i=0;i<8;++i)frame.nv12[i]=char(235);
        const auto image=SnapshotScene::capture(&host,&video,frame).rasterize();
        QCOMPARE(image.size(),QSize(320,200));QCOMPARE(image.pixelColor(100,80),QColor(Qt::green));
        QCOMPARE(image.pixelColor(140,100),QColor(Qt::white));QCOMPARE(image.pixelColor(60,46),QColor(Qt::black));
        QCOMPARE(image.pixelColor(10,10),QColor(Qt::blue));QCOMPARE(video.paints,0);QVERIFY(video.updatesEnabled());
    }
    /** @brief 一次根绘制仍保持透明兄弟控件、mask裁剪和覆盖顺序。 */
    void masksTransparencyAndStacking(){
        PaintedWidget host;host.resize(320,180);host.color=Qt::blue;
        PaintedWidget behind(&host);behind.setGeometry(0,0,320,180);behind.color=Qt::yellow;behind.show();
        QWidget container(&host);container.setGeometry(20,20,200,140);container.setAttribute(Qt::WA_TranslucentBackground);container.show();
        OpaqueVideoWidget video(&container);video.setGeometry(20,20,100,80);video.show();
        PaintedWidget front(&host);front.setGeometry(70,50,100,80);front.color=Qt::green;front.setMask(QRegion(0,0,40,40));front.show();front.raise();
        exposeScene(host,video);
        const auto scene=SnapshotScene::capture(&host,&video,SnapshotVideoFrame());const auto image=scene.rasterize();QVERIFY(!image.isNull());QCOMPARE(video.paints,0);
        QCOMPARE(image.pixelColor(30,30),QColor(Qt::yellow));QCOMPARE(image.pixelColor(45,45),QColor(Qt::black));QCOMPARE(image.pixelColor(80,60),QColor(Qt::green));QCOMPARE(image.pixelColor(120,60),QColor(Qt::black));QCOMPARE(image.pixelColor(240,60),QColor(Qt::yellow));
    }
    /** @brief 纹理画刷直接绘成像素后仍保持原图案变换，不依赖后台QPixmap。 */
    void imageTextureBrushParity(){
        class Texture:public QWidget{protected:void paintEvent(QPaintEvent*)override{QPainter p(this);QImage tile(8,8,QImage::Format_RGB32);tile.fill(Qt::red);{QPainter t(&tile);t.fillRect(0,0,4,4,Qt::green);}QBrush brush(QPixmap::fromImage(tile));QTransform tr;tr.translate(3,2);tr.scale(2,2);brush.setTransform(tr);p.fillRect(rect(),brush);}} host;
        host.resize(180,100);QImage expected(host.size(),QImage::Format_ARGB32_Premultiplied);expected.fill(Qt::transparent);{QPainter p(&expected);host.render(&p);}
        const auto scene=SnapshotScene::capture(&host);QCOMPARE(scene.rasterize(),expected);
    }
    /** @brief 一次根绘制保留覆盖控件阴影，不需要局部grab。 */
    void graphicsEffectOutsideVideo(){
        PaintedWidget host;host.resize(320,180);host.color=Qt::white;
        OpaqueVideoWidget video(&host);video.setGeometry(20,20,240,140);video.color=Qt::black;video.show();
        PaintedWidget front(&host);front.setGeometry(80,50,50,40);front.color=Qt::red;front.show();auto *effect=new QGraphicsDropShadowEffect(&front);effect->setBlurRadius(12);effect->setOffset(4,6);front.setGraphicsEffect(effect);
        exposeScene(host,video);
        QImage expected(host.size(),QImage::Format_ARGB32_Premultiplied);expected.fill(Qt::transparent);{QPainter p(&expected);host.render(&p);}
        video.paints=0;const auto scene=SnapshotScene::capture(&host,&video,SnapshotVideoFrame());const auto actual=scene.rasterize();QCOMPARE(video.paints,0);if(actual!=expected){actual.save(".web-build/snapshot-one-render-20261104/effect-actual.png");expected.save(".web-build/snapshot-one-render-20261104/effect-expected.png");}QCOMPARE(actual,expected);
    }
    /** @brief 跳过视频画布绘制，以已有NV12插入原堆叠位置，后置UI仍覆盖视频。 */
    void videoReuseAndLayering(){
        PaintedWidget host;host.color=Qt::blue;host.resize(320,180);QWidget container(&host);container.setGeometry(20,20,240,140);container.setStyleSheet("background:black;");container.show();
        OpaqueVideoWidget video(&container);video.setGeometry(0,0,240,140);video.show();QLabel overlay("",&host);overlay.setGeometry(80,60,40,40);overlay.setStyleSheet("background:#00ff00;");overlay.show();overlay.raise();
        exposeScene(host,video);
        SnapshotVideoFrame frame;frame.size=QSize(4,2);frame.nv12=QByteArray(12,char(128));for(int i=0;i<8;++i)frame.nv12[i]=char(235);
        const auto scene=SnapshotScene::capture(&host,&video,frame);QVERIFY(scene.valid());QCOMPARE(video.paints,0);
        frame.nv12.fill(char(16)); // 后续写入不能修改场景持有的旧帧。
        const auto image=scene.rasterize();QCOMPARE(image.pixelColor(100,80),QColor(Qt::green));QCOMPARE(image.pixelColor(40,24),QColor(Qt::black));QVERIFY(image.pixelColor(130,50).red()>250);
        QCOMPARE(video.paints,0);
    }
    /** @brief 普通小图和高清原图沿用宽高比，销毁源控件后仍可在统一后台使用自有像素。 */
    void backgroundLifetimeAndDimensions(){
        SnapshotScene scene;{QWidget host;host.resize(1920,1080);host.setStyleSheet("background:#305070;");scene=SnapshotScene::capture(&host);}
        QImage ordinary,hd;bool done=false;quintptr worker=0;
        SnapshotWork::queue().enqueue([&]{SerialWorkQueue::Task task;task.reportSlow=false;task.run=[&]{worker=quintptr(QThread::currentThreadId());ordinary=scene.rasterize(QSize(560,360));hd=scene.rasterize();return QString();};task.done=[&](const QString &){done=true;};return task;});
        QTRY_VERIFY(done);QVERIFY(worker!=quintptr(QThread::currentThreadId()));QCOMPARE(ordinary.size(),QSize(560,315));QCOMPARE(hd.size(),QSize(1920,1080));QCOMPARE(hd.pixelColor(100,100),QColor("#305070"));
    }
};
QTEST_MAIN(SnapshotSceneTests)
#include "snapshot_scene_tests.moc"
