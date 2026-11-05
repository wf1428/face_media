/**
 * @file diagnostic_cause_tests.cpp
 * @brief 验证波形分阶段观测、有限录音、调度读取和UI嵌套计时，不访问设备或真实配置。
 * @author Du lin
 * @date 2026-11-04
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <thread>
#include "src/call/AudioProcessing.h"
#include "src/call/PcmDiagnosticRecorder.h"
#include "src/call/SchedulingDiagnostics.h"
#include "common/debug/ui_performance.h"

static QStringList messages; /**< 本测试UI线程的诊断消息，不收集PCM正文。 */
/** @brief 测试专用Qt日志接收器；生产消息处理器不变。 */
static void captureMessage(QtMsgType,const QMessageLogContext &,const QString &message) { messages.append(message); }
/** @brief 在隔离目录写测试伪文件；失败直接终止当前测试用例。 */
static bool writeFixture(const QString &path,const QByteArray &content) { QDir().mkpath(QFileInfo(path).path());QFile file(path);return file.open(QIODevice::WriteOnly) && file.write(content)==content.size(); }
/** @brief 读取测试文件，不触及真实/proc或系统配置。 */
static QByteArray readFixture(const QString &path) { QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); }

/** @brief 无硬件诊断回归；所有声音为合成样本。 */
class DiagnosticCauseTests:public QObject {
    Q_OBJECT
private slots:
    /** @brief 平顶低于数字满幅也保留线索；正常正弦/静音不能被简单当成削顶。 */
    void amplitudeShapeEvidence() {
        VoiceCall::Pcm clean{},analog{},hard{},silence{};
        for(int i=0;i<VoiceCall::Samples;++i) {
            const double wave=std::sin(2*3.141592653589793*700*i/16000.0);
            clean[size_t(i)]=int16_t(20000*wave);
            analog[size_t(i)]=int16_t(std::max(-17000,std::min(17000,int(26000*wave))));
            hard[size_t(i)]=int16_t(std::max(-32768,std::min(32767,int(55000*wave))));
        }
        VoiceCall::PcmShapeTracker tracker;
        const auto good=tracker.observe(clean),flat=tracker.observe(analog),clipped=tracker.observe(hard),quiet=tracker.observe(silence);
        QCOMPARE(good.clipped,0);QCOMPARE(good.flatRun,0);
        QCOMPARE(flat.peak,17000);QCOMPARE(flat.clipped,0);QCOMPARE(flat.rails,0);QVERIFY(flat.flatRun>=4);
        QVERIFY(clipped.clipped>0);QVERIFY(clipped.rails>0);QCOMPARE(quiet.flatRun,0);
        QCOMPARE(quiet.boundaryStep,std::abs(int(hard.back())));QCOMPARE(good.hash,VoiceCall::pcmHash(clean));
    }
    /** @brief 验证本地hash与协议实际小端PCM区一致，协议大小及符号端点不变。 */
    void wirePcmIdentity() {
        VoiceCall::Frame frame;for(int i=0;i<VoiceCall::Samples;++i) frame.pcm[size_t(i)]=int16_t(i*197-32000);
        frame.pcm[0]=-32768;frame.pcm[1]=32767;
        const auto packet=VoiceCall::encode(frame,123);QCOMPARE(packet.size(),size_t(650));
        uint32_t hash=2166136261U;for(size_t i=10;i<packet.size();++i) hash=(hash^packet[i])*16777619U;
        QCOMPARE(hash,VoiceCall::pcmHash(frame.pcm));VoiceCall::Frame decoded;
        QVERIFY(VoiceCall::decode(packet.data(),int(packet.size()),0,decoded));QVERIFY(decoded.pcm==frame.pcm);
    }
    /** @brief 仅启用中间PCM副本时真实Speex输出逐样本相同，覆盖大输入与参考丢失/恢复。 */
    void observationPreservesRealDspOutput() {
        VoiceCall::ProcessingConfig config;config.libraryPath=QString::fromLocal8Bit(qgetenv("VOICE_TEST_SPEEX_LIBRARY"));
        QVERIFY2(!config.libraryPath.isEmpty(),"set actual SpeexDSP test library");QString infoA,infoB;
        VoiceCall::AudioProcessing first(config,infoA),second(config,infoB);QVERIFY(first.active());QVERIFY(second.active());
        first.enablePcmObservation(true);uint32_t seed=73;
        for(int frame=0;frame<350;++frame) {
            VoiceCall::Pcm mic{},ref{};
            for(int i=0;i<VoiceCall::Samples;++i) { seed=seed*1664525U+1013904223U;
                const double t=(frame*VoiceCall::Samples+i)/16000.0;
                ref[size_t(i)]=int16_t(9000*std::sin(2*3.141592653589793*450*t));
                const int value=int((frame>140 ? 27000:5000)*std::sin(2*3.141592653589793*1100*t))+int(ref[size_t(i)])/3+int(seed%501)-250;
                mic[size_t(i)]=int16_t(std::max(-32768,std::min(32767,value)));
            }
            auto copy=mic;const auto *reference=(frame%79<2)?nullptr:&ref;
            first.process(mic,reference,frame<200 ? 1:2);second.process(copy,reference,frame<200 ? 1:2);
            QVERIFY(mic==copy);const auto observation=first.observation();
            QVERIFY(observation.valid&1);QVERIFY(observation.valid&8);QCOMPARE(observation.output.hash,VoiceCall::pcmHash(mic));
            QCOMPARE(second.pcmObservation().valid,0U);
            if(reference) { QVERIFY(observation.valid&2);QVERIFY(first.pcmObservation().valid&2); }
        }
        QCOMPARE(first.stats().failures,uint64_t(0));QVERIFY(first.observation().pre.gainValid);
    }
    /** @brief 全部处理关闭仍逐样本直通，即使开启只读观测也不能改PCM。 */
    void disabledDspRemainsV1() {
        VoiceCall::ProcessingConfig config;config.aecEnabled=config.preprocessEnabled=false;QString info;
        VoiceCall::AudioProcessing processing(config,info);VoiceCall::Pcm pcm{};for(int i=0;i<VoiceCall::Samples;++i) pcm[size_t(i)]=int16_t(i*101-15000);
        const auto original=pcm;processing.enablePcmObservation(true);processing.process(pcm,nullptr,1);
        QVERIFY(pcm==original);QCOMPARE(processing.observation().valid,9U);QCOMPARE(processing.pcmObservation().valid,0U);
    }
    /** @brief WAV限制500帧，固定前史正确、端序正确，不延长或修改真实音频。 */
    void boundedStageRecording() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto directory=temporary.path()+"/capture";
        bool completed=false;uint64_t recorded=0;
        std::thread writer([&] {
            VoiceCall::PcmDiagnosticRecorder recorder(directory);VoiceCall::PcmShapeTracker raw,aec,pre,out;
            for(uint64_t index=1;index<=750;++index) {
                VoiceCall::PcmDiagnosticFrame frame;frame.index=index;frame.captureUs=int64_t(index)*20000;frame.valid=31;
                frame.raw.fill(index<=100 ? int16_t(1000+index):int16_t(13000));frame.aec.fill(2001);frame.preprocess.fill(3001);frame.output.fill(4001);frame.reference.fill(5001);
                frame.observation.raw=raw.observe(frame.raw);frame.observation.aec=aec.observe(frame.aec);frame.observation.preprocess=pre.observe(frame.preprocess);frame.observation.output=out.observe(frame.output);
                recorder.consume(frame);if(recorder.done()) { recorded=index;completed=true;break; }
            }
            recorder.finish();
        });writer.join();QVERIFY(completed);QCOMPARE(recorded,uint64_t(500));
        for(const auto &name:{"raw.wav","aec.wav","preprocess.wav","upload.wav","reference.wav"}) {
            const auto bytes=readFixture(directory+'/'+name);QCOMPARE(bytes.size(),44+500*320*2);QCOMPARE(bytes.left(4),QByteArray("RIFF"));QCOMPARE(bytes.mid(8,4),QByteArray("WAVE"));
        }
        const auto raw=readFixture(directory+"/raw.wav");QCOMPARE(uint16_t(uint8_t(raw[44]))|(uint16_t(uint8_t(raw[45]))<<8),uint16_t(1001));
        const auto csv=readFixture(directory+"/frames.csv");QVERIFY(csv.contains("1,20000,31,0,"));QVERIFY(csv.contains("500,10000000,31,0,"));QVERIFY(!csv.contains("501,10020000,"));
    }
    /** @brief 诊断队列缺帧要在索引标明；无效中间阶段不能被当作有效静音。 */
    void recordingGapAndNoOverwrite() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto directory=temporary.path()+"/gap";
        std::thread writer([&] { VoiceCall::PcmDiagnosticRecorder recorder(directory);VoiceCall::PcmDiagnosticFrame frame;
            frame.index=1;frame.valid=9;frame.raw.fill(100);frame.output.fill(200);recorder.consume(frame,true);
            frame.index=4;recorder.consume(frame);recorder.finish(); });writer.join();
        const auto csv=readFixture(directory+"/frames.csv");QVERIFY(csv.contains("4,0,9,2,"));
        const auto original=readFixture(directory+"/raw.wav");
        std::thread second([&] { VoiceCall::PcmDiagnosticRecorder recorder(directory);VoiceCall::PcmDiagnosticFrame frame;recorder.consume(frame,true); });second.join();
        QCOMPARE(readFixture(directory+"/raw.wav"),original);
    }
    /** @brief 解析累计运行队列等待、主动切换及cgroup节流，支持comm内括号及/proc伪文件读取方式。 */
    void schedulingEvidence() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto proc=temporary.path()+"/proc",cg=temporary.path()+"/cg",thread=proc+"/self/task/123";
        QVERIFY(writeFixture(proc+"/sys/kernel/sched_schedstats","1\n"));QVERIFY(writeFixture(proc+"/self/cgroup","0::/apps/test\n"));
        QVERIFY(writeFixture(cg+"/apps/test/cpu.max","20000 100000\n"));QVERIFY(writeFixture(cg+"/apps/test/cpu.stat","nr_periods 10\nnr_throttled 2\nthrottled_usec 1000\n"));
        QStringList stat;for(int i=0;i<45;++i) stat<<"0";stat[0]="R";stat[15]="20";stat[16]="0";stat[36]="2";stat[38]="0";
        QVERIFY(writeFixture(thread+"/stat",("123 (worker (nested)) "+stat.join(' ')+'\n').toUtf8()));
        QVERIFY(writeFixture(thread+"/status","voluntary_ctxt_switches:\t5\nnonvoluntary_ctxt_switches:\t7\n"));
        QVERIFY(writeFixture(thread+"/wchan","futex_wait_queue_me\n"));QVERIFY(writeFixture(thread+"/schedstat","100000000 2000000 10\n"));
        VoiceCall::SchedulingDiagnostics sampler(proc,cg);sampler.poll(1000000,{{123,0,0}});
        QVERIFY(writeFixture(thread+"/schedstat","150000000 22000000 15\n"));
        QVERIFY(writeFixture(thread+"/status","voluntary_ctxt_switches:\t6\nnonvoluntary_ctxt_switches:\t9\n"));
        QVERIFY(writeFixture(cg+"/apps/test/cpu.stat","nr_periods 12\nnr_throttled 3\nthrottled_usec 16000\n"));
        const auto lines=sampler.poll(1300000,{{123,0,0}},true).join('\n');
        QVERIFY(lines.contains("runUs=50000 runqueueWaitUs=20000 slices=5"));QVERIFY(lines.contains("vCtx=1 ivCtx=2"));
        QVERIFY(lines.contains("throttleCountDelta=1 throttledUsDelta=15000"));QVERIFY(lines.contains("processor=2"));
        QVERIFY(sampler.poll(1350000,{{123,0,0}},true).isEmpty());
    }
    /** @brief 无proc/内核能力保持未知，不能把不支持错误解释为零等待。 */
    void schedulingUnknown() {
        QTemporaryDir temporary;VoiceCall::SchedulingDiagnostics sampler(temporary.path()+"/missing",temporary.path()+"/missing-cg");
        const auto lines=sampler.poll(1000000,{{123,0,0}},true).join('\n');QVERIFY(lines.contains("schedstatAvailable=0"));QVERIFY(lines.contains("runUs=-1 runqueueWaitUs=-1"));
    }
    /** @brief 慢业务含嵌套Qt事件时，自身时长扣除子事件，不把全部墙钟归给业务或MQTT。 */
    void nestedUiAccounting() {
        messages.clear();const auto previousHandler=qInstallMessageHandler(captureMessage);
        struct RestoreHandler { QtMessageHandler handler;~RestoreHandler(){qInstallMessageHandler(handler);} } restore{previousHandler};
        UiPerformance::setCallContext("diagnostic-test");QObject owner;owner.setObjectName("fixture");
        {
            UiPerformance::OperationScope operation(UiPerformance::Operation::MqttBusiness,&owner,"getPlayInfo-test");
            QEventLoop nested;QTimer::singleShot(0,&nested,[&] {QThread::msleep(22);nested.quit();});nested.exec();QThread::msleep(12);operation.setResult(1);
        }
        UiPerformance::reportNow("nested-test");QString row;
        for(const auto &line:messages) if(line.contains("op=mqttBusiness") && line.contains("nested-test")) row=line;
        QVERIFY(!row.isEmpty());const auto field=[&](const QString &name) {const auto m=QRegularExpression("\\b"+name+"=(-?\\d+)").match(row);return m.hasMatch()?m.captured(1).toLongLong():-1;};
        QVERIFY(field("nestedNotifyUs")>=20000);QVERIFY(field("selfUs")>=10000);QVERIFY(field("nestedNotifyCount")>0);QCOMPARE(field("result"),qint64(1));
        QVERIFY(row.contains("detail=\"getPlayInfo-test\""));QVERIFY(row.contains("vCtx="));
        UiPerformance::setCallContext({});QCoreApplication::processEvents();
    }
};
/** @brief 使用真实诊断QApplication，所有设备边界均为隔离测试数据。 */
int main(int argc,char **argv) { UiDiagnosticApplication app(argc,argv);DiagnosticCauseTests tests;return QTest::qExec(&tests,argc,argv); }
#include "diagnostic_cause_tests.moc"
