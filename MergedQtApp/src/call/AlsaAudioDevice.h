/**
 * @file AlsaAudioDevice.h
 * @brief 封装RK809的Channel 0单声道copy采集、双声道播放及非阻塞通话路由。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；通话采集打开前将硬件Capture Volume逐通道限制到240，保留更低值及原环境变量覆盖；关闭/打开失败按占用值校验恢复原增益，不改DSP和Channel 0。
 */
#pragma once
#include "CallProtocol.h"
#include "PlaybackReference.h"
#include "AudioDiagnostics.h"
#include <QString>
#include <atomic>
struct _snd_pcm;
using snd_pcm_t = _snd_pcm;
struct _snd_config;
using snd_config_t = _snd_config;

/** @brief 一个线程独占的 ALSA 句柄；禁止同时对同一实例读写/关闭。 */
class AlsaAudioDevice {
public:
    /** @brief 创建未打开设备；不占用声卡。 */
    AlsaAudioDevice()=default;
    /** @brief drop 后关闭设备，不等待音频播放完。 */
    ~AlsaAudioDevice();
    /** @brief 打开非阻塞 16kHz S16_LE 设备。
     * @param[in] capture true为局部copy/0→0映射的mono采集，false为原stereo播放。
     * @param[out] error 打开/参数设置失败原因。
     * @return true 已就绪；false 时关闭部分创建的句柄。
     * @note 只在音频后台线程调用；设备名可用环境变量覆盖，默认plughw:0,0。
     *       采集打开前后台保存Capture Volume并应用默认240上限，失败/关闭恢复本次仍拥有的硬件值。
     *       默认plughw局部强制copy及仅0→0传输表，不改全局配置；不能配置时失败，不回退混音。
     *       自定义采集PCM环境变量仍有效；其额外路由由该PCM定义负责。
     *       打开后只读查询几何/软件阈值供诊断；该查询失败不改变原打开结果。
     */
    bool open(bool capture,QString &error);
    /** @brief 读/写恰好一帧，支持部分传输；PREPARED采集显式启动，恢复后同样重新启动。
     * @param[in,out] pcm 采集成功仅填充Channel 0的mono PCM，失败保持原值；播放复制左右声道，失败前可能已写出部分采样。
     * @param[in] stop 跨线程原子停止标志。
     * @return true 一帧传输完成；false 为取消、超时或不可恢复错误。
     * @note 只允许设备所属线程调用；每次等待最多10ms，整帧最长100ms。wait错误按I/O错误处理；停止不输出超时日志。
     *       XRUN恢复只在固定快照中累计阶段耗时，不做文本输出；由调用者读取transferTrace异步诊断。
     *       时间查询首次仅因跨度超过2000us失效时最多立即重查一次；第二次失败仍保持valid=false。
     *       关闭AEC时不为诊断新增逐帧avail_delay查询。
     */
    bool transfer(VoiceCall::Pcm &pcm,const std::atomic_bool &stop);
    /** @brief 按需开启时间元数据；关闭时不新增ALSA时间查询，不改变当前录放声道规则。
     * @param[in] enabled true仅为AEC采样时间；在设备所属线程调用。
     */
    void enableTiming(bool enabled) { if (timingEnabled_!=enabled) lastTiming_={};timingEnabled_=enabled; }
    /** @return 最近一次传输的时间估计；失效时valid=false并有reason，所属线程读取；未启用为Disabled。 */
    VoiceCall::AudioTiming timing() const { return lastTiming_; }
    /** @brief 复制实际双声道写入缓冲的左声道参考；本机播放L/R相同。
     * @param[out] pcm 成功时写入最终提交的320点，包括静音；失败不修改。
     * @param[out] timing 成功时写入有效首样本时间估计。
     * @return true为已完整提交且计时有效；false不可用于AEC。
     */
    bool playbackReference(VoiceCall::Pcm &pcm,VoiceCall::AudioTiming &timing) const {
        if (capture_ || !timingEnabled_ || !lastTiming_.valid) return false;
        pcm=lastPlaybackPcm_;timing=lastTiming_;return true;
    }
    /** @return 声卡排队采样估计延时，单位毫秒；获取失败返回 0。 */
    int delayMs() const;
    /** @brief 后台关闭句柄、恢复本次采集增益并清除几何/单帧诊断；幂等，不清零XRUN计数。 */
    void close();
    /** @return 本实例进入XRUN/挂起恢复分支的次数，包含prepare失败，非网络丢帧数。 */
    uint64_t xruns() const { return xruns_; }
    /** @return 本次open的应用PCM缓冲/阈值快照；未知字段为-1，仅所属线程读取。 */
    const VoiceCall::AlsaGeometry &geometry() const { return geometry_; }
    /** @return 最近一次transfer快照，包括失败/取消；仅所属线程读取，不做ALSA调用。 */
    const VoiceCall::AlsaTransferTrace &transferTrace() const { return transferTrace_; }
    /** @brief 在后台设置SPK_HP、Main Mic，并校验原采集增益覆盖参数。
     * @param[out] error 路由或录音增益设置失败原因。
     * @note QT_YCEST_CALL_CAPTURE_VOLUME仍接受0..255；实际增益由采集实例open保存/设置/恢复。
     *       未覆盖时逐通道上限240，保留更低硬件值，不更改播放音量或DSP参数。
     * @return true 路由及可选增益设置成功；false 表示设备不可用于本次通话。
     */
    static bool configureRoutes(QString &error);
private:
    /** @brief 后台保存原硬件采集增益并应用240上限/显式覆盖，失败交由close恢复。
     * @param[out] error 读取/校验/设置失败原因。
     * @return true增益可用于通话；false不进入录音。
     */
    bool prepareCaptureGain(QString &error);
    /** @brief 当前读回值等于本次写入值时恢复原增益；不同值视为外部更改并保留。
     * @note 只在设备后台close调用，amixer等待有界；读取或恢复失败不抛异常，也不阻止PCM资源释放。
     *       读取/写入非原子，无法识别相同值的外部写入；本实例不宣称持有全系统混音器锁。
     */
    void restoreCaptureGain();
    QString originalCaptureGain_,appliedCaptureGain_; /**< 本实例拥有的原值/实际写入值CSV，空串表示没有待恢复的增益租约。 */
    snd_pcm_t *pcm_=nullptr; /**< 本实例拥有，仅所属音频线程释放。 */
    snd_config_t *captureConfig_=nullptr; /**< 本实例拥有的ALSA局部配置树；先关PCM再删除，不持有全局树引用。 */
    bool capture_=false; /**< true请求mono并由局部plughw copy只路由Channel 0；false复制左右声道。 */
    uint64_t xruns_=0; /**< EPIPE/ESTRPIPE恢复分支累计次数，在prepare前递增，含恢复失败。 */
    bool timingEnabled_=false; /**< 默认false保留V1；仅AEC有效时开启元数据查询。 */
    VoiceCall::AudioTiming lastTiming_; /**< 本次完整读写后的时间，失败/关闭时失效。 */
    VoiceCall::Pcm lastPlaybackPcm_{}; /**< 由实际交错输出缓冲解码的左声道，只在计时启用时更新。 */
    VoiceCall::AlsaGeometry geometry_; /**< 打开时只读查询，不把查询失败当录放失败。 */
    VoiceCall::AlsaTransferTrace transferTrace_; /**< 固定单帧诊断，不跨线程共享。 */
    uint64_t transferSequence_=0;int64_t lastIoSuccessUs_=0; /**< 本次open调用序号及最近成功I/O单调微秒；仅所属线程访问。 */
};
