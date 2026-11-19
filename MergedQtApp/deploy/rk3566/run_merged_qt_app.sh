#!/bin/sh
# @file run_merged_qt_app.sh
# @brief 以 EGLFS 直接启动 MergedQtApp。
# @author Du lin
# @note 最近修改：2026-11-17；修改人：Du lin；从 Qt 启动脚本加载共享存储函数，直接启动前导出存储模式。
set -eu

storage_script="$(dirname -- "$0")/start_face_media.sh"
if [ ! -r "$storage_script" ]; then
    storage_script="$(dirname -- "$0")/../../scripts/start_face_media.sh"
fi
if [ ! -r "$storage_script" ]; then
    printf '%s\n' '[face-media] missing start_face_media.sh' >&2
    exit 1
fi
FACE_MEDIA_LOAD_STORAGE_ONLY=1 . "$storage_script"
unset FACE_MEDIA_LOAD_STORAGE_ONLY
face_media_detect_storage

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

: "${QT_QPA_PLATFORM:=eglfs}"
: "${QT_QPA_EGLFS_INTEGRATION:=eglfs_kms}"
: "${QT_QPA_EGLFS_FORCE888:=1}"
: "${QT_QPA_EGLFS_SWAPINTERVAL:=1}"

export QT_QPA_PLATFORM
export QT_QPA_EGLFS_INTEGRATION
export QT_QPA_EGLFS_FORCE888
export QT_QPA_EGLFS_SWAPINTERVAL
unset QT_QPA_EGLFS_HIDECURSOR

cd "$APP_DIR"
exec "$APP_DIR/MergedQtApp" "$@"
