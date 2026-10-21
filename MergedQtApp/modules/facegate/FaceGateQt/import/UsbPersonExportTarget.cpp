/**
 * @file UsbPersonExportTarget.cpp
 * @brief 人员 XLSX 导出预约、脚本挂载确认、目录创建和安全卸载。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "UsbPersonExportTarget.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>
#include <QThread>
#include <QUuid>

namespace {
const QString kExportControlDirectory = QStringLiteral("/run/face-media");
const QString kExportRequestPath = kExportControlDirectory
        + QStringLiteral("/person-export.request");
const QString kExportReadyPath = kExportControlDirectory
        + QStringLiteral("/person-export.ready");

QStringList controlFileLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList lines;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.isEmpty()) lines.append(line);
    }
    return lines;
}

/** @brief 还原 /proc/mounts 对空格、制表符和反斜杠的八进制转义。 */
QString decodeMountField(QString value)
{
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return value;
}

bool isMountedAt(const QString &mountPoint)
{
    QFile mounts(QStringLiteral("/proc/mounts"));
    if (!mounts.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    // procfs 文件可能报告大小为0；先读取，不能用 QFile::atEnd() 决定是否读取。
    for (const QByteArray &line : mounts.readAll().split('\n')) {
        const QStringList fields = QString::fromUtf8(line).trimmed()
                .split(QLatin1Char(' '), QString::SkipEmptyParts);
        if (fields.size() >= 2 && decodeMountField(fields.at(1)) == mountPoint) return true;
    }
    return false;
}

/** @brief 执行有限时长的系统挂载命令，并保留标准错误作为失败原因。 */
bool runProcess(const QString &program, const QStringList &arguments, QString *error)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        if (error) *error = QStringLiteral("无法启动 %1：%2").arg(program, process.errorString());
        return false;
    }
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error) *error = QStringLiteral("%1 执行超时").arg(program);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            const QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            *error = detail.isEmpty() ? QStringLiteral("%1 执行失败").arg(program) : detail;
        }
        return false;
    }
    return true;
}

/** @brief 创建 Ycest/person 目录并通过临时探针文件验证介质确实可写。 */
bool preparePersonDirectory(const QString &mountPoint,
                            QString *personDirectory,
                            QString *error)
{
    const QString directory = QDir(mountPoint).absoluteFilePath(QStringLiteral("Ycest/person"));
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("无法在U盘创建 Ycest/person 文件夹");
        return false;
    }

    const QString probePath = QDir(directory).absoluteFilePath(
                QStringLiteral(".facegate_write_test_%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile probe(probePath);
    if (!probe.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("U盘不可写：%1").arg(probe.errorString());
        return false;
    }
    probe.write("ok", 2);
    probe.close();
    probe.remove();
    if (personDirectory) *personDirectory = directory;
    return true;
}

} // namespace

void UsbPersonExportTarget::clearExportRequestOnStartup()
{
    // 仅在应用入口调用，不能在每次点击导出时清理正在使用的预约。
    for (const QString &path : {kExportRequestPath, kExportReadyPath,
                               kExportControlDirectory + QStringLiteral("/person-export.lock")}) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            qWarning() << "无法清理遗留人员导出标记:" << path;
        }
    }
}

/** @brief 在插盘前原子发布导出请求；与现有导入预约使用独立文件。 */
QString UsbPersonExportTarget::beginExportRequest(QString *error)
{
    if (!QDir().mkpath(kExportControlDirectory)) {
        if (error) *error = QStringLiteral("无法创建U盘导出控制目录");
        return {};
    }
    QLockFile lock(kExportControlDirectory + QStringLiteral("/person-export.lock"));
    if (!lock.tryLock(0)) {
        if (error) *error = QStringLiteral("已有人员导出请求，请稍后重试");
        return {};
    }
    const QStringList previous = controlFileLines(kExportRequestPath);
    if (previous.size() >= 2) {
        bool validPid = false;
        const qint64 pid = previous.at(0).toLongLong(&validPid);
        if (validPid && pid > 0 &&
            (pid == QCoreApplication::applicationPid() ||
             QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid)))) {
            if (error) *error = QStringLiteral("已有人员导出预约，请先完成或取消");
            return {};
        }
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QFile::remove(kExportReadyPath);
    QSaveFile request(kExportRequestPath);
    if (!request.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法创建人员导出请求：%1").arg(request.errorString());
        return {};
    }
    const QByteArray content = QByteArray::number(QCoreApplication::applicationPid())
            + '\n' + token.toUtf8() + '\n';
    if (request.write(content) != content.size() || !request.commit()) {
        if (error) *error = QStringLiteral("无法提交人员导出请求：%1").arg(request.errorString());
        return {};
    }
    return token;
}

