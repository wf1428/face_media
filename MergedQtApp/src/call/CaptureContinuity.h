/**
 * @file CaptureContinuity.h
 * @brief 采集线程独占的AEC路径切换平滑；只混合同一采集帧，不延迟或重放语音。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include "CallProtocol.h"
namespace VoiceCall {
/** @brief 为无预处理的参考缺失分支准备DC抑制后备，并在路径切换时衔接PCM。
 * @details 后备DC状态始终用当前原始采样推进。恢复时用同一帧后备/正常输出做20ms线性混合；
 *          丢参考时不伪造AEC参考，首5ms以衰减偏移衔接上一帧末点。不静音、不丢帧、无额外排队。
 *          只在AEC已启用的上下文调用；关闭全部处理时不调用，保持V1逐样本一致。
 */
class CaptureContinuity {
public:
    /** @brief 重新开始通话/采集时间线，清除后备DC和边界状态。 */
    void reset() { input_=dc_=0;have_=full_=false;tail_=0; }
    /** @brief 用当前原始帧推进一阶DC阻断器，供无独立预处理器时使用。
     * @param[in] raw 当前麦克风原始320样本。
     * @param[out] fallback 同一帧DC阻断输出，系数0.995，不改变正常AEC路径。
     */
    void prepare(const Pcm &raw,Pcm &fallback) {
        for(size_t i=0;i<raw.size();++i) { const double value=raw[i]-input_+0.995*dc_;input_=raw[i];dc_=value;fallback[i]=sample(value); }
    }
    /** @brief 衔接正常AEC和后备输出，仅路径变化时修改PCM。
     * @param[in,out] pcm 当前所选路径的同一帧音频。
     * @param[in] full true为正常AEC输出，false为缺参考后备。
     * @param[in] fallback 恢复时同一帧后备音频，不能是历史帧；其他情况允许空。
     * @return true发生了路径切换平滑，false未改当前路径采样。
     */
    bool finish(Pcm &pcm,bool full,const Pcm *fallback) {
        const bool changed=have_ && full_!=full;
        if(changed) {
            if(full && fallback) for(size_t i=0;i<pcm.size();++i) {
                const double weight=double(i)/double(pcm.size()-1);
                pcm[i]=sample((*fallback)[i]*(1-weight)+pcm[i]*weight);
            }
            const int offset=tail_-int(pcm.front());
            for(int i=0;i<80;++i) pcm[size_t(i)]=sample(pcm[size_t(i)]+double(offset)*(80-i)/80);
        }
        tail_=pcm.back();full_=full;have_=true;return changed;
    }
private:
    /** @brief 四舍五入并限制到S16，不发生整数回绕。
     * @param[in] value 中间幅度。
     * @return 有效S16采样。
     */
    static int16_t sample(double value) { return int16_t(std::max(-32768.0,std::min(32767.0,std::round(value)))); }
    double input_=0,dc_=0;int tail_=0;bool have_=false,full_=false; /**< 仅本采集线程持有的DC及路径边界状态。 */
};
}
