/**
 * @file MainWindow.cpp
 * @brief 管理融合应用页面、功能模块生命周期和跨模块信号路由。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "MainWindow.h"

#include "AccessPasswordDialog.h"
#include "AdminPanel.h"
#include "AdminLoginDialog.h"
#include "AppMessageDialog.h"
#include "AppPasswordDialog.h"
#include "CameraPreviewWidget.h"
#include "MaintenanceWorker.h"
#include "PersonExportWorker.h"
#include "PersonImportWorker.h"
#include "SnapshotService.h"
#include "common/face_image_sync_bridge.h"
#include "ic_board/ic_event_bridge.h"

#include <QAbstractSocket>
#include <QApplication>
#include <QLabel>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QMetaType>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QResizeEvent>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSysInfo>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

/** @brief 按配置构建界面并创建服务对象。 */
FaceGateMainWindow::FaceGateMainWindow(const AppConfig &config, QWidget *parent)
    : QMainWindow(parent), config_(config)
{
    buildUi();
    startServices();
}

/** @brief 销毁前执行幂等模块关闭。 */
FaceGateMainWindow::~FaceGateMainWindow()
{
    shutdownModule();
}

/** @brief 启动或恢复摄像头、推理和门禁业务。 */
bool FaceGateMainWindow::activateModule()
{
    if (shutdown_) {
        qWarning() << "[FACEGATE-ADAPTER] activate ignored after shutdown";
        return false;
    }
    if (moduleActive_) {
        qInfo() << "[FACEGATE-ADAPTER] duplicate activate ignored";
        return true;
    }

    bool engineReady = false;
    if (faceInferenceWorker_ && faceInferenceThread_.isRunning()) {
        FaceInferenceWorker *worker = faceInferenceWorker_;
        LivenessWorker *livenessWorker = &liveness_;
        const AppConfig activationConfig = config_;
        const bool activationLivenessEnabled = livenessEnabled_;
        QMetaObject::invokeMethod(
            worker,
            [worker, livenessWorker, activationConfig,
             activationLivenessEnabled, &engineReady]() {
                engineReady = worker->activate(
                    activationConfig, livenessWorker, activationLivenessEnabled);
            },
            Qt::BlockingQueuedConnection);
    }
    faceEngineReady_ = engineReady;
    const bool livenessReady = !livenessEnabled_ ||
                               liveness_.isReady() ||
                               liveness_.start(config_);
    if (!engineReady || !livenessReady) {
        qWarning() << "[FACEGATE-ADAPTER] engine activation failed"
                   << "faceEngine=" << engineReady
                   << "liveness=" << livenessReady;
        if (faceInferenceWorker_ && faceInferenceThread_.isRunning()) {
            FaceInferenceWorker *worker = faceInferenceWorker_;
            QMetaObject::invokeMethod(
                worker,
                [worker]() {
                    worker->deactivate(QStringLiteral("人脸引擎激活失败"));
                },
                Qt::BlockingQueuedConnection);
        }
        liveness_.stop();
        moduleActive_ = false;
        return false;
    }

    moduleActive_ = true;
    updateCameraPowerState();

    const bool cameraReady = !currentUiNeedsCamera() || camera_.isRunning();
    if (!cameraReady) {
        qWarning() << "[FACEGATE-ADAPTER] camera activation failed";
        moduleActive_ = false;
        camera_.stop();
        if (faceInferenceWorker_ && faceInferenceThread_.isRunning()) {
            FaceInferenceWorker *worker = faceInferenceWorker_;
            QMetaObject::invokeMethod(
                worker,
                [worker]() {
                    worker->deactivate(QStringLiteral("摄像头激活失败"));
                },
                Qt::BlockingQueuedConnection);
        }
        liveness_.stop();
        clearCameraFrames();
        return false;
    }

    qInfo() << "[FACEGATE-ADAPTER] camera and recognition chain activated";
    return true;
}

/** @brief 暂停摄像头和验证，但保留后台服务以便恢复。 */
void FaceGateMainWindow::deactivateModule()
{
    if (!moduleActive_ && !camera_.isRunning() && !liveness_.isReady()) {
        return;
    }

    moduleActive_ = false;
    if (adminPanel_) {
        adminPanel_->close();
    }
    camera_.stop();
    if (faceInferenceWorker_ && faceInferenceThread_.isRunning()) {
        FaceInferenceWorker *worker = faceInferenceWorker_;
        QMetaObject::invokeMethod(
            worker,
            [worker]() {
                worker->deactivate(QStringLiteral("人脸模态已暂停"));
            },
            Qt::BlockingQueuedConnection);
    }
    faceFramePending_ = false;
    faceAccessPending_ = false;
    passwordAccessActive_ = false;
    ++passwordAccessGeneration_;
    if (passwordAccessButton_) {
        passwordAccessButton_->setEnabled(true);
    }
    pendingFaceAccessLog_ = VerifyLog();
    liveness_.stop();
    clearCameraFrames();
    adminLivenessPending_ = false;
    qInfo() << "[FACEGATE-ADAPTER] camera and recognition chain deactivated";
}

/** @brief 停止所有线程、服务和播放器；重复调用保持幂等。 */
void FaceGateMainWindow::shutdownModule()
{
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    deactivateModule();
    if (personImportWorker_) {
        personImportWorker_->cancel();
    }
    if (personImportThread_ && personImportThread_->isRunning()) {
        personImportThread_->quit();
        personImportThread_->wait();
    }
    personImportWorker_ = nullptr;
    personImportThread_ = nullptr;
    if (personExportWorker_) {
        personExportWorker_->cancel();
    }
    if (personExportThread_ && personExportThread_->isRunning()) {
        personExportThread_->quit();
        personExportThread_->wait();
    }
    personExportWorker_ = nullptr;
    personExportThread_ = nullptr;
    if (faceInferenceWorker_ && faceInferenceThread_.isRunning()) {
        FaceInferenceWorker *worker = faceInferenceWorker_;
        QMetaObject::invokeMethod(
            worker,
            [worker]() {
                worker->shutdown();
            },
            Qt::BlockingQueuedConnection);
        faceInferenceThread_.quit();
        faceInferenceThread_.wait();
        faceInferenceWorker_ = nullptr;
    }
    faceEngineReady_ = false;
    if (databaseWorker_ && databaseThread_.isRunning()) {
        QMetaObject::invokeMethod(databaseWorker_, "close", Qt::BlockingQueuedConnection);
    }
    maintenanceThread_.quit();
    maintenanceThread_.wait();
    databaseThread_.quit();
    databaseThread_.wait();
    qInfo() << "[FACEGATE-ADAPTER] services shutdown complete";
}

/** @return 门禁模块当前活动时返回 true。 */
bool FaceGateMainWindow::isModuleActive() const
{
    return moduleActive_;
}

bool FaceGateMainWindow::allowsPresenceSwitch() const
{
    if (!moduleActive_ || adminLoginActive_ || adminModeActive_ ||
        passwordAccessActive_ ||
        (adminPanel_ && adminPanel_->isVisible())) {
        return false;
    }

    QWidget *modal = QApplication::activeModalWidget();
    return !modal || (modal != this && !isAncestorOf(modal));
}

/** @brief 创建预览、状态栏和覆盖信息面板。 */
void FaceGateMainWindow::buildUi()
{
    // 主界面采用中央预览区叠加左右信息面板的布局。
    auto *central = new QWidget(this);
    central->setObjectName("gateRoot");
    setCentralWidget(central);

    preview_ = new CameraPreviewWidget(central);
    preview_->setGeometry(central->rect());
    preview_->lower();

    leftOverlay_ = new QWidget(central);
    leftOverlay_->setObjectName("leftOverlay");
    leftOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *leftLayout = new QVBoxLayout(leftOverlay_);
    leftLayout->setContentsMargins(22, 22, 22, 22);
    leftLayout->setSpacing(10);

    auto *brandLabel = new QLabel("人脸闸机", leftOverlay_);
    brandLabel->setObjectName("brandLabel");
    timeLabel_ = new QLabel("--:--", leftOverlay_);
    timeLabel_->setObjectName("timeLabel");
    dateLabel_ = new QLabel("--", leftOverlay_);
    dateLabel_->setObjectName("dateLabel");
    networkLabel_ = new QLabel("网络 检查中", leftOverlay_);
    networkLabel_->setObjectName("infoChip");
    networkLabel_->setMinimumHeight(44);
    networkLabel_->setWordWrap(true);
    cameraLabel_ = new QLabel("摄像头 启动中", leftOverlay_);
    cameraLabel_->setObjectName("infoChip");
    cameraLabel_->setMinimumHeight(44);
    cameraLabel_->setWordWrap(true);
    databaseLabel_ = new QLabel("数据库 打开中", leftOverlay_);
    databaseLabel_->setObjectName("infoChip");
    databaseLabel_->setMinimumHeight(44);
    databaseLabel_->setWordWrap(true);

    leftLayout->addWidget(brandLabel);
    leftLayout->addSpacing(16);
    leftLayout->addWidget(timeLabel_);
    leftLayout->addWidget(dateLabel_);
    leftLayout->addSpacing(18);
    leftLayout->addWidget(networkLabel_);
    leftLayout->addWidget(cameraLabel_);
    leftLayout->addWidget(databaseLabel_);
    leftLayout->addStretch();

    rightOverlay_ = new QWidget(central);
    rightOverlay_->setObjectName("rightOverlay");
    rightOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    auto *rightLayout = new QVBoxLayout(rightOverlay_);
    rightLayout->setContentsMargins(22, 22, 22, 22);
    rightLayout->setSpacing(10);

    recognitionLabel_ = new QLabel("就绪", rightOverlay_);
    recognitionLabel_->setObjectName("primaryStatus");
    gateLabel_ = new QLabel("闸机空闲", rightOverlay_);
    gateLabel_->setObjectName("infoChip");
    gateLabel_->setMinimumHeight(44);
    gateLabel_->setWordWrap(true);
    livenessLabel_ = new QLabel(livenessEnabled_ ? "活体开启" : "活体关闭", rightOverlay_);
    livenessLabel_->setObjectName("infoChip");
    livenessLabel_->setMinimumHeight(44);
    livenessLabel_->setWordWrap(true);
    galleryLabel_ = new QLabel("底库 0", rightOverlay_);
    galleryLabel_->setObjectName("infoChip");
    galleryLabel_->setMinimumHeight(44);
    galleryLabel_->setWordWrap(true);
    hintLabel_ = new QLabel("FaceGate V1.0.0", rightOverlay_);
    hintLabel_->setObjectName("hintLabel");
    hintLabel_->setWordWrap(true);

    passwordAccessButton_ = new QPushButton(QStringLiteral("密码通行"), rightOverlay_);
    passwordAccessButton_->setObjectName(QStringLiteral("passwordAccessButton"));
    passwordAccessButton_->setIcon(QIcon(QStringLiteral(":/icons/action/key.svg")));
    passwordAccessButton_->setIconSize(QSize(22, 22));
    passwordAccessButton_->setMinimumHeight(48);
    passwordAccessButton_->setCursor(Qt::PointingHandCursor);

    rightLayout->addWidget(recognitionLabel_);
    rightLayout->addWidget(gateLabel_);
    rightLayout->addWidget(livenessLabel_);
    rightLayout->addWidget(galleryLabel_);
    rightLayout->addWidget(passwordAccessButton_);
    rightLayout->addStretch();
    rightLayout->addWidget(hintLabel_);

    leftOverlay_->raise();
    rightOverlay_->raise();

    setStyleSheet(
        "QMainWindow,#gateRoot{background:#05070a;}"
        "QLabel{color:#f5f7fb;font-family:Arial;font-size:16px;}"
        "#leftOverlay,#rightOverlay{background:rgba(5,10,14,168);}"
        "#brandLabel{color:#49d1ff;font-size:20px;font-weight:700;}"
        "#timeLabel{font-size:42px;font-weight:700;color:#ffffff;}"
        "#dateLabel{font-size:16px;color:#9fb3bf;}"
        "#primaryStatus{font-size:26px;font-weight:700;color:#ffffff;padding:8px 0px;}"
        "#infoChip{background:rgba(255,255,255,34);border:1px solid rgba(255,255,255,46);"
        "border-radius:6px;padding:8px 10px;color:#dce8ee;font-size:13px;}"
        "#hintLabel{color:#8ea1ac;font-size:13px;}"
        "#passwordAccessButton{background:rgba(73,209,255,38);color:#eafaff;"
        "border:1px solid rgba(73,209,255,150);text-align:left;padding:10px 14px;"
        "font-size:15px;font-weight:600;}"
        "#passwordAccessButton:pressed{background:rgba(73,209,255,78);}"
        "#passwordAccessButton:disabled{color:#71838d;border-color:rgba(255,255,255,30);"
        "background:rgba(255,255,255,12);}"
        "QPushButton{font-size:16px;padding:10px 16px;border-radius:6px;}"
    );

    connect(preview_, &CameraPreviewWidget::clicked, this, &FaceGateMainWindow::handleAdminTap);
    connect(passwordAccessButton_, &QPushButton::clicked,
            this, &FaceGateMainWindow::handlePasswordAccess);

    connect(&clockTimer_, &QTimer::timeout, this, &FaceGateMainWindow::updateClock);
    clockTimer_.start(1000);
    updateClock();

    connect(&networkTimer_, &QTimer::timeout, this, &FaceGateMainWindow::updateNetworkSummary);
    networkTimer_.start(5000);
    updateNetworkSummary();
    layoutOverlayPanels();
}

