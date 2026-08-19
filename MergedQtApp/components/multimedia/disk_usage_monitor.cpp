/**
 * @file disk_usage_monitor.cpp
 * @brief 周期监测指定挂载点并以短时闪现方式提示磁盘空间不足的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "disk_usage_monitor.h"

#include <QApplication>
#include <QPainter>
#include <QFontMetrics>
#include <QScreen>
#include <QEvent>
#include <QLinearGradient>
#include <QtMath>

#include <sys/statvfs.h>
#include "platform/rk3566_platform.h"

/** @brief 创建指定挂载点和使用率阈值的磁盘告警覆盖层。 */
DiskUsageMonitor::DiskUsageMonitor(QWidget *mainWindow,
                                   const QString &mountPoint,
                                   double threshold)
    : QWidget(mainWindow),
      main_(mainWindow),
      mountPoint_(mountPoint.isEmpty() ? Rk3566Platform::storageRoot() : mountPoint),
      threshold_(threshold)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);

    baseFont_ = QApplication::font();

    if (main_) {
        main_->installEventFilter(this);
        setGeometry(main_->rect());
    }

    checkTimer_.setInterval(checkIntervalMs_);
    connect(&checkTimer_, &QTimer::timeout,
            this, &DiskUsageMonitor::checkDiskUsage);

    blinkTimer_.setInterval(blinkIntervalMs_);
    connect(&blinkTimer_, &QTimer::timeout,
            this, &DiskUsageMonitor::onBlinkTimeout);

    autoHideTimer_.setSingleShot(true);
    connect(&autoHideTimer_, &QTimer::timeout, this, [this]() {
        hideWarningNow();
    });

    hide();
}

/** @brief 启动周期磁盘检查。 */
void DiskUsageMonitor::start()
{
    checkDiskUsage();       // 启动先检查一次
    checkTimer_.start();
}

/** @brief 停止检查、闪烁和自动隐藏定时器。 */
void DiskUsageMonitor::stop()
{
    checkTimer_.stop();
    blinkTimer_.stop();
    autoHideTimer_.stop();

    warningActive_ = false;
    warningVisible_ = false;
    hide();
}

/** @return 当前磁盘使用率达到告警阈值时返回 true。 */
bool DiskUsageMonitor::isWarningActive() const
{
    return warningActive_;
}

/** @brief 通过文件系统接口查询挂载点容量。 */
DiskUsageMonitor::UsageInfo DiskUsageMonitor::queryUsage() const
{
    UsageInfo info;

    struct statvfs st;
    if (::statvfs(mountPoint_.toLocal8Bit().constData(), &st) != 0) {
        return info;
    }

    const quint64 blockSize = static_cast<quint64>(st.f_frsize ? st.f_frsize : st.f_bsize);
    const quint64 totalBlocks = static_cast<quint64>(st.f_blocks);
    const quint64 availBlocks = static_cast<quint64>(st.f_bavail);

    info.totalBytes = totalBlocks * blockSize;
    info.availableBytes = availBlocks * blockSize;
    info.usedBytes = (info.totalBytes >= info.availableBytes)
            ? (info.totalBytes - info.availableBytes)
            : 0;

    if (info.totalBytes > 0) {
        info.usedRatio = static_cast<double>(info.usedBytes) /
                         static_cast<double>(info.totalBytes);
    }

    info.valid = true;
    return info;
}

/** @return 查询有效且已用比例达到阈值时返回 true。 */
bool DiskUsageMonitor::shouldWarn(const UsageInfo &info) const
{
    return info.valid && info.usedRatio >= threshold_;
}

/** @brief 用容量和百分比生成告警文字。 */
void DiskUsageMonitor::updateWarningText(const UsageInfo &info)
{
    const int percent = qRound(info.usedRatio * 100.0);

    text_ = QStringLiteral("存储空间不足！\n 已使用 %2%\n%3 / %4")
            .arg(percent)
            .arg(formatBytes(info.usedBytes))
            .arg(formatBytes(info.totalBytes));
}

/** @brief 查询挂载点使用率并更新告警状态。 */
void DiskUsageMonitor::checkDiskUsage()
{
    const UsageInfo info = queryUsage();

    if (!info.valid) {
        warningActive_ = false;
        warningVisible_ = false;
        blinkTimer_.stop();
        autoHideTimer_.stop();
        hide();
        return;
    }

    if (shouldWarn(info)) {
        updateWarningText(info);
        warningActive_ = true;

        // 第一次进入告警：立即弹一次，并启动闪烁
        if (!blinkTimer_.isActive()) {
            showWarningNow();
            blinkTimer_.start();
        } else {
            // 已在告警中，更新文案即可
            if (warningVisible_) {
                resizeToFitText();
                moveBoxToScreenCenter();
                update();
            }
        }
    } else {
        warningActive_ = false;
        warningVisible_ = false;
        blinkTimer_.stop();
        autoHideTimer_.stop();
        hide();
    }
}

