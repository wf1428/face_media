"""Validate actual configuration preparation and playback persistence with isolated files.

Author: Du lin
Date: 2026-11-04
Compiles the complete MqttService and actual playback persistence method. Platform
paths and clock changes are replaced. No production settings or hardware are used.
"""
import os
from pathlib import Path
import subprocess
from media_async_check import function


def main():
    """Run background preparation, failure ordering, state coalescing and merge checks."""
    root=Path.cwd()
    output=root/'.web-build/voice-media-async-20261104/configuration'
    output.mkdir(parents=True,exist_ok=True)
    source=(root/'components/multimedia/multimediademo.cpp').read_text(encoding='utf-8')
    route=source[source.index('struct ActiveMqttRouteInfo'):source.index('/**\n * @brief 从 RTSP 状态行')]
    fixture=r'''
#include <QtTest>
#include <memory>
#include "common/configuration_work.h"
#include "components/settings/mqtt/mqttservice.h"
#include "common/media_content_policy.h"
static QString testRoot;static std::atomic_int timeDelay{0},videoDirectoryCalls{0};
namespace Rk3566Platform {
QString netConfigDir(){return testRoot;}
QString netConfigPath(){return testRoot+"/net_cfg.ini";}
QString videoDir(){++videoDirectoryCalls;return testRoot+"/video";}
QString staticRoot(){return testRoot+"/static";}
bool setSystemDateTime(const QDateTime &,QString*) { QThread::msleep(timeDelay.load());return true; }
bool syncRtcFromSystem(QString*) { return true; }
}
#define LOG_CONFIG(text) do { qInfo()<<text; } while(false)
struct FakePanel { void setInfo(const QString&,const QString&,const QString&,const QString&,const QString&,const QString&){} };
class MultimediaDemo:public QObject {
public:
    quint64 m_playbackPersistRevision=0;bool m_liveSourceValidated=false;
    QString m_desiredLiveUrl,m_streamUrl,m_desiredLiveKind,m_netIp,m_mqttClientId,m_mqttRouteName,m_mqttHost,m_mqttSubTopics;
    FakePanel *m_netInfoPanel=nullptr;
    QString netCfgPath()const{return Rk3566Platform::netConfigPath();}
    void persistPlaybackStateToNetCfg(const QString&,const QString&,const QString&);
};
'''
    fixture+=route
    fixture+=function(source,'static QString normalizePlaybackModeForCfg(')
    fixture+=function(source,'static QString detectLiveKindFromUrlForCfg(')
    fixture+=function(source,'void MultimediaDemo::persistPlaybackStateToNetCfg(')
    fixture+=r'''
class ConfigurationAsyncTests:public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    static QJsonObject request(const QString &type,QJsonObject extra={}) {
        extra.insert("videoType",type);return {{"method","videoControl"},{"data",extra}};
    }
    static void barrier(bool &done) {
        ConfigurationWork::queue().enqueue([&done]{return SerialWorkQueue::Task{[]{return QString();},[&done](const QString &){done=true;}};});
    }
private slots:
    void init() {
        QVERIFY(temporary.isValid());testRoot=temporary.path();timeDelay=0;
        QSettings ini(Rk3566Platform::netConfigPath(),QSettings::IniFormat);ini.clear();
        ini.setValue("feature/online_v1",true);ini.setValue("VoiceCallDsp/echoSuppressDb",-40);
        ini.setValue("mqtt/host_online_v1","broker");ini.setValue("mqtt/sub_topics_online_v1","request");ini.sync();
    }
    void preparationDoesNotBlockUi() {
        int ticks=0;QTimer heartbeat;heartbeat.setInterval(5);connect(&heartbeat,&QTimer::timeout,[&]{++ticks;});heartbeat.start();
        auto payload=request("LIVE",{{"videoUrl","rtsp://fixture/live"}});
        payload.insert("time",QDateTime::currentDateTime().addSecs(20).toString("yyyy-MM-dd HH:mm:ss"));timeDelay=100;
        auto result=std::make_shared<VideoControlPreparation>();bool done=false;
        ConfigurationWork::queue().enqueue([result,payload,&done]{return SerialWorkQueue::Task{
            [result,payload]{MqttService local;*result=local.prepareVideoControlConfiguration(payload);return QString();},
            [&done](const QString &error){QVERIFY(error.isEmpty());done=true;}};});
        QTRY_VERIFY(done);QVERIFY(ticks>=5);QCOMPARE(int(result->failed),int(VideoControlPreparation::Ready));
        MqttService service;QSignalSpy live(&service,&MqttService::liveStreamReady);bool status=false;
        QVERIFY(service.handleVideoControlMessage("request",payload,*result,&status));QCOMPARE(live.size(),1);QVERIFY(status);
        QSettings ini(Rk3566Platform::netConfigPath(),QSettings::IniFormat);QCOMPARE(ini.value("stream/url").toString(),QString("rtsp://fixture/live"));
        QCOMPARE(ini.value("VoiceCallDsp/echoSuppressDb").toInt(),-40);
    }
    void snapshotPreparationAvoidsMediaReseeding() {
        MqttService service;const auto before=videoDirectoryCalls.load();
        const auto prepare=service.configurationPreparation(request("LIVE",{{"videoUrl","rtsp://snapshot"}}));
        QCOMPARE(videoDirectoryCalls.load(),before);bool done=false;
        ConfigurationWork::queue().enqueue([prepare,&done]{return SerialWorkQueue::Task{
            [prepare]{const auto result=prepare();return result.failed==VideoControlPreparation::Ready ? QString() : result.error;},
            [&done](const QString &error){QVERIFY(error.isEmpty());done=true;}};});
        QTRY_VERIFY(done);QCOMPARE(videoDirectoryCalls.load(),before);
    }
    void recordedPreparationPreservesProtocolAndVolume() {
        const auto payload=request("RECORDED",{{"videoPath","folder/photo.png"},{"contentType","IMAGE"},{"smallImageMode",1},{"ftpPath","fixture-host"},{"ftpPort","21"},{"ftpUser","fixture"},{"ftpPwd","fixture-only"},{"videoVolume",44}});
        auto result=std::make_shared<VideoControlPreparation>();bool done=false;
        ConfigurationWork::queue().enqueue([result,payload,&done]{return SerialWorkQueue::Task{
            [result,payload]{MqttService local;*result=local.prepareVideoControlConfiguration(payload);return QString();},[&done](const QString &){done=true;}};});
        QTRY_VERIFY(done);QCOMPARE(int(result->failed),int(VideoControlPreparation::Ready));
        MqttService service;QSignalSpy ftp(&service,&MqttService::ftpDownloadTaskReady),volume(&service,&MqttService::mqttVolumeReceived);
        bool ok=false;QVERIFY(service.handleVideoControlMessage("request",payload,*result,&ok));QVERIFY(ok);QCOMPARE(ftp.size(),1);QCOMPARE(volume.size(),1);
        QCOMPARE(service.currentPlayInfo().status,QString("downloading"));QVERIFY(!service.hasPendingVolumeAck());
        QSettings ini(Rk3566Platform::netConfigPath(),QSettings::IniFormat);
        QCOMPARE(ini.value("ftp/remote_path").toString(),QString("/photo.png"));
        QCOMPARE(ini.value("ftp/local_path").toString(),MediaContentPolicy::pictureDir(Rk3566Platform::staticRoot()));
        QCOMPARE(ini.value("playback/contentType").toString(),QString("IMAGE"));
    }
    void failureStageKeepsExistingVolumeOrder() {
        MqttService service;QSignalSpy volume(&service,&MqttService::mqttVolumeReceived),live(&service,&MqttService::liveStreamReady);
        auto payload=request("LIVE",{{"videoUrl","rtsp://fixture"},{"videoVolume",35}});VideoControlPreparation result;
        result.failed=VideoControlPreparation::Stream;result.error="injected write failure";bool ok=true;
        service.handleVideoControlMessage("request",payload,result,&ok);QCOMPARE(volume.size(),1);QCOMPARE(live.size(),0);QVERIFY(!ok);
        QCOMPARE(service.currentPlayInfo().status,QString("error"));
    }
    void obsoleteSourceKeepsVolumeAndCurrentState() {
        MqttService service;QSignalSpy live(&service,&MqttService::liveStreamReady),volume(&service,&MqttService::mqttVolumeReceived);
        VideoControlPreparation result;result.playbackObsolete=true;bool ok=true;
        service.handleVideoControlMessage("request",request("LIVE",{{"videoUrl","rtsp://old"},{"videoVolume",37}}),result,&ok);
        QCOMPARE(live.size(),0);QCOMPARE(volume.size(),1);QCOMPARE(service.currentPlayInfo().videoType,QString("RECORDED"));
        QCOMPARE(service.currentPlayInfo().videoVolume,37);QVERIFY(service.hasPendingVolumeAck());QVERIFY(!ok);
    }
    void shutdownDrainsQueuedValueWrites() {
        std::atomic_int writes{0};SerialWorkQueue queue("config.exit",nullptr,true);
        queue.enqueue([&]{return SerialWorkQueue::Task{[&]{QThread::msleep(20);++writes;return QString();},{}};});
        queue.enqueue([&]{return SerialWorkQueue::Task{[&]{++writes;return QString();},{}};});
        queue.shutdown();QCOMPARE(writes.load(),2);
    }
    void coalescedStateAndUnrelatedSettings() {
        ConfigurationWork::queue().enqueue([]{return SerialWorkQueue::Task{[]{QThread::msleep(80);return QString();},{}};});
        MultimediaDemo demo;demo.m_desiredLiveUrl="rtsp://fixture/latest";
        demo.persistPlaybackStateToNetCfg("LIVE","LIVE","old");
        demo.persistPlaybackStateToNetCfg("RECORDED","RECORDED","middle");
        demo.persistPlaybackStateToNetCfg("NONE","NONE","new");
        bool done=false;barrier(done);QTRY_VERIFY(done);
        QSettings ini(Rk3566Platform::netConfigPath(),QSettings::IniFormat);
        QCOMPARE(ini.value("playback/target_mode").toString(),QString("NONE"));QCOMPARE(ini.value("playback/last_reason").toString(),QString("new"));
        QCOMPARE(ini.value("VoiceCallDsp/echoSuppressDb").toInt(),-40);QCOMPARE(demo.m_mqttHost,QString("broker"));
    }
    void offlineDoesNotPersist() {
        {QSettings ini(Rk3566Platform::netConfigPath(),QSettings::IniFormat);ini.setValue("feature/offline_v1",true);ini.setValue("feature/online_v1",false);ini.sync();}
        MultimediaDemo demo;demo.persistPlaybackStateToNetCfg("NONE","NONE","skip");bool done=false;barrier(done);QTRY_VERIFY(done);
        QSettings ini(Rk3566Platform::netConfigPath(),QSettings::IniFormat);QVERIFY(!ini.contains("playback/command_received"));
    }
};
QTEST_GUILESS_MAIN(ConfigurationAsyncTests)
#include "fixture.moc"
'''
    (output/'fixture.cpp').write_text(fixture,encoding='utf-8')
    pro='QT += core testlib\nQT -= gui\nCONFIG += console testcase c++17\nTARGET = configuration_async_tests\n'
    pro+='INCLUDEPATH += "'+root.as_posix()+'"\nSOURCES += fixture.cpp "'+(root/'components/settings/mqtt/mqttservice.cpp').as_posix()+'"\n'
    pro+='HEADERS += "'+(root/'components/settings/mqtt/mqttservice.h').as_posix()+'"\n'
    (output/'tests.pro').write_text(pro,encoding='utf-8')
    qt=root/'.web-build-tools/sdk/5.15.2/mingw81_64/bin';gcc=root/'.web-build-tools/sdk/Tools/mingw810_64/bin'
    env=dict(os.environ);env['PATH']=str(qt)+os.pathsep+str(gcc)+os.pathsep+env.get('PATH','')
    for command,name in [([str(qt/'qmake.exe'),'tests.pro'],'qmake.log'),([str(gcc/'mingw32-make.exe'),'-j4'],'build.log'),([str(output/'release/configuration_async_tests.exe'),'-o',str(output/'results.txt')+',txt'],'run.log')]:
        result=subprocess.run(command,cwd=output,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
        (output/name).write_bytes(result.stdout)
        if result.returncode:
            print(result.stdout.decode('utf-8',errors='replace')[-5000:]);result.check_returncode()
    text=(output/'results.txt').read_text(encoding='utf-8',errors='replace')
    print('\n'.join(line for line in text.splitlines() if line.startswith(('PASS','FAIL','Totals'))))


if __name__=='__main__':
    main()
