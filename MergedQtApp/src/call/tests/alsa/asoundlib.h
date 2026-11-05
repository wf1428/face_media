/**
 * @file asoundlib.h
 * @brief 仅供本机测试的 ALSA API 声明，不提供真实设备或 ABI 兼容保证。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-03；修改人：Du lin；补充只读PCM几何/软件阈值查询声明，验证诊断失败不影响传输和软件参数资源配对；不改变生产ALSA头。
 */
#pragma once
#ifndef ESTRPIPE
#define ESTRPIPE 86 /**< Linux 的挂起错误号，补足 Windows 测试编译环境。 */
#endif
extern "C" {
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_config snd_config_t;
typedef struct _snd_input snd_input_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
enum { SND_PCM_STREAM_PLAYBACK=0,SND_PCM_STREAM_CAPTURE=1,SND_PCM_NONBLOCK=1,
       SND_PCM_FORMAT_S16_LE=2,SND_PCM_ACCESS_RW_INTERLEAVED=3 };
typedef enum { SND_PCM_STATE_PREPARED=2,SND_PCM_STATE_RUNNING=3 } snd_pcm_state_t;
/** @brief 创建测试句柄，验证包装器使用非阻塞模式。
 * @param[out] pcm 返回由测试 close 释放的句柄。
 * @param[in] name 设备名，替身不访问设备。
 * @param[in] stream 采集/播放方向。
 * @param[in] mode 打开模式，须包含 NONBLOCK。
 * @return 0 成功，负值表示模式错误。
 */
int snd_pcm_open(snd_pcm_t **pcm,const char *name,int stream,int mode);
/** @brief 用局部配置打开测试PCM。
 * @param[out] pcm 成功时返回由close释放的句柄。
 * @param[in] name 借用的设备名。
 * @param[in] stream 采集方向。
 * @param[in] mode 必须包含NONBLOCK。
 * @param[in] config 局部copy/仅0→0配置，借用到PCM关闭。
 * @return 0成功；负值为注入打开失败或配置不符合约束。
 */
int snd_pcm_open_lconf(snd_pcm_t **pcm,const char *name,int stream,int mode,snd_config_t *config);
/** @brief 取得静态全局测试树的引用。
 * @param[out] config 成功返回借用全局树，须配对unref。
 * @return 0成功；负值为注入失败。
 */
int snd_config_update_ref(snd_config_t **config);
/** @brief 深复制测试配置。
 * @param[out] destination 成功返回由delete释放的局部树。
 * @param[in] source 有效源树，不修改。
 * @return 0成功；负值为注入分配失败。
 */
int snd_config_copy(snd_config_t **destination,snd_config_t *source);
/** @brief 配对归还静态全局树引用。
 * @param[in] config 必须是update_ref返回的全局树，不能是局部副本。
 */
void snd_config_unref(snd_config_t *config);
/** @brief 查找测试局部plughw节点。
 * @param[in] config 有效局部树。
 * @param[in] key 必须为pcm.plughw。
 * @param[out] result 成功返回树内借用节点，不单独释放。
 * @return 0成功；负值为查找或注入错误。
 */
int snd_config_search(snd_config_t *config,const char *key,snd_config_t **result);
/** @brief 复制内存规则到测试输入对象。
 * @param[out] input 成功返回由input_close释放的输入。
 * @param[in] buffer 有效规则字节。
 * @param[in] size 字节数，测试只使用正数。
 * @return 0成功；负值为注入失败。
 */
int snd_input_buffer_open(snd_input_t **input,const char *buffer,long size);
/** @brief 覆盖局部copy及整张0→0传输表。
 * @param[in,out] config 局部plughw节点，不能修改全局树。
 * @param[in] input 借用规则输入，调用后仍需input_close。
 * @return 0规则正确；负值为规则或注入错误。
 */
int snd_config_load_override(snd_config_t *config,snd_input_t *input);
/** @brief 释放测试规则输入。
 * @param[in] input 有效且由buffer_open创建的输入。
 * @return 固定0。
 */
int snd_input_close(snd_input_t *input);
/** @brief 释放测试局部配置树。
 * @param[in] config 有效局部树，不能是借用全局树。
 * @return 固定0。
 */
int snd_config_delete(snd_config_t *config);
/** @brief 令PREPARED采集进入RUNNING。
 * @param[in,out] pcm 有效测试采集句柄；失败保留未启动状态。
 * @return 0成功；负值为注入启动错误。
 */
int snd_pcm_start(snd_pcm_t *pcm);
/** @brief 记录格式/通道协商或注入参数失败。
 * @param[in,out] pcm 已打开测试句柄。
 * @param[in] format PCM 格式，应为 S16_LE。
 * @param[in] access 应为交错访问。
 * @param[in] channels 采集1（局部copy只取0）、播放2。
 * @param[in] rate 应为 16000Hz。
 * @param[in] resample 替身忽略软重采样选项。
 * @param[in] latency 替身忽略缓冲目标，单位微秒。
 * @return 0 成功，负值为格式/注入错误。
 */
int snd_pcm_set_params(snd_pcm_t *pcm,int format,int access,unsigned channels,unsigned rate,int resample,unsigned latency);
/** @brief 返回测试PCM几何，不改参数。
 * @param[in] pcm 有效模拟句柄。
 * @param[out] buffer 成功返回960音频帧。
 * @param[out] period 成功返回320音频帧。
 * @return 0成功，负值为诊断查询故障注入。
 */
int snd_pcm_get_params(snd_pcm_t *pcm,snd_pcm_uframes_t *buffer,snd_pcm_uframes_t *period);
/** @brief 分配测试软件参数快照。
 * @param[out] params 返回须配对free的对象。
 * @return 0成功，负值为注入失败。
 */
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **params);
/** @brief 释放测试快照。
 * @param[in] params malloc返回的有效对象。
 */
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *params);
/** @brief 只读加载测试软件参数。
 * @param[in] pcm 有效模拟句柄。
 * @param[out] params 已分配的快照。
 * @return 0成功，负值为注入失败。
 */
