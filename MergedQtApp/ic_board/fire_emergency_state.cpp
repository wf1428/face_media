/**
 * @file fire_emergency_state.cpp
 * @brief 实现消防业务状态的无锁跨线程读写。
 * @author Du lin
 * @date 2026-11-19
 */

#include "fire_emergency_state.h"

#include <atomic>

namespace {
std::atomic_bool g_fireActive(false); /**< true 时平台梯控配置只保存，不发送 RS485。 */
}

namespace FireEmergencyState {

bool isActive()
{
    return g_fireActive.load(std::memory_order_acquire);
}

bool exchangeActive(bool active)
{
    return g_fireActive.exchange(active, std::memory_order_acq_rel);
}

} // namespace FireEmergencyState
