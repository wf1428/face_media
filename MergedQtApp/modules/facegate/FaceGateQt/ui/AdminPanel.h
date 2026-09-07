/**
 * @file AdminPanel.h
 * @brief 人脸门禁管理员综合面板。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef ADMIN_PANEL_H
#define ADMIN_PANEL_H

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QString>
#include <QThread>
#include <QVector>

#include "AppConfig.h"
#include "VerificationTypes.h"

class FaceEnrollWidget;
class QDateTimeEdit;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSlider;
class QTableWidget;
class QTabWidget;
class QTimer;
class QQuickWidget;
class QWidget;
class QHideEvent;
class QShowEvent;

/** @brief 后台线程采集的一次设备运行状态快照。 */
struct DeviceStatusSnapshot {
    QString serialNumber;
    QString ipAddress;
    QString macAddress;
    QString diskCapacity;
    QString diskRootPath;
    QString cpuUsage;
    QString memoryUsage;
    QString temperature;
    QString npuUsage;
};

/**
 * @brief 人脸门禁管理员综合面板。
 *
 * 组织录入、人员、日志、同步、维护、设备信息和设置页面；界面只收集输入并发出请求，
 * 数据库、维护和硬件操作由主窗口投递到对应后台线程。
 */
class AdminPanel : public QDialog {
    Q_OBJECT

public:
    /** @brief 按当前配置创建导航、内容页和内嵌虚拟键盘。 */
    explicit AdminPanel(const AppConfig &config, QWidget *parent = nullptr);

    /** @brief 停止设备状态采集线程。 */
    ~AdminPanel() override;

    /** @brief 更新录入页面的最新原图和预览图。 */
    void setCurrentFrame(const QImage &image, const QImage &previewImage);

    /** @brief 清除录入预览。 */
    void clearEnrollPreview();

    /** @brief 在录入预览区域显示摄像头异常提示。 */
    void setCameraUnavailableMessage(const QString &message);

    /** @brief 更新录入页面的人脸分析结果。 */
    void setEnrollAnalysis(const QImage &image,
                           const QImage &previewImage,
                           const QVector<DetectedFace> &faces,
                           const QString &message);

    /** @brief 更新录入活体功能状态。 */
    void setEnrollLivenessStatus(bool enabled, const QString &message);

    /** @brief 更新录入活体结果。 */
    void setEnrollLivenessResult(bool enabled, const LivenessResult &result, const QString &message);

    /** @brief 更新面板缓存的人脸图库。 */
    void setGallery(const QVector<FaceRecord> &records);

    /** @brief 填充人员管理表格。 */
    void setPeople(const QVector<PersonAdminRecord> &records);
    void setNetworkPeople(const QJsonArray &records);

    /** @brief 填充验证日志表格。 */
    void setPassedVerifyLogs(const QVector<VerifyLogViewRecord> &records);

    /** @brief 填充同步任务表格。 */
    void setSyncTasks(const QVector<SyncTaskRecord> &records);

    /** @brief 填充系统事件表格。 */
    void setSystemEvents(const QVector<SystemEventLog> &records);

    /** @brief 更新人员、特征、日志和磁盘容量摘要。 */
    void setStorageStats(const StorageStats &stats);

    /** @brief 同步活体复选框和录入页面状态。 */
    void setLivenessEnabled(bool enabled);

    /** @brief 更新面板全局状态文本。 */
    void setStatusText(const QString &text);
    void showNetworkSyncStatus(bool ok, const QString &text);

    /** @brief 切换U盘人员导入按钮和状态提示。 */
    void setPersonImportBusy(bool busy, const QString &message = QString());

    /** @brief 切换U盘人员导出按钮和状态提示。 */
    void setPersonExportBusy(bool busy, const QString &message = QString());

    /** @brief 切换到网络人员列表页。 */
    void showNetworkPeoplePage();

    /** @return 当前选中页面或录入模式需要摄像头时返回 true。 */
    bool cameraRequired() const;

