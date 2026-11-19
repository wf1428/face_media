#!/bin/bash
# @file emmc-sd-upgrade.sh
# @brief 从 SD 启动后离线写入并校验 eMMC，按配置在成功后关机。
# @author Du lin
# @note 最近修改：2026-11-17；修改人：Du lin；识别 SD 与 eMMC 的野火 A/B 布局，按槽同步根文件系统与引导，保留全局槽位选择和共享数据。
# RK3568：从 SD 卡启动 Ubuntu，将系统离线升级到 eMMC（不备份旧系统、不提供自动回滚）。
# 如果升级被中断，重新插入同一张 SD 启动卡并启动，即可重新执行完整升级。

set -Eeuo pipefail

# 常规输出和工具进度不记录；错误通过显式提交分支进入 journal。
exec >/dev/null 2>&1

# @brief 每次开机按错误键向 journal 提交一次错误，跨服务重启生效。
# @param $1 无斜杠的稳定错误键；其余参数为首条错误的详细信息。
# /run 中仅建立空标记。mkdir 原子占用键；提交失败释放标记，记录故障不影响业务返回值。
journal_error_once() (
    error_key=$1
    shift
    error_dir=/run/face-media-errors/emmc-sd-upgrade
    claimed=0
    if mkdir -p "$error_dir" 2>/dev/null; then
        if mkdir "$error_dir/$error_key" 2>/dev/null; then
            claimed=1
        elif [ -d "$error_dir/$error_key" ]; then
            exit 0
        fi
    fi
    if ! logger -p daemon.err -t emmc-sd-upgrade -- "$*" 2>/dev/null; then
        [ "$claimed" -eq 0 ] || rmdir "$error_dir/$error_key" 2>/dev/null || true
    fi
)

CONFIG=/etc/default/emmc-sd-upgrade
[ -f "$CONFIG" ] && . "$CONFIG"

AUTO_APPLY=${AUTO_APPLY:-0}
AUTO_POWEROFF=${AUTO_POWEROFF:-1}

# Linux 文本控制台升级界面；设置 UPGRADE_UI=0 可关闭屏幕进度。
UPGRADE_UI=${UPGRADE_UI:-1}
UPGRADE_UI_TTY=${UPGRADE_UI_TTY:-/dev/tty1}
UPGRADE_UI_FONT=${UPGRADE_UI_FONT:-auto}  # auto=自动选择字体；off=不修改字体；也可指定 .psf 字体的绝对路径
UPGRADE_UI_SWITCH=${UPGRADE_UI_SWITCH:-1}
# 成功提示的额外展示时间（秒），默认 0；仅升级界面已启动时叠加到固定的 3 秒等待上。
UPGRADE_UI_SUCCESS_HOLD=${UPGRADE_UI_SUCCESS_HOLD:-0}

STATE_DIR=/var/lib/emmc-sd-upgrade
TARGET_MNT=/mnt/emmc-upgrade-root
AB_SOURCE_MNT=
AB_MODE=0
LOCK_FILE=/run/emmc-sd-upgrade.lock

BUSINESS_SERVICES=(
    face-media.service
    disable-console-blank.service
    udisk-watcher.service
    weston-kiosk.service
)

# LCD 进度使用独立的文件描述符 8。
UI_READY=0
UI_PID=""
UI_STARTED=0
UI_ERROR_LAST=""
UI_DEMO=0
UI_STATUS_FILE=/run/emmc-sd-upgrade-ui.status

# @brief 记录未显式处理的失败命令，原命令返回码由 errexit 保留。
# @param $1 命令退出码；$2 脚本行号；$3 命令文本。键不含时间或 PID。
on_upgrade_error()
{
    [ -z "$UI_ERROR_LAST" ] || return 0
    UI_ERROR_LAST="Command failed at line $2 (rc=$1): $3"
    journal_error_once "command-$2-$1" "$UI_ERROR_LAST"
}

trap 'on_upgrade_error "$?" "$LINENO" "$BASH_COMMAND"' ERR
mkdir -p "$STATE_DIR" "$TARGET_MNT"
if [ "$UPGRADE_UI" = "1" ] && [ -c "$UPGRADE_UI_TTY" ] && \
   { exec 8>"$UPGRADE_UI_TTY"; } 2>/dev/null; then
    UI_READY=1
