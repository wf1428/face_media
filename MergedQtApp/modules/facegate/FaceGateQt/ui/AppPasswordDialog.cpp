/**
 * @file AppPasswordDialog.cpp
 * @brief 使用人脸模块共享虚拟键盘的统一密码输入弹窗实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AppPasswordDialog.h"
#include "AppMessageDialog.h"
#include "common/responsive_ui.h"

#include <QEvent>
#include <QFontMetrics>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

/** @brief 创建旧密码、新密码和确认密码输入界面。 */
AppPasswordDialog::AppPasswordDialog(const QString &title,
                                     const QString &description,
                                     QWidget *parent)
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
    }
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("AppPasswordDialog");
    if (!embeddedForWayland_) {
        const QSize targetSize = ResponsiveUi::dialogSize(
                    parent, QSize(680, 620), QSize(520, 540));
        setMinimumSize(qMin(520, targetSize.width()),
                       qMin(540, targetSize.height()));
        resize(targetSize);
    }

    buildUi(title, description);
    loadStyleSheet();

}

/** @brief 销毁前移除宿主尺寸监听并释放输入焦点。 */
AppPasswordDialog::~AppPasswordDialog()
{
    if (embeddedForWayland_ && parentWidget()) {
        parentWidget()->removeEventFilter(this);
    }
    if (oldPasswordEdit_) oldPasswordEdit_->clearFocus();
    if (newPasswordEdit_) newPasswordEdit_->clearFocus();
    if (confirmPasswordEdit_) confirmPasswordEdit_->clearFocus();
}

/** @return 旧密码输入。 */
QString AppPasswordDialog::oldPassword() const
{
    return oldPasswordEdit_ ? oldPasswordEdit_->text() : QString();
}

/** @return 新密码输入。 */
QString AppPasswordDialog::newPassword() const
{
    return newPasswordEdit_ ? newPasswordEdit_->text() : QString();
}

/** @return 新密码确认输入。 */
QString AppPasswordDialog::confirmPassword() const
{
    return confirmPasswordEdit_ ? confirmPasswordEdit_->text() : QString();
}

/** @brief 以模态方式收集并校验一次密码修改输入。 */
bool AppPasswordDialog::getPasswordChange(QWidget *parent,
                                          QString *oldPassword,
                                          QString *newPassword)
{
    AppPasswordDialog dialog(QStringLiteral("管理员密码修改"),
                             QStringLiteral("请输入当前密码，并设置新的管理员密码。"),
                             parent);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    if (oldPassword) {
        *oldPassword = dialog.oldPassword();
    }
    if (newPassword) {
        *newPassword = dialog.newPassword();
    }
    return true;
}

/** @brief 显示时稳定标题布局并聚焦第一个输入框。 */
void AppPasswordDialog::showEvent(QShowEvent *event)
{
    if (embeddedForWayland_ && parentWidget()) {
        setGeometry(parentWidget()->rect());
        if (keyboardClearance_) {
            keyboardClearance_->setFixedHeight(
                        ResponsiveUi::keyboardHeight(height(), 300));
        }
    }
    QDialog::showEvent(event);

    // 首次弹出时主动稳定标题/提示的高度，避免 Qt 5.12 在套用 QSS 后出现文字上下重叠。
    stabilizeHeaderLayout();
    QTimer::singleShot(0, this, [this]() {
        stabilizeHeaderLayout();
        update();
        repaint();
    });

    if (oldPasswordEdit_) {
        oldPasswordEdit_->setFocus();
        QTimer::singleShot(80, this, &AppPasswordDialog::ensureFocusedEditorVisible);
    }
}

