/**
 * @file mqttservice.h
 * @brief 处理 MQTT 媒体命令、维护播放状态并构建协议响应包。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <QObject>
#include <QJsonObject>
#include <functional>
#include <QDateTime>
#include "platform/rk3566_platform.h"

/** @brief 当前媒体播放状态，用于 getPlayInfo 响应。 */
struct PlayInfoState
{
    QString videoType = "RECORDED"; /**< NONE、LIVE 或 RECORDED。 */
    int videoVolume = 0;           /**< 用户音量。 */
    QString videoPath;             /**< 录播文件名，不含目录。 */
    QString videoUrl;              /**< 当前直播地址；录播时为空。 */
    QString name;                  /**< 当前播放资源的显示名称。 */
    QString base64Image;           /**< 当前画面截图的 Base64 数据。 */
    QString status;                /**< 业务状态：ok 或 error。 */
    QString deviceLog;             /**< 最近设备状态或错误信息。 */
};

/**
 * @brief MQTT 媒体业务命令处理和响应包构建服务。
 *
 * 不直接发送 MQTT；所有方法只更新本地状态、触发播放器信号或返回待发布 JSON，
 * 实际 IPC 发布由 MqttManager 完成。
 */
class MqttService : public QObject
{
    Q_OBJECT
public:
    /** @brief 初始化为空闲播放状态。 */
    explicit MqttService(QObject *parent = nullptr);

    /** @brief 处理播放、音量、停止和下载等 videoControl 命令。 */
    bool handleVideoControlMessage(const QString& topic, const QJsonObject& payloadObj, bool *status = nullptr);

    /** @brief 校验并删除指定本地播放文件。 */
    bool handleDeletePlayFileMessage(const QString &topic, const QJsonObject &payloadObj);

    /** @brief 枚举本地视频并构建文件列表响应包。 */
    QJsonObject buildPlayFileListPublishPacket(const QString &reqTopic,
                                               const QString &reqId,
                                               const QString &deviceId,
                                               QString *respTopicOut = nullptr);

    /** @brief 根据 playInfo_ 构建 sendPlayInfo 响应包。 */
    QJsonObject buildPlayInfoPublishPacket(const QString &reqTopic,
                                          const QString &reqId,
                                          const QString &deviceId,
                                          QString *respTopicOut);

    /** @brief 构建 videoControl 确认包，可选择包含状态和下载结果字段。 */
    QJsonObject buildVideoControlAckPublishPacket(const QString &reqTopic, const QJsonObject &reqPayload, bool status, bool ftpDownloaded,
                                                    QString *respTopicOut, bool includeStatus = true, bool includeFtpDownload = true,
                                                    const QString &downloadResult = QString(), const QString &reason = QString());

    /** @brief 更新录播下载状态和设备日志。 */
    void updateRecordedDownloadStatus(bool statusOk, const QString &deviceLog);

    /** @brief 构建本地音量变化的 videoControl 状态包。 */
    QJsonObject buildLocalVideoControlStatusPublishPacket(const QString &reqTopic, const QString &deviceId, int volume, bool ok, QString *respTopicOut);

    /** @brief 构建 Linux 重启命令确认包。 */
    QJsonObject buildLinuxRebootAckPublishPacket(const QString &reqTopic, const QJsonObject &reqPayload, QString *respTopicOut);

    /** @brief 更新当前播放画面 Base64。 */
    void updateCurrentPlayImage(const QString &base64Image);

    /** @return 当前播放状态副本。 */
    PlayInfoState currentPlayInfo() const;

    /** @brief 更新当前音量。 */
    void updateCurrentPlayVolume(int volume);

    /** @brief 设置当前录播文件并更新类型、路径和名称。 */
    void setCurrentRecordedFile(const QString &fullPath);

    /** @brief 清除当前录播文件并恢复无播放状态。 */
    void clearCurrentRecordedFile();

    /** @brief 将客户端 ID、流主题和订阅主题写入网络配置。 */
    static bool writeMqttIni(const QString& clientId,
                             const QString& streamUrlTopic,
                             const QString& subTopics,
                             QString* err = nullptr);

    /** @brief 更新播放信息响应中的设备日志。 */
    void updateCurrentDeviceLog(const QString &deviceLog);
    /** @brief 清空播放信息中的设备日志。 */
    void clearCurrentDeviceLog();