    /** @return 人员表格是否应包含软删除记录。 */
    bool includeDeletedPeople() const;

signals:
    /** @brief 对话框关闭通知。 */
    void closed();

    /** @brief 提交一条人员录入请求。 */
    void enrollRequested(const PersonInfo &person, const QImage &image);

    /** @brief 活体开关修改请求。 */
    void livenessEnabledChanged(bool enabled);

    /** @brief 人员列表刷新请求。 */
    void peopleRefreshRequested();
    void networkPeopleRefreshRequested();

    /** @brief 从U盘导入网络人员 XLS/XLSX 的请求。 */
    void peopleImportRequested();

    /** @brief 导出人员XLSX；范围为 network、local 或 all。 */
    void peopleExportRequested(const QString &personnelScope);

    /** @brief 验证日志筛选刷新请求。 */
    void passedLogsRefreshRequested(const VerifyLogFilter &filter);

    /** @brief 同步任务刷新请求。 */
    void syncTasksRefreshRequested();

    /** @brief 设备信息刷新请求。 */
    void deviceInfoRefreshRequested();

    /** @brief 数据库备份请求。 */
    void maintenanceBackupRequested();

    /** @brief 按保留天数清理请求。 */
    void maintenanceCleanupRequested(int days);

    /** @brief 五项识别阈值保存请求。 */
    void thresholdConfigSaveRequested(float faceCosine, float duplicateFaceCosine, float faceQuality, float faceDetect, float liveness);

    /** @brief DHCP 或静态网络配置保存请求。 */
    void networkConfigSaveRequested(bool dhcp, const QString &ip, const QString &netmask, const QString &gateway, const QString &dns);

    /** @brief 音频配置保存请求。 */
    void audioConfigSaveRequested(const AppConfig &audioConfig);

    /** @brief 指定提示音测试播放请求。 */
    void audioTestPlayRequested(const QString &promptKey);

    /** @brief 管理员密码修改请求。 */
    void adminPasswordChangeRequested(const QString &oldPassword, const QString &newPassword);

    /** @brief 人员编号修改请求。 */
    void personNoChangeRequested(qint64 personId, const QString &newPersonNo);

    /** @brief 人员启用状态修改请求。 */
    void personEnabledChangeRequested(qint64 personId, bool enabled);

    /** @brief 人员软删除请求。 */
    void personDeleteRequested(qint64 personId);

    /** @brief 页面切换导致摄像头需求变化时发出。 */
    void cameraRequiredChanged(bool required);

protected:
    /** @brief 管理文本焦点、内嵌键盘点击和键盘可见性。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 关闭时发出 closed() 并继续基类关闭流程。 */
    void closeEvent(QCloseEvent *event) override;

    /** @brief 面板显示后按当前页面启停设备状态刷新。 */
    void showEvent(QShowEvent *event) override;

    /** @brief 面板隐藏时停止设备状态刷新。 */
    void hideEvent(QHideEvent *event) override;

private:
    /** @return 人员录入和管理组合页面。 */
    QWidget *createPersonManagementPage();

    /** @return 人员列表页面。 */
    QWidget *createPeoplePage();

    /** @return 网络人员与已删除人员切换页面。 */
    QWidget *createNetworkPeoplePage();

    /** @brief 按当前删除状态筛选并重建网络人员表格。 */
    void refreshNetworkPeopleTable();

    /** @brief 切换正常/已删除网络人员视图并刷新表格。 */
    void setNetworkDeletedView(bool deleted);

    /** @brief 显示指定表格行对应人员的全部人脸原图。 */
    void showNetworkPersonFaces(int row);

    /** @brief 将网络同步提示条定位到网络人员页底部中央。 */
    void positionNetworkSyncToast();

    /** @return 验证日志查询页面。 */
    QWidget *createLogPage();

    /** @return 同步任务与系统事件页面。 */
    QWidget *createSyncPage();