/** @brief 窗口尺寸变化后重新布局左右覆盖面板。 */
void FaceGateMainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (preview_ && centralWidget()) {
        preview_->setGeometry(centralWidget()->rect());
    }
    layoutOverlayPanels();
    if (adminPanel_ && adminPanel_->isVisible()) {
        adminPanel_->setGeometry(rect());
        adminPanel_->raise();
    }
}

/**
 * @brief 将侧边信息面板固定在 1024x768 闸机边缘。
 */
void FaceGateMainWindow::layoutOverlayPanels()
{
    QWidget *root = centralWidget();
    if (!root || !leftOverlay_ || !rightOverlay_) {
        return;
    }

    const int panelWidth = qBound(190, root->width() / 5, 228);
    leftOverlay_->setGeometry(0, 0, panelWidth, root->height());
    rightOverlay_->setGeometry(root->width() - panelWidth, 0, panelWidth, root->height());
    if (preview_) {
        preview_->setSideOverlayWidth(panelWidth);
    }
    leftOverlay_->raise();
    rightOverlay_->raise();
}

/**
 * @brief 刷新安静时钟浮层，不影响识别流程。
 */
void FaceGateMainWindow::updateClock()
{
    const QDateTime now = QDateTime::currentDateTime();
    if (timeLabel_) {
        timeLabel_->setText(now.toString("HH:mm"));
    }
    if (dateLabel_) {
        dateLabel_->setText(now.toString("yyyy-MM-dd ddd"));
    }
}

/**
 * @brief 从第一个活动的非 loopback 地址显示简洁在线/离线状态。
 */
void FaceGateMainWindow::updateNetworkSummary()
{
    QString address;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
                address = ip.toString();
                break;
            }
        }
        if (!address.isEmpty()) {
            break;
        }
    }

    if (networkLabel_) {
        networkLabel_->setText(address.isEmpty() ? "网络 离线" : "网络 " + address);
    }
}

/**
 * @brief 连接摄像头、识别、活体、数据库和闸机服务。
 *
 * 摄像头帧始终是拷贝后的值对象。数据库任务投递给 DatabaseWorker
 * 通过队列调用执行，验证流程不会直接调用 MySQL。
 */
