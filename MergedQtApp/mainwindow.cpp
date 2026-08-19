/**
 * @file mainwindow.cpp
 * @brief 管理融合应用页面、功能模块生命周期和跨模块信号路由。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QKeyEvent>
#include <QDebug>
#include <QMessageBox>
#include <QTimer>
#include <QFileInfo>
#include "platform/rk3566_platform.h"

namespace {

/**
 * @brief 保证三种协议功能开关最多启用一个。
 *
 * 冲突时按 offline_v1、online_v1、online_v2 的既定优先级保留首选项，
 * 其他非协议功能开关不受影响。
 */
static FeatureSettings normalizeFeatureSettings(FeatureSettings s)
{
    const int enabledCount =
            (s.offline_v1 ? 1 : 0) +
            (s.online_v1  ? 1 : 0) +
            (s.online_v2  ? 1 : 0);

    if (enabledCount <= 1) {
        return s;
    }

    // 约定优先级：offline_v1 > online_v1 > online_v2
    if (s.offline_v1) {
        s.online_v1 = false;
        s.online_v2 = false;
        return s;
    }

    if (s.online_v1) {
        s.online_v2 = false;
        return s;
    }

    return s;
}

/** @return 根据功能开关推导出的 MQTT 协议模式："offline" 或 "online"。 */
static QString protocolModeFromFeatures(const FeatureSettings &s)
{
    // offline_v1：纯离线
    if (s.offline_v1) {
        return QStringLiteral("offline");
    }

    // online_v1：仍然连 v1 的 MQTT，刷卡/二维码走离线协议
    if (s.online_v1) {
        return QStringLiteral("offline");
    }

    // online_v2：走在线协议
    if (s.online_v2) {
        return QStringLiteral("online");
    }

    // 默认兜底
    return QStringLiteral("offline");
}

/**
 * @brief 将历史别名归一化为当前使用的 "offline"/"online"。
 * @return 未识别值按离线模式处理，避免意外启用在线协议。
 */
static QString normalizeProtocolModeValue(const QString &raw)
{
    const QString v = raw.trimmed().toLower();

    if (v == QStringLiteral("offline") || v == QStringLiteral("ic")) {
        return QStringLiteral("offline");
    }
    if (v == QStringLiteral("online") || v == QStringLiteral("legacy")) {
        return QStringLiteral("online");
    }
    return QStringLiteral("offline");
}

}



