#!/bin/bash

set -Eeuo pipefail

CONFIG=/etc/default/emmc-sd-upgrade
[ -f "$CONFIG" ] && . "$CONFIG"

AUTO_APPLY=${AUTO_APPLY:-0}
AUTO_POWEROFF=${AUTO_POWEROFF:-1}

LOG=/run/emmc-sd-upgrade.log
STATE_DIR=/var/lib/emmc-sd-upgrade
TARGET_MNT=/mnt/emmc-upgrade-root

mkdir -p "$STATE_DIR"
mkdir -p "$TARGET_MNT"

exec > >(tee -a "$LOG") 2>&1


log()
{
    echo "[$(date '+%F %T')] $*"
}


die()
{
    log "ERROR: $*"
    exit 1
}


#
# /dev/mmcblk1p3 -> /dev/mmcblk1
#
parent_disk()
{
    local dev
    dev=$(readlink -f "$1")

    if [[ "$dev" =~ ^(/dev/mmcblk[0-9]+)p[0-9]+$ ]]; then
        echo "${BASH_REMATCH[1]}"
        return 0
    fi

    local pk
    pk=$(lsblk -ndo PKNAME "$dev" 2>/dev/null | head -1 || true)

    if [ -n "$pk" ]; then
        echo "/dev/$pk"
        return 0
    fi

    return 1
}


mmc_type()
{
    local disk="$1"
    local name

    name=$(basename "$disk")

    [ -r "/sys/class/block/$name/device/type" ] || return 1

    tr -d '\r\n' < "/sys/class/block/$name/device/type"
}


#
# 找系统中的唯一 eMMC
#
find_emmc()
{
    local x type
    local found=""

    for x in /sys/class/block/mmcblk[0-9]*; do
        [ -e "$x/device/type" ] || continue

        type=$(cat "$x/device/type" 2>/dev/null || true)

        if [ "$type" = "MMC" ]; then
            if [ -n "$found" ]; then
                die "More than one eMMC/MMC device found"
            fi

            found="/dev/$(basename "$x")"
        fi
    done

    [ -n "$found" ] || return 1

    echo "$found"
}


#
# mmcblk 设备的分区设备名
#
partdev()
{
    local disk="$1"
    local num="$2"

    echo "${disk}p${num}"
}


#
# 获取 partition 起始 sector
#
part_start()
{
    local disk="$1"
    local num="$2"

    parted -sm "$disk" unit s print |
        awk -F: -v n="$num" '
            $1 == n {
                gsub(/s/, "", $2)
                print $2
                exit
            }
        '
}


#
# 获取 partition 大小 sector
#
part_size()
{
    local disk="$1"
    local num="$2"

    parted -sm "$disk" unit s print |
        awk -F: -v n="$num" '
            $1 == n {
                gsub(/s/, "", $4)
                print $4
                exit
            }
        '
}


#
# 当前版本固定布局：
#
# p1 uboot
# start=0x4000=16384
# size =0x4000=16384 sectors
#
# p2 boot
# start=0x8000=32768
# size =0x40000=262144 sectors
#
# p3 rootfs
# start=0x48000=294912
#
check_layout()
{
    local disk="$1"
    local role="$2"

    log "Checking $role layout: $disk"

    local p1_start p1_size
    local p2_start p2_size
    local p3_start

    p1_start=$(part_start "$disk" 1)
    p1_size=$(part_size "$disk" 1)

    p2_start=$(part_start "$disk" 2)
    p2_size=$(part_size "$disk" 2)

    p3_start=$(part_start "$disk" 3)

    log "p1: start=$p1_start size=$p1_size"
    log "p2: start=$p2_start size=$p2_size"
    log "p3: start=$p3_start"

    [ "$p1_start" = "16384" ] ||
        die "$role p1 start mismatch"

    [ "$p1_size" = "16384" ] ||
        die "$role p1 size mismatch"

    [ "$p2_start" = "32768" ] ||
        die "$role p2 start mismatch"

    [ "$p2_size" = "262144" ] ||
        die "$role p2 size mismatch"

    [ "$p3_start" = "294912" ] ||
        die "$role p3 start mismatch"

    log "$role layout OK"
}


