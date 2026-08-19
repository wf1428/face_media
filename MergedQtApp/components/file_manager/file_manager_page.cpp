/**
 * @file file_manager_page.cpp
 * @brief 基于 QFileSystemModel 的本地文件浏览和删除页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "file_manager_page.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QHeaderView>
#include <QFileInfo>
#include <QResizeEvent>
#include <QDir>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include "platform/rk3566_platform.h"

/** @brief 创建文件模型、树视图和导航按钮。 */
FileManagerPage::FileManagerPage(QWidget *parent)
    : QWidget(parent)
{
    // 顶部栏
    pathEdit = new QLineEdit(this);
    pathEdit->setReadOnly(true);

    btnUp = new QPushButton("上一级", this);
    btnRefresh = new QPushButton("刷新", this);
    btnBack = new QPushButton("返回菜单", this);

    auto *topBar = new QHBoxLayout;
    topBar->addWidget(btnBack);
    topBar->addWidget(btnUp);
    topBar->addWidget(btnRefresh);
    topBar->addWidget(pathEdit, 1);

    // 文件系统模型
    model = new QFileSystemModel(this);
    model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot); // 显示文件/文件夹
    model->setReadOnly(false);
    model->setRootPath("/"); // 先给个根，后面 setRoot 会切

    // 视图
    view = new QTreeView(this);
    view->setModel(model);
    //view->setSortingEnabled(true);
    //view->sortByColumn(0, Qt::AscendingOrder);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);


    auto *hdr = view->header();
    hdr->setStretchLastSection(false);
    hdr->setSectionResizeMode(QHeaderView::Interactive);

    view->setColumnWidth(0, 600);
    view->setColumnWidth(3, 180);

    // 右键菜单
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QTreeView::customContextMenuRequested,
            this, &FileManagerPage::onCustomContextMenu);

    // 双击进入 / 打开
    connect(view, &QTreeView::doubleClicked,
            this, &FileManagerPage::onDoubleClicked);

    // 按钮
    connect(btnUp, &QPushButton::clicked, this, &FileManagerPage::goUp);
    connect(btnRefresh, &QPushButton::clicked, this, &FileManagerPage::refresh);
    connect(btnBack, &QPushButton::clicked, this, &FileManagerPage::backToMenuRequested);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(topBar);
    mainLayout->addWidget(view, 1);
    setLayout(mainLayout);

    applyStyle();

    // 默认根目录：
    setRoot(Rk3566Platform::storageRoot());
}

/** @brief 切换浏览根目录并同步路径栏。 */
void FileManagerPage::setRoot(const QString &path)
{
    currentRoot = path;
    pathEdit->setText(currentRoot);

    // setRootPath 返回的 index 才能用于 setRootIndex
    model->setRootPath(currentRoot);
    QModelIndex rootIndex = model->index(currentRoot);
    view->setRootIndex(rootIndex);

}

/** @brief 双击目录时进入目录，双击文件时交给系统打开。 */
void FileManagerPage::onDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid()) return;

    QString path = model->filePath(index);
    QFileInfo info(path);

    if (info.isDir()) {
        setRoot(path);
        return;
    }

    // 文件：尝试用系统打开（在某些嵌入式环境可能没有 xdg-open，这里失败就提示）
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::information(this, "打开文件", QString("无法调用系统打开：\n%1").arg(path));
    }
}

/** @brief 在点击位置显示包含删除操作的上下文菜单。 */
void FileManagerPage::onCustomContextMenu(const QPoint &pos)
{
    QModelIndex index = view->indexAt(pos);
    if (!index.isValid()) return;

    QString path = model->filePath(index);
    QFileInfo info(path);

    QMenu menu(this);
    QAction *actDelete = menu.addAction("删除");

    QAction *chosen = menu.exec(view->viewport()->mapToGlobal(pos));
    if (chosen != actDelete) return;

    QString text = info.isDir() ? "确定要删除文件夹及其所有内容吗？" : "确定要删除该文件吗？";
    auto ret = QMessageBox::question(this, "删除确认", text + "\n\n" + path,
                                    QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    if (!removePath(path)) {
        QMessageBox::warning(this, "删除失败", "删除失败，权限不足或文件正在被占用：\n" + path);
    } else {
        refresh();
    }
}

/**
 * @brief 删除文件或递归删除目录。
 * @return 删除全部成功时返回 true。
 */
bool FileManagerPage::removePath(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists()) return true;

    if (info.isDir()) {
        QDir dir(path);
        return dir.removeRecursively();
    } else {
        return QFile::remove(path);
    }
}

