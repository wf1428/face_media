/**
 * @file settingsdialog.cpp
 * @brief 网络、蓝牙、MQTT、系统、显示、待机和 FTP 页面容器及信号中转器的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-12；修改人：Du lin；转发下载卡片画面变化通知，使 MQTT 页面作废待发的普通截图。
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
#include "common/device_feature_switches.h"
#include "common/toggle_switch.h"
#include "components/quick/QuickNavigationWidget.h"
#include "shell/QuickUiRuntime.h"

#include <QDebug>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
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
    soundButton_ = new QPushButton(QStringLiteral("声音"), sidebarWidget_);


    // 设置按钮样式（模拟 element-ui 风格）
    navigationButtons_ = {networkButton, bluetoothButton, mqttButton, ftpButton,
                          standbyLockButton, systemInfoButton, displayButton, soundButton_};
    for (auto *button : navigationButtons_) {
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setMinimumHeight(40);
        button->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
    }
    for (int index = 0; index < navigationButtons_.size(); ++index) {
        navigationLayout_->addWidget(navigationButtons_.at(index), index, 0);
    }
    navigationLayout_->setRowStretch(navigationButtons_.size(), 1);
    navigationBack_ = new QPushButton(QStringLiteral("‹"), sidebarWidget_);
    navigationBack_->setAccessibleName(QStringLiteral("返回"));
    navigationBack_->setFixedWidth(46);
    navigationBack_->hide();
    connect(navigationBack_, &QPushButton::clicked, this, &QDialog::reject);
    soundButton_->setProperty("normalSoundButtonStyle",soundButton_->styleSheet());

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
    soundContainer_ = wrapScrollablePage(createSoundPage());
    contentStack->addWidget(soundContainer_);

    // Quick 只接管导航框架；各设置页仍复用原 QWidget 和原保存/连接逻辑。
    quickNavigation_ = new QuickNavigationWidget(this);
    QStringList navigationLabels;
    for (const auto *button : navigationButtons_) navigationLabels.append(button->text());
    quickNavigation_->setItems(navigationLabels);
    quickNavigation_->setHeader(QStringLiteral("系统设置"), QString());
    quickNavigation_->setBackVisible(true);
    quickNavigation_->setCurrentIndex(0);
    connect(quickNavigation_, &QuickNavigationWidget::backRequested,
            this, &QDialog::reject);
    connect(quickNavigation_, &QuickNavigationWidget::itemActivated,
            this, [this](int index) {
        if (index >= 0 && index < navigationButtons_.size())
            navigationButtons_.at(index)->click();
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

    const QSize hostSize = ResponsiveUi::hostRect(parentWidget()).size();
    // 实际屏幕旋转后，配置里的背景尺寸可能仍是横屏。
    const bool top = ResponsiveUi::usesTopNavigation(hostSize) ||
                     ResponsiveUi::usesTopNavigation(ResponsiveUi::layoutReferenceSize(this));
    const bool quick = !top && quickNavigation_->status() == QQuickWidget::Ready;
    quickNavigation_->setVisible(quick);
    sidebarWidget_->setVisible(!quick);
    mainLayout_->setDirection(top ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);

    const int columns = top ? (hostSize.width() >= 560 ? 4 : 3) : 1;
    if (columns != navigationColumns_) {
        navigationColumns_ = columns;
        navigationLayout_->removeWidget(navigationBack_);
        for (int row = 0; row < navigationLayout_->rowCount(); ++row)
            navigationLayout_->setRowStretch(row, 0);
        for (int index = 0; index < navigationButtons_.size(); ++index) {
            auto *button = navigationButtons_.at(index);
            navigationLayout_->removeWidget(button);
            navigationLayout_->addWidget(button, index / columns, index % columns);
        }
        const int rows = (navigationButtons_.size() + columns - 1) / columns;
        if (top) navigationLayout_->addWidget(navigationBack_, 0, columns, rows, 1);
        else navigationLayout_->setRowStretch(rows, 1);
    }
    navigationBack_->setVisible(top);
    sidebarWidget_->setMinimumWidth(top ? 0 : 198);
    sidebarWidget_->setMaximumWidth(top ? QWIDGETSIZE_MAX : 210);
    sidebarWidget_->setSizePolicy(QSizePolicy::Preferred, top ? QSizePolicy::Maximum : QSizePolicy::Preferred);

    // 宽屏继续用原来的 Quick 侧栏。
    quickNavigation_->setMinimumWidth(198);
    quickNavigation_->setMaximumWidth(198);
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
    connect(soundButton_, &QPushButton::clicked, this, &SettingsDialog::onSoundButtonClicked);

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
    for (auto *button : QList<QPushButton *>{networkButton,bluetoothButton,mqttButton,ftpButton,
                                            standbyLockButton,systemInfoButton,displayButton}) {
        connect(button,&QPushButton::clicked,this,[this] {
            soundButton_->setStyleSheet(soundButton_->property("normalSoundButtonStyle").toString());
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
    connect(mqttPage, &MqttPage::multimediaUpdateProgress,
            this, &SettingsDialog::multimediaUpdateProgress);


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

void SettingsDialog::invalidatePlayImage()
{
    if (mqttPage) {
        mqttPage->invalidatePlayImage();
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

QWidget *SettingsDialog::createSoundPage()
{
    auto *page = new QWidget(contentStack);
    auto *layout = new QVBoxLayout(page);
    auto *group = new QGroupBox(QStringLiteral("声音"),page);
    auto *rows = new QGridLayout(group);
    const auto saved = DeviceFeatureSwitches::read();
    auto addSwitch = [this,group,rows,saved](int row,const QString &title,bool DeviceFeatureSwitches::*member) {
        rows->addWidget(new QLabel(title,group),row,0);
        auto *toggle = new ToggleSwitch(group);
        toggle->setChecked(saved.*member);
        rows->addWidget(toggle,row,1,Qt::AlignRight);
        connect(&DeviceFeatureSwitches::soundChanges(),&DeviceSoundChanges::changed,toggle,[toggle,member] {
            const QSignalBlocker blocker(toggle);
            toggle->setChecked(DeviceFeatureSwitches::current().*member);
        });
        connect(toggle,&QCheckBox::toggled,this,[this,toggle,member](bool checked) {
            auto settings = DeviceFeatureSwitches::read();
            const bool before = DeviceFeatureSwitches::current().*member;
            settings.*member = checked;
            if (settings.saveSound()) {
                settings.applySound();
            } else {
                const QSignalBlocker blocker(toggle);
                toggle->setChecked(before);
                QMessageBox::warning(this,QStringLiteral("声音"),QStringLiteral("声音设置保存失败"));
            }
        });
    };
    addSwitch(0,QStringLiteral("广播音频"),&DeviceFeatureSwitches::broadcastAudio);
    addSwitch(1,QStringLiteral("IC 卡播报"),&DeviceFeatureSwitches::icCardAudio);
    addSwitch(2,QStringLiteral("人脸播报"),&DeviceFeatureSwitches::faceAudio);
    layout->addWidget(group);
    layout->addStretch();
    return page;
}

void SettingsDialog::onSoundButtonClicked()
{
    if (quickNavigation_) quickNavigation_->setCurrentIndex(contentStack->indexOf(soundContainer_));
    contentStack->setCurrentWidget(soundContainer_);
    const auto normal = soundButton_->property("normalSoundButtonStyle").toString();
    for (auto *button : QList<QPushButton *>{networkButton,bluetoothButton,mqttButton,ftpButton,
                                            standbyLockButton,systemInfoButton,displayButton})
        button->setStyleSheet(normal);
    soundButton_->setStyleSheet(QStringLiteral(
        "QPushButton { text-align:left; padding:10px 20px; border:1px solid #409eff;"
        "border-radius:6px; background-color:#ecf5ff; color:#409eff; font-size:14px; }"));
}
