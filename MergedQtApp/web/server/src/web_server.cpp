/**
 * @file web_server.cpp
 * @brief 登录、文件上传/获取/删除、Excel 本地导入 API 的实现。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_server.h"
#include "console_log.h"
#include <array>
#include <cerrno>
#include <fstream>
#include <thread>
#include <regex>
#include <system_error>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace deviceweb {
namespace {
using RequestClock = std::chrono::steady_clock;

struct RequestTrace {
    const httplib::Request *request = nullptr;
    RequestClock::time_point start;
};

// cpp-httplib routes and writes a response on the same worker. No request
// metadata is added to browser-visible headers, parameters or cookies.
thread_local RequestTrace requestTrace;

/**
 * @brief 记录请求开始时间，供控制台访问日志计算耗时。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 */
void beginRequest(const httplib::Request &request)
{
    requestTrace = {&request, RequestClock::now()};
}

/**
 * @brief 输出不含请求体、凭据和查询参数的访问日志。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param response 待写入的 HTTP 响应。
 */
void logRequest(const httplib::Request &request, const httplib::Response &response)
{
    const auto elapsed = requestTrace.request == &request
                             ? std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  RequestClock::now() - requestTrace.start)
                                                  .count()) +
                                   "ms"
                             : "unavailable";
    requestTrace.request = nullptr;
    const auto level = response.status >= 500   ? LogLevel::Error
                       : response.status >= 400 ? LogLevel::Warning
                                                : LogLevel::Info;
    // path excludes the query string. Never log body, Cookie or auth headers.
    consoleLog(level,
               "HTTP client=" + request.remote_addr + " " + request.method + " " + request.path +
                   " status=" + std::to_string(response.status) + " elapsed=" + elapsed);
}

/**
 * @brief 取得当前平台 socket 错误的固定诊断文本。
 * @return 当前监听错误的可读说明。
 */
std::string socketError()
{
#ifdef _WIN32
    const int code = WSAGetLastError();
#else
    const int code = errno;
#endif
    return " OS_error=" + std::to_string(code) + " (" + std::system_category().message(code) + ")";
}
} // namespace

/**
 * @brief 读取并校验监听地址、证书和部署路径。
 * @param path 待访问的文件或配置路径。
 * @return 通过校验的服务配置；无效路径或配置抛出异常。
 */
ServerConfig ServerConfig::load(const std::string &path)
{
    std::ifstream file(path);
    Json data;
    file >> data;
    ServerConfig config;
    config.bind = data.at("bind");
    config.port = data.value("port", 443);
    config.portalPort = data.value("portal_port", 80);
    config.origin = data.at("origin");
    config.socket = data.at("socket");
    config.www = data.at("www");
    config.certificate = data.value("certificate", std::string{});
    config.privateKey = data.value("private_key", std::string{});
    config.developmentHttp = data.value("development_http", false);
    if (!std::regex_match(config.origin, std::regex(R"(https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?)")) ||
        config.bind.empty() || config.socket.empty() || config.www.empty() || config.port < 1 ||
        config.port > 65535 || config.portalPort < 0 || config.portalPort > 65535 ||
        config.port == config.portalPort)
        throw std::runtime_error("invalid server configuration");
    if (config.developmentHttp &&
        (config.bind != "127.0.0.1" || config.origin.rfind("http://", 0) != 0 ||
         config.portalPort != 0))
        throw std::runtime_error("development HTTP requires loopback bind/origin and no portal");
    if (!config.developmentHttp && (config.origin.rfind("https://", 0) != 0 ||
                                    config.certificate.empty() || config.privateKey.empty()))
        throw std::runtime_error("production HTTPS requires certificate/key and HTTPS origin");
    return config;
}

/**
 * @brief 创建 HTTPS 服务、限制请求资源并登记路由。
 * @param config 设备受控配置，包含运行所需路径或功能设置。
 */
WebServer::WebServer(ServerConfig config) : config_(std::move(config)), rpc_(config_.socket)
{
    if (config_.developmentHttp)
        server_ = std::make_unique<httplib::Server>();
    else {
#ifdef DEVICEWEB_TLS
        server_ = std::make_unique<httplib::SSLServer>(config_.certificate.c_str(),
                                                       config_.privateKey.c_str());
#else
        throw std::runtime_error("this build has no TLS; production HTTPS is unavailable");
#endif
    }
    server_->new_task_queue = []() {
        return new httplib::ThreadPool(4, 16);
    };
    server_->set_payload_max_length(8ULL * 1024 * 1024 * 1024);
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(30, 0);
    server_->set_keep_alive_max_count(20);
    if (!server_->is_valid() || !server_->set_mount_point("/", config_.www))
        throw std::runtime_error("HTTPS credentials or static directory unavailable");
    routes();
}

