#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export QT_YCEST_XORG_CHILD=1
export XDG_SESSION_TYPE=x11

if command -v xset >/dev/null 2>&1; then
    xset s off || true
    xset s noblank || true
    xset -dpms || true
fi
if command -v xsetroot >/dev/null 2>&1; then
    xsetroot -solid black || true

    EMPTY_CURSOR=/tmp/qt_ycest-empty-cursor.xbm
    EMPTY_MASK=/tmp/qt_ycest-empty-mask.xbm
    cat > "$EMPTY_CURSOR" <<'CURSOR_EOF'
#define qt_ycest_empty_width 16
#define qt_ycest_empty_height 16
#define qt_ycest_empty_x_hot 0
#define qt_ycest_empty_y_hot 0
static unsigned char qt_ycest_empty_bits[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
CURSOR_EOF
    cp "$EMPTY_CURSOR" "$EMPTY_MASK"
    xsetroot -cursor "$EMPTY_CURSOR" "$EMPTY_MASK" || true
fi

start_window_manager()
{
    if [ -n "${QT_YCEST_WINDOW_MANAGER:-}" ]; then
        echo "[qt_ycest] starting custom window manager: $QT_YCEST_WINDOW_MANAGER" >&2
        sh -c "$QT_YCEST_WINDOW_MANAGER" >/tmp/qt_ycest-wm.log 2>&1 &
        return 0
    fi

    if command -v matchbox-window-manager >/dev/null 2>&1; then
        matchbox-window-manager -use_titlebar no >/tmp/qt_ycest-wm.log 2>&1 &
        return 0
    fi

    if command -v openbox >/dev/null 2>&1; then
        openbox >/tmp/qt_ycest-wm.log 2>&1 &
        return 0
    fi

    if command -v xfwm4 >/dev/null 2>&1; then
        xfwm4 --replace >/tmp/qt_ycest-wm.log 2>&1 &
        if command -v xfconf-query >/dev/null 2>&1; then
            xfconf-query -c xfwm4 -p /general/borderless_maximize -s true \
                >/dev/null 2>&1 || true
            if [ "${QT_YCEST_XORG_COMPOSITOR:-}" = "0" ]; then
                xfconf-query -c xfwm4 -p /general/use_compositing -s false \
                    >/dev/null 2>&1 || true
            fi
        fi
        return 0
    fi

    return 1
}

if ! start_window_manager; then
    echo "[qt_ycest] no supported window manager found" >&2
    echo "[qt_ycest] install xfwm4/openbox/matchbox or set QT_YCEST_WINDOW_MANAGER" >&2
    exit 1
fi

sleep "${QT_YCEST_WM_START_DELAY:-1}"

if [ "${QT_YCEST_XORG_SESSION_ONLY:-0}" = "1" ]; then
    echo "[qt_ycest] persistent Xorg session ready on ${DISPLAY:-:0}" >&2
    echo "[qt_ycest] application is not auto-started in --xorg-only mode" >&2
    # 保持 xinit 的首个客户端存活，使 Xorg 不依赖 qt_ycest 进程寿命。
    while :; do
        sleep 3600
    done
fi

exec "$SCRIPT_DIR/run_qt_ycest.sh" --app-only "$@"
