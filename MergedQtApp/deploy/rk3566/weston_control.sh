#!/bin/sh
# @file weston_control.sh
# @brief 管理 Weston 运行环境、显示旋转和维护后的 Qt 启停。
# @author Du lin
# @note 最近修改：2026-11-17；修改人：Du lin；共享存储模式保存 LCD 角度，Weston 启动及旋转回滚恢复该角度，继续读取本槽硬件配置。
set -eu

# 触摸矩阵等查询和 Weston 日志过滤不访问业务存储。
case "${1:-}" in
    start|validate-rotation|rotate|apply-layout|restart-app)
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
        face_media_detect_storage || exit $?
        ;;
    *) FACE_MEDIA_AB_STORAGE=0 ;;
esac

CONFIG=${WESTON_CONFIG:-/etc/xdg/weston/weston.ini}
SERVICE=${WESTON_SERVICE:-weston-kiosk.service}
SOCKET=${WAYLAND_DISPLAY:-wayland-0}
# 与系统服务及 Qt 使用同一专用目录，不继承 SSH/PAM 的用户会话运行目录。
RUNTIME_DIR=/run/face-media-wayland
OUTPUT_NAME=${WESTON_OUTPUT_NAME:-DSI-1}
TOUCH_NAME=${WESTON_TOUCH_NAME:-Goodix Capacitive TouchScreen}
TOUCH_RULE=${WESTON_TOUCH_RULE:-/etc/udev/rules.d/99-goodix-weston-rotation.rules}
APP_NAME=${YCEST_APP_NAME:-MergedQtApp}
APP_BIN_DEFAULT=${YCEST_APP_BIN:-/home/cat/face_media/MergedQtApp}
APP_QPA_PLATFORM=${YCEST_APP_QPA_PLATFORM:-wayland-egl}
UI_CONFIG=${YCEST_UI_CONFIG:-/mnt/UDISK/res/static/demoResources/images/logo/ycest_cfg.ini}
APP_LOG=${YCEST_APP_LOG:-/var/log/face-media/face_media.log}
if [ "$FACE_MEDIA_AB_STORAGE" = 1 ]; then
    UI_CONFIG=$(face_media_data_path "$UI_CONFIG")
    APP_LOG=$(face_media_data_path "$APP_LOG")
    ROTATION_STATE=$(face_media_data_path /var/lib/qt_ycest/weston-rotation)
fi
APP_SERVICE=${YCEST_APP_SERVICE:-face-media.service}
APP_STATE_DIR=${YCEST_APP_STATE_DIR:-/run/face-media}
APP_PIDFILE=${YCEST_APP_PIDFILE:-$APP_STATE_DIR/app.pid}
APP_MAINTENANCE_FILE=${YCEST_APP_MAINTENANCE_FILE:-$APP_STATE_DIR/maintenance}
ROTATE_LOCK=/run/weston-rotate.lock
MAINTENANCE_OWNED=0
WESTON_START_STAGE=

# 脚本失败由对应分支提交；查询结果仍通过 stdout 返回。
exec 2>/dev/null

# @brief 每次开机按错误键向 journal 提交一次错误，跨服务重启生效。
# @param $1 无斜杠的稳定错误键；其余参数为首条错误的详细信息。
# /run 中仅建立空标记。mkdir 原子占用键；提交失败释放标记，记录故障不影响业务返回值。
journal_error_once() (
    error_key=$1
    shift
    error_dir=/run/face-media-errors/weston-kiosk
    claimed=0
    if mkdir -p "$error_dir" 2>/dev/null; then
        if mkdir "$error_dir/$error_key" 2>/dev/null; then
            claimed=1
        elif [ -d "$error_dir/$error_key" ]; then
            exit 0
        fi
    fi
    if ! logger -p daemon.err -t weston-kiosk -- "$*" 2>/dev/null; then
        [ "$claimed" -eq 0 ] || rmdir "$error_dir/$error_key" 2>/dev/null || true
    fi
)


usage()
{
    journal_error_once invalid-arguments "Usage: $0 prepare-environment | start | current | touch-matrix | restart-app | apply-layout [ycest_cfg.ini] | validate-rotation normal|0|90|180|270 [ycest_cfg.ini] | rotate normal|0|90|180|270"
    exit 2
}

normalize_rotation()
{
    case "${1:-}" in
        normal|0) echo 0 ;;
        90|180|270) echo "$1" ;;
        *) return 1 ;;
    esac
}

