/**
 * @file DebugMousePresenceSensor.h
 * @brief 使用鼠标事件模拟人员存在状态的调试传感器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef DEBUGMOUSEPRESENCESENSOR_H
#define DEBUGMOUSEPRESENCESENSOR_H

#include "IPresenceSensor.h"

#include <QPointer>

class QEvent;
class QTimer;
class QWidget;

/**
 * @brief 使用鼠标事件模拟人员存在状态的调试传感器。
 *
 * 多媒体页面活动期间，持续按住鼠标左键达到配置时长会产生 presenceDetected()；
 * presenceLost() 由调试入口显式触发。事件过滤器始终返回 false，不吞掉业务鼠标事件。
 */
class DebugMousePresenceSensor final : public IPresenceSensor
{
    Q_OBJECT

public:
    /**
     * @brief 创建鼠标调试传感器。
     * @param multimediaRoot 允许触发长按检测的多媒体根窗口。
     * @param leftHoldEnabled 是否启用左键长按检测。
     * @param holdDurationMs 触发检测所需持续时间，单位 ms，最小按 100 ms 处理。
     * @param parent Qt 父对象。
     */
    explicit DebugMousePresenceSensor(QWidget *multimediaRoot,
                                      bool leftHoldEnabled,
                                      int holdDurationMs,
                                      QObject *parent = nullptr);

    /** @brief 销毁前移除全局事件过滤器并停止定时器。 */
    ~DebugMousePresenceSensor() override;

    /** @brief 在 QApplication 上安装事件过滤器。 */
    bool start() override;

    /** @brief 移除事件过滤器并取消尚未完成的长按。 */
    void stop() override;

    /** @return 全局事件过滤器已经安装时返回 true。 */
    bool isRunning() const override;

    /** @brief 更新多媒体页面活动状态；离开该页面时取消长按。 */
    void setMultimediaActive(bool active);

public slots:
    /** @brief 调试触发人员离开；传感器未运行时忽略。 */
    void simulatePresenceLost();

protected:
    /** @brief 观察左键按下、释放和窗口失活事件，以维护长按状态。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @return object 是多媒体根窗口或其子窗口时返回 true。 */
    bool isMultimediaObject(QObject *object) const;

    /** @brief 停止长按定时器并清除按住状态，reason 仅用于诊断日志。 */
    void cancelHold(const char *reason);

    QPointer<QWidget> multimediaRoot_; /**< 限制长按触发范围，并自动感知窗口销毁。 */
    int holdDurationMs_ = 3000;        /**< 左键长按触发时间，单位 ms。 */
    bool running_ = false;             /**< 是否已安装应用级事件过滤器。 */
    bool multimediaActive_ = false;    /**< 多媒体模块是否位于前台。 */
    bool leftHoldEnabled_ = true;      /**< 配置是否允许左键长按模拟。 */
    bool holdingLeftButton_ = false;   /**< 当前是否已启动一次左键长按计时。 */
    QTimer *holdTimer_ = nullptr;       /**< 单次触发的长按计时器。 */
};

#endif // DEBUGMOUSEPRESENCESENSOR_H
