/**
 * @file rpc_client.cpp
 * @brief 使用限时 Unix socket 进行 Qt RPC；二进制数据不在 IPC 内传输。
 * @author Du lin
 * @date 2026-10-09
 */

#include "rpc_client.h"
#include "console_log.h"
#include <atomic>
#include <cstring>
#include <chrono>
#include <system_error>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#endif

namespace deviceweb {

/**
 * @brief 序列化白名单请求，经 Unix socket 交给 Qt 并解包响应。
 * @param operation 白名单业务操作名。
 * @param args 操作对应的结构化参数。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @return Qt 响应中的 data；传输、协议或业务失败抛出 ApiError。
 * @note 连接、发送和接收共用 25 秒截止时间；IPC 单帧上限 1 MiB，不传送媒体文件字节。
 */
Json RpcClient::call(const std::string &operation, const Json &args, int owner) const
{
    const auto failure =
        [&](int code, const std::string &message, const char *stage, int systemError = 0) {
            std::string details = "Qt RPC operation=" + operation + " socket=" + path_ +
                                  " stage=" + stage + " status=" + std::to_string(code);
            if (systemError)
                details += " errno=" + std::to_string(systemError) + " (" +
                           std::system_category().message(systemError) + ")";
            // A backend reply can contain business data; record only its status.
            // Transport messages are fixed diagnostics, never serialized args.
            if (std::string(stage) != "Qt reply")
                details += " reason=" + message;
            consoleLog(code >= 500 ? LogLevel::Error : LogLevel::Warning, details);
            return ApiError(code, message);
        };
#ifdef _WIN32
    (void)operation;
    (void)args;
    (void)owner;
    throw failure(503, "Windows 构建仅供单元检查；生产 RPC 要求 Linux Unix socket", "platform");
#else
    // 请求只包含结构化业务参数，不通过 IPC 传递视频或图片字节。
    static std::atomic<uint64_t> sequence{0};
    const std::string id = std::to_string(++sequence);
    std::string request =
        Json{{"version", 1}, {"id", id}, {"operation", operation}, {"args", args}, {"owner", owner}}
            .dump() +
        '\n';
    constexpr size_t maxFrame = 1024 * 1024;
    if (request.size() > maxFrame)
        throw failure(413, "IPC 请求超过长度限制", "prepare");
    if (path_.size() >= sizeof(sockaddr_un::sun_path))
        throw failure(503, "IPC 路径过长", "prepare");

    // 使用非阻塞 Unix socket，并以统一截止时间限制连接与读写。
    struct Descriptor {
        int fd;

        /** @brief 在所有返回和异常路径关闭 IPC socket。 */
        ~Descriptor()
        {
            if (fd >= 0)
                ::close(fd);
        }
    } socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};

    if (socket.fd < 0)
        throw failure(503, "无法连接设备业务服务", "socket", errno);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);

    // 所有连接与读写等待共用同一个截止时间，避免多次等待累积阻塞。
    const auto ready = [&](short events) {
        while (true) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0)
                throw failure(503, "设备业务服务响应超时，请查询任务状态", "wait", ETIMEDOUT);
            pollfd descriptor{socket.fd, events, 0};
            const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0 || descriptor.revents & (POLLERR | POLLNVAL))
                throw failure(503,
                              "设备业务服务暂不可用",
                              result > 0 ? "poll error events" : "poll",
                              result < 0    ? errno
                              : result == 0 ? ETIMEDOUT
                                            : 0);
            if (descriptor.revents & events)
                return;
            if (descriptor.revents & POLLHUP)
                throw failure(503, "设备业务连接中断", "poll hangup");
        }
    };
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    if (::connect(socket.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS)
            throw failure(503, "设备业务服务暂不可用", "connect", errno);
        ready(POLLOUT);
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
            throw failure(503, "设备业务服务连接失败", "connect completion", error ? error : errno);
    }

    // 按实际发送长度推进，避免短写丢失请求尾部。
    size_t offset = 0;
    while (offset < request.size()) {
        ready(POLLOUT);
        const auto sent =
            ::send(socket.fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (sent <= 0)
            throw failure(503, "设备业务请求发送失败", "send", sent < 0 ? errno : 0);
        offset += static_cast<size_t>(sent);
    }

    // 响应必须在限额内完成单行 JSON 帧。
    std::string response;
    while (response.find('\n') == std::string::npos) {
        ready(POLLIN);
        char buffer[8192];
        const auto received = ::recv(socket.fd, buffer, sizeof(buffer), 0);
        if (received < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (received <= 0)
            throw failure(503, "设备业务响应中断", "receive", received < 0 ? errno : 0);
        response.append(buffer, static_cast<size_t>(received));
        if (response.size() > maxFrame)
            throw failure(503, "设备业务响应过大", "receive limit");
    }

    // 校验协议版本和请求编号，避免把其他响应误配到本次请求。
    Json result = Json::parse(response.substr(0, response.find('\n')), nullptr, false);
    if (!result.is_object() || result.value("version", 0) != 1 ||
        result.value("id", std::string{}) != id)
        throw failure(503, "设备业务响应格式错误", "parse reply");
    const int code = result.value("code", 500);
    if (code != 200)
        throw failure(code, result.value("message", std::string("设备操作失败")), "Qt reply");
    return result.at("data");
#endif
}
} // namespace deviceweb
