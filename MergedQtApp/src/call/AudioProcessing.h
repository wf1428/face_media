/**
 * @file AudioProcessing.h
 * @brief 定义兼容V1的采集处理接口、配置快照及串行SpeexDSP处理上下文。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；增加只读分阶段波形/AGC观测及按需PCM快照，保留原process接口、DSP参数、后备/平滑和逐样本行为。
 */
#pragma once
#include <cstdint>
#include <chrono>
#include <ctime>
#include <memory>
#include <QString>
#include "CallProtocol.h"
#include "PcmDiagnostics.h"

/** @brief 仅由采集线程使用的可选回声消除器；关闭DSP时不创建实例。 */
class IAec {
public:
    /** @brief 释放实现自有资源。 */
    virtual ~IAec() = default;
    /** @brief 新通话或时序失步时清除滤波器历史。 */
    virtual void reset() = 0;
    /** @brief 处理一帧，调用方须先完成麦克风与实际播放参考的时序对齐。
     * @param[in] mic 16kHz 单声道麦克风采样。
     * @param[in] ref 同格式且完成延迟对齐的实际播放采样。
     * @param[out] out 输出采样，可与 mic 指向同一缓冲。
     * @param[in] samples 样本数，当前固定为 320。
     */
    virtual void process(const int16_t *mic,const int16_t *ref,int16_t *out,int samples)=0;
};
/** @brief 仅由采集线程使用的可选预处理器，与AEC独立；关闭时旁路。 */
class IPreprocessor {
public:
    /** @brief 释放实现自有资源。 */
    virtual ~IPreprocessor() = default;
    /** @brief 新通话或时序失步时清除处理历史。 */
    virtual void reset()=0;
    /** @brief 原位处理 16kHz 单声道 PCM。
     * @param[in,out] inOut 可写采样缓冲。
     * @param[in] samples 样本数，当前固定为 320。
     */
    virtual void process(int16_t *inOut,int samples)=0;
};

namespace VoiceCall {
/** @return 本地单调微秒；Linux使用CLOCK_MONOTONIC，与现有媒体队列计时同源。 */
inline int64_t monotonicUs() {
#ifdef Q_OS_LINUX
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC,&time)==0) return int64_t(time.tv_sec)*1000000+time.tv_nsec/1000;
#endif
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/** @brief 每通电话读取一次的VoiceCallDsp配置，范围约束在load中统一处理。 */
struct ProcessingConfig {
    bool aecEnabled=true; /**< 默认开启AEC；缺库/失步自动旁路。 */
    bool preprocessEnabled=true; /**< 默认开启预处理；false时降噪、残余抑制和AGC均不运行。 */
    bool denoiseEnabled=true; /**< 仅预处理开启时启用降噪。 */
    bool agcEnabled=true; /**< 默认开启自动增益；仅预处理开启时运行。 */
    int filterLengthMs=200; /**< AEC滤波尾长，40~500ms；不是播放缓冲。 */
    int referenceDelayMs=0; /**< 参考读取偏移，-100~300ms；正值取更早的播放样本。 */
    int alignmentToleranceMs=5; /**< 播放锚点连续性容差，1~20ms，超限清历史。 */
    int referenceHistoryFrames=32; /**< 固定环使用的历史上限，8~64帧，每帧20ms。 */
    int noiseSuppressDb=-10; /**< 降噪衰减下限，-30~0dB。 */
    int echoSuppressDb=-40; /**< 非近端讲话时残余回声衰减，-40~0dB。 */
    int echoSuppressActiveDb=-12; /**< 近端讲话残余抑制，-12~0dB，且不强于非近端值。 */
    int agcTarget=8000; /**< AGC感知响度目标，1000~16000；不是固定输出RMS或硬件音量。 */
    int agcMaxGainDb=6; /**< 可选AGC最大增益，0~12dB。 */
    int agcIncrementDbPerSec=6; /**< 可选AGC增益上升速率，1~12dB/s。 */
    int agcDecrementDbPerSec=-12; /**< 可选AGC下降速率，-30~-1dB/s；Speex接口要求负值。 */
    QString libraryPath; /**< 仅供程序内测试注入库；INI不读取此值，生产保持空值并搜索系统库。 */
    /** @brief 从现有INI的VoiceCallDsp组读取配置；不写文件，不改变其他配置。
     * @param[in] path net_cfg.ini路径；空/缺失时使用上述默认值。
     * @param[out] warnings 非空时接收无效/限幅参数说明，不包含其他组或密码。
     * @return 本通电话的有效参数快照；修改INI在下一通生效。
     */
    static ProcessingConfig load(const QString &path,QString *warnings=nullptr);
};
/** @brief 小型DSP诊断；只在采集线程更新，跨线程读取时由引擎复制并加锁。 */
struct ProcessingStats {
    uint64_t aecFrames=0,preprocessFrames=0; /**< 实际AEC帧数及当前选用路径的预处理帧数；未选用的后备热处理不计preprocessFrames，耗时仍计入maxProcessingUs。 */
    uint64_t bypassFrames=0,referenceMisses=0; /**< AEC/整处理旁路帧数及缺参考帧数；有后备预处理时不再意味着原PCM直通。 */
    uint64_t fallbackFrames=0,transitionFrames=0; /**< 实际选择后备输出和路径切换平滑的帧数；不改变帧数量。 */
    uint64_t resets=0,failures=0; /**< 历史重置次数及导致降级的故障次数。 */
    int64_t maxProcessingUs=0; /**< 单帧处理/重同步最大耗时，单调微秒。 */
};
/** @brief 每通话一个、只由采集线程使用的AEC→预处理上下文。
 * @details RAII销毁顺序为预处理器先于AEC；参考丢失时绕过AEC、保持独立后备降噪/AGC，失步只清AEC历史，
 *          全部关闭或库不可用时保留原PCM，不门控、不引入额外采集帧等待。
 */
