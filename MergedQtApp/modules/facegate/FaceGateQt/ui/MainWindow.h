/**
 * @file MainWindow.h
 * @brief 管理融合应用页面、功能模块生命周期和跨模块信号路由。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include "AppConfig.h"
#include "AudioService.h"
#include "CameraService.h"
#include "DatabaseWorker.h"
#include "FaceInferenceWorker.h"
#include "GateOutputService.h"
#include "LivenessWorker.h"

class AdminPanel;
class CameraPreviewWidget;
class MaintenanceWorker;
class PersonExportWorker;
class PersonImportWorker;
class QLabel;
class QPushButton;
class QResizeEvent;

/**
 * @brief 1024x768 人脸门禁主窗口及服务协调器。
 *
 * 主线程负责界面、摄像头帧分发、音频和开闸；人脸推理、数据库、维护分别运行在
 * 独立线程。通过单帧 pending 标志实施背压，避免实时帧在推理事件队列中堆积。
 */
class FaceGateMainWindow : public QMainWindow {
    Q_OBJECT

public:
    /** @brief 按配置构建界面并创建服务对象。 */
    explicit FaceGateMainWindow(const AppConfig &config, QWidget *parent = nullptr);

    /** @brief 销毁前执行幂等模块关闭。 */
    ~FaceGateMainWindow() override;

    /** @brief 启动或恢复摄像头、推理和门禁业务。 */
    bool activateModule();

    /** @brief 暂停摄像头和验证，但保留后台服务以便恢复。 */
    void deactivateModule();

    /** @brief 停止所有线程、服务和播放器；重复调用保持幂等。 */
    void shutdownModule();

    /** @return 门禁模块当前活动时返回 true。 */
    bool isModuleActive() const;

    /** @return true on the recognition screen, false during password/admin UI. */
    bool allowsPresenceSwitch() const;

signals:
    /** Emitted after the final access decision succeeds and the result is shown. */
    void recognitionSucceeded();

protected:
    /** @brief 窗口尺寸变化后重新布局左右覆盖面板。 */
    void resizeEvent(QResizeEvent *event) override;

private:
    /** @brief 创建预览、状态栏和覆盖信息面板。 */
    void buildUi();

    /** @brief 启动数据库、推理和周期状态服务。 */
    void startServices();

    /** @brief 创建数据库 worker 线程并连接所有请求/结果信号。 */
    void startDatabaseThread();

    /** @brief 创建人脸推理 worker 线程并连接帧背压信号。 */
    void startFaceInferenceThread();

    /** @brief 在推理线程重置验证状态机。 */
    void resetFaceVerification(const QString &message);

    /** @brief 统计限定时间内的预览点击次数并触发管理员登录。 */
    void handleAdminTap();

    /** @brief 弹出人员通行密码键盘并提交本地权限校验。 */
    void handlePasswordAccess();

    /** @brief 完成管理员认证后打开综合管理面板。 */
    void openAdminPanel();

    /** @brief 保存录入图像并投递后台特征提取。 */
    void handleEnrollRequest(const PersonInfo &person, const QImage &image);

    /** @brief 将管理模式帧投递到推理线程进行检测。 */
    void processAdminFrame(const CameraFrame &frame);

    /** @brief 将管理员检测分析和预览更新到录入面板。 */
    void handleAdminAnalysis(const CameraFrame &frame, const FaceAnalysisResult &analysis);

    /** @brief 处理录入特征、重复人脸检查并写入数据库。 */
    void handleEnrollmentFeature(const PersonInfo &person,
                                 const QString &imagePath,
                                 bool ok,
                                 const QString &errorText,
                                 const FaceFeatureData &feature,
                                 bool duplicate,
                                 const FaceRecord &similarRecord,
                                 float duplicateCosine);

    /** @brief 请求数据库更新人员编号并写审计日志。 */
    void handlePersonNoChange(qint64 personId, const QString &newPersonNo);

    /** @brief 请求数据库切换人员启用状态并写审计日志。 */
    void handlePersonEnabledChange(qint64 personId, bool enabled);

    /** @brief 请求数据库软删除人员并写审计日志。 */
    void handlePersonDelete(qint64 personId);

    /** @brief 请求刷新管理员人员列表。 */
    void refreshAdminPeople();

    /** @brief 请求按条件刷新验证日志。 */
    void refreshAdminPassedLogs(const VerifyLogFilter &filter = VerifyLogFilter());

    /** @brief 请求刷新存储统计。 */
    void refreshAdminStorageStats();

    /** @brief 启动U盘网络人员 XLS/XLSX 后台导入。 */
    void handlePeopleImport();

    /** @brief 将网络、本地或全部人员以固定XLSX格式导出到U盘。 */
    void handlePeopleExport(const QString &personnelScope);

    /** @brief 投递 SQLite 备份维护任务。 */
    void handleMaintenanceBackup();

    /** @brief 投递过期日志和抓拍清理任务。 */
    void handleMaintenanceCleanup(int days);

    /** @brief 校验并持久化五项识别阈值，再同步推理线程。 */
    void handleThresholdConfigSave(float faceCosine, float duplicateFaceCosine, float faceQuality, float faceDetect, float liveness);

    /** @brief 保存 DHCP 或静态网络配置。 */
    void handleNetworkConfigSave(bool dhcp, const QString &ip, const QString &netmask, const QString &gateway, const QString &dns);

