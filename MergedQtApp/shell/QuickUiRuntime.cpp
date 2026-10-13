/**
 * @file QuickUiRuntime.cpp
 * @brief Qt Quick 统一运行时与覆盖层实现。
 */

#include "QuickUiRuntime.h"

#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QEvent>
#include <QEventLoop>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QThread>
#include <QTimer>
#include <QWidget>

namespace {
const char kQuickHostSource[] = "qrc:/qml/QuickSceneHost.qml";
}

QuickUiRuntime *QuickUiRuntime::instance()
{
    static QuickUiRuntime *runtime = nullptr;
    if (!runtime) {
        runtime = new QuickUiRuntime(qApp);
    }
    return runtime;
}

QString QuickUiRuntime::moduleId(UiModule module)
{
    switch (module) {
    case ApplicationShellModule: return QStringLiteral("application.shell");
    case MultimediaMainModule: return QStringLiteral("multimedia.main");
    case FaceRecognitionModule: return QStringLiteral("face.recognition");
    case FaceAdministrationModule: return QStringLiteral("face.administration");
    case SystemSettingsModule: return QStringLiteral("system.settings");
    case FeaturePagesModule: return QStringLiteral("feature.pages");
    case FileManagerModule: return QStringLiteral("file.manager");
    case DebugToolsModule: return QStringLiteral("debug.tools");
    case GlobalOverlayModule: return QStringLiteral("overlay.global");
    }
    return QString();
}

QuickUiRuntime::QuickUiRuntime(QObject *parent)
    : QObject(parent), engine_(new QQmlEngine(this))
{
    // uiRuntime 是所有迁移页面唯一的路由和覆盖层入口。业务对象仍由各模块
    // 自己持有，不直接暴露给 QML，避免界面层绕过原有 C++ 状态机。
    engine_->rootContext()->setContextProperty(QStringLiteral("uiRuntime"), this);
    connect(engine_, &QQmlEngine::warnings, this,
            [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings) {
            qWarning().noquote() << "[QUICK-UI]" << warning.toString();
        }
    });
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, this, [this]() {
        if (!toastVisible_) {
            return;
        }
        toastVisible_ = false;
        emit toastChanged();
        updateHostVisibility();
    });
}

QuickUiRuntime::~QuickUiRuntime()
{
    dismissOverlays();
    if (overlayParent_) {
        overlayParent_->removeEventFilter(this);
    }
}

QQmlEngine *QuickUiRuntime::engine() const
{
    return engine_;
}

void QuickUiRuntime::attachOverlayHost(QWidget *host)
{
    if (!host || overlayParent_ == host) {
        return;
    }
    if (overlayHost_) {
        qWarning() << "[QUICK-UI] overlay host is already attached";
        return;
    }

    overlayParent_ = host;
    overlayParent_->installEventFilter(this);

    overlayHost_ = new QQuickWidget(engine_, host);
    overlayHost_->setObjectName(QStringLiteral("unifiedQuickOverlayHost"));
    overlayHost_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    overlayHost_->setClearColor(Qt::transparent);
    overlayHost_->setAttribute(Qt::WA_AlwaysStackOnTop, true);
    overlayHost_->setAttribute(Qt::WA_AcceptTouchEvents, true);
    overlayHost_->setFocusPolicy(Qt::StrongFocus);
    overlayHost_->installEventFilter(this);
    connect(overlayHost_, &QObject::destroyed, this, [this]() {
        dismissOverlays();
    });
    connect(overlayHost_, &QQuickWidget::statusChanged, this,
            [this](QQuickWidget::Status status) {
        if (status == QQuickWidget::Ready) {
            updateOverlayGeometry();
        } else if (status == QQuickWidget::Error) {
            // 失去可用场景时必须释放模态等待和输入遮罩。
            dismissOverlays();
        }
    });
    updateOverlayGeometry();
    overlayHost_->setSource(QUrl(QString::fromLatin1(kQuickHostSource)));
    updateOverlayGeometry();
    if (QQuickWindow *window = overlayHost_->quickWindow()) {
        window->setPersistentOpenGLContext(true);
        window->setPersistentSceneGraph(true);
    }
    overlayHost_->hide();

    if (overlayHost_->status() == QQuickWidget::Error) {
        const QList<QQmlError> errors = overlayHost_->errors();
        for (const QQmlError &error : errors) {
            qWarning().noquote() << "[QUICK-UI] overlay load failed:" << error.toString();
        }
    } else {
        qInfo() << "[QUICK-UI] persistent overlay host ready";
    }
}

