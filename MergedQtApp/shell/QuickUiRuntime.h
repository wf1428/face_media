/**
 * @file QuickUiRuntime.h
 * @brief Qt Quick 界面的统一运行时、模块目录和覆盖层管理器。
 *
 * 该对象只管理界面呈现，不接管播放器、人脸识别、权限校验或设备通信。
 * 所有 Quick 页面共用一个 QQmlEngine，并通过一个长期存活的覆盖层宿主显示，
 * 避免 Qt 5.12 下频繁创建 QML/EGL 上下文。
 */

#ifndef QUICKUIRUNTIME_H
#define QUICKUIRUNTIME_H

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

class QEvent;
class QEventLoop;
class QQmlEngine;
class QQuickWidget;
class QTimer;
class QWidget;

class QuickUiRuntime final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeModuleId READ activeModuleId NOTIFY activeModuleChanged)
    Q_PROPERTY(int activeLayer READ activeLayer NOTIFY activeModuleChanged)
    Q_PROPERTY(QVariantList moduleCatalog READ moduleCatalog NOTIFY moduleCatalogChanged)
    Q_PROPERTY(bool videoSurfaceProtected READ videoSurfaceProtected NOTIFY activeModuleChanged)
    Q_PROPERTY(bool quickPageActive READ quickPageActive NOTIFY activeModuleChanged)

    Q_PROPERTY(bool dialogVisible READ dialogVisible NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogType READ dialogType NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogTitle READ dialogTitle NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogMessage READ dialogMessage NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogAcceptText READ dialogAcceptText NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogCancelText READ dialogCancelText NOTIFY dialogChanged)
    Q_PROPERTY(QStringList dialogChoices READ dialogChoices NOTIFY dialogChanged)
    Q_PROPERTY(QString dialogIconSource READ dialogIconSource NOTIFY dialogChanged)
    Q_PROPERTY(bool dialogFaceTheme READ dialogFaceTheme NOTIFY dialogChanged)
    Q_PROPERTY(bool toastVisible READ toastVisible NOTIFY toastChanged)
    Q_PROPERTY(QString toastText READ toastText NOTIFY toastChanged)
    Q_PROPERTY(bool toastSuccess READ toastSuccess NOTIFY toastChanged)
    Q_PROPERTY(bool toastFaceTheme READ toastFaceTheme NOTIFY toastChanged)
    Q_PROPERTY(bool progressVisible READ progressVisible NOTIFY progressChanged)
    Q_PROPERTY(QString progressTitle READ progressTitle NOTIFY progressChanged)
    Q_PROPERTY(int progressValue READ progressValue NOTIFY progressChanged)

