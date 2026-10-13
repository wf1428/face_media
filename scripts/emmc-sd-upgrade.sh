#!/bin/bash
# RK3568：从 SD 卡启动 Ubuntu，将系统离线升级到 eMMC（不备份旧系统、不提供自动回滚）。
# 如果升级被中断，重新插入同一张 SD 启动卡并启动，即可重新执行完整升级。

set -Eeuo pipefail

CONFIG=/etc/default/emmc-sd-upgrade
[ -f "$CONFIG" ] && . "$CONFIG"

AUTO_APPLY=${AUTO_APPLY:-0}
AUTO_POWEROFF=${AUTO_POWEROFF:-1}

# Linux 文本控制台升级界面；设置 UPGRADE_UI=0 可恢复原始日志输出模式。
UPGRADE_UI=${UPGRADE_UI:-1}
UPGRADE_UI_TTY=${UPGRADE_UI_TTY:-/dev/tty1}
UPGRADE_UI_FONT=${UPGRADE_UI_FONT:-auto}  # auto=自动选择字体；off=不修改字体；也可指定 .psf 字体的绝对路径
UPGRADE_UI_SWITCH=${UPGRADE_UI_SWITCH:-1}
# 升级成功后的额外展示时间，之后仍会执行原有的 3 秒关机前延时。
UPGRADE_UI_SUCCESS_HOLD=${UPGRADE_UI_SUCCESS_HOLD:-5}

LOG=/run/emmc-sd-upgrade.log
STATE_DIR=/var/lib/emmc-sd-upgrade
TARGET_MNT=/mnt/emmc-upgrade-root
LOCK_FILE=/run/emmc-sd-upgrade.lock

BUSINESS_SERVICES=(
    face-media.service
    disable-console-blank.service
    udisk-watcher.service
    weston-kiosk.service
)

mkdir -p "$STATE_DIR" "$TARGET_MNT"

# 详细日志始终保存到同一个日志文件。物理控制台可用时，
# 不在 LCD 上刷屏显示 rsync/dd 的详细输出；界面独立使用文件描述符 8。
UI_READY=0
UI_PID=""
UI_STARTED=0
UI_ERROR_LAST=""
UI_DEMO=0
UI_STATUS_FILE=/run/emmc-sd-upgrade-ui.status
if [ "$UPGRADE_UI" = "1" ] && [ -c "$UPGRADE_UI_TTY" ] && \
   { exec 8>"$UPGRADE_UI_TTY"; } 2>/dev/null; then
    UI_READY=1
    exec >>"$LOG" 2>&1
else
    exec > >(tee -a "$LOG") 2>&1
fi

# 使用文件锁，防止多个升级进程同时写入同一块 eMMC。
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    echo "Another emmc-sd-upgrade instance is already running."
    exit 1
fi

log()
{
    echo "[$(date '+%F %T')] $*"
}

