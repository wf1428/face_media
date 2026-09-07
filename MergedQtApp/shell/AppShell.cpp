#include "AppShell.h"

#include "LedFillLight.h"
#include "ModeController.h"
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
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    settings.setIniCodec("UTF-8");
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
        settings.value(QStringLiteral("modeSwitch/recognitionReturnMs"), 5000).toInt();
    const QString fillLightDevice =
        settings.value(QStringLiteral("fillLight/device"), QStringLiteral("/dev/led"))
            .toString().trimmed();
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
    connect(faceGateModule_, &FaceGateModuleAdapter::recognitionSucceeded,
            this, [this]() {
                if (fillLight_) {
                    fillLight_->turnOff();
                }
            });
    connect(faceGateModule_, &FaceGateModuleAdapter::recognitionFinished,
            modeController_, &ModeController::handleRecognitionFinished);
    connect(faceGateModule_, &FaceGateModuleAdapter::facePresenceChanged,
            modeController_, &ModeController::handleFacePresenceChanged);
    connect(presenceReconcileTimer_, &QTimer::timeout, this, [this]() {
        if (presenceSensor_ && modeController_) {
            modeController_->reconcilePresenceLevel(
                presenceSensor_->hasValidLevel(), presenceSensor_->isPresent());
        }
    });
    connect(modeController_, &ModeController::stateChanged,
            this, [this](ModeController::AppModeState state) {
                if (state == ModeController::AppModeState::FaceGateActive
                        && fillLight_) {
                    fillLight_->turnOn();
                } else if (state == ModeController::AppModeState::MultimediaActive
                           && fillLight_) {
                    fillLight_->turnOff();
                }
            });
    connect(modeController_, &ModeController::recognitionResumed,
            this, [this]() {
                if (fillLight_) {
                    fillLight_->turnOn();
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
            << "recognitionReturnMs=" << recognitionReturnMs
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
