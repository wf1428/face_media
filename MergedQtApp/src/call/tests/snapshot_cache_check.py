"""Compile actual ordinary snapshot methods with Qt widgets and an isolated player.

Author: Du lin
Created: 2026-11-04
Note: 最近修改：2026-11-04；修改人：Du lin；验证普通/高清隐藏视频均不读取NV12、走一次普通根绘制，保留尺寸/缓存/请求ID和上传背压回归。
Exercises deferred capture, two-second cache reuse, nested requests and stale source
completion. The fixture replaces app wiring/player hardware, not the cache methods.
No network, production INI or audio device is accessed. Writes only .web-build outputs.
"""
import os
from pathlib import Path
import subprocess


def function(source, signature):
    """Extract one actual C++ body, requiring balanced braces and a present signature."""
    start=source.index(signature); opening=source.index('{',start);depth=0
    for index in range(opening,len(source)):
        if source[index]=='{': depth+=1
        elif source[index]=='}':
            depth-=1
            if not depth: return source[start:index+1]
    raise ValueError('Incomplete function: '+signature)


def main():
    """Build/run isolated Qt cache regressions; preserve compiler output on failure."""
    root=Path.cwd();output=root/'.web-build/snapshot-final-20261104/snapshot';output.mkdir(parents=True,exist_ok=True)
    source=(root/'components/multimedia/multimediademo.cpp').read_text(encoding='utf-8')
    fixture=r'''
#include <QtTest>
#include <QtWidgets>
#include <QDataStream>
#include <QScopedValueRollback>
#include "components/multimedia/snapshot_processor.h"
#include "common/media_selection_cache.h"
#include <functional>
#include "common/debug/ui_performance.h"
namespace ResponsiveUi { static QWidget *overlayHost(QWidget *w) { return w->window(); } }
struct Player {
    bool active=true,composited=false;QWidget *canvas=nullptr;mutable int frameReads=0;bool snapshotIncludesVideo() const { return composited; }SnapshotVideoFrame snapshotVideoFrame() const {++frameReads;return {};}QWidget *snapshotVideoWidget() const {return canvas;}int requests=0;std::function<void()> callback;
    bool hasActiveMedia() const { return active; }bool isAvailable() const { return true; }bool isPlaying() const {return active;}
    void requestSnapshot() { ++requests;if(callback) callback(); }
};
struct Amp { int currentVolume() const {return 80;} };
struct Manager { bool busy=false;QStringList ids;QString image;void updateCurrentPlayImage(const QString &s){image=s;}
    bool snapshotUploadBusy() const {return busy;}
    void sendPlayInfoNow(const QString&,const QString&id,const QString&){ids<<id;}
};
class MqttPage:public QObject {
    Q_OBJECT
public:
    struct PendingPlayInfo { QString topic,id,deviceId;bool imageReady=false; };
    QList<PendingPlayInfo> pendingPlayInfo_;bool playInfoDrainQueued_=false;Manager *manager_=nullptr;
    void onCurrentPlayImageChanged(const QString&);void onGetPlayInfoRequested(const QString&,const QString&,const QString&);void drainPlayInfoReplies();
signals:
    void requestCapturePlayImage();
};
class MultimediaDemo:public QWidget {
    Q_OBJECT
public:
    QString m_currentVideoRectMediaPath="source-a",m_desiredLiveUrl;
    MediaSelectionCache *m_mediaSelectionCache=nullptr;Amp *m_ampCtrl=nullptr;
    bool m_modulePlaybackSuspended=false,m_liveDesired=false,m_startupPlaybackReleased=true;QStringList videoRectMediaList;
    QJsonObject localWebPlaybackState() const;
    bool m_isLiveMode=false,m_currentVideoRectIsImage=false,m_downloadPauseActive=true,PicDisplay_en=false;
    int VideoDisplay_x=0,VideoDisplay_y=0,VideoDisplay_x_Size=320,VideoDisplay_y_Size=180,currentPictureIndex=0;
    QStringList pictureFileList;QWidget *label_picture=nullptr;Player *mediaPlayer=nullptr;
    bool m_snapshotRefreshQueued=false,m_snapshotCaptureInProgress=false,m_snapshotWorkerBusy=false,m_snapshotPending=false;
    quint64 m_snapshotContentRevision=0;QByteArray m_snapshotCacheKey,m_snapshotWorkKey;QTimer *m_snapshotTimeoutTimer=nullptr;
    qint64 m_lastSnapshotMs=0,m_snapshotRequestMs=0;QString m_lastSnapshotBase64,m_companySnapshotToken;
    int grabs=0;std::function<void()> duringGrab;
    MultimediaDemo() { resize(320,180); }
    void paintEvent(QPaintEvent *event) override { ++grabs;auto action=std::move(duringGrab);duringGrab={};if(action) action();QWidget::paintEvent(event); }
    bool submitScaledSnapshot(QWidget*,const QImage &video={},const QRect &rect={});
    QByteArray snapshotCacheKey() const;void captureCompositeSnapshot();void captureCompositeSnapshotNow();
    void onVideoSnapshotReady(const QImage&);void onSnapshotProcessFinished(const QString&);void onSnapshotProcessFailed(const QString&);
    void captureCompanySnapshot(const QString&);void deliverCompanySnapshot(const QString&,const QImage&);
signals:
    void currentPlayImageChanged(const QString&);
    void processUiSnapshotAsync(const QImage&);
    void processSnapshotAsync(const QImage&,const QImage&,const QRect&);
    void companySnapshotReady(const QString&,const QImage&,const QImage&,const QRect&,const QString&);
    void companySceneReady(const QString&,const SnapshotScene&);
    void processSceneSnapshotAsync(const SnapshotScene&);
};
'''
    for signature in ['QJsonObject MultimediaDemo::localWebPlaybackState() const', 'bool MultimediaDemo::submitScaledSnapshot(', 'QByteArray MultimediaDemo::snapshotCacheKey() const','void MultimediaDemo::captureCompositeSnapshot()','void MultimediaDemo::captureCompositeSnapshotNow()',
                      'void MultimediaDemo::captureCompanySnapshot(', 'void MultimediaDemo::deliverCompanySnapshot(', 'void MultimediaDemo::onVideoSnapshotReady(','void MultimediaDemo::onSnapshotProcessFinished(','void MultimediaDemo::onSnapshotProcessFailed(']:
        fixture+='\n'+function(source,signature)
    fixture+='\n'+function(source,'static void applyLabelStyle(')
    page=(root/'components/settings/mqttpage.cpp').read_text(encoding='utf-8')
    for signature in ['void MqttPage::onCurrentPlayImageChanged(','void MqttPage::onGetPlayInfoRequested(','void MqttPage::drainPlayInfoReplies()']:
        fixture+='\n'+function(page,signature)
    fixture+=r'''
class SnapshotTests:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){qRegisterMetaType<SnapshotScene>("SnapshotScene");}
    void labelStylesOnlyChangeWhenValuesChange(){
        struct Observer:QObject{int styles=0;bool eventFilter(QObject*,QEvent *event)override{if(event->type()==QEvent::StyleChange)++styles;return false;}} observer;
        QLabel label;label.ensurePolished();label.installEventFilter(&observer);applyLabelStyle(&label,10,20,100,30,14,1,12,34,56);const int first=observer.styles;
        for(int i=0;i<100;++i)applyLabelStyle(&label,10,20,100,30,14,1,12,34,56);
        QCOMPARE(observer.styles,first);QCOMPARE(label.geometry(),QRect(10,20,100,30));
        applyLabelStyle(&label,10,20,100,30,14,1,13,34,56);QCOMPARE(observer.styles,first+1);
    }
    void configurationSnapshotStaysResponsive() {
        QTemporaryDir dir;const auto path=dir.filePath("net.ini");
        { QSettings settings(path,QSettings::IniFormat);settings.setValue("playback/contentType","IMAGE");settings.setValue("playback/smallImageMode",1);settings.sync(); }
        MultimediaDemo demo;demo.m_mediaSelectionCache=new MediaSelectionCache(path,"VIDEO",false,&demo);
        auto &cache=*demo.m_mediaSelectionCache;
        ConfigurationWork::queue().enqueue([] { SerialWorkQueue::Task t;t.reportSlow=false;t.run=[] { QThread::msleep(250);return QString(); };return t; });
        cache.refresh();QCOMPARE(cache.contentType(),QString("VIDEO"));QVERIFY(!cache.smallImageMode());
        const auto oldState=demo.localWebPlaybackState();QCOMPARE(oldState.value("contentType").toString(),QString("VIDEO"));QCOMPARE(oldState.value("smallImageMode").toInt(),0);
        bool alive=false;QTimer::singleShot(10,[&] { alive=true; });QTest::qWait(40);QVERIFY(alive);
        QTRY_COMPARE(cache.contentType(),QString("IMAGE"));QVERIFY(cache.smallImageMode());
        const auto newState=demo.localWebPlaybackState();QCOMPARE(newState.value("contentType").toString(),QString("IMAGE"));QCOMPARE(newState.value("smallImageMode").toInt(),1);
        auto *discarded=new MediaSelectionCache(path,"VIDEO",false);discarded->refresh();delete discarded;QTest::qWait(30);
    }
    void scaledCaptureAndBackgroundEncoding() {
        MultimediaDemo demo;demo.resize(1920,1080);demo.setStyleSheet("background:#305070;");
        QSignalSpy captured(&demo,&MultimediaDemo::processSceneSnapshotAsync);demo.captureCompositeSnapshot();QTRY_COMPARE(captured.count(),1);
        const QImage image=captured[0][0].value<SnapshotScene>().rasterize();QCOMPARE(image.size(),QSize(560,315));QCOMPARE(image.pixelColor(100,100),QColor("#305070"));
        SnapshotProcessor worker;QSignalSpy finished(&worker,&SnapshotProcessor::finished);worker.processUiOnlySnapshot(image);
        QTRY_COMPARE(finished.count(),1);
        const auto bytes=QByteArray::fromBase64(finished[0][0].toString().section(',',1).toLatin1());QCOMPARE(QImage::fromData(bytes).size(),QSize(560,315));
    }
    void compositedVideoOnlyCapturesOnce() {
        MultimediaDemo demo;Player player;player.composited=true;demo.mediaPlayer=&player;demo.m_downloadPauseActive=false;
        QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync);demo.captureCompositeSnapshot();
        QTRY_COMPARE(encoded.count(),1);QCOMPARE(player.requests,0);QCOMPARE(demo.grabs,1);
    }
    void hiddenVideoDoesNotReadFramesForEitherSize() {
        MultimediaDemo demo;demo.resize(800,480);Player player;player.composited=true;demo.mediaPlayer=&player;demo.m_downloadPauseActive=false;
        QWidget hidden(&demo);hidden.hide();player.canvas=&hidden;demo.show();QCoreApplication::processEvents();demo.grabs=0;
        QSignalSpy ordinary(&demo,&MultimediaDemo::processSceneSnapshotAsync),hd(&demo,&MultimediaDemo::companySceneReady);
        demo.captureCompositeSnapshot();QTRY_COMPARE(ordinary.count(),1);QCOMPARE(player.frameReads,0);QCOMPARE(demo.grabs,1);
        QCOMPARE(ordinary[0][0].value<SnapshotScene>().rasterize().size(),QSize(560,336));
        demo.captureCompanySnapshot("hidden-hd");QCOMPARE(hd.count(),1);QCOMPARE(player.frameReads,0);QCOMPARE(demo.grabs,2);
        QCOMPARE(hd[0][1].value<SnapshotScene>().rasterize().size(),QSize(800,480));QVERIFY(hidden.isHidden());QVERIFY(hidden.updatesEnabled());
    }
    void nativeVideoRectUsesScaledCoordinates() {
        MultimediaDemo demo;demo.resize(1920,1080);QImage frame(320,180,QImage::Format_RGB32);frame.fill(Qt::blue);
        QSignalSpy encoded(&demo,&MultimediaDemo::processSnapshotAsync);
        QVERIFY(demo.submitScaledSnapshot(&demo,frame,QRect(960,540,960,540)));QCOMPARE(encoded.count(),1);
        QCOMPARE(encoded[0][0].value<QImage>().size(),QSize(560,315));QCOMPARE(encoded[0][2].toRect(),QRect(280,158,280,158));
    }
    void deferredAndDownloadCache() {
        MultimediaDemo demo;QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync),images(&demo,&MultimediaDemo::currentPlayImageChanged);
        demo.captureCompositeSnapshot();QCOMPARE(demo.grabs,0);
        for(int i=0;i<10;++i) demo.captureCompositeSnapshot();
        QTRY_COMPARE(encoded.count(),1);QCOMPARE(demo.grabs,1);QVERIFY(demo.m_snapshotWorkerBusy);
        demo.onSnapshotProcessFinished("cached-image");
        for(int i=0;i<10;++i) demo.captureCompositeSnapshot();
        QCOMPARE(demo.grabs,1);QCOMPARE(images.count(),11);QCOMPARE(encoded.count(),1);
        demo.m_lastSnapshotMs-=3000;demo.captureCompositeSnapshot();QCOMPARE(demo.grabs,1);
        QTRY_COMPARE(encoded.count(),2);QCOMPARE(demo.grabs,2);
    }
    void nestedCaptureIsCoalesced() {
        MultimediaDemo demo;QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync);
        demo.duringGrab=[&] { for(int i=0;i<20;++i) demo.captureCompositeSnapshot();QCoreApplication::processEvents(); };
        demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),1);QCOMPARE(demo.grabs,1);
        QTest::qWait(20);QCOMPARE(demo.grabs,1);
    }
    void oldSourceCompletionIsRejected() {
        MultimediaDemo demo;QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync),images(&demo,&MultimediaDemo::currentPlayImageChanged);
        demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),1);
        demo.m_currentVideoRectMediaPath="source-b";demo.captureCompositeSnapshot();QVERIFY(demo.m_lastSnapshotBase64.isEmpty());
        demo.onSnapshotProcessFinished("stale-a");QCOMPARE(images.count(),0);
        QTRY_COMPARE(encoded.count(),2);demo.onSnapshotProcessFinished("fresh-b");
        QCOMPARE(images.count(),1);QCOMPARE(images[0][0].toString(),QString("fresh-b"));
    }
    void samePathContentRefreshRejectsOldImage() {
        MultimediaDemo demo;QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync),images(&demo,&MultimediaDemo::currentPlayImageChanged);
        demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),1);++demo.m_snapshotContentRevision;
        demo.onSnapshotProcessFinished("old-file-content");QCOMPARE(images.count(),0);QTRY_COMPARE(encoded.count(),2);
        demo.onSnapshotProcessFinished("new-file-content");QCOMPARE(images.count(),1);
    }
    void geometryAndClockRollbackExpireCache() {
        MultimediaDemo demo;QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync);
        demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),1);demo.onSnapshotProcessFinished("a");
        demo.resize(330,190);demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),2);demo.onSnapshotProcessFinished("b");
        demo.m_lastSnapshotMs=QDateTime::currentMSecsSinceEpoch()+5000;demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),3);
    }
    void videoPipelineAndCacheIsolation() {
        MultimediaDemo demo;Player player;demo.mediaPlayer=&player;demo.m_downloadPauseActive=false;
        QSignalSpy encoded(&demo,&MultimediaDemo::processSnapshotAsync);QImage frame(320,180,QImage::Format_RGB32);frame.fill(Qt::blue);
        player.callback=[&] { demo.onVideoSnapshotReady(frame); };
        demo.captureCompositeSnapshot();QTRY_COMPARE(encoded.count(),1);QCOMPARE(player.requests,1);
        demo.onSnapshotProcessFinished("video-a");demo.captureCompositeSnapshot();QCOMPARE(player.requests,1);
        player.active=false;demo.captureCompositeSnapshot();QVERIFY(demo.m_lastSnapshotBase64.isEmpty());
        player.active=true;QSignalSpy company(&demo,&MultimediaDemo::companySnapshotReady);
        demo.captureCompanySnapshot("company-token");QCOMPARE(company.count(),1);QCOMPARE(company[0][0].toString(),QString("company-token"));
        QVERIFY(company[0][4].toString().isEmpty());QVERIFY(demo.m_lastSnapshotBase64.isEmpty());
    }
    void coalescedCaptureRepliesEveryRequest() {
        MultimediaDemo demo;Manager manager;MqttPage page;page.manager_=&manager;
        connect(&page,&MqttPage::requestCapturePlayImage,&demo,&MultimediaDemo::captureCompositeSnapshot);
        connect(&demo,&MultimediaDemo::currentPlayImageChanged,&page,&MqttPage::onCurrentPlayImageChanged);
        QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync);
        for(int i=0;i<12;++i) page.onGetPlayInfoRequested("topic",QString::number(i),"device");
        QTRY_COMPARE(encoded.count(),1);QCOMPARE(demo.grabs,1);QVERIFY(manager.ids.isEmpty());
        demo.onSnapshotProcessFinished("shared-image");QVERIFY(manager.ids.isEmpty());
        QTRY_COMPARE(manager.ids.size(),12);for(int i=0;i<12;++i) QCOMPARE(manager.ids[i],QString::number(i));
        QVERIFY(page.pendingPlayInfo_.isEmpty());QCOMPARE(manager.image,QString("shared-image"));
    }
    void busyUploadDoesNotCaptureAgain(){
        Manager manager;manager.busy=true;MqttPage page;page.manager_=&manager;QSignalSpy captures(&page,&MqttPage::requestCapturePlayImage);
        page.onGetPlayInfoRequested("topic","one","device");QTest::qWait(120);QCOMPARE(captures.count(),0);
        manager.busy=false;QTRY_COMPARE(captures.count(),1);page.onCurrentPlayImageChanged("fresh");QTRY_COMPARE(manager.ids,QStringList({"one"}));
    }
    void ordinaryUploadBackpressureRetainsRequestIds(){
        Manager manager;manager.busy=true;MqttPage page;page.manager_=&manager;
        page.onGetPlayInfoRequested("topic","one","device");page.onGetPlayInfoRequested("topic","two","device");page.onCurrentPlayImageChanged("cached");
        QTest::qWait(100);QVERIFY(manager.ids.isEmpty());QCOMPARE(page.pendingPlayInfo_.size(),2);
        manager.busy=false;QTRY_COMPARE(manager.ids,QStringList({"one","two"}));
    }
    void pendingRepliesAreBoundedWithoutDroppingIds() {
        Manager manager;MqttPage page;page.manager_=&manager;
        for(int i=0;i<40;++i) page.onGetPlayInfoRequested("topic",QString::number(i),"device");
        QCOMPARE(page.pendingPlayInfo_.size(),32);QCOMPARE(manager.ids.size(),8);
        page.onCurrentPlayImageChanged("image");QTRY_COMPARE(manager.ids.size(),40);
        for(int i=0;i<40;++i) QCOMPARE(manager.ids[i],QString::number(i));
    }
    void companyCaptureDoesNotLoseQueuedOrdinaryRefresh() {
        MultimediaDemo demo;QSignalSpy encoded(&demo,&MultimediaDemo::processSceneSnapshotAsync);
        demo.m_companySnapshotToken="company-inflight";demo.captureCompositeSnapshot();QTest::qWait(35);
        QCOMPARE(encoded.count(),0);QVERIFY(demo.m_snapshotRefreshQueued);
        demo.m_companySnapshotToken.clear();QTRY_COMPARE(encoded.count(),1);QCOMPARE(demo.grabs,1);
    }
    void queuedRefreshDiesWithWidget() {
        auto *demo=new MultimediaDemo;demo->captureCompositeSnapshot();delete demo;QTest::qWait(20);
    }
};
QTEST_MAIN(SnapshotTests)
#include "fixture.moc"
'''
    (output/'fixture.cpp').write_text(fixture,encoding='utf-8')
    project='QT += core gui widgets testlib\nCONFIG += console testcase c++17\nTARGET = snapshot_tests\n'
    project+='INCLUDEPATH += "'+root.as_posix()+'"\nSOURCES += fixture.cpp "'+(root/'common/debug/ui_performance.cpp').as_posix()+'"\n'
    project+='SOURCES += "'+(root/'components/multimedia/snapshot_processor.cpp').as_posix()+'"\nHEADERS += "'+(root/'components/multimedia/snapshot_processor.h').as_posix()+'"\n'
    project+='SOURCES += \"'+(root/'common/snapshot_scene.cpp').as_posix()+'\"\n'
    (output/'tests.pro').write_text(project,encoding='utf-8')
    qt=root/'.web-build-tools/sdk/5.15.2/mingw81_64/bin';gcc=root/'.web-build-tools/sdk/Tools/mingw810_64/bin'
    env=dict(os.environ,QT_QPA_PLATFORM='offscreen',QT_QPA_FONTDIR='C:/Windows/Fonts')
    env['PATH']=str(qt)+os.pathsep+str(gcc)+os.pathsep+env.get('PATH','')
    for command,name in [([str(qt/'qmake.exe'),'tests.pro'],'qmake.log'),([str(gcc/'mingw32-make.exe'),'-j4'],'build.log'),([str(output/'release/snapshot_tests.exe'),'-o',str(output/'results.txt')+',txt'],'run.log')]:
        result=subprocess.run(command,cwd=output,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
        (output/name).write_bytes(result.stdout)
        if result.returncode:
            print(result.stdout.decode(errors='replace')[-5000:]);result.check_returncode()
    results=(output/'results.txt').read_text(encoding='utf-8',errors='replace')
    print('\n'.join(line for line in results.splitlines() if line.startswith(('PASS','FAIL','Totals:'))))


if __name__=='__main__':
    main()
