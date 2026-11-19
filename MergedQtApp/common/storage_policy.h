/**
 * @file storage_policy.h
 * @brief 媒体、注册照以及通行记录的设备端存储配额策略。
 * @author Du lin
 * @note 最近修改：2026-11-17；修改人：Du lin；公开目标分区的实时容量检查，支持在文件已落盘后复核 95% 保护线。
 */

#ifndef STORAGE_POLICY_H
#define STORAGE_POLICY_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace StoragePolicy {

/** @brief 一个目录类别当前的实际文件数量和字节数。 */
struct Usage {
    qint64 bytes = 0;
    int fileCount = 0;
};

/** 下载后文件系统最大可用空间占用率，单位为百分比。 */
constexpr int DownloadUsageLimitPercent = 95;

/**
 * @brief 查询目标文件系统在下载保护阈值以内还能写入的字节数。
 * @param path 文件或目录，可尚未创建；按最近的已存在父目录查找挂载。
 * @param reserveBytes 额外要求保留的可用字节数，取它与 5% 保留空间的较大值。
 * @return 剩余字节数；容量无效、只读或挂载退回根分区时返回 -1。
 */
qint64 downloadWritableBytes(const QString &path, qint64 reserveBytes = 0);

/**
 * @brief 以当前分区占用校验计划写入量，允许恰好达到保护线。
 * @param path 目标文件或目录，允许尚未存在。
 * @param incomingBytes 尚未落盘的字节数，暂存完成后传 0 校验现状。
 * @param reserveBytes 额外要求保留的可用字节数。
 * @return 容量有效且写入后可满足保护线时返回 true。
 */
bool canWriteDownloadBytes(const QString &path, qint64 incomingBytes, qint64 reserveBytes = 0);

qint64 registrationLimitBytes();
qint64 accessSoftLimitBytes();
qint64 accessHardLimitBytes();
/** @return 当前业务分区在界面展示的注册照参考数量。 */
int registrationPhotoCapacity();

Usage registrationUsage();

/** @brief 在当前设备的注册照配额内原子保存本地或网络注册照片。 */
bool writeRegistrationPhoto(const QString &targetPath,
                            const QByteArray &encodedImage,
                            QString *error = nullptr);

/** @brief 在通行记录配额内保存一张已经编码好的抓拍图。 */
bool writeAccessSnapshot(const QString &snapshotRoot,
                         const QString &targetPath,
                         const QByteArray &encodedImage,
                         QString *error = nullptr);

/**
 * @brief 向 data/access_logs/yyyyMMdd.log 追加一条 JSON Lines 通行记录。
 *
 * record 可包含 type/name/credential/success/reason/snapshotPath/faceHash；
 * 本函数统一补充毫秒级准确时间，并执行 15 天及当前分区的比例配额的清理策略。
 */
bool appendAccessRecord(const QString &snapshotRoot,
                        QJsonObject record,
                        QString *error = nullptr);

/** @return 应用目录下单独存放通行日志的目录。 */
QString accessLogDirectory();

} // namespace StoragePolicy

#endif // STORAGE_POLICY_H