fi

# 使用文件锁，防止多个升级进程同时写入同一块 eMMC。
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    journal_error_once upgrade-lock "Another emmc-sd-upgrade instance is already running."
    exit 1
fi

# LCD 界面仅负责显示：即使显示出错，也不能影响升级和校验结果。
# ANSI 控制字符只写入文件描述符 8；状态文件供仪表盘读取。
ui_font()
{
    local candidate
    [ "$UI_READY" = "1" ] || return 0
    [ "$UPGRADE_UI_FONT" != "off" ] || return 0
    command -v setfont >/dev/null 2>&1 || {
        return 0
    }

    if [ "$UPGRADE_UI_FONT" = "double" ]; then
        # setfont -d 可将 8×16 字形放大为 16×32，不依赖额外的字体文件。
        setfont -C "$UPGRADE_UI_TTY" -d >/dev/null 2>&1 || true
        return 0
    fi

    if [ "$UPGRADE_UI_FONT" != "auto" ]; then
        [ -f "$UPGRADE_UI_FONT" ] || {
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
            return 0
        fi
    done

    if setfont -C "$UPGRADE_UI_TTY" -d >/dev/null 2>&1; then
        return 0
    fi
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
            ui_center 'CHECK JOURNAL - TARGET MAY NOT BOOT' "$red"
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
        journal_error_once demo-console "Dashboard unavailable: connect a Linux VT or set UPGRADE_UI_TTY."
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


# @brief 保留具体错误供 LCD 显示并终止升级，退出状态为 1。
# @param $* 出错原因；已记录的下层错误优先用于 LCD 和 journal。
die()
{
    if [ -z "$UI_ERROR_LAST" ]; then
        local fingerprint
        UI_ERROR_LAST="$*"
        fingerprint=$(printf '%s\n' "$UI_ERROR_LAST" | cksum) || fingerprint=unknown
        journal_error_once "failure-${fingerprint%% *}" "$UI_ERROR_LAST"
    fi
    exit 1
}

# @brief 提交已识别的失败并保留原命令诊断，调用方决定返回码或退出。
# @param $1 稳定错误键；$2 LCD 错误摘要；$3 可选的工具诊断，仅提交至 journal。
upgrade_error()
{
    local error_key="$1" detail="${3:-}"
    UI_ERROR_LAST="$2"
    journal_error_once "$error_key" "$UI_ERROR_LAST${detail:+: $detail}"
}

# @brief 运行升级工具并保留其退出码；只在失败时提交有限长度的标准错误。
# @param $1 错误键；$2 步骤说明；其余参数为命令及其参数。
run_upgrade_command()
{
    local error_key="$1" description="$2" detail rc
    shift 2
    if detail=$("$@" 2>&1 >/dev/null | LC_ALL=C awk '
        length(message) < 512 { message = message sep substr($0, 1, 512 - length(message)); sep = " " }
        END { print message }
    '); then
        return 0
    else
        rc=$?
    fi
    upgrade_error "$error_key" "$description (rc=$rc)" "$detail"
    return "$rc"
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

# @brief 输出唯一 eMMC 节点；无设备返回 1，多个候选返回 2，由主流程记录错误。
find_emmc()
{
    local x type found=""
    for x in /sys/class/block/mmcblk[0-9]*; do
        [ -e "$x/device/type" ] || continue
        type=$(cat "$x/device/type" 2>/dev/null || true)
        if [ "$type" = "MMC" ]; then
            [ -z "$found" ] || return 2
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

    p1_start=$(part_start "$disk" 1)
    p1_size=$(part_size "$disk" 1)
    p2_start=$(part_start "$disk" 2)
    p2_size=$(part_size "$disk" 2)
    p3_start=$(part_start "$disk" 3)


    [ "$p1_start" = "16384" ] || die "$role p1 start mismatch"
    [ "$p1_size" = "16384" ] || die "$role p1 size mismatch"
    [ "$p2_start" = "32768" ] || die "$role p2 start mismatch"
    [ "$p2_size" = "262144" ] || die "$role p2 size mismatch"
    [ "$p3_start" = "294912" ] || die "$role p3 start mismatch"
}

unmount_emmc()
{
    local name mountpoint
    while read -r name mountpoint; do
        [ -n "$name" ] || continue
        [ -n "${mountpoint:-}" ] || continue
        if [[ "/dev/$name" == "$EMMC_DISK"* ]]; then
            if [ "$AUTO_APPLY" = "1" ]; then
                umount "/dev/$name"
            fi
        fi
    done < <(lsblk -nr -o NAME,MOUNTPOINT "$EMMC_DISK")
}

# @brief 校验完整 A/B GPT 布局及分区名称，避免按编号写入其他板型的布局。
# @param $1 整盘设备节点。@param $2 用于错误输出的设备角色。
check_ab_layout()
{
    local disk="$1" role="$2" i label expected
    local -a labels=(uboot boot boot_a boot_b rootfs_a rootfs_b data)
    local partition_table
    partition_table=$(parted -sm "$disk" unit s print)
    [ "$(printf '%s\n' "$partition_table" | awk -F: '$1 ~ /^[0-9]+$/ { n++ } END { print n }')" = 7 ] ||
        die "$role must contain seven A/B partitions"
    for i in 1 2 3 4 5 6 7; do
        [ -b "$(partdev "$disk" "$i")" ] || die "$role missing A/B partition p$i"
        label=$(printf '%s\n' "$partition_table" | awk -F: -v n="$i" '$1 == n {print $6; exit}')
        expected=${labels[i-1]}
        case "$i:$label" in
            2:misc|7:userdata) ;;
            *) [ "$label" = "$expected" ] || die "$role p$i expected $expected, found $label" ;;
        esac
    done

    [ "$(blockdev --getsize64 "$(partdev "$disk" 1)")" -eq $((8 * 1024 * 1024)) ] &&
    [ "$(blockdev --getsize64 "$(partdev "$disk" 2)")" -eq $((4 * 1024 * 1024)) ] &&
    [ "$(blockdev --getsize64 "$(partdev "$disk" 3)")" -eq $((128 * 1024 * 1024)) ] &&
    [ "$(blockdev --getsize64 "$(partdev "$disk" 4)")" -eq $((128 * 1024 * 1024)) ] ||
        die "$role A/B boot partition sizes do not match the deployed layout"
}

# @brief 使用实际设备的 PARTUUID 定位槽位及共享分区。
# @param $1 分区设备。@return 打印 PARTUUID；读取失败中止上层操作。
read_partuuid()
{
    local uuid
    uuid=$(blkid -s PARTUUID -o value "$1") || return 1
    [ -n "$uuid" ] || return 1
    printf '%s\n' "$uuid"
}

# @brief 确认对应插槽的完整来源及目标信息，刷写前执行。
# @note 应用识别只依赖 /userdata；实际写盘还需要双盘分区表、挂载和文件系统相符。
prepare_ab_layout()
{
    local userdata boot part root_type boot_type global_boot_fs
    userdata=$(findmnt -rn -M /userdata -o SOURCE) || die "/userdata exists but is not mounted"
    [ "$(readlink -f "$userdata")" = "$(partdev "$SD_DISK" 7)" ] ||
        die "/userdata is not mounted from SD p7"
    case "$ROOT_SRC" in
        "$(partdev "$SD_DISK" 5)") SD_ACTIVE_ROOT_PART=5 ;;
        "$(partdev "$SD_DISK" 6)") SD_ACTIVE_ROOT_PART=6 ;;
        *) die "SD / is not on A/B rootfs p5 or p6" ;;
    esac
    boot=$(findmnt -rn -M /boot -o SOURCE) || die "SD slot /boot is not mounted"
    [ "$(readlink -f "$boot")" = "$(partdev "$SD_DISK" "$((SD_ACTIVE_ROOT_PART - 2))")" ] ||
        die "SD /boot does not match its running rootfs slot"

    check_ab_layout "$SD_DISK" SD
    check_ab_layout "$EMMC_DISK" eMMC
    global_boot_fs=$(blkid -s TYPE -o value "$(partdev "$EMMC_DISK" 2)" || true)
    case "$global_boot_fs" in
        ext2|ext3|ext4) AB_GLOBAL_BOOT_FS=$global_boot_fs ;;
        *) die "eMMC global boot p2 does not have an ext filesystem" ;;
    esac
    AB_DATA_UUID=$(read_partuuid "$(partdev "$EMMC_DISK" 7)") || die "Cannot read target data PARTUUID"
    AB_DATA_FS=$(blkid -s TYPE -o value "$(partdev "$EMMC_DISK" 7)") || die "Cannot read target data filesystem"
    [ "$AB_DATA_FS" = ext4 ] || die "eMMC data partition is not ext4"
    for part in 3 4; do
        boot_type=$(blkid -s TYPE -o value "$(partdev "$SD_DISK" "$part")" || true)
        case "$boot_type" in ext2|ext3|ext4) ;; *) die "SD p$part boot filesystem is unsupported" ;; esac
        AB_BOOT_FS[part]=$boot_type
        AB_BOOT_UUID[part]=$(read_partuuid "$(partdev "$EMMC_DISK" "$part")") ||
            die "Cannot read target boot p$part PARTUUID"
    done
    for part in 5 6; do
        [ "$(blkid -s TYPE -o value "$(partdev "$SD_DISK" "$part")" || true)" = ext4 ] ||
            die "SD rootfs p$part is not ext4"
        AB_ROOT_UUID[part]=$(read_partuuid "$(partdev "$EMMC_DISK" "$part")") ||
            die "Cannot read target rootfs p$part PARTUUID"
        root_type=$(blkid -s TYPE -o value "$(partdev "$EMMC_DISK" "$part")" || true)
        if [ "$root_type" != ext4 ] && { [ "$AUTO_APPLY" != 1 ] || [ ! -f "$INPROGRESS_FILE" ]; }; then
            die "eMMC rootfs p$part is not ext4; manual filesystem repair required"
        fi
    done
}