/** @brief 创建全部页面、运行模块并建立跨页面信号连接。 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->statusbar->hide();   // 隐藏底部状态栏

    menuPage = this->takeCentralWidget();   // 不会被删除

    // 创建堆栈，并设为新的 centralWidget
    stack = new QStackedWidget(this);
    this->setCentralWidget(stack);

    // 把菜单页加入 stack
    stack->addWidget(menuPage);

    // app3 页面
    app3Page = new MultimediaDemo(this);
    app3Page->setWindowFlags(Qt::Widget);

    stack->addWidget(app3Page);

    // 启动默认显示 app3Page
    stack->setCurrentWidget(app3Page);

    // 无桌面 EGLFS 下顶层窗口由 main.cpp 中唯一的根窗口统一管理。
    // 这里不再单独 showFullScreen()，避免创建第二个 DRM/EGL 顶层窗口。

    // 开启鼠标跟踪
    this->setMouseTracking(true);
    stack->setMouseTracking(true);

    // menuPage 也能追踪鼠标（切回菜单时）
    menuPage->setMouseTracking(true);
    app3Page->setMouseTracking(true);

    //this->centralWidget()->setMouseTracking(true);

    // 密码正确后返回菜单
    auto *cast = qobject_cast<MultimediaDemo*>(app3Page);
    if (cast) {
        connect(cast, &MultimediaDemo::backToMenuRequested, this, [=]() {
            stack->setCurrentWidget(menuPage);

        });
    }


    connect(stack, &QStackedWidget::currentChanged, this, [=](int){
        auto *mm = qobject_cast<MultimediaDemo*>(app3Page);
        if (!mm) return;

        if (stack->currentWidget() == app3Page) {
            QTimer::singleShot(0, mm, [mm]() {
                mm->resumeLocalPlaylistFromStart();     // 回到 app3
            });
        } else {
            QTimer::singleShot(0, mm, [mm]() {
                mm->leaveAndStopVideo();                // 离开 app3
            });
        }

    });

    // 文件管理页面
    filePage = new FileManagerPage(this);
    stack->addWidget(filePage);

    // 文件页返回菜单
    connect(filePage, &FileManagerPage::backToMenuRequested, this, [=](){
        stack->setCurrentWidget(menuPage);
    });

    settingsDialog = new SettingsDialog(this);

    cmdDialog = new CommandDialog(this);
    cmdDialog->hide();


    if (cast && settingsDialog) {
        // 播放状态先经设置对话框转交 MQTT 页面，保持主窗口只负责跨模块路由。
        connect(cast, &MultimediaDemo::currentRecordedFileChanged,
                settingsDialog, &SettingsDialog::onCurrentRecordedFileChanged);

        connect(cast, &MultimediaDemo::currentPlayImageChanged,
                settingsDialog, &SettingsDialog::onCurrentPlayImageChanged);

        connect(settingsDialog, &SettingsDialog::requestCapturePlayImage,
                cast, &MultimediaDemo::captureCompositeSnapshot);

        connect(cast, &MultimediaDemo::currentPlayVolumeChanged,
                settingsDialog, &SettingsDialog::onCurrentPlayVolumeChanged);

        connect(settingsDialog, &SettingsDialog::mqttConnMarkChanged,
                cast, &MultimediaDemo::onMqttConnMarkChanged);

        connect(cast, &MultimediaDemo::localManualVolumeChanged,
                settingsDialog, &SettingsDialog::onLocalManualVolumeChanged);
    }

    if (cast) {
        QTimer::singleShot(0, cast, [cast]() {
            cast->notifyCurrentRecordedFile();
        });
    }

    // 启动时读取上次保存的 Feature 状态
    FeatureSettings savedFeatures = loadFeatureSettings();

    if (settingsDialog) {
        settingsDialog->setFeatureSettings(savedFeatures);

        // 启动时如果 online_v1 或 online_v2 启用，则自动连网
        if (savedFeatures.online_v1 || savedFeatures.online_v2) {
            QTimer::singleShot(0, this, [this]() {
                if (settingsDialog) {
                    settingsDialog->startNetworkAutoConnectNow();
                }
            });
        }
    }

    // 设置音量
    if (cast && settingsDialog) {
        connect(settingsDialog, &SettingsDialog::mqttVolumeChanged,
                cast, &MultimediaDemo::onMqttVolumeChanged);
    }

    // 拉流跳转到媒体页面
    connect(settingsDialog, &SettingsDialog::requestOpenApp3Player,
            this, [=](const QString& url, const QString& kind) {

        // 先把拉流参数交给播放器，再切换页面；确保页面显示时播放源已经就绪。
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->setStreamUrl(url, kind);
        }

        stack->setCurrentWidget(app3Page);

        // 用户主动发起的拉流完成页面跳转后关闭模态设置窗口。
        settingsDialog->accept();
    });

    connect(settingsDialog, &SettingsDialog::requestAutoOpenApp3Player,
            this, [this](const QString& url, const QString& kind) {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->setStreamUrl(url, kind);
        }

        stack->setCurrentWidget(app3Page);

        // 注意：自动拉流不关闭 settingsDialog
    });

    connect(settingsDialog, &SettingsDialog::requestStopApp3Live,
            this, [this]() {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->switchToRecordedMode();
        }
    });

    connect(settingsDialog, &SettingsDialog::requestStopApp3Playback,
            this, [this]() {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->stopAllPlayback();
        }
    });

    connect(settingsDialog, &SettingsDialog::requestResumeApp3RecordedPlayback,
            this, [this]() {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->resumeLocalPlaylistFromStart();
        }
    });

    connect(settingsDialog, &SettingsDialog::videoControlNotice,
            this, [this](const QString &text) {
        if (udiskOverlay) {
            udiskOverlay->showMessage(text, 4000);
        }
    });


    connect(settingsDialog, &SettingsDialog::downloadedVideoReady,
            this, [this](const QString& filePath) {
                auto *player = qobject_cast<MultimediaDemo*>(app3Page);
                if (player) {
                    player->onDownloadedVideoReady(filePath);
                }
            });

    // 连接应用按钮点击信号
    connect(ui->appButton1, &QPushButton::clicked, this, [=]() {
        if (!settingsDialog) return;

        settingsDialog->show();
        settingsDialog->raise();
        settingsDialog->activateWindow();

    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadStarted,
            this, [this](const QString& fileName) {
        if (udiskOverlay) {
            udiskOverlay->showMessage(
                QStringLiteral("开始下载：%1").arg(fileName),
                4000
            );
        }
    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadFinished,
            this, [this](const QString& filePath) {
        if (udiskOverlay) {
            udiskOverlay->showMessage(
                QStringLiteral("下载完成：%1").arg(QFileInfo(filePath).fileName()),
                4000
            );
        }
    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadFailed,
            this, [this](const QString& fileName, const QString& reason) {
        if (udiskOverlay) {
            udiskOverlay->showMessage(
                QStringLiteral("下载失败：%1，原因：%2").arg(fileName, reason),
                5000
            );
        }
    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadStarted,
            this, [this](const QString& fileName) {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->onDownloadStarted(fileName);
        }
    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadProgress,
            this, [this](const QString& fileName, int percent) {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->onDownloadProgress(fileName, percent);
        }
    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadFinished,
            this, [this](const QString& filePath) {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->onDownloadFinished(filePath);
        }
    });

    connect(settingsDialog, &SettingsDialog::ftpDownloadFailed,
            this, [this](const QString& fileName, const QString& reason) {
        auto *player = qobject_cast<MultimediaDemo*>(app3Page);
        if (player) {
            player->onDownloadFailed(fileName, reason);
        }
    });

    connect(ui->appButton2, &QPushButton::clicked, this, [=]() {
        FeaturesDialog *featuresDialog = new FeaturesDialog(this);
        // 设置窗口大小
        featuresDialog->resize(400, 500);

        // 打开时回显上次保存的状态
        FeatureSettings currentSettings = loadFeatureSettings();
        featuresDialog->setFeatureSettings(currentSettings);

        // 以模态方式显示对话框
        if (featuresDialog->exec() == QDialog::Accepted) {
            FeatureSettings settings = featuresDialog->getFeatureSettings();

            // 保存到 RK3566 平台配置文件
            saveFeatureSettings(settings);

            // 下发到 SettingsDialog -> NetworkPage
            if (settingsDialog) {
                settingsDialog->setFeatureSettings(settings);

                // 勾选了 online_v1 或 online_v2，立刻自动连接网络
                if (settings.online_v1 || settings.online_v2) {
                    settingsDialog->startNetworkAutoConnectNow();
                }
            }
        }
        // 对话框关闭后删除
        delete featuresDialog;
    });

    connect(ui->appButton3, &QPushButton::clicked, this, [=]() {
        stack->setCurrentWidget(app3Page);
    });


    connect(ui->appButton4, &QPushButton::clicked, this, [=]() {

        cmdDialog->show();
        cmdDialog->raise();
        cmdDialog->activateWindow();
    });


    key = new KeyService(this);

    connect(key, &KeyService::logMessage,
            this, [](const QString &msg) {
        qInfo() << "[AUDIO-KEY]" << msg;
    });

    connect(key, &KeyService::key3Pressed,
            this, [this]() {

        // KEY3 的功能是查询注册卡号, 对话框拉起来方便看结果
//        cmdDialog->show();
//        cmdDialog->raise();
//        cmdDialog->activateWindow();
        cmdDialog->appendLog("硬按键 KEY3 按下");
        cmdDialog->triggerAction("legacy_key3_query_registered_cards");
    });

    connect(key, &KeyService::key4Pressed,
            this, [this]() {

        cmdDialog->appendLog("硬按键 KEY4 按下");
        cmdDialog->triggerAction("legacy_key4_reset");
    });

    key->start();



    connect(ui->appButton5, &QPushButton::clicked, this, [=]() {
        // 每次进入时确保根目录
        filePage->setRoot(Rk3566Platform::storageRoot());
        stack->setCurrentWidget(filePage);
    });

    // 连接退出按钮点击信号，执行重启
    connect(ui->exitButton, &QPushButton::clicked, this, [=]() {

        auto ret = QMessageBox::question(
            this,
            "确认重启",
            "确认要重启设备吗？",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No   // 默认选中“否”，避免误触
        );

        if (ret != QMessageBox::Yes) {
            return; // 取消
        }

        QString rebootError;
        if (!Rk3566Platform::reboot(&rebootError)) {
            QMessageBox::warning(this, "重启失败", rebootError);
        }
    });

    QTimer::singleShot(200, this, SLOT(setupUdiskOverlayLater()));

    // mqtt 提示信号
    if (cast) {
        connect(cast, &MultimediaDemo::statusMessageRequested,
                this, [this](const QString &text, int durationMs) {
            if (udiskOverlay) {
                udiskOverlay->showMessage(text, durationMs);
            }
        });
    }


    moduleActive_ = true;
}

/** @brief 停止活动模块并释放 Qt Designer UI。 */
MainWindow::~MainWindow()
{
    delete ui;
}

