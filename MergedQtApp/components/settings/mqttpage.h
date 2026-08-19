/**
 * @file mqttpage.h
 * @brief MQTT/IPC 配置、路由选择、连接操作和运行日志页面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QGroupBox>
#include <QEvent>
#include <QTimer>
#include <QJsonObject>

#include "components/settings/mqtt/mqttmanager.h"
#include "components/cursoroverlay/keyboard_dialog.h"
#include "platform/rk3566_platform.h"

/** @brief MQTT/IPC 配置、路由选择、连接操作和运行日志页面。 */
class MqttPage : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建配置表单、日志区和 MqttManager。 */
    explicit MqttPage(QWidget *parent = nullptr);

    /** @brief 断开连接并释放页面资源。 */
    ~MqttPage();

    /** @brief 从配置文件加载当前功能路由对应的 MQTT 输入。 */
    bool loadInputsFromConfigFile(const QString& cfgDir);

    /** @brief 按保存配置自动选择路由并连接。 */
    void startAutoConnect();

    /** @brief 同步播放器当前音量到 MQTT 状态。 */
    void onCurrentPlayVolumeChanged(int volume);

    /** @brief 发布本地手动音量变更结果。 */
    void onLocalManualVolumeChanged(int volume, const QString &reason);


public slots:
    /** @brief 将当前录播文件同步到 MQTT 播放状态。 */
    void onCurrentRecordedFileChanged(const QString &fullPath);
    /** @brief 将最新播放截图同步到 MQTT 播放状态。 */
    void onCurrentPlayImageChanged(const QString &base64Image);

    /** @brief 收到播放信息请求后触发截图，随后发布响应。 */
    void onGetPlayInfoRequested(const QString &topic,
                                const QString &reqId,
                                const QString &deviceId);

    /** @brief 下载完成后结束延迟 videoControl 确认。 */
    void onRecordedDownloadFinished(const QString &localFile);
    /** @brief 下载失败后发布带原因的控制确认。 */
    void onRecordedDownloadFailed(const QString &name, const QString &reason);
    /** @brief 文件已存在时按成功可用结果完成控制确认。 */
    void onRecordedDownloadAlreadyExists(const QString &localFile, const QString &reason);
    /** @brief 磁盘空间不足时发布拒绝确认。 */
    void onRecordedDownloadStorageRejected(const QString &name, const QString &reason);
    /** @brief 请求参数或任务状态无效时发布拒绝确认。 */
    void onRecordedDownloadRequestRejected(const QString &name, const QString &reason);



signals:
    /** @brief 输出页面和业务日志。 */
    void mqttLog(const QString &text);

    /** @brief 透传收到的直播 URL。 */
    void streamUrlReceived(const QString &url);
    /** @brief 请求媒体页设置音量。 */
    void mqttVolumeReceived(int volume);
    /** @brief 请求停止当前直播。 */
    void stopLiveRequested();
    /** @brief 请求停止全部播放。 */
    void stopAllPlayRequested();
    /** @brief 直播地址已分类并可交给播放器。 */
    void liveStreamReady(const QString &url, const QString &kind);
    /** @brief FTP 下载配置已经就绪。 */
    void ftpDownloadTaskReady();

    /** @brief 请求媒体页捕获当前播放画面。 */
    void requestCapturePlayImage();

    /** @brief 请求媒体页恢复录播轮播。 */
    void resumeRecordedPlaybackRequested();
    /** @brief 请求显示视频控制提示。 */
    void videoControlNotice(const QString &text);

    /** @brief MQTT Broker 连接标志变化。 */
    void mqttConnMarkChanged(int mark);

