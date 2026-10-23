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
/** @brief 控制台输出等级，仅影响日志前缀。 */
enum class LogLevel {
    Info,    /**< 正常操作与状态信息。 */
    Warning, /**< 可继续服务的异常或被拒绝操作。 */
    Error    /**< 请求或模块执行失败。 */
};

// Keep untrusted paths and error descriptions on one bounded console line.

/**
 * @brief 将外部文本转换为有长度上限的单行日志内容。
 * @param value 待清理控制字符并限制长度的外部日志文本。
 * @return 转义控制字符后的单行日志文本。
 */
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

/**
 * @brief 线程安全地输出带时间和级别的控制台日志，并立即刷新。
 * @param level 日志等级。
 * @param message 可返回调用方的结果或错误说明。
 */
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
