/**
 * @file network_config.h
 * @brief 有线 IPv4 配置的 INI 读写和参数校验，沿用后台 network 配置键。
 * @author Du lin
 * @date 2026-10-20
 */

#pragma once

#include <QJsonObject>
#include <QString>

namespace NetworkConfig {

/** @brief 读取 net_cfg.ini 保存值；不以运行时地址替换配置。 */
QJsonObject read(const QString &path, QString *error);

/** @brief 校验 DHCP/静态输入，目标网卡仅取配置，不接受浏览器指定。 */
bool normalize(const QJsonObject &input,
               const QJsonObject &current,
               QJsonObject *output,
               QString *error);

/** @brief 仅更新 network 组，保留 MQTT、FTP 及其他原有配置。 */
bool save(const QString &path, const QJsonObject &value, QString *error);

} // namespace NetworkConfig
