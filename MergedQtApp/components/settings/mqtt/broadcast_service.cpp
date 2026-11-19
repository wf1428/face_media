/**
 * @file broadcast_service.cpp
 * @brief 按设备路由管理广播文件、定时播放与直播状态。
 * @author Du lin
 * @date 2026-11-05
 * @note 最近修改：2026-11-16；修改人：Du lin；广播计划、单次播放状态和缓存音频统一读写当前数据分区。
 */
#include "broadcast_service.h"
#include "broadcast_live.h"
#include "common/ftp_transfer.h"
#include "common/serial_work_queue.h"
#include "components/multimedia/gst_player_widget.h"
#include "components/multimedia/vol_ctrl.h"
#include "platform/rk3566_platform.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

namespace {
using Focus = AudioPromptFocusManager;

/** @brief 将平台音频时长换成毫秒。
 * @param[in] value JSON 秒数，可为小数。
 * @return 1..86400000 毫秒；缺失、为零或越界返回0。
 */
qint64 durationMs(const QJsonValue &value)
{
    const double seconds = value.toDouble();
    return seconds > 0 && seconds <= 86400 ? qMax<qint64>(1,qRound64(seconds * 1000)) : 0;
}

constexpr qint64 ScheduleGraceMs = 500; /**< 固定开播点允许的调度迟到毫秒数。 */
constexpr int MaxEntries = 128; /**< 清单、计划和待下载任务数量上限。 */
constexpr qint64 MaxFileBytes = 128 * 1024 * 1024; /**< 单音频大小上限，字节。 */

/** @brief 提取安全范围内的整数，非法或缺失时返回0。
 * @param[in] value JSON数字。
 * @return 0或可精确表示的正整数。
 */
qint64 number(const QJsonValue &value)
{
    const double n = value.toDouble();
    if (n < 1 || n > 9007199254740991.0) return 0;
    const auto integer = qint64(n);
    return double(integer) == n ? integer : 0;
}

/** @brief 将数字ID规范化为非空键，非法ID返回空串。
 * @param[in] value 协议ID。
 * @return 正整数的十进制字符串或空串。
 */
QString key(const QJsonValue &value)
{
    const qint64 id = number(value);
    return id ? QString::number(id) : QString();
}

/** @brief 验证有界文件描述及安全的远端路径。
 * @param[in] audio 清单中的文件描述。
 * @return true表示字段可用于缓存和传输。
 */
bool validAudio(const QJsonObject &audio)
{
    static const QRegularExpression md5(QStringLiteral("^[a-fA-F0-9]{32}$"));
    const QString path = audio.value("ftpPath").toString();
    const auto size = number(audio.value("size"));
    const auto segments = path.split('/');
    return !key(audio.value("audioId")).isEmpty() && size > 0 && size <= MaxFileBytes &&
        md5.match(audio.value("md5").toString()).hasMatch() && path.size() <= 512 &&
        !path.contains(QLatin1Char(92)) && !segments.contains("..") && !segments.contains(".") &&
        (path.startsWith("broadcast/") || path.startsWith("/broadcast/")) &&
        !audio.value("name").toString().isEmpty() && audio.value("name").toString().size() <= 200;
}

/** @brief 检查可执行的周日、时段和播放模式。
 * @param[in] plan 候选排程。
 * @return true表示排程结构有效。
 */
bool validPlan(const QJsonObject &plan)
{
    const auto start = QTime::fromString(plan.value("start").toString(), "HH:mm");
    const auto end = QTime::fromString(plan.value("end").toString(), "HH:mm");
    const auto days = plan.value("days").toArray();
    const QString mode = plan.value("playMode").toString();
    if (key(plan.value("planId")).isEmpty() || key(plan.value("audioId")).isEmpty() ||
            plan.value("mode").toString() != "SCHEDULED" || !start.isValid() || !end.isValid() ||
            start == end || days.isEmpty() || days.size() > 7 || (mode != "ONCE" && mode != "LOOP") ||
            (!plan.value("tag").toString().isEmpty() && mode == "LOOP")) return false;
    for (const auto &day : days) if (number(day) < 1 || number(day) > 7) return false;
    return true;
}

/** @brief 计算当前本地时段实例，跨午夜归开始日。
 * @param[in] plan 有效排程。
 * @param[in] now 当前本地日期和时间。
 * @param[out] startMs 可空，命中时写入时段起点的 epoch 毫秒。
 * @return 活动时段键，未命中返回空串。
 */
QString window(const QJsonObject &plan, const QDateTime &now, qint64 *startMs = nullptr)
{
    const auto start = QTime::fromString(plan.value("start").toString(), "HH:mm");
    const auto end = QTime::fromString(plan.value("end").toString(), "HH:mm");
    QDate day = now.date();
    const QTime time = now.time();
    if (start < end) {
        if (time < start || time >= end) return {};
    } else if (time < start) {
        if (time >= end) return {};
        day = day.addDays(-1);
    }
    if (!plan.value("days").toArray().contains(day.dayOfWeek())) return {};
    if (startMs) *startMs = QDateTime(day,start,Qt::LocalTime).toMSecsSinceEpoch();
    return key(plan.value("planId")) + '/' + day.toString(Qt::ISODate);
}

/** @brief 校验大小及MD5，后台调用。
 * @param[in] path 本地候选文件。
 * @param[in] size 预期字节数。
 * @param[in] md5 预期十六进制MD5。
 * @return true表示文件完整匹配。
 */
bool matches(const QString &path, qint64 size, const QString &md5)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != size) return false;
    QCryptographicHash hash(QCryptographicHash::Md5);
    return hash.addData(&file) && QString::fromLatin1(hash.result().toHex()) == md5;
}

/** @brief 在串行后台原子保存设备广播快照。
 * @param[in] directory 当前设备配置目录。
 * @param[in] bytes 已序列化的配置，不含 FTP 凭据。
 * @return 空串表示成功，否则返回失败原因。
 */
