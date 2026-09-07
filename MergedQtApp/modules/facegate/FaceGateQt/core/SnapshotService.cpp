/**
 * @file SnapshotService.cpp
 * @brief 验证抓拍文件命名和持久化服务的实现。
 *
 * @author Dulin
 * @date 2026-07-25
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

/** @return 基于应用目录解析并确保存在的抓拍根路径。 */
QString SnapshotService::snapshotRootPath() const
{
    const QString configured = config_.snapshotDir.trimmed().isEmpty()
        ? QStringLiteral("./data/snapshots")
        : config_.snapshotDir.trimmed();
    const QFileInfo info(configured);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    return QDir(Rk3566Platform::applicationRoot()).absoluteFilePath(configured);
}

/** @return 基于应用目录解析后的抓拍根路径。 */
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
