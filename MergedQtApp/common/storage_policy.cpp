/**
 * @file storage_policy.cpp
 * @brief 媒体、注册照以及通行记录存储配额策略的实现。
 */

#include "storage_policy.h"

#include "platform/rk3566_platform.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

namespace {

const qint64 kGiB = 1024LL * 1024LL * 1024LL;
const qint64 kMiB = 1024LL * 1024LL;
const qint64 kMediaLimit = 2LL * kGiB;
const qint64 kRegistrationLimit = 1LL * kGiB;
const qint64 kAccessSoftLimit = 400LL * kMiB;
const qint64 kAccessHardLimit = 500LL * kMiB;
const int kAccessRetentionDays = 15;

QMutex g_storageMutex;

QString absoluteCleanPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isInside(const QString &path, const QString &root)
{
    const QString absolutePath = absoluteCleanPath(path);
    const QString absoluteRoot = absoluteCleanPath(root);
    return absolutePath == absoluteRoot
            || absolutePath.startsWith(absoluteRoot + QDir::separator());
}

QString mediaRoot()
{
    return QDir(Rk3566Platform::staticRoot()).filePath(
                QStringLiteral("demoResources/images/video"));
}

QStringList registrationRoots()
{
    return {
        QDir(Rk3566Platform::applicationRoot()).filePath(
                    QStringLiteral("enrollments")),
        QDir(QFileInfo(Rk3566Platform::databasePath()).absolutePath()).filePath(
                    QStringLiteral("network_faces"))
    };
}

StoragePolicy::Usage directoryUsage(const QString &root)
{
    StoragePolicy::Usage result;
    if (!QFileInfo(root).isDir()) {
        return result;
    }
    QDirIterator it(root,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        result.bytes += qMax<qint64>(0, info.size());
        ++result.fileCount;
    }
    return result;
}

StoragePolicy::Usage registrationUsageUnlocked()
{
    StoragePolicy::Usage result;
    for (const QString &root : registrationRoots()) {
        const StoragePolicy::Usage current = directoryUsage(root);
        result.bytes += current.bytes;
        result.fileCount += current.fileCount;
    }
    return result;
}

bool writeAtomically(const QString &targetPath,
                     const QByteArray &data,
                     QString *error)
{
    const QFileInfo targetInfo(targetPath);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        if (error) {
            *error = QStringLiteral("创建存储目录失败：%1")
                    .arg(targetInfo.absolutePath());
        }
        return false;
    }

    QSaveFile file(targetPath);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)
            || file.write(data) != data.size()
            || !file.commit()) {
        file.cancelWriting();
        if (error) {
            *error = QStringLiteral("写入文件失败：%1").arg(targetPath);
        }
        return false;
    }
    return true;
}

qint64 accessUsageBytes(const QString &snapshotRoot)
{
    return directoryUsage(snapshotRoot).bytes
            + directoryUsage(StoragePolicy::accessLogDirectory()).bytes;
}

QStringList accessDays(const QString &snapshotRoot)
{
    static const QRegularExpression dayExpression(QStringLiteral("^\\d{8}$"));
    QSet<QString> days;
    const QDir snapshotDir(snapshotRoot);
    const QFileInfoList snapshotDays = snapshotDir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot,
                QDir::Name);
    for (const QFileInfo &info : snapshotDays) {
        if (dayExpression.match(info.fileName()).hasMatch()) {
            days.insert(info.fileName());
        }
    }

    const QDir logDir(StoragePolicy::accessLogDirectory());
    const QFileInfoList logs = logDir.entryInfoList(
                QStringList() << QStringLiteral("*.log"),
                QDir::Files,
                QDir::Name);
    for (const QFileInfo &info : logs) {
        if (dayExpression.match(info.completeBaseName()).hasMatch()) {
            days.insert(info.completeBaseName());
        }
    }

    QStringList result = days.values();
    std::sort(result.begin(), result.end());
    return result;
}

bool removeAccessDay(const QString &snapshotRoot,
                     const QString &day,
                     QString *error)
{
    const QString snapshotDay = QDir(snapshotRoot).filePath(day);
    if (QFileInfo::exists(snapshotDay)
            && !QDir(snapshotDay).removeRecursively()) {
        if (error) {
            *error = QStringLiteral("清理最早一天抓拍目录失败：%1").arg(snapshotDay);
        }
        return false;
    }

    const QString logPath = QDir(StoragePolicy::accessLogDirectory())
            .filePath(day + QStringLiteral(".log"));
    if (QFileInfo::exists(logPath) && !QFile::remove(logPath)) {
        if (error) {
            *error = QStringLiteral("清理最早一天通行日志失败：%1").arg(logPath);
        }
        return false;
    }
    return true;
}

