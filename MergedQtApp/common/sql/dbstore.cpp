/**
 * @file dbstore.cpp
 * @brief 提供配置和刷卡日志的 SQLite 存储访问，并按线程隔离数据库连接。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "dbstore.h"

#include <QMutex>
#include <QMutexLocker>
#include <QThread>

/**
 * @brief DbStore
 * SQLite 存储封装：负责
 * 1) 建立/复用命名连接
 * 2) 打开数据库（必要时）
 * 3) 设置 SQLite PRAGMA
 * 4) 建表/建索引
 * 5) 提供通用 exec/query 接口
 * 6) 提供 config / swipe_log 的业务接口
 *
 * 说明：
 * - 采用静态连接名 + QSqlDatabase::database(name) 获取连接引用。
 * - 错误信息通过 m_lastError 统一输出，便于上层显示/日志记录。
 */

namespace {

QMutex g_dbPathMutex;
QString g_dbPath;

/**
 * @brief 记录后续线程本地数据库连接使用的 SQLite 文件路径。
 *
 * @param dbPath SQLite 数据库文件路径。
 *
 * @note QSqlDatabase 连接具有线程亲和性，这里只共享数据库路径，不共享连接对象。
 */
void rememberDatabasePath(const QString& dbPath)
{
    QMutexLocker locker(&g_dbPathMutex);
    g_dbPath = dbPath;
}

/**
 * @brief 获取最近一次初始化时记录的 SQLite 文件路径。
 *
 * @return SQLite 数据库文件路径；尚未初始化时返回空字符串。
 *
 * @note 返回值是字符串副本，调用方不会持有内部互斥锁。
 */
QString rememberedDatabasePath()
{
    QMutexLocker locker(&g_dbPathMutex);
    return g_dbPath;
}

} // namespace

// -------------------- Error Handling --------------------

/** 当前线程最近一次错误信息；thread_local 防止不同线程相互覆盖。 */
thread_local QString DbStore::m_lastError;

/** 记录当前线程最近一次数据库错误。 */
static void setErr(const QString& s) { DbStore::m_lastError = s; }

/** 返回当前线程最近一次数据库错误。 */
QString DbStore::lastError() { return m_lastError; }

// -------------------- Connection Helpers --------------------

/**
 * @brief connName
 * 使用当前线程 ID 生成稳定连接名，既能在同一线程复用连接，又避免跨线程共享连接。
 */
QString DbStore::connName()
{
    const quintptr threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    return QStringLiteral("app_sqlite_conn_%1").arg(threadId, 0, 16);
}

/**
 * @brief dbRef
 * 获取当前连接名对应的数据库对象（QSqlDatabase 是轻量句柄拷贝）
 *
 * 注意：
 * - QSqlDatabase::database(name) 会返回一个与 name 关联的连接句柄。
 * - 需要确保 bootstrap() 已经创建了该连接，否则 contains() 检测会失败。
 */
static QSqlDatabase dbRef()
{
    return QSqlDatabase::database(DbStore::connName(), false);
}

// -------------------- Bootstrapping --------------------

/**
 * @brief ensureDriver
 * 检查 Qt SQL 驱动是否包含 QSQLITE。
 */
bool DbStore::ensureDriver()
{
    const auto drivers = QSqlDatabase::drivers();
    if (!drivers.contains("QSQLITE")) {
        setErr(QString("QSQLITE driver not available. Drivers=%1").arg(drivers.join(",")));
        return false;
    }
    return true;
}

/**
 * @brief openIfNeeded
 * 若连接存在且未打开则尝试打开；确保后续操作可用。
 *
 * 失败场景：
 * - 尚未 bootstrap()，连接不存在
 * - 数据库打开失败（路径无权限/损坏/被锁等）
 */
