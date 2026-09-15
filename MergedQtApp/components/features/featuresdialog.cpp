/**
 * @file featuresdialog.cpp
 * @brief 编辑功能开关并约束离线、在线 V1、在线 V2 协议互斥关系。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "featuresdialog.h"
#include "ui_featuresdialog.h"
#include "common/responsive_ui.h"

#include <QDebug>
#include <QCheckBox>
#include <QTimer>
#include <QSignalBlocker>
#include <QEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QWidget>

/** @brief 创建功能复选框并建立互斥约束连接。 */
FeaturesDialog::FeaturesDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FeaturesDialog)
{
    embeddedForWayland_ = ResponsiveUi::isWaylandPlatform() && parentWidget();
    if (embeddedForWayland_) {
        setWindowFlags(Qt::Widget);
        parentWidget()->installEventFilter(this);
    }
    ui->setupUi(this);
    if (embeddedForWayland_) {
        // setupUi() 会应用 .ui 中的 modal=false，Wayland 内嵌后重新恢复模态语义。
        setModal(true);
    }

    const QSize targetSize = ResponsiveUi::dialogSize(
                parent, QSize(450, 560), QSize(400, 480), 16);
    setMinimumSize(qMin(400, targetSize.width()),
                   qMin(480, targetSize.height()));
    resize(targetSize);

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
    delete waylandBlocker_;
    delete ui;
}

bool FeaturesDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (embeddedForWayland_ && watched == parentWidget() && event &&
            event->type() == QEvent::Resize) {
        if (waylandBlocker_) {
            waylandBlocker_->setGeometry(parentWidget()->rect());
        }
        centerInHost();
    }
    return QDialog::eventFilter(watched, event);
}

void FeaturesDialog::showEvent(QShowEvent *event)
{
    if (embeddedForWayland_ && parentWidget()) {
        if (!waylandBlocker_) {
            waylandBlocker_ = new QWidget(parentWidget());
            waylandBlocker_->setObjectName(QStringLiteral("waylandModalBlocker"));
            waylandBlocker_->setAttribute(Qt::WA_StyledBackground, true);
            waylandBlocker_->setStyleSheet(QStringLiteral(
                "#waylandModalBlocker{background:rgba(0,0,0,90);}"));
        }
        waylandBlocker_->setGeometry(parentWidget()->rect());
        waylandBlocker_->show();
        waylandBlocker_->raise();
        centerInHost();
    }

    QDialog::showEvent(event);
    if (embeddedForWayland_) {
        raise();
    }
}

void FeaturesDialog::hideEvent(QHideEvent *event)
{
    if (waylandBlocker_) {
        waylandBlocker_->hide();
    }
    QDialog::hideEvent(event);
}

void FeaturesDialog::centerInHost()
{
    if (!embeddedForWayland_ || !parentWidget()) {
        return;
    }
    const QRect host = parentWidget()->rect();
    move(host.x() + qMax(0, (host.width() - width()) / 2),
         host.y() + qMax(0, (host.height() - height()) / 2));
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
