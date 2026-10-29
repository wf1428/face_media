/**
 * @file network_config.h
 * @brief 有线 IPv4 配置的 INI 读写和参数校验，沿用后台 network 配置键。
 * @author Du lin
 * @date 2026-10-20
 * @note 最近修改：2026-10-27；修改人：Du lin；声明独立链路模式校验与保存接口，缺省改为百兆，支持千兆并兼容旧 auto；原 IPv4 与 Web 意图接口保持不变。
 */

#pragma once

#include <QJsonObject>
#include <QString>

namespace NetworkConfig {

/**
 * @brief 读取 net_cfg.ini 保存值；不以运行时地址替换配置。
 * @param path 待访问的文件或配置路径。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 保存的网络配置及 linkMode（缺少键时缺省 100-full）；文件缺失或读取失败返回空对象。
 */
QJsonObject read(const QString &path, QString *error);

/**
 * @brief 校验 DHCP/静态输入，目标网卡仅取配置，不接受浏览器指定。
 * @param input 待校验的输入数据。
 * @param current 当前保存配置，提供受控网卡等字段。
 * @param[out] output 接收规范化结果，调用方应提供有效指针。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 全部字段合法时为 true；失败时为 false 且写入错误说明。
 * @note 网卡名取自 current；DHCP 模式清空静态字段；linkMode 支持 100-full/1000-full，兼容旧 auto，缺省保留当前值。静态模式校验掩码、网关及 DNS。
 */
bool normalize(const QJsonObject &input,
               const QJsonObject &current,
               QJsonObject *output,
               QString *error);

/**
 * @brief 仅更新 network 组，保留 MQTT、FTP 及其他原有配置。
 * @param path 待访问的文件或配置路径。
 * @param value 已通过统一校验的网络配置，包含 iface、dhcp 和静态地址字段；可选 linkMode 未提供时保留原值。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return INI 同步成功时为 true；失败返回 false 并写入错误说明。
 */
bool save(const QString &path, const QJsonObject &value, QString *error);

/** @brief 检查百兆/千兆模式，保留旧 auto 兼容值。
 *  @param mode 待检查的链路模式文本。
 *  @return 100-full、1000-full 或 auto 时为 true。 */
bool isLinkModeValid(const QString &mode);

/** @brief 仅更新 network/link_mode，不改变 IPv4、自动连接和 Web 联网意图。
 *  @param path 现有 net_cfg.ini 路径。
 *  @param iface 必须与当前保存的有线接口一致。
 *  @param mode 100-full、1000-full 或兼容的 auto。
 *  @param[out] error 可空失败说明。
 *  @return INI 同步成功时为 true，否则返回 false；不保证与 NM 跨文件原子提交。 */
bool saveLinkMode(const QString &path, const QString &iface, const QString &mode, QString *error);

} // namespace NetworkConfig