/**
 * @brief 根据请求 Cookie 获取当前登录会话。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @return 校验通过的会话；未登录或过期抛出 ApiError。
 */
Session WebServer::session(const httplib::Request &request)
{
    return sessions_.get(sessionCookie(request.get_header_value("Cookie")));
}

/**
 * @brief 写入统一格式的 JSON 响应。
 * @param response 待写入的 HTTP 响应。
 * @param data 本次操作的数据对象。
 * @param status 接收本次操作的业务状态码。
 */
void WebServer::result(httplib::Response &response, const Json &data, int status)
{
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(Json{{"code", status}, {"message", "ok"}, {"data", data}}.dump(),
                         "application/json; charset=utf-8");
}

/**
 * @brief 限长读取并解析 JSON 请求体。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param reader 流式请求体读取器。
 * @return 通过大小及 JSON 格式校验的请求对象；非法请求抛出 ApiError。
 */
Json WebServer::readJson(const httplib::Request &request, const httplib::ContentReader &reader)
{
    if (request.get_header_value("Content-Type").rfind("application/json", 0) != 0 ||
        request.get_header_value_u64("Content-Length", 0) > 131072)
        throw ApiError(400, "请使用限长 JSON 请求");
    std::string body;
    if (!reader([&](const char *data, size_t size) {
            if (body.size() + size > 131072)
                return false;
            body.append(data, size);
            return true;
        }))
        throw ApiError(413, "JSON 请求读取失败或超出限制");
    Json data = Json::parse(body, nullptr, false);
    if (!data.is_object())
        throw ApiError(400, "请求 JSON 格式错误");
    return data;
}

/**
 * @brief 登记公开、认证、文件和人员导入接口及请求保护。
 */
