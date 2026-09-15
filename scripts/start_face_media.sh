#!/bin/sh

set -eu

APP_DIR=/home/cat/face_media
APP_START_DELAY_SECONDS=${APP_START_DELAY_SECONDS:-5}
APP_RESTART_DELAY_SECONDS=${APP_RESTART_DELAY_SECONDS:-2}
WESTON_SERVICE=${WESTON_SERVICE:-weston-kiosk.service}
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
STATE_DIR=${FACE_MEDIA_STATE_DIR:-/run/face-media}
APP_PID_FILE=${FACE_MEDIA_APP_PID_FILE:-$STATE_DIR/app.pid}
MAINTENANCE_FILE=${FACE_MEDIA_MAINTENANCE_FILE:-$STATE_DIR/maintenance}
LOG_DIR=/var/log/face-media
LOG_FILE=$LOG_DIR/face_media.log
LOG_DATE_FILE=$LOG_DIR/.active_date
LOG_FIFO=$STATE_DIR/qt-log.fifo
LOG_MAX_SIZE=$((50 * 1024 * 1024))
LOG_RETENTION_DAYS=10
LOG_BURST_LIMIT=$((10 * 1024 * 1024))
LOG_MAX_LINE_SIZE=$((64 * 1024))
APP_PID=""
MQTT_PID=""
LOGGER_PID=""
LOG_MAINTENANCE_PID=""
LOG_PIPE_OPEN=0
LOG_CAPTURE_MODE=none

log()
{
    printf '%s\n' "[face-media] $*"
}

get_file_size()
{
    size_file=$1
    [ -f "$size_file" ] || {
        echo 0
        return 0
    }
    wc -c < "$size_file" 2>/dev/null | tr -d ' '
}

valid_log_date()
{
    case "${1:-}" in
        [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) return 0 ;;
        *) return 1 ;;
    esac
}

next_archive_log_path()
{
    archive_date=$1
    archive_path="$LOG_DIR/face_media_${archive_date}.log"
    if [ ! -e "$archive_path" ]; then
        echo "$archive_path"
        return 0
    fi

    archive_index=1
    while [ -e "$LOG_DIR/face_media_${archive_date}_${archive_index}.log" ]; do
        archive_index=$((archive_index + 1))
    done
    echo "$LOG_DIR/face_media_${archive_date}_${archive_index}.log"
}

archive_active_log()
{
    archive_date=$1
    active_size=$(get_file_size "$LOG_FILE")
    [ "$active_size" -gt 0 ] || return 0

    archive_path=$(next_archive_log_path "$archive_date")
    mv "$LOG_FILE" "$archive_path"
    : > "$LOG_FILE"
}

