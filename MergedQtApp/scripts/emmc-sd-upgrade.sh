#!/bin/bash
# RK3568 SD boot -> eMMC offline upgrade (without rollback backup).
# If interrupted, boot the SAME SD card again to retry the full upgrade.

set -Eeuo pipefail

CONFIG=/etc/default/emmc-sd-upgrade
[ -f "$CONFIG" ] && . "$CONFIG"

AUTO_APPLY=${AUTO_APPLY:-0}
AUTO_POWEROFF=${AUTO_POWEROFF:-1}

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
exec > >(tee -a "$LOG") 2>&1

# Prevent two upgrade processes from writing the same eMMC at once.
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    echo "Another emmc-sd-upgrade instance is already running."
    exit 1
fi

log()
{
    echo "[$(date '+%F %T')] $*"
}

die()
{
    log "ERROR: $*"
    exit 1
}

require_commands()
{
    local cmd
    for cmd in \
        awk blkid blockdev cmp dd e2fsck findmnt flock \
        lsblk mount mv parted readlink rsync sync systemctl umount; do
        command -v "$cmd" >/dev/null 2>&1 ||
            die "Required command not found: $cmd"
    done
}

# /dev/mmcblk1p3 -> /dev/mmcblk1
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

# Fixed layout for both 2+8 and 4+32:
#   p1 uboot: start=16384, size=16384 sectors
#   p2 boot : start=32768, size=262144 sectors
#   p3 rootfs: start=294912; size may differ.
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

# Copy rootfs without old-eMMC backup.
# Exclude SD runtime logs and its first-boot marker: they are not firmware content.
# -O prevents volatile directory mtimes from causing false verification failures.
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
        --exclude='/media/***' \
        --exclude='/var/log/***' \
        --exclude='/usr/local/first_boot_flag' \
        --exclude='/lost+found' \
        --exclude='/var/lib/emmc-sd-upgrade/***' \
        "$src" "$dst"
}

# Full rootfs file-by-file checksum verification is intentionally omitted:
# / is the running SD system, and system services mutate files continuously.
# We rely on rsync exit status and a quick target ext4/basic-file check.

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
        # Avoid errors if a business unit is not present on the SD boot card.
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

# Remove backups left by the OLD, rollback-enabled script.
# No in-progress markers for any device are deleted here.
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

# On retry only, repair an ext4 journal/filesystem left inconsistent by power loss.
# Automatic repair does not recreate a destroyed filesystem; in that rare case,
# stop and require a deliberate manual recovery/reformat.
repair_interrupted_rootfs()
{
    local rc
    [ -f "$INPROGRESS_FILE" ] || return 0

    log "Unfinished upgrade marker detected for CID=$EMMC_CID"
    log "Checking target ext4 before restarting from scratch..."
    rc=0
    e2fsck -p "$EMMC_ROOT" || rc=$?

    # e2fsck: 0=clean, 1=errors corrected. Other statuses need intervention.
    if [ "$rc" -gt 1 ]; then
        die "eMMC rootfs fsck cannot auto-repair (rc=$rc); manual repair required"
    fi

    log "eMMC filesystem is ready for full re-upgrade"
}

# Per-CID marker is *not* a permanent anti-repeat flag.
# An interrupted run leaves the marker; the next run simply retries.
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

    # Flush the marker BEFORE beginning to modify the target eMMC.
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

    # Current / is mounted from SD p3.
    rsync_rootfs / "$TARGET_MNT/"
    ensure_mountpoints "$TARGET_MNT"
    sync
    umount "$TARGET_MNT"
    log "rootfs synchronization finished; pending final verification"
}