/** @brief 激活多媒体模块，并恢复模块切换前的 LIVE 或本地轮播状态。 */
void MainWindow::activateModule()
{
    if (moduleActive_) {
        qInfo() << "[MULTIMEDIA-ADAPTER] duplicate activate ignored";
        return;
    }

    auto *multimedia = qobject_cast<MultimediaDemo *>(app3Page);
    if (multimedia && stack && stack->currentWidget() == app3Page) {
        multimedia->resumePlaybackAfterModuleSwitch();
    }
    moduleActive_ = true;
    qInfo() << "[MULTIMEDIA-ADAPTER] playback resumed";
}

/** @brief 停用多媒体模块并暂停当前播放，同时保留返回时需要恢复的状态。 */
void MainWindow::deactivateModule()
{
    if (!moduleActive_) {
        qInfo() << "[MULTIMEDIA-ADAPTER] duplicate deactivate ignored";
        return;
    }

    auto *multimedia = qobject_cast<MultimediaDemo *>(app3Page);
    if (multimedia) {
        multimedia->suspendPlaybackForModuleSwitch();
    }
    moduleActive_ = false;
    qInfo() << "[MULTIMEDIA-ADAPTER] playback paused";
}

/** @return 多媒体模块当前处于活动状态时返回 true。 */
bool MainWindow::isModuleActive() const
{
    return moduleActive_;
}

