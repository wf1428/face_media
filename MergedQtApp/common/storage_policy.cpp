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
#include <QStorageInfo>

#include <algorithm>

namespace {

const qint64 kGiB = 1024LL * 1024LL * 1024LL;
const qint64 kMiB = 1024LL * 1024LL;
const qint64 kLargeEmmcFilesystemThreshold = 24LL * kGiB;
const int kAccessRetentionDays = 15;

/** @brief 一档设备存储容量对应的各业务配额。 */
struct StorageLimits {
    qint64 mediaBytes;
    qint64 registrationBytes;
    qint64 accessSoftBytes;
    qint64 accessHardBytes;
    int registrationPhotoCapacity;
};

const StorageLimits kLegacyStorageLimits = {
    2LL * kGiB,
    1LL * kGiB,
    400LL * kMiB,
    500LL * kMiB,
    3000
};

const StorageLimits kLargeEmmcStorageLimits = {
    8LL * kGiB,
    15LL * kGiB,
    2LL * kGiB * 4LL / 5LL,
    2LL * kGiB,
    52000
};

QMutex g_storageMutex;

/**
 * @brief 按应用数据所在文件系统选择 8GB 或 32GB eMMC 配额档位。
 *
 * 标称 32GB eMMC 格式化后的根文件系统通常约为 29 GiB，因此使用
 * 24 GiB 作为两档之间的安全分界；容量读取失败时保守沿用原 8GB 策略。
 */
const StorageLimits &currentStorageLimits()
{
    QStorageInfo storage(Rk3566Platform::applicationRoot());
    storage.refresh();
    if (!storage.isValid() || !storage.isReady() || storage.bytesTotal() <= 0) {
        storage = QStorageInfo::root();
        storage.refresh();
    }
    if (storage.isValid()
            && storage.isReady()
            && storage.bytesTotal() >= kLargeEmmcFilesystemThreshold) {
        return kLargeEmmcStorageLimits;
    }
    return kLegacyStorageLimits;
}

QString capacityText(qint64 bytes)
{
    if (bytes % kGiB == 0) {
        return QStringLiteral("%1 GiB").arg(bytes / kGiB);
    }
    return QStringLiteral("%1 MiB").arg(bytes / kMiB);
}

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
    const StorageLimits &limits = currentStorageLimits();
    if (incomingBytes < 0 || incomingBytes > limits.accessHardBytes) {
        if (error) {
            *error = QStringLiteral("单条通行数据超过 %1 限制")
                    .arg(capacityText(limits.accessHardBytes));
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

    // 到达当前档位软上限时，本次只主动清理最早的一天；后续写入继续逐日处理。
    if (!cleanedOneDay
            && projected >= limits.accessSoftBytes
            && !days.isEmpty()
            && days.first() != today) {
        if (!removeAccessDay(snapshotRoot, days.first(), error)) {
            return false;
        }
        projected = accessUsageBytes(snapshotRoot) + incomingBytes;
        days = accessDays(snapshotRoot);
    }

    // 当前档位硬上限不可突破；仍超限时继续逐日清理，但绝不删除当天记录。
    while (projected > limits.accessHardBytes
           && !days.isEmpty()
           && days.first() != today) {
        if (!removeAccessDay(snapshotRoot, days.first(), error)) {
            return false;
        }
        projected = accessUsageBytes(snapshotRoot) + incomingBytes;
        days = accessDays(snapshotRoot);
    }

    if (projected > limits.accessHardBytes) {
        if (error) {
            *error = QStringLiteral(
                        "通行抓拍和日志已达到 %1，且只剩当天记录，无法继续保存")
                    .arg(capacityText(limits.accessHardBytes));
        }
        return false;
    }
    return true;
}

} // namespace

namespace StoragePolicy {

qint64 mediaLimitBytes() { return currentStorageLimits().mediaBytes; }
qint64 registrationLimitBytes() { return currentStorageLimits().registrationBytes; }
qint64 accessSoftLimitBytes() { return currentStorageLimits().accessSoftBytes; }
qint64 accessHardLimitBytes() { return currentStorageLimits().accessHardBytes; }
int registrationPhotoCapacity()
{
    return currentStorageLimits().registrationPhotoCapacity;
}

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
    const qint64 mediaLimit = currentStorageLimits().mediaBytes;
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
    if (incomingBytes < 0 || projected > mediaLimit) {
        if (error) {
            *error = QStringLiteral(
                        "媒体目录容量不足：视频和图片总量限制为 %1，当前文件无法写入")
                    .arg(capacityText(mediaLimit));
        }
        return false;
    }
    return true;
}

qint64 mediaWritableBytes(const QString &targetPath)
{
    QMutexLocker locker(&g_storageMutex);
    const qint64 mediaLimit = currentStorageLimits().mediaBytes;
    if (!isInside(targetPath, mediaRoot())) {
        return -1;
    }
    const qint64 oldSize = QFileInfo(targetPath).isFile()
            ? QFileInfo(targetPath).size() : 0;
    const QString partPath = targetPath + QStringLiteral(".part");
    const qint64 oldPartSize = QFileInfo(partPath).isFile()
            ? QFileInfo(partPath).size() : 0;
    return qMax<qint64>(0, mediaLimit - directoryUsage(mediaRoot()).bytes
                        + oldSize + oldPartSize);
}

bool canCommitMediaFile(const QString &targetPath,
                        const QString &partPath,
                        QString *error)
{
    QMutexLocker locker(&g_storageMutex);
    const qint64 mediaLimit = currentStorageLimits().mediaBytes;
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
    if (projected > mediaLimit) {
        if (error) {
            *error = QStringLiteral(
                        "媒体目录容量不足：下载完成后视频和图片总量将超过 %1")
                    .arg(capacityText(mediaLimit));
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
    const qint64 registrationLimit = currentStorageLimits().registrationBytes;
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
    if (encodedImage.isEmpty() || projected > registrationLimit) {
        if (error) {
            const qint64 remaining = qMax<qint64>(
                        0, registrationLimit - registrationUsageUnlocked().bytes + oldSize);
            *error = QStringLiteral(
                        "注册照存储空间不足：总量限制为 %1，本张需要 %2 字节，剩余 %3 字节")
                    .arg(capacityText(registrationLimit))
                    .arg(encodedImage.size())
                    .arg(remaining);
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