bool QuickUiRuntime::registerModule(const ModuleDefinition &definition)
{
    const QString id = definition.id.trimmed();
    if (id.isEmpty()) {
        return false;
    }

    ModuleDefinition normalized = definition;
    normalized.id = id;
    const int existing = moduleIndex(id);
    if (existing >= 0) {
        modules_[existing] = normalized;
    } else {
        modules_.append(normalized);
    }
    emit moduleCatalogChanged();
    updateHostVisibility();
    return true;
}

bool QuickUiRuntime::synchronizeActiveModule(const QString &moduleId)
{
    const QString normalized = moduleId.trimmed();
    if (moduleIndex(normalized) < 0) {
        qWarning() << "[QUICK-UI] refusing unknown module" << normalized;
        return false;
    }
    if (activeModuleId_ == normalized) {
        return true;
    }
    activeModuleId_ = normalized;
    emit activeModuleChanged();
    updateHostVisibility();
    return true;
}

QString QuickUiRuntime::activeModuleId() const
{
    return activeModuleId_;
}

int QuickUiRuntime::activeLayer() const
{
    const int index = moduleIndex(activeModuleId_);
    return index >= 0 ? static_cast<int>(modules_.at(index).layer)
                      : static_cast<int>(ApplicationShellLayer);
}

QVariantList QuickUiRuntime::moduleCatalog() const
{
    QVariantList catalog;
    catalog.reserve(modules_.size());
    for (const ModuleDefinition &module : modules_) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), module.id);
        item.insert(QStringLiteral("layer"), static_cast<int>(module.layer));
        item.insert(QStringLiteral("renderMode"), static_cast<int>(module.renderMode));
        item.insert(QStringLiteral("source"), module.source);
        item.insert(QStringLiteral("keepAlive"), module.keepAlive);
        catalog.append(item);
    }
    return catalog;
}

bool QuickUiRuntime::videoSurfaceProtected() const
{
    const int index = moduleIndex(activeModuleId_);
    return index >= 0 && modules_.at(index).renderMode == NativeOpenGlMode;
}

bool QuickUiRuntime::quickPageActive() const
{
    const int index = moduleIndex(activeModuleId_);
    if (index < 0) {
        return false;
    }
    const ModuleDefinition &module = modules_.at(index);
    // QuickOverlayMode 由弹窗/覆盖层栈按需唤起，不作为整页路由显示；只有
    // 不与视频叠加的 QuickOpaqueMode 才能让统一宿主常驻前台。
    return !module.source.isEmpty() && module.renderMode == QuickOpaqueMode;
}

bool QuickUiRuntime::dialogVisible() const { return dialogVisible_; }
QString QuickUiRuntime::dialogType() const { return dialogType_; }
QString QuickUiRuntime::dialogTitle() const { return dialogTitle_; }
QString QuickUiRuntime::dialogMessage() const { return dialogMessage_; }
QString QuickUiRuntime::dialogAcceptText() const { return dialogAcceptText_; }
QString QuickUiRuntime::dialogCancelText() const { return dialogCancelText_; }
QStringList QuickUiRuntime::dialogChoices() const { return dialogChoices_; }
QString QuickUiRuntime::dialogIconSource() const { return iconSourceForType(dialogType_); }
bool QuickUiRuntime::toastVisible() const { return toastVisible_; }
QString QuickUiRuntime::toastText() const { return toastText_; }
bool QuickUiRuntime::toastSuccess() const { return toastSuccess_; }
bool QuickUiRuntime::progressVisible() const { return progressVisible_; }
QString QuickUiRuntime::progressTitle() const { return progressTitle_; }
int QuickUiRuntime::progressValue() const { return progressValue_; }

