/**
 * @file CallPanel.h
 * @brief 融合窗口内的非模态通话状态、电平与挂断面板。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-10-30；修改人：Du lin；面板仅在活动通话显示状态、电平与挂断，移除空闲呼叫及手动接听接口；呼叫入口统一放在右键密码键盘。
 */
#pragma once
#include <QWidget>
#include <QPointer>
#include <QTimer>
class CallManager;
class ReceiveAudioPlot;
class QPushButton;
class QLabel;

/** @brief 子控件覆盖层，不创建原生顶层窗口，不切换/暂停视频页面。 */
class CallPanel : public QWidget {
    Q_OBJECT
public:
    /** @brief 创建通话操作、接收幅度图和 10Hz 只读刷新，并连接状态机。
     * @param[in] manager 借用现有通话控制器，生命周期覆盖面板。
     * @param[in] parent 主窗口，面板随之销毁。
     */
    CallPanel(CallManager *manager,QWidget *parent);
protected:
    /** @brief 跟随主窗口尺寸定位，不改变原有页面布局。
     * @param[in] watched 被监视的主窗口或其他对象，借用且不释放。
     * @param[in] event Qt 提供的事件，借用且不释放。
     * @return QWidget 的过滤结果；本面板不主动吞掉原窗口事件。
     */
    bool eventFilter(QObject *watched,QEvent *event) override;
private:
    /** @brief 更新可见性、按钮状态和面板位置。 */
    void refresh();
    /** @brief 以 10Hz 读取小型电平快照，滚动绘图并区分未收到、静音和有幅度。
     * @note 只在 Talking 运行；不播放、保存或消费任何音频样本。
     */
    void refreshMeter();
    QPointer<CallManager> manager_; /**< 借用控制器，销毁时自动失效；所有 UI 操作在主线程。 */
    ReceiveAudioPlot *plot_; /**< 父对象拥有的 6 秒接收电平图，不持有原始 PCM。 */
    QLabel *meterLabel_; /**< 父对象拥有的接收/静音/中断状态文字。 */
    QTimer meterTimer_; /**< Talking 时每 100ms 刷新，其他状态停止。 */
    QLabel *label_; /**< 父对象拥有的通话状态文字。 */
    QPushButton *hangup_; /**< 父对象拥有的非阻塞挂断按钮；自动接听不再提供手动接听选项。 */
};
