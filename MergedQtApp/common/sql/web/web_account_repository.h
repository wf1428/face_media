/**
 * @file web_account_repository.h
 * @brief 出厂 Web 账号和密码校验；与原本机管理员配置分离。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include <QString>
#include <QJsonObject>

namespace WebAccounts {

/** @return 校验成功的账号信息；使用 PBKDF2-SHA256 和常量时间比较。 */
QJsonObject authenticate(const QString &username, const QString &password);

/** @brief 导入一次性工厂凭据文件，仅空账号库允许执行，文件不含明文密码。 */
void provision(const QString &filePath);

/** @brief 验证原口令，沿用账号 KDF 参数更新摘要，并要求重新登录。 */
QJsonObject changePassword(int owner, const QString &oldPassword, const QString &newPassword);
} // namespace WebAccounts
