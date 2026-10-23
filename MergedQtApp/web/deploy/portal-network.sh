#!/bin/sh
# @file portal-network.sh
# @brief NetworkManager 专用热点的检测、配置、隔离、SSID 保存和重新激活。
# @author Du lin
# @date 2026-10-16

set -eu
export LC_ALL=C

# 脚本迁入 /usr/local/sbin，继续读取原配置并保留测试/部署覆盖入口。
. "${PORTAL_ENV:-/home/cat/web-stage/portal.env}"
: "${PORTAL_RUNTIME:=/run/device-webd}"
AP_IP=${AP_ADDRESS%/*}
RULE_TAG=yc-device-portal

# @brief 按脚本来源输出一条控制台日志。
# @param ... 日志文本，按原样连接后输出单行。
log() {
    printf '%s\n' "[portal-network] $*" >&2
}

# @brief 对 NetworkManager 命令设置统一等待与总超时上限。
# @param ... 传递给 nmcli 的独立参数。
# @return 固定超时内的 nmcli 退出码。
nm() {
    timeout 20 nmcli --wait 15 --escape no "$@"
}

# @brief 读取原始 UTF-8 SSID，避免 C 字符集将中文转换为问号。
# @return 成功时返回 0 并将原始 UTF-8 SSID 写到标准输出。
read_ssid() {
    LC_ALL=C.UTF-8 timeout 20 nmcli --wait 15 --escape no \
        -g 802-11-wireless.ssid connection show id "$AP_CONNECTION"
}

# @brief 校验 WPA-PSK 密码，空值只允许由调用方解释为保持原密码。
# @param password 第一个位置参数，待校验的 WPA-PSK 密码。
# @return 合法的 8~63 字节可打印密码或 64 位十六进制 PSK 返回 0。
validate_password() {
    case "$1" in *[!\ -~]*) return 1 ;; esac
    length=${#1}
    if [ "$length" -ge 8 ] && [ "$length" -le 63 ]; then
        return 0
    fi
    [ "$length" -eq 64 ] || return 1
    case "$1" in *[!0-9a-fA-F]*) return 1 ;; esac
}

# @brief 限制实际网卡名称的字符和长度，防止无效设备名进入命令。
# @param interface 第一个位置参数，候选网卡名。
# @return 字符与长度均合法时返回 0。
valid_interface() {
    case "$1" in '' | *[!a-zA-Z0-9_.-]*) return 1 ;; esac
    [ "${#1}" -le 15 ]
}

# @brief 读取本门户记录的实际网卡，并再次校验名称。
# @return 已记录网卡存在且名称合法时返回 0，并设置脚本内 iface。
read_interface() {
    [ -f "$PORTAL_RUNTIME/interface" ] || return 1
    IFS= read -r iface < "$PORTAL_RUNTIME/interface"
    valid_interface "$iface"
}

# @brief 检查配置的专用 NetworkManager 连接是否存在。
# @return 配置的专用连接存在时返回 0。
profile_exists() {
    nm -g connection.id connection show id "$AP_CONNECTION" > /dev/null 2>&1
}

# @brief 确认同名连接属于无线网络类型。
# @return 专用连接是 Wi-Fi 类型时返回 0。
profile_is_wifi() {
    [ "$(nm -g connection.type connection show id "$AP_CONNECTION" 2> /dev/null)" = 802-11-wireless ]
}

# @brief 核对专用连接及实际网卡是否已取得配置的热点地址。
# @return 专用连接已激活且包含目标热点地址时返回 0。
active() {
    read_interface || return 1
    [ "$(nm -g GENERAL.CONNECTION device show "$iface" 2> /dev/null)" = "$AP_CONNECTION" ] \
        && nm -g IP4.ADDRESS device show "$iface" 2> /dev/null | grep -Fxq "$AP_ADDRESS"
}

# @brief 核对所需命令和 NetworkManager 的运行状态。
tools() {
    for tool in nmcli iptables timeout ip flock; do
        command -v "$tool" > /dev/null 2>&1 || {
            log "Missing tool: $tool"
            return 1
        }
    done
    [ "$(nm -t -f RUNNING general 2> /dev/null)" = running ] || {
        log 'NetworkManager is not running'
        return 1
    }
}

# @brief 选择空闲、受管理且支持 AP 的实际无线接口。
# @return 找到可用网卡时返回 0 并输出名称；无可用网卡返回非 0。
detect() {
    tools || return 1
    [ "$(nm radio wifi 2> /dev/null)" = enabled ] || {
        log 'WiFi radio is disabled'
        return 1
    }
    candidates=$(nm -t -f DEVICE,TYPE device status | awk -F: '$2 == "wifi" {print $1}')
    for candidate in $candidates; do
        valid_interface "$candidate" || continue
        [ "$AP_INTERFACE" = auto ] || [ "$AP_INTERFACE" = "$candidate" ] || continue
        [ "$(nm -g WIFI-PROPERTIES.AP device show "$candidate" 2> /dev/null)" = yes ] || continue
        [ "$(nm -g GENERAL.NM-MANAGED device show "$candidate" 2> /dev/null)" = yes ] || continue
        current=$(nm -g GENERAL.CONNECTION device show "$candidate" 2> /dev/null || true)
        case "$current" in
            '' | '--') ;;
            "$AP_CONNECTION")
                [ "$(nm -g 802-11-wireless.mode connection show id "$AP_CONNECTION" 2> /dev/null)" = ap ] || {
                    log "Skipping non-AP connection on $candidate"
                    continue
                }
                ;;
            *)
                log "Skipping occupied WiFi interface $candidate"
                continue
                ;;
        esac
        printf '%s\n' "$candidate"
        return 0
    done
    log 'No idle, managed, AP-capable WiFi interface; portal skipped'
    return 1
}

# @brief 仅删除带本门户标记的转发规则，设置循环上限避免工具异常。
rule_remove() {
    # Exact, tagged rules only. Bounded to avoid loops on broken firewall tools.
    count=0
    while iptables -w 5 -C FORWARD -i "$iface" -m comment --comment "$RULE_TAG" -j DROP 2> /dev/null; do
        iptables -w 5 -D FORWARD -i "$iface" -m comment --comment "$RULE_TAG" -j DROP || return 1
        count=$((count + 1))
        [ "$count" -lt 16 ] || return 1
    done
}

# @brief 在 FORWARD 链首安装并验证热点客户端流量隔离规则。
isolate() {
    rule_remove
    iptables -w 5 -I FORWARD 1 -i "$iface" -m comment --comment "$RULE_TAG" -j DROP
    iptables -w 5 -C FORWARD -i "$iface" -m comment --comment "$RULE_TAG" -j DROP
}

# @brief 原子更新本门户专用的 DNS 重定向配置。
write_dns() {
    install -d -m 0755 "$(dirname -- "$PORTAL_DNS_CONFIG")"
    tmp="$PORTAL_DNS_CONFIG.tmp.$$"
    printf '# managed by device-webd\naddress=/#/%s\n' "$AP_IP" > "$tmp"
    chmod 0644 "$tmp"
    mv -f "$tmp" "$PORTAL_DNS_CONFIG"
}

# @brief 为直接调用脚本的路径校验 SSID 字节数与控制字符。
# @param ssid 第一个位置参数，待校验的 UTF-8 名称。
# @return 名称为 1~32 字节且无控制字符、首尾空格时返回 0。
validate_ssid() {
    [ -n "$1" ] && [ "$(printf '%s' "$1" | wc -c)" -le 32 ] || return 1
    # Qt also validates UTF-8 length and whitespace. This protects direct helper calls.
    [ "$(printf '%s' "$1" | tr -d '\000-\037\177')" = "$1" ] || return 1
    case "$1" in ' '* | *' ') return 1 ;; esac
}

# @brief 校验接口、网段和配置后建立专用共享热点，并落实隔离规则。
# @return 专用热点准备成功返回 0；无候选网卡返回 10，其余失败为非 0。
prepare() {
    iface=$(detect) || return 10
    install -d -m 0750 "$PORTAL_RUNTIME"
    # Do not collide with another process using our selected gateway subnet.
    conflicts=$(ip -o -4 address show | awk -v ip="$AP_IP" -v dev="$iface" '
        function number(s, a) {split(s,a,"."); return a[1]*16777216+a[2]*65536+a[3]*256+a[4]}
        $2!=dev {split($4,a,"/"); size=2^(32-a[2]);
          if (int(number(ip)/size)==int(number(a[1])/size) ||
              int(number(ip)/256)==int(number(a[1])/256)) print $2}')
    [ -z "$conflicts" ] || {
        log "Portal IP already used by $conflicts"
        return 1
    }
    if [ -s "$PORTAL_CUSTOM_SSID" ]; then
        IFS= read -r ssid < "$PORTAL_CUSTOM_SSID"
    else
        ssid=$(sed -n '3p' "$QT_READY_FILE")
    fi
    validate_ssid "$ssid" || {
        log 'Invalid or unavailable SSID'
        return 1
    }
    if profile_exists; then
        profile_is_wifi || {
            log 'Configured connection is not WiFi'
            return 1
        }
    else
        case "${AP_PASSWORD:-}" in '' | __FACTORY_*)
            log 'Provision the AP password before creating a new hotspot'
            return 1
            ;;
        esac
        [ "${#AP_PASSWORD}" -ge 8 ] && [ "${#AP_PASSWORD}" -le 63 ] || {
            log 'Invalid AP password length'
            return 1
        }
        nm connection add type wifi ifname "$iface" con-name "$AP_CONNECTION" autoconnect no ssid "$ssid" > /dev/null
        if ! nm connection modify id "$AP_CONNECTION" wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$AP_PASSWORD" > /dev/null 2>&1; then
            log 'Could not provision the new hotspot password'
            nm connection delete id "$AP_CONNECTION" > /dev/null 2>&1 || true
            return 1
        fi
    fi
    nm connection modify id "$AP_CONNECTION" connection.interface-name "$iface" connection.autoconnect no \
        802-11-wireless.mode ap 802-11-wireless.ssid "$ssid" ipv4.method shared \
        ipv4.addresses "$AP_ADDRESS" ipv4.never-default yes ipv6.method disabled > /dev/null
    printf '%s\n' "$iface" > "$PORTAL_RUNTIME/interface"
    write_dns
    # Apply before activation, then reassert after NM installs its sharing rules.
    isolate
    if [ "$(nm -g GENERAL.CONNECTION device show "$iface" 2> /dev/null || true)" = "$AP_CONNECTION" ]; then
        nm connection down id "$AP_CONNECTION" > /dev/null
    fi
    nm connection up id "$AP_CONNECTION" ifname "$iface" > /dev/null
    isolate
    active || {
        log 'Hotspot did not acquire the configured IP'
        return 1
    }
    log "Hotspot ready: interface=$iface address=$AP_ADDRESS"
    iptables -vnL FORWARD --line-numbers >&2
}

# @brief 保存 SSID 和可选新密码；持久化失败时恢复原连接设置。
# @param ssid 第一个位置参数，新热点名称；密码由标准输入传入。
# @return 配置及重载请求按既有脚本路径成功时返回 0。
save_ssid() {
    ssid=$1
    validate_ssid "$ssid" || {
        log 'Invalid SSID'
        return 1
    }
    password=
    if [ "${2:-}" = --password-stdin ]; then
        IFS= read -r password || {
            log 'Could not read hotspot password'
            return 1
        }
        validate_password "$password" || {
            log 'Invalid hotspot password'
            return 1
        }
    fi
    active || {
        log 'Hotspot is not active'
        return 1
    }
    previous=$(read_ssid)
    if [ -n "$password" ]; then
        # 回滚用的旧密钥只保留在本次进程内，不输出或另存明文文件。
        previous_password=$(nm --show-secrets -g 802-11-wireless-security.psk connection show id "$AP_CONNECTION")
        previous_flags=$(nm -g 802-11-wireless-security.psk-flags connection show id "$AP_CONNECTION")
        # 部分 NM 版本返回“0 (none)”；回滚时只使用原始标志数值。
        previous_flags=${previous_flags%% *}
        case "$previous_flags" in [0-7]) ;; *)
            log 'Could not read hotspot password flags'
            return 1
            ;;
        esac
        nm connection modify id "$AP_CONNECTION" 802-11-wireless.ssid "$ssid" \
            wifi-sec.psk "$password" wifi-sec.psk-flags 0 > /dev/null 2>&1 || {
            log 'Could not save hotspot settings'
            return 1
        }
    else
        nm connection modify id "$AP_CONNECTION" 802-11-wireless.ssid "$ssid" > /dev/null
    fi
    tmp="$PORTAL_CUSTOM_SSID.tmp.$$"
    if ! (
        umask 077
        printf '%s\n' "$ssid" > "$tmp" && mv -f "$tmp" "$PORTAL_CUSTOM_SSID"
    ); then
        rm -f "$tmp"
        if [ -n "$password" ]; then
            nm connection modify id "$AP_CONNECTION" 802-11-wireless.ssid "$previous" \
                wifi-sec.psk "$previous_password" wifi-sec.psk-flags "$previous_flags" > /dev/null 2>&1 || true
        else
            nm connection modify id "$AP_CONNECTION" 802-11-wireless.ssid "$previous" > /dev/null || true
        fi
        return 1
    fi
    log 'Hotspot settings saved; waiting for delayed activation'
}

# @brief 只重新激活专用热点连接，并保留 Qt 与 Web 进程。
refresh() {
    active || {
        log 'Hotspot is not active'
        return 1
    }
    printf '%s\n' "$$" > "$PORTAL_RUNTIME/refreshing"
    trap 'rm -f "$PORTAL_RUNTIME/refreshing"' EXIT
    trap 'exit 1' HUP INT TERM
    write_dns
    nm connection down id "$AP_CONNECTION" > /dev/null
    nm connection up id "$AP_CONNECTION" ifname "$iface" > /dev/null
    isolate
    active || return 1
    log 'Hotspot reactivated; Web/Qt processes were retained'
}

# @brief 清理本门户的活动连接、DNS 文件和带标记规则。
stop() {
    if read_interface; then
        if [ "$(nm -g GENERAL.CONNECTION device show "$iface" 2> /dev/null || true)" = "$AP_CONNECTION" ]; then
            nm connection down id "$AP_CONNECTION" > /dev/null 2>&1 || true
        fi
        rule_remove || true
        rm -f "$PORTAL_RUNTIME/interface"
    fi
    if [ -f "$PORTAL_DNS_CONFIG" ] && grep -Fqx '# managed by device-webd' "$PORTAL_DNS_CONFIG"; then
        rm -f "$PORTAL_DNS_CONFIG"
    fi
}

[ "$(id -u)" -eq 0 ] || {
    log 'Must run as root'
    exit 1
}
action=${1:-}
case "$action" in
    detect) detect ;;
    ssid) read_ssid ;;
    check) active ;;
    request-refresh)
        # ExecReload is managed in this service's cgroup, so stopping the portal
        # cannot leave a Qt-owned helper bringing the hotspot back afterwards.
        systemctl is-active --quiet device-webd.service || {
            log 'Portal service is not active'
            exit 1
        }
        exec systemctl reload device-webd.service
        ;;
    refreshing)
        [ -f "$PORTAL_RUNTIME/refreshing" ] || exit 1
        IFS= read -r pid < "$PORTAL_RUNTIME/refreshing"
        case "$pid" in '' | *[!0-9]*) exit 1 ;; esac
        [ "$pid" -gt 0 ] && kill -0 "$pid" 2> /dev/null
        ;;
    prepare | save | refresh | stop)
        install -d -m 0750 "$PORTAL_RUNTIME"
        exec 9> "$PORTAL_RUNTIME/network.lock"
        flock -w 5 9 || {
            log 'Hotspot update already in progress'
            exit 1
        }
        case "$action" in
            prepare) prepare ;;
            save)
                if [ "$#" -eq 2 ]; then
                    save_ssid "$2"
                elif [ "$#" -eq 3 ] && [ "$3" = --password-stdin ]; then
                    save_ssid "$2" "$3"
                else
                    exit 1
                fi
                ;;
            refresh) refresh ;;
            stop) stop ;;
        esac
        ;;
    *)
        log 'Usage: portal-network.sh detect|prepare|ssid|save SSID [--password-stdin]|request-refresh|refresh|check|refreshing|stop'
        exit 2
        ;;
esac
