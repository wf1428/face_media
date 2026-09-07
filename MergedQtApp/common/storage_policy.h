/**
 * @file storage_policy.h
 * @brief 媒体、注册照以及通行记录的设备端存储配额策略。
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

qint64 mediaLimitBytes();
qint64 registrationLimitBytes();
qint64 accessSoftLimitBytes();
qint64 accessHardLimitBytes();

Usage mediaUsage();
Usage registrationUsage();

/** @brief 下载前检查目标媒体文件写入后是否仍在 2 GiB 配额内。 */
bool canStoreMediaFile(const QString &targetPath,
                       qint64 incomingBytes,
                       QString *error = nullptr);

/** @brief 返回替换目标媒体文件时最多还可下载的字节数；非媒体目录返回 -1。 */
qint64 mediaWritableBytes(const QString &targetPath);

/** @brief 下载完成后，在删除旧文件前校验 .part 原子替换是否满足媒体配额。 */
bool canCommitMediaFile(const QString &targetPath,
                        const QString &partPath,
                        QString *error = nullptr);

/** @brief 在 1 GiB 注册照配额内原子保存本地或网络注册照片。 */
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
 * 本函数统一补充毫秒级准确时间并执行 15 天、400 MiB/500 MiB 清理策略。
 */
bool appendAccessRecord(const QString &snapshotRoot,
                        QJsonObject record,
                        QString *error = nullptr);

/** @return 应用目录下单独存放通行日志的目录。 */
QString accessLogDirectory();

} // namespace StoragePolicy

#endif // STORAGE_POLICY_H
