/**
 * @file web_upload_presenter.h
 * @brief 复用 FTP 进度卡片显示手机视频的接收、校验、发布状态。
 * @author Du lin
 * @date 2026-10-16
 */
#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QElapsedTimer>

class QWidget;
class QTimer;
class DownloadProgressToast;

/** @brief 在 Qt 主线程管理并发手机视频上传提示及中断清理。 */
class WebUploadPresenter final : public QObject {
public:

    /** @brief 在设备主窗口创建与 FTP 相同样式的独立进度卡片。 */
    explicit WebUploadPresenter(QWidget *parent);

    /** @brief 接收上传任务的真实接收进度，发布后短时显示完成结果。 */
    void
    updateProgress(const QString &token, const QString &name, int percent, const QString &state);

private:

    /** @brief 显示最近更新的传输；多个任务独立记录，结束一个不隐藏其他任务。 */
    void showActive();

    /** @brief daemon 异常退出后清理失去心跳的提示，避免进度卡永久停留。 */
    void expire();

    struct Entry {
        QString name;
        int percent = 0;
        qint64 updated = 0;
        bool validating = false;
    };

    QMap<QString, Entry> active_;
    QElapsedTimer clock_;
    DownloadProgressToast *toast_ = nullptr;
    QTimer *hideTimer_ = nullptr;
};
