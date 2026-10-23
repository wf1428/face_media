/**
 * @file web_account_repository.cpp
 * @brief Web 账号独立 CRUD，空库工厂配置及加盐密码验证。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_account_repository.h"
#include "web_sql.h"
#include <QFile>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <cstring>

namespace {

/**
 * @brief 比较两个密码摘要，避免按首个不同字节提前退出。
 * @param left 第一个待比较的字节序列。
 * @param right 第二个待比较的字节序列。
 * @return 两个等长摘要内容相同；循环不会因首个不同字节提前退出。
 */
bool constantEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size())
        return false;
    unsigned int difference = 0;
    for (int i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return difference == 0;
}

/**
 * @brief 按工厂账号约定派生 32 字节 PBKDF2-HMAC-SHA256 摘要。
 * @param password 待验证的明文登录密码；仅用于摘要计算，不在结果中返回。
 * @param salt 密码派生使用的随机盐。
 * @param iterations 密码派生迭代次数。
 * @return 与出厂工具 hashlib.pbkdf2_hmac('sha256', ..., 32) 相同的单块派生摘要。
 */
QByteArray passwordDigest(const QByteArray &password, const QByteArray &salt, int iterations)
{
    // 本协议固定输出 32 字节，SHA-256 只需计算 PBKDF2 的第一个块。
    QByteArray block = salt;
    block.append("\0\0\0\1", 4);
    QByteArray previous =
        QMessageAuthenticationCode::hash(block, password, QCryptographicHash::Sha256);
    QByteArray digest = previous;
    for (int round = 1; round < iterations; ++round) {
        previous = QMessageAuthenticationCode::hash(previous, password, QCryptographicHash::Sha256);
        for (int index = 0; index < digest.size(); ++index)
            digest[index] = static_cast<char>(static_cast<unsigned char>(digest.at(index)) ^
                                              static_cast<unsigned char>(previous.at(index)));
    }
    return digest;
}
} // namespace

/**
 * @brief 验证出厂 Web 账号密码并返回会话身份。
 * @param username Web 登录账号。
 * @param password 待验证的明文登录密码；仅用于摘要计算，不在结果中返回。
 * @return 认证成功的账号信息；认证失败抛出 WebError。
 */
QJsonObject WebAccounts::authenticate(const QString &username, const QString &password)
{
    if (username.size() > 80 || password.size() > 256)
        throw WebError(401, QStringLiteral("用户名或密码错误"));
    const auto rows =
        WebSql::query(QStringLiteral("SELECT * FROM web_accounts WHERE username=? AND enabled=1"),
                      {username});

    // 未知账号同样执行 KDF，避免用户名存在性时间泄露。
    const QVariantMap row = rows.isEmpty() ? QVariantMap() : rows.first();
    const QByteArray salt = rows.isEmpty() ? QByteArray(32, '0') : row.value("salt").toByteArray();
    const int iterations = rows.isEmpty() ? 210000 : row.value("iterations").toInt();
    if (iterations < 210000 || iterations > 1000000 || salt.size() != 32)
        throw WebError(503, QStringLiteral("Web 账号配置无效"));
    const QByteArray hash = passwordDigest(password.toUtf8(), salt, iterations);
    if (rows.isEmpty() || !constantEqual(hash, row.value("password_hash").toByteArray()))
        throw WebError(401, QStringLiteral("用户名或密码错误"));
    return {{"id", row.value("id").toInt()}, {"username", username}};
}

/**
 * @brief 在空账号表中导入出厂摘要文件。
 * @param filePath 待读取的文件路径。
 */
void WebAccounts::provision(const QString &filePath)
{
    if (filePath.isEmpty() ||
        !WebSql::query(QStringLiteral("SELECT id FROM web_accounts LIMIT 1")).isEmpty())
        return;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        throw WebError(503, QStringLiteral("工厂 Web 账号文件无法读取"));
    const auto value = QJsonDocument::fromJson(file.readAll()).object();
    const QString username = value.value("username").toString().trimmed();
    const QByteArray salt = QByteArray::fromHex(value.value("salt_hex").toString().toLatin1());
    const QByteArray hash = QByteArray::fromHex(value.value("hash_hex").toString().toLatin1());
    const int iterations = value.value("iterations").toInt();
    if (username.isEmpty() || username.size() > 80 || salt.size() != 32 || hash.size() != 32 ||
        iterations < 210000 || iterations > 1000000)
        throw WebError(503, QStringLiteral("工厂 Web 账号文件格式无效"));
    WebSql::execute(
        QStringLiteral(
            "INSERT INTO web_accounts(username,salt,password_hash,iterations) VALUES(?,?,?,?)"),
        {username, salt, hash, iterations});
}

/**
 * @brief 验证原口令，沿用账号 KDF 参数更新摘要，并要求重新登录。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @param oldPassword 当前登录密码。
 * @param newPassword 待设置的新密码。
 * @return 密码修改结果；原密码错误或新密码不合法时抛出 WebError。
 */
QJsonObject
WebAccounts::changePassword(int owner, const QString &oldPassword, const QString &newPassword)
{
    if (newPassword.size() < 8 || newPassword.size() > 128)
        throw WebError(400, QStringLiteral("新密码需为8至128个字符"));
    const auto rows = WebSql::query(
        "SELECT username,password_hash,iterations FROM web_accounts WHERE id=? AND enabled=1",
        {owner});
    if (rows.isEmpty())
        throw WebError(401, QStringLiteral("账号不可用"));
    try {
        authenticate(rows.first().value("username").toString(), oldPassword);
    } catch (const WebError &error) {
        if (error.status == 401)
            throw WebError(400, QStringLiteral("原密码错误"));
        throw;
    }

    // 原密码验证通过后才生成新随机盐，并沿用账号已有迭代次数。
    QByteArray salt(32, '\0');
    for (int i = 0; i < salt.size(); i += 4) {
        const auto value = QRandomGenerator::system()->generate();
        std::memcpy(salt.data() + i, &value, 4);
    }
    const auto iterations = rows.first().value("iterations").toInt();
    const auto digest = passwordDigest(newPassword.toUtf8(), salt, iterations);
    WebSql::execute("UPDATE web_accounts SET salt=?,password_hash=?,iterations=? WHERE id=? AND "
                    "enabled=1 AND password_hash=?",
                    {salt, digest, iterations, owner, rows.first().value("password_hash")});
    if (WebSql::query("SELECT changes() AS n").first().value("n").toInt() != 1)
        throw WebError(409, QStringLiteral("账号口令已更新，请重新登录"));
    return {{"changed", true}, {"reloginRequired", true}};
}
