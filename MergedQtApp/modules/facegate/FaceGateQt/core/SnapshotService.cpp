/**
 * @file SnapshotService.cpp
 * @brief 验证抓拍文件命名和持久化服务的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-16；修改人：Du lin；将配置的绝对或相对抓拍目录解析到共享数据分区，保存和配额使用同一路径。
 */

#include "SnapshotService.h"

#include "common/storage_policy.h"
#include "platform/rk3566_platform.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

/** @brief 保存抓拍目录配置快照。 */
SnapshotService::SnapshotService(const AppConfig &config) : config_(config)
{
}

/** @return 配置在当前数据分区上的绝对抓拍目录。 */
QString SnapshotService::snapshotRootPath() const
{
    const QString configured = config_.snapshotDir.trimmed().isEmpty()
        ? QStringLiteral("./data/snapshots")
        : config_.snapshotDir.trimmed();
    const QFileInfo info(configured);
    const QString absolute = info.isAbsolute() ? info.absoluteFilePath()
            : QDir(Rk3566Platform::applicationRoot()).absoluteFilePath(configured);
    return Rk3566Platform::dataPath(absolute);
}

/** @return 当前数据分区上的绝对抓拍目录。 */
QString SnapshotService::rootPath() const
{
    return snapshotRootPath();
}

/** @return 移除路径非法字符后的安全文件名片段。 */
QString SnapshotService::safeFilePart(const QString &text) const
{
    QString out;
    for (const QChar ch : text) {
        out.append(ch.isLetterOrNumber() || ch == '_' || ch == '-' ? ch : QChar('_'));
    }
    return out.isEmpty() ? QStringLiteral("unknown") : out;
}

/**
 * @brief 按日期目录和人员/结果安全文件名保存验证图片。
 * @return 成功时返回图片路径，失败时返回空字符串并填写 errorText。
 */
QString SnapshotService::saveVerifySnapshot(const QImage &image,
                                             const QString &personName,
                                            const QString &result,
                                            QString *errorText) const
{
    if (image.isNull()) {
        if (errorText) {
            *errorText = QStringLiteral("没有可保存的抓拍画面");
        }
        return QString();
    }

    const QDateTime now = QDateTime::currentDateTime();
    QDir root(snapshotRootPath());
    const QString day = now.toString("yyyyMMdd");
    if (!root.mkpath(day)) {
        if (errorText) {
            *errorText = QStringLiteral("创建抓拍目录失败：") + root.absoluteFilePath(day);
        }
        return QString();
    }

    const QString fileName = QString("%1_%2_%3.jpg")
        .arg(now.toString("yyyyMMdd_HHmmss"))
        .arg(safeFilePart(personName))
        .arg(safeFilePart(result));
    const QString path = root.absoluteFilePath(day + "/" + fileName);
    QByteArray encodedImage;
    QBuffer buffer(&encodedImage);
    buffer.open(QIODevice::WriteOnly);
    const bool encoded = image.save(&buffer, "JPG", 92);
    buffer.close();
    QString storageError;
    if (!encoded || !StoragePolicy::writeAccessSnapshot(
                root.absolutePath(), path, encodedImage, &storageError)) {
        if (errorText) {
            *errorText = storageError.isEmpty()
                    ? QStringLiteral("保存抓拍图片失败：") + path
                    : storageError;
        }
        return QString();
    }
    return path;
}
