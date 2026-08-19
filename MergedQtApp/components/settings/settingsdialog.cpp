/**
 * @file settingsdialog.cpp
 * @brief 网络、蓝牙、MQTT、系统、待机和 FTP 页面容器及信号中转器的实现。
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

#include <QDebug>
#include <QStyle>
#include <QMessageBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QDateTime>
#include "platform/rk3566_platform.h"

/** @brief 创建导航按钮、页面栈和自动连接管理器。 */
SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setupUI();

    // 创建自动连接管理器
    autoConnectMgr = new AutoConnectManager(this);
    autoConnectMgr->setConfigLocation(configDir_, configFileName_);
    autoConnectMgr->bindMqttPage(mqttPage);

    setupConnections();

    // 设置窗口属性
    setWindowTitle("设置");

    // 外部资源根目录
    const QString configPath = Rk3566Platform::uiConfigPath();
    qDebug() << "开始读取磁盘配置文件: " << configPath;

    // 默认窗口大小
    int windowWidth = 1024;
    int windowHeight = 768;

    if (QFile::exists(configPath)) {
        QSettings settings(configPath, QSettings::IniFormat);

        settings.beginGroup("backRect");
        windowWidth = settings.value("backPic_x_Size", 1024).toInt();
        windowHeight = settings.value("backPic_y_Size", 768).toInt();

        qDebug() << "读取配置文件成功: " << configPath
                  << ", backPic_x_Size=" << windowWidth
                  << ", backPic_y_Size=" << windowHeight;
    } else {
        qDebug() << "配置文件不存在，设置界面使用默认窗口大小: 1024x768";
    }

    setMinimumSize(windowWidth, windowHeight);
    resize(windowWidth, windowHeight);

    // 默认显示网络连接页面
    onNetworkButtonClicked();

    autoConnectMgr->startIfEnabled(500);
}

/** @brief 释放设置子页面和 UI 资源。 */
SettingsDialog::~SettingsDialog()
{}

/** @brief 创建导航栏和内容页面栈。 */
void SettingsDialog::setupUI()
{
    // 创建主布局
    QHBoxLayout *mainLayout = new QHBoxLayout(this);

    // 创建左侧导航栏
    QWidget *sidebarWidget = new QWidget(this);
    sidebarWidget->setMinimumWidth(200);
    sidebarWidget->setMaximumWidth(250);
    sidebarWidget->setStyleSheet("background-color: #f0f0f0; border-right: 1px solid #d0d0d0;");

    QVBoxLayout *sidebarLayout = new QVBoxLayout(sidebarWidget);
    sidebarLayout->setSpacing(10);
    sidebarLayout->setContentsMargins(20, 30, 20, 20);

    // 创建导航按钮
    networkButton = new QPushButton("网络连接", sidebarWidget);
    bluetoothButton = new QPushButton("蓝牙连接", sidebarWidget);
    mqttButton = new QPushButton("MQTT连接", sidebarWidget);
    ftpButton = new QPushButton("视频下载", sidebarWidget);
    standbyLockButton = new QPushButton("待机和锁屏", sidebarWidget);
    systemInfoButton = new QPushButton("系统信息", sidebarWidget);


    // 设置按钮样式（模拟 element-ui 风格）
    QList<QPushButton*> buttons = {networkButton, bluetoothButton, mqttButton, ftpButton, standbyLockButton, systemInfoButton};
    for (auto button : buttons) {
        button->setMinimumHeight(40);
        button->setStyleSheet("QPushButton { text-align: left; padding: 10px 20px; border: 1px solid #dcdfe6; border-radius: 6px; background-color: white; color: #606266; font-size: 14px; } "
                             "QPushButton:hover { border-color: #409eff; background-color: #ecf5ff; color: #409eff; } "
                             "QPushButton:pressed { border-color: #337ecc; background-color: #d9ecff; color: #337ecc; } ");
        sidebarLayout->addWidget(button);
    }

    sidebarLayout->addStretch();

    // 创建内容区域
    contentStack = new QStackedWidget(this);

    // 创建各个子页面
    networkPage = new NetworkPage(contentStack);
    bluetoothPage = new BluetoothPage(contentStack);
    mqttPage = new MqttPage(contentStack);
    ftpPage = new FtpPage(contentStack);
    standbyLockPage = new StandbyLockPage(contentStack);
    systemInfoPage = new SystemInfoPage(contentStack);

    // 添加页面到栈中
    contentStack->addWidget(networkPage);
    contentStack->addWidget(bluetoothPage);
    contentStack->addWidget(mqttPage);
    contentStack->addWidget(ftpPage);
    contentStack->addWidget(standbyLockPage);
    contentStack->addWidget(systemInfoPage);

    // 添加到主布局
    mainLayout->addWidget(sidebarWidget);
    mainLayout->addWidget(contentStack, 1);  // 内容区域占据剩余空间

    setLayout(mainLayout);
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
    contentStack->setCurrentWidget(networkPage);
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
    contentStack->setCurrentWidget(bluetoothPage);
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
    contentStack->setCurrentWidget(mqttPage);
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
    contentStack->setCurrentWidget(ftpPage);

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
    contentStack->setCurrentWidget(systemInfoPage);
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
    contentStack->setCurrentWidget(standbyLockPage);
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
