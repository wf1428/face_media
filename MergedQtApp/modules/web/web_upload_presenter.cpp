/**
 * @file web_upload_presenter.cpp
 * @brief 手机视频上传的设备接收、校验及发布进度卡片。
 * @author Du lin
 * @date 2026-10-16
 */
#include "web_upload_presenter.h"
#include "components/multimedia/media_toast.h"
#include <QThread>
#include <QTimer>

/** @brief 初始化独立非模态卡片、结果隐藏定时器及失联检查。 */
WebUploadPresenter::WebUploadPresenter(QWidget *parent) : QObject(parent)
{
    clock_.start();
    toast_ = new DownloadProgressToast(parent);
    toast_->setObjectName("webUploadProgress");
    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, toast_, &DownloadProgressToast::hideToast);
    auto *watchdog = new QTimer(this);
    connect(watchdog, &QTimer::timeout, this, &WebUploadPresenter::expire);
    watchdog->start(5000);
}

/** @brief 更新并发接收/校验任务；终止状态只移除本任务。 */
void WebUploadPresenter::updateProgress(const QString &token,
                                        const QString &name,
                                        int percent,
                                        const QString &state)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (state == "STARTED" || state == "PROGRESS" || state == "VALIDATING") {
        if (!active_.contains(token) && active_.size() >= 16)
            return;
        hideTimer_->stop();
        active_.insert(token,
                       {name, qBound(0, percent, 99), clock_.elapsed(), state == "VALIDATING"});
        showActive();
        return;
    }
    if (state != "COMPLETED" && state != "FAILED")
        return;
    active_.remove(token);
    if (!active_.isEmpty()) {
        showActive();
        return;
    }
    toast_->showProgress(state == "COMPLETED" ? QStringLiteral("视频接收完成")
                                              : QStringLiteral("视频接收中断"),
                         state == "COMPLETED" ? 100 : qBound(0, percent, 99),
                         name);
    hideTimer_->start(2500);
}

/** @brief 同时存在多个上传时显示任务数量及最近更新文件。 */
void WebUploadPresenter::showActive()
{
    if (active_.isEmpty())
        return;
    auto latest = active_.cbegin();
    for (auto it = active_.cbegin(); it != active_.cend(); ++it)
        if (it.value().updated >= latest.value().updated)
            latest = it;
    const auto title = active_.size() == 1
                           ? (latest.value().validating ? QStringLiteral("正在校验视频")
                                                        : QStringLiteral("正在接收手机视频"))
                           : QStringLiteral("正在接收手机视频（%1项）").arg(active_.size());
    toast_->showProgress(title, latest.value().percent, latest.value().name);
}

/** @brief 清理 90 秒没有更新的上传，设备提示与实际传输业务互不阻塞。 */
void WebUploadPresenter::expire()
{
    const auto now = clock_.elapsed();
    QString expired;
    for (auto it = active_.begin(); it != active_.end();) {
        if (now - it.value().updated > 90000) {
            expired = it.value().name;
            it = active_.erase(it);
        } else {
            ++it;
        }
    }
    if (expired.isEmpty())
        return;
    if (!active_.isEmpty()) {
        showActive();
    } else {
        toast_->showProgress(QStringLiteral("视频接收状态已中断"), 0, expired);
        hideTimer_->start(2500);
    }
}
