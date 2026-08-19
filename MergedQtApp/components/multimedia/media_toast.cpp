/**
 * @file media_toast.cpp
 * @brief 居中显示下载进度或忙碌状态的非模态卡片提示的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "media_toast.h"


#include <QLabel>
#include <QFont>
#include <QEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QProgressBar>
#include <QGraphicsDropShadowEffect>

/** @brief 创建标题、详情、进度条和百分比徽章。 */
DownloadProgressToast::DownloadProgressToast(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    hide();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_card = new QWidget(this);
    m_card->setObjectName("downloadProgressCard");
    m_card->setStyleSheet(
        "QWidget#downloadProgressCard {"
        " background-color: rgba(17, 24, 39, 228);"
        " border: 1px solid rgba(96, 165, 250, 110);"
        " border-radius: 22px;"
        "}"
    );

    auto *shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(0, 0, 0, 90));
    m_card->setGraphicsEffect(shadow);

    auto *cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(28, 24, 28, 24);
    cardLayout->setSpacing(14);

    m_titleLabel = new QLabel(m_card);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(19);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setStyleSheet("color: white; background: transparent;");
    m_titleLabel->setWordWrap(true);
    cardLayout->addWidget(m_titleLabel);

    m_progressBar = new QProgressBar(m_card);
    m_progressBar->setTextVisible(false);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(40);
    m_progressBar->setStyleSheet(
        "QProgressBar {"
        " color: white;"
        " background-color: rgba(255,255,255,0.10);"
        " border: 1px solid rgba(255,255,255,0.12);"
        " border-radius: 12px;"
        " padding: 0px;"
        "}"
        "QProgressBar::chunk {"
        " background-color: #22c55e;"
        " border-radius: 12px;"
        "}"
    );

    m_percentBadge = new QWidget(m_progressBar);
    m_percentBadge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_percentBadge->setStyleSheet(
        "background-color: rgba(15, 23, 42, 150);"
        "border: 1px solid rgba(255,255,255,0.14);"
        "border-radius: 8px;"
    );

    auto *badgeLayout = new QHBoxLayout(m_percentBadge);
    badgeLayout->setContentsMargins(8, 1, 8, 1);
    badgeLayout->setSpacing(0);


    m_percentLabel = new QLabel(m_percentBadge);
    QFont percentFont = m_percentLabel->font();
    percentFont.setPointSize(20);
    percentFont.setBold(true);
    m_percentLabel->setFont(percentFont);
    m_percentLabel->setAlignment(Qt::AlignCenter);
    m_percentLabel->setStyleSheet("color: white; background: transparent;");
    m_percentLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_percentLabel->setText("0%");
    badgeLayout->addWidget(m_percentLabel);
    m_progressBar->installEventFilter(this);

    cardLayout->addWidget(m_progressBar);

    m_detailLabel = new QLabel(m_card);
    QFont detailFont = m_detailLabel->font();
    detailFont.setPointSize(13);

    m_detailLabel->setFont(detailFont);
    m_detailLabel->setStyleSheet("color: rgba(255,255,255,0.82); background: transparent;");
    m_detailLabel->setWordWrap(true);

    cardLayout->addWidget(m_detailLabel);

    root->addWidget(m_card);
}

/** @brief 根据父窗口尺寸更新覆盖层和卡片几何。 */
void DownloadProgressToast::updateGeometryByParent()
{
    QWidget *p = parentWidget();
    if (!p) return;

    const int w = qMin(500, qMax(380, p->width() - 64));
    const int h = 164;
    const int x = (p->width() - w) / 2;
    const int y = qMax(36, p->height() / 2 - h / 2);
    setGeometry(x, y, w, h);
}

/** @brief 显示 0~100 的下载进度和可选详情。 */
void DownloadProgressToast::showProgress(const QString &titleText, int percent, const QString &detailText)
{
    updateGeometryByParent();
    m_titleLabel->setText(titleText);
    m_detailLabel->setText(detailText);
    const int boundedPercent = qBound(0, percent, 100);
    m_progressBar->setRange(0, 100);

    // 进度条内部的百分比
    m_progressBar->setValue(boundedPercent);
    m_percentLabel->setText(QString::number(boundedPercent) + "%");
    updatePercentLabelGeometry();
    show();
    raise();
}

/** @brief 显示无确定百分比的忙碌状态。 */
void DownloadProgressToast::showBusy(const QString &titleText, const QString &detailText)
{
    updateGeometryByParent();
    m_titleLabel->setText(titleText);
    m_detailLabel->setText(detailText);
    m_progressBar->setRange(0, 0);
    m_percentLabel->setText("--");

    updatePercentLabelGeometry();
    show();
    raise();
}

/** @brief 隐藏提示卡。 */
void DownloadProgressToast::hideToast()
{
    hide();
}


/** @brief 父窗口尺寸变化时重新居中提示卡。 */
bool DownloadProgressToast::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_progressBar && event) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::LayoutRequest:
            updatePercentLabelGeometry();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

/** @brief 将百分比标签定位到进度条当前进度附近。 */
void DownloadProgressToast::updatePercentLabelGeometry()
{
    if (!m_progressBar || !m_percentLabel) return;

    const QRect r = m_progressBar->rect();
    const int rightPadding = 8;
    const int topPadding = 4;
    const int badgeWidth = 84;
    const int badgeHeight = 24;

    if (m_percentBadge) {
        m_percentBadge->setGeometry(r.width() - badgeWidth - rightPadding,
                                    topPadding,
                                    badgeWidth,
                                    badgeHeight);
        m_percentBadge->raise();
    }

    m_percentLabel->raise();
}
