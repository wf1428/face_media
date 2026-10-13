/**
 * @file mqttmanager.h
 * @brief 协调 mqttd IPC、业务服务、消息路由和 IC 在线网关。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <QDir>
#include <QByteArray>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QQueue>
#include <QSet>
#include <QSettings>
#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QStringList>

#include "mqttipcclient.h"
#include "mqttmessagerouter.h"
#include "mqttservice.h"
#include "common/sql/network_personnel_store.h"
#include "ic_board/network_access_service.h"
#include "platform/rk3566_platform.h"

class IcMqttGateway;
class QThread;

/** @brief mqttd IPC、Broker 路由和在线 IC 硬件参数的统一配置。 */
struct MqttConfig
{
    QString ipcPath;          /**< mqttd 本地套接字路径。 */
    QString host;             /**< MQTT Broker 主机。 */
    int port = 1883;          /**< Broker TCP 端口。 */
    QString clientId;         /**< MQTT 客户端 ID。 */
    QString username;         /**< Broker 用户名。 */
    QString password;         /**< Broker 密码。 */
    bool tls = false;         /**< 是否启用 TLS。 */
    int keepalive = 30;       /**< MQTT keepalive，单位秒。 */
    int qos = 0;              /**< 发布/订阅 QoS。 */
    QStringList subTopics;    /**< 订阅主题列表。 */
    QString streamUrlTopic;   /**< 拉流 URL 业务主题。 */

    QString protocolMode = "legacy"; /**< "legacy"/"online"/"offline" 兼容模式。 */
    QString deviceName;              /**< 设备业务名称。 */
    QString publishTopic;            /**< 默认发布主题。 */
    QString subscribeTopic;          /**< 默认订阅主题。 */

    QString dbPath = Rk3566Platform::databasePath(); /**< IC、二维码和人脸统一数据库路径。 */
    QString qrDev = Rk3566Platform::qrDevice();      /**< 二维码串口设备。 */
    int qrBaud = 9600;                               /**< 二维码串口波特率。 */
    QString cardDev = Rk3566Platform::cardDevice();  /**< 刷卡串口设备。 */
    int cardBaud = 9600;                             /**< 刷卡串口波特率。 */
    QString rs485Dev = Rk3566Platform::rs485Device(); /**< RS485 数据串口设备。 */
    int rs485Baud = 9600;                            /**< RS485 串口波特率。 */
};

/** @brief online_v1 人员同步 event 回包中的 data.resultcode。 */
enum class PersonnelSyncResultCode : int
{
    Failure = -1, /**< 同步处理失败。 */
    Success = 0   /**< 同步处理成功，包括哈希未变化等成功结果。 */
};


/** @brief 等待 FTP 下载结果后再发送的视频控制确认上下文。 */
struct PendingVideoControlAck
{
    bool active = false;       /**< 是否存在待完成确认。 */
    QString reqTopic;          /**< 原请求主题。 */
    QJsonObject reqPayload;    /**< 原请求 payload。 */
};

/** @brief online_v1 单笔在线二维码授权事务的内存状态。 */
struct PendingOnlineQrAccess
{
    bool active = false;
    QString scanId;
    QString qrCode;
    QString personId;
    QString floorControlId;
    bool scanAccepted = false;
    bool rs485Requested = false;
    bool rs485Finished = false;
    bool rs485Success = false;
    bool accessResultReported = false;
};

/** @brief MQTT Broker 断线期间的一笔本地二维码控梯任务。 */
struct PendingOfflineQrAccess
{
    bool active = false;
    QString sourceTag;
    NetworkAccessResult access;
};

/** @brief 等待 FaceEngine 完成服务器人脸图像质量校验的上下文。 */
struct PendingFaceImageValidation
{
    QString personId;
    QJsonObject responsePayload;
    QJsonArray requestedFaces;
    QJsonArray initialFailedFaces;
};

