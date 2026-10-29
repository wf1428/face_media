/**
 * @file multimedia_update.cpp
 * @brief 后台执行 FTP 模板资源下载、大小/MD5 校验和提交屏障后的目录覆盖。
 * @author Du lin
 * @date 2026-10-24
 * @note 最近修改：2026-10-24；修改人：Du lin；清单未提供 logo/net_cfg.ini 时清理 logo 的其他内容并保留原配置，明确提供时才覆盖并同步配置镜像；其余下载、回执及重启流程不变。
 */
#include "multimedia_update.h"
#include "common/ftp_transfer.h"
#include "common/resource_update_lock.h"
#include "common/storage_policy.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QDebug>
#include <cmath>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
/** @brief 只在工作线程内传播的协议错误，不携带账号或密码。 */
struct UpdateError {
    QString code; /**< 可发平台的错误码。 */
    QString message; /**< 可发平台的错误说明。 */
};
/**
 * @brief 以受控错误码终止当前后台步骤，由 run 统一收尾。
 * @param code 可上报的平台错误码。
 * @param message 不包含认证信息的可读说明。
 */
[[noreturn]] void fail(const QString &code, const QString &message) { throw UpdateError{code, message}; }

/**
 * @brief 验证无空段、穿越、隐藏段、驱动器语法或控制字符的相对文件路径。
 * @param path 待检查的清单路径或 FTP 根路径。
 * @return 所有目录段合法时为 true，否则为 false。
 */
bool relativePath(const QString &path)
{
    if (path.isEmpty() || path.startsWith('/') || path.contains('\\') || path.contains(':')) return false;
    for (const QString &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.startsWith('.')) return false;
        for (const QChar ch : part)
            if (ch.unicode() < 32 || ch.unicode() == 127) return false;
    }
    return true;
}

/**
 * @brief 拒绝目标路径及其祖先的符号链接和非目录祖先。
 * @param path 受控绝对路径，允许目标尚未创建。
 * @note 资源写入方共用文件锁；本函数不承诺防御不遵守锁的外部进程同时换路径。
 */
void safeAncestors(QString path)
{
    for (;;) {
        const QFileInfo info(path);
        if (info.isSymLink()) fail("INVALID_PATH", QStringLiteral("资源路径不允许符号链接"));
        const QString parent = info.absolutePath();
        if (parent == path) break;
        const QFileInfo ancestor(parent);
        if (ancestor.exists() && !ancestor.isDir()) fail("INVALID_PATH", QStringLiteral("资源路径的父级不是目录"));
        path = parent;
    }
}

/**
 * @brief Unix 检查文件/目录 fsync 结果；非 Unix 不执行额外系统同步。
 * @param path 已存在的普通文件或目录。
 * @param directory true 表示目录，false 表示普通文件。
 * @note 同步失败抛出内部 UpdateError，由工作线程统一转为 LOCAL_IO_ERROR。
 */
void syncPath(const QString &path, bool directory = false)
{
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    if (fd < 0) fail("LOCAL_IO_ERROR", QStringLiteral("资源同步打开失败"));
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) fail("LOCAL_IO_ERROR", QStringLiteral("资源写盘失败"));
#else
    Q_UNUSED(path);
    Q_UNUSED(directory);
#endif
}

/**
 * @brief 同步运行受控的重启调度命令；仅工作线程调用。
 * @param command 程序路径和独立参数，不经过 shell。
 * @return 正常退出且状态码为 0 时为 true；缺失、启动失败、超时或非零退出为 false。
 * @note 启动最多等 5 秒，完成最多等 15 秒，超时后的终止最多再等 2 秒。
 */
