/**
 * @file AppMessageDialog.cpp
 * @brief 实现项目统一消息和确认弹窗，避免依赖系统 QMessageBox。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AppMessageDialog.h"

#include <QApplication>
#include <QColor>
#include <QDesktopWidget>
#include <QFile>
#include <QFrame>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

/** @brief 构造提示、确认或直接选项弹窗并按类型建立界面。 */
AppMessageDialog::AppMessageDialog(Type type,
                                   const QString &title,
                                   const QString &message,
                                   QWidget *parent,
                                   bool confirmMode,
                                   const QString &confirmText,
                                   const QString &cancelText,
                                   const QStringList &choiceTexts)
    : QDialog(parent),
      type_(type),
      confirmMode_(confirmMode),
      confirmText_(confirmText),
      cancelText_(cancelText),
      choiceTexts_(choiceTexts)
{
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setObjectName("AppMessageDialog");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    buildUi(title, message);
    loadStyleSheet();
}

/** @brief 显示阻塞式信息提示。 */
void AppMessageDialog::information(QWidget *parent, const QString &title, const QString &message)
{
    AppMessageDialog dialog(Type::Info, title, message, parent);
    dialog.exec();
}

/** @brief 显示阻塞式成功提示。 */
void AppMessageDialog::success(QWidget *parent, const QString &title, const QString &message)
{
    AppMessageDialog dialog(Type::Success, title, message, parent);
    dialog.exec();
}

/** @brief 显示阻塞式警告提示。 */
void AppMessageDialog::warning(QWidget *parent, const QString &title, const QString &message)
{
    AppMessageDialog dialog(Type::Warning, title, message, parent);
    dialog.exec();
}

/** @brief 显示阻塞式错误提示。 */
void AppMessageDialog::error(QWidget *parent, const QString &title, const QString &message)
{
    AppMessageDialog dialog(Type::Error, title, message, parent);
    dialog.exec();
}

/** @return 用户选择确认时返回 true。 */
bool AppMessageDialog::confirm(QWidget *parent,
                               const QString &title,
                               const QString &message,
                               const QString &confirmText,
                               const QString &cancelText)
{
    AppMessageDialog dialog(Type::Question, title, message, parent, true, confirmText, cancelText);
    return dialog.exec() == QDialog::Accepted;
}

/** @return 用户选择的选项序号；关闭或取消时返回 -1。 */
int AppMessageDialog::choose(QWidget *parent,
                             const QString &title,
                             const QString &message,
                             const QStringList &choices,
                             const QString &cancelText)
{
    // 选项直接位于同一个模态窗口中，避免 QComboBox 创建 Qt::Popup
    // 并抢占 EGLFS 下的软件鼠标事件。
    AppMessageDialog dialog(Type::Question, title, message, parent,
                            false, QString(), cancelText, choices);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedChoice_ : -1;
}

/** @brief 显示时稳定文本布局并居中到父窗口。 */
void AppMessageDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    // Qt 5.12 在无边框弹窗首次显示时，QSS 字体和 word-wrap 标签高度偶发没有立即生效。
    // 先主动重算一次，再在事件循环下一拍重算并重绘，避免文字初次弹出时上下重叠。
    stabilizeTextLayout();
    centerOnParent();
    QTimer::singleShot(0, this, [this]() {
        stabilizeTextLayout();
        centerOnParent();
        update();
        repaint();
    });
}

/** @return 指定类型对应的资源图标路径。 */
QString AppMessageDialog::iconPath(Type type)
{
    switch (type) {
    case Type::Success: return QStringLiteral(":/icons/dialog/success.svg");
    case Type::Warning: return QStringLiteral(":/icons/dialog/warning.svg");
    case Type::Error: return QStringLiteral(":/icons/dialog/error.svg");
    case Type::Question: return QStringLiteral(":/icons/dialog/question.svg");
    case Type::Info:
    default: return QStringLiteral(":/icons/dialog/info.svg");
    }
}

/** @return 指定类型的样式名称。 */
QString AppMessageDialog::typeName(Type type)
{
    switch (type) {
    case Type::Success: return QStringLiteral("success");
    case Type::Warning: return QStringLiteral("warning");
    case Type::Error: return QStringLiteral("error");
    case Type::Question: return QStringLiteral("question");
    case Type::Info:
    default: return QStringLiteral("info");
    }
}

/** @return 指定类型默认确认按钮文本。 */
QString AppMessageDialog::defaultAcceptText(Type type)
{
    switch (type) {
    case Type::Question: return QStringLiteral("确定");
    case Type::Error: return QStringLiteral("知道了");
    case Type::Warning: return QStringLiteral("知道了");
    case Type::Success: return QStringLiteral("完成");
    case Type::Info:
    default: return QStringLiteral("确定");
    }
}

