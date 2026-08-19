/**
 * @file AdminLoginDialog.h
 * @brief 带内嵌 Qt 虚拟键盘的管理员登录对话框。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ADMIN_LOGIN_DIALOG_H
#define ADMIN_LOGIN_DIALOG_H

#include <QDialog>

#include "AppConfig.h"

class QEvent;
class QLineEdit;
class QQuickWidget;

/** @brief 带内嵌 Qt 虚拟键盘的管理员登录对话框。 */
class AdminLoginDialog : public QDialog {
    Q_OBJECT

public:
    /** @brief 按配置创建用户名、密码输入和虚拟键盘界面。 */
    explicit AdminLoginDialog(const AppConfig &config, QWidget *parent = nullptr);

    /** @brief 销毁前释放键盘焦点和资源。 */
    ~AdminLoginDialog() override;

    /** @return 当前密码输入框内容。 */
    QString password() const;

protected:
    /** @brief 根据文本编辑器焦点和点击位置显示或隐藏内嵌键盘。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @brief 使用 AppConfig 校验管理员凭据并接受或提示失败。 */
    void verify();

    /** @brief 显示并定位内嵌虚拟键盘。 */
    void showKeyboard();

    /** @brief 隐藏内嵌虚拟键盘。 */
    void hideKeyboard();

    /** @brief 根据对话框可用区域更新键盘几何位置。 */
    void positionKeyboard();

    /** @return widget 是可编辑文本输入控件时返回 true。 */
    bool isTextEditor(QWidget *widget) const;

private:
    AppConfig config_;                 /**< 管理员用户名和密码散列配置。 */
    QLineEdit *userEdit_ = nullptr;    /**< 用户名输入框。 */
    QLineEdit *passwordEdit_ = nullptr; /**< 密码输入框。 */
    QQuickWidget *keyboardWidget_ = nullptr; /**< 内嵌 Qt 虚拟键盘。 */
};

#endif
