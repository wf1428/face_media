/**
 * @file SpeexDsp.h
 * @brief 通过原IAec/IPreprocessor接口封装可选SpeexDSP运行库与RAII状态。
 * @author Du lin
 * @note 最近修改：2026-11-04；修改人：Du lin；新增预处理当前AGC增益、感知响度及讲话概率只读GET观测；不支持只标未知，不调整参数或降级处理。
 * @date 2026-10-31
 */
#pragma once
#include "AudioProcessing.h"
struct SpeexDspApi;
class SpeexPreprocessor;
/** @brief 仅采集线程使用的Speex AEC；具体ABI和状态只出现在实现文件。 */
class SpeexAec final : public IAec {
public:
    /** @brief 加载运行库并建立320样本、16kHz的AEC。
     * @param[in] config 配置快照，尾长单位ms。
     * @param[out] error 失败原因。
     * @return 有效实例；缺库/初始化失败时为空。
     */
    static std::unique_ptr<SpeexAec> create(const VoiceCall::ProcessingConfig &config,QString &error);
    /** @brief 销毁回声状态；关联预处理器必须先释放。 */
    ~SpeexAec() override;
    /** @brief 原生状态独占所有权，不允许复制。 */
    SpeexAec(const SpeexAec &)=delete;
    /** @brief 禁止复制赋值，避免同一原生状态被重复释放。 */
    SpeexAec &operator=(const SpeexAec &)=delete;
    /** @brief 清除自适应滤波历史，不改变帧长/采样率。 */
    void reset() override;
    /** @brief 使用显式对齐参考执行echo_cancellation，固定暂存保证允许输入/输出同址。
     * @param[in] mic 麦克风320点。
     * @param[in] ref 实际播放参考320点。
     * @param[out] out 近端输出，可与mic同址。
     * @param[in] samples 只能为320；无效输入不调用库。
     */
    void process(const int16_t *mic,const int16_t *ref,int16_t *out,int samples) override;
private:
    /** @brief 仅静态工厂使用的空状态构造，不加载库。 */
    SpeexAec()=default;
    friend class SpeexPreprocessor;
    std::shared_ptr<SpeexDspApi> api_; /**< 共享的运行库引用；状态销毁后才释放。 */
    void *state_=nullptr; /**< 本实例拥有的SpeexEchoState，禁止跨线程访问。 */
    VoiceCall::Pcm scratch_{}; /**< 固定输出暂存，逐帧不分配。 */
};
/** @brief 同一采集线程的可选Speex预处理器；VAD固定关闭，绝不丢帧或半双工。 */
class SpeexPreprocessor final : public IPreprocessor {
public:
    /** @brief 创建独立预处理器，存在AEC时复用其库并关联同一状态。
     * @param[in] config 已限制范围的抑制/AGC配置。
     * @param[in] aec 借用关联的AEC，可空；必须比本实例长寿。
     * @param[out] error 失败原因。
     * @return 有效实例；失败为空。
     */
    static std::unique_ptr<SpeexPreprocessor> create(const VoiceCall::ProcessingConfig &config,SpeexAec *aec,QString &error);
    /** @brief 释放预处理状态，不拥有AEC。 */
    ~SpeexPreprocessor() override;
    /** @brief 原生状态独占所有权，不允许复制。 */
    SpeexPreprocessor(const SpeexPreprocessor &)=delete;
    /** @brief 禁止复制赋值。 */
    SpeexPreprocessor &operator=(const SpeexPreprocessor &)=delete;
    /** @brief 稀有重同步时重建预处理历史并重设参数；失败后valid为false。 */
    void reset() override;
    /** @brief 原位处理，忽略VAD返回值，不截断话音。
     * @param[in,out] inOut 320点可写PCM。
     * @param[in] samples 必须为320。
     */
    void process(int16_t *inOut,int samples) override;
    /** @return 状态创建和全部控制参数是否成功。 */
    bool valid() const { return state_!=nullptr; }
    /** @return 同线程读取原生GET_AGC_GAIN/LOUDNESS/PROB；仅已启用AGC才查增益，不支持保持未知。 */
    VoiceCall::PreprocessorObservation observation() const;
private:
    /** @brief 仅静态工厂使用的空状态构造。 */
    SpeexPreprocessor()=default;
    /** @brief 创建状态并应用参数，控制请求失败立即销毁。
     * @return true全部成功；false为库拒绝参数或分配失败。
     */
    bool initialize();
    std::shared_ptr<SpeexDspApi> api_; /**< 本实例持有的库引用。 */
    void *state_=nullptr; /**< 本实例拥有的预处理状态。 */
    SpeexAec *aec_=nullptr; /**< 同线程借用，不销毁。 */
    VoiceCall::ProcessingConfig config_; /**< 本通话的有效参数。 */
};
