#!/bin/sh
set -eu

APP_ROOT=${QT_YCEST_APP_ROOT:-/opt/qt_ycest/bin}
export QT_YCEST_APP_ROOT="$APP_ROOT"
export QT_YCEST_CONFIG_DIR=${QT_YCEST_CONFIG_DIR:-/var/lib/qt_ycest}

# 无桌面运行：Qt 直接通过 EGLFS/KMS 输出，不进入 linuxfb 或 /dev/fb0 mmap。
unset DISPLAY
unset WAYLAND_DISPLAY
unset XDG_SESSION_TYPE
unset FRAMEBUFFER
unset QT_QPA_FB_DRM
unset QT_QPA_FB_TSLIB

# EGLFS 必须取得 DRM master。默认停止仍在占用 DRM/KMS 的桌面显示管理器；
# 需要保留桌面调试时不要使用本脚本，显式以 xcb 启动。
if command -v systemctl >/dev/null 2>&1 && \
   systemctl is-active --quiet display-manager 2>/dev/null; then
    echo "[qt_ycest] stopping display-manager for EGLFS/KMS" >&2
    if ! systemctl stop display-manager; then
        echo "[qt_ycest] failed to stop display-manager; EGLFS cannot acquire DRM master" >&2
        exit 1
    fi
fi

if [ ! -e /dev/dri/card0 ]; then
    echo "[qt_ycest] /dev/dri/card0 not found; EGLFS/KMS is unavailable" >&2
    exit 1
fi

export QT_YCEST_QPA_PLATFORM=eglfs
export QT_QPA_PLATFORM=eglfs
export QT_QPA_EGLFS_INTEGRATION=${QT_QPA_EGLFS_INTEGRATION:-eglfs_kms}
export QT_QPA_EGLFS_HIDECURSOR=${QT_QPA_EGLFS_HIDECURSOR:-1}
export QT_QPA_EGLFS_FORCE888=${QT_QPA_EGLFS_FORCE888:-1}
export QT_QPA_EGLFS_SWAPINTERVAL=${QT_QPA_EGLFS_SWAPINTERVAL:-1}

# RK3566 Linux 4.19 + Qt 5.12 默认使用 legacy page-flip。
export QT_YCEST_EGLFS_ATOMIC=${QT_YCEST_EGLFS_ATOMIC:-0}
export QT_QPA_EGLFS_KMS_ATOMIC=$QT_YCEST_EGLFS_ATOMIC
export QT_OPENGL=${QT_OPENGL:-es2}

# GStreamer 继续优先使用 MPP 硬件解码；NV12 通过 appsink 交给 Qt OpenGL ES 合成。
export QT_YCEST_VIDEO_OUTPUT=qt
unset QT_YCEST_DRM_DRIVER
unset QT_YCEST_DRM_CONNECTOR_ID
unset QT_YCEST_DRM_PLANE_ID
unset QT_YCEST_DRM_VIDEO_ZPOS

export QT_YCEST_GST_AUDIO_SINK=${QT_YCEST_GST_AUDIO_SINK:-alsasink}
export QT_YCEST_ALSA_DEVICE=${QT_YCEST_ALSA_DEVICE:-default}

# 只恢复背光电源，不访问 fb0。
for backlight in /sys/class/backlight/*; do
    [ -d "$backlight" ] || continue
    [ -w "$backlight/bl_power" ] && echo 0 > "$backlight/bl_power" || true
done

mkdir -p "$QT_YCEST_CONFIG_DIR"
cd "$APP_ROOT"

if [ ! -x ./qt_ycest ]; then
    echo "[qt_ycest] executable not found: $APP_ROOT/qt_ycest" >&2
    exit 1
fi

echo "[qt_ycest] platform=eglfs integration=$QT_QPA_EGLFS_INTEGRATION atomic=$QT_QPA_EGLFS_KMS_ATOMIC swapInterval=$QT_QPA_EGLFS_SWAPINTERVAL" >&2
exec ./qt_ycest "$@"
