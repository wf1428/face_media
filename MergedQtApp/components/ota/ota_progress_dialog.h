/**
 * @file ota_progress_dialog.h
 * @brief 使用现有 FTP 下载卡片承载 OTA 进度和限时结果提示。
 * @author Du lin
 * @date 2026-11-18
 * @note 最近修改：2026-11-20；修改人：Du lin；复用 DownloadProgressToast 并接收显示毫秒数，保留安装中不可取消与原终态显示时长；定位固定在 appShellRoot 所属显示屏中心，根窗口变化时同步；去掉关闭按钮，仅显示共用 FTP 进度卡片；输入百分比表示当前阶段进度，卡片布局与隐藏计时不变。
 */
#pragma once
#include <QDialog>
class DownloadProgressToast;
class QTimer;
class QEvent;
/** @brief GUI 线程中的唯一 OTA 卡片容器；不调用媒体下载/播放业务。 */
class OtaProgressDialog final : public QDialog {
public:
    /** @brief 创建或刷新屏幕居中的 FTP 样式卡片；新通知取消旧的自动隐藏定时器。
     * @param text 安全的阶段/错误详情。
     * @param percent 当前显示阶段的百分比0..100；阶段重置由 OTA 协调器通知。
     * @param terminal true 为结果提示，false 为活动升级且禁止通过 reject 关闭。
     * @param success true 为健康确认成功。
     * @param displayMs 终态自动隐藏时长（毫秒），0 表示持续显示，后续状态刷新仍可替换当前提示。
     */
    static void present(const QString &text, int percent, bool terminal, bool success, int displayMs = 0);
protected:
    /** @brief 保留 Escape 对终态的关闭行为，活动升级不提供取消写盘操作。 */
    void reject() override;
    /** @brief 根窗口移动、缩放或显示时重算屏幕中心，不改变终态计时。
     * @param watched 受监视的根窗口。
     * @param event 窗口事件。
     * @return 返回基类结果，不拦截根窗口事件。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    /** @brief 创建透明非模态容器及共用下载卡片，控件由本对象拥有。
     * @param parent 固定根窗口，存在时使用子覆盖层；缺失时使用屏幕居中的工具窗口。
     */
    explicit OtaProgressDialog(QWidget *parent);
    /** @brief 将所属屏幕的全局中心换算到根窗口局部坐标，并同步卡片布局。 */
    void reposition();
    DownloadProgressToast *toast_ = nullptr; /**< 与视频 FTP 提示相同的卡片控件。 */
    QTimer *hideTimer_ = nullptr; /**< 单次隐藏计时器，新状态到来即停止。 */
    bool terminal_ = false; /**< false 时不允许通过 reject 关闭活动提示。 */
};
