/**
 * @file rpc_client.h
 * @brief 独立非 Qt 服务的 NDJSON Unix Domain Socket 客户端。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include "json.hpp"
#include <string>
#include <stdexcept>

namespace deviceweb {

using Json = nlohmann::json; /**< daemon 内唯一 JSON 模型，与 Qt 只通过协议通信。 */

/** @brief HTTP/API 错误，保留可呈现说明与状态。 */
struct ApiError : std::runtime_error {
    int status; /**< HTTP 状态码。 */

    /**
     * @brief 保存状态及业务错误文本。
     * @param code 映射到 HTTP 的错误状态码。
     * @param message 可返回调用方的结果或错误说明。
     */
    ApiError(int code, const std::string &message) : std::runtime_error(message), status(code)
    {
    }
};

/** @brief 每次调用使用独立 socket，可由 HTTP 工作线程并行调用。 */
class RpcClient {
public:

    /**
     * @brief 保存系统私有 socket 路径。
     * @param path Qt 业务服务 Unix socket 路径。
     */
    explicit RpcClient(std::string path) : path_(std::move(path))
    {
    }

    /**
     * @brief 在统一截止时间内调用 Qt 业务接口并校验 NDJSON 响应。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 校验 version/id 后的 Qt 数据；超时或限长失败抛出 ApiError。
     * @note 连接、发送和接收共用 25 秒截止时间；IPC 单帧上限 1 MiB，不传送媒体文件字节。
     */
    Json call(const std::string &operation, const Json &args = Json::object(), int owner = 0) const;

private:

    std::string path_; /**< 不允许 HTTP 请求覆盖的 socket 路径。 */
};
} // namespace deviceweb
