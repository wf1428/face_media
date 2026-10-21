#!/bin/sh
set -eu

CONFIG=${WESTON_CONFIG:-/etc/xdg/weston/weston.ini}
SERVICE=${WESTON_SERVICE:-weston-kiosk.service}
SOCKET=${WAYLAND_DISPLAY:-wayland-0}
RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
OUTPUT_NAME=${WESTON_OUTPUT_NAME:-DSI-1}
TOUCH_NAME=${WESTON_TOUCH_NAME:-Goodix Capacitive TouchScreen}
TOUCH_RULE=${WESTON_TOUCH_RULE:-/etc/udev/rules.d/99-goodix-weston-rotation.rules}
APP_NAME=${YCEST_APP_NAME:-MergedQtApp}
APP_BIN_DEFAULT=${YCEST_APP_BIN:-/home/cat/face_media/MergedQtApp}
APP_QPA_PLATFORM=${YCEST_APP_QPA_PLATFORM:-wayland-egl}
APP_LOG=${YCEST_APP_LOG:-/var/log/merged-qt-app.log}
APP_SERVICE=${YCEST_APP_SERVICE:-face-media.service}
APP_STATE_DIR=${YCEST_APP_STATE_DIR:-/run/face-media}
APP_PIDFILE=${YCEST_APP_PIDFILE:-$APP_STATE_DIR/app.pid}
APP_MAINTENANCE_FILE=${YCEST_APP_MAINTENANCE_FILE:-$APP_STATE_DIR/maintenance}
UI_CONFIG=${YCEST_UI_CONFIG:-/mnt/UDISK/res/static/demoResources/images/logo/ycest_cfg.ini}
ROTATE_LOCK=/run/weston-rotate.lock
MAINTENANCE_OWNED=0

usage()
{
    echo "Usage: $0 prepare-environment | start | current | touch-matrix | restart-app | apply-layout [ycest_cfg.ini] | validate-rotation normal|0|90|180|270 [ycest_cfg.ini] | rotate normal|0|90|180|270" >&2
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
export XDG_RUNTIME_DIR=/run/user/0
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
        echo "[weston] UI config is missing or unreadable: $ui_config_path" >&2
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
        echo "[weston] invalid [backRect] size in $ui_config_path; both dimensions must be integers >= 600" >&2
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
    echo "[weston] square UI size ${layout_width}x${layout_height} has no orientation" >&2
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

    if ! layout_rotation=$(rotation_from_ui_config "$ui_config_path"); then
        echo "[weston] cannot determine UI layout orientation: $ui_config_path" >&2
        return 4
    fi

    if rotation_matches_layout "$degrees" "$layout_rotation"; then
        echo "[weston] rotation ${degrees} degrees matches the UI layout orientation"
        return 0
    fi

    echo "[weston] rotation ${degrees} degrees conflicts with the UI layout orientation; request ignored" >&2
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
        echo "[weston] touchscreen not found; skip touch calibration"
        return 0
    fi

    if ! command -v udevadm >/dev/null 2>&1; then
        echo "[weston] udevadm not found; touch calibration was not refreshed" >&2
        return 1
    fi
    if [ ! -r "$TOUCH_RULE" ]; then
        echo "[weston] touchscreen rule not found: $TOUCH_RULE" >&2
        return 1
    fi

    udevadm control --reload-rules
    udevadm trigger --action=change --subsystem-match=input
    udevadm settle --timeout=5 || true
    echo "[weston] touchscreen calibration refreshed: $TOUCH_NAME"
}

set_rotation()
{
    degrees=$1
    case "$degrees" in
        0) transform=normal ;;
        90|180|270) transform=$degrees ;;
        *) return 1 ;;
    esac

    if [ ! -r "$CONFIG" ]; then
        echo "[weston] config not found: $CONFIG" >&2
        return 1
    fi

    # 只更新指定 output 段；HDMI 的 transform 必须保持独立。
    rotation_config=$(readlink -f "$CONFIG") || return 1
    rotation_tmp=$(mktemp "${rotation_config}.XXXXXX") || return 1
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
        echo "[weston] expected one output section named $OUTPUT_NAME in $CONFIG" >&2
        return 1
    fi
    if ! chmod --reference="$rotation_config" "$rotation_tmp" ||
       ! chown --reference="$rotation_config" "$rotation_tmp" ||
       ! mv -f "$rotation_tmp" "$rotation_config"; then
        rm -f "$rotation_tmp"
        return 1
    fi
    echo "[weston] transform=$transform"
}

