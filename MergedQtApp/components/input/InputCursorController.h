/**
 * @file InputCursorController.h
 * @brief 根据最近使用的触摸屏或鼠标输入动态控制系统光标。
 *
 * @author Dulin
 * @date 2026-09-02
 */

#ifndef INPUTCURSORCONTROLLER_H
#define INPUTCURSORCONTROLLER_H

#include <QObject>

class QEvent;

/**
 * @brief 应用级输入模式观察器。
 *
 * 触摸屏存在时初始隐藏光标；真实触摸事件保持隐藏，真实鼠标事件恢复显示。
 * Qt 由触摸合成的鼠标事件不会被当成物理鼠标输入。
 */
class InputCursorController final : public QObject
{
public:
    /** @brief 安装应用级事件过滤器并检测已注册的触摸屏设备。 */
    explicit InputCursorController(QObject *parent = nullptr);

    /** @brief 移除事件过滤器。 */
    ~InputCursorController() override;

protected:
    /** @brief 根据触摸或真实鼠标事件切换系统光标。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @return Qt 输入系统已注册至少一个触摸屏设备时返回 true。 */
    bool hasTouchScreen() const;

    /** @brief 根据当前设备列表应用初始光标状态。 */
    void applyInitialDeviceState();

    /** @brief 隐藏系统光标，并保持调用幂等。 */
    void hideCursor();

    /** @brief 恢复系统光标，并保持调用幂等。 */
    void showCursor();

    bool cursorHidden_ = false; /**< 最近一次输入策略是否要求隐藏光标。 */
    bool inputActivitySeen_ = false; /**< 启动后是否已经收到真实输入，防止延迟探测覆盖最新状态。 */
};

#endif
