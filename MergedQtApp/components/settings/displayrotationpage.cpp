/**
 * @file displayrotationpage.cpp
 * @brief Weston 显示方向查询与切换页面的实现。
 */

#include "displayrotationpage.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kRotationRequestDelayMs = 120;
}

DisplayRotationPage::DisplayRotationPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("displayRotationPage"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("界面显示方向"), this);
    title->setObjectName(QStringLiteral("pageTitle"));

    auto *group = new QGroupBox(QStringLiteral("屏幕旋转"), this);
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(16, 22, 16, 16);
    groupLayout->setSpacing(12);

    currentRotationLabel_ = new QLabel(QStringLiteral("当前方向：读取中"), group);
    currentRotationLabel_->setObjectName(QStringLiteral("metricName"));

    auto *selectionRow = new QHBoxLayout();
    auto *selectionLabel = new QLabel(QStringLiteral("目标方向"), group);
    selectionLabel->setObjectName(QStringLiteral("metricName"));
    rotationCombo_ = new QComboBox(group);
    rotationCombo_->setEditable(false);
    rotationCombo_->addItem(QStringLiteral("正常（0°）"), 0);
    rotationCombo_->addItem(QStringLiteral("顺时针 90°"), 90);
    rotationCombo_->addItem(QStringLiteral("旋转 180°"), 180);
    rotationCombo_->addItem(QStringLiteral("顺时针 270°"), 270);
    rotationCombo_->setMinimumWidth(180);
    selectionRow->addWidget(selectionLabel);
    selectionRow->addWidget(rotationCombo_, 1);

    applyButton_ = new QPushButton(QStringLiteral("应用旋转"), group);
    applyButton_->setObjectName(QStringLiteral("toolButton"));
    applyButton_->setMinimumHeight(38);

    statusLabel_ = new QLabel(group);
    statusLabel_->setObjectName(QStringLiteral("pageHint"));
    statusLabel_->setWordWrap(true);

    groupLayout->addWidget(currentRotationLabel_);
    groupLayout->addLayout(selectionRow);
    groupLayout->addWidget(applyButton_, 0, Qt::AlignLeft);
    groupLayout->addWidget(statusLabel_);

    layout->addWidget(title);
    layout->addWidget(group);
    layout->addStretch();

    queryProcess_ = new QProcess(this);
    connect(queryProcess_,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        queryStarted_ = false;
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            currentRotationLabel_->setText(QStringLiteral("当前方向：读取失败"));
            statusLabel_->setText(QStringLiteral("请检查 /usr/local/sbin/weston-control 是否已正确安装。"));
            return;
        }

        bool ok = false;
        const int degrees = QString::fromLocal8Bit(queryProcess_->readAllStandardOutput())
                                .trimmed().toInt(&ok);
        if (!ok || (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270)) {
            currentRotationLabel_->setText(QStringLiteral("当前方向：配置无效"));
            return;
        }
        setCurrentRotation(degrees);
        statusLabel_->clear();
    });
    connect(queryProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            queryStarted_ = false;
            currentRotationLabel_->setText(QStringLiteral("当前方向：控制脚本不可用"));
            statusLabel_->setText(QStringLiteral("请先安装 weston-control。"));
        }
    });
    connect(applyButton_, &QPushButton::clicked,
            this, &DisplayRotationPage::requestRotation);

    QTimer::singleShot(0, this, [this]() { refreshCurrentRotation(); });
}

void DisplayRotationPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshCurrentRotation();
}

QString DisplayRotationPage::controlProgram() const
{
    const QByteArray overridePath = qgetenv("YCEST_WESTON_CONTROL").trimmed();
    if (!overridePath.isEmpty()) {
        return QString::fromLocal8Bit(overridePath);
    }
    return QStringLiteral("/usr/local/sbin/weston-control");
}

void DisplayRotationPage::refreshCurrentRotation()
{
    if (!queryProcess_ || queryStarted_ ||
        queryProcess_->state() != QProcess::NotRunning) {
        return;
    }

    const QString program = controlProgram();
    if (!QFileInfo(program).isExecutable()) {
        currentRotationLabel_->setText(QStringLiteral("当前方向：控制脚本未安装"));
        statusLabel_->setText(QStringLiteral("缺少 /usr/local/sbin/weston-control。"));
        return;
    }

    queryStarted_ = true;
    currentRotationLabel_->setText(QStringLiteral("当前方向：读取中"));
    queryProcess_->start(program, QStringList() << QStringLiteral("current"));
}

void DisplayRotationPage::requestRotation()
{
    const int degrees = rotationCombo_->currentData().toInt();
    if (degrees == currentRotation_) {
        statusLabel_->setText(QStringLiteral("当前已经是所选方向，无需重启。"));
        return;
    }

    const QString program = controlProgram();
    if (!QFileInfo(program).isExecutable()) {
        statusLabel_->setText(QStringLiteral("旋转失败：weston-control 未安装或不可执行。"));
        return;
    }

    // 资源配置只决定横屏或竖屏：横屏允许 0/180 度，竖屏允许 90/270 度。
    // 先校验再启动旋转事务，避免不匹配请求导致 Qt/Weston 无意义重启。
    QProcess validationProcess;
    validationProcess.start(
        program,
        QStringList() << QStringLiteral("validate-rotation")
                      << QString::number(degrees));
    if (!validationProcess.waitForStarted(1000) ||
        !validationProcess.waitForFinished(2000)) {
        validationProcess.kill();
        statusLabel_->setText(QStringLiteral("无法读取界面资源方向，旋转请求已忽略。"));
        return;
    }
    if (validationProcess.exitStatus() != QProcess::NormalExit ||
        validationProcess.exitCode() != 0) {
        if (validationProcess.exitCode() == 3) {
            statusLabel_->setText(
                QStringLiteral("所选方向与界面资源的横竖布局不一致，旋转请求已忽略。"));
        } else {
            statusLabel_->setText(
                QStringLiteral("界面资源方向配置无效，旋转请求已忽略。"));
        }
        return;
    }

    applyButton_->setEnabled(false);
    rotationCombo_->setEnabled(false);
    statusLabel_->setText(QStringLiteral("正在切换显示方向，Qt 程序将自动重启……"));

    // 先让提示完成一次绘制，再脱离当前 Qt 进程启动控制脚本。控制脚本会
    // 正常结束本进程，因此不能使用阻塞式 QProcess::execute()。
    QTimer::singleShot(kRotationRequestDelayMs, this, [this, program, degrees]() {
        const bool started = QProcess::startDetached(
            program,
            QStringList() << QStringLiteral("rotate") << QString::number(degrees));
        if (!started) {
            applyButton_->setEnabled(true);
            rotationCombo_->setEnabled(true);
            statusLabel_->setText(QStringLiteral("旋转请求启动失败，请检查脚本权限。"));
        }
    });
}

void DisplayRotationPage::setCurrentRotation(int degrees)
{
    currentRotation_ = degrees;
    currentRotationLabel_->setText(
        QStringLiteral("当前方向：%1°").arg(degrees));
    const int index = rotationCombo_->findData(degrees);
    if (index >= 0) {
        rotationCombo_->setCurrentIndex(index);
    }
    applyButton_->setEnabled(true);
    rotationCombo_->setEnabled(true);
}
