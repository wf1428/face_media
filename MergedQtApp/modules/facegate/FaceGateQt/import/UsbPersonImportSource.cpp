/**
 * @file UsbPersonImportSource.cpp
 * @brief U 盘人员表导入源实现；挂载、复制和卸载均不在界面线程执行。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "UsbPersonImportSource.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QUuid>

namespace {

/** @brief 一次人员表查找使用的 USB 卷信息。 */
struct UsbVolume
{
    QString device;      /**< 块设备节点。 */
    QString mountPoint;  /**< 已有或临时挂载目录。 */
    bool mounted = false; /**< 卷当前是否处于挂载状态。 */
};

/** @brief 还原 /proc/mounts 对空格、制表符和反斜杠的八进制转义。 */
QString decodeMountField(QString value)
{
    value.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return value;
}

/** @return 当前已挂载的 /dev/sd* USB 卷。 */
QList<UsbVolume> mountedUsbVolumes()
{
    QList<UsbVolume> result;
    QFile mounts(QStringLiteral("/proc/mounts"));
    if (!mounts.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    while (!mounts.atEnd()) {
        const QString line = QString::fromUtf8(mounts.readLine()).trimmed();
        const QStringList fields = line.split(QLatin1Char(' '), QString::SkipEmptyParts);
        if (fields.size() < 2) continue;
        const QString device = decodeMountField(fields.at(0));
        if (!device.startsWith(QStringLiteral("/dev/sd"))) continue;
        UsbVolume volume;
        volume.device = device;
        volume.mountPoint = decodeMountField(fields.at(1));
        volume.mounted = true;
        result.append(volume);
    }
    return result;
}

/**
 * @brief 枚举尚未挂载的 USB 块设备。
 * @return 优先返回分区节点；没有分区时才返回整盘节点。
 */
QStringList unmountedUsbDevices(const QList<UsbVolume> &mounted)
{
    QSet<QString> mountedDevices;
    for (const UsbVolume &volume : mounted) mountedDevices.insert(volume.device);
    QStringList partitions;
    QStringList wholeDevices;
    const QDir blockDir(QStringLiteral("/sys/class/block"));
    const QFileInfoList entries = blockDir.entryInfoList(QStringList() << QStringLiteral("sd*"),
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

/** @brief 执行有限时长的挂载命令，并保留标准错误作为失败原因。 */
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

/** @return person 目录中最新的非临时 XLS/XLSX 文件路径。 */
QString newestWorkbook(const QString &mountPoint)
{
    const QDir personDir(QDir(mountPoint).absoluteFilePath(QStringLiteral("person")));
    if (!personDir.exists()) return QString();
    const QFileInfoList files = personDir.entryInfoList(
                QDir::Files | QDir::Readable, QDir::Time);
    for (const QFileInfo &file : files) {
        const QString suffix = file.suffix();
        const bool supported = suffix.compare(QStringLiteral("xls"), Qt::CaseInsensitive) == 0
                || suffix.compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) == 0;
        if (supported && !file.fileName().startsWith(QStringLiteral("~$"))) {
            return file.absoluteFilePath();
        }
    }
    return QString();
}

/** @brief 卸载指定卷；空挂载点视为无需处理。 */
bool unmountVolume(const QString &mountPoint, QString *error)
{
    if (mountPoint.isEmpty()) return true;
    return runProcess(QStringLiteral("umount"), QStringList() << mountPoint, error);
}

/**
 * @brief 将卷中最新人员表复制到本机唯一临时文件。
 * @return 包含来源文件名和临时路径的复制结果。
 */
UsbPersonImportFile copyWorkbook(const UsbVolume &volume)
{
    UsbPersonImportFile result;
    const QString workbook = newestWorkbook(volume.mountPoint);
    if (workbook.isEmpty()) {
        result.error = QStringLiteral("U盘 person 目录下未找到 xls 或 xlsx 文件");
        return result;
    }

    const QString tempDirectory = QStringLiteral("/tmp/facegate_person_import");
    if (!QDir().mkpath(tempDirectory)) {
        result.error = QStringLiteral("无法创建人员导入临时目录");
        return result;
    }
    const QFileInfo sourceInfo(workbook);
    result.sourceFileName = sourceInfo.fileName();
    result.temporaryPath = QDir(tempDirectory).absoluteFilePath(
                QStringLiteral("%1_%2.%3")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")),
                     QUuid::createUuid().toString(QUuid::WithoutBraces),
                     sourceInfo.suffix().toLower()));
    if (!QFile::copy(workbook, result.temporaryPath)) {
        result.error = QStringLiteral("复制人员表到临时目录失败");
        result.temporaryPath.clear();
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace

/**
 * @brief 轮询 U 盘并把人员表复制到本机，完成后再返回调用方。
 *
 * 未挂载设备以只读方式挂载。若卸载失败，会删除已经复制的临时文件并返回失败，
 * 避免界面误以为介质已经可以安全拔出。
 */
UsbPersonImportFile UsbPersonImportSource::waitAndCopy(int timeoutMs,
                                                       const std::atomic_bool *cancelled)
{
    QElapsedTimer timer;
    timer.start();
    QString lastError;
    while (timer.elapsed() < timeoutMs) {
        if (cancelled && cancelled->load()) {
            UsbPersonImportFile cancelledResult;
            cancelledResult.error = QStringLiteral("人员导入已取消");
            return cancelledResult;
        }

        const QList<UsbVolume> mounted = mountedUsbVolumes();
        for (const UsbVolume &volume : mounted) {
            const QString workbook = newestWorkbook(volume.mountPoint);
            if (workbook.isEmpty()) continue;
            UsbPersonImportFile result = copyWorkbook(volume);
            QString unmountError;
            if (!unmountVolume(volume.mountPoint, &unmountError)) {
                if (result.ok) {
                    QFile::remove(result.temporaryPath);
                    result.ok = false;
                    result.temporaryPath.clear();
                }
                result.error = QStringLiteral("人员表已找到，但U盘卸载失败：%1").arg(unmountError);
            }
            return result;
        }

        const QStringList devices = unmountedUsbDevices(mounted);
        for (const QString &device : devices) {
            const QString mountPoint = QStringLiteral("/mnt/facegate_person_import");
            if (!QDir().mkpath(mountPoint)) {
                lastError = QStringLiteral("无法创建U盘挂载目录：%1").arg(mountPoint);
                continue;
            }
            QString mountError;
            if (!runProcess(QStringLiteral("mount"),
                            QStringList() << QStringLiteral("-o") << QStringLiteral("ro")
                                          << device << mountPoint,
                            &mountError)) {
                lastError = QStringLiteral("U盘挂载失败：%1").arg(mountError);
                continue;
            }
            UsbVolume volume;
            volume.device = device;
            volume.mountPoint = mountPoint;
            volume.mounted = true;
            UsbPersonImportFile result = copyWorkbook(volume);
            QString unmountError;
            if (!unmountVolume(mountPoint, &unmountError)) {
                if (result.ok) {
                    QFile::remove(result.temporaryPath);
                    result.ok = false;
                    result.temporaryPath.clear();
                }
                result.error = QStringLiteral("U盘卸载失败：%1").arg(unmountError);
            }
            QDir().rmdir(mountPoint);
            if (result.ok || !result.error.contains(QStringLiteral("未找到"))) return result;
            lastError = result.error;
        }
        QThread::msleep(500);
    }

    UsbPersonImportFile result;
    result.error = lastError.isEmpty()
            ? QStringLiteral("等待U盘超时，请确认U盘已插入且根目录存在 person/*.xls 或 person/*.xlsx")
            : lastError;
    return result;
}
