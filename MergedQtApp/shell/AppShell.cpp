/**
 * @file AppShell.cpp
 * @brief 初始化应用模块并协调人体感应、识别显示、模式切换和补光灯。
 * @author Du lin
 * @note 最近修改：2026-10-24；修改人：Du lin；启动时读取 net_cfg.ini 的 fillLight/enabled，
 *       缺省启用，禁用时不执行识别及管理状态的开灯动作；保留原模式切换和关灯流程。
 */
#include "AppShell.h"

#include "LedFillLight.h"
#include "ModeController.h"
#include "QuickUiRuntime.h"
#include "Sr505PresenceSensor.h"
#include "modules/facegate/FaceGateModuleAdapter.h"
#include "modules/multimedia/MultimediaModuleAdapter.h"
#include "platform/rk3566_platform.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

AppShell::AppShell(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("appShellRoot"));
    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#appShellRoot { background: black; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("moduleStack"));
    layout->addWidget(stack_);

    // 六层界面统一登记在同一个目录中。当前仍依赖 OpenGL 的两个视频层
    // 明确标记为 NativeOpenGlMode，迁移期间不会被 QQuickWidget 覆盖。
    quickUiRuntime_ = QuickUiRuntime::instance();
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::ApplicationShellModule),
                                     QuickUiRuntime::ApplicationShellLayer,
                                     QuickUiRuntime::ManagedQuickIslandMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::MultimediaMainModule),
                                     QuickUiRuntime::MultimediaLayer,
                                     QuickUiRuntime::NativeOpenGlMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::FaceRecognitionModule),
                                     QuickUiRuntime::FaceRecognitionLayer,
                                     QuickUiRuntime::NativeOpenGlMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::FaceAdministrationModule),
                                     QuickUiRuntime::FaceAdministrationLayer,
                                     QuickUiRuntime::ManagedQuickIslandMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::SystemSettingsModule),
                                     QuickUiRuntime::SystemSettingsLayer,
                                     QuickUiRuntime::ManagedQuickIslandMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::FeaturePagesModule),
                                     QuickUiRuntime::FeaturePageLayer,
                                     QuickUiRuntime::ManagedQuickIslandMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::FileManagerModule),
                                     QuickUiRuntime::FeaturePageLayer,
                                     QuickUiRuntime::ManagedQuickIslandMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::DebugToolsModule),
                                     QuickUiRuntime::FeaturePageLayer,
                                     QuickUiRuntime::ManagedQuickIslandMode,
                                     QUrl(), true});
    quickUiRuntime_->registerModule({QuickUiRuntime::moduleId(QuickUiRuntime::GlobalOverlayModule),
                                     QuickUiRuntime::OverlayLayer,
                                     QuickUiRuntime::QuickOverlayMode,
                                     QUrl(QStringLiteral("qrc:/qml/QuickSceneHost.qml")),
                                     true});
    quickUiRuntime_->synchronizeActiveModule(
                QuickUiRuntime::moduleId(QuickUiRuntime::ApplicationShellModule));
    quickUiRuntime_->attachOverlayHost(this);
}

AppShell::~AppShell()
{
    shutdown();
}

