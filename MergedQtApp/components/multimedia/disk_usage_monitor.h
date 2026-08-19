/**
 * @file disk_usage_monitor.h
 * @brief 周期监测指定挂载点并以短时闪现方式提示磁盘空间不足。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef DISK_USAGE_MONITOR_H
#define DISK_USAGE_MONITOR_H

#include <QWidget>
#include <QTimer>
#include <QRect>
#include <QString>
#include <QFont>

/**
 * @brief 周期监测指定挂载点并以短时闪现方式提示磁盘空间不足。
 *
 * 告警活动状态与控件可见状态分离，避免持续遮挡媒体画面。
 */
class DiskUsageMonitor : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建指定挂载点和使用率阈值的磁盘告警覆盖层。 */
    explicit DiskUsageMonitor(QWidget *mainWindow,
                              const QString &mountPoint = QString(),
                              double threshold = 0.8);

    /** @brief 启动周期磁盘检查。 */
    void start();

    /** @brief 停止检查、闪烁和自动隐藏定时器。 */
    void stop();

    /** @return 当前磁盘使用率达到告警阈值时返回 true。 */
    bool isWarningActive() const;

protected:
    /** @brief 绘制磁盘空间告警卡片。 */
    void paintEvent(QPaintEvent *event) override;
    /** @brief 主窗口尺寸变化时重新定位告警。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    /** @brief 查询挂载点使用率并更新告警状态。 */
    void checkDiskUsage();
    /** @brief 告警持续存在时再次短时显示。 */
    void onBlinkTimeout();

private:
    /** @brief 一次文件系统容量查询结果，字节数使用 64 位无符号值。 */
    struct UsageInfo {
        bool valid = false;
        quint64 totalBytes = 0;
        quint64 usedBytes = 0;
        quint64 availableBytes = 0;
        double usedRatio = 0.0;
    };

private:
    /** @brief 通过文件系统接口查询挂载点容量。 */
    UsageInfo queryUsage() const;
    /** @return 查询有效且已用比例达到阈值时返回 true。 */
    bool shouldWarn(const UsageInfo &info) const;
    /** @brief 用容量和百分比生成告警文字。 */
    void updateWarningText(const UsageInfo &info);

    /** @brief 立即显示告警并启动自动隐藏定时器。 */
    void showWarningNow();
    /** @brief 仅隐藏控件，保留活动告警状态。 */
    void hideWarningNow();

    /** @brief 按文字内容和最大宽度调整控件尺寸。 */
    void resizeToFitText();
    /** @brief 将告警框移动到主窗口中央。 */
    void moveBoxToScreenCenter();

    /** @brief 将字节数格式化为适合显示的容量单位。 */
    static QString formatBytes(quint64 bytes);

private:
    QWidget *main_ = nullptr;

    QString mountPoint_;
    double threshold_ = 0.8;

    QTimer checkTimer_;
    QTimer blinkTimer_;
    QTimer autoHideTimer_;

    bool warningActive_ = false;
    bool warningVisible_ = false;

    QString text_;
    QRect boxRect_;
    QFont baseFont_;

    int maxWidth_ = 520;
    int padX_ = 28;
    int padY_ = 20;

    int checkIntervalMs_ = 10000;  /**< 每 10 秒检查一次磁盘使用率。 */
    int blinkIntervalMs_ = 8000;   /**< 告警期间每 8 秒重新显示一次。 */
    int visibleDurationMs_ = 5000; /**< 每次告警显示 5 秒。 */
};

#endif // DISK_USAGE_MONITOR_H