/** @brief 创建密码表单和滚动区域。 */
void AppPasswordDialog::buildUi(const QString &title, const QString &description)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(10);

    contentScrollArea_ = new QScrollArea(this);
    contentScrollArea_->setObjectName("passwordContentScroll");
    contentScrollArea_->setWidgetResizable(true);
    contentScrollArea_->setFrameShape(QFrame::NoFrame);
    contentScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contentScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contentScrollArea_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentScrollArea_->setMinimumHeight(210);
    if (contentScrollArea_->viewport()) {
        contentScrollArea_->viewport()->setObjectName("passwordContentViewport");
        contentScrollArea_->viewport()->setAutoFillBackground(false);
    }

    auto *contentWidget = new QWidget(contentScrollArea_);
    contentWidget->setObjectName("passwordContentWidget");
    auto *contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    auto *card = new QFrame(contentWidget);
    card->setObjectName("passwordCard");
    if (embeddedForWayland_) {
        card->setMaximumWidth(680);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 22, 24, 18);
    cardLayout->setSpacing(14);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);

    auto *icon = new QLabel(card);
    icon->setObjectName("metricIcon");
    icon->setFixedSize(44, 44);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(QIcon(QStringLiteral(":/icons/action/lock.svg")).pixmap(24, 24));

    auto *titleBox = new QVBoxLayout();
    titleBox->setSpacing(4);
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("pageTitle");
    titleLabel->setTextFormat(Qt::PlainText);
    titleLabel->setMinimumHeight(36);
    auto *descLabel = new QLabel(description, card);
    descLabel->setObjectName("pageHint");
    descLabel->setWordWrap(true);
    descLabel->setTextFormat(Qt::PlainText);
    descLabel->setMinimumHeight(34);
    titleBox->addWidget(titleLabel);
    titleBox->addWidget(descLabel);

    auto *closeButton = new QPushButton(card);
    closeButton->setObjectName("dangerButton");
    closeButton->setIcon(QIcon(QStringLiteral(":/icons/action/x.svg")));
    closeButton->setIconSize(QSize(18, 18));
    closeButton->setFixedSize(42, 42);
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    header->addWidget(icon);
    header->addLayout(titleBox, 1);
    header->addWidget(closeButton);

    oldPasswordEdit_ = new QLineEdit(card);
    newPasswordEdit_ = new QLineEdit(card);
    confirmPasswordEdit_ = new QLineEdit(card);

    const QList<QLineEdit *> edits = {oldPasswordEdit_, newPasswordEdit_, confirmPasswordEdit_};
    for (QLineEdit *edit : edits) {
        edit->setEchoMode(QLineEdit::Password);
        edit->setAttribute(Qt::WA_InputMethodEnabled, true);
        edit->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhSensitiveData);
        edit->installEventFilter(this);
        edit->setMinimumHeight(44);
    }
    oldPasswordEdit_->setPlaceholderText(QStringLiteral("当前管理员密码"));
    newPasswordEdit_->setPlaceholderText(QStringLiteral("新管理员密码"));
    confirmPasswordEdit_->setPlaceholderText(QStringLiteral("再次输入新密码"));

    auto addField = [cardLayout, card](const QString &name, QLineEdit *edit) {
        auto *box = new QVBoxLayout();
        box->setSpacing(7);
        auto *label = new QLabel(name, card);
        label->setObjectName("metricName");
        box->addWidget(label);
        box->addWidget(edit);
        cardLayout->addLayout(box);
    };

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(12);
    buttonRow->addStretch();
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), card);
    cancelButton->setObjectName("backButton");
    cancelButton->setMinimumSize(110, 44);
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    auto *okButton = new QPushButton(QStringLiteral("确认修改"), card);
    okButton->setObjectName("toolButton");
    okButton->setIcon(QIcon(QStringLiteral(":/icons/action/check.svg")));
    okButton->setIconSize(QSize(18, 18));
    okButton->setMinimumSize(126, 44);
    okButton->setCursor(Qt::PointingHandCursor);
    connect(okButton, &QPushButton::clicked, this, &AppPasswordDialog::validateAndAccept);

    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(okButton);

    cardLayout->addLayout(header);
    addField(QStringLiteral("当前密码"), oldPasswordEdit_);
    addField(QStringLiteral("新密码"), newPasswordEdit_);
    addField(QStringLiteral("确认新密码"), confirmPasswordEdit_);
    cardLayout->addLayout(buttonRow);

    if (embeddedForWayland_) {
        contentLayout->addWidget(card, 0, Qt::AlignHCenter);
        keyboardClearance_ = new QWidget(contentWidget);
        keyboardClearance_->setFixedHeight(
                    ResponsiveUi::keyboardHeight(height(), 300));
        keyboardClearance_->setAttribute(Qt::WA_TransparentForMouseEvents);
        contentLayout->addWidget(keyboardClearance_);
    } else {
        contentLayout->addWidget(card);
    }
    contentLayout->addStretch(1);
    contentScrollArea_->setWidget(contentWidget);

    root->addWidget(contentScrollArea_, 1);
}

