#!/bin/sh
# @file udisk_watcher.sh
# @brief 监听 U 盘并导入 UI、视频及协调人员导入导出。
# @author Du lin
# @note 最近修改：2026-11-03；修改人：Du lin；保留UI整批暂存校验与回滚，按本批文件总量保留剩余空间检查、取消固定预留；兼容文件属性复制；日志不可写和提交后备份清理失败不再误判文件错误，任一UI文件最终失败仍禁止整批发布。

# 最终拷贝目的目录（视频仍然拷到这里的根目录）
UDISK_DIR="/mnt/UDISK"

# 资源最终拷贝根目录
RES_DIR="${UDISK_DIR}/res"
# 资源最终拷贝目录
STATIC_DST_DIR="${RES_DIR}/static/demoResources/images"



# RK3566/RK3568 已由系统自动挂载 U 盘到此目录
MOUNT_BASE="/media/usb0/"

# U盘内视频目录（相对挂载点）
VIDEO_SUBDIR="Ycest/video"

# 视频最终拷贝目录
VIDEO_DST_DIR="${STATIC_DST_DIR}/video"

VIDEO_EXTS="mp4 MP4 avi AVI mov MOV mkv MKV wmv WMV flv FLV ts TS mpg MPG"

# 新增：支持常见图片格式，图片也从 Ycest/video 拷贝到 video 目标目录
IMAGE_EXTS="jpg JPG jpeg JPEG png PNG bmp BMP gif GIF webp WEBP tif TIF tiff TIFF"

# video目录内实际要处理的媒体文件类型：视频 + 图片
MEDIA_EXTS="$VIDEO_EXTS $IMAGE_EXTS"

# U盘内qrc资源目录（相对挂载点）
YCEST_SUBDIR="Ycest"
# qrc资源在U盘内要拷贝到的目标目录
QRC_DST_SUBDIR="res/static/demoResources/images"


# qrc拷贝完成后，额外同步的配置文件
NET_CFG_SRC="${UDISK_DIR}/${QRC_DST_SUBDIR}/logo/net_cfg.ini"
NET_CFG_DST_DIR="/mnt/app"
NET_CFG_DST="${NET_CFG_DST_DIR}/net_cfg.ini"

POLL_INTERVAL=1
PID_FILE="/tmp/udisk_watcher.pid"

# Qt实时读取的状态文件（JSON，原子更新）
STATUS_FILE="/tmp/udisk_status.json"

# 人员导入由Qt界面显式预约。预约存在时，本次插入只提供
# /media/usb0/Ycest/person，禁止触发任何UI资源或视频复制。
PERSON_IMPORT_CONTROL_DIR="/run/face-media"
PERSON_IMPORT_REQUEST="${PERSON_IMPORT_CONTROL_DIR}/person-import.request"
PERSON_IMPORT_READY="${PERSON_IMPORT_CONTROL_DIR}/person-import.ready"

# 导出单独预约，不改动现有人员导入协议；/run 重启后清空。
PERSON_EXPORT_REQUEST="${PERSON_IMPORT_CONTROL_DIR}/person-export.request"
PERSON_EXPORT_READY="${PERSON_IMPORT_CONTROL_DIR}/person-export.ready"

# 可选日志
LOG_FILE="/tmp/udisk_watcher.log"

# 程序的停止、启动和旋转统一交给 Weston 控制脚本管理。
WESTON_CONTROL="/usr/local/sbin/weston-control"
FACE_MEDIA_APP_PIDFILE="/run/face-media/app.pid"
# 必须读取已经完整复制到设备的数据，不能读取即将拔出的U盘源文件。
# Weston 控制脚本会使用 [backRect] 的宽高自动选择横屏0°或竖屏90°。
UI_CONFIG="${STATIC_DST_DIR}/logo/ycest_cfg.ini"

restart_current_program() {
    if [ ! -x "$WESTON_CONTROL" ]; then
        ulog "Weston control script not found or not executable: $WESTON_CONTROL"
        return 1
    fi

    # apply-layout 是U盘更新与显示管理之间的唯一入口：方向改变时执行
    # Weston+触摸+Qt完整旋转事务，方向不变时仅重启一次Qt。这样避免
    # 先restart-app再rotate造成连续杀进程，并复用同一把旋转锁。
    ulog "applying UI layout and restarting MergedQtApp through Weston control: $UI_CONFIG"
    "$WESTON_CONTROL" apply-layout "$UI_CONFIG" >> "$LOG_FILE" 2>&1 || return 1

    # Weston 返回成功后再次核对监督脚本记录的 PID，确认 Qt 已稳定恢复。
    managed_pid="$(sed -n '1p' "$FACE_MEDIA_APP_PIDFILE" 2>/dev/null)"
    case "$managed_pid" in
        ''|*[!0-9]*)
            ulog "Face Media supervised app pid is missing or invalid"
            return 1
            ;;
    esac

    [ -r "/proc/$managed_pid/stat" ] || return 1
    kill -0 "$managed_pid" 2>/dev/null || return 1

    managed_state="$(awk '{print $3}' "/proc/$managed_pid/stat" 2>/dev/null)"
    [ "$managed_state" != "Z" ] || return 1

    managed_name="$(sed -n '1p' "/proc/$managed_pid/comm" 2>/dev/null)"
    if [ "$managed_name" != "MergedQtApp" ]; then
        ulog "Face Media supervised pid has unexpected process name: pid=$managed_pid name=$managed_name"
        return 1
    fi

    # 再等待一秒并确认 PID 文件未被替换，保证交给后续旋转的是稳定实例。
    sleep 1
    current_managed_pid="$(sed -n '1p' "$FACE_MEDIA_APP_PIDFILE" 2>/dev/null)"
    if [ "$current_managed_pid" != "$managed_pid" ] || ! kill -0 "$managed_pid" 2>/dev/null; then
        ulog "Face Media supervised app changed or exited during verification: old=$managed_pid new=$current_managed_pid"
        return 1
    fi

    ulog "MergedQtApp restart verified through Weston: pid=$managed_pid"
    return 0
}


# 日志是辅助信息；不可打开时改用空设备，避免stderr重定向失败阻断实际文件操作。
# 仅调整本进程的LOG_FILE，不影响状态JSON或文件完整性校验。
prepare_update_log() {
    if ! ( : >> "$LOG_FILE" ) 2>/dev/null; then
        LOG_FILE="/dev/null"
    fi
}

ulog() {
    ts="$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
    [ -z "$ts" ] && ts="TIME"
    echo "$ts $*" >> "$LOG_FILE"
}

json_escape() {
    echo "$1" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e ':a;N;$!ba;s/\n/\\n/g' \
        -e 's/\r/\\r/g'
}

