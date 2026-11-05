/**
 * @file speex_fault_library.cpp
 * @brief 仅测试：符号齐全但初始化失败的SpeexDSP替身，不加入应用或部署。
 * @author Du lin
 * @date 2026-10-31
 */
#include "third_party/speexdsp/include/speex/speex_echo.h"
#include "third_party/speexdsp/include/speex/speex_preprocess.h"
extern "C" {
/** @brief 故意使AEC初始化失败。
 * @param[in] frame_size 忽略的帧长。
 * @param[in] filter_length 忽略的尾长。
 * @return 始终为空，不分配资源。
 */
SpeexEchoState *speex_echo_state_init(int frame_size,int filter_length) { (void)frame_size;(void)filter_length;return nullptr; }
/** @brief 无状态可释放。
 * @param[in] st 测试不创建的状态。
 */
void speex_echo_state_destroy(SpeexEchoState *st) { (void)st; }
/** @brief 无状态可重置。
 * @param[in] st 不使用。
 */
void speex_echo_state_reset(SpeexEchoState *st) { (void)st; }
/** @brief 控制请求固定失败。
 * @param[in] st 不使用。
 * @param[in] request 不使用。
 * @param[in,out] ptr 不修改。
 * @return 固定-1。
 */
int speex_echo_ctl(SpeexEchoState *st,int request,void *ptr) { (void)st;(void)request;(void)ptr;return -1; }
/** @brief 不执行处理；初始化失败后不得调用。
 * @param[in] st 不使用。
 * @param[in] rec 不读取。
 * @param[in] play 不读取。
 * @param[out] out 不修改。
 */
void speex_echo_cancellation(SpeexEchoState *st,const spx_int16_t *rec,const spx_int16_t *play,spx_int16_t *out) { (void)st;(void)rec;(void)play;(void)out; }
/** @brief 故意使预处理初始化失败。
 * @param[in] frame_size 不使用。
 * @param[in] sampling_rate 不使用。
 * @return 始终为空。
 */
SpeexPreprocessState *speex_preprocess_state_init(int frame_size,int sampling_rate) { (void)frame_size;(void)sampling_rate;return nullptr; }
/** @brief 无状态可释放。
 * @param[in] st 不使用。
 */
void speex_preprocess_state_destroy(SpeexPreprocessState *st) { (void)st; }
/** @brief 控制请求固定失败。
 * @param[in] st 不使用。
 * @param[in] request 不使用。
 * @param[in,out] ptr 不修改。
 * @return 固定-1。
 */
int speex_preprocess_ctl(SpeexPreprocessState *st,int request,void *ptr) { (void)st;(void)request;(void)ptr;return -1; }
/** @brief 不执行处理；初始化失败后不得调用。
 * @param[in] st 不使用。
 * @param[in,out] x 不修改。
 * @return 固定1，不提供VAD门控。
 */
int speex_preprocess_run(SpeexPreprocessState *st,spx_int16_t *x) { (void)st;(void)x;return 1; }
}