/** @brief 已发送且尚未收到 face.responseImages 的请求上下文。 */
struct PendingFaceImageRequest
{
    QJsonObject requestData;
    QJsonObject requestPayload;
    bool awaitingResponse = false;
};

/** @brief 一笔等待后台线程落盘、落库并完成人脸校验的图片响应。 */
struct PendingFaceImageWork
{
    QString requestKey;
    QString contentKey;
    QString personId;
    QJsonObject responsePayload;
};


/**
 * @brief MQTT 页面、mqttd IPC、业务服务和消息路由的协调器。
 *
 * 负责下发连接配置、断线重连、解析 IPC JSON、按 method 分派业务，并统一发布响应。
 */
class MqttManager : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建 IPC 客户端、业务服务、IC 网关和消息路由器。 */
    explicit MqttManager(QObject *parent = nullptr);
    ~MqttManager() override;

    /** @brief 注册一个可处理一组 method 的扩展处理器。 */
    bool registerMessageHandler(IMqttMessageHandler *handler);

    /** @brief 替换 MQTT/IC 配置并同步给网关。 */
    void setConfig(const MqttConfig &cfg);

    /** @return 当前配置常量引用。 */
    const MqttConfig& config() const { return cfg_; }

    /** @brief 连接 mqttd 并在连接后下发 Broker 配置。 */
    void connectAndApply();

    /** @brief 主动断开 IPC 并停止自动重连。 */
    void disconnectIpc();

    /** @brief 仅测试 IPC/Broker 配置，不改变业务播放状态。 */
    void testConnection();


    /** @brief 更新业务服务中的当前录播文件。 */
    void setCurrentRecordedFile(const QString &fullPath);

    /** @brief 清除当前录播文件信息。 */
    void clearCurrentRecordedFile();
    //PlayInfoState currentPlayInfo() const;
    /** @brief 更新当前播放画面 Base64。 */
    void updateCurrentPlayImage(const QString &base64Image);

    /** @brief 立即构建并发布播放信息响应。 */
    void sendPlayInfoNow(const QString &reqTopic,
                         const QString &reqId,
                         const QString &deviceId);

    /** @brief 更新当前播放音量。 */
    void updateCurrentPlayVolume(int volume);

    /** @brief 完成待处理的录播下载控制确认。 */
    void notifyRecordedDownloadResult(bool statusOk, bool ftpDownloaded,
                                      const QString &downloadResult,
                                      const QString &reason = QString());

    /** @brief 更新当前设备日志文本。 */
    void updateCurrentDeviceLog(const QString &deviceLog);

    /** @brief 清除当前设备日志。 */
    void clearCurrentDeviceLog();

    /** @brief 接收音量控制执行结果并发布确认。 */
    void onVolumeControlResultReady(bool ok, const QString &reqTopic, const QJsonObject &reqPayload);

    /** @brief 通知业务服务本地音量设置完成。 */
    void notifyVolumeSetFinished(bool ok, const QString &reason);

    /** @brief 发布一次本地音量变更状态。 */
    void publishLocalVideoControlStatus(int volume, bool ok, const QString &reason);

    /** @brief 通过 IPC 发布已组装的 JSON packet。 */
    bool publishJsonPacket(const QJsonObject &packet, const QString &tag = QString());