start_weston()
{
    install_and_activate_runtime_environment

    if [ ! -r "$CONFIG" ]; then
        echo "[weston] config not found: $CONFIG" >&2
        exit 1
    fi

    install -d -m 0700 "$RUNTIME_DIR"
    rm -f "$RUNTIME_DIR/$SOCKET" "$RUNTIME_DIR/$SOCKET.lock"

    # 每次启动都核对界面配置的横竖方向。已保存角度与布局同向时保留
    # 0/180 或 90/270 的正反朝向；只有横竖方向冲突时才回到布局默认角度。
    if [ -r "$UI_CONFIG" ]; then
        initial_rotation=$(rotation_from_ui_config "$UI_CONFIG" 2>/dev/null || true)
        if [ -n "$initial_rotation" ]; then
            saved_rotation=$(current_rotation)
            if rotation_matches_layout "$saved_rotation" "$initial_rotation"; then
                echo "[weston] saved rotation ${saved_rotation} degrees matches UI layout; keeping it"
            else
                set_rotation "$initial_rotation" ||
                    echo "[weston] failed to apply initial rotation; keeping current transform" >&2
            fi
        fi
    fi

    # 每次服务启动都按当前 transform 刷新一次触摸属性，保证重启主板或
    # 手动重启 Weston 后仍使用正确矩阵。触摸校正失败不阻止显示启动。
    refresh_touch_properties ||
        echo "[weston] continuing without refreshed touch calibration" >&2

    export XDG_RUNTIME_DIR="$RUNTIME_DIR"
    export WAYLAND_DISPLAY="$SOCKET"
    exec /usr/bin/weston \
        --backend=drm-backend.so \
        --tty=1 \
        --socket="$SOCKET" \
        --config="$CONFIG" \
        --log=/var/log/weston-kiosk.log
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
        echo "[weston] Qt did not exit after SIGINT; forced shutdown:$forced_pids" >&2
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
            echo "[weston] Qt process is still alive after SIGKILL: pid=$candidate_pid" >&2
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
        systemctl start "$APP_SERVICE" || {
            echo "[weston] failed to start $APP_SERVICE" >&2
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
        echo "[weston] supervised Qt did not start; service=$APP_SERVICE log=$APP_LOG" >&2
        return 1
    fi

    # 启动页约 3 秒。必须观察到启动页结束以后，才能确认 Qt 没有因为
    # Wayland 二次断连或初始化失败而退出。
    attempts=0
    while [ "$attempts" -lt 60 ]; do
        if ! pid_is_running_app "$new_app_pid"; then
            echo "[weston] Qt exited during startup; see $APP_LOG" >&2
            if kill -0 "$new_app_pid" 2>/dev/null; then
                kill -9 "$new_app_pid" 2>/dev/null || true
            fi
            rm -f "$APP_PIDFILE" 2>/dev/null || true
            return 1
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
    echo "[weston] Qt restarted successfully by $APP_SERVICE: pid=$new_app_pid"
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

        echo "[weston] Qt launch attempt $launch_attempt failed" >&2
        if [ "$launch_attempt" -lt 3 ]; then
            if ! wait_for_weston_stable; then
                echo "[weston] compositor became unavailable before Qt retry" >&2
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
        echo "[weston] compositor is not ready; Qt was not restarted" >&2
        return 1
    fi
    start_qt_app
}

restart_qt_app()
{
    if [ "$(id -u)" -ne 0 ]; then
        echo "[weston] Qt restart requires root privileges" >&2
        return 1
    fi

    # 与旋转共用锁，防止 U 盘触发的重启和旋转事务交叉执行。
    exec 9>"$ROTATE_LOCK"
    if ! flock 9; then
        echo "[weston] failed to acquire the rotation lock" >&2
        return 1
    fi

    restart_qt_app_locked
}

restart_weston()
{
    systemctl restart "$SERVICE"
    if ! wait_for_weston_stable; then
        echo "[weston] compositor did not become stable" >&2
        return 1
    fi
}

rollback_rotation()
{
    old_degrees=$1
    echo "[weston] rolling back to ${old_degrees} degrees" >&2
    set_rotation "$old_degrees" || true
    refresh_touch_properties || true
    systemctl restart "$SERVICE" || true
    wait_for_weston_stable || true
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
        echo "[weston] rotation requires root privileges" >&2
        exit 1
    fi

    # 同一时刻只允许一次旋转，避免界面连续点击造成交叉重启。
    # 界面手动操作保持非阻塞；U盘 apply-layout 必须等待正在执行的
    # 事务结束，不能因为一次锁竞争而漏掉新配置对应的自动方向。
    exec 9>"$ROTATE_LOCK"
    if [ "$restart_if_unchanged" -eq 1 ]; then
        if ! flock 9; then
            echo "[weston] failed to acquire the rotation lock" >&2
            exit 1
        fi
    else
        if ! flock -n 9; then
            echo "[weston] another rotation is already running" >&2
            exit 1
        fi
    fi

    old_degrees=$(current_rotation)
    if [ "$degrees" = "$old_degrees" ]; then
        echo "[weston] rotation already set to ${degrees} degrees"
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
        echo "[weston] transform entry not found in $CONFIG" >&2
        exit 1
    fi

    capture_app_context
    if [ ! -x "$APP_BIN" ]; then
        echo "[weston] captured Qt executable is unavailable: $APP_BIN" >&2
        exit 1
    fi
    if ! stop_qt_app; then
        echo "[weston] Qt could not be stopped; rotation was not changed" >&2
        exit 1
    fi

    if ! set_rotation "$degrees"; then
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
        echo "[weston] Qt restart failed after rotation; rolling back" >&2
        rollback_rotation "$old_degrees"
        exit 1
    fi

    echo "[weston] rotation completed: ${degrees} degrees; output=$OUTPUT_NAME"
}

apply_layout_from_config()
{
    [ "$#" -le 1 ] || usage
    layout_config=${1:-$UI_CONFIG}

    # 配置异常时保留当前 Weston 方向，但仍重启 Qt，让U盘更新的其他
    # 合法资源正常生效；错误会写入U盘脚本日志，避免错误尺寸误转屏幕。
    if ! target_rotation=$(rotation_from_ui_config "$layout_config"); then
        echo "[weston] keep current rotation and restart Qt only" >&2
        restart_qt_app
        return $?
    fi

    current_degrees=$(current_rotation)
    if rotation_matches_layout "$current_degrees" "$target_rotation"; then
        # U盘资源横竖方向未变时，保留用户选择的正反朝向。
        target_rotation=$current_degrees
    fi

    echo "[weston] UI config requests ${target_rotation} degrees: $layout_config"
    # rotate_weston 自己持有统一锁。目标方向未变化时也只重启一次 Qt；
    # 方向变化时则复用既有的 Weston、触摸与 Qt 完整事务。
    rotate_weston "$target_rotation" 1
}

# 任意失败或中断都必须退出维护态；监督脚本会在 Weston 可用后恢复 Qt。
trap 'leave_app_maintenance' EXIT
trap 'exit 1' HUP INT TERM

case "${1:-}" in
    prepare-environment)
        [ "$#" -eq 1 ] || usage
        install_and_activate_runtime_environment
        ;;
    start)
        [ "$#" -eq 1 ] || usage
        start_weston
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
        restart_qt_app
        ;;
    apply-layout)
        shift
        apply_layout_from_config "$@"
        ;;
    validate-rotation)
        shift
        validate_rotation_from_ui_config "$@"
        ;;
    rotate)
        shift
        rotate_weston "$@"
        ;;
    *)
        usage
        ;;
esac