void FaceGateMainWindow::startServices()
{
    // 注册跨线程信号需要传递的自定义类型。
    qRegisterMetaType<VerifyState>("VerifyState");
    qRegisterMetaType<PersonInfo>("PersonInfo");
    qRegisterMetaType<QVector<PersonInfo>>("QVector<PersonInfo>");
    qRegisterMetaType<FaceFeatureData>("FaceFeatureData");
    qRegisterMetaType<FaceRecord>("FaceRecord");
    qRegisterMetaType<QVector<FaceRecord>>("QVector<FaceRecord>");
    qRegisterMetaType<PersonAdminRecord>("PersonAdminRecord");
    qRegisterMetaType<QVector<PersonAdminRecord>>("QVector<PersonAdminRecord>");
    qRegisterMetaType<VerifyLogViewRecord>("VerifyLogViewRecord");
    qRegisterMetaType<QVector<VerifyLogViewRecord>>("QVector<VerifyLogViewRecord>");
    qRegisterMetaType<StorageStats>("StorageStats");
    qRegisterMetaType<DetectedFace>("DetectedFace");
    qRegisterMetaType<QVector<DetectedFace>>("QVector<DetectedFace>");
    qRegisterMetaType<VerifyLog>("VerifyLog");
    qRegisterMetaType<VerifyLogFilter>("VerifyLogFilter");
    qRegisterMetaType<OperatorAuditLog>("OperatorAuditLog");
    qRegisterMetaType<SyncTaskRecord>("SyncTaskRecord");
    qRegisterMetaType<QVector<SyncTaskRecord>>("QVector<SyncTaskRecord>");
    qRegisterMetaType<SystemEventLog>("SystemEventLog");
    qRegisterMetaType<QVector<SystemEventLog>>("QVector<SystemEventLog>");
    qRegisterMetaType<LivenessResult>("LivenessResult");
    qRegisterMetaType<CameraFrame>("CameraFrame");
    qRegisterMetaType<FaceAnalysisResult>("FaceAnalysisResult");
    qRegisterMetaType<QJsonArray>("QJsonArray");
    qRegisterMetaType<QStringList>("QStringList");

    startFaceInferenceThread();
    FaceImageSyncBridge *faceSyncBridge = FaceImageSyncBridge::instance();
    connect(faceSyncBridge,
            &FaceImageSyncBridge::storedFaceValidationRequested,
            this,
            [this, faceSyncBridge](const QString &token,
                                   const QString &personId,
                                   const QJsonArray &faces) {
        if (shutdown_ || !faceInferenceWorker_
                || !faceInferenceThread_.isRunning()) {
            QJsonArray failedFaces;
            const QString message = QStringLiteral(
                        "人脸识别服务未就绪，请稍后重新录入人脸。");
            for (const QJsonValue &value : faces) {
                QJsonObject failed = value.toObject();
                failed.insert(QStringLiteral("message"), message);
                failedFaces.append(failed);
            }
            faceSyncBridge->reportStoredFaceValidation(
                        token, personId, false,
                        message, QJsonArray(), failedFaces);
            return;
        }

        FaceInferenceWorker *worker = faceInferenceWorker_;
        const AppConfig validationConfig = config_;
        const bool queued = QMetaObject::invokeMethod(
                    worker,
                    [worker, token, personId, faces, validationConfig]() {
            worker->validateSyncedFaceImages(
                        token, personId, faces, validationConfig);
        },
        Qt::QueuedConnection);
        if (!queued) {
            QJsonArray failedFaces;
            const QString message = QStringLiteral(
                        "提交人脸图像校验任务失败，请重新录入人脸。");
            for (const QJsonValue &value : faces) {
                QJsonObject failed = value.toObject();
                failed.insert(QStringLiteral("message"), message);
                failedFaces.append(failed);
            }
            faceSyncBridge->reportStoredFaceValidation(
                        token, personId, false,
                        message, QJsonArray(), failedFaces);
        }
    });
    connect(faceInferenceWorker_,
            &FaceInferenceWorker::syncedFaceImagesValidated,
            faceSyncBridge,
            &FaceImageSyncBridge::reportStoredFaceValidation,
            Qt::QueuedConnection);
    connect(faceSyncBridge,
            &FaceImageSyncBridge::networkFaceGalleryRefreshRequested,
            this,
            [this]() {
        if (databaseWorker_ && databaseThread_.isRunning()) {
            QMetaObject::invokeMethod(
                        databaseWorker_, "reloadGallery",
                        Qt::QueuedConnection);
        }
    });
    connect(&audio_, &AudioService::audioStatus, this, [](const QString &message) {
        qInfo().noquote() << "音频播报：" << message;
    });
    connect(&audio_, &AudioService::audioError, this, [this](const QString &message) {
        qWarning().noquote() << "音频播报错误：" << message;
        writeSystemEvent("audio", "warning", "音频播报错误", message);
    });
    audio_.configure(config_);
    livenessEnabled_ = config_.livenessEnabled;
    if (livenessLabel_) {
        livenessLabel_->setText(livenessEnabled_ ? "活体开启" : "活体关闭");
    }
    startDatabaseThread();
    maintenanceWorker_ = new MaintenanceWorker(config_);
    maintenanceWorker_->moveToThread(&maintenanceThread_);
    connect(&maintenanceThread_, &QThread::finished, maintenanceWorker_, &QObject::deleteLater);
    connect(maintenanceWorker_, &MaintenanceWorker::maintenanceFinished, this, [this](bool ok, const QString &message) {
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        if (!ok) {
            writeSystemEvent("maintenance", "error", "运维任务失败", message);
        }
        refreshAdminStorageStats();
        if (databaseWorker_) {
            QMetaObject::invokeMethod(databaseWorker_, "loadSystemEvents", Qt::QueuedConnection);
        }
    });
    maintenanceThread_.start();

    connect(&camera_, &CameraService::frameAvailable, this, [this]() {
        CameraFrame frame;
        if (!camera_.takeLatestFrame(&frame)) {
            return;
        }
        latestFrame_ = frame.image;
        latestEnrollmentPreview_ = frame.enrollmentPreview;
        if (adminModeActive_) {
            if (adminPanel_ && adminPanel_->cameraRequired()) {
                processAdminFrame(frame);
            }
            return;
        }

        preview_->setFrame(frame.image);
        if (!moduleActive_ || !faceInferenceWorker_ || faceFramePending_) {
            return;
        }
        faceFramePending_ = true;
        const bool queued = QMetaObject::invokeMethod(
            faceInferenceWorker_,
            "processVerificationFrame",
            Qt::QueuedConnection,
            Q_ARG(CameraFrame, frame));
        if (!queued) {
            faceFramePending_ = false;
            qWarning() << "[FACE-INFERENCE] failed to queue verification frame";
        }
    });

    connect(&camera_, &CameraService::cameraStatus, this, [this](const QString &message) {
        cameraLabel_->setText("摄像头 " + message);
    });
    connect(&camera_, &CameraService::cameraError, this, [this](const QString &message) {
        qWarning().noquote() << "摄像头错误：" << message;
        cameraLabel_->setText("摄像头 " + message);
        writeSystemEvent("camera", "error", "摄像头错误", message);
    });

    connect(faceInferenceWorker_, &FaceInferenceWorker::engineStatus, this, [this](const QString &message) {
        qInfo().noquote() << "人脸引擎：" << message;
        recognitionLabel_->setText(message);
    });
    connect(&liveness_, &LivenessWorker::workerStatus, this, [this](const QString &message) {
        livenessLabel_->setText("活体 " + message);
    });

    connect(&liveness_, &LivenessWorker::resultReady, this, [this](const LivenessResult &result) {
        if (!adminModeActive_ ||
            !adminLivenessPending_ ||
            result.snapshot.frameIndex != adminLivenessLastFrame_) {
            return;
        }
        adminLivenessPending_ = false;
        if (!adminPanel_ || !adminPanel_->cameraRequired()) {
            return;
        }

        const bool livePassed =
            result.valid &&
            result.passed &&
            result.realScore >= config_.livenessThreshold;

        if (livePassed) {
            adminLastLivenessValid_ = true;
            adminLastLivenessResult_ = result;
            adminLastLivenessTimer_.restart();
        } else {
            // 活体失败结果只用于本次界面提示和语音播报，不能保留为录入状态。
            // 否则自动采集下一轮即使活体通过，也可能被上一轮失败结果污染。
            adminLastLivenessValid_ = false;
            adminLastLivenessResult_ = LivenessResult();
            adminLastLivenessTimer_.invalidate();
        }

        const QString text = result.valid
            ? QString("%1 真人=%2 纸张=%3 屏幕=%4")
                .arg(livePassed ? "活体通过" : "活体拒绝")
                .arg(result.realScore, 0, 'f', 2)
                .arg(result.paperScore, 0, 'f', 2)
                .arg(result.screenScore, 0, 'f', 2)
            : result.message;
        adminPanel_->setEnrollLivenessResult(livenessEnabled_, result, text);

        if (result.valid && !livePassed) {
            audio_.playPrompt(AudioService::Prompt::LivenessFailed);
        }
    });

    connect(faceInferenceWorker_, &FaceInferenceWorker::frameProcessed, this, [this]() {
        faceFramePending_ = false;
    });

    connect(faceInferenceWorker_, &FaceInferenceWorker::adminAnalysisReady,
            this, &FaceGateMainWindow::handleAdminAnalysis);

    connect(faceInferenceWorker_, &FaceInferenceWorker::enrollmentFeatureReady,
            this, &FaceGateMainWindow::handleEnrollmentFeature);

    connect(faceInferenceWorker_, &FaceInferenceWorker::stateChanged, this,
        [this](VerifyState state, const QString &message) {
            if (adminModeActive_ || passwordAccessActive_) {
                return;
            }
            preview_->setState(state);
            preview_->setStateText(message.isEmpty() ? stateName(state) : message);
            recognitionLabel_->setText(stateName(state).toUpper());
        });

    connect(faceInferenceWorker_, &FaceInferenceWorker::facesUpdated, this,
        [this](const QVector<DetectedFace> &faces) {
            if (adminModeActive_ || passwordAccessActive_) {
                return;
            }
            preview_->setFaces(faces);
        });

    connect(faceInferenceWorker_, &FaceInferenceWorker::verificationPassed, this,
        [this](const VerifyLog &log) {
            if (!moduleActive_ || adminModeActive_ || passwordAccessActive_) {
                return;
            }
            if (config_.sameFaceSuppressMs > 0 &&
                lastPassedTimer_.isValid() &&
                lastPassedTimer_.elapsed() < config_.sameFaceSuppressMs &&
                ((log.personId > 0 && log.personId == lastPassedPersonId_) ||
                 (!log.personNo.isEmpty() && log.personNo == lastPassedPersonNo_))) {
                qInfo().noquote() << "同人重复通行已抑制：" << log.personNo;
                recognitionLabel_->setText("已通过，请勿重复刷脸");
                preview_->setStateText("已通过，请勿重复刷脸");
                return;
            }
            if (faceAccessPending_) {
                recognitionLabel_->setText(QStringLiteral("正在识别"));
                preview_->setStateText(QStringLiteral("正在识别"));
                return;
            }

            VerifyLog enriched = log;
            enriched.result = QStringLiteral("pending");
            enriched.eventType = QStringLiteral("face_verify");
            enriched.direction = QStringLiteral("in");
            enriched.deviceSn = QString::fromLocal8Bit(QSysInfo::machineUniqueId());
            if (enriched.nameSnapshot.isEmpty()) {
                for (const FaceRecord &record : latestGallery_) {
                    if (record.person.id == enriched.personId
                            || record.person.personNo == enriched.personNo) {
                        enriched.nameSnapshot = record.person.name;
                        break;
                    }
                }
            }
            QString snapshotError;
            enriched.snapshotPath = SnapshotService(config_).saveVerifySnapshot(latestFrame_, enriched.personNo, enriched.result, &snapshotError);
            if (enriched.snapshotPath.isEmpty() && !snapshotError.isEmpty()) {
                qWarning().noquote() << snapshotError;
            }
            IcEventBridge::instance()->emitFaceUploadRequested(
                        enriched.personNo, enriched.faceHash,
                        enriched.snapshotPath, true);

            faceAccessPending_ = true;
            pendingFaceAccessLog_ = enriched;
            recognitionLabel_->setText(QStringLiteral("正在识别"));
            preview_->setStateText(QStringLiteral("正在识别"));
            qInfo().noquote() << "人脸匹配成功，等待权限校验："
                              << enriched.personNo << enriched.cosine << enriched.liveScore;
            qInfo().noquote() << "[FACE-ACCESS] dispatch permission lookup"
                              << "personId=" << enriched.personNo
                              << "localPersonId=" << enriched.personId;
            IcEventBridge::instance()->emitFaceRecognized(
                        enriched.personNo, enriched.personId,
                        enriched.nameSnapshot, enriched.faceHash);
        });

    connect(faceInferenceWorker_, &FaceInferenceWorker::verificationFailed, this,
        [this](const VerifyLog &log) {
            if (!moduleActive_ || adminModeActive_ || passwordAccessActive_) {
                return;
            }
            VerifyLog enriched = log;
            enriched.eventType = enriched.eventType.isEmpty() ? QStringLiteral("face_verify") : enriched.eventType;
            enriched.direction = enriched.direction.isEmpty() ? QStringLiteral("in") : enriched.direction;
            enriched.deviceSn = QString::fromLocal8Bit(QSysInfo::machineUniqueId());
            if (enriched.failReason.isEmpty()) {
                enriched.failReason = enriched.result == QStringLiteral("failed")
                    ? QStringLiteral("验证失败，请正对摄像头")
                    : QStringLiteral("验证失败");
            }
            const bool uploadFailure = enriched.result == QStringLiteral("stranger")
                    || enriched.result == QStringLiteral("liveness_failed");
            if (config_.saveFailedSnapshot && uploadFailure) {
                QString snapshotError;
                const QString personNo = enriched.personNo.isEmpty() ? QStringLiteral("unknown") : enriched.personNo;
                enriched.snapshotPath = SnapshotService(config_).saveVerifySnapshot(latestFrame_, personNo, enriched.result, &snapshotError);
                if (enriched.snapshotPath.isEmpty() && !snapshotError.isEmpty()) {
                    qWarning().noquote() << snapshotError;
                }
            }
            qInfo().noquote() << "验证失败：" << enriched.result << enriched.personNo << enriched.cosine << enriched.liveScore;
            if (enriched.result == QStringLiteral("stranger")) {
                audio_.playPrompt(AudioService::Prompt::Stranger);
            } else if (enriched.result == QStringLiteral("liveness_failed")) {
                audio_.playPrompt(AudioService::Prompt::LivenessFailed);
            } else if (enriched.result == QStringLiteral("failed")) {
                audio_.playPrompt(AudioService::Prompt::VerifyFailed);
            }
            recognitionLabel_->setText(enriched.failReason);
            if (databaseWorker_) {
                QMetaObject::invokeMethod(databaseWorker_, "addVerifyLog", Qt::QueuedConnection, Q_ARG(VerifyLog, enriched));
            }
            if (uploadFailure) {
                IcEventBridge::instance()->emitFaceUploadRequested(
                            QString(), QString(), enriched.snapshotPath, false);
            }
        });

    connect(IcEventBridge::instance(), &IcEventBridge::faceAccessFinished,
            this,
            [this](const QString &personId, const QString &faceHash,
                   const QString &floors,
                   const QByteArray &rs485Frame, bool success,
                   const QString &reason) {
        Q_UNUSED(faceHash)
        if (!moduleActive_ || adminModeActive_) {
            if (faceAccessPending_ && pendingFaceAccessLog_.personNo == personId) {
                faceAccessPending_ = false;
                pendingFaceAccessLog_ = VerifyLog();
            }
            return;
        }
        const QString detail = QStringLiteral(
                    "personId=%1 floors=%2 rs485=%3 success=%4 reason=%5")
                .arg(personId,
                     floors,
                     QString::fromLatin1(rs485Frame.toHex()),
                     success ? QStringLiteral("true") : QStringLiteral("false"),
                     reason);
        const bool matchesPending = faceAccessPending_
                && pendingFaceAccessLog_.personNo == personId;
        VerifyLog completedLog;
        if (matchesPending) {
            completedLog = pendingFaceAccessLog_;
            completedLog.result = success
                    ? QStringLiteral("passed") : QStringLiteral("failed");
            completedLog.failReason = success
                    ? QString() : (reason.isEmpty()
                                   ? QStringLiteral("人脸通行权限校验失败") : reason);
        }
        if (success) {
            if (matchesPending) {
                lastPassedPersonId_ = completedLog.personId;
                lastPassedPersonNo_ = completedLog.personNo;
                lastPassedTimer_.restart();
                audio_.playPrompt(AudioService::Prompt::VerifyPassed);
                const QString successText = config_.privacyMode
                        ? QStringLiteral("验证通过\n已授权人员")
                        : QStringLiteral("验证通过\n%1").arg(
                              completedLog.nameSnapshot.isEmpty()
                              ? completedLog.personNo : completedLog.nameSnapshot);
                recognitionLabel_->setText(successText);
                preview_->setStateText(successText);
                emit recognitionSucceeded();
            }
            gateLabel_->setText(QStringLiteral("楼层权限 已下发"));
            writeSystemEvent(QStringLiteral("face_elevator"),
                             QStringLiteral("info"),
                             QStringLiteral("人脸楼层权限下发成功"), detail);
        } else {
            const QString failureText = reason.isEmpty()
                    ? QStringLiteral("人脸通行权限校验失败") : reason;
            gateLabel_->setText(rs485Frame.isEmpty()
                                ? QStringLiteral("权限校验失败")
                                : QStringLiteral("楼层权限 下发失败"));
            recognitionLabel_->setText(failureText);
            preview_->setStateText(failureText);
            if (matchesPending) {
                audio_.playPrompt(AudioService::Prompt::VerifyFailed);
            }
            writeSystemEvent(QStringLiteral("face_elevator"),
                             QStringLiteral("warning"),
                             QStringLiteral("人脸楼层权限下发失败"), detail);
        }
        if (matchesPending && databaseWorker_) {
            QMetaObject::invokeMethod(databaseWorker_, "addVerifyLog",
                                      Qt::QueuedConnection,
                                      Q_ARG(VerifyLog, completedLog));
        }
        if (matchesPending) {
            faceAccessPending_ = false;
            pendingFaceAccessLog_ = VerifyLog();
        }
        qInfo().noquote() << "[FACE-ACCESS] completed" << detail;
    }, Qt::QueuedConnection);

    connect(IcEventBridge::instance(), &IcEventBridge::passwordAccessFinished,
            this,
            [this](const QString &personId, const QString &floors,
                   const QByteArray &rs485Frame, bool success,
                   const QString &reason) {
        if (!passwordAccessActive_) {
            return;
        }

        const QString failureText = reason.isEmpty()
                ? QStringLiteral("密码通行权限校验失败") : reason;
        const QString detail = QStringLiteral(
                    "personId=%1 floors=%2 rs485=%3 success=%4 reason=%5")
                .arg(personId,
                     floors,
                     QString::fromLatin1(rs485Frame.toHex()),
                     success ? QStringLiteral("true") : QStringLiteral("false"),
                     success ? QString() : failureText);

        VerifyLog completedLog;
        completedLog.personNo = personId;
        completedLog.result = success
                ? QStringLiteral("passed") : QStringLiteral("failed");
        completedLog.failReason = success ? QString() : failureText;
        completedLog.deviceSn = QString::fromLocal8Bit(QSysInfo::machineUniqueId());
        completedLog.eventType = QStringLiteral("password_verify");
        completedLog.direction = QStringLiteral("in");
        for (const FaceRecord &record : latestGallery_) {
            if (record.person.personNo == personId) {
                completedLog.personId = record.person.id;
                completedLog.nameSnapshot = record.person.name;
                break;
            }
        }

        if (moduleActive_ && !adminModeActive_) {
            if (success) {
                lastPassedPersonId_ = completedLog.personId;
                lastPassedPersonNo_ = completedLog.personNo;
                lastPassedTimer_.restart();
                audio_.playPrompt(AudioService::Prompt::VerifyPassed);
                const QString displayName = completedLog.nameSnapshot.isEmpty()
                        ? QStringLiteral("已授权人员") : completedLog.nameSnapshot;
                const QString successText = config_.privacyMode
                        ? QStringLiteral("密码验证通过\n已授权人员")
                        : QStringLiteral("密码验证通过\n%1").arg(displayName);
                preview_->setState(VerifyState::Passed);
                preview_->setStateText(successText);
                recognitionLabel_->setText(successText);
                gateLabel_->setText(QStringLiteral("楼层权限 已下发"));
                emit recognitionSucceeded();
                writeSystemEvent(QStringLiteral("password_elevator"),
                                 QStringLiteral("info"),
                                 QStringLiteral("密码通行楼层权限下发成功"), detail);
            } else {
                audio_.playPrompt(AudioService::Prompt::VerifyFailed);
                preview_->setState(VerifyState::Failed);
                preview_->setStateText(failureText);
                recognitionLabel_->setText(failureText);
                gateLabel_->setText(rs485Frame.isEmpty()
                                    ? QStringLiteral("权限校验失败")
                                    : QStringLiteral("楼层权限 下发失败"));
                writeSystemEvent(QStringLiteral("password_elevator"),
                                 QStringLiteral("warning"),
                                 QStringLiteral("密码通行楼层权限下发失败"), detail);
            }

            if (databaseWorker_) {
                QMetaObject::invokeMethod(databaseWorker_, "addVerifyLog",
                                          Qt::QueuedConnection,
                                          Q_ARG(VerifyLog, completedLog));
            }
        }

        qInfo().noquote() << "[PASSWORD-ACCESS] completed" << detail;
        const quint64 completedGeneration = passwordAccessGeneration_;
        QTimer::singleShot(1500, this, [this, completedGeneration]() {
            if (completedGeneration != passwordAccessGeneration_) {
                return;
            }
            passwordAccessActive_ = false;
            if (passwordAccessButton_) {
                passwordAccessButton_->setEnabled(true);
            }
            if (moduleActive_ && !adminModeActive_) {
                resetFaceVerification(QStringLiteral("正在识别"));
            }
        });
    }, Qt::QueuedConnection);

    connect(&gate_, &GateOutputService::gateStatus, this, [this](const QString &message) {
        gateLabel_->setText("闸机 " + message);
    });

    servicesInitialized_ = true;
    qInfo() << "[FACEGATE-ADAPTER] UI/database services initialized; camera deferred";
}