bool scheduleRestart(const QStringList &command)
{
    if (command.isEmpty()) return false;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(command.first(), command.mid(1));
    if (!process.waitForStarted(5000)) return false;
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(2000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

MultimediaUpdate::MultimediaUpdate(const MultimediaUpdateOptions &options, QObject *parent)
    : QThread(parent), options_(options) {}

void MultimediaUpdate::authorizeCommit(bool submitted)
{
    QMutexLocker guard(&decisionMutex_);
    if (decided_) return;
    submitted_ = submitted;
    decided_ = true;
    decisionReady_.wakeAll();
}

void MultimediaUpdate::run()
{
    bool acknowledged = false;
    int lastPercent = -1;
    QString current;
    const auto interrupted = [this]() {
        if (isInterruptionRequested()) fail("CANCELLED", QStringLiteral("资源更新已取消"));
    };
    const auto report = [this, &lastPercent, &current](int percent, const QString &path) {
        if (percent != lastPercent || path != current) {
            lastPercent = percent;
            current = path;
            emit progress(percent, path);
        }
    };
    try {
        const QJsonObject data = options_.data;
        const QJsonObject ftp = data.value("ftp").toObject();
        const QJsonArray files = data.value("files").toArray();
        const QString host = ftp.value("host").toString();
        const QString user = ftp.value("user").toString();
        const QString password = ftp.value("pwd").toString();
        const QJsonValue portValue = ftp.value("port");
        bool portOk = false;
        const int port = portValue.isString() ? portValue.toString().toInt(&portOk)
                                             : portValue.toInt(-1);
        if (portValue.isDouble()) portOk = portValue.toDouble() == port;
        QString remoteRoot = ftp.value("root").toString();
        if (remoteRoot.startsWith('/')) remoteRoot.remove(0, 1);
        if (remoteRoot.endsWith('/')) remoteRoot.chop(1);
        if (data.value("action").toString() != "apply" || data.value("target").toString() != "Ycest" ||
            !data.value("configVersion").isString() || data.value("configVersion").toString().trimmed().isEmpty() ||
            !QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9.-]*$")).match(host).hasMatch() ||
            !portOk || port < 1 || port > 65535 || user.isEmpty() || !ftp.value("pwd").isString() ||
            user.contains(':') || user.contains('\n') || password.contains('\n') ||
            !relativePath(remoteRoot) || files.isEmpty() || files.size() > 20000)
            fail("INVALID_REQUEST", QStringLiteral("资源包参数不完整或无效"));

        QSet<QString> paths;
        QSet<QString> uiDirectories;
        qint64 total = 0;
        const QRegularExpression md5(QStringLiteral("^[0-9a-fA-F]{32}$"));
        for (const QJsonValue &value : files) {
            interrupted();
            const QJsonObject file = value.toObject();
            const QString path = file.value("path").toString();
            const double size = file.value("size").toDouble(-1);
            if (!relativePath(path) || paths.contains(path.toCaseFolded()) || !file.value("size").isDouble() ||
                !std::isfinite(size) || size < 0 || std::floor(size) != size || size > 9007199254740991.0 - total ||
                !md5.match(file.value("md5").toString()).hasMatch())
                fail("INVALID_REQUEST", QStringLiteral("资源文件清单无效：%1").arg(path.left(200)));
            paths.insert(path.toCaseFolded());
            total += qint64(size);
            const QString first = path.section('/', 0, 0);
            // 与 U 盘一致：仅清理实际提供的 UI 一级目录；video/person 不清空。
            if (path.contains('/') && first != "video" && first != "person") uiDirectories.insert(first);
        }
        for (const QString &path : paths) {
            QString parent = path;
            while (parent.contains('/')) {
                parent = parent.left(parent.lastIndexOf('/'));
                if (paths.contains(parent)) fail("INVALID_REQUEST", QStringLiteral("清单存在文件与目录冲突"));
            }
        }
        const QString root = QDir::cleanPath(QFileInfo(options_.root).absoluteFilePath());
        safeAncestors(root);
        if (!QDir().mkpath(root)) fail("LOCAL_IO_ERROR", QStringLiteral("无法创建资源目录"));
        ResourceUpdateLock lock;
        if (!lock.acquire(root)) fail("BUSY", QStringLiteral("已有资源更新或媒体下载任务"));
        const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
        if (curl.isEmpty()) fail("FTP_DOWNLOAD_FAILED", QStringLiteral("系统缺少 curl"));
        if (options_.restart.isEmpty() || QStandardPaths::findExecutable(options_.restart.first()).isEmpty())
            fail("RESTART_UNAVAILABLE", QStringLiteral("Qt 重启调度程序不可用"));
        for (const QString &argument : options_.restart)
            if (argument.endsWith("/weston-control") && !QFileInfo(argument).isExecutable())
                fail("RESTART_UNAVAILABLE", QStringLiteral("Qt 资源生效控制程序不可用"));
        QStorageInfo storage(root);
        storage.refresh();
        const qint64 reserve = 64LL * 1024 * 1024;
        if (!storage.isReady() || storage.isReadOnly() || total > storage.bytesAvailable() - reserve)
            fail("NO_SPACE", QStringLiteral("资源包暂存空间不足或目录只读"));
        qint64 mediaDelta = 0;
        for (const QJsonValue &value : files) {
            const QJsonObject file = value.toObject();
            const QString path = file.value("path").toString();
            const QString target = QDir(root).filePath(path);
            safeAncestors(target);
            const QFileInfo info(target);
            if (info.exists() && !info.isFile()) fail("INVALID_PATH", QStringLiteral("文件目标不是普通文件：%1").arg(path));
            if (path.startsWith("video/")) mediaDelta += qint64(file.value("size").toDouble()) - (info.exists() ? info.size() : 0);
        }
        if (mediaDelta > 0 && StoragePolicy::mediaUsage().bytes > StoragePolicy::mediaLimitBytes() - mediaDelta)
            fail("NO_SPACE", QStringLiteral("资源包中的媒体超过设备容量限制"));
        QTemporaryDir stage(root + QStringLiteral("/.multimedia-update-XXXXXX"));
        if (!stage.isValid()) fail("LOCAL_IO_ERROR", QStringLiteral("无法创建资源暂存目录"));
        qint64 received = 0;
        report(0, QString());
        for (const QJsonValue &value : files) {
            interrupted();
            const QJsonObject file = value.toObject();
            const QString path = file.value("path").toString();
            const qint64 expected = qint64(file.value("size").toDouble());
            const QString staged = QDir(stage.path()).filePath(path);
            const QString part = QDir(stage.path()).filePath(QStringLiteral(".download.part"));
            if (!QDir().mkpath(QFileInfo(staged).absolutePath())) fail("LOCAL_IO_ERROR", QStringLiteral("无法创建资源暂存子目录"));
            // -v 不用于资源任务，避免在日志输出临时 FTP 凭据。
            QStringList args{QStringLiteral("--ftp-ssl"), QStringLiteral("--ftp-pasv"),
                             QStringLiteral("--insecure"), QStringLiteral("--silent"), QStringLiteral("--show-error"),
                             QStringLiteral("--globoff"), QStringLiteral("--user"), user + ':' + password,
                             QStringLiteral("--output"), part, QStringLiteral("--connect-timeout"), QStringLiteral("8"),
                             QStringLiteral("--speed-limit"), QStringLiteral("1"), QStringLiteral("--speed-time"), QStringLiteral("60"),
                             QStringLiteral("--max-filesize"), QString::number(expected),
                             FtpTransfer::url(host, port, remoteRoot + '/' + path)};
            QProcess process;
            process.setProcessChannelMode(QProcess::MergedChannels);
            FtpTransfer::start(&process, curl, args);
            if (!process.waitForStarted(5000)) fail("FTP_DOWNLOAD_FAILED", QStringLiteral("FTP 下载进程启动失败"));
            report(total ? int(100.0 * received / total) : 0, path);
            while (!process.waitForFinished(40)) {
                process.readAll(); // 持续清空有限诊断输出，不转发含服务器内容的原始文本。
                const qint64 count = QFileInfo(part).size();
                if (isInterruptionRequested() || count > expected) {
                    process.kill();
                    process.waitForFinished(2000);
                    interrupted();
                    fail("SIZE_MISMATCH", path + QStringLiteral(" 大小超出清单"));
                }
                report(total ? qBound(0, int(100.0 * (received + count) / total), 100) : 0, path);
            }
            interrupted();
            if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 63)
                fail("SIZE_MISMATCH", path + QStringLiteral(" 大小超出清单"));
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
                fail("FTP_DOWNLOAD_FAILED", path + QStringLiteral(" FTP 下载失败（%1）").arg(process.exitCode()));
            QFile input(part);
            if (!input.open(QIODevice::ReadOnly)) fail("LOCAL_IO_ERROR", path + QStringLiteral(" 无法读取暂存文件"));
            if (input.size() != expected) fail("SIZE_MISMATCH", path + QStringLiteral(" 大小校验失败"));
            report(total ? int(100.0 * (received + expected) / total) : 100, path);
            QCryptographicHash hash(QCryptographicHash::Md5);
            while (!input.atEnd()) {
                interrupted();
                const QByteArray block = input.read(65536);
                if (block.isEmpty() && input.error() != QFile::NoError) fail("LOCAL_IO_ERROR", path + QStringLiteral(" 校验读取失败"));
                hash.addData(block);
            }
            input.close();
            if (hash.result().toHex() != file.value("md5").toString().toLower().toLatin1())
                fail("MD5_MISMATCH", path + QStringLiteral(" 校验失败"));
            syncPath(part);
            if (!QFile::rename(part, staged)) fail("LOCAL_IO_ERROR", path + QStringLiteral(" 暂存文件就绪失败"));
            received += expected;
            report(total ? int(100.0 * received / total) : 100, path);
        }
        QDirIterator directories(stage.path(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (directories.hasNext()) syncPath(directories.next(), true);
        syncPath(stage.path(), true);
        syncPath(root, true);
        emit prepared();
        {
            QMutexLocker guard(&decisionMutex_);
            // IPC 断线时保留完整暂存包，主线程重试提交；退出时显式取消并唤醒。
            // 不单独超时产生 failed，避免与主线程正在提交的 ok 形成两个终态。
            while (!decided_ && !isInterruptionRequested())
                decisionReady_.wait(&decisionMutex_, 200);
            if (!decided_ || !submitted_) fail("ACK_SUBMIT_FAILED", QStringLiteral("成功回执未提交，保留原资源"));
            acknowledged = true;
        }
        // 成功回执提交后才允许修改正式资源；不读取 rollback，也不再产生第二个终态。
        interrupted();
        // files 是本次发布的完整清单；不因服务器存在未列出的文件而删除设备配置。
        bool suppliesNetConfig = false;
        for (const QJsonValue &value : files)
            if (value.toObject().value("path").toString() == QLatin1String("logo/net_cfg.ini"))
                suppliesNetConfig = true;
        for (const QString &name : uiDirectories) {
            const QString target = QDir(root).filePath(name);
            safeAncestors(target);
            if (name == QLatin1String("logo") && !suppliesNetConfig) {
                // 保留根层 net_cfg.ini 本身；其他文件、隐藏项和子目录照常清理。
                const auto entries = QDir(target).entryInfoList(
                    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
                for (const QFileInfo &entry : entries) {
                    interrupted();
                    if (entry.fileName() == QLatin1String("net_cfg.ini")) continue;
                    const bool removed = entry.isDir() && !entry.isSymLink()
                            ? QDir(entry.absoluteFilePath()).removeRecursively()
                            : QFile::remove(entry.absoluteFilePath());
                    if (!removed)
                        fail("APPLY_FAILED", QStringLiteral("logo 资源清理失败：%1").arg(entry.fileName()));
                }
            } else if (QFileInfo::exists(target) && !QDir(target).removeRecursively())
                fail("APPLY_FAILED", QStringLiteral("UI 资源目录清理失败：%1").arg(name));
        }
        for (const QJsonValue &value : files) {
            interrupted();
            const QString path = value.toObject().value("path").toString();
            const QString target = QDir(root).filePath(path);
            safeAncestors(target);
            if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
                (QFileInfo::exists(target) && !QFile::remove(target)) ||
                !QFile::rename(QDir(stage.path()).filePath(path), target))
                fail("APPLY_FAILED", QStringLiteral("资源覆盖失败：%1").arg(path));
            syncPath(target);
            syncPath(QFileInfo(target).absolutePath(), true);
        }
        // 仅本次明确提供新网络配置时同步镜像；保留旧配置时也保留原镜像。
        if (!options_.netConfigMirror.isEmpty() && suppliesNetConfig) {
            const QString source = QDir(root).filePath("logo/net_cfg.ini");
            if (QFileInfo(source).isFile()) {
                safeAncestors(options_.netConfigMirror);
                if (!QDir().mkpath(QFileInfo(options_.netConfigMirror).absolutePath()) ||
                    (QFileInfo::exists(options_.netConfigMirror) && !QFile::remove(options_.netConfigMirror)) ||
                    !QFile::copy(source, options_.netConfigMirror))
                    qWarning() << "[multimediaControl] net_cfg.ini mirror failed";
                else syncPath(options_.netConfigMirror);
            }
        }
        syncPath(root, true);
        if (!scheduleRestart(options_.restart)) fail("RESTART_FAILED", QStringLiteral("资源已覆盖，但 Qt 重启调度失败"));
        emit completed(true, acknowledged, QString(), QString());
    } catch (const UpdateError &error) {
        emit completed(false, acknowledged, error.code, error.message);
    } catch (const std::exception &) {
        emit completed(false, acknowledged, "LOCAL_IO_ERROR", QStringLiteral("资源更新发生内部错误"));
    }
}
