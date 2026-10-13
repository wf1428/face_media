/**
 * @file QuickPageHeaderWidget.h
 * @brief 后台功能页共用的长期复用 Qt Quick 标题操作栏。
 */

#ifndef QUICKPAGEHEADERWIDGET_H
#define QUICKPAGEHEADERWIDGET_H

#include <QQuickWidget>
#include <QStringList>

class QuickPageHeaderWidget final : public QQuickWidget
{
    Q_OBJECT
public:
    explicit QuickPageHeaderWidget(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setDetail(const QString &detail);
    void setActions(const QStringList &actions);

signals:
    void actionTriggered(int index);

private:
    void syncRootProperties();

    QString title_;
    QString detail_;
    QStringList actions_;
};

#endif // QUICKPAGEHEADERWIDGET_H
