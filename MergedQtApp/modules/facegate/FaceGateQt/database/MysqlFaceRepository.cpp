/**
 * @file MysqlFaceRepository.cpp
 * @brief 实现 SQLite、MySQL 和旧本地文件兼容的门禁数据仓储。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "MysqlFaceRepository.h"

#include "common/storage_policy.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QVariant>
#include <QUuid>

#include <cmath>

namespace {
constexpr quint32 kLocalDbMagic = 0x46474442; // "FGDB"
constexpr quint32 kLocalDbVersion = 2;
}

/** @brief 保存数据库和本地文件配置并生成独立连接名。 */
MysqlFaceRepository::MysqlFaceRepository(const AppConfig &config)
    : config_(config),
      connectionName_("face_gate_" + QUuid::createUuid().toString(QUuid::Id128))
{
}

/** @brief 销毁前关闭 SQL 或本地文件存储。 */
MysqlFaceRepository::~MysqlFaceRepository()
{
    close();
}

/**
 * @brief 打开配置的 SQL 连接，并在本地表缺失时创建表。
 *
 * QSQLITE 是设备唯一的本地存储方式，生成的 .db 可以被
 * DB Browser for SQLite 打开。缺少 QSQLITE 时直接失败，避免再创建独立 FGDB。
 */
bool MysqlFaceRepository::open()
{
    lastError_.clear();
    localFileMode_ = false;
    sqliteMode_ = false;

    QString driver = config_.databaseDriver.trimmed().isEmpty()
        ? QStringLiteral("QSQLITE")
        : config_.databaseDriver.trimmed().toUpper();

    if (driver == QStringLiteral("SQLITE")) {
        driver = QStringLiteral("QSQLITE");
    } else if (driver == QStringLiteral("LOCAL_FILE") || driver == QStringLiteral("FGDB")) {
        driver = QStringLiteral("LOCAL");
    }

    if (driver == QStringLiteral("LOCAL")) {
        if (QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
            qInfo().noquote() << "请求使用 LOCAL 数据库驱动；为兼容 DB Browser，改用真实 SQLite 文件";
            return openSqliteStore(driver);
        }

        lastError_ = QStringLiteral("QSQLITE 驱动不可用，已禁止创建独立 FGDB 数据库");
        qWarning().noquote() << lastError_;
        return false;
    }

    if (driver == QStringLiteral("QSQLITE")) {
        // 优先打开真实 SQLite 文件，便于外部工具查看和维护。
        return openSqliteStore(driver);
    }

    // 使用实例级连接名，避免多个 Repository 之间的 Qt SQL 连接冲突。
    if (QSqlDatabase::contains(connectionName_)) {
        db_ = QSqlDatabase::database(connectionName_);
    } else {
        db_ = QSqlDatabase::addDatabase(driver, connectionName_);
    }

    if (!db_.isValid()) {
        lastError_ = QString("为驱动 %1 创建 SQL 连接失败，可用 SQL 驱动：%2")
            .arg(driver)
            .arg(QSqlDatabase::drivers().join(", "));
        qWarning().noquote() << lastError_;
        return false;
    }

    db_.setHostName(config_.mysqlHost);
    db_.setPort(config_.mysqlPort);
    db_.setUserName(config_.mysqlUser);
    db_.setPassword(config_.mysqlPassword);

    if (!ensureDatabase()) {
        return false;
    }

    db_.close();
    db_.setDatabaseName(config_.mysqlDatabase);
    if (!db_.open()) {
        lastError_ = QString("打开 MySQL 数据库 '%1' 失败：%2，可用 SQL 驱动：%3")
            .arg(config_.mysqlDatabase)
            .arg(db_.lastError().text())
            .arg(QSqlDatabase::drivers().join(", "));
        qWarning().noquote() << lastError_;
        return false;
    }

    qInfo().noquote() << "MySQL 数据库已打开：" << config_.mysqlHost << config_.mysqlPort << config_.mysqlDatabase;
    return ensureSchema();
}

/**
 * @brief 在全部排队数据库任务结束后关闭并移除 Qt SQL 连接。
 */
void MysqlFaceRepository::close()
{
    if (localFileMode_) {
        qInfo().noquote() << "本地离线数据库已关闭：" << localStoreFilePath();
        clearLocalStore();
        localFileMode_ = false;
        sqliteMode_ = false;
        return;
    }

    if (db_.isValid()) {
        db_.close();
    }
    const QString name = connectionName_;
    db_ = QSqlDatabase();
    if (QSqlDatabase::contains(name)) {
        QSqlDatabase::removeDatabase(name);
    }
    sqliteMode_ = false;
}

/**
 * @brief 插入或更新一名人员，并追加采集到的人脸特征。
 *
 * SQL 特意保持可移植，确保同一套录入流程可以同时适用于
 * 离线 SQLite 和网络 MySQL 存储。
 */
