/**
 * @file AdminLoginDialog.h
 * @brief 使用人脸模块共享 Qt Virtual Keyboard 的管理员登录对话框。
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
class QShowEvent;

/** @brief 使用人脸模块共享 Qt Virtual Keyboard 的管理员登录对话框。 */
class AdminLoginDialog : public QDialog {
    Q_OBJECT

public:
    enum ResultCode {
        PasswordAccessRequested = QDialog::Accepted + 1
    };

    /** @brief 按配置创建用户名、密码输入和登录界面。 */
    explicit AdminLoginDialog(const AppConfig &config, QWidget *parent = nullptr);

    /** @brief 销毁前移除宿主尺寸监听。 */
    ~AdminLoginDialog() override;

    /** @return 当前密码输入框内容。 */
    QString password() const;

protected:
    /** @brief Wayland 宿主尺寸变化时同步覆盖层几何。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief Wayland 内嵌模式显示时覆盖父界面。 */
    void showEvent(QShowEvent *event) override;

private:
    /** @brief 使用 AppConfig 校验管理员凭据并接受或提示失败。 */
    void verify();

    /** @brief 关闭管理员登录并切换到原有人员密码通行流程。 */
    void requestPasswordAccess();

private:
    AppConfig config_;                 /**< 管理员用户名和密码散列配置。 */
    QLineEdit *userEdit_ = nullptr;    /**< 用户名输入框。 */
    QLineEdit *passwordEdit_ = nullptr; /**< 密码输入框。 */
    bool embeddedForWayland_ = false;  /**< 是否复用人脸主界面的 Wayland surface。 */
};

#endif
