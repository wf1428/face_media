/**
 * @file systeminfopage.h
 * @brief 展示 RK3566 硬件、软件、网络和运行时长的系统信息页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SYSTEMINFOPAGE_H
#define SYSTEMINFOPAGE_H

#include <QWidget>

class QGroupBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;

/** @brief 展示 RK3566 硬件、软件、网络和运行时长的系统信息页面。 */
class SystemInfoPage : public QWidget
{
    Q_OBJECT

public:
    /** @brief 创建信息分组和运行时长定时器。 */
    explicit SystemInfoPage(QWidget *parent = nullptr);

    /** @brief 停止并释放定时器。 */
    ~SystemInfoPage() override;

protected:
    /** @brief 页面显示时刷新完整信息并启动运行时长更新。 */
    void showEvent(QShowEvent *event) override;

    /** @brief 页面隐藏时停止周期更新。 */
    void hideEvent(QHideEvent *event) override;

private slots:
    /** @brief 用户点击后重新读取全部系统信息。 */
    void onRefreshButtonClicked();

    /** @brief 更新系统启动时长文本。 */
    void updateUptime();

private:
    /** @brief 创建硬件、软件和网络信息 UI。 */
    void setupUI();

    /** @brief 读取 CPU、温度、内存、存储、版本和网络信息。 */
    void updateSystemInfo();

    QLabel *titleLabel = nullptr;
    QGroupBox *hardwareGroupBox = nullptr;
    QGroupBox *softwareGroupBox = nullptr;
    QGroupBox *networkGroupBox = nullptr;

    QLabel *cpuInfoLabel = nullptr;
    QLabel *gpuInfoLabel = nullptr;
    QLabel *temperatureLabel = nullptr;
    QLabel *serialNumberLabel = nullptr;
    QLabel *memoryInfoLabel = nullptr;
    QLabel *storageInfoLabel = nullptr;

    QLabel *osVersionLabel = nullptr;
    QLabel *kernelVersionLabel = nullptr;
    QLabel *appVersionLabel = nullptr;
    QLabel *systemUptimeLabel = nullptr;

    QLabel *macAddressLabel = nullptr;
    QLabel *ipAddressLabel = nullptr;

    QPushButton *refreshButton = nullptr;
    QTimer *uptimeTimer = nullptr;
};

#endif // SYSTEMINFOPAGE_H
