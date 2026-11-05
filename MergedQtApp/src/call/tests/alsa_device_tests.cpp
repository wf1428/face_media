/**
 * @file alsa_device_tests.cpp
 * @brief 通过实际 AlsaAudioDevice 实现验证部分传输、双声道复制、字节序和 XRUN 恢复。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；以隔离amixer子进程验证默认240逐通道上限、保留低值、显式覆盖、失败回滚及关闭恢复/外部变更保留；原PCM和XRUN回归不访问真实硬件。
 */
#include <QtTest>
#include "src/call/AlsaAudioDevice.h"
#include "src/call/AudioProcessing.h"
#include "alsa/asoundlib.h"
#include <cerrno>

/** @brief 测试句柄，保留设备协商参数和已传输样本数。 */
struct _snd_pcm {
    int stream; /**< 输入或输出方向。 */
    unsigned channels=0; /**< 实际封装请求的通道数。 */
    unsigned offset=0; /**< 已读写的音频帧数。 */
    bool started=false; /**< capture仅显式start后产出数据；prepare后清除。 */
    snd_config_t *config=nullptr; /**< 借用捕获配置，要求比测试句柄长寿。 */
};
/** @brief 最小配置替身；故意以双通道平均为全局默认，验证局部覆盖不会污染全局。 */
struct _snd_config {
    bool local=false; /**< false为静态全局树，true由测试delete释放。 */
    QByteArray policy="average"; /**< 当前plughw转换策略。 */
    double channel0=.5,channel1=.5; /**< 源通道0/1到目标单声道的系数。 */
};
/** @brief 内存配置输入，持有规则正文，由input_close释放。 */
struct _snd_input { QByteArray rules; };
/** @brief 测试所有的软件参数快照，仅用于只读诊断。 */
struct _snd_pcm_sw_params {};
static _snd_config globalConfig; /**< 静态借用全局树，生产实现不得修改。 */
static int configObjects=0,inputObjects=0,configReferences=0; /**< 未释放配置、内存输入及全局引用数。 */
static int configFailure=0; /**< 1..6依次注入update/copy/search/input/load/open失败；0不注入。 */
static int starts=0,waitError=0; /**< 采集启动调用次数及一次性wait错误注入。 */
static bool noData=false,failStart=false; /**< 模拟一直无数据或显式启动失败。 */
static QByteArray playbackBytes; /**< API 替身收到的真实交错 PCM 字节。 */
static int injectedError=0,prepared=0,handles=0; /**< 注入下次传输错误、prepare 次数、未释放句柄数。 */
static int timingQueries=0; /**< 同步延迟查询次数，关闭时必须保持0。 */
static bool failTiming=false; /**< true注入元数据查询失败，不应让基础传输失败。 */
static snd_pcm_state_t deviceState=SND_PCM_STATE_RUNNING; /**< 可注入尚未开始播放的状态。 */
static bool failParameters=false; /**< true 让参数协商失败，检查句柄清理。 */
static int geometryFailure=0,softwareObjects=0; /**< 1..5依次注入几何/malloc/current/start/avail失败；软件快照存量。 */
static int64_t timingAvailable=640,timingDelay=640; /**< 时间查询数值注入，单位应用PCM采样帧。 */
static int failQueryAt=-1; /**< 指定累计查询序号注入错误，用于区分重查错误与首查慢。 */
static int querySlowRemaining=0; /**< 连续慢查询次数；0时原queryPauseMs仍只注入一次。 */
static int ioPauseMs=0,waitPauseMs=0,queryPauseMs=0; /**< 下一I/O、wait、时间查询的一次性耗时注入，毫秒。 */
extern "C" {
/** @brief 分配测试句柄，验证非阻塞模式。
 * @param[out] pcm 返回测试拥有的句柄。
 * @param[in] stream 方向；其他参数按实际接口传入。
 * @param[in] name 设备名，测试不访问设备。
 * @param[in] mode 必须含非阻塞标记。
 * @return 0 成功；负值为模式错误。
 */
int snd_pcm_open(snd_pcm_t **pcm,const char *name,int stream,int mode) {
    Q_UNUSED(name); if (!(mode&SND_PCM_NONBLOCK)) return -EINVAL;
    *pcm=new _snd_pcm{stream}; ++handles; return 0;
}
/** @brief 校验生产局部配置为仅0→0，配置借用至句柄关闭。 */
int snd_pcm_open_lconf(snd_pcm_t **pcm,const char *name,int stream,int mode,snd_config_t *config) {
    if(configFailure==6) return -EIO;
    if(!config || !config->local || config->policy!="copy" || config->channel0!=1 || config->channel1!=0) return -EINVAL;
    const int rc=snd_pcm_open(pcm,name,stream,mode);if(rc>=0) (*pcm)->config=config;return rc;
}
/** @brief 只增加静态全局树引用；可注入读取失败。 */
int snd_config_update_ref(snd_config_t **config) { if(configFailure==1) return -EIO;*config=&globalConfig;++configReferences;return 0; }
/** @brief 分配深复制树；不修改源对象。 */
int snd_config_copy(snd_config_t **destination,snd_config_t *source) {
    if(configFailure==2) return -ENOMEM;
    *destination=new _snd_config(*source);(*destination)->local=true;++configObjects;return 0;
}
/** @brief 配对归还全局引用，局部树不能走此释放路径。 */
void snd_config_unref(snd_config_t *config) { Q_ASSERT(config==&globalConfig);--configReferences; }
/** @brief 借用本替身的plughw配置节点。 */
int snd_config_search(snd_config_t *config,const char *key,snd_config_t **result) {
    if(configFailure==3) return -ENOENT;
    if(QByteArray(key)!="pcm.plughw") return -EINVAL;
    *result=config;return 0;
}
/** @brief 复制内存规则字符串，失败不分配输入。 */
int snd_input_buffer_open(snd_input_t **input,const char *buffer,long size) {
    if(configFailure==4) return -ENOMEM;
    *input=new _snd_input{QByteArray(buffer,int(size))};++inputObjects;return 0;
}
/** @brief 只接受copy及仅0→0规则；覆盖原有Channel 1系数，模拟整表替换。 */
int snd_config_load_override(snd_config_t *config,snd_input_t *input) {
    if(configFailure==5) return -EINVAL;
    if(!config->local || !input->rules.contains("route_policy copy") || !input->rules.contains("ttable { 0 { 0 1.0 } }")) return -EINVAL;
    config->policy="copy";config->channel0=1;config->channel1=0;return 0;
}
/** @brief 释放内存输入并记录剩余数量。 */
int snd_input_close(snd_input_t *input) { delete input;--inputObjects;return 0; }
/** @brief 仅释放局部配置树，误删全局树立即触发断言。 */
int snd_config_delete(snd_config_t *config) { Q_ASSERT(config->local);delete config;--configObjects;return 0; }
/** @brief PREPARED采集显式启动；失败保留未启动状态。 */
int snd_pcm_start(snd_pcm_t *pcm) { ++starts;if(failStart) return -EIO;pcm->started=true;return 0; }
/** @brief 保存协商通道数，仅接受实际协议要求的格式、采样率和交错访问。 */
int snd_pcm_set_params(snd_pcm_t *pcm,int format,int access,unsigned channels,unsigned rate,int resample,unsigned latency) {
    Q_UNUSED(resample); Q_UNUSED(latency); pcm->channels=channels;
    return failParameters || format!=SND_PCM_FORMAT_S16_LE || rate!=16000 || access!=SND_PCM_ACCESS_RW_INTERLEAVED ? -EINVAL : 0;
}
/** @brief 返回固定应用PCM几何，故障只影响只读诊断。 */
int snd_pcm_get_params(snd_pcm_t *,snd_pcm_uframes_t *buffer,snd_pcm_uframes_t *period) {
    if(geometryFailure==1) return -EIO;
    *buffer=960;*period=320;return 0;
}
/** @brief 分配一个测试快照或注入分配失败。 */
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **params) {
    if(geometryFailure==2) return -ENOMEM;
    *params=new _snd_pcm_sw_params;++softwareObjects;return 0;
}
/** @brief 配对释放软件快照。 */
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *params) { delete params;--softwareObjects; }
/** @brief 注入只读快照加载失败，不修改PCM。 */
int snd_pcm_sw_params_current(snd_pcm_t *,snd_pcm_sw_params_t *) { return geometryFailure==3 ? -EIO : 0; }
/** @brief 返回固定启动阈值，支持单项查询失败。 */
int snd_pcm_sw_params_get_start_threshold(const snd_pcm_sw_params_t *,snd_pcm_uframes_t *value) { if(geometryFailure==4) return -EIO;*value=960;return 0; }
/** @brief 返回固定唤醒阈值，支持单项查询失败。 */
int snd_pcm_sw_params_get_avail_min(const snd_pcm_sw_params_t *,snd_pcm_uframes_t *value) { if(geometryFailure==5) return -EIO;*value=320;return 0; }
/** @brief 模拟copy插件输出mono：Channel 0为确定波形，Channel 1负满幅仅在错误映射时混入。 */
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm,void *buffer,snd_pcm_uframes_t frames) {
    if (injectedError) { int error=injectedError; injectedError=0; return error; }
    if (pcm->channels!=1 || !pcm->config) return -EINVAL;
    if (!pcm->started || noData) return -EAGAIN;
    const unsigned n=unsigned(qMin<snd_pcm_uframes_t>(frames,80)); auto *bytes=static_cast<unsigned char *>(buffer);
    for (unsigned i=0;i<n;++i) {
        const int channel0=int(pcm->offset+i)*199-32000;
        const uint16_t value=uint16_t(int(pcm->config->channel0*channel0+pcm->config->channel1*(-32768)));
        bytes[2*i]=uint8_t(value); bytes[2*i+1]=uint8_t(value>>8);
    }
    pcm->offset+=n; return n;
}
/** @brief 保留最多 80 帧双声道字节，以强制生产实现处理部分写入。 */
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm,const void *buffer,snd_pcm_uframes_t frames) {
    if(ioPauseMs) { const int delay=ioPauseMs;ioPauseMs=0;QTest::qSleep(delay); }
    if (injectedError) { int error=injectedError; injectedError=0; return error; }
    if (pcm->channels!=2) return -EINVAL;
    const unsigned n=unsigned(qMin<snd_pcm_uframes_t>(frames,80));
    playbackBytes.append(static_cast<const char *>(buffer),int(n*4)); pcm->offset+=n; return n;
}
/** @brief 可控等待；测试设备数据立即可用。 */
int snd_pcm_wait(snd_pcm_t *,int timeout) {
    if(waitPauseMs) { const int delay=waitPauseMs;waitPauseMs=0;QTest::qSleep(delay); }
    if(waitError) { const int error=waitError;waitError=0;return error; }
    if(noData) { QTest::qSleep(timeout);return 0; }return 1;
}
/** @brief 记录 XRUN 恢复并重置模拟传输位置。 */
int snd_pcm_prepare(snd_pcm_t *pcm) { ++prepared; pcm->offset=0;pcm->started=false;return 0; }
/** @brief 提供 40ms 模拟输出排队延迟。 */
int snd_pcm_delay(snd_pcm_t *,snd_pcm_sframes_t *delay) { *delay=640; return 0; }
/** @brief 同步查询替身，便于验证延迟被放在采集与播放时间公式的不同方向。 */
int snd_pcm_avail_delay(snd_pcm_t *,snd_pcm_sframes_t *available,snd_pcm_sframes_t *delay) {
    if(queryPauseMs) { const int pause=queryPauseMs;if(querySlowRemaining>0) --querySlowRemaining;if(querySlowRemaining==0) queryPauseMs=0;QTest::qSleep(pause); }
    ++timingQueries;*available=timingAvailable;*delay=timingDelay;return failTiming || timingQueries==failQueryAt ? -EIO : 0;
}
/** @return 注入的设备运行状态。 */
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm) {
    return pcm->stream==SND_PCM_STREAM_CAPTURE && !pcm->started ? SND_PCM_STATE_PREPARED : deviceState;
}

