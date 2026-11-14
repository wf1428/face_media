/**
 * @file monotonic_clock.h
 * @brief 提供可跨线程比较的单调毫秒，用于媒体到帧和超时判断。
 * @author Du lin
 * @date 2026-11-13
 */
#pragma once

#include <QtGlobal>
#include <chrono>

namespace MonotonicClock {

/** @brief 读取进程内共享的单调时间基准。
 * @return 相对首次调用的毫秒值加 1，保留 0 表示未采样。可跨线程比较，不受系统校时影响。
 */
inline qint64 nowMs()
{
    using Clock = std::chrono::steady_clock;
    static const auto origin = Clock::now();
    return 1 + std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count();
}

} // namespace MonotonicClock
