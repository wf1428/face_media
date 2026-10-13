/**
 * @file featuresdialog.cpp
 * @brief Qt Quick 功能开关页面及 C++ ViewModel 实现。
 */

#include "featuresdialog.h"

#include "common/responsive_ui.h"
#include "shell/QuickUiRuntime.h"

#include <QColor>
#include <QCheckBox>
#include <QEvent>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QQmlContext>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {
const char *const kFeatureNames[] = {
    "离线版梯控系统 V1",
    "网络版梯控系统 V1",
    "网络版梯控系统 V2",
    "电梯空调系统",
    "多媒体电梯系统",
    "电梯广播系统",
    "可视对讲系统"
};
}

FeaturesDialog::FeaturesDialog(QWidget *parent)
    : QDialog(parent)
{
    embeddedForWayland_ = ResponsiveUi::isWaylandPlatform() && parentWidget();
    if (embeddedForWayland_) {
        setWindowFlags(Qt::Widget);
        parentWidget()->installEventFilter(this);
    } else {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    }
    setModal(true);
    setWindowTitle(QStringLiteral("功能选项"));
    setObjectName(QStringLiteral("QuickFeaturesDialog"));

    const QSize targetSize = ResponsiveUi::dialogSize(
                parent, QSize(500, 620), QSize(420, 500), 16);
    setMinimumSize(qMin(400, targetSize.width()),
                   qMin(480, targetSize.height()));
    resize(targetSize);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    quickView_ = new QQuickWidget(QuickUiRuntime::instance()->engine(), this);
    quickView_->setObjectName(QStringLiteral("quickFeaturesPanel"));
    quickView_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quickView_->setClearColor(QColor(QStringLiteral("#ffffff")));
    quickView_->rootContext()->setContextProperty(
                QStringLiteral("featuresBridge"), this);
    quickView_->setSource(QUrl(QStringLiteral("qrc:/qml/QuickFeaturesPanel.qml")));
    if (QQuickWindow *window = quickView_->quickWindow()) {
        window->setPersistentOpenGLContext(true);
        window->setPersistentSceneGraph(true);
    }
    auto *fallbackPanel = new QWidget(this);
    fallbackPanel->setObjectName(QStringLiteral("featureFallbackPanel"));
    fallbackPanel->setStyleSheet(QStringLiteral(
        "QWidget#featureFallbackPanel{background:#ffffff;color:#303133;}"
        "QLabel{color:#303133;font-size:22px;font-weight:700;}"
        "QCheckBox{color:#606266;font-size:17px;spacing:12px;padding:8px;}"
        "QPushButton{min-width:104px;min-height:46px;border-radius:10px;"
        "background:#ffffff;color:#606266;border:1px solid #dcdfe6;font-size:17px;}"
        "QPushButton#featureFallbackApply{background:#409eff;color:#ffffff;"
        "border-color:#409eff;font-weight:700;}"));
    auto *fallbackLayout = new QVBoxLayout(fallbackPanel);
    fallbackLayout->setContentsMargins(24, 22, 24, 22);
    fallbackLayout->setSpacing(10);
    fallbackLayout->addWidget(new QLabel(QStringLiteral("功能选项"), fallbackPanel));
    for (int index = 0; index < 7; ++index) {
        auto *check = new QCheckBox(QString::fromUtf8(kFeatureNames[index]), fallbackPanel);
        fallbackChecks_.append(check);
        fallbackLayout->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [this, index](bool checked) {
            setFeatureEnabled(index, checked);
        });
    }
    fallbackLayout->addStretch(1);
    auto *fallbackButtons = new QHBoxLayout();
    fallbackButtons->addStretch(1);
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), fallbackPanel);
    auto *applyButton = new QPushButton(QStringLiteral("应用"), fallbackPanel);
    applyButton->setObjectName(QStringLiteral("featureFallbackApply"));
    connect(cancelButton, &QPushButton::clicked,
            this, &FeaturesDialog::cancelAndClose);
    connect(applyButton, &QPushButton::clicked,
            this, &FeaturesDialog::applyAndClose);
    fallbackButtons->addWidget(cancelButton);
    fallbackButtons->addWidget(applyButton);
    fallbackLayout->addLayout(fallbackButtons);

    layout->addWidget(quickView_);
    layout->addWidget(fallbackPanel);
    const auto updatePresentation = [this, fallbackPanel](QQuickWidget::Status status) {
        const bool useFallback = status == QQuickWidget::Error;
        quickView_->setVisible(!useFallback);
        fallbackPanel->setVisible(useFallback);
        if (useFallback) syncFallbackControls();
    };
    connect(quickView_, &QQuickWidget::statusChanged,
            this, updatePresentation);
    updatePresentation(quickView_->status());

    statusTimer_ = new QTimer(this);
    statusTimer_->setSingleShot(true);
    statusTimer_->setInterval(2000);
    connect(statusTimer_, &QTimer::timeout, this, [this]() {
        setStatusText(QString());
    });

    hide();
}

