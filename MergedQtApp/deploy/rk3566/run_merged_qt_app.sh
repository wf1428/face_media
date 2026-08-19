#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

: "${QT_QPA_PLATFORM:=eglfs}"
: "${QT_QPA_EGLFS_INTEGRATION:=eglfs_kms}"
: "${QT_QPA_EGLFS_HIDECURSOR:=1}"
: "${QT_QPA_EGLFS_FORCE888:=1}"
: "${QT_QPA_EGLFS_SWAPINTERVAL:=1}"

export QT_QPA_PLATFORM
export QT_QPA_EGLFS_INTEGRATION
export QT_QPA_EGLFS_HIDECURSOR
export QT_QPA_EGLFS_FORCE888
export QT_QPA_EGLFS_SWAPINTERVAL

cd "$APP_DIR"
exec "$APP_DIR/MergedQtApp" "$@"
