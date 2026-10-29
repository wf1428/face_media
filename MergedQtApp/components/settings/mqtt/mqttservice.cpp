/**
 * @file mqttservice.cpp
 * @brief 处理 MQTT 媒体命令、维护播放状态并构建协议响应包。
 *
 * @author Du lin
 * @note 最近修改：2026-10-24；修改人：Du lin；复用播放信息字段构造高清响应元数据，允许跳过普通截图缓存并指定高清 method，普通 sendPlayInfo 默认行为不变。
 * @date 2026-07-25
 */

#include "mqttservice.h"
#include "common/media_content_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTextCodec>
#include "platform/rk3566_platform.h"

/** @brief 初始化为空闲播放状态。 */
MqttService::MqttService(QObject *parent)
    : QObject(parent)
{
}


/** @brief 设置 Linux 系统时间。 */
bool MqttService::setSystemTimeFromLocal(const QDateTime& localDT, QString* err)
{
    return Rk3566Platform::setSystemDateTime(localDT, err);
}


/** @brief 将本地时间写入 RTC。 */
bool MqttService::setRtcFromLocal(const QDateTime& localDT, QString* err)
{
    Q_UNUSED(localDT);
    return Rk3566Platform::syncRtcFromSystem(err);
}


/** @brief 解析服务端时间字符串并同步系统与 RTC。 */
bool MqttService::syncSystemTimeFromServerString(const QString& serverTimeStr, QString* err)
{
    const QString t = serverTimeStr.trimmed();
    if (t.isEmpty()) {
        if (err) *err = "服务端 time 为空";
        return false;
    }

    QDateTime serverDt = QDateTime::fromString(t, "yyyy-MM-dd HH:mm:ss");
    if (!serverDt.isValid()) {
        if (err) *err = QString("服务端 time 格式无效: %1").arg(t);
        return false;
    }

    // 服务端传的是本地时间字符串
    serverDt.setTimeSpec(Qt::LocalTime);

    const QDateTime now = QDateTime::currentDateTime();
    const qint64 diff = qAbs(now.secsTo(serverDt));

    if (diff < 2) {
        return true;
    }

    QString e1;
    if (!setSystemTimeFromLocal(serverDt, &e1)) {
        if (err) *err = QString("设置系统时间失败: %1").arg(e1);
        return false;
    }

    if (diff >= 10) {
        QString e2;
        if (!setRtcFromLocal(serverDt, &e2)) {
            if (err) *err = QString("写入 RTC 失败: %1").arg(e2);
            return false;
        }
    }

    emit logMessage(QString("时间同步成功：server=%1 local_before=%2 diff=%3秒")
                    .arg(serverDt.toString("yyyy-MM-dd HH:mm:ss"))
                    .arg(now.toString("yyyy-MM-dd HH:mm:ss"))
                    .arg(diff));

    return true;
}


/**
 * @brief 写入 FTP 基础配置到 ini 文件。
 *
 * 该函数会确保 平台配置目录存在，并将 FTP 主机、端口、用户名、密码
 * 写入 平台 net_cfg.ini 的 [ftp] 分组中。
 *
 * @param host    FTP 主机地址。
 * @param portStr FTP 端口字符串。
 * @param user    FTP 用户名。
 * @param pwd     FTP 密码。
 * @param err     输出参数，失败时返回错误描述。
 * @return true  写入成功。
 * @return false 写入失败。
 */
bool MqttService::writeFtpIni(const QString& host,
                              const QString& portStr,
                              const QString& user,
                              const QString& pwd,
                              QString* err)
{

    // 平台配置目录不存在时尽量创建
    QDir dir(Rk3566Platform::netConfigDir());
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (err) *err = "无法创建平台配置目录";
            return false;
        }
    }

    QSettings set(iniPath, QSettings::IniFormat);
    set.setIniCodec("UTF-8"); // Qt5 可用

    set.beginGroup("ftp");
    set.setValue("host", host);
    set.setValue("port", portStr);
    set.setValue("username", user);
    set.setValue("password", pwd);
    set.endGroup();

    set.sync();
    if (set.status() != QSettings::NoError) {
        if (err) *err = "写入 ini 失败（权限或磁盘问题）";
        return false;
    }
    return true;
}


/**
 * @brief 从 ini 文件中读取 FTP 本地目录配置。
 *
 * 从 平台 net_cfg.ini 的 [ftp].local_path 读取本地目录路径。
 *
 * @param localDirOut 输出参数，返回读取到的本地目录。
 * @param err         输出参数，失败时返回错误描述。
 * @return true  读取成功。
 * @return false 配置目录或下载目录无法创建，或配置读写失败。
 */
bool MqttService::readIniFtpLocalDir(QString& localDirOut, QString* err)
{
    QDir cfgDir(Rk3566Platform::netConfigDir());
    if (!cfgDir.exists() && !cfgDir.mkpath(QStringLiteral("."))) {
        if (err) *err = QStringLiteral("无法创建平台配置目录");
        return false;
    }

    QSettings set(iniPath, QSettings::IniFormat);
    set.setIniCodec("UTF-8");

    set.beginGroup("ftp");
    localDirOut = set.value("local_path", Rk3566Platform::videoDir()).toString().trimmed();
    if (localDirOut.isEmpty()) {
        localDirOut = Rk3566Platform::videoDir();
    }
    localDirOut = QDir::cleanPath(localDirOut);
    set.setValue("local_path", localDirOut);
    set.endGroup();
    set.sync();

    if (set.status() != QSettings::NoError) {
        if (err) *err = QStringLiteral("读取或更新 [ftp].local_path 失败");
        return false;
    }

    QDir localDir(localDirOut);
    if (!localDir.exists() && !localDir.mkpath(QStringLiteral("."))) {
        if (err) *err = QStringLiteral("无法创建 FTP 本地目录：%1").arg(localDirOut);
        return false;
    }
    return true;
}