/** @brief 丢弃测试数据，无阻塞。 */
int snd_pcm_drop(snd_pcm_t *) { return 0; }
/** @brief 释放测试拥有的句柄并更新计数。 */
int snd_pcm_close(snd_pcm_t *pcm) { if(pcm->config) Q_ASSERT(pcm->config->local && configObjects>0);delete pcm; --handles; return 0; }
/** @return 固定测试错误描述。 */
const char *snd_strerror(int) { return "injected ALSA error"; }
}
/** @brief 执行实际驱动包装器测试，无 ALSA 硬件依赖。 */
class AlsaDeviceTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir mixerDirectory;QString mixerFile;
private slots:
    /** @brief 安装同一测试程序的amixer模式，仅改隔离文件，不访问声卡。 */
    void initTestCase() {
        QVERIFY(mixerDirectory.isValid());mixerFile=mixerDirectory.filePath("volume.txt");
#ifdef Q_OS_WIN
        const auto name=QStringLiteral("amixer.exe");
#else
        const auto name=QStringLiteral("amixer");
#endif
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),mixerDirectory.filePath(name)));
        qputenv("VOICE_TEST_MIXER_FILE",mixerFile.toLocal8Bit());
        qputenv("PATH",(mixerDirectory.path()+QDir::listSeparator()+qEnvironmentVariable("PATH")).toLocal8Bit());
    }