write_status() {
    # $1=state, $2=message, $3=mountp, $4=src_dir, $5=dst_dir
    # $6=progress(0-100), $7=done_count, $8=total_count, $9=current_file, $10=error_code
    # $11=task(optional): video/static/...
    state="$1"
    message="$2"
    mountp="$3"
    src_dir="$4"
    dst_dir="$5"
    progress="$6"
    done_count="$7"
    total_count="$8"
    current_file="$9"
    error_code="${10}"
    task="${11:-video}"

    # 同一批次只记住首个实际错误，后续复制进度不能把文件错误覆盖为成功。
    if [ "${JOB_TRACKING:-0}" -eq 1 ] && [ "$error_code" -ne 0 ]; then
        ulog "update file error: code=$error_code file=$current_file message=$message"
        if [ "${JOB_ERROR_CODE:-0}" -eq 0 ]; then
            JOB_ERROR_CODE="$error_code"
            JOB_ERROR_TEXT="$message"
            [ -z "$current_file" ] || JOB_ERROR_TEXT="$JOB_ERROR_TEXT：$current_file"
        fi
    fi

    ts="$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
    [ -z "$ts" ] && ts="TIME"

    msg_e="$(json_escape "$message")"
    mount_e="$(json_escape "$mountp")"
    src_e="$(json_escape "$src_dir")"
    dst_e="$(json_escape "$dst_dir")"
    cur_e="$(json_escape "$current_file")"
    state_e="$(json_escape "$state")"
    ts_e="$(json_escape "$ts")"
    task_e="$(json_escape "$task")"

    tmp="${STATUS_FILE}.tmp.$$"
    cat > "$tmp" <<EOF
{
  "timestamp": "$ts_e",
  "state": "$state_e",
  "task": "$task_e",
  "message": "$msg_e",
  "mount_point": "$mount_e",
  "src_dir": "$src_e",
  "dst_dir": "$dst_e",
  "progress": $progress,
  "done_count": $done_count,
  "total_count": $total_count,
  "current_file": "$cur_e",
  "error_code": $error_code
}
EOF
    mv -f "$tmp" "$STATUS_FILE" 2>/dev/null
}

is_running_pid() {
    pid="$1"
    [ -n "$pid" ] || return 1
    kill -0 "$pid" 2>/dev/null
}

# 返回当前有效人员导入请求的令牌。请求文件第一行为应用PID，第二行为令牌。
person_import_request_token() {
    [ -r "$PERSON_IMPORT_REQUEST" ] || return 1

    request_pid="$(sed -n '1p' "$PERSON_IMPORT_REQUEST" 2>/dev/null | tr -d '\r')"
    request_token="$(sed -n '2p' "$PERSON_IMPORT_REQUEST" 2>/dev/null | tr -d '\r')"
    case "$request_pid" in
        ''|*[!0-9]*|0) request_pid="" ;;
    esac

    if [ -z "$request_pid" ] || [ -z "$request_token" ] \
       || ! kill -0 "$request_pid" 2>/dev/null; then
        rm -f "$PERSON_IMPORT_REQUEST" "$PERSON_IMPORT_READY" 2>/dev/null
        return 1
    fi

    echo "$request_token"
    return 0
}

# 原子发布本次预约对应的挂载点，Qt只有读到相同令牌后才会访问U盘。
publish_person_import_ready() {
    request_token="$1"
    mountp="$2"
    mkdir -p "$PERSON_IMPORT_CONTROL_DIR" 2>/dev/null || return 1
    ready_tmp="${PERSON_IMPORT_READY}.tmp.$$"
    printf '%s\n%s\n' "$request_token" "$mountp" > "$ready_tmp" 2>/dev/null || return 1
    mv -f "$ready_tmp" "$PERSON_IMPORT_READY" 2>/dev/null || {
        rm -f "$ready_tmp" 2>/dev/null
        return 1
    }
    return 0
}

# 返回有效导出令牌；应用异常退出后自动清除失效预约。
person_export_request_token() {
    [ -r "$PERSON_EXPORT_REQUEST" ] || return 1
    export_request_pid="$(sed -n '1p' "$PERSON_EXPORT_REQUEST" 2>/dev/null | tr -d '\r')"
    export_request_token="$(sed -n '2p' "$PERSON_EXPORT_REQUEST" 2>/dev/null | tr -d '\r')"
    case "$export_request_pid" in
        ''|*[!0-9]*|0) export_request_pid="" ;;
    esac
    if [ -z "$export_request_pid" ] || [ -z "$export_request_token" ] \
       || ! kill -0 "$export_request_pid" 2>/dev/null; then
        rm -f "$PERSON_EXPORT_REQUEST" "$PERSON_EXPORT_READY" 2>/dev/null
        return 1
    fi
    echo "$export_request_token"
}

# 第二行为挂载点，或 BUSY；第三行为可选拒绝原因。
publish_person_export_ready() {
    export_ready_tmp="${PERSON_EXPORT_READY}.tmp.$$"
    mkdir -p "$PERSON_IMPORT_CONTROL_DIR" 2>/dev/null || return 1
    printf '%s\n%s\n%s\n' "$1" "$2" "${3:-}" > "$export_ready_tmp" 2>/dev/null || return 1
    mv -f "$export_ready_tmp" "$PERSON_EXPORT_READY" 2>/dev/null || {
        rm -f "$export_ready_tmp" 2>/dev/null
        return 1
    }
}

find_mountpoint_under_base() {
    # /proc/mounts 中挂载点通常不带末尾斜杠，匹配前先规范化。
    base="${1%/}"
    [ -r /proc/mounts ] || return 1

    best=""
    while read dev mnt fstype opts a b; do
        case "$mnt" in
            "$base"/*|"$base")
                if [ -z "$best" ] || [ "${#mnt}" -gt "${#best}" ]; then
                    best="$mnt"
                fi
                ;;
        esac
    done < /proc/mounts

    [ -n "$best" ] && echo "$best" && return 0
    return 1
}

is_dir_readable() {
    d="$1"
    [ -d "$d" ] && [ -r "$d" ] && [ -x "$d" ]
}

is_dir_writable() {
    d="$1"
    [ -d "$d" ] && [ -w "$d" ] && [ -x "$d" ]
}

ensure_dir_writable() {
    d="$1"
    if [ ! -d "$d" ]; then
        mkdir -p "$d" 2>/dev/null || return 1
    fi
    [ -w "$d" ] && [ -x "$d" ]
}

# --- BusyBox最稳：用glob判断/统计mp4，不用find/ls ---
has_video() {
    d="$1"
    [ -d "$d" ] || return 1

    # 保持函数名不变，但实际判断 video 目录下是否存在视频或图片
    for ext in $MEDIA_EXTS; do
        set -- "$d"/*."$ext"
        [ -e "$1" ] && return 0
    done

    return 1
}

count_video() {
    d="$1"
    [ -d "$d" ] || { echo 0; return; }

    cnt=0

    # 保持函数名不变，但实际统计视频 + 图片
    for ext in $MEDIA_EXTS; do
        for f in "$d"/*."$ext"; do
            [ -e "$f" ] || continue
            cnt=$((cnt + 1))
        done
    done

    echo "$cnt"
}

# 判断 Ycest 下是否存在除 video、person 外的UI资源子目录
has_non_video_dirs_under_ycest() {
    mountp="$1"
    src_root="${mountp}/${YCEST_SUBDIR}"

    [ -d "$src_root" ] || return 1

    for d in "$src_root"/*; do
        [ -d "$d" ] || continue
        name="${d##*/}"
        case "$name" in
            video|person) continue ;;
            *) return 0 ;;
        esac
    done

    return 1
}


