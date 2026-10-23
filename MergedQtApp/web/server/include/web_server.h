/**
 * @file web_server.h
 * @brief 独立 C++ HTTP/HTTPS API、流式上传与静态站点入口。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include "rpc_client.h"
#include "session_store.h"
#include "httplib.h"
#include <unordered_set>

namespace deviceweb {

/** @brief daemon 静态配置，只从设备部署文件读取。 */
struct ServerConfig {
    std::string bind;             /**< AP 接口地址，不默认绑定公网接口。 */
    int port = 443;               /**< HTTPS 监听端口。 */
    int portalPort = 80;          /**< HTTP captive portal 入口端口，0 禁用。 */
    std::string origin;           /**< 固定外部 HTTPS origin，用于 Origin/Host 校验与跳转。 */
    std::string socket;           /**< Qt Unix socket。 */
    std::string www;              /**< Vue 离线构建目录。 */
    std::string certificate;      /**< 证书链 PEM 文件。 */
    std::string privateKey;       /**< 私钥 PEM 文件。 */
    bool developmentHttp = false; /**< 仅显式本机开发允许 HTTP Cookie。 */

    /**
     * @brief 读取并校验固定的 Web 服务部署配置。
     * @param file 待检查的文件路径。
     * @return 检查范围后的配置，未知路径不由请求决定。
     */
    static ServerConfig load(const std::string &file);
};

/** @brief 有界工作池及会话，真实业务通过 Qt RPC 执行。 */
class WebServer {
public:

    /**
     * @brief 创建 HTTP 或 HTTPS 服务并登记 V1 白名单路由。
     * @param config 设备受控配置，包含运行所需路径或功能设置。
     */
    explicit WebServer(ServerConfig config);

    /**
     * @brief 启动 HTTP 引导和主服务，阻塞至收到退出信号。
     * @return 监听正常结束时的结果；监听建立失败返回 false。
     */
    bool run();

    /**
     * @brief 停止监听，等待当前工作线程退出。
     */
    void stop();

private:

    /**
     * @brief 设置会话/CSRF/Host 保护及业务路由。
     */
    void routes();

    /**
     * @brief 登记标准 API 白名单，将路径、参数及业务指令显式对应。
     */
    void standardRoutes();

    /**
     * @brief 复用会话并转换标准参数，处理图片暂存、任务结果和 HTTP 返回。
     * @param operation 白名单业务操作名。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @param response 待写入的 HTTP 响应。
     * @param args 操作对应的结构化参数。
     */
    void standardCall(const std::string &operation,
                      const httplib::Request &request,
                      httplib::Response &response,
                      Json args = Json::object());

    /**
     * @brief 分块接收标准 multipart 视频或资源包，复用受控上传任务。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @param response 待写入的 HTTP 响应。
     * @param reader 流式请求体读取器。
     * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
     */
    void standardUpload(const httplib::Request &request,
                        httplib::Response &response,
                        const httplib::ContentReader &reader,
                        const std::string &kind = "video");

    /**
     * @brief 返回热点浏览器引导页、离线资源或固定服务器地址，不进入登录页。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @param response 待写入的 HTTP 响应。
     */
    void portalResponse(const httplib::Request &request, httplib::Response &response);

    /**
     * @brief 校验 Cookie 并取得有效会话副本。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @return 当前请求 Cookie 会话。
     */
    Session session(const httplib::Request &request);

    /**
     * @brief 从 ContentReader 限长读取 JSON，防止普通 API 缓冲大请求。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @param reader 流式请求体读取器。
     * @return 通过大小及 JSON 格式校验的请求对象；非法请求抛出 ApiError。
     */
    Json readJson(const httplib::Request &request, const httplib::ContentReader &reader);

    /**
     * @brief 写 JSON 统一结果。
     * @param response 待写入的 HTTP 响应。
     * @param data 本次操作的数据对象。
     * @param status 接收本次操作的业务状态码。
     */
    static void result(httplib::Response &response, const Json &data, int status = 200);

    /**
     * @brief 接收 octet-stream，使用 Qt 的受控路径和长度，不解析文件名为路径。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @param response 待写入的 HTTP 响应。
     * @param reader 流式请求体读取器。
     */
    void upload(const httplib::Request &request,
                httplib::Response &response,
                const httplib::ContentReader &reader);

    /**
     * @brief 分块读取视频下载，租约保护下载期间的删除。
     * @param request 当前 HTTP 请求，身份由会话校验取得。
     * @param response 待写入的 HTTP 响应。
     */
    void download(const httplib::Request &request, httplib::Response &response);
    ServerConfig config_;                            /**< 部署配置副本。 */
    RpcClient rpc_;                                  /**< 独立 socket 请求客户端。 */
    SessionStore sessions_;                          /**< 有界内存会话。 */
    std::unique_ptr<httplib::Server> server_;        /**< 主 HTTP/HTTPS listener。 */
    httplib::Server portal_;                         /**< 提供完整浏览器引导页的 HTTP listener。 */
    std::mutex downloadsMutex_;                      /**< 下载文件的删除协调锁。 */
    std::unordered_map<std::string, int> downloads_; /**< 活跃下载引用计数。 */
    std::mutex uploadsMutex_;                 /**< 创建临时文件之前即登记接收，协调取消竞态。 */
    std::unordered_set<std::string> uploads_; /**< 当前 daemon 正在处理的上传 ID。 */
};
} // namespace deviceweb
