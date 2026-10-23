/**
 * @file settingsdialog.cpp
 * @brief 网络、蓝牙、MQTT、系统、显示、待机和 FTP 页面容器及信号中转器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "settingsdialog.h"
#include "networkpage.h"
#include "bluetoothpage.h"
#include "mqttpage.h"
#include "systeminfopage.h"
#include "standbylockpage.h"
#include "ftppage.h"
#include "displayrotationpage.h"
#include "common/responsive_ui.h"
#include "components/quick/QuickNavigationWidget.h"
#include "shell/QuickUiRuntime.h"

#include <QDebug>
#include <QBoxLayout>
#include <QStyle>
#include <QGroupBox>
#include <QGridLayout>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QScrollArea>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QSizePolicy>
#include "platform/rk3566_platform.h"

/** @brief 创建导航按钮、页面栈和自动连接管理器。 */
SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    if (ResponsiveUi::isWaylandPlatform() && parentWidget()) {
        setWindowFlags(Qt::Widget);
        setAttribute(Qt::WA_StyledBackground, true);
    }
    setupUI();

    // 创建自动连接管理器
    autoConnectMgr = new AutoConnectManager(this);
    autoConnectMgr->setConfigLocation(configDir_, configFileName_);
    autoConnectMgr->bindMqttPage(mqttPage);

    setupConnections();

    // 设置窗口属性
    setWindowTitle("设置");

    // 测试阶段由 backPic_x_Size/backPic_y_Size 统一决定布局方向和缩放；
    // 窗口几何仍受实际宿主区域限制，配置无效时回退到原有逻辑。
    setMinimumSize(ResponsiveUi::kMinimumScreenExtent,
                   ResponsiveUi::kMinimumScreenExtent);
    if (parentWidget()) {
        parentWidget()->installEventFilter(this);
    }
    fitToHost();

    // 默认显示网络连接页面
    onNetworkButtonClicked();

    autoConnectMgr->startIfEnabled(500);

    // Wayland 下本窗口是 MainWindow 的普通子控件。若不显式隐藏，父窗口首次
    // show() 时 Qt 会把尚未设置显隐状态的子控件一并显示，造成开机直接覆盖
    // 到网络设置页。保持初始隐藏，只在用户点击“设置”入口时显示。
    hide();
}

/** @brief 释放设置子页面和 UI 资源。 */
SettingsDialog::~SettingsDialog()
{}

