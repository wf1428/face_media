/**
 * @file MysqlFaceRepository.h
 * @brief 实现 SQLite、MySQL 和旧本地文件兼容的门禁数据仓储。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MYSQL_FACE_REPOSITORY_H
#define MYSQL_FACE_REPOSITORY_H

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "AppConfig.h"
#include "IFaceRepository.h"

/**
 * @brief 支持 SQLite、MySQL 和旧本地文件的门禁仓储实现。
 *
 * 优先按 databaseDriver 打开 SQL 存储；必要时识别旧 .fgdb 文件并迁移到 SQLite。
 * 本地文件模式使用内存向量并显式保存，SQL 模式通过独立连接名隔离线程连接。
 */
class MysqlFaceRepository : public IFaceRepository {
public:
    /** @brief 保存数据库和本地文件配置并生成独立连接名。 */
    explicit MysqlFaceRepository(const AppConfig &config);

    /** @brief 销毁前关闭 SQL 或本地文件存储。 */
    ~MysqlFaceRepository() override;

    /** @brief 打开配置的存储后端并确保结构完整。 */
    bool open() override;

    /** @brief 关闭 SQL 连接或清理本地内存状态。 */
    void close() override;

    /** @brief 原子保存人员、人脸特征和同步任务。 */
    bool addPersonFace(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath) override;

    /** @brief 批量新增或更新人员基础信息。 */
    bool importPeopleBasic(const QVector<PersonInfo> &people, int *inserted, int *updated) override;

    /** @brief 加载启用且未删除人员的全部有效特征。 */
    bool loadAllEnabledFaces(QVector<FaceRecord> &records) override;

    /** @brief 从统一 SQLite 的 network_person/network_face 加载网络人脸特征。 */
    bool loadAllEnabledNetworkFaces(QVector<FaceRecord> &records);

    /** @brief 加载人员管理汇总列表。 */
    bool loadPeople(QVector<PersonAdminRecord> &records, bool includeDeleted) override;

    /** @brief 加载最近通过的验证日志。 */
    bool loadPassedVerifyLogs(QVector<VerifyLogViewRecord> &records, int limit) override;

    /** @brief 按筛选器加载验证日志。 */
    bool loadVerifyLogs(const VerifyLogFilter &filter, QVector<VerifyLogViewRecord> &records) override;

    /** @brief 加载待处理同步任务。 */
    bool loadSyncTasks(QVector<SyncTaskRecord> &records, int limit) override;

    /** @brief 加载最近系统事件。 */
    bool loadSystemEvents(QVector<SystemEventLog> &records, int limit) override;

    /** @brief 统计人员、特征和验证日志数量。 */
    bool loadStorageStats(StorageStats &stats) override;

    /** @brief 更新人员编号并保持唯一性。 */
    bool updatePersonNo(qint64 personId, const QString &newPersonNo) override;

    /** @brief 更新人员启用状态。 */
    bool updatePersonEnabled(qint64 personId, bool enabled) override;

    /** @brief 软删除人员。 */
    bool deletePerson(qint64 personId) override;

    /** @brief 写入验证日志并创建同步任务。 */
    bool addVerifyLog(const VerifyLog &log) override;

    /** @brief 写入管理员操作审计日志。 */
    bool addOperatorAuditLog(const OperatorAuditLog &log) override;

    /** @brief 写入系统事件日志。 */
    bool addSystemEventLog(const SystemEventLog &log) override;

    /** @brief 清理指定天数前的验证日志并统计删除数。 */
    bool cleanupOldVerifyLogs(int days, int *removed) override;

    /** @return 最近一次仓储错误文本。 */
    QString lastError() const;

private:
    /** @brief 旧本地文件中的人脸特征记录。 */
    struct LocalFeatureEntry {
        qint64 id = 0;          /**< 特征主键。 */
        qint64 personId = 0;    /**< 所属人员主键。 */
        QString imagePath;      /**< 录入图片路径。 */
        FaceFeatureData feature; /**< 特征向量和元数据。 */
        QString createdAt;      /**< 创建时间文本。 */
    };

    /** @brief 确保存储后端已经打开。 */
    bool ensureDatabase();

    /** @brief 创建基础表、索引并增量补齐新字段。 */
    bool ensureSchema();

    /** @return 指定表包含字段时返回 true。 */
    bool tableHasColumn(const QString &tableName, const QString &columnName) const;

    /** @brief 按当前 SQL 方言补充缺失字段。 */
    bool ensureColumn(const QString &tableName, const QString &columnName, const QString &sqliteAlter, const QString &mysqlAlter);

    /** @brief 执行一条结构迁移 SQL 并记录错误。 */
    bool execSchemaSql(const QString &sql);

    /** @brief 将待同步业务写入同步任务表或本地向量。 */
    bool enqueueSyncTask(const QString &taskType, const QString &payloadJson);

    /** @return 当前 SQL 连接使用 SQLite 驱动时返回 true。 */
    bool isSqlite() const;

    /** @return 当前使用旧本地文件模式时返回 true。 */

    /** @brief 打开 SQLite，并在需要时迁移旧本地文件。 */
    bool openSqliteStore(const QString &requestedDriver);

    /** @brief 检查目标文件格式并决定是否需要旧格式迁移。 */
    bool prepareSqliteFile(const QString &path, bool *migrateLegacyLocalStore);

    /** @return path 是现有 SQLite 数据库时返回 true。 */
    bool isExistingSqliteDatabase(const QString &path) const;

