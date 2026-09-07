/**
 * @file InputCursorController.cpp
 * @brief 触摸屏与鼠标输入之间自动切换系统光标的实现。
 *
 * @author Dulin
 * @date 2026-09-02
 */

#include "InputCursorController.h"

#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QList>
#include <QMouseEvent>
#include <QTimer>
#include <QTouchDevice>
#include <QWheelEvent>

InputCursorController::InputCursorController(QObject *parent)
    : QObject(parent)
{
    if (qApp) {
        qApp->installEventFilter(this);
    }

    // EGLFS 输入设备通常已在 QApplication 构造期间注册；再延迟检查一次，
    // 兼容平台插件在首轮事件循环中才完成触摸设备注册的情况。
    applyInitialDeviceState();
    QTimer::singleShot(0, this, [this]() {
        if (!inputActivitySeen_) {
            applyInitialDeviceState();
        }
    });
}

InputCursorController::~InputCursorController()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
}

bool InputCursorController::hasTouchScreen() const
{
    const QList<const QTouchDevice *> devices = QTouchDevice::devices();
    for (const QTouchDevice *device : devices) {
        if (device && device->type() == QTouchDevice::TouchScreen) {
            return true;
        }
    }
    return false;
}

void InputCursorController::applyInitialDeviceState()
{
    const bool touchScreenDetected = hasTouchScreen();
    qInfo() << "[INPUT-CURSOR] touchscreen=" << touchScreenDetected;
    if (touchScreenDetected) {
        hideCursor();
    } else {
        showCursor();
    }
}

void InputCursorController::hideCursor()
{
    if (cursorHidden_) {
        return;
    }

    QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    cursorHidden_ = true;
    qInfo() << "[INPUT-CURSOR] hidden for touchscreen input";
}

void InputCursorController::showCursor()
{
    if (!cursorHidden_) {
        return;
    }

    QApplication::restoreOverrideCursor();
    cursorHidden_ = false;
    qInfo() << "[INPUT-CURSOR] visible for mouse input";
}

bool InputCursorController::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (!event) {
        return false;
    }

    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        inputActivitySeen_ = true;
        hideCursor();
        break;

    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        inputActivitySeen_ = true;
        if (mouseEvent->source() == Qt::MouseEventNotSynthesized) {
            showCursor();
        } else {
            // 某些 QWidget 不接收原始 QTouchEvent，Qt 会把触摸转换成鼠标事件。
            // 将这类事件明确视为触摸，避免鼠标刚显示后触摸滑动仍残留光标。
            hideCursor();
        }
        break;
    }

    case QEvent::Wheel: {
        const auto *wheelEvent = static_cast<QWheelEvent *>(event);
        inputActivitySeen_ = true;
        if (wheelEvent->source() == Qt::MouseEventNotSynthesized) {
            showCursor();
        } else {
            hideCursor();
        }
        break;
    }

    default:
        break;
    }

    // 只观察输入模式，不拦截或改变原有触摸、鼠标事件分发。
    return false;
}
