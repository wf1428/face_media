/**
 * @file ota_service.cpp
 * @brief 将 OTA 下行交给独立执行器，按持久状态回报阶段及重启结果。
 * @author Du lin
 * @date 2026-11-18
 * @note 最近修改：2026-11-18；修改人：Du lin；修复 Unix 锁路径局部变量与 name 参数重名，显式保存 QByteArray 编码后传给 open，保持锁及 OTA 流程不变；同步 Shell 执行器的依赖错误提示和锁说明，MQTT 协议与调用入口不变。
 */
#include "ota_service.h"
#include "ota_protocol.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <chrono>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStorageInfo>
#include <QRegularExpression>
#include <QUuid>
#include <QLockFile>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
/** @brief 与 Shell 安装器共用 flock，保护取消临界区及失败状态的写入所有权。 */
class TaskGate final {
public:
    /** @brief 非阻塞争用任务目录内的固定 gate；失败表示安装已进入临界区或路径不可写。
     * @param directory 已存在、由 root 控制的目录。
     * @param name 固定锁文件名，默认是取消临界锁，失败收尾使用 executor.lock。
     */
    explicit TaskGate(const QString &directory, const QString &name = QStringLiteral(".ota-cancel.lock"))
#ifndef Q_OS_UNIX
        : lock_(directory + "/" + name + QStringLiteral(".qt-lock"))
#endif
    {
#ifdef Q_OS_UNIX
        const QByteArray encodedPath = QFile::encodeName(directory + "/" + name);
        fd_ = ::open(encodedPath.constData(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        acquired_ = fd_ >= 0 && ::flock(fd_, LOCK_EX | LOCK_NB) == 0;
#else
        acquired_ = lock_.tryLock(0);
#endif
    }
    /** @brief 释放任务临界锁，Linux 保留锁文件防止 inode 分裂。 */
    ~TaskGate() {
#ifdef Q_OS_UNIX
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    /** @return 是否可在本次临界区检查状态并写入取消标记。 */
    bool acquired() const { return acquired_; }
private:
    bool acquired_ = false; /**< true 时持有取消与安装状态提交的互斥权。 */
#ifdef Q_OS_UNIX
    int fd_ = -1; /**< 自有 flock 文件描述符，析构释放。 */
#else
    QLockFile lock_; /**< 非 Linux 测试环境的互斥替身。 */
#endif
};
/** @return 进程单调时钟毫秒，不受设备校时影响。 */
qint64 monotonicMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
/** @brief 原子替换小型状态；返回 false 时保留旧文件。
 * @param path 内部文件路径。
 * @param state 不含凭据的状态。
 * @return 文件提交成功为 true。
 */
bool save(const QString &path, const QJsonObject &state)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto bytes = OtaProtocol::encode(state);
    return file.write(bytes) == bytes.size() && file.commit();
}
/** @return 生产状态根确实挂载在 userdata；测试使用独立临时根。 */
bool writableStorage(const QString &root)
{
    if (!root.startsWith(QStringLiteral("/userdata/"))) return true;
    const QStorageInfo storage(QStringLiteral("/userdata"));
    return storage.isValid() && storage.isReady() && !storage.isReadOnly()
            && QDir::cleanPath(storage.rootPath()) == QStringLiteral("/userdata");
}
/** @return 只允许固定路径使用的 boot ID；读取失败时为空。 */
QString bootId() { QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id")); return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.read(128)).trimmed() : QString(); }
/** @return 可在失败终态后由平台重新派发的错误码。 */
bool retryable(int code) { return code == -1 || code == -2 || code == -4; }
}

OtaService::OtaService(std::function<bool(const QJsonObject &)> publisher, QObject *parent)
    : OtaService(std::move(publisher), Environment(), parent) {}

OtaService::OtaService(std::function<bool(const QJsonObject &)> publisher, const Environment &environment, QObject *parent)
    : QObject(parent), publisher_(std::move(publisher)), environment_(environment)
{
    timer_.setParent(this);
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, &OtaService::poll);
    timer_.start();
}

OtaService::~OtaService()
{
    stopping_ = true;
    timer_.stop();
    if (prepare_ && prepare_->state() != QProcess::NotRunning) {
        prepare_->disconnect(this);
        prepare_->terminate();
        if (!prepare_->waitForFinished(1500)) { prepare_->kill(); prepare_->waitForFinished(1000); }
    }
    // Killing the systemd-run waiting client does not stop the independent unit.
    if (launcher_ && launcher_->state() != QProcess::NotRunning) {
        launcher_->disconnect(this); launcher_->terminate();
        if (!launcher_->waitForFinished(500)) { launcher_->kill(); launcher_->waitForFinished(500); }
    }
}

void OtaService::configure(const QString &deviceId)
{
    const bool valid = QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]+$")).match(deviceId).hasMatch();
    device_ = valid ? deviceId : QString();
}

