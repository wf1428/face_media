"""Run production broadcast control and PCM buffering with a simulated audio sink.

Author: Du lin
Created: 2026-11-05
Note: 最近修改：2026-11-10；修改人：Du lin；更新循环排程回归，验证有效时段首次立即申请播放、后续按平台间隔而非音频时长固定触发及迟到轮次跳过。
Runs against an isolated configuration/cache directory. A loopback FTP fixture exercises the real curl download. Board audio and the broker
use isolated test seams. Uses the bundled Windows Qt build tools when available.
"""
import os
from pathlib import Path
import shutil
import socket
import socketserver
import subprocess
from threading import Thread


FTP_BODY = b'isolated broadcast FTP fixture'


class FixtureFtp(socketserver.StreamRequestHandler):
    """Serve one known file over a loopback-only passive FTP connection."""

    def handle(self):
        """Answer the curl command subset and always close the passive listener."""
        listener = None
        self.wfile.write(b'220 local test FTP\r\n')
        try:
            for line in self.rfile:
                command = line.strip().split(maxsplit=1)[0]
                response = b'502 unsupported\r\n'
                if command == b'USER':
                    response = b'331 password\r\n'
                elif command == b'PASS':
                    response = b'230 logged in\r\n'
                elif command in (b'CWD', b'TYPE'):
                    response = b'250 OK\r\n'
                elif command == b'PWD':
                    response = b'257 "/"\r\n'
                elif command == b'EPSV':
                    listener = socket.create_server(('127.0.0.1', 0))
                    listener.settimeout(5)
                    response = b'229 Entering Extended Passive Mode (|||%d|)\r\n' % listener.getsockname()[1]
                elif command == b'SIZE':
                    response = b'213 %d\r\n' % len(FTP_BODY)
                elif command == b'RETR' and listener is not None:
                    self.wfile.write(b'150 opening data\r\n')
                    transfer, _ = listener.accept()
                    with transfer:
                        transfer.sendall(FTP_BODY)
                    response = b'226 transfer complete\r\n'
                elif command == b'QUIT':
                    self.wfile.write(b'221 goodbye\r\n')
                    break
                self.wfile.write(response)
        finally:
            if listener is not None:
                listener.close()


