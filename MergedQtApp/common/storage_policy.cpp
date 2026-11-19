/**
 * @file storage_policy.cpp
 * @brief 媒体、注册照以及通行记录存储配额策略的实现。
 * @author Du lin
 * @note 最近修改：2026-11-17；修改人：Du lin；媒体按目标文件系统实际可写余量动态增长，注册照和通行记录继续按各自比例校验。
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

#include <limits>

namespace {

const qint64 kGiB = 1024LL * 1024LL * 1024LL;
const qint64 kMiB = 1024LL * 1024LL;
const int kRegistrationPercent = 45;  /**< 本地与网络注册照占业务分区比例。 */
const int kAccessHardPercent = 6;     /**< 通行文本和抓拍的硬上限占比。 */
const qint64 kReferencePhotoBytes = 300 * 1024; /**< 界面估算每张注册照的字节数。 */
const int kAccessRetentionDays = 15;

/** @brief 按业务分区实际容量计算的字节配额及注册照参考数量。 */
struct StorageLimits {
    qint64 registrationBytes;
    qint64 accessSoftBytes;
    qint64 accessHardBytes;
    int registrationPhotoCapacity;
};

QMutex g_storageMutex;

/** @brief 计算整数百分比，按整字节向下取整，避免总字节数乘百分数溢出。 */
qint64 percentOf(qint64 total, int percent)
{
    return total / 100 * percent + (total % 100) * percent / 100;
}

/** @return 业务分区的比例配额；无法确认可写分区时按零配额处理。 */
StorageLimits currentStorageLimits()
{
    const bool shared = Rk3566Platform::usesSharedDataStorage();
    const QString volume = shared ? QStringLiteral("/userdata") : QDir::rootPath();
    QStorageInfo storage(volume);
    storage.refresh();
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()
            || storage.bytesTotal() <= 0 || (shared && storage.rootPath() != volume)) {
        return {};
    }
    const qint64 total = storage.bytesTotal();
    const qint64 registration = percentOf(total, kRegistrationPercent);
    const qint64 accessHard = percentOf(total, kAccessHardPercent);
    return {registration,
            percentOf(accessHard, 80),
            accessHard,
            int(qMin<qint64>(registration / kReferencePhotoBytes, std::numeric_limits<int>::max()))};
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
            || absolutePath.startsWith(absoluteRoot + QLatin1Char('/'));
}

/** @return 本地和网络注册照片所在的业务目录，用于共用配额统计。 */
QStringList registrationRoots()
{
    return {
        QDir(Rk3566Platform::applicationDataRoot()).filePath(
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
    const StorageLimits limits = currentStorageLimits();
    if (limits.accessHardBytes <= 0) {
        if (error) *error = QStringLiteral("无法确认通行记录所在的数据分区容量");
        return false;
    }
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

    // 到达当前分区配额软上限时，本次只主动清理最早的一天；后续写入继续逐日处理。
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

    // 当前分区配额硬上限不可突破；仍超限时继续逐日清理，但绝不删除当天记录。
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

/**
 * @brief 从目标分区实际可用容量扣除 5% 或调用方要求的更高余量。
 * @param path 下载或上传的目标路径，允许尚未存在。
 * @param reserveBytes 要保留的最小可用字节数。
 * @param[out] headroom 当前可额外写入的字节数，负值表示已越过保护线。
 * @return 能确认可写分区时返回 true，失败时不读取 headroom。
 */
bool queryDownloadHeadroom(const QString &path, qint64 reserveBytes, qint64 &headroom)
{
    QDir directory = QFileInfo(path).isDir() ? QDir(path) : QFileInfo(path).absoluteDir();
    while (!directory.exists()) {
        const QString parent = QFileInfo(directory.absolutePath()).absolutePath();
        if (parent == directory.absolutePath()) return false;
        directory.setPath(parent);
    }
    QStorageInfo storage(directory.absolutePath());
    storage.refresh();
    const qint64 total = storage.bytesTotal();
    const qint64 available = storage.bytesAvailable();
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()
            || total <= 0 || available < 0 || available > total || reserveBytes < 0
            || (Rk3566Platform::usesSharedDataStorage()
                && isInside(directory.absolutePath(), QStringLiteral("/userdata"))
                && !isInside(storage.rootPath(), QStringLiteral("/userdata")))) {
        return false;
    }
    const qint64 thresholdReserve = total - percentOf(total, StoragePolicy::DownloadUsageLimitPercent);
    headroom = available - qMax(reserveBytes, thresholdReserve);
    return true;
}

} // namespace

namespace StoragePolicy {

qint64 downloadWritableBytes(const QString &path, qint64 reserveBytes)
{
    qint64 headroom = 0;
    return queryDownloadHeadroom(path, reserveBytes, headroom) ? qMax<qint64>(0, headroom) : -1;
}

bool canWriteDownloadBytes(const QString &path, qint64 incomingBytes, qint64 reserveBytes)
{
    qint64 headroom = 0;
    return incomingBytes >= 0 && queryDownloadHeadroom(path, reserveBytes, headroom)
            && incomingBytes <= headroom;
}

qint64 registrationLimitBytes() { return currentStorageLimits().registrationBytes; }
qint64 accessSoftLimitBytes() { return currentStorageLimits().accessSoftBytes; }
qint64 accessHardLimitBytes() { return currentStorageLimits().accessHardBytes; }
int registrationPhotoCapacity()
{
    return currentStorageLimits().registrationPhotoCapacity;
}

Usage registrationUsage()
{
    QMutexLocker locker(&g_storageMutex);
    return registrationUsageUnlocked();
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
    return QDir(Rk3566Platform::applicationDataRoot()).filePath(
                QStringLiteral("data/access_logs"));
}

} // namespace StoragePolicy