#
# 卸载 eMMC 上所有已经自动挂载的分区
#
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

    done < <(
        lsblk -nr -o NAME,MOUNTPOINT "$EMMC_DISK"
    )
}


#
# 完整写固定分区
#
flash_raw_partition()
{
    local name="$1"
    local src="$2"
    local dst="$3"

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

    if [ "$AUTO_APPLY" != "1" ]; then
        log "DRY-RUN: skip dd $name"
        return
    fi

    dd \
        if="$src" \
        of="$dst" \
        bs=4M \
        conv=fsync \
        status=progress

    sync

    log "Verifying $name..."

    if ! cmp -n "$src_size" "$src" "$dst"; then
        die "$name verification failed"
    fi

    log "$name verification OK"
}


#
# rootfs 使用文件级全量同步。
#
# 原因：
# SD rootfs 是 grow 分区，有可能比 eMMC rootfs 大，
# 不能简单整分区 dd。
#
flash_rootfs()
{
    local src_root="$1"
    local dst_root="$2"

    log "Updating rootfs..."
    log "source rootfs: $src_root"
    log "target rootfs: $dst_root"

    command -v rsync >/dev/null 2>&1 ||
        die "rsync not installed"

    if [ "$AUTO_APPLY" != "1" ]; then
        log "DRY-RUN: rootfs synchronization skipped"
        return
    fi

    umount "$TARGET_MNT" 2>/dev/null || true

    mount "$dst_root" "$TARGET_MNT"

    #
    # -a  archive
    # -H  hardlink
    # -A  ACL
    # -X  xattr/capability
    # -x  不跨文件系统
    # --delete 删除旧系统中、新系统已经不存在的文件
    #
    # boot 是独立分区，所以不复制 /boot 内容。
    #
    rsync \
        -aHAXx \
        --numeric-ids \
        --delete \
        --info=progress2 \
        --exclude='/boot/*' \
        --exclude='/proc/*' \
        --exclude='/sys/*' \
        --exclude='/dev/*' \
        --exclude='/run/*' \
        --exclude='/tmp/*' \
        --exclude='/mnt/*' \
        --exclude='/media/*' \
        --exclude='/lost+found' \
        --exclude='/var/lib/emmc-sd-upgrade/*' \
        / "$TARGET_MNT/"

    #
    # 确保标准 mountpoint 存在
    #
    mkdir -p \
        "$TARGET_MNT/boot" \
        "$TARGET_MNT/proc" \
        "$TARGET_MNT/sys" \
        "$TARGET_MNT/dev" \
        "$TARGET_MNT/run" \
        "$TARGET_MNT/tmp" \
        "$TARGET_MNT/mnt" \
        "$TARGET_MNT/media"

    #
    # 基础启动文件检查
    #
    [ -e "$TARGET_MNT/sbin/init" ] ||
        die "Target rootfs missing /sbin/init"

    [ -f "$TARGET_MNT/etc/os-release" ] ||
        die "Target rootfs missing /etc/os-release"

    sync

    umount "$TARGET_MNT"

    log "rootfs synchronization OK"
}