    /** @return 存在等待本地音量执行结果的请求时返回 true。 */
    bool hasPendingVolumeAck() const;
    /** @brief 完成挂起的音量请求并发出统一结果信号。 */
    void notifyVolumeSetFinished(bool ok, const QString &reason = QString());


signals:
    /** @brief 输出业务处理日志。 */
    void logMessage(const QString &msg);

    /** @brief 请求媒体页设置音量。 */
    void mqttVolumeReceived(int volume);
    /** @brief 请求停止直播。 */
    void stopLiveRequested();
    /** @brief 请求播放已分类的直播地址。 */
    void liveStreamReady(const QString &url, const QString &kind);
    /** @brief FTP 下载参数已经写入配置。 */
    void ftpDownloadTaskReady();

    /** @brief 请求停止所有媒体播放。 */
    void stopAllPlayRequested();

    /** @brief 请求恢复本地录播轮播。 */
    void resumeRecordedPlaybackRequested();

    /** @brief 本地音量执行完成，管理器据此发布确认。 */
    void volumeControlResultReady(bool ok, const QString &reqTopic, const QJsonObject &reqPayload);

private:
    /** @brief 将 FTP 连接参数写入网络配置。 */
    bool writeFtpIni(const QString& host, const QString& portStr, const QString& user, const QString& pwd, QString* err = nullptr);

    /** @brief 从配置读取 FTP 本地保存目录。 */
    bool readIniFtpLocalDir(QString& localDirOut, QString* err = nullptr);

    /** @brief 从配置读取设备客户端 IP。 */
    QString readDeviceClientIpFromIni() const;

    /** @brief 将 FTP 远端、本地路径及可选流地址写入配置。 */
    bool writeIniFtpPaths(const QString& remotePath, const QString& localPath, const QString& streamUrl = QString(), QString* err = nullptr);

    /** @brief 扫描本地视频目录并生成协议文件数组。 */
    QJsonArray buildLocalVideoFileArray();

    /** @brief 校验文件名边界后删除本地视频。 */
    bool deleteLocalVideoByName(const QString &targetName, QString *deletedPath = nullptr, QString *err = nullptr);

    /** @brief 根据请求主题约定构造响应主题。 */
    static QString buildResponseTopicFromRequest(const QString &reqTopic);

    /** @brief 构造含 method、请求 ID、设备 ID 和 data 的 payload。 */
    QJsonObject buildMethodPayload(const QString &method, const QString &reqId, const QString &deviceId, const QJsonObject &dataObj) const;

    /** @brief 将主题和 payload 封装为 mqttd publish packet。 */
    QJsonObject buildPublishPacket(const QString &topic, const QJsonObject &payload) const;

    /** @brief 以统一格式记录待发布 packet。 */
    void logPublishPacket(const QString &tag, const QJsonObject &payload, const QJsonObject &root);

    /** @brief 构造通用业务事件的发布 packet。 */
    QJsonObject buildEventPublishPacket(const QString &reqTopic, const QString &method, const QString &reqId, const QString &deviceId,
                                            const QJsonObject &dataObj, QString *respTopicOut, const QString &logTag);

    /** @brief 设置 Linux 系统时间。 */
    bool setSystemTimeFromLocal(const QDateTime& localDT, QString* err);
    /** @brief 将本地时间写入 RTC。 */
    bool setRtcFromLocal(const QDateTime& localDT, QString* err);
    /** @brief 解析服务端时间字符串并同步系统与 RTC。 */
    bool syncSystemTimeFromServerString(const QString& serverTimeStr, QString* err);

    PlayInfoState playInfo_;
    const QString iniPath = Rk3566Platform::netConfigPath();
    const QString videoDir = Rk3566Platform::videoDir();


    QJsonObject pendingVolumeReqPayload_; /**< 等待本地执行完成的原始音量请求。 */

    bool pendingVolumeAck_ = false;
    QString pendingVolumeReqTopic_;
    QString pendingVolumeReqId_;
    QString pendingVolumeDeviceId_;
    int pendingVolumeValue_ = -1;

    bool pendingVolumeAckShouldPublish_ = false; /**< 本次挂起请求是否需要 MQTT 回执。 */

};
