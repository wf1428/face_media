/**
 * @file standbylockpage.cpp
 * @brief 待机与锁屏启用状态和超时时间设置页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "standbylockpage.h"
#include <QMessageBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIntValidator>

/** @brief 创建待机和锁屏开关、秒数输入及操作按钮。 */
StandbyLockPage::StandbyLockPage(QWidget *parent) : QWidget(parent),
    standbyTimeout(300), // 默认5分钟
    lockTimeout(600),    // 默认10分钟
    standbyEnabled(true),
    lockEnabled(true)
{
    setupUI();
}

/** @brief 释放页面资源。 */
StandbyLockPage::~StandbyLockPage()
{}

/** @brief 创建并布局页面控件。 */
void StandbyLockPage::setupUI()
{
    // 创建主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(30, 30, 30, 30);
    mainLayout->setSpacing(20);
    
    // 创建标题
    titleLabel = new QLabel("待机和锁屏设置", this);
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; color: #333333;");
    mainLayout->addWidget(titleLabel);
    
    // 创建主卡片
    QWidget *mainCard = new QWidget(this);
    mainCard->setStyleSheet(R"(
    QWidget {
            background-color: white;
            border-radius: 12px;
            border: 1px solid #dddddd;
            padding: 20px;
    })");
    
    QVBoxLayout *cardLayout = new QVBoxLayout(mainCard);
    cardLayout->setSpacing(20);
    
    // 创建待机设置组
    standbyGroup = new QGroupBox("待机设置", this);
    standbyGroup->setStyleSheet(R"(
    QGroupBox {
        font-size: 16px;
        font-weight: bold;
        color: #333333;
        border: none;
        margin-bottom: 10px;
    })");
    
    QGridLayout *standbyLayout = new QGridLayout(standbyGroup);
    standbyLayout->setSpacing(15);
    standbyLayout->setColumnStretch(0, 1);
    standbyLayout->setColumnStretch(1, 2);
    
    // 待机开关
    standbyCheckBox = new QCheckBox("启用待机功能", this);
    standbyCheckBox->setChecked(standbyEnabled);
    standbyCheckBox->setStyleSheet(R"(
    QCheckBox {
        font-size: 14px;
        color: #666666;
        spacing: 8px;
    }
    QCheckBox::indicator {
        width: 20px;
        height: 20px;
        border: 2px solid #e0e0e0;
        border-radius: 50%;
        background-color: white;
    }
    QCheckBox::indicator:checked {
        background-color: #409eff;
        border: 1px solid #409eff;
    }
    QCheckBox::indicator:checked::after {
        position: absolute;
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background-color: white;
        border: 1px solid #c0c4cc;
    })");
    standbyLayout->addWidget(standbyCheckBox, 0, 0, 1, 2);
    
    // 待机超时时间
    QLabel *standbyLabel = new QLabel("待机超时时间:", this);
    standbyLabel->setStyleSheet("font-size: 14px; color: #666666;");
    standbyLayout->addWidget(standbyLabel, 1, 0);
    
    QHBoxLayout *standbyTimeoutLayout = new QHBoxLayout();
    standbyTimeoutEdit = new QLineEdit(QString::number(standbyTimeout), this);
    standbyTimeoutEdit->setValidator(new QIntValidator(1, 3600, this));
    standbyTimeoutEdit->setMinimumHeight(36);
    standbyTimeoutEdit->setStyleSheet(R"(
    QLineEdit {
        border: 1px solid #e0e0e0;
        border-radius: 8px;
        padding: 0 12px;
        font-size: 14px;
        color: #333333;
        background-color: white;
    }
    QLineEdit:hover {
        border-color: #409eff;
    }
    QLineEdit:focus {
        border-color: #409eff;
    }
    QLineEdit:disabled {
        background-color: #f5f7fa;
        color: #c0c4cc;
    })");

    standbyTimeoutLayout->addWidget(standbyTimeoutEdit);
    
    standbyUnitLabel = new QLabel("秒", this);
    standbyUnitLabel->setStyleSheet("font-size: 14px; color: #666666; margin-left: 8px;");
    standbyTimeoutLayout->addWidget(standbyUnitLabel);
    standbyTimeoutLayout->addStretch();
    
    standbyLayout->addLayout(standbyTimeoutLayout, 1, 1);
    
    // 创建锁屏设置组
    lockGroup = new QGroupBox("锁屏设置", this);
    lockGroup->setStyleSheet(R"(
    QGroupBox {
        font-size: 16px;
        font-weight: bold;
        color: #333333;
        border: none;
        margin-bottom: 10px;
    })");
    
    QGridLayout *lockLayout = new QGridLayout(lockGroup);
    lockLayout->setSpacing(15);
    lockLayout->setColumnStretch(0, 1);
    lockLayout->setColumnStretch(1, 2);
    
    // 锁屏开关
    lockCheckBox = new QCheckBox("启用锁屏功能", this);
    lockCheckBox->setChecked(lockEnabled);
    lockCheckBox->setStyleSheet(R"(
    QCheckBox {
        font-size: 14px;
        color: #666666;
        spacing: 8px;
    }
    QCheckBox::indicator {
        width: 20px;
        height: 20px;
        border: 2px solid #e0e0e0;
        border-radius: 50%;
        background-color: white;
    }
    QCheckBox::indicator:checked {
        background-color: #409eff;
        border: 1px solid #409eff;
    }
    QCheckBox::indicator:checked::after {
        position: absolute;
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background-color: white;
        border: 1px solid #c0c4cc;
    })");

    lockLayout->addWidget(lockCheckBox, 0, 0, 1, 2);
    
    // 锁屏超时时间
    QLabel *lockLabel = new QLabel("锁屏超时时间:", this);
    lockLabel->setStyleSheet("font-size: 14px; color: #666666;");
    lockLayout->addWidget(lockLabel, 1, 0);
    
    QHBoxLayout *lockTimeoutLayout = new QHBoxLayout();
    lockTimeoutEdit = new QLineEdit(QString::number(lockTimeout), this);
    lockTimeoutEdit->setValidator(new QIntValidator(1, 3600, this));
    lockTimeoutEdit->setMinimumHeight(36);
    lockTimeoutEdit->setStyleSheet(R"(
    QLineEdit {
        border: 1px solid #e0e0e0;
        border-radius: 8px;
        padding: 0 12px;
        font-size: 14px;
        color: #333333;
        background-color: white;
    }
    QLineEdit:hover {
        border-color: #409eff;
    }
    QLineEdit:focus {
        border-color: #409eff;
    }
    QLineEdit:disabled {
        background-color: #f5f7fa;
        color: #c0c4cc;
    })");

    lockTimeoutLayout->addWidget(lockTimeoutEdit);
    
    lockUnitLabel = new QLabel("秒", this);
    lockUnitLabel->setStyleSheet("font-size: 14px; color: #666666; margin-left: 8px;");
    lockTimeoutLayout->addWidget(lockUnitLabel);
    lockTimeoutLayout->addStretch();
    
    lockLayout->addLayout(lockTimeoutLayout, 1, 1);
    
    // 添加设置组到卡片
    cardLayout->addWidget(standbyGroup);
    cardLayout->addWidget(lockGroup);
    
    // 创建按钮布局
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(16);
    buttonLayout->addStretch();
    
    // 重置按钮
    resetButton = new QPushButton("重置", this);
    resetButton->setMinimumSize(100, 40);
    resetButton->setStyleSheet(R"(
    QPushButton {
        background-color: white;
        border: 1px solid #e0e0e0;
        border-radius: 8px;
        color: #606266;
        font-size: 14px;
    }
    QPushButton:hover {
        border-color: #409eff;
        color: #409eff;
    }
    QPushButton:pressed {
        border-color: #337ecc;
        color: #337ecc;
    })");

    buttonLayout->addWidget(resetButton);
    
    // 应用按钮
    applyButton = new QPushButton("应用", this);
    applyButton->setMinimumSize(100, 40);
    applyButton->setStyleSheet(R"(
    QPushButton {
        background-color: #409eff;
        border: none;
        border-radius: 8px;
        color: white;
        font-size: 14px;
    }
    QPushButton:hover {
        background-color: #66b1ff;
    }
    QPushButton:pressed {
        background-color: #337ecc;
    })");

    buttonLayout->addWidget(applyButton);
    
    // 添加按钮布局到卡片
    cardLayout->addLayout(buttonLayout);
    
    // 添加卡片到主布局
    mainLayout->addWidget(mainCard);
    
    // 连接信号槽
    connect(standbyCheckBox, &QCheckBox::stateChanged, this, &StandbyLockPage::onStandbyCheckBoxStateChanged);
    connect(lockCheckBox, &QCheckBox::stateChanged, this, &StandbyLockPage::onLockCheckBoxStateChanged);
    connect(applyButton, &QPushButton::clicked, this, &StandbyLockPage::onApplyButtonClicked);
    connect(resetButton, &QPushButton::clicked, this, &StandbyLockPage::onResetButtonClicked);
    
    // 设置初始状态
    onStandbyCheckBoxStateChanged(standbyEnabled ? Qt::Checked : Qt::Unchecked);
    onLockCheckBoxStateChanged(lockEnabled ? Qt::Checked : Qt::Unchecked);
    
    setLayout(mainLayout);
}

