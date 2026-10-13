/**
 * @file MaintenanceWorker.cpp
 * @brief 在后台线程执行数据库备份和过期抓拍清理的维护对象的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "MaintenanceWorker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

/** @brief 保存数据库、备份和抓拍目录配置。 */
MaintenanceWorker::MaintenanceWorker(const AppConfig &config, QObject *parent)
    : QObject(parent), config_(config)
{
}

/** @return 相对路径基于应用目录解析后的绝对路径。 */
QString MaintenanceWorker::absoluteAppPath(const QString &path) const
{
    const QFileInfo info(path);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(path);
}

/** @brief 将 SQLite 数据库复制到带时间戳的备份文件。 */
void MaintenanceWorker::backupSqliteDatabase()
{
    if (config_.databaseDriver.toUpper() != QStringLiteral("QSQLITE")) {
        emit maintenanceFinished(false, "当前不是 SQLite 数据库，不能执行本地文件备份");
        return;
    }
    const QString source = absoluteAppPath(config_.sqlitePath);
    if (!QFileInfo::exists(source)) {
        emit maintenanceFinished(false, "SQLite 数据库文件不存在：" + source);
        return;
    }
    const QString backupRoot = absoluteAppPath(config_.backupDir);
    if (!QDir().mkpath(backupRoot)) {
        emit maintenanceFinished(false, "创建备份目录失败：" + backupRoot);
        return;
    }
    const QString target = QDir(backupRoot).absoluteFilePath(
        QString("face_gate_%1.db").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    if (!QFile::copy(source, target)) {
        emit maintenanceFinished(false, "数据库备份失败：" + target);
        return;
    }
    emit maintenanceFinished(true, "数据库已备份：" + target);
}

/** @return path 中成功删除的、修改时间早于 cutoff 的文件数量。 */
int MaintenanceWorker::cleanupDirectory(const QString &path, const QDateTime &cutoff)
{
    int removed = 0;
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) {
            removed += cleanupDirectory(entry.absoluteFilePath(), cutoff);
            QDir(entry.absolutePath()).rmdir(entry.fileName());
            continue;
        }
        if (entry.lastModified() < cutoff && QFile::remove(entry.absoluteFilePath())) {
            ++removed;
        }
    }
    return removed;
}

/** @brief 删除早于指定天数的抓拍文件。 */
void MaintenanceWorker::cleanupSnapshots(int days)
{
    const int safeDays = qMax(1, days);
    const QString root = absoluteAppPath(config_.snapshotDir);
    if (!QDir(root).exists()) {
        emit maintenanceFinished(true, "抓拍目录不存在，无需清理");
        return;
    }
    const int removed = cleanupDirectory(root, QDateTime::currentDateTime().addDays(-safeDays));
    emit maintenanceFinished(true, QString("已清理 %1 个过期抓拍文件").arg(removed));
}
