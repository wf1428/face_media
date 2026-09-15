#!/bin/sh

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

# 判断 Ycest 下是否存在除 video 外的子目录
has_non_video_dirs_under_ycest() {
    mountp="$1"
    src_root="${mountp}/${YCEST_SUBDIR}"

    [ -d "$src_root" ] || return 1

    for d in "$src_root"/*; do
        [ -d "$d" ] || continue
        name="${d##*/}"
        [ "$name" = "video" ] && continue
        return 0
    done

    return 1
}


# =========================
# 0) U盘内自拷贝 Ycest 下除 video 外的目录 -> images（最先执行）
#    从:  <mountp>/Ycest 下除 video 外的所有子目录
#    到:  /mnt/UDISK/res/static/demoResources/images
# =========================
copy_ycest_non_video_dirs_to_udisk_images_with_progress() {
    mountp="$1"
    src_root="${mountp}/${YCEST_SUBDIR}"       # <mountp>/Ycest
    dst_dir="${UDISK_DIR}/${QRC_DST_SUBDIR}"   # /mnt/UDISK/res/static/demoResources/images

    if ! is_dir_readable "$src_root"; then
        write_status "error" "U盘未找到Ycest目录或不可读" "$mountp" "$src_root" "$dst_dir" 0 0 0 "" 601 "qrc"
        return 1
    fi

    if ! is_dir_readable "$UDISK_DIR" || ! is_dir_writable "$UDISK_DIR"; then
        write_status "error" "文件不可写，无法进行拷贝" "$mountp" "$src_root" "$dst_dir" 0 0 0 "" 602 "qrc"
        return 1
    fi

    if ! ensure_dir_writable "$dst_dir"; then
        write_status "error" "目标目录不可创建或不可写" "$mountp" "$src_root" "$dst_dir" 0 0 0 "" 603 "qrc"
        return 1
    fi

    # 没有除 video 外的目录：直接跳过，不报错
    if ! has_non_video_dirs_under_ycest "$mountp"; then
        write_status "copying" "未找到文件夹，跳过资源拷贝..." \
            "$mountp" "$src_root" "$dst_dir" 100 0 0 "" 0 "qrc"
        return 0
    fi

    # 优先逐文件拷贝，支持进度与拔出检测
    if command -v find >/dev/null 2>&1 && command -v wc >/dev/null 2>&1; then
        tmp_dir_list="/tmp/ycest_dir_list.$$"
        tmp_file_list="/tmp/ycest_file_list.$$"

        : > "$tmp_dir_list"
        : > "$tmp_file_list"

        for d in "$src_root"/*; do
            [ -d "$d" ] || continue
            name="${d##*/}"
            [ "$name" = "video" ] && continue

            # 先记录一级目录本身
            echo "$name" >> "$tmp_dir_list" 2>/dev/null

            # 再记录该目录下的子目录（相对 Ycest）
            find "$d" -mindepth 1 -type d 2>/dev/null | sed "s#^$src_root/##" >> "$tmp_dir_list" 2>/dev/null

            # 记录该目录下所有文件（相对 Ycest）
            find "$d" -type f 2>/dev/null | sed "s#^$src_root/##" >> "$tmp_file_list" 2>/dev/null
        done

        # 先创建目录结构
        while IFS= read -r rel_dir; do
            [ -n "$rel_dir" ] || continue
            mkdir -p "$dst_dir/$rel_dir" 2>/dev/null || {
                rm -f "$tmp_dir_list" "$tmp_file_list" 2>/dev/null
                write_status "error" "目标目录创建失败" "$mountp" "$src_root" "$dst_dir" 0 0 0 "$rel_dir" 604 "qrc"
                return 1
            }
        done < "$tmp_dir_list"

        total="$(wc -l < "$tmp_file_list" 2>/dev/null)"
        [ -z "$total" ] && total=0

        # 只有目录没有文件，也算成功
        if [ "$total" -le 0 ]; then
            rm -f "$tmp_dir_list" "$tmp_file_list" 2>/dev/null
            sync
            write_status "copying" "资源目录已同步，未发现可拷贝文件..." "$mountp" "$src_root" "$dst_dir" 100 0 0 "" 0 "qrc"
            return 0
        fi

        done_count=0
        while IFS= read -r rel; do
            [ -n "$rel" ] || continue

            cur="$(find_mountpoint_under_base "$MOUNT_BASE" 2>/dev/null)"
            if [ -z "$cur" ] || [ "$cur" != "$mountp" ]; then
                prog=$((done_count * 100 / total))
                write_status "error" "拷贝失败，U盘被拔出" "$mountp" "$src_root" "$dst_dir" "$prog" "$done_count" "$total" "" 611 "qrc"
                rm -f "$tmp_dir_list" "$tmp_file_list" 2>/dev/null
                return 2
            fi

            prog=$((done_count * 100 / total))
            write_status "copying" "正在拷贝资源..." "$mountp" "$src_root" "$dst_dir" "$prog" "$done_count" "$total" "$rel" 0 "qrc"

            subdir="$(dirname "$rel" 2>/dev/null)"
            if [ -n "$subdir" ] && [ "$subdir" != "." ]; then
                mkdir -p "$dst_dir/$subdir" 2>/dev/null
            fi

            cp -p -f "$src_root/$rel" "$dst_dir/$rel" 2>/dev/null
            if [ $? -ne 0 ]; then
                prog=$((done_count * 100 / total))
                write_status "error" "拷贝失败，复制出错" "$mountp" "$src_root" "$dst_dir" "$prog" "$done_count" "$total" "$rel" 612 "qrc"
                rm -f "$tmp_dir_list" "$tmp_file_list" 2>/dev/null
                return 1
            fi

            done_count=$((done_count + 1))
        done < "$tmp_file_list"

        rm -f "$tmp_dir_list" "$tmp_file_list" 2>/dev/null
        sync
        write_status "copying" "资源拷贝完成，准备同步..." "$mountp" "$src_root" "$dst_dir" 100 "$done_count" "$total" "" 0 "qrc"
        return 0
    fi

    # 降级方案：整体按目录拷贝
    write_status "copying" "正在拷贝资源..." "$mountp" "$src_root" "$dst_dir" 0 0 1 "" 0 "qrc"

    copied_any=0
    for d in "$src_root"/*; do
        [ -d "$d" ] || continue
        name="${d##*/}"
        [ "$name" = "video" ] && continue

        cp -a "$d" "$dst_dir"/ 2>/dev/null || cp -R "$d" "$dst_dir"/ 2>/dev/null
        if [ $? -ne 0 ]; then
            write_status "error" "资源拷贝失败（cp出错）" "$mountp" "$src_root" "$dst_dir" 0 0 0 "$name" 613 "qrc"
            return 1
        fi
        copied_any=1
    done

    sync

    if [ "$copied_any" -eq 1 ]; then
        write_status "copying" "资源已同步，准备同步..." "$mountp" "$src_root" "$dst_dir" 100 1 1 "" 0 "qrc"
    else
        write_status "copying" "未找到文件夹，跳过资源拷贝..." "$mountp" "$src_root" "$dst_dir" 100 0 0 "" 0 "qrc"
    fi

    return 0
}



