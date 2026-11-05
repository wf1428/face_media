/**
 * @file CallProtocol.h
 * @brief 定义与 UI、ALSA 无关的 V1 PCM 帧、编解码和有界抖动队列。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；更新本地processingMode的AEC旁路语义，bit2可与bit1组合表示后备预处理；本地字段不序列化，MQTT650B帧不变。
 */
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>

namespace VoiceCall {
constexpr int Samples = 320; /**< 16kHz 单声道的 20ms 采样数。 */
constexpr int FrameBytes = 650; /**< 10B 帧头加 640B PCM，MQTT 不使用文本编码。 */
using Pcm = std::array<int16_t, Samples>;
using Packet = std::array<uint8_t, FrameBytes>;

/** @brief 已校验下行 PCM 的只读电平快照；不受播放音量、功放或静音状态影响。 */
struct ReceivedAudioLevel {
    uint64_t frames=0; /**< 当前通话已接收的有效帧数，0 表示尚无有效音频。 */
    int peak=0,rms=0; /**< 最近 200ms 的峰值和均方根幅度，满幅为 32768。 */
    int64_t ageMs=-1; /**< 最近有效帧距今的本地单调毫秒；-1 表示无帧或未通话。 */
};

/** @brief 本通话 PCM 电平统计，不输出原始录音或修改采样。 */
struct LevelStats {
    uint64_t samples=0,clipped=0; /**< 采样/接近满幅采样数，满幅阈值 32760。 */
    int peak=0; /**< 峰值绝对幅度，范围 0..32768。 */
    int64_t sum=0; /**< 有符号采样和，用于识别 DC 偏置。 */
    double squares=0; /**< 采样平方和，用于计算 RMS。 */
    /** @brief 累计一帧，调用方负责跨线程互斥。
     * @param[in] pcm 本机字节序的单声道 320 样本。
     */
    void add(const Pcm &pcm) {
        for (int16_t sample:pcm) {
            const int value=sample,magnitude=std::abs(value);
            ++samples;sum+=value;squares+=double(value)*value;peak=std::max(peak,magnitude);
            if (magnitude>=32760) ++clipped;
        }
    }
    /** @return 整段 PCM 均方根幅度，尚无采样时为 0。 */
    int rms() const { return samples ? int(std::sqrt(squares/double(samples))) : 0; }
    /** @return 平均直流偏置，尚无采样时为 0。 */
    int dc() const { return samples ? int(sum/int64_t(samples)) : 0; }
};

/** @brief 一帧单声道音频；本地时间与远端时间分开，禁止假设两端时钟同步。 */
struct Frame {
    Pcm pcm{}; /**< 本机字节序的有符号采样；序列化时转为小端。 */
    uint8_t seq = 0; /**< 远端帧序号，按 256 回绕。 */
    uint32_t timestamp = 0; /**< 远端发送毫秒低 32 位，按 2^32 回绕。 */
    uint64_t captureIndex=0; /**< 本地采集序号，仅诊断；不序列化，非TALKING采集也推进。 */
    uint8_t processingMode=0; /**< 本地诊断bit0=AEC已运行、bit1=预处理已运行、bit2=AEC/整处理旁路（可同时有bit1后备预处理）；不写入协议。 */
    int64_t localMs = 0; /**< 接收/采集/实际播放估计时刻，本地单调时钟毫秒。 */
};

/** @brief 编码固定 650B 帧。
 * @param[in] frame 音频及seq；localMs、captureIndex、processingMode仅本地，不写入协议。
 * @param[in] sendMs 发送时刻毫秒低 32 位，写为大端。
 * @return 含小端 PCM 的 MQTT 二进制包。
 */
inline Packet encode(const Frame &frame, uint32_t sendMs)
{
    Packet out{};
    out[0]=0xA1; out[1]=0xC0; out[2]=1; out[3]=0; out[4]=1; out[5]=frame.seq;
    for (int i=0;i<4;++i) out[6+i]=uint8_t(sendMs >> (24-8*i));
    for (int i=0;i<Samples;++i) {
        const uint16_t value = uint16_t(frame.pcm[i]);
        out[10+2*i]=uint8_t(value); out[11+2*i]=uint8_t(value >> 8);
    }
    return out;
}

/** @brief 严格校验长度、magic、版本并解码；失败不改动输出。
 * @param[in] bytes 可为空的包指针。
 * @param[in] size 字节数，V1 只接受 650。
 * @param[in] arrivalMs 本地接收单调时钟毫秒。
 * @param[out] out 成功时写入完整帧。
 * @return true 表示有效 V1 帧；false 表示格式错误。
 */
inline bool decode(const uint8_t *bytes, int size, int64_t arrivalMs, Frame &out)
{
    if (!bytes || size!=FrameBytes || bytes[0]!=0xA1 || bytes[1]!=0xC0 ||
        bytes[2]!=1 || bytes[3]!=0 || bytes[4]!=1) return false;
    Frame frame;
    frame.seq=bytes[5]; frame.localMs=arrivalMs;
    for (int i=0;i<4;++i) frame.timestamp=(frame.timestamp << 8)|bytes[6+i];
    for (int i=0;i<Samples;++i) {
        const uint16_t value = uint16_t(bytes[10+2*i])|(uint16_t(bytes[11+2*i]) << 8);
        frame.pcm[i] = value < 32768 ? int16_t(value) : int16_t(int(value)-65536);
    }
    out=frame;
    return true;
}

/** @brief 限长抖动缓冲；由 AudioEngine 外部互斥保护，不自行加锁。
 * @details 按远端时间戳模差排序，seq 去重；不比较远端时间与本地墙钟。
 *          默认预缓冲 4 帧，容量 10 帧，200ms 本地过期；欠载返回静音；连续 4 帧欠载才重新预缓冲，避免把单次 20ms 缺口放大。
 */
class JitterBuffer {
public:
    /** @brief 清空全部数据和排序基线；新通话必须调用。 */
    void clear() { count_=0; primed_=false; played_=false; consecutiveUnderruns_=0; }
    /** @brief 插入有效帧，拒绝重复和已播放旧帧，溢出丢最旧。
     * @param[in] frame 已解码且带本地接收时刻的帧。
     * @return true 表示已接收；false 表示重复或迟到。
     */
    bool push(const Frame &frame) {
        if (played_ && delta(frame.timestamp,lastTs_)<=0) { ++dropped; return false; }
        for (size_t i=0;i<count_;++i) if (frames_[i].seq==frame.seq && frames_[i].timestamp==frame.timestamp) {
            ++dropped; return false;
        }
        size_t at=0;while(at<count_ && delta(frame.timestamp,frames_[at].timestamp)>=0) ++at;
        for(size_t i=count_;i>at;--i) frames_[i]=frames_[i-1];
        frames_[at]=frame;++count_;
        if(count_>10) { popFront();++dropped; }
        highWater=std::max(highWater,count_);
        return true;
    }
    /** @brief 每个播放周期取一帧，欠载或预缓冲时输出静音。
     * @param[in] nowMs 本地单调时钟毫秒。
     * @return PCM 及元数据；静音帧的 localMs 为 nowMs。
     */
    Frame take(int64_t nowMs) {
        while (count_!=0 && nowMs-frames_[0].localMs>200) {
            popFront(); ++dropped;
        }
        if (!primed_ && count_!=0 &&
            (count_>=4 || nowMs-frames_[0].localMs>=80)) primed_=true;
        if (!primed_ || count_==0) {
            if (primed_) ++underruns;
            if (primed_ && ++consecutiveUnderruns_>=4) primed_=false;
            Frame silence; silence.localMs=nowMs; return silence;
        }
        consecutiveUnderruns_=0;
        Frame frame=frames_[0]; popFront();
        played_=true; lastTs_=frame.timestamp; return frame;
    }
    /** @return 当前待消费帧数；调用方须沿用队列互斥，不执行清理、预缓冲或消费。 */
    size_t pendingFrames() const { return count_; }
    uint64_t dropped=0; /**< 重复、迟到、过期及溢出丢帧总数。 */
    uint64_t underruns=0; /**< 已开始播放后的欠载次数。 */
    size_t highWater=0; /**< 本通话接收队列最高深度，单位帧。 */
private:
    /** @brief 得到 32 位时间戳模差，支持回绕，要求间隔小于 2^31 ms。
     * @param[in] a 待比较的发送毫秒低 32 位。
     * @param[in] b 排序基线的发送毫秒低 32 位。
     * @return 有符号模差；正值表示 a 更新，负值表示 a 更旧。
     */
    static int64_t delta(uint32_t a,uint32_t b) {
        const uint32_t d=a-b; return d<=0x7fffffffU ? int64_t(d) : int64_t(d)-0x100000000LL;
    }
    /** @brief 非空时移走队首；最多移动10帧，固定内存且不调用分配器。 */
    void popFront() { for(size_t i=1;i<count_;++i) frames_[i-1]=frames_[i];--count_; }
    std::array<Frame,11> frames_{}; /**< 10帧容量加一个溢出插入槽，外部短锁保护。 */
    size_t count_=0; /**< 当前有效帧数，公开操作返回时最多10。 */
    int consecutiveUnderruns_=0; /**< 连续欠载计数；短缺口不重做预缓冲，连续 4 帧欠载才重新缓冲。 */
    bool primed_=false; /**< true 表示已完成首次/欠载后的预缓冲。 */
    bool played_=false; /**< true 表示 lastTs_ 为当前通话的有效播放基线。 */
    uint32_t lastTs_=0; /**< 已消费的最后远端时间戳；clear 后失效。 */
};
} // namespace VoiceCall