int snd_pcm_sw_params_current(snd_pcm_t *pcm,snd_pcm_sw_params_t *params);
/** @brief 获取测试启动阈值。
 * @param[in] params 有效模拟快照。
 * @param[out] value 成功返回960音频帧。
 * @return 0成功，负值为注入失败。
 */
int snd_pcm_sw_params_get_start_threshold(const snd_pcm_sw_params_t *params,snd_pcm_uframes_t *value);
/** @brief 获取测试唤醒阈值。
 * @param[in] params 有效模拟快照。
 * @param[out] value 成功返回320音频帧。
 * @return 0成功，负值为注入失败。
 */
int snd_pcm_sw_params_get_avail_min(const snd_pcm_sw_params_t *params,snd_pcm_uframes_t *value);
/** @brief 测试采集每次最多返回 80 帧，或返回注入的错误。
 * @param[in,out] pcm 测试采集句柄及读取游标。
 * @param[out] buffer 单声道小端 PCM 缓冲。
 * @param[in] frames 可容纳的音频帧数。
 * @return 正值为已读帧数，负值为注入错误或通道错误。
 */
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm,void *buffer,snd_pcm_uframes_t frames);
/** @brief 测试播放每次最多接收 80 帧，保留字节供左右通道断言。
 * @param[in,out] pcm 测试播放句柄及写入游标。
 * @param[in] buffer 交错双声道小端 PCM。
 * @param[in] frames 需要写入的音频帧数。
 * @return 正值为已写帧数，负值为注入错误或通道错误。
 */
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm,const void *buffer,snd_pcm_uframes_t frames);
/** @brief 模拟等待就绪、超时和一次性设备错误。
 * @param[in] pcm 替身忽略的测试句柄。
 * @param[in] timeout 等待上限，单位毫秒；无数据时模拟该长度的等待。
 * @return 1就绪、0无数据超时，负值为注入错误。
 */
int snd_pcm_wait(snd_pcm_t *pcm,int timeout);
/** @brief 记录恢复次数、重置采样游标并回到未启动状态。
 * @param[in,out] pcm 需要恢复的测试句柄。
 * @return 固定返回 0，模拟恢复成功。
 */
int snd_pcm_prepare(snd_pcm_t *pcm);
/** @brief 返回固定 640 帧排队深度供时延转换测试。
 * @param[in] pcm 替身忽略的测试句柄。
 * @param[out] delay 返回音频帧数。
 * @return 固定返回 0。
 */
int snd_pcm_delay(snd_pcm_t *pcm,snd_pcm_sframes_t *delay);
/** @brief 提供同步可用帧与延迟，专用于计时路径测试。
 * @param[in] pcm 模拟句柄。
 * @param[out] avail 可用音频帧数。
 * @param[out] delay 排队音频帧数。
 * @return 0成功，负值模拟计时失败，不影响基础传输。
 */
int snd_pcm_avail_delay(snd_pcm_t *pcm,snd_pcm_sframes_t *avail,snd_pcm_sframes_t *delay);
/** @brief 读取模拟设备状态。
 * @param[in] pcm 模拟句柄。
 * @return 未启动采集为PREPARED；其他情况返回注入的设备状态。
 */
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm);
/** @brief 模拟无阻塞丢弃。
 * @param[in] pcm 替身忽略的测试句柄。
 * @return 固定返回 0。
 */
int snd_pcm_drop(snd_pcm_t *pcm);
/** @brief 释放 open 分配的测试句柄并更新资源计数。
 * @param[in] pcm 非空、仍有效且由测试拥有的句柄。
 * @return 固定返回 0。
 */
int snd_pcm_close(snd_pcm_t *pcm);
/** @brief 提供固定的借用错误描述。
 * @param[in] error 替身忽略的错误号。
 * @return 静态字符串，调用方不释放。
 */
const char *snd_strerror(int error);
}
