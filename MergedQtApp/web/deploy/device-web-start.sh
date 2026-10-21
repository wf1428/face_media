#!/bin/sh
# @file device-web-start.sh
# @brief 等待 Qt 身份稳定后启动可选热点与 Web 子进程，失败不阻塞 Qt。
# @author Du lin
# @date 2026-10-16

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "${PORTAL_ENV:-$SCRIPT_DIR/portal.env}"
NETWORK="$SCRIPT_DIR/portal-network.sh"
WEB_PID=
PREPARING=0

# @brief 按脚本来源输出一条控制台日志。
log() {
    printf '%s\n' "[device-web-start] $*"
}

# @brief 核对实际 Qt PID、Unix socket 和设备身份就绪文件。
qt_ready() {
    [ -S "$QT_SOCKET" ] && [ -f "$QT_READY_FILE" ] || return 1
    pid=$(sed -n '1p' "$QT_READY_FILE")
    case "$pid" in '' | *[!0-9]*) return 1 ;; esac
    [ "$pid" -gt 0 ] && kill -0 "$pid" 2> /dev/null || return 1
    identity=$(sed -n '2,3p' "$QT_READY_FILE")
    [ -n "$(sed -n '2p' "$QT_READY_FILE")" ] && [ -n "$(sed -n '3p' "$QT_READY_FILE")" ]
}

# @brief 退出时清理本服务的 Web 子进程和已准备的专用热点。
cleanup() {
    trap - EXIT HUP INT TERM
    if [ -n "$WEB_PID" ]; then
        kill "$WEB_PID" 2> /dev/null || true
        wait "$WEB_PID" 2> /dev/null || true
    fi
    if [ "$PREPARING" -eq 1 ]; then "$NETWORK" stop || true; fi
}
trap cleanup EXIT
trap 'exit 0' HUP INT TERM

# After=face-media only orders service startup. Wait for the actual Qt instance.
waited=0
stable=0
previous_pid=
previous_identity=
while [ "$waited" -lt "$QT_WAIT_SECONDS" ]; do
    if qt_ready; then
        if [ "$pid" = "$previous_pid" ] && [ "$identity" = "$previous_identity" ]; then
            stable=$((stable + 1))
        else
            stable=0
        fi
        previous_pid=$pid
        previous_identity=$identity
        [ "$stable" -ge "$QT_STABLE_SECONDS" ] && break
    else
        stable=0
        previous_pid=
        previous_identity=
    fi
    sleep 1
    waited=$((waited + 1))
done
if [ "$stable" -lt "$QT_STABLE_SECONDS" ]; then
    log 'Qt Web interface did not become stable before timeout; portal skipped'
    exit 0
fi
if ! "$NETWORK" detect > /dev/null; then
    log 'No usable WiFi hotspot interface; Web server not started'
    exit 0
fi
[ -x "$WEB_BINARY" ] || {
    log 'Web executable is unavailable'
    exit 1
}
PREPARING=1
"$NETWORK" prepare
"$WEB_BINARY" "$WEB_CONFIG" &
WEB_PID=$!
log "Web server started: pid=$WEB_PID"
while kill -0 "$WEB_PID" 2> /dev/null; do
    sleep 2
    if ! "$NETWORK" check && ! "$NETWORK" refreshing && ! "$NETWORK" check; then
        log 'Hotspot became unavailable; stopping only this optional portal'
        exit 1
    fi
done
set +e
wait "$WEB_PID"
status=$?
set -e
WEB_PID=
log "Web server exited: status=$status"
exit 1