bool QuickUiRuntime::canPresentOverlay() const
{
    QWidget *modal = QApplication::activeModalWidget();
    if (modal && overlayHost_ && !modal->isAncestorOf(overlayHost_)) {
        // 原生模态窗口会屏蔽外壳的输入，此时沿用调用方的 QWidget 弹窗。
        return false;
    }
    // Ready 只代表 QML 编译成功，不能据此认定卡片有可见面积。
    return overlayParent_ && overlayParent_->isVisible() && overlayHost_ &&
            overlayParent_->width() > 0 && overlayParent_->height() > 0 &&
            overlayHost_->status() == QQuickWidget::Ready &&
            overlayHost_->rootObject() && !dialogLoop_;
}

bool QuickUiRuntime::execDialog(const QString &type,
                                const QString &title,
                                const QString &message,
                                const QString &acceptText,
                                const QString &cancelText,
                                const QStringList &choices,
                                int *selectedChoice,
                                bool faceTheme)
{
    if (!canPresentOverlay() || QThread::currentThread() != thread() || dialogLoop_) {
        return false;
    }

    updateOverlayGeometry();
    QQuickItem *root = overlayHost_->rootObject();
    if (!root || root->width() <= 0 || root->height() <= 0) {
        qWarning() << "[QUICK-UI] refusing zero-sized dialog surface";
        return false;
    }
    QEventLoop loop;
    dialogLoop_ = &loop;
    previousFocus_ = QApplication::focusWidget();
    dialogFaceTheme_ = faceTheme;
    dialogType_ = type.trimmed().toLower();
    dialogTitle_ = title;
    dialogMessage_ = message;
    dialogAcceptText_ = acceptText;
    dialogCancelText_ = cancelText;
    dialogChoices_ = choices;
    dialogChoice_ = -1;
    dialogAccepted_ = false;
    dialogVisible_ = true;
    emit dialogChanged();

    // Toast 单独显示时宿主会穿透鼠标；切换到模态弹窗必须恢复输入，
    // 否则恰好与 Toast 重叠出现的确认框将无法点击。
    overlayHost_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    overlayHost_->show();
    overlayHost_->raise();
    overlayHost_->setFocus(Qt::OtherFocusReason);

    // show/raise 可能同步触发宿主关闭；已经结束时不能再进入等待循环。
    if (dialogVisible_) loop.exec(QEventLoop::AllEvents);
    dialogLoop_ = nullptr;

    if (selectedChoice) {
        *selectedChoice = dialogChoice_;
    }
    return dialogAccepted_;
}

bool QuickUiRuntime::showToast(const QString &text,
                               int durationMs,
                               bool success,
                               bool faceTheme)
{
    if (!overlayParent_ || !overlayHost_ ||
            overlayHost_->status() != QQuickWidget::Ready || dialogLoop_) {
        return false;
    }
    toastFaceTheme_ = faceTheme;
    toastText_ = text;
    toastSuccess_ = success;
    toastVisible_ = true;
    emit toastChanged();
    updateHostVisibility();
    toastTimer_->start(qMax(1, durationMs));
    return true;
}

bool QuickUiRuntime::showProgress(const QString &title, int value)
{
    if (!overlayParent_ || !overlayHost_ ||
            overlayHost_->status() != QQuickWidget::Ready || dialogLoop_) {
        return false;
    }
    progressTitle_ = title;
    progressValue_ = qBound(0, value, 100);
    progressVisible_ = true;
    emit progressChanged();
    updateHostVisibility();
    return true;
}

void QuickUiRuntime::hideProgress()
{
    if (!progressVisible_) {
        return;
    }
    progressVisible_ = false;
    emit progressChanged();
    updateHostVisibility();
}