bool prepareAccessWriteUnlocked(const QString &snapshotRoot,
                                qint64 incomingBytes,
                                QString *error)
{
    if (incomingBytes < 0 || incomingBytes > kAccessHardLimit) {
        if (error) {
            *error = QStringLiteral("单条通行数据超过 500 MiB 限制");
        }
        return false;
    }

    QDir().mkpath(snapshotRoot);
    QDir().mkpath(StoragePolicy::accessLogDirectory());
    const QString today = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
    const QString firstKeptDay = QDate::currentDate()
            .addDays(-(kAccessRetentionDays - 1))
            .toString(QStringLiteral("yyyyMMdd"));

    // 每次写入最多主动清理一个过期日期，避免一次清空多个历史日期。
    bool cleanedOneDay = false;
    const QStringList retainedDays = accessDays(snapshotRoot);
    if (!retainedDays.isEmpty() && retainedDays.first() < firstKeptDay) {
        if (!removeAccessDay(snapshotRoot, retainedDays.first(), error)) {
            return false;
        }
        cleanedOneDay = true;
    }

    qint64 projected = accessUsageBytes(snapshotRoot) + incomingBytes;
    QStringList days = accessDays(snapshotRoot);

    // 到达 400 MiB 时，本次只主动清理最早的一天；后续写入会继续逐日处理。
    if (!cleanedOneDay
            && projected >= kAccessSoftLimit
            && !days.isEmpty()
            && days.first() != today) {
        if (!removeAccessDay(snapshotRoot, days.first(), error)) {
            return false;
        }
        projected = accessUsageBytes(snapshotRoot) + incomingBytes;
        days = accessDays(snapshotRoot);
    }

    // 500 MiB 是硬上限；仍超限时继续逐日清理，但绝不删除当天记录。
    while (projected > kAccessHardLimit
           && !days.isEmpty()
           && days.first() != today) {
        if (!removeAccessDay(snapshotRoot, days.first(), error)) {
            return false;
        }
        projected = accessUsageBytes(snapshotRoot) + incomingBytes;
        days = accessDays(snapshotRoot);
    }

    if (projected > kAccessHardLimit) {
        if (error) {
            *error = QStringLiteral(
                        "通行抓拍和日志已达到 500 MiB，且只剩当天记录，无法继续保存");
        }
        return false;
    }
    return true;
}

} // namespace

