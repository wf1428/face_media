/**
 * @file web_schema.h
 * @brief 同一 SQLite 文件内 Web 专用表的独立版本管理入口。
 * @author Du lin
 * @date 2026-10-09
 */
#pragma once
namespace WebSchema {
/** @brief 在当前工作线程创建 Web 表；不修改原 Qt 业务表及 dbstore 建表代码。 */
void migrate();
}
