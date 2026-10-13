/**
 * @file QuickPageHeaderWidget.cpp
 * @brief 后台功能页共用 Quick 标题操作栏实现。
 */

#include "QuickPageHeaderWidget.h"

#include "shell/QuickUiRuntime.h"

#include <QColor>
#include <QDebug>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlError>

QuickPageHeaderWidget::QuickPageHeaderWidget(QWidget *parent)
    : QQuickWidget(QuickUiRuntime::instance()->engine(), parent)
{
    setObjectName(QStringLiteral("managedQuickPageHeader"));
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    setClearColor(QColor(QStringLiteral("#ffffff")));
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setFocusPolicy(Qt::NoFocus);
    setMinimumHeight(72);
    setMaximumHeight(84);
    setSource(QUrl(QStringLiteral("qrc:/qml/QuickPageHeader.qml")));

    if (QQuickWindow *window = quickWindow()) {
        window->setPersistentOpenGLContext(true);
        window->setPersistentSceneGraph(true);
    }
    if (status() == QQuickWidget::Ready && rootObject()) {
        connect(rootObject(), SIGNAL(actionTriggered(int)),
                this, SIGNAL(actionTriggered(int)));
        syncRootProperties();
    } else {
        const QList<QQmlError> loadErrors = errors();
        for (const QQmlError &error : loadErrors) {
            qWarning().noquote() << "[QUICK-HEADER]" << error.toString();
        }
        hide();
    }
}

void QuickPageHeaderWidget::setTitle(const QString &title)
{
    title_ = title;
    syncRootProperties();
}

void QuickPageHeaderWidget::setDetail(const QString &detail)
{
    detail_ = detail;
    syncRootProperties();
}

void QuickPageHeaderWidget::setActions(const QStringList &actions)
{
    actions_ = actions;
    syncRootProperties();
}

void QuickPageHeaderWidget::syncRootProperties()
{
    QObject *root = rootObject();
    if (!root) return;
    root->setProperty("title", title_);
    root->setProperty("detail", detail_);
    root->setProperty("actions", actions_);
}