# 仅接受直接 UI 目录名；禁止让暂存/回滚路径越过 images 或进入 video/person。
ui_resource_name_valid() {
    ui_name_cr="$(printf '\r')"
    case "$1" in
        ''|.|..|.*|*/*|video|person|*'
'*|*"$ui_name_cr"*) return 1 ;;
    esac
    return 0
}

# $1 为同一 images 文件系统内的固定工作目录。只删除带本脚本版本标记的暂存区。
ui_remove_work() {
    [ "$1" = "$STATIC_DST_DIR/.udisk-ui-update" ] && [ ! -L "$1" ] || return 1
    [ "$(cat "$1/format" 2>/dev/null)" = "udisk-ui-v1" ] || return 1
    rm -rf "$1" 2>>"$LOG_FILE"
}

# 持独占锁调用；根据发布前落盘的 plan/original 恢复未提交更新。
# 返回非零时保留备份与日志，禁止后续覆盖；committed 只表示整批目录已发布，不重放 U 盘源。
recover_ui_transaction() {
    ur_work="$STATIC_DST_DIR/.udisk-ui-update"
    [ -e "$ur_work" ] || [ -L "$ur_work" ] || return 0
    [ -d "$ur_work" ] && [ ! -L "$ur_work" ] &&
        [ "$(cat "$ur_work/format" 2>/dev/null)" = "udisk-ui-v1" ] || return 1
    if [ -f "$ur_work/publishing" ] && [ ! -f "$ur_work/committed" ]; then
        [ -f "$ur_work/plan" ] && [ ! -L "$ur_work/plan" ] || return 1
        # 先校验完整计划，再执行任何删除或改名。
        while IFS= read -r ur_name; do
            ui_resource_name_valid "$ur_name" || return 1
        done < "$ur_work/plan"
        while IFS= read -r ur_name; do
            ur_target="$STATIC_DST_DIR/$ur_name"
            ur_backup="$ur_work/backup/$ur_name"
            if [ -f "$ur_work/original/$ur_name" ]; then
                if [ -e "$ur_backup" ] || [ -L "$ur_backup" ]; then
                    rm -rf "$ur_target" 2>>"$LOG_FILE" || return 1
                    mv "$ur_backup" "$ur_target" 2>>"$LOG_FILE" || return 1
                else
                    [ -e "$ur_target" ] || [ -L "$ur_target" ] || return 1
                fi
            elif [ ! -e "$ur_work/stage/$ur_name" ]; then
                # 原先不存在且已发布的目录，回滚时移除；尚在 stage 的不动正式目录。
                rm -rf "$ur_target" 2>>"$LOG_FILE" || return 1
            fi
        done < "$ur_work/plan"
        sync || return 1
        ulog "unfinished UI update rolled back: $ur_work"
    fi
    ui_remove_work "$ur_work"
}

# $1=错误码，$2=说明，$3=可选文件。只记录错误，待工作函数返回、清单句柄关闭后统一回滚。
ui_update_failed() {
    UI_FAILURE_CODE="$1"; UI_FAILURE_MESSAGE="$2"; UI_FAILURE_FILE="${3:-}"
    ulog "UI update failed: code=$UI_FAILURE_CODE message=$UI_FAILURE_MESSAGE file=$UI_FAILURE_FILE"
    return 1
}

# 清单重定向退出后才恢复/清理，避免回滚时仍持有正在读取的暂存文件。
# 返回0表示更新或跳过成功；非零失败先尝试恢复，恢复失败保留备份与错误状态。
copy_ycest_non_video_dirs_to_udisk_images_with_progress() {
    UI_FAILURE_CODE=603; UI_FAILURE_MESSAGE="暂存目录创建失败，设备原文件已保留"; UI_FAILURE_FILE=""
    UI_KEEP_WORK=0
    stage_and_publish_ui_resources "$1"
    ui_copy_rc=$?
    [ "$ui_copy_rc" -ne 0 ] || return 0
    if [ "$UI_KEEP_WORK" -ne 0 ]; then
        : # 新资源已完整发布，但完成标记无法清除；保留备份并报告错误。
    elif ! recover_ui_transaction; then
        UI_FAILURE_CODE=615
        UI_FAILURE_MESSAGE="文件更新失败，旧资源恢复失败，请检查日志"
    fi
    write_status "error" "$UI_FAILURE_MESSAGE" "$1" "$1/$YCEST_SUBDIR" "$STATIC_DST_DIR" 0 0 0 "$UI_FAILURE_FILE" "$UI_FAILURE_CODE" "qrc"
    return "$ui_copy_rc"
}

# 先枚举/暂存/校验整批非空一级 UI 目录，再以备份改名发布；缺失和空目录保留设备原目录。
# $1=U盘挂载点。返回0表示完成或无UI可更新，非零表示失败且已尝试回滚。
# UI_PUBLISHED_COUNT 为本次真正替换的目录数；所有临时清单放在设备 images 文件系统。
# 此工作函数只记录失败，外层负责在清单句柄关闭后执行回滚。
stage_and_publish_ui_resources() {
    UI_PUBLISHED_COUNT=0
    ui_mount="$1"; ui_source="$1/$YCEST_SUBDIR"
    ui_work="$STATIC_DST_DIR/.udisk-ui-update"
    if ! recover_ui_transaction; then
        write_status "error" "文件更新恢复失败，备份已保留，请检查设备日志" "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 0 0 0 "" 615 "qrc"
        return 1
    fi
    [ -e "$ui_source" ] || return 0
    if [ -L "$ui_source" ] || ! is_dir_readable "$ui_source"; then
        write_status "error" "Ycest目录错误或不可读，设备原文件已保留" "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 0 0 0 "" 601 "qrc"
        return 1
    fi
    # 只有视频/人员或根目录散文件时，跳过UI检查，不改变原媒体/人员入口。
    has_non_video_dirs_under_ycest "$ui_mount" || return 0
    for ui_tool in find cksum df awk; do
        command -v "$ui_tool" >/dev/null 2>&1 || {
            write_status "error" "文件检查工具缺失，设备原文件已保留" "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 0 0 0 "$ui_tool" 602 "qrc"
            return 1
        }
    done
    ensure_dir_writable "$STATIC_DST_DIR" || return 1
    mkdir "$ui_work" 2>>"$LOG_FILE" || return 1
    printf 'udisk-ui-v1\n' > "$ui_work/format" || return 1
    mkdir "$ui_work/stage" "$ui_work/backup" "$ui_work/original" 2>>"$LOG_FILE" || {
        ui_update_failed 603 "文件暂存目录创建失败，设备原文件已保留"; return 1;
    }
    : > "$ui_work/plan"; : > "$ui_work/files"; : > "$ui_work/checksums"
    ui_bytes=0; ui_total=0
    write_status "copying" "正在检查文件..." "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 0 0 0 "" 0 "qrc"
    for ui_item in "$ui_source"/*; do
        [ -e "$ui_item" ] || [ -L "$ui_item" ] || continue
        ui_name="${ui_item##*/}"
        case "$ui_name" in video|person) continue ;; esac
        [ -d "$ui_item" ] || [ -L "$ui_item" ] || continue
        if ! ui_resource_name_valid "$ui_name" || [ -L "$ui_item" ]; then
            ui_update_failed 606 "目录错误，设备原文件已保留" "$ui_name"; return 1
        fi
        # 单独检查 find 的退出码，不用管道的最后一条命令掩盖读取失败。
        if ! find "$ui_item" ! -type f ! -type d -print > "$ui_work/special" 2>>"$LOG_FILE" ||
           ! find "$ui_item" -type f -print > "$ui_work/current" 2>>"$LOG_FILE"; then
            ui_update_failed 606 "文件读取失败，设备原文件已保留" "$ui_name"; return 1
        fi
        if [ -s "$ui_work/special" ]; then
            ui_update_failed 606 "包含不支持的链接或文件，设备原文件已保留" "$ui_name"; return 1
        fi
        [ -s "$ui_work/current" ] || continue
        printf '%s\n' "$ui_name" >> "$ui_work/plan" || { ui_update_failed 603 "清单写入失败"; return 1; }
        while IFS= read -r ui_full; do
            case "$ui_full" in "$ui_source"/*) ui_rel="${ui_full#"$ui_source"/}" ;; *) ui_update_failed 606 "文件路径错误"; return 1 ;; esac
            [ -f "$ui_full" ] && [ ! -L "$ui_full" ] || { ui_update_failed 606 "文件不存在或名称不支持" "$ui_rel"; return 1; }
            ui_sum="$(cksum < "$ui_full" 2>>"$LOG_FILE")" || { ui_update_failed 606 "文件校验失败" "$ui_rel"; return 1; }
            printf '%s\n' "$ui_rel" >> "$ui_work/files" && printf '%s\n' "$ui_sum" >> "$ui_work/checksums" || {
                ui_update_failed 603 "清单写入失败" "$ui_rel"; return 1;
            }
            set -- $ui_sum
            ui_bytes=$((ui_bytes + $2)); ui_total=$((ui_total + 1))
        done < "$ui_work/current"
        if ! find "$ui_item" -type d -print > "$ui_work/directories" 2>>"$LOG_FILE"; then
            ui_update_failed 606 "目录读取失败" "$ui_name"; return 1
        fi
        while IFS= read -r ui_full; do
            case "$ui_full" in "$ui_source"/*) ui_rel="${ui_full#"$ui_source"/}" ;; *) ui_update_failed 606 "目录路径错误"; return 1 ;; esac
            mkdir -p "$ui_work/stage/$ui_rel" 2>>"$LOG_FILE" || { ui_update_failed 604 "暂存目录创建失败" "$ui_rel"; return 1; }
        done < "$ui_work/directories"
    done
    if [ "$ui_total" -eq 0 ]; then
        ui_remove_work "$ui_work" || return 1
        write_status "copying" "未提供非空UI资源目录，设备原资源已保留" "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 100 0 0 "" 0 "qrc"
        return 0
    fi
    # 空间检查必做：按本批文件总字节数比较剩余空间，不额外强制预留64MiB。
    # 读不到可靠空间信息就停止；实际复制写入和整批内容校验仍全部检查。
    if ! df -Pk "$STATIC_DST_DIR" > "$ui_work/space" 2>>"$LOG_FILE"; then
        ui_update_failed 607 "无法检查设备空间，原文件已保留"; return 1
    fi
    ui_free="$(awk 'NR > 1 { n=$4 } END { print n }' "$ui_work/space" 2>>"$LOG_FILE")"
    case "$ui_free" in ''|*[!0-9]*) ui_update_failed 607 "设备空间信息错误，原文件已保留"; return 1 ;; esac
    if [ "$ui_free" -lt "$(((ui_bytes + 1023) / 1024))" ]; then
        ui_update_failed 607 "设备空间不足，原文件已保留"; return 1
    fi
    ui_done=0
    while IFS= read -r ui_rel && IFS= read -r ui_expected <&3; do
        ui_current_mount="$(find_mountpoint_under_base "$MOUNT_BASE" 2>/dev/null)"
        if [ "$ui_current_mount" != "$ui_mount" ]; then
            ui_update_failed 611 "U盘已移除，更新未完成，设备原文件已保留" "$ui_rel"; return 2
        fi
        write_status "copying" "正在校验..." "$ui_mount" "$ui_source" "$STATIC_DST_DIR" "$((ui_done * 100 / ui_total))" "$ui_done" "$ui_total" "$ui_rel" 0 "qrc"
        if ! cp -p -f "$ui_source/$ui_rel" "$ui_work/stage/$ui_rel" 2>>"$LOG_FILE"; then
            # 某些文件系统无法保留时间/权限；普通复制重试后仍必须通过全部内容校验。
            ulog "UI attribute-preserving copy failed; retrying ordinary copy: $ui_rel"
            if ! cp -f "$ui_source/$ui_rel" "$ui_work/stage/$ui_rel" 2>>"$LOG_FILE"; then
                ui_update_failed 612 "文件复制失败，设备原文件已保留" "$ui_rel"; return 1
            fi
        fi
        ui_actual="$(cksum < "$ui_work/stage/$ui_rel" 2>>"$LOG_FILE")" || { ui_update_failed 612 "暂存文件校验失败" "$ui_rel"; return 1; }
        ui_source_sum="$(cksum < "$ui_source/$ui_rel" 2>>"$LOG_FILE")" || { ui_update_failed 611 "U盘文件读取失败" "$ui_rel"; return 1; }
        if [ "$ui_actual" != "$ui_expected" ] || [ "$ui_source_sum" != "$ui_expected" ]; then
            ui_update_failed 612 "文件不完整或复制期间已变化，原文件已保留" "$ui_rel"; return 1
        fi
        ui_done=$((ui_done + 1))
    done < "$ui_work/files" 3< "$ui_work/checksums"
    [ "$ui_done" -eq "$ui_total" ] || { ui_update_failed 612 "校验清单不完整"; return 1; }
    # 在第一次改名之前，先完整落盘恢复计划；同一文件系统内只改名，不复制旧目录。
    while IFS= read -r ui_name; do
        if [ -e "$STATIC_DST_DIR/$ui_name" ] || [ -L "$STATIC_DST_DIR/$ui_name" ]; then
            : > "$ui_work/original/$ui_name" || { ui_update_failed 614 "备份写入失败"; return 1; }
        fi
    done < "$ui_work/plan"
    sync || { ui_update_failed 614 "暂存同步失败"; return 1; }
    : > "$ui_work/publishing" && sync || { ui_update_failed 614 "发布计划同步失败"; return 1; }
    write_status "copying" "文件校验通过，正在替换UI资源..." "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 100 "$ui_total" "$ui_total" "" 0 "qrc"
    while IFS= read -r ui_name; do
        if [ -f "$ui_work/original/$ui_name" ]; then
            mv "$STATIC_DST_DIR/$ui_name" "$ui_work/backup/$ui_name" 2>>"$LOG_FILE" || {
                ui_update_failed 614 "旧备份失败，已尝试恢复原文件" "$ui_name"; return 1;
            }
        fi
        mv "$ui_work/stage/$ui_name" "$STATIC_DST_DIR/$ui_name" 2>>"$LOG_FILE" || {
            ui_update_failed 614 "替换失败，已尝试恢复原文件" "$ui_name"; return 1;
        }
    done < "$ui_work/plan"
    sync || { ui_update_failed 614 "替换同步失败，已尝试恢复原文件"; return 1; }
    if ! : > "$ui_work/committed" || ! sync; then
        if ! rm -f "$ui_work/committed" 2>>"$LOG_FILE"; then
            UI_KEEP_WORK=1
            UI_PUBLISHED_COUNT="$(wc -l < "$ui_work/plan")"
            ui_update_failed 615 "UI文件已更新，但完成状态写入失败，备份已保留"; return 1
        fi
        ui_update_failed 614 "完成状态写入失败"; return 1
    fi
    UI_PUBLISHED_COUNT="$(wc -l < "$ui_work/plan")"
    if ! ui_remove_work "$ui_work"; then
        # 已完成整批发布与同步，备份清理问题不代表UI文件错误，后续恢复入口再清理。
        ulog "committed UI backup cleanup deferred; validated UI update remains successful"
    fi
    write_status "copying" "UI资源更新完成，未提供及空目录的原资源已保留" "$ui_mount" "$ui_source" "$STATIC_DST_DIR" 100 "$ui_total" "$ui_total" "" 0 "qrc"
    return 0
}

# UI发布后沿用原配置同步入口；使用临时文件改名，失败记录为本批文件错误，继续视频流程。
copy_net_cfg_to_app() {
    nc_mount="$1"; nc_source="$NET_CFG_SRC"; nc_target="$NET_CFG_DST"
    if [ ! -f "$nc_source" ]; then
        write_status "copying" "本次无可同步net_cfg.ini，设备原配置保留" "$nc_mount" "$nc_source" "$NET_CFG_DST_DIR" 100 0 0 "" 0 "netcfg"
        return 0
    fi
    if ! ensure_dir_writable "$NET_CFG_DST_DIR" || [ ! -r "$nc_source" ]; then
        write_status "error" "配置文件读取或目标目录错误，原配置已保留" "$nc_mount" "$nc_source" "$NET_CFG_DST_DIR" 100 0 1 "net_cfg.ini" 621 "netcfg"
        return 0
    fi
    nc_tmp="$NET_CFG_DST_DIR/.net_cfg.ini.udisk-$$"
    write_status "copying" "正在同步net_cfg.ini..." "$nc_mount" "$nc_source" "$nc_target" 0 0 1 "net_cfg.ini" 0 "netcfg"
    if ! cp -p -f "$nc_source" "$nc_tmp" 2>>"$LOG_FILE" ||
       ! cmp -s "$nc_source" "$nc_tmp" 2>>"$LOG_FILE" ||
       ! mv -f "$nc_tmp" "$nc_target" 2>>"$LOG_FILE"; then
        rm -f "$nc_tmp" 2>>"$LOG_FILE"
        write_status "error" "net_cfg.ini文件同步失败，原配置已保留" "$nc_mount" "$nc_source" "$nc_target" 100 0 1 "net_cfg.ini" 622 "netcfg"
        return 0
    fi
    sync
    write_status "copying" "net_cfg.ini已同步，准备处理视频..." "$nc_mount" "$nc_source" "$nc_target" 100 1 1 "net_cfg.ini" 0 "netcfg"
    return 0
}


is_video_file_name() {
    name="${1##*/}"
    ext="${name##*.}"
    [ "$ext" = "$name" ] && return 1

    for vext in $VIDEO_EXTS; do
        [ "$ext" = "$vext" ] && return 0
    done

    return 1
}