# @brief 确保两套源槽启动文件都可读取。未运行的 SD 槽仅读挂载且禁用日志回放。
validate_ab_sources()
{
    local inactive_part boot_dev boot_options
    inactive_part=$((11 - SD_ACTIVE_ROOT_PART))
    AB_SOURCE_MNT=$(mktemp -d /mnt/emmc-upgrade-sd.XXXXXX)
    mount -o ro,noload "$(partdev "$SD_DISK" "$inactive_part")" "$AB_SOURCE_MNT"
    [ -e "$AB_SOURCE_MNT/sbin/init" ] && [ -f "$AB_SOURCE_MNT/etc/fstab" ] ||
        die "Inactive SD slot lacks init or fstab"
    [ -e /sbin/init ] && [ -f /etc/fstab ] && [ -s /boot/boot.scr ] ||
        die "Running SD slot lacks init, fstab or boot.scr"
    boot_dev=$(partdev "$SD_DISK" "$((inactive_part - 2))")
    boot_options=ro,noload
    if [ "${AB_BOOT_FS[inactive_part-2]}" = ext2 ]; then boot_options=ro; fi
    mount -o "$boot_options" "$boot_dev" "$TARGET_MNT"
    [ -s "$TARGET_MNT/boot.scr" ] || die "Inactive SD slot lacks boot.scr"
    umount "$TARGET_MNT"

    # p2 保持不写；先确认目标镜像已具备能选择 A/B 的全局引导脚本。
    boot_options=ro,noload
    if [ "$AB_GLOBAL_BOOT_FS" = ext2 ]; then boot_options=ro; fi
    mount -o "$boot_options" "$(partdev "$EMMC_DISK" 2)" "$TARGET_MNT"
    [ -s "$TARGET_MNT/boot.scr" ] || die "eMMC global boot p2 lacks boot.scr"
    umount "$TARGET_MNT"
}

