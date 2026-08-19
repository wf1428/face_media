/**
 * @file probe_log.h
 * @brief 轻量级探针日志工具。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef PROBE_LOG_H
#define PROBE_LOG_H

#include <QDateTime>
#include <QByteArray>
#include <QString>

#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/**
 * @brief 轻量级探针日志工具。
 *
 * 日志统一编码为 UTF-8，并通过底层文件描述符写入 stderr，避免依赖 Qt 消息处理器。
 * 接口保留调用文件、行号和函数参数，以便宏调用保持统一签名。
 */
namespace ProbeLog {

/**
 * @brief 尽可能将指定长度的数据完整写入文件描述符。
 *
 * 被信号中断时继续写入；遇到其他写入错误或无进展时停止，避免无限循环。
 *
 * @param fd 目标文件描述符，负数时直接返回。
 * @param data 待写入数据首地址。
 * @param len 待写入长度，单位字节。
 */
inline void writeAllFd(int fd, const char *data, int len)
{
    if (fd < 0 || !data || len <= 0) return;

    const char *p = data;
    int left = len;
    while (left > 0) {
        const int n = ::write(fd, p, left);
        if (n > 0) {
            p += n;
            left -= n;
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

/**
 * @brief 生成带本地时间戳和探针标记的单行 UTF-8 日志。
 * @param file 调用文件名；当前输出格式暂不包含该字段。
 * @param line 调用行号；当前输出格式暂不包含该字段。
 * @param func 调用函数名；当前输出格式暂不包含该字段。
 * @param msg 日志正文，空指针按空字符串处理。
 * @return 以换行符结尾的日志字节串。
 */
inline QByteArray buildLine(const char *file,
                            int line,
                            const char *func,
                            const char *msg)
{
    Q_UNUSED(file);
    Q_UNUSED(line);
    Q_UNUSED(func);

    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    QByteArray out;
    out.reserve(1024);
    out.append('[');
    out.append(ts.toUtf8());
    out.append("][P] ");
    out.append(msg ? msg : "");
    out.append('\n');
    return out;
}


/**
 * @brief 格式化日志行并写入标准错误输出。
 *
 * 实际输出进入 fd=2 的现有日志链。
 */
inline void writeLineRaw(const char *file,
                         int line,
                         const char *func,
                         const char *msg)
{
    const QByteArray out = buildLine(file, line, func, msg);

    writeAllFd(2, out.constData(), out.size());
}


/**
 * @brief 使用 printf 风格参数生成探针日志。
 *
 * 格式化缓冲区固定为 1024 字节，超长内容由 vsnprintf 截断并保证末尾为 '\0'。
 */
inline void writeFmt(const char *file,
                     int line,
                     const char *func,
                     const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        writeLineRaw(file, line, func, "vsnprintf_failed");
        return;
    }

    buf[sizeof(buf) - 1] = '\0';
    writeLineRaw(file, line, func, buf);
}

/** @brief 将 QString 转为 UTF-8 后写入探针日志。 */
inline void writeQString(const char *file,
                         int line,
                         const char *func,
                         const QString &msg)
{
    const QByteArray utf8 = msg.toUtf8();
    writeLineRaw(file, line, func, utf8.constData());
}

} // namespace ProbeLog

/** @brief 写入 C 字符串探针日志，并自动附带调用位置参数。 */
#define PROBE(msg) \
    ::ProbeLog::writeLineRaw(__FILE__, __LINE__, Q_FUNC_INFO, (msg))

/** @brief 写入 printf 风格探针日志，并自动附带调用位置参数。 */
#define PROBEF(fmt, ...) \
    ::ProbeLog::writeFmt(__FILE__, __LINE__, Q_FUNC_INFO, (fmt), ##__VA_ARGS__)

/** @brief 写入 QString 探针日志，并自动附带调用位置参数。 */
#define PROBEQ(msg) \
    ::ProbeLog::writeQString(__FILE__, __LINE__, Q_FUNC_INFO, (msg))

#endif // PROBE_LOG_H
