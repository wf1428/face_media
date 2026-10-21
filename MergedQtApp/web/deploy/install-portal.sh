#!/bin/sh
# @file install-portal.sh
# @brief 安装并启用门户服务，保留原 Qt/MQTT 运行状态及现有证书。
# @author Du lin
# @date 2026-10-16

set -eu
[ "$(id -u)" -eq 0 ] || {
    echo 'Run as root' >&2
    exit 1
}
# 资源包由 Python 标准库安全解压，不依赖独立 unzip/tar 守护进程。
command -v python3 >/dev/null 2>&1 || {
    echo 'Python 3 is required for Ycest resource updates' >&2
    exit 1
}
SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
STAGE=/home/cat/web-stage
install -d -m 0750 "$STAGE"
install -m 0755 "$SOURCE/scripts/start_face_media.sh" /usr/local/sbin/start_face_media.sh
for script in device-web-start.sh portal-network.sh ycest-resource-update.py; do
    install -m 0755 "$SOURCE/web/deploy/$script" "$STAGE/$script"
done
if [ ! -f "$STAGE/portal.env" ]; then
    install -m 0600 "$SOURCE/web/deploy/portal.env" "$STAGE/portal.env"
fi
if [ ! -f "$STAGE/web.json" ]; then
    install -m 0640 "$SOURCE/web/deploy/web.json" "$STAGE/web.json"
else
    # Preserve the user's port, certificate paths, and every other deployment setting.
    sed 's/192\.168\.50\.1/192.168.10.1/g' "$STAGE/web.json" > "$STAGE/web.json.new"
    chmod 0640 "$STAGE/web.json.new"
    mv -f "$STAGE/web.json.new" "$STAGE/web.json"
fi
install -m 0644 "$SOURCE/web/deploy/device-webd.service" /etc/systemd/system/device-webd.service
systemctl disable --now device-hotspot.service 2> /dev/null || true
# Only explicitly retired portal-owned files are removed. No recursive cleanup.
rm -f /etc/systemd/system/device-hotspot.service
for old in device-hotspot.service hotspot-start.sh hotspot.env hostapd.conf dnsmasq.conf mergedqt-web.conf; do
    rm -f "$STAGE/$old"
done
# The old sample instructed users to install this exact Web drop-in.
rm -f /etc/systemd/system/face-media.service.d/web.conf
systemctl daemon-reload
systemctl enable face-media.service device-webd.service
printf '%s\n' 'Portal installed and enabled for boot. Set portal.env factory password or reuse its dedicated NM profile.'
printf '%s\n' 'Deploy the rebuilt Qt binary and complete dist; existing TLS files are untouched.'
printf '%s\n' 'Apply at the next planned Qt restart/reboot; this installer has not restarted Qt or MQTT.'