# @brief 更新并加载唯一环境配置块；Wayland 目录与系统服务一致，覆盖旧会话目录。
# @note 写入失败沿用 set -e 退出；本函数不负责启动或自动恢复 Weston。
install_and_activate_runtime_environment()
{
    profile_file=${FACE_MEDIA_PROFILE_FILE:-/etc/profile}
    profile_begin="# BEGIN FACE_MEDIA_ENV"
    profile_end="# END FACE_MEDIA_ENV"

    if [ ! -f "$profile_file" ]; then
        : > "$profile_file"
    fi

    # 由最先使用图形环境的 Weston 负责维护唯一配置块，避免单独 env 文件。
    profile_tmp=$(mktemp /tmp/face-media-profile.XXXXXX)
    awk -v begin="$profile_begin" -v end="$profile_end" '
        $0 == begin { skipping=1; next }
        $0 == end   { skipping=0; next }
        !skipping  { print }
    ' "$profile_file" > "$profile_tmp"
    cat "$profile_tmp" > "$profile_file"
    rm -f "$profile_tmp"

    cat >> "$profile_file" <<'EOF'
# BEGIN FACE_MEDIA_ENV
export QT_QPA_PLATFORM=wayland-egl
export XDG_RUNTIME_DIR=/run/face-media-wayland
export WAYLAND_DISPLAY=wayland-0
export QT_WAYLAND_SHELL_INTEGRATION=wl-shell
export QT_WAYLAND_DISABLE_WINDOWDECORATION=1
export LD_PRELOAD=/lib/aarch64-linux-gnu/libgomp.so.1
export QT_PLUGIN_PATH=/usr/lib/aarch64-linux-gnu/qt5/plugins
export QML2_IMPORT_PATH=/usr/lib/aarch64-linux-gnu/qt5/qml
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/aarch64-linux-gnu/qt5/plugins/platforms
case ":${LD_LIBRARY_PATH:-}:" in
    *:/usr/lib/aarch64-linux-gnu/mali:*) ;;
    *) export LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu/mali${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac
# END FACE_MEDIA_ENV
EOF

    # 写入后立即激活；exec 启动的 Weston 会继承当前脚本环境。
    set +u
    . "$profile_file"
    set -u
}