void OtaService::setOnline(bool online)
{
    if (online_ == online) return;
    online_ = online;
    if (online) {
        lastReplayMs_ = -30000;
        if (prepare_ && !stage_.isEmpty() && request_.value("deviceId").toString() == device_)
            publish("ota.progress", {{"taskId", currentId_}, {"state", stage_}, {"percent", qMax(0, percent_)}});
        else { stage_.clear(); poll(); }
    }
}

QString OtaService::runningVersion() const
{
    QFile file(environment_.versionFile);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto value = QString::fromUtf8(file.read(256)).trimmed();
    return QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$")).match(value).hasMatch() ? value : QString();
}

QJsonObject OtaService::read(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) return {};
    return QJsonDocument::fromJson(OtaProtocol::preserveTaskIds(file.readAll())).object();
}

QString OtaService::taskPath(const QString &id) const
{
    return environment_.stateRoot + QStringLiteral("/tasks/") + id + QStringLiteral("/state.json");
}

bool OtaService::publish(const QString &method, const QJsonObject &data)
{
    if (!online_ || device_.isEmpty() || !publisher_) return false;
    const QJsonObject payload{{"method", method}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"deviceId", device_}, {"data", data}, {"time", QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))}};
    // String payload avoids cJSON and Qt 5 double conversions in mqttd. The
    // mqttd implementation recognizes ota.* in this text and enforces QoS 2.
    return publisher_({{"cmd", "publish"}, {"topic", QStringLiteral("device/ycLinux/%1/event").arg(device_)},
                       {"qos", 2}, {"retain", false}, {"ota", true},
                       {"payload", QString::fromUtf8(OtaProtocol::encode(payload))}});
}

