/**
 * @file QuickMainMenuWidget.h
 * @brief 多媒体控制台的长期复用 Qt Quick 主菜单。
 */

#ifndef QUICKMAINMENUWIDGET_H
#define QUICKMAINMENUWIDGET_H

#include <QQuickWidget>

class QEvent;

class QuickMainMenuWidget final : public QQuickWidget
{
    Q_OBJECT
public:
    explicit QuickMainMenuWidget(QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void syncToParent();

signals:
    void actionTriggered(int index);
};

#endif // QUICKMAINMENUWIDGET_H
