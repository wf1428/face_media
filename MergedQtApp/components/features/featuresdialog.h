/**
 * @file featuresdialog.h
 * @brief 编辑功能开关并约束离线、在线 V1、在线 V2 协议互斥关系。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef FEATURESDIALOG_H
#define FEATURESDIALOG_H

#include <QDialog>
#include <QTimer>
#include <QSerialPort>
#include <QCheckBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QPropertyAnimation>

#include "command_dialog.h"
#include "common/protocols/icboard_splitter.h"


/** @brief 功能开关和三种互斥协议模式的持久化值对象。 */
struct FeatureSettings
{
    bool offline_v1 = false; /**< 启用离线 IC 协议。 */
    bool online_v1  = false; /**< 启用第一版在线协议。 */
    bool online_v2  = false; /**< 启用第二版在线协议。 */
    bool air_sys    = false; /**< 启用空调信息显示。 */
    bool media_sys  = false; /**< 启用在线多媒体业务。 */
    bool broad_sys  = false; /**< 启用广播功能。 */
    bool tbk_sys    = false; /**< 启用可视对讲功能。 */
};

namespace Ui {
class FeaturesDialog;
}

/** @brief 编辑并约束设备功能开关的设置对话框。 */
class FeaturesDialog : public QDialog
{
    Q_OBJECT

public:
    /** @brief 创建功能复选框并建立互斥约束连接。 */
    explicit FeaturesDialog(QWidget *parent = nullptr);

    /** @brief 释放 Qt Designer 界面对象。 */
    ~FeaturesDialog();

    /** @return 当前复选框对应的功能设置。 */
    FeatureSettings getFeatureSettings() const;

    /** @brief 将功能设置回显到复选框。 */
    void setFeatureSettings(const FeatureSettings &settings);

private slots:
    /** @brief 保证 offline_v1、online_v1、online_v2 最多选中一个。 */
    void onFeatureChecked(int state);

private:
    /** @brief 建立复选框和对话框按钮信号连接。 */
    void setupConnections();

private:
    Ui::FeaturesDialog *ui;
};



#endif // FEATURESDIALOG_H