bool MainWindow::allowsPresenceSwitch() const
{
    if (!moduleActive_ || !stack || stack->currentWidget() != app3Page) {
        return false;
    }
    const auto *multimedia = qobject_cast<const MultimediaDemo *>(app3Page);
    if (!multimedia || !multimedia->allowsPresenceSwitch()) {
        return false;
    }
    if ((settingsDialog && settingsDialog->isVisible()) ||
        (cmdDialog && cmdDialog->isVisible())) {
        return false;
    }

    QWidget *modal = QApplication::activeModalWidget();
    return !modal || (modal != this && !isAncestorOf(modal));
}


/** @brief 将设备键盘快捷键转换为页面或调试入口操作。 */
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && windowState() & Qt::WindowFullScreen)
    {
        // 如果当前是全屏模式且按下 ESC，则退出全屏
        showNormal();
    }
    else
    {
        // 其他按键事件交给基类处理
        QMainWindow::keyPressEvent(event);
    }
}

/** @brief 窗口尺寸变化后同步页面栈和 U 盘覆盖层几何。 */
void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 覆盖层采用延迟初始化，窗口早期收到 resize 时可能尚不存在。
    if (!udiskOverlay) return;

    // 排队执行可让宿主布局先稳定，再按最终尺寸调整覆盖层。
    QMetaObject::invokeMethod(udiskOverlay, "onHostResized", Qt::QueuedConnection);

}


