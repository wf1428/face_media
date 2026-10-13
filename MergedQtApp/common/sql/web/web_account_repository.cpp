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

namespace {
/** @return 两个等长摘要内容相同；循环不会因首个不同字节提前退出。 */
bool constantEqual(const QByteArray &left, const QByteArray &right) {
    if (left.size() != right.size()) return false;
    unsigned int difference = 0;
    for (int i = 0; i < left.size(); ++i) difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return difference == 0;
}
/** @return 与出厂工具 hashlib.pbkdf2_hmac('sha256', ..., 32) 相同的单块派生摘要。 */
QByteArray passwordDigest(const QByteArray &password, const QByteArray &salt, int iterations) {
    // 本协议固定输出 32 字节，SHA-256 只需计算 PBKDF2 的第一个块。
    QByteArray block = salt;
    block.append("\0\0\0\1", 4);
    QByteArray previous = QMessageAuthenticationCode::hash(block, password, QCryptographicHash::Sha256);
    QByteArray digest = previous;
    for (int round = 1; round < iterations; ++round) {
        previous = QMessageAuthenticationCode::hash(previous, password, QCryptographicHash::Sha256);
        for (int index = 0; index < digest.size(); ++index)
            digest[index] = static_cast<char>(static_cast<unsigned char>(digest.at(index)) ^
                                              static_cast<unsigned char>(previous.at(index)));
    }
    return digest;
}
}

QJsonObject WebAccounts::authenticate(const QString &username, const QString &password)
{
    if (username.size() > 80 || password.size() > 256) throw WebError(401, QStringLiteral("用户名或密码错误"));
    const auto rows = WebSql::query(QStringLiteral("SELECT * FROM web_accounts WHERE username=? AND enabled=1"), {username});
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

void WebAccounts::provision(const QString &filePath)
{
    if (filePath.isEmpty() || !WebSql::query(QStringLiteral("SELECT id FROM web_accounts LIMIT 1")).isEmpty()) return;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) throw WebError(503, QStringLiteral("工厂 Web 账号文件无法读取"));
    const auto value = QJsonDocument::fromJson(file.readAll()).object();
    const QString username = value.value("username").toString().trimmed();
    const QByteArray salt = QByteArray::fromHex(value.value("salt_hex").toString().toLatin1());
    const QByteArray hash = QByteArray::fromHex(value.value("hash_hex").toString().toLatin1());
    const int iterations = value.value("iterations").toInt();
    if (username.isEmpty() || username.size() > 80 || salt.size() != 32 || hash.size() != 32 || iterations < 210000 || iterations > 1000000)
        throw WebError(503, QStringLiteral("工厂 Web 账号文件格式无效"));
    WebSql::execute(QStringLiteral("INSERT INTO web_accounts(username,salt,password_hash,iterations) VALUES(?,?,?,?)"), {username,salt,hash,iterations});
}