/** @brief 只清理自己的预约，旧任务结束不会误删新任务令牌。 */
void UsbPersonExportTarget::endExportRequest(const QString &requestToken)
{
    if (requestToken.isEmpty()) return;
    const QStringList request = controlFileLines(kExportRequestPath);
    if (request.size() >= 2 && request.at(1) == requestToken) QFile::remove(kExportRequestPath);
    const QStringList ready = controlFileLines(kExportReadyPath);
    if (!ready.isEmpty() && ready.at(0) == requestToken) QFile::remove(kExportReadyPath);
}

/**
 * @brief 只使用 udisk watcher 为本次导出令牌确认的实际挂载点。
 *
 * 挂载点为只读时只尝试就地重新挂载，不枚举任何块设备。
 */
UsbPersonExportVolume UsbPersonExportTarget::waitForWritableVolume(
        int timeoutMs,
        const std::atomic_bool *cancelled,
        const QString &requestToken)
{
    if (requestToken.trimmed().isEmpty()) {
        UsbPersonExportVolume result;
        result.error = QStringLiteral("人员导出预约无效，请重新点击导出");
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    QString lastError;
    while (timer.elapsed() < timeoutMs) {
        if (cancelled && cancelled->load()) {
            UsbPersonExportVolume result;
            result.error = QStringLiteral("人员导出已取消");
            return result;
        }

        const QStringList request = controlFileLines(kExportRequestPath);
        if (request.size() < 2 || request.at(1) != requestToken) {
            UsbPersonExportVolume result;
            result.error = QStringLiteral("人员导出预约已失效，请重新点击导出");
            return result;
        }
        const QStringList ready = controlFileLines(kExportReadyPath);
        QString mountPoint;
        if (ready.size() >= 2 && ready.at(0) == requestToken) {
            if (ready.at(1) == QLatin1String("BUSY")) {
                UsbPersonExportVolume result;
                result.error = ready.size() >= 3 ? ready.at(2)
                        : QStringLiteral("U盘正在执行其他任务，请拔盘后重新预约导出");
                return result;
            }
            mountPoint = QDir::cleanPath(decodeMountField(ready.at(1)));
        }
        const bool usbMount = mountPoint == QLatin1String("/media/usb0") ||
                mountPoint.startsWith(QStringLiteral("/media/usb0/"));
        if (usbMount && isMountedAt(mountPoint)) {
            UsbPersonExportVolume result;
            result.mountPoint = mountPoint;
            if (preparePersonDirectory(result.mountPoint,
                                       &result.personDirectory,
                                       &result.error)) {
                result.ok = true;
                return result;
            }
            QString remountError;
            if (runProcess(QStringLiteral("mount"),
                           QStringList() << QStringLiteral("-o")
                                         << QStringLiteral("remount,rw")
                                         << mountPoint,
                           &remountError)
                    && preparePersonDirectory(result.mountPoint,
                                              &result.personDirectory,
                                              &result.error)) {
                result.ok = true;
                return result;
            }
            if (!remountError.isEmpty()) {
                result.error += QStringLiteral("；重新挂载为可写失败：%1").arg(remountError);
            }
            lastError = result.error;
        }
        QThread::msleep(500);
    }

    UsbPersonExportVolume result;
    result.error = lastError.isEmpty()
            ? QStringLiteral("等待U盘导出确认超时，请确认 udisk 服务已更新，并在点击导出后插入可写U盘")
            : lastError;
    return result;
}

/**
 * @brief 先执行 sync，再卸载脚本确认的挂载目录。
 *
 * 即使 sync 报错仍尝试卸载，以优先释放系统资源；最终把同步失败返回调用方。
 */
bool UsbPersonExportTarget::flushAndUnmount(const UsbPersonExportVolume &volume,
                                            QString *error)
{
    if (volume.mountPoint.isEmpty()) return true;
    QString syncError;
    const bool synced = runProcess(QStringLiteral("sync"), QStringList(), &syncError);
    QString unmountError;
    if (!runProcess(QStringLiteral("umount"),
                    QStringList() << volume.mountPoint,
                    &unmountError)) {
        if (error) *error = QStringLiteral("U盘卸载失败：%1").arg(unmountError);
        return false;
    }
    if (!synced) {
        if (error) *error = QStringLiteral("U盘数据同步失败：%1").arg(syncError);
        return false;
    }
    return true;
}