bool MysqlFaceRepository::addPersonFace(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath)
{
    QString storedImagePath = imagePath;
    if (storedImagePath.isNull()) {
        storedImagePath = QStringLiteral("");
    }

    if (localFileMode_) {
        return addPersonFaceToLocal(person, feature, storedImagePath);
    }

    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QString duplicateMessage;
    if (hasDuplicatePerson(person, &duplicateMessage) ||
        hasDuplicateFeature(feature, &duplicateMessage)) {
        lastError_ = duplicateMessage;
        qWarning().noquote() << lastError_;
        return false;
    }

    const QString now = nowString();
    if (!db_.transaction()) {
        lastError_ = "启动录入事务失败：" + db_.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    qint64 personId = -1;
    // 先写 person 表，再写 face_feature 表，保证人员与特征关联完整。
    QSqlQuery find(db_);
    find.prepare("SELECT id FROM person WHERE person_no=?");
    find.addBindValue(person.personNo);
    if (!find.exec()) {
        lastError_ = "查询人员失败：" + find.lastError().text();
        db_.rollback();
        qWarning().noquote() << lastError_;
        return false;
    }

    if (find.next()) {
        personId = find.value(0).toLongLong();
        QSqlQuery update(db_);
        update.prepare("UPDATE person SET name=?, enabled=?, deleted=?, updated_at=? WHERE id=?");
        update.addBindValue(person.name);
        update.addBindValue(person.enabled ? 1 : 0);
        update.addBindValue(0);
        update.addBindValue(now);
        update.addBindValue(personId);
        if (!update.exec()) {
            lastError_ = "更新人员失败：" + update.lastError().text();
            db_.rollback();
            qWarning().noquote() << lastError_;
            return false;
        }
    } else {
        QSqlQuery insert(db_);
        insert.prepare("INSERT INTO person(person_no, name, enabled, created_at, updated_at) VALUES(?, ?, ?, ?, ?)");
        insert.addBindValue(person.personNo);
        insert.addBindValue(person.name);
        insert.addBindValue(person.enabled ? 1 : 0);
        insert.addBindValue(now);
        insert.addBindValue(now);
        if (!insert.exec()) {
            lastError_ = "插入人员失败：" + insert.lastError().text();
            db_.rollback();
            qWarning().noquote() << lastError_;
            return false;
        }

        QVariant insertedId = insert.lastInsertId();
        if (insertedId.isValid() && !insertedId.isNull()) {
            personId = insertedId.toLongLong();
        } else {
            QSqlQuery idq(db_);
            idq.prepare("SELECT id FROM person WHERE person_no=?");
            idq.addBindValue(person.personNo);
            if (!idq.exec() || !idq.next()) {
                lastError_ = "查询已插入人员 ID 失败：" + idq.lastError().text();
                db_.rollback();
                qWarning().noquote() << lastError_;
                return false;
            }
            personId = idq.value(0).toLongLong();
        }
    }

    QSqlQuery featureCountQuery(db_);
    featureCountQuery.prepare("SELECT COUNT(*) FROM face_feature WHERE person_id=?");
    featureCountQuery.addBindValue(personId);
    if (!featureCountQuery.exec() || !featureCountQuery.next()) {
        lastError_ = "统计人员人脸模板失败：" + featureCountQuery.lastError().text();
        db_.rollback();
        qWarning().noquote() << lastError_;
        return false;
    }
    if (featureCountQuery.value(0).toInt() >= config_.maxFeaturesPerPerson) {
        lastError_ = QString("该人员人脸模板已达到上限：%1 张").arg(config_.maxFeaturesPerPerson);
        db_.rollback();
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery featureQuery(db_);
    featureQuery.prepare("INSERT INTO face_feature(person_id, image_path, feature_blob, feature_size, model_version, quality, created_at) "
                         "VALUES(?, ?, ?, ?, ?, ?, ?)");
    featureQuery.addBindValue(personId);
    featureQuery.addBindValue(storedImagePath);
    featureQuery.addBindValue(feature.blob);
    featureQuery.addBindValue(feature.blob.size());
    featureQuery.addBindValue(feature.modelVersion);
    featureQuery.addBindValue(feature.quality);
    featureQuery.addBindValue(now);
    if (!featureQuery.exec()) {
        lastError_ = "保存人脸特征失败：" + featureQuery.lastError().text();
        db_.rollback();
        qWarning().noquote() << lastError_;
        return false;
    }

    if (!db_.commit()) {
        lastError_ = "提交录入事务失败：" + db_.lastError().text();
        db_.rollback();
        qWarning().noquote() << lastError_;
        return false;
    }

    qInfo().noquote() << "人脸已录入数据库：" << person.personNo << person.name << storedImagePath;
    return true;
}

/**
 * @brief 加载全部启用的人脸特征，用于内存一对一比对。
 */
bool MysqlFaceRepository::loadAllEnabledFaces(QVector<FaceRecord> &records)
{
    if (localFileMode_) {
        return loadAllEnabledFacesFromLocal(records);
    }

    records.clear();
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("SELECT p.id, p.person_no, p.name, p.enabled, p.created_at, p.updated_at, "
              "f.id, f.image_path, f.feature_blob, f.feature_size, f.model_version, f.quality, f.created_at "
              "FROM person p JOIN face_feature f ON f.person_id=p.id "
              "WHERE p.enabled=1 AND COALESCE(p.deleted, 0)=0");

    if (!q.exec()) {
        lastError_ = "加载启用人脸失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    while (q.next()) {
        FaceRecord r;
        r.person.id = q.value(0).toLongLong();
        r.person.personNo = q.value(1).toString();
        r.person.name = q.value(2).toString();
        r.person.enabled = q.value(3).toInt() != 0;
        r.person.createdAt = q.value(4).toString();
        r.person.updatedAt = q.value(5).toString();
        r.featureId = q.value(6).toLongLong();
        r.imagePath = q.value(7).toString();
        r.feature.blob = q.value(8).toByteArray();
        r.feature.modelVersion = q.value(10).toString();
        r.feature.quality = q.value(11).toFloat();
        r.enrolledAt = q.value(12).toString();
        records.push_back(r);
    }

    qInfo().noquote() << "已加载启用的人脸底库：" << records.size();
    return true;
}

/**
 * @brief 加载状态有效且已具备本地特征的网络人员人脸图库。
 *
 * 本地文件兼容模式没有网络人员表，按空图库成功返回；数据库模式下只选择
 * IMAGE_READY 记录，避免把尚未完成图像校验的同步数据交给识别线程。
 */
bool MysqlFaceRepository::loadAllEnabledNetworkFaces(
        QVector<FaceRecord> &records)
{
    records.clear();
    if (localFileMode_) {
        return true;
    }
    if (!db_.isOpen()) {
        lastError_ = QStringLiteral("数据库连接未打开");
        return false;
    }

    const QStringList tables = db_.tables();
    if (!tables.contains(QStringLiteral("network_person"),
                         Qt::CaseInsensitive)
            || !tables.contains(QStringLiteral("network_face"),
                                Qt::CaseInsensitive)) {
        return true;
    }

    QSqlQuery q(db_);
    q.prepare(QStringLiteral(
        "SELECT p.person_id,p.name,p.created_at,p.updated_at,"
        "f.id,f.name,f.image_path,f.feature_blob,f.model_version,f.bind_time,"
        "f.face_hash "
        "FROM network_person p "
        "JOIN network_face f ON f.person_id=p.person_id "
        "WHERE p.active=1 AND p.deleted=0 AND p.status=1 "
        "AND f.status='IMAGE_READY' "
        "AND f.feature_blob IS NOT NULL "
        "ORDER BY p.person_id,f.id"));
    if (!q.exec()) {
        lastError_ = QStringLiteral("加载网络人脸底库失败：")
                + q.lastError().text();
        return false;
    }

    while (q.next()) {
        const QString personId = q.value(0).toString().trimmed();
        const QByteArray featureBlob = q.value(7).toByteArray();
        const QString modelVersion = q.value(8).toString().trimmed();
        if (personId.isEmpty()
                || featureBlob.isEmpty()
                || featureBlob.size() % static_cast<int>(sizeof(float)) != 0
                || modelVersion.isEmpty()) {
            continue;
        }

        FaceRecord record;
        record.featureId = 0;
        record.person.id = 0;
        record.person.personNo = personId;
        record.person.name = q.value(1).toString().trimmed();
        if (record.person.name.isEmpty()) {
            record.person.name = q.value(5).toString().trimmed();
        }
        if (record.person.name.isEmpty()) {
            record.person.name = personId;
        }
        record.person.enabled = true;
        record.person.deleted = false;
        record.person.createdAt = q.value(2).toString();
        record.person.updatedAt = q.value(3).toString();
        record.faceHash = q.value(10).toString().trimmed();
        record.imagePath = q.value(6).toString();
        record.feature.blob = featureBlob;
        record.feature.modelVersion = modelVersion;
        record.enrolledAt = q.value(9).toString();
        records.push_back(record);
    }

    qInfo().noquote() << "已加载启用的网络人脸底库：" << records.size();
    return true;
}

/** @brief 批量新增或更新人员基础信息。 */
bool MysqlFaceRepository::importPeopleBasic(const QVector<PersonInfo> &people, int *inserted, int *updated)
{
    if (inserted) { *inserted = 0; }
    if (updated) { *updated = 0; }
    if (localFileMode_) {
        return importPeopleBasicToLocal(people, inserted, updated);
    }
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        return false;
    }
    if (!db_.transaction()) {
        lastError_ = "启动人员导入事务失败：" + db_.lastError().text();
        return false;
    }
    const QString now = nowString();
    for (const PersonInfo &person : people) {
        const QString personNo = person.personNo.trimmed();
        const QString name = person.name.trimmed();
        if (personNo.isEmpty() || name.isEmpty()) {
            continue;
        }
        QSqlQuery find(db_);
        find.prepare("SELECT id FROM person WHERE person_no=? LIMIT 1");
        find.addBindValue(personNo);
        if (!find.exec()) {
            lastError_ = "查询导入人员失败：" + find.lastError().text();
            db_.rollback();
            return false;
        }
        if (find.next()) {
            QSqlQuery update(db_);
            update.prepare("UPDATE person SET name=?, enabled=?, deleted=0, updated_at=? WHERE id=?");
            update.addBindValue(name);
            update.addBindValue(person.enabled ? 1 : 0);
            update.addBindValue(now);
            update.addBindValue(find.value(0).toLongLong());
            if (!update.exec()) {
                lastError_ = "更新导入人员失败：" + update.lastError().text();
                db_.rollback();
                return false;
            }
            if (updated) { ++(*updated); }
        } else {
            QSqlQuery insert(db_);
            insert.prepare("INSERT INTO person(person_no, name, enabled, deleted, created_at, updated_at) VALUES(?, ?, ?, 0, ?, ?)");
            insert.addBindValue(personNo);
            insert.addBindValue(name);
            insert.addBindValue(person.enabled ? 1 : 0);
            insert.addBindValue(now);
            insert.addBindValue(now);
            if (!insert.exec()) {
                lastError_ = "插入导入人员失败：" + insert.lastError().text();
                db_.rollback();
                return false;
            }
            if (inserted) { ++(*inserted); }
        }
        enqueueSyncTask(QStringLiteral("person_basic"), QString("{\"person_no\":\"%1\",\"name\":\"%2\"}").arg(personNo, name));
    }
    if (!db_.commit()) {
        lastError_ = "提交人员导入事务失败：" + db_.lastError().text();
        db_.rollback();
        return false;
    }
    return true;
}


/**
 * @brief 为管理员“人员”页面按人员加载一行管理数据。
 */
bool MysqlFaceRepository::loadPeople(QVector<PersonAdminRecord> &records, bool includeDeleted)
{
    if (localFileMode_) {
        return loadPeopleFromLocal(records, includeDeleted);
    }

    records.clear();
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("SELECT p.id, p.person_no, p.name, p.enabled, p.created_at, p.updated_at, "
              "COALESCE(p.deleted, 0), COUNT(f.id), MIN(f.created_at), MAX(f.created_at) "
              "FROM person p LEFT JOIN face_feature f ON f.person_id=p.id "
              "WHERE (?=1 OR COALESCE(p.deleted, 0)=0) "
              "GROUP BY p.id, p.person_no, p.name, p.enabled, p.created_at, p.updated_at, COALESCE(p.deleted, 0) "
              "ORDER BY p.id DESC");
    q.addBindValue(includeDeleted ? 1 : 0);
    if (!q.exec()) {
        lastError_ = "加载人员失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    while (q.next()) {
        PersonAdminRecord record;
        record.id = q.value(0).toLongLong();
        record.personNo = q.value(1).toString();
        record.name = q.value(2).toString();
        record.enabled = q.value(3).toInt() != 0;
        record.createdAt = q.value(4).toString();
        record.updatedAt = q.value(5).toString();
        record.deleted = q.value(6).toInt() != 0;
        record.featureCount = q.value(7).toInt();
        record.firstFeatureAt = q.value(8).toString();
        record.lastFeatureAt = q.value(9).toString();
        records.push_back(record);
    }

    return true;
}

/**
 * @brief 仅为管理员“识别记录”页面加载成功验证记录。
 */
bool MysqlFaceRepository::loadPassedVerifyLogs(QVector<VerifyLogViewRecord> &records, int limit)
{
    if (localFileMode_) {
        return loadPassedVerifyLogsFromLocal(records, limit);
    }

    records.clear();
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    const int safeLimit = qBound(1, limit, 1000);
    QSqlQuery q(db_);
    q.prepare(QString("SELECT l.id, l.person_id, COALESCE(p.person_no, l.person_no), COALESCE(p.name, l.name_snapshot, ''), "
                      "l.result, l.cosine, l.live_score, l.snapshot_path, l.name_snapshot, l.fail_reason, "
                      "l.device_sn, l.event_type, l.direction, l.created_at "
                      "FROM verify_log l LEFT JOIN person p ON p.id=l.person_id "
                      "WHERE l.result='passed' ORDER BY l.created_at DESC LIMIT %1").arg(safeLimit));
    if (!q.exec()) {
        lastError_ = "加载通过验证日志失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    while (q.next()) {
        VerifyLogViewRecord record;
        record.logId = q.value(0).toLongLong();
        record.personId = q.value(1).toLongLong();
        record.personNo = q.value(2).toString();
        record.name = q.value(3).toString();
        record.result = q.value(4).toString();
        record.cosine = q.value(5).toFloat();
        record.liveScore = q.value(6).toFloat();
        record.snapshotPath = q.value(7).toString();
        record.nameSnapshot = q.value(8).toString();
        record.failReason = q.value(9).toString();
        record.deviceSn = q.value(10).toString();
        record.eventType = q.value(11).toString();
        record.direction = q.value(12).toString();
        record.createdAt = q.value(13).toString();
        records.push_back(record);
    }

    return true;
}

/**
 * @brief 读取本机信息页需要的存储统计。
 */
bool MysqlFaceRepository::loadStorageStats(StorageStats &stats)
{
    if (localFileMode_) {
        return loadStorageStatsFromLocal(stats);
    }

    stats = StorageStats();
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    /** @brief 一项存储统计查询及其结果写入位置。 */
    struct CountQuery {
        const char *sql;    /**< 只返回单个 COUNT 值的 SQL。 */
        int *target;        /**< StorageStats 中对应的结果字段。 */
        const char *name;   /**< 查询失败日志使用的中文名称。 */
    };

    CountQuery queries[] = {
        {"SELECT COUNT(*) FROM person", &stats.personCount, "人员"},
        {"SELECT COUNT(*) FROM face_feature", &stats.faceFeatureCount, "注册照"},
        {"SELECT COUNT(*) FROM verify_log", &stats.verifyLogCount, "验证日志"},
    };

    for (const CountQuery &entry : queries) {
        QSqlQuery q(db_);
        if (!q.exec(QString::fromLatin1(entry.sql)) || !q.next()) {
            lastError_ = QString("统计%1失败：%2").arg(QString::fromUtf8(entry.name)).arg(q.lastError().text());
            qWarning().noquote() << lastError_;
            return false;
        }
        *entry.target = q.value(0).toInt();
    }

    const StoragePolicy::Usage registration = StoragePolicy::registrationUsage();
    stats.faceFeatureCount = registration.fileCount;
    stats.registrationPhotoBytes = registration.bytes;

    return true;
}

/**
 * @brief 只修改人员编号，操作员可见姓名保持不变。
 */
bool MysqlFaceRepository::updatePersonNo(qint64 personId, const QString &newPersonNo)
{
    if (localFileMode_) {
        return updatePersonNoInLocal(personId, newPersonNo);
    }

    const QString trimmedNo = newPersonNo.trimmed();
    if (personId <= 0 || trimmedNo.isEmpty()) {
        lastError_ = "人员 ID 和新人员编号不能为空";
        qWarning().noquote() << lastError_;
        return false;
    }
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery dup(db_);
    dup.prepare("SELECT id FROM person WHERE person_no=? AND id<>? LIMIT 1");
    dup.addBindValue(trimmedNo);
    dup.addBindValue(personId);
    if (!dup.exec()) {
        lastError_ = "检查人员编号失败：" + dup.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    if (dup.next()) {
        lastError_ = "人员编号已存在";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("UPDATE person SET person_no=?, updated_at=? WHERE id=?");
    q.addBindValue(trimmedNo);
    q.addBindValue(nowString());
    q.addBindValue(personId);
    if (!q.exec()) {
        lastError_ = "更新人员编号失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    if (q.numRowsAffected() <= 0) {
        lastError_ = "未找到人员";
        qWarning().noquote() << lastError_;
        return false;
    }

    return true;
}

/**
 * @brief 更新人员启用权限，禁用后该人员不会加载到底库参与验证。
 */
bool MysqlFaceRepository::updatePersonEnabled(qint64 personId, bool enabled)
{
    if (localFileMode_) {
        return updatePersonEnabledInLocal(personId, enabled);
    }

    if (personId <= 0) {
        lastError_ = "需要有效的人员 ID";
        qWarning().noquote() << lastError_;
        return false;
    }
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("UPDATE person SET enabled=?, updated_at=? WHERE id=?");
    q.addBindValue(enabled ? 1 : 0);
    q.addBindValue(nowString());
    q.addBindValue(personId);
    if (!q.exec()) {
        lastError_ = "更新人员权限失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    if (q.numRowsAffected() <= 0) {
        lastError_ = "未找到人员";
        qWarning().noquote() << lastError_;
        return false;
    }

    return true;
}

/**
 * @brief 删除一名人员及其全部已录入人脸模板。
 */
bool MysqlFaceRepository::deletePerson(qint64 personId)
{
    if (localFileMode_) {
        return deletePersonFromLocal(personId);
    }

    if (personId <= 0) {
        lastError_ = "需要有效的人员 ID";
        qWarning().noquote() << lastError_;
        return false;
    }
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery delPerson(db_);
    delPerson.prepare("UPDATE person SET deleted=1, enabled=0, updated_at=? WHERE id=?");
    delPerson.addBindValue(nowString());
    delPerson.addBindValue(personId);
    if (!delPerson.exec()) {
        lastError_ = "软删除人员失败：" + delPerson.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    if (delPerson.numRowsAffected() <= 0) {
        lastError_ = "未找到人员";
        qWarning().noquote() << lastError_;
        return false;
    }

    return true;
}

/**
 * @brief 追加一条验证结果记录，不阻塞摄像头处理。
 */
bool MysqlFaceRepository::addVerifyLog(const VerifyLog &log)
{
    if (localFileMode_) {
        return addVerifyLogToLocal(log);
    }

    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("INSERT INTO verify_log(person_id, person_no, result, cosine, live_score, snapshot_path, "
              "name_snapshot, fail_reason, device_sn, event_type, direction, created_at) "
              "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(log.personId > 0 ? QVariant(log.personId) : QVariant());
    q.addBindValue(log.personNo);
    q.addBindValue(log.result);
    q.addBindValue(log.cosine);
    q.addBindValue(log.liveScore);
    q.addBindValue(log.snapshotPath);
    q.addBindValue(log.nameSnapshot);
    q.addBindValue(log.failReason);
    q.addBindValue(log.deviceSn);
    q.addBindValue(log.eventType);
    q.addBindValue(log.direction);
    q.addBindValue(log.createdAt.toString("yyyy-MM-dd HH:mm:ss"));
    if (!q.exec()) {
        lastError_ = "写入验证日志失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    if (log.result == QStringLiteral("passed") && !log.personNo.isEmpty()) {
        const QString createdAt = log.createdAt.toString("yyyy-MM-dd HH:mm:ss");
        const QString workDate = log.createdAt.date().toString("yyyy-MM-dd");
        QSqlQuery findDaily(db_);
        findDaily.prepare("SELECT id, first_pass_at, last_pass_at, pass_count FROM attendance_daily "
                          "WHERE person_no=? AND work_date=? LIMIT 1");
        findDaily.addBindValue(log.personNo);
        findDaily.addBindValue(workDate);
        if (findDaily.exec() && findDaily.next()) {
            QSqlQuery updateDaily(db_);
            updateDaily.prepare("UPDATE attendance_daily SET first_pass_at=?, last_pass_at=?, pass_count=?, updated_at=? WHERE id=?");
            const QString firstPass = findDaily.value(1).toString().isEmpty()
                ? createdAt
                : qMin(findDaily.value(1).toString(), createdAt);
            const QString lastPass = qMax(findDaily.value(2).toString(), createdAt);
            updateDaily.addBindValue(firstPass);
            updateDaily.addBindValue(lastPass);
            updateDaily.addBindValue(findDaily.value(3).toInt() + 1);
            updateDaily.addBindValue(createdAt);
            updateDaily.addBindValue(findDaily.value(0).toLongLong());
            updateDaily.exec();
        } else {
            QSqlQuery insertDaily(db_);
            insertDaily.prepare("INSERT INTO attendance_daily(person_id, person_no, name_snapshot, work_date, "
                                "first_pass_at, last_pass_at, pass_count, updated_at) VALUES(?, ?, ?, ?, ?, ?, 1, ?)");
            insertDaily.addBindValue(log.personId > 0 ? QVariant(log.personId) : QVariant());
            insertDaily.addBindValue(log.personNo);
            insertDaily.addBindValue(log.nameSnapshot);
            insertDaily.addBindValue(workDate);
            insertDaily.addBindValue(createdAt);
            insertDaily.addBindValue(createdAt);
            insertDaily.addBindValue(createdAt);
            insertDaily.exec();
        }
    }

    const QString payload = QString("{\"log_id\":%1,\"person_no\":\"%2\",\"result\":\"%3\",\"created_at\":\"%4\"}")
        .arg(q.lastInsertId().toLongLong())
        .arg(log.personNo)
        .arg(log.result)
        .arg(log.createdAt.toString("yyyy-MM-dd HH:mm:ss"));
    enqueueSyncTask(QStringLiteral("verify_log"), payload);
    return true;
}

/** @brief 按筛选器加载验证日志。 */
bool MysqlFaceRepository::loadVerifyLogs(const VerifyLogFilter &filter, QVector<VerifyLogViewRecord> &records)
{
    if (localFileMode_) {
        return loadVerifyLogsFromLocal(filter, records);
    }

    records.clear();
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        qWarning().noquote() << lastError_;
        return false;
    }

    QStringList where;
    QList<QVariant> binds;
    if (!filter.result.trimmed().isEmpty() && filter.result != QStringLiteral("all")) {
        where << "l.result=?";
        binds << filter.result.trimmed();
    }
    if (!filter.personNo.trimmed().isEmpty()) {
        where << "COALESCE(p.person_no, l.person_no) LIKE ?";
        binds << ("%" + filter.personNo.trimmed() + "%");
    }
    if (!filter.name.trimmed().isEmpty()) {
        where << "COALESCE(p.name, l.name_snapshot, '') LIKE ?";
        binds << ("%" + filter.name.trimmed() + "%");
    }
    if (filter.startTime.isValid()) {
        where << "l.created_at>=?";
        binds << filter.startTime.toString("yyyy-MM-dd HH:mm:ss");
    }
    if (filter.endTime.isValid()) {
        where << "l.created_at<=?";
        binds << filter.endTime.toString("yyyy-MM-dd HH:mm:ss");
    }

    const int safeLimit = qBound(1, filter.limit, 5000);
    QString sql = "SELECT l.id, l.person_id, COALESCE(p.person_no, l.person_no), COALESCE(p.name, l.name_snapshot, ''), "
                  "l.result, l.cosine, l.live_score, l.snapshot_path, l.name_snapshot, l.fail_reason, "
                  "l.device_sn, l.event_type, l.direction, l.created_at "
                  "FROM verify_log l LEFT JOIN person p ON p.id=l.person_id";
    if (!where.isEmpty()) {
        sql += " WHERE " + where.join(" AND ");
    }
    sql += QString(" ORDER BY l.created_at DESC LIMIT %1").arg(safeLimit);

    QSqlQuery q(db_);
    q.prepare(sql);
    for (const QVariant &bind : binds) {
        q.addBindValue(bind);
    }
    if (!q.exec()) {
        lastError_ = "加载通行记录失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    while (q.next()) {
        VerifyLogViewRecord record;
        record.logId = q.value(0).toLongLong();
        record.personId = q.value(1).toLongLong();
        record.personNo = q.value(2).toString();
        record.name = q.value(3).toString();
        record.result = q.value(4).toString();
        record.cosine = q.value(5).toFloat();
        record.liveScore = q.value(6).toFloat();
        record.snapshotPath = q.value(7).toString();
        record.nameSnapshot = q.value(8).toString();
        record.failReason = q.value(9).toString();
        record.deviceSn = q.value(10).toString();
        record.eventType = q.value(11).toString();
        record.direction = q.value(12).toString();
        record.createdAt = q.value(13).toString();
        records.push_back(record);
    }
    return true;
}

/** @brief 加载待处理同步任务。 */
bool MysqlFaceRepository::loadSyncTasks(QVector<SyncTaskRecord> &records, int limit)
{
    if (localFileMode_) {
        return loadSyncTasksFromLocal(records, limit);
    }
    records.clear();
    const int safeLimit = qBound(1, limit, 1000);
    QSqlQuery q(db_);
    q.prepare(QString("SELECT id, task_type, payload_json, sync_status, retry_count, last_error, created_at, updated_at "
                      "FROM sync_task ORDER BY created_at DESC LIMIT %1").arg(safeLimit));
    if (!q.exec()) {
        lastError_ = "加载同步队列失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    while (q.next()) {
        SyncTaskRecord record;
        record.id = q.value(0).toLongLong();
        record.taskType = q.value(1).toString();
        record.payloadJson = q.value(2).toString();
        record.syncStatus = q.value(3).toString();
        record.retryCount = q.value(4).toInt();
        record.lastError = q.value(5).toString();
        record.createdAt = q.value(6).toString();
        record.updatedAt = q.value(7).toString();
        records.push_back(record);
    }
    return true;
}

/** @brief 加载最近系统事件。 */
bool MysqlFaceRepository::loadSystemEvents(QVector<SystemEventLog> &records, int limit)
{
    if (localFileMode_) {
        return loadSystemEventsFromLocal(records, limit);
    }
    records.clear();
    const int safeLimit = qBound(1, limit, 1000);
    QSqlQuery q(db_);
    q.prepare(QString("SELECT id, event_type, level, message, detail, created_at "
                      "FROM system_event_log WHERE level='error' ORDER BY created_at DESC LIMIT %1").arg(safeLimit));
    if (!q.exec()) {
        lastError_ = "加载系统事件失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    while (q.next()) {
        SystemEventLog record;
        record.id = q.value(0).toLongLong();
        record.eventType = q.value(1).toString();
        record.level = q.value(2).toString();
        record.message = q.value(3).toString();
        record.detail = q.value(4).toString();
        record.createdAt = q.value(5).toString();
        records.push_back(record);
    }
    return true;
}

/** @brief 写入管理员操作审计日志。 */
bool MysqlFaceRepository::addOperatorAuditLog(const OperatorAuditLog &log)
{
    if (localFileMode_) {
        return addOperatorAuditLogToLocal(log);
    }
    QSqlQuery q(db_);
    q.prepare("INSERT INTO operator_audit_log(operator_name, action, target_type, target_id, result, detail, created_at) "
              "VALUES(?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(log.operatorName);
    q.addBindValue(log.action);
    q.addBindValue(log.targetType);
    q.addBindValue(log.targetId);
    q.addBindValue(log.result);
    q.addBindValue(log.detail);
    q.addBindValue(log.createdAt.isEmpty() ? nowString() : log.createdAt);
    if (!q.exec()) {
        lastError_ = "写入管理员审计日志失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    return true;
}

/** @brief 写入系统事件日志。 */
bool MysqlFaceRepository::addSystemEventLog(const SystemEventLog &log)
{
    if (localFileMode_) {
        return addSystemEventLogToLocal(log);
    }
    QSqlQuery q(db_);
    q.prepare("INSERT INTO system_event_log(event_type, level, message, detail, created_at) "
              "VALUES(?, ?, ?, ?, ?)");
    q.addBindValue(log.eventType);
    q.addBindValue(log.level);
    q.addBindValue(log.message);
    q.addBindValue(log.detail);
    q.addBindValue(log.createdAt.isEmpty() ? nowString() : log.createdAt);
    if (!q.exec()) {
        lastError_ = "写入系统事件失败：" + q.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }
    return true;
}

/** @brief 清理指定天数前的验证日志并统计删除数。 */
bool MysqlFaceRepository::cleanupOldVerifyLogs(int days, int *removed)
{
    if (removed) { *removed = 0; }
    if (localFileMode_) {
        return cleanupOldVerifyLogsFromLocal(days, removed);
    }
    if (!db_.isOpen()) {
        lastError_ = "数据库连接未打开";
        return false;
    }
    const QString cutoff = QDateTime::currentDateTime().addDays(-qMax(1, days)).toString("yyyy-MM-dd HH:mm:ss");
    QSqlQuery q(db_);
    q.prepare("DELETE FROM verify_log WHERE created_at < ?");
    q.addBindValue(cutoff);
    if (!q.exec()) {
        lastError_ = "清理通行日志失败：" + q.lastError().text();
        return false;
    }
    if (removed) {
        *removed = qMax(0, q.numRowsAffected());
    }
    return true;
}

/** @return 最近一次仓储错误文本。 */
QString MysqlFaceRepository::lastError() const
{
    return lastError_;
}

/**
 * @brief 打开 DB Browser for SQLite 可直接使用的真实 SQLite 数据库文件。
 */
bool MysqlFaceRepository::openSqliteStore(const QString &requestedDriver)
{
    if (QSqlDatabase::contains(connectionName_)) {
        db_ = QSqlDatabase::database(connectionName_);
    } else {
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    }

    if (!db_.isValid()) {
        lastError_ = QString("创建 SQLite 连接失败，可用 SQL 驱动：%1")
            .arg(QSqlDatabase::drivers().join(", "));
        qWarning().noquote() << lastError_;
        return false;
    }

    sqliteMode_ = true;
    const QString dbPath = sqliteDatabasePath();
    const QFileInfo info(dbPath);
    if (!QDir().mkpath(info.absolutePath())) {
        lastError_ = "创建 SQLite 数据库目录失败：" + info.absolutePath();
        qWarning().noquote() << lastError_;
        sqliteMode_ = false;
        return false;
    }

    bool migrateLegacyLocalStore = false;
    if (!prepareSqliteFile(dbPath, &migrateLegacyLocalStore)) {
        sqliteMode_ = false;
        return false;
    }

    db_.setDatabaseName(dbPath);
    if (!db_.open()) {
        lastError_ = QString("打开 SQLite 数据库失败：%1，路径：%2，可用 SQL 驱动：%3")
            .arg(db_.lastError().text())
            .arg(dbPath)
            .arg(QSqlDatabase::drivers().join(", "));
        qWarning().noquote() << lastError_;
        sqliteMode_ = false;
        return false;
    }

    if (!ensureSchema()) {
        sqliteMode_ = false;
        return false;
    }

    if (migrateLegacyLocalStore) {
        if (!migrateLocalStoreToSqlite()) {
            db_.close();
            sqliteMode_ = false;
            return false;
        }
        clearLocalStore();
    }

    qInfo().noquote() << "SQLite 数据库已打开：" << dbPath << "请求驱动：" << requestedDriver;
    return true;
}

/**
 * @brief 校验 SQLite 目标路径，并在发现旧 FGDB 内容时执行迁移。
 */
bool MysqlFaceRepository::prepareSqliteFile(const QString &path, bool *migrateLegacyLocalStore)
{
    if (migrateLegacyLocalStore) {
        *migrateLegacyLocalStore = false;
    }

    const QFileInfo info(path);
    if (!info.exists() || info.size() == 0) {
        return true;
    }

    if (isExistingSqliteDatabase(path)) {
        return true;
    }

    if (!isExistingLegacyLocalDatabase(path)) {
        lastError_ = "配置的 SQLite 路径已存在，但不是 SQLite 数据库：" + path;
        qWarning().noquote() << lastError_;
        return false;
    }

    if (!loadLocalStoreFromFile(path)) {
        return false;
    }

    QString backupPath = path + QStringLiteral(".fgdb.bak");
    if (QFileInfo::exists(backupPath)) {
        backupPath = path + QStringLiteral(".")
            + QDateTime::currentDateTime().toString("yyyyMMddHHmmss")
            + QStringLiteral(".fgdb.bak");
    }

    if (!QFile::rename(path, backupPath)) {
        lastError_ = "SQLite 迁移前移动旧本地数据库失败：" + path;
        qWarning().noquote() << lastError_;
        return false;
    }

    if (migrateLegacyLocalStore) {
        *migrateLegacyLocalStore = true;
    }
    qWarning().noquote() << "旧 FGDB 本地存储已移动到备份：" << backupPath
                         << "即将在此处创建真实 SQLite 数据库：" << path;
    return true;
}

/**
 * @brief 检查已有文件是否带有 SQLite 文件签名。
 */
bool MysqlFaceRepository::isExistingSqliteDatabase(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray header = file.read(16);
    return header == QByteArray("SQLite format 3\000", 16);
}

/**
 * @brief 检查已有文件是否为旧版基于 QDataStream 的 FGDB 存储。
 */
bool MysqlFaceRepository::isExistingLegacyLocalDatabase(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_6);
    quint32 magic = 0;
    quint32 version = 0;
    in >> magic >> version;
    return in.status() == QDataStream::Ok && magic == kLocalDbMagic && version >= 1 && version <= kLocalDbVersion;
}

/**
 * @brief 从指定路径加载旧版无驱动 FGDB 本地存储。
 */
bool MysqlFaceRepository::loadLocalStoreFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        lastError_ = "打开本地数据库文件失败：" + file.errorString();
        qWarning().noquote() << lastError_;
        return false;
    }

    clearLocalStore();

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_6);

    quint32 magic = 0;
    quint32 version = 0;
    in >> magic >> version;
    if (in.status() != QDataStream::Ok || magic != kLocalDbMagic || version < 1 || version > kLocalDbVersion) {
        lastError_ = "本地数据库文件格式无效，请检查 local_store_path 或重新创建本地数据库文件";
        clearLocalStore();
        qWarning().noquote() << lastError_;
        return false;
    }

    in >> localLastPersonId_ >> localLastFeatureId_ >> localLastLogId_;

    quint32 personCount = 0;
    in >> personCount;
    for (quint32 i = 0; i < personCount; ++i) {
        PersonInfo person;
        in >> person.id >> person.personNo >> person.name >> person.enabled;
        if (version >= 2) {
            in >> person.createdAt >> person.updatedAt;
        }
        localPersons_.push_back(person);
    }

    quint32 featureCount = 0;
    in >> featureCount;
    for (quint32 i = 0; i < featureCount; ++i) {
        LocalFeatureEntry entry;
        in >> entry.id
           >> entry.personId
           >> entry.imagePath
           >> entry.feature.blob
           >> entry.feature.modelVersion
           >> entry.feature.quality
           >> entry.createdAt;
        localFeatures_.push_back(entry);
    }

    quint32 logCount = 0;
    in >> logCount;
    for (quint32 i = 0; i < logCount; ++i) {
        VerifyLog log;
        if (version >= 2) {
            in >> log.logId;
        }
        in >> log.personId
           >> log.personNo
           >> log.result
           >> log.cosine
           >> log.liveScore
           >> log.snapshotPath
           >> log.createdAt;
        if (log.logId <= 0) {
            log.logId = static_cast<qint64>(i + 1);
        }
        localVerifyLogs_.push_back(log);
    }

    if (in.status() != QDataStream::Ok) {
        lastError_ = "读取本地数据库文件失败";
        clearLocalStore();
        qWarning().noquote() << lastError_;
        return false;
    }

    return true;
}

/**
 * @brief 将旧 FGDB 本地存储复制迁移到真实 SQLite 表中。
 */
bool MysqlFaceRepository::migrateLocalStoreToSqlite()
{
    if (!db_.isOpen() || !isSqlite()) {
        lastError_ = "SQLite 数据库未打开，无法执行本地迁移";
        qWarning().noquote() << lastError_;
        return false;
    }

    if (!db_.transaction()) {
        lastError_ = "启动本地到 SQLite 迁移事务失败：" + db_.lastError().text();
        qWarning().noquote() << lastError_;
        return false;
    }

    for (const PersonInfo &person : localPersons_) {
        QSqlQuery q(db_);
        q.prepare("INSERT OR IGNORE INTO person(id, person_no, name, enabled, created_at, updated_at) "
                  "VALUES(?, ?, ?, ?, ?, ?)");
        q.addBindValue(person.id);
        q.addBindValue(person.personNo);
        q.addBindValue(person.name);
        q.addBindValue(person.enabled ? 1 : 0);
        q.addBindValue(person.createdAt.isEmpty() ? nowString() : person.createdAt);
        q.addBindValue(person.updatedAt.isEmpty() ? nowString() : person.updatedAt);
        if (!q.exec()) {
            lastError_ = "迁移人员到 SQLite 失败：" + q.lastError().text();
            db_.rollback();
            qWarning().noquote() << lastError_;
            return false;
        }
    }

    for (const LocalFeatureEntry &entry : localFeatures_) {
        QSqlQuery q(db_);
        q.prepare("INSERT OR IGNORE INTO face_feature(id, person_id, image_path, feature_blob, feature_size, model_version, quality, created_at) "
                  "VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
        q.addBindValue(entry.id);
        q.addBindValue(entry.personId);
        q.addBindValue(entry.imagePath);
        q.addBindValue(entry.feature.blob);
        q.addBindValue(entry.feature.blob.size());
        q.addBindValue(entry.feature.modelVersion);
        q.addBindValue(entry.feature.quality);
        q.addBindValue(entry.createdAt.isEmpty() ? nowString() : entry.createdAt);
        if (!q.exec()) {
            lastError_ = "迁移人脸特征到 SQLite 失败：" + q.lastError().text();
            db_.rollback();
            qWarning().noquote() << lastError_;
            return false;
        }
    }

    for (const VerifyLog &log : localVerifyLogs_) {
        QSqlQuery q(db_);
        q.prepare("INSERT OR IGNORE INTO verify_log(id, person_id, person_no, result, cosine, live_score, snapshot_path, created_at) "
                  "VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
        q.addBindValue(log.logId);
        q.addBindValue(log.personId > 0 ? QVariant(log.personId) : QVariant());
        q.addBindValue(log.personNo);
        q.addBindValue(log.result);
        q.addBindValue(log.cosine);
        q.addBindValue(log.liveScore);
        q.addBindValue(log.snapshotPath);
        q.addBindValue(log.createdAt.toString("yyyy-MM-dd HH:mm:ss"));
        if (!q.exec()) {
            lastError_ = "迁移验证日志到 SQLite 失败：" + q.lastError().text();
            db_.rollback();
            qWarning().noquote() << lastError_;
            return false;
        }
    }

    if (!db_.commit()) {
        lastError_ = "提交本地到 SQLite 迁移失败：" + db_.lastError().text();
        db_.rollback();
        qWarning().noquote() << lastError_;
        return false;
    }

    qInfo().noquote() << "旧本地存储已迁移到 SQLite"
                      << "人员：" << localPersons_.size()
                      << "特征：" << localFeatures_.size()
                      << "日志：" << localVerifyLogs_.size();
    return true;
}

/**
 * @brief 以原子方式持久化内存中的本地数据库。
 */
bool MysqlFaceRepository::saveLocalStore()
{
    QSaveFile file(localStoreFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        lastError_ = "保存本地数据库文件失败：" + file.errorString();
        qWarning().noquote() << lastError_;
        return false;
    }

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_5_6);
    out << kLocalDbMagic
        << kLocalDbVersion
        << localLastPersonId_
        << localLastFeatureId_
        << localLastLogId_;

    out << static_cast<quint32>(localPersons_.size());
    for (const PersonInfo &person : localPersons_) {
        out << person.id
            << person.personNo
            << person.name
            << person.enabled
            << person.createdAt
            << person.updatedAt;
    }

    out << static_cast<quint32>(localFeatures_.size());
    for (const LocalFeatureEntry &entry : localFeatures_) {
        out << entry.id
            << entry.personId
            << entry.imagePath
            << entry.feature.blob
            << entry.feature.modelVersion
            << entry.feature.quality
            << entry.createdAt;
    }

    out << static_cast<quint32>(localVerifyLogs_.size());
    for (const VerifyLog &log : localVerifyLogs_) {
        out << log.logId
            << log.personId
            << log.personNo
            << log.result
            << log.cosine
            << log.liveScore
            << log.snapshotPath
            << log.createdAt;
    }

    if (out.status() != QDataStream::Ok) {
        lastError_ = "写入本地数据库流失败";
        qWarning().noquote() << lastError_;
        return false;
    }

    if (!file.commit()) {
        lastError_ = "提交本地数据库文件失败：" + file.errorString();
        qWarning().noquote() << lastError_;
        return false;
    }

    return true;
}

/**
 * @brief 重置本地离线数据库的内存副本。
 */
void MysqlFaceRepository::clearLocalStore()
{
    localLastPersonId_ = 0;
    localLastFeatureId_ = 0;
    localLastLogId_ = 0;
    localPersons_.clear();
    localFeatures_.clear();
    localVerifyLogs_.clear();
}

/**
 * @brief 在本地文件数据库中插入或更新人员，并追加一条人脸特征。
 */
bool MysqlFaceRepository::addPersonFaceToLocal(const PersonInfo &person, const FaceFeatureData &feature, const QString &imagePath)
{
    const QString now = nowString();

    QString duplicateMessage;
    if (hasDuplicatePerson(person, &duplicateMessage) ||
        hasDuplicateFeature(feature, &duplicateMessage)) {
        lastError_ = duplicateMessage;
        qWarning().noquote() << lastError_;
        return false;
    }

    qint64 personId = 0;
    for (PersonInfo &existing : localPersons_) {
        if (existing.personNo.compare(person.personNo, Qt::CaseInsensitive) == 0 &&
            existing.name.compare(person.name, Qt::CaseInsensitive) == 0) {
            personId = existing.id;
            existing.enabled = person.enabled;
            existing.deleted = false;
            existing.updatedAt = now;
            break;
        }
    }
    if (personId <= 0) {
        PersonInfo savedPerson = person;
        savedPerson.id = nextLocalId("last_person_id");
        savedPerson.enabled = person.enabled;
        savedPerson.deleted = false;
        savedPerson.createdAt = now;
        savedPerson.updatedAt = now;
        personId = savedPerson.id;
        localPersons_.push_back(savedPerson);
    }

    int featureCount = 0;
    for (const LocalFeatureEntry &existingFeature : localFeatures_) {
        if (existingFeature.personId == personId) {
            ++featureCount;
        }
    }
    if (featureCount >= config_.maxFeaturesPerPerson) {
        lastError_ = QString("该人员人脸模板已达到上限：%1 张").arg(config_.maxFeaturesPerPerson);
        qWarning().noquote() << lastError_;
        return false;
    }

    const qint64 featureId = nextLocalId("last_feature_id");
    LocalFeatureEntry entry;
    entry.id = featureId;
    entry.personId = personId;
    entry.imagePath = imagePath;
    entry.feature = feature;
    entry.createdAt = now;
    localFeatures_.push_back(entry);

    if (!saveLocalStore()) {
        return false;
    }

    qInfo().noquote() << "人脸已录入本地离线数据库：" << person.personNo << person.name << imagePath;
    return true;
}

/** @brief 在旧本地模式批量导入人员。 */
bool MysqlFaceRepository::importPeopleBasicToLocal(const QVector<PersonInfo> &people, int *inserted, int *updated)
{
    if (inserted) { *inserted = 0; }
    if (updated) { *updated = 0; }
    const QString now = nowString();
    for (const PersonInfo &person : people) {
        const QString personNo = person.personNo.trimmed();
        const QString name = person.name.trimmed();
        if (personNo.isEmpty() || name.isEmpty()) {
            continue;
        }
        bool found = false;
        for (PersonInfo &existing : localPersons_) {
            if (existing.personNo.compare(personNo, Qt::CaseInsensitive) == 0) {
                existing.name = name;
                existing.enabled = person.enabled;
                existing.deleted = false;
                existing.updatedAt = now;
                found = true;
                if (updated) { ++(*updated); }
                break;
            }
        }
        if (!found) {
            PersonInfo saved = person;
            saved.id = nextLocalId("last_person_id");
            saved.personNo = personNo;
            saved.name = name;
            saved.deleted = false;
            saved.createdAt = now;
            saved.updatedAt = now;
            localPersons_.push_back(saved);
            if (inserted) { ++(*inserted); }
        }
    }
    return saveLocalStore();
}

/**
 * @brief 从本地文件数据库加载已启用的人脸底库。
 */
bool MysqlFaceRepository::loadAllEnabledFacesFromLocal(QVector<FaceRecord> &records)
{
    records.clear();

    QHash<qint64, PersonInfo> enabledPersons;
    for (const PersonInfo &person : localPersons_) {
        if (!person.enabled || person.deleted) {
            continue;
        }
        enabledPersons.insert(person.id, person);
    }

    for (const LocalFeatureEntry &entry : localFeatures_) {
        if (!enabledPersons.contains(entry.personId)) {
            continue;
        }

        FaceRecord record;
        record.featureId = entry.id;
        record.person = enabledPersons.value(entry.personId);
        record.imagePath = entry.imagePath;
        record.feature = entry.feature;
        record.enrolledAt = entry.createdAt;
        records.push_back(record);
    }

    qInfo().noquote() << "已加载启用的本地人脸底库：" << records.size();
    return true;
}


/**
 * @brief 从本地内存存储构建管理员人员表。
 */
bool MysqlFaceRepository::loadPeopleFromLocal(QVector<PersonAdminRecord> &records, bool includeDeleted)
{
    records.clear();
    for (const PersonInfo &person : localPersons_) {
        if (!includeDeleted && person.deleted) {
            continue;
        }
        PersonAdminRecord record;
        record.id = person.id;
        record.personNo = person.personNo;
        record.name = person.name;
        record.enabled = person.enabled;
        record.deleted = person.deleted;
        record.createdAt = person.createdAt;
        record.updatedAt = person.updatedAt;

        for (const LocalFeatureEntry &entry : localFeatures_) {
            if (entry.personId != person.id) {
                continue;
            }
            ++record.featureCount;
            if (record.firstFeatureAt.isEmpty() || entry.createdAt < record.firstFeatureAt) {
                record.firstFeatureAt = entry.createdAt;
            }
            if (record.lastFeatureAt.isEmpty() || entry.createdAt > record.lastFeatureAt) {
                record.lastFeatureAt = entry.createdAt;
            }
        }
        records.push_back(record);
    }
    return true;
}

/**
 * @brief 从本地内存存储构建成功识别记录表。
 */
bool MysqlFaceRepository::loadPassedVerifyLogsFromLocal(QVector<VerifyLogViewRecord> &records, int limit)
{
    records.clear();
    const int safeLimit = qBound(1, limit, 1000);

    QHash<qint64, PersonInfo> peopleById;
    peopleById.reserve(localPersons_.size());
    for (const PersonInfo &person : localPersons_) {
        peopleById.insert(person.id, person);
    }

    for (int i = localVerifyLogs_.size() - 1; i >= 0 && records.size() < safeLimit; --i) {
        const VerifyLog &log = localVerifyLogs_[i];
        if (log.result != QStringLiteral("passed")) {
            continue;
        }

        VerifyLogViewRecord record;
        record.logId = log.logId;
        record.personId = log.personId;
        record.personNo = log.personNo;
        record.result = log.result;
        record.cosine = log.cosine;
        record.liveScore = log.liveScore;
        record.snapshotPath = log.snapshotPath;
        record.nameSnapshot = log.nameSnapshot;
        record.failReason = log.failReason;
        record.deviceSn = log.deviceSn;
        record.eventType = log.eventType;
        record.direction = log.direction;
        record.createdAt = log.createdAt.toString("yyyy-MM-dd HH:mm:ss");

        const auto personIt = peopleById.constFind(log.personId);
        if (personIt != peopleById.constEnd()) {
            record.personNo = personIt->personNo;
            record.name = personIt->name;
        }
        records.push_back(record);
    }
    return true;
}

/**
 * @brief 从本地内存存储生成本机信息统计。
 */
bool MysqlFaceRepository::loadStorageStatsFromLocal(StorageStats &stats)
{
    stats.personCount = localPersons_.size();
    const StoragePolicy::Usage registration = StoragePolicy::registrationUsage();
    stats.faceFeatureCount = registration.fileCount;
    stats.registrationPhotoBytes = registration.bytes;
    stats.verifyLogCount = localVerifyLogs_.size();
    return true;
}

/**
 * @brief 更新本地人员编号，并保持姓名不变。
 */
bool MysqlFaceRepository::updatePersonNoInLocal(qint64 personId, const QString &newPersonNo)
{
    const QString trimmedNo = newPersonNo.trimmed();
    if (personId <= 0 || trimmedNo.isEmpty()) {
        lastError_ = "人员 ID 和新人员编号不能为空";
        qWarning().noquote() << lastError_;
        return false;
    }

    for (const PersonInfo &person : localPersons_) {
        if (person.id != personId && person.personNo.compare(trimmedNo, Qt::CaseInsensitive) == 0) {
            lastError_ = "人员编号已存在";
            qWarning().noquote() << lastError_;
            return false;
        }
    }

    for (PersonInfo &person : localPersons_) {
        if (person.id == personId) {
            person.personNo = trimmedNo;
            person.updatedAt = nowString();
            return saveLocalStore();
        }
    }

    lastError_ = "未找到人员";
    qWarning().noquote() << lastError_;
    return false;
}

/**
 * @brief 更新本地人员启用权限。
 */
bool MysqlFaceRepository::updatePersonEnabledInLocal(qint64 personId, bool enabled)
{
    if (personId <= 0) {
        lastError_ = "需要有效的人员 ID";
        qWarning().noquote() << lastError_;
        return false;
    }

    for (PersonInfo &person : localPersons_) {
        if (person.id == personId) {
            person.enabled = enabled;
            person.updatedAt = nowString();
            return saveLocalStore();
        }
    }

    lastError_ = "未找到人员";
    qWarning().noquote() << lastError_;
    return false;
}

/**
 * @brief 删除本地人员及其本地人脸特征。
 */
bool MysqlFaceRepository::deletePersonFromLocal(qint64 personId)
{
    if (personId <= 0) {
        lastError_ = "需要有效的人员 ID";
        qWarning().noquote() << lastError_;
        return false;
    }

    for (PersonInfo &person : localPersons_) {
        if (person.id == personId) {
            person.deleted = true;
            person.enabled = false;
            person.updatedAt = nowString();
            return saveLocalStore();
        }
    }

    lastError_ = "未找到人员";
    qWarning().noquote() << lastError_;
    return false;
}

/**
 * @brief 向本地文件数据库追加一条验证记录。
 */
bool MysqlFaceRepository::addVerifyLogToLocal(const VerifyLog &log)
{
    VerifyLog saved = log;
    saved.logId = nextLocalId("last_log_id");
    localVerifyLogs_.push_back(saved);

    if (!saveLocalStore()) {
        return false;
    }

    return true;
}

/** @brief 从旧本地模式按筛选器加载验证日志。 */
bool MysqlFaceRepository::loadVerifyLogsFromLocal(const VerifyLogFilter &filter, QVector<VerifyLogViewRecord> &records)
{
    records.clear();
    const int safeLimit = qBound(1, filter.limit, 5000);
    QHash<qint64, PersonInfo> peopleById;
    for (const PersonInfo &person : localPersons_) {
        peopleById.insert(person.id, person);
    }
    for (int i = localVerifyLogs_.size() - 1; i >= 0 && records.size() < safeLimit; --i) {
        const VerifyLog &log = localVerifyLogs_[i];
        if (!filter.result.isEmpty() && filter.result != QStringLiteral("all") && log.result != filter.result) {
            continue;
        }
        if (filter.startTime.isValid() && log.createdAt < filter.startTime) {
            continue;
        }
        if (filter.endTime.isValid() && log.createdAt > filter.endTime) {
            continue;
        }
        VerifyLogViewRecord record;
        record.logId = log.logId;
        record.personId = log.personId;
        record.personNo = log.personNo;
        record.result = log.result;
        record.cosine = log.cosine;
        record.liveScore = log.liveScore;
        record.snapshotPath = log.snapshotPath;
        record.nameSnapshot = log.nameSnapshot;
        record.failReason = log.failReason;
        record.deviceSn = log.deviceSn;
        record.eventType = log.eventType;
        record.direction = log.direction;
        record.createdAt = log.createdAt.toString("yyyy-MM-dd HH:mm:ss");
        const auto personIt = peopleById.constFind(log.personId);
        if (personIt != peopleById.constEnd()) {
            record.name = personIt->name;
        }
        if (!filter.personNo.isEmpty() && !record.personNo.contains(filter.personNo, Qt::CaseInsensitive)) {
            continue;
        }
        if (!filter.name.isEmpty() && !record.name.contains(filter.name, Qt::CaseInsensitive) &&
            !record.nameSnapshot.contains(filter.name, Qt::CaseInsensitive)) {
            continue;
        }
        records.push_back(record);
    }
    return true;
}

/** @brief 从旧本地模式加载同步任务。 */
bool MysqlFaceRepository::loadSyncTasksFromLocal(QVector<SyncTaskRecord> &records, int limit)
{
    records.clear();
    const int safeLimit = qBound(1, limit, 1000);
    for (int i = localSyncTasks_.size() - 1; i >= 0 && records.size() < safeLimit; --i) {
        records.push_back(localSyncTasks_[i]);
    }
    return true;
}

/** @brief 从旧本地模式加载系统事件。 */
bool MysqlFaceRepository::loadSystemEventsFromLocal(QVector<SystemEventLog> &records, int limit)
{
    records.clear();
    const int safeLimit = qBound(1, limit, 1000);
    for (int i = localSystemEvents_.size() - 1; i >= 0 && records.size() < safeLimit; --i) {
        if (localSystemEvents_[i].level != QStringLiteral("error")) {
            continue;
        }
        records.push_back(localSystemEvents_[i]);
    }
    return true;
}

/** @brief 在旧本地模式追加审计日志。 */
bool MysqlFaceRepository::addOperatorAuditLogToLocal(const OperatorAuditLog &log)
{
    OperatorAuditLog saved = log;
    saved.id = localAuditLogs_.size() + 1;
    saved.createdAt = saved.createdAt.isEmpty() ? nowString() : saved.createdAt;
    localAuditLogs_.push_back(saved);
    return true;
}

/** @brief 在旧本地模式追加系统事件。 */
bool MysqlFaceRepository::addSystemEventLogToLocal(const SystemEventLog &log)
{
    SystemEventLog saved = log;
    saved.id = localSystemEvents_.size() + 1;
    saved.createdAt = saved.createdAt.isEmpty() ? nowString() : saved.createdAt;
    localSystemEvents_.push_back(saved);
    return true;
}

/** @brief 在旧本地模式删除过期验证日志。 */
bool MysqlFaceRepository::cleanupOldVerifyLogsFromLocal(int days, int *removed)
{
    if (removed) { *removed = 0; }
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-qMax(1, days));
    for (int i = localVerifyLogs_.size() - 1; i >= 0; --i) {
        if (localVerifyLogs_[i].createdAt < cutoff) {
            localVerifyLogs_.removeAt(i);
            if (removed) { ++(*removed); }
        }
    }
    return saveLocalStore();
}

/**
 * @brief 在本地文件数据库中分配下一个单调递增 ID。
 */
qint64 MysqlFaceRepository::nextLocalId(const QString &key)
{
    if (key == "last_person_id") {
        return ++localLastPersonId_;
    }
    if (key == "last_feature_id") {
        return ++localLastFeatureId_;
    }
    if (key == "last_log_id") {
        return ++localLastLogId_;
    }
    return 0;
}

/**
 * @brief 当人员编号或姓名已存在时拒绝录入。
 */
bool MysqlFaceRepository::hasDuplicatePerson(const PersonInfo &person, QString *message)
{
    const QString personNo = person.personNo.trimmed();
    const QString name = person.name.trimmed();

    if (localFileMode_) {
        for (const PersonInfo &existing : localPersons_) {
            const bool sameNo = existing.personNo.compare(personNo, Qt::CaseInsensitive) == 0;
            const bool sameName = existing.name.compare(name, Qt::CaseInsensitive) == 0;
            if ((sameNo && !sameName) || (sameName && !sameNo)) {
                if (message) {
                    *message = QString("人员已录入，已存在人员：%1 %2")
                        .arg(existing.personNo)
                        .arg(existing.name);
                }
                return true;
            }
        }
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("SELECT person_no, name FROM person "
              "WHERE (person_no=? AND name<>?) OR (name=? AND person_no<>?) LIMIT 1");
    q.addBindValue(personNo);
    q.addBindValue(name);
    q.addBindValue(name);
    q.addBindValue(personNo);
    if (!q.exec()) {
        if (message) {
            *message = "检查重复人员失败：" + q.lastError().text();
        }
        return true;
    }

    if (q.next()) {
        if (message) {
            *message = QString("人员已录入，已存在人员：%1 %2")
                .arg(q.value(0).toString())
                .arg(q.value(1).toString());
        }
        return true;
    }

    return false;
}

/**
 * @brief 当特征与已有人员相同或非常接近时拒绝录入。
 */
bool MysqlFaceRepository::hasDuplicateFeature(const FaceFeatureData &feature, QString *message)
{
    if (feature.blob.isEmpty()) {
        return false;
    }

    if (localFileMode_) {
        QHash<qint64, PersonInfo> persons;
        for (const PersonInfo &person : localPersons_) {
            persons.insert(person.id, person);
        }

        for (const LocalFeatureEntry &entry : localFeatures_) {
            if (entry.feature.blob.isEmpty()) {
                continue;
            }

            const bool exactSame = entry.feature.blob == feature.blob;
            const float cosine = exactSame ? 1.0f : featureCosine(entry.feature.blob, feature.blob);
            if (exactSame || cosine >= config_.duplicateFaceCosineThreshold) {
                const PersonInfo existing = persons.value(entry.personId);
                if (message) {
                    *message = QString("人员已录入，相似人员：%1 %2，相似度=%3")
                        .arg(existing.personNo)
                        .arg(existing.name)
                        .arg(cosine, 0, 'f', 3);
                }
                return true;
            }
        }
        return false;
    }

    QSqlQuery q(db_);
    q.prepare("SELECT p.person_no, p.name, f.feature_blob "
              "FROM person p JOIN face_feature f ON f.person_id=p.id");
    if (!q.exec()) {
        if (message) {
            *message = "检查重复人脸失败：" + q.lastError().text();
        }
        return true;
    }

    while (q.next()) {
        const QByteArray existingBlob = q.value(2).toByteArray();
        if (existingBlob.isEmpty()) {
            continue;
        }

        const bool exactSame = existingBlob == feature.blob;
        const float cosine = exactSame ? 1.0f : featureCosine(existingBlob, feature.blob);
        if (exactSame || cosine >= config_.duplicateFaceCosineThreshold) {
            if (message) {
                *message = QString("人员已录入，相似人员：%1 %2，相似度=%3")
                    .arg(q.value(0).toString())
                    .arg(q.value(1).toString())
                    .arg(cosine, 0, 'f', 3);
            }
            return true;
        }
    }

    return false;
}

/**
 * @brief 直接基于已存储的 float 特征向量计算余弦相似度。
 */
float MysqlFaceRepository::featureCosine(const QByteArray &left, const QByteArray &right) const
{
    const int count = qMin(left.size(), right.size()) / static_cast<int>(sizeof(float));
    if (count <= 0 || left.size() != right.size()) {
        return -1.0f;
    }

    const float *l = reinterpret_cast<const float *>(left.constData());
    const float *r = reinterpret_cast<const float *>(right.constData());
    double dot = 0.0;
    double leftNorm = 0.0;
    double rightNorm = 0.0;
    for (int i = 0; i < count; ++i) {
        dot += static_cast<double>(l[i]) * static_cast<double>(r[i]);
        leftNorm += static_cast<double>(l[i]) * static_cast<double>(l[i]);
        rightNorm += static_cast<double>(r[i]) * static_cast<double>(r[i]);
    }

    if (leftNorm <= 0.0 || rightNorm <= 0.0) {
        return -1.0f;
    }
    return static_cast<float>(dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm)));
}

/**
 * @brief 在按名称打开 MySQL 数据库前，先创建配置的数据库。
 */
bool MysqlFaceRepository::ensureDatabase()
{
    db_.setDatabaseName(QString());
    if (!db_.open()) {
        lastError_ = QString("打开 MySQL 服务器失败：%1，可用 SQL 驱动：%2")
            .arg(db_.lastError().text())
            .arg(QSqlDatabase::drivers().join(", "));
        qWarning().noquote() << lastError_;
        return false;
    }

    QSqlQuery q(db_);
    const QString sql = QString("CREATE DATABASE IF NOT EXISTS %1 CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci")
        .arg(quoteIdentifier(config_.mysqlDatabase));
    if (!q.exec(sql)) {
        lastError_ = "创建 MySQL 数据库失败：" + q.lastError().text();
        db_.close();
        qWarning().noquote() << lastError_;
        return false;
    }

    return true;
}

/** @return 指定表包含字段时返回 true。 */
bool MysqlFaceRepository::tableHasColumn(const QString &tableName, const QString &columnName) const
{
    const QSqlRecord record = db_.record(tableName);
    return record.indexOf(columnName) >= 0;
}

/** @brief 执行一条结构迁移 SQL 并记录错误。 */
bool MysqlFaceRepository::execSchemaSql(const QString &sql)
{
    QSqlQuery q(db_);
    if (!q.exec(sql)) {
        lastError_ = "执行数据库迁移失败：" + q.lastError().text() + " SQL=" + sql;
        qWarning().noquote() << lastError_;
        return false;
    }
    return true;
}

/** @brief 按当前 SQL 方言补充缺失字段。 */
bool MysqlFaceRepository::ensureColumn(const QString &tableName,
                                       const QString &columnName,
                                       const QString &sqliteAlter,
                                       const QString &mysqlAlter)
{
    if (tableHasColumn(tableName, columnName)) {
        return true;
    }
    // 字段不存在时才 ALTER，保证旧库升级不丢 person/face_feature/verify_log 数据。
    return execSchemaSql(isSqlite() ? sqliteAlter : mysqlAlter);
}

/** @brief 将待同步业务写入同步任务表或本地向量。 */
bool MysqlFaceRepository::enqueueSyncTask(const QString &taskType, const QString &payloadJson)
{
    if (localFileMode_) {
        SyncTaskRecord record;
        record.id = localSyncTasks_.size() + 1;
        record.taskType = taskType;
        record.payloadJson = payloadJson;
        record.syncStatus = QStringLiteral("pending");
        record.retryCount = 0;
        record.createdAt = nowString();
        record.updatedAt = record.createdAt;
        localSyncTasks_.push_back(record);
        return true;
    }
    const QString now = nowString();
    QSqlQuery q(db_);
    if (tableHasColumn("sync_task", "payload_text")) {
        q.prepare("INSERT INTO sync_task(task_type, payload_text, payload_json, sync_status, retry_count, last_error, created_at, updated_at) "
                  "VALUES(?, ?, ?, 'pending', 0, '', ?, ?)");
        q.addBindValue(taskType);
        q.addBindValue(payloadJson);
        q.addBindValue(payloadJson);
        q.addBindValue(now);
        q.addBindValue(now);
    } else {
        q.prepare("INSERT INTO sync_task(task_type, payload_json, sync_status, retry_count, last_error, created_at, updated_at) "
                  "VALUES(?, ?, 'pending', 0, '', ?, ?)");
        q.addBindValue(taskType);
        q.addBindValue(payloadJson);
        q.addBindValue(now);
        q.addBindValue(now);
    }
    if (!q.exec()) {
        qWarning().noquote() << "写入同步队列失败：" << q.lastError().text();
        return false;
    }
    return true;
}

/**
 * @brief 创建闸机系统需要的 SQL 持久化表。
 */
bool MysqlFaceRepository::ensureSchema()
{
    // 按当前数据库类型生成建表 SQL，字段语义保持一致。
    const QStringList sql = isSqlite()
        ? QStringList{
            "PRAGMA journal_mode = WAL",
            "PRAGMA synchronous = NORMAL",
            "PRAGMA foreign_keys = ON",
            "PRAGMA busy_timeout = 5000",

            "CREATE TABLE IF NOT EXISTS person ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "person_no TEXT NOT NULL UNIQUE,"
            "name TEXT NOT NULL,"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "deleted INTEGER NOT NULL DEFAULT 0,"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS face_feature ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "person_id INTEGER NOT NULL,"
            "image_path TEXT NOT NULL,"
            "feature_blob BLOB NOT NULL,"
            "feature_size INTEGER NOT NULL,"
            "model_version TEXT,"
            "quality REAL DEFAULT 0,"
            "created_at TEXT NOT NULL,"
            "FOREIGN KEY (person_id) REFERENCES person(id))",

            "CREATE TABLE IF NOT EXISTS verify_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "person_id INTEGER,"
            "person_no TEXT,"
            "result TEXT NOT NULL,"
            "cosine REAL DEFAULT 0,"
            "live_score REAL DEFAULT 0,"
            "snapshot_path TEXT,"
            "name_snapshot TEXT,"
            "fail_reason TEXT,"
            "device_sn TEXT,"
            "event_type TEXT NOT NULL DEFAULT 'face_verify',"
            "direction TEXT NOT NULL DEFAULT 'in',"
            "created_at TEXT NOT NULL)",

            "CREATE INDEX IF NOT EXISTS idx_verify_log_result_created "
            "ON verify_log(result, created_at)",

            "CREATE INDEX IF NOT EXISTS idx_verify_log_person_id "
            "ON verify_log(person_id)",

            "CREATE TABLE IF NOT EXISTS sync_task ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "task_type TEXT NOT NULL,"
            "payload_json TEXT NOT NULL,"
            "sync_status TEXT NOT NULL DEFAULT 'pending',"
            "retry_count INTEGER NOT NULL DEFAULT 0,"
            "last_error TEXT,"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS operator_audit_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "operator_name TEXT,"
            "action TEXT NOT NULL,"
            "target_type TEXT,"
            "target_id TEXT,"
            "result TEXT,"
            "detail TEXT,"
            "created_at TEXT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS system_event_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "event_type TEXT NOT NULL,"
            "level TEXT NOT NULL DEFAULT 'info',"
            "message TEXT NOT NULL,"
            "detail TEXT,"
            "created_at TEXT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS attendance_daily ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "person_id INTEGER,"
            "person_no TEXT,"
            "name_snapshot TEXT,"
            "work_date TEXT NOT NULL,"
            "first_pass_at TEXT,"
            "last_pass_at TEXT,"
            "pass_count INTEGER NOT NULL DEFAULT 0,"
            "updated_at TEXT NOT NULL)"
        }
        : QStringList{
            "CREATE TABLE IF NOT EXISTS person ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "person_no VARCHAR(64) NOT NULL UNIQUE,"
            "name VARCHAR(128) NOT NULL,"
            "enabled TINYINT NOT NULL DEFAULT 1,"
            "deleted TINYINT NOT NULL DEFAULT 0,"
            "created_at DATETIME NOT NULL,"
            "updated_at DATETIME NOT NULL)",

            "CREATE TABLE IF NOT EXISTS face_feature ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "person_id BIGINT NOT NULL,"
            "image_path VARCHAR(512) NOT NULL,"
            "feature_blob LONGBLOB NOT NULL,"
            "feature_size INT NOT NULL,"
            "model_version VARCHAR(128),"
            "quality FLOAT DEFAULT 0,"
            "created_at DATETIME NOT NULL,"
            "FOREIGN KEY (person_id) REFERENCES person(id))",

            "CREATE TABLE IF NOT EXISTS verify_log ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "person_id BIGINT,"
            "person_no VARCHAR(64),"
            "result VARCHAR(32) NOT NULL,"
            "cosine FLOAT DEFAULT 0,"
            "live_score FLOAT DEFAULT 0,"
            "snapshot_path VARCHAR(512),"
            "name_snapshot VARCHAR(128),"
            "fail_reason VARCHAR(255),"
            "device_sn VARCHAR(128),"
            "event_type VARCHAR(32) NOT NULL DEFAULT 'face_verify',"
            "direction VARCHAR(16) NOT NULL DEFAULT 'in',"
            "created_at DATETIME NOT NULL)",

            "CREATE TABLE IF NOT EXISTS sync_task ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "task_type VARCHAR(64) NOT NULL,"
            "payload_json TEXT NOT NULL,"
            "sync_status VARCHAR(32) NOT NULL DEFAULT 'pending',"
            "retry_count INT NOT NULL DEFAULT 0,"
            "last_error TEXT,"
            "created_at DATETIME NOT NULL,"
            "updated_at DATETIME NOT NULL)",

            "CREATE TABLE IF NOT EXISTS operator_audit_log ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "operator_name VARCHAR(64),"
            "action VARCHAR(64) NOT NULL,"
            "target_type VARCHAR(64),"
            "target_id VARCHAR(128),"
            "result VARCHAR(32),"
            "detail TEXT,"
            "created_at DATETIME NOT NULL)",

            "CREATE TABLE IF NOT EXISTS system_event_log ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "event_type VARCHAR(64) NOT NULL,"
            "level VARCHAR(16) NOT NULL DEFAULT 'info',"
            "message VARCHAR(255) NOT NULL,"
            "detail TEXT,"
            "created_at DATETIME NOT NULL)",

            "CREATE TABLE IF NOT EXISTS attendance_daily ("
            "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
            "person_id BIGINT,"
            "person_no VARCHAR(64),"
            "name_snapshot VARCHAR(128),"
            "work_date DATE NOT NULL,"
            "first_pass_at DATETIME,"
            "last_pass_at DATETIME,"
            "pass_count INT NOT NULL DEFAULT 0,"
            "updated_at DATETIME NOT NULL)"
        };

    for (const QString &s : sql) {
        QSqlQuery q(db_);
        if (!q.exec(s)) {
            lastError_ = "创建表结构失败：" + q.lastError().text();
            qWarning().noquote() << lastError_;
            return false;
        }
    }

    if (!ensureColumn("person", "deleted",
                      "ALTER TABLE person ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE person ADD COLUMN deleted TINYINT NOT NULL DEFAULT 0")) {
        return false;
    }
    if (!ensureColumn("verify_log", "name_snapshot",
                      "ALTER TABLE verify_log ADD COLUMN name_snapshot TEXT",
                      "ALTER TABLE verify_log ADD COLUMN name_snapshot VARCHAR(128)")) {
        return false;
    }
    if (!ensureColumn("verify_log", "fail_reason",
                      "ALTER TABLE verify_log ADD COLUMN fail_reason TEXT",
                      "ALTER TABLE verify_log ADD COLUMN fail_reason VARCHAR(255)")) {
        return false;
    }
    if (!ensureColumn("verify_log", "device_sn",
                      "ALTER TABLE verify_log ADD COLUMN device_sn TEXT",
                      "ALTER TABLE verify_log ADD COLUMN device_sn VARCHAR(128)")) {
        return false;
    }
    if (!ensureColumn("verify_log", "event_type",
                      "ALTER TABLE verify_log ADD COLUMN event_type TEXT NOT NULL DEFAULT 'face_verify'",
                      "ALTER TABLE verify_log ADD COLUMN event_type VARCHAR(32) NOT NULL DEFAULT 'face_verify'")) {
        return false;
    }
    if (!ensureColumn("verify_log", "direction",
                      "ALTER TABLE verify_log ADD COLUMN direction TEXT NOT NULL DEFAULT 'in'",
                      "ALTER TABLE verify_log ADD COLUMN direction VARCHAR(16) NOT NULL DEFAULT 'in'")) {
        return false;
    }
    if (!ensureColumn("sync_task", "payload_json",
                      "ALTER TABLE sync_task ADD COLUMN payload_json TEXT",
                      "ALTER TABLE sync_task ADD COLUMN payload_json TEXT")) {
        return false;
    }
    if (!ensureColumn("sync_task", "last_error",
                      "ALTER TABLE sync_task ADD COLUMN last_error TEXT",
                      "ALTER TABLE sync_task ADD COLUMN last_error TEXT")) {
        return false;
    }
    if (tableHasColumn("sync_task", "payload_text")) {
        execSchemaSql("UPDATE sync_task SET payload_json=payload_text WHERE (payload_json IS NULL OR payload_json='')");
    }

    qInfo().noquote() << "数据库表结构已就绪，驱动：" << config_.databaseDriver;
    return true;
}

/**
 * @brief 当配置驱动为离线 SQLite 后端时返回 true。
 */
bool MysqlFaceRepository::isSqlite() const
{
    return sqliteMode_ && !localFileMode_;
}

/**
 * @brief 对 MySQL 标识符加引用，同时保证创建数据库 SQL 合法。
 */
QString MysqlFaceRepository::quoteIdentifier(const QString &identifier) const
{
    QString out = identifier;
    out.replace('`', "``");
    return "`" + out + "`";
}

/**
 * @brief 基于可执行文件目录解析配置的 SQLite 文件路径。
 */
QString MysqlFaceRepository::sqliteDatabasePath() const
{
    const QString configuredPath = config_.sqlitePath.trimmed().isEmpty()
        ? QStringLiteral("/home/cat/face_media/access_control.db")
        : config_.sqlitePath.trimmed();
    const QFileInfo info(configuredPath);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(configuredPath);
}

/**
 * @brief 基于可执行文件目录解析配置的本地文件数据库路径。
 */
QString MysqlFaceRepository::localStoreFilePath() const
{
    const QString configuredPath = config_.localStorePath.trimmed().isEmpty()
        ? QStringLiteral("/home/cat/face_media/access_control.db")
        : config_.localStorePath.trimmed();
    const QFileInfo info(configuredPath);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(configuredPath);
}

/**
 * @brief 生成 SQLite 和 MySQL 都接受的可移植时间戳字符串。
 */
QString MysqlFaceRepository::nowString() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}
