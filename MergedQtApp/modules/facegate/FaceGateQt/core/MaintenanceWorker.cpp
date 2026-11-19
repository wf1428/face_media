/**
 * @file MaintenanceWorker.cpp
 * @brief 在后台线程执行数据库备份和过期抓拍清理的维护对象的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-18；修改人：Du lin；使用独立 SQLite 连接生成包含已提交 WAL 的一致性快照，完成后发布单个 db 文件；两种分区沿用统一路径解析。
 */

#include "MaintenanceWorker.h"
#include "platform/rk3566_platform.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>
#include <QtGlobal>

/** @brief 保存数据库、备份和抓拍目录配置。 */
MaintenanceWorker::MaintenanceWorker(const AppConfig &config, QObject *parent)
    : QObject(parent), config_(config)
{
}

/**
 * @brief 解析维护操作的数据库、备份或抓拍路径。
 * @param[in] path 配置的绝对路径或相对于可执行文件目录的路径。
 * @return 对应的数据分区绝对路径。
 */
QString MaintenanceWorker::absoluteAppPath(const QString &path) const
{
    const QFileInfo info(path);
    const QString absolute = info.isAbsolute() ? info.absoluteFilePath()
            : QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(path);
    return Rk3566Platform::dataPath(absolute);
}

/**
 * @brief 在维护线程内生成包含已提交 WAL 数据的 SQLite 一致性快照。
 * @note 独立连接通过 VACUUM INTO 读取备份快照建立时已提交的主库与 WAL 内容；
 *       后续提交由下一次备份收录。临时快照与正式文件位于同一分区，
 *       关闭备份连接后才发布正式文件；失败由临时目录清理未完成的快照。
 */
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
    QTemporaryDir temporary(QDir(backupRoot).filePath(QStringLiteral(".face_gate_backup_XXXXXX")));
    if (!temporary.isValid()) {
        emit maintenanceFinished(false, "创建备份临时目录失败：" + backupRoot);
        return;
    }
    const QString snapshot = QDir(temporary.path()).filePath(QStringLiteral("snapshot.db"));
    const QString connectionName = QStringLiteral("maintenance_backup_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString backupError;
    bool snapshotOk = false;
    {
        // 使用与业务相同的 Qt SQLite 驱动，在当前维护线程创建和释放专用连接。
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(source);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!database.open()) {
            backupError = database.lastError().text();
        } else {
            {
                QSqlQuery query(database);
                if (query.prepare(QStringLiteral("VACUUM main INTO ?"))) {
                    query.addBindValue(snapshot);
                    snapshotOk = query.exec();
                }
                backupError = query.lastError().text();
            }
            database.close();
        }
    }
    // 必须先销毁所有 QSqlQuery / QSqlDatabase 引用，再移除线程内的连接。
    QSqlDatabase::removeDatabase(connectionName);
    if (!snapshotOk) {
        emit maintenanceFinished(false, QStringLiteral("数据库备份失败：%1；%2")
                                 .arg(target, backupError));
        return;
    }
    if (!QFile::rename(snapshot, target)) {
        emit maintenanceFinished(false, "保存数据库备份失败：" + target);
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
