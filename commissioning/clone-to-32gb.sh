#!/bin/bash
# commissioning/clone-to-32gb.sh — Write a card image onto a larger card and grow p3 to fill it
#
# Usage:
#   sudo ./commissioning/clone-to-32gb.sh --clone-from <image_or_device> /dev/sdX
#   sudo ./commissioning/clone-to-32gb.sh --expand-only /dev/sdX
#
# This script writes a card image (as rootfs/make-card-image.sh builds it, ~1.6 GB)
# onto a larger (16-128GB) SD card and grows p3 (the root filesystem, which also
# holds /home/root/data) to the end of the card.  The layout is ARCA_LAYOUT in
# lib/arca-identify.sh, three primaries:
#   p1: FAT32 boot, bootable (MLO, U-Boot, uImage, DTB)
#   p2: ext4, 1 GiB (logs, mounted at /home/root/log)
#   p3: ext4, root filesystem — LAST, so nothing sits behind it
#
# Strategy: dd the image, rewrite the table with p3's size dropped (sfdisk then
# extends it to the end of the disk), e2fsck + resize2fs.  The UUID is preserved
# because we resize, not reformat.
#
# Full background: SD_CARD_UPGRADE.md.  This script goes away once the unit
# grows p3 itself on first boot.

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -euo pipefail

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
MIN_TARGET_SIZE_GB=16
# Upper bound as well as a lower one: a minimum alone rejects a card as small as the image
# card and accepts a 4 TB disk.  Env-overridable for a genuinely large card.
MAX_TARGET_SIZE_GB=${MAX_TARGET_SIZE_GB:-128}
EXPECTED_PARTITION_COUNT=3  # The image has three primaries: boot, log, root
SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Layout fingerprint and the rest of "is this an Arca card".  Sourced
# rather than reimplemented: rootfs/make-card-image.sh asks the same question of
# the same cards.
#
# ⚠️ Do not gate this script on a rootfs UUID.  A filesystem UUID is generated at
# mkfs time, so it names ONE CARD: two Arca units on the identical firmware
# build share none of their UUIDs, so such a gate warns on every unit but
# the one the constant came from.  And it must never be "repaired" by running
# `tune2fs -U` to make a card match another — two cards claiming one UUID is a
# harder bug than the one it hides.  The partition LAYOUT is the invariant:
# U-Boot has root=/dev/mmcblk0p3 compiled in with no saveenv, so p3 is the rootfs
# by position on every unit.
# shellcheck source=../lib/arca-identify.sh
. "$REPO_ROOT/lib/arca-identify.sh"
# shellcheck source=../lib/product.sh
. "$REPO_ROOT/lib/product.sh"