/** @brief 创建导航栏和内容页面栈。 */
void SettingsDialog::setupUI()
{
    // 创建主布局
    mainLayout_ = new QHBoxLayout(this);
    mainLayout_->setContentsMargins(0, 0, 0, 0);
    mainLayout_->setSpacing(0);
    mainLayout_->setSizeConstraint(QLayout::SetNoConstraint);

    // 创建左侧导航栏
    sidebarWidget_ = new QWidget(this);
    sidebarWidget_->setStyleSheet("background-color: #f0f0f0; border-right: 1px solid #d0d0d0;");

    navigationLayout_ = new QGridLayout(sidebarWidget_);
    navigationLayout_->setSpacing(10);
    navigationLayout_->setContentsMargins(14, 18, 14, 14);

    // 创建导航按钮
    networkButton = new QPushButton("网络连接", sidebarWidget_);
    bluetoothButton = new QPushButton("蓝牙连接", sidebarWidget_);
    mqttButton = new QPushButton("MQTT连接", sidebarWidget_);
    ftpButton = new QPushButton("视频下载", sidebarWidget_);
    standbyLockButton = new QPushButton("待机和锁屏", sidebarWidget_);
    systemInfoButton = new QPushButton("系统信息", sidebarWidget_);
    displayButton = new QPushButton("显示设置", sidebarWidget_);


    // 设置按钮样式（模拟 element-ui 风格）
    QList<QPushButton*> buttons = {networkButton, bluetoothButton, mqttButton, ftpButton,
                                   standbyLockButton, systemInfoButton, displayButton};
    for (auto button : buttons) {
        button->setMinimumHeight(40);
        button->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    }
    for (int index = 0; index < buttons.size(); ++index) {
        navigationLayout_->addWidget(buttons.at(index), index, 0);
    }
    navigationLayout_->setRowStretch(buttons.size(), 1);

    // 创建内容区域
    contentStack = new QStackedWidget(this);
    contentStack->setMinimumSize(0, 0);
    contentStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    // 创建各个子页面
    networkPage = new NetworkPage(contentStack);
    bluetoothPage = new BluetoothPage(contentStack);
    mqttPage = new MqttPage(contentStack);
    ftpPage = new FtpPage(contentStack);
    standbyLockPage = new StandbyLockPage(contentStack);
    systemInfoPage = new SystemInfoPage(contentStack);
    displayPage = new DisplayRotationPage(contentStack);

    // 普通页面由滚动容器兜底；MQTT/FTP 已有自己的长内容滚动区。
    networkContainer_ = wrapScrollablePage(networkPage);
    bluetoothContainer_ = wrapScrollablePage(bluetoothPage);
    mqttContainer_ = mqttPage;
    ftpContainer_ = ftpPage;
    standbyLockContainer_ = wrapScrollablePage(standbyLockPage);
    systemInfoContainer_ = wrapScrollablePage(systemInfoPage);
    displayContainer_ = wrapScrollablePage(displayPage);

    contentStack->addWidget(networkContainer_);
    contentStack->addWidget(bluetoothContainer_);
    contentStack->addWidget(mqttContainer_);
    contentStack->addWidget(ftpContainer_);
    contentStack->addWidget(standbyLockContainer_);
    contentStack->addWidget(systemInfoContainer_);
    contentStack->addWidget(displayContainer_);

    // Quick 只接管导航框架；各设置页仍复用原 QWidget 和原保存/连接逻辑。
    quickNavigation_ = new QuickNavigationWidget(this);
    quickNavigation_->setItems({QStringLiteral("网络连接"),
                                QStringLiteral("蓝牙连接"),
                                QStringLiteral("MQTT连接"),
                                QStringLiteral("视频下载"),
                                QStringLiteral("待机和锁屏"),
                                QStringLiteral("系统信息"),
                                QStringLiteral("显示设置")});
    quickNavigation_->setHeader(QStringLiteral("系统设置"), QString());
    quickNavigation_->setBackVisible(true);
    quickNavigation_->setCurrentIndex(0);
    connect(quickNavigation_, &QuickNavigationWidget::backRequested,
            this, &QDialog::reject);
    connect(quickNavigation_, &QuickNavigationWidget::itemActivated,
            this, [this](int index) {
        const QList<QPushButton *> buttons = {
            networkButton, bluetoothButton, mqttButton, ftpButton,
            standbyLockButton, systemInfoButton, displayButton
        };
        if (index >= 0 && index < buttons.size() && buttons.at(index)) {
            buttons.at(index)->click();
        }
    });
    sidebarWidget_->hide();

    // 添加到主布局
    mainLayout_->addWidget(quickNavigation_);
    // 资源异常时仍可使用原按钮导航，避免设置页面失去操作入口。
    mainLayout_->addWidget(sidebarWidget_);
    mainLayout_->addWidget(contentStack, 1);  // 内容区域占据剩余空间

    applyResponsiveLayout();
}

QWidget *SettingsDialog::wrapScrollablePage(QWidget *page)
{
    auto *scroll = new QScrollArea(contentStack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setMinimumSize(0, 0);
    scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    scroll->setWidget(page);
    return scroll;
}

void SettingsDialog::fitToHost()
{
    const QRect host = ResponsiveUi::hostRect(parentWidget());
    setGeometry(host);
}

void SettingsDialog::applyResponsiveLayout()
{
    if (!mainLayout_ || !navigationLayout_ || !sidebarWidget_ ||
            !quickNavigation_) {
        return;
    }

    const bool top = ResponsiveUi::usesTopNavigation(
                ResponsiveUi::layoutReferenceSize(this));
    topNavigation_ = top;
    const bool quickReady = quickNavigation_->status() == QQuickWidget::Ready;
    quickNavigation_->setVisible(quickReady);
    sidebarWidget_->setVisible(!quickReady);
    if (!quickReady) {
        mainLayout_->setDirection(QBoxLayout::LeftToRight);
        sidebarWidget_->setMinimumWidth(198);
        sidebarWidget_->setMaximumWidth(210);
        mainLayout_->invalidate();
        return;
    }
    mainLayout_->setDirection(top ? QBoxLayout::TopToBottom
                                  : QBoxLayout::LeftToRight);

    quickNavigation_->setHorizontal(top);
    if (top) {
        quickNavigation_->setMinimumWidth(0);
        quickNavigation_->setMaximumWidth(QWIDGETSIZE_MAX);
        quickNavigation_->setMinimumHeight(92);
        quickNavigation_->setMaximumHeight(112);
    } else {
        quickNavigation_->setMinimumHeight(0);
        quickNavigation_->setMaximumHeight(QWIDGETSIZE_MAX);
        quickNavigation_->setMinimumWidth(198);
        quickNavigation_->setMaximumWidth(198);
    }
    mainLayout_->invalidate();
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        fitToHost();
        applyResponsiveLayout();
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    fitToHost();
    applyResponsiveLayout();
    QuickUiRuntime *runtime = QuickUiRuntime::instance();
    previousModuleId_ = runtime->activeModuleId();
    runtime->synchronizeActiveModule(
                QuickUiRuntime::moduleId(QuickUiRuntime::SystemSettingsModule));
    QDialog::showEvent(event);
}

void SettingsDialog::hideEvent(QHideEvent *event)
{
    QDialog::hideEvent(event);
    QuickUiRuntime::instance()->synchronizeActiveModule(
                previousModuleId_.isEmpty()
                ? QuickUiRuntime::moduleId(QuickUiRuntime::MultimediaMainModule)
                : previousModuleId_);
    previousModuleId_.clear();
}

void SettingsDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    applyResponsiveLayout();
}