    /** @return 备份和清理维护页面。 */
    QWidget *createMaintenancePage();

    /** @return 设备资源与运行状态页面。 */
    QWidget *createDeviceInfoPage();

    /** @return 阈值、网络、音频和密码设置页面。 */
    QWidget *createSettingsPage();

    /** @brief 校验选择后请求修改人员编号。 */
    void requestModifySelectedPersonNo();

    /** @brief 确认后请求删除选中人员。 */
    void requestDeleteSelectedPerson();

    /** @brief 请求切换选中人员启用状态。 */
    void requestToggleSelectedPersonEnabled();

    /** @brief 提示插入U盘并发出人员表导入请求。 */
    void requestImportPeopleFromUsb();

    /** @brief 提示插入U盘、选择人员范围并发出XLSX导出请求。 */
    void requestExportPeopleToUsb();

    /** @brief 将选中人员信息带入录入页重新采集。 */
    void requestReEnrollSelectedPerson();

    /** @brief 收集筛选控件并请求查询验证日志。 */
    void requestQueryVerifyLogs();

    /** @brief 将阈值滑块值转换为浮点配置并请求保存。 */
    void requestSaveThresholdConfig();

    /** @brief 校验网络输入并请求保存。 */
    void requestSaveNetworkConfig();

    /** @brief 同步音频编辑器后请求保存配置。 */
    void requestSaveAudioConfig();

    /** @brief 把音频控件内容写入 config_ 快照。 */
    void syncAudioEditorsToConfig();

    /** @brief 请求测试指定音频提示。 */
    void requestTestAudioPrompt(const QString &promptKey);

    /** @brief 打开统一密码弹窗并请求修改管理员密码。 */
    void requestAdminPasswordChange();

    /** @brief 将识别和活体阈值恢复为内置默认值。 */
    void resetThresholdDefaults();

    /** @brief 请求将日期时间编辑器值应用到系统。 */
    void requestApplyDateTime();

    /** @brief 根据 DHCP/静态单选状态启用网络输入框。 */
    void updateNetworkEditorsEnabled();

    /** @brief 刷新序列号、资源占用、温度和容量等设备信息。 */
    void updateDeviceInfo();

    /** @brief 只更新无需系统采样的版本、运行时间和数据库容量信息。 */
    void updateDeviceSummaryLabels();

    /** @brief 根据页面和面板可见性启停设备状态定时刷新。 */
    void updateDeviceStatusRefreshState();

    /** @brief 向后台采集线程投递一次状态读取请求。 */
    void requestDeviceStatusRefresh();

    /** @brief 在 GUI 线程把后台快照更新到标签。 */
    void applyDeviceStatusSnapshot(const DeviceStatusSnapshot &snapshot);

    /** @brief 根据当前文本焦点和点击对象更新虚拟键盘。 */
    void updateEmbeddedKeyboardVisibility();

    /** @brief 隐藏内嵌虚拟键盘。 */
    void hideEmbeddedKeyboard();

    /** @brief 重新计算当前页面摄像头需求并在变化时发信号。 */
    void updateCameraRequirement();

    /** @return widget 属于内嵌键盘对象树时返回 true。 */
    bool clickedWidgetIsEmbeddedKeyboard(QWidget *widget) const;

    /** @return widget 是可编辑文本控件时返回 true。 */
    bool clickedWidgetIsTextEditor(QWidget *widget) const;

    /** @return 人员表格当前行的人员主键；无选择时返回 0。 */
    qint64 selectedPersonId() const;

    /** @return 人员表格当前行的业务编号。 */
    QString selectedPersonNo() const;