bool DbStore::openIfNeeded()
{
    const QString name = connName();
    if (!QSqlDatabase::contains(name)) {
        const QString dbPath = rememberedDatabasePath();
        if (dbPath.isEmpty()) {
            setErr("DB not bootstrapped: database path not set.");
            return false;
        }

        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(dbPath);
    }

    auto db = dbRef();
    if (db.isOpen()) return true;

    if (!db.open()) {
        setErr("DB open failed: " + db.lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief applyPragmas
 * 设置 SQLite PRAGMA，主要用于：
 * - WAL 模式：提高并发/写入性能，适合设备端持续写日志
 * - synchronous=NORMAL：性能/安全折中（更保守可用 FULL）
 * - foreign_keys=ON：启用外键约束
 * - temp_store=MEMORY：临时表使用内存，减少磁盘 IO
 */
bool DbStore::applyPragmas()
{
    if (!openIfNeeded()) return false;
    QSqlQuery q(dbRef());

    const QStringList pragmas = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
        "PRAGMA foreign_keys=ON;",
        "PRAGMA busy_timeout=5000;",
        "PRAGMA temp_store=MEMORY;"
    };

    for (const auto& sql : pragmas) {
        if (!q.exec(sql)) {
            setErr("PRAGMA failed: " + q.lastError().text());
            return false;
        }
    }
    return true;
}

/**
 * @brief createTables
 * 创建业务表：
 * - config：简单 KV 配置（用 TEXT 存 value，便于 QVariant->QString）
 * - swipe_log：刷卡日志 + 上报状态 uploaded
 */
bool DbStore::createTables()
{
    if (!openIfNeeded()) return false;
    QSqlQuery q(dbRef());

    // config：key/value 配置存储
    const char* createConfig = R"(
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
    )";

    // swipe_log：门禁刷卡日志（uploaded=0 未上报，=1 已上报）
    const char* createSwipeLog = R"(
        CREATE TABLE IF NOT EXISTS swipe_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts TEXT NOT NULL,
            card_id TEXT,
            card_type TEXT,
            result INTEGER NOT NULL,
            reason TEXT,
            raw BLOB,
            uploaded INTEGER NOT NULL DEFAULT 0
        )
    )";

    if (!q.exec(createConfig)) { setErr(q.lastError().text()); return false; }
    if (!q.exec(createSwipeLog)) { setErr(q.lastError().text()); return false; }

    return true;
}

/**
 * @brief prepareIndices
 * 创建索引，优化常见查询：
 * - 按时间查询/排序
 * - 按上传状态挑选未上传记录（uploaded,id 组合便于分页/顺序处理）
 */
bool DbStore::prepareIndices()
{
    if (!openIfNeeded()) return false;
    QSqlQuery q(dbRef());

    const QStringList idx = {
        "CREATE INDEX IF NOT EXISTS idx_swipe_log_ts ON swipe_log(ts);",
        "CREATE INDEX IF NOT EXISTS idx_swipe_log_uploaded ON swipe_log(uploaded, id);"
    };

    for (const auto& sql : idx) {
        if (!q.exec(sql)) {
            setErr("Create index failed: " + q.lastError().text());
            return false;
        }
    }
    return true;
}

/**
 * @brief bootstrap
 * 数据库初始化入口：
 * 1) 检查 SQLite 驱动
 * 2) 创建或复用命名连接
 * 3) 打开 DB
 * 4) 执行 PRAGMA / 建表 / 建索引
 *
 * @param dbPath SQLite 文件路径
 */
bool DbStore::bootstrap(const QString& dbPath)
{
    m_lastError.clear();
    rememberDatabasePath(dbPath);

    if (!ensureDriver()) return false;

    // 建立/复用命名连接（避免重复 addDatabase）
    QSqlDatabase db;
    const QString name = connName();
    if (QSqlDatabase::contains(name)) {
        db = QSqlDatabase::database(name, false);
    } else {
        db = QSqlDatabase::addDatabase("QSQLITE", name);
    }

    if (db.isOpen() && db.databaseName() != dbPath) {
        db.close();
    }

    db.setDatabaseName(dbPath);

    if (!db.open()) {
        setErr("DB open failed: " + db.lastError().text());
        return false;
    }

    if (!applyPragmas())   return false;
    if (!createTables())   return false;
    if (!prepareIndices()) return false;

    return true;
}

/**
 * @brief close
 * 关闭并移除数据库连接。
 *
 * 注意：
 * - Qt 文档建议 removeDatabase 前确保所有 QSqlQuery/QSqlDatabase 句柄都已析构/离开作用域。
 * - 这里 dbRef() 返回的是句柄拷贝，close() 后 removeDatabase() 仍可能被其他残留句柄影响，
 *   使用时要避免在外部长期持有同连接的 QSqlQuery。
 */
void DbStore::close()
{
    const QString name = connName();
    if (!QSqlDatabase::contains(name)) return;

    {
        QSqlDatabase db = dbRef();  // 限定在块内
        if (db.isOpen()) db.close();
    } // 这里 db 析构

    QSqlDatabase::removeDatabase(name);
}

// -------------------- Generic SQL APIs --------------------

/**
 * @brief exec
 * 执行不需要返回结果集的 SQL（DDL/DML）。
 */