# 使用文件级 rsync 同步 rootfs，不备份旧 eMMC。
# 排除共享业务存储、SD 系统运行日志及首次启动标志。
# -O 不同步目录修改时间，避免运行期间目录时间变化造成不必要的差异。
rsync_rootfs()
{
    local src="$1" dst="$2"

    # 排除不需要同步的目录及设备本地数据
    local -a excludes=(
        '/boot/***'                                # 排除启动分区
        '/proc/***'                                # 排除内核进程虚拟文件系统
        '/sys/***'                                 # 排除内核设备虚拟文件系统
        '/dev/***'                                 # 排除设备节点
        '/run/***'                                 # 排除系统运行时数据
        '/tmp/***'                                 # 排除临时文件
        '/mnt/***'                                 # 排除临时挂载目录
        '/userdata/***'                            # 排除 A/B 共用业务存储

        # 门禁业务数据
        '/home/cat/face_media/*.db*'               # 排除门禁主库文件
        '/home/cat/face_media/volume.dat'          # 排除设备音量配置
        '/home/cat/face_media/modules'             # 排除门禁管理员配置

        # 人员照片及通行记录
        '/home/cat/face_media/network_faces/'      # 排除网络人员照片
        '/home/cat/face_media/enrollments/'        # 排除本地人员照片
        '/home/cat/face_media/data/'               # 排除通行文本日志和验证抓拍数据
        '/home/cat/face_media/通行记录_*.csv'      # 排除通行记录 CSV 文件
        '/home/cat/face_media/backup/'             # 排除数据库备份

        # Web 服务本地数据
        '/home/cat/web-stage/imports/'             # 排除 Web 服务导入文件

        # 系统相关目录及状态文件
        '/media/***'                               # 排除外部媒体挂载目录
        '/var/log/***'                             # 排除系统日志
        '/usr/local/first_boot_flag'               # 排除首次启动标记文件
        '/lost+found'                              # 排除文件系统恢复目录
        '/var/lib/emmc-sd-upgrade/***'             # 排除 eMMC/SD 升级工作目录
    )

    # 将排除路径转换为 rsync 参数
    local -a exclude_args=()
    local path

    for path in "${excludes[@]}"; do
        exclude_args+=(--exclude="$path")
    done

    # 执行根文件系统同步
    run_upgrade_command sync-rootfs "Rootfs synchronization failed" rsync \
        -aHAXxSO \
        --numeric-ids \
        --delete \
        --info=progress2 \
        "${exclude_args[@]}" \
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

}

