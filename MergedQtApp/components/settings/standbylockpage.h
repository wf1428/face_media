/**
 * @file standbylockpage.h
 * @brief 待机与锁屏启用状态和超时时间设置页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef STANDBYLOCKPAGE_H
#define STANDBYLOCKPAGE_H

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QGroupBox>

/** @brief 待机与锁屏启用状态和超时时间设置页面。 */
class StandbyLockPage : public QWidget
{
    Q_OBJECT

public:
    /** @brief 创建待机和锁屏开关、秒数输入及操作按钮。 */
    explicit StandbyLockPage(QWidget *parent = nullptr);

    /** @brief 释放页面资源。 */
    ~StandbyLockPage();

    /** @brief 设置待机超时，单位秒。 */
    void setStandbyTimeout(int seconds);
    /** @return 待机超时秒数。 */
    int getStandbyTimeout() const;
    /** @brief 设置锁屏超时，单位秒。 */
    void setLockTimeout(int seconds);
    /** @return 锁屏超时秒数。 */
    int getLockTimeout() const;
    /** @brief 设置是否启用待机。 */
    void setStandbyEnabled(bool enabled);
    /** @return 待机功能已启用时返回 true。 */
    bool isStandbyEnabled() const;
    /** @brief 设置是否启用锁屏。 */
    void setLockEnabled(bool enabled);
    /** @return 锁屏功能已启用时返回 true。 */
    bool isLockEnabled() const;

private slots:
    /** @brief 校验输入并应用当前设置。 */
    void onApplyButtonClicked();
    /** @brief 恢复页面默认设置。 */
    void onResetButtonClicked();
    /** @brief 待机开关变化时更新超时输入可用性。 */
    void onStandbyCheckBoxStateChanged(int state);
    /** @brief 锁屏开关变化时更新超时输入可用性。 */
    void onLockCheckBoxStateChanged(int state);

private:
    /** @brief 创建并布局页面控件。 */
    void setupUI();

    // UI组件
    QLabel *titleLabel;
    QGroupBox *standbyGroup;
    QCheckBox *standbyCheckBox;
    QLineEdit *standbyTimeoutEdit;
    QLabel *standbyUnitLabel;
    QGroupBox *lockGroup;
    QCheckBox *lockCheckBox;
    QLineEdit *lockTimeoutEdit;
    QLabel *lockUnitLabel;
    QPushButton *applyButton;
    QPushButton *resetButton;

    int standbyTimeout;  /**< 待机超时时间，单位秒。 */
    int lockTimeout;     /**< 锁屏超时时间，单位秒。 */
    bool standbyEnabled; /**< 是否启用待机。 */
    bool lockEnabled;    /**< 是否启用锁屏。 */
};

#endif // STANDBYLOCKPAGE_H
