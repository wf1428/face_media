/**
 * @file SharedVirtualKeyboard.cpp
 * @brief 人脸模块长期复用的唯一 Qt Virtual Keyboard 宿主实现。
 *
 * @author Dulin
 * @date 2026-09-10
 */

#include "SharedVirtualKeyboard.h"

#include "common/responsive_ui.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QInputMethod>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QtQml/QQmlError>
#include <QQuickWidget>
#include <QTimer>
#include <QTextEdit>
#include <QUrl>
#include <QVariant>
#include <QWidget>

namespace {
constexpr int kSharedKeyboardHeight = 300;
}

/** @brief 创建唯一 QQuickWidget 并监听人脸窗口内的输入焦点。 */
SharedVirtualKeyboard::SharedVirtualKeyboard(QWidget *host)
    : QObject(host), host_(host)
{
    if (!host_) {
        return;
    }

    keyboard_ = new QQuickWidget(host_);
    keyboard_->setObjectName(QStringLiteral("faceSharedVirtualKeyboard"));
    keyboard_->setAttribute(Qt::WA_AcceptTouchEvents, true);
    keyboard_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    keyboard_->setClearColor(QColor(QStringLiteral("#101820")));
    keyboard_->setFocusPolicy(Qt::NoFocus);
    keyboard_->setSource(
                QUrl(QStringLiteral("qrc:/qml/EmbeddedVirtualKeyboard.qml")));
    keyboard_->hide();

    qInfo().noquote() << "[FACE-KEYBOARD] shared QML keyboard status="
                      << keyboard_->status()
                      << "errors=" << keyboard_->errors().size();

    host_->installEventFilter(this);
    qApp->installEventFilter(this);

    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *now) {
        if (now && belongsToHost(now) && isTextEditor(now)) {
            activeEditor_ = now;
            QTimer::singleShot(0, this, [this]() {
                showForEditor(activeEditor_);
            });
            return;
        }

        if (now && !belongsToHost(now)) {
            // 焦点已经切到多媒体等其他模块时，只收起本模块面板，不改变
            // 全局输入法状态，避免干扰其他模块自己的输入方案。
            activeEditor_.clear();
            if (keyboard_) {
                keyboard_->hide();
            }
            return;
        }

        if (!now || (belongsToHost(now) && !isKeyboardWidget(now))) {
            activeEditor_.clear();
            hideKeyboard();
        }
    });

    if (QGuiApplication::inputMethod()) {
        connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged,
                this, [this]() {
            if (changingVisibility_ || !QGuiApplication::inputMethod()) {
                return;
            }
            if (!QGuiApplication::inputMethod()->isVisible()) {
                if (keyboard_) {
                    keyboard_->hide();
                }
                return;
            }
            if (activeEditor_ && isTextEditor(activeEditor_)) {
                showForEditor(activeEditor_);
            }
        });
    }

    updateGeometry();
}

/** @brief 移除事件监听并释放唯一键盘控件。 */
SharedVirtualKeyboard::~SharedVirtualKeyboard()
{
    qApp->removeEventFilter(this);
    if (host_) {
        host_->removeEventFilter(this);
    }
    delete keyboard_;
    keyboard_ = nullptr;
}

/** @brief 按当前宿主尺寸把键盘固定到窗口底部。 */
void SharedVirtualKeyboard::updateGeometry()
{
    if (!host_ || !keyboard_) {
        return;
    }

    const QRect area = host_->rect();
    const int height = qMin(
                area.height(),
                ResponsiveUi::keyboardHeight(area.height(),
                                             kSharedKeyboardHeight));
    keyboard_->setGeometry(0, qMax(0, area.height() - height),
                           area.width(), height);
    keyboard_->setFixedHeight(height);
    if (keyboard_->isVisible()) {
        keyboard_->raise();
    }
}

/** @brief 隐藏键盘并通知 Qt 输入法收起。 */
void SharedVirtualKeyboard::hideKeyboard()
{
    if (keyboard_) {
        keyboard_->hide();
    }
    if (changingVisibility_ || !QGuiApplication::inputMethod() ||
            !QGuiApplication::inputMethod()->isVisible()) {
        return;
    }

    changingVisibility_ = true;
    QGuiApplication::inputMethod()->hide();
    changingVisibility_ = false;
}