bool OtaService::handle(const QString &topic, const QJsonObject &payload)
{
    const auto method = payload.value("method").toString();
    if (method != "ota.upgrade" && method != "ota.cancel" && method != "ota.query") return false;
    if (device_.isEmpty() || topic != QStringLiteral("device/ycLinux/%1/request").arg(device_)
            || payload.value("deviceId").toString() != device_) return true;
    auto data = payload.value("data").toObject();
    if (method == "ota.upgrade") {
        data.insert("notificationId", payload.value("id").toString().left(128));
        data.insert("deviceId", device_);
    }
    const auto id = OtaProtocol::taskId(data);
    if (method == "ota.query") {
        if (data.contains("taskId") && id.isEmpty()) return true;
        reportVersion(id);
        return true;
    }
    if (id.isEmpty()) return true; // Cannot correlate a malformed task ID safely.
    if (method == "ota.cancel") {
        if (id == currentId_) pendingRetry_ = {};
        TaskGate gate(QFileInfo(taskPath(id)).absolutePath());
        if (!gate.acquired()) return true;
        const auto state = read(taskPath(id));
        const auto phase = state.value("phase").toString();
        if (state.value("writeStarted").toBool() || phase == "installing" || phase == "awaiting_boot" || phase == "trial" || phase == "confirming" || phase == "recovering" || phase == "finished") return true;
        if (id != currentId_ && state.isEmpty()) return true;
        // The executor decides the cancellation boundary under its own lock.
        // Qt never reports -9 merely because this request file was written.
        QSaveFile file(QFileInfo(taskPath(id)).absolutePath() + QStringLiteral("/cancel"));
        QDir().mkpath(QFileInfo(file.fileName()).absolutePath());
        if (file.open(QIODevice::WriteOnly) && file.write("cancel\n") > 0 && file.commit()) {
            if (id == currentId_ && prepare_ && prepare_->state() == QProcess::Running) prepare_->terminate();
        }
        return true;
    }
    if (prepare_ || launcher_) {
        if (id != currentId_) publish("ota.result", {{"taskId", id}, {"result", "failed"}, {"resultCode", -5}, {"errorMsg", QStringLiteral("已有 OTA 任务正在执行")}});
        else if (prepare_) {
            const bool sameImage = request_.value("version") == data.value("version") && request_.value("action") == data.value("action")
                    && request_.value("md5").toString().compare(data.value("md5").toString(), Qt::CaseInsensitive) == 0 && request_.value("size") == data.value("size");
            const auto digest = QCryptographicHash::hash(QJsonDocument(data.value("ftp").toObject()).toJson(QJsonDocument::Compact), QCryptographicHash::Md5);
            if (sameImage && digest != credentialDigest_ && (stage_ == "received" || stage_ == "downloading")) {
                pendingRetry_ = data; // Only the newest issued account survives a slow attempt.
                prepare_->terminate();
            }
        }
        return true;
    }
    const auto active = read(environment_.stateRoot + "/active.json");
    const auto activeId = OtaProtocol::taskId(active);
    const auto activeState = activeId.isEmpty() ? QJsonObject() : read(taskPath(activeId));
    const auto phase = activeState.value("phase").toString();
    if (!activeState.isEmpty() && phase != "finished") {
        if (activeId == id) { currentId_ = id; progress(activeState); }
        else publish("ota.result", {{"taskId", id}, {"result", "failed"}, {"resultCode", -5}, {"errorMsg", QStringLiteral("已有 OTA 任务正在执行")}});
        return true;
    }
    const auto prior = read(taskPath(id));
    if (!prior.isEmpty()) {
        const bool sameDevice = prior.value("deviceId").toString().isEmpty() || prior.value("deviceId").toString() == device_;
        const bool same = sameDevice && prior.value("version") == data.value("version") && prior.value("md5").toString().compare(data.value("md5").toString(), Qt::CaseInsensitive) == 0
                && prior.value("action") == data.value("action") && prior.value("size") == data.value("size");
        if (!same) {
            publish("ota.result", {{"taskId", id}, {"result", "failed"}, {"resultCode", -5}, {"errorMsg", QStringLiteral("任务号已绑定其他固件")}});
            return true;
        }
        if (prior.value("result").toString() != "failed" || !retryable(prior.value("resultCode").toInt())) { replay(prior); return true; }
    }
    prepare(data);
    return true;
}

