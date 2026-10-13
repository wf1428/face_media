/**
 * @file AccessPasswordDialog.cpp
 * @brief 人脸识别主页的人员通行密码输入窗口实现。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "AccessPasswordDialog.h"

#include "AppMessageDialog.h"
#include "common/responsive_ui.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>

/** @brief 初始化无边框模态窗口并创建密码输入界面。 */
AccessPasswordDialog::AccessPasswordDialog(QWidget *parent)
    : QDialog(parent)
{
    embeddedForWayland_ = ResponsiveUi::isWaylandPlatform() && parentWidget();
    setModal(true);
    if (embeddedForWayland_) {
        setWindowFlags(Qt::Widget);
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
    } else {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setMinimumSize(680, 560);
        resize(720, 580);
    }
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName(QStringLiteral("accessPasswordDialog"));
    buildUi();
}

/** @brief 移除宿主尺寸监听并释放输入焦点。 */
AccessPasswordDialog::~AccessPasswordDialog()
{
    if (embeddedForWayland_ && parentWidget()) {
        parentWidget()->removeEventFilter(this);
    }
    if (passwordEdit_) {
        passwordEdit_->clearFocus();
    }
}

/** @return 当前密码输入框文本；控件尚未创建时返回空字符串。 */
QString AccessPasswordDialog::password() const
{
    return passwordEdit_ ? passwordEdit_->text() : QString();
}

/** @brief 以模态方式采集密码，仅在用户确认时写入输出参数。 */
bool AccessPasswordDialog::getPassword(QWidget *parent, QString *password)
{
    AccessPasswordDialog dialog(parent);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    if (password) {
        *password = dialog.password();
    }
    return true;
}

/** @brief Wayland 宿主尺寸变化时同步覆盖层几何。 */
bool AccessPasswordDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (embeddedForWayland_ && watched == parentWidget() &&
            event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QDialog::eventFilter(watched, event);
}

/** @brief 首次显示后聚焦密码框，并等待窗口稳定再拉起键盘。 */
void AccessPasswordDialog::showEvent(QShowEvent *event)
{
    if (embeddedForWayland_ && parentWidget()) {
        setGeometry(parentWidget()->rect());
    }
    QDialog::showEvent(event);
    if (passwordEdit_) {
        QTimer::singleShot(80, this, [this]() {
            if (isVisible() && passwordEdit_) {
                passwordEdit_->setFocus();
            }
        });
    }
}

/** @brief 创建密码卡片、操作按钮和样式。 */
void AccessPasswordDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(12);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("accessPasswordCard"));
    if (embeddedForWayland_) {
        card->setMaximumWidth(720);
    }
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 20, 24, 20);
    cardLayout->setSpacing(14);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);
    auto *icon = new QLabel(card);
    icon->setFixedSize(44, 44);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(QIcon(QStringLiteral(":/icons/action/key.svg")).pixmap(26, 26));

    auto *titleBox = new QVBoxLayout();
    titleBox->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("密码通行"), card);
    title->setObjectName(QStringLiteral("accessPasswordTitle"));
    auto *description = new QLabel(
                QStringLiteral("请输入人员通行密码，验证成功后将下发该人员的楼层权限。"), card);
    description->setObjectName(QStringLiteral("accessPasswordHint"));
    description->setWordWrap(true);
    titleBox->addWidget(title);
    titleBox->addWidget(description);

    auto *closeButton = new QPushButton(card);
    closeButton->setObjectName(QStringLiteral("accessPasswordClose"));
    closeButton->setIcon(QIcon(QStringLiteral(":/icons/action/x.svg")));
    closeButton->setIconSize(QSize(18, 18));
    closeButton->setFixedSize(42, 42);
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    header->addWidget(icon);
    header->addLayout(titleBox, 1);
    header->addWidget(closeButton);

    auto *fieldLabel = new QLabel(QStringLiteral("人员通行密码"), card);
    fieldLabel->setObjectName(QStringLiteral("accessPasswordFieldLabel"));
    passwordEdit_ = new QLineEdit(card);
    passwordEdit_->setObjectName(QStringLiteral("accessPasswordEdit"));
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText(QStringLiteral("请输入密码"));
    passwordEdit_->setMaxLength(64);
    passwordEdit_->setMinimumHeight(48);
    passwordEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    passwordEdit_->setInputMethodHints(Qt::ImhNoPredictiveText |
                                       Qt::ImhSensitiveData);
    connect(passwordEdit_, &QLineEdit::returnPressed,
            this, &AccessPasswordDialog::acceptInput);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(12);
    buttonRow->addStretch();
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), card);
    cancelButton->setObjectName(QStringLiteral("accessPasswordCancel"));
    cancelButton->setMinimumSize(110, 44);
    auto *confirmButton = new QPushButton(QStringLiteral("验证并通行"), card);
    confirmButton->setObjectName(QStringLiteral("accessPasswordConfirm"));
    confirmButton->setIcon(QIcon(QStringLiteral(":/icons/action/check.svg")));
    confirmButton->setIconSize(QSize(18, 18));
    confirmButton->setMinimumSize(140, 44);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(confirmButton, &QPushButton::clicked,
            this, &AccessPasswordDialog::acceptInput);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(confirmButton);

    cardLayout->addLayout(header);
    cardLayout->addWidget(fieldLabel);
    cardLayout->addWidget(passwordEdit_);
    cardLayout->addLayout(buttonRow);

    if (embeddedForWayland_) {
        root->addWidget(card, 0, Qt::AlignHCenter | Qt::AlignTop);
    } else {
        root->addWidget(card);
    }
    root->addStretch(1);

    setStyleSheet(QStringLiteral(
        "QDialog#accessPasswordDialog{background:#101a2a;color:#edf5f8;"
        "border:1px solid #30445f;border-radius:16px;}"
        "QFrame#accessPasswordCard{background:#162236;border:1px solid #30445f;"
        "border-radius:12px;}"
        "QLabel{color:#dce8ee;font-family:Arial;font-size:15px;}"
        "QLabel#accessPasswordTitle{color:#ffffff;font-size:24px;font-weight:700;}"
        "QLabel#accessPasswordHint{color:#9fb3bf;font-size:14px;}"
        "QLabel#accessPasswordFieldLabel{color:#49d1ff;font-size:14px;font-weight:600;}"
        "QLineEdit#accessPasswordEdit{background:#0d1624;color:#ffffff;"
        "border:1px solid #3c526e;border-radius:8px;padding:0 12px;font-size:20px;}"
        "QLineEdit#accessPasswordEdit:focus{border:2px solid #49d1ff;}"
        "QPushButton{border-radius:8px;padding:8px 16px;font-size:15px;}"
        "QPushButton#accessPasswordConfirm{background:#1688b8;color:#ffffff;"
        "border:1px solid #49d1ff;font-weight:600;}"
        "QPushButton#accessPasswordConfirm:pressed{background:#0f6f98;}"
        "QPushButton#accessPasswordCancel,QPushButton#accessPasswordClose{"
        "background:#223149;color:#dce8ee;border:1px solid #3c526e;}"
        "QPushButton#accessPasswordCancel:pressed,QPushButton#accessPasswordClose:pressed{"
        "background:#2e405b;}"));
}

/** @brief 拒绝空密码；有效输入则释放焦点后接受对话框。 */
void AccessPasswordDialog::acceptInput()
{
    if (!passwordEdit_ || passwordEdit_->text().trimmed().isEmpty()) {
        AppMessageDialog::warning(this, QStringLiteral("密码通行"),
                                  QStringLiteral("请输入人员通行密码"));
        return;
    }
    passwordEdit_->clearFocus();
    accept();
}