# 清空/mnt/UDISK/.../images 目录（不包括 video），然后重建 images 目录
clear_udisk_images_dir() {
    images_dir="${UDISK_DIR}/${QRC_DST_SUBDIR}"   # /mnt/UDISK/res/static/demoResources/images
    video_dir="${VIDEO_DST_DIR}"                  # /mnt/UDISK/res/static/demoResources/images/video


    mkdir -p "$images_dir" 2>/dev/null || return 1
    mkdir -p "$video_dir" 2>/dev/null || return 1

    # 只清理images目录下除video外的内容，保留video目录及其里面的文件
    for item in "$images_dir"/*; do
        [ -e "$item" ] || continue
        [ "$item" = "$video_dir" ] && continue
        rm -rf "$item" 2>/dev/null || return 1
    done

    return 0
}

copy_net_cfg_to_app() {
    mountp="$1"
    src="$NET_CFG_SRC"
    dst_dir="$NET_CFG_DST_DIR"
    dst="$NET_CFG_DST"

    # 源文件不存在：仅提示，不失败
    if [ ! -f "$src" ] || [ ! -r "$src" ]; then
        write_status "copying" "未找到net_cfg.ini，跳过同步..." "$mountp" "$src" "$dst_dir" 100 0 0 "" 0 "netcfg"
        return 0
    fi

    # 目标目录不可创建/不可写：仅提示，不失败
    if ! ensure_dir_writable "$dst_dir"; then
        write_status "copying" "目标目录不可写，跳过net_cfg.ini同步..." "$mountp" "$src" "$dst_dir" 100 0 0 "" 0 "netcfg"
        return 0
    fi

    write_status "copying" "正在同步net_cfg.ini..." "$mountp" "$src" "$dst" 0 0 1 "net_cfg.ini" 0 "netcfg"

    cp -p -f "$src" "$dst" 2>/dev/null
    if [ $? -ne 0 ]; then
        write_status "copying" "net_cfg.ini复制失败，跳过..." "$mountp" "$src" "$dst" 100 0 1 "net_cfg.ini" 0 "netcfg"
        return 0
    fi

    sync
    write_status "copying" "net_cfg.ini已同步，准备拷贝视频..." "$mountp" "$src" "$dst" 100 1 1 "net_cfg.ini" 0 "netcfg"
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

    cp -f "$src" "$tmp" 2>/dev/null || return 1

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

        mv -f "$tmp_file" "${dst_dir}/${final_name}" 2>/dev/null
        if [ $? -ne 0 ]; then
            write_status "error" "可播放文件更新失败" \
                "$mountp" "$src_dir" "$dst_dir" 100 "$done" "$total" "$final_name" 205 "video"

            rm -rf "$staging_dir" 2>/dev/null
            return 1
        fi
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




do_job_once() {
    mountp="$1"

    res_src="${mountp}/${YCEST_SUBDIR}"
    qrc_dst="${UDISK_DIR}/${QRC_DST_SUBDIR}"

    # 0) 只有 Ycest 下存在除 video 外的子目录时，才清理images（保留video）并拷贝资源
    if has_non_video_dirs_under_ycest "$mountp"; then
        if ! clear_udisk_images_dir "$mountp"; then
            write_status "error" "清理images目录失败" "$mountp" "$qrc_dst" "$qrc_dst" 0 0 0 "" 106 "qrc"
            return 1
        fi

        # 做 U盘内 Ycest(除video外) -> images 拷贝
        copy_ycest_non_video_dirs_to_udisk_images_with_progress "$mountp"
        rc=$?
        [ "$rc" -eq 0 ] || return "$rc"

        # 资源拷贝成功后，额外同步 logo/net_cfg.ini 到 /mnt/app
        copy_net_cfg_to_app "$mountp"
    else
        write_status "copying" "未找到文件夹，跳过资源拷贝，准备处理视频..." \
            "$mountp" "$res_src" "$qrc_dst" 100 0 0 "" 0 "qrc"
    fi

    # 视频拷贝
    copy_video_with_progress "$mountp"
    rc=$?

    # 3 = 所有新视频都有问题，没有导入新文件。
    # 状态已经写成 error，这里返回 0，避免 daemon 反复重试。
    if [ "$rc" -eq 3 ]; then
        return 0
    fi

    # 4 = 有效文件已导入，但部分视频异常被跳过。
    if [ "$rc" -eq 4 ]; then
        write_status "done" "拷贝完成，部分视频文件错误，请拔出U盘重启程序" \
            "$mountp" "$mountp" "$UDISK_DIR" 100 0 0 "" 0 "all"
        return 0
    fi

    [ "$rc" -eq 0 ] || return "$rc"

    # 拷贝完成后不立即重启，等待用户拔出U盘
    write_status "done" "拷贝完成，请拔出U盘重启程序" \
        "$mountp" "$mountp" "$UDISK_DIR" 100 0 0 "" 0 "all"

    return 0


}



daemon_loop() {
    ulog "===== watcher started pid=$$ ====="
    running=1
    trap 'running=0' INT TERM

    inserted=0
    last_mount=""
    job_done=0   # 0=未完成；1=完成后等待拔出

    write_status "idle" "等待插入U盘" "" "" "$UDISK_DIR" 0 0 0 "" 0 "all"

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
                write_status "inserted" "已插入U盘" "$mountp" "$video_src" "$UDISK_DIR" 0 0 0 "" 0 "all"
            fi

            # 如果已经全部拷贝完成，就只等待拔出，不再重复拷贝
            if [ "$job_done" -eq 1 ]; then
                sleep "$POLL_INTERVAL"
                continue
            fi

            # 源挂载点和目标目录均就绪后启动拷贝
            if is_dir_readable "$UDISK_DIR" && is_dir_writable "$UDISK_DIR" \
               && is_dir_readable "$mountp" && is_dir_writable "$mountp"; then

                do_job_once "$mountp"
                rc=$?

                if [ "$rc" -eq 0 ]; then
                    job_done=1
                else
                    job_done=0
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

                inserted=0
                last_mount=""

                if [ "$was_done" -eq 1 ]; then
                    write_status "restarting" "U盘已拔出，正在重启程序..." \
                        "" "" "$UDISK_DIR" 100 0 0 "" 0 "all"
                    # 只在整批文件复制成功且U盘已经拔出后读取目标配置。
                    # sync 完成后再交给 Weston，避免读到半写入的 ini 或在
                    # 资源复制过程中重启 Qt/Weston。
                    sync

                    restart_current_program
                    if [ $? -ne 0 ]; then
                        write_status "error" "U盘已拔出，程序重启失败" \
                            "" "" "$UDISK_DIR" 100 0 0 "" 701 "all"
                    else
                        write_status "done" "U盘已拔出，程序重启完成" \
                            "" "" "$UDISK_DIR" 100 0 0 "" 0 "all"
                    fi
                else
                    write_status "removed" "U盘已拔出" "" "" "$UDISK_DIR" 0 0 0 "" 0 "all"
                fi

                job_done=0
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
