/**
 * @file CursorOverlay.cpp
 * @brief EGLFS 单原生窗口环境中的软件光标覆盖控件的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "CursorOverlay.h"

#include <QApplication>
#include <QChildEvent>
#include <QDateTime>
#include <QEvent>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QRegion>
#include <QTimer>
#include <QDebug>

namespace {

constexpr int kCursorWidth = 16;
constexpr int kCursorHeight = 24;

/** @brief 构造软件光标使用的箭头轮廓。 */
QPolygon cursorShape()
{
    return QPolygon()
            << QPoint(0, 0)
            << QPoint(0, 19)
            << QPoint(4, 14)
            << QPoint(8, 23)
            << QPoint(12, 22)
            << QPoint(8, 13)
            << QPoint(15, 13);
}

/** @brief 递归启用控件及其子控件的鼠标跟踪，保证无按键移动也能更新软件光标。 */
void enablePointerTracking(QWidget *widget)
{
    if (!widget) return;
    widget->setMouseTracking(true);
    widget->setAttribute(Qt::WA_Hover, true);
    const auto children = widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *child : children) {
        enablePointerTracking(child);
    }
}

/** @brief 判断控件是否可作为当前软件光标的可见宿主。 */
bool isUsableHost(QWidget *widget)
{
    return widget && widget->isVisible() &&
           widget->objectName() != QStringLiteral("softwareCursorOverlay");
}

} // namespace