/** @brief 校验输入并应用当前设置。 */
void StandbyLockPage::onApplyButtonClicked()
{
    // 获取设置值
    standbyTimeout = standbyTimeoutEdit->text().toInt();
    lockTimeout = lockTimeoutEdit->text().toInt();
    standbyEnabled = standbyCheckBox->isChecked();
    lockEnabled = lockCheckBox->isChecked();
    
    // 显示成功提示
    QMessageBox::information(this, "提示", "设置已应用", QMessageBox::Ok);
    
    // 这里可以添加保存设置到配置文件的逻辑
    // 或者发送信号通知主程序更新设置
}

/** @brief 恢复页面默认设置。 */
void StandbyLockPage::onResetButtonClicked()
{
    // 重置为默认值
    standbyTimeout = 300;
    lockTimeout = 600;
    standbyEnabled = true;
    lockEnabled = true;
    
    // 更新UI
    standbyCheckBox->setChecked(standbyEnabled);
    lockCheckBox->setChecked(lockEnabled);
    standbyTimeoutEdit->setText(QString::number(standbyTimeout));
    lockTimeoutEdit->setText(QString::number(lockTimeout));
    
    // 更新控件状态
    onStandbyCheckBoxStateChanged(standbyEnabled ? Qt::Checked : Qt::Unchecked);
    onLockCheckBoxStateChanged(lockEnabled ? Qt::Checked : Qt::Unchecked);
    
    QMessageBox::information(this, "提示", "设置已重置为默认值", QMessageBox::Ok);
}

