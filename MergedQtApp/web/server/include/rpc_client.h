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

    /** @brief 保存状态及业务错误文本。 */
    ApiError(int code, const std::string &message) : std::runtime_error(message), status(code)
    {
    }
};

/** @brief 每次调用使用独立 socket，可由 HTTP 工作线程并行调用。 */
class RpcClient {
public:

    /** @brief 保存系统私有 socket 路径。 */
    explicit RpcClient(std::string path) : path_(std::move(path))
    {
    }

    /** @return 校验 version/id 后的 Qt 数据；超时或限长失败抛出 ApiError。 */
    Json call(const std::string &operation, const Json &args = Json::object(), int owner = 0) const;

private:

    std::string path_; /**< 不允许 HTTP 请求覆盖的 socket 路径。 */
};
} // namespace deviceweb
