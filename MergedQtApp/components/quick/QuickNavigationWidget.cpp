/**
 * @file QuickNavigationWidget.cpp
 * @brief 共用 Qt Quick 导航宿主实现。
 */

#include "QuickNavigationWidget.h"

#include "shell/QuickUiRuntime.h"

#include <QColor>
#include <QDebug>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlError>

QuickNavigationWidget::QuickNavigationWidget(QWidget *parent)
    : QQuickWidget(QuickUiRuntime::instance()->engine(), parent)
{
    setObjectName(QStringLiteral("managedQuickNavigation"));
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    setClearColor(QColor(QStringLiteral("#f0f0f0")));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setSource(QUrl(QStringLiteral("qrc:/qml/QuickNavigationRail.qml")));

    if (QQuickWindow *window = quickWindow()) {
        window->setPersistentOpenGLContext(true);
        window->setPersistentSceneGraph(true);
    }

    if (status() == QQuickWidget::Ready && rootObject()) {
        connect(rootObject(), SIGNAL(itemActivated(int)),
                this, SIGNAL(itemActivated(int)));
        connect(rootObject(), SIGNAL(backRequested()),
                this, SIGNAL(backRequested()));
        syncRootProperties();
    } else {
        const QList<QQmlError> loadErrors = errors();
        for (const QQmlError &error : loadErrors) {
            qWarning().noquote() << "[QUICK-NAV]" << error.toString();
        }
    }
}

void QuickNavigationWidget::setItems(const QStringList &items)
{
    items_ = items;
    syncRootProperties();
}

void QuickNavigationWidget::setCurrentIndex(int index)
{
    currentIndex_ = qMax(0, index);
    syncRootProperties();
}

void QuickNavigationWidget::setHorizontal(bool horizontal)
{
    horizontal_ = horizontal;
    syncRootProperties();
}

void QuickNavigationWidget::setHeader(const QString &title,
                                      const QString &subtitle)
{
    title_ = title;
    subtitle_ = subtitle;
    syncRootProperties();
}

void QuickNavigationWidget::setBackVisible(bool visible)
{
    backVisible_ = visible;
    syncRootProperties();
}

void QuickNavigationWidget::syncRootProperties()
{
    QObject *root = rootObject();
    if (!root) {
        return;
    }
    root->setProperty("items", items_);
    root->setProperty("currentIndex", currentIndex_);
    root->setProperty("horizontal", horizontal_);
    root->setProperty("title", title_);
    root->setProperty("subtitle", subtitle_);
    root->setProperty("backVisible", backVisible_);
    root->setProperty("faceTheme", faceTheme_);
}

void QuickNavigationWidget::setFaceTheme(bool enabled)
{
    faceTheme_ = enabled;
    setClearColor(QColor(enabled ? QStringLiteral("#111c2d")
                                : QStringLiteral("#f0f0f0")));
    syncRootProperties();
}