cleanup_old_log_days()
{
    days_file="$STATE_DIR/log-days.$$"
    days_next="$days_file.next"
    active_date=$(sed -n '1p' "$LOG_DATE_FILE" 2>/dev/null || true)

    {
        for archived_log in "$LOG_DIR"/face_media_[0-9]*.log; do
            [ -f "$archived_log" ] || continue
            archive_name=${archived_log##*/}
            archive_stem=${archive_name#face_media_}
            archive_stem=${archive_stem%.log}
            archive_day=${archive_stem%%_*}
            if valid_log_date "$archive_day"; then
                echo "$archive_day"
            fi
        done
        if valid_log_date "$active_date"; then
            echo "$active_date"
        fi
    } | sort -u > "$days_file"

    day_count=$(wc -l < "$days_file" 2>/dev/null | tr -d ' ')
    while [ "${day_count:-0}" -gt "$LOG_RETENTION_DAYS" ]; do
        oldest_day=$(sed -n '1p' "$days_file")
        if ! valid_log_date "$oldest_day"; then
            break
        fi

        rm -f "$LOG_DIR/face_media_${oldest_day}.log"
        for old_part in "$LOG_DIR/face_media_${oldest_day}_"*.log; do
            [ -f "$old_part" ] || continue
            rm -f "$old_part"
        done

        sed '1d' "$days_file" > "$days_next"
        mv "$days_next" "$days_file"
        day_count=$((day_count - 1))
        log "removed the oldest Qt log day: $oldest_day"
    done

    rm -f "$days_file" "$days_next" 2>/dev/null || true
}

prepare_log_storage()
{
    install -d -m 0755 "$LOG_DIR"

    today=$(date '+%Y%m%d')
    stored_date=$(sed -n '1p' "$LOG_DATE_FILE" 2>/dev/null || true)
    active_size=$(get_file_size "$LOG_FILE")

    if ! valid_log_date "$stored_date"; then
        stored_date=""
        if [ "$active_size" -gt 0 ]; then
            stored_date=$(date -r "$LOG_FILE" '+%Y%m%d' 2>/dev/null || true)
            valid_log_date "$stored_date" || stored_date=$today
        fi
    fi

    if [ -n "$stored_date" ] && [ "$stored_date" != "$today" ]; then
        archive_active_log "$stored_date"
    fi

    [ -e "$LOG_FILE" ] || : > "$LOG_FILE"
    printf '%s\n' "$today" > "$LOG_DATE_FILE"
    cleanup_old_log_days
}

log_maintenance_loop()
{
    while true; do
        sleep 60
        cleanup_old_log_days
    done
}

start_log_maintenance()
{
    log_maintenance_loop &
    LOG_MAINTENANCE_PID=$!
}

start_qt_log_writer()
{
    rm -f "$LOG_FIFO" 2>/dev/null || true
    mkfifo "$LOG_FIFO"

    initial_date=$(sed -n '1p' "$LOG_DATE_FILE" 2>/dev/null || date '+%Y%m%d')
    initial_size=$(get_file_size "$LOG_FILE")

    # awk 直接消费 Qt 的 stdout/stderr，保持原始行内容；只在日志风暴时
    # 添加一次限流说明并采样，避免错误循环迅速耗尽存储空间。
    LC_ALL=C awk \
        -v active="$LOG_FILE" \
        -v logdir="$LOG_DIR" \
        -v statefile="$LOG_DATE_FILE" \
        -v day="$initial_date" \
        -v size="$initial_size" \
        -v maxsize="$LOG_MAX_SIZE" \
        -v burstlimit="$LOG_BURST_LIMIT" \
        -v maxline="$LOG_MAX_LINE_SIZE" '
        function file_exists(path, command) {
            command = "[ -e \"" path "\" ]"
            return system(command) == 0
        }
        function next_archive_path(logday, path, index) {
            path = logdir "/face_media_" logday ".log"
            if (!file_exists(path)) return path
            index = 1
            while (file_exists(logdir "/face_media_" logday "_" index ".log")) index++
            return logdir "/face_media_" logday "_" index ".log"
        }
        function rotate_active(logday, destination, command, result) {
            if (size <= 0) return
            close(active)
            destination = next_archive_path(logday)
            command = "mv -f \"" active "\" \"" destination "\""
            result = system(command)
            if (result == 0) size = 0
        }
        function save_active_day(value) {
            print value > statefile
            close(statefile)
        }
        function append_line(text, bytes) {
            bytes = length(text) + 1
            if (size > 0 && size + bytes > maxsize) rotate_active(day)
            print text >> active
            fflush(active)
            size += bytes
        }
        BEGIN {
            minute_key = -1
            minute_bytes = 0
            limited = 0
            last_sample_second = -1
        }
        {
            now = systime()
            current_day = strftime("%Y%m%d", now)
            if (current_day != day) {
                rotate_active(day)
                day = current_day
                save_active_day(day)
                minute_key = -1
            }

            text = $0
            raw_bytes = length(text) + 1
            current_minute = int(now / 60)
            if (current_minute != minute_key) {
                minute_key = current_minute
                minute_bytes = 0
                limited = 0
                last_sample_second = -1
            }
            minute_bytes += raw_bytes

            if (raw_bytes > maxline) {
                append_line(substr(text, 1, maxline) " [line truncated by face-media logger]")
                next
            }

            if (minute_bytes <= burstlimit) {
                append_line(text)
                next
            }

            if (!limited) {
                append_line("[face-media-log] log growth exceeded 10MB in one minute; high-frequency output is now sampled")
                limited = 1
            }

            # 超过每分钟10MB后每秒仍保留一条原始打印；下一分钟自动恢复，
            # 防止完全关闭日志后丢失后续正常状态信息。
            if (now != last_sample_second) {
                append_line(text)
                last_sample_second = now
            }
        }
    ' < "$LOG_FIFO" &
    LOGGER_PID=$!

    # awk 是 FIFO 唯一读端。读端在后台等待后再打开监督进程写端，避免原先
    # O_RDWR 持有方式产生额外读端，也避免日志数据滞留在无人读取的描述符上。
    exec 3> "$LOG_FIFO"
    LOG_PIPE_OPEN=1
    LOG_CAPTURE_MODE=pipe

    # 通过同一条管道写入握手标记，并确认消费者确实写到了目标文件。
    # 若 awk 在目标系统上不兼容，立即报错，避免继续静默运行空日志。
    logger_probe="[face-media-log] capture started: $(date '+%Y-%m-%d %H:%M:%S')"
    printf '%s\n' "$logger_probe" >&3
    attempts=0
    while [ "$attempts" -lt 20 ]; do
        if ! pid_is_running "$LOGGER_PID"; then
            log "Qt log writer exited during startup"
            exec 3>&-
            LOG_PIPE_OPEN=0
            wait "$LOGGER_PID" 2>/dev/null || true
            LOGGER_PID=""
            LOG_CAPTURE_MODE=none
            rm -f "$LOG_FIFO" 2>/dev/null || true
            return 1
        fi
        if tail -n 8 "$LOG_FILE" 2>/dev/null | grep -Fqx "$logger_probe"; then
            return 0
        fi
        sleep 0.05
        attempts=$((attempts + 1))
    done

    log "Qt log writer did not acknowledge the startup probe"
    exec 3>&-
    LOG_PIPE_OPEN=0
    wait "$LOGGER_PID" 2>/dev/null || true
    LOGGER_PID=""
    LOG_CAPTURE_MODE=none
    rm -f "$LOG_FIFO" 2>/dev/null || true
    return 1
}

finish_qt_log_writer()
{
    writer_pid=${LOGGER_PID:-}
    [ -n "$writer_pid" ] || return 0

    # Qt 已退出后关闭监督进程持有的写端，使 awk 收到 EOF 并正常落盘退出。
    if [ "$LOG_PIPE_OPEN" -eq 1 ]; then
        exec 3>&-
        LOG_PIPE_OPEN=0
    fi

    attempts=0
    while pid_is_running "$writer_pid" && [ "$attempts" -lt 30 ]; do
        sleep 0.1
        attempts=$((attempts + 1))
    done
    if pid_is_running "$writer_pid"; then
        kill "$writer_pid" 2>/dev/null || true
    fi
    wait "$writer_pid" 2>/dev/null || true
    LOGGER_PID=""
    LOG_CAPTURE_MODE=none
    rm -f "$LOG_FIFO" 2>/dev/null || true
}

append_supervisor_log()
{
    log_line="[face-media] $*"
    if [ "$LOG_CAPTURE_MODE" = "pipe" ] && [ "$LOG_PIPE_OPEN" -eq 1 ]; then
        printf '%s\n' "$log_line" >&3 2>/dev/null || true
    else
        printf '%s\n' "$log_line" >> "$LOG_FILE" 2>/dev/null || true
    fi
    log "$*"
}

pid_is_running()
{
    candidate_pid=${1:-}
    case "$candidate_pid" in
        ''|0|*[!0-9]*) return 1 ;;
    esac
    [ -r "/proc/$candidate_pid/stat" ] || return 1
    kill -0 "$candidate_pid" 2>/dev/null || return 1
    state=$(awk '{print $3}' "/proc/$candidate_pid/stat" 2>/dev/null || true)
    [ "$state" != "Z" ]
}

wait_for_weston()
{
    attempts=0
    while [ "$attempts" -lt 300 ]; do
        if systemctl is-active --quiet "$WESTON_SERVICE" \
                && [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
            if ! command -v weston-info >/dev/null 2>&1 \
                    || XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
                       WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
                       weston-info >/dev/null 2>&1; then
                return 0
            fi
        fi
        attempts=$((attempts + 1))
        sleep 0.1
    done
    return 1
}

wait_for_maintenance_end()
{
    announced=0
    while [ -e "$MAINTENANCE_FILE" ]; do
        if [ "$announced" -eq 0 ]; then
            reason=$(sed -n '1p' "$MAINTENANCE_FILE" 2>/dev/null || true)
            log "Qt restart paused for maintenance: ${reason:-unknown}"
            announced=1
        fi
        owner_pid=${reason%%:*}
        case "$owner_pid" in
            ''|*[!0-9]*) ;;
            *)
                if ! kill -0 "$owner_pid" 2>/dev/null; then
                    log "removing stale maintenance state left by pid=$owner_pid"
                    rm -f "$MAINTENANCE_FILE" 2>/dev/null || true
                    break
                fi
                ;;
        esac
        sleep 0.1
    done
}

