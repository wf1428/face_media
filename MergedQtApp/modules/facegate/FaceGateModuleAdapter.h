/**
 * @file FaceGateModuleAdapter.h
 * @brief 将人脸门禁主窗口适配为应用外壳统一生命周期接口。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef FACEGATEMODULEADAPTER_H
#define FACEGATEMODULEADAPTER_H

#include "shell/IApplicationModule.h"

#include <QObject>
#include <QPointer>
#include <QString>

class FaceGateMainWindow;
class QWidget;

/** @brief 将人脸门禁主窗口适配为应用外壳统一生命周期接口。 */
class FaceGateModuleAdapter final : public QObject, public IApplicationModule
{
    Q_OBJECT

public:
    /** @brief 保存窗口父对象和配置路径，实际窗口在 initialize() 中创建。 */
    explicit FaceGateModuleAdapter(QWidget *widgetParent,
                                   const QString &configPath,
                                   QObject *parent = nullptr);

    /** @return 门禁主窗口；尚未初始化时返回 nullptr。 */
    QWidget *rootWidget() override;

    /** @brief 创建并初始化门禁主窗口。 */
    bool initialize() override;

    /** @brief 激活摄像头、推理和门禁界面。 */
    void activate() override;

    /** @brief 停止门禁前台活动并保留可恢复资源。 */
    void deactivate() override;

    /** @brief 关闭门禁模块；重复调用保持幂等。 */
    void shutdown() override;

    /** @return 模块已经激活且未关闭时返回 true。 */
    bool isActive() const override;

    /** @return 仅在无人值守的人脸识别主页允许外壳执行在场切换。 */
    bool allowsPresenceSwitch() const override;

signals:
    /** @brief 当门禁界面显示识别成功结果时向应用外壳转发。 */
    void recognitionSucceeded();

    /** @brief 当门禁界面显示最终人脸识别结果时向应用外壳转发。 */
    void recognitionFinished();

    /** @brief 转发主识别画面中的人脸检测状态。 */
    void facePresenceChanged(bool present);

private:
    QPointer<QWidget> widgetParent_;    /**< 门禁窗口的 QWidget 父对象。 */
    QPointer<FaceGateMainWindow> window_; /**< 门禁模块根窗口。 */
    QString configPath_;               /**< FaceGate INI 配置路径。 */
    bool active_ = false;              /**< 当前是否处于前台活动态。 */
    bool shutdown_ = false;            /**< 防止关闭流程重复执行。 */
};

#endif // FACEGATEMODULEADAPTER_H