/**
 * @brief 延迟设置U盘覆盖层
 * 
 * 延迟创建覆盖层，确保主窗口和页面栈已经完成初始布局。
 */
void MainWindow::setupUdiskOverlayLater()
{
    // 防止重复初始化
    if (udiskOverlay) return;

    udiskOverlay = new UdiskStatusOverlay(this, this);
    udiskOverlay->start(300);
}


/** @brief 归一化并保存 [feature] 开关，同时同步 MQTT 协议模式。 */
void MainWindow::saveFeatureSettings(const FeatureSettings &settings)
{
    const FeatureSettings normalized = normalizeFeatureSettings(settings);
    const QString cfgPath = Rk3566Platform::netConfigPath();

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    ini.setValue("feature/offline_v1", normalized.offline_v1);
    ini.setValue("feature/online_v1", normalized.online_v1);
    ini.setValue("feature/online_v2", normalized.online_v2);
    ini.setValue("feature/air_sys", normalized.air_sys);
    ini.setValue("feature/media_sys", normalized.media_sys);
    ini.setValue("feature/broad_sys", normalized.broad_sys);
    ini.setValue("feature/tbk_sys", normalized.tbk_sys);

    // feature 作为总开关，自动同步 protocol_mode
    const QString protocolMode = protocolModeFromFeatures(normalized);
    ini.setValue("mqtt/protocol_mode", protocolMode);

    ini.sync();

    qDebug() << "保存 feature 配置:" << cfgPath
             << "offline_v1=" << normalized.offline_v1
             << "online_v1=" << normalized.online_v1
             << "online_v2=" << normalized.online_v2
             << "protocol_mode=" << protocolMode;
}


/** @return 从平台配置读取并归一化后的功能开关。 */
FeatureSettings MainWindow::loadFeatureSettings() const
{
    FeatureSettings settings;

    const QString cfgPath = Rk3566Platform::netConfigPath();
    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    settings.offline_v1 = ini.value("feature/offline_v1", false).toBool();
    settings.online_v1  = ini.value("feature/online_v1", false).toBool();
    settings.online_v2  = ini.value("feature/online_v2", false).toBool();
    settings.air_sys    = ini.value("feature/air_sys", false).toBool();
    settings.media_sys  = ini.value("feature/media_sys", false).toBool();
    settings.broad_sys  = ini.value("feature/broad_sys", false).toBool();
    settings.tbk_sys    = ini.value("feature/tbk_sys", false).toBool();

    settings = normalizeFeatureSettings(settings);

    // 用 feature 反推 protocol_mode，并回写，确保以后配置一致
    const QString currentMode =
            normalizeProtocolModeValue(ini.value("mqtt/protocol_mode", "online").toString());
    const QString expectedMode = protocolModeFromFeatures(settings);

    if (currentMode != expectedMode) {
        ini.setValue("mqtt/protocol_mode", expectedMode);
        ini.sync();
        qDebug() << "修正 protocol_mode:" << currentMode << "->" << expectedMode;
    }

    qDebug() << "读取 feature 配置:"
             << "offline_v1=" << settings.offline_v1
             << "online_v1=" << settings.online_v1
             << "online_v2=" << settings.online_v2
             << "protocol_mode=" << expectedMode;

    return settings;
}