cleanup()
{
    status=$?
    trap - EXIT HUP INT TERM

    if pid_is_running "$LOG_MAINTENANCE_PID"; then
        kill "$LOG_MAINTENANCE_PID" 2>/dev/null || true
        wait "$LOG_MAINTENANCE_PID" 2>/dev/null || true
    fi

    if pid_is_running "$APP_PID"; then
        kill -2 "$APP_PID" 2>/dev/null || true
        sleep 1
    fi
    if pid_is_running "$APP_PID"; then
        kill -9 "$APP_PID" 2>/dev/null || true
    fi
    if [ -n "$APP_PID" ]; then
        wait "$APP_PID" 2>/dev/null || true
    fi
    finish_qt_log_writer
    rm -f "$APP_PID_FILE" 2>/dev/null || true

    if pid_is_running "$MQTT_PID"; then
        kill "$MQTT_PID" 2>/dev/null || true
        wait "$MQTT_PID" 2>/dev/null || true
    fi
    exit "$status"
}

stop_stale_qt_instances()
{
    stale_pids=""
    if command -v pgrep >/dev/null 2>&1; then
        stale_pids=$(pgrep -x MergedQtApp 2>/dev/null || true)
    elif command -v pidof >/dev/null 2>&1; then
        stale_pids=$(pidof MergedQtApp 2>/dev/null || true)
    fi
    [ -n "$stale_pids" ] || return 0

    append_supervisor_log "stopping Qt instances not owned by this supervisor: $stale_pids"
    for stale_pid in $stale_pids; do
        kill -2 "$stale_pid" 2>/dev/null || true
    done
    attempts=0
    while [ "$attempts" -lt 50 ]; do
        remaining=""
        for stale_pid in $stale_pids; do
            if pid_is_running "$stale_pid"; then
                remaining="$remaining $stale_pid"
            fi
        done
        [ -z "$remaining" ] && return 0
        attempts=$((attempts + 1))
        sleep 0.1
    done
    for stale_pid in $stale_pids; do
        if pid_is_running "$stale_pid"; then
            kill -9 "$stale_pid" 2>/dev/null || true
        fi
    done
}

