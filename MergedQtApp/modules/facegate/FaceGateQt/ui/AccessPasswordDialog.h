/**
 * @file AccessPasswordDialog.h
 * @brief 人脸识别主页的人员通行密码输入窗口。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef ACCESS_PASSWORD_DIALOG_H
#define ACCESS_PASSWORD_DIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;
class QQuickWidget;
class QShowEvent;

/** @brief 人脸识别主页使用的人员通行密码输入窗口。 */
class AccessPasswordDialog : public QDialog
{
    Q_OBJECT
public:
    /** @brief 创建带内嵌虚拟键盘的模态密码输入窗口。 */
    explicit AccessPasswordDialog(QWidget *parent = nullptr);

    /** @brief 隐藏系统输入法并销毁窗口资源。 */
    ~AccessPasswordDialog() override;

    /** @return 当前输入的原始密码文本。 */
    QString password() const;

    /**
     * @brief 模态获取人员通行密码。
     * @param parent  对话框父窗口。
     * @param password  接收用户确认后的密码；可为 nullptr。
     * @return 用户确认输入时返回 true，取消时返回 false。
     */
    static bool getPassword(QWidget *parent, QString *password);

protected:
    /** @brief 显示后聚焦密码框并延迟拉起虚拟键盘。 */
    void showEvent(QShowEvent *event) override;

private:
    /** @brief 创建密码表单、操作按钮和内嵌 QML 键盘。 */
    void buildUi();

    /** @brief 校验非空输入，隐藏键盘后接受对话框。 */
    void acceptInput();

    /** @brief 同时显示内嵌键盘和 Qt 输入法。 */
    void showKeyboard();

    /** @brief 同时隐藏内嵌键盘和 Qt 输入法。 */
    void hideKeyboard();

    QLineEdit *passwordEdit_ = nullptr;       /**< 密码输入控件。 */
    QQuickWidget *keyboardWidget_ = nullptr;  /**< 内嵌 QML 虚拟键盘。 */
};

#endif // ACCESS_PASSWORD_DIALOG_H
