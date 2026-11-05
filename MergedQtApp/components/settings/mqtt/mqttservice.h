/**
 * @file mqttservice.h
 * @brief 处理 MQTT 媒体命令、维护播放状态并构建协议响应包。
 *
 * @author Du lin
 * @note 最近修改：2026-11-04；修改人：Du lin；拆分后台配置准备与UI业务，后台使用已初始化路径快照避免重复补种媒体；过滤过期播放源并保留原音量/下载处理及协议。
 * @date 2026-07-25
 */

#pragma once

#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QMap>
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

/** @brief 后台准备的纯值结果；失败阶段供UI在原控制流程位置报告错误。 */
struct VideoControlPreparation {
    enum Stage {
        Ready, /**< 全部准备成功。 */
        Selection, /**< 广告类型保存失败。 */
        Ftp, /**< FTP连接参数保存失败。 */
        Directory, /**< 下载目录创建失败。 */
        Paths, /**< FTP路径保存失败。 */
        Stream /**< 流地址保存失败。 */
    }; /**< 失败所处原处理步骤，Ready表示全部准备成功。 */
    bool playbackObsolete=false; /**< UI确认被后续指令替代；只保留原音量处理，不启动/改写旧播放源。 */
    Stage failed=Ready;QString error;QStringList notices; /**< 错误文本及时间同步日志；不保存FTP凭据。 */
};

/** @brief 主实例在UI维护媒体业务；后台局部实例仅执行配置准备，不共享QObject状态。 */
class MqttService : public QObject
{
    Q_OBJECT
public:
    /** @brief 初始化为空闲播放状态。 */
    explicit MqttService(QObject *parent = nullptr);

    /** @brief 后台准备广告选择、FTP/流配置、目录及原时间同步；必须在后台线程创建局部服务调用。
     * @param[in] payloadObj 原videoControl值快照，不访问UI或主服务的共享状态。
     * @return 准备结果及原失败阶段；失败时不会继续后续写入。
     */
    VideoControlPreparation prepareVideoControlConfiguration(const QJsonObject &payloadObj);
    /** @brief 在主服务线程捕获配置路径，返回后台准备函数；不重复初始化/补种媒体目录。
     * @param[in] payloadObj 待处理请求的值快照。
     * @return 仅在后台执行的准备函数，不持有主服务QObject或可变状态。
     */
    std::function<VideoControlPreparation()> configurationPreparation(const QJsonObject &payloadObj) const;
    /** @brief UI消费已完成的配置准备结果，更新状态并发出原播放/音量/下载信号。
     * @param[in] topic 请求主题。
     * @param[in] payloadObj 原请求对象。
     * @param[in] preparation 已串行执行的后台准备结果。
     * @param[out] status 可空；保持原业务成功/等待音量回执语义。
     * @return true为已识别videoControl；false为其他方法。
     */
    bool handleVideoControlMessage(const QString& topic, const QJsonObject& payloadObj,
                                  const VideoControlPreparation &preparation, bool *status = nullptr);

    /** @brief 校验并删除指定本地播放文件。 */
    bool handleDeletePlayFileMessage(const QString &topic, const QJsonObject &payloadObj);

    /** @brief 枚举固定目录的视频和图片并构建文件列表响应包。 */
    QJsonObject buildPlayFileListPublishPacket(const QString &reqTopic,
                                               const QString &reqId,
                                               const QString &deviceId,
                                               QString *respTopicOut = nullptr);

    /**
     * @brief 根据当前播放状态构建普通或高清播放信息回执。
     * @param[in] reqTopic 原请求主题，按既有规则生成响应主题。
     * @param[in] reqId 原请求 id，交由既有信封规则回显。
     * @param[in] deviceId 原设备标识，交由既有信封规则处理。
     * @param[out] respTopicOut 可为空，非空时写入响应主题。
     * @param[in] method 回执 method；默认 sendPlayInfo，高清调用为 sendPlayInfoFromCompany。
     * @param[in] includeCachedImage 默认 true；高清请求传 false，避免混用普通低清截图。
     * @return mqttd publish 信封；除 method 和是否带缓存图像外，播放字段保持一致。
     * @note 在服务对象线程构造无高清图的状态快照，高清图和大 JSON 编码由后台补入。
     */
    QJsonObject buildPlayInfoPublishPacket(const QString &reqTopic,
                                          const QString &reqId,
                                          const QString &deviceId,
                                          QString *respTopicOut,
                                          const QString &method = QStringLiteral("sendPlayInfo"),
                                          bool includeCachedImage = true);

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

    /** @brief 本地媒体删除成功，通知播放器移除文件及图片缓存。 */
    void playFileDeleted(const QString &fullPath);

    /** @brief 请求停止所有媒体播放。 */
    void stopAllPlayRequested();

    /** @brief 请求恢复本地录播轮播。 */
    void resumeRecordedPlaybackRequested();

    /** @brief 本地音量执行完成，管理器据此发布确认。 */
    void volumeControlResultReady(bool ok, const QString &reqTopic, const QJsonObject &reqPayload);

private:
    /** @brief 后台局部服务使用已初始化的路径快照，避免再次补种默认媒体文件。
     * @param[in] configurationPath 主服务已确定的INI路径；不在构造时重新解析平台目录。
     * @param[in] mediaDirectory 主服务已确定的媒体目录；仅保存值，不创建或复制文件。
     * @note 对象只在创建它的后台线程使用、销毁，不连接主服务可变状态。
     */
    MqttService(const QString &configurationPath,const QString &mediaDirectory);
    /** @brief 将 FTP 连接参数写入网络配置。 */
    bool writeFtpIni(const QString& host, const QString& portStr, const QString& user, const QString& pwd, QString* err = nullptr);

    /** @brief 从配置读取 FTP 本地保存目录。 */
    bool readIniFtpLocalDir(QString& localDirOut, QString* err = nullptr);

    /** @brief 从配置读取设备客户端 IP。 */
    QString readDeviceClientIpFromIni() const;

    /** @brief 将 FTP 远端、本地路径及可选流地址写入配置。 */
    bool writeIniFtpPaths(const QString& remotePath, const QString& localPath, const QString& streamUrl = QString(), QString* err = nullptr);

    /** @brief 扫描固定媒体目录并记录协议ID对应的文件快照。 */
    QJsonArray buildLocalVideoFileArray();

    struct LocalPlayFile { QString name; QString path; };
    /** @brief 使用文件列表相同的目录、格式和名称规则扫描媒体。 */
    QList<LocalPlayFile> localPlayFiles() const;
    /** @brief 按上报列表的id/name定位；旧版无id请求仅允许唯一名称。 */
    bool deleteLocalPlayFile(const QString &targetName, const QJsonValue &targetId,
                             QString *deletedPath, QString *err);
    QMap<int, LocalPlayFile> advertisedPlayFiles_;
    bool hasPlayFileSnapshot_ = false;

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
    QString currentRecordedFilePath_; /**< 文件信息来源完整路径，sendPlayInfo仅接受video目录。 */
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