void OtaService::prepare(const QJsonObject &data)
{
    currentId_ = OtaProtocol::taskId(data);
    credentialDigest_ = QCryptographicHash::hash(QJsonDocument(data.value("ftp").toObject()).toJson(QJsonDocument::Compact), QCryptographicHash::Md5);
    request_ = {};
    for (const auto *key : {"taskId", "version", "md5", "action", "size", "policy", "notificationId", "deviceId"})
        if (data.contains(QLatin1String(key))) request_.insert(QLatin1String(key), data.value(QLatin1String(key)));
    request_.insert("taskId", currentId_);
    request_.insert("md5", data.value("md5").toString().toLower());
    output_.clear(); lastError_ = {}; prepared_ = false; stage_.clear(); percent_ = -10;
    terminalToken_.clear(); launchToken_.clear();
    const auto prior = read(taskPath(currentId_));
    const int previousPercent = prior.value("percent").toInt();
    progress({{"state", "received"}, {"percent", previousPercent}});
    if (!writableStorage(environment_.stateRoot)) {
        fail(-5, QStringLiteral("远程 OTA 需要已挂载的 /userdata 与 A/B 分区")); return;
    }
    QDir().mkpath(QFileInfo(taskPath(currentId_)).absolutePath());
    QFile::remove(QFileInfo(taskPath(currentId_)).absolutePath() + QStringLiteral("/cancel"));
    auto *process = new QProcess(this);
    prepare_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    const auto drain = [this, process] {
        if (prepare_ != process || stopping_) return;
        output_ += process->readAllStandardOutput();
        if (output_.size() > 65536) { process->kill(); output_.clear(); return; }
        int end;
        while ((end = output_.indexOf('\n')) >= 0) {
            const auto event = QJsonDocument::fromJson(output_.left(end)).object();
            output_.remove(0, end + 1);
            if (event.contains("state")) progress(event);
            if (event.value("prepared").toBool()) prepared_ = true;
            if (event.value("result").toString() == "failed") lastError_ = event;
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::started, this, [this, process, bytes = OtaProtocol::encode(data)]() mutable {
        process->setProperty("otaPid", process->processId());
        process->write(bytes); process->closeWriteChannel();
        bytes.fill(0); bytes.clear(); // Release the callback copy of the one-time credentials.
        if (!pendingRetry_.isEmpty()) process->terminate();
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, drain](int code, QProcess::ExitStatus status) {
        if (prepare_ != process || stopping_) return;
        drain();
        const auto state = read(taskPath(currentId_));
        prepare_ = nullptr;
        process->disconnect(this); process->deleteLater();
        if (!pendingRetry_.isEmpty()) {
            const auto next = pendingRetry_; pendingRetry_ = {};
            if (status == QProcess::NormalExit && code == 0 && prepared_) { poll(); return; }
            if (state.value("phase").toString() != "finished") {
                TaskGate owner(environment_.stateRoot, QStringLiteral("executor.lock"));
                if (!owner.acquired()) { poll(); return; }
                auto interrupted = state.isEmpty() ? request_ : state;
                interrupted.insert("phase", "finished"); interrupted.insert("result", "failed");
                interrupted.insert("resultCode", -1); interrupted.insert("percent", qMax(state.value("percent").toInt(), qMax(0, percent_)));
                // This replaced attempt has no result publication: the platform
                // already issued a new generation for the same device task.
                save(taskPath(currentId_), interrupted);
                save(environment_.stateRoot + "/active.json", {{"taskId", currentId_}});
            }
            prepare(next); return;
        }
        if (status != QProcess::NormalExit || code != 0 || !prepared_) {
            if (state.value("phase").toString() != "finished"
                    || state.value("preparePid").toDouble() != process->property("otaPid").toDouble()) {
                // Parse a safe executor error even if preflight failed before state creation.
                const auto error = lastError_;
                const bool canceled = QFileInfo::exists(QFileInfo(taskPath(currentId_)).absolutePath() + QStringLiteral("/cancel"));
                const int fallbackCode = status == QProcess::CrashExit ? -1 : -5;
                fail(canceled ? -9 : error.value("resultCode").toInt(fallbackCode), canceled ? QStringLiteral("平台已取消升级") : error.value("errorMsg").toString(QStringLiteral("OTA 执行器异常退出，请检查脚本与系统命令依赖")));
            }
        }
        poll();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || prepare_ != process) return;
        prepare_ = nullptr; process->disconnect(this); process->deleteLater();
        pendingRetry_ = {};
        fail(-5, QStringLiteral("无法启动 /usr/local/sbin/update_ab_system.sh"));
        poll();
    });
    process->start(environment_.script, {QStringLiteral("--ota-prepare")});
}