get_file_size() {
    [ -f "$1" ] || {
        echo 0
        return
    }

    wc -c < "$1" 2>/dev/null | tr -d ' '
}

copy_file_to_tmp_checked() {
    src="$1"
    tmp="$2"

    cp -f "$src" "$tmp" 2>>"$LOG_FILE" || return 1

    src_size="$(get_file_size "$src")"
    tmp_size="$(get_file_size "$tmp")"

    [ -n "$src_size" ] || src_size=0
    [ -n "$tmp_size" ] || tmp_size=0

    # 最简单校验：非空 + 拷贝前后大小一致
    [ "$src_size" != "0" ] || return 2
    [ "$src_size" = "$tmp_size" ] || return 3

    return 0
}


# =========================
# 1) 视频拷贝（原逻辑，增加task=video）
# =========================
copy_video_with_progress() {
    VIDEO_UPDATED_COUNT=0  # 已发布的有效媒体数，用于决定拔盘后是否需要原有重启流程。
    mountp="$1"
    src_dir="${mountp}/${VIDEO_SUBDIR}"
    dst_dir="$VIDEO_DST_DIR"

    # 1. 没有video目录：跳过，不删除旧文件，不报错中断
    if ! is_dir_readable "$src_dir"; then
        write_status "copying" "未找到目录，跳过..." "$mountp" "$src_dir" "$dst_dir" 100 0 0 "" 0 "video"
        return 0
    fi

    total="$(count_video "$src_dir")"
    [ -z "$total" ] && total=0

    # 2. video目录存在，但没有可识别的视频或图片文件：跳过，不删除旧文件
    if [ "$total" -le 0 ]; then
        write_status "copying" "目录中未找到支持格式的视频或图片文件，跳过..." "$mountp" "$src_dir" "$dst_dir" 100 0 0 "" 0 "video"
        return 0
    fi

    # 3. 只有确认要拷新文件时，才检查目标目录
    if ! is_dir_readable "$UDISK_DIR" || ! is_dir_writable "$UDISK_DIR"; then
        write_status "error" "目标目录不可用" "$mountp" "$src_dir" "$dst_dir" 0 0 "$total" "" 101 "video"
        return 1
    fi

    if ! ensure_dir_writable "$dst_dir"; then
        write_status "error" "目标视频目录不可创建或不可写" "$mountp" "$src_dir" "$dst_dir" 0 0 "$total" "" 101 "video"
        return 1
    fi

    # 不直接复制到最终可播放目录：先复制到隐藏临时目录并加
    # .copying 后缀，全部复制完成后再统一改为正式文件名。

    staging_dir="${dst_dir}/.import_tmp_$$"

    rm -rf "${dst_dir}"/.import_tmp_* 2>/dev/null
    mkdir -p "$staging_dir" 2>/dev/null
    if [ $? -ne 0 ]; then
        write_status "error" "目录创建失败" "$mountp" "$src_dir" "$dst_dir" 0 0 "$total" "" 102 "video"
        return 1
    fi

    write_status "copying" "检测到新视频或图片，正在拷贝..." \
        "$mountp" "$src_dir" "$dst_dir" 0 0 "$total" "" 0 "video"

    done=0
    staged_count=0
    skipped_video_count=0

    for ext in $MEDIA_EXTS; do
        for f in "$src_dir"/*."$ext"; do
            [ -e "$f" ] || continue

            cur="$(find_mountpoint_under_base "$MOUNT_BASE" 2>/dev/null)"
            if [ -z "$cur" ] || [ "$cur" != "$mountp" ]; then
                prog=$((done * 100 / total))
                write_status "error" "拷贝失败，U盘被拔出" "$mountp" "$src_dir" "$dst_dir" "$prog" "$done" "$total" "" 201 "video"
                rm -rf "$staging_dir" 2>/dev/null
                return 2
            fi

            base="$(basename "$f" 2>/dev/null)"
            tmp_file="${staging_dir}/${base}.copying"

            prog=$((done * 100 / total))
            write_status "copying" "正在拷贝视频或图片..." \
                "$mountp" "$src_dir" "$dst_dir" "$prog" "$done" "$total" "$base" 0 "video"

            copy_file_to_tmp_checked "$f" "$tmp_file"
            cp_rc=$?

            if [ "$cp_rc" -ne 0 ]; then
                rm -f "$tmp_file" 2>/dev/null

                if is_video_file_name "$base"; then
                    skipped_video_count=$((skipped_video_count + 1))
                    done=$((done + 1))
                    prog=$((done * 100 / total))

                    write_status "error" "视频文件格式问题或文件不完整，已跳过该视频" \
                        "$mountp" "$src_dir" "$dst_dir" "$prog" "$done" "$total" "$base" 203 "video"

                    ulog "video invalid or incomplete, skipped: $base"
                    continue
                fi

                prog=$((done * 100 / total))
                write_status "error" "拷贝失败，复制出错" \
                    "$mountp" "$src_dir" "$dst_dir" "$prog" "$done" "$total" "$base" 202 "video"

                rm -rf "$staging_dir" 2>/dev/null
                return 1
            fi

            staged_count=$((staged_count + 1))
            done=$((done + 1))
            prog=$((done * 100 / total))

            write_status "copying" "正在拷贝视频或图片..." \
                "$mountp" "$src_dir" "$dst_dir" "$prog" "$done" "$total" "$base" 0 "video"
        done
    done

    # 如果没有任何可用文件，不清理旧播放文件
    if [ "$staged_count" -le 0 ]; then
        rm -rf "$staging_dir" 2>/dev/null
        sync

        if [ "$skipped_video_count" -gt 0 ]; then
            write_status "error" "视频文件格式问题或文件不完整，未导入新视频，旧文件已保留" \
                "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "" 203 "video"
            return 3
        fi

        write_status "copying" "未发现可导入文件，旧文件已保留..." \
            "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "" 0 "video"
        return 0
    fi

    # 到这里说明临时区已经有完整文件，再清理旧文件
    write_status "copying" "文件拷贝完成，正在更新可播放文件..." \
        "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "" 0 "video"

    for ext in $MEDIA_EXTS; do
        rm -f "${dst_dir}"/*."$ext" 2>/dev/null
    done

    # 最后一步：把 .copying 临时文件改成正式文件名
    for tmp_file in "$staging_dir"/*.copying; do
        [ -e "$tmp_file" ] || continue

        tmp_name="$(basename "$tmp_file" 2>/dev/null)"
        final_name="${tmp_name%.copying}"

        mv -f "$tmp_file" "${dst_dir}/${final_name}" 2>>"$LOG_FILE"
        if [ $? -ne 0 ]; then
            write_status "error" "可播放文件更新失败" \
                "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "$final_name" 205 "video"

            rm -rf "$staging_dir" 2>/dev/null
            return 1
        fi
        VIDEO_UPDATED_COUNT=$((VIDEO_UPDATED_COUNT + 1))
    done

    rm -rf "$staging_dir" 2>/dev/null
    sync

    if [ "$skipped_video_count" -gt 0 ]; then
        write_status "copying" "视频和图片拷贝完成，部分视频异常已跳过..." \
            "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "" 0 "video"
        return 4
    fi

    write_status "copying" "视频和图片拷贝完成，准备继续后续流程..." \
        "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "" 0 "video"

    return 0
}




# $1=是否有已发布文件需要重启，$2=错误码，$3=最终消息；原子写入本次运行结果。
write_job_result() {
    jr_tmp="${STATUS_FILE}.result.tmp.$$"
    printf '%s\n%s\n%s\n' "$1" "$2" "$3" > "$jr_tmp" &&
        mv -f "$jr_tmp" "${STATUS_FILE}.result"
}

# 汇总整批结果：有错误始终用error，不把已导入部分有效文件描述成全部成功。
finish_update_job() {
    fj_restart=0
    [ "${UI_PUBLISHED_COUNT:-0}" -eq 0 ] && [ "${VIDEO_UPDATED_COUNT:-0}" -eq 0 ] || fj_restart=1
    if [ "${JOB_ERROR_CODE:-0}" -ne 0 ]; then
        if [ "$fj_restart" -eq 1 ]; then
            fj_message="更新存在文件错误：$JOB_ERROR_TEXT；请拔出U盘重启后检查错误文件"
        else
            fj_message="文件更新失败：$JOB_ERROR_TEXT；请拔出U盘检查后重试"
        fi
        write_job_result "$fj_restart" "$JOB_ERROR_CODE" "$JOB_ERROR_TEXT" || return 1
        write_status "error" "$fj_message" "$job_mount" "$job_mount/$YCEST_SUBDIR" "$UDISK_DIR" 100 0 0 "" "$JOB_ERROR_CODE" "all"
        return 1
    fi
    if [ "$fj_restart" -eq 1 ]; then
        fj_message="文件更新完成，请拔出U盘重启程序"
    else
        fj_message="未发现可更新文件，设备原文件已保留，请拔出U盘"
    fi
    write_job_result "$fj_restart" 0 "$fj_message" || return 1
    write_status "done" "$fj_message" "$job_mount" "$job_mount/$YCEST_SUBDIR" "$UDISK_DIR" 100 0 0 "" 0 "all"
    return 0
}

# 与 Qt/Web 共用独占锁；75仅表示忙碌，可重试，不清理或覆盖任何正式资源。
do_job_once() (
    prepare_update_log
    mkdir -p "$STATIC_DST_DIR" || return 1
    exec 8>"$STATIC_DST_DIR/.ycest-update.lock" || return 1
    flock -n 8 || return 75
    do_job_once_locked "$1"
)

# 持锁完成UI事务、原配置同步及原媒体复制。错误由统一终态汇总，不再预清理设备UI。
do_job_once_locked() {
    job_mount="$1"
    JOB_TRACKING=1; JOB_ERROR_CODE=0; JOB_ERROR_TEXT=""
    UI_PUBLISHED_COUNT=0; VIDEO_UPDATED_COUNT=0
    if ! copy_ycest_non_video_dirs_to_udisk_images_with_progress "$job_mount"; then
        if [ "$JOB_ERROR_CODE" -eq 0 ]; then
            JOB_ERROR_CODE=603; JOB_ERROR_TEXT="目录创建或恢复失败，未完成UI更新"
        fi
        finish_update_job
        return 1
    fi
    # 缺失或空UI目录不触发配置覆盖；UI有实际更新后沿用原同步顺序。
    [ "$UI_PUBLISHED_COUNT" -eq 0 ] || copy_net_cfg_to_app "$job_mount"
    copy_video_with_progress "$job_mount"
    job_video_rc=$?
    if [ "$job_video_rc" -ne 0 ] && [ "$JOB_ERROR_CODE" -eq 0 ]; then
        JOB_ERROR_CODE=202; JOB_ERROR_TEXT="视频或图片文件更新失败"
    fi
    finish_update_job
}

# 脚本启动时先恢复本脚本未完成事务；锁忙不抢占，下一次资源更新仍会先恢复。
recover_ui_on_startup() (
    prepare_update_log
    [ -e "$STATIC_DST_DIR/.udisk-ui-update" ] || return 0
    exec 8>"$STATIC_DST_DIR/.ycest-update.lock" || return 1
    flock -n 8 || return 75
    if ! recover_ui_transaction; then
        write_status "error" "上次UI文件更新恢复失败，备份已保留，请检查日志" "" "" "$STATIC_DST_DIR" 0 0 0 "" 615 "qrc"
        return 1
    fi
)




daemon_loop() {
    prepare_update_log
    ulog "===== watcher started pid=$$ ====="
    running=1
    trap 'running=0' INT TERM

    inserted=0
    last_mount=""
    job_done=0   # 0=待处理/锁忙；1=本次结束（含文件错误），等待拔出
    restart_needed=0
    job_error_code=0
    job_error_message=""
    person_session=0
    person_token=""
    person_request_active=0
    export_session=0
    export_token=""
    export_request_active=0
    copy_started=0
    rm -f "$PERSON_IMPORT_READY" 2>/dev/null
    rm -f "$PERSON_EXPORT_READY" 2>/dev/null

    write_status "idle" "等待插入U盘" "" "" "$UDISK_DIR" 0 0 0 "" 0 "all"
    recover_ui_on_startup

    while [ "$running" -eq 1 ]; do
        mountp="$(find_mountpoint_under_base "$MOUNT_BASE" 2>/dev/null)"

        if [ -n "$mountp" ]; then
            video_src="${mountp}/${VIDEO_SUBDIR}"
            qrc_src="${mountp}/${YCEST_SUBDIR}"

            # 新插入 或 挂载点变化：重置状态
            if [ "$inserted" -eq 0 ] || [ "$mountp" != "$last_mount" ]; then
                inserted=1
                last_mount="$mountp"
                job_done=0
                restart_needed=0
                job_error_code=0
                job_error_message=""
                person_session=0
                person_token=""
                person_request_active=0
                export_session=0
                export_token=""
                export_request_active=0
                copy_started=0
                rm -f "$PERSON_IMPORT_READY" 2>/dev/null
                rm -f "$PERSON_EXPORT_READY" 2>/dev/null
                write_status "inserted" "已插入U盘" "$mountp" "$video_src" "$UDISK_DIR" 0 0 0 "" 0 "all"
            fi

            # 点击“导入人员表”会提前写入预约。只要本次插入被预约，整轮都
            # 不再执行UI资源和视频复制；即使应用完成或取消，也要等拔盘后
            # 才恢复普通自动复制，避免同一块盘在导入结束后又被二次处理。
            current_person_token="$(person_import_request_token 2>/dev/null)"
            current_export_token="$(person_export_request_token 2>/dev/null)"
            if [ "$export_session" -eq 0 ] && [ -n "$current_export_token" ]; then
                if [ "$person_session" -eq 1 ] || [ -n "$current_person_token" ] \
                   || [ "$copy_started" -eq 1 ]; then
                    # 已开始的更新/导入不能被中途接管，也不改变原有拔盘重启行为。
                    publish_person_export_ready "$current_export_token" "BUSY" \
                        "当前U盘已用于资源更新或人员导入，请拔出后重新点击导出，再插入U盘"
                else
                    export_session=1
                    export_token="$current_export_token"
                    export_request_active=1
                    job_done=1
                    if publish_person_export_ready "$export_token" "$mountp"; then
                        write_status "person_ready" "人员导出U盘已就绪" \
                            "$mountp" "${mountp}/Ycest/person" "" 0 0 0 "" 0 "person"
                        ulog "person export session reserved: mount=$mountp token=$export_token"
                    else
                        write_status "error" "人员导出就绪状态写入失败" \
                            "$mountp" "${mountp}/Ycest/person" "" 0 0 0 "" 802 "person"
                    fi
                fi
            fi

            # 本次插入一旦用于导出，即使完成或取消也不再自动拷贝；拔盘后复位。
            if [ "$export_session" -eq 1 ]; then
                if [ -n "$current_export_token" ] && [ "$current_export_token" != "$export_token" ]; then
                    export_token="$current_export_token"
                    export_request_active=1
                    if publish_person_export_ready "$export_token" "$mountp"; then
                        write_status "person_ready" "人员导出U盘已就绪" \
                            "$mountp" "${mountp}/Ycest/person" "" 0 0 0 "" 0 "person"
                    fi
                elif [ -z "$current_export_token" ] && [ "$export_request_active" -eq 1 ]; then
                    export_request_active=0
                    rm -f "$PERSON_EXPORT_READY" 2>/dev/null
                    write_status "person_done" "人员导出处理完成，请拔出U盘" \
                        "$mountp" "${mountp}/Ycest/person" "" 100 0 0 "" 0 "person"
                fi
                sleep "$POLL_INTERVAL"
                continue
            fi

            if [ "$person_session" -eq 0 ] && [ -n "$current_person_token" ]; then
                person_session=1
                person_token="$current_person_token"
                person_request_active=1
                job_done=1
                if publish_person_import_ready "$person_token" "$mountp"; then
                    write_status "person_ready" "人员导入U盘已就绪" \
                        "$mountp" "${mountp}/Ycest/person" "" 0 0 0 "" 0 "person"
                    ulog "person import session reserved: mount=$mountp token=$person_token"
                else
                    write_status "error" "人员导入就绪状态写入失败" \
                        "$mountp" "${mountp}/Ycest/person" "" 0 0 0 "" 801 "person"
                fi
            fi

            if [ "$person_session" -eq 1 ]; then
                if [ -n "$current_person_token" ] && [ "$current_person_token" != "$person_token" ]; then
                    person_token="$current_person_token"
                    person_request_active=1
                    if publish_person_import_ready "$person_token" "$mountp"; then
                        write_status "person_ready" "人员导入U盘已就绪" \
                            "$mountp" "${mountp}/Ycest/person" "" 0 0 0 "" 0 "person"
                    fi
                elif [ -z "$current_person_token" ] && [ "$person_request_active" -eq 1 ]; then
                    person_request_active=0
                    rm -f "$PERSON_IMPORT_READY" 2>/dev/null
                    write_status "person_done" "人员导入处理完成，请拔出U盘" \
                        "$mountp" "${mountp}/Ycest/person" "" 100 0 0 "" 0 "person"
                fi
                sleep "$POLL_INTERVAL"
                continue
            fi

            # 如果已经全部拷贝完成，就只等待拔出，不再重复拷贝
            if [ "$job_done" -eq 1 ]; then
                sleep "$POLL_INTERVAL"
                continue
            fi

            # 源挂载点和目标目录均就绪后启动拷贝
            if is_dir_readable "$UDISK_DIR" && is_dir_writable "$UDISK_DIR" \
               && is_dir_readable "$mountp" && is_dir_writable "$mountp"; then

                copy_started=1
                rm -f "${STATUS_FILE}.result" 2>/dev/null
                do_job_once "$mountp"
                rc=$?

                if [ "$rc" -eq 75 ]; then
                    # 锁忙可以等待，不是文件错误；其他终态本次插入不重复清理/复制。
                    job_done=0
                else
                    job_done=1
                    restart_needed=0
                    if [ -f "${STATUS_FILE}.result" ]; then
                        restart_needed="$(sed -n '1p' "${STATUS_FILE}.result")"
                        job_error_code="$(sed -n '2p' "${STATUS_FILE}.result")"
                        job_error_message="$(sed -n '3,$p' "${STATUS_FILE}.result")"
                        case "$restart_needed" in 0|1) ;; *) restart_needed=0 ;; esac
                        case "$job_error_code" in ''|*[!0-9]*) job_error_code=699 ;; esac
                    elif [ "$rc" -eq 0 ]; then
                        # 保留旧部署/隔离替身的成功返回约定；实际入口总会写结果文件。
                        restart_needed=1
                    else
                        job_error_code=699
                        job_error_message="文件更新异常中断，请检查U盘文件和设备日志"
                        write_status "error" "$job_error_message" "$mountp" "$mountp/$YCEST_SUBDIR" "$UDISK_DIR" 0 0 0 "" "$job_error_code" "all"
                    fi
                fi
            else

                reason=""
                is_dir_readable "$UDISK_DIR" || reason="${reason} dst_not_readable;"
                is_dir_writable "$UDISK_DIR" || reason="${reason} dst_not_writable;"
                is_dir_writable "$mountp" || reason="${reason} mount_not_writable;"

                msg="U盘已插入，等待就绪：${reason}"
                write_status "waiting" "$msg" "$mountp" "$mountp" "$UDISK_DIR" 0 0 0 "" 0 "all"
            fi
	else
            # 未检测到挂载点：认为拔出
            if [ "$inserted" -eq 1 ]; then
                was_done="$job_done"
                was_restart_needed="$restart_needed"
                was_error_code="$job_error_code"
                was_error_message="$job_error_message"
                was_person_session="$person_session"
                was_export_session="$export_session"

                inserted=0
                last_mount=""
                rm -f "$PERSON_IMPORT_READY" 2>/dev/null
                rm -f "$PERSON_EXPORT_READY" 2>/dev/null

                if [ "$was_done" -eq 1 ] && [ "$was_restart_needed" -eq 1 ]; then
                    write_status "restarting" "U盘已拔出，正在重启程序..." \
                        "" "" "$UDISK_DIR" 100 0 0 "" 0 "all"
                    # 只在本批有实际发布的有效文件且U盘已拔出后读取目标配置；文件错误另行保留。
                    # sync 完成后再交给 Weston，避免读到半写入的 ini 或在
                    # 资源复制过程中重启 Qt/Weston。
                    sync

                    restart_current_program
                    if [ $? -ne 0 ]; then
                        if [ "$was_error_code" -ne 0 ]; then
                            write_status "error" "U盘已拔出，程序重启失败；文件错误：$was_error_message" \
                                "" "" "$UDISK_DIR" 100 0 0 "" 701 "all"
                        else
                            write_status "error" "U盘已拔出，程序重启失败" \
                                "" "" "$UDISK_DIR" 100 0 0 "" 701 "all"
                        fi
                    elif [ "$was_error_code" -ne 0 ]; then
                        write_status "error" "U盘已拔出，程序已重启；本次更新存在文件错误：$was_error_message" \
                            "" "" "$UDISK_DIR" 100 0 0 "" "$was_error_code" "all"
                    else
                        write_status "done" "U盘已拔出，程序重启完成" \
                            "" "" "$UDISK_DIR" 100 0 0 "" 0 "all"
                    fi
                elif [ "$was_export_session" -eq 1 ]; then
                    write_status "removed" "人员导出U盘已拔出" "" "" "$UDISK_DIR" 0 0 0 "" 0 "person"
                elif [ "$was_person_session" -eq 1 ]; then
                    write_status "removed" "人员导入U盘已拔出" "" "" "$UDISK_DIR" 0 0 0 "" 0 "person"
                elif [ "$was_error_code" -ne 0 ]; then
                    write_status "error" "U盘已拔出，本次文件更新未完成：$was_error_message" \
                        "" "" "$UDISK_DIR" 0 0 0 "" "$was_error_code" "all"
                else
                    write_status "removed" "U盘已拔出，未更新文件，设备原文件已保留" "" "" "$UDISK_DIR" 0 0 0 "" 0 "all"
                fi

                job_done=0
                restart_needed=0
                job_error_code=0
                job_error_message=""
                person_session=0
                person_token=""
                person_request_active=0
                export_session=0
                export_token=""
                export_request_active=0
                copy_started=0
            fi
        fi
        sleep "$POLL_INTERVAL"
    done

    ulog "===== watcher stopped pid=$$ ====="
    rm -f "$PID_FILE" 2>/dev/null
    exit 0
}

start_daemon() {
    if [ -f "$PID_FILE" ]; then
        oldpid="$(cat "$PID_FILE" 2>/dev/null)"
        if is_running_pid "$oldpid"; then
            echo "Watcher already running (pid=$oldpid). Status: $STATUS_FILE"
            exit 0
        else
            rm -f "$PID_FILE" 2>/dev/null
        fi
    fi

    ( daemon_loop ) >/dev/null 2>&1 &
    child_pid="$!"
    echo "$child_pid" > "$PID_FILE"
    echo "Watcher started (pid=$child_pid). Status: $STATUS_FILE"
    exit 0
}

stop_daemon() {
    if [ -f "$PID_FILE" ]; then
        pid="$(cat "$PID_FILE" 2>/dev/null)"
        if is_running_pid "$pid"; then
            kill "$pid" 2>/dev/null
            echo "Sent TERM to watcher pid=$pid"
            exit 0
        fi
    fi
    echo "Watcher not running."
    exit 1
}

status_daemon() {
    if [ -f "$PID_FILE" ]; then
        pid="$(cat "$PID_FILE" 2>/dev/null)"
        if is_running_pid "$pid"; then
            echo "Watcher running (pid=$pid). Status: $STATUS_FILE"
            exit 0
        fi
    fi
    echo "Watcher not running."
    exit 1
}

case "$1" in
    run)      daemon_loop ;;
    start|"") start_daemon ;;
    stop)     stop_daemon ;;
    status)   status_daemon ;;
    *) echo "Usage: $0 {run|start|stop|status}"; exit 1 ;;
esac