/** @return 键盘面板当前可见时返回 true。 */
bool SharedVirtualKeyboard::isVisible() const
{
    return keyboard_ && keyboard_->isVisible();
}

/** @brief 处理宿主尺寸、输入焦点和点击位置变化。 */
bool SharedVirtualKeyboard::eventFilter(QObject *watched, QEvent *event)
{
    if (!event) {
        return QObject::eventFilter(watched, event);
    }

    if (watched == host_ && event->type() == QEvent::Resize) {
        updateGeometry();
        if (keyboard_ && keyboard_->isVisible()) {
            keyboard_->raise();
        }
    } else if (watched == host_ && event->type() == QEvent::Hide) {
        activeEditor_.clear();
        hideKeyboard();
    }

    QWidget *widget = qobject_cast<QWidget *>(watched);
    if (event->type() == QEvent::Hide && widget && activeEditor_ &&
            (widget == activeEditor_ || widget->isAncestorOf(activeEditor_))) {
        activeEditor_.clear();
        hideKeyboard();
    } else if (event->type() == QEvent::MouseButtonRelease &&
            widget && belongsToHost(widget) && isTextEditor(widget)) {
        activeEditor_ = widget;
        QTimer::singleShot(0, this, [this]() {
            showForEditor(activeEditor_);
        });
    } else if (event->type() == QEvent::MouseButtonPress &&
               widget && belongsToHost(widget) &&
               !isTextEditor(widget) && !isKeyboardWidget(widget) &&
               keyboard_ && keyboard_->isVisible()) {
        QTimer::singleShot(0, this, [this]() {
            QWidget *focus = QApplication::focusWidget();
            if (!focus || !belongsToHost(focus) || !isTextEditor(focus)) {
                activeEditor_.clear();
                hideKeyboard();
            }
        });
    }

    return QObject::eventFilter(watched, event);
}

/** @brief 为当前输入控件显示并置顶键盘。 */
void SharedVirtualKeyboard::showForEditor(QWidget *editor)
{
    if (!host_ || !keyboard_ || !editor || !host_->isVisible() ||
            !belongsToHost(editor) || !isTextEditor(editor)) {
        return;
    }

    activeEditor_ = editor;
    updateGeometry();
    keyboard_->show();
    keyboard_->raise();

    if (!changingVisibility_ && QGuiApplication::inputMethod() &&
            !QGuiApplication::inputMethod()->isVisible()) {
        changingVisibility_ = true;
        QGuiApplication::inputMethod()->show();
        changingVisibility_ = false;
    }
}

/** @return widget 属于人脸根窗口对象树时返回 true。 */
bool SharedVirtualKeyboard::belongsToHost(QWidget *widget) const
{
    return host_ && widget &&
            (widget == host_ || host_->isAncestorOf(widget));
}

/** @return widget 或其父控件是可编辑输入控件时返回 true。 */
bool SharedVirtualKeyboard::isTextEditor(QWidget *widget) const
{
    QWidget *cursor = widget;
    while (cursor && cursor != host_) {
        if (cursor->isEnabled() &&
                cursor->testAttribute(Qt::WA_InputMethodEnabled)) {
            if (QLineEdit *lineEdit = qobject_cast<QLineEdit *>(cursor)) {
                return !lineEdit->isReadOnly();
            }
            if (QAbstractSpinBox *spinBox = qobject_cast<QAbstractSpinBox *>(cursor)) {
                return !spinBox->isReadOnly();
            }
            if (QTextEdit *textEdit = qobject_cast<QTextEdit *>(cursor)) {
                return !textEdit->isReadOnly();
            }
            if (QPlainTextEdit *plainTextEdit = qobject_cast<QPlainTextEdit *>(cursor)) {
                return !plainTextEdit->isReadOnly();
            }
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

/** @return widget 属于共享键盘对象树时返回 true。 */
bool SharedVirtualKeyboard::isKeyboardWidget(QWidget *widget) const
{
    return keyboard_ && widget &&
            (widget == keyboard_ || keyboard_->isAncestorOf(widget));
}