current_rotation()
{
    if [ ! -r "$CONFIG" ]; then
        echo 0
        return
    fi

    value=$(awk -v target="$OUTPUT_NAME" '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        function finish_section() {
            if (in_output && output_name == target && !found) {
                rotation = output_transform
                found = 1
            }
        }
        /^[[:space:]]*\[/ {
            finish_section()
            section = $0
            sub(/[;#].*$/, "", section)
            gsub(/[[:space:]]/, "", section)
            in_output = (tolower(section) == "[output]")
            output_name = ""
            output_transform = ""
            next
        }
        in_output && /^[[:space:]]*(name|transform)[[:space:]]*=/ {
            separator = index($0, "=")
            key = trim(substr($0, 1, separator - 1))
            value = substr($0, separator + 1)
            sub(/[;#].*$/, "", value)
            value = trim(value)
            if (key == "name") output_name = value
            if (key == "transform") output_transform = value
        }
        END { finish_section(); print rotation }
    ' "$CONFIG")
    normalize_rotation "${value:-normal}" || echo 0
}

rotation_from_ui_config()
{
    ui_config_path=$1
    if [ ! -r "$ui_config_path" ]; then
        journal_error_once layout-unreadable "UI config is missing or unreadable: $ui_config_path"
        return 1
    fi

    # 只解析 [backRect] 内的两个尺寸键；同时兼容项目配置使用的
    # key=value;//comment 写法、CRLF 换行及键名大小写差异。
    layout_size=$(awk '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        /^[[:space:]]*\[/ {
            section=tolower($0)
            gsub(/[[:space:]]/, "", section)
            in_back_rect=(section == "[backrect]")
            next
        }
        in_back_rect && index($0, "=") > 0 {
            line=$0
            sub(/\r$/, "", line)
            key=tolower(trim(substr(line, 1, index(line, "=") - 1)))
            value=substr(line, index(line, "=") + 1)
            sub(/[;#].*$/, "", value)
            value=trim(value)
            if (key == "backpic_x_size") width=value
            if (key == "backpic_y_size") height=value
        }
        END {
            if (width ~ /^[0-9]+$/ && height ~ /^[0-9]+$/ &&
                (width + 0) >= 600 && (height + 0) >= 600) {
                print (width + 0), (height + 0)
                exit 0
            }
            exit 1
        }
    ' "$ui_config_path") || {
        journal_error_once layout-invalid-size "invalid [backRect] size in $ui_config_path; both dimensions must be integers >= 600"
        return 1
    }

    layout_width=${layout_size%% *}
    layout_height=${layout_size#* }
    if [ "$layout_width" -gt "$layout_height" ]; then
        echo 0
        return 0
    fi
    if [ "$layout_height" -gt "$layout_width" ]; then
        echo 90
        return 0
    fi

    # 正方形尺寸不能表达横竖方向，不能擅自改变当前物理显示方向。
    journal_error_once layout-square "square UI size ${layout_width}x${layout_height} has no orientation"
    return 1
}

rotation_matches_layout()
{
    degrees=$(normalize_rotation "$1") || return 2
    layout_rotation=$(normalize_rotation "$2") || return 2

    case "$layout_rotation:$degrees" in
        0:0|0:180|90:90|90:270) return 0 ;;
        *) return 1 ;;
    esac
}

validate_rotation_from_ui_config()
{
    [ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
    degrees=$(normalize_rotation "$1") || usage
    ui_config_path=${2:-$UI_CONFIG}
    if [ "$FACE_MEDIA_AB_STORAGE" = 1 ]; then
        ui_config_path=$(face_media_data_path "$ui_config_path")
    fi

    if ! layout_rotation=$(rotation_from_ui_config "$ui_config_path"); then
        return 4
    fi

    if rotation_matches_layout "$degrees" "$layout_rotation"; then
        return 0
    fi

    journal_error_once layout-conflict "rotation ${degrees} degrees conflicts with the UI layout orientation; request ignored"
    return 3
}

touch_matrix()
{
    # 当前设备已经确认：Weston 旋转 90 度时，触摸方向与显示相差 180 度。
    # 0/180 度返回单位矩阵（等同不校正），90/270 度补偿 180 度。
    case "$(current_rotation)" in
        90|270) echo "-1 0 1 0 -1 1" ;;
        0|180)  echo "1 0 0 0 1 0" ;;
        *)      echo "1 0 0 0 1 0" ;;
    esac
}

touchscreen_present()
{
    for name_file in /sys/class/input/event*/device/name; do
        [ -r "$name_file" ] || continue
        input_name=$(sed -n '1p' "$name_file" 2>/dev/null || true)
        if [ "$input_name" = "$TOUCH_NAME" ]; then
            return 0
        fi
    done
    return 1
}

refresh_touch_properties()
{
    if ! touchscreen_present; then
        return 0
    fi

    if ! command -v udevadm >/dev/null 2>&1; then
        journal_error_once udevadm-missing "udevadm not found; touch calibration was not refreshed"
        return 1
    fi
    if [ ! -r "$TOUCH_RULE" ]; then
        journal_error_once touch-rule-missing "touchscreen rule not found: $TOUCH_RULE"
        return 1
    fi

    # 保留原 set -e 行为以及 settle 可失败的约定。
    udevadm control --reload-rules >/dev/null || {
        udev_status=$?
        journal_error_once touch-reload "udevadm control --reload-rules failed (rc=$udev_status)"
        (exit "$udev_status")
    }
    udevadm trigger --action=change --subsystem-match=input >/dev/null || {
        udev_status=$?
        journal_error_once touch-trigger "udevadm trigger failed (rc=$udev_status)"
        (exit "$udev_status")
    }
    udevadm settle --timeout=5 >/dev/null || true
}

# @brief 修改本槽 Weston 配置中指定 LCD 的 transform。
# @param $1 已验证的角度，取值 0、90、180、270。
# @return 0 表示成功；输出段缺失或配置写入失败时返回非零。
set_rotation()
{
    degrees=$1
    case "$degrees" in
        0) transform=normal ;;
        90|180|270) transform=$degrees ;;
        *) return 1 ;;
    esac

    if [ ! -r "$CONFIG" ]; then
        journal_error_once weston-config-missing "config not found: $CONFIG"
        return 1
    fi

    # 只更新指定 output 段；HDMI 的 transform 必须保持独立。
    rotation_config=$(readlink -f "$CONFIG") || {
        journal_error_once rotation-path "cannot resolve Weston config: $CONFIG"
        return 1
    }
    rotation_tmp=$(mktemp "${rotation_config}.XXXXXX") || {
        journal_error_once rotation-temp "cannot create temporary Weston config: $rotation_config"
        return 1
    }
    if ! awk -v target="$OUTPUT_NAME" -v replacement="$transform" '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        function finish_section(ordinal, changed, text) {
            if (!line_count) return
            changed = 0
            if (in_output && output_name == target) matched++
            for (ordinal = 1; ordinal <= line_count; ordinal++) {
                text = lines[ordinal]
                if (in_output && output_name == target &&
                    text ~ /^[[:space:]]*transform[[:space:]]*=/) {
                    sub(/transform[[:space:]]*=.*/, "transform=" replacement, text)
                    changed = 1
                }
                print text
            }
            if (in_output && output_name == target && !changed) {
                print "transform=" replacement
            }
            line_count = 0
        }
        /^[[:space:]]*\[/ {
            finish_section()
            section = $0
            sub(/[;#].*$/, "", section)
            gsub(/[[:space:]]/, "", section)
            in_output = (tolower(section) == "[output]")
            output_name = ""
        }
        {
            lines[++line_count] = $0
            if (in_output && $0 ~ /^[[:space:]]*name[[:space:]]*=/) {
                value = substr($0, index($0, "=") + 1)
                sub(/[;#].*$/, "", value)
                output_name = trim(value)
            }
        }
        END { finish_section(); if (matched != 1) exit 2 }
    ' "$CONFIG" > "$rotation_tmp"; then
        rm -f "$rotation_tmp"
        journal_error_once rotation-output-invalid "expected one output section named $OUTPUT_NAME in $CONFIG"
        return 1
    fi
    if ! chmod --reference="$rotation_config" "$rotation_tmp" ||
       ! chown --reference="$rotation_config" "$rotation_tmp" ||
       ! mv -f "$rotation_tmp" "$rotation_config"; then
        rm -f "$rotation_tmp"
        journal_error_once rotation-write "cannot replace Weston config: $rotation_config"
        return 1
    fi
}

# @brief 原子保存供两槽共用的旋转角度；单分区由 Weston 配置保存。
# @param $1 角度，取值 0、90、180、270。
# @return 0 表示成功；共享状态写入失败返回非零。
save_rotation() (
    [ "$FACE_MEDIA_AB_STORAGE" = 1 ] || exit 0
    install -d -m 0755 "$(dirname -- "$ROTATION_STATE")" || exit 1
    tmp=$(mktemp "${ROTATION_STATE}.XXXXXX") || exit 1
    trap 'rm -f -- "$tmp"' EXIT
    printf '%s\n' "$1" > "$tmp" && mv -f -- "$tmp" "$ROTATION_STATE"
)

# @brief 在专用运行目录启动 Weston，保留原布局和触摸初始化。
# @note 系统服务先通过 RuntimeDirectory 创建目录；直接调用时沿用原 install -d 兜底。
start_weston()
{
    WESTON_START_STAGE=environment
    install_and_activate_runtime_environment
    WESTON_START_STAGE=

    if [ ! -r "$CONFIG" ]; then
        journal_error_once weston-config-missing "config not found: $CONFIG"
        exit 1
    fi

    WESTON_START_STAGE=runtime-directory
    install -d -m 0700 "$RUNTIME_DIR"
    rm -f "$RUNTIME_DIR/$SOCKET" "$RUNTIME_DIR/$SOCKET.lock"
    WESTON_START_STAGE=

    # 共享角度优先，布局的横竖方向仍是约束。只修改本槽 LCD 段，其他输出使用镜像配置。
    slot_rotation=$(current_rotation)
    saved_rotation=$slot_rotation
    if [ "$FACE_MEDIA_AB_STORAGE" = 1 ] && [ -r "$ROTATION_STATE" ]; then
        shared_rotation=$(sed -n '1p' "$ROTATION_STATE")
        if [ -n "$shared_rotation" ]; then
            saved_rotation=$(normalize_rotation "$shared_rotation") || saved_rotation=$slot_rotation
        fi
    fi
    if [ -r "$UI_CONFIG" ]; then
        initial_rotation=$(rotation_from_ui_config "$UI_CONFIG" 2>/dev/null || true)
        if [ -n "$initial_rotation" ] && ! rotation_matches_layout "$saved_rotation" "$initial_rotation"; then
            saved_rotation=$initial_rotation
        fi
    fi
    if [ "$saved_rotation" != "$slot_rotation" ]; then
        if ! set_rotation "$saved_rotation" && [ "$FACE_MEDIA_AB_STORAGE" = 1 ]; then exit 1; fi
    fi
    if [ "$FACE_MEDIA_AB_STORAGE" = 1 ] &&
       { [ ! -s "$ROTATION_STATE" ] || [ "$saved_rotation" != "${shared_rotation:-}" ]; }; then
        save_rotation "$saved_rotation" || {
            journal_error_once shared-rotation-save "cannot persist Weston rotation: $ROTATION_STATE"
            exit 1
        }
    fi

    # 每次服务启动都按当前 transform 刷新一次触摸属性，保证重启主板或
    # 手动重启 Weston 后仍使用正确矩阵。触摸校正失败不阻止显示启动。
    refresh_touch_properties || true

    export XDG_RUNTIME_DIR="$RUNTIME_DIR"
    export WAYLAND_DISPLAY="$SOCKET"
    # 过滤器只消费输出；两层 exec 保持 Weston 为 systemd 追踪的主进程。
    WESTON_START_STAGE=launch
    exec /bin/bash -c 'exec /usr/bin/weston "$@" > >(/bin/sh "$0" filter-weston-errors) 2>&1' \
        "$0" --backend=drm-backend.so --tty=1 --socket="$SOCKET" --config="$CONFIG"
}

# @brief Weston 原生输出缺少统一优先级，按错误措辞挑选需提交的行。
# @note 后台持续消费管道。去重键忽略时间戳、PID 和重试序号；不改变 Weston 的退出状态。
filter_weston_errors()
{
    LC_ALL=C awk '
        {
            sub(/^[[:space:]]*\[[[:space:]0-9:.]+\][[:space:]]*/, "");
            sub(/^[[:space:]]+/, "");
            if (tolower($0) ~ /(^|[^[:alnum:]_])(error|fatal|fail(ed|ure)?|cannot|could not|unable to|permission denied|not found|no such|no .* found|no space left)([^[:alnum:]_]|$)/) {
                print;
                fflush();
            }
        }
    ' | while IFS= read -r weston_error; do
        fingerprint=$(printf '%s\n' "$weston_error" |
            sed -E 's/((pid|attempt|retry)[=: ]+)[0-9]+/\1#/Ig' | cksum)
        journal_error_once "native-${fingerprint%% *}" "$weston_error"
    done
}

wait_for_weston()
{
    attempts=0
    while [ "$attempts" -lt 150 ]; do
        if systemctl is-active --quiet "$SERVICE" && [ -S "$RUNTIME_DIR/$SOCKET" ]; then
            if ! command -v weston-info >/dev/null 2>&1 ||
               XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY="$SOCKET" \
                   weston-info >/dev/null 2>&1; then
                return 0
            fi
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
    return 1
}

wait_for_weston_stable()
{
    # socket 刚出现并不代表 Weston 已经稳定。若 compositor 随后被 systemd
    # 再次拉起，过早启动的 Qt 会在启动页结束后因 Wayland 断连而退出。
    wait_for_weston || return 1

    stable_pid=$(systemctl show -p MainPID --value "$SERVICE" 2>/dev/null || true)
    case "$stable_pid" in
        ''|0|*[!0-9]*) return 1 ;;
    esac

    attempts=0
    while [ "$attempts" -lt 30 ]; do
        current_pid=$(systemctl show -p MainPID --value "$SERVICE" 2>/dev/null || true)
        if [ "$current_pid" != "$stable_pid" ] ||
           ! systemctl is-active --quiet "$SERVICE" ||
           [ ! -S "$RUNTIME_DIR/$SOCKET" ]; then
            return 1
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done

    if command -v weston-info >/dev/null 2>&1; then
        XDG_RUNTIME_DIR="$RUNTIME_DIR" WAYLAND_DISPLAY="$SOCKET" \
            weston-info >/dev/null 2>&1 || return 1
    fi
    return 0
}

pid_is_running_app()
{
    candidate_pid=${1:-}
    case "$candidate_pid" in
        ''|*[!0-9]*) return 1 ;;
    esac

    [ -r "/proc/$candidate_pid/stat" ] || return 1
    kill -0 "$candidate_pid" 2>/dev/null || return 1

    process_state=$(awk '{print $3}' "/proc/$candidate_pid/stat" 2>/dev/null || true)
    [ "$process_state" != "Z" ] || return 1

    process_name=$(sed -n '1p' "/proc/$candidate_pid/comm" 2>/dev/null || true)
    [ "$process_name" = "$APP_NAME" ]
}

enter_app_maintenance()
{
    install -d -m 0755 "$APP_STATE_DIR"
    printf '%s\n' "$$:weston-control" > "$APP_MAINTENANCE_FILE"
    MAINTENANCE_OWNED=1
}

leave_app_maintenance()
{
    if [ "$MAINTENANCE_OWNED" -eq 1 ]; then
        rm -f "$APP_MAINTENANCE_FILE" 2>/dev/null || true
        MAINTENANCE_OWNED=0
    fi
}

list_running_app_pids()
{
    if command -v pgrep >/dev/null 2>&1; then
        pgrep -x "$APP_NAME" 2>/dev/null || true
        return
    fi

    if command -v pidof >/dev/null 2>&1; then
        pidof "$APP_NAME" 2>/dev/null || true
    fi
}

capture_app_context()
{
    APP_BIN="$APP_BIN_DEFAULT"
    APP_DIR=$(dirname "$APP_BIN_DEFAULT")
    APP_PIDS="$(list_running_app_pids)"
    app_pid=""

    # 优先采用 start_face_media 监督脚本记录的 PID。
    if [ -r "$APP_PIDFILE" ]; then
        managed_pid=$(sed -n '1p' "$APP_PIDFILE" 2>/dev/null || true)
        if pid_is_running_app "$managed_pid"; then
            app_pid=$managed_pid

            # 即使本次 pgrep/pidof 漏报，也必须把监督脚本记录的 PID 纳入停止集合。
            managed_pid_listed=0
            for candidate_pid in $APP_PIDS; do
                if [ "$candidate_pid" = "$managed_pid" ]; then
                    managed_pid_listed=1
                    break
                fi
            done
            if [ "$managed_pid_listed" -eq 0 ]; then
                APP_PIDS="$managed_pid $APP_PIDS"
            fi
        fi
    fi

    # 兼容首次部署：没有 PID 文件时接管一个真实、非僵尸的现有实例。
    if [ -z "$app_pid" ]; then
        for candidate_pid in $APP_PIDS; do
            if pid_is_running_app "$candidate_pid"; then
                app_pid=$candidate_pid
                break
            fi
        done
    fi

    if [ -z "$app_pid" ]; then
        rm -f "$APP_PIDFILE" 2>/dev/null || true
        return 0
    fi

    running_bin=$(readlink -f "/proc/$app_pid/exe" 2>/dev/null || true)
    running_dir=$(readlink -f "/proc/$app_pid/cwd" 2>/dev/null || true)
    if [ -n "$running_bin" ]; then
        APP_BIN=$running_bin
    fi
    if [ -n "$running_dir" ]; then
        APP_DIR=$running_dir
    else
        APP_DIR=$(dirname "$APP_BIN")
    fi
}

stop_qt_app()
{
    # 必须先进入维护态，避免监督脚本在旋转或U盘更新期间抢先拉起 Qt。
    enter_app_maintenance
    sleep 0.1
    # 覆盖“监督脚本恰好正在创建 Qt”的窄竞态，再抓取一次完整进程集合。
    capture_app_context

    # 清理本次事务开始时存在的全部实例，防止残留同名进程干扰旋转后的判定。
    for candidate_pid in $APP_PIDS; do
        if pid_is_running_app "$candidate_pid"; then
            kill -2 "$candidate_pid" 2>/dev/null || true
        fi
    done

    attempts=0
    while [ "$attempts" -lt 50 ]; do
        still_running=0
        for candidate_pid in $APP_PIDS; do
            if pid_is_running_app "$candidate_pid"; then
                still_running=1
                break
            fi
        done
        [ "$still_running" -eq 0 ] && break

        attempts=$((attempts + 1))
        sleep 0.1
    done

    forced_pids=""
    for candidate_pid in $APP_PIDS; do
        if pid_is_running_app "$candidate_pid"; then
            forced_pids="$forced_pids $candidate_pid"
            kill -9 "$candidate_pid" 2>/dev/null || true
        fi
    done
    if [ -n "$forced_pids" ]; then
        journal_error_once app-stop-timeout "Qt did not exit after SIGINT; forced shutdown:$forced_pids"
    fi

    attempts=0
    while [ "$attempts" -lt 20 ]; do
        still_running=0
        for candidate_pid in $APP_PIDS; do
            if pid_is_running_app "$candidate_pid"; then
                still_running=1
                break
            fi
        done
        [ "$still_running" -eq 0 ] && break

        attempts=$((attempts + 1))
        sleep 0.1
    done

    for candidate_pid in $APP_PIDS; do
        if pid_is_running_app "$candidate_pid"; then
            journal_error_once app-stop-failed "Qt process is still alive after SIGKILL: pid=$candidate_pid"
            return 1
        fi
    done

    rm -f "$APP_PIDFILE" 2>/dev/null || true
    return 0
}

start_qt_app_once()
{
    # 退出维护态后，Qt 只能由常驻的 start_face_media 监督脚本拉起。
    leave_app_maintenance
    if ! systemctl is-active --quiet "$APP_SERVICE"; then
        systemctl start "$APP_SERVICE" >/dev/null || {
            journal_error_once app-service-start "failed to start $APP_SERVICE"
            return 1
        }
    fi

    # 等待监督脚本写入新 PID，并确认进程已经成为 MergedQtApp。
    attempts=0
    app_detected=0
    new_app_pid=""
    while [ "$attempts" -lt 150 ]; do
        new_app_pid=$(sed -n '1p' "$APP_PIDFILE" 2>/dev/null || true)
        if pid_is_running_app "$new_app_pid"; then
            app_detected=1
            break
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
    if [ "$app_detected" -ne 1 ]; then
        journal_error_once app-pid-timeout "supervised Qt did not start; service=$APP_SERVICE log=$APP_LOG"
        return 1
    fi

    # 启动页约 3 秒。必须观察到启动页结束以后，才能确认 Qt 没有因为
    # Wayland 二次断连或初始化失败而退出。
    attempts=0
    while [ "$attempts" -lt 60 ]; do
        if ! pid_is_running_app "$new_app_pid"; then
            journal_error_once app-start-exit "Qt exited during startup; see $APP_LOG"
            if kill -0 "$new_app_pid" 2>/dev/null; then
                kill -9 "$new_app_pid" 2>/dev/null || true
            fi
            rm -f "$APP_PIDFILE" 2>/dev/null || true
            return 1
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
}

start_qt_app()
{
    # Weston 重启后的首次客户端连接偶尔会失败。统一在这里重试，确保
    # U盘更新和界面旋转走完全相同、可重复的拉起流程。
    launch_attempt=1
    while [ "$launch_attempt" -le 3 ]; do
        if start_qt_app_once; then
            return 0
        fi

        if [ "$launch_attempt" -lt 3 ]; then
            if ! wait_for_weston_stable; then
                journal_error_once qt-retry-compositor "compositor became unavailable before Qt retry"
                return 1
            fi
            sleep "$launch_attempt"
        fi
        launch_attempt=$((launch_attempt + 1))
    done
    return 1
}

restart_qt_app_locked()
{
    capture_app_context
    if ! stop_qt_app; then
        return 1
    fi
    if ! wait_for_weston_stable; then
        journal_error_once qt-restart-compositor "compositor is not ready; Qt was not restarted"
        return 1
    fi
    start_qt_app
}

restart_qt_app()
{
    if [ "$(id -u)" -ne 0 ]; then
        journal_error_once restart-permission "Qt restart requires root privileges"
        return 1
    fi

    # 与旋转共用锁，防止 U 盘触发的重启和旋转事务交叉执行。
    exec 9>"$ROTATE_LOCK"
    if ! flock 9; then
        journal_error_once rotation-lock "failed to acquire the rotation lock"
        return 1
    fi

    restart_qt_app_locked
}

restart_weston()
{
    systemctl restart "$SERVICE" >/dev/null || {
        service_status=$?
        journal_error_once weston-restart "cannot restart $SERVICE (rc=$service_status)"
        (exit "$service_status")
    }
    if ! wait_for_weston_stable; then
        journal_error_once weston-ready-timeout "compositor did not become stable"
        return 1
    fi
}

# @brief 失败时恢复本槽输出、共享角度以及原 Qt 会话。
# @param $1 原角度。
rollback_rotation()
{
    old_degrees=$1
    set_rotation "$old_degrees" || true
    save_rotation "$old_degrees" || journal_error_once shared-rotation-rollback "cannot restore shared Weston rotation"
    refresh_touch_properties || true
    systemctl restart "$SERVICE" >/dev/null || journal_error_once rollback-restart "cannot restart $SERVICE during rotation recovery"
    wait_for_weston_stable || journal_error_once rollback-ready "Weston did not become stable during rotation recovery"
    start_qt_app || true
}

rotate_weston()
{
    # 第二个参数仅供 apply-layout 内部使用：目标方向已经生效时，仍需
    # 重启 Qt 以加载U盘刚更新的配置和资源；界面手动旋转保持原行为。
    [ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
    degrees=$(normalize_rotation "$1") || usage
    restart_if_unchanged=${2:-0}
    case "$restart_if_unchanged" in
        0|1) ;;
        *) usage ;;
    esac

    # 设置页的手动旋转只允许改变同一横竖布局内的正反朝向。
    # apply-layout 的目标角度已经由同一配置计算得出，无需重复校验。
    if [ "$restart_if_unchanged" -eq 0 ]; then
        validate_rotation_from_ui_config "$degrees" || return $?
    fi

    if [ "$(id -u)" -ne 0 ]; then
        journal_error_once rotation-permission "rotation requires root privileges"
        exit 1
    fi

    # 同一时刻只允许一次旋转，避免界面连续点击造成交叉重启。
    # 界面手动操作保持非阻塞；U盘 apply-layout 必须等待正在执行的
    # 事务结束，不能因为一次锁竞争而漏掉新配置对应的自动方向。
    exec 9>"$ROTATE_LOCK"
    if [ "$restart_if_unchanged" -eq 1 ]; then
        if ! flock 9; then
            journal_error_once rotation-lock "failed to acquire the rotation lock"
            exit 1
        fi
    else
        if ! flock -n 9; then
            journal_error_once rotation-busy "another rotation is already running"
            exit 1
        fi
    fi

    old_degrees=$(current_rotation)
    if [ "$degrees" = "$old_degrees" ]; then
        if [ "$restart_if_unchanged" -eq 1 ]; then
            # 已持有旋转锁，直接调用无锁版本，避免嵌套 flock 导致死锁。
            if restart_qt_app_locked; then
                return 0
            fi
            return 1
        fi
        return 0
    fi
    if [ ! -r "$CONFIG" ] || ! grep -q '^[[:space:]]*transform[[:space:]]*=' "$CONFIG"; then
        journal_error_once transform-missing "transform entry not found in $CONFIG"
        exit 1
    fi

    capture_app_context
    if [ ! -x "$APP_BIN" ]; then
        journal_error_once app-bin-unavailable "captured Qt executable is unavailable: $APP_BIN"
        exit 1
    fi
    if ! stop_qt_app; then
        exit 1
    fi

    if ! set_rotation "$degrees"; then
        start_qt_app || true
        exit 1
    fi
    if ! save_rotation "$degrees"; then
        journal_error_once shared-rotation-save "cannot persist Weston rotation"
        set_rotation "$old_degrees" || true
        start_qt_app || true
        exit 1
    fi
    if ! refresh_touch_properties; then
        rollback_rotation "$old_degrees"
        exit 1
    fi
    if ! restart_weston; then
        rollback_rotation "$old_degrees"
        exit 1
    fi
    if ! start_qt_app; then
        journal_error_once app-rotation-restart "Qt restart failed after rotation; rolling back"
        rollback_rotation "$old_degrees"
        exit 1
    fi

}

apply_layout_from_config()
{
    [ "$#" -le 1 ] || usage
    layout_config=${1:-$UI_CONFIG}
    if [ "$FACE_MEDIA_AB_STORAGE" = 1 ]; then
        layout_config=$(face_media_data_path "$layout_config")
    fi

    # 配置异常时保留当前 Weston 方向，但仍重启 Qt，让U盘更新的其他
    # 合法资源正常生效；布局错误已写入 journal，避免错误尺寸误转屏幕。
    if ! target_rotation=$(rotation_from_ui_config "$layout_config"); then
        restart_qt_app
        return $?
    fi

    current_degrees=$(current_rotation)
    if rotation_matches_layout "$current_degrees" "$target_rotation"; then
        # U盘资源横竖方向未变时，保留用户选择的正反朝向。
        target_rotation=$current_degrees
    fi

    # rotate_weston 自己持有统一锁。目标方向未变化时也只重启一次 Qt；
    # 方向变化时则复用既有的 Weston、触摸与 Qt 完整事务。
    rotate_weston "$target_rotation" 1
}

# 任意失败或中断都必须退出维护态；监督脚本会在 Weston 可用后恢复 Qt。
# @brief 清除维护标记，并补报初始化中由 set -e 直接结束的失败。
weston_exit()
{
    weston_status=$1
    leave_app_maintenance
    if [ "$weston_status" -ne 0 ] && [ -n "$WESTON_START_STAGE" ]; then
        journal_error_once "startup-$WESTON_START_STAGE" "Weston startup failed during $WESTON_START_STAGE (rc=$weston_status)"
    fi
}

trap 'weston_exit "$?"' EXIT
trap 'exit 1' HUP INT TERM

case "${1:-}" in
    prepare-environment)
        [ "$#" -eq 1 ] || usage
        WESTON_START_STAGE=environment
        install_and_activate_runtime_environment >/dev/null
        WESTON_START_STAGE=
        ;;
    filter-weston-errors)
        filter_weston_errors
        ;;
    start)
        [ "$#" -eq 1 ] || usage
        start_weston >/dev/null
        ;;
    current)
        [ "$#" -eq 1 ] || usage
        current_rotation
        ;;
    touch-matrix)
        [ "$#" -eq 1 ] || usage
        touch_matrix
        ;;
    restart-app)
        [ "$#" -eq 1 ] || usage
        restart_qt_app >/dev/null
        ;;
    apply-layout)
        shift
        apply_layout_from_config "$@" >/dev/null
        ;;
    validate-rotation)
        shift
        validate_rotation_from_ui_config "$@"
        ;;
    rotate)
        shift
        rotate_weston "$@" >/dev/null
        ;;
    *)
        usage
        ;;
esac
