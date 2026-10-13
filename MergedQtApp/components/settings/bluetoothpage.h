/**
 * @file bluetoothpage.h
 * @brief 蓝牙开关、扫描、连接和断开操作页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef BLUETOOTHPAGE_H
#define BLUETOOTHPAGE_H

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QCheckBox>
#include <QHBoxLayout>

/** @brief 蓝牙开关、扫描、连接和断开操作页面。 */
class BluetoothPage : public QWidget
{
    Q_OBJECT

public:
    /** @brief 创建蓝牙页面控件。 */
    explicit BluetoothPage(QWidget *parent = nullptr);

    /** @brief 释放页面资源。 */
    ~BluetoothPage();

private slots:
    /** @brief 扫描可用蓝牙设备并刷新列表。 */
    void onScanButtonClicked();

    /** @brief 请求连接当前选中设备。 */
    void onConnectButtonClicked();

    /** @brief 请求断开当前选中设备。 */
    void onDisconnectButtonClicked();

private:
    /** @brief 创建标题、开关、设备列表和操作按钮。 */
    void setupUI();
    
    QLabel *titleLabel;                       /**< 页面标题。 */
    QCheckBox *bluetoothEnabledCheckBox;      /**< 蓝牙启用开关。 */
    QListWidget *deviceListWidget;            /**< 扫描到的设备列表。 */
    QPushButton *scanButton;                  /**< 扫描按钮。 */
    QPushButton *connectButton;               /**< 连接按钮。 */
    QPushButton *disconnectButton;            /**< 断开按钮。 */
};

#endif // BLUETOOTHPAGE_H
