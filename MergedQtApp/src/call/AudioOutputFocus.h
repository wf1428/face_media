/**
 * @file AudioOutputFocus.h
 * @brief 声明通话的后台设备交接和可取消异步音量焦点，避免引擎依赖 GStreamer 类型。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-10-30；修改人：Du lin；明确通话跨音量控制器的硬件保护、旧命令退出后就绪，以及保留逻辑视频音量并在释放后恢复最新值的异步语义。
 */
#pragma once
#include <functional>
class QObject;
namespace VoiceCall {
/** @brief 在窗口/业务对象初始化阶段预创建既有音量焦点，避免呼叫时触发同步首次硬件初始化。
 * @note 仅 UI 线程初始化时调用；运行中的申请/归还继续走异步接口。
 */
void initializeOutputVolume();

/** @brief 在 UI 线程异步申请既有语音音量与功放焦点，成功/失败收口后再启动声卡。
 * @param[in] owner 非空调用者，拥有可取消的本次申请；必须位于 UI 线程。
 * @param[in] ready 硬件音量就绪通知，不在函数内同步调用；owner 释放后不再调用。
 * @note 不改持久化视频音量，使用已有语音播报音量；跨控制器保护硬件增益/功放。
 *       旧异步 amixer 完成后才发就绪，重叠提示仍共用原焦点所有权。
 */
void acquireOutputVolume(QObject *owner,std::function<void()> ready);
/** @brief 取消尚未完成的申请并归还焦点，异步恢复最新用户音量和功放。
 * @note 全部重叠焦点结束且恢复完成后才放开硬件保护；正常挂断不等待进程。
 * @param[in] owner 之前申请的 UI 对象；可重复归还，空指针忽略。
 */
void releaseOutputVolume(QObject *owner);

/** @brief 在后台线程串行交接共享音频输出，视频播放不受影响。
 * @param[in] exclusive true 释放共享声卡并丢弃媒体 PCM；false 恢复共享输出。
 * @return true 已释放声卡或已提交异步恢复；false 表示状态切换失败。
 * @note 实现由现有播放器提供，状态确认最多等待 1 秒，状态切换耗时由 GStreamer 决定；禁止从 UI 线程调用。
 */
bool setMediaAudioExclusive(bool exclusive);
}
