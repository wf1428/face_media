/**
 * @file console_log.h
 * @brief 线程安全、逐行刷新的控制台日志，不创建日志文件。
 * @author Du lin
 * @date 2026-10-16
 */

#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace deviceweb {
enum class LogLevel { Info, Warning, Error };

// Keep untrusted paths and error descriptions on one bounded console line.

/** @brief 将外部文本转换为有长度上限的单行日志内容。 */
inline std::string logText(const std::string &value)
{
    std::string result;
    for (size_t i = 0; i < value.size() && i < 2048; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else if (c == '\t')
            result += "\\t";
        else if (c < 32 || c == 127)
            result += '?';
        else
            result += static_cast<char>(c);
    }
    if (value.size() > 2048)
        result += "...";
    return result;
}

/** @brief 线程安全地输出带时间和级别的控制台日志，并立即刷新。 */
inline void consoleLog(LogLevel level, const std::string &message)
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const char *label = level == LogLevel::Error     ? "ERROR"
                        : level == LogLevel::Warning ? "WARN"
                                                     : "INFO";
    auto &output = level == LogLevel::Info ? std::cout : std::cerr;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " [" << label << "] "
           << logText(message) << std::endl;
}
} // namespace deviceweb
