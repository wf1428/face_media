/**
 * @file MultimediaModuleAdapter.h
 * @brief 将原多媒体 MainWindow 适配为应用外壳统一生命周期接口。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MULTIMEDIAMODULEADAPTER_H
#define MULTIMEDIAMODULEADAPTER_H

#include "shell/IApplicationModule.h"
#include "modules/facegate/FaceGateQt/core/VerificationTypes.h"

#include <QImage>
#include <QObject>
#include <QPointer>

class MainWindow;
class MultimediaDemo;
class QWidget;

/** @brief 将原多媒体 MainWindow 适配为应用外壳统一生命周期接口。 */
class MultimediaModuleAdapter final : public QObject, public IApplicationModule
{
    Q_OBJECT

public:
    /** @brief 保存窗口父对象，实际多媒体窗口在 initialize() 中创建。 */
    explicit MultimediaModuleAdapter(QWidget *widgetParent, QObject *parent = nullptr);

    /** @return 多媒体主窗口；尚未初始化时返回 nullptr。 */
    QWidget *rootWidget() override;

    /** @brief 创建多媒体主窗口并建立初始活动状态。 */
    bool initialize() override;

    /** @brief 恢复多媒体模块前台播放。 */
    void activate() override;

    /** @brief 停止多媒体播放并标记模块停用。 */
    void deactivate() override;

    /** @brief 关闭多媒体模块；重复调用保持幂等。 */
    void shutdown() override;

    /** @return 多媒体模块当前活动时返回 true。 */
    bool isActive() const override;

    /** @return 仅在多媒体主页面允许外壳执行在场切换。 */
    bool allowsPresenceSwitch() const override;

    void beginEmbeddedFaceRecognition();
    void endEmbeddedFaceRecognition();
    void setEmbeddedFaceFrame(const QImage &image);
    void setEmbeddedFaceFaces(const QVector<DetectedFace> &faces);
    void setEmbeddedFaceState(VerifyState state, const QString &message);
    void setEmbeddedFaceUnavailable(const QString &message);
    void showRecognitionImage(const QImage &image, const QString &name, int durationMs);

signals:
    /** @brief 转发多媒体页面五秒长按管理入口。 */
    void adminLongPressRequested();

    /** @brief 转发等待图片新批次开始显示的通知。 */
    void recognitionResultDisplayStarted();

private:
    QPointer<QWidget> widgetParent_; /**< 多媒体窗口的 QWidget 父对象。 */
    QPointer<MainWindow> window_;    /**< 多媒体模块根窗口。 */
    QPointer<MultimediaDemo> multimediaPage_; /**< 实际多媒体主页面。 */
    bool active_ = false;           /**< 当前是否处于前台活动态。 */
    bool shutdown_ = false;         /**< 防止关闭流程重复执行。 */
};

#endif // MULTIMEDIAMODULEADAPTER_H