/** @brief 建立各子页面与多媒体主窗口之间的转发连接。 */
void SettingsDialog::setupConnections()
{
    connect(networkButton, &QPushButton::clicked, this, &SettingsDialog::onNetworkButtonClicked);
    connect(bluetoothButton, &QPushButton::clicked, this, &SettingsDialog::onBluetoothButtonClicked);
    connect(mqttButton, &QPushButton::clicked, this, &SettingsDialog::onMqttButtonClicked);
    connect(ftpButton, &QPushButton::clicked, this, &SettingsDialog::onFtpButtonClicked);
    connect(systemInfoButton, &QPushButton::clicked, this, &SettingsDialog::onSystemInfoButtonClicked);
    connect(standbyLockButton, &QPushButton::clicked, this, &SettingsDialog::onStandbyLockButtonClicked);
    connect(displayButton, &QPushButton::clicked, this, &SettingsDialog::onDisplayButtonClicked);

    // 现有导航页仍保留各自的选中样式逻辑；离开显示页时只恢复新增按钮，
    // 避免同时出现两个选中项。
    const QList<QPushButton *> existingButtons = {
        networkButton, bluetoothButton, mqttButton,
        ftpButton, standbyLockButton, systemInfoButton
    };
    for (QPushButton *button : existingButtons) {
        connect(button, &QPushButton::clicked, this, [this]() {
            displayButton->setStyleSheet(
                "QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
        });
    }
    connect(networkPage, &NetworkPage::cancelRequested, this, &QDialog::reject);

    // 转发 FtpPage 的“跳播放器页”请求
    connect(ftpPage, &FtpPage::requestOpenPlayerPage,
            this, [this](const QString& url, const QString& kind) {
        emit requestOpenApp3Player(url, kind);
    });

    connect(ftpPage, &FtpPage::ftpDownloadFinished,
            mqttPage, &MqttPage::onRecordedDownloadFinished);

    connect(ftpPage, &FtpPage::ftpDownloadFailed,
            mqttPage, &MqttPage::onRecordedDownloadFailed);

    connect(ftpPage, &FtpPage::ftpDownloadAlreadyExists,
            mqttPage, &MqttPage::onRecordedDownloadAlreadyExists);

    connect(ftpPage, &FtpPage::ftpDownloadStorageRejected,
            mqttPage, &MqttPage::onRecordedDownloadStorageRejected);

    connect(ftpPage, &FtpPage::ftpDownloadRequestRejected,
            mqttPage, &MqttPage::onRecordedDownloadRequestRejected);

    connect(ftpPage, &FtpPage::ftpDownloadStorageRejected,
            this, [this](const QString &name, const QString &reason) {
        emit ftpDownloadFailed(name, reason);
    });

    connect(mqttPage, &MqttPage::mqttConnMarkChanged,
            this, &SettingsDialog::mqttConnMarkChanged);
    connect(mqttPage, &MqttPage::videoControlNotice,
            this, &SettingsDialog::videoControlNotice);


    connect(mqttPage, &MqttPage::liveStreamReady,
            this, [this](const QString& url, const QString& kind) {
                emit requestAutoOpenApp3Player(url, kind);
            });

    connect(mqttPage, &MqttPage::stopLiveRequested,
            this, [this]() {
                emit requestStopApp3Live();
            });

    connect(mqttPage, &MqttPage::stopAllPlayRequested,
            this, [this]() {
                emit requestStopApp3Playback();
            });

    connect(mqttPage, &MqttPage::resumeRecordedPlaybackRequested,
            this, [this]() {
                emit requestResumeApp3RecordedPlayback();
            });

    connect(mqttPage, &MqttPage::mqttVolumeReceived,
            this, &SettingsDialog::mqttVolumeChanged);
    connect(mqttPage, &MqttPage::playFileDeleted,
            this, &SettingsDialog::playFileDeleted);


    // 自动连接
    connect(mqttPage, &MqttPage::ftpDownloadTaskReady,
            ftpPage, [this]() {
                ftpPage->appendLog(QStringLiteral("MQTT已写入FTP下载参数，开始自动下载"));
                ftpPage->startAutoDownloadFromConfig();
            });

    connect(autoConnectMgr, &AutoConnectManager::requestAutoConnectNetwork,
            this, [this]() {
                if (networkPage) {
                    networkPage->autoConnectFromSavedConfig();
                }
            });

    connect(autoConnectMgr, &AutoConnectManager::requestAutoPrepareFtp,
            this, [this]() {
                if (ftpPage) {
                    ftpPage->prepareAutoFtp();
                }
            });

    connect(autoConnectMgr, &AutoConnectManager::logMessage,
            this, [this](const QString &msg) {
                if (mqttPage) {
                    //mqttPage->appendLog(QStringLiteral("[Auto] %1").arg(msg));
                }
                if (ftpPage) {
                    ftpPage->appendLog(QStringLiteral("[Auto] %1").arg(msg));
                }
            });

    connect(mqttPage, &MqttPage::streamUrlReceived,
            ftpPage, [this](const QString& url){
                ftpPage->setStreamUrl(url);
                ftpPage->appendLog(QStringLiteral("MQTT返回URL：%1").arg(url));
            });


    connect(mqttPage, &MqttPage::requestCapturePlayImage,
            this, &SettingsDialog::requestCapturePlayImage);

//    connect(mqttPage, &MqttPage::streamUrlError,
//            ftpPage, [this](const QString& reason){
//                ftpPage->appendLog(QStringLiteral("MQTT获取失败：%1").arg(reason));
//            });

    // 建立日志连接
    connect(mqttPage, &MqttPage::mqttLog,
            ftpPage, [=](const QString& line){
                ftpPage->appendLog(QStringLiteral("[MQTT] %1").arg(line));
            });

    connect(ftpPage, &FtpPage::ftpDownloadStarted,
            this, &SettingsDialog::ftpDownloadStarted);

    connect(ftpPage, &FtpPage::ftpDownloadFinished,
            this, &SettingsDialog::ftpDownloadFinished);

    connect(ftpPage, &FtpPage::ftpDownloadFailed,
            this, &SettingsDialog::ftpDownloadFailed);

    connect(ftpPage, &FtpPage::ftpDownloadProgress,
            this, &SettingsDialog::ftpDownloadProgress);

    connect(ftpPage, &FtpPage::localFileDownloaded,
            this, [this](const QString& filePath){
                emit downloadedVideoReady(filePath);
            });

    connect(networkPage, &NetworkPage::networkAppliedSuccessfully,
            this, [this]() {
                if (mqttPage) {
                    //mqttPage->appendLog(QStringLiteral("检测到网络已连接，准备自动重连 MQTT"));
                    qDebug() << "检测到网络已连接，准备自动重连 MQTT";
                    mqttPage->startAutoConnect();
                }
            });


}

/** @brief 将功能开关同步给网络和 MQTT 页面。 */
void SettingsDialog::setFeatureSettings(const FeatureSettings &settings)
{
    if (networkPage) {
        networkPage->setFeatureSettings(settings);
    }
}

/** @brief 立即执行保存配置对应的网络自动连接。 */
void SettingsDialog::startNetworkAutoConnectNow()
{
    if (networkPage) {
        networkPage->autoConnectFromSavedConfig();
    }
}


/** @brief 把当前录播文件转交 MQTT 页面。 */
void SettingsDialog::onCurrentRecordedFileChanged(const QString &fullPath)
{
    if (mqttPage) {
        mqttPage->onCurrentRecordedFileChanged(fullPath);
    }
}

/** @brief 把当前播放截图转交 MQTT 页面。 */
void SettingsDialog::onCurrentPlayImageChanged(const QString &base64Image)
{
    if (mqttPage) {
        mqttPage->onCurrentPlayImageChanged(base64Image);
    }
}

/** @brief 把当前播放器音量转交 MQTT 页面。 */
void SettingsDialog::onCurrentPlayVolumeChanged(int volume)
{
    if (mqttPage) {
        mqttPage->onCurrentPlayVolumeChanged(volume);
    }
}

/** @brief 上报本地手动音量变化。 */
void SettingsDialog::onLocalManualVolumeChanged(int volume, const QString &reason)
{
    if (mqttPage) {
        mqttPage->onLocalManualVolumeChanged(volume, reason);
    }
}

/** @brief 切换到网络页面。 */
void SettingsDialog::onNetworkButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(0);
    contentStack->setCurrentWidget(networkContainer_);
    // 更新按钮样式（模拟 element-ui 风格）
    networkButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");
    bluetoothButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                 "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                 "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    mqttButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                            "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                            "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    ftpButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    systemInfoButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                  "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                  "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    standbyLockButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                   "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                   "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
}

