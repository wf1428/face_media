/**
 * @file session_store.cpp
 * @brief 有界会话缓存与密码学随机令牌生成，无默认口令。
 * @author Du lin
 * @date 2026-10-09
 */

#include "session_store.h"
#include <array>
#include <fstream>
#ifdef DEVICEWEB_TLS
#include <openssl/rand.h>
#endif

namespace deviceweb {

/** @brief 逐字节比较令牌，避免因首个不同字节泄露比较进度。 */
bool secureEqual(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    unsigned difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

/** @brief 从系统随机源生成会话与 CSRF 令牌。 */
std::string SessionStore::randomToken()
{
    std::array<unsigned char, 32> bytes{};
#ifdef DEVICEWEB_TLS
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw ApiError(503, "安全随机源不可用");
#else
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw ApiError(503, "安全随机源不可用");
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

/** @brief 移除过期会话和登录限流记录。 */
void SessionStore::purge()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();)
        it = it->second.expires <= now || it->second.absolute <= now ? sessions_.erase(it)
                                                                     : std::next(it);
    for (auto it = attempts_.begin(); it != attempts_.end();)
        it = it->second.until <= now ? attempts_.erase(it) : std::next(it);
}

/** @brief 为认证成功的账号建立受数量和期限约束的会话。 */
Session SessionStore::create(int owner, const std::string &username)
{
    std::lock_guard<std::mutex> lock(mutex_);
    purge();
    if (sessions_.size() >= 64)
        throw ApiError(429, "登录会话过多，请稍后重试");
    const auto now = std::chrono::steady_clock::now();
    Session session{owner,
                    username,
                    randomToken(),
                    randomToken(),
                    now + std::chrono::minutes(15),
                    now + std::chrono::hours(8)};
    sessions_.emplace(session.token, session);
    return session;
}

/** @brief 查找会话并在有效期内刷新空闲到期时间。 */
Session SessionStore::get(const std::string &token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    purge();
    const auto found = sessions_.find(token);
    if (found == sessions_.end())
        throw ApiError(401, "登录已过期，请重新登录");
    found->second.expires = std::chrono::steady_clock::now() + std::chrono::minutes(15);
    return found->second;
}

/** @brief 注销指定会话令牌。 */
void SessionStore::erase(const std::string &token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(token);
}

/** @brief 撤销指定账号的全部 Web 会话，保留其他账号会话。 */
void SessionStore::eraseOwner(int owner)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();)
        if (it->second.owner == owner)
            it = sessions_.erase(it);
        else
            ++it;
}

/** @brief 按客户端地址累计登录尝试并执行限流。 */
void SessionStore::attempt(const std::string &address)
{
    std::lock_guard<std::mutex> lock(mutex_);
    purge();
    if (!attempts_.count(address) && attempts_.size() >= 128)
        throw ApiError(429, "登录请求过多，请稍后重试");
    auto &attempt = attempts_[address];
    if (attempt.count == 0)
        attempt.until = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    if (++attempt.count > 5)
        throw ApiError(429, "登录尝试过于频繁，请一分钟后再试");
}

/** @brief 从 Cookie 头提取唯一的设备会话令牌。 */
std::string sessionCookie(const std::string &cookies)
{
    std::string token;
    size_t start = 0;
    bool found = false;
    while (start < cookies.size()) {
        const auto end = cookies.find(';', start);
        std::string item =
            cookies.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto first = item.find_first_not_of(" \t");
        if (first != std::string::npos)
            item.erase(0, first);
        if (item.rfind("device_session=", 0) == 0) {
            if (found)
                return {};
            found = true;
            token = item.substr(15);
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return token;
}
} // namespace deviceweb
