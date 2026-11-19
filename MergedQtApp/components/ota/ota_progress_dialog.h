/**
 * @file ota_progress_dialog.h
 * @brief 系统 OTA 的持续进度与重启后结果提示窗口。
 * @author Du lin
 * @date 2026-11-18
 */
#pragma once
#include <QDialog>
class QLabel;
class QProgressBar;
class QPushButton;
/** @brief 仅在 GUI 线程使用的无日志提示；安装中不提供取消刷写操作。 */
class OtaProgressDialog final : public QDialog {
public:
    /** @brief 创建/刷新唯一的 OTA 提示窗口，结果允许手动关闭。
     * @param text 已净化的进度或结果文本。
     * @param percent 整体百分比 0..100。
     * @param terminal true 表示显示结果和关闭按钮。
     * @param success true 表示健康确认成功，可在10秒后自动隐藏。
     */
    static void present(const QString &text, int percent, bool terminal, bool success);
protected:
    /** @brief 安装过程拒绝关闭，终态遵循普通对话框关闭行为。 */
    void reject() override;
private:
    /** @brief 在 GUI 线程创建轻量对话框，控件由 Qt 父子关系释放。 */
    OtaProgressDialog();
    QLabel *message_ = nullptr; /**< 自有文本标签。 */
    QProgressBar *progress_ = nullptr; /**< 自有整体进度条。 */
    QPushButton *close_ = nullptr; /**< 仅终态可用的关闭按钮。 */
    bool terminal_ = false; /**< false 时不能关闭升级提示。 */
    quint64 generation_ = 0; /**< 过期成功隐藏回调不能关闭新的任务提示。 */
};