// 获取配置文件的本地IP
QString MqttService::readDeviceClientIpFromIni() const
{
    QFileInfo fi(iniPath);
    if (!fi.exists() || !fi.isFile()) {
        return QString();
    }

    QSettings set(iniPath, QSettings::IniFormat);
    set.setIniCodec("UTF-8");

    set.beginGroup("network");

    QString ip = set.value("IP").toString().trimmed();
    if (ip.isEmpty()) {
        ip = set.value("ip").toString().trimmed();
    }

    set.endGroup();
    return ip;
}


/**
 * @brief 写入 FTP 路径与流地址相关配置。
 *
 * 该函数会按需写入：
 * - [ftp].remote_path
 * - [ftp].local_path
 * - [stream].url
 *
 * 空字符串参数表示该项不更新。
 *
 * @param remotePath 远端文件路径。
 * @param localPath  本地下载目录。
 * @param streamUrl  直播流地址。
 * @param err        输出参数，失败时返回错误描述。
 * @return true  写入成功。
 * @return false ini 不存在、不可写或写入失败。
 */
bool MqttService::writeIniFtpPaths(const QString& remotePath,
                                   const QString& localPath,
                                   const QString& streamUrl,
                                   QString* err)
{
    QFileInfo fi(iniPath);
    if (!fi.exists() || !fi.isFile()) {
        if (err) *err = "net_cfg.ini 不存在";
        return false;
    }
    if (!fi.isWritable()) {
        if (err) *err = "net_cfg.ini 不可写";
        return false;
    }

    QSettings set(iniPath, QSettings::IniFormat);
    set.setIniCodec("UTF-8");

    // 可选写 [ftp]
    if (!remotePath.isEmpty() || !localPath.isEmpty()) {
        set.beginGroup("ftp");
        if (!remotePath.isEmpty()) set.setValue("remote_path", remotePath);
        if (!localPath.isEmpty())  set.setValue("local_path", localPath);
        set.endGroup();
    }

    // 可选写 [stream]
    if (!streamUrl.isEmpty()) {
        set.beginGroup("stream");
        set.setValue("url", streamUrl);
        set.endGroup();
    }

    set.sync();
    if (set.status() != QSettings::NoError) {
        if (err) *err = "写入 ini 失败";
        return false;
    }
    return true;
}


/** @brief 将客户端 ID、流主题和订阅主题写入网络配置。 */
bool MqttService::writeMqttIni(const QString& clientId,
                               const QString& streamUrlTopic,
                               const QString& subTopics,
                               QString* err)
{
    QDir dir(Rk3566Platform::netConfigDir());
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (err) *err = "无法创建平台配置目录";
            return false;
        }
    }

    QSettings set(Rk3566Platform::netConfigPath(), QSettings::IniFormat);
    set.setIniCodec("UTF-8");

    set.beginGroup("mqtt");
    if (!clientId.trimmed().isEmpty())
        set.setValue("client_id", clientId.trimmed());

//    if (!streamUrlTopic.trimmed().isEmpty())
//        set.setValue("stream_url_topic", streamUrlTopic.trimmed());

//    if (!subTopics.trimmed().isEmpty())
//        set.setValue("sub_topics", subTopics.trimmed());

    set.endGroup();

    set.sync();
    if (set.status() != QSettings::NoError) {
        if (err) *err = "写入 mqtt ini 失败（权限或磁盘问题）";
        return false;
    }

    return true;
}

/**
 * @brief 构造本地视频及图片文件列表 JSON 数组，并保存ID到路径的映射。
 *
 * 扫描固定媒体目录，并生成如下结构数组：
 * @code
 * [
 *   {"id":1, "name":"xxx"},
 *   {"id":2, "name":"yyy"}
 * ]
 * @endcode
 *
 * 对于文件名编码异常的情况，会尝试使用 GBK 重新解码。
 *
 * @return 本地媒体文件数组；若目录不存在则返回空数组。
 */
QJsonArray MqttService::buildLocalVideoFileArray()
{
    QJsonArray arr;
    advertisedPlayFiles_.clear();
    hasPlayFileSnapshot_ = true;
    int idx = 1;
    for (const LocalPlayFile &file : localPlayFiles()) {
        advertisedPlayFiles_.insert(idx, file);
        arr.append(QJsonObject{{"id", idx++}, {"name", file.name}});
    }
    return arr;
}

QList<MqttService::LocalPlayFile> MqttService::localPlayFiles() const
{
    QList<LocalPlayFile> files;

    // 固定媒体目录分别扫描；小图位于pic，仍沿用原来的id/name字段。
    QFileInfoList list;
    for (const QString &directory : {videoDir,
                                    MediaContentPolicy::pictureDir(Rk3566Platform::staticRoot())}) {
        const bool pictureDirectory = directory != videoDir;
        const auto entries = QDir(directory).entryInfoList(
                    QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &file : entries) {
            if (MediaContentPolicy::isImage(file.fileName()) ||
                (!pictureDirectory && MediaContentPolicy::isVideo(file.fileName()))) {
                list.append(file);
            }
        }
    }

    QTextCodec *gbk = QTextCodec::codecForName("GBK");

    for (const QFileInfo &fi : list) {
        QString name = fi.completeBaseName();

        // 如果 Qt 直接拿到的是 ?? 或明显异常，尝试按 GBK 重解
        if ((name.contains('?') || name.isEmpty()) && gbk) {
            QByteArray raw = QFile::encodeName(fi.fileName());
            QString repaired = gbk->toUnicode(raw);
            QString repairedBase = QFileInfo(repaired).completeBaseName();
            if (!repairedBase.isEmpty()) {
                name = repairedBase;
            }
        }

        files.append(LocalPlayFile{name, fi.absoluteFilePath()});
    }

    return files;
}


