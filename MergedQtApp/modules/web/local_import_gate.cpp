/**
 * @file local_import_gate.cpp
 * @brief 进程内导入互斥的原子标志实现，允许结束回调从同一任务线程释放。
 * @author Du lin
 * @date 2026-10-09
 */

#include "local_import_gate.h"
#include <atomic>

namespace {

std::atomic_bool busy{false}; /**< 全应用共享本地人员批量导入占用标志。 */
}

/**
 * @brief 申请本地人员导入互斥门，防止与 U 盘导入并发。
 * @return 成功取得导入互斥权时为 true；已有导入或校验任务占用时为 false。
 */
bool LocalImportGate::acquire()
{
    bool expected = false;
    return busy.compare_exchange_strong(expected, true);
}

/**
 * @brief 释放本地人员导入互斥门。
 */
void LocalImportGate::release()
{
    busy.store(false);
}
