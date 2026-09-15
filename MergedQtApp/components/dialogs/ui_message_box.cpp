/**
 * @file ui_message_box.cpp
 * @brief 实现不依赖 Wayland 顶层窗口坐标的应用内消息覆盖层。
 */

#include "ui_message_box.h"

#include "common/responsive_ui.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace {

class EmbeddedMessageDialog final : public QDialog
{
public:
    EmbeddedMessageDialog(QWidget *parent,
                          QMessageBox::Icon icon,
                          const QString &title,
                          const QString &text,
                          QMessageBox::StandardButtons buttons,
                          QMessageBox::StandardButton defaultButton)
        : QDialog(ResponsiveUi::overlayHost(parent)),
          buttons_(buttons)
    {
        setWindowFlags(Qt::Widget);
        setModal(true);
        setObjectName(QStringLiteral("embeddedMessageOverlay"));
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral(
            "QDialog#embeddedMessageOverlay{background:rgba(0,0,0,105);}"
            "QWidget#messageCard{background:#ffffff;border:1px solid #dcdfe6;"
            "border-radius:14px;}"
            "QLabel#messageTitle{color:#303133;font-size:20px;font-weight:600;}"
            "QLabel#messageText{color:#4b5563;font-size:16px;}"
            "QPushButton{min-width:96px;min-height:42px;padding:0 16px;"
            "border:1px solid #dcdfe6;border-radius:8px;background:#ffffff;"
            "color:#4b5563;font-size:15px;}"
            "QPushButton:hover{border-color:#409eff;color:#409eff;background:#ecf5ff;}"
            "QPushButton[primary=\"true\"]{background:#409eff;border-color:#409eff;color:white;}"
            "QPushButton[destructive=\"true\"]{background:#ef4444;border-color:#ef4444;color:white;}"));

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(24, 24, 24, 24);
        root->addStretch(1);

        auto *card = new QWidget(this);
        card->setObjectName(QStringLiteral("messageCard"));
        card->setAttribute(Qt::WA_StyledBackground, true);
        card->setMaximumWidth(560);
        card->setMinimumWidth(360);

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(24, 22, 24, 22);
        cardLayout->setSpacing(18);

        auto *header = new QHBoxLayout();
        header->setSpacing(14);

        auto *iconLabel = new QLabel(card);
        iconLabel->setFixedSize(42, 42);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setPixmap(iconPixmap(icon));

        auto *titleLabel = new QLabel(title, card);
        titleLabel->setObjectName(QStringLiteral("messageTitle"));
        titleLabel->setWordWrap(true);
        titleLabel->setTextFormat(Qt::PlainText);
        header->addWidget(iconLabel);
        header->addWidget(titleLabel, 1);
        cardLayout->addLayout(header);

        auto *messageLabel = new QLabel(text, card);
        messageLabel->setObjectName(QStringLiteral("messageText"));
        messageLabel->setWordWrap(true);
        messageLabel->setTextFormat(Qt::PlainText);
        messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(messageLabel);

        auto *buttonRow = new QHBoxLayout();
        buttonRow->setSpacing(12);
        buttonRow->addStretch(1);
        addButtons(buttonRow, card, buttons, defaultButton, icon);
        cardLayout->addLayout(buttonRow);

        root->addWidget(card, 0, Qt::AlignHCenter);
        root->addStretch(1);

        if (parentWidget()) {
            parentWidget()->installEventFilter(this);
            setGeometry(parentWidget()->rect());
        }
    }

    QMessageBox::StandardButton clickedButton() const
    {
        if (clickedButton_ != QMessageBox::NoButton) {
            return clickedButton_;
        }
        if (buttons_.testFlag(QMessageBox::Cancel)) {
            return QMessageBox::Cancel;
        }
        if (buttons_.testFlag(QMessageBox::No)) {
            return QMessageBox::No;
        }
        return QMessageBox::NoButton;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return QDialog::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        if (parentWidget()) {
            setGeometry(parentWidget()->rect());
        }
        QDialog::showEvent(event);
        raise();
    }