/**
 * @brief 按上报文件列表中的名称和ID删除媒体文件。
 *
 * ID对应最近一次上报的文件快照，删除后不重排，防止连续删除发生错位。
 * 兼容未携带ID的旧请求，但同名存在多个文件时拒绝猜测。
 *
 * @param targetName  要删除的目标文件名（不含扩展名）。
 * @param targetId    文件列表中的ID；旧版请求可以不携带。
 * @param deletedPath 输出参数，成功时返回删除文件的绝对路径。
 * @param err         输出参数，失败时返回错误描述。
 * @return true  删除成功。
 * @return false 未找到匹配文件或删除失败。
 */
bool MqttService::deleteLocalPlayFile(const QString &targetName, const QJsonValue &targetId,
                                     QString *deletedPath, QString *err)
{
    if (deletedPath) deletedPath->clear();
    if (err) err->clear();

    const QString rawName = targetName;
    const QString trimmedName = targetName.trimmed();

    if (rawName.isEmpty() && trimmedName.isEmpty()) {
        if (err) *err = "name 为空";
        return false;
    }

    LocalPlayFile selected;
    const bool hasId = !targetId.isUndefined() && !targetId.isNull() &&
            !targetId.toVariant().toString().trimmed().isEmpty();
    if (hasId) {
        bool validId = false;
        const int id = targetId.toVariant().toString().trimmed().toInt(&validId);
        if (!validId || id <= 0) {
            if (err) *err = "id 无效";
            return false;
        }
        // 启动后未查询列表时允许首次扫描；已有快照时不得重新扫描导致ID移位。
        if (!hasPlayFileSnapshot_) buildLocalVideoFileArray();
        const auto it = advertisedPlayFiles_.constFind(id);
        if (it == advertisedPlayFiles_.constEnd() ||
            (it->name != rawName && it->name.trimmed() != trimmedName)) {
            if (err) *err = "id/name 与文件列表不匹配，请重新获取文件列表";
            return false;
        }
        selected = it.value();
    } else {
        const auto files = localPlayFiles();
        QList<LocalPlayFile> matches;
        for (const LocalPlayFile &file : files)
            if (file.name == rawName) matches.append(file);
        if (matches.isEmpty()) {
            for (const LocalPlayFile &file : files)
                if (file.name.trimmed() == trimmedName) matches.append(file);
        }
        if (matches.size() != 1) {
            if (err) *err = matches.isEmpty() ? "未找到匹配文件" : "存在同名文件，需提供id";
            return false;
        }
        selected = matches.first();
    }

    if (!QFile(selected.path).remove()) {
        if (err) *err = QString("删除失败: %1").arg(selected.path);
        return false;
    }
    // 同批删除保留其余文件原ID；重复请求不能删除排到相同位置的其他文件。
    for (auto it = advertisedPlayFiles_.begin(); it != advertisedPlayFiles_.end();) {
        if (it->path == selected.path) it = advertisedPlayFiles_.erase(it);
        else ++it;
    }
    if (currentRecordedFilePath_ == selected.path) clearCurrentRecordedFile();
    if (deletedPath) *deletedPath = selected.path;
    emit playFileDeleted(selected.path);
    return true;
}


/**
 * @brief 根据请求 topic 计算默认响应 topic。
 *
 * 规则如下：
 * - 若请求 topic 以 /request 结尾，则替换为 /event
 * - 否则直接在末尾追加 /event
 *
 * @param reqTopic 请求 topic。
 * @return 响应 topic。
 */
QString MqttService::buildResponseTopicFromRequest(const QString &reqTopic)
{
    QString t = reqTopic.trimmed();
    if (t.endsWith("/request")) {
        t.chop(QString("/request").size());
        t += "/event";
        return t;
    }
    return t + "/event";
}


/**
 * @brief 构造带公共字段的业务 payload。
 *
 * 公共字段包括：
 * - method
 * - id
 * - time
 * - deviceId
 * - data
 *
 * @param method   业务方法名。
 * @param reqId    请求 ID。
 * @param deviceId 设备 ID。
 * @param dataObj  业务数据对象。
 * @return 构造后的 payload JSON 对象。
 */