namespace StoragePolicy {

qint64 mediaLimitBytes() { return kMediaLimit; }
qint64 registrationLimitBytes() { return kRegistrationLimit; }
qint64 accessSoftLimitBytes() { return kAccessSoftLimit; }
qint64 accessHardLimitBytes() { return kAccessHardLimit; }

Usage mediaUsage()
{
    QMutexLocker locker(&g_storageMutex);
    return directoryUsage(mediaRoot());
}

Usage registrationUsage()
{
    QMutexLocker locker(&g_storageMutex);
    return registrationUsageUnlocked();
}

bool canStoreMediaFile(const QString &targetPath,
                       qint64 incomingBytes,
                       QString *error)
{
    QMutexLocker locker(&g_storageMutex);
    if (!isInside(targetPath, mediaRoot())) {
        return true;
    }
    const qint64 oldSize = QFileInfo(targetPath).isFile()
            ? QFileInfo(targetPath).size() : 0;
    const QString partPath = targetPath + QStringLiteral(".part");
    const qint64 oldPartSize = QFileInfo(partPath).isFile()
            ? QFileInfo(partPath).size() : 0;
    const qint64 projected = directoryUsage(mediaRoot()).bytes
            - oldSize - oldPartSize + qMax<qint64>(0, incomingBytes);
    if (incomingBytes < 0 || projected > kMediaLimit) {
        if (error) {
            *error = QStringLiteral(
                        "媒体目录容量不足：视频和图片总量限制为 2 GiB，当前文件无法写入");
        }
        return false;
    }
    return true;
}

qint64 mediaWritableBytes(const QString &targetPath)
{
    QMutexLocker locker(&g_storageMutex);
    if (!isInside(targetPath, mediaRoot())) {
        return -1;
    }
    const qint64 oldSize = QFileInfo(targetPath).isFile()
            ? QFileInfo(targetPath).size() : 0;
    const QString partPath = targetPath + QStringLiteral(".part");
    const qint64 oldPartSize = QFileInfo(partPath).isFile()
            ? QFileInfo(partPath).size() : 0;
    return qMax<qint64>(0, kMediaLimit - directoryUsage(mediaRoot()).bytes
                        + oldSize + oldPartSize);
}

bool canCommitMediaFile(const QString &targetPath,
                        const QString &partPath,
                        QString *error)
{
    QMutexLocker locker(&g_storageMutex);
    if (!isInside(targetPath, mediaRoot())) {
        return true;
    }
    if (!QFileInfo(partPath).isFile()) {
        if (error) *error = QStringLiteral("媒体下载临时文件不存在");
        return false;
    }
    const qint64 oldSize = QFileInfo(targetPath).isFile()
            ? QFileInfo(targetPath).size() : 0;
    const qint64 projected = directoryUsage(mediaRoot()).bytes - oldSize;
    if (projected > kMediaLimit) {
        if (error) {
            *error = QStringLiteral(
                        "媒体目录容量不足：下载完成后视频和图片总量将超过 2 GiB");
        }
        return false;
    }
    return true;
}

bool writeRegistrationPhoto(const QString &targetPath,
                            const QByteArray &encodedImage,
                            QString *error)
{
    QMutexLocker locker(&g_storageMutex);
    bool recognizedRoot = false;
    for (const QString &root : registrationRoots()) {
        if (isInside(targetPath, root)) {
            recognizedRoot = true;
            break;
        }
    }
    if (!recognizedRoot) {
        if (error) *error = QStringLiteral("注册照目标路径不在受控目录内");
        return false;
    }

    const qint64 oldSize = QFileInfo(targetPath).isFile()
            ? QFileInfo(targetPath).size() : 0;
    const qint64 projected = registrationUsageUnlocked().bytes
            - oldSize + encodedImage.size();
    if (encodedImage.isEmpty() || projected > kRegistrationLimit) {
        if (error) {
            const qint64 remaining = qMax<qint64>(
                        0, kRegistrationLimit - registrationUsageUnlocked().bytes + oldSize);
            *error = QStringLiteral(
                        "注册照存储空间不足：总量限制为 1 GiB，本张需要 %1 字节，剩余 %2 字节")
                    .arg(encodedImage.size()).arg(remaining);
        }
        return false;
    }
    return writeAtomically(targetPath, encodedImage, error);
}

bool writeAccessSnapshot(const QString &snapshotRoot,
                         const QString &targetPath,
                         const QByteArray &encodedImage,
                         QString *error)
{
    QMutexLocker locker(&g_storageMutex);
    if (!isInside(targetPath, snapshotRoot)) {
        if (error) *error = QStringLiteral("抓拍图片目标路径不在抓拍目录内");
        return false;
    }
    const qint64 oldSize = QFileInfo(targetPath).isFile()
            ? QFileInfo(targetPath).size() : 0;
    if (encodedImage.isEmpty()
            || !prepareAccessWriteUnlocked(snapshotRoot,
                                           qMax<qint64>(0, encodedImage.size() - oldSize),
                                           error)) {
        return false;
    }
    return writeAtomically(targetPath, encodedImage, error);
}

bool appendAccessRecord(const QString &snapshotRoot,
                        QJsonObject record,
                        QString *error)
{
    QMutexLocker locker(&g_storageMutex);
    const QDateTime now = QDateTime::currentDateTime();
    record.insert(QStringLiteral("time"),
                  now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
    QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (!prepareAccessWriteUnlocked(snapshotRoot, line.size(), error)) {
        return false;
    }

    const QString path = QDir(accessLogDirectory()).filePath(
                now.toString(QStringLiteral("yyyyMMdd")) + QStringLiteral(".log"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)
            || file.write(line) != line.size()
            || !file.flush()) {
        if (error) *error = QStringLiteral("追加通行日志失败：%1").arg(path);
        return false;
    }
    return true;
}

QString accessLogDirectory()
{
    return QDir(Rk3566Platform::applicationRoot()).filePath(
                QStringLiteral("data/access_logs"));
}

} // namespace StoragePolicy