FeaturesDialog::~FeaturesDialog()
{
    if (parentWidget()) {
        parentWidget()->removeEventFilter(this);
    }
    delete waylandBlocker_;
}

FeatureSettings FeaturesDialog::getFeatureSettings() const
{
    FeatureSettings settings;
    settings.offline_v1 = featureEnabled_[0];
    settings.online_v1 = featureEnabled_[1];
    settings.online_v2 = featureEnabled_[2];
    // 保持原页面既有字段映射，不在界面迁移过程中改变配置语义。
    settings.air_sys = featureEnabled_[2];
    settings.media_sys = featureEnabled_[3];
    settings.broad_sys = featureEnabled_[4];
    settings.tbk_sys = featureEnabled_[5];
    return settings;
}

void FeaturesDialog::setFeatureSettings(const FeatureSettings &settings)
{
    featureEnabled_[0] = settings.offline_v1;
    featureEnabled_[1] = settings.online_v1;
    featureEnabled_[2] = settings.online_v2;
    // 原实现随后用 air_sys 覆盖第三项，并未回显第七项；这里保持兼容。
    featureEnabled_[2] = settings.air_sys;
    featureEnabled_[3] = settings.media_sys;
    featureEnabled_[4] = settings.broad_sys;
    featureEnabled_[5] = settings.tbk_sys;
    featureEnabled_[6] = false;
    syncFallbackControls();
    emit featureItemsChanged();
}

QVariantList FeaturesDialog::featureItems() const
{
    QVariantList result;
    result.reserve(7);
    for (int index = 0; index < 7; ++index) {
        QVariantMap item;
        item.insert(QStringLiteral("text"),
                    QString::fromUtf8(kFeatureNames[index]));
        item.insert(QStringLiteral("enabled"), featureEnabled_[index]);
        result.append(item);
    }
    return result;
}

QString FeaturesDialog::statusText() const
{
    return statusText_;
}

void FeaturesDialog::setFeatureEnabled(int index, bool enabled)
{
    if (index < 0 || index >= 7 || featureEnabled_[index] == enabled) {
        return;
    }
    featureEnabled_[index] = enabled;
    if (enabled && index < 3) {
        for (int other = 0; other < 3; ++other) {
            if (other != index) {
                featureEnabled_[other] = false;
            }
        }
    }
    syncFallbackControls();
    emit featureItemsChanged();
    setStatusText(QStringLiteral("已%1 %2")
                  .arg(enabled ? QStringLiteral("启用") : QStringLiteral("禁用"),
                       QString::fromUtf8(kFeatureNames[index])));
    statusTimer_->start();
}

void FeaturesDialog::applyAndClose()
{
    setStatusText(QStringLiteral("配置已应用"));
    accept();
}

void FeaturesDialog::cancelAndClose()
{
    reject();
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
    QuickUiRuntime *runtime = QuickUiRuntime::instance();
    previousModuleId_ = runtime->activeModuleId();
    runtime->synchronizeActiveModule(
                QuickUiRuntime::moduleId(QuickUiRuntime::FeaturePagesModule));
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
    QuickUiRuntime::instance()->synchronizeActiveModule(
                previousModuleId_.isEmpty()
                ? QuickUiRuntime::moduleId(QuickUiRuntime::MultimediaMainModule)
                : previousModuleId_);
    previousModuleId_.clear();
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

void FeaturesDialog::setStatusText(const QString &text)
{
    if (statusText_ == text) {
        return;
    }
    statusText_ = text;
    emit statusTextChanged();
}

/** @brief QML 加载失败时，使备用复选框与同一份功能状态保持一致。 */
void FeaturesDialog::syncFallbackControls()
{
    const int count = qMin(fallbackChecks_.size(), 7);
    for (int index = 0; index < count; ++index) {
        QCheckBox *check = fallbackChecks_.at(index);
        if (!check) continue;
        const QSignalBlocker blocker(check);
        check->setChecked(featureEnabled_[index]);
    }
}