private:
    /** @brief 每轮通过文件设置模拟实际硬件增益。 */
    void setMixer(const QByteArray &value) { QFile file(mixerFile);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(value),qint64(value.size())); }
    /** @return 隔离硬件增益文件的当前内容。 */
    QByteArray mixer() { QFile file(mixerFile);if(!file.open(QIODevice::ReadOnly)) return {};return file.readAll(); }
private slots:
    /** @brief 各测试独立复位计时注入，不触及生产设备。 */
    void init() {
        qunsetenv("QT_YCEST_CALL_CAPTURE_VOLUME");setMixer("255,255");
        QCOMPARE(handles,0);QCOMPARE(configObjects,0);QCOMPARE(inputObjects,0);QCOMPARE(configReferences,0);
        timingAvailable=timingDelay=640;timingQueries=0;failTiming=false;deviceState=SND_PCM_STATE_RUNNING;
        configFailure=0;starts=0;waitError=0;noData=false;failStart=false;
        failQueryAt=-1;querySlowRemaining=0;geometryFailure=0;ioPauseMs=waitPauseMs=queryPauseMs=0;injectedError=0;failParameters=false;
        QCOMPARE(softwareObjects,0);
    }
    /** @brief 每项检查全局映射未污染且所有局部资源已释放。 */
    void cleanup() {
        QCOMPARE(handles,0);QCOMPARE(configObjects,0);QCOMPARE(inputObjects,0);QCOMPARE(configReferences,0);
        QCOMPARE(globalConfig.policy,QByteArray("average"));QCOMPARE(globalConfig.channel1,.5);
        QCOMPARE(softwareObjects,0);
    }
    /** @brief 验证上限只降低高值、关闭恢复原非对称值；重复close不覆盖外部更改。 */
    void captureGainHeadroom() {
        QString error;AlsaAudioDevice capture;setMixer("255,225");
        QVERIFY2(capture.open(true,error),qPrintable(error));QCOMPARE(mixer(),QByteArray("240,225"));
        capture.close();QCOMPARE(mixer(),QByteArray("255,225"));
        setMixer("190,200");QVERIFY(capture.open(true,error));QCOMPARE(mixer(),QByteArray("190,200"));capture.close();QCOMPARE(mixer(),QByteArray("190,200"));
        setMixer("255,255");QVERIFY(capture.open(true,error));setMixer("210,210");capture.close();QCOMPARE(mixer(),QByteArray("210,210"));
        capture.close();QCOMPARE(mixer(),QByteArray("210,210"));
    }
    /** @brief 显式原环境变量仍覆盖默认上限，关闭恢复原值；无效参数/开声卡失败不遗留增益更改。 */
    void captureGainOverrideAndRollback() {
        QString error;AlsaAudioDevice capture;qputenv("QT_YCEST_CALL_CAPTURE_VOLUME","245");setMixer("220,225");
        QVERIFY(capture.open(true,error));QCOMPARE(mixer(),QByteArray("245,245"));capture.close();QCOMPARE(mixer(),QByteArray("220,225"));
        qputenv("QT_YCEST_CALL_CAPTURE_VOLUME","256");QVERIFY(!capture.open(true,error));QCOMPARE(mixer(),QByteArray("220,225"));
        qunsetenv("QT_YCEST_CALL_CAPTURE_VOLUME");setMixer("255,255");failParameters=true;QVERIFY(!capture.open(true,error));failParameters=false;QCOMPARE(mixer(),QByteArray("255,255"));
        setMixer("bad");QVERIFY(!capture.open(true,error));QCOMPARE(mixer(),QByteArray("bad"));
    }
    /** @brief 验证只选Channel 0，不混入Channel 1满幅偏置；部分读拼接及L/R复制均保持。 */
    void monoStereo() {
        AlsaAudioDevice capture,playback; QString error; std::atomic_bool stop{false};
        QVERIFY(capture.open(true,error)); QVERIFY(playback.open(false,error));
        VoiceCall::Pcm pcm{}; QVERIFY(capture.transfer(pcm,stop));
        QCOMPARE(starts,1);
        for (int i=0;i<320;++i) QCOMPARE(pcm[i],int16_t(i*199-32000));
        pcm[0]=-32768; pcm[1]=32767; pcm[2]=-1; playbackBytes.clear();
        QVERIFY(playback.transfer(pcm,stop)); QCOMPARE(playbackBytes.size(),1280);
        for (int i=0;i<320;++i) {
            const auto value=uint16_t(pcm[i]);
            QCOMPARE(uint8_t(playbackBytes[4*i]),uint8_t(value));
            QCOMPARE(uint8_t(playbackBytes[4*i+1]),uint8_t(value>>8));
            QCOMPARE(playbackBytes.mid(4*i,2),playbackBytes.mid(4*i+2,2));
        }
        QCOMPARE(playback.delayMs(),40);QCOMPARE(timingQueries,0);
        capture.close();playback.close();QCOMPARE(handles,0);
    }
    /** @brief 从真正交错写缓冲取参考；采集回溯60ms、播放预测20ms，失败计时不破坏录放。 */
    void timestampsAndReference() {
        AlsaAudioDevice capture,playback;QString error;std::atomic_bool stop{false};
        QVERIFY(capture.open(true,error));QVERIFY(playback.open(false,error));
        capture.enableTiming(true);playback.enableTiming(true);
        VoiceCall::Pcm pcm{};for(int i=0;i<320;++i) pcm[size_t(i)]=int16_t(i-160);
        const auto original=pcm;const auto before=VoiceCall::monotonicUs();
        QVERIFY(playback.transfer(pcm,stop));const auto after=VoiceCall::monotonicUs();
        pcm.fill(0);VoiceCall::Pcm reference{};VoiceCall::AudioTiming timing;
        QVERIFY(playback.playbackReference(reference,timing));QVERIFY(reference==original);
        QVERIFY(timing.valid);QCOMPARE(timing.queueDelayUs,int64_t(40000));QVERIFY(timing.firstSampleUs>=before+20000);QVERIFY(timing.firstSampleUs<=after+20000);
        const auto captureBefore=VoiceCall::monotonicUs();QVERIFY(capture.transfer(pcm,stop));
        const auto captureAfter=VoiceCall::monotonicUs();timing=capture.timing();QVERIFY(timing.valid);
        QVERIFY(timing.firstSampleUs>=captureBefore-60000);QVERIFY(timing.firstSampleUs<=captureAfter-60000);
        deviceState=SND_PCM_STATE_PREPARED;QVERIFY(playback.transfer(pcm,stop));
        QVERIFY(!playback.playbackReference(reference,timing));
        deviceState=SND_PCM_STATE_RUNNING;failTiming=true;QVERIFY(capture.transfer(pcm,stop));QVERIFY(!capture.timing().valid);
        failTiming=false;playback.enableTiming(false);const int queries=timingQueries;
        QVERIFY(playback.transfer(pcm,stop));QCOMPARE(timingQueries,queries);
        capture.close();playback.close();QCOMPARE(handles,0);
    }
    /** @brief XRUN/挂起采用非阻塞 prepare；不可恢复错误和停止请求不继续传输。 */
    void errors() {
        AlsaAudioDevice device; QString error; std::atomic_bool stop{false}; VoiceCall::Pcm pcm{};
        QVERIFY(device.open(true,error));
        injectedError=-EPIPE; QVERIFY(device.transfer(pcm,stop)); QCOMPARE(device.xruns(),uint64_t(1));
        injectedError=-ESTRPIPE; QVERIFY(device.transfer(pcm,stop)); QCOMPARE(device.xruns(),uint64_t(2));
        injectedError=-ENODEV; QVERIFY(!device.transfer(pcm,stop));
        stop=true; QVERIFY(!device.transfer(pcm,stop)); device.close(); QCOMPARE(handles,0);
        failParameters=true; QVERIFY(!device.open(true,error)); QCOMPARE(handles,0); failParameters=false;
    }
    /** @brief XRUN后prepare不自动产数据，必须重新start，且不反复启动RUNNING句柄。 */
    void captureRestart() {
        AlsaAudioDevice capture;QString error;std::atomic_bool stop{false};VoiceCall::Pcm pcm{};
        QVERIFY(capture.open(true,error));QCOMPARE(starts,0);QVERIFY(capture.transfer(pcm,stop));QCOMPARE(starts,1);
        QVERIFY(capture.transfer(pcm,stop));QCOMPARE(starts,1);
        injectedError=-EPIPE;QVERIFY(capture.transfer(pcm,stop));QCOMPARE(starts,2);QCOMPARE(capture.xruns(),uint64_t(1));
        for(int i=0;i<320;++i) QCOMPARE(pcm[size_t(i)],int16_t(i*199-32000));
    }
    /** @brief wait的XRUN必须恢复并重启，拔除错误应及时失败，而非等到100ms。 */
    void waitFailures() {
        AlsaAudioDevice capture;QString error;std::atomic_bool stop{false};VoiceCall::Pcm pcm{};
        QVERIFY(capture.open(true,error));injectedError=-EAGAIN;waitError=-EPIPE;
        QVERIFY(capture.transfer(pcm,stop));QCOMPARE(capture.xruns(),uint64_t(1));QCOMPARE(starts,2);
        injectedError=-EAGAIN;waitError=-ENODEV;QVERIFY(!capture.transfer(pcm,stop));
    }
    /** @brief 无数据保留原PCM且等待有界；显式启动失败和取消不发送半帧。 */
    void boundedNoData() {
        AlsaAudioDevice capture;QString error;std::atomic_bool stop{false};VoiceCall::Pcm pcm{};pcm.fill(1234);const auto original=pcm;
        QVERIFY(capture.open(true,error));noData=true;QElapsedTimer elapsed;elapsed.start();
        QVERIFY(!capture.transfer(pcm,stop));QVERIFY(elapsed.elapsed()>=100);QVERIFY(elapsed.elapsed()<500);QVERIFY(pcm==original);
        noData=false;capture.close();QVERIFY(capture.open(true,error));failStart=true;
        QVERIFY(!capture.transfer(pcm,stop));QVERIFY(pcm==original);failStart=false;
        stop=true;QVERIFY(!capture.transfer(pcm,stop));QVERIFY(pcm==original);
    }
    /** @brief 对每个局部配置创建步骤注入失败，验证没有默认混音回退。 */
    void configFailures_data() {
        QTest::addColumn<int>("stage");for(int stage=1;stage<=6;++stage) QTest::newRow(qPrintable(QString::number(stage)))<<stage;
    }
    /** @brief 配置或局部打开失败必须关闭PCM、输入和树，归还全局引用。 */
    void configFailures() {
        QFETCH(int,stage);configFailure=stage;AlsaAudioDevice capture;QString error;
        QVERIFY(!capture.open(true,error));QVERIFY(!error.isEmpty());
        QCOMPARE(handles,0);QCOMPARE(configObjects,0);QCOMPARE(inputObjects,0);QCOMPARE(configReferences,0);
    }
    /** @brief 故障注入逐项验证只读诊断查询失败不影响打开或基础PCM播放。 */
    void diagnosticGeometryFailures_data() {
        QTest::addColumn<int>("stage");for(int stage=0;stage<=5;++stage) QTest::newRow(qPrintable(QString::number(stage)))<<stage;
    }
    /** @brief 几何/阈值查询仅读取一次；所有失败均保留录放且释放软件参数对象。 */
    void diagnosticGeometryFailures() {
        QFETCH(int,stage);geometryFailure=stage;AlsaAudioDevice playback;QString error;
        QVERIFY(playback.open(false,error));QCOMPARE(softwareObjects,0);
        const auto geometry=playback.geometry();
        QCOMPARE(geometry.bufferFrames,int64_t(stage==1 ? -1 : 960));
        QCOMPARE(geometry.startThreshold,int64_t(stage==2 || stage==3 || stage==4 ? -1 : 960));
        QCOMPARE(geometry.availMin,int64_t(stage==2 || stage==3 || stage==5 ? -1 : 320));
        VoiceCall::Pcm pcm{};std::atomic_bool stop{false};QVERIFY(playback.transfer(pcm,stop));QCOMPARE(timingQueries,0);
        QVERIFY(playback.transferTrace().completed);QCOMPARE(playback.transferTrace().queryRc,1);
        playback.close();QCOMPARE(playback.geometry().bufferFrames,int64_t(-1));
    }
    /** @brief 驱动查询失败原因逐分支独立注入，原查询阈值不放宽，关闭AEC不额外查询。 */
    void timingFailureReasons() {
        AlsaAudioDevice device;QString error;VoiceCall::Pcm pcm{};std::atomic_bool stop{false};QVERIFY(device.open(false,error));
        QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::Disabled));
        device.enableTiming(true);QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::Valid));
        failTiming=true;QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::QueryError));failTiming=false;
        deviceState=SND_PCM_STATE_PREPARED;QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::NotRunning));deviceState=SND_PCM_STATE_RUNNING;
        timingAvailable=-1;QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::NegativeAvailable));timingAvailable=640;
        timingDelay=-1;QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::NegativeDelay));
        timingDelay=16001;QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::ExcessDelay));timingDelay=640;
        queryPauseMs=6;querySlowRemaining=2;QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::SlowQuery));QVERIFY(device.timing().querySpanUs>=6000);
        QVERIFY(!device.timing().valid);QVERIFY(device.transfer(pcm,stop));QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::Valid));
    }
    /** @brief 同一录放路径仅首查慢时恢复；连续两次慢时保持无效且不发生第三次查询。 */
    void boundedTimingRetry() {
        for(bool capture:{false,true}) for(int slow:{1,2}) {
            AlsaAudioDevice device;QString error;VoiceCall::Pcm pcm{};std::atomic_bool stop{false};
            QVERIFY(device.open(capture,error));device.enableTiming(true);
            queryPauseMs=6;querySlowRemaining=slow;const auto before=timingQueries;
            QVERIFY(device.transfer(pcm,stop));const auto timing=device.timing();
            QCOMPARE(timingQueries-before,2);QCOMPARE(timing.queryAttempts,2U);
            QVERIFY(timing.firstQuerySpanUs>=6000);QVERIFY(timing.queryTotalUs>=6000);
            QCOMPARE(timing.valid,slow==1);
            QCOMPARE(int(timing.reason),int(slow==1 ? VoiceCall::TimingReason::Valid : VoiceCall::TimingReason::SlowQuery));
            if(slow==1) { QVERIFY(timing.querySpanUs<=2000);QVERIFY(timing.firstSampleUs>0); }
            else { QVERIFY(timing.querySpanUs>=6000);QCOMPARE(timing.firstSampleUs,int64_t(0)); }
            const auto trace=device.transferTrace();QCOMPARE(trace.queryAttempts,2U);QCOMPARE(trace.firstQueryUs,timing.firstQuerySpanUs);
            device.enableTiming(false);const auto disabled=timingQueries;QVERIFY(device.transfer(pcm,stop));QCOMPARE(timingQueries,disabled);
            QCOMPARE(device.transferTrace().queryAttempts,0U);
        }
    }
    /** @brief 首查驱动错误不重查；首查慢后第二次驱动错误仍无效且严格限制两次。 */
    void retryErrorIsBounded() {
        AlsaAudioDevice device;QString error;VoiceCall::Pcm pcm{};std::atomic_bool stop{false};
        QVERIFY(device.open(true,error));device.enableTiming(true);failTiming=true;
        auto before=timingQueries;QVERIFY(device.transfer(pcm,stop));QCOMPARE(timingQueries-before,1);
        QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::QueryError));
        failTiming=false;queryPauseMs=6;querySlowRemaining=1;failQueryAt=timingQueries+2;
        before=timingQueries;QVERIFY(device.transfer(pcm,stop));QCOMPARE(timingQueries-before,2);
        QVERIFY(!device.timing().valid);QCOMPARE(int(device.timing().reason),int(VoiceCall::TimingReason::QueryError));
        QCOMPARE(device.timing().queryAttempts,2U);QCOMPARE(device.timing().firstSampleUs,int64_t(0));
    }
    /** @brief 真实封装记录I/O、wait及时间查询的独立延迟，并保留XRUN前最近查询与写入时刻。 */
    void diagnosticTransferStages() {
        AlsaAudioDevice playback;QString error;std::atomic_bool stop{false};VoiceCall::Pcm pcm{};
        QVERIFY(playback.open(false,error));playback.enableTiming(true);ioPauseMs=8;
        QVERIFY(playback.transfer(pcm,stop));const auto first=playback.transferTrace();
        QVERIFY(first.completed);QCOMPARE(first.frames,320);QCOMPARE(first.ioCalls,4);QCOMPARE(first.partial,3);
        QVERIFY(first.ioMaxUs>=8000);QCOMPARE(first.queryRc,0);QCOMPARE(first.delayFrames,int64_t(640));
        QTest::qSleep(25);injectedError=-EAGAIN;waitError=-EPIPE;waitPauseMs=12;queryPauseMs=6;
        QVERIFY(playback.transfer(pcm,stop));const auto second=playback.transferTrace();
        QCOMPARE(second.sequence,uint64_t(2));QCOMPARE(second.faultCode,-EPIPE);QCOMPARE(second.faultSource,2);
        QCOMPARE(second.xrunsAfter-second.xrunsBefore,uint64_t(1));QCOMPARE(second.recoverRc,0);
        QVERIFY(second.waitMaxUs>=12000);QCOMPARE(second.waitCalls,1);QCOMPARE(second.again,1);
        QVERIFY(second.timingUs>=6000);QCOMPARE(second.previousDelayFrames,int64_t(640));
        QCOMPARE(second.previousQueryUs,first.queryEndUs);QVERIFY(second.faultSinceWriteUs>=25000);
        QVERIFY(second.endUs>=second.faultUs);QVERIFY(second.lastWriteBeforeUs>0);QCOMPARE(second.frames,320);
        injectedError=-EPIPE;QVERIFY(playback.transfer(pcm,stop));QCOMPARE(playback.transferTrace().faultSource,1);
        playback.enableTiming(false);const int previous=timingQueries;
        QVERIFY(playback.transfer(pcm,stop));QCOMPARE(timingQueries,previous);QCOMPARE(playback.transferTrace().queryRc,1);
        QCOMPARE(playback.transferTrace().faultCode,0);QCOMPARE(playback.transferTrace().waitCalls,0);
        injectedError=-ENODEV;QVERIFY(!playback.transfer(pcm,stop));
        QVERIFY(!playback.transferTrace().completed);QCOMPARE(playback.transferTrace().faultCode,-ENODEV);
    }
};
/** @brief 测试子进程amixer替身与实际封装回归主入口，无真实硬件命令。
 * @param[in] argc 命令行数量。
 * @param[in] argv 标准QtTest参数或amixer模拟参数。
 * @return 0成功；非零为夹具/测试失败。
 */
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(QFileInfo(app.applicationFilePath()).baseName()=="amixer") {
        QFile file(qEnvironmentVariable("VOICE_TEST_MIXER_FILE"));const auto args=app.arguments();
        if(args.contains("cget")) { if(!file.open(QIODevice::ReadOnly)) return 2;const auto bytes=file.readAll();fputs("numid=4\n  : values=",stdout);fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);fputs("\n",stdout);return 0; }
        if(args.contains("cset")) { if(!file.open(QIODevice::WriteOnly)) return 2;const auto bytes=args.last().toLocal8Bit();return file.write(bytes)==bytes.size()?0:2; }
        return 2;
    }
    AlsaDeviceTests test;return QTest::qExec(&test,argc,argv);
}
#include "alsa_device_tests.moc"