/** @brief 切换到蓝牙页面。 */
void SettingsDialog::onBluetoothButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(1);
    contentStack->setCurrentWidget(bluetoothContainer_);
    // 更新按钮样式（模拟 element-ui 风格）
    bluetoothButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");
    networkButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                               "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                               "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    mqttButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                            "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                            "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    ftpButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    systemInfoButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                  "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                  "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    standbyLockButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                   "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                   "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
}

/** @brief 切换到 MQTT 页面。 */
void SettingsDialog::onMqttButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(2);
    contentStack->setCurrentWidget(mqttContainer_);
    // 更新按钮样式（模拟 element-ui 风格）
    mqttButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");

    ftpButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    networkButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                               "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                               "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    bluetoothButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                 "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                 "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    systemInfoButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                  "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                  "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    standbyLockButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                   "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                   "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
}

/** @brief 切换到 FTP 页面。 */
void SettingsDialog::onFtpButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(3);
    contentStack->setCurrentWidget(ftpContainer_);

    // 选中态
    ftpButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");

    // 其他恢复默认态
    networkButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    bluetoothButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                   "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                   "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    mqttButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                              "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                              "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    standbyLockButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                    "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                    "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    systemInfoButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                    "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                    "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
}


