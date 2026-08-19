/**
 * @file UsbPersonExportTarget.cpp
 * @brief 人员XLSX导出所需的U盘检测、可写挂载、目录创建和安全卸载。
 */

#include "UsbPersonExportTarget.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QUuid>

namespace {

struct MountedVolume
{
    QString device;
    QString mountPoint;
};

QString decodeMountField(QString value)
{
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return value;
}

QList<MountedVolume> mountedUsbVolumes()
{
    QList<MountedVolume> result;
    QFile mounts(QStringLiteral("/proc/mounts"));
    if (!mounts.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    while (!mounts.atEnd()) {
        const QStringList fields = QString::fromUtf8(mounts.readLine()).trimmed()
                .split(QLatin1Char(' '), QString::SkipEmptyParts);
        if (fields.size() < 2) continue;
        const QString device = decodeMountField(fields.at(0));
        if (!device.startsWith(QStringLiteral("/dev/sd"))) continue;
        MountedVolume volume;
        volume.device = device;
        volume.mountPoint = decodeMountField(fields.at(1));
        result.append(volume);
    }
    return result;
}

QStringList unmountedUsbDevices(const QList<MountedVolume> &mounted)
{
    QSet<QString> mountedDevices;
    for (const MountedVolume &volume : mounted) mountedDevices.insert(volume.device);
    QStringList partitions;
    QStringList wholeDevices;
    const QDir blockDir(QStringLiteral("/sys/class/block"));
    const QFileInfoList entries = blockDir.entryInfoList(
                QStringList() << QStringLiteral("sd*"),
                QDir::Dirs | QDir::NoDotAndDotDot,
                QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString device = QStringLiteral("/dev/") + entry.fileName();
        if (mountedDevices.contains(device) || !QFileInfo::exists(device)) continue;
        if (QFileInfo::exists(entry.absoluteFilePath() + QStringLiteral("/partition"))) {
            partitions.append(device);
        } else {
            wholeDevices.append(device);
        }
    }
    return partitions.isEmpty() ? wholeDevices : partitions;
}

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

bool preparePersonDirectory(const QString &mountPoint,
                            QString *personDirectory,
                            QString *error)
{
    const QString directory = QDir(mountPoint).absoluteFilePath(QStringLiteral("person"));
    if (!QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("无法在U盘根目录创建 person 文件夹");
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

UsbPersonExportVolume UsbPersonExportTarget::waitForWritableVolume(
        int timeoutMs,
        const std::atomic_bool *cancelled)
{
    QElapsedTimer timer;
    timer.start();
    QString lastError;
    while (timer.elapsed() < timeoutMs) {
        if (cancelled && cancelled->load()) {
            UsbPersonExportVolume result;
            result.error = QStringLiteral("人员导出已取消");
            return result;
        }

        const QList<MountedVolume> mounted = mountedUsbVolumes();
        for (const MountedVolume &source : mounted) {
            UsbPersonExportVolume result;
            result.device = source.device;
            result.mountPoint = source.mountPoint;
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
                                         << source.device << source.mountPoint,
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

        const QStringList devices = unmountedUsbDevices(mounted);
        for (const QString &device : devices) {
            const QString mountPoint = QStringLiteral("/mnt/facegate_person_export");
            if (!QDir().mkpath(mountPoint)) {
                lastError = QStringLiteral("无法创建U盘挂载目录：%1").arg(mountPoint);
                continue;
            }
            QString mountError;
            if (!runProcess(QStringLiteral("mount"),
                            QStringList() << QStringLiteral("-o") << QStringLiteral("rw")
                                          << device << mountPoint,
                            &mountError)) {
                lastError = QStringLiteral("U盘挂载失败：%1").arg(mountError);
                continue;
            }

            UsbPersonExportVolume result;
            result.device = device;
            result.mountPoint = mountPoint;
            result.mountedByApplication = true;
            if (!preparePersonDirectory(mountPoint,
                                        &result.personDirectory,
                                        &result.error)) {
                QString ignored;
                runProcess(QStringLiteral("umount"), QStringList() << mountPoint, &ignored);
                QDir().rmdir(mountPoint);
                lastError = result.error;
                continue;
            }
            result.ok = true;
            return result;
        }
        QThread::msleep(500);
    }

    UsbPersonExportVolume result;
    result.error = lastError.isEmpty()
            ? QStringLiteral("等待U盘超时，请确认U盘已插入且可以写入")
            : lastError;
    return result;
}

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
    if (volume.mountedByApplication) QDir().rmdir(volume.mountPoint);
    if (!synced) {
        if (error) *error = QStringLiteral("U盘数据同步失败：%1").arg(syncError);
        return false;
    }
    return true;
}
