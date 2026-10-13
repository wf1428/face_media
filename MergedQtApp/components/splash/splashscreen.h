/**
 * @file splashscreen.h
 * @brief 应用启动期间显示并按定时器自动结束的启动页。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include <QTimer>
#include <QWidget>
#include <QIcon>

/** @brief 应用启动期间显示并按定时器自动结束的启动页。 */
class SplashScreen : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建启动页 UI 并启动结束定时器。 */
    explicit SplashScreen(QWidget *parent = nullptr);

    /** @brief 停止并释放定时器。 */
    ~SplashScreen();

signals:
    /** @brief 启动显示时间结束时发出。 */
    void splashFinished();

private slots:
    /** @brief 停止定时器并通知主界面显示。 */
    void onTimerTimeout();

private:
    QTimer *m_timer = nullptr; /**< 启动页显示时长定时器。 */

    /** @brief 创建背景、图标或标题布局。 */
    void setupUI();
};

#endif // SPLASHSCREEN_H