# Captured by do_verify before repartitioning and checked again by
# do_final_verify.  The meaningful question after a resize is whether the UUID
# survived it, which is a comparison against this card's own earlier value — not
# against a constant copied from some other card.
ORIG_P3_UUID=""

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# ---------------------------------------------------------------------------
# Global state
# ---------------------------------------------------------------------------
MODE=""            # "clone" or "expand"
SOURCE=""          # Image file or source device (clone mode only)
DEVICE=""          # Target block device
DRY_RUN=false
ALLOW_FIXED_DISK=false   # --allow-fixed-disk: accept a target with removable=0
STEP=0

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
info()    { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*"; }
step()    { STEP=$((STEP + 1)); echo -e "\n${BLUE}${BOLD}=== Step ${STEP}: $* ===${NC}"; }
dry_run() { echo -e "${YELLOW}[DRY-RUN]${NC} $*"; }

# ---------------------------------------------------------------------------
# Error trap
# ---------------------------------------------------------------------------
on_error() {
    local exit_code=$?
    local line_no=$1
    error "Script failed at line ${line_no} (exit code ${exit_code})"
    if [ $STEP -gt 0 ]; then
        error "Failure occurred during Step ${STEP}"
    fi
    error "The target device may be in an inconsistent state."
    error "If cloning was complete, you can re-run with --expand-only to retry expansion."
    exit "${exit_code}"
}
trap 'on_error ${LINENO}' ERR

# ---------------------------------------------------------------------------
# Partition naming helper
# ---------------------------------------------------------------------------
# Given a base device like /dev/sdb or /dev/mmcblk0, return the partition
# device path for partition number N.
#   /dev/sdb   → /dev/sdb1, /dev/sdb2, ...
#   /dev/mmcblk0 → /dev/mmcblk0p1, /dev/mmcblk0p2, ...
part_dev() {
    local base="$1"
    local num="$2"
    if [[ "$base" =~ [0-9]$ ]]; then
        echo "${base}p${num}"
    else
        echo "${base}${num}"
    fi
}

# ---------------------------------------------------------------------------
# Usage / help
# ---------------------------------------------------------------------------
usage() {
    echo -e "${BOLD}${SCRIPT_NAME}${NC} — Write a card image to a larger card and grow p3"
    echo ""
    echo -e "${BOLD}USAGE${NC}"
    echo "  sudo ./${SCRIPT_NAME} --clone-from <image_or_device> <target_device>"
    echo "  sudo ./${SCRIPT_NAME} --expand-only <target_device>"
    echo "  sudo ./${SCRIPT_NAME} --help"
    echo ""
    echo -e "${BOLD}MODES${NC}"
    echo "  --clone-from <src>   Clone from an image file or source device, then expand."
    echo "                       Example: --clone-from card.img /dev/sdb"
    echo "                       Example: --clone-from /dev/sdc /dev/sdb"
    echo ""
    echo "  --expand-only        Skip cloning; expand partitions on a card that already"
    echo "                       has the $PRODUCT_NAME image cloned onto it."
    echo "                       Example: --expand-only /dev/sdb"
    echo ""
    echo -e "${BOLD}OPTIONS${NC}"
    echo "  --dry-run            Show what would be done without making any changes."
    echo "  --allow-fixed-disk   Accept a target whose /sys/block/<dev>/removable is 0."
    echo "                       Needed for a card reader that reports as fixed, and for"
    echo "                       any disk attached under WSL — every one of those reads 0,"
    echo "                       including the disk carrying /.  The root-disk, mount and"
    echo "                       size checks still apply and cannot be bypassed."
    echo "  --help, -h           Show this help message."
    echo ""
    echo -e "${BOLD}SAFETY${NC}"
    echo "  • Must be run as root (sudo)"
    echo "  • Refuses the disk carrying / — resolved through partitions, LVM and LUKS"
    echo "  • Refuses a target with anything mounted below it, holder tree included"
    echo "  • Refuses a partition; the target must be a whole disk"
    echo "  • Requires removable media unless --allow-fixed-disk is given"
    echo "  • Requires ${MIN_TARGET_SIZE_GB}–${MAX_TARGET_SIZE_GB} GB (override the max with MAX_TARGET_SIZE_GB=n)"
    echo "  • Requires explicit confirmation before destructive operations"
    echo ""
    echo -e "${BOLD}WHAT IT DOES${NC}"
    echo "  1. (Clone mode) dd the source image/device to the target"
    echo "  2. Verify the image's 3-partition layout and record the p3 UUID"
    echo "  3. Rewrite the table so p3 (root + data) runs to the end of the card"
    echo "  4. Grow the ext4 filesystem on p3 with e2fsck + resize2fs"
    echo "  5. Verify UUID is preserved"
    echo ""
    echo -e "${BOLD}PARTITION LAYOUT (the image's 3 primaries)${NC}"
    echo "  p1: FAT32  ~70.6 MB  bootable  (boot: MLO, U-Boot, uImage, DTB)"
    echo "  p2: ext4   1 GiB               (system logs)"
    echo "  p3: ext4   ~512 MB, GROWN      (root filesystem, holds /home/root/data)"
    echo ""
    echo -e "${BOLD}SEE ALSO${NC}"
    echo "  SD_CARD_UPGRADE.md — Full documentation of the upgrade procedure"
    echo ""
    exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
parse_args() {
    if [ $# -eq 0 ]; then
        usage
    fi

    while [ $# -gt 0 ]; do
        case "$1" in
            --help|-h)
                usage
                ;;
            --dry-run)
                DRY_RUN=true
                shift
                ;;
            --allow-fixed-disk)
                ALLOW_FIXED_DISK=true
                shift
                ;;
            --clone-from)
                MODE="clone"
                if [ $# -lt 2 ]; then
                    error "--clone-from requires a source argument"
                    exit 1
                fi
                SOURCE="$2"
                shift 2
                ;;
            --expand-only)
                MODE="expand"
                shift
                ;;
            -*)
                error "Unknown option: $1"
                echo "Run '${SCRIPT_NAME} --help' for usage."
                exit 1
                ;;
            *)
                # Positional argument = target device
                if [ -n "$DEVICE" ]; then
                    error "Multiple target devices specified: '${DEVICE}' and '$1'"
                    exit 1
                fi
                DEVICE="$1"
                shift
                ;;
        esac
    done

    # Validate required arguments
    if [ -z "$MODE" ]; then
        error "Must specify either --clone-from <source> or --expand-only"
        echo "Run '${SCRIPT_NAME} --help' for usage."
        exit 1
    fi

    if [ -z "$DEVICE" ]; then
        error "Must specify target device (e.g., /dev/sdb)"
        echo "Run '${SCRIPT_NAME} --help' for usage."
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        error "This script must be run as root (use sudo)"
        exit 1
    fi
}

