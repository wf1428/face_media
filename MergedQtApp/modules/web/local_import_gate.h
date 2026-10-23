/**
 * @file local_import_gate.h
 * @brief 原 U 盘导入与新增 Web 导入共用的互斥预约，不改变各自解析/写入语义。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

namespace LocalImportGate {

/**
 * @brief 尝试取得本地导入入口之间的互斥预约。
 * @return 成功预约唯一导入权时返回 true，不阻塞 UI 或工作线程。
 */
bool acquire();

/**
 * @brief 当前持有者结束所有人员/图片阶段后释放预约。
 */
void release();
} // namespace LocalImportGate
