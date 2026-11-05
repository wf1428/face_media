"""Run actual player lifecycle methods against a delayed GStreamer boundary.

Author: Du lin
Date: 2026-11-04
Note: 最近修改：2026-11-04；修改人：Du lin；新增真实NV12快照tryLock、写槽排除、只读分离和旧代次过滤，保留管线异步进度回归。
The production header, SerialWorkQueue, and extracted lifecycle methods are compiled.
Only hardware, GStreamer ABI calls, canvas and setup are replaced. No real device,
network or production configuration is accessed. This is not a target GStreamer build.
"""
import os
from pathlib import Path
import subprocess


def function(source, signature):
    """Extract one complete implementation, failing if its signature is missing."""
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError('Incomplete function: ' + signature)


def main():
    """Build and execute UI heartbeat, stale source, ordering and lifetime regressions."""
    root = Path.cwd()
    output = root / '.web-build/snapshot-final-20261104/media'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'include/gst/video').mkdir(parents=True, exist_ok=True)
    gst = r'''
#pragma once
#include <QtCore>
#include <atomic>
struct FakeGst { QString uri;int state=0;int id=0; };
using GstElement=FakeGst;using GstBus=FakeGst;using GstPad=FakeGst;using GstSample=FakeGst;
struct GstMessage;struct GstPadProbeInfo;
using gpointer=void*;using gulong=unsigned long;using gint64=qint64;using gdouble=double;
using GstBusSyncReply=int;using GstPadProbeReturn=int;using GstFlowReturn=int;using GstStateChangeReturn=int;
enum GstSeekFlags { GST_SEEK_FLAG_FLUSH=1,GST_SEEK_FLAG_KEY_UNIT=2 };
constexpr int GST_STATE_NULL=0,GST_STATE_PAUSED=1,GST_STATE_PLAYING=2,GST_STATE_CHANGE_FAILURE=-1,GST_FORMAT_TIME=0;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
constexpr qint64 GST_MSECOND=1000000;
#define G_OBJECT(o) (o)
'''
    (output / 'include/gst/gst.h').write_text(gst, encoding='utf-8')
    (output / 'include/gst/video/video.h').write_text('#pragma once\n', encoding='utf-8')
    source = (root / 'components/multimedia/gst_player_widget.cpp').read_text(encoding='utf-8')
    signatures = [
        'SnapshotVideoFrame GstPlayerWidget::snapshotVideoFrame() const',
        'QWidget *GstPlayerWidget::snapshotVideoWidget() const',
        'GstPlayerWidget::~GstPlayerWidget()',
        'void GstPlayerWidget::setMedia(', 'void GstPlayerWidget::play()',
        'void GstPlayerWidget::pause()', 'void GstPlayerWidget::stop()',
        'bool GstPlayerWidget::recreatePipeline()', 'void GstPlayerWidget::seekMs(',
        'void GstPlayerWidget::setVolume(', 'void GstPlayerWidget::applyEffectiveVolume()', 'float GstPlayerWidget::effectiveVolume01()',
        'void GstPlayerWidget::setPromptAudioMode(', 'void GstPlayerWidget::setPreloadMode(',
        'void GstPlayerWidget::releasePipeline()', 'void GstPlayerWidget::releasePipelineResources()',
        'void GstPlayerWidget::removeFrameProbes()', 'void GstPlayerWidget::pollPosition()',
    ]
    fixture = r'''
#include <QtTest>
#include <QtWidgets>
#include <memory>
#include <atomic>
#include "common/serial_work_queue.h"
#include "common/debug/ui_performance.h"
#define private public
#include "components/multimedia/gst_player_widget.h"
#undef private
static QMutex recordsMutex;static QStringList records;
static std::atomic_int entered{0},slowMs{0},objects{0},nextId{0};
static bool failPaused=false;
static void record(const QString &s);
static bool positionOk=true,durationOk=true;static std::atomic_int positionDelayMs{0};
static bool gst_element_query_position(GstElement *,int,gint64 *v) { Q_ASSERT(QThread::currentThread()!=qApp->thread());record("position");QThread::msleep(positionDelayMs.load());*v=12*GST_MSECOND;return positionOk; }
static bool gst_element_query_duration(GstElement *,int,gint64 *v) { Q_ASSERT(QThread::currentThread()!=qApp->thread());record("duration");*v=34*GST_MSECOND;return durationOk; }
static void record(const QString &s) { QMutexLocker lock(&recordsMutex);records<<s; }
static QStringList history() { QMutexLocker lock(&recordsMutex);return records; }
static GstElement *makeObject() { ++objects;auto *p=new FakeGst;p->id=++nextId;return p; }
static void gst_object_unref(FakeGst *p) { QVERIFY(QThread::currentThread()!=qApp->thread() || slowMs.load()==0);--objects;delete p; }
static int gst_element_set_state(GstElement *p,int state) {
    Q_ASSERT(QThread::currentThread()!=qApp->thread());++entered;
    record(QString("state:%1:%2").arg(p->id).arg(state));QThread::msleep(slowMs.load());
    if(failPaused && state==GST_STATE_PAUSED) return GST_STATE_CHANGE_FAILURE;
    p->state=state;return 0;
}
static bool setNullStateAndWait(GstElement *p) { return !p || gst_element_set_state(p,GST_STATE_NULL)!=GST_STATE_CHANGE_FAILURE; }
static void gst_bus_set_flushing(GstBus *,int) {}
static void gst_bus_set_sync_handler(GstBus *,void*,void*,void*) {}
static void gst_pad_remove_probe(GstPad *,gulong) {}
static void gst_element_seek_simple(GstElement *,int,GstSeekFlags,gint64) { record("seek"); }
static void setBooleanProperty(void *,const char *,int) {}
static void g_object_set(GstElement *p,const char *key,...) {
    Q_ASSERT(QThread::currentThread()!=qApp->thread());
    if(QByteArray(key)=="uri") { va_list args;va_start(args,key);p->uri=QString::fromUtf8(va_arg(args,const char*));va_end(args);record("uri:"+p->uri); }
}
class SharedAudioMixer { public: static SharedAudioMixer &instance(){static SharedAudioMixer v;return v;}void removeSource(void*){}void resetPromptSource(void*){} };
GstPlayerWidget::GstPlayerWidget(QWidget *p):QWidget(p) { pipelineQueue_.reset(new SerialWorkQueue("media.test")); }
void GstPlayerWidget::setVideoOutput(QWidget *w) { videoWidget_=w; }
void GstPlayerWidget::ensurePipeline() { if(!playbin_) { playbin_=makeObject();videoSink_=audioSink_=appSink_=playbin_;bus_=makeObject();setVolume(volume01_); } }
QString GstPlayerWidget::mediaUri(const QString &s) const { return s; }
void GstPlayerWidget::clearVideoCanvas() {}
bool GstPlayerWidget::useQtCompositedVideo() const {return true;}
VideoHoleWidget *GstPlayerWidget::videoCanvas() const {return nullptr;}
void GstPlayerWidget::resetPromptPrerollState() { promptAudioPrimed_.storeRelease(0);promptLeadInQueued_.storeRelease(0); }
void GstPlayerWidget::resetPlaybackDiagnostics() {}
void GstPlayerWidget::requestDisplayRectUpdate() {}
void GstPlayerWidget::pollBus() {}
void GstPlayerWidget::pollPlaybackDiagnostics() {}
void GstPlayerWidget::showEvent(QShowEvent *e) { QWidget::showEvent(e); }
void GstPlayerWidget::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); }
void GstPlayerWidget::moveEvent(QMoveEvent *e) { QWidget::moveEvent(e); }
'''
    fixture += '\n'.join(function(source, signature) for signature in signatures)
    fixture += r'''
class MediaAsyncTests:public QObject {
    Q_OBJECT
private slots:
    void videoSnapshotValuesDoNotBlockOrAlias(){
        GstPlayerWidget player;auto &old=player.frameMailboxSlots_[0];old.state=GstPlayerWidget::FrameSlotFree;old.sequence=1;old.generation=player.mediaGeneration_.loadAcquire();old.width=old.height=2;old.nv12Frame=QByteArray(6,'a');
        auto &latest=player.frameMailboxSlots_[1];latest=old;latest.state=GstPlayerWidget::FrameSlotReady;latest.sequence=2;latest.nv12Frame.fill('b');
        auto frame=player.snapshotVideoFrame();QVERIFY(frame.valid());QCOMPARE(frame.nv12[0],char('b'));latest.nv12Frame.data()[0]='x';QCOMPARE(frame.nv12[0],char('b'));
        latest.state=GstPlayerWidget::FrameSlotWriting;QCOMPARE(player.snapshotVideoFrame().nv12[0],char('a'));
        player.frameMailboxMutex_.lock();const auto locked=player.snapshotVideoFrame();player.frameMailboxMutex_.unlock();QVERIFY(!locked.valid());
        player.mediaGeneration_.fetchAndAddOrdered(1);QVERIFY(!player.snapshotVideoFrame().valid());
    }
    void init() { QMutexLocker lock(&recordsMutex);records.clear();entered=0;slowMs=0;positionDelayMs=0;failPaused=false;QCOMPARE(objects.load(),0); }
    void positionQueriesRetainSemantics() {
        GstPlayerWidget player;player.ensurePipeline();QTRY_VERIFY(!player.pipelineQueue_->busy());player.prepared_=true;
        connect(&player,&GstPlayerWidget::positionChanged,&player,[](int p,int d){record(QString("notify:%1:%2").arg(p).arg(d));});
        { QMutexLocker lock(&recordsMutex);records.clear(); }
        positionOk=durationOk=true;player.pollPosition();QTRY_VERIFY(!player.pipelineQueue_->busy());QCOMPARE(history(),QStringList({"position","duration","notify:12:34"}));
        { QMutexLocker lock(&recordsMutex);records.clear(); }
        positionOk=false;player.pollPosition();QTRY_VERIFY(!player.pipelineQueue_->busy());QCOMPARE(history(),QStringList({"position"}));
        { QMutexLocker lock(&recordsMutex);records.clear(); }
        positionOk=true;durationOk=false;player.pollPosition();QTRY_VERIFY(!player.pipelineQueue_->busy());QCOMPARE(history(),QStringList({"position","duration","notify:12:34"}));
        durationOk=true;
        player.pipelineQueue_->enqueue([]{return SerialWorkQueue::Task{[]{QThread::msleep(40);return QString();},{}};});
        { QMutexLocker lock(&recordsMutex);records.clear(); }player.pollPosition();QVERIFY(history().isEmpty());
        QTRY_VERIFY(!player.pipelineQueue_->busy());
    }
    void slowPositionStaysOffUiAndFiltersOldSource() {
        GstPlayerWidget player;player.ensurePipeline();QTRY_VERIFY(!player.pipelineQueue_->busy());player.prepared_=true;
        QSignalSpy notified(&player,&GstPlayerWidget::positionChanged);positionDelayMs=90;
        int beats=0;QTimer heartbeat;heartbeat.setInterval(5);connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start();
        QElapsedTimer elapsed;elapsed.start();player.pollPosition();QVERIFY(elapsed.elapsed()<25);
        for(int i=0;i<10;++i) player.pollPosition();
        QTest::qWait(25);QVERIFY(beats>=2);player.setMedia("replacement");
        QTRY_VERIFY(!player.pipelineQueue_->busy());QCOMPARE(notified.count(),0);
        QCOMPARE(history().count("position"),1);positionDelayMs=0;
    }
    void serialUiAndSupersede() {
        GstPlayerWidget player;int ticks=0;QTimer heartbeat;heartbeat.setInterval(5);connect(&heartbeat,&QTimer::timeout,[&]{++ticks;});heartbeat.start();
        slowMs=80;player.setMedia("old");player.play();QTRY_VERIFY(entered.load()>0);
        QElapsedTimer call;call.start();player.stop();player.setMedia("new");player.play();QVERIFY(call.elapsed()<25);
        QTRY_VERIFY_WITH_TIMEOUT(!player.pipelineQueue_->busy(),2000);
        const auto log=history();QVERIFY(!log.contains("uri:old"));QVERIFY(log.contains("uri:new"));QVERIFY(ticks>=8);
        QCOMPARE(player.playbin_->state,GST_STATE_PLAYING);slowMs=0;
    }
    void stopFiltersQueuedPlay() {
        GstPlayerWidget player;slowMs=70;player.setMedia("old");player.play();QTRY_VERIFY(entered.load()>0);
        player.stop();QTRY_VERIFY_WITH_TIMEOUT(!player.pipelineQueue_->busy(),2000);
        for(const auto &line:history()) QVERIFY(!line.endsWith(":2"));
        QCOMPARE(player.playbin_->state,GST_STATE_NULL);QVERIFY(!player.playRequested_);slowMs=0;
    }
    void releaseAndRecreateBarrier() {
        GstPlayerWidget player;slowMs=20;player.setMedia("old");player.play();
        player.recreatePipeline();player.setPreloadMode(true);player.setPreloadMode(false);
        player.setMedia("new");player.play();QTRY_VERIFY_WITH_TIMEOUT(!player.pipelineQueue_->busy(),2000);
        QCOMPARE(player.playbin_->uri,QString("new"));QCOMPARE(player.playbin_->state,GST_STATE_PLAYING);
        QCOMPARE(objects.load(),2);slowMs=0;
    }
    void invalidSourceStopsPrevious() {
        GstPlayerWidget player;player.setMedia("old");player.play();QTRY_VERIFY(!player.pipelineQueue_->busy());
        QSignalSpy errors(&player,&GstPlayerWidget::errorOccured);player.setMedia("");QTRY_VERIFY(!player.pipelineQueue_->busy());
        QCOMPARE(errors.size(),1);QCOMPARE(player.playbin_->state,GST_STATE_NULL);
    }
    void failureIsReported() {
        GstPlayerWidget player;QSignalSpy errors(&player,&GstPlayerWidget::errorOccured);failPaused=true;
        player.setMedia("bad");QTRY_VERIFY(!player.pipelineQueue_->busy());QCOMPARE(errors.size(),1);
    }
    void promptGating() {
        GstPlayerWidget player;player.setPromptAudioMode(true);player.setMedia("prompt");player.play();QTRY_VERIFY(!player.pipelineQueue_->busy());
        QCOMPARE(player.playbin_->state,GST_STATE_PAUSED);player.promptAudioPrimed_.storeRelease(1);player.promptLeadInQueued_.storeRelease(1);
        player.play();QTRY_VERIFY(!player.pipelineQueue_->busy());QCOMPARE(player.playbin_->state,GST_STATE_PLAYING);
    }
    void cancellationAndCompletionLifetime() {
        auto *queue=new SerialWorkQueue("lifetime");bool completed=false;int stale=0;
        queue->enqueue([&]{return SerialWorkQueue::Task{[]{QThread::msleep(30);return QString();},[&](const QString &){completed=true;delete queue;queue=nullptr;}};});
        queue->enqueue([&]{++stale;return SerialWorkQueue::Task{};});
        QTRY_VERIFY(completed);QCOMPARE(stale,0);QVERIFY(!queue);
    }
    void cleanup() { slowMs=0;QCOMPARE(objects.load(),0); }
};
QTEST_MAIN(MediaAsyncTests)
#include "fixture.moc"
'''
    (output / 'fixture.cpp').write_text(fixture, encoding='utf-8')
    pro = 'QT += core gui widgets opengl testlib\nCONFIG += console testcase c++17\nTARGET = media_async_tests\n'
    pro += 'INCLUDEPATH += include "' + root.as_posix() + '"\nSOURCES += fixture.cpp "' + (root / 'common/debug/ui_performance.cpp').as_posix() + '"\n'
    pro += 'HEADERS += "' + (root / 'components/multimedia/gst_player_widget.h').as_posix() + '"\n'
    (output / 'tests.pro').write_text(pro, encoding='utf-8')
    qt = root / '.web-build-tools/sdk/5.15.2/mingw81_64/bin'
    gcc = root / '.web-build-tools/sdk/Tools/mingw810_64/bin'
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QPA_FONTDIR='C:/Windows/Fonts')
    env['PATH'] = str(qt) + os.pathsep + str(gcc) + os.pathsep + env.get('PATH', '')
    for command, name in [([str(qt / 'qmake.exe'),'tests.pro'],'qmake.log'),([str(gcc / 'mingw32-make.exe'),'-j4'],'build.log'),([str(output / 'release/media_async_tests.exe'),'-o',str(output / 'results.txt')+',txt'],'run.log')]:
        result = subprocess.run(command, cwd=output, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (output / name).write_bytes(result.stdout)
        if result.returncode:
            print(result.stdout.decode('utf-8', errors='replace')[-6000:])
            result.check_returncode()
    print((output / 'results.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    main()