private:
    static QPixmap iconPixmap(QMessageBox::Icon icon)
    {
        QStyle::StandardPixmap standardIcon = QStyle::SP_MessageBoxInformation;
        switch (icon) {
        case QMessageBox::Warning:
            standardIcon = QStyle::SP_MessageBoxWarning;
            break;
        case QMessageBox::Critical:
            standardIcon = QStyle::SP_MessageBoxCritical;
            break;
        case QMessageBox::Question:
            standardIcon = QStyle::SP_MessageBoxQuestion;
            break;
        case QMessageBox::Information:
        default:
            break;
        }
        return QApplication::style()->standardIcon(standardIcon).pixmap(32, 32);
    }

    static QString buttonText(QMessageBox::StandardButton button)
    {
        switch (button) {
        case QMessageBox::Ok: return QStringLiteral("确定");
        case QMessageBox::Save: return QStringLiteral("保存");
        case QMessageBox::Cancel: return QStringLiteral("取消");
        case QMessageBox::Close: return QStringLiteral("关闭");
        case QMessageBox::Yes: return QStringLiteral("是");
        case QMessageBox::No: return QStringLiteral("否");
        case QMessageBox::Abort: return QStringLiteral("中止");
        case QMessageBox::Retry: return QStringLiteral("重试");
        case QMessageBox::Ignore: return QStringLiteral("忽略");
        default: return QStringLiteral("确定");
        }
    }

    void addButtons(QHBoxLayout *layout,
                    QWidget *parent,
                    QMessageBox::StandardButtons buttons,
                    QMessageBox::StandardButton defaultButton,
                    QMessageBox::Icon icon)
    {
        const QMessageBox::StandardButton order[] = {
            QMessageBox::Cancel, QMessageBox::No, QMessageBox::Close,
            QMessageBox::Abort, QMessageBox::Ignore, QMessageBox::Retry,
            QMessageBox::Save, QMessageBox::Yes, QMessageBox::Ok
        };

        QPushButton *fallbackDefault = nullptr;
        for (QMessageBox::StandardButton standardButton : order) {
            if (!buttons.testFlag(standardButton)) {
                continue;
            }
            auto *button = new QPushButton(buttonText(standardButton), parent);
            const bool affirmative = standardButton == QMessageBox::Ok ||
                                     standardButton == QMessageBox::Yes ||
                                     standardButton == QMessageBox::Save ||
                                     standardButton == QMessageBox::Retry;
            button->setProperty("primary", affirmative);
            button->setProperty("destructive",
                                icon == QMessageBox::Critical && affirmative);
            connect(button, &QPushButton::clicked, this, [this, standardButton]() {
                clickedButton_ = standardButton;
                accept();
            });
            layout->addWidget(button);
            if (affirmative) {
                fallbackDefault = button;
            }
            if (standardButton == defaultButton) {
                button->setDefault(true);
                button->setFocus(Qt::OtherFocusReason);
                fallbackDefault = nullptr;
            }
        }

        if (fallbackDefault && defaultButton == QMessageBox::NoButton) {
            fallbackDefault->setDefault(true);
            fallbackDefault->setFocus(Qt::OtherFocusReason);
        }
    }

    QMessageBox::StandardButtons buttons_;
    QMessageBox::StandardButton clickedButton_ = QMessageBox::NoButton;
};

QMessageBox::StandardButton showMessage(QWidget *parent,
                                        QMessageBox::Icon icon,
                                        const QString &title,
                                        const QString &text,
                                        QMessageBox::StandardButtons buttons,
                                        QMessageBox::StandardButton defaultButton)
{
    EmbeddedMessageDialog dialog(parent, icon, title, text, buttons, defaultButton);
    dialog.exec();
    return dialog.clickedButton();
}

} // namespace

namespace UiMessageBox {

QMessageBox::StandardButton information(QWidget *parent, const QString &title,
                                        const QString &text,
                                        QMessageBox::StandardButtons buttons,
                                        QMessageBox::StandardButton defaultButton)
{
    if (!ResponsiveUi::isWaylandPlatform()) {
        return QMessageBox::information(parent, title, text, buttons, defaultButton);
    }
    return showMessage(parent, QMessageBox::Information, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton warning(QWidget *parent, const QString &title,
                                    const QString &text,
                                    QMessageBox::StandardButtons buttons,
                                    QMessageBox::StandardButton defaultButton)
{
    if (!ResponsiveUi::isWaylandPlatform()) {
        return QMessageBox::warning(parent, title, text, buttons, defaultButton);
    }
    return showMessage(parent, QMessageBox::Warning, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton critical(QWidget *parent, const QString &title,
                                     const QString &text,
                                     QMessageBox::StandardButtons buttons,
                                     QMessageBox::StandardButton defaultButton)
{
    if (!ResponsiveUi::isWaylandPlatform()) {
        return QMessageBox::critical(parent, title, text, buttons, defaultButton);
    }
    return showMessage(parent, QMessageBox::Critical, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton question(QWidget *parent, const QString &title,
                                     const QString &text,
                                     QMessageBox::StandardButtons buttons,
                                     QMessageBox::StandardButton defaultButton)
{
    if (!ResponsiveUi::isWaylandPlatform()) {
        return QMessageBox::question(parent, title, text, buttons, defaultButton);
    }
    return showMessage(parent, QMessageBox::Question, title, text, buttons, defaultButton);
}

} // namespace UiMessageBox