bool AppShell::initialize()
{
    if (initialized_) {
        return true;
    }

    QSettings settings(shellConfigPath(), QSettings::IniFormat);
    QSettings netSettings(Rk3566Platform::netConfigPath(), QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    settings.setIniCodec("UTF-8");
    netSettings.setIniCodec("UTF-8");
#endif
    const QString initialMode =
        settings.value(QStringLiteral("application/initialMode"), QStringLiteral("multimedia"))
            .toString().trimmed().toLower();
    const QString sr505Device =
        settings.value(QStringLiteral("modeSwitch/sr505Device"), QStringLiteral("/dev/sr505"))
            .toString().trimmed();
    const bool sr505ActiveHigh =
        settings.value(QStringLiteral("modeSwitch/sr505ActiveHigh"), true).toBool();
    const int sr505ReconnectMs =
        settings.value(QStringLiteral("modeSwitch/sr505ReconnectMs"), 1000).toInt();
    const int recognitionReturnMs =
        settings.value(QStringLiteral("modeSwitch/recognitionReturnMs"), 20000).toInt();
    const int recognitionImageMs =
        settings.value(QStringLiteral("modeSwitch/recognitionImageMs"), 3000).toInt();
    const QString fillLightDevice =
        settings.value(QStringLiteral("fillLight/device"), QStringLiteral("/dev/led"))
            .toString().trimmed();
    // 仅在初始化时读取功能开关，缺省保留旧行为；配置不写回，修改后重启程序生效。
    const bool fillLightEnabled =
        netSettings.value(QStringLiteral("fillLight/enabled"), true).toBool();
    if (initialMode != QStringLiteral("multimedia")) {
        qWarning() << "[SHELL] unsupported initialMode; forcing multimedia" << initialMode;
    }

    multimediaModule_ = new MultimediaModuleAdapter(stack_, this);
    faceGateModule_ = new FaceGateModuleAdapter(stack_, faceGateConfigPath(), this);
    if (!multimediaModule_->initialize() || !faceGateModule_->initialize()) {
        qCritical() << "[SHELL] module initialization failed";
        return false;
    }

    stack_->addWidget(multimediaModule_->rootWidget());
    stack_->addWidget(faceGateModule_->rootWidget());

    modeController_ = new ModeController(stack_,
                                         multimediaModule_,
                                         faceGateModule_,
                                         recognitionReturnMs,
                                         this);
    presenceSensor_ = new Sr505PresenceSensor(sr505Device,
                                               sr505ActiveHigh,
                                               sr505ReconnectMs,
                                               this);
    fillLight_ = new LedFillLight(fillLightDevice, this);
    presenceReconcileTimer_ = new QTimer(this);
    presenceReconcileTimer_->setInterval(200);

    connect(presenceSensor_, &Sr505PresenceSensor::presenceDetected,
            modeController_, &ModeController::handlePresenceDetected);
    connect(presenceSensor_, &Sr505PresenceSensor::presenceLost,
            modeController_, &ModeController::handlePresenceLost);
    connect(presenceSensor_, &Sr505PresenceSensor::sensorFault,
            this, [](const QString &message) {
                qWarning() << "[PRESENCE] fault" << message;
            });
    connect(faceGateModule_, &FaceGateModuleAdapter::recognitionVerified,
            modeController_, &ModeController::handleRecognitionVerified);
    connect(faceGateModule_, &FaceGateModuleAdapter::recognitionSucceeded,
            modeController_, &ModeController::handleRecognitionSucceeded);
    connect(faceGateModule_, &FaceGateModuleAdapter::recognitionFinished,
            modeController_, &ModeController::handleRecognitionFinished);
    connect(faceGateModule_, &FaceGateModuleAdapter::duplicateRecognitionSuppressed,
            this, [this](const QString &message) {
                if (multimediaModule_) {
                    multimediaModule_->showStatusMessage(message, 2500);
                }
            });
    connect(faceGateModule_, &FaceGateModuleAdapter::facePresenceChanged,
            modeController_, &ModeController::handleFacePresenceChanged);
#ifndef FACEGATE_LEGACY_PAGE_MODE
    connect(faceGateModule_, &FaceGateModuleAdapter::previewFrameChanged,
            multimediaModule_, &MultimediaModuleAdapter::setEmbeddedFaceFrame);
    connect(faceGateModule_, &FaceGateModuleAdapter::previewUnavailable,
            multimediaModule_, &MultimediaModuleAdapter::setEmbeddedFaceUnavailable);
    connect(faceGateModule_, &FaceGateModuleAdapter::previewUnavailable,
            modeController_, &ModeController::handleCameraUnavailable);
    connect(faceGateModule_, &FaceGateModuleAdapter::previewFacesChanged,
            multimediaModule_, &MultimediaModuleAdapter::setEmbeddedFaceFaces);
    connect(faceGateModule_, &FaceGateModuleAdapter::previewStateChanged,
            multimediaModule_, &MultimediaModuleAdapter::setEmbeddedFaceState);
    connect(faceGateModule_, &FaceGateModuleAdapter::recognitionImageReady,
            this, [this, recognitionImageMs](const QImage &image,
                                              const QString &name) {
                if (multimediaModule_) {
                    multimediaModule_->showRecognitionImage(
                                image, name, qMax(1, recognitionImageMs));
                }
            });
    connect(multimediaModule_, &MultimediaModuleAdapter::adminLongPressRequested,
            modeController_, &ModeController::handleAdminRequested);
    connect(multimediaModule_, &MultimediaModuleAdapter::recognitionResultDisplayStarted,
            modeController_, &ModeController::handleRecognitionDisplayStarted);
    connect(faceGateModule_, &FaceGateModuleAdapter::adminSessionFinished,
            modeController_, &ModeController::handleAdminSessionFinished);
    connect(faceGateModule_, &FaceGateModuleAdapter::adminSessionStarted,
            modeController_, &ModeController::handleAdminSessionStarted);
#endif
    connect(presenceReconcileTimer_, &QTimer::timeout, this, [this]() {
        if (presenceSensor_ && modeController_) {
            modeController_->reconcilePresenceLevel(
                presenceSensor_->hasValidLevel(), presenceSensor_->isPresent());
        }
    });
    connect(modeController_, &ModeController::stateChanged,
            this, [this, fillLightEnabled](ModeController::AppModeState state) {
                if (quickUiRuntime_) {
                    switch (state) {
                    case ModeController::AppModeState::MultimediaActive:
                        quickUiRuntime_->synchronizeActiveModule(
                                    QuickUiRuntime::moduleId(QuickUiRuntime::MultimediaMainModule));
                        break;
                    case ModeController::AppModeState::FaceGateActive:
                        quickUiRuntime_->synchronizeActiveModule(
                                    QuickUiRuntime::moduleId(QuickUiRuntime::FaceRecognitionModule));
                        break;
                    case ModeController::AppModeState::AdminActive:
                        quickUiRuntime_->synchronizeActiveModule(
                                    QuickUiRuntime::moduleId(QuickUiRuntime::FaceAdministrationModule));
                        break;
                    case ModeController::AppModeState::ShuttingDown:
                        quickUiRuntime_->synchronizeActiveModule(
                                    QuickUiRuntime::moduleId(QuickUiRuntime::ApplicationShellModule));
                        break;
                    case ModeController::AppModeState::SwitchingToFaceGate:
                    case ModeController::AppModeState::SwitchingToMultimedia:
                        // 过渡状态继续保留当前稳定页面，避免 QML 路由提前切换。
                        break;
                    }
                }
                if (state == ModeController::AppModeState::FaceGateActive
                        || state == ModeController::AppModeState::AdminActive) {
                    if (fillLight_ && fillLightEnabled) {
                        fillLight_->turnOn();
                    }
                } else if (state == ModeController::AppModeState::MultimediaActive
                           && fillLight_) {
                    fillLight_->turnOff();
                }
            });
    connect(modeController_, &ModeController::switchFailed,
            this, [](const QString &message) {
                qWarning() << "[MODE] switch failed" << message;
            });

    // Establish the default multimedia state before reading SR505. This makes
    // the driver's initial HIGH sample immediately perform a valid transition.
    if (!modeController_->start() || !presenceSensor_->start()) {
        shutdown();
        return false;
    }
    presenceReconcileTimer_->start();

    initialized_ = true;
    shutdown_ = false;
    qInfo() << "[SHELL] initialized"
            << "config=" << shellConfigPath()
            << "presenceDriver=sr505"
            << "sr505Device=" << sr505Device
            << "fillLightDevice=" << fillLightDevice
            << "fillLightEnabled=" << fillLightEnabled
            << "recognitionReturnMs=" << recognitionReturnMs
            << "recognitionImageMs=" << recognitionImageMs
            << "facegateConfig=" << faceGateConfigPath();
    return true;
}

void AppShell::shutdown()
{
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    if (presenceSensor_) {
        presenceSensor_->stop();
    }
    if (presenceReconcileTimer_) {
        presenceReconcileTimer_->stop();
    }
    if (fillLight_) {
        fillLight_->turnOff();
    }
    if (modeController_) {
        modeController_->shutdown();
    }
    if (quickUiRuntime_) {
        quickUiRuntime_->dismissOverlays();
        quickUiRuntime_->synchronizeActiveModule(
                    QuickUiRuntime::moduleId(QuickUiRuntime::ApplicationShellModule));
    }
    initialized_ = false;
    qInfo() << "[SHELL] shutdown complete";
}

QWidget *AppShell::multimediaRootWidget() const
{
    return multimediaModule_ ? multimediaModule_->rootWidget() : nullptr;
}

QWidget *AppShell::activeRootWidget() const
{
    return stack_ ? stack_->currentWidget() : nullptr;
}

QString AppShell::shellConfigPath() const
{
    const QByteArray overridePath = qgetenv("APP_SHELL_CONFIG");
    if (!overridePath.trimmed().isEmpty()) {
        return QString::fromLocal8Bit(overridePath);
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString deployed = QDir(appDir).filePath(QStringLiteral("app_shell.ini"));
    if (QFileInfo::exists(deployed)) {
        return deployed;
    }
    const QString appConfig = QDir(appDir).filePath(QStringLiteral("config/app_shell.ini"));
    if (QFileInfo::exists(appConfig)) {
        return appConfig;
    }
    return QDir::current().filePath(QStringLiteral("config/app_shell.ini"));
}

QString AppShell::faceGateConfigPath() const
{
    return Rk3566Platform::faceGateConfigPath();
}
