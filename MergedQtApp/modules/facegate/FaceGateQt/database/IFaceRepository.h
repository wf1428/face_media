/**
 * @file IFaceRepository.h
 * @brief 人员、特征、验证日志和运维数据的持久化抽象接口。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IFACE_REPOSITORY_H
#define IFACE_REPOSITORY_H

#include <QVector>

#include "VerificationTypes.h"

/** @brief 人员、特征、验证日志和运维数据的持久化抽象接口。 */
class IFaceRepository {
public:
    /** @brief 允许通过接口指针安全销毁实现。 */
    virtual ~IFaceRepository() = default;

    /** @brief 打开存储并确保所需结构可用。 */
    virtual bool open() = 0;

    /** @brief 关闭当前存储连接。 */
    virtual void close() = 0;

    /** @brief 在一个业务操作中保存人员及其人脸特征。 */
    virtual bool addPersonFace(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath) = 0;

    /** @brief 批量导入人员基础信息并统计新增、更新数量。 */
    virtual bool importPeopleBasic(const QVector<PersonInfo> &people, int *inserted, int *updated) = 0;

    /** @brief 加载所有启用且未删除人员的人脸图库。 */
    virtual bool loadAllEnabledFaces(QVector<FaceRecord> &records) = 0;

    /** @brief 加载管理界面人员列表，可选包含软删除记录。 */
    virtual bool loadPeople(QVector<PersonAdminRecord> &records, bool includeDeleted) = 0;

    /** @brief 加载最近通过的验证日志。 */
    virtual bool loadPassedVerifyLogs(QVector<VerifyLogViewRecord> &records, int limit) = 0;

    /** @brief 按复合筛选条件加载验证日志。 */
    virtual bool loadVerifyLogs(const VerifyLogFilter &filter, QVector<VerifyLogViewRecord> &records) = 0;

    /** @brief 加载待同步任务。 */
    virtual bool loadSyncTasks(QVector<SyncTaskRecord> &records, int limit) = 0;

    /** @brief 加载最近系统事件。 */
    virtual bool loadSystemEvents(QVector<SystemEventLog> &records, int limit) = 0;

    /** @brief 汇总人员、特征和验证日志数量。 */
    virtual bool loadStorageStats(StorageStats &stats) = 0;

    /** @brief 更新人员业务编号。 */
    virtual bool updatePersonNo(qint64 personId, const QString &newPersonNo) = 0;

    /** @brief 启用或停用指定人员。 */
    virtual bool updatePersonEnabled(qint64 personId, bool enabled) = 0;

    /** @brief 软删除指定人员及其验证资格。 */
    virtual bool deletePerson(qint64 personId) = 0;

    /** @brief 写入一条验证日志。 */
    virtual bool addVerifyLog(const VerifyLog &log) = 0;

    /** @brief 写入一条管理员操作审计日志。 */
    virtual bool addOperatorAuditLog(const OperatorAuditLog &log) = 0;

    /** @brief 写入一条系统事件日志。 */
    virtual bool addSystemEventLog(const SystemEventLog &log) = 0;

    /** @brief 删除早于指定天数的验证日志并返回删除数量。 */
    virtual bool cleanupOldVerifyLogs(int days, int *removed) = 0;
};

#endif