# @brief 将两个系统槽绑定至各自 eMMC 的 /、/boot 和同一份 /userdata。
# @param $1 eMMC 根分区号，5 或 6。
fix_ab_fstab()
{
    local part="$1" boot_part=$(($1 - 2))
    local fstab="$TARGET_MNT/etc/fstab" tmp
    [ -f "$fstab" ] || die "Target A/B slot p$part lacks fstab"
    tmp=$(mktemp "$TARGET_MNT/etc/.fstab.XXXXXX")
    awk -v root="PARTUUID=${AB_ROOT_UUID[part]}" '
        /^[[:space:]]*#/ { print; next }
        $2 == "/boot" || $2 == "/userdata" { next }
        $2 == "/" {
            if (!seen++) { sub(/^[[:space:]]*[^[:space:]]+/, root); print }
            next
        }
        { print }
        END { if (!seen) print root "  /  ext4  defaults  0  1" }
    ' "$fstab" > "$tmp"
    printf 'PARTUUID=%s  /boot  %s  defaults  0  2\n' "${AB_BOOT_UUID[boot_part]}" "${AB_BOOT_FS[boot_part]}" >> "$tmp"
    printf 'PARTUUID=%s  /userdata  %s  defaults  0  2\n' "$AB_DATA_UUID" "$AB_DATA_FS" >> "$tmp"
    chown --reference="$fstab" "$tmp"
    chmod --reference="$fstab" "$tmp"
    mv -f -- "$tmp" "$fstab"
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
    for svc in "${BUSINESS_SERVICES[@]}"; do
        # SD 启动系统可能没有安装某些业务服务，不应因此报错。
        if systemctl cat "$svc" >/dev/null 2>&1; then
            systemctl stop "$svc" || die "Cannot stop $svc"
            if systemctl is-active --quiet "$svc"; then
                die "$svc is still active"
            fi
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
        rm -rf -- "$old_dir"
    done
}

# 仅在检测到上次升级中断时，尝试修复因断电造成的不一致 ext4 文件系统。
# 自动修复无法重建已经严重损坏的文件系统；这种情况下必须停止升级，
# 并由维护人员手动修复或重新格式化。
repair_interrupted_rootfs()
{
    local rc fsck_detail
    [ -f "$INPROGRESS_FILE" ] || return 0

    rc=0
    fsck_detail=$(e2fsck -p "$EMMC_ROOT" 2>&1) || rc=$?

    # e2fsck 返回码：0=文件系统正常，1=已修复错误；其他返回码需人工处理。
    if [ "$rc" -gt 1 ]; then
        upgrade_error "repair-fsck-$rc" "eMMC rootfs fsck cannot auto-repair (rc=$rc); manual repair required" "$fsck_detail"
        exit 1
    fi

}

