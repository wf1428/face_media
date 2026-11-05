/**
 * @file SpeexDsp.cpp
 * @brief 动态加载开源SpeexDSP并封装单线程AEC与温和预处理，缺库可安全旁路。
 * @author Du lin
 * @note 最近修改：2026-11-04；修改人：Du lin；预处理新增只读AGC增益/响度/讲话概率诊断，不改变SET参数、处理次序、异常和PCM输出。
 * @date 2026-10-31
 */
#include "SpeexDsp.h"
#include <QLibrary>
#include <QDir>
#include "third_party/speexdsp/include/speex/speex_echo.h"
#include "third_party/speexdsp/include/speex/speex_preprocess.h"

static_assert(sizeof(spx_int16_t)==sizeof(int16_t),"SpeexDSP PCM16 ABI mismatch");
/** @brief 同一采集上下文共享的库引用及官方C函数指针；不混用异步echo API。
 * @details 函数地址借用library；加载期间可空，全部符号通过检查后才发布。
 *          所有调用及最后一次引用释放均在采集线程，卸载后不保存这些地址。
 */
struct SpeexDspApi {
    QLibrary library; /**< 最后一个状态引用释放后才允许卸载。 */
    decltype(&speex_echo_state_init) echoInit=nullptr;
    decltype(&speex_echo_state_destroy) echoDestroy=nullptr;
    decltype(&speex_echo_state_reset) echoReset=nullptr;
    decltype(&speex_echo_ctl) echoCtl=nullptr;
    decltype(&speex_echo_cancellation) echoProcess=nullptr;
    decltype(&speex_preprocess_state_init) preInit=nullptr;
    decltype(&speex_preprocess_state_destroy) preDestroy=nullptr;
    decltype(&speex_preprocess_ctl) preCtl=nullptr;
    decltype(&speex_preprocess_run) preProcess=nullptr;
    /** @brief 所有状态已释放后显式归还本实例的动态库引用。 */
    ~SpeexDspApi() { if (library.isLoaded()) library.unload(); }
    /** @brief 加载系统或管理员指定的运行库，并核对全部所需符号。
     * @param[in] path 可空；非空须为绝对路径，不擅自加载相对配置路径。
     * @param[out] error 缺库、错误架构或缺符号原因。
     * @return 可用的共享接口；失败为空。
     */
    static std::shared_ptr<SpeexDspApi> load(const QString &path,QString &error) {
        auto api=std::make_shared<SpeexDspApi>();
        QStringList candidates;
        if (!path.isEmpty()) {
            if (!QDir::isAbsolutePath(path)) { error=QStringLiteral("SpeexDSP libraryPath must be absolute");return {}; }
            candidates<<path;
        } else {
#ifdef Q_OS_WIN
            candidates<<QStringLiteral("speexdsp.dll")<<QStringLiteral("libspeexdsp-1.dll");
#else
            candidates<<QStringLiteral("libspeexdsp.so.1")<<QStringLiteral("speexdsp");
#endif
        }
        for (const auto &candidate:candidates) {
            api->library.setFileName(candidate);
            if (api->library.load()) break;
        }
        if (!api->library.isLoaded()) { error=api->library.errorString();return {}; }
#define LOAD_SPEEX(member,name) \
        api->member=reinterpret_cast<decltype(api->member)>(api->library.resolve(#name)); \
        if (!api->member) { error=QStringLiteral("SpeexDSP missing symbol: ")+QStringLiteral(#name);return {}; }
        LOAD_SPEEX(echoInit,speex_echo_state_init)
        LOAD_SPEEX(echoDestroy,speex_echo_state_destroy)
        LOAD_SPEEX(echoReset,speex_echo_state_reset)
        LOAD_SPEEX(echoCtl,speex_echo_ctl)
        LOAD_SPEEX(echoProcess,speex_echo_cancellation)
        LOAD_SPEEX(preInit,speex_preprocess_state_init)
        LOAD_SPEEX(preDestroy,speex_preprocess_state_destroy)
        LOAD_SPEEX(preCtl,speex_preprocess_ctl)
        LOAD_SPEEX(preProcess,speex_preprocess_run)
#undef LOAD_SPEEX
        return api;
    }
};
std::unique_ptr<SpeexAec> SpeexAec::create(const VoiceCall::ProcessingConfig &config,QString &error)
{
    auto result=std::unique_ptr<SpeexAec>(new SpeexAec);
    result->api_=SpeexDspApi::load(config.libraryPath,error);
    if (!result->api_) return {};
    result->state_=result->api_->echoInit(VoiceCall::Samples,qBound(40,config.filterLengthMs,500)*16);
    if (!result->state_) { error=QStringLiteral("Speex echo state initialization failed");return {}; }
    int rate=16000;
    if (result->api_->echoCtl(static_cast<SpeexEchoState *>(result->state_),SPEEX_ECHO_SET_SAMPLING_RATE,&rate)!=0) {
        error=QStringLiteral("Speex rejected 16000Hz echo rate");return {};
    }
    return result;
}
SpeexAec::~SpeexAec() { if (state_) api_->echoDestroy(static_cast<SpeexEchoState *>(state_)); }
void SpeexAec::reset() { if (state_) api_->echoReset(static_cast<SpeexEchoState *>(state_)); }
void SpeexAec::process(const int16_t *mic,const int16_t *ref,int16_t *out,int samples)
{
    if (!state_ || !mic || !ref || !out || samples!=VoiceCall::Samples) return;
    api_->echoProcess(static_cast<SpeexEchoState *>(state_),mic,ref,scratch_.data());
    std::copy(scratch_.begin(),scratch_.end(),out);
}
std::unique_ptr<SpeexPreprocessor> SpeexPreprocessor::create(const VoiceCall::ProcessingConfig &config,SpeexAec *aec,QString &error)
{
    auto result=std::unique_ptr<SpeexPreprocessor>(new SpeexPreprocessor);
    result->api_=aec ? aec->api_ : SpeexDspApi::load(config.libraryPath,error);
    result->aec_=aec;result->config_=config;
    if (!result->api_) return {};
    if (!result->initialize()) { error=QStringLiteral("Speex preprocess initialization/control failed (AGC may require floating point build)");return {}; }
    return result;
}
bool SpeexPreprocessor::initialize()
{
    state_=api_->preInit(VoiceCall::Samples,16000);
    if (!state_) return false;
    auto *state=static_cast<SpeexPreprocessState *>(state_);
    const auto ctl=[&](int request,int value) { return api_->preCtl(state,request,&value)==0; };
    bool ok=ctl(SPEEX_PREPROCESS_SET_DENOISE,config_.denoiseEnabled ? 1 : 0)
        && ctl(SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,config_.noiseSuppressDb)
        && ctl(SPEEX_PREPROCESS_SET_ECHO_SUPPRESS,config_.echoSuppressDb)
        && ctl(SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE,config_.echoSuppressActiveDb);
    ok=ok && api_->preCtl(state,SPEEX_PREPROCESS_SET_ECHO_STATE,aec_ ? aec_->state_ : nullptr)==0;
    // 上游固定点库没有AGC控制项且本来不运行AGC；关闭状态可接受该单项不支持。
    const bool agcSupported=ctl(SPEEX_PREPROCESS_SET_AGC,config_.agcEnabled ? 1 : 0);
    if (config_.agcEnabled) {
        float target=float(config_.agcTarget);
        ok=ok && agcSupported && api_->preCtl(state,SPEEX_PREPROCESS_SET_AGC_LEVEL,&target)==0
            && ctl(SPEEX_PREPROCESS_SET_AGC_MAX_GAIN,config_.agcMaxGainDb)
            && ctl(SPEEX_PREPROCESS_SET_AGC_INCREMENT,config_.agcIncrementDbPerSec)
            && ctl(SPEEX_PREPROCESS_SET_AGC_DECREMENT,config_.agcDecrementDbPerSec);
    }
    // 新状态默认VAD关闭；先查询避免上游SET_VAD即使设0也打印过时警告。
    int vad=0;
    ok=ok && api_->preCtl(state,SPEEX_PREPROCESS_GET_VAD,&vad)==0;
    if (vad) ok=ok && ctl(SPEEX_PREPROCESS_SET_VAD,0);
    if (!ok) { api_->preDestroy(state);state_=nullptr; }
    return ok;
}
SpeexPreprocessor::~SpeexPreprocessor() { if (state_) api_->preDestroy(static_cast<SpeexPreprocessState *>(state_)); }
void SpeexPreprocessor::reset()
{
    if (state_) api_->preDestroy(static_cast<SpeexPreprocessState *>(state_));
    state_=nullptr;initialize();
}
void SpeexPreprocessor::process(int16_t *inOut,int samples)
{
    if (state_ && inOut && samples==VoiceCall::Samples)
        (void)api_->preProcess(static_cast<SpeexPreprocessState *>(state_),inOut);
}

/** @brief 获取当前状态的只读原生诊断；控制不支持不改变DSP有效性或增益。 */
VoiceCall::PreprocessorObservation SpeexPreprocessor::observation() const
{
    VoiceCall::PreprocessorObservation result;if(!state_) return result;
    auto *state=static_cast<SpeexPreprocessState *>(state_);spx_int32_t value=0;
    if(config_.agcEnabled) {
        if(api_->preCtl(state,SPEEX_PREPROCESS_GET_AGC_GAIN,&value)==0) { result.gainValid=true;result.gainDb=int(value); }
        if(api_->preCtl(state,SPEEX_PREPROCESS_GET_AGC_LOUDNESS,&value)==0) result.loudness=int(value);
    }
    if(api_->preCtl(state,SPEEX_PREPROCESS_GET_PROB,&value)==0) result.speechProbability=int(value);
    return result;
}
