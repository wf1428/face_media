/**
 * @file udisk_status.h
 * @brief 轮询 U 盘状态 JSON 并在宿主底部显示状态/进度的覆盖层。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef UDISK_STATUS_H
#define UDISK_STATUS_H

#include <QObject>
#include <QString>
#include <QDateTime>

class QWidget;
class QLabel;
class QProgressBar;
class QTimer;

/** @brief 轮询 U 盘状态 JSON 并在宿主底部显示状态/进度的覆盖层。 */
class UdiskStatusOverlay : public QObject
{
    Q_OBJECT
public:
    /** @brief 保存宿主窗口并创建覆盖层控件。 */
    explicit UdiskStatusOverlay(QWidget *hostWindow, QObject *parent = nullptr);

    /** @brief 以指定毫秒间隔启动状态轮询。 */
    void start(int intervalMs = 300);

    /** @brief 停止轮询并隐藏覆盖层。 */
    void stop();

    /** @brief 显式显示或隐藏覆盖层。 */
    void setVisible(bool on);

    /** @return 覆盖层当前可见时返回 true。 */
    bool isVisible() const;

    /** @brief 临时显示媒体业务消息，并在 durationMs 后恢复自动状态。 */
    void showMessage(const QString &text, int durationMs);

signals:
    /** @brief U 盘状态或进度变化时通知外部。 */
    void udiskStateChanged(const QString &state, int progress);

private slots:
    /** @brief 读取一次状态文件并刷新覆盖层。 */
    void pollOnce();

private:
    /** @brief 创建半透明标签和进度条。 */
    void initUi();

    /** @brief 将覆盖层贴到宿主窗口底部。 */
    void layoutUi();

    /** @brief 解析状态 JSON，处理插入、复制进度、完成和拔出。 */
    void updateFromJson(const QByteArray &data);

private:
    QWidget *m_host = nullptr;         // MainWindow
    QWidget *m_overlay = nullptr;      // 半透明容器
    QLabel *m_label = nullptr;         // 状态文字
    QProgressBar *m_progress = nullptr;// 进度条
    QTimer *m_timer = nullptr;

    bool m_seenInserted = false;   // 是否曾经插入过U盘（用于控制首次不显示“已拔出”）
    int  m_hideToken = 0;          // 用于取消旧的延迟隐藏任务

    int m_doneTipMs = 2500;     // 提示显示时长（2~3s）

    QString m_lastState;
    int m_lastProgress = -1;

    qint64 m_manualMessageUntilMs = 0;

public slots:
    /** @brief 宿主尺寸变化后重新执行贴底布局。 */
    void onHostResized();
};

#endif // UDISK_STATUS_H
