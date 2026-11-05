/**
 * @file PcmDiagnostics.h
 * @brief 只读统计PCM各处理阶段的幅度、削顶、峰顶平坦、跳变与校验摘要。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include "CallProtocol.h"
#include <limits>

namespace VoiceCall {
/** @brief 单帧320点的波形证据；固定存储、不分配、不修改PCM，不将指标单独判为失真。 */
struct PcmShape {
    int rms=0,acRms=0,dc=0,peak=0,minimum=0,maximum=0; /**< 幅度单位为PCM16整数；acRms扣除均值。 */
    int clipped=0,rails=0,flatPeak=0,flatRun=0,adjacentStep=0,boundaryStep=0; /**< 近满幅/精确轨顶/峰顶平坦采样数、连续平坦长度和采样差。 */
    int first=0,last=0;uint32_t hash=2166136261U; /**< 首尾值和逐字节小端FNV-1a；不是密码学校验或协议字段。 */
};
/** @brief 单线程波形观察器，记录相邻块边界，不在声音路径加入滤波/限幅。 */
class PcmShapeTracker {
public:
    /** @brief 清除跨帧边界；仅在原DSP重置时调用，不清除音频处理状态。 */
    void reset() { havePrevious_=false;previous_=0; }
    /** @brief 观察一帧；全零/低幅量化平台不算峰顶平坦。
     * @param[in] pcm 借用本机字节序mono320点；返回前不修改或保存外部指针。
     * @return 幅度及连续性证据；flat只表示幅度>=4096、位于本帧峰顶约2%内且相邻差<=4的平坦线索。
     * @note 模拟饱和可低于数字满幅，平坦指标也可能由正常低频信号产生，必须结合短录音判断。
     */
    PcmShape observe(const Pcm &pcm) {
        PcmShape out;out.first=pcm.front();out.last=pcm.back();out.minimum=out.maximum=pcm.front();
        int64_t sum=0;double squares=0;int previous=pcm.front();
        for(const auto sample:pcm) {
            const int value=sample,magnitude=std::abs(value);sum+=value;squares+=double(value)*value;
            out.peak=std::max(out.peak,magnitude);out.minimum=std::min(out.minimum,value);out.maximum=std::max(out.maximum,value);
            out.clipped+=magnitude>=32760;out.rails+=value==32767 || value==-32768;
            out.adjacentStep=std::max(out.adjacentStep,std::abs(value-previous));previous=value;
            const auto wire=uint16_t(sample);out.hash=(out.hash^uint8_t(wire))*16777619U;out.hash=(out.hash^uint8_t(wire>>8))*16777619U;
        }
        const double mean=double(sum)/Samples;out.dc=int(sum/Samples);out.rms=int(std::sqrt(squares/Samples));
        out.acRms=int(std::sqrt(std::max(0.0,squares/Samples-mean*mean)));
        int run=0;previous=pcm.front();
        for(int i=1;i<Samples;++i) {
            const int value=pcm[size_t(i)];
            const bool flat=out.peak>=4096 && std::abs(value)*100>=out.peak*98 && std::abs(previous)*100>=out.peak*98 &&
                ((value>=0)==(previous>=0)) && std::abs(value-previous)<=4;
            run=flat ? run+1 : 0;if(flat) ++out.flatPeak;out.flatRun=std::max(out.flatRun,run ? run+1 : 0);previous=value;
        }
        if(havePrevious_) out.boundaryStep=std::abs(out.first-previous_);
        previous_=out.last;havePrevious_=true;return out;
    }
private:
    int previous_=0;bool havePrevious_=false; /**< 所属处理线程独占的上一帧尾采样及有效标志。 */
};
/** @brief Speex预处理的只读GET结果；不支持时保留未知，不导致DSP降级。 */
struct PreprocessorObservation {
    bool gainValid=false;int gainDb=0,loudness=-1,speechProbability=-1; /**< 原生当前AGC增益dB、感知响度及讲话概率0..100；gainValid=false时gainDb无效。 */
};
/** @brief 一次采集处理的纯数值阶段快照，仅所属采集线程写、交接后诊断线程读副本。 */
struct ProcessingObservation {
    PcmShape raw,aec,preprocess,output;PreprocessorObservation pre; /**< 输入、AEC后、预处理后、最终输出和选用路径的预处理GET结果。 */
    unsigned valid=0;bool fallback=false; /**< bit0输入、bit1AEC后、bit2预处理后、bit3输出；fallback=true表示选用后备预处理。 */
};
/** @brief 按需短录音的固定PCM副本；正常未启用时不填充这些副本。 */
struct ProcessingPcm {
    Pcm aec{},preprocess{};unsigned valid=0; /**< bit1/bit2有效性与ProcessingObservation一致，不以零值冒充有效阶段。 */
};
/** @brief 帧从采集到序列化的纯字节摘要，不申请内存。
 * @param[in] pcm 最终发送PCM320点。
 * @return 与PcmShape.hash相同的小端PCM FNV-1a摘要，不包含协议头。
 */
inline uint32_t pcmHash(const Pcm &pcm) { uint32_t hash=2166136261U;for(auto value:pcm) { const auto v=uint16_t(value);hash=(hash^uint8_t(v))*16777619U;hash=(hash^uint8_t(v>>8))*16777619U; }return hash; }
}
