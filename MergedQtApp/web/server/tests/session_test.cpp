/**
 * @file session_test.cpp
 * @brief 有界会话与 Cookie/来源配置测试，失败返回非零，不依赖 Qt/设备 SDK。
 * @author Du lin
 * @date 2026-10-09
 */

#include "session_store.h"
#include "web_server.h"
#include <iostream>
#include <functional>
#include <algorithm>
#include <thread>
#include <chrono>

namespace {

/** @brief 显式断言，不受 NDEBUG 影响。 */
void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

/** @brief 检查操作确实被指定业务状态拒绝。 */
void rejects(int status, const std::function<void()> &action)
{
    try {
        action();
    } catch (const deviceweb::ApiError &e) {
        check(e.status == status, "unexpected status");
        return;
    }
    throw std::runtime_error("operation unexpectedly accepted");
}

/** @brief 同时预留两个本机随机端口，避免测试依赖固定端口或公网地址。 */
std::pair<int, int> availablePorts()
{
    struct Socket {
        decltype(::socket(AF_INET, SOCK_STREAM, 0)) value = ::socket(AF_INET, SOCK_STREAM, 0);

        /** @brief 释放仅供测试选端口使用的 socket。 */
        ~Socket()
        {
#ifdef _WIN32
            ::closesocket(value);
#else
            ::close(value);
#endif
        }
    } first, second;

    const auto reserve = [](const auto &socket) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(socket.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
              "could not reserve test port");
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        check(::getsockname(socket.value, reinterpret_cast<sockaddr *>(&address), &size) == 0,
              "could not inspect test port");
        return int(ntohs(address.sin_port));
    };
    return {reserve(first), reserve(second)};
}

/** @brief 通过实际 HTTP listener 验证探测请求停留在引导页且地址不可被请求覆盖。 */
void portalGuide()
{
    using namespace deviceweb;
    const auto ports = availablePorts();
    std::string source = __FILE__;
    std::replace(source.begin(), source.end(), '\\', '/');
    const auto root = source.rfind("/web/server/tests/");
    check(root != std::string::npos, "test source root unavailable");
    ServerConfig config;
    config.bind = "127.0.0.1";
    config.port = ports.first;
    config.portalPort = ports.second;
    config.origin = "http://127.0.0.1:" + std::to_string(config.port);
    config.www = source.substr(0, root) + "/web/frontend/public";
    config.socket = "/unused-portal-test/socket";
    // 仅本机单元测试直接构造配置；生产 load() 仍禁止明文业务服务及此组合。
    config.developmentHttp = true;
    WebServer service(config);
    bool listened = false;
    std::exception_ptr failure;
    std::thread listener([&]() {
        try {
            listened = service.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    try {
        httplib::Client portal(config.bind, config.portalPort);
        portal.set_connection_timeout(0, 100000);
        portal.set_read_timeout(2, 0);
        httplib::Result guide;
        for (int n = 0; n < 100; ++n) {
            guide = portal.Get("/generate_204", {{"Host", "connectivitycheck.example"}});
            if (guide)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(guide && guide->status == 200 && !guide->has_header("Location") &&
                  guide->body.find("open-browser") != std::string::npos,
              "portal probe redirected to login instead of browser guide");
        const auto destination = portal.Get("/portal-config.json?origin=https://untrusted.invalid");
        check(destination &&
                  Json::parse(destination->body).at("loginUrl") == config.origin + "/#/login",
              "portal destination was not taken from fixed configuration");
        for (const auto &asset : {"/portal.js", "/portal.css", "/hotspot-detect.html"}) {
            const auto response = portal.Get(asset);
            check(response && response->status == 200 && !response->body.empty(),
                  "offline portal asset unavailable");
        }
        const auto rejected = portal.Post("/generate_204", "unused", "text/plain");
        check(rejected && rejected->status == 405, "portal accepted a write request");
        httplib::Client main(config.bind, config.port);
        const auto discovery = main.Get("/api/v1/captive");
        check(discovery &&
                  Json::parse(discovery->body).at("user-portal-url") == config.origin + "/portal",
              "captive discovery did not point at guide");
    } catch (...) {
        service.stop();
        listener.join();
        throw;
    }
    service.stop();
    listener.join();
    if (failure)
        std::rethrow_exception(failure);
    check(listened, "portal listener failed");
}
} // namespace

/** @brief 执行 Web 模块回归测试。 */
int main()
{
    try {
        using namespace deviceweb;
        portalGuide();
        check(sessionCookie("other=x; device_session=abcd; foo=y") == "abcd", "cookie parse");
        check(sessionCookie("device_session=one; device_session=two").empty(),
              "duplicate cookie accepted");
        check(sessionCookie("device_session=; device_session=two").empty(),
              "empty duplicate cookie accepted");
        check(sessionCookie("evil_device_session=x").empty(), "substring cookie accepted");
        check(secureEqual("abcd", "abcd") && !secureEqual("abcd", "abce") &&
                  !secureEqual("abc", "abcd"),
              "token comparison");
        SessionStore store;
        rejects(401, [&]() {
            store.get("missing");
        });
        for (int i = 0; i < 5; ++i)
            store.attempt("10.1.0.2");
        rejects(429, [&]() {
            store.attempt("10.1.0.2");
        });
#ifndef _WIN32
        const auto session = store.create(7, "factory");
        check(session.token.size() == 64 && session.csrf.size() == 64 &&
                  session.token != session.csrf,
              "random token");
        check(store.get(session.token).owner == 7, "session owner");
        store.erase(session.token);
        rejects(401, [&]() {
            store.get(session.token);
        });
        const auto first = store.create(7, "factory"), second = store.create(7, "factory"),
                   other = store.create(8, "other");
        store.eraseOwner(7);
        rejects(401, [&]() {
            store.get(first.token);
        });
        rejects(401, [&]() {
            store.get(second.token);
        });
        check(store.get(other.token).owner == 8, "password change invalidated another account");
        store.erase(other.token);
        for (int i = 0; i < 64; ++i)
            store.create(i + 1, "user");
        rejects(429, [&]() {
            store.create(65, "overflow");
        });
#endif
        std::cout << "deviceweb unit checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
