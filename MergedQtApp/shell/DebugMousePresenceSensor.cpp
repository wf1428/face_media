/**
 * @file DebugMousePresenceSensor.cpp
 * @brief 使用鼠标事件模拟人员存在状态的调试传感器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "DebugMousePresenceSensor.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

/**
 * @brief 创建鼠标调试传感器。
 * @param multimediaRoot 允许触发长按检测的多媒体根窗口。
 * @param leftHoldEnabled 是否启用左键长按检测。
 * @param holdDurationMs 触发检测所需持续时间，单位 ms，最小按 100 ms 处理。
 * @param parent Qt 父对象。
 */
DebugMousePresenceSensor::DebugMousePresenceSensor(QWidget *multimediaRoot,
                                                   bool leftHoldEnabled,
                                                   int holdDurationMs,
                                                   QObject *parent)
    : IPresenceSensor(parent),
      multimediaRoot_(multimediaRoot),
      holdDurationMs_(qMax(100, holdDurationMs)),
      leftHoldEnabled_(leftHoldEnabled)
{
    holdTimer_ = new QTimer(this);
    holdTimer_->setSingleShot(true);
    connect(holdTimer_, &QTimer::timeout, this, [this]() {
        if (!running_ || !multimediaActive_ ||
            !leftHoldEnabled_ || !holdingLeftButton_) {
            cancelHold("timeout conditions changed");
            return;
        }

        holdingLeftButton_ = false;
        qInfo() << "[PRESENCE] left-button hold completed"
                << "durationMs=" << holdDurationMs_;
        emit presenceDetected();
    });
}

/** @brief 销毁前移除全局事件过滤器并停止定时器。 */
DebugMousePresenceSensor::~DebugMousePresenceSensor()
{
    stop();
}

/** @brief 在 QApplication 上安装事件过滤器。 */
bool DebugMousePresenceSensor::start()
{
    if (running_) {
        return true;
    }
    if (!qApp || !multimediaRoot_) {
        emit sensorFault(QStringLiteral("debug mouse sensor has no application or multimedia root"));
        return false;
    }

    qApp->installEventFilter(this);
    running_ = true;
    cancelHold("start");
    qInfo() << "[PRESENCE] debug mouse driver started"
            << "leftHoldEnabled=" << leftHoldEnabled_
            << "holdDurationMs=" << holdDurationMs_;
    return true;
}

/** @brief 移除事件过滤器并取消尚未完成的长按。 */
void DebugMousePresenceSensor::stop()
{
    if (!running_) {
        return;
    }
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    running_ = false;
    cancelHold("stop");
    qInfo() << "[PRESENCE] debug mouse driver stopped";
}

/** @return 全局事件过滤器已经安装时返回 true。 */
bool DebugMousePresenceSensor::isRunning() const
{
    return running_;
}

/** @brief 更新多媒体页面活动状态；离开该页面时取消长按。 */
void DebugMousePresenceSensor::setMultimediaActive(bool active)
{
    multimediaActive_ = active;
    if (!active) {
        cancelHold("multimedia inactive");
    }
}

/** @brief 调试触发人员离开；传感器未运行时忽略。 */
void DebugMousePresenceSensor::simulatePresenceLost()
{
    if (!running_) {
        qInfo() << "[PRESENCE] simulated presenceLost ignored: sensor stopped";
        return;
    }
    qInfo() << "[PRESENCE] simulated presenceLost";
    emit presenceLost();
}

/** @brief 观察左键按下、释放和窗口失活事件，以维护长按状态。 */
bool DebugMousePresenceSensor::eventFilter(QObject *watched, QEvent *event)
{
    if (!event) {
        return false;
    }

    // 全局观察释放事件，避免指针移出多媒体子窗口后松键却遗留已启动的长按状态。
    if (event->type() == QEvent::MouseButtonRelease && holdingLeftButton_) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            cancelHold("left button released before timeout");
        }
        return false;
    }

    if (event->type() == QEvent::UngrabMouse ||
        event->type() == QEvent::WindowDeactivate) {
        cancelHold("mouse grab or window activation lost");
        return false;
    }

    if (!running_ || !multimediaActive_ || !leftHoldEnabled_ ||
        event->type() != QEvent::MouseButtonPress ||
        !isMultimediaObject(watched)) {
        return false;
    }

    const auto *mouseEvent = static_cast<QMouseEvent *>(event);
    if (mouseEvent->button() == Qt::LeftButton && !holdingLeftButton_) {
        holdingLeftButton_ = true;
        holdTimer_->start(holdDurationMs_);
        qInfo() << "[PRESENCE] left-button hold started"
                << "durationMs=" << holdDurationMs_;
    }

    return false;
}

/** @return object 是多媒体根窗口或其子窗口时返回 true。 */
bool DebugMousePresenceSensor::isMultimediaObject(QObject *object) const
{
    QWidget *widget = qobject_cast<QWidget *>(object);
    return multimediaRoot_ && widget &&
           (widget == multimediaRoot_.data() || multimediaRoot_->isAncestorOf(widget));
}

/** @brief 停止长按定时器并清除按住状态，reason 仅用于诊断日志。 */
void DebugMousePresenceSensor::cancelHold(const char *reason)
{
    const bool wasHolding = holdingLeftButton_ || (holdTimer_ && holdTimer_->isActive());
    if (holdTimer_) {
        holdTimer_->stop();
    }
    holdingLeftButton_ = false;
    if (wasHolding) {
        qInfo() << "[PRESENCE] left-button hold cancelled"
                << "reason=" << reason;
    }
}