signals:
    /** @brief 输出统一业务日志。 */
    void logMessage(const QString &msg);

    /** @brief mqttd 本地 IPC 连接状态变化。 */
    void ipcStateChanged(bool connected);
    /** @brief Broker 连接状态文案变化。 */
    void mqttStateTextChanged(const QString &text);
    /** @brief mqttd 返回的订阅主题文案变化。 */
    void subTopicsTextChanged(const QString &text);

    /** @brief 从 mqttd 状态包取得远端 MQTT 配置。 */
    void mqttConfigFromStatus(const QJsonObject &mqttObj);

    /** @brief 收到可播放的直播 URL。 */
    void streamUrlReceived(const QString &url);
    /** @brief 收到服务端音量控制值。 */
    void mqttVolumeReceived(int volume);
    /** @brief 请求停止当前直播并回退业务策略。 */
    void stopLiveRequested();
    /** @brief 请求停止全部媒体播放。 */
    void stopAllPlayRequested();
    /** @brief 直播地址已分类并可交给播放器。 */
    void liveStreamReady(const QString &url, const QString &kind);
    /** @brief FTP 下载配置已就绪。 */
    void ftpDownloadTaskReady();

    /** @brief 请求业务层捕获当前播放信息后发送响应。 */
    void getPlayInfoRequested(const QString &topic,
                              const QString &reqId,
                              const QString &deviceId);
    /** @brief Broker 连接标志变化，供媒体页面更新状态。 */
    void mqttConnMarkChanged(int mark);

    /** @brief 请求恢复本地录播轮播。 */
    void resumeRecordedPlaybackRequested();
    /** @brief 请求显示视频控制结果提示。 */
    void videoControlNotice(const QString &text);



private slots:
    /** @brief IPC 建立后发送配置并停止重连计时。 */
    void onIpcConnected();
    /** @brief IPC 断开后按策略启动重连。 */
    void onIpcDisconnected();
    /** @brief 解析一帧 mqttd JSON 并分派状态或业务消息。 */
    void onIpcFrameReceived(const QByteArray &frame);
    /** @brief 记录 IPC 错误并更新页面状态。 */
    void onIpcError(const QString &err);


