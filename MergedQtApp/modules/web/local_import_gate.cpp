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
bool LocalImportGate::acquire() { bool expected=false; return busy.compare_exchange_strong(expected,true); }
void LocalImportGate::release() { busy.store(false); }