QJsonObject MqttService::buildMethodPayload(const QString &method,
                                            const QString &reqId,
                                            const QString &deviceId,
                                            const QJsonObject &dataObj) const
{
    QJsonObject payload;
    payload["method"] = method;
    payload["id"] = reqId;
    payload["time"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    payload["deviceId"] = deviceId;
    payload["data"] = dataObj;
    return payload;
}


/**
 * @brief 构造带公共字段的业务 payload。
 *
 * 公共字段包括：
 * - method
 * - id
 * - time
 * - deviceId
 * - data
 *
 * @param method   业务方法名。
 * @param reqId    请求 ID。
 * @param deviceId 设备 ID。
 * @param dataObj  业务数据对象。
 * @return 构造后的 payload JSON 对象。
 */
QJsonObject MqttService::buildPublishPacket(const QString &topic,
                                            const QJsonObject &payload) const
{
    QJsonObject root;
    root["cmd"] = "publish";
    root["topic"] = topic;
    root["payload"] = payload;
    return root;
}


/**
 * @brief 输出 publish 报文调试日志。
 *
 * 统一输出：
 * - payload 的十六进制内容
 * - 整个发送报文的原始 JSON 行
 *
 * @param tag     日志标签，用于区分不同业务。
 * @param payload 业务 payload。
 * @param root    完整 publish 根包。
 */
void MqttService::logPublishPacket(const QString &tag, const QJsonObject &payload, const QJsonObject &root)
{
    const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
//    emit logMessage(QString("%1 PAYLOAD HEX: %2")
//                    .arg(tag, QString::fromLatin1(payloadBytes.toHex())));

    QByteArray line = QJsonDocument(root).toJson(QJsonDocument::Compact);
    line.append('\n');
//    emit logMessage(QString("%1 TX RAW: %2")
//                    .arg(tag, QString::fromUtf8(line)));
}


/**
 * @brief 构造统一的事件发布报文。
 *
 * 该函数统一完成以下步骤：
 * - 根据请求 topic 推导响应 topic
 * - 组装公共 payload 字段
 * - 组装 publish 根包
 * - 输出统一发送日志
 *
 * @param reqTopic     请求 topic。
 * @param method       响应 method。
 * @param reqId        请求 ID。
 * @param deviceId     设备 ID。
 * @param dataObj      业务数据对象。
 * @param respTopicOut 输出参数，返回最终响应 topic。
 * @param logTag       日志标签。
 * @return 完整 publish 报文 JSON 对象。
 */
QJsonObject MqttService::buildEventPublishPacket(const QString &reqTopic, const QString &method, const QString &reqId, const QString &deviceId,
                                                        const QJsonObject &dataObj, QString *respTopicOut, const QString &logTag)
{
    const QString respTopic = buildResponseTopicFromRequest(reqTopic);
    if (respTopicOut) {
        *respTopicOut = respTopic;
    }

    const QJsonObject payload = buildMethodPayload(method, reqId, deviceId, dataObj);
    const QJsonObject root = buildPublishPacket(respTopic, payload);

    logPublishPacket(logTag, payload, root);
    return root;
}


/**
 * @brief 处理 videoControl 类型的控制消息。
 *
 * 根据 payload 中的 data 字段解析视频类型与音量，并更新内部播放状态。
 * 支持的主要类型包括：
 * - RECORDED：配置 FTP 下载路径并触发下载任务
 * - LIVE：写入流地址并通知进入直播播放
 * - NONE / 其他：停止直播并保持本地播放模式
 *
 * @param topic      消息 topic。
 * @param payloadObj 消息 payload 对象。
 * @return true  已识别并处理 videoControl 请求。
 * @return false 当前消息不是 videoControl 请求。
 */
bool MqttService::handleVideoControlMessage(const QString& topic, const QJsonObject& payloadObj, bool *status)
{
    Q_UNUSED(topic);

    if (status) *status = false;
    playInfo_.status = "error";

    // 只处理 videoControl
    const QString method = payloadObj.value("method").toString().trimmed();
    if (method != "videoControl") return false;

    const QJsonObject dataObj = payloadObj.value("data").toObject();
    const QString videoType = dataObj.value("videoType").toString().trimmed().toUpper();

    // 仅新增广告选择项，不修改音量、直播和FTP其他参数的语义。
    if (videoType == QLatin1String("RECORDED") || dataObj.contains("contentType") ||
        dataObj.contains("smallImageMode")) {
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.setIniCodec("UTF-8");
        ini.setValue("playback/contentType", MediaContentPolicy::normalizeContentType(
                         dataObj.value("contentType").toString()));
        ini.setValue("playback/smallImageMode",
                     MediaContentPolicy::smallImageMode(dataObj.value("smallImageMode")) ? 1 : 0);
        ini.sync();
        if (ini.status() != QSettings::NoError) {
            emit logMessage(QStringLiteral("保存广告播放类型失败"));
            return true;
        }
    }

    const QString serverTime = payloadObj.value("time").toString().trimmed();
    if (!serverTime.isEmpty()) {
        QString terr;
        if (!syncSystemTimeFromServerString(serverTime, &terr)) {
            emit logMessage(QString("根据服务端 time 同步系统时间失败：%1").arg(terr));
        }
    }

    // 先清理上一次挂起的音量回执状态
    pendingVolumeAck_ = false;
    pendingVolumeAckShouldPublish_ = false;
    pendingVolumeReqTopic_.clear();
    pendingVolumeReqId_.clear();
    pendingVolumeDeviceId_.clear();
    pendingVolumeReqPayload_ = QJsonObject{};
    pendingVolumeValue_ = -1;

    // 只在真的携带 videoVolume 时才处理音量
    bool hasVideoVolume = false;
    int videoVolume = playInfo_.videoVolume;
    const QJsonValue vv = dataObj.value("videoVolume");

    if (!vv.isUndefined() && !vv.isNull()) {
        if (vv.isDouble()) {
            videoVolume = vv.toInt(playInfo_.videoVolume);
            hasVideoVolume = true;
        } else if (vv.isString()) {
            bool ok = false;
            const int t = vv.toString().trimmed().toInt(&ok);
            if (ok) {
                videoVolume = t;
                hasVideoVolume = true;
            }
        }
    }

    if (hasVideoVolume) {
        if (videoVolume < 0) videoVolume = 0;
        if (videoVolume > 100) videoVolume = 100;
    }

    QString videoPath = dataObj.value("videoPath").toString().trimmed();
    videoPath = QFileInfo(videoPath).fileName().trimmed();

    const bool isRecordedDownloadRequest =
            (videoType == "RECORDED" && !videoPath.isEmpty());

    if (hasVideoVolume) {
        emit logMessage(QString("videoVolume原始类型=%1, 解析结果=%2")
                        .arg(vv.type())
                        .arg(videoVolume));

        if (!isRecordedDownloadRequest) {
            pendingVolumeAck_ = true;
            pendingVolumeAckShouldPublish_ = true;
            pendingVolumeReqTopic_ = topic;
            pendingVolumeReqPayload_ = payloadObj;
            pendingVolumeValue_ = videoVolume;

            emit logMessage(QString("解析到 videoVolume=%1，已发出 mqttVolumeReceived，等待音量设置完成后统一回执")
                            .arg(videoVolume));
        } else {
            emit logMessage(QString("解析到 videoVolume=%1，但当前是 RECORDED 下载任务，最终只由 FTP 结果统一回执")
                            .arg(videoVolume));
        }

        emit mqttVolumeReceived(videoVolume);
    } else {
        pendingVolumeAck_ = false;
        pendingVolumeAckShouldPublish_ = false;
        emit logMessage("本次 videoControl 未携带 videoVolume，不等待音量回执");
    }

    // 记录当前播放状态
    playInfo_.videoType = videoType.isEmpty() ? "NONE" : videoType;
    if (hasVideoVolume) {
        playInfo_.videoVolume = videoVolume;
    }

    if (playInfo_.videoType != "LIVE") {
        playInfo_.videoUrl.clear();
    }
    if (playInfo_.videoType != "RECORDED") {
        currentRecordedFilePath_.clear();
        playInfo_.videoPath.clear();
    }


    // RECORDED：写 ini 并准备 FTP 下载
    if (videoType == "RECORDED") {
        playInfo_.videoType = "RECORDED";
        playInfo_.videoUrl.clear();

        // 没带 videoPath：切回录播或改音量
        if (videoPath.isEmpty()) {
            emit logMessage("RECORDED：仅切回本地录播/处理音量");

            currentRecordedFilePath_.clear();
            playInfo_.videoPath.clear();
            playInfo_.name.clear();

            emit stopLiveRequested();
            emit resumeRecordedPlaybackRequested();

            if (!hasVideoVolume) {
                if (status) *status = false;
                playInfo_.status.clear();
            }

            return true;
        }

        QString ftpHost = dataObj.value("ftpPath").toString().trimmed();
        QString ftpPort = dataObj.value("ftpPort").toString().trimmed();
        QString ftpUser = dataObj.value("ftpUser").toString().trimmed();
        QString ftpPwd  = dataObj.value("ftpPwd").toString().trimmed();
        if (ftpPwd.compare("null", Qt::CaseInsensitive) == 0) ftpPwd.clear();

        QString werr;
        if (!writeFtpIni(ftpHost, ftpPort, ftpUser, ftpPwd, &werr)) {
            emit logMessage(QString("RECORDED：写入 FTP 配置失败：%1").arg(werr));

            // FTP 没启动成功，直接立即回 error
            pendingVolumeAck_ = false;
            pendingVolumeAckShouldPublish_ = false;
            pendingVolumeReqTopic_.clear();
            pendingVolumeReqPayload_ = QJsonObject{};
            pendingVolumeValue_ = -1;

            return true;
        }

        QString remotePath = videoPath;
        if (!remotePath.startsWith('/')) remotePath.prepend('/');

        QString localDir;
        QString err;
        // 只有图片且开启小图模式才进入pic；视频始终进入固定video目录。
        const bool smallPicture = MediaContentPolicy::isImage(videoPath) &&
                MediaContentPolicy::smallImageMode(dataObj.value("smallImageMode"));
        localDir = smallPicture
                ? MediaContentPolicy::pictureDir(Rk3566Platform::staticRoot())
                : Rk3566Platform::videoDir();
        if (!QDir().mkpath(localDir)) {
            emit logMessage(QString("RECORDED：无法创建下载目录：%1").arg(localDir));

            pendingVolumeAck_ = false;
            pendingVolumeAckShouldPublish_ = false;
            pendingVolumeReqTopic_.clear();
            pendingVolumeReqPayload_ = QJsonObject{};
            pendingVolumeValue_ = -1;

            return true;
        }

        // 根据下载目录生成最终本地文件路径
        const QString localFile = QDir(localDir).filePath(videoPath);

        // 判断是否重复
        if (QFileInfo::exists(localFile)) {
            emit logMessage(QString("RECORDED：本地已存在同名文件，将覆盖：%1").arg(localFile));
        } else {
            emit logMessage(QString("RECORDED：本地不存在同名文件，将下载：%1").arg(localFile));
        }

        // 写回 ini；local_path 始终保存目录，不保存完整文件名
        if (!writeIniFtpPaths(remotePath, localDir, QString(), &err)) {
            emit logMessage(QString("RECORDED：写入 remote/local path 失败：%1").arg(err));

            pendingVolumeAck_ = false;
            pendingVolumeAckShouldPublish_ = false;
            pendingVolumeReqTopic_.clear();
            pendingVolumeReqPayload_ = QJsonObject{};
            pendingVolumeValue_ = -1;

            return true;
        }

        emit logMessage(QString("RECORDED：已写入 FTP 配置 host=%1 port=%2 user=%3")
                        .arg(ftpHost, ftpPort, ftpUser));

        // 收到新 RECORDED 任务后，先更新 deviceLog
        playInfo_.deviceLog = QStringLiteral("RECORDED Downloading：%1").arg(videoPath);
        currentRecordedFilePath_ = localFile;
        playInfo_.videoPath = videoPath;
        playInfo_.name = QFileInfo(videoPath).completeBaseName();

        // 在触发同步的本地存储预检查前先进入 downloading，避免拒绝结果
        // 已经回调后又被本函数尾部覆盖成 downloading。
        if (status) *status = true;
        playInfo_.status = "downloading";

        emit stopLiveRequested();
        emit ftpDownloadTaskReady();

        // 最终成功、终止性拒绝或可重试失败由 FTP 回调决定。
        return true;
    }

    // LIVE：写入直播流配置并通知开始直播
    if (videoType == "LIVE") {
        const QString url = dataObj.value("videoUrl").toString().trimmed();

        playInfo_.videoType = "LIVE";
        playInfo_.videoUrl = url;
        playInfo_.videoPath.clear();
        // 直播场景下 name 留空
        playInfo_.name.clear();

        if (url.isEmpty()) {
            emit logMessage("LIVE：videoUrl 为空，无法写入 stream.url");
            playInfo_.status = "error";
            return true;
        }

        QString err;
        if (!writeIniFtpPaths(QString(), QString(), url, &err)) {
            emit logMessage(QString("LIVE：写入 stream.url 失败：%1").arg(err));
            playInfo_.status = "error";
            return true;
        }

        emit logMessage(QString("LIVE：已写入 [stream].url=%1").arg(url));

        QString kind = "live";
        const QString lower = url.toLower();
        if (lower.startsWith("rtsp://")) kind = "rtsp";
        else if (lower.startsWith("rtmp://") || lower.startsWith("rtmps://")) kind = "rtmp";
        else if (lower.startsWith("srt://")) kind = "srt";
        else if (lower.startsWith("udp://")) kind = "udp";
        else if (lower.startsWith("http://") || lower.startsWith("https://")) {
            if (lower.contains(".m3u8")) kind = "hls";
            else if (lower.contains(".ts")) kind = "http-ts";
            else kind = "live";
        }

        emit liveStreamReady(url, kind);

        if (!hasVideoVolume) {
            if (status) *status = true;
            playInfo_.status = "ok";
        }

        return true;
    }

    // NONE：停止播放
    if (videoType == "NONE") {
        playInfo_.videoType = "NONE";
        playInfo_.videoUrl.clear();
        playInfo_.videoPath.clear();
        playInfo_.name.clear();
        // base64Image 先保留，便于 getPlayInfo 返回最后一帧

        emit logMessage("videoType=NONE：停止播放");
        emit stopAllPlayRequested();

        if (!hasVideoVolume) {
            if (status) *status = true;
            playInfo_.status = "ok";
        }

        return true;
    }

    playInfo_.videoType = videoType.isEmpty() ? "NONE" : videoType;
    playInfo_.videoUrl.clear();
    playInfo_.videoPath.clear();
    playInfo_.name.clear();
    // base64Image 这里先不清，方便 getPlayInfo 还能回最后一帧

    emit logMessage(QString("videoType=%1：未知类型，按停止所有播放处理")
                    .arg(playInfo_.videoType));

    //emit stopLiveRequested();
    emit stopAllPlayRequested();

    return true;
}


/**
 * @brief 处理 deletePlayFile 删除文件请求。
 *
 * 从 payload.data 中提取name和id，删除文件列表中对应的视频或图片。
 *
 * @param topic      消息 topic。
 * @param payloadObj 消息 payload 对象。
 * @return true  已识别并处理 deletePlayFile 请求。
 * @return false 当前消息不是 deletePlayFile 请求。
 */
bool MqttService::handleDeletePlayFileMessage(const QString &topic, const QJsonObject &payloadObj)
{
    Q_UNUSED(topic);

    const QString method = payloadObj.value("method").toString().trimmed();
    if (method != "deletePlayFile") return false;

    const QJsonObject dataObj = payloadObj.value("data").toObject();

    const QString name = dataObj.value("name").toString();
    const QJsonValue fileId = dataObj.value("id");

    emit logMessage(QString("收到 deletePlayFile 请求：name=%1 id=%2")
                    .arg(name, fileId.toVariant().toString()));

    QString deletedPath;
    QString err;
    if (deleteLocalPlayFile(name, fileId, &deletedPath, &err)) {
        emit logMessage(QString("deletePlayFile 删除成功：name=%1 path=%2")
                        .arg(name, deletedPath));
    } else {
        emit logMessage(QString("deletePlayFile 删除失败：name=%1 err=%2")
                        .arg(name, err));
    }

    return true;
}


/**
 * @brief 构造播放文件列表发布报文。
 *
 * 该函数会扫描本地视频目录，生成文件列表后构造 sendPlayFileList 发布消息。
 *
 * @param reqTopic     请求 topic。
 * @param reqId        请求 ID。
 * @param deviceId     设备 ID。
 * @param respTopicOut 输出参数，返回响应 topic。
 * @return 完整 publish 报文 JSON 对象。
 */
QJsonObject MqttService::buildPlayFileListPublishPacket(const QString &reqTopic,
                                                        const QString &reqId,
                                                        const QString &deviceId,
                                                        QString *respTopicOut)
{
    const QString respTopic = buildResponseTopicFromRequest(reqTopic);
    const QJsonArray files = buildLocalVideoFileArray();

    emit logMessage(QString("准备发送播放文件列表，reqTopic=%1 respTopic=%2 文件数=%3")
                    .arg(reqTopic, respTopic)
                    .arg(files.size()));

    QJsonObject dataObj;
    dataObj["file"] = files;

    return buildEventPublishPacket(reqTopic,
                                   "sendPlayFileList",
                                   reqId,
                                   deviceId,
                                   dataObj,
                                   respTopicOut,
                                   "sendPlayFileList");
}


/**
 * @brief 构造播放信息发布报文。
 *
 * 根据当前播放状态生成普通回执或不含缓存图像的高清回执元数据。
 *
 * @param reqTopic     请求 topic。
 * @param reqId        请求 ID。
 * @param deviceId     设备 ID。
 * @param respTopicOut 输出参数，返回响应 topic。
 * @param method 回执方法，普通默认 sendPlayInfo，高清为 sendPlayInfoFromCompany。
 * @param includeCachedImage 是否使用普通缓存图；高清传 false，等待后台编码补入。
 * @return 完整 publish 报文 JSON 对象。
 */
QJsonObject MqttService::buildPlayInfoPublishPacket(const QString &reqTopic,
                                                    const QString &reqId,
                                                    const QString &deviceId,
                                                    QString *respTopicOut,
                                                    const QString &method,
                                                    bool includeCachedImage)
{
    const PlayInfoState &info = playInfo_;
    // 不以同名文件反查其他目录，避免pic下载状态或旧回调污染主区域文件信息。
    const bool reportRecordedFile = !currentRecordedFilePath_.isEmpty() &&
            MediaContentPolicy::isInDirectory(currentRecordedFilePath_, videoDir) &&
            (MediaContentPolicy::isVideo(currentRecordedFilePath_) ||
             MediaContentPolicy::isImage(currentRecordedFilePath_));

    QJsonObject infoObj;
    infoObj["videoType"] = info.videoType.toLower();  // 返回 none/live/recorded
    infoObj["videoVolume"] = info.videoVolume;
    infoObj["videoPath"] = reportRecordedFile ? info.videoPath : QString();
    infoObj["videoUrl"] = (info.videoType.compare("LIVE", Qt::CaseInsensitive) == 0)
                            ? info.videoUrl
                            : QString();
    infoObj["name"] = reportRecordedFile ? info.name : QString();
    infoObj["base64Image"] = includeCachedImage ? info.base64Image : QString();
    infoObj["deviceLog"] = info.deviceLog;

    QJsonObject dataObj;
    dataObj["info"] = infoObj;

    return buildEventPublishPacket(reqTopic,
                                   method,
                                   reqId,
                                   deviceId,
                                   dataObj,
                                   respTopicOut,
                                   method);
}


/** 保留原请求 data，并按调用方选择补入执行状态和 FTP 下载结果。 */
QJsonObject MqttService::buildVideoControlAckPublishPacket(const QString &reqTopic, const QJsonObject &reqPayload, bool status,
                                                            bool ftpDownloaded, QString *respTopicOut, bool includeStatus,
                                                            bool includeFtpDownload, const QString &downloadResult,
                                                            const QString &reason)
{
    QJsonObject dataObj = reqPayload.value("data").toObject();

    // 已下发值原样回显，归一化仅用于本机配置和播放。
    if (!dataObj.contains("contentType")) dataObj["contentType"] = QStringLiteral("VIDEO_IMAGE");
    if (!dataObj.contains("smallImageMode")) dataObj["smallImageMode"] = 0;

    if (includeStatus) {
        dataObj["status"] = status ? "ok" : "error";
    }

    dataObj["deviceClientIp"] = readDeviceClientIpFromIni();

    if (includeFtpDownload) {
        dataObj["ftpDownload"] = ftpDownloaded ? "100%" : "0%";
    }

    if (!downloadResult.trimmed().isEmpty()) {
        dataObj["downloadResult"] = downloadResult.trimmed();
    }
    if (!reason.trimmed().isEmpty()) {
        dataObj["reason"] = reason.trimmed();
    }

    const QString method   = reqPayload.value("method").toString().trimmed();
    const QString reqId    = reqPayload.value("id").toString().trimmed();
    const QString deviceId = reqPayload.value("deviceId").toString().trimmed();

    return buildEventPublishPacket(reqTopic,
                                   method,        // 仍然回 videoControl
                                   reqId,
                                   deviceId,
                                   dataObj,
                                   respTopicOut,
                                   "videoControlAck");
}


/** @brief 更新录播下载状态和设备日志。 */
void MqttService::updateRecordedDownloadStatus(bool statusOk, const QString &deviceLog)
{
    playInfo_.status = statusOk ? QStringLiteral("ok") : QStringLiteral("error");
    if (!deviceLog.trimmed().isEmpty()) {
        playInfo_.deviceLog = deviceLog.trimmed();
    }
}


// 本地调音量同步
QJsonObject MqttService::buildLocalVideoControlStatusPublishPacket(const QString &reqTopic,
                                                                  const QString &deviceId,
                                                                  int volume,
                                                                  bool ok,
                                                                  QString *respTopicOut)
{
    QJsonObject dataObj;
    dataObj["videoType"] = playInfo_.videoType.toLower();
    dataObj["videoVolume"] = volume;
    dataObj["videoPath"] = playInfo_.videoPath;
    dataObj["videoUrl"] = playInfo_.videoUrl;
    dataObj["contentType"] = MediaContentPolicy::contentType(iniPath);
    dataObj["smallImageMode"] = MediaContentPolicy::smallImageMode(iniPath) ? 1 : 0;
    dataObj["status"] = ok ? "ok" : "error";
    dataObj["deviceClientIp"] = readDeviceClientIpFromIni();
    dataObj["ftpDownload"] = "0%";

    if (!playInfo_.name.isEmpty()) {
        dataObj["name"] = playInfo_.name;
    }

    const QString reqId =
        QString::number(QDateTime::currentMSecsSinceEpoch());

    return buildEventPublishPacket(reqTopic,
                                   "videoControl",
                                   reqId,
                                   deviceId,
                                   dataObj,
                                   respTopicOut,
                                   "localVideoControlStatus");
}


// LinuxReboot 回执包
QJsonObject MqttService::buildLinuxRebootAckPublishPacket(const QString &reqTopic,
                                                          const QJsonObject &reqPayload,
                                                          QString *respTopicOut)
{
    QJsonObject dataObj;
    dataObj["status"] = "ok";

    const QString method   = reqPayload.value("method").toString().trimmed();
    const QString reqId    = reqPayload.value("id").toString().trimmed();
    const QString deviceId = reqPayload.value("deviceId").toString().trimmed();

    return buildEventPublishPacket(reqTopic,
                                   method,        // linuxReboot
                                   reqId,
                                   deviceId,
                                   dataObj,
                                   respTopicOut,
                                   "linuxRebootAck");
}


/**
 * @brief 更新当前播放画面的 base64 图像数据。
 *
 * @param base64Image base64 编码图像字符串。
 */
void MqttService::updateCurrentPlayImage(const QString &base64Image)
{
    playInfo_.base64Image = base64Image;
}


/**
 * @brief 获取当前播放状态快照。
 *
 * @return 当前播放状态结构体。
 */
PlayInfoState MqttService::currentPlayInfo() const
{
    return playInfo_;
}


/**
 * @brief 设置当前录制文件。
 *
 * 若文件存在，则记录：
 * - videoPath：文件名
 * - name：不带扩展名的文件名
 *
 * 若文件不存在，则清空相关状态。
 *
 * @param fullPath 当前录制文件完整路径。
 */
void MqttService::setCurrentRecordedFile(const QString &fullPath)
{
    // 先判断来源目录，再读取文件属性；pic和其他目录不参与当前文件上报。
    if (fullPath.isEmpty() || !MediaContentPolicy::isInDirectory(fullPath, videoDir) ||
        (!MediaContentPolicy::isVideo(fullPath) && !MediaContentPolicy::isImage(fullPath))) {
        clearCurrentRecordedFile();
        return;
    }
    QFileInfo fi(fullPath);
    if (!fi.exists() || !fi.isFile()) {
        clearCurrentRecordedFile();
        return;
    }

    currentRecordedFilePath_ = fi.absoluteFilePath();
    playInfo_.videoPath = fi.fileName();        // xxx.mp4
    playInfo_.name = fi.completeBaseName();     // xxx
}


/**
 * @brief 清空当前录制文件相关状态。
 */
void MqttService::clearCurrentRecordedFile()
{
    currentRecordedFilePath_.clear();
    playInfo_.videoPath.clear();
    playInfo_.name.clear();
}


/**
 * @brief 更新当前播放音量。
 *
 * 会自动将音量限制在 [0, 100] 范围内。
 *
 * @param volume 当前音量值。
 */
void MqttService::updateCurrentPlayVolume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    playInfo_.videoVolume = volume;
}