ensure_runtime_environment()
{
    PROFILE_FILE=${FACE_MEDIA_PROFILE_FILE:-/etc/profile}

    # Weston 服务已经先行写入 /etc/profile。systemd 服务不会自动读取它，
    # 因此 Qt 监督进程仍需主动加载一次，让 mqttd 和 Qt 子进程继承变量。
    if [ -r "$PROFILE_FILE" ]; then
        set +u
        . "$PROFILE_FILE"
        set -u
    else
        log "runtime profile is missing: $PROFILE_FILE"
    fi

    # 服务依赖异常或配置块缺失时保留安全默认值，避免 Qt 因空变量无法连接 Weston。
    : "${QT_QPA_PLATFORM:=wayland-egl}"
    : "${XDG_RUNTIME_DIR:=/run/user/0}"
    : "${WAYLAND_DISPLAY:=wayland-0}"
    : "${QT_WAYLAND_SHELL_INTEGRATION:=wl-shell}"
    : "${QT_WAYLAND_DISABLE_WINDOWDECORATION:=1}"
    : "${LD_PRELOAD:=/lib/aarch64-linux-gnu/libgomp.so.1}"
    : "${QT_PLUGIN_PATH:=/usr/lib/aarch64-linux-gnu/qt5/plugins}"
    : "${QML2_IMPORT_PATH:=/usr/lib/aarch64-linux-gnu/qt5/qml}"
    : "${QT_QPA_PLATFORM_PLUGIN_PATH:=/usr/lib/aarch64-linux-gnu/qt5/plugins/platforms}"
    case ":${LD_LIBRARY_PATH:-}:" in
        *:/usr/lib/aarch64-linux-gnu/mali:*) ;;
        *) LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu/mali${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
    esac

    export QT_QPA_PLATFORM XDG_RUNTIME_DIR WAYLAND_DISPLAY
    export QT_WAYLAND_SHELL_INTEGRATION QT_WAYLAND_DISABLE_WINDOWDECORATION
    export LD_PRELOAD QT_PLUGIN_PATH QML2_IMPORT_PATH
    export QT_QPA_PLATFORM_PLUGIN_PATH LD_LIBRARY_PATH
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

cp /usr/lib/aarch64-linux-gnu/librknnrt.so /usr/lib/

ensure_runtime_environment

if [ ! -d "$APP_DIR" ]; then
    log "application directory not found: $APP_DIR"
    exit 1
fi

cd "$APP_DIR"
install -d -m 0755 "$STATE_DIR"
rm -f "$APP_PID_FILE" 2>/dev/null || true
trap cleanup EXIT HUP INT TERM

prepare_log_storage
start_log_maintenance

load_driver 2h_sr505_drv.ko

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
MQTT_PID=$!

if command -v udevadm >/dev/null 2>&1; then
    udevadm settle --timeout=10 || log "udev settle timed out; continuing"
fi

if ! pid_is_running "$MQTT_PID"; then
    wait "$MQTT_PID" || true
    log "mqttd exited before MergedQtApp was started"
    exit 1
fi

log "waiting for Weston before the initial Qt launch"
until wait_for_weston; do
    log "Weston is not ready; retrying"
    sleep 2
done

log "waiting ${APP_START_DELAY_SECONDS}s before the initial Qt launch"
sleep "$APP_START_DELAY_SECONDS"

while true; do
    wait_for_maintenance_end

    if ! wait_for_weston; then
        log "Weston became unavailable; Qt launch deferred"
        sleep 2
        continue
    fi

    # 维护状态可能在 Weston 就绪检查后刚刚建立，启动前必须再次确认。
    if [ -e "$MAINTENANCE_FILE" ]; then
        continue
    fi

    log "starting MergedQtApp"
    if ! start_qt_log_writer; then
        # 日志轮转器故障不能阻塞业务程序。降级时直接追加到活动日志；下次
        # Qt 重启仍会重新尝试恢复按日/按大小轮转。
        LOG_CAPTURE_MODE=direct
        append_supervisor_log "advanced Qt log capture unavailable; using direct append mode"
    fi

    # 仅在已经具备可用日志输出路径、马上要启动受管 Qt 时清理遗留实例。
    # 避免日志消费者异常期间的重试循环反复终止人工诊断进程。
    stop_stale_qt_instances
    if [ -e "$MAINTENANCE_FILE" ]; then
        finish_qt_log_writer
        continue
    fi

    if [ "$LOG_CAPTURE_MODE" = "pipe" ]; then
        ./MergedQtApp >&3 2>&1 &
    else
        ./MergedQtApp >> "$LOG_FILE" 2>&1 &
    fi
    APP_PID=$!

    # 若维护事务与进程创建同时发生，立即撤销本次启动，避免旋转期间漏出实例。
    if [ -e "$MAINTENANCE_FILE" ]; then
        kill -2 "$APP_PID" 2>/dev/null || true
        wait "$APP_PID" 2>/dev/null || true
        finish_qt_log_writer
        APP_PID=""
        continue
    fi

    printf '%s\n' "$APP_PID" > "$APP_PID_FILE"
    log "MergedQtApp started: pid=$APP_PID"

    set +e
    wait "$APP_PID"
    app_status=$?
    set -e
    finish_qt_log_writer
    rm -f "$APP_PID_FILE" 2>/dev/null || true
    APP_PID=""

    if [ -e "$MAINTENANCE_FILE" ]; then
        log "MergedQtApp stopped for planned maintenance"
        continue
    fi

    append_supervisor_log "MergedQtApp exited unexpectedly: status=$app_status; restarting in ${APP_RESTART_DELAY_SECONDS}s"
    sleep "$APP_RESTART_DELAY_SECONDS"
done