    /** @return path 是可识别旧本地门禁数据库时返回 true。 */
    bool isExistingLegacyLocalDatabase(const QString &path) const;

    /** @brief 从旧本地文件反序列化全部记录。 */
    bool loadLocalStoreFromFile(const QString &path);

    /** @brief 将已加载的旧本地数据写入当前 SQLite。 */
    bool migrateLocalStoreToSqlite();

    /** @brief 打开或创建旧本地文件模式。 */

    /** @brief 将内存向量原子保存到旧本地文件。 */
    bool saveLocalStore();

    /** @brief 清空旧本地文件模式的全部内存数据和 ID 计数。 */
    void clearLocalStore();

    /** @brief 在旧本地模式保存人员及特征。 */
    bool addPersonFaceToLocal(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath);

    /** @brief 在旧本地模式批量导入人员。 */
    bool importPeopleBasicToLocal(const QVector<PersonInfo> &people, int *inserted, int *updated);

    /** @brief 从旧本地模式生成启用人脸图库。 */
    bool loadAllEnabledFacesFromLocal(QVector<FaceRecord> &records);

    /** @brief 从旧本地模式生成人员管理列表。 */
    bool loadPeopleFromLocal(QVector<PersonAdminRecord> &records, bool includeDeleted);

    /** @brief 从旧本地模式加载最近通过日志。 */
    bool loadPassedVerifyLogsFromLocal(QVector<VerifyLogViewRecord> &records, int limit);

    /** @brief 从旧本地模式按筛选器加载验证日志。 */
    bool loadVerifyLogsFromLocal(const VerifyLogFilter &filter, QVector<VerifyLogViewRecord> &records);

    /** @brief 从旧本地模式加载同步任务。 */
    bool loadSyncTasksFromLocal(QVector<SyncTaskRecord> &records, int limit);

    /** @brief 从旧本地模式加载系统事件。 */
    bool loadSystemEventsFromLocal(QVector<SystemEventLog> &records, int limit);

    /** @brief 汇总旧本地模式存储数量。 */
    bool loadStorageStatsFromLocal(StorageStats &stats);

    /** @brief 在旧本地模式更新人员编号。 */
    bool updatePersonNoInLocal(qint64 personId, const QString &newPersonNo);

    /** @brief 在旧本地模式更新启用状态。 */
    bool updatePersonEnabledInLocal(qint64 personId, bool enabled);

    /** @brief 在旧本地模式软删除人员。 */
    bool deletePersonFromLocal(qint64 personId);

    /** @brief 在旧本地模式追加验证日志。 */
    bool addVerifyLogToLocal(const VerifyLog &log);

    /** @brief 在旧本地模式追加审计日志。 */
    bool addOperatorAuditLogToLocal(const OperatorAuditLog &log);

    /** @brief 在旧本地模式追加系统事件。 */
    bool addSystemEventLogToLocal(const SystemEventLog &log);

    /** @brief 在旧本地模式删除过期验证日志。 */
    bool cleanupOldVerifyLogsFromLocal(int days, int *removed);

    /** @brief 获取并递增指定类别的旧本地主键计数。 */
    qint64 nextLocalId(const QString &key);

    /** @return 人员编号或其他唯一字段已存在时返回 true 并填写 message。 */
    bool hasDuplicatePerson(const PersonInfo &person, QString *message);

    /** @return 特征与已有特征达到去重阈值时返回 true。 */
    bool hasDuplicateFeature(const FaceFeatureData &feature, QString *message);

    /** @return 两个相同格式特征向量的余弦相似度。 */
    float featureCosine(const QByteArray &left, const QByteArray &right) const;

    /** @return 适合当前 SQL 驱动的安全标识符引用文本。 */
    QString quoteIdentifier(const QString &identifier) const;

    /** @return 基于应用目录解析的 SQLite 路径。 */
    QString sqliteDatabasePath() const;

    /** @return 基于应用目录解析的旧本地存储路径。 */
    QString localStoreFilePath() const;

    /** @return 当前时间的数据库文本格式。 */
    QString nowString() const;

private:
    AppConfig config_;                       /**< 数据库、路径和去重阈值配置。 */
    QString connectionName_;                 /**< 当前实例独立的 Qt SQL 连接名。 */
    QSqlDatabase db_;                        /**< SQL 模式数据库句柄。 */
    bool localFileMode_ = false;             /**< 是否使用旧本地文件存储。 */
    bool sqliteMode_ = false;                /**< SQL 模式是否为 SQLite。 */
    qint64 localLastPersonId_ = 0;           /**< 旧本地人员 ID 计数。 */
    qint64 localLastFeatureId_ = 0;          /**< 旧本地特征 ID 计数。 */
    qint64 localLastLogId_ = 0;              /**< 旧本地日志 ID 计数。 */
    QVector<PersonInfo> localPersons_;       /**< 旧本地人员集合。 */
    QVector<LocalFeatureEntry> localFeatures_; /**< 旧本地特征集合。 */
    QVector<VerifyLog> localVerifyLogs_;     /**< 旧本地验证日志集合。 */
    QVector<OperatorAuditLog> localAuditLogs_; /**< 旧本地审计日志集合。 */
    QVector<SystemEventLog> localSystemEvents_; /**< 旧本地系统事件集合。 */
    QVector<SyncTaskRecord> localSyncTasks_; /**< 旧本地同步任务集合。 */
    QString lastError_;                     /**< 最近一次仓储错误。 */
};

#endif
