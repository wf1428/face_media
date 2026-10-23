#!/bin/sh
# @file start_face_media.sh
# @brief 初始化运行环境并监督 mqttd、Qt 程序及看门狗。
# @author Du lin
# @note 最近修改：2026-10-23；修改人：Du lin；在 SR505 驱动加载后，
#       复用 load_driver 加载同一 APP_DIR 下的 2h_led_drv.ko；沿用已有模块检查和失败退出行为。

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
NET_CONFIG_FILE=${FACE_MEDIA_NET_CONFIG_FILE:-/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini}
HEARTBEAT_FILE=${FACE_MEDIA_HEARTBEAT_FILE:-$STATE_DIR/heartbeat}
WATCHDOG_RESTART_HISTORY=${FACE_MEDIA_WATCHDOG_RESTART_HISTORY:-$STATE_DIR/watchdog-restarts}
HARDWARE_WATCHDOG_DEVICE=${FACE_MEDIA_HARDWARE_WATCHDOG_DEVICE:-/dev/watchdog}
WATCHDOG_CHECK_INTERVAL_SECONDS=10
WATCHDOG_START_GRACE_SECONDS=60
WATCHDOG_MISSED_LIMIT=6
WATCHDOG_RESTART_WINDOW_SECONDS=300
WATCHDOG_RESTART_LIMIT=3
LOG_DIR=/var/log/face-media
LOG_FILE=$LOG_DIR/face_media.log
LOG_DATE_FILE=$LOG_DIR/.active_date
LOG_FIFO=$STATE_DIR/qt-log.fifo
LOG_ACK_FILE=$STATE_DIR/qt-log-ack.$$
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
WATCHDOG_TIMEOUT_RESTART=0
WATCHDOG_RESTART_ATTEMPT=0
WATCHDOG_RECOVERY_PENDING=0
WATCHDOG_CONSECUTIVE_FAILURES=0
APP_LAUNCH_HEALTHY=0
APP_LAUNCH_FAILURE_RECORDED=0

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
    rm -f "$LOG_FIFO" "$LOG_ACK_FILE" 2>/dev/null || true
    mkfifo "$LOG_FIFO"

    initial_date=$(sed -n '1p' "$LOG_DATE_FILE" 2>/dev/null || date '+%Y%m%d')
    initial_size=$(get_file_size "$LOG_FILE")
    logger_probe="[face-media-log] capture started: $(date '+%Y-%m-%d %H:%M:%S') pid=$$"

    # mawk 从 FIFO 默认按块读取：探针可能直到写端关闭后才被处理。
    # 仅对 mawk 启用逐行读取；其它 awk 保持原有调用方式。
    awk_input_mode=
    if awk -W version 2>&1 | grep -q '^mawk '; then
        awk_input_mode='-W interactive'
    fi

    # awk 直接消费 Qt 的 stdout/stderr，保持原始行内容；只在日志风暴时
    # 添加一次限流说明并采样，避免错误循环迅速耗尽存储空间。
    LC_ALL=C awk $awk_input_mode \
        -v active="$LOG_FILE" \
        -v logdir="$LOG_DIR" \
        -v statefile="$LOG_DATE_FILE" \
        -v day="$initial_date" \
        -v size="$initial_size" \
        -v maxsize="$LOG_MAX_SIZE" \
        -v burstlimit="$LOG_BURST_LIMIT" \
        -v maxline="$LOG_MAX_LINE_SIZE" \
        -v probe="$logger_probe" \
        -v ackfile="$LOG_ACK_FILE" '
        function file_exists(path, command) {
            command = "[ -e \"" path "\" ]"
            return system(command) == 0
        }
        function next_archive_path(logday, path, archive_index) {
            path = logdir "/face_media_" logday ".log"
            if (!file_exists(path)) return path
            archive_index = 1
            while (file_exists(logdir "/face_media_" logday "_" archive_index ".log")) archive_index++
            return logdir "/face_media_" logday "_" archive_index ".log"
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
            if (text == probe) {
                append_line(text)
                print "ready" > ackfile
                close(ackfile)
                next
            }
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

    # 消费者写入并刷新日志后才创建确认文件；不再反复扫描活动日志的最后 8 行。
    printf '%s\n' "$logger_probe" >&3
    if wait_for_log_writer_ack "awk"; then
        return 0
    fi
    finish_qt_log_writer
    return 1
}