void WebServer::routes()
{
    server_->set_logger(logRequest);
    // 先校验固定设备地址、来源和会话，业务路由无需重复实现请求保护。
    server_->set_pre_routing_handler([this](const httplib::Request &request,
                                            httplib::Response &response) {
        beginRequest(request);
        const std::string host = config_.origin.substr(config_.origin.find("://") + 3);
        if (request.get_header_value("Host") != host) {
            response.status = 421;
            response.set_content("设备访问地址不正确", "text/plain; charset=utf-8");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (request.path.rfind("/api/", 0) != 0)
            return httplib::Server::HandlerResponse::Unhandled;
        try {
            if (request.method != "GET" && request.method != "HEAD") {
                if (request.get_header_value("Origin") != config_.origin)
                    throw ApiError(403, "请求来源无效");
                if (request.path != "/api/v1/auth/login" &&
                    request.path != "/api/device/verify-password" &&
                    !secureEqual(request.get_header_value("X-CSRF-Token"), session(request).csrf))
                    throw ApiError(403, "登录校验过期，请刷新页面");
            }
            if (request.path != "/api/v1/auth/login" &&
                request.path != "/api/device/verify-password" &&
                request.path != "/api/device/info" && request.path != "/api/v1/device/info" &&
                request.path != "/api/v1/health" && request.path != "/api/v1/captive")
                session(request);
            return httplib::Server::HandlerResponse::Unhandled;
        } catch (const ApiError &e) {
            consoleLog(e.status >= 500 ? LogLevel::Error : LogLevel::Warning,
                       "HTTP rejected " + request.method + " " + request.path +
                           " status=" + std::to_string(e.status) + " reason=" + e.what());
            response.status = e.status;
            response.set_content(Json{{"code", e.status}, {"message", e.what()}}.dump(),
                                 "application/json; charset=utf-8");
            return httplib::Server::HandlerResponse::Handled;
        }
    });

    // 为静态页面和 API 统一设置浏览器安全响应头。
    server_->set_post_routing_handler([](const httplib::Request &, httplib::Response &response) {
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Referrer-Policy", "no-referrer");
        response.set_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src "
            "'self' data: blob:; font-src 'self' data:; object-src 'none'; frame-ancestors 'none'; "
            "base-uri 'none'");
    });
    server_->set_exception_handler(
        [](const httplib::Request &request, httplib::Response &response, std::exception_ptr error) {
            int code = 500;
            std::string message = "设备服务内部错误";
            try {
                std::rethrow_exception(error);
            } catch (const ApiError &e) {
                code = e.status;
                message = e.what();
            } catch (const std::exception &e) {
                consoleLog(LogLevel::Error, std::string("HTTP internal exception: ") + e.what());
            }
            consoleLog(code >= 500 ? LogLevel::Error : LogLevel::Warning,
                       "HTTP failed " + request.method + " " + request.path +
                           " status=" + std::to_string(code) + " reason=" + message);
            response.status = code;
            response.set_header("Cache-Control", "no-store");
            response.set_content(Json{{"code", code}, {"message", message}}.dump(),
                                 "application/json; charset=utf-8");
        });
    server_->set_error_handler([](const httplib::Request &, httplib::Response &response) {
        if (response.body.empty())
            response.set_content(
                Json{{"code", response.status}, {"message", "请求失败或本版本未接入该操作"}}.dump(),
                "application/json; charset=utf-8");
    });

    standardRoutes();
    // 完整浏览器的登录入口保持原样；门户专用路径始终展示浏览器引导。
    server_->Get("/portal", [this](const auto &request, auto &response) {
        portalResponse(request, response);
    });
    server_->Get("/portal-config.json", [this](const auto &request, auto &response) {
        portalResponse(request, response);
    });
    // 公开接口只提供设备展示信息、业务健康状态和门户发现信息。
    server_->Get("/api/v1/device/info", [this](const auto &, auto &response) {
        result(response, rpc_.call("device.info"));
    });
    server_->Post("/api/v1/device/wifi",
                  [this](const httplib::Request &request,
                         httplib::Response &response,
                         const httplib::ContentReader &reader) {
                      result(response,
                             rpc_.call("device.wifi.save",
                                       readJson(request, reader),
                                       session(request).owner));
                  });
    server_->Get("/api/v1/health", [this](const auto &, auto &response) {
        result(response, rpc_.call("health"));
    });
    server_->Get("/api/v1/captive", [this](const auto &, auto &response) {
        response.set_content(
            Json{{"captive", false}, {"user-portal-url", config_.origin + "/portal"}}.dump(),
            "application/captive+json");
    });

    // 认证成功后由 daemon 创建 HttpOnly Cookie 会话，Qt 只校验账号。
    server_->Post("/api/v1/auth/login",
                  [this](const httplib::Request &request,
                         httplib::Response &response,
                         const httplib::ContentReader &reader) {
                      sessions_.attempt(request.remote_addr);
                      const auto user = rpc_.call("auth.login", readJson(request, reader));
                      const auto created = sessions_.create(user.at("id"), user.at("username"));
                      response.set_header("Set-Cookie",
                                          "device_session=" + created.token +
                                              "; Path=/; HttpOnly; SameSite=Strict" +
                                              (config_.developmentHttp ? "" : "; Secure"));
                      result(response, {{"username", created.username}, {"csrf", created.csrf}});
                  });
    server_->Get("/api/v1/auth/session", [this](const auto &request, auto &response) {
        const auto user = session(request);
        result(response, {{"username", user.username}, {"csrf", user.csrf}});
    });
    server_->Post(
        "/api/v1/auth/logout",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            readJson(request, reader);
            sessions_.erase(session(request).token);
            response.set_header(
                "Set-Cookie",
                std::string("device_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0") +
                    (config_.developmentHttp ? "" : "; Secure"));
            result(response, Json::object());
        });

    // 上传任务先在 Qt 登记，再由 daemon 接收原始文件字节。
    server_->Post(
        "/api/v1/uploads",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            auto data =
                rpc_.call("uploads.create", readJson(request, reader), session(request).owner);
            data.erase("part_path");
            result(response, data);
        });
    server_->Put(R"(/api/v1/uploads/([a-f0-9-]{36})/content)",
                 [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
                     upload(request, response, reader);
                 });
    server_->Get(R"(/api/v1/uploads/([a-f0-9-]{36}))", [this](const auto &request, auto &response) {
        result(response,
               rpc_.call("uploads.status",
                         {{"id", request.matches[1].str()}},
                         session(request).owner));
    });
    for (const std::string action : {"commit", "cancel"})
        server_->Post("/api/v1/uploads/([a-f0-9-]{36})/" + action,
                      [this, action](const auto &request,
                                     auto &response,
                                     const httplib::ContentReader &reader) {
                          readJson(request, reader);
                          std::lock_guard<std::mutex> lock(uploadsMutex_);
                          if (uploads_.count(request.matches[1].str()))
                              throw ApiError(409, "文件正在传输，请等待连接结束");
                          result(response,
                                 rpc_.call("uploads." + action,
                                           {{"id", request.matches[1].str()}},
                                           session(request).owner));
                      });

    // 文件与人员读取仍由 Qt 的统一数据库和设备目录决定。
    server_->Get("/api/v1/files", [this](const auto &request, auto &response) {
        result(response, rpc_.call("files.list", Json::object(), session(request).owner));
    });
    server_->Get("/api/v1/persons", [this](const auto &request, auto &response) {
        result(response, rpc_.call("persons.list", Json::object(), session(request).owner));
    });
    server_->Get(R"(/api/v1/persons/([0-9]+))", [this](const auto &request, auto &response) {
        result(
            response,
            rpc_.call("persons.get", {{"id", request.matches[1].str()}}, session(request).owner));
    });
    server_->Get(R"(/api/v1/files/([a-f0-9-]{36}|[a-f0-9]{64})/content)",
                 [this](const auto &request, auto &response) {
                     download(request, response);
                 });
    server_->Delete(
        R"(/api/v1/files/([a-f0-9-]{36}|[a-f0-9]{64}))",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            readJson(request, reader);
            const std::string id = request.matches[1];
            std::lock_guard<std::mutex> lock(downloadsMutex_);
            if (downloads_.count(id))
                throw ApiError(423, "文件正在下载，请稍后重试");
            result(response, rpc_.call("files.delete", {{"id", id}}, session(request).owner));
        });

    // 人员导入仅接受批次清单、状态查询、启动与丢弃四类操作。
    server_->Post(
        "/api/v1/imports",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            result(response,
                   rpc_.call("imports.create", readJson(request, reader), session(request).owner));
        });
    server_->Get(R"(/api/v1/imports/([a-f0-9-]{36}))", [this](const auto &request, auto &response) {
        result(response,
               rpc_.call("imports.status",
                         {{"id", request.matches[1].str()}},
                         session(request).owner));
    });
    server_->Post(
        R"(/api/v1/imports/([a-f0-9-]{36})/start)",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            readJson(request, reader);
            result(response,
                   rpc_.call("imports.start",
                             {{"id", request.matches[1].str()}},
                             session(request).owner));
        });
    server_->Post(
        R"(/api/v1/imports/([a-f0-9-]{36})/discard)",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            readJson(request, reader);
            result(response,
                   rpc_.call("imports.discard",
                             {{"id", request.matches[1].str()}},
                             session(request).owner));
        });
}