/** @brief 更新播放信息响应中的设备日志。 */
void MqttService::updateCurrentDeviceLog(const QString &deviceLog)
{
    playInfo_.deviceLog = deviceLog.trimmed();
}

/** @brief 清空播放信息中的设备日志。 */
void MqttService::clearCurrentDeviceLog()
{
    playInfo_.deviceLog.clear();
}



/** @return 存在等待本地音量执行结果的请求时返回 true。 */
bool MqttService::hasPendingVolumeAck() const
{
    return pendingVolumeAck_;
}

/** @brief 完成挂起的音量请求并发出统一结果信号。 */
void MqttService::notifyVolumeSetFinished(bool ok, const QString &reason)
{
    if (!pendingVolumeAck_) {
        emit logMessage("音量设置完成回调到达，但当前没有挂起的音量回执");
        return;
    }

    // 更新当前播放信息里的音量状态日志
    if (ok) {
        playInfo_.deviceLog = QStringLiteral("Volume setting successful：%1").arg(pendingVolumeValue_);
        playInfo_.status = "ok";
    } else {
        playInfo_.deviceLog = reason.trimmed().isEmpty()
                ? QStringLiteral("Volume setting failed")
                : reason.trimmed();
        playInfo_.status = "error";
    }

    const bool shouldPublishAck = pendingVolumeAckShouldPublish_;
    const QString reqTopic = pendingVolumeReqTopic_;
    const QJsonObject reqPayload = pendingVolumeReqPayload_;

    pendingVolumeAck_ = false;
    pendingVolumeAckShouldPublish_ = false;
    pendingVolumeReqTopic_.clear();
    pendingVolumeReqId_.clear();
    pendingVolumeDeviceId_.clear();
    pendingVolumeValue_ = -1;
    pendingVolumeReqPayload_ = QJsonObject{};


    if (shouldPublishAck) {
        emit volumeControlResultReady(ok, reqTopic, reqPayload);
    } else {
        emit logMessage("音量设置完成：当前请求由 FTP 最终结果统一回执，不单独发送 videoControl 回执");
    }

}