    /** @brief 保存音频配置并重新配置 AudioService。 */
    void handleAudioConfigSave(const AppConfig &audioConfig);

    /** @brief 请求 AudioService 测试指定提示音。 */
    void handleAudioTestPlay(const QString &promptKey);

    /** @brief 修改管理员密码并持久化配置。 */
    void handleAdminPasswordChange(const QString &oldPassword, const QString &newPassword);

    /** @return 用户完成或按策略跳过密码修改时返回 true。 */

    /** @brief 异步写入管理员操作审计记录。 */
    void writeOperatorAudit(const QString &action, const QString &targetType, const QString &targetId, const QString &result, const QString &detail);

    /** @brief 异步写入系统事件记录。 */
    void writeSystemEvent(const QString &eventType, const QString &level, const QString &message, const QString &detail = QString());

    /** @brief 同步活体开关到 UI、推理线程和配置。 */
    void setLivenessEnabled(bool enabled);

    /** @brief 刷新日期和时间显示。 */
    void updateClock();

    /** @brief 刷新网络接口摘要。 */
    void updateNetworkSummary();

    /** @brief 按窗口尺寸布局左右覆盖信息面板。 */
    void layoutOverlayPanels();

    /** @brief 根据模块活动态和当前页面需求启停摄像头。 */
    void updateCameraPowerState();

    /** @brief 清除主预览和录入预览中的旧帧。 */
    void clearCameraFrames();

    /** @return 当前验证界面或管理页面需要摄像头时返回 true。 */
    bool currentUiNeedsCamera() const;

    /** @return VerifyState 对应的中文状态名。 */
    QString stateName(VerifyState state) const;

private:
    AppConfig config_;
    CameraPreviewWidget *preview_ = nullptr;
    QLabel *cameraLabel_ = nullptr;
    QLabel *recognitionLabel_ = nullptr;
    QLabel *gateLabel_ = nullptr;
    QLabel *timeLabel_ = nullptr;
    QLabel *dateLabel_ = nullptr;
    QLabel *networkLabel_ = nullptr;
    QLabel *databaseLabel_ = nullptr;
    QLabel *livenessLabel_ = nullptr;
    QLabel *galleryLabel_ = nullptr;
    QLabel *hintLabel_ = nullptr;
    QPushButton *passwordAccessButton_ = nullptr;
    QWidget *leftOverlay_ = nullptr;
    QWidget *rightOverlay_ = nullptr;
    AdminPanel *adminPanel_ = nullptr;
    QImage latestFrame_;
    QImage latestEnrollmentPreview_;
    QVector<FaceRecord> latestGallery_;
    QVector<PersonAdminRecord> latestPeople_;
    QVector<VerifyLogViewRecord> latestPassedLogs_;
    StorageStats latestStorageStats_;
    qint64 lastPassedPersonId_ = 0;       /**< 最近通过人员主键，用于重复抑制。 */
    QString lastPassedPersonNo_;         /**< 最近通过人员编号。 */
    QElapsedTimer lastPassedTimer_;      /**< 同人重复通过抑制计时器。 */
    bool livenessEnabled_ = true;        /**< 当前活体开关。 */
    bool adminLoginActive_ = false;      /**< 管理员密码登录弹窗是否正在处理输入。 */
    bool adminModeActive_ = false;       /**< 管理面板是否打开。 */
    bool adminLivenessPending_ = false;  /**< 是否等待管理员录入活体结果。 */
    int adminLivenessLastFrame_ = -1000000; /**< 最近提交活体的帧序号。 */
    bool adminLastLivenessValid_ = false;   /**< 最近管理员活体结果是否有效。 */
    LivenessResult adminLastLivenessResult_;
    QElapsedTimer adminLastLivenessTimer_;
    int adminTapCount_ = 0;              /**< 管理入口手势当前点击次数。 */
    QElapsedTimer adminTapTimer_;        /**< 管理入口点击时间窗口。 */
    QTimer clockTimer_;
    QTimer networkTimer_;
    QThread faceInferenceThread_;
    FaceInferenceWorker *faceInferenceWorker_ = nullptr;
    QThread databaseThread_;
    DatabaseWorker *databaseWorker_ = nullptr;
    QThread maintenanceThread_;
    MaintenanceWorker *maintenanceWorker_ = nullptr;
    QThread *personImportThread_ = nullptr;
    QPointer<PersonImportWorker> personImportWorker_;
    QThread *personExportThread_ = nullptr;
    QPointer<PersonExportWorker> personExportWorker_;

    CameraService camera_;
    LivenessWorker liveness_;
    GateOutputService gate_;
    AudioService audio_;
    bool faceAccessPending_ = false;
    bool passwordAccessActive_ = false;
    quint64 passwordAccessGeneration_ = 0;
    VerifyLog pendingFaceAccessLog_;
    bool faceEngineReady_ = false;       /**< 推理引擎已经初始化。 */
    bool faceFramePending_ = false;      /**< 已有一帧在推理线程处理中。 */
    bool enrollmentPending_ = false;     /**< 已提交一项录入特征任务。 */
    bool servicesInitialized_ = false;   /**< 后台线程和服务已经启动。 */
    bool moduleActive_ = false;          /**< 模块当前处于前台活动态。 */
    bool shutdown_ = false;              /**< 防止关闭流程重复执行。 */
};

#endif