/**
 * @brief 流式接收上传内容并与 Qt 中的上传任务协调。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param response 待写入的 HTTP 响应。
 * @param reader 流式请求体读取器。
 */
void WebServer::upload(const httplib::Request &request,
                       httplib::Response &response,
                       const httplib::ContentReader &reader)
{
#ifdef _WIN32
    (void)request;
    (void)response;
    (void)reader;
    throw ApiError(503, "上传仅在 Linux 板端提供");
#else
    const auto user = session(request);
    const std::string id = request.matches[1];
    if (request.get_header_value("Content-Type") != "application/octet-stream" ||
        request.has_header("Transfer-Encoding"))
        throw ApiError(400, "请上传完整原始文件并提供长度");

    // 在接收首字节前占用任务，避免并发写入和取消同一任务。
    {
        std::lock_guard<std::mutex> lock(uploadsMutex_);
        if (!uploads_.insert(id).second)
            throw ApiError(409, "此任务正在接收，不能重复上传");
    }

    struct ReceivingLease {
        WebServer *server;
        std::string id;

        /** @brief 释放当前任务的接收占用标记。 */
        ~ReceivingLease()
        {
            std::lock_guard<std::mutex> lock(server->uploadsMutex_);
            server->uploads_.erase(id);
        }
    } lease{this, id};

    // 临时文件使用独占创建和文件锁，任何请求路径都不来自客户端文件名。
    const auto started = rpc_.call("uploads.begin", {{"id", id}}, user.owner);
    const auto expected = started.at("size").get<uint64_t>();

    struct Descriptor {
        int fd = -1;

        /** @brief 在异常路径自动关闭临时文件描述符。 */
        ~Descriptor()
        {
            if (fd >= 0)
                ::close(fd);
        }
    } output;

    try {
        if (!request.has_header("Content-Length") ||
            request.get_header_value_u64("Content-Length") != expected)
            throw ApiError(400, "文件长度与任务声明不一致");
        const std::string path = started.at("part_path");
        output.fd =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0640);
        if (output.fd < 0 || ::flock(output.fd, LOCK_EX | LOCK_NB) != 0)
            throw ApiError(409, "无法创建临时文件，任务可能正在接收");
        uint64_t received = 0, checkpoint = 0;
        const auto began = std::chrono::steady_clock::now();
        auto lastSessionCheck = began;
        auto lastProgress = began;

        // 流式写入时同时约束声明长度、总耗时和会话有效期。
        const bool complete = reader([&](const char *data, size_t size) {
            const auto now = std::chrono::steady_clock::now();
            if (received + size > expected || now - began > std::chrono::hours(2))
                return false;

            // 实际传输是活动操作，每分钟刷新空闲期限；绝对期限/注销仍有效。
            if (now - lastSessionCheck >= std::chrono::minutes(1)) {
                sessions_.get(user.token);
                lastSessionCheck = now;
            }
            size_t offset = 0;
            while (offset < size) {
                const auto written = ::write(output.fd, data + offset, size - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    return false;
                offset += static_cast<size_t>(written);
            }
            received += size;
            if (received - checkpoint >= 16 * 1024 * 1024 ||
                now - lastProgress >= std::chrono::seconds(1)) {
                rpc_.call("uploads.progress", {{"id", id}}, user.owner);
                checkpoint = received;
                lastProgress = now;
            }
            return true;
        });

        // 完整写入并同步到磁盘后，才通知 Qt 进入待校验状态。
        if (!complete || received != expected)
            throw ApiError(400, "上传中断或数据长度不完整");
        if (::fsync(output.fd) != 0)
            throw ApiError(507, "存储同步失败");
        ::close(output.fd);
        output.fd = -1;
        result(response, rpc_.call("uploads.received", {{"id", id}}, user.owner));
    } catch (...) {
        // 保留失败任务状态供客户端核对；Qt 不可用时由重启恢复流程处理。
        if (output.fd >= 0) {
            ::close(output.fd);
            output.fd = -1;
        }
        try {
            rpc_.call("uploads.failed", {{"id", id}}, user.owner);
        } catch (...) { /* Qt 失联时保留临时文件，由 Qt 重启恢复流程核对。 */
        }
        throw;
    }
#endif
}