/** @brief 创建标题、消息和触摸友好按钮。 */
void AppMessageDialog::buildUi(const QString &title, const QString &message)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);

    auto *card = new QFrame(this);
    card->setObjectName("dialogCard");
    card->setProperty("dialogType", typeName(type_));
    card->setMinimumWidth(460);
    card->setMaximumWidth(560);

    auto *shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(36);
    shadow->setOffset(0, 18);
    shadow->setColor(QColor(0, 0, 0, 150));
    card->setGraphicsEffect(shadow);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 22, 24, 22);
    cardLayout->setSpacing(18);

    auto *headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(14);

    auto *iconLabel = new QLabel(card);
    iconLabel->setObjectName("dialogIcon");
    iconLabel->setProperty("dialogType", typeName(type_));
    iconLabel->setFixedSize(44, 44);
    iconLabel->setPixmap(QIcon(iconPath(type_)).pixmap(24, 24));
    iconLabel->setAlignment(Qt::AlignCenter);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("dialogTitle");
    titleLabel->setWordWrap(true);
    titleLabel->setTextFormat(Qt::PlainText);
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    titleLabel->setMinimumHeight(36);

    auto *closeButton = new QPushButton(card);
    closeButton->setObjectName("dialogCloseButton");
    closeButton->setIcon(QIcon(QStringLiteral(":/icons/dialog/close.svg")));
    closeButton->setIconSize(QSize(18, 18));
    closeButton->setFixedSize(38, 38);
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    headerRow->addWidget(iconLabel);
    headerRow->addWidget(titleLabel, 1);
    headerRow->addWidget(closeButton);

    auto *messageLabel = new QLabel(message, card);
    messageLabel->setObjectName("dialogMessage");
    messageLabel->setWordWrap(true);
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    messageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    messageLabel->setMinimumHeight(54);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 4, 0, 0);
    buttonRow->setSpacing(12);
    buttonRow->addStretch();

    if (!choiceTexts_.isEmpty()) {
        auto *cancelButton = new QPushButton(cancelText_.isEmpty() ? QStringLiteral("取消") : cancelText_, card);
        cancelButton->setObjectName("dialogCancelButton");
        cancelButton->setCursor(Qt::PointingHandCursor);
        cancelButton->setMinimumSize(90, 46);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        buttonRow->addWidget(cancelButton);

        for (int index = 0; index < choiceTexts_.size(); ++index) {
            auto *choiceButton = new QPushButton(choiceTexts_.at(index), card);
            choiceButton->setObjectName("dialogPrimaryButton");
            choiceButton->setProperty("dialogType", typeName(type_));
            choiceButton->setCursor(Qt::PointingHandCursor);
            choiceButton->setMinimumSize(104, 46);
            connect(choiceButton, &QPushButton::clicked, this, [this, index]() {
                selectedChoice_ = index;
                accept();
            });
            buttonRow->addWidget(choiceButton);
        }
    } else if (confirmMode_) {
        auto *cancelButton = new QPushButton(cancelText_.isEmpty() ? QStringLiteral("取消") : cancelText_, card);
        cancelButton->setObjectName("dialogCancelButton");
        cancelButton->setCursor(Qt::PointingHandCursor);
        cancelButton->setMinimumSize(110, 46);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        buttonRow->addWidget(cancelButton);
    }

    if (choiceTexts_.isEmpty()) {
        auto *acceptButton = new QPushButton(confirmMode_
            ? (confirmText_.isEmpty() ? QStringLiteral("确定") : confirmText_)
            : defaultAcceptText(type_), card);
        acceptButton->setObjectName(confirmMode_ ? "dialogDangerButton" : "dialogPrimaryButton");
        acceptButton->setProperty("dialogType", typeName(type_));
        acceptButton->setCursor(Qt::PointingHandCursor);
        acceptButton->setMinimumSize(112, 46);
        connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
        buttonRow->addWidget(acceptButton);
    }

    cardLayout->addLayout(headerRow);
    cardLayout->addWidget(messageLabel);
    cardLayout->addLayout(buttonRow);

    rootLayout->addWidget(card);
    setLayout(rootLayout);
    adjustSize();
}

/** @brief 加载项目统一弹窗样式表。 */
void AppMessageDialog::loadStyleSheet()
{
    QFile file(QStringLiteral(":/qss/shadcn_dialog.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStyleSheet(QString::fromUtf8(file.readAll()));
    }
}

/** @brief 将弹窗移动到父窗口或屏幕中心。 */
void AppMessageDialog::centerOnParent()
{
    QRect targetRect;
    if (parentWidget()) {
        targetRect = parentWidget()->window()->frameGeometry();
    } else {
        targetRect = QApplication::desktop()->availableGeometry(this);
    }
    move(targetRect.center() - rect().center());
}

/** @brief 在字体与窗口几何稳定后重新计算文本换行。 */
void AppMessageDialog::stabilizeTextLayout()
{
    const auto labels = findChildren<QLabel *>();
    for (QLabel *label : labels) {
        if (!label) {
            continue;
        }
        const QString name = label->objectName();
        if (name != QLatin1String("dialogTitle") && name != QLatin1String("dialogMessage")) {
            continue;
        }

        const int fallbackWidth = name == QLatin1String("dialogTitle") ? 380 : 500;
        const int availableWidth = qMax(80, label->width() > 0 ? label->width() : fallbackWidth);
        const QFontMetrics fm(label->font());
        const QRect textRect = fm.boundingRect(QRect(0, 0, availableWidth, 2000),
                                               Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter,
                                               label->text());
        const int baseHeight = name == QLatin1String("dialogTitle") ? 36 : 54;
        const int targetHeight = qMax(baseHeight, textRect.height() + fm.lineSpacing());
        if (label->minimumHeight() != targetHeight) {
            label->setMinimumHeight(targetHeight);
        }
        label->updateGeometry();
    }

    if (layout()) {
        layout()->activate();
    }
    adjustSize();
}