private:
    /** @brief 将当前 Broker 配置写入 mqttd，applyNow 控制是否立即应用。 */
    void sendConfigToMqttd(bool applyNow);
    /** @brief 解析一行 IPC JSON 并按消息类型处理。 */
    void handleIpcLine(const QByteArray &line);
    /** @brief 注册媒体服务和 IC 网关等内置处理器。 */
    void registerBuiltinMessageHandlers();

    /** @brief 按 method 将业务 payload 分派给扩展或内置处理器。 */
    bool dispatchPayloadMessage(const QString &topic, const QJsonObject &payloadObj);

    /** @brief 处理本地播放文件列表请求并发布响应。 */
    bool handleGetPlayFileList(const QString &topic, const QJsonObject &payloadObj, const QString &method);
    /** @brief 请求媒体页刷新截图后发送播放信息。 */
    bool handleGetPlayInfo(const QString &topic, const QJsonObject &payloadObj, const QString &method);
    /** @brief 删除指定本地播放文件并发布结果。 */
    bool handleDeletePlayFile(const QString &topic, const QJsonObject &payloadObj, const QString &method);
    /** @brief 处理播放、停止、音量和 FTP 下载控制。 */
    bool handleVideoControl(const QString &topic, const QJsonObject &payloadObj);
    /** @brief 处理独立直播 URL 消息。 */
    bool handleStreamUrlMessage(const QString &topic, const QJsonObject &payloadObj);
    /** @brief 处理网络人员全量、哈希核对和凭证删除消息并发布业务回执。 */
    bool handlePersonnelSync(const QString &topic,
                             const QJsonObject &payloadObj,
                             const QString &method);
    bool handleFaceImages(const QString &topic,
                           const QJsonObject &payloadObj,
                           const QString &method);
    /** @brief 在后台串行处理下一笔人脸图片响应。 */
    void processNextFaceImageWork();
    /** @brief 接收后台图片解码、文件落盘和数据库路径提交结果。 */
    void onFaceImagesPrepared(const NetworkPersonnelSyncResult &result);
    /** @brief 释放当前图片请求的进程内去重状态并继续下一笔。 */
    void finishCurrentFaceImageWork();
    /** @brief 接收 FaceEngine 图片质量校验结果并完成数据库确认或回滚。 */
    void onStoredFaceValidationFinished(const QString &token,
                                        const QString &personId,
                                        bool ok,
                                        const QString &message,
                                         const QJsonArray &validatedFaces,
                                         const QJsonArray &failedFaces);
    /** @brief 后台提交特征事务后，在MQTT线程发布逐图结果并推进队列。 */
    void completeStoredFaceValidation(
            const PendingFaceImageValidation &pending,
            bool personMatches,
            const QString &validationMessage,
            const QJsonArray &validationFailedFaces,
            bool finalized,
            bool allFacesReady,
            const QString &finalizeError,
            const QJsonArray &commitFailedFaces,
            const QJsonArray &commitFaceNotices);
    /** @brief 按下发人脸逐张发布 face.responseImages 的 Imagesresult 业务结果。 */
    bool publishFaceImagesResult(const QJsonObject &responsePayload,
                                 const QString &personId,
                                 bool result,
                                 const QString &message,
                                 const QJsonArray &failedFaces,
                                  const QJsonArray &faceNotices = QJsonArray());
    /** @brief 后台人员同步完成后提交逐笔 event，并触发后续图片补拉。 */
    void completePersonnelSync(const QString &requestTopic,
                               const QJsonObject &payloadObj,
                               const QString &method,
                               const NetworkPersonnelSyncResult &result);
    /** @brief 延迟合并网络人员列表、图库及存储统计刷新。 */
    void scheduleNetworkFaceRefresh();
    /** @brief 执行一次已合并的网络人员列表与图库刷新。 */
    void flushNetworkFaceRefresh();
    /** @brief 处理 online_v1 平台下发的单楼层远程呼梯指令。 */
    bool handleOnlineV1RemoteCall(const QString &topic,
                                  const QJsonObject &payloadObj,
                                  const QString &method);
    bool handleOnlineV1RemoteCallDeductResult(const QString &topic,
                                              const QJsonObject &payloadObj,
                                              const QString &method);
    /** @brief 处理 online_v1 楼层开放/限行设置与当前状态查询。 */
    bool handleOnlineV1FloorLimit(const QString &topic,
                                  const QJsonObject &payloadObj,
                                  const QString &method);
    /** @brief 处理 online_v1 定时楼层规则的全量覆盖与查询。 */
    bool handleOnlineV1FloorSchedule(const QString &topic,
                                     const QJsonObject &payloadObj,
                                     const QString &method);
    /** @brief 处理 online_v1 电梯自定义模式设置与查询。 */
    bool handleOnlineV1ElevatorMode(const QString &topic,
                                    const QJsonObject &payloadObj,
                                    const QString &method);
    /** @brief 按设备本地星期和分钟执行到期的楼层开放/限行规则。 */
    void onOnlineV1FloorScheduleTick();
    /** @brief 处理 online_v1 平台探活并原样回显请求 id。 */
    bool handleOnlineV1Heartbeat(const QString &topic,
                                 const QJsonObject &payloadObj,
                                 const QString &method);
    /** @brief 记录 online_v1 远程呼梯的 RS485 实际发送结果，不发布 event。 */
    void onOnlineV1RemoteCallRs485Finished(const QString &sourceTag,
                                           bool success,
                                           const QString &reason);
    /** @brief fullPersonnel 落库后自动请求该人员尚未完整保存的人脸原图。 */
    bool requestFaceImagesForPerson(const QString &personId,
                                    bool reconnectRetry = false);
    /** @brief Broker重连后立即补发本次Keepalive中断影响的图片请求。 */
    void resendPendingFaceImageRequestsAfterReconnect();
    /** @brief 处理 online_v1 平台下发的二维码校验、楼层控制和扣次结果。 */
    bool handleOnlineV1QrMessage(const QString &topic,
                                 const QJsonObject &payloadObj,
                                 const QString &method);
    /** @brief 扫描器得到二维码后发布 qr.scan。 */
    void onOnlineV1QrScanned(const QString &qrCode);
    /** @brief 处理 online_v1 二维码 RS485 实际发送结果。 */
    void onOnlineV1QrRs485Finished(const QString &sourceTag,
                                   bool success,
                                   const QString &reason);
    /** @brief 等待平台授权或扣次结果超时。 */
    void onOnlineV1QrTimeout();
    /** @brief 发布 online_v1 二维码 event 消息。 */
    bool publishOnlineV1QrEvent(const QString &method,
                                const QJsonObject &data,
                                const QString &messageId,
                                const QString &tag);
    bool publishOnlineV1Event(const QString &method,
                              const QJsonObject &data,
                              const QString &messageId,
                              const QString &tag,
                              const QString &eventTime = QString());
    /** @return 网络不可用或 MQTT Broker 未连接时返回 true。 */
    bool shouldRecordOfflineAccess() const;
    /** @brief 持久化一次 Broker 断线期间的成功通行。 */
    bool recordOfflineAccessResult(const QString &method,
                                   const QString &personId);
    /** @brief Broker 恢复后按通行类型批量补报并消费本地快照。 */
    void flushOfflineAccessResults();
    void onOnlineV1FaceAccessFinished(const QString &personId,
                                      const QString &faceHash,
                                      const QString &floors,
                                      const QByteArray &rs485Frame,
                                      bool success,
                                      const QString &reason);
    /** @brief 密码校验并完成 RS485 下发后发布 password.accessResult。 */
    void onOnlineV1PasswordAccessFinished(const QString &personId,
                                          const QString &floors,
                                          const QByteArray &rs485Frame,
                                          bool success,
                                          const QString &reason);
    /** @brief 在独立线程读取抓拍并编码 Base64，随后发布 face.upload。 */
    void onOnlineV1FaceUploadRequested(const QString &personId,
                                       const QString &faceHash,
                                       const QString &snapshotPath,
                                       bool success);
    /** @brief 在 MQTT 所在线程组装并发布人脸抓拍记录。 */
    void publishOnlineV1FaceUpload(const QString &personId,
                                   const QString &faceHash,
                                   const QString &faceImage,
                                   bool success,
                                   const QString &imageError);
    void onOnlineV1CardAccessFinished(const QString &personId,
                                      const QString &cardId,
                                      const QString &floors,
                                      const QByteArray &rs485Frame,
                                      bool success,
                                      const QString &reason);
    /** @brief 处理刷卡或人脸通行后的平台扣次结果。 */
    bool handleOnlineV1AccessDeductResult(const QString &topic,
                                          const QJsonObject &payloadObj,
                                          const QString &method);
    /** @brief RS485完成后发布一次 qr.accessResult。 */
    bool publishOnlineV1QrAccessResult(bool success,
                                       const QString &failureReason = QString());
    /** @brief 清理当前内存事务，不删除数据库审计记录。 */
    void clearPendingOnlineV1Qr();
    /** @brief 确认 Linux 重启请求并触发系统重启。 */
    bool handleLinuxReboot(const QString &topic, const QJsonObject &payloadObj, const QString &method);

    /** @brief 处理 mqttd 的 status/config 类状态消息。 */
    void handleStatusLikeMessage(const QJsonObject &obj);
    /** @brief 处理 Broker 连接状态消息。 */
    void handleConnMessage(const QJsonObject &obj);
    /** @brief 处理 mqttd 配置确认消息。 */
    void handleAckMessage(const QJsonObject &obj);

    /** @brief 兼容不同 IPC envelope，提取主题和 payload 对象。 */
    bool extractPayloadObjectFromMsgObject(const QJsonObject &obj, QString &topicOut, QJsonObject &payloadOut);

    /** @brief 在允许自动重连时启动固定间隔定时器。 */
    void startIpcReconnect();
    /** @brief 停止 IPC 重连定时器。 */
    void stopIpcReconnect();

    /** @brief 立即构建并发布 videoControl 确认包。 */
    void sendVideoControlAckNow(const QString &reqTopic, const QJsonObject &reqPayload, bool ok, bool ftpDownloaded,
                                bool includeStatus = true, bool includeFtpDownload = true,
                                const QString &downloadResult = QString(), const QString &reason = QString());

    /** @brief 将当前 MQTT 路由字段持久化到网络 INI。 */
    bool persistCurrentMqttRouteToIni(QString *err = nullptr) const;

