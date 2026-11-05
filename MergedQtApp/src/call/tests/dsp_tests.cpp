/**
 * @file dsp_tests.cpp
 * @brief 以真实SpeexDSP验证回声下降、双讲人声保留及V1旁路；参考/配置独立回归。
 * @author Du lin
 * @date 2026-10-31
 * @note 最近修改：2026-11-04；修改人：Du lin；验证参考失败细分、同帧平滑/后备预处理、失参后恢复及双讲能量；保留全部禁用V1逐样本一致与既有AEC收敛基线。
 */
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QFile>
#include "src/call/AudioProcessing.h"
#include "src/call/PlaybackReference.h"
#include <vector>
#include "src/call/CaptureContinuity.h"

/** @brief 可重现的宽带远端/近端信号试验结果，不是现场AEC效果保证。 */
struct ScenarioResult {
    double erleDb=0,nearEnergyRatio=0; /**< 远端独讲残余能量比及双讲输出/近端RMS比。 */
    int voicedFrames=0; /**< 双讲阶段非静音帧数。 */
    VoiceCall::ProcessingStats stats;
};
/** @brief 用真实库处理确定性线性回声与同时近端讲话，无播放设备或录音文件。
 * @param[in] config 测试参数快照，库路径来自测试环境或系统搜索。
 * @param[out] result 单讲/双讲指标。
 * @param[out] error 初始化失败说明。
 * @return true已完成；false处理器不可用。
 */