# LCD 界面仅负责显示：即使显示出错，也不能影响升级和校验结果。
# ANSI 控制字符只写入文件描述符 8，不写入日志文件。
ui_font()
{
    local candidate
    [ "$UI_READY" = "1" ] || return 0
    [ "$UPGRADE_UI_FONT" != "off" ] || return 0
    command -v setfont >/dev/null 2>&1 || {
        log "Console font enlargement unavailable: setfont not installed"
        return 0
    }

    if [ "$UPGRADE_UI_FONT" = "double" ]; then
        # setfont -d 可将 8×16 字形放大为 16×32，不依赖额外的字体文件。
        setfont -C "$UPGRADE_UI_TTY" -d >/dev/null 2>&1 || true
        return 0
    fi

    if [ "$UPGRADE_UI_FONT" != "auto" ]; then
        [ -f "$UPGRADE_UI_FONT" ] || {
            log "Console font not found: $UPGRADE_UI_FONT"
            return 0
        }
        setfont -C "$UPGRADE_UI_TTY" "$UPGRADE_UI_FONT" >/dev/null 2>&1 || true
        return 0
    fi

    # 优先尝试常见的大号 Linux 控制台字体（约 16×32）。
    # 字体放大失败不能阻断正常升级。
    for candidate in \
        /usr/share/consolefonts/*TerminusBold32x16*.psf* \
        /usr/share/consolefonts/*Terminus32x16*.psf* \
        /usr/share/consolefonts/*ter-*32*.psf* \
        /usr/share/kbd/consolefonts/*ter-*32*.psf* \
        /usr/share/consolefonts/*Terminus*28x14*.psf* \
        /usr/share/consolefonts/*Terminus*24x12*.psf*; do
        [ -f "$candidate" ] || continue
        if setfont -C "$UPGRADE_UI_TTY" "$candidate" >/dev/null 2>&1; then
            log "Console dashboard large font: $candidate"
            return 0
        fi
    done

    if setfont -C "$UPGRADE_UI_TTY" -d >/dev/null 2>&1; then
        log "Console dashboard large font: setfont -d (double-size glyphs)"
        return 0
    fi
    log "Console dashboard: no supported large console font; keep existing font"
}
# 进度条表示五个升级阶段的完成情况，不伪造文件传输百分比。
# 界面状态文件放在 /run（tmpfs），通过原子重命名避免读取到不完整内容。
ui_publish()
{
    local state="$1" step="$2" title="$3" detail="$4"
    [ "$UI_STARTED" = "1" ] || return 0
    {
        printf '%s\n' "$state"
        printf '%s\n' "$step"
        printf '%s\n' "$title"
        printf '%s\n' "$detail"
    } >"${UI_STATUS_FILE}.$$" && \
    mv -f "${UI_STATUS_FILE}.$$" "$UI_STATUS_FILE" || true
}

# 界面排版辅助函数：根据实际终端行列数居中，包括字体调整后的尺寸。
# 使用纯 ASCII 字符，保证兼容常见 Linux 虚拟控制台字体。
UI_COLS=80
UI_ROWS=24
UI_CLOCK_ROW=0
ui_geometry()
{
    local dimensions=""
    dimensions=$(stty -F "$UPGRADE_UI_TTY" size 2>/dev/null || true)
    if [[ "$dimensions" =~ ^([0-9]+)[[:space:]]+([0-9]+)$ ]]; then
        UI_ROWS=${BASH_REMATCH[1]}
        UI_COLS=${BASH_REMATCH[2]}
    else
        UI_ROWS=24
        UI_COLS=80
    fi
    ((UI_ROWS >= 12)) || UI_ROWS=24
    ((UI_COLS >= 32)) || UI_COLS=80
}

ui_center()
{
    local message="$1" ink="${2:-}" len pad
    # 参与居中计算的文本不包含 ANSI 转义序列，避免宽度计算不准确。
    len=${#message}
    if ((len > UI_COLS - 2)); then
        message=${message:0:UI_COLS-2}
        len=${#message}
    fi
    pad=$(((UI_COLS-len)/2))
    ((pad >= 0)) || pad=0
    printf '%*s%s%s\033[0m\n' "$pad" '' "$ink" "$message" >&8
}

ui_render()
{
    local state step title detail
    local blue=$'\033[1;96m' white=$'\033[1;97m' gray=$'\033[0;37m'
    local green=$'\033[1;92m' yellow=$'\033[1;93m' red=$'\033[1;91m'
    local fill=0 i bar="" state_ink="$blue" state_text="UPGRADING"
    local line top line_count=21 stage_label
    [ "$UI_READY" = "1" ] || return 0
    [ -r "$UI_STATUS_FILE" ] || return 0
    {
        IFS= read -r state || return 0
        IFS= read -r step || return 0
        IFS= read -r title || return 0
        IFS= read -r detail || return 0
    } <"$UI_STATUS_FILE"
    [[ "$step" =~ ^[0-5]$ ]] || step=0
    ui_geometry
    case "$state" in
        success) state_text="UPGRADE SUCCESS"; state_ink="$green"; fill=5 ;;
        failed)  state_text="UPGRADE FAILED";  state_ink="$red";   fill=$((step>0 ? step-1 : 0)) ;;
        dryrun)  state_text="CHECK ONLY";      state_ink="$yellow"; fill=1 ;;
        *)       state_text="SYSTEM UPDATING"; state_ink="$blue"; fill=$((step>0 ? step-1 : 0)) ;;
    esac
    for ((i=0;i<5;i++)); do
        if ((i<fill)); then
            bar+="######"
        elif ((i==fill)) && [[ "$state" == running ]]; then
            bar+="==>---"
        else
            bar+="------"
        fi
    done

    # 在 1024×768 屏幕和 16×32 字体下，24 行控制台可容纳 21 行界面。
    # 如果终端更高，则按实际行数垂直居中，而非固定显示在左上角。
    top=$(((UI_ROWS-line_count)/2))
    ((top >= 0)) || top=0
    printf '\033[?25l\033[40m\033[2J\033[H' >&8 2>/dev/null || true
    for ((line=0;line<top;line++)); do printf '\n' >&8; done
    ui_center '       .--.       ' "$blue"
    ui_center '      |o_o |      ' "$white"
    ui_center '      |:_/ |      ' "$white"
    ui_center '     //   \ \     ' "$white"
    ui_center '    (|     |)    ' "$white"
    ui_center "   /'\\_   _/\`\\   " "$white"
    ui_center '   \___)=(___/   ' "$white"
    ui_center ''
    ui_center 'L I N U X   S Y S T E M' "$blue"
    ui_center 'O F F L I N E   U P D A T E' "$white"
    ui_center '----------------------------------------' "$gray"
    ui_center "$state_text" "$state_ink"
    ui_center "$title" "$state_ink"
    ui_center "$detail" "$white"
    ui_center ''
    ui_center "[$bar]" "$blue"
    case "$state" in
        success) stage_label='ALL 5 STAGES COMPLETE' ;;
        failed)  stage_label="FAILED AT STAGE $step / 5" ;;
        dryrun)  stage_label='PREVIEW ONLY - NO FLASH WRITES' ;;
        *)       stage_label="STAGE $step / 5   (STEP-BASED DISPLAY)" ;;
    esac
    ui_center "$stage_label" "$gray"
    # 计时器位于界面顶部起第 18 行（行号从 1 开始）。
    UI_CLOCK_ROW=$((top + 18))
    if [ "$state" = running ]; then
        ui_center 'STAGE TIME: 00:00' "$gray"
    else
        ui_center '                      ' "$gray"
    fi
    ui_center ''
    case "$state" in
        success)
            ui_center 'UPDATE COMPLETE - VERIFIED' "$green"
            ui_center 'REMOVE SD CARD AFTER POWER OFF' "$yellow" ;;
        failed)
            ui_center 'CHECK LOG - TARGET MAY NOT BOOT' "$red"
            ui_center 'REINSERT SD FOR A FULL RETRY' "$yellow" ;;
        dryrun)
            ui_center 'eMMC WAS NOT MODIFIED' "$yellow"
            ui_center 'CHECK ONLY - NO UPGRADE' "$gray" ;;
        *)
            ui_center '!!! DO NOT POWER OFF !!!' "$yellow"
            ui_center '!!! DO NOT REMOVE SD CARD !!!' "$yellow" ;;
    esac
    printf '\033[0m' >&8 2>/dev/null || true
}

ui_dashboard_loop()
{
    local owner_pid="$1" previous="" snapshot="" state=""
    local stage_begin=$SECONDS elapsed minutes seconds
    # 显示子进程不持有升级锁（启动时关闭文件描述符 9）。
    while kill -0 "$owner_pid" 2>/dev/null; do
        if [ -r "$UI_STATUS_FILE" ]; then
            snapshot=$(<"$UI_STATUS_FILE")
            if [ "$snapshot" != "$previous" ]; then
                ui_render
                previous="$snapshot"
                stage_begin=$SECONDS
            fi
            IFS= read -r state <"$UI_STATUS_FILE" || state=""
            if [ "$state" = "running" ] && ((UI_CLOCK_ROW > 0)); then
                elapsed=$((SECONDS - stage_begin))
                minutes=$((elapsed / 60))
                seconds=$((elapsed % 60))
                printf '\033[%d;1H\033[2K' "$UI_CLOCK_ROW" >&8 2>/dev/null || true
                ui_center "STAGE TIME: $(printf '%02d:%02d' "$minutes" "$seconds")" $'\033[1;97m'
            fi
        fi
        sleep 1
    done
}
ui_begin()
{
    [ "$UI_READY" = "1" ] || return 0
    UI_STARTED=1
    if [ "$UPGRADE_UI_SWITCH" = "1" ] && [ "$UPGRADE_UI_TTY" = /dev/tty1 ] && \
       command -v chvt >/dev/null 2>&1; then
        chvt 1 >/dev/null 2>&1 || true
    fi
    ui_font
    ui_publish running 1 "CHECKING DEVICE" "Checking SD and eMMC partitions"
    ui_render
    # 显示子进程关闭文件描述符 9，防止主进程意外退出后升级锁仍被占用。
    ui_dashboard_loop "$BASHPID" 9>&- &
    UI_PID=$!
    log "Console dashboard active on $UPGRADE_UI_TTY (pid=$UI_PID)"
}

ui_finish()
{
    [ "$UI_STARTED" = "1" ] || return 0
    if [ -n "$UI_PID" ]; then
        kill "$UI_PID" 2>/dev/null || true
        wait "$UI_PID" 2>/dev/null || true
        UI_PID=""
    fi
    ui_render
    printf '\033[?25h\033[0m' >&8 2>/dev/null || true
}

# 仅预览界面：不操作块设备，也不检查 SD/eMMC 设备。
ui_demo()
{
    local outcome="${1:-success}"
    if [ "$UI_READY" != "1" ]; then
        echo "Dashboard unavailable: connect a Linux VT or set UPGRADE_UI_TTY."
        return 1
    fi
    ui_begin
    ui_publish running 1 "CHECKING DEVICE" "Found boot SD and target eMMC"
    sleep 1
    ui_publish running 2 "PREPARING UPDATE" "Stopping business services"
    sleep 1
    ui_publish running 3 "SYNCING ROOTFS" "Copying system files to eMMC"
    sleep 2
    ui_publish running 4 "WRITING BOOT" "Flashing U-Boot and Boot"
    sleep 1
    ui_publish running 5 "VERIFYING SYSTEM" "Checking rootfs, U-Boot and Boot"
    sleep 1
    if [ "$outcome" = "failed" ]; then
        ui_publish failed 5 "VERIFICATION ERROR" "Example: rootfs check failed"
    else
        ui_publish success 5 "SYSTEM VERIFIED" "eMMC update completed"
    fi
    ui_finish
    return 0
}


die()
{
    UI_ERROR_LAST="$*"
    log "ERROR: $*"
    exit 1
}

require_commands()
{
    local cmd
    for cmd in \
        awk blkid blockdev chmod chown cmp dd e2fsck findmnt flock \
        lsblk mktemp mount mv parted readlink rsync sync systemctl umount; do
        command -v "$cmd" >/dev/null 2>&1 ||
            die "Required command not found: $cmd"
    done
}

# 从分区设备名提取整盘设备名，例如 /dev/mmcblk1p3 → /dev/mmcblk1。
parent_disk()
{
    local dev pk
    dev=$(readlink -f "$1")

    if [[ "$dev" =~ ^(/dev/mmcblk[0-9]+)p[0-9]+$ ]]; then
        echo "${BASH_REMATCH[1]}"
        return 0
    fi

    pk=$(lsblk -ndo PKNAME "$dev" 2>/dev/null | head -1 || true)
    if [ -n "$pk" ]; then
        echo "/dev/$pk"
        return 0
    fi
    return 1
}

mmc_type()
{
    local name
    name=$(basename "$1")
    [ -r "/sys/class/block/$name/device/type" ] || return 1
    tr -d '\r\n' < "/sys/class/block/$name/device/type"
}

find_emmc()
{
    local x type found=""
    for x in /sys/class/block/mmcblk[0-9]*; do
        [ -e "$x/device/type" ] || continue
        type=$(cat "$x/device/type" 2>/dev/null || true)
        if [ "$type" = "MMC" ]; then
            [ -z "$found" ] || die "More than one eMMC/MMC device found"
            found="/dev/$(basename "$x")"
        fi
    done
    [ -n "$found" ] || return 1
    echo "$found"
}

partdev()
{
    echo "${1}p${2}"
}

part_start()
{
    parted -sm "$1" unit s print |
        awk -F: -v n="$2" '$1 == n {gsub(/s/, "", $2); print $2; exit}'
}

part_size()
{
    parted -sm "$1" unit s print |
        awk -F: -v n="$2" '$1 == n {gsub(/s/, "", $4); print $4; exit}'
}

# 2+8GB 与 4+32GB 板型共用的固定分区布局：
#   p1 U-Boot：起始扇区 16384，大小 16384 扇区。
#   p2 Boot：起始扇区 32768，大小 262144 扇区。
#   p3 rootfs：起始扇区 294912，大小允许不同。
check_layout()
{
    local disk="$1" role="$2"
    local p1_start p1_size p2_start p2_size p3_start

    log "Checking $role layout: $disk"
    p1_start=$(part_start "$disk" 1)
    p1_size=$(part_size "$disk" 1)
    p2_start=$(part_start "$disk" 2)
    p2_size=$(part_size "$disk" 2)
    p3_start=$(part_start "$disk" 3)

    log "p1: start=$p1_start size=$p1_size"
    log "p2: start=$p2_start size=$p2_size"
    log "p3: start=$p3_start"

    [ "$p1_start" = "16384" ] || die "$role p1 start mismatch"
    [ "$p1_size" = "16384" ] || die "$role p1 size mismatch"
    [ "$p2_start" = "32768" ] || die "$role p2 start mismatch"
    [ "$p2_size" = "262144" ] || die "$role p2 size mismatch"
    [ "$p3_start" = "294912" ] || die "$role p3 start mismatch"
    log "$role layout OK"
}

unmount_emmc()
{
    local name mountpoint
    log "Checking mounted eMMC partitions..."
    while read -r name mountpoint; do
        [ -n "$name" ] || continue
        [ -n "${mountpoint:-}" ] || continue
        if [[ "/dev/$name" == "$EMMC_DISK"* ]]; then
            log "Unmount /dev/$name from $mountpoint"
            if [ "$AUTO_APPLY" = "1" ]; then
                umount "/dev/$name"
            fi
        fi
    done < <(lsblk -nr -o NAME,MOUNTPOINT "$EMMC_DISK")
}

# 使用文件级 rsync 同步 rootfs，不备份旧 eMMC。
# 排除 SD 系统运行日志及首次启动标志；这些内容不属于待升级的固件文件。
# -O 不同步目录修改时间，避免运行期间目录时间变化造成不必要的差异。
rsync_rootfs()
{
    local src="$1" dst="$2"
    rsync \
        -aHAXxSO \
        --numeric-ids \
        --delete \
        --info=progress2 \
        --exclude='/boot/***' \
        --exclude='/proc/***' \
        --exclude='/sys/***' \
        --exclude='/dev/***' \
        --exclude='/run/***' \
        --exclude='/tmp/***' \
        --exclude='/mnt/***' \
        --exclude='/home/cat/face_media/*.db*' \
        --exclude='/home/cat/face_media/volume.dat' \
        --exclude='/home/cat/face_media/modules' \
        --exclude='/media/***' \
        --exclude='/var/log/***' \
        --exclude='/usr/local/first_boot_flag' \
        --exclude='/lost+found' \
        --exclude='/var/lib/emmc-sd-upgrade/***' \
        "$src" "$dst"
}

# 修复目标 eMMC 的 /etc/fstab 中 /boot 的自动挂载项。
# rootfs 来自 SD，复制后可能留下 /dev/mmcblk1p2 等 SD 专用项，
# 或同时存在多条 /boot 项，导致 eMMC 启动后 boot.mount 依赖失败。
# 动态读取目标 eMMC p2 的 PARTUUID，避免写死 /dev/mmcblk0p2。
# 注意：/boot 位于独立的 p2，仍通过后续 dd 完整更新；rsync 的排除规则不得移除。
fix_emmc_boot_fstab()
{
    local fstab="$TARGET_MNT/etc/fstab"
    local tmp partuuid

    [ -f "$fstab" ] || die "目标 eMMC 缺少 /etc/fstab"

    partuuid=$(blkid -s PARTUUID -o value "$EMMC_BOOT" 2>/dev/null) || \
        die "无法读取目标 eMMC Boot 分区 PARTUUID：$EMMC_BOOT"
    [ -n "$partuuid" ] || die "目标 eMMC Boot 分区 PARTUUID 为空：$EMMC_BOOT"

    # 在目标 rootfs 的 /etc 内创建临时文件，最后以重命名方式替换。
    tmp=$(mktemp "$TARGET_MNT/etc/.fstab.XXXXXX") || \
        die "无法创建 eMMC fstab 临时文件"

    # 保留原有其他挂载项和注释，仅删除所有已有的 /boot 挂载项。
    # 无论原来使用设备节点、UUID 还是 PARTUUID，都只保留最终新增的一条。
    awk '
        /^[[:space:]]*#/ { print; next }
        $2 == "/boot" { next }
        { print }
    ' "$fstab" > "$tmp"

    printf 'PARTUUID=%s  /boot  ext2  defaults  0  2\n' "$partuuid" >> "$tmp"

    # fstab 可能具有定制的属主及权限，替换后应与原文件保持一致。
    chown --reference="$fstab" "$tmp"
    chmod --reference="$fstab" "$tmp"
    mv -f -- "$tmp" "$fstab"

    log "已修复目标 eMMC /boot 自动挂载项：PARTUUID=$partuuid"
}

# 不执行 rootfs 逐文件 checksum 全量校验：
# 当前 / 是正在运行的 SD 系统，系统服务可能持续修改其中的文件。
# 改为依赖 rsync 的退出状态，以及对目标 ext4 和必要系统文件的快速检查。

ensure_mountpoints()
{
    local root="$1"
    mkdir -p \
        "$root/boot" "$root/proc" "$root/sys" "$root/dev" \
        "$root/run" "$root/tmp" "$root/mnt" "$root/media"
}

stop_business_services()
{
    local svc
    log "Stopping services that must not run during eMMC upgrade..."
    for svc in "${BUSINESS_SERVICES[@]}"; do
        # SD 启动系统可能没有安装某些业务服务，不应因此报错。
        if systemctl cat "$svc" >/dev/null 2>&1; then
            log "Stop $svc"
            systemctl stop "$svc" || die "Cannot stop $svc"
            if systemctl is-active --quiet "$svc"; then
                die "$svc is still active"
            fi
        else
            log "Service not installed, skip: $svc"
        fi
    done
}

# 清理旧版“带回滚备份”脚本在 SD 上留下的备份数据。
# 不删除任何设备的未完成升级标志。
cleanup_legacy_backups()
{
    local old_dir
    for old_dir in \
        "$STATE_DIR"/devices/*/backup \
        "$STATE_DIR"/devices/*/backup.tmp; do
        [ -d "$old_dir" ] || continue
        log "Remove obsolete SD rollback backup: $old_dir"
        rm -rf -- "$old_dir"
    done
}

# 仅在检测到上次升级中断时，尝试修复因断电造成的不一致 ext4 文件系统。
# 自动修复无法重建已经严重损坏的文件系统；这种情况下必须停止升级，
# 并由维护人员手动修复或重新格式化。
repair_interrupted_rootfs()
{
    local rc
    [ -f "$INPROGRESS_FILE" ] || return 0

    log "Unfinished upgrade marker detected for CID=$EMMC_CID"
    log "Checking target ext4 before restarting from scratch..."
    rc=0
    e2fsck -p "$EMMC_ROOT" || rc=$?

    # e2fsck 返回码：0=文件系统正常，1=已修复错误；其他返回码需人工处理。
    if [ "$rc" -gt 1 ]; then
        die "eMMC rootfs fsck cannot auto-repair (rc=$rc); manual repair required"
    fi

    log "eMMC filesystem is ready for full re-upgrade"
}

# 以 eMMC CID 区分的标志不是永久“禁止重复升级”标志。
# 升级中断则保留标志，下次遇到该设备时直接重新执行完整升级。
arm_upgrade_marker()
{
    mkdir -p "$DEVICE_DIR"
    if [ -f "$INPROGRESS_FILE" ]; then
        log "Previous upgrade of THIS eMMC was incomplete: $EMMC_CID"
        log "No rollback is required or attempted. Retrying a complete upgrade."
        return 0
    fi

    {
        echo "cid=$EMMC_CID"
        echo "started_at=$(date '+%F %T')"
        echo "mode=retry-only-no-backup"
    } > "$INPROGRESS_FILE.tmp"

    # 必须先把未完成标志写入 SD 并刷新落盘，再开始修改目标 eMMC。
    sync
    mv -f "$INPROGRESS_FILE.tmp" "$INPROGRESS_FILE"
    sync
    log "Upgrade marker armed: $INPROGRESS_FILE"
}

flash_raw_partition()
{
    local name="$1" src="$2" dst="$3"
    local src_size dst_size
    src_size=$(blockdev --getsize64 "$src")
    dst_size=$(blockdev --getsize64 "$dst")

    log "Flash $name"
    log "  source : $src"
    log "  target : $dst"
    log "  srcsize: $src_size"
    log "  dstsize: $dst_size"
    [ "$src_size" -le "$dst_size" ] ||
        die "$name source partition is larger than target"

    dd if="$src" of="$dst" bs=4M conv=fsync status=progress
    sync
}

flash_rootfs()
{
    local src_root="$1" dst_root="$2"
    log "Updating rootfs..."
    log "source rootfs: $src_root"
    log "target rootfs: $dst_root"

    umount "$TARGET_MNT" 2>/dev/null || true
    mount "$dst_root" "$TARGET_MNT"

    # 当前系统根目录 / 来自 SD 卡的 p3 分区。
    rsync_rootfs / "$TARGET_MNT/"
    ensure_mountpoints "$TARGET_MNT"
    # 修复同步过来的 SD fstab，保证新 eMMC 系统启动后能自动挂载自身 p2。
    fix_emmc_boot_fstab
    sync
    umount "$TARGET_MNT"
    log "rootfs synchronization finished; pending final verification"
}

verify_new_system()
{
    local fsck_rc=0
    log "===== VERIFY NEW eMMC SYSTEM ====="

    # rsync 完成后，执行 fsck 前必须先卸载 eMMC 的 rootfs 分区。
    if findmnt -rn -S "$EMMC_ROOT" >/dev/null 2>&1; then
        log "ERROR: target rootfs is still mounted, refuse fsck"
        return 1
    fi

    # -p：按需检查并自动修复，不使用 -f 强制全盘扫描，
    # 也不使用 -n 纯只读检查；e2fsck 返回 0=正常、1=已修复。
    log "Checking rootfs ext4 (quick, not forced)..."
    e2fsck -p "$EMMC_ROOT" > /run/emmc-rootfs-fsck.log 2>&1 || fsck_rc=$?
    if (( fsck_rc != 0 && fsck_rc != 1 )); then
        cat /run/emmc-rootfs-fsck.log || true
        log "ERROR: eMMC rootfs filesystem check failed (rc=$fsck_rc)"
        return 1
    fi
    log "Rootfs ext4 check OK (rc=$fsck_rc)"

    # 通过只读挂载，确认目标 rootfs 文件系统可以正常访问。
    if ! mount -o ro "$EMMC_ROOT" "$TARGET_MNT"; then
        log "ERROR: cannot mount eMMC rootfs for verification"
        return 1
    fi
    if [ ! -e "$TARGET_MNT/sbin/init" ] || \
       [ ! -f "$TARGET_MNT/etc/os-release" ]; then
        log "ERROR: target rootfs lacks /sbin/init or /etc/os-release"
        umount "$TARGET_MNT" || true
        return 1
    fi
    if ! umount "$TARGET_MNT"; then
        log "ERROR: cannot unmount eMMC rootfs after verification"
        return 1
    fi
    log "Rootfs basic file checks OK (full checksum intentionally skipped)"

    log "Checking uboot (byte-for-byte)..."
    cmp -n "$(blockdev --getsize64 "$SD_UBOOT")" \
        "$SD_UBOOT" "$EMMC_UBOOT" || return 1
    log "Checking boot (byte-for-byte)..."
    cmp -n "$(blockdev --getsize64 "$SD_BOOT")" \
        "$SD_BOOT" "$EMMC_BOOT" || return 1

    log "New eMMC system basic verification OK"
}

save_log_copy()
{
    local result="$1" stamp
    mkdir -p "$STATE_DIR/logs"
    stamp=$(date '+%Y%m%d-%H%M%S')
    cp "$LOG" "$STATE_DIR/logs/${EMMC_CID:-unknown}-${stamp}-${result}.log" \
        2>/dev/null || true
    sync
}

on_exit()
{
    local rc=$?
    trap - EXIT INT TERM
    set +e
    if [ "$UI_DEMO" != "1" ]; then
        umount "$TARGET_MNT" 2>/dev/null || true
    fi

    if [ "$rc" -ne 0 ]; then
        ui_publish failed 0 "UPDATE INTERRUPTED" "${UI_ERROR_LAST:-See log for error details}"
        log "Upgrade aborted, rc=$rc"
        if [ -n "${INPROGRESS_FILE:-}" ] && [ -f "$INPROGRESS_FILE" ]; then
            log "INCOMPLETE: marker retained for this CID; no rollback backup exists."
            log "Reboot from upgrade SD to perform a fresh full upgrade."
        fi
        if [ -n "${EMMC_CID:-}" ]; then
            save_log_copy "failed"
        fi
    fi
    ui_finish
    exit "$rc"
}

trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

main()
{
    log "============================================"
    log "RK3568 SD -> eMMC offline upgrade (no rollback backup)"
    log "AUTO_APPLY=$AUTO_APPLY"
    log "============================================"
    require_commands
    ui_begin

    ROOT_SRC=$(findmnt -n -o SOURCE /)
    ROOT_SRC=$(readlink -f "$ROOT_SRC")
    log "Current root source: $ROOT_SRC"
    SD_DISK=$(parent_disk "$ROOT_SRC") || die "Cannot determine root disk"
    ROOT_TYPE=$(mmc_type "$SD_DISK" || true)
    log "Current root disk: $SD_DISK"
    log "Current root type: $ROOT_TYPE"

    if [ "$ROOT_TYPE" != "SD" ]; then
        log "Current system is not booted from SD. Skip eMMC upgrade."
        ui_publish dryrun 1 "NOT BOOTED FROM SD" "No eMMC changes were made"
        return 0
    fi

    EMMC_DISK=$(find_emmc) || die "eMMC not found"
    log "Detected eMMC: $EMMC_DISK"
    [ "$SD_DISK" != "$EMMC_DISK" ] ||
        die "SD disk and eMMC disk are identical"

    EMMC_NAME=$(basename "$EMMC_DISK")
    EMMC_CID=$(cat "/sys/class/block/$EMMC_NAME/device/cid" 2>/dev/null || true)
    [ -n "$EMMC_CID" ] || die "Cannot read eMMC CID"
    DEVICE_DIR="$STATE_DIR/devices/$EMMC_CID"
    INPROGRESS_FILE="$DEVICE_DIR/upgrade.in_progress"
    log "eMMC CID: $EMMC_CID"

    check_layout "$SD_DISK" "SD"
    check_layout "$EMMC_DISK" "eMMC"

    SD_UBOOT=$(partdev "$SD_DISK" 1)
    SD_BOOT=$(partdev "$SD_DISK" 2)
    SD_ROOT=$(partdev "$SD_DISK" 3)
    EMMC_UBOOT=$(partdev "$EMMC_DISK" 1)
    EMMC_BOOT=$(partdev "$EMMC_DISK" 2)
    EMMC_ROOT=$(partdev "$EMMC_DISK" 3)

    log "Upgrade map:"
    log "$SD_UBOOT -> $EMMC_UBOOT"
    log "$SD_BOOT  -> $EMMC_BOOT"
    log "$SD_ROOT  -> $EMMC_ROOT"

    SD_BOOT_FS=$(blkid -s TYPE -o value "$SD_BOOT" || true)
    SD_ROOT_FS=$(blkid -s TYPE -o value "$SD_ROOT" || true)
    EMMC_ROOT_FS=$(blkid -s TYPE -o value "$EMMC_ROOT" || true)
    log "SD boot filesystem   : $SD_BOOT_FS"
    log "SD root filesystem   : $SD_ROOT_FS"
    log "eMMC root filesystem : $EMMC_ROOT_FS"
    [ "$SD_ROOT_FS" = "ext4" ] || die "SD rootfs is not ext4"
    # 上次写入中断后，blkid 可能暂时无法识别目标 ext4；
    # 允许在确有本设备未完成标志时，先使用 e2fsck 尝试修复后再确认类型。
    if [ "$EMMC_ROOT_FS" != "ext4" ]; then
        if [ "$AUTO_APPLY" != "1" ] || [ ! -f "$INPROGRESS_FILE" ]; then
            die "eMMC rootfs is not ext4; manual filesystem repair required"
        fi
        log "eMMC rootfs type unknown after interrupted upgrade; attempt fsck first"
    fi

    [ "$(blockdev --getsize64 "$SD_UBOOT")" -le \
      "$(blockdev --getsize64 "$EMMC_UBOOT")" ] ||
        die "uboot source partition is larger than target"
    [ "$(blockdev --getsize64 "$SD_BOOT")" -le \
      "$(blockdev --getsize64 "$EMMC_BOOT")" ] ||
        die "boot source partition is larger than target"

    unmount_emmc

    if [ "$AUTO_APPLY" != "1" ]; then
        log "DRY-RUN PASSED. No eMMC data was changed."
        ui_publish dryrun 1 "DRY-RUN PASSED" "No eMMC changes were made"
        log "Set AUTO_APPLY=1 in /etc/default/emmc-sd-upgrade to apply."
        return 0
    fi

    # 真正升级前，先停止所有业务程序和显示相关服务。
    ui_publish running 2 "PREPARING UPDATE" "Stopping business services"
    stop_business_services

    # 之前支持回滚的旧版脚本可能在 SD 卡上保留了体积较大的系统备份。
    # 仅清理这些旧备份（包括其他 CID 的备份），不删除任何设备的中断标志。
    cleanup_legacy_backups

    # 如果当前 eMMC 上次升级中断，则按需修复 ext4，
    # 然后重新从头升级全部内容，不执行旧系统回滚。
    repair_interrupted_rootfs
    EMMC_ROOT_FS=$(blkid -s TYPE -o value "$EMMC_ROOT" || true)
    [ "$EMMC_ROOT_FS" = "ext4" ] ||
        die "eMMC rootfs is not ext4 after check/repair; manual repair required"

    # 按设备 CID 创建未完成标志，但不会阻止后续再次升级。
    arm_upgrade_marker

    # 保留原有写入顺序和传输方式。
    sync
    ui_publish running 3 "SYNCING ROOTFS" "Copying system files to eMMC"
    log "===== UPDATE ROOTFS ====="
    flash_rootfs "$SD_ROOT" "$EMMC_ROOT"
    ui_publish running 4 "WRITING U-BOOT" "Flashing eMMC bootloader partition"
    log "===== FLASH UBOOT ====="
    flash_raw_partition "uboot" "$SD_UBOOT" "$EMMC_UBOOT"
    ui_publish running 4 "WRITING BOOT" "Flashing Linux boot partition"
    log "===== FLASH BOOT ====="
    flash_raw_partition "boot" "$SD_BOOT" "$EMMC_BOOT"
    sync

    # 统一校验三个分区；校验失败不进行旧系统恢复。
    ui_publish running 5 "VERIFYING SYSTEM" "Checking rootfs, U-Boot and Boot"
    verify_new_system || die "Final eMMC verification failed"

    log "Final verification PASSED; committing upgrade"
    rm -f "$INPROGRESS_FILE"
    sync
    save_log_copy "success"

    log "============================================"
    log "eMMC upgrade SUCCESS"
    ui_publish success 5 "SYSTEM VERIFIED" "eMMC update completed"
    ui_render
    log "uboot  : updated + verified"
    log "boot   : updated + verified"
    log "rootfs : synced + ext4/basic checks (no full checksum)"
    log "MiniLoader: NOT changed"
    log "Permanent anti-repeat marker: DISABLED"
    log "Old-eMMC backup and automatic rollback: DISABLED"
    log "Please remove SD card before next boot."
    log "============================================"

    if [ "$AUTO_POWEROFF" = "1" ]; then
        # 保留大字体“升级成功 / 关机后拔出 SD 卡”提示画面。
        if [ "$UI_STARTED" = "1" ] && [[ "$UPGRADE_UI_SUCCESS_HOLD" =~ ^[0-9]+$ ]] &&
           ((UPGRADE_UI_SUCCESS_HOLD > 0)); then
            sleep "$UPGRADE_UI_SUCCESS_HOLD"
        fi
        sleep 3
        systemctl --no-block poweroff
    fi
}

if [ "${1:-}" = "--ui-demo" ] || [ "${1:-}" = "--ui-demo-fail" ]; then
    # 仅预览界面，不读取或写入块设备、不停止服务、不执行真实升级。
    UI_DEMO=1
    if [ "${1:-}" = "--ui-demo-fail" ]; then
        ui_demo failed
    else
        ui_demo success
    fi
else
    main "$@"
fi
