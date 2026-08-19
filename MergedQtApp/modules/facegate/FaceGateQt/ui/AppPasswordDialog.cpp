/**
 * @file AppPasswordDialog.cpp
 * @brief 项目统一密码输入弹窗，内部集成虚拟键盘的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AppPasswordDialog.h"
#include "AppMessageDialog.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputMethod>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMouseEvent>
#include <QPushButton>
#include <QQuickWidget>
#include <QScrollArea>
#include <QShowEvent>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const int kPasswordKeyboardHeight = 280;
}

/** @brief 创建旧密码、新密码和确认密码输入界面。 */
AppPasswordDialog::AppPasswordDialog(const QString &title,
                                     const QString &description,
                                     QWidget *parent)
    : QDialog(parent)
{
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("AppPasswordDialog");
    setMinimumSize(640, 600);
    resize(680, 620);

    buildUi(title, description);
    loadStyleSheet();

    qApp->installEventFilter(this);
}

/** @brief 销毁前释放虚拟键盘资源。 */
AppPasswordDialog::~AppPasswordDialog()
{
    qApp->removeEventFilter(this);
    hideKeyboard();
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

/** @brief 显示时稳定标题布局和键盘几何位置。 */
void AppPasswordDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    // 首次弹出时主动稳定标题/提示的高度，避免 Qt 5.12 在套用 QSS 后出现文字上下重叠。
    stabilizeHeaderLayout();
    updateKeyboardGeometry();
    QTimer::singleShot(0, this, [this]() {
        stabilizeHeaderLayout();
        updateKeyboardGeometry();
        update();
        repaint();
    });

    if (oldPasswordEdit_) {
        oldPasswordEdit_->setFocus();
        QTimer::singleShot(80, this, &AppPasswordDialog::showKeyboard);
    }
}

/** @brief 创建密码表单、滚动区域和虚拟键盘。 */
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
    contentScrollArea_->setMinimumHeight(260);
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

    keyboardWidget_ = new QQuickWidget(this);
    keyboardWidget_->setObjectName("passwordEmbeddedKeyboard");
    keyboardWidget_->setAttribute(Qt::WA_AcceptTouchEvents, true);
    keyboardWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    keyboardWidget_->setClearColor(QColor("#101a2a"));
    keyboardWidget_->setFocusPolicy(Qt::NoFocus);
    keyboardWidget_->setFixedHeight(kPasswordKeyboardHeight);
    keyboardWidget_->setSource(QUrl(QStringLiteral("qrc:/qml/EmbeddedVirtualKeyboard.qml")));
    keyboardWidget_->hide();

    contentLayout->addWidget(card);
    contentScrollArea_->setWidget(contentWidget);

    root->addWidget(contentScrollArea_, 1);
    root->addWidget(keyboardWidget_, 0);
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

/** @brief 显示虚拟键盘并保证当前编辑器可见。 */
void AppPasswordDialog::showKeyboard()
{
    if (keyboardWidget_) {
        updateKeyboardGeometry();
        keyboardWidget_->show();
        keyboardWidget_->raise();
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->show();
    }

    QTimer::singleShot(0, this, [this]() {
        ensureFocusedEditorVisible();
    });
}

/** @brief 隐藏虚拟键盘。 */
void AppPasswordDialog::hideKeyboard()
{
    if (keyboardWidget_) {
        keyboardWidget_->hide();
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->hide();
    }
    if (layout()) {
        layout()->activate();
    }
}

/** @brief 根据弹窗可用高度调整键盘区域。 */
void AppPasswordDialog::updateKeyboardGeometry()
{
    if (!keyboardWidget_) {
        return;
    }
    keyboardWidget_->setFixedHeight(kPasswordKeyboardHeight);
    keyboardWidget_->updateGeometry();
    if (layout()) {
        layout()->activate();
    }
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

/** @return widget 是密码文本编辑器时返回 true。 */
bool AppPasswordDialog::isTextEditor(QWidget *widget) const
{
    QWidget *cursor = widget;
    while (cursor && cursor != this) {
        if (cursor == oldPasswordEdit_ || cursor == newPasswordEdit_ || cursor == confirmPasswordEdit_) {
            return true;
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

/** @return widget 属于内嵌键盘对象树时返回 true。 */
bool AppPasswordDialog::isKeyboardWidget(QWidget *widget) const
{
    return keyboardWidget_
        && widget
        && (widget == keyboardWidget_ || keyboardWidget_->isAncestorOf(widget));
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
    hideKeyboard();
    accept();
}

/** @brief 管理文本焦点、键盘点击和键盘可见性。 */
bool AppPasswordDialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == oldPasswordEdit_ || watched == newPasswordEdit_ || watched == confirmPasswordEdit_) &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonRelease)) {
        QTimer::singleShot(0, this, [this]() {
            showKeyboard();
            ensureFocusedEditorVisible();
        });
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QWidget *clicked = QApplication::widgetAt(mouse->globalPos());
        if (isVisible() && clicked && isAncestorOf(clicked) &&
            !isTextEditor(clicked) && !isKeyboardWidget(clicked)) {
            hideKeyboard();
        }
    }
    return QDialog::eventFilter(watched, event);
}