main()
{
    log "============================================"
    log "RK3568 SD -> eMMC upgrade"
    log "AUTO_APPLY=$AUTO_APPLY"
    log "============================================"

    #
    # 1. 当前 / 必须来自 SD
    #
    ROOT_SRC=$(findmnt -n -o SOURCE /)
    ROOT_SRC=$(readlink -f "$ROOT_SRC")

    log "Current root source: $ROOT_SRC"

    SD_DISK=$(parent_disk "$ROOT_SRC") ||
        die "Cannot determine root disk"

    ROOT_TYPE=$(mmc_type "$SD_DISK" || true)

    log "Current root disk: $SD_DISK"
    log "Current root type: $ROOT_TYPE"

    if [ "$ROOT_TYPE" != "SD" ]; then
        log "Current system is not booted from SD."
        log "Skip eMMC upgrade."
        exit 0
    fi


    #
    # 2. 自动找到 eMMC
    #
    EMMC_DISK=$(find_emmc) ||
        die "eMMC not found"

    log "Detected eMMC: $EMMC_DISK"

    [ "$SD_DISK" != "$EMMC_DISK" ] ||
        die "SD disk and eMMC disk are identical"


    #
    # 3. 获取 eMMC CID
    #
    EMMC_NAME=$(basename "$EMMC_DISK")

    EMMC_CID=$(
        cat "/sys/class/block/$EMMC_NAME/device/cid" \
            2>/dev/null ||
        echo unknown
    )

    DONE_FILE="$STATE_DIR/${EMMC_CID}.done"

    if [ -f "$DONE_FILE" ]; then
        log "This eMMC has already been upgraded with this SD card."
        log "Marker: $DONE_FILE"
        exit 0
    fi


    #
    # 4. 检查 SD 和 eMMC 分区布局
    #
    check_layout "$SD_DISK" "SD"
    check_layout "$EMMC_DISK" "eMMC"


    #
    # 5. 当前固定映射
    #
    SD_UBOOT=$(partdev "$SD_DISK" 1)
    SD_BOOT=$(partdev "$SD_DISK" 2)
    SD_ROOT=$(partdev "$SD_DISK" 3)

    EMMC_UBOOT=$(partdev "$EMMC_DISK" 1)
    EMMC_BOOT=$(partdev "$EMMC_DISK" 2)
    EMMC_ROOT=$(partdev "$EMMC_DISK" 3)

    log "============================================"
    log "Upgrade map:"
    log "$SD_UBOOT -> $EMMC_UBOOT"
    log "$SD_BOOT  -> $EMMC_BOOT"
    log "$SD_ROOT  -> $EMMC_ROOT"
    log "============================================"


    #
    # 6. 文件系统检查
    #
    SD_BOOT_FS=$(blkid -s TYPE -o value "$SD_BOOT" || true)
    SD_ROOT_FS=$(blkid -s TYPE -o value "$SD_ROOT" || true)

    log "SD boot filesystem : $SD_BOOT_FS"
    log "SD root filesystem : $SD_ROOT_FS"

    [ "$SD_ROOT_FS" = "ext4" ] ||
        die "SD rootfs is not ext4"


    #
    # 7. 卸载 eMMC 自动挂载分区
    #
    unmount_emmc


    if [ "$AUTO_APPLY" != "1" ]; then

        log "============================================"
        log "DRY-RUN PASSED."
        log "No eMMC data was changed."
        log ""
        log "If all information above is correct:"
        log "set AUTO_APPLY=1"
        log "in /etc/default/emmc-sd-upgrade"
        log "============================================"

        exit 0
    fi


    #
    # 8. 开始实际升级
    #
    sync


    log "===== UPDATE ROOTFS ====="

    flash_rootfs \
        "$SD_ROOT" \
        "$EMMC_ROOT"


    log "===== FLASH UBOOT ====="

    flash_raw_partition \
        "uboot" \
        "$SD_UBOOT" \
        "$EMMC_UBOOT"


    log "===== FLASH BOOT ====="

    flash_raw_partition \
        "boot" \
        "$SD_BOOT" \
        "$EMMC_BOOT"

    sync

    #
    # 9. 标记成功
    #
    date > "$DONE_FILE"

    cp "$LOG" "${DONE_FILE%.done}.log" 2>/dev/null || true

    sync


    log "============================================"
    log "eMMC upgrade SUCCESS"
    log ""
    log "uboot  : updated"
    log "boot   : updated"
    log "rootfs : updated"
    log ""
    log "MiniLoader: NOT changed"
    log ""
    log "Please remove SD card before next boot."
    log "============================================"


    if [ "$AUTO_POWEROFF" = "1" ]; then
        sleep 3
        systemctl --no-block poweroff
    fi
}


trap '
    rc=$?
    log "Upgrade aborted, rc=$rc"
    umount "$TARGET_MNT" 2>/dev/null || true
    exit $rc
' ERR


main "$@"