/**
 * @brief 创建 InspireFace 专用线程。
 *
 * FaceEngine 和 VerificationController 会在该线程首次激活时创建，
 * 从而保证 SDK session 的创建、推理、匹配和销毁都不进入 GUI 线程。
 */
void FaceGateMainWindow::startFaceInferenceThread()
{
    faceInferenceWorker_ = new FaceInferenceWorker();
    faceInferenceWorker_->moveToThread(&faceInferenceThread_);
    faceInferenceThread_.setObjectName(QStringLiteral("FaceInferenceThread"));
    connect(&faceInferenceThread_, &QThread::finished,
            faceInferenceWorker_, &QObject::deleteLater);
    faceInferenceThread_.start();
}

/** @brief 在推理线程重置验证状态机。 */
void FaceGateMainWindow::resetFaceVerification(const QString &message)
{
    if (!faceInferenceWorker_ || !faceInferenceThread_.isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(
        faceInferenceWorker_,
        "resetVerification",
        Qt::QueuedConnection,
        Q_ARG(QString, message));
}

/**
 * @brief 创建专用数据库线程，并连接其结果信号。
 */
void FaceGateMainWindow::startDatabaseThread()
{
    databaseWorker_ = new DatabaseWorker(config_);
    databaseWorker_->moveToThread(&databaseThread_);

    connect(&databaseThread_, &QThread::started, databaseWorker_, &DatabaseWorker::open);
    connect(&databaseThread_, &QThread::finished, databaseWorker_, &QObject::deleteLater);

    connect(databaseWorker_, &DatabaseWorker::databaseReady, this, [this](bool ok, const QString &message) {
        if (ok) {
            qInfo().noquote() << "数据库就绪：" << message;
        } else {
            qWarning().noquote() << "数据库不可用：" << message;
        }
        databaseLabel_->setText(ok ? "数据库 在线" : "数据库 错误");
        if (!ok) {
            recognitionLabel_->setText("数据库错误");
            writeSystemEvent("database", "error", "数据库不可用", message);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::galleryLoaded, this, [this](const QVector<FaceRecord> &records) {
        qInfo().noquote() << "底库已发送到人脸引擎：" << records.size();
        latestGallery_ = records;
        // 管理员界面打开时复用已有窗口，避免重复创建信号连接。
    if (adminPanel_) {
            adminPanel_->setGallery(records);
        }
        galleryLabel_->setText(QString("底库 %1").arg(records.size()));
    });

    connect(databaseWorker_, &DatabaseWorker::networkGalleryLoaded, this,
            [this](const QVector<FaceRecord> &records) {
        qInfo().noquote() << "网络底库已发送到人脸引擎：" << records.size();
    });

    connect(databaseWorker_, &DatabaseWorker::recognitionGalleriesLoaded, this,
            [this](const QVector<FaceRecord> &localRecords,
                   const QVector<FaceRecord> &networkRecords) {
        if (faceInferenceWorker_) {
            QMetaObject::invokeMethod(
                faceInferenceWorker_,
                "updateRecognitionGalleries",
                Qt::QueuedConnection,
                Q_ARG(QVector<FaceRecord>, localRecords),
                Q_ARG(QVector<FaceRecord>, networkRecords));
        }
    });

    connect(databaseWorker_, &DatabaseWorker::peopleLoaded, this, [this](const QVector<PersonAdminRecord> &records) {
        latestPeople_ = records;
        if (adminPanel_) {
            adminPanel_->setPeople(records);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::passedVerifyLogsLoaded, this, [this](const QVector<VerifyLogViewRecord> &records) {
        latestPassedLogs_ = records;
        if (adminPanel_) {
            adminPanel_->setPassedVerifyLogs(records);
        }
    });
    connect(databaseWorker_, &DatabaseWorker::verifyLogsLoaded, this, [this](const QVector<VerifyLogViewRecord> &records) {
        latestPassedLogs_ = records;
        if (adminPanel_) {
            adminPanel_->setPassedVerifyLogs(records);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::syncTasksLoaded, this, [this](const QVector<SyncTaskRecord> &records) {
        if (adminPanel_) {
            adminPanel_->setSyncTasks(records);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::systemEventsLoaded, this, [this](const QVector<SystemEventLog> &records) {
        if (adminPanel_) {
            adminPanel_->setSystemEvents(records);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::storageStatsLoaded, this, [this](const StorageStats &stats) {
        latestStorageStats_ = stats;
        if (adminPanel_) {
            adminPanel_->setStorageStats(stats);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::personUpdated, this, [this](bool ok, const QString &message) {
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        if (!ok) {
            AppMessageDialog::warning(this, "人员", message);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::personDeleted, this, [this](bool ok, const QString &message) {
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        if (!ok) {
            AppMessageDialog::warning(this, "人员", message);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::enrollFinished, this, [this](bool ok, const QString &message) {
        if (ok) {
            qInfo().noquote() << "录入完成：" << message;
            audio_.playPrompt(AudioService::Prompt::EnrollSuccess);
        } else {
            qWarning().noquote() << "录入失败：" << message;
            audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        }
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        if (!ok) {
            AppMessageDialog::warning(this, "录入", message);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::peopleImportFinished, this, [this](bool ok, const QString &message) {
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        if (!ok) {
            AppMessageDialog::warning(this, "导入人员", message);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::verifyLogWritten, this, [this](bool ok, const QString &message) {
        if (!ok) {
            qWarning().noquote() << "验证日志状态：" << message;
            databaseLabel_->setText("数据库 日志错误");
            writeSystemEvent("database", "error", "验证日志写入失败", message);
        }
    });

    connect(databaseWorker_, &DatabaseWorker::cleanupFinished, this, [this](bool ok, const QString &message) {
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        if (!ok) {
            writeSystemEvent("maintenance", "error", "日志清理失败", message);
        }
        refreshAdminPassedLogs();
    });

    databaseThread_.start();
}

/** @brief 统计限定时间内的预览点击次数并触发管理员登录。 */
void FaceGateMainWindow::handleAdminTap()
{
    if (!adminTapTimer_.isValid() || adminTapTimer_.elapsed() > 3000) {
        adminTapTimer_.restart();
        adminTapCount_ = 0;
    }

    ++adminTapCount_;
    if (adminTapCount_ >= 5) {
        adminTapCount_ = 0;
        qInfo().noquote() << "管理员点击快捷入口已触发";
        if (config_.adminLoginRequired) {
            AdminLoginDialog dialog(config_, this);
            adminLoginActive_ = true;
            const int loginResult = dialog.exec();
            adminLoginActive_ = false;
            if (loginResult != QDialog::Accepted) {
                qInfo().noquote() << "管理员登录已取消或失败";
                writeOperatorAudit("管理员登录", "admin", config_.adminUsername, "失败", "登录取消或密码错误");
                return;
            }
            writeOperatorAudit("管理员登录", "admin", config_.adminUsername, "成功", "进入管理后台");
        }
        openAdminPanel();
    }
}

void FaceGateMainWindow::handlePasswordAccess()
{
    if (!moduleActive_ || adminModeActive_ || adminLoginActive_) {
        return;
    }
    if (passwordAccessActive_) {
        recognitionLabel_->setText(QStringLiteral("密码验证正在处理中"));
        preview_->setStateText(QStringLiteral("密码验证正在处理中"));
        return;
    }
    if (faceAccessPending_) {
        recognitionLabel_->setText(QStringLiteral("人脸权限正在校验，请稍候"));
        preview_->setStateText(QStringLiteral("人脸权限正在校验，请稍候"));
        return;
    }

    passwordAccessActive_ = true;
    ++passwordAccessGeneration_;
    if (passwordAccessButton_) {
        passwordAccessButton_->setEnabled(false);
    }
    resetFaceVerification(QStringLiteral("密码输入中"));

    QString password;
    const bool accepted = AccessPasswordDialog::getPassword(this, &password);
    if (!accepted) {
        passwordAccessActive_ = false;
        if (passwordAccessButton_) {
            passwordAccessButton_->setEnabled(true);
        }
        recognitionLabel_->setText(QStringLiteral("正在识别"));
        preview_->setStateText(QStringLiteral("正在识别"));
        resetFaceVerification(QStringLiteral("正在识别"));
        return;
    }

    recognitionLabel_->setText(QStringLiteral("密码验证中"));
    preview_->setStateText(QStringLiteral("密码验证中"));
    gateLabel_->setText(QStringLiteral("楼层权限 校验中"));
    qInfo() << "[PASSWORD-ACCESS] dispatch permission lookup";
    IcEventBridge::instance()->emitPasswordAccessRequested(password);
    password.fill(QLatin1Char('\0'));
    password.clear();
}

/** @brief 异步写入管理员操作审计记录。 */
void FaceGateMainWindow::writeOperatorAudit(const QString &action,
                                    const QString &targetType,
                                    const QString &targetId,
                                    const QString &result,
                                    const QString &detail)
{
    if (!databaseWorker_) {
        return;
    }
    OperatorAuditLog log;
    log.operatorName = config_.adminUsername;
    log.action = action;
    log.targetType = targetType;
    log.targetId = targetId;
    log.result = result;
    log.detail = detail;
    log.createdAt = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QMetaObject::invokeMethod(databaseWorker_, "addOperatorAuditLog", Qt::QueuedConnection, Q_ARG(OperatorAuditLog, log));
}

/** @brief 异步写入系统事件记录。 */
void FaceGateMainWindow::writeSystemEvent(const QString &eventType,
                                  const QString &level,
                                  const QString &message,
                                  const QString &detail)
{
    if (!databaseWorker_) {
        return;
    }
    SystemEventLog log;
    log.eventType = eventType;
    log.level = level;
    log.message = message;
    log.detail = detail;
    log.createdAt = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QMetaObject::invokeMethod(databaseWorker_, "addSystemEventLog", Qt::QueuedConnection, Q_ARG(SystemEventLog, log));
}

/** @brief 修改管理员密码并持久化配置。 */
void FaceGateMainWindow::handleAdminPasswordChange(const QString &oldPassword, const QString &newPassword)
{
    QString errorText;
    if (!config_.setAdminPassword(oldPassword, newPassword, &errorText)) {
        AppMessageDialog::warning(this, "管理员密码修改", errorText);
        if (adminPanel_) {
            adminPanel_->setStatusText("管理员密码修改失败：" + errorText);
        }
        return;
    }

    const bool saved = config_.save(config_.configPath, &errorText);
    const QString message = saved ? "管理员密码已修改" : ("管理员密码已修改，保存失败：" + errorText);
    writeOperatorAudit("修改管理员密码", "admin", config_.adminUsername, saved ? "成功" : "失败", message);
    if (adminPanel_) {
        adminPanel_->setStatusText(message);
    }
    if (!saved) {
        AppMessageDialog::warning(this, "管理员密码修改", message);
    } else if (!adminPanel_) {
        AppMessageDialog::information(this, "管理员密码修改", message);
    }
}

/** @brief 完成管理员认证后打开综合管理面板。 */
void FaceGateMainWindow::openAdminPanel()
{
    if (!adminPanel_) {
        adminPanel_ = new AdminPanel(config_, this);
        adminPanel_->setWindowFlags(Qt::Widget);
        adminPanel_->setAttribute(Qt::WA_DeleteOnClose);
        connect(adminPanel_, &AdminPanel::enrollRequested, this, &FaceGateMainWindow::handleEnrollRequest);
        connect(adminPanel_, &AdminPanel::livenessEnabledChanged, this, &FaceGateMainWindow::setLivenessEnabled);
        connect(adminPanel_, &AdminPanel::peopleRefreshRequested, this, &FaceGateMainWindow::refreshAdminPeople);
        FaceImageSyncBridge *faceSyncBridge = FaceImageSyncBridge::instance();
        connect(adminPanel_, &AdminPanel::networkPeopleRefreshRequested,
                faceSyncBridge, &FaceImageSyncBridge::requestPersonnelList);
        connect(faceSyncBridge, &FaceImageSyncBridge::personnelListReady,
                adminPanel_, &AdminPanel::setNetworkPeople);
        connect(faceSyncBridge, &FaceImageSyncBridge::syncStatusChanged,
                adminPanel_, [this](bool ok, const QString &message) {
            if (adminPanel_) {
                adminPanel_->showNetworkSyncStatus(ok, message);
            }
        });
        connect(adminPanel_, &AdminPanel::peopleImportRequested,
                this, &FaceGateMainWindow::handlePeopleImport);
        connect(adminPanel_, &AdminPanel::peopleExportRequested,
                this, &FaceGateMainWindow::handlePeopleExport);
        connect(adminPanel_, &AdminPanel::passedLogsRefreshRequested, this, &FaceGateMainWindow::refreshAdminPassedLogs);
        connect(adminPanel_, &AdminPanel::syncTasksRefreshRequested, this, [this]() {
            if (databaseWorker_) {
                QMetaObject::invokeMethod(databaseWorker_, "loadSyncTasks", Qt::QueuedConnection);
            }
        });
        connect(adminPanel_, &AdminPanel::maintenanceBackupRequested, this, &FaceGateMainWindow::handleMaintenanceBackup);
        connect(adminPanel_, &AdminPanel::maintenanceCleanupRequested, this, &FaceGateMainWindow::handleMaintenanceCleanup);
        connect(adminPanel_, &AdminPanel::deviceInfoRefreshRequested, this, &FaceGateMainWindow::refreshAdminStorageStats);
        connect(adminPanel_, &AdminPanel::thresholdConfigSaveRequested, this, &FaceGateMainWindow::handleThresholdConfigSave);
        connect(adminPanel_, &AdminPanel::networkConfigSaveRequested, this, &FaceGateMainWindow::handleNetworkConfigSave);
        connect(adminPanel_, &AdminPanel::audioConfigSaveRequested, this, &FaceGateMainWindow::handleAudioConfigSave);
        connect(adminPanel_, &AdminPanel::audioTestPlayRequested, this, &FaceGateMainWindow::handleAudioTestPlay);
        connect(adminPanel_, &AdminPanel::adminPasswordChangeRequested, this, &FaceGateMainWindow::handleAdminPasswordChange);
        connect(adminPanel_, &AdminPanel::personNoChangeRequested, this, &FaceGateMainWindow::handlePersonNoChange);
        connect(adminPanel_, &AdminPanel::personEnabledChangeRequested, this, &FaceGateMainWindow::handlePersonEnabledChange);
        connect(adminPanel_, &AdminPanel::personDeleteRequested, this, &FaceGateMainWindow::handlePersonDelete);
        connect(adminPanel_, &AdminPanel::cameraRequiredChanged, this, [this](bool) {
            updateCameraPowerState();
        });
        connect(adminPanel_, &AdminPanel::closed, this, [this]() {
            adminModeActive_ = false;
            adminLivenessPending_ = false;
            resetFaceVerification(QStringLiteral("等待人脸"));
            updateCameraPowerState();
        });
        connect(adminPanel_, &QObject::destroyed, this, [this]() {
            adminPanel_ = nullptr;
            qInfo().noquote() << "管理员面板已销毁";
        });
    }

    qInfo().noquote() << "正在全屏打开管理员面板";
    adminModeActive_ = true;
    adminLivenessPending_ = false;
    adminLivenessLastFrame_ = -1000000;
    resetFaceVerification(QStringLiteral("管理员模式"));
    adminPanel_->setCurrentFrame(latestFrame_, latestEnrollmentPreview_);
    adminPanel_->setEnrollLivenessStatus(livenessEnabled_, livenessEnabled_ ? "等待真人人脸" : "活体关闭");
    adminPanel_->setGallery(latestGallery_);
    adminPanel_->setPeople(latestPeople_);
    adminPanel_->setStorageStats(latestStorageStats_);
    adminPanel_->setLivenessEnabled(livenessEnabled_);
    refreshAdminPeople();
    refreshAdminStorageStats();
    FaceImageSyncBridge::instance()->requestPersonnelList();

    // EGLFS 融合模式下管理面板保持为统一 AppShell 内的普通子控件，
    // 不创建第二个全屏顶层窗口。
    adminPanel_->setGeometry(rect());
    adminPanel_->show();
    adminPanel_->raise();
    adminPanel_->setFocus();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    updateCameraPowerState();
}


/**
 * @brief 向管理员录入预览送帧，但不运行身份验证。
 *
 * 管理员面板打开时，主闸机预览和 VerificationController 会被
 * 有意绕过。录入页面只接收人脸框和可选
 * 活体反馈，因此不会执行底库识别、开闸，
 * 也不会在后台写入识别记录。
 */
void FaceGateMainWindow::processAdminFrame(const CameraFrame &frame)
{
    if (!moduleActive_ ||
        !adminPanel_ ||
        !adminPanel_->cameraRequired() ||
        !faceInferenceWorker_ ||
        faceFramePending_) {
        return;
    }

    faceFramePending_ = true;
    const bool queued = QMetaObject::invokeMethod(
        faceInferenceWorker_,
        "processAdminFrame",
        Qt::QueuedConnection,
        Q_ARG(CameraFrame, frame));
    if (!queued) {
        faceFramePending_ = false;
        qWarning() << "[FACE-INFERENCE] failed to queue admin frame";
    }
}

/** @brief 将管理员检测分析和预览更新到录入面板。 */
void FaceGateMainWindow::handleAdminAnalysis(const CameraFrame &frame,
                                             const FaceAnalysisResult &analysis)
{
    if (!adminModeActive_ || !adminPanel_ || !adminPanel_->cameraRequired()) {
        return;
    }

    adminPanel_->setEnrollAnalysis(
        frame.image, frame.enrollmentPreview, analysis.faces, analysis.message);

    if (!livenessEnabled_) {
        adminLivenessPending_ = false;
        adminLastLivenessValid_ = false;
        adminLastLivenessResult_ = LivenessResult();
        adminLastLivenessTimer_.invalidate();
        adminPanel_->setEnrollLivenessStatus(false, "活体关闭");
        return;
    }

    if (analysis.faces.isEmpty()) {
        adminLivenessPending_ = false;
        adminLastLivenessValid_ = false;
        adminLastLivenessResult_ = LivenessResult();
        adminLastLivenessTimer_.invalidate();
        adminPanel_->setEnrollLivenessStatus(true, analysis.message.isEmpty() ? "未检测到人脸" : analysis.message);
        return;
    }

    if (!liveness_.isReady()) {
        adminLastLivenessValid_ = false;
        adminLastLivenessResult_ = LivenessResult();
        adminLastLivenessTimer_.invalidate();
        adminPanel_->setEnrollLivenessStatus(true, "活体工作线程未就绪");
        return;
    }

    if (adminLivenessPending_) {
        return;
    }

    const int interval = qMax(1, config_.collectFrameCount);
    if (frame.frameIndex - adminLivenessLastFrame_ < interval) {
        return;
    }

    auto bestFaceIt = std::max_element(analysis.faces.begin(), analysis.faces.end(),
        [&frame](const DetectedFace &a, const DetectedFace &b) {
            const float areaA = frame.image.isNull() ? 0.0f :
                static_cast<float>(a.rect.width() * a.rect.height()) /
                static_cast<float>(qMax(1, frame.image.width() * frame.image.height()));
            const float areaB = frame.image.isNull() ? 0.0f :
                static_cast<float>(b.rect.width() * b.rect.height()) /
                static_cast<float>(qMax(1, frame.image.width() * frame.image.height()));
            const float scoreA = a.detConfidence + (a.qualityValid ? a.quality : 0.0f) + areaA;
            const float scoreB = b.detConfidence + (b.qualityValid ? b.quality : 0.0f) + areaB;
            return scoreA < scoreB;
        });

    VerificationSnapshot snapshot;
    snapshot.image = frame.image;
    snapshot.yuv420sp = frame.yuv420sp;
    snapshot.pixelFormat = frame.pixelFormat;
    snapshot.width = frame.width;
    snapshot.height = frame.height;
    snapshot.nv21 = frame.nv21;
    snapshot.face = *bestFaceIt;
    snapshot.frameIndex = frame.frameIndex;

    adminLivenessPending_ = true;
    adminLivenessLastFrame_ = frame.frameIndex;
    adminPanel_->setEnrollLivenessStatus(true, "正在检测活体...");
    liveness_.submit(snapshot);
}


/** @return 当前验证界面或管理页面需要摄像头时返回 true。 */
bool FaceGateMainWindow::currentUiNeedsCamera() const
{
    if (!adminModeActive_) {
        return true;
    }
    return adminPanel_ && adminPanel_->cameraRequired();
}

/** @brief 根据模块活动态和当前页面需求启停摄像头。 */
void FaceGateMainWindow::updateCameraPowerState()
{
    const bool needCamera = moduleActive_ && currentUiNeedsCamera();
    if (needCamera) {
        if (!camera_.isRunning()) {
            if (cameraLabel_) {
                cameraLabel_->setText("摄像头 启动中");
            }
            camera_.start(config_.activeCameraProfile());
        }
        if (adminModeActive_) {
            adminLivenessPending_ = false;
            adminLivenessLastFrame_ = -1000000;
            adminLastLivenessValid_ = false;
            adminLastLivenessResult_ = LivenessResult();
            adminLastLivenessTimer_.invalidate();
        }
        return;
    }

    adminLivenessPending_ = false;
    adminLivenessLastFrame_ = -1000000;
    // 自动采集成功后会停止摄像头并进入填写人员信息页，此时必须保留
    // 刚刚通过的活体结果，供保存录入时校验。失败结果在 resultReady 中已清空。
    if (!(adminLastLivenessValid_ &&
          adminLastLivenessResult_.valid &&
          adminLastLivenessResult_.passed &&
          adminLastLivenessResult_.realScore >= config_.livenessThreshold)) {
        adminLastLivenessValid_ = false;
        adminLastLivenessResult_ = LivenessResult();
        adminLastLivenessTimer_.invalidate();
    }
    if (camera_.isRunning()) {
        qInfo().noquote() << "当前界面不需要摄像头，停止采集链路";
        if (cameraLabel_) {
            cameraLabel_->setText("摄像头 正在停止");
        }
        camera_.stop();
    }
    clearCameraFrames();
    if (cameraLabel_) {
        cameraLabel_->setText("摄像头 已停止");
    }
}

/** @brief 清除主预览和录入预览中的旧帧。 */
void FaceGateMainWindow::clearCameraFrames()
{
    latestFrame_ = QImage();
    latestEnrollmentPreview_ = QImage();
    if (preview_) {
        preview_->clearFrame();
        preview_->setState(VerifyState::Idle);
        preview_->setStateText("等待中");
    }
    if (adminPanel_) {
        adminPanel_->clearEnrollPreview();
    }
}

/** @brief 请求刷新管理员人员列表。 */
void FaceGateMainWindow::refreshAdminPeople()
{
    if (databaseWorker_) {
        const bool includeDeleted = adminPanel_ && adminPanel_->includeDeletedPeople();
        QMetaObject::invokeMethod(databaseWorker_, "loadPeople", Qt::QueuedConnection, Q_ARG(bool, includeDeleted));
    }
}

/** @brief 请求按条件刷新验证日志。 */
void FaceGateMainWindow::refreshAdminPassedLogs(const VerifyLogFilter &requestedFilter)
{
    if (databaseWorker_) {
        VerifyLogFilter filter = requestedFilter;
        if (filter.result.isEmpty()) {
            filter.result = QStringLiteral("all");
        }
        if (filter.limit <= 0) {
            filter.limit = 500;
        }
        QMetaObject::invokeMethod(databaseWorker_, "loadVerifyLogs", Qt::QueuedConnection, Q_ARG(VerifyLogFilter, filter));
    }
}

/** @brief 启动U盘复制、Excel解析和网络人员数据库更新后台任务。 */
void FaceGateMainWindow::handlePeopleImport()
{
    if (personExportThread_ && personExportThread_->isRunning()) {
        AppMessageDialog::information(this, QStringLiteral("导入网络人员"),
                                      QStringLiteral("人员表正在导出，请等待导出结束"));
        return;
    }
    if (personImportThread_ && personImportThread_->isRunning()) {
        AppMessageDialog::information(this, QStringLiteral("导入网络人员"),
                                      QStringLiteral("人员表正在导入，请勿重复操作"));
        return;
    }
    writeOperatorAudit(QStringLiteral("导入网络人员Excel"), QStringLiteral("network_person"),
                       QStringLiteral("batch"), QStringLiteral("请求"),
                       QStringLiteral("等待U盘人员表"));
    if (adminPanel_) {
        adminPanel_->setPersonImportBusy(true, QStringLiteral("正在检测U盘…"));
    }

    personImportThread_ = new QThread(this);
    personImportWorker_ = new PersonImportWorker();
    PersonImportWorker *worker = personImportWorker_;
    QThread *thread = personImportThread_;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &PersonImportWorker::run);
    connect(worker, &PersonImportWorker::progress, this, [this](const QString &message) {
        if (adminPanel_) adminPanel_->setPersonImportBusy(true, message);
    });
    connect(worker, &PersonImportWorker::finished, this,
            [this](bool ok,
                   int inserted,
                   int updated,
                   int unchanged,
                   int failed,
                   const QStringList &unchangedNames,
                   const QStringList &failedNames,
                   const QString &message,
                   const QString &sourceFileName) {
        if (shutdown_) return;
        const int completed = inserted + updated;
        QString detail = QStringLiteral("文件：%1\n更新完成：%2 人（新增 %3，修改 %4）\n未做更新：%5 人")
                .arg(sourceFileName.isEmpty() ? QStringLiteral("未找到") : sourceFileName)
                .arg(completed).arg(inserted).arg(updated).arg(unchanged);
        if (!unchangedNames.isEmpty()) {
            detail += QStringLiteral("\n未更新人员：") + unchangedNames.join(QStringLiteral("、"));
        }
        if (failed > 0) {
            detail += QStringLiteral("\n失败：%1 人").arg(failed);
            if (!failedNames.isEmpty()) {
                detail += QStringLiteral("\n失败人员：") + failedNames.join(QStringLiteral("、"));
            }
        }
        if (!message.isEmpty()) detail += QStringLiteral("\n\n") + message;

        if (adminPanel_) adminPanel_->setPersonImportBusy(false, message);
        writeOperatorAudit(QStringLiteral("导入网络人员Excel"), QStringLiteral("network_person"),
                           sourceFileName, ok ? QStringLiteral("成功") : QStringLiteral("部分失败"), detail);

        if (completed > 0) {
            if (adminPanel_) adminPanel_->showNetworkPeoplePage();
            if (databaseWorker_) {
                QMetaObject::invokeMethod(databaseWorker_, "reloadGallery", Qt::QueuedConnection);
                QMetaObject::invokeMethod(databaseWorker_, "loadStorageStats", Qt::QueuedConnection);
            }
            FaceImageSyncBridge::instance()->requestPersonnelList();
        }
        if (ok) {
            AppMessageDialog::information(this, QStringLiteral("导入网络人员"), detail);
        } else {
            AppMessageDialog::warning(this, QStringLiteral("导入网络人员"), detail);
        }
    });
    connect(worker, &PersonImportWorker::finished, thread, &QThread::quit);
    connect(worker, &PersonImportWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (personImportThread_ == thread) {
            personImportThread_ = nullptr;
            personImportWorker_ = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}

/** @brief 在后台读取选定人员范围，生成固定XLSX并写入U盘 person 目录。 */
void FaceGateMainWindow::handlePeopleExport(const QString &personnelScope)
{
    if (personImportThread_ && personImportThread_->isRunning()) {
        AppMessageDialog::information(this, QStringLiteral("导出人员"),
                                      QStringLiteral("人员表正在导入，请等待导入结束"));
        return;
    }
    if (personExportThread_ && personExportThread_->isRunning()) {
        AppMessageDialog::information(this, QStringLiteral("导出人员"),
                                      QStringLiteral("人员表正在导出，请勿重复操作"));
        return;
    }

    PersonExportKind kind = PersonExportKind::Local;
    if (personnelScope == QLatin1String("network")) {
        kind = PersonExportKind::Network;
    } else if (personnelScope == QLatin1String("all")) {
        kind = PersonExportKind::All;
    }
    const QString kindName = PersonXlsxExporter::kindName(kind);
    const QString auditTarget = kind == PersonExportKind::Network
            ? QStringLiteral("network_person")
            : (kind == PersonExportKind::All
               ? QStringLiteral("all_person") : QStringLiteral("local_person"));
    writeOperatorAudit(QStringLiteral("导出人员Excel"),
                       auditTarget,
                       QStringLiteral("batch"), QStringLiteral("请求"),
                       QStringLiteral("等待U盘并导出%1").arg(kindName));
    if (adminPanel_) {
        adminPanel_->setPersonExportBusy(true, QStringLiteral("正在检测U盘…"));
    }

    personExportThread_ = new QThread(this);
    personExportWorker_ = new PersonExportWorker(kind);
    PersonExportWorker *worker = personExportWorker_;
    QThread *thread = personExportThread_;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &PersonExportWorker::run);
    connect(worker, &PersonExportWorker::progress, this, [this](const QString &message) {
        if (adminPanel_) adminPanel_->setPersonExportBusy(true, message);
    });
    connect(worker, &PersonExportWorker::finished, this,
            [this](bool ok,
                   int personCount,
                   const QString &kindName,
                   const QString &fileName,
                   const QString &message) {
        if (shutdown_) return;
        if (adminPanel_) adminPanel_->setPersonExportBusy(false, message);
        const QString detail = QStringLiteral("类型：%1\n文件：%2\n人员数量：%3\n%4")
                .arg(kindName,
                     fileName.isEmpty() ? QStringLiteral("未生成") : fileName)
                .arg(personCount)
                .arg(message);
        const QString auditTarget = kindName == QStringLiteral("网络人员")
                ? QStringLiteral("network_person")
                : (kindName == QStringLiteral("全部人员")
                   ? QStringLiteral("all_person") : QStringLiteral("local_person"));
        writeOperatorAudit(QStringLiteral("导出人员Excel"),
                           auditTarget,
                           fileName, ok ? QStringLiteral("成功") : QStringLiteral("失败"),
                           detail);
        if (ok) {
            AppMessageDialog::success(this, QStringLiteral("导出人员"), detail);
        } else {
            AppMessageDialog::warning(this, QStringLiteral("导出人员"), detail);
        }
    });
    connect(worker, &PersonExportWorker::finished, thread, &QThread::quit);
    connect(worker, &PersonExportWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (personExportThread_ == thread) {
            personExportThread_ = nullptr;
            personExportWorker_ = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}

/** @brief 投递 SQLite 备份维护任务。 */
void FaceGateMainWindow::handleMaintenanceBackup()
{
    writeOperatorAudit("数据库备份", "maintenance", "sqlite", "请求", config_.backupDir);
    if (maintenanceWorker_) {
        QMetaObject::invokeMethod(maintenanceWorker_, "backupSqliteDatabase", Qt::QueuedConnection);
    }
}

/** @brief 投递过期日志和抓拍清理任务。 */
void FaceGateMainWindow::handleMaintenanceCleanup(int days)
{
    writeOperatorAudit("清理日志抓拍", "maintenance", QString::number(days), "请求", "清理过期数据");
    if (databaseWorker_) {
        QMetaObject::invokeMethod(databaseWorker_, "cleanupOldVerifyLogs", Qt::QueuedConnection, Q_ARG(int, days));
    }
    if (maintenanceWorker_) {
        QMetaObject::invokeMethod(maintenanceWorker_, "cleanupSnapshots", Qt::QueuedConnection, Q_ARG(int, days));
    }
}

/** @brief 请求刷新存储统计。 */
void FaceGateMainWindow::refreshAdminStorageStats()
{
    if (databaseWorker_) {
        QMetaObject::invokeMethod(databaseWorker_, "loadStorageStats", Qt::QueuedConnection);
        QMetaObject::invokeMethod(databaseWorker_, "loadSystemEvents", Qt::QueuedConnection);
    }
}

/** @brief 校验并持久化五项识别阈值，再同步推理线程。 */
void FaceGateMainWindow::handleThresholdConfigSave(float faceCosine, float duplicateFaceCosine, float faceQuality, float faceDetect, float liveness)
{
    config_.faceCosineThreshold = qBound(0.0f, faceCosine, 1.0f);
    config_.duplicateFaceCosineThreshold = qBound(0.0f, duplicateFaceCosine, config_.faceCosineThreshold);
    config_.faceQualityThreshold = qBound(0.0f, faceQuality, 1.0f);
    config_.faceDetectThreshold = qBound(0.0f, faceDetect, 1.0f);
    config_.livenessThreshold = qBound(0.0f, liveness, 1.0f);

    QString errorText;
    const bool saved = config_.save(config_.configPath, &errorText);
    if (faceInferenceWorker_) {
        FaceInferenceWorker *worker = faceInferenceWorker_;
        const AppConfig updatedConfig = config_;
        QMetaObject::invokeMethod(
            worker,
            [worker, updatedConfig]() {
                worker->updateConfiguration(updatedConfig);
            },
            Qt::QueuedConnection);
    }
    liveness_.updateConfig(config_);
    if (faceInferenceWorker_) {
        QMetaObject::invokeMethod(
            faceInferenceWorker_,
            "setLivenessEnabled",
            Qt::QueuedConnection,
            Q_ARG(bool, livenessEnabled_));
    }

    const QString message = saved ? "阈值设置已保存" : ("阈值已应用，保存失败：" + errorText);
    writeOperatorAudit("修改阈值", "threshold", "face", saved ? "成功" : "失败", message);
    if (adminPanel_) {
        adminPanel_->setStatusText(message);
    }
    if (!saved) {
        AppMessageDialog::warning(this, "设置", message);
    }
}

/** @brief 保存 DHCP 或静态网络配置。 */
void FaceGateMainWindow::handleNetworkConfigSave(bool dhcp, const QString &ip, const QString &netmask, const QString &gateway, const QString &dns)
{
    qDebug().noquote() << "MainWindow 收到网络设置："
                       << "dhcp=" << dhcp
                       << "ip=" << ip
                       << "netmask=" << netmask
                       << "gateway=" << gateway
                       << "dns=" << dns
                       << "configPath=" << config_.configPath;
    config_.networkDhcp = dhcp;
    config_.networkStaticIp = ip;
    config_.networkNetmask = netmask;
    config_.networkGateway = gateway;
    config_.networkDns = dns;

    QString errorText;
    const bool saved = config_.save(config_.configPath, &errorText);
    qDebug().noquote() << "网络设置写回结果："
                       << "saved=" << saved
                       << "path=" << config_.configPath
                       << "error=" << errorText;
    const QString message = saved
        ? "网络设置已保存，系统网络应用由部署脚本读取配置后生效"
        : ("网络设置保存失败：" + errorText);
    writeOperatorAudit("修改网络配置", "network", config_.networkDhcp ? "dhcp" : config_.networkStaticIp, saved ? "成功" : "失败", message);
    if (adminPanel_) {
        adminPanel_->setStatusText(message);
    }
    if (!saved) {
        AppMessageDialog::warning(this, "设置", message);
    }
}

/** @brief 保存音频配置并重新配置 AudioService。 */
void FaceGateMainWindow::handleAudioConfigSave(const AppConfig &audioConfig)
{
    config_.audioEnabled = audioConfig.audioEnabled;
    config_.audioPlayer = audioConfig.audioPlayer;
    config_.audioDevice = audioConfig.audioDevice;
    config_.audioDir = audioConfig.audioDir;
    config_.audioVolume = qBound(0, audioConfig.audioVolume, 100);
    config_.audioMixerControl = audioConfig.audioMixerControl;
    config_.audioCooldownMs = qMax(0, audioConfig.audioCooldownMs);

    config_.promptVerifyPassedEnabled = audioConfig.promptVerifyPassedEnabled;
    config_.promptVerifyPassedText = audioConfig.promptVerifyPassedText;
    config_.promptVerifyPassedFile = audioConfig.promptVerifyPassedFile;

    config_.promptVerifyFailedEnabled = audioConfig.promptVerifyFailedEnabled;
    config_.promptVerifyFailedText = audioConfig.promptVerifyFailedText;
    config_.promptVerifyFailedFile = audioConfig.promptVerifyFailedFile;

    config_.promptStrangerEnabled = audioConfig.promptStrangerEnabled;
    config_.promptStrangerText = audioConfig.promptStrangerText;
    config_.promptStrangerFile = audioConfig.promptStrangerFile;

    config_.promptLivenessFailedEnabled = audioConfig.promptLivenessFailedEnabled;
    config_.promptLivenessFailedText = audioConfig.promptLivenessFailedText;
    config_.promptLivenessFailedFile = audioConfig.promptLivenessFailedFile;

    config_.promptEnrollSuccessEnabled = audioConfig.promptEnrollSuccessEnabled;
    config_.promptEnrollSuccessText = audioConfig.promptEnrollSuccessText;
    config_.promptEnrollSuccessFile = audioConfig.promptEnrollSuccessFile;

    config_.promptEnrollFailedEnabled = audioConfig.promptEnrollFailedEnabled;
    config_.promptEnrollFailedText = audioConfig.promptEnrollFailedText;
    config_.promptEnrollFailedFile = audioConfig.promptEnrollFailedFile;

    QString errorText;
    const bool saved = config_.save(config_.configPath, &errorText);
    audio_.configure(config_);

    const QString message = saved ? "音频设置已保存" : ("音频设置已应用，保存失败：" + errorText);
    writeOperatorAudit("修改音频设置", "audio", config_.audioEnabled ? "enabled" : "disabled", saved ? "成功" : "失败", message);
    if (adminPanel_) {
        adminPanel_->setStatusText(message);
    }
    if (!saved) {
        AppMessageDialog::warning(this, "音频设置", message);
    }
}

/** @brief 请求 AudioService 测试指定提示音。 */
void FaceGateMainWindow::handleAudioTestPlay(const QString &promptKey)
{
    audio_.configure(config_);
    audio_.testPlay(promptKey);
}

/** @brief 请求数据库更新人员编号并写审计日志。 */
void FaceGateMainWindow::handlePersonNoChange(qint64 personId, const QString &newPersonNo)
{
    writeOperatorAudit("修改人员编号", "person", QString::number(personId), "请求", newPersonNo);
    if (databaseWorker_) {
        QMetaObject::invokeMethod(
            databaseWorker_,
            "updatePersonNo",
            Qt::QueuedConnection,
            Q_ARG(qint64, personId),
            Q_ARG(QString, newPersonNo));
    }
}

/** @brief 请求数据库切换人员启用状态并写审计日志。 */
void FaceGateMainWindow::handlePersonEnabledChange(qint64 personId, bool enabled)
{
    writeOperatorAudit("修改人员权限", "person", QString::number(personId), "请求", enabled ? "启用" : "禁用");
    if (databaseWorker_) {
        QMetaObject::invokeMethod(
            databaseWorker_,
            "updatePersonEnabled",
            Qt::QueuedConnection,
            Q_ARG(qint64, personId),
            Q_ARG(bool, enabled));
    }
}

/** @brief 请求数据库软删除人员并写审计日志。 */
void FaceGateMainWindow::handlePersonDelete(qint64 personId)
{
    writeOperatorAudit("删除人员", "person", QString::number(personId), "请求", "软删除");
    if (databaseWorker_) {
        QMetaObject::invokeMethod(
            databaseWorker_,
            "deletePerson",
            Qt::QueuedConnection,
            Q_ARG(qint64, personId));
    }
}

/**
 * @brief 切换 MiniFASNet 活体检测，同时保持人脸识别运行。
 */
void FaceGateMainWindow::setLivenessEnabled(bool enabled)
{
    livenessEnabled_ = enabled;
    if (faceInferenceWorker_) {
        QMetaObject::invokeMethod(
            faceInferenceWorker_,
            "setLivenessEnabled",
            Qt::QueuedConnection,
            Q_ARG(bool, enabled));
    }
    adminLivenessPending_ = false;
    adminLastLivenessValid_ = false;
    adminLastLivenessResult_ = LivenessResult();
    adminLastLivenessTimer_.invalidate();
    qInfo().noquote() << "活体开关已切换：" << (enabled ? "已启用" : "已关闭");
    livenessLabel_->setText(enabled ? "活体开启" : "活体关闭");
    if (adminPanel_) {
        adminPanel_->setEnrollLivenessStatus(enabled, enabled ? "等待真人人脸" : "活体关闭");
    }
}

/**
 * @brief 保存管理员录入截图、提取特征，并排队写入数据库。
 *
 * 特征提取仍放在 FaceEngine 中，以集中管理 SDK 资源归属；
 * 实际 MySQL 写入投递给 DatabaseWorker 执行。
 */
void FaceGateMainWindow::handleEnrollRequest(const PersonInfo &person, const QImage &image)
{
    if (enrollmentPending_) {
        if (adminPanel_) {
            adminPanel_->setStatusText(QStringLiteral("正在提取人脸特征，请稍候"));
        }
        return;
    }

    if (image.isNull()) {
        qWarning().noquote() << "请求录入时没有当前帧";
        if (adminPanel_) {
            adminPanel_->setStatusText("没有可用画面");
        }
        audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        return;
    }

    if (!faceEngineReady_ || !faceInferenceWorker_) {
        const QString message = "FaceEngine 未就绪";
        qWarning().noquote() << message;
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        AppMessageDialog::warning(this, "录入", message);
        audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        return;
    }

    if (livenessEnabled_) {
        const int liveResultMaxAgeMs = qMax(3000, config_.resultHoldMs);
        const bool autoCapturedWaitingForInfo = adminPanel_ && !adminPanel_->cameraRequired();
        const bool liveFresh =
            adminLastLivenessTimer_.isValid() &&
            (autoCapturedWaitingForInfo || adminLastLivenessTimer_.elapsed() <= liveResultMaxAgeMs);
        const bool livePassed =
            adminLastLivenessValid_ &&
            adminLastLivenessResult_.valid &&
            adminLastLivenessResult_.passed &&
            adminLastLivenessResult_.realScore >= config_.livenessThreshold &&
            liveFresh;

        if (!livePassed) {
            const QString message = QStringLiteral("活体检测失败，禁止录入");
            qWarning().noquote() << message
                                << "valid=" << adminLastLivenessValid_
                                << "fresh=" << liveFresh
                                << "real=" << adminLastLivenessResult_.realScore;
            if (adminPanel_) {
                adminPanel_->setStatusText(message);
            }
            AppMessageDialog::warning(this, QStringLiteral("录入"), message);
            audio_.playPrompt(AudioService::Prompt::LivenessFailed);
            return;
        }
    }

    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.exists("enrollments")) {
        dir.mkdir("enrollments");
    }

    QString safePersonNo;
    for (const QChar ch : person.personNo) {
        safePersonNo.append(ch.isLetterOrNumber() || ch == '_' || ch == '-' ? ch : QChar('_'));
    }
    if (safePersonNo.isEmpty()) {
        safePersonNo = "person";
    }
    const QString fileName = QString("enrollments/%1_%2.jpg")
        .arg(safePersonNo)
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
    const QString imagePath = dir.absoluteFilePath(fileName);

    if (!image.save(imagePath, "JPG", 92)) {
        const QString message = "保存录入截图失败";
        qWarning().noquote() << message << imagePath;
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        AppMessageDialog::warning(this, "录入", message);
        audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        return;
    }

    enrollmentPending_ = true;
    const bool queued = QMetaObject::invokeMethod(
        faceInferenceWorker_,
        "extractEnrollmentFeature",
        Qt::QueuedConnection,
        Q_ARG(PersonInfo, person),
        Q_ARG(QString, imagePath),
        Q_ARG(float, config_.duplicateFaceCosineThreshold));
    if (!queued) {
        enrollmentPending_ = false;
        const QString message = QStringLiteral("提交人脸特征提取任务失败");
        qWarning().noquote() << message << imagePath;
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        AppMessageDialog::warning(this, QStringLiteral("录入"), message);
        audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        QFile::remove(imagePath);
    }
}

/** @brief 处理录入特征、重复人脸检查并写入数据库。 */
void FaceGateMainWindow::handleEnrollmentFeature(const PersonInfo &person,
                                                 const QString &imagePath,
                                                 bool ok,
                                                 const QString &errorText,
                                                 const FaceFeatureData &feature,
                                                 bool duplicate,
                                                 const FaceRecord &similarRecord,
                                                 float duplicateCosine)
{
    enrollmentPending_ = false;

    if (!ok) {
        const QString message = errorText.isEmpty()
            ? QStringLiteral("提取录入特征失败")
            : errorText;
        qWarning().noquote() << "提取录入特征失败：" << message << imagePath;
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        AppMessageDialog::warning(this, QStringLiteral("录入"), message);
        audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        QFile::remove(imagePath);
        return;
    }

    qInfo().noquote() << "录入特征已提取："
                      << person.personNo << person.name
                      << imagePath << feature.blob.size();

    if (duplicate) {
        const QString message = QString("人员已录入，相似人员：%1 %2，相似度=%3")
            .arg(similarRecord.person.personNo)
            .arg(similarRecord.person.name)
            .arg(duplicateCosine, 0, 'f', 3);
        qWarning().noquote() << "拒绝重复录入：" << message;
        if (adminPanel_) {
            adminPanel_->setStatusText(message);
        }
        AppMessageDialog::warning(this, QStringLiteral("录入"), message);
        audio_.playPrompt(AudioService::Prompt::EnrollFailed);
        QFile::remove(imagePath);
        return;
    }

    if (databaseWorker_) {
        QMetaObject::invokeMethod(
            databaseWorker_,
            "addPersonFace",
            Qt::QueuedConnection,
            Q_ARG(PersonInfo, person),
            Q_ARG(FaceFeatureData, feature),
            Q_ARG(QString, imagePath));
    }
}

/** @return VerifyState 对应的中文状态名。 */
QString FaceGateMainWindow::stateName(VerifyState state) const
{
    switch (state) {
    case VerifyState::Idle: return "空闲";
    case VerifyState::CollectingFrames: return "采集中";
    case VerifyState::SelectBestFrame: return "选择中";
    case VerifyState::LivenessChecking: return "活体检测";
    case VerifyState::FeatureMatching: return "匹配中";
    case VerifyState::Passed: return "通过";
    case VerifyState::Failed: return "失败";
    case VerifyState::Cooldown: return "冷却中";
    }
    return "未知";
}