void OtaService::fail(int code, const QString &message)
{
    const bool storage = writableStorage(environment_.stateRoot);
    TaskGate owner(environment_.stateRoot, QStringLiteral("executor.lock"));
    if (storage && !owner.acquired()) return; // An independent executor owns the state.
    auto state = read(taskPath(currentId_));
    if (state.isEmpty()) state = request_;
    state.remove("ftp");
    for (auto it = request_.begin(); it != request_.end(); ++it) state.insert(it.key(), it.value());
    state.insert("taskId", currentId_);
    state.insert("phase", "finished"); state.insert("result", "failed");
    state.insert("resultCode", code); state.insert("errorMsg", message);
    state.insert("percent", qMax(state.value("percent").toInt(), qMax(0, percent_))); state.insert("beforeBootId", bootId());
    if (storage && save(taskPath(currentId_), state)) {
        const auto index = read(environment_.stateRoot + "/active.json");
        const auto id = OtaProtocol::taskId(index);
        if (id.isEmpty() || id == currentId_ || read(taskPath(id)).value("phase").toString() == "finished")
            save(environment_.stateRoot + "/active.json", {{"taskId", currentId_}});
    }
    replay(state);
}

void OtaService::progress(const QJsonObject &state)
{
    const auto stage = state.value("state").toString();
    const int percent = qBound(0, state.value("percent").toInt(), 100);
    if (stage == stage_ && percent < percent_ + 5) return;
    const int next = qMax(percent, qMax(0, percent_));
    if (request_.value("deviceId").toString().isEmpty() || request_.value("deviceId").toString() == device_)
        publish("ota.progress", {{"taskId", currentId_}, {"state", stage}, {"percent", next}});
    stage_ = stage; percent_ = next;
    QString text;
    if (stage == "received") text = QStringLiteral("已收到系统升级任务");
    else if (stage == "downloading") text = QStringLiteral("正在下载系统升级包");
    else if (stage == "verifying") text = QStringLiteral("正在校验和解压系统升级包");
    else if (stage == "installing") text = QStringLiteral("正在安装系统更新，请勿断电");
    else if (stage == "rebooting") text = QStringLiteral("系统更新已写入，即将重启验证");
    else return;
    emit notice(text, next, false, false);
}

void OtaService::reportVersion(const QString &id)
{
    QJsonObject data{{"version", runningVersion()}, {"reason", "query_reply"}};
    if (!id.isEmpty()) data.insert("taskId", id);
    const auto activeId = OtaProtocol::taskId(read(environment_.stateRoot + "/active.json"));
    const auto state = activeId.isEmpty() ? QJsonObject() : read(taskPath(activeId));
    if (state.value("result").toString() == "success" && state.value("actualVersion").toString() == runningVersion()) data.insert("md5", state.value("md5"));
    publish("ota.version", data);
}