bool DbStore::exec(const QString& sql)
{
    if (!openIfNeeded()) return false;

    QSqlQuery q(dbRef());
    if (!q.exec(sql)) {
        setErr("Exec failed: " + q.lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief 使用位置参数绑定执行无需返回结果集的 SQL。
 */
bool DbStore::execute(const QString& sql, const QList<QVariant>& binds)
{
    if (!openIfNeeded()) return false;

    QSqlQuery q(dbRef());
    if (!q.prepare(sql)) {
        setErr("Prepare failed: " + q.lastError().text());
        return false;
    }
    for (const auto& b : binds) q.addBindValue(b);

    if (!q.exec()) {
        setErr("Execute failed: " + q.lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief query
 * 执行带绑定参数的查询，并将结果集转为 QVariantMap 列表：
 * - key: 字段名
 * - value: 字段值 QVariant
 *
 * @param sql   需要 prepare 的 SQL（使用 ? 占位符）
 * @param binds 绑定参数列表，顺序对应 SQL 中的 ? 顺序
 */
QList<QVariantMap> DbStore::query(const QString& sql, const QList<QVariant>& binds)
{
    QList<QVariantMap> rows;
    if (!openIfNeeded()) return rows;

    QSqlQuery q(dbRef());
    q.prepare(sql);
    for (const auto& b : binds) q.addBindValue(b);

    if (!q.exec()) {
        setErr("Query failed: " + q.lastError().text());
        return rows;
    }

    while (q.next()) {
        QVariantMap m;
        QSqlRecord rec = q.record();
        for (int i = 0; i < rec.count(); ++i) {
            m.insert(rec.fieldName(i), q.value(i));
        }
        rows.push_back(m);
    }
    return rows;
}

// -------------------- Config APIs --------------------

/**
 * @brief setConfig
 * 设置配置项：key 不存在则插入，存在则更新（SQLite UPSERT）。
 */
bool DbStore::setConfig(const QString& key, const QVariant& value)
{
    if (!openIfNeeded()) return false;

    QSqlQuery q(dbRef());
    q.prepare("INSERT INTO config(key,value) VALUES(?,?) "
              "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.addBindValue(key);
    q.addBindValue(value.toString());

    if (!q.exec()) {
        setErr("setConfig failed: " + q.lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief getConfig
 * 获取配置项：若不存在/查询失败则返回 defaultValue。
 */
QVariant DbStore::getConfig(const QString& key, const QVariant& defaultValue)
{
    if (!openIfNeeded()) return defaultValue;

    QSqlQuery q(dbRef());
    q.prepare("SELECT value FROM config WHERE key=?");
    q.addBindValue(key);

    if (!q.exec()) {
        setErr("getConfig failed: " + q.lastError().text());
        return defaultValue;
    }
    if (!q.next()) return defaultValue;

    return q.value(0);
}

/**
 * @brief deleteConfig
 * 删除配置项。
 */
bool DbStore::deleteConfig(const QString& key)
{
    if (!openIfNeeded()) return false;

    QSqlQuery q(dbRef());
    q.prepare("DELETE FROM config WHERE key=?");
    q.addBindValue(key);

    if (!q.exec()) {
        setErr("deleteConfig failed: " + q.lastError().text());
        return false;
    }
    return true;
}

// -------------------- Swipe Log APIs --------------------

/**
 * @brief insertSwipeLog
 * 插入刷卡日志。
 *
 * @param ts        时间戳（字符串形式，建议上层统一格式，例如 ISO-8601）
 * @param cardId    卡号
 * @param cardType  卡类型
 * @param result    结果码（业务自定义）
 * @param reason    失败原因/说明（可空）
 * @param raw       原始数据（BLOB）
 * @param uploaded  上报状态：0 未上报，1 已上报
 */
bool DbStore::insertSwipeLog(const QString& ts,
                            const QString& cardId,
                            const QString& cardType,
                            int result,
                            const QString& reason,
                            const QByteArray& raw,
                            int uploaded)
{
    if (!openIfNeeded()) return false;

    QSqlQuery q(dbRef());
    q.prepare(R"(
        INSERT INTO swipe_log(ts, card_id, card_type, result, reason, raw, uploaded)
        VALUES(?,?,?,?,?,?,?)
    )");

    q.addBindValue(ts);
    q.addBindValue(cardId);
    q.addBindValue(cardType);
    q.addBindValue(result);
    q.addBindValue(reason);
    q.addBindValue(raw);
    q.addBindValue(uploaded);

    if (!q.exec()) {
        setErr("insertSwipeLog failed: " + q.lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief toRow
 * 将当前 QSqlQuery 指向的行转换为 SwipeLogRow。
 *
 * 注意：
 * - 使用字段名取值要求 SELECT 中包含对应字段别名/名称。
 */
static SwipeLogRow toRow(QSqlQuery& q)
{
    SwipeLogRow r;
    r.id       = q.value("id").toLongLong();
    r.ts       = q.value("ts").toString();
    r.cardId   = q.value("card_id").toString();
    r.cardType = q.value("card_type").toString();
    r.result   = q.value("result").toInt();
    r.reason   = q.value("reason").toString();
    r.raw      = q.value("raw").toByteArray();
    r.uploaded = q.value("uploaded").toInt();
    return r;
}

/**
 * @brief querySwipeLogs
 * 分页查询刷卡日志，按 id 倒序（最新在前）。
 */
QList<SwipeLogRow> DbStore::querySwipeLogs(int limit, int offset)
{
    QList<SwipeLogRow> out;
    if (!openIfNeeded()) return out;

    QSqlQuery q(dbRef());
    q.prepare(R"(
        SELECT id, ts, card_id, card_type, result, reason, raw, uploaded
        FROM swipe_log
        ORDER BY id DESC
        LIMIT ? OFFSET ?
    )");
    q.addBindValue(limit);
    q.addBindValue(offset);

    // 执行查询数据
    if (!q.exec()) {
        setErr("querySwipeLogs failed: " + q.lastError().text());
        return out;
    }

    // 判断是否查到数据
    while (q.next()) out.push_back(toRow(q));
    return out;
}

/**
 * @brief queryUnuploadedLogs
 * 查询未上报日志（uploaded=0），按 id 正序（便于按时间顺序上报/重试）。
 */
QList<SwipeLogRow> DbStore::queryUnuploadedLogs(int limit)
{
    QList<SwipeLogRow> out;
    if (!openIfNeeded()) return out;

    QSqlQuery q(dbRef());
    q.prepare(R"(
        SELECT id, ts, card_id, card_type, result, reason, raw, uploaded
        FROM swipe_log
        WHERE uploaded=0
        ORDER BY id ASC
        LIMIT ?
    )");
    q.addBindValue(limit);

    if (!q.exec()) {
        setErr("queryUnuploadedLogs failed: " + q.lastError().text());
        return out;
    }

    while (q.next()) out.push_back(toRow(q));
    return out;
}

/**
 * @brief markUploaded
 * 状态更新函数, 将指定 id 列表标记为已上报（uploaded=1）。
 *
 * 实现要点：
 * - 使用事务提升多次 UPDATE 的性能，并保证原子性：
 *   任何一条失败则 rollback。
 */
bool DbStore::markUploaded(const QList<qint64>& ids)
{
    if (ids.isEmpty()) return true;
    if (!openIfNeeded()) return false;

    // 事务提高性能并确保一致性
    if (!transactionBegin()) return false;

    QSqlQuery q(dbRef());
    q.prepare("UPDATE swipe_log SET uploaded=1 WHERE id=?");

    QVariantList idList;
    idList.reserve(ids.size());
    for (auto id : ids) idList << id;

    q.addBindValue(idList);

    if (!q.execBatch()) {
        setErr("markUploaded failed: " + q.lastError().text());
        transactionRollback();
        return false;
    }

    return transactionCommit();
}

// -------------------- Transaction APIs --------------------

/**
 * @brief transactionBegin
 * 开启事务。
 */
bool DbStore::transactionBegin()
{
    if (!openIfNeeded()) return false;

    if (!dbRef().transaction()) {
        setErr("transactionBegin failed: " + dbRef().lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief transactionCommit
 * 提交事务。
 */
bool DbStore::transactionCommit()
{
    if (!openIfNeeded()) return false;

    if (!dbRef().commit()) {
        setErr("transactionCommit failed: " + dbRef().lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief transactionRollback
 * 回滚事务。
 */
bool DbStore::transactionRollback()
{
    if (!openIfNeeded()) return false;

    if (!dbRef().rollback()) {
        setErr("transactionRollback failed: " + dbRef().lastError().text());
        return false;
    }
    return true;
}

/**
 * @brief 检查指定配置键是否存在。
 *
 * 查询失败时保留数据库错误并返回 false，因此调用方如需区分“不存在”和“查询失败”，
 * 应同时检查 lastError()。
 */
bool DbStore::hasConfig(const QString& key)
{
    if (!openIfNeeded()) return false;

    QSqlQuery q(dbRef());
    q.prepare("SELECT 1 FROM config WHERE key=? LIMIT 1");
    q.addBindValue(key);

    if (!q.exec()) {
        setErr("hasConfig failed: " + q.lastError().text());
        return false;
    }
    return q.next();
}


// -------------------- Maintenance APIs --------------------

/**
 * @brief checkpoint
 * WAL checkpoint：将 WAL 日志合并回主库，并 TRUNCATE WAL 文件（收缩体积）。
 */
bool DbStore::checkpoint()
{
    return exec("PRAGMA wal_checkpoint(TRUNCATE);");
}

/**
 * @brief vacuum
 * VACUUM：重建数据库文件以回收空间（空闲时执行）。
 */
bool DbStore::vacuum()
{
    return exec("VACUUM;");
}
