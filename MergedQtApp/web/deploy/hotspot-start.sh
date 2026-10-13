#!/bin/sh
# @file hotspot-start.sh
# @brief 只配置指定 AP 网卡、启动独立 hostapd/dnsmasq，不重启现有网络/Qt/MQTT。
# @author Du lin
# @date 2026-10-09
set -eu
: "${AP_INTERFACE:?AP_INTERFACE required}"
: "${AP_ADDRESS:?AP_ADDRESS required}"
: "${HOSTAPD_CONFIG:?HOSTAPD_CONFIG required}"
: "${DNSMASQ_CONFIG:?DNSMASQ_CONFIG required}"
# 必须完成出厂配置才启动，避免把占位密码当作实际 Wi-Fi 密码。
if grep -q '__FACTORY_' "$HOSTAPD_CONFIG"; then
    echo 'Factory AP password has not been provisioned' >&2
    exit 1
fi
case "$AP_INTERFACE" in wlan[0-9]|ap[0-9]) ;; *) echo 'Invalid dedicated AP interface' >&2; exit 1;; esac
ip link show dev "$AP_INTERFACE" >/dev/null
ip link set dev "$AP_INTERFACE" up
# 仅添加专用热点地址，不 flush 地址或路由，不改全局默认网关。
ip address replace "$AP_ADDRESS" dev "$AP_INTERFACE"
hostapd "$HOSTAPD_CONFIG" &
ap_pid=$!
dnsmasq --keep-in-foreground --conf-file="$DNSMASQ_CONFIG" &
dns_pid=$!
# systemd 对整个控制组清理；脚本正常退出时也移除自己添加的地址。
cleanup() {
    kill "$ap_pid" "$dns_pid" 2>/dev/null || true
    ip address del "$AP_ADDRESS" dev "$AP_INTERFACE" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' INT TERM
while kill -0 "$ap_pid" 2>/dev/null && kill -0 "$dns_pid" 2>/dev/null; do sleep 1; done
exit 1
