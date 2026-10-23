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

/**
 * @brief 核验 Web 账号及密码摘要。
 * @param username Web 登录账号。
 * @param password 待验证的明文登录密码；仅用于摘要计算，不在结果中返回。
 * @return 校验成功的账号信息；使用 PBKDF2-SHA256 和常量时间比较。
 */
QJsonObject authenticate(const QString &username, const QString &password);

/**
 * @brief 导入一次性工厂凭据文件，仅空账号库允许执行，文件不含明文密码。
 * @param filePath 待读取的文件路径。
 */
void provision(const QString &filePath);

/**
 * @brief 验证原口令，沿用账号 KDF 参数更新摘要，并要求重新登录。
 * @param owner 已认证 Web 账号主键，用于任务归属校验。
 * @param oldPassword 当前登录密码。
 * @param newPassword 待设置的新密码。
 * @return 密码修改结果；原密码错误或新密码不合法时抛出 WebError。
 */
QJsonObject changePassword(int owner, const QString &oldPassword, const QString &newPassword);
} // namespace WebAccounts
