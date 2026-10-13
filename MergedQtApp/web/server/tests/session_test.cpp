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
namespace {
/** @brief 显式断言，不受 NDEBUG 影响。 */
void check(bool condition,const char *message) { if (!condition) throw std::runtime_error(message); }
/** @brief 检查操作确实被指定业务状态拒绝。 */
void rejects(int status,const std::function<void()> &action) {
    try { action(); } catch (const deviceweb::ApiError &e) { check(e.status==status,"unexpected status"); return; }
    throw std::runtime_error("operation unexpectedly accepted");
}
}
int main() {
    try {
        using namespace deviceweb;
        check(sessionCookie("other=x; device_session=abcd; foo=y")=="abcd","cookie parse");
        check(sessionCookie("device_session=one; device_session=two").empty(),"duplicate cookie accepted");
        check(sessionCookie("device_session=; device_session=two").empty(),"empty duplicate cookie accepted");
        check(sessionCookie("evil_device_session=x").empty(),"substring cookie accepted");
        check(secureEqual("abcd","abcd") && !secureEqual("abcd","abce") && !secureEqual("abc","abcd"),"token comparison");
        SessionStore store;
        rejects(401,[&]() { store.get("missing"); });
        for (int i=0;i<5;++i) store.attempt("10.1.0.2");
        rejects(429,[&]() { store.attempt("10.1.0.2"); });
#ifndef _WIN32
        const auto session=store.create(7,"factory");
        check(session.token.size()==64 && session.csrf.size()==64 && session.token!=session.csrf,"random token");
        check(store.get(session.token).owner==7,"session owner");
        store.erase(session.token); rejects(401,[&]() { store.get(session.token); });
        for (int i=0;i<64;++i) store.create(i+1,"user");
        rejects(429,[&]() { store.create(65,"overflow"); });
#endif
        std::cout<<"deviceweb unit checks passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