verify_new_system()
{
    local fsck_rc=0
    log "===== VERIFY NEW eMMC SYSTEM ====="

    # After the rsync phase, eMMC rootfs MUST be unmounted before fsck.
    if findmnt -rn -S "$EMMC_ROOT" >/dev/null 2>&1; then
        log "ERROR: target rootfs is still mounted, refuse fsck"
        return 1
    fi

    # -p: only check/repair as necessary. Do NOT use -f (forced full scan)
    # or -n (which never repairs). e2fsck returns 0=clean, 1=corrected.
    log "Checking rootfs ext4 (quick, not forced)..."
    e2fsck -p "$EMMC_ROOT" > /run/emmc-rootfs-fsck.log 2>&1 || fsck_rc=$?
    if (( fsck_rc != 0 && fsck_rc != 1 )); then
        cat /run/emmc-rootfs-fsck.log || true
        log "ERROR: eMMC rootfs filesystem check failed (rc=$fsck_rc)"
        return 1
    fi
    log "Rootfs ext4 check OK (rc=$fsck_rc)"

    # Read-only mount confirms the target filesystem is accessible.
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
    umount "$TARGET_MNT" 2>/dev/null || true

    if [ "$rc" -ne 0 ]; then
        log "Upgrade aborted, rc=$rc"
        if [ -n "${INPROGRESS_FILE:-}" ] && [ -f "$INPROGRESS_FILE" ]; then
            log "INCOMPLETE: marker retained for this CID; no rollback backup exists."
            log "Reboot from upgrade SD to perform a fresh full upgrade."
        fi
        if [ -n "${EMMC_CID:-}" ]; then
            save_log_copy "failed"
        fi
    fi
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

    ROOT_SRC=$(findmnt -n -o SOURCE /)
    ROOT_SRC=$(readlink -f "$ROOT_SRC")
    log "Current root source: $ROOT_SRC"
    SD_DISK=$(parent_disk "$ROOT_SRC") || die "Cannot determine root disk"
    ROOT_TYPE=$(mmc_type "$SD_DISK" || true)
    log "Current root disk: $SD_DISK"
    log "Current root type: $ROOT_TYPE"

    if [ "$ROOT_TYPE" != "SD" ]; then
        log "Current system is not booted from SD. Skip eMMC upgrade."
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
    # On a previous interrupted write, blkid might be unable to identify ext4
    # until e2fsck has repaired it. Allow only that known retry case through.
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
        log "Set AUTO_APPLY=1 in /etc/default/emmc-sd-upgrade to apply."
        return 0
    fi

    # Stop all business/UI services before the actual upgrade.
    stop_business_services

    # The previous rollback-enabled version left huge backups on SD.
    # Clean only those backups, including other CIDs, but never their markers.
    cleanup_legacy_backups

    # If this specific eMMC was interrupted, repair ext4 if needed, then
    # retry a COMPLETE fresh upgrade, never a rollback.
    repair_interrupted_rootfs
    EMMC_ROOT_FS=$(blkid -s TYPE -o value "$EMMC_ROOT" || true)
    [ "$EMMC_ROOT_FS" = "ext4" ] ||
        die "eMMC rootfs is not ext4 after check/repair; manual repair required"

    # Record a per-device unfinished upgrade marker; do not block future upgrades.
    arm_upgrade_marker

    # Original write order and transfer methods are retained.
    sync
    log "===== UPDATE ROOTFS ====="
    flash_rootfs "$SD_ROOT" "$EMMC_ROOT"
    log "===== FLASH UBOOT ====="
    flash_raw_partition "uboot" "$SD_UBOOT" "$EMMC_UBOOT"
    log "===== FLASH BOOT ====="
    flash_raw_partition "boot" "$SD_BOOT" "$EMMC_BOOT"
    sync

    # Complete verification, all three partitions; no recovery on failure.
    verify_new_system || die "Final eMMC verification failed"

    log "Final verification PASSED; committing upgrade"
    rm -f "$INPROGRESS_FILE"
    sync
    save_log_copy "success"

    log "============================================"
    log "eMMC upgrade SUCCESS"
    log "uboot  : updated + verified"
    log "boot   : updated + verified"
    log "rootfs : synced + ext4/basic checks (no full checksum)"
    log "MiniLoader: NOT changed"
    log "Permanent anti-repeat marker: DISABLED"
    log "Old-eMMC backup and automatic rollback: DISABLED"
    log "Please remove SD card before next boot."
    log "============================================"

    if [ "$AUTO_POWEROFF" = "1" ]; then
        sleep 3
        systemctl --no-block poweroff
    fi
}

main "$@"