protected:
    /** @brief 点击输入控件时打开对应屏幕键盘。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** @brief 根据 offline_v1/online_v1/online_v2 选择生效路由后的完整结果。 */
    struct FeatureRouteDecision
    {
        bool ok = false;             /**< 是否得到唯一且有效的路由。 */
        bool online_v1 = false;      /**< 第一版在线协议被选中。 */
        bool online_v2 = false;      /**< 第二版在线协议被选中。 */
        QString routeName;           /**< 用于 UI 和日志的路由名称。 */
        QString host;                /**< 当前路由 Broker 地址。 */
        QString protocolMode;        /**< 传给业务网关的协议模式。 */
        QString deviceName;          /**< 当前路由设备名。 */
        QString subscribeTopic;      /**< 业务主订阅主题。 */
        QString username;            /**< Broker 用户名。 */
        QString password;            /**< Broker 密码。 */
        QString publishTopic;        /**< 业务默认发布主题。 */
        QStringList subTopics;       /**< 下发给 mqttd 的完整订阅列表。 */
        QString streamUrlTopic;      /**< 获取直播 URL 的主题。 */
        QString topicText;           /**< 回显到输入框的主题文本。 */
        QString error;               /**< 路由无效时的用户可读原因。 */
    };

    /** @brief 创建 IPC、MQTT 参数、日志和操作按钮区域。 */
    void setupUI();

    /** @brief 在日志开关启用时追加页面日志。 */
    void appendLog(const QString &text);

    /** @brief 更新 IPC 连接状态控件和可操作按钮。 */
    void updateIpcUiState(bool ipcConnected);

    /** @brief 读取功能开关并生成唯一活动 MQTT 路由。 */
    FeatureRouteDecision decideRouteByFeature(const QString &cfgPath, const QString &clientId) const;

    /** @brief 将当前输入保存到被选中的 v1 或 v2 配置键。 */
    bool saveUiInputsToActiveRouteConfig(const QString &cfgPath, const QString &clientId, const QString &v1Sub,
                                            const QString &v1Pub, const QString &v2Sub, const QString &v2Pub, QString *error);

    /** @brief 把路由决定回显到表单并同步 MqttManager 配置。 */
    void applyRouteToUi(const FeatureRouteDecision &decision);

    /** @brief 校验、保存、应用路由并连接 mqttd。 */
    bool routeConnectAndApply(bool autoMode);

    /** @return 去除分隔空白并规范化后的主题文本。 */

    /** @brief 使用屏幕键盘编辑文本输入。 */
    void openKeyboardFor(QLineEdit *edit, const QString &title,
                         QLineEdit::EchoMode echo, int maxLen);

    /** @brief 使用屏幕键盘编辑端口等整数输入。 */
    void openKeyboardForPort(QSpinBox *spin, const QString &title, int minV, int maxV);

private:
    /** MQTT 配置、IPC 和业务路由协调器。 */
    MqttManager *manager_ = nullptr;

    // 基础配置
    QString configDir_ = Rk3566Platform::netConfigDir();
    QString configFileName_ = "net_cfg.ini";
    QString streamUrlTopic_;

    QString currentUiRouteName_;
    bool routeInputsDirty_ = false;

    // 界面组件
    QLabel *titleLabel = nullptr;

    // IPC 区
    QLineEdit *ipcPathEdit = nullptr;
    QLineEdit *ipcStatusEdit = nullptr;
    QLineEdit *mqttStatusEdit = nullptr;
    QLineEdit *subTopicsEdit = nullptr;

    // MQTT 参数区
    QLineEdit *brokerAddressEdit = nullptr;
    QSpinBox *portSpinBox = nullptr;
    QLineEdit *clientIdEdit = nullptr;
    QLineEdit *topicsInputEdit = nullptr;
    QLineEdit *publishTopicEdit = nullptr;
    QSpinBox *keepaliveSpinBox = nullptr;
    QComboBox *qosComboBox = nullptr;
    QLineEdit *usernameEdit = nullptr;
    QLineEdit *passwordEdit = nullptr;
    QCheckBox *sslCheckBox = nullptr;


    bool pendingPlayInfoReply_ = false; /**< 正在等待媒体页截图后响应播放信息。 */
    QString pendingPlayInfoTopic_;
    QString pendingPlayInfoId_;
    QString pendingPlayInfoDeviceId_;

    // 日志区
    QCheckBox *showLogCheckBox = nullptr;
    QGroupBox *logGroup = nullptr;
    QPlainTextEdit *rxLogEdit = nullptr;

    // 按钮区
    QPushButton *btnLoadCfg = nullptr;
    QPushButton *connectButton = nullptr;
    QPushButton *testConnectionButton = nullptr;
    QPushButton *disconnectButton = nullptr;
};
