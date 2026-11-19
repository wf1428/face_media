/**
 * @file MaintenanceWorker.h
 * @brief 在后台线程执行数据库备份和过期抓拍清理的维护对象。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-18；修改人：Du lin；明确在线备份的 WAL 快照、线程归属及失败发布约束。
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
    /**
     * @brief 将当前数据分区的 SQLite 已提交快照保存为单个带时间戳的 db 文件。
     * @note 在对象所属维护线程内执行，使用独立 QSQLITE 连接读取主库及 WAL；
     *       仅在完整快照生成后发布文件，通过 maintenanceFinished 报告结果。
     *       SQLite 需支持 VACUUM INTO（3.27.0 及以上）。
     */
    void backupSqliteDatabase();

    /** @brief 删除早于指定天数的抓拍文件。 */
    void cleanupSnapshots(int days);

signals:
    /** @brief 输出维护任务成功状态和结果摘要。 */
    void maintenanceFinished(bool ok, const QString &message);

private:
    /**
     * @param[in] path 配置的绝对路径或相对于可执行文件目录的路径。
     * @return 解析后位于当前业务数据分区的绝对路径。
     */
    QString absoluteAppPath(const QString &path) const;

    /** @return path 中成功删除的、修改时间早于 cutoff 的文件数量。 */
    int cleanupDirectory(const QString &path, const QDateTime &cutoff);

private:
    AppConfig config_; /**< SQLite、备份和抓拍路径配置快照。 */
};

#endif
