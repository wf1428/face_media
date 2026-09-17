/**
 * @file AdminLoginDialog.cpp
 * @brief 使用人脸模块共享 Qt Virtual Keyboard 的管理员登录对话框实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AdminLoginDialog.h"
#include "AppMessageDialog.h"
#include "common/responsive_ui.h"

#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

/** @brief 按配置创建用户名、密码输入和登录界面。 */
AdminLoginDialog::AdminLoginDialog(const AppConfig &config, QWidget *parent)
    : QDialog(parent), config_(config)
{
    embeddedForWayland_ = ResponsiveUi::isWaylandPlatform() && parentWidget();
    setObjectName(QStringLiteral("AdminLoginDialog"));
    setWindowTitle(QStringLiteral("管理员登录"));
    setModal(true);
    if (embeddedForWayland_) {
        // 登录框和人脸主页共用一个 Wayland surface。这样确认登录时只隐藏
        // 普通子控件，不会销毁独立 xdg/wl-shell surface 及关联 EGL 资源。
        setWindowFlags(Qt::Widget);
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        setGeometry(parentWidget()->rect());
    } else {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    }
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    QPalette loginPalette = palette();
    loginPalette.setColor(QPalette::Window, QColor("#162236"));
    loginPalette.setColor(QPalette::Base, QColor("#162236"));
    setPalette(loginPalette);
    if (!embeddedForWayland_) {
        const QSize targetSize = ResponsiveUi::dialogSize(
                    parent, QSize(760, 580), QSize(520, 520));
        setMinimumSize(qMin(520, targetSize.width()),
                       qMin(520, targetSize.height()));
        resize(targetSize);
    }

    userEdit_ = new QLineEdit(this);
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    // 输入框只接入人脸根窗口长期存在的共享 InputPanel；登录框自身不再
    // 创建或销毁 QQuickWidget，因此确认登录后可以安全进入管理后台。
    userEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    passwordEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    userEdit_->setInputMethodHints(Qt::ImhNoPredictiveText);
    passwordEdit_->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhSensitiveData);

    userEdit_->setText(config_.adminUsername);
    QLabel *hintLabel = nullptr;
    if (config_.adminPasswordChangeRequired) {
        hintLabel = new QLabel(
                    QStringLiteral("账号：%1    %2")
                    .arg(config_.adminUsername, config_.adminPasswordHint), this);
        hintLabel->setTextFormat(Qt::PlainText);
        hintLabel->setWordWrap(true);
    }

    auto *loginButton = new QPushButton(QStringLiteral("登录"), this);
    loginButton->setObjectName("dialogPrimaryButton");
    loginButton->setCursor(Qt::PointingHandCursor);
    loginButton->setMinimumWidth(132);

    auto *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    cancelButton->setObjectName("dialogCancelButton");
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setMinimumWidth(132);

    connect(loginButton, &QPushButton::clicked, this, &AdminLoginDialog::verify);
    connect(cancelButton, &QPushButton::clicked, this, &AdminLoginDialog::reject);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 4, 0, 0);
    buttonRow->setSpacing(12);
    buttonRow->addStretch(1);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(loginButton);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(14);
    form->addRow(QStringLiteral("用户名"), userEdit_);
    form->addRow(QStringLiteral("密码"), passwordEdit_);

    auto *formShell = new QWidget(this);
    formShell->setObjectName(QStringLiteral("loginCard"));
    if (embeddedForWayland_) {
        formShell->setMaximumWidth(760);
        formShell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    auto *formShellLayout = new QVBoxLayout(formShell);
    formShellLayout->setContentsMargins(24, 18, 24, 12);
    formShellLayout->setSpacing(14);
    if (hintLabel) {
        formShellLayout->addWidget(hintLabel);
    }
    formShellLayout->addLayout(form);
    formShellLayout->addLayout(buttonRow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (embeddedForWayland_) {
        layout->addStretch(1);
        layout->addWidget(formShell, 0, Qt::AlignHCenter);
        layout->addStretch(1);
    } else {
        layout->addWidget(formShell);
    }

    if (embeddedForWayland_ && parentWidget()) {
        parentWidget()->installEventFilter(this);
    }

    QString style;
    QFile adminStyleFile(QStringLiteral(":/qss/admin_panel.qss"));
    if (adminStyleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        style += QString::fromUtf8(adminStyleFile.readAll());
    }
    QFile dialogStyleFile(QStringLiteral(":/qss/shadcn_dialog.qss"));
    if (dialogStyleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        style += QString::fromUtf8(dialogStyleFile.readAll());
    }
    style += QStringLiteral(
        "QDialog#AdminLoginDialog{background:#162236;color:#edf5f8;border:none;}"
        "QWidget#loginCard{background:#1b2a40;border:1px solid #46617f;border-radius:18px;}"
        "QLabel{color:#d8e5eb;font-size:16px;}"
        "QLineEdit{font-size:18px;min-height:42px;}"
        "QPushButton#dialogPrimaryButton,QPushButton#dialogCancelButton{min-width:132px;}"
    );
    setStyleSheet(style);
}

/** @brief 销毁前移除宿主尺寸监听。 */
AdminLoginDialog::~AdminLoginDialog()
{
    if (embeddedForWayland_ && parentWidget()) {
        parentWidget()->removeEventFilter(this);
    }
}

/** @return 当前密码输入框内容。 */
QString AdminLoginDialog::password() const
{
    return passwordEdit_ ? passwordEdit_->text() : QString();
}

/** @brief 使用 AppConfig 校验管理员凭据并接受或提示失败。 */
void AdminLoginDialog::verify()
{
    if (config_.verifyAdminPassword(userEdit_->text(), passwordEdit_->text())) {
        userEdit_->clearFocus();
        passwordEdit_->clearFocus();
        accept();
        return;
    }

    AppMessageDialog::warning(this, QStringLiteral("管理员登录"), QStringLiteral("用户名或密码错误"));
}

/** @brief Wayland 宿主尺寸变化时同步覆盖层几何。 */
bool AdminLoginDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (embeddedForWayland_ && watched == parentWidget() &&
        event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }

    return QDialog::eventFilter(watched, event);
}

/** @brief 显示时刷新 Wayland 覆盖层几何。 */
void AdminLoginDialog::showEvent(QShowEvent *event)
{
    if (embeddedForWayland_ && parentWidget()) {
        setGeometry(parentWidget()->rect());
    }
    QDialog::showEvent(event);
    if (embeddedForWayland_) {
        raise();
    }

    // 用户名已有配置默认值，直接聚焦密码框，由共享键盘自动显示。
    if (passwordEdit_) {
        QTimer::singleShot(80, this, [this]() {
            if (isVisible() && passwordEdit_) {
                passwordEdit_->setFocus();
            }
        });
    }
}