void QuickUiRuntime::dismissOverlays()
{
    if (toastTimer_) {
        toastTimer_->stop();
    }
    if (toastVisible_) {
        toastVisible_ = false;
        emit toastChanged();
    }
    if (progressVisible_) {
        progressVisible_ = false;
        emit progressChanged();
    }
    if (dialogVisible_ || dialogLoop_) {
        finishDialog(false, -1);
    } else {
        updateHostVisibility();
    }
}

void QuickUiRuntime::acceptDialog(int choiceIndex)
{
    finishDialog(true, choiceIndex);
}

void QuickUiRuntime::rejectDialog()
{
    finishDialog(false, -1);
}

bool QuickUiRuntime::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == overlayHost_ && event) {
        if (event->type() == QEvent::Hide && dialogVisible_) {
            finishDialog(false, -1);
        } else if (event->type() == QEvent::KeyPress && dialogVisible_ &&
                   static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            rejectDialog();
            return true;
        }
    }
    if (watched == overlayParent_ && event) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            updateOverlayGeometry();
            if (dialogVisible_ && overlayHost_) {
                overlayHost_->raise();
            }
        } else if (event->type() == QEvent::Hide) {
            dismissOverlays();
        }
    }
    return QObject::eventFilter(watched, event);
}

void QuickUiRuntime::updateOverlayGeometry()
{
    if (overlayParent_ && overlayHost_) {
        overlayHost_->setGeometry(overlayParent_->rect());
        // 隐藏的 QQuickWidget 在 Qt 5.12 下可能尚未处理 resizeEvent。
        // 根节点由宿主单向赋值，不再与 anchors.fill 的尺寸绑定竞争。
        if (QQuickItem *root = overlayHost_->rootObject()) {
            root->setSize(QSizeF(overlayHost_->size()));
        }
    }
}

void QuickUiRuntime::updateHostVisibility()
{
    if (!overlayHost_) {
        return;
    }
    const bool required = dialogVisible_ || toastVisible_ ||
            progressVisible_ || quickPageActive();
    if (required) {
        overlayHost_->setAttribute(
                    Qt::WA_TransparentForMouseEvents,
                    toastVisible_ && !dialogVisible_ && !progressVisible_ &&
                    !quickPageActive());
        updateOverlayGeometry();
        overlayHost_->show();
        overlayHost_->raise();
    } else {
        overlayHost_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        overlayHost_->hide();
    }
}

void QuickUiRuntime::finishDialog(bool accepted, int choiceIndex)
{
    if (!dialogVisible_ && !dialogLoop_) {
        return;
    }
    dialogAccepted_ = accepted;
    dialogChoice_ = accepted ? choiceIndex : -1;
    dialogVisible_ = false;
    emit dialogChanged();
    updateHostVisibility();
    if (previousFocus_ && previousFocus_->isVisible() && previousFocus_->isEnabled()) {
        previousFocus_->setFocus(Qt::OtherFocusReason);
    }
    previousFocus_.clear();
    if (dialogLoop_ && dialogLoop_->isRunning()) {
        dialogLoop_->exit(accepted ? QDialog::Accepted : QDialog::Rejected);
    }
}

int QuickUiRuntime::moduleIndex(const QString &moduleId) const
{
    for (int index = 0; index < modules_.size(); ++index) {
        if (modules_.at(index).id == moduleId) {
            return index;
        }
    }
    return -1;
}

QString QuickUiRuntime::iconSourceForType(const QString &type) const
{
    const QString normalized = type.trimmed().toLower();
    if (normalized == QLatin1String("success") ||
            normalized == QLatin1String("warning") ||
            normalized == QLatin1String("error") ||
            normalized == QLatin1String("question")) {
        return QStringLiteral("qrc:/icons/dialog/%1.svg").arg(normalized);
    }
    return QStringLiteral("qrc:/icons/dialog/info.svg");
}