/** @brief 待机开关变化时更新超时输入可用性。 */
void StandbyLockPage::onStandbyCheckBoxStateChanged(int state)
{
    bool enabled = (state == Qt::Checked);
    standbyTimeoutEdit->setEnabled(enabled);
    standbyUnitLabel->setEnabled(enabled);
}

/** @brief 锁屏开关变化时更新超时输入可用性。 */
void StandbyLockPage::onLockCheckBoxStateChanged(int state)
{
    bool enabled = (state == Qt::Checked);
    lockTimeoutEdit->setEnabled(enabled);
    lockUnitLabel->setEnabled(enabled);
}

/** @brief 设置待机超时，单位秒。 */
void StandbyLockPage::setStandbyTimeout(int seconds)
{
    standbyTimeout = seconds;
    standbyTimeoutEdit->setText(QString::number(seconds));
}

/** @return 待机超时秒数。 */
int StandbyLockPage::getStandbyTimeout() const
{
    return standbyTimeout;
}

/** @brief 设置锁屏超时，单位秒。 */
void StandbyLockPage::setLockTimeout(int seconds)
{
    lockTimeout = seconds;
    lockTimeoutEdit->setText(QString::number(seconds));
}

/** @return 锁屏超时秒数。 */
int StandbyLockPage::getLockTimeout() const
{
    return lockTimeout;
}

/** @brief 设置是否启用待机。 */
void StandbyLockPage::setStandbyEnabled(bool enabled)
{
    standbyEnabled = enabled;
    standbyCheckBox->setChecked(enabled);
    onStandbyCheckBoxStateChanged(enabled ? Qt::Checked : Qt::Unchecked);
}

/** @return 待机功能已启用时返回 true。 */
bool StandbyLockPage::isStandbyEnabled() const
{
    return standbyEnabled;
}

/** @brief 设置是否启用锁屏。 */
void StandbyLockPage::setLockEnabled(bool enabled)
{
    lockEnabled = enabled;
    lockCheckBox->setChecked(enabled);
    onLockCheckBoxStateChanged(enabled ? Qt::Checked : Qt::Unchecked);
}

/** @return 锁屏功能已启用时返回 true。 */
bool StandbyLockPage::isLockEnabled() const
{
    return lockEnabled;
}
