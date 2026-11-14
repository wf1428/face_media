"""Exercise the real volume controllers and focus manager with a fake hardware process.

Author: Du lin
Date: 2026-10-30
Note: 最近修改：2026-11-11；修改人：Du lin；模拟旧 PCM 已清除但共享输出恢复失败，验证普通播报仍写入播报增益并在结束时恢复用户音量；保留未清除时的原策略。
The entire production vol_ctrl.cpp is compiled; only its fixed data path and GStreamer
widget include are redirected. Qt QProcess, timers, persistence, and arbitration stay real.
--baseline uses the saved pre-fix source and runs the reported media-volume regression.
No real amplifier, mixer, network, or production configuration is accessed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    """Build the hardware substitute and run production-volume regressions.

    Raises:
        OSError: Required source or local Qt tools are unavailable.
        ValueError: Production hardware-boundary anchors no longer match.
        subprocess.CalledProcessError: A build or a regression fails.
        subprocess.TimeoutExpired: An external tool exceeds its bounded run time.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', action='store_true', help='Run mediaCommands against saved pre-fix implementation')
    args = parser.parse_args()
    root = Path.cwd()
    task = root / '.web-build/voice-call-videoctrl-20261030'
    output = task / ('baseline' if args.baseline else 'volume')
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt':
        qt = root / '.web-build-tools/sdk/5.15.2/mingw81_64/bin'
        gcc = root / '.web-build-tools/sdk/Tools/mingw810_64/bin'
        env['PATH'] = str(qt) + os.pathsep + str(gcc) + os.pathsep + env.get('PATH', '')
        qmake, make, suffix = str(qt / 'qmake.exe'), str(gcc / 'mingw32-make.exe'), '.exe'
        binary_dir = output / 'release'
    else:
        qmake, make, suffix = 'qmake', 'make', ''
        binary_dir = output
    env['VOICE_VOLUME_TEST_ROOT'] = output.as_posix()
    env['VOICE_TEST_AMIXER'] = (binary_dir / ('fake_amixer' + suffix)).as_posix()
    helper = r"""
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QThread>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();
    if(!args.contains("numid=3")) return 0;
    const int raw=args.last().split(',').first().toInt();
    const auto path=QDir(qEnvironmentVariable("VOICE_VOLUME_TEST_ROOT"));
    const auto log=[&](const QByteArray &entry) { QFile f(path.filePath("writes.log"));if(f.open(QIODevice::WriteOnly|QIODevice::Append)) f.write(entry); };
    log("begin "+QByteArray::number(raw)+"\n");
    if(qEnvironmentVariableIntValue("VOICE_DELAY_RAW")==raw && qEnvironmentVariableIntValue("VOICE_DELAY_MS")>0)
        QThread::msleep(unsigned(qEnvironmentVariableIntValue("VOICE_DELAY_MS")));
    QFile file(path.filePath("hardware.dat"));
    if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)) return 2;
    file.write(QByteArray::number(raw));file.close();log("done "+QByteArray::number(raw)+"\n");return 0;
}
"""
    (output / 'fake_amixer.cpp').write_text(helper, encoding='utf-8')
    (output / 'helper.pro').write_text('QT += core\nQT -= gui\nCONFIG += console c++17\nTARGET = fake_amixer\nSOURCES += fake_amixer.cpp\n', encoding='utf-8')
    for command in [[qmake, 'helper.pro', '-o', 'Makefile.helper'], [make, '-f', 'Makefile.helper', '-j4']]:
        subprocess.run(command, cwd=output, env=env, check=True, timeout=60, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    source_path = task / 'before/components/multimedia/vol_ctrl.cpp' if args.baseline else root / 'components/multimedia/vol_ctrl.cpp'
    source = source_path.read_text(encoding='utf-8')
    anchor = 'const char kVolumeStatePath[] = "/home/cat/face_media/volume.dat";'
    if source.count(anchor) != 1 or source.count('#include "gst_player_widget.h"') != 1:
        raise ValueError('Production volume hardware boundary changed')
    source = source.replace(anchor, 'const char kVolumeStatePath[] = ' + json.dumps((output / 'volume.dat').as_posix()) + ';')
    source = source.replace('#include "gst_player_widget.h"', '#include "fake_player.h"')
    (output / 'production_volume.cpp').write_text(source, encoding='utf-8')
    (output / 'fake_player.h').write_text(r"""
#pragma once
#include <QWidget>
#include <QThread>
#include <atomic>
class GstPlayerWidget:public QWidget {
    Q_OBJECT
public:
    static void setPriorityAudioOwner(GstPlayerWidget *,bool) {}
    inline static bool videoHeld=false;
    inline static std::atomic_int clearCalls{0},clearDelayMs{0};
    inline static std::atomic_bool clearOk{true},clearConfirmed{true};
    static void holdVideoAudio(bool held) { videoHeld=held; }
    static bool discardQueuedAudioForPrompt(bool *nullConfirmed=nullptr) {
        ++clearCalls;
        QThread::msleep(unsigned(clearDelayMs.load()));
        if (nullConfirmed) *nullConfirmed=clearConfirmed.load();
        return clearOk.load();
    }
    bool active=true;float duck=1;
    bool hasVideoOutput() const { return true; }
    bool hasActiveMedia() const { return active; }
    void setAudioDucking(const QObject *,float value) { duck=value; }
    void clearAudioDucking(const QObject *) { duck=1; }
};
""", encoding='utf-8')
    tests = r"""
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include "components/multimedia/vol_ctrl.h"
#include "src/call/AudioOutputFocus.h"
#include "fake_player.h"
namespace Rk3566Platform {
QString configDir() { return qEnvironmentVariable("VOICE_VOLUME_TEST_ROOT"); }
QString amplifierDevice() { return QDir(configDir()).filePath("amp.dat"); }
QString alsaCard() { return "0"; }
QString uiConfigPath() { return QDir(configDir()).filePath("ui.ini"); }
QString findExecutable(const QStringList &) { return qEnvironmentVariable("VOICE_TEST_AMIXER"); }
}
static QByteArray read(const char *name) { QFile f(QDir(Rk3566Platform::configDir()).filePath(name));return f.open(QIODevice::ReadOnly) ? f.readAll().replace("\r\n","\n") : QByteArray(); }
static int gain() { bool ok=false;const int n=read("hardware.dat").toInt(&ok);return ok ? n : -1; }
static void pump(int ms) { QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec(); }
class CallMediaVolumeTests:public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qputenv("VOICE_DELAY_RAW","-1");qputenv("VOICE_DELAY_MS","0");
        QFile file(QDir(Rk3566Platform::configDir()).filePath("volume.dat"));
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        file.write("volume:50\nprompt_volume:60\namp_flag:1\n");file.close();
        VoiceCall::initializeOutputVolume();QCOMPARE(gain(),164);
    }
    void mediaCommands() {
        AmpVolumeController media;QObject call;GstPlayerWidget video;int ready=0;
        QSignalSpy applied(&media,&AmpVolumeController::volumeApplied);
        VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });
        QTRY_COMPARE(ready,1);QCOMPARE(gain(),230);QCOMPARE(video.duck,0.0f);
        // LIVE / NONE / RECORDED commonly carry videoVolume; execute the real shared controller path.
        for(int volume:{0,100,37}) {
            media.setVolume(volume);QTRY_VERIFY(!applied.isEmpty());applied.clear();pump(140);
            QCOMPARE(media.currentVolume(),volume);
            QVERIFY(read("volume.dat").contains("volume:"+QByteArray::number(volume)+"\n"));
            QCOMPARE(gain(),230);QCOMPARE(read("amp.dat"),QByteArray(1,char(1)));
            video.active=!video.active; // Stopping/restarting media does not release the call owner.
        }
        media.setRawVolumeTemporaryAsync(0);media.setRawVolumeTemporary(0);pump(100);
        QCOMPARE(gain(),230);QCOMPARE(read("amp.dat"),QByteArray(1,char(1)));
        { AmpVolumeController late;QCOMPARE(gain(),230); }
        VoiceCall::releaseOutputVolume(&call);media.setVolume(73); // Last value still inside original 80ms debounce.
        QTRY_COMPARE(gain(),195);pump(140);QCOMPARE(video.duck,1.0f);
        media.setVolume(37);QTRY_COMPARE(gain(),147); // Normal media control works again.
        media.setRawVolumeTemporaryAsync(0);QTRY_COMPARE(gain(),0);
        QTRY_COMPARE(read("amp.dat"),QByteArray(1,char(0)));
        // Explicit nonzero endpoints, true zero mute, rounding, and input clamping.
        const QList<QPair<int,int>> mapping={{0,0},{1,100},{2,101},{50,164},{100,230},{-10,0},{110,230}};
        for (const auto &point:mapping) {
            applied.clear();media.setVolume(point.first);QTRY_VERIFY(!applied.isEmpty());
            QTRY_COMPARE(gain(),point.second);
            QTRY_COMPARE(read("amp.dat"),QByteArray(1,point.second ? char(1) : char(0)));
            QCOMPARE(media.currentRawVolume(),point.second);
            QCOMPARE(media.loadAmpFlagFromFile(),point.second!=0);
            if (point.first==0) {
                AmpVolumeController reloaded;
                QCOMPARE(reloaded.currentRawVolume(),0);QVERIFY(!reloaded.loadAmpFlagFromFile());
            }
        }
        media.setVolume(50);QTRY_COMPARE(gain(),164);
        QTRY_VERIFY(media.findChild<QProcess *>()->state()==QProcess::NotRunning);
    }
    void previousWriterBarrier() {
        AmpVolumeController media;QObject call;int ready=0;
        qputenv("VOICE_DELAY_RAW","0");qputenv("VOICE_DELAY_MS","500");
        media.setRawVolumeTemporaryAsync(0);QTRY_VERIFY(read("writes.log").endsWith("begin 0\n"));
        int beats=0;QTimer heartbeat;heartbeat.setInterval(10);connect(&heartbeat,&QTimer::timeout,this,[&]() { ++beats; });heartbeat.start();
        QElapsedTimer elapsed;elapsed.start();VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });
        QVERIFY(elapsed.elapsed()<50);pump(150);QCOMPARE(ready,0);
        QTRY_COMPARE(ready,1);QVERIFY(beats>=10);QCOMPARE(gain(),230);
        media.setVolume(20);pump(150);QCOMPARE(gain(),230);
        qputenv("VOICE_DELAY_MS","0");VoiceCall::releaseOutputVolume(&call);QTRY_COMPARE(gain(),125);pump(50);
    }
    /** @brief 验证直播和计划依次挂起、释放后按优先级恢复。 */
    void broadcastPreemptionRestoresPreviousOwners() {
        QObject ordinary, tagged, scheduled, live, call;
        GstPlayerWidget player;
        auto &focus=AudioPromptFocusManager::instance();
        using Priority=AudioPromptFocusManager::Priority;
        QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        QSignalSpy paused(&focus,&AudioPromptFocusManager::focusSuspended);
        focus.acquire(&ordinary,&player);
        QTRY_COMPARE(ready.size(),1);
        focus.acquire(&tagged,&player,Priority::Tagged);
        QTRY_COMPARE(ready.size(),2);
        focus.acquire(&scheduled,&player,Priority::Schedule);
        QTRY_COMPARE(ready.size(),3);
        focus.acquire(&live,&player,Priority::Live);
        QTRY_COMPARE(ready.size(),4);
        focus.acquireCall(&call);
        QTRY_COMPARE(ready.size(),5);
        QCOMPARE(paused.size(),4);
        QVERIFY(focus.callOwnsFocus());
        focus.release(&call);
        QTRY_COMPARE(ready.size(),6);
        QCOMPARE(qvariant_cast<QObject *>(ready.last()[0]),&live);
        focus.release(&live);
        QTRY_COMPARE(ready.size(),7);
        QCOMPARE(qvariant_cast<QObject *>(ready.last()[0]),&scheduled);
        focus.release(&tagged);
        focus.release(&scheduled);
        QTRY_COMPARE(ready.size(),8);
        QCOMPARE(qvariant_cast<QObject *>(ready.last()[0]),&ordinary);
        focus.release(&ordinary);
    }
    /** @brief 直播结束后，硬件仍在写入原增益期间视频继续静音。 */
    void liveRestoreWaitsForAmplifier() {
        AmpVolumeController media;
        media.setVolume(50);QTRY_COMPARE(gain(),164);
        QObject live;GstPlayerWidget video;
        auto &focus=AudioPromptFocusManager::instance();
        QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        focus.acquire(&live,&video,AudioPromptFocusManager::Priority::Live);
        QTRY_VERIFY(!ready.isEmpty());
        QCOMPARE(gain(),230);
        qputenv("VOICE_DELAY_RAW","164");qputenv("VOICE_DELAY_MS","350");
        focus.release(&live);
        QTRY_VERIFY(read("writes.log").endsWith("begin 164\n"));
        const bool held=GstPlayerWidget::videoHeld && video.duck==0.0f;
        qputenv("VOICE_DELAY_MS","0");
        QVERIFY(held);
        QTRY_VERIFY(!GstPlayerWidget::videoHeld);
        QCOMPARE(gain(),164);
        QCOMPARE(video.duck,1.0f);
    }
    void overlapAndCancel() {
        AmpVolumeController media;QObject prompt,call;GstPlayerWidget video;
        auto &focus=AudioPromptFocusManager::instance();QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        focus.acquire(&prompt, &video);QTRY_VERIFY(!ready.isEmpty());
        int callReady=0;VoiceCall::acquireOutputVolume(&call,[&]() { ++callReady; });QTRY_COMPARE(callReady,1);
        VoiceCall::releaseOutputVolume(&call);media.setVolume(65);pump(150);
        QCOMPARE(gain(),230); // Another prompt still owns the shared focus.
        focus.release(&prompt);QTRY_COMPARE(gain(),184);pump(50);
        int cancelled=0;VoiceCall::acquireOutputVolume(&call,[&]() { ++cancelled; });
        media.setVolume(25);VoiceCall::releaseOutputVolume(&call);QTRY_COMPARE(gain(),132);pump(200);QCOMPARE(cancelled,0);
        int muteReady=0;VoiceCall::acquireOutputVolume(&call,[&]() { ++muteReady; });QTRY_COMPARE(muteReady,1);
        media.setVolume(0);pump(140);QCOMPARE(gain(),230);
        QCOMPARE(read("amp.dat"),QByteArray(1,char(1)));QVERIFY(!media.loadAmpFlagFromFile());
        VoiceCall::releaseOutputVolume(&call);QTRY_COMPARE(gain(),0);
        QTRY_COMPARE(read("amp.dat"),QByteArray(1,char(0))); // Saved video mute applies only after call release.
        media.setVolume(50);QTRY_COMPARE(gain(),164);
    }
    void cancelWhileOlderWriterRuns() {
        AmpVolumeController media;QObject call;int ready=0;
        media.setVolume(50);pump(130);
        qputenv("VOICE_DELAY_RAW","0");qputenv("VOICE_DELAY_MS","400");
        media.setRawVolumeTemporaryAsync(0);QTRY_VERIFY(read("writes.log").endsWith("begin 0\n"));
        VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });
        VoiceCall::releaseOutputVolume(&call);media.setVolume(64);
        QTRY_COMPARE(gain(),183);pump(120);QCOMPARE(ready,0);
        qputenv("VOICE_DELAY_MS","0");media.setVolume(50);QTRY_COMPARE(gain(),164);pump(50);
    }
    void promptClearAndRestoreOrdering() {
        QObject first,second;
        GstPlayerWidget video;
        auto &focus=AudioPromptFocusManager::instance();
        QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        GstPlayerWidget::clearCalls=0;
        GstPlayerWidget::clearDelayMs=180;
        focus.acquire(&first,&video);
        focus.acquire(&second,&video);
        QVERIFY(GstPlayerWidget::videoHeld);
        pump(80);
        QCOMPARE(ready.size(),0);
        QCOMPARE(gain(),164);
        QTRY_COMPARE(ready.size(),1);
        QCOMPARE(gain(),230);
        focus.release(&first);
        QTRY_COMPARE(ready.size(),2);
        QCOMPARE(GstPlayerWidget::clearCalls.load(),1);

        qputenv("VOICE_DELAY_RAW","164");qputenv("VOICE_DELAY_MS","180");
        focus.release(&second);
        pump(60);
        QVERIFY(GstPlayerWidget::videoHeld);
        QCOMPARE(gain(),230);
        QTRY_VERIFY(!GstPlayerWidget::videoHeld);
        QCOMPARE(gain(),164);
        QCOMPARE(video.duck,1.0f);
        qputenv("VOICE_DELAY_MS","0");
        GstPlayerWidget::clearDelayMs=0;
    }
    void promptTimeoutUsesOriginalGain() {
        QObject prompt;
        GstPlayerWidget video;
        auto &focus=AudioPromptFocusManager::instance();
        QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        GstPlayerWidget::clearOk=false;
        GstPlayerWidget::clearConfirmed=false;
        const auto before=read("writes.log");
        focus.acquire(&prompt,&video);
        QTRY_COMPARE(ready.size(),1);
        QCOMPARE(gain(),164);
        QCOMPARE(read("writes.log"),before);
        QVERIFY(GstPlayerWidget::videoHeld);
        focus.release(&prompt);
        QVERIFY(!GstPlayerWidget::videoHeld);
        QCOMPARE(gain(),164);
        GstPlayerWidget::clearOk=true;
        GstPlayerWidget::clearConfirmed=true;
    }
    /** @brief 旧 PCM 已确认清除时，声卡重开失败不再使文件播报沿用较小的用户音量。 */
    void promptResumeFailureUsesPromptGain() {
        QObject prompt;
        GstPlayerWidget video;
        auto &focus=AudioPromptFocusManager::instance();
        QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        GstPlayerWidget::clearOk=false;
        GstPlayerWidget::clearConfirmed=true;
        focus.acquire(&prompt,&video);
        QTRY_COMPARE(ready.size(),1);
        QCOMPARE(gain(),230);
        QVERIFY(GstPlayerWidget::videoHeld);
        focus.release(&prompt);
        QTRY_COMPARE(gain(),164);
        QTRY_VERIFY(!GstPlayerWidget::videoHeld);
        GstPlayerWidget::clearOk=true;
    }
    void callPreemptsPendingPromptClear() {
        QObject prompt,call;
        GstPlayerWidget video;
        auto &focus=AudioPromptFocusManager::instance();
        QSignalSpy ready(&focus,&AudioPromptFocusManager::focusReady);
        GstPlayerWidget::clearCalls=0;
        GstPlayerWidget::clearDelayMs=300;
        focus.acquire(&prompt,&video);
        QTRY_COMPARE(GstPlayerWidget::clearCalls.load(),1);
        focus.acquireCall(&call);
        QTRY_COMPARE(ready.size(),1);
        QCOMPARE(qvariant_cast<QObject *>(ready.last()[0]),&call);
        pump(350);
        QCOMPARE(ready.size(),1);
        focus.release(&prompt);
        focus.release(&call);
        QTRY_VERIFY(!GstPlayerWidget::videoHeld);
        QCOMPARE(gain(),164);
        GstPlayerWidget::clearDelayMs=0;
    }
    void nextCallDuringRestore() {
        AmpVolumeController media;QObject call;int ready=0;
        VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });QTRY_COMPARE(ready,1);QCOMPARE(gain(),230);
        qputenv("VOICE_DELAY_RAW","164");qputenv("VOICE_DELAY_MS","400");
        VoiceCall::releaseOutputVolume(&call);QTRY_VERIFY(read("writes.log").endsWith("begin 164\n"));
        VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });pump(140);QCOMPARE(ready,1);
        QTRY_COMPARE(ready,2);QCOMPARE(gain(),230);
        qputenv("VOICE_DELAY_MS","0");VoiceCall::releaseOutputVolume(&call);QTRY_COMPARE(gain(),164);pump(100);
    }
};
QTEST_MAIN(CallMediaVolumeTests)
#include "tests.moc"
"""
    if args.baseline:
        # Saved isolation baseline predates the new mapping; retain its original initialization.
        tests = tests.replace('VoiceCall::initializeOutputVolume();QCOMPARE(gain(),164);',
                              'VoiceCall::initializeOutputVolume();QCOMPARE(gain(),130);', 1)
    (output / 'tests.cpp').write_text(tests, encoding='utf-8')
    project = 'QT += core gui widgets testlib\nCONFIG += console testcase c++17\nTARGET = volume_isolation\n'
    project += 'SOURCES += tests.cpp production_volume.cpp\nHEADERS += fake_player.h "' + (root / 'components/multimedia/vol_ctrl.h').as_posix() + '"\n'
    project += 'INCLUDEPATH += "' + root.as_posix() + '" "' + (root / 'components/multimedia').as_posix() + '"\n'
    (output / 'volume.pro').write_text(project, encoding='utf-8')
    for command in [[qmake, 'volume.pro'], [make, '-j4']]:
        result = subprocess.run(command, cwd=output, env=env, timeout=60, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / ('qmake.log' if command[0] == qmake else 'build.log')).write_bytes(result.stdout)
        if result.returncode:
            print(result.stdout.decode('utf-8', errors='replace')[-6000:])
            result.check_returncode()
    env['QT_QPA_PLATFORM'] = 'offscreen'
    command = [str(binary_dir / ('volume_isolation' + suffix))]
    if args.baseline:
        command += ['mediaCommands']
    command += ['-o', str(output / 'results.txt') + ',txt']
    result = subprocess.run(command, cwd=output, env=env, timeout=25)
    text = (output / 'results.txt').read_text(encoding='utf-8')
    print('\n'.join(line for line in text.splitlines() if line.startswith(('PASS', 'FAIL', 'Totals')) or 'Actual' in line or 'Expected' in line))
    result.check_returncode()


if __name__ == '__main__':
    main()