void OtaService::replay(const QJsonObject &state)
{
    const auto id = OtaProtocol::taskId(state);
    if (id.isEmpty() || state.value("result").toString().isEmpty()) return;
    if (!state.value("deviceId").toString().isEmpty() && state.value("deviceId").toString() != device_) return;
    QJsonObject data{{"taskId", id}, {"result", state.value("result")}, {"resultCode", state.value("resultCode")}};
    if (!state.value("errorMsg").toString().isEmpty()) data.insert("errorMsg", state.value("errorMsg"));
    if (state.value("result").toString() == "success") data.insert("md5", state.value("md5"));
    const bool queued = publish("ota.result", data);
    // Send version only after result was accepted in the same ordered IPC queue.
    if (queued && !state.value("outcomeBootId").toString().isEmpty()) {
        QJsonObject version{{"taskId", id}, {"version", runningVersion()}, {"reason", "after_ota"}};
        if (state.value("result").toString() == "success" && state.value("actualVersion").toString() == runningVersion()) version.insert("md5", state.value("md5"));
        publish("ota.version", version);
    }
    if (id != currentId_) return;
    const auto token = id + ':' + state.value("result").toString() + ':' + QString::number(state.value("resultCode").toInt());
    if (token == terminalToken_) return;
    terminalToken_ = token;
    const bool success = state.value("result").toString() == "success";
    const bool restored = state.value("result").toString() == "rolled_back";
    const auto text = success ? QStringLiteral("系统升级成功，当前版本：%1").arg(runningVersion())
                    : restored ? QStringLiteral("系统升级未通过，已恢复原版本：%1").arg(runningVersion())
                    : QStringLiteral("系统升级失败：%1").arg(state.value("errorMsg").toString());
    emit notice(text, success ? 100 : qMax(0, percent_), true, success);
}

void OtaService::poll()
{
    if (stopping_ || device_.isEmpty() || prepare_) return;
    const auto index = read(environment_.stateRoot + "/active.json");
    const auto id = OtaProtocol::taskId(index);
    if (id.isEmpty()) return;
    const auto state = read(taskPath(id));
    if (state.isEmpty()) return;
    if (!state.value("deviceId").toString().isEmpty() && state.value("deviceId").toString() != device_) return;
    if (currentId_ != id) { currentId_ = id; stage_.clear(); percent_ = -10; terminalToken_.clear(); }
    const auto phase = state.value("phase").toString();
    if (phase == "finished") {
        percent_ = qMax(percent_, state.value("percent").toInt());
        if (terminalToken_.isEmpty() || monotonicMs() - lastReplayMs_ >= 30000) {
            replay(state); lastReplayMs_ = monotonicMs();
        }
        return;
    }
    if (phase == "trial" || phase == "confirming" || phase == "recovering") {
        const auto text = phase == "recovering" ? QStringLiteral("系统升级验证未通过，正在恢复原分区") : QStringLiteral("系统更新已启动，正在验证运行状态");
        if (stage_ != phase) { stage_ = phase; emit notice(text, 99, false, false); }
        return;
    }
    if ((phase == "received" || phase == "downloading" || phase == "verifying" || phase == "extracting")
            && !QFileInfo::exists(QStringLiteral("/proc/%1").arg(state.value("preparePid").toInt()))) {
        request_ = state; fail(-1, QStringLiteral("下载准备进程已中断，等待平台重试")); return;
    }
    progress(state);
    if (phase != "prepared" || launcher_) return;
    const auto token = id + ':' + QString::number(state.value("attempt").toInt());
    if (token == launchToken_) return;
    launchToken_ = token;
    request_ = state;
    auto *process = new QProcess(this);
    launcher_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::readyReadStandardOutput, process, [process] { process->readAllStandardOutput(); });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        if (launcher_ != process || stopping_) return;
        launcher_ = nullptr; process->deleteLater();
        const auto state = read(taskPath(currentId_));
        if ((code != 0 || status != QProcess::NormalExit)
                && (state.value("phase").toString() == "prepared" || state.value("phase").toString() == "installing"))
            fail(-5, QStringLiteral("独立 OTA 安装服务启动失败"));
        // A failed waiting client never overwrites a running critical write.
        poll();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || launcher_ != process || stopping_) return;
        launcher_ = nullptr; process->deleteLater(); fail(-5, QStringLiteral("systemd-run 不可用"));
    });
    process->start(environment_.systemdRun, {QStringLiteral("--unit=face-media-ota-install"), QStringLiteral("--wait"), QStringLiteral("--collect"),
        QStringLiteral("--property=Type=exec"), QStringLiteral("--property=StandardOutput=null"), QStringLiteral("--property=StandardError=null"),
        environment_.script, QStringLiteral("--ota-install"), id});
}