class AudioProcessing {
public:
    /** @brief 在采集线程创建可选处理器；初始化失败降级，不向UI抛异常。
     * @param[in] config 已验证参数快照。
     * @param[out] information 初始化、降级及有效配置摘要。
     */
    AudioProcessing(const ProcessingConfig &config,QString &information);
    /** @brief 在所属采集线程释放Speex状态和库引用。 */
    ~AudioProcessing();
    /** @return true表示有已创建的处理器。 */
    bool active() const;
    /** @return true表示AEC有效，需要采集/播放的ALSA时间信息。 */
    bool needsReference() const;
    /** @brief 原位串行处理，AEC先于预处理；缺参考使用同帧后备并平滑恢复，全部禁用保持原PCM。
     * @param[in,out] mic 16kHz mono、320样本。
     * @param[in] reference 同时间线的实际播放320样本；缺失传nullptr，不以零帧伪造。
     * @param[in] generation 播放历史代次；变化时清DSP历史。
     */
    void process(Pcm &mic,const Pcm *reference,uint64_t generation);
    /** @brief 采集XRUN/进入通话时清AEC/主预处理及边界历史；后备预处理保持热状态，失败降级直通。
     * @param[in] reason 不含PCM的重同步原因。
     */
    void reset(const QString &reason);
    /** @return 当前计数与耗时，仅所属采集线程调用。 */
    ProcessingStats stats() const;
    /** @return 最近一帧的纯数值阶段观测，所属采集线程调用，不修改Speex状态。 */
    ProcessingObservation observation() const;
    /** @brief 仅开启固定PCM副本，供按需本地短录音，不改变采样或协议。
     * @param[in] enabled true才复制AEC/预处理中间PCM；在采集线程设置。
     */
    void enablePcmObservation(bool enabled);
    /** @return 最近一帧已启用的中间PCM副本及有效位；关闭时valid=0，所属线程调用。 */
    ProcessingPcm pcmObservation() const;
    /** @return 一次性重置/失败说明；正常逐帧路径返回空，不输出原始录音。 */
    QString takeNotice();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_; /**< 在采集线程创建、处理和销毁，UI/MQTT不访问Speex状态。 */
};
}