wait_for_log_writer_ack()
{
    writer_kind=$1
    attempts=0
    # 板端启动和校时时可能同时触发归档；等待写入完成，最多约 1 秒。
    while [ "$attempts" -lt 10 ]; do
        if [ -s "$LOG_ACK_FILE" ]; then
            rm -f "$LOG_ACK_FILE" 2>/dev/null || true
            return 0
        fi
        if ! pid_is_running "$LOGGER_PID"; then
            log "$writer_kind Qt log writer exited during startup"
            return 1
        fi
        sleep 0.1
        attempts=$((attempts + 1))
    done
    log "$writer_kind Qt log writer did not acknowledge the startup probe"
    return 1
}

# awk 无法启动时使用同一个 FIFO；后备写入器仍按日期、50 MiB、限流规则处理。
shell_rotate_active_log()
{
    [ "$shell_size" -gt 0 ] || return 0
    shell_archive=$(next_archive_log_path "$shell_day") || return 1
    mv "$LOG_FILE" "$shell_archive" || return 1
    : > "$LOG_FILE" || return 1
    shell_size=0
}

shell_append_log_line()
{
    shell_line=$1
    shell_bytes=$((${#shell_line} + 1))
    if [ "$shell_size" -gt 0 ] &&
       [ $((shell_size + shell_bytes)) -gt "$LOG_MAX_SIZE" ]; then
        shell_rotate_active_log || return 1
    fi
    printf '%s\n' "$shell_line" >> "$LOG_FILE" || return 1
    shell_size=$((shell_size + shell_bytes))
}

shell_qt_log_writer()
{
    LC_ALL=C
    export LC_ALL
    shell_day=$(sed -n '1p' "$LOG_DATE_FILE" 2>/dev/null || date '+%Y%m%d')
    shell_size=$(get_file_size "$LOG_FILE")
    shell_minute_key=-1
    shell_minute_bytes=0
    shell_limited=0
    shell_last_sample=-1
    while IFS= read -r text || [ -n "$text" ]; do
        set -- $(date '+%Y%m%d %s')
        current_day=$1
        now=$2
        if [ "$current_day" != "$shell_day" ]; then
            shell_rotate_active_log || return 1
            shell_day=$current_day
            printf '%s\n' "$shell_day" > "$LOG_DATE_FILE" || return 1
            shell_minute_key=-1
        fi

        if [ "$text" = "$logger_probe" ]; then
            shell_append_log_line "$text" || return 1
            printf 'ready\n' > "$LOG_ACK_FILE" || return 1
            continue
        fi

        raw_bytes=$((${#text} + 1))
        current_minute=$((now / 60))
        if [ "$current_minute" -ne "$shell_minute_key" ]; then
            shell_minute_key=$current_minute
            shell_minute_bytes=0
            shell_limited=0
            shell_last_sample=-1
        fi
        shell_minute_bytes=$((shell_minute_bytes + raw_bytes))

        if [ "$raw_bytes" -gt "$LOG_MAX_LINE_SIZE" ]; then
            text="$(printf '%.65536s' "$text") [line truncated by face-media logger]"
            shell_append_log_line "$text" || return 1
            continue
        fi
        if [ "$shell_minute_bytes" -le "$LOG_BURST_LIMIT" ]; then
            shell_append_log_line "$text" || return 1
            continue
        fi
        if [ "$shell_limited" -eq 0 ]; then
            shell_append_log_line '[face-media-log] log growth exceeded 10MB in one minute; high-frequency output is now sampled' || return 1
            shell_limited=1
        fi
        if [ "$now" -ne "$shell_last_sample" ]; then
            shell_append_log_line "$text" || return 1
            shell_last_sample=$now
        fi
    done
}

start_shell_qt_log_writer()
{
    rm -f "$LOG_FIFO" "$LOG_ACK_FILE" 2>/dev/null || true
    mkfifo "$LOG_FIFO" || return 1
    logger_probe="[face-media-log] shell capture started: $(date '+%Y-%m-%d %H:%M:%S') pid=$$"
    shell_qt_log_writer < "$LOG_FIFO" &
    LOGGER_PID=$!
    exec 3> "$LOG_FIFO"
    LOG_PIPE_OPEN=1
    LOG_CAPTURE_MODE=pipe
    printf '%s\n' "$logger_probe" >&3
    if wait_for_log_writer_ack "shell"; then
        return 0
    fi
    finish_qt_log_writer
    return 1
}

finish_qt_log_writer()
{
    writer_pid=${LOGGER_PID:-}
    if [ -z "$writer_pid" ]; then
        LOG_CAPTURE_MODE=none
        rm -f "$LOG_ACK_FILE" 2>/dev/null || true
        return 0
    fi

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
    rm -f "$LOG_FIFO" "$LOG_ACK_FILE" 2>/dev/null || true
}

append_bounded_supervisor_line()
{
    bounded_line=$1
    bounded_day=$(sed -n '1p' "$LOG_DATE_FILE" 2>/dev/null || true)
    current_day=$(date '+%Y%m%d')
    if valid_log_date "$bounded_day" && [ "$bounded_day" != "$current_day" ]; then
        archive_active_log "$bounded_day" || return 1
        printf '%s\n' "$current_day" > "$LOG_DATE_FILE" || return 1
        bounded_day=$current_day
    elif ! valid_log_date "$bounded_day"; then
        bounded_day=$current_day
        printf '%s\n' "$bounded_day" > "$LOG_DATE_FILE" || return 1
    fi
    bounded_size=$(get_file_size "$LOG_FILE")
    bounded_bytes=$(printf '%s\n' "$bounded_line" | wc -c | tr -d ' ')
    if [ "$bounded_size" -gt 0 ] &&
       [ $((bounded_size + bounded_bytes)) -gt "$LOG_MAX_SIZE" ]; then
        archive_active_log "$bounded_day" || return 1
    fi
    printf '%s\n' "$bounded_line" >> "$LOG_FILE"
}

append_supervisor_log()
{
    log_line="[face-media] $*"
    if [ "$LOG_CAPTURE_MODE" = "pipe" ] && [ "$LOG_PIPE_OPEN" -eq 1 ]; then
        printf '%s\n' "$log_line" >&3 2>/dev/null || true
    elif [ "$LOG_CAPTURE_MODE" != "journal" ]; then
        append_bounded_supervisor_line "$log_line" 2>/dev/null || true
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

pid_is_managed_qt()
{
    candidate_pid=${1:-}
    pid_is_running "$candidate_pid" || return 1

    process_name=$(sed -n '1p' "/proc/$candidate_pid/comm" 2>/dev/null || true)
    [ "$process_name" = "MergedQtApp" ] || return 1

    process_exe=$(readlink "/proc/$candidate_pid/exe" 2>/dev/null || true)
    case "$process_exe" in
        "$APP_DIR/MergedQtApp"|"$APP_DIR/MergedQtApp (deleted)") return 0 ;;
        *) return 1 ;;
    esac
}

qt_watchdog_enabled()
{
    # 配置文件缺失或配置值无效时默认启用，确保正式部署仍具备卡死恢复能力。
    [ -r "$NET_CONFIG_FILE" ] || return 0

    watchdog_value=$(awk '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        /^[[:space:]]*\[/ {
            section=tolower($0)
            gsub(/[[:space:]]/, "", section)
            in_watchdog=(section == "[qt_watchdog]")
            next
        }
        in_watchdog && index($0, "=") > 0 {
            line=$0
            sub(/\r$/, "", line)
            key=tolower(trim(substr(line, 1, index(line, "=") - 1)))
            if (key != "enabled") next
            value=substr(line, index(line, "=") + 1)
            sub(/[;#].*$/, "", value)
            print tolower(trim(value))
            exit
        }
    ' "$NET_CONFIG_FILE" 2>/dev/null || true)

    case "$watchdog_value" in
        false|0|no|off) return 1 ;;
        *) return 0 ;;
    esac
}

read_qt_heartbeat()
{
    [ -r "$HEARTBEAT_FILE" ] || return 1
    heartbeat_line=$(sed -n '1p' "$HEARTBEAT_FILE" 2>/dev/null || true)
    [ -n "$heartbeat_line" ] || return 1

    # 新格式为“PID 序号 Unix时间”。兼容升级过程中的旧版单时间戳格式，
    # 但只接受新格式中 PID 与当前受管 Qt 一致的心跳，拒绝残留实例干扰。
    set -- $heartbeat_line
    if [ "$#" -ge 3 ] && [ "$1" != "$APP_PID" ]; then
        return 1
    fi
    printf '%s\n' "$heartbeat_line"
}

register_watchdog_restart()
{
    restart_now=$(date '+%s')
    restart_cutoff=$((restart_now - WATCHDOG_RESTART_WINDOW_SECONDS))
    restart_tmp="$WATCHDOG_RESTART_HISTORY.$$"

    if [ -r "$WATCHDOG_RESTART_HISTORY" ]; then
        awk -v cutoff="$restart_cutoff" '
            $1 ~ /^[0-9]+$/ && $1 >= cutoff { print $1 }
        ' "$WATCHDOG_RESTART_HISTORY" > "$restart_tmp"
    else
        : > "$restart_tmp"
    fi

    restart_count=$(wc -l < "$restart_tmp" 2>/dev/null | tr -d ' ')
    WATCHDOG_RESTART_ATTEMPT=$((restart_count + 1))
    if [ "${restart_count:-0}" -ge "$WATCHDOG_RESTART_LIMIT" ]; then
        rm -f "$restart_tmp" 2>/dev/null || true
        return 1
    fi

    printf '%s\n' "$restart_now" >> "$restart_tmp"
    mv -f "$restart_tmp" "$WATCHDOG_RESTART_HISTORY"
    return 0
}

record_failed_watchdog_recovery()
{
    [ "$WATCHDOG_RECOVERY_PENDING" -eq 1 ] || return 0
    [ "$APP_LAUNCH_HEALTHY" -eq 0 ] || return 0
    [ "$APP_LAUNCH_FAILURE_RECORDED" -eq 0 ] || return 0

    APP_LAUNCH_FAILURE_RECORDED=1
    WATCHDOG_CONSECUTIVE_FAILURES=$((WATCHDOG_CONSECUTIVE_FAILURES + 1))
    if [ "$WATCHDOG_CONSECUTIVE_FAILURES" -ge "$WATCHDOG_RESTART_LIMIT" ]; then
        trigger_system_recovery
    fi
}

arm_hardware_watchdog()
{
    [ -c "$HARDWARE_WATCHDOG_DEVICE" ] || return 1
    [ -w "$HARDWARE_WATCHDOG_DEVICE" ] || return 1

    # 打开后保持描述符且不再喂狗。若常规系统重启路径失效，硬件看门狗
    # 会在其板级超时时间到达后完成最终复位。
    exec 9> "$HARDWARE_WATCHDOG_DEVICE" 2>/dev/null
}

trigger_system_recovery()
{
    arm_hardware_watchdog || true
    sync

    if command -v systemctl >/dev/null 2>&1; then
        systemctl reboot >/dev/null 2>&1 || true
        sleep 20
    fi
    if command -v reboot >/dev/null 2>&1; then
        reboot -f >/dev/null 2>&1 || true
        sleep 20
    fi
    if [ -w /proc/sysrq-trigger ]; then
        printf 'b\n' > /proc/sysrq-trigger 2>/dev/null || true
    fi

    # 不退出监督进程，避免关闭 /dev/watchdog 描述符而取消硬件复位。
    while true; do
        sleep 60
    done
}

stop_qt_for_watchdog()
{
    watchdog_pid=$1
    pid_is_managed_qt "$watchdog_pid" || return 1

    kill -TERM "$watchdog_pid" 2>/dev/null || true
    stop_attempts=0
    while pid_is_managed_qt "$watchdog_pid" && [ "$stop_attempts" -lt 50 ]; do
        sleep 0.1
        stop_attempts=$((stop_attempts + 1))
    done
    if ! pid_is_managed_qt "$watchdog_pid"; then
        return 0
    fi

    kill -KILL "$watchdog_pid" 2>/dev/null || true
    stop_attempts=0
    while pid_is_managed_qt "$watchdog_pid" && [ "$stop_attempts" -lt 30 ]; do
        sleep 0.1
        stop_attempts=$((stop_attempts + 1))
    done
    ! pid_is_managed_qt "$watchdog_pid"
}

monitor_qt_process()
{
    WATCHDOG_TIMEOUT_RESTART=0
    APP_LAUNCH_HEALTHY=0
    APP_LAUNCH_FAILURE_RECORDED=0
    heartbeat_started_at=$(date '+%s')
    heartbeat_next_check=$((heartbeat_started_at + WATCHDOG_CHECK_INTERVAL_SECONDS))
    heartbeat_config_next_check=$((heartbeat_started_at + WATCHDOG_CHECK_INTERVAL_SECONDS))
    heartbeat_missed=0
    heartbeat_updates=0
    heartbeat_last=""
    heartbeat_maintenance_seen=0
    if qt_watchdog_enabled; then
        heartbeat_enabled=1
    else
        heartbeat_enabled=0
    fi

    while pid_is_running "$APP_PID"; do
        heartbeat_now=$(date '+%s')

        # 旋转屏幕、U盘更新等维护事务拥有最高优先级。维护期间不累计漏失，
        # 维护结束后重新给予60秒宽限，杜绝维护操作触发误杀。
        if [ -e "$MAINTENANCE_FILE" ]; then
            heartbeat_maintenance_seen=1
            heartbeat_missed=0
            heartbeat_updates=0
            heartbeat_last=""
            sleep 1
            continue
        fi
        if [ "$heartbeat_maintenance_seen" -eq 1 ]; then
            heartbeat_maintenance_seen=0
            heartbeat_started_at=$heartbeat_now
            heartbeat_next_check=$((heartbeat_now + WATCHDOG_CHECK_INTERVAL_SECONDS))
            heartbeat_config_next_check=$heartbeat_now
            heartbeat_missed=0
            heartbeat_last=""
        fi

        # 配置开关允许运行中修改，但最多每10秒读取一次，避免常驻脚本
        # 每秒启动awk读取配置文件。
        if [ "$heartbeat_now" -ge "$heartbeat_config_next_check" ]; then
            heartbeat_config_next_check=$((heartbeat_now + WATCHDOG_CHECK_INTERVAL_SECONDS))
            if qt_watchdog_enabled; then
                if [ "$heartbeat_enabled" -eq 0 ]; then
                    heartbeat_enabled=1
                    heartbeat_started_at=$heartbeat_now
                    heartbeat_next_check=$((heartbeat_now + WATCHDOG_CHECK_INTERVAL_SECONDS))
                    heartbeat_missed=0
                    heartbeat_updates=0
                    heartbeat_last=""
                fi
            else
                heartbeat_enabled=0
                heartbeat_missed=0
                heartbeat_updates=0
                heartbeat_last=""
                WATCHDOG_RECOVERY_PENDING=0
                WATCHDOG_CONSECUTIVE_FAILURES=0
                rm -f "$WATCHDOG_RESTART_HISTORY" 2>/dev/null || true
            fi
        fi
        if [ "$heartbeat_enabled" -eq 0 ]; then
            sleep 1
            continue
        fi

        # 启动后的首次心跳只建立基线，不计作一次检查。
        if [ -z "$heartbeat_last" ]; then
            heartbeat_candidate=$(read_qt_heartbeat || true)
            if [ -n "$heartbeat_candidate" ]; then
                heartbeat_last=$heartbeat_candidate
            fi
        fi

        if [ "$heartbeat_now" -lt "$heartbeat_next_check" ]; then
            sleep 1
            continue
        fi
        heartbeat_next_check=$((heartbeat_now + WATCHDOG_CHECK_INTERVAL_SECONDS))
        heartbeat_current=$(read_qt_heartbeat || true)

        if [ -n "$heartbeat_current" ] && [ "$heartbeat_current" != "$heartbeat_last" ]; then
            heartbeat_last=$heartbeat_current
            heartbeat_missed=0
            heartbeat_updates=$((heartbeat_updates + 1))
        else
            heartbeat_missed=$((heartbeat_missed + 1))
        fi

        heartbeat_elapsed=$((heartbeat_now - heartbeat_started_at))
        if [ "$APP_LAUNCH_HEALTHY" -eq 0 ] \
                && [ "$heartbeat_elapsed" -ge "$WATCHDOG_START_GRACE_SECONDS" ] \
                && [ "$heartbeat_updates" -gt 0 ]; then
            APP_LAUNCH_HEALTHY=1
            WATCHDOG_RECOVERY_PENDING=0
            WATCHDOG_CONSECUTIVE_FAILURES=0
        fi

        if [ "$heartbeat_elapsed" -ge "$WATCHDOG_START_GRACE_SECONDS" ] \
                && [ "$heartbeat_missed" -ge "$WATCHDOG_MISSED_LIMIT" ]; then
            record_failed_watchdog_recovery
            watchdog_process_state=$(awk '{print $3}' "/proc/$APP_PID/stat" 2>/dev/null || true)
            watchdog_stale_seconds=$((heartbeat_missed * WATCHDOG_CHECK_INTERVAL_SECONDS))

            # 5分钟内已经执行3次恢复时，仅停止继续重启；这不是触发系统
            # 重启的条件。窗口释放后才能再次尝试Qt恢复。
            if register_watchdog_restart; then
                WATCHDOG_TIMEOUT_RESTART=1
                WATCHDOG_RECOVERY_PENDING=1
                append_supervisor_log "Qt heartbeat timeout; restarting MergedQtApp: pid=$APP_PID state=${watchdog_process_state:-unknown} stale=${watchdog_stale_seconds}s missed=$heartbeat_missed/$WATCHDOG_MISSED_LIMIT restart=$WATCHDOG_RESTART_ATTEMPT/$WATCHDOG_RESTART_LIMIT"
            else
                heartbeat_started_at=$heartbeat_now
                heartbeat_next_check=$((heartbeat_now + WATCHDOG_CHECK_INTERVAL_SECONDS))
                heartbeat_missed=0
                continue
            fi
            stop_qt_for_watchdog "$APP_PID" || trigger_system_recovery
            return 0
        fi

        sleep 1
    done
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
    rm -f "$HEARTBEAT_FILE" 2>/dev/null || true

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
    export QT_YCEST_HEARTBEAT_FILE="$HEARTBEAT_FILE"
}

# Lightweight setup only: no WiFi detection or Web child in the Qt supervisor.
prepare_qt_web_environment()
{
    export MERGEDQT_WEB_ENABLE=1
    export MERGEDQT_WEB_SOCKET=${MERGEDQT_WEB_SOCKET:-/run/mergedqt/web.sock}
    export MERGEDQT_WEB_MEDIA_MOUNT=${MERGEDQT_WEB_MEDIA_MOUNT:-/mnt/UDISK}
    export MERGEDQT_WEB_IMPORTS=${MERGEDQT_WEB_IMPORTS:-/home/cat/web-stage/imports}
    export MERGEDQT_WEB_FACTORY=${MERGEDQT_WEB_FACTORY:-/home/cat/web-stage/factory-account.json}
    export MERGEDQT_WEB_READY_FILE=${MERGEDQT_WEB_READY_FILE:-/run/mergedqt/web-ready}
    export MERGEDQT_WEB_HOTSPOT_HELPER=${MERGEDQT_WEB_HOTSPOT_HELPER:-/usr/local/sbin/portal-network.sh}
    if ! install -d -m 0750 "$(dirname -- "$MERGEDQT_WEB_SOCKET")" "$(dirname -- "$MERGEDQT_WEB_READY_FILE")"; then
        log 'Optional Web runtime directory could not be prepared; continuing Qt startup'
    fi
    if ! install -d -m 0770 "$MERGEDQT_WEB_IMPORTS"; then
        log 'Optional Web imports directory could not be prepared; continuing Qt startup'
    fi
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
load_driver 2h_led_drv.ko

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
        if start_shell_qt_log_writer; then
            append_supervisor_log "advanced Qt log capture unavailable; using bounded shell capture"
        else
            # 两种文件采集器均不可用时交给 journald，避免活动文件无上限增长。
            LOG_CAPTURE_MODE=journal
            append_supervisor_log "Qt file log capture unavailable; using system journal"
        fi
    fi

    # 仅在已经具备可用日志输出路径、马上要启动受管 Qt 时清理遗留实例。
    # 避免日志消费者异常期间的重试循环反复终止人工诊断进程。
    stop_stale_qt_instances
    if [ -e "$MAINTENANCE_FILE" ]; then
        finish_qt_log_writer
        continue
    fi

    # 每次启动前删除上一个实例的心跳，禁止旧文件被当成新进程存活证据。
    rm -f "$HEARTBEAT_FILE" 2>/dev/null || true

    prepare_qt_web_environment
    rm -f "$MERGEDQT_WEB_READY_FILE" 2>/dev/null || true

    if [ "$LOG_CAPTURE_MODE" = "pipe" ]; then
        ./MergedQtApp >&3 2>&1 &
    else
        ./MergedQtApp &
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

    # 同一个监督循环同时观察Qt退出和主线程心跳；配置关闭时仅保留原有
    # wait/restart行为，不执行心跳判定、限频或系统级兜底。
    set +e
    monitor_qt_process
    wait "$APP_PID"
    app_status=$?
    set -e
    finish_qt_log_writer
    rm -f "$APP_PID_FILE" 2>/dev/null || true
    rm -f "$HEARTBEAT_FILE" 2>/dev/null || true
    APP_PID=""

    if [ -e "$MAINTENANCE_FILE" ]; then
        log "MergedQtApp stopped for planned maintenance"
        continue
    fi

    if [ "$WATCHDOG_TIMEOUT_RESTART" -eq 1 ]; then
        sleep "$APP_RESTART_DELAY_SECONDS"
        continue
    fi

    if qt_watchdog_enabled; then
        record_failed_watchdog_recovery

        # Qt 已退出但5分钟窗口内已有3次重启时，保持停止状态并等待窗口
        # 释放；限频本身绝不触发系统重启或硬件看门狗。
        restart_registered=0
        while qt_watchdog_enabled; do
            if [ -e "$MAINTENANCE_FILE" ]; then
                break
            fi
            if register_watchdog_restart; then
                restart_registered=1
                WATCHDOG_RECOVERY_PENDING=1
                break
            fi
            sleep "$WATCHDOG_CHECK_INTERVAL_SECONDS"
        done

        if [ -e "$MAINTENANCE_FILE" ]; then
            continue
        fi
        if [ "$restart_registered" -eq 0 ]; then
            # 等待期间配置被关闭，立即恢复原有无限自动拉起行为。
            WATCHDOG_RECOVERY_PENDING=0
            WATCHDOG_CONSECUTIVE_FAILURES=0
            rm -f "$WATCHDOG_RESTART_HISTORY" 2>/dev/null || true
        fi
    else
        WATCHDOG_RECOVERY_PENDING=0
        WATCHDOG_CONSECUTIVE_FAILURES=0
        rm -f "$WATCHDOG_RESTART_HISTORY" 2>/dev/null || true
    fi

    append_supervisor_log "MergedQtApp exited unexpectedly: status=$app_status; restarting in ${APP_RESTART_DELAY_SECONDS}s"
    sleep "$APP_RESTART_DELAY_SECONDS"
done