/**
 * @brief 按文件租约分块发送视频内容。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param response 待写入的 HTTP 响应。
 */
void WebServer::download(const httplib::Request &request, httplib::Response &response)
{
    // 持有下载计数期间禁止删除同一文件。
    const auto user = session(request);
    const std::string id = request.matches[1];
    std::lock_guard<std::mutex> lock(downloadsMutex_);
    const auto data = rpc_.call("files.get", {{"id", id}}, user.owner);
    const std::string path = data.at("path");
    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*file)
        throw ApiError(404, "文件读取失败");
    const uint64_t size = data.at("bytes").get<uint64_t>();
    ++downloads_[id];

    // 固定下载文件名，不把原始中文/标点名字直接写进 HTTP header。
    response.set_header("Content-Disposition",
                        "attachment; filename=\"video" + path.substr(path.find_last_of('.')) +
                            "\"");

    // 按需读取文件块，并在长下载期间定期复核登录会话。
    response.set_content_provider(
        static_cast<size_t>(size),
        "application/octet-stream",
        [file, this, token = user.token, lastCheck = std::chrono::steady_clock::now()](
            size_t offset,
            size_t length,
            httplib::DataSink &sink) mutable {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCheck >= std::chrono::minutes(1)) {
                try {
                    sessions_.get(token);
                    lastCheck = now;
                } catch (const ApiError &) {
                    return false;
                }
            }
            std::array<char, 64 * 1024> buffer{};
            file->clear();
            file->seekg(static_cast<std::streamoff>(offset));
            const auto count = std::min(length, buffer.size());
            file->read(buffer.data(), static_cast<std::streamsize>(count));
            const auto actual = file->gcount();
            return actual > 0 && sink.write(buffer.data(), static_cast<size_t>(actual));
        },
        [this, id, file](bool) {
            file->close();
            std::lock_guard<std::mutex> guard(downloadsMutex_);
            if (--downloads_[id] == 0)
                downloads_.erase(id);
        });
}