QString writeConfig(const QString &directory, const QByteArray &bytes)
{
    if (!QDir().mkpath(directory)) return QStringLiteral("无法创建广播目录");
    QSaveFile file(QDir(directory).filePath("config.json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("广播配置写入失败");
    return {};
}
}

BroadcastService::BroadcastService(Publisher publisher, QObject *parent)
    : QObject(parent), publish_(std::move(publisher)),
      files_(new SerialWorkQueue(QStringLiteral("broadcast.files"), nullptr, true)),
      liveInput_(new GstPlayerWidget), live_(new BroadcastLive)
{
    liveInput_->enablePriorityAudio();
    clock_.start();
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    auto &focus = Focus::instance();
    connect(&focus, &Focus::callFocusChanged, this, [this](bool active) {
        // 在释放边界也处理尚未被250ms定时器观察到的到点轮次。
        schedule(QDateTime::currentDateTime().addMSecs(active ? 0 : -1),true);
    });
    connect(&focus, &Focus::focusSuspended, this, [this](QObject *owner) {
        if (owner != liveInput_ || liveId_.isEmpty() || manualLivePause_ || liveReleasePending_) return;
        if (!resumeAfterCall_) { stopLive("CALL"); return; }
        callLivePause_ = true;
        liveFocusDeadline_ = 0;
        setLiveMedia(false);
        liveState("PAUSED", "CALL");
    });
    connect(&focus, &Focus::focusReady, this, [this](QObject *owner) {
        if (owner != liveInput_ || liveId_.isEmpty() || manualLivePause_ || liveReleasePending_) return;
        const auto now = clock_.elapsed();
        if (now >= liveDeadline_ || (liveFocusDeadline_ && now >= liveFocusDeadline_)) {
            stopLive("TIMEOUT");
            return;
        }
        liveFocusDeadline_ = 0;
        callLivePause_ = false;
        if (!setLiveMedia(true)) { stopLive("IPC"); return; }
        livePlaying_ = true;
        if (!liveStartId_.isEmpty()) {
            send("broadcastLiveAck", liveStartId_, {{"liveId",liveId_},{"result","OK"},{"state","PLAYING"}});
            liveStartId_.clear();
        }
        liveState("PLAYING");
    });
}

BroadcastService::~BroadcastService()
{
    online_ = false;
    stopLive({});
    stopFile(tagged_);
    stopFile(scheduled_);
    if (process_) {
        disconnect(process_, nullptr, this, nullptr);
        process_->kill();
        process_->waitForFinished(1000);
    }
    files_->shutdown();
    if (!temporary_.isEmpty()) QFile::remove(temporary_);
    live_.reset(); // 先等待工作线程关闭 ALSA 并归还输出。
    if (liveReleasePending_) Focus::instance().release(liveInput_);
    delete liveInput_;
    delete tagged_.player;
    delete scheduled_.player;
}

std::function<void(const QJsonObject &)> BroadcastService::mediaReceiver() const { return live_->receiver(); }

/**
 * @brief 切换广播设备并从当前业务分区恢复计划与播放状态。
 * @param[in] device 设备标识，用于选定广播目录。
 * @param[in] ftpHost 广播音频下载的 FTP 主机；同设备更新时只替换主机。
 */
void BroadcastService::configure(const QString &device, const QString &ftpHost)
{
    ftpHost_ = ftpHost;
    if (device_ == device) return;
    stopLive("ROUTE");
    stopFile(tagged_);
    stopFile(scheduled_);
    ++routeGeneration_;
    ++generation_;
    downloads_.clear();
    if (process_) process_->kill();
    device_ = device;
    version_ = 0;
    enabled_ = false;
    plans_ = {};
    audio_.clear();
    local_.clear();
    unavailableLogged_.clear();
    once_.clear();
    loopAnchor_.clear();
    lastLoop_.clear();
    stopped_.clear();
    seen_.clear();
    replies_.clear();
    static const QRegularExpression safeDevice(QStringLiteral("^[a-zA-Z0-9_-]{1,128}$"));
    if (!safeDevice.match(device_).hasMatch()) {
        device_.clear();
        timer_.stop();
        return;
    }
    directory_ = QDir(Rk3566Platform::persistentConfigDir())
            .filePath("broadcast/" + device_);
    QFile saved(QDir(directory_).filePath("config.json"));
    if (saved.open(QIODevice::ReadOnly) && saved.size() <= 512 * 1024) {
        const auto doc = QJsonDocument::fromJson(saved.readAll()).object();
        const auto prior = doc.value("once").toObject();
        for (auto it = prior.begin(); it != prior.end(); ++it) once_.insert(it.key(), it.value().toString());
        apply(doc, {}, true);
    }
    timer_.start();
    if (online_) state("ONLINE");
}

void BroadcastService::setScheduledEnabled(bool enabled)
{
    if (scheduledEnabled_ == enabled) return;
    scheduledEnabled_ = enabled;
    if (!enabled && !scheduled_.plan.isEmpty()) {
        stopFile(scheduled_);
        state("CONTROL");
    }
}

void BroadcastService::setOnline(bool online)
{
    if (online_ == online) return;
    online_ = online;
    if (online_) state("ONLINE");
    else stopLive("OFFLINE");
}

void BroadcastService::send(const QString &method, const QString &id, const QJsonObject &data)
{
    if (!id.isEmpty() && (method.endsWith("Ack") || method == "broadcastTagPlay")) {
        // 只保存最近已收指令的结果，旧下载完成不让去重表无限增长。
        for (const auto &entry : seen_) {
            if (entry.endsWith('/' + id)) replies_.insert(entry, {method,data});
        }
    }
    const bool canPublish = online_ && !device_.isEmpty() && publish_;
    const bool liveEvent = method == "broadcastLiveAck" || method == "broadcastLiveState";
    if (!canPublish && !liveEvent) return;
    const auto messageId = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    const bool queued = canPublish && publish_({{"cmd","publish"},{"topic","device/ycLinux/" + device_ + "/event"},
        {"payload",QJsonObject{{"method",method},{"id",messageId},{"deviceId",device_},{"data",data},
            {"time",QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")}}}});
    if (liveEvent) {
        qInfo() << "[broadcast-live] event" << "method=" << method << "id=" << messageId
                << "liveId=" << data.value("liveId").toString() << "state=" << data.value("state").toString()
                << "result=" << data.value("result").toString() << "reason=" << data.value("reason").toString()
                << "ipcQueued=" << queued << "online=" << online_;
    }
}

void BroadcastService::state(const QString &trigger, const QString &id)
{
    lastState_ = clock_.elapsed();
    const auto &slot = !tagged_.plan.isEmpty() && (tagged_.playing || scheduled_.plan.isEmpty()) ? tagged_ : scheduled_;
    QJsonArray files, queue;
    for (auto it = local_.cbegin(); it != local_.cend(); ++it) files.append(it.key().toDouble());
    for (const auto &value : plans_) {
        const auto plan = value.toObject();
        if (plan.value("enabled").toBool(true))
            queue.append(QJsonObject{{"planId",plan.value("planId")},{"audioId",plan.value("audioId")},
                {"start",plan.value("start")},{"end",plan.value("end")}});
    }
    const QString status = trigger == "ERROR" ? QStringLiteral("ERROR") : !slot.plan.isEmpty() ? (slot.playing ? "PLAYING" : "PAUSED") :
        (enabled_ ? "WAITING" : "IDLE");
    QJsonObject data{{"trigger",trigger},{"state",status},{"queue",queue},{"localAudios",files},
        {"templateExists",version_ > 0},{"cfgVersion",double(version_)}};
    if (!slot.plan.isEmpty()) {
        data.insert("audioId",slot.plan.value("audioId"));
        data.insert("planId",slot.plan.value("planId"));
        data.insert("playMode",slot.plan.value("playMode").toString("ONCE"));
        data.insert("fileName",audio_.value(key(slot.plan.value("audioId"))).value("name"));
        data.insert("progressSec",slot.position/1000);
        data.insert("durationSec",slot.duration > 0 ? slot.duration/1000 :
            audio_.value(key(slot.plan.value("audioId"))).value("durationSec").toInt());
        data.insert("volume",qBound(0,slot.plan.value("volume").toInt(80),100));
    }
    send("broadcastState",id,data);
}

void BroadcastService::apply(const QJsonObject &data, const QString &id, bool loading, bool committed)
{
    const auto version = number(data.value("cfgVersion"));
    const auto list = data.value("audios").toArray();
    const auto plans = data.value("plans").toArray();
    // 只有本地落盘的初始停用快照允许按零版本加载；完整下行仍要求正版本。
    const auto rawVersion = data.value("cfgVersion");
    const bool initialDisabled = loading && rawVersion.isDouble() && rawVersion.toDouble() == 0 &&
        !data.value("enabled").toBool() && plans.isEmpty();
    bool valid = (version > 0 || initialDisabled) && version >= version_ && data.value("enabled").isBool() &&
        data.value("audios").isArray() && data.value("plans").isArray() &&
        list.size() <= MaxEntries && plans.size() <= MaxEntries &&
        data.value("applyPolicy").toString("IMMEDIATE") == "IMMEDIATE" &&
        data.value("tagPausePolicy").toString("PAUSE_ON_TRIGGER") == "PAUSE_ON_TRIGGER";
    QHash<QString,QJsonObject> catalog;
    const auto durations = data.value("audioDuration").toObject();
    for (const auto &value : list) {
        const auto source = value.toObject();
        QJsonObject audio;
        for (const auto &name : {"audioId","name","md5","size","ftpPath","durationSec"})
            audio.insert(QLatin1String(name),source.value(QLatin1String(name)));
        const QString audioId = key(audio.value("audioId"));
        if (!validAudio(audio) || catalog.contains(audioId)) { valid = false; break; }
        const auto suppliedDuration = durations.value(audioId);
        if (durationMs(suppliedDuration) > 0) audio.insert("durationSec",suppliedDuration);
        catalog.insert(audioId,audio);
    }
    QSet<QString> ids;
    for (const auto &value : plans) {
        const auto plan = value.toObject();
        const QString planId = key(plan.value("planId"));
        if (!validPlan(plan) || ids.contains(planId)) { valid = false; break; }
        ids.insert(planId);
    }
    if (!valid) {
        if (!loading) send("broadcastAck",id,{{"result","REJECTED"},{"cfgVersion",double(version_)}});
        return;
    }
    if (version > version_ && !loading && !committed) {
        QJsonArray clean;
        for (const auto &audio : catalog) clean.append(audio);
        const QString directory = directory_;
        const auto route = routeGeneration_;
        const auto snapshot = QJsonObject{{"cfgVersion",double(version)},{"enabled",data.value("enabled")},
            {"audios",clean},{"plans",plans}};
        files_->enqueue([this,directory,route,version,id,data,snapshot] {
            if (routeGeneration_ != route) return SerialWorkQueue::Task{};
            if (version <= version_) return SerialWorkQueue::Task{{},[this,data,id](const QString &) {
                apply(data,id);
            }};
            QJsonObject current = snapshot, once;
            for (auto it = once_.cbegin(); it != once_.cend(); ++it) once.insert(it.key(),it.value());
            current.insert("once",once);
            const QByteArray bytes = QJsonDocument(current).toJson(QJsonDocument::Compact);
            return SerialWorkQueue::Task{[directory,bytes] {
                return writeConfig(directory,bytes);
            },[this,route,id,data](const QString &error) {
                if (routeGeneration_ != route) return;
                if (error.isEmpty()) apply(data,id,false,true);
                else send("broadcastAck",id,{{"result","FAILED"},{"cfgVersion",double(version_)},{"errorMsg",error}});
            }};
        });
        return;
    }
    if (version > version_ || initialDisabled) {
        ++generation_;
        downloads_.clear();
        if (process_) process_->kill();
        for (auto it = local_.begin(); it != local_.end();) {
            if (!catalog.contains(it.key()) || audio_.value(it.key()).value("md5") != catalog.value(it.key()).value("md5"))
                it = local_.erase(it);
            else ++it;
        }
        version_ = version;
        audio_ = catalog;
        unavailableLogged_.clear();
        loopAnchor_.clear();
        lastLoop_.clear();
        plans_ = plans;
        enabled_ = data.value("enabled").toBool();
        if (!enabled_ || !plans_.contains(scheduled_.plan) ||
                !local_.contains(key(scheduled_.plan.value("audioId")))) stopFile(scheduled_);
        if (!enabled_ || !local_.contains(key(tagged_.plan.value("audioId")))) stopFile(tagged_);
    }
    if (!loading) send("broadcastAck",id,{{"result","OK"},{"cfgVersion",double(version_)}});
    const auto ftp = data.value("ftp").toObject();
    for (const auto &audio : catalog) queueAudio(audio, ftp, id);
    if (!loading) state("CONFIG");
}

QByteArray BroadcastService::configBytes(qint64 version, bool enabled, const QJsonArray &plans,
                                         bool keepOnce) const
{
    QJsonArray list;
    QJsonObject once;
    for (const auto &audio : audio_) list.append(audio);
    if (keepOnce)
        for (auto it = once_.cbegin(); it != once_.cend(); ++it) once.insert(it.key(),it.value());
    return QJsonDocument(QJsonObject{{"cfgVersion",double(version)},{"enabled",enabled},
        {"audios",list},{"plans",plans},{"once",once}}).toJson(QJsonDocument::Compact);
}

void BroadcastService::clearPlans(const QJsonObject &data, const QString &id)
{
    const auto rawVersion = data.value("cfgVersion");
    const auto version = number(rawVersion);
    // 平台用数字 0 标记最后一条计划已删除。它没有顺序信息，不用于覆盖设备已有版本。
    const bool clearWithoutVersion = rawVersion.isDouble() && rawVersion.toDouble() == 0;
    const bool immediate = data.value("applyPolicy").toString("IMMEDIATE") == "IMMEDIATE";
    const auto route = routeGeneration_;
    const auto directory = directory_;
    files_->enqueue([this,version,clearWithoutVersion,immediate,route,directory,id] {
        if (routeGeneration_ != route) return SerialWorkQueue::Task{};
        if (!immediate || (!clearWithoutVersion && (version == 0 || version <= version_))) {
            const bool replay = immediate && version > 0 && version == version_ && !enabled_ && plans_.isEmpty();
            return SerialWorkQueue::Task{{},[this,id,replay](const QString &) {
                send("broadcastAck",id,{{"result",replay ? "OK" : "REJECTED"},{"cfgVersion",double(version_)}});
                if (replay) state("CONFIG");
            }};
        }

        const auto savedVersion = clearWithoutVersion ? version_ : version;
        const auto bytes = configBytes(savedVersion,false,{},false);
        return SerialWorkQueue::Task{[directory,bytes] { return writeConfig(directory,bytes); },
            [this,savedVersion,route,id](const QString &error) {
                if (routeGeneration_ != route) return;
                if (!error.isEmpty()) {
                    send("broadcastAck",id,{{"result","FAILED"},{"cfgVersion",double(version_)},{"errorMsg",error}});
                    return;
                }
                version_ = savedVersion;
                enabled_ = false;
                plans_ = {};
                unavailableLogged_.clear();
                once_.clear();
                loopAnchor_.clear();
                lastLoop_.clear();
                stopped_.clear();
                stopFile(scheduled_);
                stopFile(tagged_);
                send("broadcastAck",id,{{"result","OK"},{"cfgVersion",double(version_)}});
                state("CONFIG");
            }};
    });
}

void BroadcastService::persist()
{
    const QString directory = directory_;
    const auto route = routeGeneration_;
    files_->enqueue([this,directory,route] {
        if (routeGeneration_ != route) return SerialWorkQueue::Task{};
        // 取队列执行时的当前快照，避免异步清空计划后被旧版本重新写回。
        const auto bytes = configBytes(version_,enabled_,plans_,true);
        return SerialWorkQueue::Task{[directory,bytes] { return writeConfig(directory,bytes); },
            [](const QString &error) {
                if (!error.isEmpty()) qWarning() << "[broadcast]" << error;
            }};
    }, directory + "/config");
}

void BroadcastService::queueAudio(const QJsonObject &audio, const QJsonObject &ftp, const QString &id)
{
    Download job;
    job.request = id;
    job.id = key(audio.value("audioId"));
    job.md5 = audio.value("md5").toString().toLower();
    job.path = audio.value("ftpPath").toString();
    job.bytes = number(audio.value("size"));
    job.version = version_;
    job.generation = generation_;
    job.host = ftpHost_;
    const QString host = ftp.value("host").toString().trimmed();
    if (!host.isEmpty()) {
        // 只接受主机名或 IP。端口和路径由独立字段提供，避免把 URL 或账号解析进主机。
        QUrl endpoint;
        endpoint.setHost(host, QUrl::StrictMode);
        job.host = endpoint.isValid() ? endpoint.authority(QUrl::FullyEncoded) : QString();
    }
    job.port = ftp.value("port").toInt(8969);
    job.user = ftp.value("user").toString();
    job.password = ftp.value("pwd").toString();
    if (local_.contains(job.id)) { progress(job,"DONE",100); return; }
    if (activeDownload_.id == job.id && activeDownload_.md5 == job.md5 &&
            activeDownload_.generation == job.generation) return;
    for (const auto &item : downloads_) if (item.id == job.id && item.md5 == job.md5) return;
    if (downloads_.size() >= MaxEntries) { progress(job,"FAILED",0); return; }
    downloads_.enqueue(job);
    nextDownload();
}

void BroadcastService::nextDownload()
{
    if (!activeDownload_.id.isEmpty() || downloads_.isEmpty()) return;
    activeDownload_ = downloads_.dequeue();
    const Download job = activeDownload_;
    const auto suffix = QFileInfo(job.path).suffix().toLower();
    static const QRegularExpression safeExtension(QStringLiteral("^[a-z0-9]{1,8}$"));
    destination_ = QDir(directory_).filePath(job.id + '-' + job.md5 +
        (safeExtension.match(suffix).hasMatch() ? '.' + suffix : QStringLiteral(".audio")));
    temporary_ = destination_ + ".part";
    lastProgress_ = -1;
    const QString path = destination_;
    files_->enqueue([this,job,path] {
        return SerialWorkQueue::Task{[job,path] {
            return matches(path,job.bytes,job.md5) ? QString() : QStringLiteral("cache miss");
        }, [this,job](const QString &error) {
            if (job.generation != generation_ || audio_.value(job.id).value("md5").toString().toLower() != job.md5) {
                finishDownload(false); return;
            }
            if (error.isEmpty()) { finishDownload(true); return; }
            const QString curl = QStandardPaths::findExecutable("curl");
            if (curl.isEmpty() || job.host.isEmpty() || job.user.isEmpty() || job.password.isEmpty() ||
                    job.port < 1 || job.port > 65535 || !QDir().mkpath(directory_)) {
                finishDownload(false); return;
            }
            QFile::remove(temporary_);
            process_ = new QProcess(this);
            process_->setProcessChannelMode(QProcess::MergedChannels);
            connect(process_, &QProcess::readyRead, this, [this] { if (process_) process_->readAll(); });
            connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
                if (err == QProcess::FailedToStart) completeTransfer(false);
            });
            connect(process_, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code,QProcess::ExitStatus status) { completeTransfer(code == 0 && status == QProcess::NormalExit); });
            downloadStarted_ = clock_.elapsed();
            progress(job,"RUNNING",0);
            FtpTransfer::start(process_,curl,{"--silent","--show-error","--fail","--connect-timeout","10",
                "--max-time","300","--limit-rate","512K","--max-filesize",QString::number(job.bytes),"--user",job.user + ':' + job.password,
                "--output",temporary_,FtpTransfer::url(job.host,job.port,job.path)});
        }};
    });
}

void BroadcastService::completeTransfer(bool ok)
{
    if (!process_) return;
    disconnect(process_,nullptr,this,nullptr);
    process_->deleteLater();
    process_ = nullptr;
    if (!ok || activeDownload_.generation != generation_) { finishDownload(false); return; }
    const auto job = activeDownload_;
    const QString path = temporary_;
    files_->enqueue([this,job,path] {
        return SerialWorkQueue::Task{[job,path] {
            return matches(path,job.bytes,job.md5) ? QString() : QStringLiteral("size or MD5 mismatch");
        }, [this](const QString &error) {
            bool valid = error.isEmpty() && activeDownload_.generation == generation_ &&
                audio_.value(activeDownload_.id).value("md5").toString().toLower() == activeDownload_.md5;
            if (valid) {
                if (QFile::exists(destination_)) valid = QFile::remove(destination_);
                if (valid) valid = QFile::rename(temporary_,destination_);
            }
            finishDownload(valid);
        }};
    });
}

void BroadcastService::finishDownload(bool ok)
{
    if (activeDownload_.generation == generation_) {
        if (ok) local_.insert(activeDownload_.id,destination_);
        progress(activeDownload_,ok ? "DONE" : "FAILED",ok ? 100 : qMax(0,lastProgress_));
        if (ok) state("AUDIO_SYNC");
    }
    if (!temporary_.isEmpty()) QFile::remove(temporary_);
    activeDownload_ = {};
    temporary_.clear();
    destination_.clear();
    nextDownload();
}

void BroadcastService::progress(const Download &job, const QString &status, int progress)
{
    if (job.request.isEmpty()) return;
    send("broadcastAck",job.request,{{"result",status == "FAILED" ? "FAILED" : "OK"},
        {"cfgVersion",double(job.version)},{"audios",QJsonArray{QJsonObject{{"audioId",job.id.toDouble()},
            {"download",status},{"progress",QString::number(progress)}}}}});
}

void BroadcastService::preparePlayer(FilePlayback &slot)
{
    if (slot.player) return;
    slot.player = new GstPlayerWidget;
    auto *player = slot.player;
    player->enablePriorityAudio();
    player->setPromptAudioMode(true);
    player->setVideoOutput(nullptr);
    connect(player, &GstPlayerWidget::positionChanged, this, [&slot](int position, int duration) {
        slot.position = position;
        slot.duration = duration;
    });
    connect(player, &GstPlayerWidget::videoFinished, this, [this,&slot] {
        if (slot.plan.isEmpty()) return;
        qInfo() << "[broadcast-file] finished" << "planId=" << slot.plan.value("planId")
                << "audioId=" << slot.plan.value("audioId") << "started=" << slot.started
                << "positionMs=" << slot.position << "durationMs=" << slot.duration;
        stopFile(slot);
        state("FINISHED");
    });
    connect(player, &GstPlayerWidget::errorOccured, this, [this,&slot](const QString &error) {
        if (slot.plan.isEmpty()) return;
        qWarning() << "[broadcast-file] playback error" << "planId=" << slot.plan.value("planId")
                   << "audioId=" << slot.plan.value("audioId") << "started=" << slot.started
                   << "positionMs=" << slot.position << "error=" << error;
        if (!slot.occurrence.isEmpty()) stopped_.insert(slot.occurrence);
        if (!slot.request.isEmpty()) send("broadcastTagPlay",slot.request,{{"result","NO_AUDIO"}});
        stopFile(slot);
        state("ERROR");
    });
    auto &focus = Focus::instance();
    connect(&focus, &Focus::focusSuspended, this, [this,&slot](QObject *owner) {
        if (owner != slot.player || slot.plan.isEmpty()) return;
        qInfo() << "[broadcast-file] focus suspended" << "planId=" << slot.plan.value("planId")
                << "audioId=" << slot.plan.value("audioId") << "started=" << slot.started
                << "call=" << Focus::instance().callOwnsFocus();
        if (&slot == &scheduled_ && !slot.occurrence.isEmpty() && Focus::instance().callOwnsFocus()) {
            markOnceHandled(slot.plan,slot.occurrence);
            stopFile(slot);
            state("PREEMPTED");
            return;
        }
        slot.focusPaused = true;
        slot.playing = false;
        slot.player->pause();
        state("PREEMPTED");
    });
    connect(&focus, &Focus::focusReady, this, [this,&slot](QObject *owner) {
        if (owner != slot.player || slot.plan.isEmpty() || slot.manualPause) return;
        const auto now = QDateTime::currentDateTime();
        if (!slot.occurrence.isEmpty() && window(slot.plan,now) != slot.occurrence) {
            qInfo() << "[broadcast-file] focus ready outside window" << "planId=" << slot.plan.value("planId")
                    << "audioId=" << slot.plan.value("audioId");
            stopFile(slot);
            state("WINDOW_END");
            return;
        }
        if (!slot.started && slot.dueMs >= 0 && now.toMSecsSinceEpoch() - slot.dueMs >= ScheduleGraceMs) {
            qInfo() << "[broadcast-file] focus ready after slot" << "planId=" << slot.plan.value("planId")
                    << "audioId=" << slot.plan.value("audioId")
                    << "lateMs=" << now.toMSecsSinceEpoch() - slot.dueMs;
            stopFile(slot);
            return;
        }
        qInfo() << "[broadcast-file] focus ready" << "planId=" << slot.plan.value("planId")
                << "audioId=" << slot.plan.value("audioId") << "automatic=" << slot.automatic
                << "firstStart=" << !slot.started << "dueMs=" << slot.dueMs
                << "lateMs=" << (slot.dueMs >= 0 ? now.toMSecsSinceEpoch() - slot.dueMs : -1);
        slot.focusPaused = false;
        slot.playing = true;
        if (slot.automatic && slot.plan.value("playMode") == "LOOP" &&
                !loopAnchor_.contains(slot.occurrence)) {
            // 首次真正获焦并请求播放时才起算；后续迟到或跳过不移动锚点。
            const qint64 firstStartMs = QDateTime::currentMSecsSinceEpoch();
            loopAnchor_.insert(slot.occurrence,firstStartMs);
            lastLoop_.insert(slot.occurrence,firstStartMs);
        }
        slot.started = true;
        slot.player->play();
        if (!slot.occurrence.isEmpty()) markOnceHandled(slot.plan,slot.occurrence);
        if (!slot.request.isEmpty()) {
            send("broadcastTagPlay",slot.request,{{"result","OK"},{"audioId",slot.plan.value("audioId")},
                {"tag",slot.plan.value("tag")}});
            slot.request.clear();
        }
        state("PLAYING");
    });
}

/** @brief 申请本地文件播放；连续文件不可用时仅记录首次告警。
 * @details 去重键由计划和音频 ID 组成。文件重新可用或配置切换后清除标记，
 *          不影响调用方的失败返回及下一次调度重试。
 * @param[in,out] slot 定时或标识符文件播放项。
 * @param[in] plan 当前待播计划。
 * @param[in] occurrence 时段键；手动点播可为空。
 * @param[in] dueMs 自动循环轮次预定 epoch 毫秒；-1 表示不限制迟到。
 * @param[in] automatic true 为自动循环轮次。
 * @return true 表示已申请播放；false 表示本地文件不可用。
 */
bool BroadcastService::startFile(FilePlayback &slot, const QJsonObject &plan, const QString &occurrence,
                                 qint64 dueMs, bool automatic)
{
    const QString warningKey = key(plan.value("planId")) + '/' + key(plan.value("audioId"));
    const QString path = local_.value(key(plan.value("audioId")));
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (!unavailableLogged_.contains(warningKey)) {
            unavailableLogged_.insert(warningKey);
            qWarning() << "[broadcast-file] start unavailable" << "planId=" << plan.value("planId")
                       << "audioId=" << plan.value("audioId") << "cached=" << !path.isEmpty();
        }
        return false;
    }
    unavailableLogged_.remove(warningKey);
    qInfo() << "[broadcast-file] start requested" << "planId=" << plan.value("planId")
            << "audioId=" << plan.value("audioId") << "automatic=" << automatic
            << "dueMs=" << dueMs << "occurrence=" << occurrence;
    stopFile(slot);
    preparePlayer(slot);
    slot.plan = plan;
    slot.occurrence = occurrence;
    slot.dueMs = dueMs;
    slot.automatic = automatic;
    slot.focusPaused = true;
    slot.player->setVolume(qBound(0,plan.value("volume").toInt(80),100)/100.0f);
    slot.player->setMedia(path);
    Focus::instance().acquire(slot.player,slot.player,
        &slot == &tagged_ ? Focus::Priority::Tagged : Focus::Priority::Schedule);
    return true;
}

/** @brief 提交当前文件播放器的异步停止，随后按原顺序归还焦点。
 * @param[in,out] slot 当前文件播放项；保留播放器实例供后续轮次复用。
 * @note 本函数返回时不能据此认定播放器后台停止或共享声卡释放已完成。
 */
void BroadcastService::stopFile(FilePlayback &slot)
{
    if (slot.plan.isEmpty()) return;
    qInfo() << "[broadcast-file] stop" << "planId=" << slot.plan.value("planId")
            << "audioId=" << slot.plan.value("audioId") << "started=" << slot.started
            << "playing=" << slot.playing << "focusPaused=" << slot.focusPaused
            << "manualPause=" << slot.manualPause << "positionMs=" << slot.position;
    auto *player = slot.player;
    slot = {};
    slot.player = player;
    player->stop();
    Focus::instance().release(player);
}

void BroadcastService::markOnceHandled(const QJsonObject &plan, const QString &occurrence)
{
    const auto id = key(plan.value("planId"));
    if (plan.value("playMode") != "ONCE" || once_.value(id) == occurrence) return;
    once_.insert(id,occurrence);
    persist();
}

void BroadcastService::schedule(const QDateTime &now, bool inCall)
{
    if (!enabled_ || !scheduledEnabled_) return;
    const qint64 nowMs = now.toMSecsSinceEpoch();
    if (!scheduled_.plan.isEmpty() && !scheduled_.occurrence.isEmpty()) {
        if (inCall && !scheduled_.manualPause) {
            markOnceHandled(scheduled_.plan,scheduled_.occurrence);
            stopFile(scheduled_);
            state("PREEMPTED");
        } else if (window(scheduled_.plan,now) != scheduled_.occurrence &&
                   (!scheduled_.started || !scheduled_.playing)) {
            stopFile(scheduled_);
            state("WINDOW_END");
        } else if (!scheduled_.started && scheduled_.dueMs >= 0 &&
                   nowMs - scheduled_.dueMs >= ScheduleGraceMs) {
            stopFile(scheduled_);
            state("MISSED");
        }
    }
    if (!inCall && !scheduled_.plan.isEmpty()) return;

    const QString today = now.date().toString(Qt::ISODate);
    const QString yesterday = now.date().addDays(-1).toString(Qt::ISODate);
    for (auto it = loopAnchor_.begin(); it != loopAnchor_.end();)
        it = it.key().endsWith(today) || it.key().endsWith(yesterday) ? ++it : loopAnchor_.erase(it);
    for (auto it = lastLoop_.begin(); it != lastLoop_.end();)
        it = it.key().endsWith(today) || it.key().endsWith(yesterday) ? ++it : lastLoop_.erase(it);
    for (auto it = stopped_.begin(); it != stopped_.end();)
        it = it->endsWith(today) || it->endsWith(yesterday) ? ++it : stopped_.erase(it);

    for (const auto &value : plans_) {
        const auto plan = value.toObject();
        if (!plan.value("enabled").toBool(true)) continue;
        qint64 startMs = 0;
        const QString occurrence = window(plan,now,&startMs);
        if (occurrence.isEmpty() || stopped_.contains(occurrence) ||
                once_.value(key(plan.value("planId"))) == occurrence) continue;

        if (plan.value("playMode") == "LOOP") {
            if (nowMs < startMs) continue;
            if (!loopAnchor_.contains(occurrence)) {
                // 计划下发时若已在时段内，第一轮可立即申请；通话占用则等首次可播再起算。
                if (!inCall && !Focus::instance().blocks(Focus::Priority::Schedule) &&
                        startFile(scheduled_,plan,occurrence,-1,true)) return;
                continue;
            }
            const qint64 period = 1000LL * qBound(0,plan.value("intervalSec").toInt(),86400);
            if (period == 0) {
                // 零间隔表示上一轮结束后尽快重播，不打断仍在播放的文件。
                if (!inCall && !Focus::instance().blocks(Focus::Priority::Schedule) &&
                        startFile(scheduled_,plan,occurrence,-1,true)) return;
                continue;
            }
            const qint64 anchor = loopAnchor_.value(occurrence);
            const qint64 due = anchor + (nowMs - anchor) / period * period;
            if (lastLoop_.value(occurrence,-1) >= due || nowMs - due >= ScheduleGraceMs) continue;
            if (inCall || (!Focus::instance().blocks(Focus::Priority::Schedule) &&
                           startFile(scheduled_,plan,occurrence,due,true))) {
                lastLoop_.insert(occurrence,due);
                if (!inCall) return;
            }
        } else if (inCall) {
            markOnceHandled(plan,occurrence);
        } else if (startFile(scheduled_,plan,occurrence)) {
            return;
        }
    }
}

void BroadcastService::liveState(const QString &status, const QString &reason)
{
    lastLiveState_ = clock_.elapsed();
    QJsonObject data{{"liveId",liveId_},{"state",status},{"volume",liveVolume_},
        {"elapsedSec",int((clock_.elapsed()-liveStarted_)/1000)},
        {"ts",double(QDateTime::currentMSecsSinceEpoch())}};
    if (!reason.isEmpty()) data.insert("reason",reason);
    send("broadcastLiveState",{},data);
}

bool BroadcastService::setLiveMedia(bool enabled)
{
    if (!enabled && livePlaying_) logLiveMedia(true);
    if (!enabled) livePlaying_ = false;
    firstLiveFrameLogged_ = false;
    firstLivePcmLogged_ = false;
    const QString topic = "device/ycLinux/" + device_ + "/broadcast/audio";
    const QString epoch = enabled ? QUuid::createUuid().toString(QUuid::WithoutBraces) : QString();
    live_->configure(enabled ? topic : QString(), frameMs_, enabled, epoch);
    const bool queued = publish_ && publish_({{"cmd","broadcast_media"},{"topic",topic},{"frameMs",frameMs_},
        {"enabled",enabled},{"epoch",epoch}});
    qInfo() << "[broadcast-live] media_gate" << "liveId=" << liveId_ << "enabled=" << enabled
            << "epoch=" << epoch << "frameMs=" << frameMs_ << "ipcQueued=" << queued;
    return queued;
}

void BroadcastService::logLiveMedia(bool final)
{
    const auto stats = live_->statistics();
    const bool firstFrame = !firstLiveFrameLogged_ && stats.acceptedFrames > 0;
    const bool firstPcm = !firstLivePcmLogged_ && stats.alsaBlocks > stats.silenceBlocks;
    if (!final && !firstFrame && !firstPcm) return;
    firstLiveFrameLogged_ |= firstFrame;
    firstLivePcmLogged_ |= firstPcm;
    qInfo() << "[broadcast-live] media" << "liveId=" << liveId_ << "final=" << final
            << "firstFrame=" << firstFrame << "firstPcm=" << firstPcm
            << "ipcFrames=" << stats.ipcFrames << "acceptedFrames=" << stats.acceptedFrames
            << "alsaBlocks=" << stats.alsaBlocks << "alsaXruns=" << stats.alsaXruns
            << "writeMaxUs=" << stats.writeMaxUs << "nonzeroBlocks=" << stats.nonzeroBlocks
            << "silenceBlocks=" << stats.silenceBlocks << "jitterDrops=" << stats.jitterDrops
            << "underruns=" << stats.underruns << "peak=" << stats.peak;
}

void BroadcastService::acquireLiveFocus()
{
    if (liveReleasePending_) {
        qInfo() << "[broadcast-live] focus acquire deferred" << "liveId=" << liveId_
                << "outputPending=" << live_->outputPending();
        liveAcquirePending_ = true;
        return;
    }
    qInfo() << "[broadcast-live] focus acquire" << "liveId=" << liveId_
            << "manualPause=" << manualLivePause_ << "callPause=" << callLivePause_;
    Focus::instance().acquire(liveInput_,liveInput_,Focus::Priority::Live);
}

void BroadcastService::releaseLiveFocus()
{
    liveAcquirePending_ = false;
    if (liveReleasePending_) return;
    liveReleasePending_ = true;
    qInfo() << "[broadcast-live] focus release waiting for output" << "liveId=" << liveId_
            << "outputPending=" << live_->outputPending() << "clockMs=" << clock_.elapsed();
    finishLiveFocusHandoff();
}

void BroadcastService::finishLiveFocusHandoff()
{
    if (live_->outputPending()) {
        QTimer::singleShot(20,this,[this] { finishLiveFocusHandoff(); });
        return;
    }
    liveReleasePending_ = false;
    qInfo() << "[broadcast-live] focus release completed" << "liveId=" << liveId_
            << "reacquire=" << liveAcquirePending_ << "clockMs=" << clock_.elapsed();
    Focus::instance().release(liveInput_);
    if (liveAcquirePending_) {
        liveAcquirePending_ = false;
        if (!liveId_.isEmpty() && !manualLivePause_) {
            if (Focus::instance().callOwnsFocus()) {
                if (!resumeAfterCall_) { stopLive("CALL"); return; }
                callLivePause_ = true;
                liveFocusDeadline_ = 0;
            } else if (liveFocusDeadline_) {
                liveFocusDeadline_ = clock_.elapsed() + 3000;
            }
            acquireLiveFocus();
        }
    }
}

/** @brief 结束会话与 MQTT 媒体门，焦点归还等待后台关闭声卡。 */
void BroadcastService::stopLive(const QString &reason)
{
    if (liveId_.isEmpty()) return;
    setLiveMedia(false);
    if (!liveStartId_.isEmpty())
        send("broadcastLiveAck",liveStartId_,{{"liveId",liveId_},{"result","FAILED"},{"state","ENDED"}});
    liveState("ENDED",reason);
    liveId_.clear();
    liveStartId_.clear();
    liveFocusDeadline_ = 0;
    manualLivePause_ = false;
    callLivePause_ = false;
    releaseLiveFocus();
}

void BroadcastService::liveCommand(const QString &method, const QString &id, const QJsonObject &data)
{
    const auto requested = data.value("liveId").toString();
    qInfo() << "[broadcast-live] command" << "method=" << method << "id=" << id
            << "liveId=" << requested << "action=" << data.value("action").toString();
    auto &focus = Focus::instance();
    const auto ack = [this,&requested,&id](const QString &result,const QString &status) {
        send("broadcastLiveAck",id,{{"liveId",requested},{"result",result},{"state",status}});
    };
    if (method == "broadcastLiveStart") {
        if (!liveId_.isEmpty() || focus.callOwnsFocus()) { ack("BUSY","IDLE"); return; }
        const int frame = data.value("frameMs").toInt();
        if (requested.isEmpty() || requested.size() > 128 || data.value("source").toString("MIC") != "MIC" ||
                data.value("codec").toString() != "PCM16" || data.value("sampleRate").toInt() != 16000 ||
                data.value("channels").toInt() != 1 || (frame != 20 && frame != 40) ||
                data.value("livePolicy").toString("PREEMPT") != "PREEMPT") {
            ack("NO_SUPPORT","IDLE");
            return;
        }
        liveId_ = requested;
        liveStartId_ = id;
        frameMs_ = frame;
        liveVolume_ = qBound(0,data.value("volume").toInt(80),100);
        live_->setVolume(liveVolume_);
        resumeAfterCall_ = data.value("resumeAfterCall").toBool(true);
        liveStarted_ = clock_.elapsed();
        liveDeadline_ = liveStarted_ + 1000LL*qBound(1,data.value("maxDurationSec").toInt(3600),3600);
        liveFocusDeadline_ = liveStarted_ + 3000;
        acquireLiveFocus();
        return;
    }
    if (requested.isEmpty() || requested != liveId_) { ack("FAILED","IDLE"); return; }
    const QString action = data.value("action").toString();
    if (action == "STOP") {
        stopLive("STOP");
        ack("OK","ENDED");
        return;
    }
    if (action == "PAUSE") {
        if (!liveStartId_.isEmpty()) {
            send("broadcastLiveAck",liveStartId_,{{"liveId",liveId_},{"result","FAILED"},{"state","PAUSED"}});
            liveStartId_.clear();
        }
        manualLivePause_ = true;
        liveFocusDeadline_ = 0;
        setLiveMedia(false);
        releaseLiveFocus();
        liveState("PAUSED","PLATFORM");
    } else if (action == "RESUME") {
        if (manualLivePause_) {
            manualLivePause_ = false;
            callLivePause_ = focus.callOwnsFocus();
            liveFocusDeadline_ = callLivePause_ ? 0 : clock_.elapsed() + 3000;
            acquireLiveFocus();
        }
    } else if (action == "VOLUME" && data.value("volume").isDouble()) {
        liveVolume_ = qBound(0,data.value("volume").toInt(),100);
        live_->setVolume(liveVolume_);
    } else {
        ack("FAILED",livePlaying_ ? "PLAYING" : "PAUSED");
        return;
    }
    ack("OK",livePlaying_ ? "PLAYING" : "PAUSED");
}

bool BroadcastService::handlesMethod(const QString &method)
{
    static const QSet<QString> methods{"broadcastSet","broadcastCtrl","broadcastQuery","broadcastAudioSync",
        "broadcastTagPlay","broadcastLiveStart","broadcastLiveCtrl"};
    return methods.contains(method);
}

bool BroadcastService::handle(const QString &topic, const QJsonObject &payload)
{
    const QString method = payload.value("method").toString();
    if (!handlesMethod(method)) return false;
    if (device_.isEmpty() || topic != "device/ycLinux/" + device_ + "/request" ||
            payload.value("deviceId").toString() != device_) return true;
    QString id = payload.value("id").toString();
    if (id.size() > 128 || id.contains('/')) return true;
    if (id.isEmpty() && (method.startsWith("broadcastLive") || method == "broadcastTagPlay")) return true;
    if (method == "broadcastQuery") { state("QUERY",id); return true; }
    if (method == "broadcastCtrl" && !scheduledEnabled_) return true;
    const auto data = payload.value("data").toObject();
    if (!payload.value("data").isObject()) return true;
    if (!id.isEmpty()) {
        const QString request = method + '/' + id;
        if (seen_.contains(request)) {
            const auto reply = replies_.value(request);
            if (!reply.first.isEmpty()) send(reply.first,id,reply.second);
            return true;
        }
        seen_.append(request);
        if (seen_.size() > 64) replies_.remove(seen_.takeFirst());
    }
    if (id.isEmpty()) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (method == "broadcastSet") {
        const auto enabled = data.value("enabled");
        if (enabled.isBool() && !enabled.toBool() && !data.contains("plans") && !data.contains("audios"))
            clearPlans(data,id);
        else
            apply(data,id);
    } else if (method == "broadcastLiveStart" || method == "broadcastLiveCtrl") {
        liveCommand(method,id,data);
    } else if (method == "broadcastAudioSync") {
        const QString audioId = key(data.value("audioId"));
        const QString op = data.value("op").toString();
        if (audioId.isEmpty() || number(data.value("cfgVersion")) < version_ ||
                (op != "DROP" && (op != "ADD" || !validAudio(data) ||
                (!audio_.contains(audioId) && audio_.size() >= MaxEntries)))) {
            send("broadcastAck",id,{{"result","REJECTED"},{"cfgVersion",double(version_)}});
            return true;
        }
        if (op == "DROP" || audio_.value(audioId).value("md5") != data.value("md5")) {
            if (key(scheduled_.plan.value("audioId")) == audioId) stopFile(scheduled_);
            if (key(tagged_.plan.value("audioId")) == audioId) stopFile(tagged_);
            const QString obsolete = local_.take(audioId);
            if (!obsolete.isEmpty()) QFile::remove(obsolete);
        }
        if (op == "DROP") {
            audio_.remove(audioId);
            send("broadcastAck",id,{{"result","OK"},{"cfgVersion",double(version_)}});
        } else {
            // 只保留目录字段；FTP凭据和单条指令元数据不落盘。
            QJsonObject audio;
            for (const auto &name : {"audioId","name","md5","size","ftpPath","durationSec"})
                audio.insert(QLatin1String(name),data.value(QLatin1String(name)));
            audio_.insert(audioId,audio);
            queueAudio(audio, {{"host",data.value("ftpHost")},{"port",data.value("ftpPort")},
                              {"user",data.value("ftpUser")},{"pwd",data.value("ftpPwd")}}, id);
        }
        if (version_ > 0) persist();
        state("AUDIO_SYNC");
    } else if (method == "broadcastTagPlay") {
        const int floor = data.value("tagFloor").toInt();
        const QString direction = data.value("tagDirection").toString();
        const QString tag = data.value("tag").toString();
        const QString label = floor < 0 ? "B" + QString::number(-floor) : QString::number(floor);
        // 平台已按标识符和时段选定 audioId，点播请求不受计划配置版本约束。
        const bool valid = enabled_ && data.value("action").toString() == "START" &&
            floor >= -8 && floor <= 120 && floor != 0 &&
            (direction == "UP" || direction == "DOWN") && tag == label + '-' + direction;
        if (!valid || !local_.contains(key(data.value("audioId"))))
            send(method,id,{{"result","NO_AUDIO"},{"audioId",data.value("audioId")},{"tag",tag}});
        else if (!tagged_.plan.isEmpty() || Focus::instance().blocks(Focus::Priority::Tagged))
            send(method,id,{{"result","BUSY"},{"audioId",data.value("audioId")},{"tag",tag}});
        else if (startFile(tagged_,data,{})) tagged_.request = id;
        else send(method,id,{{"result","NO_AUDIO"},{"audioId",data.value("audioId")},{"tag",tag}});
    } else {
        const QString action = data.value("action").toString();
        const auto planId = key(data.value("planId"));
        const auto audioId = key(data.value("audioId"));
        if ((data.contains("planId") && planId.isEmpty()) || (data.contains("audioId") && audioId.isEmpty())) {
            send("broadcastAck",id,{{"result","REJECTED"},{"cfgVersion",double(version_)}});
            return true;
        }
        const auto target = [&](const QJsonObject &plan) {
            return !plan.isEmpty() && (planId.isEmpty() || key(plan.value("planId")) == planId) &&
                (audioId.isEmpty() || key(plan.value("audioId")) == audioId);
        };
        bool ok = false;
        if (action == "START" && enabled_) {
            for (const auto &entry : plans_) {
                const auto plan = entry.toObject();
                if (target(plan) && plan.value("enabled").toBool(true)) {
                    ok = startFile(scheduled_,plan,window(plan,QDateTime::currentDateTime()));
                    break;
                }
            }
        } else if (target(scheduled_.plan)) {
            if (action == "STOP") {
                if (!scheduled_.occurrence.isEmpty()) stopped_.insert(scheduled_.occurrence);
                stopFile(scheduled_);
                ok = true;
            } else if (action == "PAUSE") {
                scheduled_.manualPause = true;
                scheduled_.playing = false;
                scheduled_.player->pause();
                Focus::instance().release(scheduled_.player);
                ok = true;
            } else if (action == "RESUME") {
                scheduled_.manualPause = false;
                Focus::instance().acquire(scheduled_.player,scheduled_.player,Focus::Priority::Schedule);
                ok = true;
            }
        }
        send("broadcastAck",id,{{"result",ok ? "OK" : "REJECTED"},{"cfgVersion",double(version_)}});
        state("CONTROL");
    }
    return true;
}

void BroadcastService::tick()
{
    schedule(QDateTime::currentDateTime(),Focus::instance().callOwnsFocus());
    const auto now = clock_.elapsed();
    if (now - lastFileDiagnosticMs_ >= 5000 &&
            (!scheduled_.plan.isEmpty() || !tagged_.plan.isEmpty())) {
        lastFileDiagnosticMs_ = now;
        // 只在服务定时器汇总状态，不在 PCM 回调中写日志。
        const auto logFile = [](const char *slotName, const FilePlayback &slot) {
            if (slot.plan.isEmpty()) return;
            qInfo() << "[broadcast-file] progress" << "slot=" << slotName
                    << "planId=" << slot.plan.value("planId") << "audioId=" << slot.plan.value("audioId")
                    << "started=" << slot.started << "playing=" << slot.playing
                    << "focusPaused=" << slot.focusPaused << "manualPause=" << slot.manualPause
                    << "positionMs=" << slot.position << "durationMs=" << slot.duration;
        };
        logFile("scheduled",scheduled_);
        logFile("tagged",tagged_);
    }
    if (process_ && now-downloadStarted_ >= 300000) process_->kill();
    if (process_ && activeDownload_.bytes > 0 && activeDownload_.generation == generation_) {
        const int percent = int(qBound<qint64>(0,QFileInfo(temporary_).size(),activeDownload_.bytes)*99 /
            activeDownload_.bytes);
        if (percent >= lastProgress_ + 5) {
            lastProgress_ = percent;
            progress(activeDownload_,"RUNNING",percent);
        }
    }
    if (online_ && now-lastState_ >= 30000) state("HEARTBEAT");
    if (liveId_.isEmpty()) return;
    if (livePlaying_ && (!firstLiveFrameLogged_ || !firstLivePcmLogged_)) logLiveMedia(false);
    // 等待通话结束不消耗焦点预算。通话释放焦点后，为自动恢复另起计时。
    if (callLivePause_ && !manualLivePause_ && !liveFocusDeadline_ && !Focus::instance().callOwnsFocus())
        liveFocusDeadline_ = now + 3000;
    if (now >= liveDeadline_ || (livePlaying_ && live_->silenceMs() >= 15000) || live_->failed() ||
            (liveFocusDeadline_ && now >= liveFocusDeadline_)) {
        stopLive(live_->failed() ? "OUTPUT" : "TIMEOUT");
    } else if (now-lastLiveState_ >= 5000) {
        liveState(livePlaying_ ? "PLAYING" : "PAUSED", manualLivePause_ ? "PLATFORM" : (callLivePause_ ? "CALL" : ""));
    }
}
