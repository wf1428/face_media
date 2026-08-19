/**
 * @file featuresdialog.cpp
 * @brief 编辑功能开关并约束离线、在线 V1、在线 V2 协议互斥关系。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "featuresdialog.h"
#include "ui_featuresdialog.h"

#include <QDebug>
#include <QCheckBox>
#include <QTimer>
#include <QSignalBlocker>

/** @brief 创建功能复选框并建立互斥约束连接。 */
FeaturesDialog::FeaturesDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FeaturesDialog)
{
    ui->setupUi(this);

    setupConnections();

    // 防止回车触发关闭
    if (ui->applyButton) {
        ui->applyButton->setAutoDefault(false);
        ui->applyButton->setDefault(false);
    }
}

/** @brief 释放 Qt Designer 界面对象。 */
FeaturesDialog::~FeaturesDialog()
{
    delete ui;
}

/** @brief 建立复选框和对话框按钮信号连接。 */
void FeaturesDialog::setupConnections()
{
    // feature checkbox
    connect(ui->feature1, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));
    connect(ui->feature2, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));
    connect(ui->feature3, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));
    connect(ui->feature4, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));
    connect(ui->feature5, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));
    connect(ui->feature6, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));
    connect(ui->feature7, SIGNAL(stateChanged(int)), this, SLOT(onFeatureChecked(int)));

    // Apply 按钮
    connect(ui->applyButton, &QPushButton::clicked, this, [this](){
        ui->statusLabel->setText("配置已应用");

        QTimer::singleShot(2000, this, [this](){
            ui->statusLabel->clear();
        });

        accept();   // 关闭对话框
    });
}

/** @brief 保证 offline_v1、online_v1、online_v2 最多选中一个。 */
void FeaturesDialog::onFeatureChecked(int state)
{
    QCheckBox *senderBox = qobject_cast<QCheckBox*>(sender());
    if (!senderBox) return;

    // offline_v1 / online_v1 / online_v2 三者互斥，只能选一个
    if (state == Qt::Checked) {
        QList<QCheckBox*> exclusiveBoxes = {
            ui->feature1,   // offline_v1
            ui->feature2,   // online_v1
            ui->feature3    // online_v2
        };

        for (QCheckBox *box : exclusiveBoxes) {
            if (!box || box == senderBox) continue;
            if (box->isChecked()) {
                QSignalBlocker blocker(box);
                box->setChecked(false);
            }
        }
    }

    const QString featureName = senderBox->text();
    const QString statusText =
            (state == Qt::Checked)
            ? QString("已启用 %1").arg(featureName)
            : QString("已禁用 %1").arg(featureName);

    qDebug() << statusText;
    ui->statusLabel->setText(statusText);

    QTimer::singleShot(2000, this, [this](){
        ui->statusLabel->clear();
    });
}


/** @return 当前复选框对应的功能设置。 */
FeatureSettings FeaturesDialog::getFeatureSettings() const
{
    FeatureSettings settings;
    settings.offline_v1 = ui->feature1->isChecked();
    settings.online_v1  = ui->feature2->isChecked();
    settings.online_v2  = ui->feature3->isChecked();
    settings.air_sys    = ui->feature3->isChecked();
    settings.media_sys  = ui->feature4->isChecked();
    settings.broad_sys  = ui->feature5->isChecked();
    settings.tbk_sys    = ui->feature6->isChecked();
    return settings;
}


/** @brief 将功能设置回显到复选框。 */
void FeaturesDialog::setFeatureSettings(const FeatureSettings &settings)
{
    ui->feature1->setChecked(settings.offline_v1);
    ui->feature2->setChecked(settings.online_v1 );
    ui->feature3->setChecked(settings.online_v2 );
    ui->feature3->setChecked(settings.air_sys   );
    ui->feature4->setChecked(settings.media_sys );
    ui->feature5->setChecked(settings.broad_sys );
    ui->feature6->setChecked(settings.tbk_sys   );
}
