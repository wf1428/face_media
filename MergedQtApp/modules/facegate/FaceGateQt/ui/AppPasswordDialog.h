/**
 * @file AppPasswordDialog.h
 * @brief 项目统一密码输入弹窗，内部集成虚拟键盘。
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
class QQuickWidget;
class QScrollArea;

/**
 * @brief 项目统一密码输入弹窗，内部集成虚拟键盘。
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

    /** @brief 销毁前释放虚拟键盘资源。 */
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
    /** @brief 管理文本焦点、键盘点击和键盘可见性。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 显示时稳定标题布局和键盘几何位置。 */
    void showEvent(QShowEvent *event) override;

private:
    /** @brief 创建密码表单、滚动区域和虚拟键盘。 */
    void buildUi(const QString &title, const QString &description);

    /** @brief 加载统一密码弹窗样式。 */
    void loadStyleSheet();

    /** @brief 显示虚拟键盘并保证当前编辑器可见。 */
    void showKeyboard();

    /** @brief 隐藏虚拟键盘。 */
    void hideKeyboard();

    /** @brief 根据弹窗可用高度调整键盘区域。 */
    void updateKeyboardGeometry();

    /** @brief 滚动表单以露出当前聚焦输入框。 */
    void ensureFocusedEditorVisible();

    /** @brief 在字体布局完成后固定标题区域高度。 */
    void stabilizeHeaderLayout();

    /** @return widget 是密码文本编辑器时返回 true。 */
    bool isTextEditor(QWidget *widget) const;

    /** @return widget 属于内嵌键盘对象树时返回 true。 */
    bool isKeyboardWidget(QWidget *widget) const;

    /** @brief 校验非空、强度和两次新密码一致后接受对话框。 */
    void validateAndAccept();

private:
    QLineEdit *oldPasswordEdit_ = nullptr;     /**< 旧密码输入框。 */
    QLineEdit *newPasswordEdit_ = nullptr;     /**< 新密码输入框。 */
    QLineEdit *confirmPasswordEdit_ = nullptr; /**< 新密码确认输入框。 */
    QScrollArea *contentScrollArea_ = nullptr; /**< 键盘出现时可滚动的表单区。 */
    QQuickWidget *keyboardWidget_ = nullptr;   /**< 内嵌 Qt 虚拟键盘。 */
};

#endif // APPPASSWORDDIALOG_H