/** @brief 加载统一密码弹窗样式。 */
void AppPasswordDialog::loadStyleSheet()
{
    QString style;
    QFile adminStyle(QStringLiteral(":/qss/admin_panel.qss"));
    if (adminStyle.open(QIODevice::ReadOnly | QIODevice::Text)) {
        style += QString::fromUtf8(adminStyle.readAll());
    }
    QFile dialogStyle(QStringLiteral(":/qss/shadcn_dialog.qss"));
    if (dialogStyle.open(QIODevice::ReadOnly | QIODevice::Text)) {
        style += QString::fromUtf8(dialogStyle.readAll());
    }
    setStyleSheet(style);
}

/** @brief 滚动表单以露出当前聚焦输入框。 */
void AppPasswordDialog::ensureFocusedEditorVisible()
{
    if (!contentScrollArea_) {
        return;
    }

    QWidget *editor = nullptr;
    if (oldPasswordEdit_ && oldPasswordEdit_->hasFocus()) {
        editor = oldPasswordEdit_;
    } else if (newPasswordEdit_ && newPasswordEdit_->hasFocus()) {
        editor = newPasswordEdit_;
    } else if (confirmPasswordEdit_ && confirmPasswordEdit_->hasFocus()) {
        editor = confirmPasswordEdit_;
    }

    if (editor) {
        contentScrollArea_->ensureWidgetVisible(editor, 24, 24);
    }
}

/** @brief 在字体布局完成后固定标题区域高度。 */
void AppPasswordDialog::stabilizeHeaderLayout()
{
    const auto labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (!label) {
            continue;
        }
        const QString name = label->objectName();
        if (name != QLatin1String("pageTitle") && name != QLatin1String("pageHint")) {
            continue;
        }

        const int fallbackWidth = 460;
        const int availableWidth = qMax(80, label->width() > 0 ? label->width() : fallbackWidth);
        const QFontMetrics fm(label->font());
        const QRect textRect = fm.boundingRect(QRect(0, 0, availableWidth, 1200),
                                               Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter,
                                               label->text());
        const int baseHeight = name == QLatin1String("pageTitle") ? 40 : 36;
        const int targetHeight = qMax(baseHeight, textRect.height() + fm.lineSpacing() / 2);
        if (label->minimumHeight() != targetHeight) {
            label->setMinimumHeight(targetHeight);
        }
        label->updateGeometry();
    }

    if (layout()) {
        layout()->activate();
    }
}

/** @brief 校验非空、强度和两次新密码一致后接受对话框。 */
void AppPasswordDialog::validateAndAccept()
{
    if (oldPassword().isEmpty()) {
        AppMessageDialog::warning(this, QStringLiteral("管理员密码修改"), QStringLiteral("请输入当前管理员密码"));
        return;
    }
    if (newPassword().isEmpty()) {
        AppMessageDialog::warning(this, QStringLiteral("管理员密码修改"), QStringLiteral("请输入新管理员密码"));
        return;
    }
    if (newPassword() != confirmPassword()) {
        AppMessageDialog::warning(this, QStringLiteral("管理员密码修改"), QStringLiteral("两次输入的新密码不一致"));
        return;
    }
    oldPasswordEdit_->clearFocus();
    newPasswordEdit_->clearFocus();
    confirmPasswordEdit_->clearFocus();
    accept();
}

/** @brief 处理宿主尺寸变化并保证聚焦输入框可见。 */
bool AppPasswordDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (embeddedForWayland_ && watched == parentWidget() &&
            event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        if (keyboardClearance_) {
            keyboardClearance_->setFixedHeight(
                        ResponsiveUi::keyboardHeight(height(), 300));
        }
    }

    if ((watched == oldPasswordEdit_ || watched == newPasswordEdit_ || watched == confirmPasswordEdit_) &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonRelease)) {
        QTimer::singleShot(0, this, [this]() {
            ensureFocusedEditorVisible();
        });
    }
    return QDialog::eventFilter(watched, event);
}