check_dependencies() {
    local missing=()
    for cmd in dd sfdisk e2fsck resize2fs blkid blockdev partprobe lsblk; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        error "Missing required commands: ${missing[*]}"
        echo "Install with: sudo apt-get install -y util-linux e2fsprogs coreutils"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Target-device safety
#
# The old guard was a literal /dev/sda blacklist plus `mount | grep "^$dev"`, and
# both are wrong in ways that matter on the host this is actually run from.  Under
# WSL the root filesystem sits on /dev/sdd (measured 2026-08-03), so the blacklist
# protected a disk that is not the system disk while leaving the real one wide
# open — and a `wsl --mount`ed physical card lands on /dev/sdd or /dev/sde too.
# `mount` also never shows the disk behind an LVM or LUKS root (it shows
# /dev/mapper/…), so that check passed on exactly the systems where getting it
# wrong is unrecoverable.
# ---------------------------------------------------------------------------

# Whole-disk device name (no /dev/ prefix) backing $1, following partition and
# device-mapper parents.  Empty if it cannot be resolved.
whole_disk_of() {
    lsblk -rnso NAME "$1" 2>/dev/null | tail -1 | tr -d ' '
}

# The whole disk carrying the root filesystem.  Empty if it cannot be resolved —
# callers must treat that as "guard inactive", not as "safe".
root_whole_disk() {
    local src
    src=$(findmnt -no SOURCE --target / 2>/dev/null | head -1 || true)
    src=${src%%\[*}                      # strip a btrfs [/subvol] suffix
    [ -n "$src" ] && [ -b "$src" ] || return 0
    whole_disk_of "$src"
}

check_device_safe() {
    local dev="$1"

    # Must exist, and be a block device — everything below reads it.
    if [ ! -e "$dev" ]; then
        error "Device '${dev}' does not exist"
        exit 1
    fi
    if [ ! -b "$dev" ]; then
        error "'${dev}' is not a block device"
        exit 1
    fi

    local name
    name=$(basename "$(readlink -f "$dev")")

    # Must be a whole disk: this script writes a partition table.
    if [ -e "/sys/class/block/${name}/partition" ]; then
        error "'${dev}' is a partition, not a whole disk"
        error "Pass the disk itself — e.g. /dev/sdb, not /dev/sdb1"
        exit 1
    fi
    if [ ! -d "/sys/block/${name}" ]; then
        error "'${dev}' does not resolve to a whole disk (no /sys/block/${name})"
        exit 1
    fi

    # THE check: never the disk this system booted from.  Resolves through
    # partitions and device-mapper, so an LVM or LUKS root is caught.
    local root_disk
    root_disk=$(root_whole_disk)
    if [ -n "$root_disk" ]; then
        if [ "$name" = "$root_disk" ]; then
            error "Refusing to operate on ${dev} — it carries this system's root filesystem"
            error "  / is on /dev/${root_disk}"
            exit 1
        fi
        info "Root filesystem is on /dev/${root_disk}; target is a different disk"
    else
        warn "Could not determine which disk carries / — the root-disk guard is INACTIVE"
        warn "Read 'lsblk' yourself and be certain about ${dev} before you continue"
    fi

    # Nothing mounted anywhere below it — partitions, LVM, LUKS, the whole holder
    # tree.  `mount | grep "^${dev}"` saw none of that.
    local mounts
    mounts=$(lsblk -rno NAME,MOUNTPOINT "$dev" 2>/dev/null | awk 'NF > 1' || true)
    if [ -n "$mounts" ]; then
        error "Device '${dev}' has mounted filesystems:"
        echo "$mounts"
        error "Unmount them first, including any LVM/LUKS mapping stacked on top"
        exit 1
    fi

    # A size *window*.  A minimum alone is not a safety check — it rejects the
    # image-sized card and waves through a 4 TB drive.
    local size_bytes size_gb
    size_bytes=$(blockdev --getsize64 "$dev")
    size_gb=$(( size_bytes / 1073741824 ))
    if [ "$size_gb" -lt "$MIN_TARGET_SIZE_GB" ]; then
        error "Target device is ${size_gb} GB — minimum ${MIN_TARGET_SIZE_GB} GB required"
        error "This check ensures you're not accidentally writing to a card as small as the image"
        exit 1
    fi
    if [ "$size_gb" -gt "$MAX_TARGET_SIZE_GB" ]; then
        error "Target device is ${size_gb} GB — over the ${MAX_TARGET_SIZE_GB} GB maximum"
        error "A card for this device is 16–64 GB; something this large is probably a real disk"
        error "If you are certain, re-run with MAX_TARGET_SIZE_GB=${size_gb} in the environment"
        exit 1
    fi

    # Removable media by default — but only as a gate you must open deliberately,
    # never as a fact to lean on: plenty of USB disks report removable=0, and
    # under WSL *every* attached disk does, the one carrying / included.
    local removable
    removable=$(cat "/sys/block/${name}/removable" 2>/dev/null || echo "0")
    if [ "$removable" != "1" ]; then
        if $ALLOW_FIXED_DISK; then
            warn "${dev} is not removable media (removable=${removable}) — allowed by --allow-fixed-disk"
        else
            error "'${dev}' is not removable media (/sys/block/${name}/removable = ${removable})"
            error "If this really is your card reader, re-run with --allow-fixed-disk"
            error "Look at 'lsblk -o NAME,SIZE,TYPE,RM,MOUNTPOINT' before you do"
            exit 1
        fi
    fi

    info "Target device: ${dev} (${size_gb} GB, removable=${removable}) — OK"
}

check_source() {
    local src="$1"
    if [ -b "$src" ]; then
        info "Source is a block device: ${src}"
        # Check it's not the same as target
        if [ "$src" = "$DEVICE" ]; then
            error "Source and target are the same device!"
            exit 1
        fi
    elif [ -f "$src" ]; then
        local size_mb=$(( $(stat -c%s "$src") / 1048576 ))
        info "Source is an image file: ${src} (${size_mb} MB)"
    else
        error "Source '${src}' is not a block device or regular file"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# User confirmation
# ---------------------------------------------------------------------------
confirm() {
    local prompt="$1"
    if $DRY_RUN; then
        dry_run "Would ask: ${prompt}"
        return 0
    fi
    echo ""
    echo -e "${YELLOW}${BOLD}${prompt}${NC}"
    echo -n "Type 'yes' to continue, anything else to abort: "
    local answer
    read -r answer
    if [ "$answer" != "yes" ]; then
        error "Aborted by user"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Phase 1: Clone
# ---------------------------------------------------------------------------
do_clone() {
    step "Clone source image to target device"

    echo ""
    info "Source: ${SOURCE}"
    info "Target: ${DEVICE}"
    echo ""
    warn "This will OVERWRITE ALL DATA on ${DEVICE}!"
    echo ""

    # Show target device info
    lsblk "$DEVICE" 2>/dev/null || true
    echo ""

    confirm "Proceed with cloning ${SOURCE} → ${DEVICE}?"

    if $DRY_RUN; then
        dry_run "dd if=${SOURCE} of=${DEVICE} bs=4M status=progress conv=fsync"
        dry_run "sync"
        dry_run "partprobe ${DEVICE}"
        return
    fi

    info "Cloning (this may take several minutes)..."
    dd if="$SOURCE" of="$DEVICE" bs=4M status=progress conv=fsync
    info "Flushing buffers..."
    sync
    info "Re-reading partition table..."
    partprobe "$DEVICE"
    # Give the kernel a moment to settle
    sleep 2

    info "Clone complete"
}

# ---------------------------------------------------------------------------
# Phase 2: Verify partition structure
# ---------------------------------------------------------------------------
# Print ARCA_LAYOUT (n:start:size:type) one partition per line.
print_expected_layout() {
    local e n s z t
    for e in $ARCA_LAYOUT; do
        IFS=: read -r n s z t <<< "$e"
        if [ "$n" = "$ARCA_LAYOUT_GROW" ]; then
            warn "  p${n}: start=${s}, type=${t}  (size is whatever the card gave)"
        else
            warn "  p${n}: start=${s}, size=${z}, type=${t}"
        fi
    done
}

do_verify() {
    step "Verify partition structure"

    local p3_dev part_count
    p3_dev=$(part_dev "$DEVICE" 3)

    part_count=$(sfdisk -d "$DEVICE" | grep -c 'start=' || true)
    info "Found ${part_count} partition(s) on ${DEVICE}"

    if [ "$part_count" -ne "$EXPECTED_PARTITION_COUNT" ]; then
        error "Expected ${EXPECTED_PARTITION_COUNT} partitions, found ${part_count}"
        sfdisk -l "$DEVICE"
        error "The target does not have the $PRODUCT_NAME card layout (p1 boot, p2 log, p3 root)."
        error "A card from an older 7-partition image is rewritten from a current image, not grown."
        exit 1
    fi

    echo ""
    info "Current partition table:"
    sfdisk -l "$DEVICE"
    echo ""

    # Verify the LAYOUT, not a UUID.  arca_is_card_disk pins start and size of p1
    # and p2 and only the start of p3, so a card that was already grown matches.
    if arca_is_card_disk "$DEVICE"; then
        info "Partition layout matches the $PRODUCT_NAME card layout ✓"
    else
        warn "Partition layout does NOT match the $PRODUCT_NAME card layout. Expected:"
        print_expected_layout
        echo ""
        confirm "Layout does not match. Continue anyway?"
    fi

    # Record this card's own rootfs UUID.  Not compared against a constant: the
    # only thing worth asserting is that the resize preserves it, which
    # do_final_verify checks against the value captured here.
    if $DRY_RUN; then
        dry_run "Would record the UUID of ${p3_dev}"
        return
    fi

    ORIG_P3_UUID=$(blkid -s UUID -o value "$p3_dev" 2>/dev/null || echo "")

    if [ -z "$ORIG_P3_UUID" ]; then
        error "Could not read UUID from ${p3_dev}"
        error "Is the $PRODUCT_NAME image properly cloned to this card?"
        exit 1
    fi
    info "p3 UUID: ${ORIG_P3_UUID} (recorded; must survive the resize)"
}

# ---------------------------------------------------------------------------
# Phase 3: Repartition
# ---------------------------------------------------------------------------
do_repartition() {
    step "Repartition — grow p3 (root + data) to the end of the card"

    local saved_table p3_dev p3_line p3_start total_sectors new_table
    saved_table=$(sfdisk -d "$DEVICE")

    info "Current sfdisk dump:"
    echo "$saved_table"
    echo ""

    # Format: /dev/sdb3 : start=   2244608, size=   1048576, type=83
    p3_dev=$(part_dev "$DEVICE" 3)
    p3_line=$(echo "$saved_table" | grep "^${p3_dev} " || true)
    if [ -z "$p3_line" ]; then
        error "Could not parse ${p3_dev} from the sfdisk dump"
        exit 1
    fi
    p3_start=$(echo "$p3_line" | sed -E 's/.*start= *([0-9]+).*/\1/')
    OLD_P3_SIZE=$(echo "$p3_line" | sed -E 's/.*size= *([0-9]+).*/\1/')

    total_sectors=$(blockdev --getsz "$DEVICE")
    NEW_P3_SIZE=$(( total_sectors - p3_start ))
    info "Device sectors: ${total_sectors}; p3 starts at ${p3_start}"
    info "p3: $(( OLD_P3_SIZE * 512 / 1048576 )) MB → $(( NEW_P3_SIZE * 512 / 1048576 )) MB; p1 and p2 unchanged"

    # The dump already is the table (label-id, p1 with its bootable flag, p2);
    # drop p3's size= and sfdisk extends it to the end of the disk.
    new_table=$(echo "$saved_table" | sed -E "s|^(${p3_dev} :.*)size= *[0-9]+, |\1|")
    if [ "$new_table" = "$saved_table" ]; then
        error "Could not drop size= from the ${p3_dev} line of the dump"
        exit 1
    fi

    confirm "Apply new partition table to ${DEVICE}?"

    if $DRY_RUN; then
        dry_run "Would apply the following partition table:"
        echo "$new_table"
        dry_run "partprobe ${DEVICE}"
        return
    fi

    info "Writing new partition table..."
    echo "$new_table" | sfdisk --force "$DEVICE"

    info "Re-reading partition table..."
    partprobe "$DEVICE"
    sleep 2

    info "Repartition complete"
    echo ""
    info "New partition table:"
    sfdisk -l "$DEVICE"
}

# ---------------------------------------------------------------------------
# Phase 4: Filesystem expansion
# ---------------------------------------------------------------------------
do_resize_fs() {
    step "Expand ext4 filesystem on rootfs (p3)"

    local p3_dev
    p3_dev=$(part_dev "$DEVICE" 3)

    if $DRY_RUN; then
        dry_run "e2fsck -f -y ${p3_dev}"
        dry_run "resize2fs ${p3_dev}"
        return
    fi

    info "Running filesystem check on ${p3_dev}..."
    # e2fsck returns 1 if errors were corrected, which is OK for us
    e2fsck -f -y "$p3_dev" || {
        local rc=$?
        if [ $rc -le 1 ]; then
            info "e2fsck completed (corrected minor issues)"
        else
            error "e2fsck failed with exit code ${rc}"
            exit 1
        fi
    }

    info "Resizing filesystem to fill partition..."
    resize2fs "$p3_dev"

    info "Filesystem expansion complete"
}

# ---------------------------------------------------------------------------
# Phase 5: Final verification
# ---------------------------------------------------------------------------
do_final_verify() {
    step "Final verification"

    local p3_dev
    p3_dev=$(part_dev "$DEVICE" 3)

    if $DRY_RUN; then
        dry_run "blkid ${p3_dev}"
        dry_run "sfdisk -l ${DEVICE}"
        dry_run "Summary: p3 $(( OLD_P3_SIZE * 512 / 1048576 )) MB → $(( NEW_P3_SIZE * 512 / 1048576 )) MB"
        return
    fi

    # Did the resize preserve this card's UUID?  resize2fs must not change it,
    # and that — not equality with some other card's UUID — is the only thing
    # worth asserting here.  Nothing on the device consumes a UUID (U-Boot passes
    # root=/dev/mmcblk0p3, /etc/fstab names partitions by position), so a changed
    # one is a curiosity, not a breakage — and never "fix" one with `tune2fs -U`.
    echo ""
    info "Checking rootfs UUID..."
    local p3_uuid
    p3_uuid=$(blkid -s UUID -o value "$p3_dev" 2>/dev/null || echo "UNKNOWN")

    if [ -z "$ORIG_P3_UUID" ]; then
        info "p3 UUID is now ${p3_uuid} (no pre-resize value was recorded)"
    elif [ "$p3_uuid" = "$ORIG_P3_UUID" ]; then
        info "✓ UUID preserved across the resize: ${p3_uuid}"
    else
        warn "✗ The resize CHANGED the rootfs UUID:"
        warn "  Before: ${ORIG_P3_UUID}"
        warn "  After:  ${p3_uuid}"
        warn "  resize2fs is not supposed to do this; something more than a resize"
        warn "  happened to the filesystem."
    fi

    echo ""
    info "Final partition table:"
    sfdisk -l "$DEVICE"

    local new_mb=$(( NEW_P3_SIZE * 512 / 1048576 ))
    echo ""
    echo -e "${GREEN}${BOLD}  SD Card Upgrade Complete!${NC}"
    echo -e "  Device:           ${BOLD}${DEVICE}${NC}"
    echo -e "  Rootfs partition: ${BOLD}${p3_dev}${NC}  (UUID ${p3_uuid})"
    echo -e "  Size:             $(( OLD_P3_SIZE * 512 / 1048576 )) MB → ${BOLD}${new_mb} MB${NC} ($(( new_mb / 1024 )) GB)"
    echo ""
    echo -e "  ${GREEN}Next steps:${NC}"
    echo -e "    1. Insert card into the unit"
    echo -e "    2. Boot it, then update: ${BOLD}./commissioning/provision.sh <device-ip>${NC}"
    echo -e "    3. Run: ${BOLD}./deploy-all.sh <device-ip>${NC}"
    echo ""
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    echo ""
    echo -e "${BLUE}${BOLD}╔══════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}${BOLD}║  $PRODUCT_NAME SD Card Clone & Expand Tool      ║${NC}"
    echo -e "${BLUE}${BOLD}║  2 GB image → larger card (any ≥${MIN_TARGET_SIZE_GB} GB)                 ║${NC}"
    echo -e "${BLUE}${BOLD}╚══════════════════════════════════════════════════════╝${NC}"
    echo ""

    parse_args "$@"

    if $DRY_RUN; then
        warn "DRY-RUN mode — no changes will be made"
        echo ""
    fi

    # Pre-flight checks
    step "Pre-flight checks"
    check_root
    check_dependencies

    if [ "$MODE" = "clone" ]; then
        check_source "$SOURCE"
    fi

    check_device_safe "$DEVICE"

    # Initialize size tracking
    OLD_P3_SIZE=0
    NEW_P3_SIZE=0

    # Execute phases
    if [ "$MODE" = "clone" ]; then
        do_clone
    else
        info "Expand-only mode — skipping clone phase"
    fi

    do_verify
    do_repartition
    do_resize_fs
    do_final_verify
}

main "$@"
