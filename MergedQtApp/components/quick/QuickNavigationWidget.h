/**
 * @file QuickNavigationWidget.h
 * @brief 人脸后台与系统设置共用的 Qt Quick 导航宿主。
 */

#ifndef QUICKNAVIGATIONWIDGET_H
#define QUICKNAVIGATIONWIDGET_H

#include <QQuickWidget>
#include <QStringList>

/**
 * @brief 使用统一 QQmlEngine 的长期存活导航组件。
 *
 * 组件只发出导航序号和返回请求，页面切换仍由原 QWidget/C++ 逻辑执行。
 */
class QuickNavigationWidget final : public QQuickWidget
{
    Q_OBJECT

public:
    explicit QuickNavigationWidget(QWidget *parent = nullptr);

    void setItems(const QStringList &items);
    void setCurrentIndex(int index);
    void setHorizontal(bool horizontal);
    void setHeader(const QString &title, const QString &subtitle);
    void setBackVisible(bool visible);
    void setFaceTheme(bool enabled);

signals:
    void itemActivated(int index);
    void backRequested();

private:
    void syncRootProperties();

    QStringList items_;
    QString title_;
    QString subtitle_;
    int currentIndex_ = 0;
    bool horizontal_ = false;
    bool backVisible_ = false;
    bool faceTheme_ = false;
};

#endif // QUICKNAVIGATIONWIDGET_H
