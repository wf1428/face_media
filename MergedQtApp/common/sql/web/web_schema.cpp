/**
 * @file web_schema.cpp
 * @brief 集中管理 web_* DDL，以事务完成新增表与版本登记。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_schema.h"
#include "web_sql.h"

/**
 * @brief 在统一 Qt 数据库中迁移独立的 Web 表。
 */
void WebSchema::migrate()
{
    if (!DbStore::transactionBegin())
        throw WebError(503, QStringLiteral("无法开启 Web 数据库迁移"));
    try {
        // 版本表独立记录 Web schema，不修改原 Qt 业务表的迁移逻辑。
        WebSql::execute(
            QStringLiteral("CREATE TABLE IF NOT EXISTS web_schema_versions(version INTEGER PRIMARY "
                           "KEY, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"));
        const auto version = WebSql::query(
            QStringLiteral("SELECT MAX(version) AS version FROM web_schema_versions"));
        if (!version.isEmpty() && version.first().value("version").toInt() > 1)
            throw WebError(503, QStringLiteral("Web 数据库版本较新，拒绝降级启动"));

        // 账号、导入批次、上传任务、视频索引和批次文件同处统一数据库。
        WebSql::execute(
            QStringLiteral("CREATE TABLE IF NOT EXISTS web_accounts(id INTEGER PRIMARY KEY, "
                           "username TEXT NOT NULL UNIQUE, salt BLOB NOT NULL, password_hash BLOB "
                           "NOT NULL, iterations INTEGER NOT NULL, enabled INTEGER NOT NULL "
                           "DEFAULT 1, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"));
        WebSql::execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS web_person_import_batches(id TEXT PRIMARY KEY, owner_id "
            "INTEGER NOT NULL REFERENCES web_accounts(id), state TEXT NOT NULL, result_json TEXT "
            "NOT NULL DEFAULT '{}', created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL)"));
        WebSql::execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS web_upload_tasks(id TEXT PRIMARY KEY, owner_id INTEGER NOT "
            "NULL REFERENCES web_accounts(id), kind TEXT NOT NULL, batch_id TEXT REFERENCES "
            "web_person_import_batches(id), original_name TEXT NOT NULL, expected_bytes INTEGER "
            "NOT NULL, received_bytes INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL, part_path "
            "TEXT NOT NULL, final_path TEXT NOT NULL, error TEXT NOT NULL DEFAULT '', updated_at "
            "INTEGER NOT NULL)"));
        WebSql::execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS web_upload_batch ON web_upload_tasks(batch_id,state)"));
        WebSql::execute(
            QStringLiteral("CREATE TABLE IF NOT EXISTS web_media_files(id TEXT PRIMARY KEY, "
                           "upload_id TEXT REFERENCES web_upload_tasks(id), name TEXT NOT NULL, "
                           "path TEXT NOT NULL UNIQUE, bytes INTEGER NOT NULL, modified_ms INTEGER "
                           "NOT NULL, state TEXT NOT NULL DEFAULT 'READY')"));
        WebSql::execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS web_person_import_assets(id TEXT PRIMARY KEY REFERENCES "
            "web_upload_tasks(id), batch_id TEXT NOT NULL REFERENCES "
            "web_person_import_batches(id), kind TEXT NOT NULL, original_name TEXT NOT NULL, path "
            "TEXT NOT NULL, UNIQUE(batch_id,original_name COLLATE NOCASE))"));

        // 表结构与版本标记作为同一事务提交，失败时全部回滚。
        WebSql::execute(
            QStringLiteral("INSERT OR IGNORE INTO web_schema_versions(version) VALUES(1)"));
        if (!DbStore::transactionCommit())
            throw WebError(503, QStringLiteral("Web 数据库迁移提交失败"));
    } catch (...) {
        DbStore::transactionRollback();
        throw;
    }
}
