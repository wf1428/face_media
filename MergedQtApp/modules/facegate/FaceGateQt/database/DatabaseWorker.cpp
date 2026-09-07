/**
 * @file DatabaseWorker.cpp
 * @brief 数据库专用线程中的仓储调用门面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "DatabaseWorker.h"

#include <QDebug>
#include <QFile>

/** @brief 使用配置构造仓储对象；连接在 open() 所在线程创建。 */
DatabaseWorker::DatabaseWorker(const AppConfig &config, QObject *parent)
    : QObject(parent), config_(config), repository_(config)
{
}

/** @brief 销毁前关闭仓储连接。 */
DatabaseWorker::~DatabaseWorker()
{
    close();
}

/**
 * @brief 打开配置的本地/网络数据库，并加载启用的人脸。
 *
 * 该槽函数运行在 databaseThread_ 中。Repository 及其 QSqlDatabase 连接
 * 不会被 UI 线程直接访问，从而保持 Qt SQL 连接线程归属正确。
 */
void DatabaseWorker::open()
{
    qInfo().noquote() << "数据库工作线程正在打开配置的存储";
    // Repository 在数据库线程中打开，避免跨线程使用 QSqlDatabase。
    opened_ = repository_.open();
    if (opened_) {
        qInfo().noquote() << "数据库工作线程已打开存储";
    } else {
        qWarning().noquote() << "数据库工作线程打开失败：" << repository_.lastError();
    }
    emit databaseReady(opened_, opened_ ? "数据库已连接" : repository_.lastError());
    if (opened_) {
        reloadGallery();
        loadPeople(false);
        loadStorageStats();
    }
}

/** @brief 关闭仓储。 */
void DatabaseWorker::close()
{
    if (opened_) {
        qInfo().noquote() << "数据库工作线程正在关闭存储";
        repository_.close();
        opened_ = false;
    }
}

/**
 * @brief 重新加载启用的人脸，并向 UI/引擎线程发送拷贝后的底库。
 */
