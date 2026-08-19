/**
 * @file CursorOverlay.h
 * @brief EGLFS 单原生窗口环境中的软件光标覆盖控件。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef CURSOROVERLAY_H
#define CURSOROVERLAY_H

#include <QPoint>
#include <QPointer>
#include <QWidget>

class QEvent;
class QPaintEvent;
class QTimer;

/**
 * @brief EGLFS 单原生窗口环境中的软件光标覆盖控件。
 *
 * 使用普通 Qt 子控件而非新原生窗口；模态弹窗出现时重挂到当前可见顶层控件，
 * 关闭后回到根窗口。高频移动事件合并到定时器中，降低重复布局和日志开销。
 */
class CursorOverlay : public QWidget
{
    Q_OBJECT
public:
    /** @brief 以 parent 为回退宿主创建软件光标并安装应用级事件过滤器。 */
    explicit CursorOverlay(QWidget *parent);

    /** @brief 销毁前移除事件过滤器。 */
    ~CursorOverlay() override;

    /** @brief 以宿主局部坐标更新光标位置。 */
    void updatePos(const QPoint &p);

    /** @brief 启用或隐藏软件光标覆盖层。 */
    void setOverlayEnabled(bool enabled);

    /** @return 软件光标功能已启用时返回 true。 */
    bool isOverlayEnabled() const;

protected:
    /** @brief 观察鼠标、顶层窗口和销毁事件，维护宿主与全局位置。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 绘制软件光标图形。 */
    void paintEvent(QPaintEvent *event) override;

private:
    /** @return object 所属的可挂接 QWidget 顶层宿主。 */
    QWidget *hostForObject(QObject *object) const;

    /** @return 当前最适合承载光标的可见顶层控件。 */
    QWidget *bestVisibleHost() const;

    /** @brief 保持全局位置不变地把光标重挂到新宿主。 */
    void attachToHost(QWidget *host);

    /** @brief 在窗口状态稳定后的事件循环中恢复最佳宿主。 */
    void restoreBestVisibleHostLater();

    /** @brief 合并一次待应用的全局鼠标位置。 */
    void queueGlobalPosition(const QPoint &globalPos);

    /** @brief 应用最近一次合并的全局位置。 */
    void applyPendingGlobalPosition();

    /** @brief 将全局坐标转换为当前宿主局部坐标并移动。 */
    void moveToGlobalPosition(const QPoint &globalPos);

    /** @brief 在启用状态下显示并按需提升光标。 */
    void showAndRaise(bool forceRaise = true);

    QPointer<QWidget> fallbackHost_;
    QPoint hotSpot_ {1, 1};
    QPoint lastGlobalPos_ {-1, -1};
    QPoint pendingGlobalPos_ {-1, -1};
    QPoint appliedGlobalPos_ {-1, -1};
    qint64 lastDiagnosticMs_ = 0;
    quint64 moveEventsSinceDiagnostic_ = 0;
    QTimer *moveTimer_ = nullptr;
    QTimer *hideTimer_ = nullptr;
    bool pendingMove_ = false;
    bool overlayEnabled_ = true;
};

#endif // CURSOROVERLAY_H