def main():
    """Build and exercise the production broadcast files with Qt and output logs."""
    root = Path(__file__).resolve().parents[3]
    out = root / '.web-build/broadcast-20261105/qt-regression'
    stubs = out / 'stubs/components/multimedia'
    stubs.mkdir(parents=True, exist_ok=True)
    alsa = out / 'stubs/src/call/AlsaAudioDevice.h'
    alsa.parent.mkdir(parents=True, exist_ok=True)
    alsa.write_text(r'''
#pragma once
#include "src/call/CallProtocol.h"
#include <QString>
#include <atomic>
class AlsaAudioDevice {
public:
    inline static std::atomic_int playbackBlocks{0}, playbackOpens{0}, playbackCloses{0}, captureOpens{0};
    bool open(bool capture, QString &) {
        if (capture) { ++captureOpens; return false; }
        ++playbackOpens; opened_=true; return true;
    }
    bool transfer(VoiceCall::Pcm &, const std::atomic_bool &stop) {
        if (stop.load()) return false;
        ++playbackBlocks; return true;
    }
    void close() { if (opened_) { opened_=false; ++playbackCloses; } }
    uint64_t xruns() const { return 0; }
private:
    bool opened_=false;
};
''', encoding='utf-8')
    player = stubs / 'gst_player_widget.h'
    player.write_text(r'''
#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <atomic>
#include <QThread>
class QWidget;
class GstPlayerWidget : public QObject {
    Q_OBJECT
public:
    void setPromptAudioMode(bool) {}
    static qint64 probeAudioDurationMs(const QString &) { return 1000; }
    void enablePriorityAudio() {}
    void setVideoOutput(QWidget *) {}
    void setVolume(float) {}
    void setMedia(const QString &) {}
    void play() {}
    void pause() {}
    void stop() {}
    static bool acquireBroadcastAudioOutput(std::atomic_bool &stop) {
        outputOwned=!stop.load();
        return outputOwned.load();
    }
    inline static std::atomic_bool outputOwned{false}, holdReturn{false}, returning{false};
    static bool releaseBroadcastAudioOutput() {
        returning=true;
        for (int i=0; holdReturn && i<200; ++i) QThread::msleep(10);
        outputOwned=false;
        returning=false;
        return true;
    }
signals:
    void positionChanged(int,int);
    void videoFinished();
    void errorOccured(const QString &);
};
''', encoding='utf-8')
    focus = stubs / 'vol_ctrl.h'
    focus.write_text(r'''
#pragma once
#include <QObject>
#include <QHash>
#include <QTimer>
class GstPlayerWidget;
class AudioPromptFocusManager : public QObject {
    Q_OBJECT
public:
    enum class Priority { Prompt, Tagged, Schedule, Live, Call };
    static AudioPromptFocusManager &instance() { static AudioPromptFocusManager value;return value; }
    void acquire(QObject *owner,GstPlayerWidget *,Priority priority) {
        const bool hadCall=callOwnsFocus();
        requests.insert(owner,priority);update();
        if (!hadCall && callOwnsFocus()) emit callFocusChanged(true);
    }
    void release(QObject *owner) {
        const bool hadCall=callOwnsFocus();
        requests.remove(owner);update();
        if (hadCall && !callOwnsFocus()) emit callFocusChanged(false);
    }
    bool callOwnsFocus() const {return active && requests.value(active)==Priority::Call;}
    bool blocks(Priority priority) const {return active && requests.value(active)>priority;}
    int readyDelayMs=0;
signals:
    void focusReady(QObject *);
    void focusSuspended(QObject *);
    void callFocusChanged(bool);
private:
    void update() {
        QObject *next=nullptr;
        for(auto i=requests.cbegin();i!=requests.cend();++i)
            if(!next || i.value()>requests.value(next))next=i.key();
        if(next==active)return;
        auto *prior=active;active=next;
        if(prior && requests.contains(prior))emit focusSuspended(prior);
        QTimer::singleShot(readyDelayMs,this,[this,next]{if(next && active==next)emit focusReady(next);});
    }
    QHash<QObject *,Priority> requests;
    QObject *active=nullptr;
};
''', encoding='utf-8')
    source = out / 'test.cpp'
    source.write_text(r'''
#include <QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <memory>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QtEndian>
#include <chrono>
#define private public
#include "components/settings/mqtt/broadcast_service.h"
#undef private
#include "components/settings/mqtt/broadcast_live.h"
#include "components/multimedia/gst_player_widget.h"
#include "components/multimedia/vol_ctrl.h"
#include "platform/rk3566_platform.h"
#include "src/call/AlsaAudioDevice.h"
static QString configPath;
QString Rk3566Platform::configDir() {return configPath;}
static const QString device="CAM01", request="device/ycLinux/CAM01/request",
    audioTopic="device/ycLinux/CAM01/broadcast/audio";
static QJsonObject cmd(const QString &method,const QString &id,const QJsonObject &data) {
    return {{"method",method},{"id",id},{"deviceId",device},{"data",data}};
}
static QJsonObject media(int ms,quint32 stamp,const QString &epoch,quint8 sample=0) {
    QByteArray bytes=QByteArray::fromHex("a1c001000100")+QByteArray(4+ms*32,char(0));
    qToBigEndian<quint32>(stamp,reinterpret_cast<uchar *>(bytes.data()+6));
    for(int i=10;i<bytes.size();i+=2) bytes[i]=char(sample);
    using namespace std::chrono;
    const auto now=duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    return {{"type","broadcast_audio"},{"topic",audioTopic},{"epoch",epoch},
            {"hex",QString::fromLatin1(bytes.toHex())},{"arrivalMs",double(now)}};
}
class BroadcastTests : public QObject {
    Q_OBJECT
private slots:
    void framesAndGeneration() {
        BroadcastLive live;auto receive=live.receiver();
        AlsaAudioDevice::playbackBlocks=0;live.configure(audioTopic,40,true,"epoch-2");
        receive(media(40,0,"epoch-1"));receive(media(20,0,"epoch-2"));
        QTRY_VERIFY(live.statistics().silenceBlocks>=2);
        QCOMPARE(live.statistics().acceptedFrames,quint64(0));
        receive(media(40,0,"epoch-2"));
        const auto mediaBlocks=[&] {
            const auto stats=live.statistics();
            return stats.alsaBlocks-stats.silenceBlocks;
        };
        QTRY_COMPARE(mediaBlocks(),quint64(2));
        QTRY_VERIFY(live.statistics().underruns>0);
        QCOMPARE(live.statistics().ipcFrames,quint64(3));
        QCOMPARE(live.statistics().acceptedFrames,quint64(1));
        QCOMPARE(live.statistics().nonzeroBlocks,quint64(0));
        live.configure(audioTopic,20,true,"epoch-3");
        receive(media(20,40,"epoch-3",100));receive(media(20,20,"epoch-3",100));
        QTRY_COMPARE(live.statistics().nonzeroBlocks,quint64(2));
        QCOMPARE(live.statistics().peak,80);
        live.configure({},20,false,{});
        receive(media(20,60,"epoch-3",100));
        QTRY_COMPARE(AlsaAudioDevice::playbackCloses.load(), 2);
        QCOMPARE(AlsaAudioDevice::playbackOpens.load(), 2);
        QCOMPARE(AlsaAudioDevice::captureOpens.load(), 0);
        const int stopped=AlsaAudioDevice::playbackBlocks.load();
        QTest::qWait(70);
        QCOMPARE(AlsaAudioDevice::playbackBlocks.load(),stopped);
        QCOMPARE(live.statistics().acceptedFrames,quint64(0));
    }
    void cachedPlanAndConfigDeletion() {
        QTemporaryDir temp; QVERIFY(temp.isValid());configPath=temp.path();
        const QByteArray pcm("file-content");
        const QString md5=QString::fromLatin1(QCryptographicHash::hash(pcm,QCryptographicHash::Md5).toHex());
        QDir dir(configPath+"/broadcast/"+device);QVERIFY(QDir().mkpath(dir.path()));
        QFile audio(dir.filePath("5-"+md5+".wav"));QVERIFY(audio.open(QIODevice::WriteOnly));
        QCOMPARE(audio.write(pcm),qint64(pcm.size()));audio.close();
        QList<QJsonObject> tx;
        BroadcastService svc([&](const QJsonObject &value){tx.append(value);return true;},nullptr);
        svc.configure(device,{});svc.setOnline(true);
        auto has=[&](const QString &method,const QString &field,const QString &value) {
            for(const auto &line:tx) {
                const auto body=line.value("payload").toObject();
                if(body.value("method").toString()==method && body.value("data").toObject().value(field).toString()==value)
                    return true;
            }
            return false;
        };
        const auto now=QDateTime::currentDateTime();
        const QString start=now.addSecs(-120).time().toString("HH:mm"),end=now.addSecs(180).time().toString("HH:mm");
        QJsonObject descriptor{{"audioId",5},{"name","file.wav"},{"md5",md5},{"size",pcm.size()},
            {"ftpPath","broadcast/scope/audio/file.wav"}};
        QJsonObject plan{{"planId",1},{"mode","SCHEDULED"},{"days",QJsonArray{1,2,3,4,5,6,7}},
            {"audioId",5},{"start",start},{"end",end},{"playMode","ONCE"},{"enabled",true}};
        QJsonObject config{{"cfgVersion",100},{"enabled",true},{"audios",QJsonArray{descriptor}},{"plans",QJsonArray{plan}}};
        QVERIFY(svc.handle(request,cmd("broadcastSet","cfg",config)));
        QTRY_VERIFY(has("broadcastState","state","PLAYING"));
        QVERIFY(QFileInfo::exists(dir.filePath("config.json")));
        const QJsonObject compact{{"cfgVersion",0},{"enabled",false},{"applyPolicy","IMMEDIATE"}};
        tx.clear();QVERIFY(svc.handle(request,cmd("broadcastSet","off",compact)));
        QTRY_VERIFY(has("broadcastState","state","IDLE"));
        QVERIFY(has("broadcastAck","result","OK"));
        auto state=tx.constLast().value("payload").toObject().value("data").toObject();
        QVERIFY(state.value("queue").isArray() && state.value("queue").toArray().isEmpty());
        QFile saved(dir.filePath("config.json"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto snapshot=QJsonDocument::fromJson(saved.readAll()).object();
        saved.close();
        QCOMPARE(snapshot.value("cfgVersion").toInt(),100);
        QCOMPARE(snapshot.value("enabled").toBool(),false);
        QCOMPARE(snapshot.value("audios").toArray().size(),1);
        QVERIFY(snapshot.value("plans").toArray().isEmpty());
        QVERIFY(snapshot.value("once").toObject().isEmpty());
        QVERIFY(QFileInfo::exists(audio.fileName()));
        QVERIFY(svc.local_.contains("5"));

        BroadcastService restarted({},nullptr);
        restarted.configure(device,{});
        QCOMPARE(restarted.version_,qint64(100));
        QVERIFY(!restarted.enabled_ && restarted.plans_.isEmpty());
        QTRY_VERIFY(restarted.local_.contains("5"));
        auto stale=config;stale["cfgVersion"]=99;
        tx.clear();
        QVERIFY(svc.handle(request,cmd("broadcastSet","stale",stale)));
        QVERIFY(has("broadcastAck","result","REJECTED"));
        auto fullZero=config;fullZero["cfgVersion"]=0;
        tx.clear();
        QVERIFY(svc.handle(request,cmd("broadcastSet","full-zero",fullZero)));
        QVERIFY(has("broadcastAck","result","REJECTED"));

        tx.clear();
        QVERIFY(svc.handle(request,cmd("broadcastSet","replay",compact)));
        QTRY_VERIFY(has("broadcastAck","result","OK"));
        QCOMPARE(svc.version_,qint64(100));

        config["cfgVersion"]=101;
        tx.clear();QVERIFY(svc.handle(request,cmd("broadcastSet","restore",config)));
        QTRY_VERIFY(has("broadcastState","state","PLAYING"));
        auto invalid=compact;invalid["cfgVersion"]="0";
        tx.clear();QVERIFY(svc.handle(request,cmd("broadcastSet","string-zero",invalid)));
        QTRY_VERIFY(has("broadcastAck","result","REJECTED"));
        QCOMPARE(svc.plans_,config.value("plans").toArray());
        tx.clear();QVERIFY(svc.handle(request,cmd("broadcastSet","off",compact)));
        QVERIFY(has("broadcastAck","result","OK"));
        QCOMPARE(svc.version_,qint64(101));
        QVERIFY(!svc.plans_.isEmpty());

        auto positive=compact;positive["cfgVersion"]=102;
        tx.clear();QVERIFY(svc.handle(request,cmd("broadcastSet","positive-off",positive)));
        QTRY_VERIFY(has("broadcastState","state","IDLE"));
        QCOMPARE(svc.version_,qint64(102));
        config["cfgVersion"]=103;config["enabled"]=false;config["plans"]=QJsonArray();
        tx.clear();QVERIFY(svc.handle(request,cmd("broadcastSet","full-off",config)));
        QTRY_VERIFY(has("broadcastState","state","IDLE"));
        QCOMPARE(svc.version_,qint64(103));
    }
    void initialUnversionedClearRestoresCatalog() {
        QTemporaryDir temp; QVERIFY(temp.isValid());configPath=temp.path();
        BroadcastService svc({},nullptr);
        svc.configure(device,{});
        const QByteArray pcm("file-content");
        const QString md5=QString::fromLatin1(QCryptographicHash::hash(pcm,QCryptographicHash::Md5).toHex());
        QDir dir(configPath+"/broadcast/"+device);QVERIFY(QDir().mkpath(dir.path()));
        QFile audio(dir.filePath("5-"+md5+".wav"));QVERIFY(audio.open(QIODevice::WriteOnly));
        QCOMPARE(audio.write(pcm),qint64(pcm.size()));audio.close();
        svc.audio_.insert("5",{{"audioId",5},{"name","file.wav"},{"md5",md5},{"size",pcm.size()},
            {"ftpPath","broadcast/scope/audio/file.wav"}});
        const QJsonObject compact{{"cfgVersion",0},{"enabled",false},{"applyPolicy","IMMEDIATE"}};
        QVERIFY(svc.handle(request,cmd("broadcastSet","initial-off",compact)));
        QTRY_VERIFY(svc.replies_.contains("broadcastSet/initial-off"));
        QCOMPARE(svc.replies_.value("broadcastSet/initial-off").second.value("result").toString(),QString("OK"));
        BroadcastService restarted({},nullptr);
        restarted.configure(device,{});
        QCOMPARE(restarted.version_,qint64(0));
        QVERIFY(!restarted.enabled_ && restarted.plans_.isEmpty());
        QTRY_VERIFY(restarted.local_.contains("5"));
    }
    void tagPlayUsesAudioIdWithoutVersionMatch() {
        QTemporaryDir temp; QVERIFY(temp.isValid());configPath=temp.path();
        QTemporaryFile first(temp.filePath("first-XXXXXX"));QVERIFY(first.open());
        QTemporaryFile second(temp.filePath("second-XXXXXX"));QVERIFY(second.open());
        QHash<QString,QJsonObject> replies;
        BroadcastService svc([&](const QJsonObject &packet) {
            const auto body=packet.value("payload").toObject();
            if (body.value("method")=="broadcastTagPlay")
                replies.insert(body.value("id").toString(),body.value("data").toObject());
            return true;
        },nullptr);
        svc.configure(device,{});
        svc.setOnline(true);
        svc.enabled_=true;svc.version_=100;
        svc.local_.insert("5",first.fileName());
        svc.local_.insert("8",second.fileName());

        QJsonObject data{{"action","START"},{"audioId",5},{"tag","B7-DOWN"},
            {"tagFloor",-7},{"tagDirection","DOWN"},{"volume",70}};
        QVERIFY(svc.handle(request,cmd("broadcastTagPlay","tag-no-version",data)));
        QTRY_COMPARE(replies.value("tag-no-version").value("result").toString(),QString("OK"));
        QCOMPARE(replies.value("tag-no-version").value("audioId").toInt(),5);
        QCOMPARE(replies.value("tag-no-version").value("tag").toString(),QString("B7-DOWN"));
        emit svc.tagged_.player->videoFinished();

        data["audioId"]=8;
        data["audioName"]=QStringLiteral("different-display-name.mp3");
        data["cfgVersion"]=99;
        QVERIFY(svc.handle(request,cmd("broadcastTagPlay","tag-old-version",data)));
        QTRY_COMPARE(replies.value("tag-old-version").value("result").toString(),QString("OK"));
        QCOMPARE(replies.value("tag-old-version").value("audioId").toInt(),8);
        emit svc.tagged_.player->videoFinished();

        data["tagDirection"]="UP";
        QVERIFY(svc.handle(request,cmd("broadcastTagPlay","tag-invalid",data)));
        QCOMPARE(replies.value("tag-invalid").value("result").toString(),QString("NO_AUDIO"));
        data["tagDirection"]="DOWN";
        data["audioId"]=12;
        QVERIFY(svc.handle(request,cmd("broadcastTagPlay","tag-missing",data)));
        QCOMPARE(replies.value("tag-missing").value("result").toString(),QString("NO_AUDIO"));
    }
    /** @brief 验证首次立即申请以及从首播时刻按平台间隔生成后续固定轮次。 */
    void fixedScheduleSlots() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QTemporaryFile audio(temp.filePath("audio-XXXXXX")); QVERIFY(audio.open());
        BroadcastService svc({},nullptr);
        svc.directory_=temp.path(); svc.enabled_=true;
        svc.local_.insert("5",audio.fileName());
        svc.audio_.insert("5",{{"durationSec",45}});
        const QDateTime anchor(QDate(2026,11,9),QTime(10,0),Qt::LocalTime);
        const QString occurrence="1/2026-11-09";
        QJsonObject loop{{"planId",1},{"audioId",5},{"start","10:00"},{"end","10:05"},
            {"days",QJsonArray{1}},{"playMode","LOOP"},{"intervalSec",60}};
        svc.plans_=QJsonArray{loop};
        const QDateTime first=anchor.addSecs(94);
        svc.schedule(first,true);
        QVERIFY(svc.scheduled_.plan.isEmpty());
        svc.schedule(first,false);
        QCOMPARE(svc.scheduled_.plan,loop);
        QVERIFY(svc.scheduled_.automatic);
        QCOMPARE(svc.scheduled_.dueMs,qint64(-1));
        svc.stopFile(svc.scheduled_);

        // 首播时间由获焦回调记录；这里注入确定的 10:01:34 以验证后续轮次。
        svc.loopAnchor_.insert(occurrence,first.toMSecsSinceEpoch());
        svc.lastLoop_.insert(occurrence,first.toMSecsSinceEpoch());
        svc.schedule(first.addSecs(59),false);
        QVERIFY(svc.scheduled_.plan.isEmpty());
        svc.schedule(first.addMSecs(60200),false);
        QCOMPARE(svc.scheduled_.plan,loop);
        QCOMPARE(svc.scheduled_.dueMs,first.addSecs(60).toMSecsSinceEpoch());
        QCOMPARE(svc.lastLoop_.value(occurrence),first.addSecs(60).toMSecsSinceEpoch());
        svc.stopFile(svc.scheduled_);
        svc.schedule(first.addMSecs(120700),false);
        QVERIFY(svc.scheduled_.plan.isEmpty());
        svc.schedule(first.addMSecs(180100),true);
        QCOMPARE(svc.lastLoop_.value(occurrence),first.addSecs(180).toMSecsSinceEpoch());
        svc.schedule(first.addMSecs(180200),false);
        QVERIFY(svc.scheduled_.plan.isEmpty());
        svc.schedule(first.addMSecs(240100),false);
        QVERIFY(svc.scheduled_.plan.isEmpty()); // 时段结束后不再开新轮次。
    }
    /** @brief 验证首次获焦时才记录循环锚点，音频时长缺失不阻止首播。 */
    void firstLoopPlaybackSetsAnchor() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QTemporaryFile audio(temp.filePath("audio-XXXXXX")); QVERIFY(audio.open());
        BroadcastService svc({},nullptr);
        svc.directory_=temp.path(); svc.enabled_=true;
        svc.local_.insert("5",audio.fileName());
        const auto now=QDateTime::currentDateTime();
        const auto start=now.addSecs(-60), end=now.addSecs(180);
        QJsonObject loop{{"planId",7},{"audioId",5},{"start",start.time().toString("HH:mm")},
            {"end",end.time().toString("HH:mm")},{"days",QJsonArray{1,2,3,4,5,6,7}},
            {"playMode","LOOP"},{"intervalSec",60}};
        svc.plans_=QJsonArray{loop};
        const qint64 before=QDateTime::currentMSecsSinceEpoch();
        svc.schedule(now,false);
        const QString occurrence=svc.scheduled_.occurrence;
        QVERIFY(!occurrence.isEmpty());
        QTRY_VERIFY(svc.loopAnchor_.contains(occurrence));
        QVERIFY(svc.loopAnchor_.value(occurrence)>=before);
        QVERIFY(svc.loopAnchor_.value(occurrence)<=QDateTime::currentMSecsSinceEpoch());
    }
    void callFocusStopsScheduledPlayback() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QTemporaryFile audio(temp.filePath("audio-XXXXXX")); QVERIFY(audio.open());
        BroadcastService svc({},nullptr);
        svc.directory_=temp.path(); svc.enabled_=true;
        svc.local_.insert("5",audio.fileName());
        const auto now=QDateTime::currentDateTime();
        const auto start=now.addSecs(-120), end=now.addSecs(120);
        QJsonObject once{{"planId",1},{"audioId",5},{"start",start.time().toString("HH:mm")},
            {"end",end.time().toString("HH:mm")},{"days",QJsonArray{1,2,3,4,5,6,7}},{"playMode","ONCE"}};
        svc.plans_=QJsonArray{once};
        svc.schedule(now,false);
        QTRY_VERIFY(svc.scheduled_.started);
        auto &focus=AudioPromptFocusManager::instance();
        QObject call;
        focus.acquire(&call,nullptr,AudioPromptFocusManager::Priority::Call);
        QVERIFY(svc.scheduled_.plan.isEmpty());
        focus.release(&call);
        svc.schedule(now,false);
        QVERIFY(svc.scheduled_.plan.isEmpty());

        once["planId"]=2;svc.plans_=QJsonArray{once};
        focus.acquire(&call,nullptr,AudioPromptFocusManager::Priority::Call);
        QVERIFY(svc.once_.contains("2"));
        focus.release(&call);
        svc.schedule(now,false);
        QVERIFY(svc.scheduled_.plan.isEmpty());
        once["planId"]=3;svc.plans_=QJsonArray{once};
        svc.schedule(now,false);
        QTRY_VERIFY(svc.scheduled_.started); // 通话暂停释放焦点后，新到点计划可以播。
    }
    void ftpDownloadAndIntegrity() {
        const int port=qEnvironmentVariableIntValue("BROADCAST_TEST_FTP_PORT");
        QVERIFY(port>0);
        QTemporaryDir temp; configPath=temp.path();
        QList<QJsonObject> tx;
        BroadcastService svc([&](const QJsonObject &value){tx.append(value);return true;},nullptr);
        svc.configure(device,"127.0.0.1");svc.setOnline(true);
        const auto progress=[&](const QString &id,const QString &expected) {
            for (const auto &message : tx) {
                const auto payload=message.value("payload").toObject();
                if(payload.value("id").toString()!=id)continue;
                const auto audio=payload.value("data").toObject().value("audios").toArray();
                if(!audio.isEmpty() && audio.first().toObject().value("download").toString()==expected)return true;
            }
            return false;
        };
        const QByteArray fixture("isolated broadcast FTP fixture");
        const QString hash=QString::fromLatin1(QCryptographicHash::hash(fixture,QCryptographicHash::Md5).toHex());
        QJsonObject entry{{"audioId",6},{"op","ADD"},{"md5",QString(32,'0')},{"name","ftp.mp3"},
            {"size",fixture.size()},{"ftpPath","broadcast/fixture/ftp.mp3"},{"ftpPort",port},
            {"ftpUser","fixture-user"},{"ftpPwd","fixture-password"}};
        QVERIFY(svc.handle(request,cmd("broadcastAudioSync","bad-md5",entry)));
        QTRY_VERIFY_WITH_TIMEOUT(progress("bad-md5","FAILED"),10000);
        const QDir cache(temp.filePath("broadcast/"+device));
        QVERIFY(cache.entryList(QDir::Files).isEmpty());
        entry["md5"]=hash;
        QVERIFY(svc.handle(request,cmd("broadcastAudioSync","good",entry)));
        QTRY_VERIFY_WITH_TIMEOUT(progress("good","DONE"),10000);
        QFile file(cache.filePath("6-"+hash+".mp3"));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),fixture);
    }
    void resumeUsesNewFocusDeadline() {
        QTemporaryDir temp;configPath=temp.path();
        bool gate=false;
        QString endReason;
        BroadcastService svc([&](const QJsonObject &value) {
            if (value.value("cmd").toString()=="broadcast_media") gate=value.value("enabled").toBool();
            const auto payload=value.value("payload").toObject();
            const auto state=payload.value("data").toObject();
            if (payload.value("method").toString()=="broadcastLiveState" && state.value("state").toString()=="ENDED")
                endReason=state.value("reason").toString();
            return true;
        },nullptr);
        svc.configure(device,{});svc.setOnline(true);
        svc.handle(request,cmd("broadcastLiveStart","focus-start",{{"liveId","focus-live"},{"codec","PCM16"},
            {"frameMs",20},{"sampleRate",16000},{"channels",1},{"maxDurationSec",5}}));
        QTRY_VERIFY(gate);
        svc.handle(request,cmd("broadcastLiveCtrl","focus-pause",{{"liveId","focus-live"},{"action","PAUSE"}}));
        QVERIFY(!gate);
        QTest::qWait(3200);
        auto &focus=AudioPromptFocusManager::instance();
        focus.readyDelayMs=450;
        svc.handle(request,cmd("broadcastLiveCtrl","focus-resume",{{"liveId","focus-live"},{"action","RESUME"}}));
        focus.readyDelayMs=0;
        QTRY_VERIFY_WITH_TIMEOUT(gate,1000);
        QVERIFY(endReason.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(endReason,QStringLiteral("TIMEOUT"),2200);
        QVERIFY(!gate);
    }
    /** Verify pause/stop retain focus until the worker returns the closed PCM output. */
    void liveFocusWaitsForAudioReturn() {
        QTemporaryDir temp;configPath=temp.path();
        int callPauses=0;
        BroadcastService svc([&](const QJsonObject &command){
            const auto event=command.value("payload").toObject();
            if(event.value("method")=="broadcastLiveState" &&
               event.value("data").toObject().value("reason")=="CALL") ++callPauses;
            return true;
        },nullptr);
        svc.configure(device,{});svc.setOnline(true);
        auto &focus=AudioPromptFocusManager::instance();
        using Player=GstPlayerWidget;
        using Priority=AudioPromptFocusManager::Priority;
        const QJsonObject start{{"liveId","handoff"},{"codec","PCM16"},{"frameMs",20},
                                {"sampleRate",16000},{"channels",1}};
        const auto control=[&](const QString &action) {
            svc.handle(request,cmd("broadcastLiveCtrl",action,{{"liveId","handoff"},{"action",action}}));
        };
        svc.handle(request,cmd("broadcastLiveStart","handoff-start",start));
        QTRY_VERIFY(Player::outputOwned.load());
        Player::holdReturn=true;
        control("PAUSE");
        QTRY_VERIFY(Player::returning.load());
        QVERIFY(focus.blocks(Priority::Schedule));
        QObject call;
        focus.acquire(&call,nullptr,Priority::Call);
        QVERIFY(focus.callOwnsFocus());
        QCOMPARE(callPauses,0);
        focus.release(&call);
        control("RESUME");
        QVERIFY(!svc.livePlaying_);
        Player::holdReturn=false;
        QTRY_VERIFY(svc.livePlaying_ && Player::outputOwned.load());
        Player::holdReturn=true;
        control("STOP");
        QTRY_VERIFY(Player::returning.load());
        QVERIFY(focus.blocks(Priority::Schedule));
        Player::holdReturn=false;
        QTRY_VERIFY(!focus.blocks(Priority::Schedule));
        QVERIFY(!Player::outputOwned.load());
    }

    void liveCallAndManualPause() {
        QTemporaryDir temp;configPath=temp.path();
        QList<QJsonObject> tx;
        BroadcastService svc([&](const QJsonObject &value){tx.append(value);return true;},nullptr);
        svc.configure(device,{});svc.setOnline(true);
        svc.setScheduledEnabled(false);
        const auto mediaGate=[&](bool enabled) {
            for(auto it=tx.crbegin();it!=tx.crend();++it)
                if(it->value("cmd").toString()=="broadcast_media") return it->value("enabled").toBool()==enabled;
            return false;
        };
        QJsonObject data{{"liveId","live-1"},{"codec","PCM16"},{"frameMs",40},{"sampleRate",16000},{"channels",1}};
        svc.handle(request,cmd("broadcastLiveStart","start",data));
        QTRY_VERIFY(mediaGate(true));
        QObject call;
        auto &focus=AudioPromptFocusManager::instance();
        focus.acquire(&call,nullptr,AudioPromptFocusManager::Priority::Call);
        QTRY_VERIFY(mediaGate(false));
        focus.release(&call);QTRY_VERIFY(mediaGate(true));
        svc.handle(request,cmd("broadcastLiveCtrl","pause",{{"liveId","live-1"},{"action","PAUSE"}}));
        QTRY_VERIFY(mediaGate(false));
        focus.acquire(&call,nullptr,AudioPromptFocusManager::Priority::Call);
        focus.release(&call);QTest::qWait(50);QVERIFY(mediaGate(false));
        svc.handle(request,cmd("broadcastLiveCtrl","stop",{{"liveId","live-1"},{"action","STOP"}}));
        QVERIFY(mediaGate(false));
    }
};
QTEST_MAIN(BroadcastTests)
#include "test.moc"
''', encoding='utf-8')
    paths = [root / 'components/settings/mqtt' / filename for filename in ('broadcast_service.cpp', 'broadcast_live.cpp')]
    quoted = lambda p: '"' + p.as_posix() + '"'
    pro = chr(10).join([
        'QT += core gui widgets testlib',
        'CONFIG += console testcase c++17',
        'TARGET = broadcast_tests',
        'INCLUDEPATH += ' + quoted(out / 'stubs') + ' ' + quoted(root),
        'HEADERS += ' + quoted(player) + ' ' + quoted(focus),
        'SOURCES += test.cpp ' + ' '.join(quoted(p) for p in paths),
        '',
    ])
    (out / 'test.pro').write_text(pro, encoding='utf-8')
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QPA_FONTDIR='C:/Windows/Fonts')
    if os.name == 'nt':
        qt = root / '.web-build-tools/sdk/5.15.2/mingw81_64/bin'
        gcc = root / '.web-build-tools/sdk/Tools/mingw810_64/bin'
        env['PATH'] = os.pathsep.join((str(qt), str(gcc), env.get('PATH','')))
        qmake, make, binary = str(qt/'qmake.exe'), str(gcc/'mingw32-make.exe'), out/'release/broadcast_tests.exe'
    else:
        qmake, make, binary = 'qmake', 'make', out/'broadcast_tests'
    (out / 'qt-results.txt').unlink(missing_ok=True)
    for args, name in (([qmake,'test.pro'],'qmake'),([make,'-j4'],'build')):
        result = subprocess.run(args, cwd=out, env=env, capture_output=True, timeout=120)
        (out/(name+'.log')).write_bytes(result.stdout+result.stderr)
        if result.returncode:
            print((result.stdout+result.stderr).decode('utf-8',errors='replace')[-7000:])
            result.check_returncode()
    if not shutil.which('curl', path=env.get('PATH')):
        raise RuntimeError('curl is required by the FTP regression')
    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), FixtureFtp) as ftp:
        ftp.daemon_threads = True
        env['BROADCAST_TEST_FTP_PORT'] = str(ftp.server_address[1])
        env['NO_PROXY'] = '127.0.0.1'
        env['no_proxy'] = '127.0.0.1'
        worker = Thread(target=ftp.serve_forever)
        worker.start()
        try:
            result = subprocess.run([str(binary),'-o','qt-results.txt,txt'], cwd=out,
                                    env=env, capture_output=True, timeout=120)
        finally:
            ftp.shutdown()
            worker.join()
    (out / 'results.log').write_bytes(result.stdout+result.stderr)
    report = out / 'qt-results.txt'
    summary = report.read_text(encoding='utf-8', errors='replace') if report.exists() else ''
    print(summary[-7000:])
    result.check_returncode()
    if 'Totals:' not in summary:
        raise RuntimeError('Qt Test did not produce a result summary')


if __name__ == '__main__':
    main()