/** @brief 告警持续存在时再次短时显示。 */
void DiskUsageMonitor::onBlinkTimeout()
{
    if (!warningActive_) {
        hideWarningNow();
        blinkTimer_.stop();
        return;
    }

    const UsageInfo info = queryUsage();
    if (!shouldWarn(info)) {
        warningActive_ = false;
        hideWarningNow();
        blinkTimer_.stop();
        return;
    }

    updateWarningText(info);
    showWarningNow();
}

/** @brief 立即显示告警并启动自动隐藏定时器。 */
void DiskUsageMonitor::showWarningNow()
{
    if (main_) {
        setGeometry(main_->rect());
    }

    resizeToFitText();
    moveBoxToScreenCenter();

    raise();
    show();
    update();

    warningVisible_ = true;

    autoHideTimer_.stop();
    autoHideTimer_.start(visibleDurationMs_);
}

/** @brief 仅隐藏控件，保留活动告警状态。 */
void DiskUsageMonitor::hideWarningNow()
{
    autoHideTimer_.stop();
    warningVisible_ = false;
    hide();
}

/** @brief 主窗口尺寸变化时重新定位告警。 */
bool DiskUsageMonitor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == main_) {
        if (event->type() == QEvent::Resize ||
            event->type() == QEvent::Move ||
            event->type() == QEvent::WindowStateChange) {
            if (main_) {
                setGeometry(main_->rect());
            }
            moveBoxToScreenCenter();
            update();
        }
    }

    return QWidget::eventFilter(watched, event);
}

/** @brief 按文字内容和最大宽度调整控件尺寸。 */
void DiskUsageMonitor::resizeToFitText()
{
    QFont f = baseFont_;
    f.setPointSize(f.pointSize() + 8);
    f.setWeight(QFont::Bold);
    setFont(f);

    QFontMetrics fm(f);

    int w = qMin(maxWidth_, width() - 40);
    if (w < 200) {
        w = 200;
    }

    QRect br = fm.boundingRect(QRect(0, 0, w - padX_ * 2, 1000),
                               Qt::TextWordWrap,
                               text_);

    int newW = br.width() + padX_ * 2;
    int newH = br.height() + padY_ * 2;

    if (newH < 128) {
        newH = 128;
    }

    boxRect_.setSize(QSize(newW, newH));
}

/** @brief 将告警框移动到主窗口中央。 */
void DiskUsageMonitor::moveBoxToScreenCenter()
{
    if (!main_) {
        return;
    }

    QScreen *s = QApplication::primaryScreen();
    if (!s) {
        return;
    }

    QPoint screenCenter = s->geometry().center();
    QPoint mainTopLeftGlobal = main_->mapToGlobal(QPoint(0, 0));
    QPoint centerInMain = screenCenter - mainTopLeftGlobal;

    QRect r = boxRect_;
    r.moveCenter(centerInMain);
    boxRect_ = r;
}

/** @brief 绘制磁盘空间告警卡片。 */
void DiskUsageMonitor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    if (text_.isEmpty() || boxRect_.isEmpty()) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal radius = 22.0;

    QColor bg(220, 38, 38, 220);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(boxRect_), radius, radius);

    QLinearGradient g(boxRect_.topLeft(), boxRect_.bottomLeft());
    g.setColorAt(0.0, QColor(255, 255, 255, 22));
    g.setColorAt(0.5, QColor(255, 255, 255, 10));
    g.setColorAt(1.0, QColor(0, 0, 0, 18));

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(g), 1.2));
    p.drawRoundedRect(QRectF(boxRect_).adjusted(0.6, 0.6, -0.6, -0.6), radius, radius);

    p.setPen(QColor(255, 255, 255, 240));
    p.drawText(boxRect_.adjusted(padX_, padY_, -padX_, -padY_),
               Qt::AlignCenter | Qt::TextWordWrap,
               text_);
}

/** @brief 将字节数格式化为适合显示的容量单位。 */
QString DiskUsageMonitor::formatBytes(quint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int idx = 0;

    while (value >= 1024.0 && idx < 4) {
        value /= 1024.0;
        ++idx;
    }

    return QString::number(value, 'f', idx == 0 ? 0 : 1) + " " + units[idx];
}