void DatabaseWorker::reloadGallery()
{
    if (!opened_) {
        qWarning().noquote() << "跳过底库重载：数据库未打开";
        emit databaseReady(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        emit galleryLoaded(QVector<FaceRecord>());
        emit networkGalleryLoaded(QVector<FaceRecord>());
        emit recognitionGalleriesLoaded(QVector<FaceRecord>(),
                                        QVector<FaceRecord>());
        return;
    }

    QVector<FaceRecord> records;
    // 加载启用特征后通过信号发送拷贝，供识别线程更新底库。
    if (!repository_.loadAllEnabledFaces(records)) {
        qWarning().noquote() << "底库重载失败：" << repository_.lastError();
        emit databaseReady(false, repository_.lastError());
        emit galleryLoaded(QVector<FaceRecord>());
        emit networkGalleryLoaded(QVector<FaceRecord>());
        emit recognitionGalleriesLoaded(QVector<FaceRecord>(),
                                        QVector<FaceRecord>());
        return;
    }

    QVector<FaceRecord> networkRecords;
    if (!repository_.loadAllEnabledNetworkFaces(networkRecords)) {
        qWarning().noquote() << "网络底库重载失败：" << repository_.lastError();
        networkRecords.clear();
    }

    qInfo().noquote() << "底库重载完成，本地记录数：" << records.size()
                      << "网络记录数：" << networkRecords.size();
    emit galleryLoaded(records);
    emit networkGalleryLoaded(networkRecords);
    emit recognitionGalleriesLoaded(records, networkRecords);
}

/**
 * @brief 加载管理员人员管理表格行。
 */
void DatabaseWorker::loadPeople(bool includeDeleted)
{
    if (!opened_) {
        qWarning().noquote() << "跳过人员加载：数据库未打开";
        emit peopleLoaded(QVector<PersonAdminRecord>());
        return;
    }

    QVector<PersonAdminRecord> records;
    if (!repository_.loadPeople(records, includeDeleted)) {
        qWarning().noquote() << "人员加载失败：" << repository_.lastError();
        emit peopleLoaded(QVector<PersonAdminRecord>());
        return;
    }
    emit peopleLoaded(records);
}

/**
 * @brief 加载管理员日志页面中的成功打卡记录。
 */
void DatabaseWorker::loadPassedVerifyLogs()
{
    if (!opened_) {
        qWarning().noquote() << "跳过通过日志加载：数据库未打开";
        emit passedVerifyLogsLoaded(QVector<VerifyLogViewRecord>());
        return;
    }

    QVector<VerifyLogViewRecord> records;
    if (!repository_.loadPassedVerifyLogs(records, 300)) {
        qWarning().noquote() << "通过日志加载失败：" << repository_.lastError();
        emit passedVerifyLogsLoaded(QVector<VerifyLogViewRecord>());
        return;
    }
    emit passedVerifyLogsLoaded(records);
}

/** @brief 按筛选条件加载验证日志。 */
void DatabaseWorker::loadVerifyLogs(const VerifyLogFilter &filter)
{
    if (!opened_) {
        qWarning().noquote() << "跳过通行记录加载：数据库未打开";
        emit verifyLogsLoaded(QVector<VerifyLogViewRecord>());
        return;
    }

    QVector<VerifyLogViewRecord> records;
    if (!repository_.loadVerifyLogs(filter, records)) {
        qWarning().noquote() << "通行记录加载失败：" << repository_.lastError();
        emit verifyLogsLoaded(QVector<VerifyLogViewRecord>());
        return;
    }
    emit verifyLogsLoaded(records);
}

/** @brief 加载同步任务。 */
void DatabaseWorker::loadSyncTasks()
{
    if (!opened_) {
        emit syncTasksLoaded(QVector<SyncTaskRecord>());
        return;
    }
    QVector<SyncTaskRecord> records;
    if (!repository_.loadSyncTasks(records, 300)) {
        qWarning().noquote() << "同步队列加载失败：" << repository_.lastError();
        emit syncTasksLoaded(QVector<SyncTaskRecord>());
        return;
    }
    emit syncTasksLoaded(records);
}

/** @brief 加载系统事件。 */
void DatabaseWorker::loadSystemEvents()
{
    if (!opened_) {
        emit systemEventsLoaded(QVector<SystemEventLog>());
        return;
    }
    QVector<SystemEventLog> records;
    if (!repository_.loadSystemEvents(records, 20)) {
        qWarning().noquote() << "系统事件加载失败：" << repository_.lastError();
        emit systemEventsLoaded(QVector<SystemEventLog>());
        return;
    }
    emit systemEventsLoaded(records);
}

/**
 * @brief 加载本机信息页使用的存储统计。
 */
void DatabaseWorker::loadStorageStats()
{
    StorageStats stats;
    if (!opened_) {
        qWarning().noquote() << "跳过存储统计：数据库未打开";
        emit storageStatsLoaded(stats);
        return;
    }

    if (!repository_.loadStorageStats(stats)) {
        qWarning().noquote() << "存储统计失败：" << repository_.lastError();
        emit storageStatsLoaded(StorageStats());
        return;
    }
    emit storageStatsLoaded(stats);
}

/**
 * @brief 保存一名录入人员及其特征，然后刷新运行时和管理员数据。
 */
void DatabaseWorker::addPersonFace(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath)
{
    if (!opened_) {
        qWarning().noquote() << "跳过录入：数据库未打开";
        QFile::remove(imagePath);
        emit enrollFinished(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }

    if (person.personNo.trimmed().isEmpty() || person.name.trimmed().isEmpty()) {
        qWarning().noquote() << "拒绝录入：人员编号或姓名为空";
        QFile::remove(imagePath);
        emit enrollFinished(false, "人员编号和姓名不能为空");
        return;
    }

    if (feature.blob.isEmpty()) {
        qWarning().noquote() << "拒绝录入：特征数据为空";
        QFile::remove(imagePath);
        emit enrollFinished(false, "人脸特征为空");
        return;
    }

    qInfo().noquote() << "正在保存录入人脸：" << person.personNo << person.name << imagePath;
    // 录入写入由 Repository 统一处理重复校验和特征持久化。
    const bool ok = repository_.addPersonFace(person, feature, imagePath);
    if (!ok) {
        qWarning().noquote() << "保存录入人脸失败：" << repository_.lastError();
        QFile::remove(imagePath);
    }
    emit enrollFinished(ok, ok ? "人脸录入完成" : repository_.lastError());
    if (ok) {
        reloadGallery();
        loadPeople();
        loadStorageStats();
    }
}

/** @brief 批量导入人员基础信息。 */
void DatabaseWorker::importPeopleBasic(const QVector<PersonInfo> &people)
{
    if (!opened_) {
        emit peopleImportFinished(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }
    int inserted = 0;
    int updated = 0;
    const bool ok = repository_.importPeopleBasic(people, &inserted, &updated);
    const QString message = ok
        ? QString("人员导入完成：新增 %1，更新 %2").arg(inserted).arg(updated)
        : repository_.lastError();
    emit peopleImportFinished(ok, message);
    if (ok) {
        reloadGallery();
        loadPeople(false);
        loadStorageStats();
    }
}

/**
 * @brief 更新一个人员编号，然后刷新运行时和管理表格。
 */
void DatabaseWorker::updatePersonNo(qint64 personId, const QString &newPersonNo)
{
    if (!opened_) {
        emit personUpdated(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }

    const bool ok = repository_.updatePersonNo(personId, newPersonNo);
    emit personUpdated(ok, ok ? "人员编号已更新" : repository_.lastError());
    if (ok) {
        reloadGallery();
        loadPeople();
        loadStorageStats();
    }
}

/**
 * @brief 更新人员启用权限，然后刷新运行时底库和管理表格。
 */
void DatabaseWorker::updatePersonEnabled(qint64 personId, bool enabled)
{
    if (!opened_) {
        emit personUpdated(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }

    const bool ok = repository_.updatePersonEnabled(personId, enabled);
    emit personUpdated(ok, ok ? (enabled ? "人员权限已启用" : "人员权限已禁用") : repository_.lastError());
    if (ok) {
        reloadGallery();
        loadPeople();
        loadStorageStats();
    }
}

/**
 * @brief 删除一名人员及其人脸模板，然后刷新运行时和管理员数据。
 */
void DatabaseWorker::deletePerson(qint64 personId)
{
    if (!opened_) {
        emit personDeleted(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }

    const bool ok = repository_.deletePerson(personId);
    emit personDeleted(ok, ok ? "人员已删除" : repository_.lastError());
    if (ok) {
        reloadGallery();
        loadPeople();
        loadStorageStats();
    }
}

/**
 * @brief 写入一条验证通过/失败日志，且不阻塞摄像头或 UI 线程。
 */
void DatabaseWorker::addVerifyLog(const VerifyLog &log)
{
    if (!opened_) {
        qWarning().noquote() << "跳过验证日志：数据库未打开";
        emit verifyLogWritten(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }

    // 验证日志异步写入，避免阻塞摄像头采集链路。
    const bool ok = repository_.addVerifyLog(log);
    if (!ok) {
        qWarning().noquote() << "写入验证日志失败：" << repository_.lastError();
    }
    emit verifyLogWritten(ok, ok ? "验证日志已写入" : repository_.lastError());
}

/** @brief 写入管理员操作审计日志。 */
void DatabaseWorker::addOperatorAuditLog(const OperatorAuditLog &log)
{
    if (!opened_) {
        emit operatorAuditLogWritten(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }
    const bool ok = repository_.addOperatorAuditLog(log);
    emit operatorAuditLogWritten(ok, ok ? "管理员审计日志已写入" : repository_.lastError());
}

/** @brief 写入系统事件日志。 */
void DatabaseWorker::addSystemEventLog(const SystemEventLog &log)
{
    if (!opened_) {
        emit systemEventLogWritten(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }
    const bool ok = repository_.addSystemEventLog(log);
    emit systemEventLogWritten(ok, ok ? "系统事件已写入" : repository_.lastError());
}

/** @brief 清理指定天数前的验证日志。 */
void DatabaseWorker::cleanupOldVerifyLogs(int days)
{
    if (!opened_) {
        emit cleanupFinished(false, repository_.lastError().isEmpty() ? "数据库未打开" : repository_.lastError());
        return;
    }
    int removed = 0;
    const bool ok = repository_.cleanupOldVerifyLogs(days, &removed);
    emit cleanupFinished(ok, ok ? QString("已清理 %1 条过期通行日志").arg(removed) : repository_.lastError());
    if (ok) {
        loadStorageStats();
    }
}