/**
 * @brief 返回离线门户页面及配置，服务器地址只使用已校验部署配置。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param response 待写入的 HTTP 响应。
 */
void WebServer::portalResponse(const httplib::Request &request, httplib::Response &response)
{
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "no-referrer");
    response.set_header(
        "Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; "
        "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'");
    if (request.method != "GET" && request.method != "HEAD") {
        response.status = 405;
        response.set_header("Allow", "GET, HEAD");
        return;
    }
    if (request.path == "/portal-config.json") {
        response.set_content(Json{{"origin", config_.origin},
                                  {"loginUrl", config_.origin + "/#/login"},
                                  {"guideUrl", config_.origin + "/portal"}}
                                 .dump(),
                             "application/json; charset=utf-8");
        return;
    }

    // 无论手机探测的 Host/路径是什么，都只返回本机固定引导资源。
    const bool script = request.path == "/portal.js";
    const bool style = request.path == "/portal.css";
    const auto name = script ? "/portal.js" : style ? "/portal.css" : "/portal.html";
    std::ifstream file(config_.www + name, std::ios::binary);
    if (!file) {
        response.status = 503;
        response.set_content("浏览器引导资源缺失，请更新完整网页 dist。",
                             "text/plain; charset=utf-8");
        return;
    }
    const std::string body((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    response.set_content(body,
                         script  ? "application/javascript; charset=utf-8"
                         : style ? "text/css; charset=utf-8"
                                 : "text/html; charset=utf-8");
}

/**
 * @brief 启动 HTTP 浏览器引导入口与主服务监听。
 * @return 监听正常结束时的结果；监听建立失败返回 false。
 */
bool WebServer::run()
{
    consoleLog(LogLevel::Info, "Web directory: " + config_.www);
    consoleLog(LogLevel::Info, "Qt socket: " + config_.socket);
    // 自动门户停留在引导页；用户点击后请求完整浏览器访问 HTTPS 管理地址。
    std::thread portalThread;
    if (config_.portalPort) {
        portal_.new_task_queue = []() {
            return new httplib::ThreadPool(2, 8);
        };
        portal_.set_payload_max_length(4096);
        portal_.set_logger(logRequest);
        portal_.set_pre_routing_handler([this](const auto &request, auto &response) {
            beginRequest(request);
            portalResponse(request, response);
            return httplib::Server::HandlerResponse::Handled;
        });
        if (!portal_.bind_to_port(config_.bind, config_.portalPort)) {
            const auto error = socketError();
            consoleLog(LogLevel::Error,
                       "HTTP portal bind failed: " + config_.bind + ":" +
                           std::to_string(config_.portalPort) + error);
            return false;
        }
    }
    bool ok = false;
    try {
        if (!server_->bind_to_port(config_.bind, config_.port)) {
            const auto error = socketError();
            consoleLog(LogLevel::Error,
                       "Main listener bind failed: " + config_.bind + ":" +
                           std::to_string(config_.port) + error);
        } else {
            if (config_.portalPort) {
                portalThread = std::thread([this]() {
                    if (!portal_.listen_after_bind())
                        consoleLog(LogLevel::Error, "HTTP portal listener failed");
                });
                consoleLog(LogLevel::Info,
                           "HTTP portal listening: " + config_.bind + ":" +
                               std::to_string(config_.portalPort));
            }
            consoleLog(LogLevel::Info,
                       std::string(config_.developmentHttp ? "HTTP" : "HTTPS") +
                           " listening: " + config_.bind + ":" + std::to_string(config_.port));
            consoleLog(LogLevel::Info, "Portal guide URL: " + config_.origin + "/portal");
            ok = server_->listen_after_bind();
            if (!ok) {
                const auto error = socketError();
                consoleLog(LogLevel::Error,
                           "Main listener failed: " + config_.bind + ":" +
                               std::to_string(config_.port) + error);
            }
        }
    } catch (...) {
        portal_.stop();
        if (portalThread.joinable())
            portalThread.join();
        throw;
    }
    portal_.stop();
    if (portalThread.joinable())
        portalThread.join();
    return ok;
}

/**
 * @brief 停止入口与主服务监听。
 */
void WebServer::stop()
{
    // 主服务退出后由 run() 唯一负责停止/回收门户线程，避免两个线程重复 stop。
    server_->stop();
}
} // namespace deviceweb