private:
    MqttConfig cfg_;
    MqttIpcClient *ipc_ = nullptr;
    MqttService *service_ = nullptr;
    IcMqttGateway *icGateway_ = nullptr;
    MqttMessageRouter *messageRouter_ = nullptr;
    NetworkPersonnelStore networkPersonnelStore_; /**< 统一 SQLite 中的网络人员聚合仓储。 */
    QThread *personnelSyncThread_ = nullptr; /**< 人员和注册照数据库任务专用串行线程。 */
    QObject *personnelSyncWorker_ = nullptr; /**< 承载后台人员同步任务的事件对象。 */
    NetworkPersonnelStore *personnelSyncStore_ = nullptr; /**< 仅由同步线程访问的仓储实例。 */
    QThread *faceUploadThread_ = nullptr; /**< 抓拍文件读取和 Base64 编码专用线程。 */
    QObject *faceUploadWorker_ = nullptr; /**< 串行执行抓拍编码任务。 */

    bool waitingStreamUrl_ = false; /**< 页面正在等待 mqttd 返回直播地址。 */

    bool mqttRoutePersisted_ = false; /**< 避免重复写入相同路由配置。 */

    QTimer *ipcReconnectTimer_ = nullptr;
    bool ipcAutoReconnect_ = true;
    int ipcReconnectIntervalMs_ = 1000;

    QTimer *onlineQrV1Timer_ = nullptr;
    int onlineQrV1TimeoutMs_ = 10000;
    PendingOnlineQrAccess pendingOnlineQr_;
    PendingOfflineQrAccess pendingOfflineQr_;
    NetworkAccessService offlineAccessService_;

    QHash<QString, PendingFaceImageValidation> pendingFaceValidations_;
    QHash<QString, QString> faceValidationTokenByPerson_;
    int faceValidationTimeoutMs_ = 30000;
    QQueue<PendingFaceImageWork> pendingFaceImageWork_;
    QSet<QString> activeFaceImageRequestKeys_;
    QSet<QString> activeFaceImageContentKeys_;
    PendingFaceImageWork currentFaceImageWork_;
    bool faceImageWorkActive_ = false;

    QTimer *networkFaceRefreshDebounceTimer_ = nullptr;
    QTimer *networkFaceRefreshMaxTimer_ = nullptr;
    int networkFaceRefreshDebounceMs_ = 2000;
    int networkFaceRefreshMaxMs_ = 10000;

    QTimer *faceImageReconnectResponseTimer_ = nullptr;
    QHash<QString, PendingFaceImageRequest> pendingFaceImageRequests_;
    QSet<QString> faceImageReconnectRetryPersons_;
    QSet<QString> faceImageReconnectAwaitingPersons_;
    int faceImageReconnectResponseTimeoutMs_ = 10000;
    bool mqttBrokerConnected_ = false;
    bool ipcConfigApplyPending_ = false; /**< IPC重连后等待mqttd状态，再决定是否重建Broker连接。 */
    int ipcConfigDecisionGeneration_ = 0; /**< 隔离旧IPC连接遗留的状态重试定时器。 */
    bool mqttReconnectRequiredAfterNetworkLoss_ = false;

    QTimer *floorScheduleTimer_ = nullptr;
    int floorScheduleCheckIntervalMs_ = 5000;

    PendingVideoControlAck pendingVideoCtrlAck_; /**< 等待 FTP 完成的延迟确认。 */
    bool recordedDownloadTaskActive_ = false;
    QString recordedDownloadTaskKey_;
};
