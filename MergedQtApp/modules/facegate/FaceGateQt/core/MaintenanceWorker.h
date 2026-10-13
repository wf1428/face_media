/**
 * @file MaintenanceWorker.h
 * @brief 在后台线程执行数据库备份和过期抓拍清理的维护对象。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MAINTENANCE_WORKER_H
#define MAINTENANCE_WORKER_H

#include <QDateTime>
#include <QObject>

#include "AppConfig.h"

/** @brief 在后台线程执行数据库备份和过期抓拍清理的维护对象。 */
class MaintenanceWorker : public QObject {
    Q_OBJECT

public:
    /** @brief 保存数据库、备份和抓拍目录配置。 */
    explicit MaintenanceWorker(const AppConfig &config, QObject *parent = nullptr);

public slots:
    /** @brief 将 SQLite 数据库复制到带时间戳的备份文件。 */
    void backupSqliteDatabase();

    /** @brief 删除早于指定天数的抓拍文件。 */
    void cleanupSnapshots(int days);

signals:
    /** @brief 输出维护任务成功状态和结果摘要。 */
    void maintenanceFinished(bool ok, const QString &message);

private:
    /** @return 相对路径基于应用目录解析后的绝对路径。 */
    QString absoluteAppPath(const QString &path) const;

    /** @return path 中成功删除的、修改时间早于 cutoff 的文件数量。 */
    int cleanupDirectory(const QString &path, const QDateTime &cutoff);

private:
    AppConfig config_; /**< SQLite、备份和抓拍路径配置快照。 */
};

#endif
