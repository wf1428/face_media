/**
 * @file CallRingtone.h
 * @brief 播放设备主动呼叫的等待铃声，随通话状态释放共享混音优先权。
 * @author Du lin
 * @date 2026-11-05
 */
#pragma once
#include "CallManager.h"
#include <memory>

class GstPlayerWidget;

/** @brief UI 线程的外呼铃声控制器，使用独立的音频播放器。
 * @details 资源在后台物化；铃声就绪后才循环播放，停止请求立即屏蔽铃声采样，
 *          管线由播放器后台停止。正常拨号与挂断均不等待文件或播放线程。
 */
class CallRingtone final : public QObject {
public:
    /** @brief 排队准备应用内 MP3 资源。
     * @param[in] parent UI 线程所有者，通常为 CallManager。
     */
    explicit CallRingtone(QObject *parent);
    /** @brief 解除铃声优先权并在对象退出时释放播放管线和临时文件。 */
    ~CallRingtone() override;

    /** @brief 在 UI 线程切换铃声播放与混音优先权。
     * @param[in] phase Off 释放优先权；HandingOff 停播并保持静音直到声卡交接结束。
     */
    void setPhase(CallManager::RingtonePhase phase);

private:
    /** @brief 首次外呼时创建隐藏播放器并绑定 EOS、播放错误。 */
    void ensurePlayer();
    /** @brief 准备成功且仍在等待时，开始一轮铃声。 */
    void playIfReady();
    /** @brief 停止当前循环；只有结束等待才解除混音优先权。 */
    void stopPlayback();

    struct Asset;
    std::shared_ptr<Asset> asset_; /**< 后台写入，完成回调送达后由 UI 读取。 */
    std::unique_ptr<GstPlayerWidget> player_; /**< 隐藏的专用播放器，仅 UI 管理生命周期。 */
    bool assetReady_=false; /**< MP3 已物化，完成回调由 UI 写入。 */
    CallManager::RingtonePhase phase_=CallManager::RingtonePhase::Off; /**< UI 独占的铃声与交接状态。 */
    bool playing_=false; /**< 本轮播放已提交；EOS 才允许开始下一轮。 */
};