# 以 eMMC CID 区分的标志不是永久“禁止重复升级”标志。
# 升级中断则保留标志，下次遇到该设备时直接重新执行完整升级。
arm_upgrade_marker()
{
    mkdir -p "$DEVICE_DIR"
    if [ -f "$INPROGRESS_FILE" ]; then
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
}

flash_raw_partition()
{
    local name="$1" src="$2" dst="$3"
    local src_size dst_size
    src_size=$(blockdev --getsize64 "$src")
    dst_size=$(blockdev --getsize64 "$dst")

    [ "$src_size" -le "$dst_size" ] ||
        die "$name source partition is larger than target"

    run_upgrade_command "flash-$name" "Failed to flash $name" \
        dd if="$src" of="$dst" bs=4M conv=fsync status=none
    sync
}

flash_rootfs()
{
    local src_root="$1" dst_root="$2"

    umount "$TARGET_MNT" 2>/dev/null || true
    run_upgrade_command mount-target "Failed to mount target rootfs" mount "$dst_root" "$TARGET_MNT"

    rsync_rootfs "$src_root" "$TARGET_MNT/"
    ensure_mountpoints "$TARGET_MNT"
    if [ "$AB_MODE" = 1 ]; then
        mkdir -p "$TARGET_MNT/userdata"
        fix_ab_fstab "${dst_root##*p}"
    else
        fix_emmc_boot_fstab
    fi
    sync
    umount "$TARGET_MNT"
}

