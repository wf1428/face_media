/**
 * @file AdminLoginDialog.cpp
 * @brief 带内嵌 Qt 虚拟键盘的管理员登录对话框的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AdminLoginDialog.h"
#include "AppMessageDialog.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QQuickWidget>
#include <QSizePolicy>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const int kLoginKeyboardHeight = 280;
}

/** @brief 按配置创建用户名、密码输入和虚拟键盘界面。 */
AdminLoginDialog::AdminLoginDialog(const AppConfig &config, QWidget *parent)
    : QDialog(parent), config_(config)
{
    setWindowTitle(QStringLiteral("管理员登录"));
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(720, 540);
    resize(760, 580);

    userEdit_ = new QLineEdit(this);
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    userEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    passwordEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    userEdit_->setInputMethodHints(Qt::ImhNoPredictiveText);
    passwordEdit_->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhSensitiveData);

    userEdit_->setText(config_.adminUsername);
    auto *hintLabel = new QLabel(QStringLiteral("账号：%1    %2").arg(config_.adminUsername, config_.adminPasswordHint), this);
    hintLabel->setTextFormat(Qt::PlainText);
    hintLabel->setWordWrap(true);

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
    auto *formShellLayout = new QVBoxLayout(formShell);
    formShellLayout->setContentsMargins(24, 18, 24, 12);
    formShellLayout->setSpacing(14);
    formShellLayout->addWidget(hintLabel);
    formShellLayout->addLayout(form);
    formShellLayout->addLayout(buttonRow);

    // 登录框使用同窗口内嵌键盘，不能使用独立 Tool 窗口。
    // dialog.exec() 会进入应用模态事件循环，独立键盘窗口在部分 X11/嵌入式环境下会被模态窗口拦截，表现为键盘可见但点不动。
    keyboardWidget_ = new QQuickWidget(this);
    keyboardWidget_->setObjectName("loginEmbeddedKeyboard");
    keyboardWidget_->setAttribute(Qt::WA_AcceptTouchEvents, true);
    keyboardWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    keyboardWidget_->setClearColor(QColor("#101a2a"));
    keyboardWidget_->setFocusPolicy(Qt::NoFocus);
    keyboardWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    keyboardWidget_->setFixedHeight(kLoginKeyboardHeight);
    keyboardWidget_->setSource(QUrl(QStringLiteral("qrc:/qml/EmbeddedVirtualKeyboard.qml")));
    keyboardWidget_->hide();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(formShell);
    layout->addStretch(1);
    layout->addWidget(keyboardWidget_);

    userEdit_->installEventFilter(this);
    passwordEdit_->installEventFilter(this);
    qApp->installEventFilter(this);

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
        "QDialog{background:#162236;color:#edf5f8;border:1px solid #30445f;border-radius:18px;}"
        "QLabel{color:#d8e5eb;font-size:16px;}"
        "QLineEdit{font-size:18px;min-height:42px;}"
        "QPushButton#dialogPrimaryButton,QPushButton#dialogCancelButton{min-width:132px;}"
    );
    setStyleSheet(style);
}

/** @brief 销毁前释放键盘焦点和资源。 */
AdminLoginDialog::~AdminLoginDialog()
{
    qApp->removeEventFilter(this);
    hideKeyboard();
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
        hideKeyboard();
        accept();
        return;
    }

    AppMessageDialog::warning(this, QStringLiteral("管理员登录"), QStringLiteral("用户名或密码错误"));
}

/** @brief 根据文本编辑器焦点和点击位置显示或隐藏内嵌键盘。 */
bool AdminLoginDialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == userEdit_ || watched == passwordEdit_) &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonRelease)) {
        QTimer::singleShot(0, this, &AdminLoginDialog::showKeyboard);
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QWidget *clicked = QApplication::widgetAt(mouse->globalPos());
        if (isVisible() && clicked && isAncestorOf(clicked) &&
            !isTextEditor(clicked) &&
            !(keyboardWidget_ && (clicked == keyboardWidget_ || keyboardWidget_->isAncestorOf(clicked)))) {
            hideKeyboard();
        }
    }
    return QDialog::eventFilter(watched, event);
}

/** @return widget 是可编辑文本输入控件时返回 true。 */
bool AdminLoginDialog::isTextEditor(QWidget *widget) const
{
    QWidget *cursor = widget;
    while (cursor && cursor != this) {
        if (cursor == userEdit_ || cursor == passwordEdit_) {
            return true;
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

/** @brief 显示并定位内嵌虚拟键盘。 */
void AdminLoginDialog::showKeyboard()
{
    if (keyboardWidget_) {
        positionKeyboard();
        keyboardWidget_->show();
        keyboardWidget_->raise();
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->show();
    }
}

/** @brief 隐藏内嵌虚拟键盘。 */
void AdminLoginDialog::hideKeyboard()
{
    if (keyboardWidget_) {
        keyboardWidget_->hide();
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->hide();
    }
}

/** @brief 根据对话框可用区域更新键盘几何位置。 */
void AdminLoginDialog::positionKeyboard()
{
    if (!keyboardWidget_) {
        return;
    }

    // 键盘在当前登录对话框布局内显示，只需要保持固定高度并刷新布局。
    keyboardWidget_->setFixedHeight(kLoginKeyboardHeight);
    keyboardWidget_->updateGeometry();
    adjustSize();
}
