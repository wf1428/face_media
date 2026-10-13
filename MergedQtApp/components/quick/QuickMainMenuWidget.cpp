/**
 * @file QuickMainMenuWidget.cpp
 * @brief Qt Quick 主菜单宿主实现。
 */

#include "QuickMainMenuWidget.h"

#include "shell/QuickUiRuntime.h"

#include <QColor>
#include <QDebug>
#include <QEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlError>
#include <QTimer>

QuickMainMenuWidget::QuickMainMenuWidget(QWidget *parent)
    : QQuickWidget(QuickUiRuntime::instance()->engine(), parent)
{
    setObjectName(QStringLiteral("managedQuickMainMenu"));
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    setClearColor(QColor(QStringLiteral("#f5f7fa")));
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setFocusPolicy(Qt::StrongFocus);
    if (parentWidget()) {
        parentWidget()->installEventFilter(this);
    }
    setSource(QUrl(QStringLiteral("qrc:/qml/QuickMainMenu.qml")));
    if (QQuickWindow *window = quickWindow()) {
        window->setPersistentOpenGLContext(true);
        window->setPersistentSceneGraph(true);
    }
    if (status() == QQuickWidget::Ready && rootObject()) {
        connect(rootObject(), SIGNAL(actionTriggered(int)),
                this, SIGNAL(actionTriggered(int)));
        syncToParent();
    } else {
        const QList<QQmlError> loadErrors = errors();
        for (const QQmlError &error : loadErrors) {
            qWarning().noquote() << "[QUICK-MENU]" << error.toString();
        }
        hide();
    }
}

/** @brief 跟随菜单页自身的布局结果铺满，避免底层 Designer 标题从边缘露出。 */
bool QuickMainMenuWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event &&
            (event->type() == QEvent::Resize ||
             event->type() == QEvent::Show ||
             event->type() == QEvent::LayoutRequest)) {
        syncToParent();
        // 父布局会在当前事件之后继续调整子页面，下一轮再按最终尺寸校准一次。
        QTimer::singleShot(0, this, [this]() { syncToParent(); });
    }
    return QQuickWidget::eventFilter(watched, event);
}

void QuickMainMenuWidget::syncToParent()
{
    QWidget *host = parentWidget();
    if (!host || status() != QQuickWidget::Ready) {
        return;
    }
    setGeometry(host->rect());
    if (host->isVisible()) {
        show();
        raise();
    }
}
