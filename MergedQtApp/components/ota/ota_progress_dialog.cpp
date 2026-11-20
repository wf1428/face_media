/**
 * @file ota_progress_dialog.cpp
 * @brief 复用视频 FTP 的下载卡片显示 OTA 状态，并按通知时长收起终态。
 * @author Du lin
 * @date 2026-11-18
 * @note 最近修改：2026-11-20；修改人：Du lin；替换独立进度样式为 DownloadProgressToast；预写入失败5秒收起，新进度停止旧隐藏定时器，保留原终态显示时长；改用 appShellRoot 子覆盖层，按所属屏幕的全局中心换算坐标，显示后及根窗口移动/缩放时重新定位；移除额外关闭按钮，保持 FTP 卡片样式与计时逻辑。
 */
#include "ota_progress_dialog.h"
#include "components/multimedia/media_toast.h"
#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QEvent>
#include <QWindow>
#include <QPointer>
#include <QScreen>
#include <QTimer>

OtaProgressDialog::OtaProgressDialog(QWidget *parent)
    : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    // 使用根窗口内的子覆盖层，避免 Wayland/EGLFS 对独立工具窗口位置的干预。
    if (parent) { setWindowFlags(Qt::Widget); parent->installEventFilter(this); }
    setObjectName(QStringLiteral("otaProgressPresenter"));
    setWindowTitle(QStringLiteral("系统升级"));
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(false);
    toast_ = new DownloadProgressToast(this);
    toast_->setObjectName(QStringLiteral("otaDownloadProgress"));
    // 保留原 OTA 的纯文本语义，不让错误内容触发富文本解析。
    for (auto *label : toast_->findChildren<QLabel *>()) label->setTextFormat(Qt::PlainText);
    hideTimer_ = new QTimer(this);
    hideTimer_->setObjectName(QStringLiteral("otaResultHideTimer"));
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, &QWidget::hide);
}

void OtaProgressDialog::present(const QString &text, int percent, bool terminal, bool success, int displayMs)
{
    static QPointer<OtaProgressDialog> dialog;
    QWidget *anchor = nullptr;
    const auto windows = QApplication::topLevelWidgets();
    for (auto *window : windows) {
        if (window->objectName() == QStringLiteral("appShellRoot")) { anchor = window; break; }
    }
    // 独立窗口测试和无 AppShell 的调用兼容普通主窗口，忽略临时对话框。
    if (!anchor) {
        for (auto *window : windows) {
            if (window != dialog && window->isVisible() && !qobject_cast<QDialog *>(window)
                    && (qobject_cast<QMainWindow *>(window) || window->windowType() == Qt::Window)) {
                anchor = window; break;
            }
        }
    }
    if (!dialog) dialog = new OtaProgressDialog(anchor);
    else if (dialog->parentWidget() != anchor) {
        if (auto *previous = dialog->parentWidget()) previous->removeEventFilter(dialog);
        dialog->setParent(anchor, anchor ? Qt::WindowFlags(Qt::Widget)
                                        : Qt::WindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint));
        if (anchor) anchor->installEventFilter(dialog);
    }
    dialog->hideTimer_->stop();
    dialog->terminal_ = terminal;
    dialog->reposition();
    const auto title = terminal ? (success ? QStringLiteral("系统升级完成") : QStringLiteral("系统升级结果"))
                                : QStringLiteral("系统升级");
    dialog->toast_->showProgress(title, qBound(0, percent, 100), text);
    dialog->show(); dialog->raise();
    // 首次 show 的布局/平台事件处理完成后再校准，后续刷新不依赖活动窗口。
    const auto alive = dialog;
    QTimer::singleShot(0, dialog, [alive] { if (alive && alive->isVisible()) alive->reposition(); });
    if (terminal && displayMs > 0) dialog->hideTimer_->start(displayMs);
}

void OtaProgressDialog::reposition()
{
    auto *root = parentWidget();
    const auto *handle = root ? root->windowHandle() : windowHandle();
    const auto *screen = handle && handle->screen() ? handle->screen() : QGuiApplication::primaryScreen();
    const QRect area = screen ? screen->geometry()
                             : (root ? QRect(root->mapToGlobal(QPoint()), root->size()) : QRect(0, 0, 1024, 768));
    resize(qMin(564, qMax(400, area.width() - 32)), 280);
    const QPoint position = area.center() - rect().center();
    move(root ? root->mapFromGlobal(position) : position);
    // 根窗口尺寸变化后也同步卡片局部几何，保持卡片本身的中心一致。
    const int width = qMin(500, qMax(380, this->width() - 64));
    toast_->setGeometry((this->width() - width) / 2, qMax(36, height() / 2 - 82), width, 164);
}

bool OtaProgressDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && isVisible()) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::WindowStateChange:
            reposition();
            break;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void OtaProgressDialog::reject()
{
    if (terminal_) { hideTimer_->stop(); QDialog::reject(); }
}
