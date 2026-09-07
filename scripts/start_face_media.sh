#!/bin/sh

set -eu

# systemd does not start this service through a login shell, so explicitly
# import the board-wide Qt/EGLFS environment configured in /etc/profile.
if [ -r /etc/profile ]; then
    set +u
    . /etc/profile
    set -u
fi

APP_DIR=/home/cat/face_media
APP_START_DELAY_SECONDS=${APP_START_DELAY_SECONDS:-5}

log()
{
    printf '%s\n' "[face-media] $*"
}

load_driver()
{
    driver_file=$1
    module_name=${driver_file%.ko}

    if [ -d "/sys/module/$module_name" ]; then
        log "$driver_file is already loaded"
        return 0
    fi

    if [ ! -f "$APP_DIR/$driver_file" ]; then
        log "driver not found: $APP_DIR/$driver_file"
        return 1
    fi

    log "loading $driver_file"
    insmod "$APP_DIR/$driver_file"
}

if [ "$(id -u)" -ne 0 ]; then
    log "this script must run as root"
    exit 1
fi

if [ ! -d "$APP_DIR" ]; then
    log "application directory not found: $APP_DIR"
    exit 1
fi

cd "$APP_DIR"

load_driver led_drv.ko
load_driver rs485_dir.ko
load_driver sr505_drv.ko

if [ ! -x "$APP_DIR/mqttd" ]; then
    log "mqttd is missing or not executable: $APP_DIR/mqttd"
    exit 1
fi

if [ ! -x "$APP_DIR/MergedQtApp" ]; then
    log "MergedQtApp is missing or not executable: $APP_DIR/MergedQtApp"
    exit 1
fi

log "starting mqttd"
./mqttd -h 192.168.3.73 -p 1883 -i rk3566-001 -t test -v &
mqttd_pid=$!

if command -v udevadm >/dev/null 2>&1; then
    udevadm settle --timeout=10 || log "udev settle timed out; continuing"
fi

log "waiting ${APP_START_DELAY_SECONDS}s before starting MergedQtApp"
sleep "$APP_START_DELAY_SECONDS"

if ! kill -0 "$mqttd_pid" 2>/dev/null; then
    wait "$mqttd_pid" || true
    log "mqttd exited before MergedQtApp was started"
    exit 1
fi

log "starting MergedQtApp"
exec ./MergedQtApp