public:
    /** @brief 与迁移计划一致的六个固定界面层，覆盖层单独管理。 */
    enum UiLayer {
        ApplicationShellLayer = 0,
        MultimediaLayer,
        FaceRecognitionLayer,
        FaceAdministrationLayer,
        SystemSettingsLayer,
        FeaturePageLayer,
        OverlayLayer
    };
    Q_ENUM(UiLayer)

    /** @brief 全部可路由界面的稳定标识，新增模块统一在此登记。 */
    enum UiModule {
        ApplicationShellModule = 0,
        MultimediaMainModule,
        FaceRecognitionModule,
        FaceAdministrationModule,
        SystemSettingsModule,
        FeaturePagesModule,
        FileManagerModule,
        DebugToolsModule,
        GlobalOverlayModule
    };
    Q_ENUM(UiModule)

    /** @brief 决定页面合成策略，视频层继续使用原生 OpenGL surface。 */
    enum RenderMode {
        NativeWidgetMode = 0,
        NativeOpenGlMode,
        ManagedQuickIslandMode,
        QuickOpaqueMode,
        QuickOverlayMode
    };
    Q_ENUM(RenderMode)

    struct ModuleDefinition {
        QString id;
        UiLayer layer = ApplicationShellLayer;
        RenderMode renderMode = NativeWidgetMode;
        QUrl source;
        bool keepAlive = true;
    };

    /** @return 进程内唯一的 Quick 运行时。首次调用后一直存活到 QApplication 退出。 */
    static QuickUiRuntime *instance();

    /** @return 模块枚举对应的稳定字符串 ID。 */
    static QString moduleId(UiModule module);

    /** @return 所有 Quick 控件共享的唯一 QML 引擎。 */
    QQmlEngine *engine() const;

    /** @brief 将唯一覆盖层宿主附着到程序最外层窗口。重复调用保持幂等。 */
    void attachOverlayHost(QWidget *host);

    /** @brief 注册或更新界面模块元数据，供后续页面迁移和扩展统一查询。 */
    bool registerModule(const ModuleDefinition &definition);

    /** @brief 由现有业务状态机同步当前界面，不在此处改变业务状态。 */
    bool synchronizeActiveModule(const QString &moduleId);

    QString activeModuleId() const;
    int activeLayer() const;
    QVariantList moduleCatalog() const;
    bool videoSurfaceProtected() const;
    bool quickPageActive() const;

    bool dialogVisible() const;
    QString dialogType() const;
    QString dialogTitle() const;
    QString dialogMessage() const;
    QString dialogAcceptText() const;
    QString dialogCancelText() const;
    QStringList dialogChoices() const;
    QString dialogIconSource() const;
    bool dialogFaceTheme() const { return dialogFaceTheme_; }
    bool toastVisible() const;
    QString toastText() const;
    bool toastSuccess() const;
    bool toastFaceTheme() const { return toastFaceTheme_; }
    bool progressVisible() const;
    QString progressTitle() const;
    int progressValue() const;

    /** @return 覆盖层宿主已安装且可显示时返回 true。 */
    bool canPresentOverlay() const;

    /**
     * @brief 使用统一 QML 覆盖层显示阻塞式消息框。
     * @return 确认或选中选项时返回 true；关闭、取消时返回 false。
     */
    bool execDialog(const QString &type,
                    const QString &title,
                    const QString &message,
                    const QString &acceptText,
                    const QString &cancelText,
                    const QStringList &choices = QStringList(),
                    int *selectedChoice = nullptr,
                    bool faceTheme = false);

    /** @brief 在统一覆盖层显示短时 Toast；不可显示时返回 false。 */
    Q_INVOKABLE bool showToast(const QString &text,
                               int durationMs = 2000,
                               bool success = true,
                               bool faceTheme = false);

    /** @brief 显示或更新阻塞式进度覆盖层。 */
    Q_INVOKABLE bool showProgress(const QString &title, int value);
    Q_INVOKABLE void hideProgress();

    /** @brief 隐藏所有 Quick 覆盖层，应用退出时调用。 */
    void dismissOverlays();

    /** @brief QML 确认按钮或选项按钮的统一回调。 */
    Q_INVOKABLE void acceptDialog(int choiceIndex = -1);

    /** @brief QML 取消按钮、关闭按钮和遮罩层的统一回调。 */
    Q_INVOKABLE void rejectDialog();

signals:
    void activeModuleChanged();
    void moduleCatalogChanged();
    void dialogChanged();
    void toastChanged();
    void progressChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit QuickUiRuntime(QObject *parent = nullptr);
    ~QuickUiRuntime() override;

    void updateOverlayGeometry();
    void updateHostVisibility();
    void finishDialog(bool accepted, int choiceIndex);
    int moduleIndex(const QString &moduleId) const;
    QString iconSourceForType(const QString &type) const;

    QQmlEngine *engine_ = nullptr;
    QPointer<QWidget> overlayParent_;
    QPointer<QWidget> previousFocus_;
    QPointer<QQuickWidget> overlayHost_;
    QList<ModuleDefinition> modules_;
    QString activeModuleId_;

    bool dialogVisible_ = false;
    bool dialogFaceTheme_ = false;
    QString dialogType_;
    QString dialogTitle_;
    QString dialogMessage_;
    QString dialogAcceptText_;
    QString dialogCancelText_;
    QStringList dialogChoices_;
    int dialogChoice_ = -1;
    bool dialogAccepted_ = false;
    QEventLoop *dialogLoop_ = nullptr;
    QTimer *toastTimer_ = nullptr;
    bool toastVisible_ = false;
    QString toastText_;
    bool toastSuccess_ = true;
    bool toastFaceTheme_ = false;
    bool progressVisible_ = false;
    QString progressTitle_;
    int progressValue_ = 0;
};

#endif // QUICKUIRUNTIME_H