    /** @return 人员表格当前行的启用状态。 */
    bool selectedPersonEnabled() const;

private:
    AppConfig config_;
    StorageStats storageStats_;
    FaceEnrollWidget *enrollWidget_ = nullptr;
    class QListWidget *navList_ = nullptr;
    QTableWidget *peopleTable_ = nullptr;
    QTableWidget *networkPeopleTable_ = nullptr;
    QWidget *networkPeoplePage_ = nullptr;
    QLabel *networkSyncToast_ = nullptr;
    QTimer *networkSyncToastTimer_ = nullptr;
    class QPushButton *networkActiveButton_ = nullptr;
    class QPushButton *networkDeletedButton_ = nullptr;
    class QPushButton *importPeopleButton_ = nullptr;
    class QPushButton *exportPeopleButton_ = nullptr;
    QJsonArray networkPeopleRecords_;
    bool networkDeletedView_ = false;
    QTableWidget *passedLogTable_ = nullptr;
    QTableWidget *syncTaskTable_ = nullptr;
    QTableWidget *systemEventTable_ = nullptr;
    QQuickWidget *keyboardWidget_ = nullptr;
    QWidget *contentShell_ = nullptr;
    class QCheckBox *livenessCheck_ = nullptr;
    class QCheckBox *showDeletedPeopleCheck_ = nullptr;
    class QStackedWidget *stack_ = nullptr;
    QTabWidget *personTabs_ = nullptr;
    QLabel *serialLabel_ = nullptr;
    QLabel *ipLabel_ = nullptr;
    QLabel *macLabel_ = nullptr;
    QLabel *versionLabel_ = nullptr;
    QLabel *uptimeLabel_ = nullptr;
    QLabel *peopleCapacityLabel_ = nullptr;
    QLabel *faceCapacityLabel_ = nullptr;
    QLabel *verifyLogCountLabel_ = nullptr;
    QLabel *diskCapacityLabel_ = nullptr;
    QLabel *cpuUsageLabel_ = nullptr;
    QLabel *memoryUsageLabel_ = nullptr;
    QLabel *temperatureLabel_ = nullptr;
    QLabel *npuUsageLabel_ = nullptr;
    QRadioButton *dhcpRadio_ = nullptr;
    QRadioButton *staticRadio_ = nullptr;
    QLineEdit *ipEdit_ = nullptr;
    QLineEdit *netmaskEdit_ = nullptr;
    QLineEdit *gatewayEdit_ = nullptr;
    QLineEdit *dnsEdit_ = nullptr;
    QDateTimeEdit *dateTimeEdit_ = nullptr;
    QSlider *faceCosineSlider_ = nullptr;
    QSlider *duplicateFaceSlider_ = nullptr;
    QSlider *faceQualitySlider_ = nullptr;
    QSlider *faceDetectSlider_ = nullptr;
    QSlider *livenessSlider_ = nullptr;
    class QCheckBox *audioEnabledCheck_ = nullptr;
    QSlider *audioVolumeSlider_ = nullptr;
    QLabel *audioVolumeValueLabel_ = nullptr;
    QHash<QString, class QCheckBox *> audioPromptEnabledChecks_;
    QHash<QString, QLineEdit *> audioPromptTextEdits_;
    QComboBox *verifyResultCombo_ = nullptr;
    QLineEdit *verifyPersonNoEdit_ = nullptr;
    QLineEdit *verifyNameEdit_ = nullptr;
    QDateTimeEdit *verifyStartEdit_ = nullptr;
    QDateTimeEdit *verifyEndEdit_ = nullptr;
    QLabel *faceCosineValueLabel_ = nullptr;
    QLabel *duplicateFaceValueLabel_ = nullptr;
    QLabel *faceQualityValueLabel_ = nullptr;
    QLabel *faceDetectValueLabel_ = nullptr;
    QLabel *livenessValueLabel_ = nullptr;
    QTimer *deviceStatusTimer_ = nullptr;
    QThread deviceStatusThread_;
    QObject *deviceStatusCollector_ = nullptr;
    bool deviceStatusRequestPending_ = false;
    bool passedLogsLoaded_ = false; /**< 验证日志页是否已完成首次加载。 */
};

#endif