static bool scenario(const VoiceCall::ProcessingConfig &config,ScenarioResult &result,QString &error)
{
    VoiceCall::AudioProcessing processing(config,error);
    if (!processing.active()) return false;
    const int frames=900;std::vector<int16_t> far(size_t(frames)*VoiceCall::Samples+128);
    uint32_t seed=17;
    for (auto &sample:far) { seed=1664525*seed+1013904223;sample=int16_t(int((seed>>16)&4095)-2048); }
    double echoEnergy=0,residualEnergy=0,nearEnergy=0,outputEnergy=0;
    for (int frame=0;frame<frames;++frame) {
        VoiceCall::Pcm reference{},mic{};double frameEnergy=0;
        for (int i=0;i<VoiceCall::Samples;++i) {
            const int index=frame*VoiceCall::Samples+i;
            reference[size_t(i)]=far[size_t(index+128)];
            const double echo=.6*far[size_t(index+64)]+.2*far[size_t(index+32)];
            const double near=frame>=650 ? 4500*std::sin(2*3.141592653589793*701*index/16000.0) : 0;
            mic[size_t(i)]=int16_t(echo+near);
            if (frame>=450 && frame<650) echoEnergy+=echo*echo;
            if (frame>=700) nearEnergy+=near*near;
        }
        processing.process(mic,&reference,1);
        for (const auto sample:mic) {
            if (frame>=450 && frame<650) residualEnergy+=double(sample)*sample;
            if (frame>=700) { outputEnergy+=double(sample)*sample;frameEnergy+=double(sample)*sample; }
        }
        if (frame>=700 && frameEnergy>320*100*100) ++result.voicedFrames;
    }
    result.erleDb=10*std::log10((echoEnergy+1)/(residualEnergy+1));
    result.nearEnergyRatio=std::sqrt(outputEnergy/(nearEnergy+1));result.stats=processing.stats();return true;
}
/** @brief DSP边界回归；算法项需要系统运行库或VOICE_TEST_SPEEX_LIBRARY绝对路径。 */
class DspTests : public QObject {
    Q_OBJECT
private:
    /** @return 本进程选择的真实库路径；空值表示使用已安装的系统库。 */
    QString library() const { return qEnvironmentVariable("VOICE_TEST_SPEEX_LIBRARY"); }
private slots:
    /** @brief 全部关闭时逐样本与V1完全相同，且不尝试加载不存在的库。 */
    void disabledPassthrough() {
        VoiceCall::ProcessingConfig cfg;cfg.aecEnabled=false;cfg.preprocessEnabled=false;
        cfg.libraryPath=QDir::temp().filePath("no-speex-library-disabled");QString info;
        VoiceCall::AudioProcessing processing(cfg,info);QVERIFY(!processing.active());QVERIFY(!processing.needsReference());
        VoiceCall::Pcm original{};for(int i=0;i<320;++i) original[size_t(i)]=int16_t(i*199-32000);
        for(int i=0;i<100;++i) { auto pcm=original;processing.process(pcm,nullptr,0);QVERIFY(pcm==original); }
        QCOMPARE(processing.stats().bypassFrames,uint64_t(100));QCOMPARE(processing.stats().failures,uint64_t(0));
    }
    /** @brief 缺少运行库时初始化安全失败，原PCM和帧长保持不变。 */
    void missingLibrary() {
        QTemporaryDir dir;VoiceCall::ProcessingConfig cfg;cfg.libraryPath=dir.filePath("missing-speex.dll");
        cfg.preprocessEnabled=true;QString info;VoiceCall::AudioProcessing processing(cfg,info);
        QVERIFY(!processing.active());QVERIFY(info.contains("bypass V1"));VoiceCall::Pcm pcm{};pcm.fill(1234);
        const auto original=pcm;processing.process(pcm,nullptr,1);QVERIFY(pcm==original);QCOMPARE(processing.stats().failures,uint64_t(1));
    }
    /** @brief 验证独立开关、非法配置限幅及双讲抑制保护，不改变INI其他内容。 */
    void configuration() {
        QTemporaryDir dir;const auto path=dir.filePath("net_cfg.ini");
        { QSettings ini(path,QSettings::IniFormat);
          ini.setValue("mqtt/host","unchanged");ini.setValue("VoiceCallDsp/aecEnabled","off");
          ini.setValue("VoiceCallDsp/preprocessEnabled","true");ini.setValue("VoiceCallDsp/filterLengthMs",99999);
          ini.setValue("VoiceCallDsp/referenceHistoryFrames",1);ini.setValue("VoiceCallDsp/echoSuppressDb",-2);
          ini.setValue("VoiceCallDsp/echoSuppressActiveDb",-99);ini.setValue("VoiceCallDsp/agcEnabled","bad");ini.sync(); }
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=file.readAll();file.close();
        QString warnings;const auto cfg=VoiceCall::ProcessingConfig::load(path,&warnings);
        QVERIFY(!cfg.aecEnabled);QVERIFY(cfg.preprocessEnabled);QVERIFY(cfg.agcEnabled);
        QCOMPARE(cfg.filterLengthMs,500);QCOMPARE(cfg.referenceHistoryFrames,8);QCOMPARE(cfg.echoSuppressActiveDb,-2);
        QCOMPARE(cfg.agcDecrementDbPerSec,-12);QVERIFY(!warnings.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);
    }
    /** @brief 校验跨帧/分数采样参考、正负偏移、环上限及断时清历史。 */
    void referenceAlignment() {
        VoiceCall::PlaybackReference history;
        for (int frame=0;frame<6;++frame) {
            VoiceCall::Pcm pcm{};for(int i=0;i<320;++i) pcm[size_t(i)]=int16_t(frame*320+i);
            history.push(pcm,1000000+frame*20000,5000,32);
        }
        VoiceCall::Pcm reference{};
        QVERIFY(history.read(1030000,0,reference));for(int i=0;i<320;++i) QCOMPARE(reference[size_t(i)],int16_t(480+i));
        QVERIFY(history.read(1030000,10000,reference));QCOMPARE(reference[0],int16_t(320));
        QVERIFY(history.read(1030000,-10000,reference));QCOMPARE(reference[0],int16_t(640));
        QVERIFY(history.read(1030040,0,reference));QCOMPARE(reference[0],int16_t(481));
        QVERIFY(!history.read(990000,0,reference));QVERIFY(!history.read(1200000,0,reference));
        const auto generation=history.generation();VoiceCall::Pcm silence{};
        history.push(silence,2000000,5000,32);QVERIFY(history.generation()!=generation);QCOMPARE(history.size(),size_t(1));
        QVERIFY(!history.read(1030000,0,reference));QVERIFY(history.read(2000000,0,reference));QVERIFY(reference==silence);
        for(int i=1;i<500;++i) history.push(silence,2000000+i*20001,5000,32);
        QCOMPARE(history.size(),size_t(32));QCOMPARE(history.generation(),generation+1);
    }
    /** @brief 缺文件使用全部新默认且不创建文件；旧INI中的库路径不再影响系统加载。 */
    void defaultsAndLegacyLibraryKey() {
        QTemporaryDir dir;const auto path=dir.filePath("net_cfg.ini");
        const auto cfg=VoiceCall::ProcessingConfig::load(path);
        QVERIFY(!QFile::exists(path));QVERIFY(cfg.aecEnabled);QVERIFY(cfg.preprocessEnabled);
        QVERIFY(cfg.denoiseEnabled);QVERIFY(cfg.agcEnabled);QCOMPARE(cfg.filterLengthMs,200);
        QCOMPARE(cfg.noiseSuppressDb,-10);QCOMPARE(cfg.echoSuppressDb,-40);QCOMPARE(cfg.echoSuppressActiveDb,-12);
        QCOMPARE(cfg.agcTarget,8000);QCOMPARE(cfg.agcMaxGainDb,6);QCOMPARE(cfg.agcIncrementDbPerSec,6);
        QCOMPARE(cfg.agcDecrementDbPerSec,-12);QCOMPARE(cfg.alignmentToleranceMs,5);
        QCOMPARE(cfg.referenceDelayMs,0);QCOMPARE(cfg.referenceHistoryFrames,32);QVERIFY(cfg.libraryPath.isEmpty());
        { QSettings ini(path,QSettings::IniFormat);ini.setValue("VoiceCallDsp/libraryPath","/missing/obsolete.so");ini.sync(); }
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=file.readAll();file.close();
        QVERIFY(VoiceCall::ProcessingConfig::load(path).libraryPath.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);
    }
    /** @brief 实际AEC在已对齐线性回声上收敛，双讲输出保留连续近端能量。 */
    void realAec() {
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=library();cfg.preprocessEnabled=false;ScenarioResult result;QString info;
        QVERIFY2(scenario(cfg,result,info),qPrintable(info));
        qInfo()<<"synthetic AEC ERLE dB="<<result.erleDb<<"double-talk RMS ratio="<<result.nearEnergyRatio;
        QVERIFY(result.erleDb>10);QVERIFY(result.nearEnergyRatio>.65);QVERIFY(result.nearEnergyRatio<1.5);
        QVERIFY(result.voicedFrames>=195);QCOMPARE(result.stats.aecFrames,uint64_t(900));QCOMPARE(result.stats.preprocessFrames,uint64_t(0));
    }
    /** @brief 旧温和配置显式保留作双讲基线，避免默认值变化改变原回归目标。 */
    void realPreprocessorDoubleTalk() {
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=library();cfg.preprocessEnabled=true;cfg.agcEnabled=false;
        cfg.noiseSuppressDb=-6;cfg.echoSuppressDb=-18;cfg.echoSuppressActiveDb=-3;ScenarioResult result;QString info;
        QVERIFY2(scenario(cfg,result,info),qPrintable(info));
        qInfo()<<"synthetic AEC+PP ERLE dB="<<result.erleDb<<"double-talk RMS ratio="<<result.nearEnergyRatio;
        QVERIFY(result.erleDb>10);QVERIFY(result.nearEnergyRatio>.50);QVERIFY(result.voicedFrames>=195);
        QCOMPARE(result.stats.preprocessFrames,uint64_t(900));QCOMPARE(result.stats.failures,uint64_t(0));
    }
    /** @brief 新生产默认的AEC/预处理/AGC均调用真实库，双讲仍有连续非静音输出。 */
    void productionDefaults() {
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=library();ScenarioResult result;QString info;
        QVERIFY2(scenario(cfg,result,info),qPrintable(info));
        qInfo()<<"production defaults ERLE dB="<<result.erleDb<<"double-talk RMS ratio="<<result.nearEnergyRatio;
        QVERIFY(result.erleDb>10);QVERIFY(result.voicedFrames>=195);QVERIFY(result.nearEnergyRatio>0);
        QCOMPARE(result.stats.aecFrames,uint64_t(900));QCOMPARE(result.stats.preprocessFrames,uint64_t(900));
        QCOMPARE(result.stats.failures,uint64_t(0));
    }
    /** @brief 可选AGC的真实控制请求可用且受最大增益约束；不通过VAD丢帧。 */
    void optionalAgc() {
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=library();cfg.aecEnabled=false;
        cfg.preprocessEnabled=true;cfg.denoiseEnabled=false;cfg.agcEnabled=true;QString info;
        VoiceCall::AudioProcessing processing(cfg,info);QVERIFY2(processing.active(),qPrintable(info));
        double input=0,output=0;
        for(int frame=0;frame<120;++frame) {
            VoiceCall::Pcm pcm{};
            for(int i=0;i<320;++i) { const auto value=int16_t(4000*std::sin(2*3.141592653589793*500*(frame*320+i)/16000.0));pcm[size_t(i)]=value;if(frame>=80) input+=double(value)*value; }
            processing.process(pcm,nullptr,0);if(frame>=80) for(const auto value:pcm) output+=double(value)*value;
        }
        const double ratio=std::sqrt(output/input);
        qInfo()<<"optional AGC RMS ratio="<<ratio;
        // AGC按感知响度可降低增益；只验证非静音、最大增益和控制参数可用，不假定固定目标RMS。
        QVERIFY(output>0);QVERIFY(ratio<2.3);
        QCOMPARE(processing.stats().preprocessFrames,uint64_t(120));QCOMPARE(processing.stats().failures,uint64_t(0));
    }
    /** @brief 库存在且符号齐全，但初始化返回空时必须安全退回V1。 */
    void initializationFailure() {
        const auto fault=qEnvironmentVariable("VOICE_TEST_SPEEX_FAULT_LIBRARY");
        if(fault.isEmpty()) QSKIP("Set VOICE_TEST_SPEEX_FAULT_LIBRARY to run controlled initialization failure");
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=fault;QString info;
        VoiceCall::AudioProcessing processing(cfg,info);QVERIFY(!processing.active());
        QVERIFY(info.contains("initialization failed"));VoiceCall::Pcm pcm{};pcm.fill(-1000);const auto original=pcm;
        processing.process(pcm,nullptr,0);QVERIFY(pcm==original);QCOMPARE(processing.stats().failures,uint64_t(1));
    }
    /** @brief 每个参考覆盖失败返回不同原因和明确边界，不使用部分插值输出。 */
    void referenceFailureReasons() {
        VoiceCall::PlaybackReference history;VoiceCall::ReferenceReadTrace trace;VoiceCall::Pcm pcm{},out{};
        QVERIFY(!history.read(1000000,0,out,&trace));QCOMPARE(int(trace.reason),int(VoiceCall::ReferenceReason::Empty));
        history.push(pcm,1000000,5000,32);
        QVERIFY(!history.read(999999,0,out,&trace));QCOMPARE(int(trace.reason),int(VoiceCall::ReferenceReason::BeforeHistory));
        QVERIFY(!history.read(1020000,0,out,&trace));QCOMPARE(int(trace.reason),int(VoiceCall::ReferenceReason::AfterHistory));
        QVERIFY(!history.read(1000031,0,out,&trace));QCOMPARE(int(trace.reason),int(VoiceCall::ReferenceReason::NextSample));QCOMPARE(trace.failedSample,319);
        QVERIFY(history.read(1000000,0,out,&trace));QCOMPARE(int(trace.reason),int(VoiceCall::ReferenceReason::Valid));
        QCOMPARE(trace.oldestUs,int64_t(1000000));QCOMPARE(trace.newestEndUs,int64_t(1020000));
    }
    /** @brief 缺参边界连续、恢复只混合同一帧，直流逐渐消除且不重放旧语音。 */
    void pcmTransition() {
        VoiceCall::CaptureContinuity continuity;VoiceCall::Pcm normal{},raw{},fallback{};normal.fill(3);raw.fill(630);
        continuity.prepare(raw,fallback);QVERIFY(!continuity.finish(normal,true,&fallback));
        for(int n=0;n<40;++n) continuity.prepare(raw,fallback);
        auto missing=fallback;QVERIFY(continuity.finish(missing,false,nullptr));QCOMPARE(missing.front(),int16_t(3));
        VoiceCall::LevelStats level;level.add(missing);QVERIFY(std::abs(level.dc())<10);
        normal.fill(20);fallback.fill(10);const auto previous=missing.back();
        QVERIFY(continuity.finish(normal,true,&fallback));QCOMPARE(normal.front(),previous);QCOMPARE(normal.back(),int16_t(20));
        normal.fill(700);const auto unmodified=normal;QVERIFY(!continuity.finish(normal,true,&fallback));QVERIFY(normal==unmodified);
    }
    /** @brief 连续近端人声叠加DC时短缺参考不静音，丢失/恢复边界连续，使用真实Speex库。 */
    void nearSpeechDuringReferenceLoss() {
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=library();cfg.preprocessEnabled=true;cfg.agcEnabled=false;cfg.denoiseEnabled=false;
        QString info;VoiceCall::AudioProcessing processing(cfg,info);QVERIFY2(processing.active(),qPrintable(info));
        VoiceCall::Pcm reference{};int tail=0;bool previousLoss=false;
        for(int frame=0;frame<260;++frame) {
            VoiceCall::Pcm mic{};for(int i=0;i<320;++i) mic[size_t(i)]=int16_t(631+3000*std::sin(2*3.141592653589793*701*(frame*320+i)/16000.0));
            const bool lost=frame==200 || frame==201 || frame==230;
            processing.process(mic,lost ? nullptr : &reference,1);
            if(lost!=previousLoss && frame>0) QCOMPARE(int(mic.front()),tail);
            if(frame>=190) { VoiceCall::LevelStats level;level.add(mic);QVERIFY2(level.rms()>100,"Near speech must not be muted during missing-reference fallback"); }
            tail=mic.back();previousLoss=lost;
        }
        QCOMPARE(processing.stats().fallbackFrames,uint64_t(3));QCOMPARE(processing.stats().transitionFrames,uint64_t(4));
        QCOMPARE(processing.stats().failures,uint64_t(0));
    }
    /** @brief 参考丢失时后备预处理并仅重置一次AEC，换代后可恢复；预处理可独立运行。 */
    void lossResetAndIndependentPreprocess() {
        VoiceCall::ProcessingConfig cfg;cfg.libraryPath=library();cfg.preprocessEnabled=true;QString info;
        VoiceCall::AudioProcessing processing(cfg,info);QVERIFY2(processing.active(),qPrintable(info));
        VoiceCall::Pcm pcm{},reference{};pcm.fill(1200);processing.process(pcm,&reference,1);
        pcm.fill(2345);const auto original=pcm;processing.process(pcm,nullptr,1);QVERIFY(pcm!=original);QCOMPARE(processing.stats().fallbackFrames,uint64_t(1));
        const auto resets=processing.stats().resets;processing.process(pcm,nullptr,1);QCOMPARE(processing.stats().resets,resets);
        processing.process(pcm,&reference,2);QVERIFY(processing.stats().aecFrames>=2);
        cfg.aecEnabled=false;VoiceCall::AudioProcessing independent(cfg,info);QVERIFY(independent.active());QVERIFY(!independent.needsReference());
        independent.process(pcm,nullptr,0);QCOMPARE(independent.stats().preprocessFrames,uint64_t(1));
    }
};
QTEST_GUILESS_MAIN(DspTests)
#include "dsp_tests.moc"
