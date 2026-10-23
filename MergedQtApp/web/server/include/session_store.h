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
    int owner = 0;                                  /**< Qt Web 账号主键。 */
    std::string username;                           /**< 登录名。 */
    std::string token;                              /**< Cookie 随机值。 */
    std::string csrf;                               /**< 状态修改请求的随机 CSRF 值。 */
    std::chrono::steady_clock::time_point expires;  /**< 空闲有效期截止时刻。 */
    std::chrono::steady_clock::time_point absolute; /**< 最大有效期截止时刻。 */
};

/** @brief 固定上限、线程安全的会话与认证防暴力尝试状态。 */
class SessionStore {
public:

    /**
     * @brief 为通过认证的账号创建有限期 Cookie 会话。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @param username Web 登录账号。
     * @return 为校验成功账号创建的会话，最多同时 64 份。
     */
    Session create(int owner, const std::string &username);

    /**
     * @brief 校验会话有效期并续展其空闲截止时间。
     * @param token HTTP Cookie 中的随机会话令牌。
     * @return 校验并刷新空闲期的会话；过期或无效时报 401。
     */
    Session get(const std::string &token);

    /**
     * @brief 注销当前 Cookie 会话。
     * @param token HTTP Cookie 中的随机会话令牌。
     */
    void erase(const std::string &token);

    /**
     * @brief 撤销指定账号的全部 Web 会话，保留其他账号会话。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     */
    void eraseOwner(int owner);

    /**
     * @brief 每地址每分钟最多 5 次登录尝试，总桶数上限 128。
     * @param address 请求来源地址，作为登录速率限制键。
     */
    void attempt(const std::string &address);

    /**
     * @brief 从系统密码学随机源生成会话或 CSRF 令牌。
     * @return 使用系统密码学随机源生成的 256 位十六进制文本。
     */
    static std::string randomToken();

private:

    /**
     * @brief 删除已过期会话和登录桶。
     */
    void purge();
    std::mutex mutex_;                                  /**< 会话/速率表锁。 */
    std::unordered_map<std::string, Session> sessions_; /**< 会话随机值到身份。 */

    struct Attempts {
        int count = 0; /**< 当前地址在本时间窗内的登录次数。 */
        std::chrono::steady_clock::time_point until; /**< 当前登录计数时间窗的到期点。 */
    }; /**< 登录次数/到期时间。 */

    std::unordered_map<std::string, Attempts> attempts_; /**< 来源地址速率限制。 */
};

/**
 * @brief 从 Cookie 头提取唯一的设备会话令牌。
 * @param cookies HTTP Cookie 请求头。
 * @return 唯一匹配的会话值；缺失或重复时为空。
 */
std::string sessionCookie(const std::string &cookies);

/**
 * @brief 逐字节比较令牌，避免因首个不同字节泄露比较进度。
 * @param a 第一个待比较的令牌。
 * @param b 第二个待比较的令牌。
 * @return 两个令牌长度和所有字节均相同时为 true。
 */
bool secureEqual(const std::string &a, const std::string &b);
} // namespace deviceweb
