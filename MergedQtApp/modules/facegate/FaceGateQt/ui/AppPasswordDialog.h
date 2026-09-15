/**
 * @file AppPasswordDialog.h
 * @brief 使用人脸模块共享虚拟键盘的统一密码输入弹窗。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef APPPASSWORDDIALOG_H
#define APPPASSWORDDIALOG_H

#include <QDialog>
#include <QString>

class QEvent;
class QLineEdit;
class QScrollArea;
class QShowEvent;

/**
 * @brief 使用人脸模块共享虚拟键盘的统一密码输入弹窗。
 *
 * 用于替代 QInputDialog::getText(..., QLineEdit::Password)，避免系统输入弹窗
 * 与 Qt Virtual Keyboard 抢焦点、键盘不可点击或被管理页面裁剪。
 */
class AppPasswordDialog : public QDialog
{
    Q_OBJECT
public:
    /** @brief 创建旧密码、新密码和确认密码输入界面。 */
    explicit AppPasswordDialog(const QString &title,
                               const QString &description,
                               QWidget *parent = nullptr);

    /** @brief 销毁前移除宿主尺寸监听并释放输入焦点。 */
    ~AppPasswordDialog() override;

    /** @return 旧密码输入。 */
    QString oldPassword() const;

    /** @return 新密码输入。 */
    QString newPassword() const;

    /** @return 新密码确认输入。 */
    QString confirmPassword() const;

    /** @brief 以模态方式收集并校验一次密码修改输入。 */
    static bool getPasswordChange(QWidget *parent,
                                  QString *oldPassword,
                                  QString *newPassword);

protected:
    /** @brief 处理宿主尺寸变化并保证聚焦输入框可见。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 显示时稳定标题布局并聚焦第一个输入框。 */
    void showEvent(QShowEvent *event) override;

private:
    /** @brief 创建密码表单和滚动区域。 */
    void buildUi(const QString &title, const QString &description);

    /** @brief 加载统一密码弹窗样式。 */
    void loadStyleSheet();

    /** @brief 滚动表单以露出当前聚焦输入框。 */
    void ensureFocusedEditorVisible();

    /** @brief 在字体布局完成后固定标题区域高度。 */
    void stabilizeHeaderLayout();

    /** @brief 校验非空、强度和两次新密码一致后接受对话框。 */
    void validateAndAccept();

private:
    QLineEdit *oldPasswordEdit_ = nullptr;     /**< 旧密码输入框。 */
    QLineEdit *newPasswordEdit_ = nullptr;     /**< 新密码输入框。 */
    QLineEdit *confirmPasswordEdit_ = nullptr; /**< 新密码确认输入框。 */
    QScrollArea *contentScrollArea_ = nullptr; /**< 键盘出现时可滚动的表单区。 */
    QWidget *keyboardClearance_ = nullptr;     /**< 让表单可滚动到共享键盘上方的留白。 */
    bool embeddedForWayland_ = false;          /**< 是否复用管理后台的 Wayland surface。 */
};

#endif // APPPASSWORDDIALOG_H
