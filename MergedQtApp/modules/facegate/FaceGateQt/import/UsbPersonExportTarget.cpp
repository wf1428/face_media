/**
 * @file UsbPersonExportTarget.cpp
 * @brief 人员 XLSX 导出所需的固定U盘挂载检测、目录创建和安全卸载。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "UsbPersonExportTarget.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QThread>
#include <QUuid>

namespace {

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
    while (!mounts.atEnd()) {
        const QStringList fields = QString::fromUtf8(mounts.readLine()).trimmed()
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

/**
 * @brief 轮询固定挂载点 /media/usb0，直到取得可写的人员导出目录。
 *
 * 挂载点为只读时只尝试就地重新挂载，不枚举任何块设备。
 */
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

        const QString fixedMountPoint = QStringLiteral("/media/usb0");
        if (isMountedAt(fixedMountPoint)) {
            UsbPersonExportVolume result;
            result.mountPoint = fixedMountPoint;
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
                                         << fixedMountPoint,
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
            ? QStringLiteral("等待U盘超时，请确认U盘已挂载到 /media/usb0 且可以写入")
            : lastError;
    return result;
}

/**
 * @brief 先执行 sync，再卸载固定挂载目录。
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
