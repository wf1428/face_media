/**
 * @file dbstore.h
 * @brief 一条刷卡日志记录。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef DBSTORE_H
#define DBSTORE_H

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QList>

/**
 * @brief 一条刷卡日志记录。
 *
 * 字段与 SQLite swipe_log 表保持对应，uploaded 用于可靠上报后的状态标记。
 */
struct SwipeLogRow {
    qint64 id = 0;      /**< 数据库自增主键。 */
    QString ts;         /**< 业务时间戳字符串。 */
    QString cardId;     /**< 卡号。 */
    QString cardType;   /**< 卡类型：visitor、multi 或 unknown。 */
    int result = 0;     /**< 刷卡结果：1 表示通过，0 表示拒绝。 */
    QString reason;     /**< 结果原因或补充说明。 */
    QByteArray raw;     /**< 可选的原始协议帧。 */
    int uploaded = 0;   /**< 上报状态：0 未上传，1 已上传。 */
};

/**
 * @brief 应用配置和刷卡日志的 SQLite 静态访问层。
 *
 * 数据库文件路径在初始化后跨线程共享，每个线程使用独立命名连接，避免跨线程复用
 * QSqlDatabase 连接对象。最近错误同样按线程保存。
 */
class DbStore {
public:
    /**
     * @brief 记录数据库路径，打开当前线程连接并完成 PRAGMA、建表和建索引。
     * @param dbPath SQLite 数据库文件路径。
     * @return 全部初始化步骤成功时返回 true。
     */
    static bool bootstrap(const QString& dbPath);

    /** @brief 关闭并移除当前线程对应的数据库连接。 */
    static void close();

    /** @brief 插入或更新一个字符串化保存的配置项。 */
    static bool setConfig(const QString& key, const QVariant& value);

    /** @return 配置值；配置不存在或查询失败时返回 defaultValue。 */
    static QVariant getConfig(const QString& key, const QVariant& defaultValue = {});

    /** @brief 删除指定配置项。 */
    static bool deleteConfig(const QString& key);

    /**
     * @brief 插入一条刷卡日志。
     * @param ts 业务时间戳字符串。
     * @param cardId 卡号。
     * @param cardType 卡类型。
     * @param result 业务结果码。
     * @param reason 结果原因或说明。
     * @param raw 可选原始帧。
     * @param uploaded 上报状态：0 未上传，1 已上传。
     */
    static bool insertSwipeLog(const QString& ts,
                               const QString& cardId,
                               const QString& cardType,
                               int result,
                               const QString& reason,
                               const QByteArray& raw = QByteArray(),
                               int uploaded = 0);

    /** @return 按 id 倒序分页取得的刷卡日志，最新记录在前。 */
    static QList<SwipeLogRow> querySwipeLogs(int limit = 50, int offset = 0);

    /** @return 按 id 正序取得的未上传日志，便于保持上报顺序。 */
    static QList<SwipeLogRow> queryUnuploadedLogs(int limit = 100);

    /** @brief 在单个事务中将指定日志原子地标记为已上传。 */
    static bool markUploaded(const QList<qint64>& ids);

    /** @brief 开启当前线程数据库连接的事务。 */
    static bool transactionBegin();

    /** @brief 提交当前线程数据库连接的事务。 */
    static bool transactionCommit();

    /** @brief 回滚当前线程数据库连接的事务。 */
    static bool transactionRollback();

    /** @brief 执行无需返回结果集的 DDL 或 DML 语句。 */
    static bool exec(const QString& sql);

    /** @brief 使用位置参数绑定执行无需返回结果集的 DDL 或 DML 语句。 */
    static bool execute(const QString& sql, const QList<QVariant>& binds = {});

    /** @return 使用位置参数绑定执行查询后得到的字段名到字段值映射列表。 */
    static QList<QVariantMap> query(const QString& sql, const QList<QVariant>& binds = {});

    /** @brief 执行 VACUUM 重建数据库文件并回收空闲空间。 */
    static bool vacuum();

    /** @brief 执行 TRUNCATE checkpoint，将 WAL 合并回主数据库。 */
    static bool checkpoint();

    /** @return 当前线程最近一次数据库错误信息。 */
    static QString lastError();

    /** @return 指定配置键存在时返回 true；查询失败同样返回 false。 */
    static bool hasConfig(const QString& key);

    static thread_local QString m_lastError; /**< 当前线程最近一次数据库错误。 */

    /** @return 包含当前线程 ID 的数据库连接名。 */
    static QString connName();

private:
    /** @brief 检查运行环境是否提供 QSQLITE 驱动。 */
    static bool ensureDriver();

    /** @brief 获取或创建当前线程连接，并在需要时打开数据库。 */
    static bool openIfNeeded();

    /** @brief 应用 WAL、同步、外键和临时存储相关 PRAGMA。 */
    static bool applyPragmas();

    /** @brief 创建 config 与 swipe_log 业务表。 */
    static bool createTables();

    /** @brief 创建刷卡日志常用查询索引。 */
    static bool prepareIndices();

};


#endif // DBSTORE_H
