/**
 * @file offline_dialog.h
 * @brief 追加显示调试状态文本的非编辑对话框。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef OFFLINE_DIALOG_H
#define OFFLINE_DIALOG_H

#include <QDialog>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>


/** @brief 追加显示调试状态文本的非编辑对话框。 */
class StatusWindow : public QDialog
{
public:
    /** @brief 创建只读文本区并设置 800x600 初始尺寸。 */
    explicit StatusWindow(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("Status");
        resize(800, 600);

        m_edit = new QPlainTextEdit(this);
        m_edit->setReadOnly(true);

        auto *layout = new QVBoxLayout(this);
        layout->addWidget(m_edit);
        setLayout(layout);
    }

    /**
     * @brief 追加一行状态消息。
     * @param clearMs 预留的清理延时；当前实现不会自动清空文本。
     */
    void showMessage(const QString &msg, int clearMs = 0)
    {
        // 保留历史状态，便于连续观察协议或设备输出。
        m_edit->appendPlainText(msg);

        if (clearMs > 0) {
            QTimer::singleShot(clearMs, this, [this](){
                // 清理逻辑暂未启用；定时回调保留为兼容接口。
                // m_edit->clear();
            });
        }
    }

private:
    QPlainTextEdit *m_edit = nullptr; /**< 只读追加式状态文本区。 */
};

#endif // OFFLINE_DIALOG_H