/** @brief 导航到当前目录的父目录，但不越过设定根目录。 */
void FileManagerPage::goUp()
{
    QDir dir(currentRoot);
    if (!dir.cdUp()) return;

    QString up = dir.absolutePath();
    // 文件访问限制在当前配置的存储根目录：
    if (!up.startsWith(QDir::cleanPath(Rk3566Platform::storageRoot()))) return;

    setRoot(up);
}


/** @brief 重新读取并显示当前目录。 */
void FileManagerPage::refresh()
{
    // QFileSystemModel 会自动刷新，但你可以强制重置 root
    setRoot(currentRoot);
}

/** @brief 应用文件管理页面样式。 */
void FileManagerPage::applyStyle()
{
    // 现代浅色 QSS（白底 + 高对比 + 清晰选中）
    const char *qss = R"(
    FileManagerPage {
        background: #f6f7fb; /* 柔和白底，避免刺眼纯白 */
    }

    /* 输入框：白底、圆角、轻边框 */
    QLineEdit {
        background: #ffffff;
        border: 1px solid #d8deea;
        color: #111827;              /* 深色字，清晰 */
        border-radius: 10px;
        padding: 8px 12px;
        font-size: 14px;
        selection-background-color: #2563eb; /* 蓝色选中 */
        selection-color: #ffffff;
    }
    QLineEdit:focus {
        border: 1px solid #2563eb;
    }

    /* 按钮：白底 + 轻描边，hover 有层次 */
    QPushButton {
        background: #ffffff;
        border: 1px solid #d8deea;
        color: #111827;
        border-radius: 10px;
        padding: 8px 12px;
        font-size: 14px;
    }
    QPushButton:hover {
        background: #f1f5ff;
        border-color: #bcd0ff;
    }
    QPushButton:pressed {
        background: #e7efff;
        border-color: #2563eb;
    }
    QPushButton:disabled {
        color: #9aa4b2;
        background: #f3f4f6;
        border-color: #e5e7eb;
    }

    /* 返回按钮：主色强调（你原来用了 btnBack，这里保留） */
    QPushButton#btnBack {
        background: #2563eb;
        border: 1px solid #2563eb;
        color: #ffffff;
        font-weight: 600;
    }
    QPushButton#btnBack:hover {
        background: #1d4ed8;
        border-color: #1d4ed8;
    }
    QPushButton#btnBack:pressed {
        background: #1e40af;
        border-color: #1e40af;
    }

    /* 文件列表：白底卡片感 */
    QTreeView {
        background: #ffffff;
        alternate-background-color: #f8fafc; /* 交替行轻灰 */
        color: #111827;
        border: 1px solid #d8deea;
        border-radius: 12px;
        padding: 6px;
        font-size: 14px;

        outline: 0;
        show-decoration-selected: 1;
    }

    QTreeView::item {
        padding: 10px 8px; /* 行高略加大，更现代也更好点 */
        border-radius: 8px;
    }

    QTreeView::item:hover {
        background: #eef2ff; /* hover 淡蓝灰 */
    }

    /* 选中：高对比，清晰可读 */
    QTreeView::item:selected {
        background: #2563eb;
        color: #ffffff;
    }

    /* 表头：浅灰底 + 分隔清晰 */
    QHeaderView::section {
        background: #f3f4f6;
        color: #374151;
        border: none;
        border-bottom: 1px solid #e5e7eb;
        padding: 8px 8px;
        font-size: 12px;
        font-weight: 600;
    }
    QHeaderView {
        background: transparent;
    }

    /* 右键菜单：白底、圆角 */
    QMenu {
        background: #ffffff;
        color: #111827;
        border: 1px solid #d8deea;
        border-radius: 10px;
        padding: 6px;
    }
    QMenu::item {
        padding: 8px 12px;
        border-radius: 8px;
    }
    QMenu::item:selected {
        background: #2563eb;
        color: #ffffff;
    }

    QMessageBox {
        background: #ffffff;
    }
    )";

    this->setStyleSheet(qss);
}


