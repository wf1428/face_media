/**
 * @file file_manager_page.h
 * @brief 基于 QFileSystemModel 的本地文件浏览和删除页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef FILE_MANAGER_PAGE_H
#define FILE_MANAGER_PAGE_H

#include <QWidget>
#include <QFileSystemModel>
#include <QTreeView>
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>

/**
 * @brief 基于 QFileSystemModel 的本地文件浏览和删除页面。
 *
 * 页面限制在 setRoot 指定的目录树内，并提供上级导航、刷新和右键删除入口。
 */
class FileManagerPage : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建文件模型、树视图和导航按钮。 */
    explicit FileManagerPage(QWidget *parent = nullptr);

    /** @brief 切换浏览根目录并同步路径栏。 */
    void setRoot(const QString &path);

signals:
    /** @brief 用户请求退出文件管理页面。 */
    void backToMenuRequested();

private slots:
    /** @brief 双击目录时进入目录，双击文件时交给系统打开。 */
    void onDoubleClicked(const QModelIndex &index);

    /** @brief 在点击位置显示包含删除操作的上下文菜单。 */
    void onCustomContextMenu(const QPoint &pos);

    /** @brief 导航到当前目录的父目录，但不越过设定根目录。 */
    void goUp();

    /** @brief 重新读取并显示当前目录。 */
    void refresh();

private:
    /**
     * @brief 删除文件或递归删除目录。
     * @return 删除全部成功时返回 true。
     */
    bool removePath(const QString &path);

    /** @brief 应用文件管理页面样式。 */
    void applyStyle();

private:
    QFileSystemModel *model = nullptr; /**< 文件系统数据模型。 */
    QTreeView *view = nullptr;         /**< 目录与文件树视图。 */
    QLineEdit *pathEdit = nullptr;     /**< 当前路径只读显示框。 */
    QPushButton *btnUp = nullptr;      /**< 返回上级目录按钮。 */
    QPushButton *btnRefresh = nullptr; /**< 刷新按钮。 */
    QPushButton *btnBack = nullptr;    /**< 返回功能菜单按钮。 */

    QString currentRoot; /**< 当前允许浏览的根目录。 */

};


#endif // FILE_MANAGER_PAGE_H
