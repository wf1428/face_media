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

/**
 * @brief 读取 net_cfg.ini 保存值；不以运行时地址替换配置。
 * @param path 待访问的文件或配置路径。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 保存的网络配置；文件缺失或读取失败返回空对象。
 */
QJsonObject read(const QString &path, QString *error);

/**
 * @brief 校验 DHCP/静态输入，目标网卡仅取配置，不接受浏览器指定。
 * @param input 待校验的输入数据。
 * @param current 当前保存配置，提供受控网卡等字段。
 * @param[out] output 接收规范化结果，调用方应提供有效指针。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 全部字段合法时为 true；失败时为 false 且写入错误说明。
 * @note 网卡名取自 current；DHCP 模式清空静态字段，静态模式校验掩码、网关及 DNS。
 */
bool normalize(const QJsonObject &input,
               const QJsonObject &current,
               QJsonObject *output,
               QString *error);

/**
 * @brief 仅更新 network 组，保留 MQTT、FTP 及其他原有配置。
 * @param path 待访问的文件或配置路径。
 * @param value 已通过统一校验的网络配置，包含 iface、dhcp 和静态地址字段。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return INI 同步成功时为 true；失败返回 false 并写入错误说明。
 */
bool save(const QString &path, const QJsonObject &value, QString *error);

} // namespace NetworkConfig
