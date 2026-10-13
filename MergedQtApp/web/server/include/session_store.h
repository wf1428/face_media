/**
 * @file session_store.h
 * @brief 内存 Cookie 会话、CSRF token 与登录速率限制；重启清空会话。
 * @author Du lin
 * @date 2026-10-09
 */
#pragma once
#include "rpc_client.h"
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace deviceweb {
/** @brief 会话副本；令牌只保留服务内存，不写前端 localStorage。 */
struct Session {
    int owner=0; /**< Qt Web 账号主键。 */
    std::string username; /**< 登录名。 */
    std::string token; /**< Cookie 随机值。 */
    std::string csrf; /**< 状态修改请求的随机 CSRF 值。 */
    std::chrono::steady_clock::time_point expires; /**< 空闲有效期截止时刻。 */
    std::chrono::steady_clock::time_point absolute; /**< 最大有效期截止时刻。 */
};
/** @brief 固定上限、线程安全的会话与认证防暴力尝试状态。 */
class SessionStore {
public:
    /** @return 为校验成功账号创建的会话，最多同时 64 份。 */
    Session create(int owner,const std::string &username);
    /** @return 校验并刷新空闲期的会话；过期或无效时报 401。 */
    Session get(const std::string &token);
    /** @brief 注销当前 Cookie 会话。 */
    void erase(const std::string &token);
    /** @brief 每地址每分钟最多 5 次登录尝试，总桶数上限 128。 */
    void attempt(const std::string &address);
    /** @return 使用系统密码学随机源生成的 256 位十六进制文本。 */
    static std::string randomToken();
private:
    /** @brief 删除已过期会话和登录桶。 */
    void purge();
    std::mutex mutex_; /**< 会话/速率表锁。 */
    std::unordered_map<std::string,Session> sessions_; /**< 会话随机值到身份。 */
    struct Attempts { int count=0; std::chrono::steady_clock::time_point until; }; /**< 登录次数/到期时间。 */
    std::unordered_map<std::string,Attempts> attempts_; /**< 来源地址速率限制。 */
};
/** @return Cookie 请求中精确匹配的会话值，拒绝重复 Cookie 名。 */
std::string sessionCookie(const std::string &cookies);
/** @return 常量时间比较令牌，避免普通字符串逐字比较。 */
bool secureEqual(const std::string &a,const std::string &b);
}
