/**
 * @file media_toast.h
 * @brief 居中显示下载进度或忙碌状态的非模态卡片提示。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MEDIA_TOAST_H
#define MEDIA_TOAST_H


#include <QWidget>

class QLabel;
class QProgressBar;
class QEvent;

/** @brief 居中显示下载进度或忙碌状态的非模态卡片提示。 */
class DownloadProgressToast : public QWidget
{
public:
    /** @brief 创建标题、详情、进度条和百分比徽章。 */
    explicit DownloadProgressToast(QWidget *parent = nullptr);

    /** @brief 显示 0~100 的下载进度和可选详情。 */
    void showProgress(const QString &titleText, int percent, const QString &detailText = QString());

    /** @brief 显示无确定百分比的忙碌状态。 */
    void showBusy(const QString &titleText, const QString &detailText = QString());

    /** @brief 隐藏提示卡。 */
    void hideToast();

protected:
    /** @brief 父窗口尺寸变化时重新居中提示卡。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @brief 根据父窗口尺寸更新覆盖层和卡片几何。 */
    void updateGeometryByParent();

    /** @brief 将百分比标签定位到进度条当前进度附近。 */
    void updatePercentLabelGeometry();

private:
    QWidget *m_card = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_detailLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QWidget *m_percentBadge = nullptr;
    QLabel *m_percentLabel = nullptr;
};



#endif // MEDIA_TOAST_H