# @brief 校验当前目标根分区的 ext4、一组系统文件以及相应启动分区。
# @return 完全验证后返回 0。
verify_new_system()
{
    local fsck_rc=0 fsck_detail

    # rsync 完成后，执行 fsck 前必须先卸载 eMMC 的 rootfs 分区。
    if findmnt -rn -S "$EMMC_ROOT" >/dev/null 2>&1; then
        upgrade_error verify-mounted "target rootfs is still mounted, refuse fsck"
        return 1
    fi

    # -p：按需检查并自动修复，不使用 -f 强制全盘扫描，
    # 也不使用 -n 纯只读检查；e2fsck 返回 0=正常、1=已修复。
    fsck_detail=$(e2fsck -p "$EMMC_ROOT" 2>&1) || fsck_rc=$?
    if (( fsck_rc != 0 && fsck_rc != 1 )); then
        upgrade_error "verify-fsck-$fsck_rc" "eMMC rootfs filesystem check failed (rc=$fsck_rc)" "$fsck_detail"
        return 1
    fi

    # 通过只读挂载，确认目标 rootfs 文件系统可以正常访问。
    if ! mount -o ro "$EMMC_ROOT" "$TARGET_MNT"; then
        upgrade_error verify-mount "cannot mount eMMC rootfs for verification"
        return 1
    fi
    if [ ! -e "$TARGET_MNT/sbin/init" ] || \
       [ ! -f "$TARGET_MNT/etc/os-release" ]; then
        upgrade_error verify-root-files "target rootfs lacks /sbin/init or /etc/os-release"
        umount "$TARGET_MNT" || true
        return 1
    fi
    if [ "$AB_MODE" = 1 ]; then
        local part=${EMMC_ROOT##*p}
        if ! awk -v root="PARTUUID=${AB_ROOT_UUID[part]}" -v boot="PARTUUID=${AB_BOOT_UUID[part-2]}" -v data="PARTUUID=$AB_DATA_UUID" '
            /^[[:space:]]*#/ { next }
            $2 == "/" { if ($1 != root) exit 1; root_seen++ }
            $2 == "/boot" { if ($1 != boot) exit 1; boot_seen++ }
            $2 == "/userdata" { if ($1 != data) exit 1; data_seen++ }
            END { if (root_seen != 1 || boot_seen != 1 || data_seen != 1) exit 1 }
        ' "$TARGET_MNT/etc/fstab"; then
            upgrade_error verify-fstab "target slot p$part has an invalid fstab"
            umount "$TARGET_MNT" || true
            return 1
        fi
    fi
    if ! umount "$TARGET_MNT"; then
        upgrade_error verify-umount "cannot unmount eMMC rootfs after verification"
        return 1
    fi

    cmp -n "$(blockdev --getsize64 "$SD_UBOOT")" \
        "$SD_UBOOT" "$EMMC_UBOOT" || {
            upgrade_error verify-uboot "U-Boot partition verification failed"
            return 1
        }
    cmp -n "$(blockdev --getsize64 "$SD_BOOT")" \
        "$SD_BOOT" "$EMMC_BOOT" || {
            upgrade_error verify-boot "Boot partition verification failed"
            return 1
        }

}


on_exit()
{
    local rc=$?
    trap - EXIT ERR INT TERM
    set +e
    if [ "$UI_DEMO" != "1" ]; then
        umount "$TARGET_MNT" 2>/dev/null || true
    fi

    if [ -n "$AB_SOURCE_MNT" ]; then
        umount "$AB_SOURCE_MNT" 2>/dev/null || true
        rmdir "$AB_SOURCE_MNT" 2>/dev/null || true
    fi

    if [ "$rc" -ne 0 ]; then
        if [ -z "$UI_ERROR_LAST" ]; then
            upgrade_error "exit-$rc" "Upgrade aborted (rc=$rc); see journal for details"
        fi
        ui_publish failed 0 "UPDATE INTERRUPTED" "$UI_ERROR_LAST"
    fi
    ui_finish
    exit "$rc"
}

trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

main()
{
    require_commands
    ui_begin

    ROOT_SRC=$(findmnt -n -o SOURCE /)
    ROOT_SRC=$(readlink -f "$ROOT_SRC")
    SD_DISK=$(parent_disk "$ROOT_SRC") || die "Cannot determine root disk"
    ROOT_TYPE=$(mmc_type "$SD_DISK" || true)

    if [ "$ROOT_TYPE" != "SD" ]; then
        ui_publish dryrun 1 "NOT BOOTED FROM SD" "No eMMC changes were made"
        return 0
    fi

    if EMMC_DISK=$(find_emmc); then
        :
    else
        [ "$?" -ne 2 ] || die "More than one eMMC/MMC device found"
        die "eMMC not found"
    fi
    [ "$SD_DISK" != "$EMMC_DISK" ] ||
        die "SD disk and eMMC disk are identical"

    EMMC_NAME=$(basename "$EMMC_DISK")
    EMMC_CID=$(cat "/sys/class/block/$EMMC_NAME/device/cid" 2>/dev/null || true)
    [ -n "$EMMC_CID" ] || die "Cannot read eMMC CID"
    DEVICE_DIR="$STATE_DIR/devices/$EMMC_CID"
    INPROGRESS_FILE="$DEVICE_DIR/upgrade.in_progress"

    if [ -d /userdata ]; then
        AB_MODE=1
        prepare_ab_layout
        upgrade_ab_system
        if [ "$AUTO_APPLY" = 1 ]; then finish_upgrade; fi
        return 0
    fi

    check_layout "$SD_DISK" "SD"
    check_layout "$EMMC_DISK" "eMMC"

    SD_UBOOT=$(partdev "$SD_DISK" 1)
    SD_BOOT=$(partdev "$SD_DISK" 2)
    SD_ROOT=$(partdev "$SD_DISK" 3)
    EMMC_UBOOT=$(partdev "$EMMC_DISK" 1)
    EMMC_BOOT=$(partdev "$EMMC_DISK" 2)
    EMMC_ROOT=$(partdev "$EMMC_DISK" 3)


    SD_BOOT_FS=$(blkid -s TYPE -o value "$SD_BOOT" || true)
    SD_ROOT_FS=$(blkid -s TYPE -o value "$SD_ROOT" || true)
    EMMC_ROOT_FS=$(blkid -s TYPE -o value "$EMMC_ROOT" || true)
    [ "$SD_ROOT_FS" = "ext4" ] || die "SD rootfs is not ext4"
    # 上次写入中断后，blkid 可能暂时无法识别目标 ext4；
    # 允许在确有本设备未完成标志时，先使用 e2fsck 尝试修复后再确认类型。
    if [ "$EMMC_ROOT_FS" != "ext4" ]; then
        if [ "$AUTO_APPLY" != "1" ] || [ ! -f "$INPROGRESS_FILE" ]; then
            die "eMMC rootfs is not ext4; manual filesystem repair required"
        fi
    fi

    [ "$(blockdev --getsize64 "$SD_UBOOT")" -le \
      "$(blockdev --getsize64 "$EMMC_UBOOT")" ] ||
        die "uboot source partition is larger than target"
    [ "$(blockdev --getsize64 "$SD_BOOT")" -le \
      "$(blockdev --getsize64 "$EMMC_BOOT")" ] ||
        die "boot source partition is larger than target"

    unmount_emmc

    if [ "$AUTO_APPLY" != "1" ]; then
        ui_publish dryrun 1 "DRY-RUN PASSED" "No eMMC changes were made"
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
    flash_rootfs / "$EMMC_ROOT"
    ui_publish running 4 "WRITING U-BOOT" "Flashing eMMC bootloader partition"
    flash_raw_partition "uboot" "$SD_UBOOT" "$EMMC_UBOOT"
    ui_publish running 4 "WRITING BOOT" "Flashing Linux boot partition"
    flash_raw_partition "boot" "$SD_BOOT" "$EMMC_BOOT"
    sync

    # 统一校验三个分区；校验失败不进行旧系统恢复。
    ui_publish running 5 "VERIFYING SYSTEM" "Checking rootfs, U-Boot and Boot"
    verify_new_system || die "Final eMMC verification failed"

    finish_upgrade
}

# @brief 离线更新两个对应槽，保留 p2 的全局引导/选槽标记和 p7 的共享数据。
upgrade_ab_system()
{
    local part src_root
    SD_UBOOT=$(partdev "$SD_DISK" 1)
    EMMC_UBOOT=$(partdev "$EMMC_DISK" 1)
    if [ "$AUTO_APPLY" != 1 ]; then
        ui_publish dryrun 1 "DRY-RUN PASSED" "A/B layout validated; no eMMC changes were made"
        return 0
    fi

    ui_publish running 2 "PREPARING A/B UPDATE" "Checking both SD system slots"
    stop_business_services
    unmount_emmc
    validate_ab_sources
    cleanup_legacy_backups
    for part in 5 6; do
        EMMC_ROOT=$(partdev "$EMMC_DISK" "$part")
        repair_interrupted_rootfs
        [ "$(blkid -s TYPE -o value "$EMMC_ROOT" || true)" = ext4 ] ||
            die "eMMC rootfs p$part remains invalid after check"
    done
    arm_upgrade_marker

    for part in 5 6; do
        EMMC_ROOT=$(partdev "$EMMC_DISK" "$part")
        src_root="$AB_SOURCE_MNT/"
        if [ "$part" -eq "$SD_ACTIVE_ROOT_PART" ]; then src_root=/; fi
        ui_publish running 3 "SYNCING A/B ROOTFS" "Copying source rootfs p$part to eMMC"
        flash_rootfs "$src_root" "$EMMC_ROOT"
    done
    ui_publish running 4 "WRITING U-BOOT" "Flashing eMMC bootloader partition"
    flash_raw_partition "uboot" "$SD_UBOOT" "$EMMC_UBOOT"
    for part in 3 4; do
        SD_BOOT=$(partdev "$SD_DISK" "$part")
        EMMC_BOOT=$(partdev "$EMMC_DISK" "$part")
        EMMC_ROOT=$(partdev "$EMMC_DISK" "$((part + 2))")
        ui_publish running 4 "WRITING A/B BOOT" "Flashing eMMC boot partition p$part"
        flash_raw_partition "boot-p$part" "$SD_BOOT" "$EMMC_BOOT"
        ui_publish running 5 "VERIFYING A/B SYSTEM" "Checking eMMC slot with boot p$part"
        verify_new_system || die "Final A/B verification failed for boot p$part"
    done
    umount "$AB_SOURCE_MNT"
    rmdir "$AB_SOURCE_MNT"
    AB_SOURCE_MNT=
}

# @brief 完成校验后清除中断标记，显示结果并按原配置关机。
finish_upgrade()
{
    rm -f "$INPROGRESS_FILE"
    sync

    ui_publish success 5 "SYSTEM VERIFIED" "eMMC update completed"
    ui_render

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