/** @brief 切换到系统信息页面。 */
void SettingsDialog::onSystemInfoButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(5);
    contentStack->setCurrentWidget(systemInfoContainer_);
    // 更新按钮样式（模拟 element-ui 风格）
    systemInfoButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");
    networkButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                               "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                               "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    bluetoothButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                 "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                 "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    mqttButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                            "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                            "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    ftpButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    standbyLockButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                   "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                   "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
}

/** @brief 切换到待机锁屏页面。 */
void SettingsDialog::onStandbyLockButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(4);
    contentStack->setCurrentWidget(standbyLockContainer_);
    // 更新按钮样式（模拟 element-ui 风格）
    standbyLockButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");
    networkButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                               "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                               "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    bluetoothButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                 "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                 "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    mqttButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                            "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                            "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    ftpButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    systemInfoButton->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                                  "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                                  "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
}

/** @brief 切换到显示旋转页面。 */
void SettingsDialog::onDisplayButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(6);
    contentStack->setCurrentWidget(displayContainer_);

    const QString normalStyle = QStringLiteral(
        "QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
        "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
        "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    const QList<QPushButton *> otherButtons = {
        networkButton, bluetoothButton, mqttButton,
        ftpButton, standbyLockButton, systemInfoButton
    };
    for (QPushButton *button : otherButtons) {
        button->setStyleSheet(normalStyle);
    }
    displayButton->setStyleSheet(
        "QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #409eff; border-radius: 6px; background-color: #ecf5ff; color: #409eff; font-size: 14px; } ");
}
