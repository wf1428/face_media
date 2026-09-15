/**
 * @file SharedVirtualKeyboard.h
 * @brief 人脸模块长期复用的唯一 Qt Virtual Keyboard 宿主。
 *
 * @author Dulin
 * @date 2026-09-10
 */

#ifndef SHARED_VIRTUAL_KEYBOARD_H
#define SHARED_VIRTUAL_KEYBOARD_H

#include <QObject>
#include <QPointer>

class QEvent;
class QQuickWidget;
class QWidget;

/**
 * @brief 在人脸根窗口内维护唯一的 QML InputPanel。
 *
 * 登录框、管理后台和密码弹窗只提供普通输入控件。键盘本身跟随
 * FaceGateMainWindow 长期存在，页面切换时只显示或隐藏，不销毁 EGL/QML 资源。
 */
class SharedVirtualKeyboard : public QObject
{
public:
    /** @brief 创建唯一 QQuickWidget 并监听人脸窗口内的输入焦点。 */
    explicit SharedVirtualKeyboard(QWidget *host);

    /** @brief 移除事件监听并释放唯一键盘控件。 */
    ~SharedVirtualKeyboard() override;

    /** @brief 按当前宿主尺寸把键盘固定到窗口底部。 */
    void updateGeometry();

    /** @brief 隐藏键盘并通知 Qt 输入法收起。 */
    void hideKeyboard();

    /** @return 键盘面板当前可见时返回 true。 */
    bool isVisible() const;

protected:
    /** @brief 处理宿主尺寸、输入焦点和点击位置变化。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @brief 为当前输入控件显示并置顶键盘。 */
    void showForEditor(QWidget *editor);

    /** @return widget 属于人脸根窗口对象树时返回 true。 */
    bool belongsToHost(QWidget *widget) const;

    /** @return widget 或其父控件是可编辑输入控件时返回 true。 */
    bool isTextEditor(QWidget *widget) const;

    /** @return widget 属于共享键盘对象树时返回 true。 */
    bool isKeyboardWidget(QWidget *widget) const;

private:
    QPointer<QWidget> host_;             /**< 人脸模块根窗口。 */
    QQuickWidget *keyboard_ = nullptr;   /**< 全模块唯一 QML InputPanel。 */
    QPointer<QWidget> activeEditor_;     /**< 当前请求输入的控件。 */
    bool changingVisibility_ = false;    /**< 防止输入法可见信号递归。 */
};

#endif // SHARED_VIRTUAL_KEYBOARD_H
