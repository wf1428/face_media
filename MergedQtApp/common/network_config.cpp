/**
 * @file network_config.cpp
 * @brief 网络配置源数据读取、IPv4 校验与原 INI 键持久化。
 * @author Du lin
 * @date 2026-10-20
 */

#include "network_config.h"
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSettings>

namespace {

/**
 * @brief 只接受点分十进制 IPv4，拒绝命令字符和含糊的数字地址。
 * @param value 待校验的点分十进制 IPv4 文本。
 * @param[out] number 可选输出，接收转换后的 IPv4 数值。
 * @return 输入为合法点分十进制 IPv4 时为 true。
 */
bool ipv4(const QString &value, quint32 *number = nullptr)
{
    static const QRegularExpression pattern("^(0|[1-9][0-9]{0,2})(\\.(0|[1-9][0-9]{0,2})){3}$");
    QHostAddress address;
    if (!pattern.match(value).hasMatch() || !address.setAddress(value) ||
        address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    if (number)
        *number = address.toIPv4Address();
    return true;
}

/**
 * @brief 统一填入调用方错误说明。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @param message 可返回调用方的结果或错误说明。
 * @return 始终返回 false，便于调用方直接返回失败。
 */
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

/**
 * @brief 每次请求重新读 INI，刷新页面可以看到 Qt 或 DHCP 更新后的保存值。
 * @param path 待访问的文件或配置路径。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 保存的网络配置；文件缺失或读取失败返回空对象。
 */
QJsonObject NetworkConfig::read(const QString &path, QString *error)
{
    if (error)
        error->clear();
    if (!QFileInfo(path).isFile()) {
        fail(error, QStringLiteral("网络配置文件不存在"));
        return {};
    }
    QSettings ini(path, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    ini.sync();
    QJsonObject result{{"iface", ini.value("network/iface", "eth0").toString().trimmed()},
                       {"dhcp", ini.value("network/dhcp", true).toBool()},
                       {"useConfig", ini.value("network/use_config", false).toBool()},
                       {"autoConnect", ini.value("network/auto_connect", false).toBool()}};
    if (result.value("iface").toString().isEmpty())
        result["iface"] = "eth0";
    for (const auto &key : {"ip", "netmask", "gateway", "dns"})
        result[key] = ini.value("network/" + QString::fromLatin1(key)).toString().trimmed();
    if (ini.status() != QSettings::NoError) {
        fail(error, QStringLiteral("网络配置文件读取失败"));
        return {};
    }
    return result;
}

/**
 * @brief 网络应用前校验全部字段，避免修改系统地址后才发现输入无效。
 * @param input 待校验的输入数据。
 * @param current 当前保存配置，提供受控网卡等字段。
 * @param[out] output 接收规范化结果，调用方应提供有效指针。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 全部字段合法时为 true；失败时为 false 且写入错误说明。
 * @note 网卡名取自 current；DHCP 模式清空静态字段，静态模式校验掩码、网关及 DNS。
 */
bool NetworkConfig::normalize(const QJsonObject &input,
                              const QJsonObject &current,
                              QJsonObject *output,
                              QString *error)
{
    if (!input.value("dhcp").isBool())
        return fail(error, QStringLiteral("请选择 DHCP 或静态 IP"));
    const auto iface = current.value("iface").toString();
    if (!QRegularExpression("^[a-zA-Z0-9_.-]{1,15}$").match(iface).hasMatch() || iface == "lo")
        return fail(error, QStringLiteral("配置中的有线网卡名称无效"));
    QJsonObject value{{"iface", iface},
                      {"dhcp", input.value("dhcp")},
                      {"useConfig", true},
                      {"autoConnect", true}};
    for (const auto &key : {"ip", "netmask", "gateway", "dns"}) {
        if (input.contains(key) && !input.value(key).isString())
            return fail(error, QStringLiteral("网络参数必须为文本"));
        value[key] =
            input.value("dhcp").toBool() ? QString() : input.value(key).toString().trimmed();
        if (value.value(key).toString().size() > 100)
            return fail(error, QStringLiteral("网络参数过长"));
    }
    if (!value.value("dhcp").toBool()) {
        quint32 ip = 0, mask = 0;
        if (!ipv4(value.value("ip").toString(), &ip) || (ip >> 24) == 0 || (ip >> 24) == 127 ||
            (ip >> 24) >= 224)
            return fail(error, QStringLiteral("IP 地址不合法"));
        if ((ip & 0xffffff00U) == 0xc0a80a00U)
            return fail(error, QStringLiteral("有线 IP 不能使用热点网段 192.168.10.0/24"));
        if (!ipv4(value.value("netmask").toString(), &mask) || mask == 0 ||
            ((~mask) & ((~mask) + 1)) != 0)
            return fail(error, QStringLiteral("子网掩码必须连续，例如 255.255.255.0"));
        const auto host = ip & ~mask;
        if (~mask > 1 && (host == 0 || host == ~mask))
            return fail(error, QStringLiteral("IP 不能是该子网的网络地址或广播地址"));
        const auto gateway = value.value("gateway").toString();
        quint32 gw = 0;
        if (!gateway.isEmpty() && (!ipv4(gateway, &gw) || gw == ip || (gw & mask) != (ip & mask) ||
                                   (~mask > 1 && ((gw & ~mask) == 0 || (gw & ~mask) == ~mask))))
            return fail(error, QStringLiteral("网关必须是同一子网中的有效地址"));
        const auto dns =
            value.value("dns").toString().split(QRegularExpression("[ ,]+"), QString::SkipEmptyParts);
        if (dns.size() > 4)
            return fail(error, QStringLiteral("DNS 最多填写四个 IPv4 地址"));
        for (const auto &server : dns)
            if (!ipv4(server))
                return fail(error, QStringLiteral("DNS 地址不合法，多个地址用空格分隔"));
        value["dns"] = dns.join(' ');
    }
    *output = value;
    return true;
}

/**
 * @brief 写入与后台一致的网络键；QSettings 合并保留其他组的配置。
 * @param path 待访问的文件或配置路径。
 * @param value 已通过统一校验的网络配置，包含 iface、dhcp 和静态地址字段。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return INI 同步成功时为 true；失败返回 false 并写入错误说明。
 */
bool NetworkConfig::save(const QString &path, const QJsonObject &value, QString *error)
{
    QSettings ini(path, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    ini.setValue("network/use_config", true);
    ini.setValue("network/auto_connect", true);
    // 仅 Web 成功设置后启用；重启时保留显式网络配置，不由 offline 模式清空。
    ini.setValue("network/web_managed", true);
    ini.setValue("network/iface", value.value("iface").toString());
    ini.setValue("network/dhcp", value.value("dhcp").toBool());
    for (const auto &key : {"ip", "netmask", "gateway", "dns"})
        ini.setValue("network/" + QString::fromLatin1(key), value.value(key).toString());
    ini.sync();
    return ini.status() == QSettings::NoError ||
           fail(error, QStringLiteral("网络已应用，但写入 net_cfg.ini 失败，请检查配置文件权限"));
}