/** @brief 以 parent 为回退宿主创建软件光标并安装应用级事件过滤器。 */
CursorOverlay::CursorOverlay(QWidget *parent)
    : QWidget(parent),
      fallbackHost_(parent)
{
    setObjectName(QStringLiteral("softwareCursorOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kCursorWidth, kCursorHeight);
    setMask(QRegion(cursorShape()));

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    hideTimer_->setInterval(8000);
    connect(hideTimer_, &QTimer::timeout, this, &QWidget::hide);

    moveTimer_ = new QTimer(this);
    moveTimer_->setSingleShot(true);
    moveTimer_->setInterval(16);
    connect(moveTimer_, &QTimer::timeout,
            this, &CursorOverlay::applyPendingGlobalPosition);

    // 不再轮询 QCursor::pos()。在当前 RK3566 EGLFS/KMS 驱动上，硬件光标
    // drmModeMoveCursor 失败后，QPA 保存的坐标可能长时间停留在 (0,0)。
    // 轮询该坐标会把软件光标不断拉回左上角；现在只信任真实输入事件坐标。
    enablePointerTracking(parent);

    if (qApp) {
        qApp->installEventFilter(this);
    }
    hide();
}

/** @brief 销毁前移除事件过滤器。 */
CursorOverlay::~CursorOverlay()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
}

/** @brief 以宿主局部坐标更新光标位置。 */
void CursorOverlay::updatePos(const QPoint &p)
{
    if (!overlayEnabled_) return;
    QWidget *host = parentWidget();
    if (!host) return;
    queueGlobalPosition(host->mapToGlobal(p));
}

/** @brief 启用或隐藏软件光标覆盖层。 */
void CursorOverlay::setOverlayEnabled(bool enabled)
{
    overlayEnabled_ = enabled;
    if (!enabled) {
        pendingMove_ = false;
        if (moveTimer_) {
            moveTimer_->stop();
        }
        hide();
    }
    qInfo() << "[CURSOR] software overlay enabled=" << overlayEnabled_;
}

/** @return 软件光标功能已启用时返回 true。 */
bool CursorOverlay::isOverlayEnabled() const
{
    return overlayEnabled_;
}

/** @return object 所属的可挂接 QWidget 顶层宿主。 */
QWidget *CursorOverlay::hostForObject(QObject *object) const
{
    QWidget *widget = qobject_cast<QWidget *>(object);
    if (!widget || widget == this || isAncestorOf(widget)) return nullptr;

    QWidget *window = widget->window();
    if (window == this || !isUsableHost(window)) return nullptr;
    return window;
}

/** @return 当前最适合承载光标的可见顶层控件。 */
QWidget *CursorOverlay::bestVisibleHost() const
{
    if (QWidget *popup = QApplication::activePopupWidget()) {
        if (isUsableHost(popup)) return popup;
    }
    if (QWidget *modal = QApplication::activeModalWidget()) {
        if (isUsableHost(modal)) return modal;
    }
    if (QWidget *active = QApplication::activeWindow()) {
        if (isUsableHost(active)) return active;
    }
    return isUsableHost(fallbackHost_) ? fallbackHost_.data() : nullptr;
}

/** @brief 保持全局位置不变地把光标重挂到新宿主。 */
void CursorOverlay::attachToHost(QWidget *host)
{
    if (!isUsableHost(host) || host == this || host == parentWidget()) return;

    const bool wasVisible = isVisible();
    hide();
    setParent(host);
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(kCursorWidth, kCursorHeight);
    setMask(QRegion(cursorShape()));
    enablePointerTracking(host);

    if (lastGlobalPos_.x() >= 0 && lastGlobalPos_.y() >= 0) {
        QPoint local = host->mapFromGlobal(lastGlobalPos_) - hotSpot_;
        local.setX(qBound(0, local.x(), qMax(0, host->width() - width())));
        local.setY(qBound(0, local.y(), qMax(0, host->height() - height())));
        move(local);
    }

    if (wasVisible) {
        show();
        raise();
    }

    qInfo() << "[EGLFS-CURSOR] overlay host changed"
            << "hostClass=" << host->metaObject()->className()
            << "hostObject=" << host->objectName()
            << "hostSize=" << host->size();
}

/** @brief 在窗口状态稳定后的事件循环中恢复最佳宿主。 */
void CursorOverlay::restoreBestVisibleHostLater()
{
    QTimer::singleShot(0, this, [this]() {
        if (QWidget *host = bestVisibleHost()) {
            attachToHost(host);
            if (lastGlobalPos_.x() >= 0 && lastGlobalPos_.y() >= 0) {
                moveToGlobalPosition(lastGlobalPos_);
            }
        }
    });
}

/** @brief 观察鼠标、顶层窗口和销毁事件，维护宿主与全局位置。 */
bool CursorOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (!overlayEnabled_ || !event || watched == this) return false;

    switch (event->type()) {
    case QEvent::MouseMove: {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        queueGlobalPosition(mouseEvent->globalPos());
        if (QWidget *host = hostForObject(watched)) attachToHost(host);
        break;
    }
    case QEvent::HoverMove: {
        // 某些控件只产生 HoverMove，不产生 MouseMove；仍使用事件携带的
        // 真实坐标，避免再次依赖 EGLFS 的全局硬件光标位置。
        const auto *hoverEvent = static_cast<QHoverEvent *>(event);
        if (auto *widget = qobject_cast<QWidget *>(watched)) {
            queueGlobalPosition(widget->mapToGlobal(hoverEvent->pos()));
        }
        if (QWidget *host = hostForObject(watched)) attachToHost(host);
        break;
    }
    case QEvent::ChildAdded: {
        // 页面和弹窗可能在启动后动态创建，确保新控件也持续产生移动/悬停事件。
        const auto *childEvent = static_cast<QChildEvent *>(event);
        if (auto *childWidget = qobject_cast<QWidget *>(childEvent->child())) {
            enablePointerTracking(childWidget);
        }
        break;
    }
    case QEvent::Show:
    case QEvent::WindowActivate:
        // QDialog/虚拟键盘是独立的 Qt 顶层控件。把光标重挂到该窗口内部，
        // 仍由唯一 EGLFS surface 合成，但不会再被弹窗盖住。
        if (QWidget *host = hostForObject(watched)) {
            attachToHost(host);
            showAndRaise();
        }
        break;
    case QEvent::ZOrderChange:
        // QQuickWidget 键盘显示后会 raise() 自己。软件光标也必须立即恢复到
        // 同一弹窗的最上层，不能等到下一次鼠标按键事件才提升。
        if (QWidget *host = hostForObject(watched)) {
            attachToHost(host);
            showAndRaise();
        }
        break;
    case QEvent::Hide:
    case QEvent::Close:
    case QEvent::Destroy: {
        // 光标当前可能正是该 QDialog 的子控件。必须在弹窗销毁子控件前
        // 立即移回长期存在的 EGLFS 根窗口，否则光标会随临时弹窗一起析构。
        QWidget *closingWindow = qobject_cast<QWidget *>(watched);
        if (closingWindow && parentWidget() == closingWindow && fallbackHost_) {
            attachToHost(fallbackHost_.data());
        }
        restoreBestVisibleHostLater();
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::Wheel:
        if (QWidget *host = hostForObject(watched)) attachToHost(host);
        showAndRaise();
        break;
    default:
        break;
    }
    return false;
}

/** @brief 合并一次待应用的全局鼠标位置。 */
void CursorOverlay::queueGlobalPosition(const QPoint &globalPos)
{
    if (!overlayEnabled_) {
        return;
    }

    lastGlobalPos_ = globalPos;
    if (!pendingMove_ && globalPos == appliedGlobalPos_ && isVisible()) {
        return;
    }
    pendingGlobalPos_ = globalPos;
    pendingMove_ = true;
    if (!moveTimer_) {
        applyPendingGlobalPosition();
        return;
    }
    if (!moveTimer_->isActive()) {
        // 首个移动事件立即绘制，后续高频事件仍在 16 ms 窗口内合并。
        // 这样保留限流，同时去掉鼠标开始移动时的固定一帧延迟。
        applyPendingGlobalPosition();
        moveTimer_->start();
    }
}

/** @brief 应用最近一次合并的全局位置。 */
void CursorOverlay::applyPendingGlobalPosition()
{
    if (!overlayEnabled_ || !pendingMove_) {
        return;
    }

    const QPoint globalPos = pendingGlobalPos_;
    pendingMove_ = false;
    moveToGlobalPosition(globalPos);
}

/** @brief 将全局坐标转换为当前宿主局部坐标并移动。 */
void CursorOverlay::moveToGlobalPosition(const QPoint &globalPos)
{
    lastGlobalPos_ = globalPos;
    QWidget *host = parentWidget();
    if (!host || !host->isVisible()) {
        host = bestVisibleHost();
        attachToHost(host);
    }
    host = parentWidget();
    if (!host || !host->isVisible()) return;

    QPoint local = host->mapFromGlobal(globalPos) - hotSpot_;
    local.setX(qBound(0, local.x(), qMax(0, host->width() - width())));
    local.setY(qBound(0, local.y(), qMax(0, host->height() - height())));
    if (local != pos()) {
        move(local);
    }
    appliedGlobalPos_ = globalPos;
    showAndRaise(false);

    ++moveEventsSinceDiagnostic_;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastDiagnosticMs_ == 0 || nowMs - lastDiagnosticMs_ >= 2000) {
        qInfo() << "[EGLFS-CURSOR] software cursor input"
                << "updates/2s=" << moveEventsSinceDiagnostic_
                << "global=" << globalPos
                << "local=" << local
                << "hostClass=" << host->metaObject()->className()
                << "hostObject=" << host->objectName()
                << "hostSize=" << host->size()
                << "visible=" << isVisible();
        moveEventsSinceDiagnostic_ = 0;
        lastDiagnosticMs_ = nowMs;
    }
}

/** @brief 在启用状态下显示并按需提升光标。 */
void CursorOverlay::showAndRaise(bool forceRaise)
{
    if (!overlayEnabled_) {
        hide();
        return;
    }
    const bool wasVisible = isVisible();
    if (!wasVisible) {
        show();
    }
    if (forceRaise || !wasVisible) {
        raise();
    }
    if (hideTimer_) hideTimer_->start();
}

/** @brief 绘制软件光标图形。 */
void CursorOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.moveTo(0.75, 0.75);
    path.lineTo(0.75, 18.0);
    path.lineTo(4.75, 14.0);
    path.lineTo(8.5, 22.5);
    path.lineTo(11.75, 21.0);
    path.lineTo(8.0, 12.75);
    path.lineTo(15.0, 12.75);
    path.closeSubpath();

    painter.setPen(QPen(Qt::black, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::white);
    painter.drawPath(path);
}
