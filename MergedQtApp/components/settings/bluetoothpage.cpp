/**
 * @file bluetoothpage.cpp
 * @brief 蓝牙开关、扫描、连接和断开操作页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "bluetoothpage.h"
#include <QMessageBox>
#include <QGroupBox>
#include <QAbstractItemView>

/** @brief 创建蓝牙页面控件。 */
BluetoothPage::BluetoothPage(QWidget *parent) : QWidget(parent)
{
    setupUI();
}

/** @brief 释放页面资源。 */
BluetoothPage::~BluetoothPage()
{}

/** @brief 创建标题、开关、设备列表和操作按钮。 */
void BluetoothPage::setupUI()
{
    // 创建主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(30, 30, 30, 30);
    
    // 创建标题
    titleLabel = new QLabel("蓝牙连接设置", this);
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; margin-bottom: 30px;");
    mainLayout->addWidget(titleLabel);
    
    // 创建启用蓝牙选项
    QWidget *enableWidget = new QWidget(this);
    QHBoxLayout *enableLayout = new QHBoxLayout(enableWidget);
    
    bluetoothEnabledCheckBox = new QCheckBox("启用蓝牙", enableWidget);
    bluetoothEnabledCheckBox->setChecked(true);
    enableLayout->addWidget(bluetoothEnabledCheckBox);
    enableLayout->addStretch();
    
    mainLayout->addWidget(enableWidget);
    mainLayout->addSpacing(20);
    
    // 创建设备列表组
    QGroupBox *deviceGroup = new QGroupBox("可用设备", this);
    QVBoxLayout *deviceLayout = new QVBoxLayout(deviceGroup);
    
    deviceListWidget = new QListWidget(deviceGroup);
    deviceListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    
    // 添加一些示例设备
    deviceListWidget->addItem("设备1 - 蓝牙耳机");
    deviceListWidget->addItem("设备2 - 蓝牙音箱");
    deviceListWidget->addItem("设备3 - 手机");
    
    deviceLayout->addWidget(deviceListWidget);
    
    mainLayout->addWidget(deviceGroup);
    mainLayout->addSpacing(20);
    
    // 创建按钮布局
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    
    scanButton = new QPushButton("扫描设备", this);
    scanButton->setMinimumWidth(120);
    scanButton->setStyleSheet("QPushButton { background-color: #1a73e8; color: white; border-radius: 4px; padding: 8px 16px; } "
                            "QPushButton:hover { background-color: #1557b0; } ");
    
    connectButton = new QPushButton("连接", this);
    connectButton->setMinimumWidth(100);
    connectButton->setStyleSheet("QPushButton { background-color: #34a853; color: white; border-radius: 4px; padding: 8px 16px; } "
                               "QPushButton:hover { background-color: #2d8f47; } ");
    
    disconnectButton = new QPushButton("断开连接", this);
    disconnectButton->setMinimumWidth(100);
    disconnectButton->setStyleSheet("QPushButton { background-color: #f1f3f4; border: 1px solid #dadce0; border-radius: 4px; padding: 8px 16px; } "
                                  "QPushButton:hover { background-color: #e8eaed; } ");
    
    buttonLayout->addWidget(scanButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(connectButton);
    buttonLayout->addWidget(disconnectButton);
    
    mainLayout->addLayout(buttonLayout);
    
    // 连接信号和槽
    connect(scanButton, &QPushButton::clicked, this, &BluetoothPage::onScanButtonClicked);
    connect(connectButton, &QPushButton::clicked, this, &BluetoothPage::onConnectButtonClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &BluetoothPage::onDisconnectButtonClicked);
    
    // 连接蓝牙启用状态变化
    connect(bluetoothEnabledCheckBox, &QCheckBox::stateChanged, [=](int state) {
        bool isEnabled = (state == Qt::Checked);
        scanButton->setEnabled(isEnabled);
        connectButton->setEnabled(isEnabled);
        disconnectButton->setEnabled(isEnabled);
        deviceListWidget->setEnabled(isEnabled);
    });
    
    setLayout(mainLayout);
}

/** @brief 扫描可用蓝牙设备并刷新列表。 */
void BluetoothPage::onScanButtonClicked()
{
    // 模拟扫描过程
    QMessageBox::information(this, "扫描", "正在扫描蓝牙设备...");
    // 实际应用中这里应该调用蓝牙API进行扫描
}

/** @brief 请求连接当前选中设备。 */
void BluetoothPage::onConnectButtonClicked()
{
    QListWidgetItem *item = deviceListWidget->currentItem();
    if (!item) {
        QMessageBox::warning(this, "警告", "请先选择一个设备");
        return;
    }
    
    QString deviceName = item->text();
    QMessageBox::information(this, "连接", QString("正在连接设备: %1").arg(deviceName));
    // 实际应用中这里应该调用蓝牙API进行连接
}

/** @brief 请求断开当前选中设备。 */
void BluetoothPage::onDisconnectButtonClicked()
{
    QListWidgetItem *item = deviceListWidget->currentItem();
    if (!item) {
        QMessageBox::warning(this, "警告", "请先选择一个设备");
        return;
    }
    
    QString deviceName = item->text();
    QMessageBox::information(this, "断开连接", QString("正在断开设备: %1").arg(deviceName));
    // 实际应用中这里应该调用蓝牙API进行断开连接
}