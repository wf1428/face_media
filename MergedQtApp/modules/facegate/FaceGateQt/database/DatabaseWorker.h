/**
 * @file DatabaseWorker.h
 * @brief 数据库专用线程中的仓储调用门面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef DATABASE_WORKER_H
#define DATABASE_WORKER_H

#include <QObject>

#include "AppConfig.h"
#include "MysqlFaceRepository.h"
#include "VerificationTypes.h"

/** @brief 数据库专用线程中的仓储调用门面。 */
class DatabaseWorker : public QObject {
    Q_OBJECT

public:
    /** @brief 使用配置构造仓储对象；连接在 open() 所在线程创建。 */
    explicit DatabaseWorker(const AppConfig &config, QObject *parent = nullptr);

    /** @brief 销毁前关闭仓储连接。 */
    ~DatabaseWorker() override;

public slots:
    /** @brief 打开仓储并发送 databaseReady()。 */
    void open();

    /** @brief 关闭仓储。 */
    void close();

    /** @brief 重新加载启用人脸图库。 */
    void reloadGallery();

    /** @brief 加载人员管理列表。 */
    void loadPeople(bool includeDeleted = false);

    /** @brief 加载最近通过日志。 */
    void loadPassedVerifyLogs();

    /** @brief 按筛选条件加载验证日志。 */
    void loadVerifyLogs(const VerifyLogFilter &filter);

    /** @brief 加载同步任务。 */
    void loadSyncTasks();

    /** @brief 加载系统事件。 */
    void loadSystemEvents();

    /** @brief 加载存储统计。 */
    void loadStorageStats();

    /** @brief 保存人员和人脸特征，成功后由上层刷新图库。 */
    void addPersonFace(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath);

    /** @brief 批量导入人员基础信息。 */
    void importPeopleBasic(const QVector<PersonInfo> &people);

    /** @brief 更新人员编号。 */
    void updatePersonNo(qint64 personId, const QString &newPersonNo);

    /** @brief 更新人员启用状态。 */
    void updatePersonEnabled(qint64 personId, bool enabled);

    /** @brief 软删除人员。 */
    void deletePerson(qint64 personId);

    /** @brief 写入验证日志。 */
    void addVerifyLog(const VerifyLog &log);

    /** @brief 写入管理员操作审计日志。 */
    void addOperatorAuditLog(const OperatorAuditLog &log);

    /** @brief 写入系统事件日志。 */
    void addSystemEventLog(const SystemEventLog &log);

    /** @brief 清理指定天数前的验证日志。 */
    void cleanupOldVerifyLogs(int days);

signals:
    /** @brief 仓储打开结果。 */
    void databaseReady(bool ok, const QString &message);

    /** @brief 图库加载结果。 */
    void galleryLoaded(const QVector<FaceRecord> &records);

    /** @brief 网络人员图库加载结果，仅供识别线程使用。 */
    void networkGalleryLoaded(const QVector<FaceRecord> &records);

    /** @brief 一次发送本地与网络图库，供识别线程原子替换匹配快照。 */
    void recognitionGalleriesLoaded(const QVector<FaceRecord> &localRecords,
                                    const QVector<FaceRecord> &networkRecords);

    /** @brief 人员列表加载结果。 */
    void peopleLoaded(const QVector<PersonAdminRecord> &records);

    /** @brief 最近通过日志加载结果。 */
    void passedVerifyLogsLoaded(const QVector<VerifyLogViewRecord> &records);

    /** @brief 筛选日志加载结果。 */
    void verifyLogsLoaded(const QVector<VerifyLogViewRecord> &records);

    /** @brief 同步任务加载结果。 */
    void syncTasksLoaded(const QVector<SyncTaskRecord> &records);

    /** @brief 系统事件加载结果。 */
    void systemEventsLoaded(const QVector<SystemEventLog> &records);

    /** @brief 存储统计加载结果。 */
    void storageStatsLoaded(const StorageStats &stats);

    /** @brief 人脸录入写入结果。 */
    void enrollFinished(bool ok, const QString &message);

    /** @brief 人员批量导入结果。 */
    void peopleImportFinished(bool ok, const QString &message);

    /** @brief 人员更新结果。 */
    void personUpdated(bool ok, const QString &message);

    /** @brief 人员删除结果。 */
    void personDeleted(bool ok, const QString &message);

    /** @brief 验证日志写入结果。 */
    void verifyLogWritten(bool ok, const QString &message);

    /** @brief 审计日志写入结果。 */
    void operatorAuditLogWritten(bool ok, const QString &message);

    /** @brief 系统事件写入结果。 */
    void systemEventLogWritten(bool ok, const QString &message);

    /** @brief 日志清理结果。 */
    void cleanupFinished(bool ok, const QString &message);

private:
    AppConfig config_;                 /**< 数据库和本地存储配置快照。 */
    MysqlFaceRepository repository_;   /**< 仅在本 worker 线程访问的仓储。 */
    bool opened_ = false;              /**< 仓储连接状态。 */
};

#endif
