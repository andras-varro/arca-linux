#!/bin/bash
#
# lib/arca-identify.sh — recognise an Arca SD card: its rootfs tree, and its disk.
#
# SOURCED, not executed:   . "$SCRIPT_DIR/lib/arca-identify.sh"
#
# ── Why this is not a UUID ──────────────────────────────────────────────────
#
# A filesystem UUID is generated at mkfs time, so it identifies ONE CARD, not a
# model. Two cards built from the same image share none, and a hardcoded UUID
# recognises exactly the one card the constant was copied from. It also cannot
# be repaired by assigning the constant to the new card: two cards with one UUID
# is a worse bug than the one it hides.
#
# What is actually invariant, and what each function below uses:
#
#   arca_is_rootfs      CONTENT. The marker file our own root filesystem (rootfs/)
#                     carries. A vendor tree has none, and is not recognised.
#   arca_is_card_disk   LAYOUT. The partition table, which our own card image
#                     (rootfs/make-card-image.sh) writes from ARCA_LAYOUT below and
#                     which U-Boot depends on — `root=/dev/mmcblk0p3` is compiled
#                     into u-boot.img with no saveenv, so p3 IS the rootfs by
#                     position and cannot be renumbered. See
#                     SYSTEM_ANALYSIS.md#42-partitions.
#   ARCA_PART_ROLES   POSITION. Which partition holds which of the two trees
#                     that are mounted. Also by position, for the same reason:
#                     /etc/fstab names /dev/mmcblk0p2 literally. Nothing on
#                     the device consumes a UUID at all.
#   arca_uboot_root_ok
#                     The same position, read out of a u-boot.img: the root= it
#                     boots must be the partition this file calls root.
#
# All of them are readable without mounting anything new and without writing anything.

# ── Content markers ─────────────────────────────────────────────────────────
#
# ARCA_ROOTFS_OURS is the identity of OUR OWN root filesystem (rootfs/), and the only
# one: rootfs/board/arca/post-build.sh writes it. Nothing in this repo prepares
# or cleans a vendor root any more, so installing into one would leave the vendor
# stack running beside ours — a tree without the marker is not a rootfs here, whatever
# else it carries (/opt/pv02, the vendor application tree, the RW20 banner in /etc/issue).
#
# ARCA_ROOTFS_REQUIRED is not identity. It is the set of files every complete root of
# ours has (account, name resolution, sshd policy, network config), so a half-built or
# half-copied tree that carries the marker is refused rather than installed into.
ARCA_ROOTFS_REQUIRED="etc/shadow etc/hosts etc/ssh/sshd_config etc/network/interfaces"
ARCA_ROOTFS_OURS="etc/arca-rootfs"

# ── Layout: the ONE table ───────────────────────────────────────────────────
#
# n:start:size:type, in 512-byte sectors, MBR, three primary partitions:
#   p1  FAT boot (63 on purpose: the ROM reads it; bootable), mlo/u-boot/kernel
#   p2  log, 1 GiB, mounted on /home/root/log
#   p3  root, the LAST partition, mounted on /; /home/root/data is a plain
#       directory inside it (there is no data partition)
# This string is the only copy: arca_layout_sfdisk_script prints it as an sfdisk
# script for the card-image builder, arca_layout_end gives the image size, and
# arca_is_card_disk below compares a real table against it.
#
# ARCA_LAYOUT_GROW names the one partition whose START is pinned and whose SIZE is
# not: p3 is extended to the card's end (and its filesystem grown) on a bigger card,
# and that card is still an Arca. Type is not compared.
ARCA_LAYOUT="1:63:144522:c 2:147456:2097152:83 3:2244608:1048576:83"
ARCA_LAYOUT_GROW=3

# ---------------------------------------------------------------------------
# arca_layout_sfdisk_script
#
# Print ARCA_LAYOUT as an sfdisk script (device names x<n>: sfdisk on an image
# file ignores them). p1 is the bootable one. Nothing else in the repo writes
# this table out.
# ---------------------------------------------------------------------------
arca_layout_sfdisk_script() {
    local e n s z t
    echo "label: dos"
    echo "label-id: 0x00000000"
    echo "unit: sectors"
    echo
    for e in $ARCA_LAYOUT; do
        IFS=: read -r n s z t <<< "$e"
        printf 'x%s : start=%s, size=%s, type=%s%s\n' "$n" "$s" "$z" "$t" "$([ "$n" = 1 ] && echo ', bootable')"
    done
}

# ---------------------------------------------------------------------------
# arca_layout_end
#
# Print the first sector past the last partition (start + size of the last
# ARCA_LAYOUT entry): the size, in sectors, of an image file that holds the table.
# ---------------------------------------------------------------------------
arca_layout_end() {
    local e n s z t end=0
    for e in $ARCA_LAYOUT; do
        IFS=: read -r n s z t <<< "$e"
        end=$((s + z))
    done
    echo "$end"
}

# ---------------------------------------------------------------------------
# arca_is_rootfs DIR
#
# 0 if DIR is the root of our own Arca rootfs. Silent; sets nothing.
# DIR may be "" or "/" for the live root — both mean the same tree.
# ---------------------------------------------------------------------------
arca_is_rootfs() {
    local d="${1:-}" m
    case "$d" in
        /) d="" ;;
        */) d="${d%/}" ;;
    esac

    [ -e "$d/$ARCA_ROOTFS_OURS" ] || return 1

    for m in $ARCA_ROOTFS_REQUIRED; do
        [ -f "$d/$m" ] || return 1
    done
    return 0
}

# ---------------------------------------------------------------------------
# arca_is_rootfs_writable DIR
#
# 0 if DIR is mounted read-write. A card auto-mounted read-only is the one way
# detection can succeed and every subsequent edit fail, so callers check this
# and say so, rather than emitting a pile of "Read-only file system" errors.
# ---------------------------------------------------------------------------
arca_is_rootfs_writable() {
    local opts
    opts=$(findmnt -rno OPTIONS --mountpoint "$1" 2>/dev/null | head -1)
    case ",$opts," in
        *,ro,*) return 1 ;;
        *) return 0 ;;
    esac
}

# ---------------------------------------------------------------------------
# arca_is_card_disk DEVICE
#
# 0 if DEVICE (a whole disk or an image file) carries the Arca partition
# layout: start and size of every entry in ARCA_LAYOUT, start only for
# ARCA_LAYOUT_GROW (a grown card still matches). Type is not compared. Reads the
# partition table only — nothing is mounted and nothing is
# written, so this is safe to run against any disk on the host.
# ---------------------------------------------------------------------------
arca_is_card_disk() {
    local dev="$1" table entry num start size want got
    [ -n "$dev" ] || return 1
    table=$(sfdisk -d "$dev" 2>/dev/null) || return 1
    [ -n "$table" ] || return 1

    for entry in $ARCA_LAYOUT; do
        IFS=: read -r num start size _ <<< "$entry"
        want="$start,$size"
        [ "$num" = "$ARCA_LAYOUT_GROW" ] && want="$start"
        # sfdisk -d prints, with the padding shown:
        #   /dev/sdf3 : start=  2244608, size=  1048576, type=83
        # so `start=` and its value are separate awk fields. Matched with a
        # regex over the whole line instead of by field index, because the
        # padding is not guaranteed and `type=` may or may not be followed by
        # `bootable`.
        #
        # The partition number is anchored on a non-digit: a bare /p?3$/ also
        # matches /dev/sdf16, and a substring test on "${dev}${num}" matches
        # nothing at all on /dev/mmcblk0, whose partitions are p-prefixed.
        got=$(echo "$table" | awk -v n="$num" '
            $1 ~ ("(^|[^0-9])p?" n "$") && /start=/ {
                s = ""; z = ""
                if (match($0, /start=[ \t]*[0-9]+/)) {
                    s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s)
                }
                if (match($0, /size=[ \t]*[0-9]+/)) {
                    z = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", z)
                }
                print s "," z
                exit
            }')
        # The grown partition: only its start counts, the size is whatever the card gave.
        [ "$num" = "$ARCA_LAYOUT_GROW" ] && got="${got%%,*}"
        [ "$got" = "$want" ] || return 1
    done
    return 0
}

# ═══════════════════════════════════════════════════════════════════════════
# The two mounts, by POSITION
# ═══════════════════════════════════════════════════════════════════════════
#
# A booted Arca assembles two partitions into one tree: p3 is the root, p2 the
# log partition mounted on /home/root/log. /home/root/data is NOT a mount: it is
# a plain directory inside p3, so there is no data role. Offline — a card in a
# reader — they are two separate mounts, and an offline tool that mounts only
# p3 sees /home/root/log as an EMPTY directory, because it is a mount point.
#
# ⚠️ ROLE IS BY POSITION, NEVER BY UUID OR BY CONTENT.
#
#   * Not UUID, for the reason at the top of this file: all of them differ per unit.
#   * Not content either, for the log partition. p3 has a marker to recognise; the
#     log partition has nothing reliable, and a rotated log is indistinguishable
#     from an empty one. Guessing from content would mis-mount a unit's
#     partitions and the install would then run against the wrong tree.
#   * Position is what the device itself uses: /etc/fstab names
#     /dev/mmcblk0p2 literally and U-Boot passes root=/dev/mmcblk0p3
#     compiled in, with no saveenv to change it. So position is not merely
#     convenient here, it is the device's own definition.
#
# p1 is deliberately NOT in this table. It carries mlo, u-boot.img and the
# kernel; leaving it untouched is what keeps a power cycle a free undo
# (SYSTEM_ANALYSIS.md#47-recovery). A caller cannot reach p1 through this
# table, which is a stronger guarantee than remembering not to.
ARCA_PART_ROLES="3:root 2:log"

# Where each role is mounted on a RUNNING device. Used to translate a
# device-absolute path (which is how the install rules are written) into a path
# under the right offline mount. A path under /home/root/data is under root.
ARCA_ROLE_DEVICE_PATH="root:/ log:/home/root/log"

# ---------------------------------------------------------------------------
# arca_uboot_root_ok FILE
#
# 0 if the u-boot.img FILE boots the partition this layout puts the root on.
# U-Boot carries its kernel command line as a NUL-terminated string in its
# built-in environment ("sysroot=/dev/mmcblk0p3 rw"), compiled in with no
# saveenv. A card whose root is p3 under a U-Boot that boots another partition
# panics at once, with no log, so the card-image builder asks this first.
#
# The expected number is the root entry of ARCA_PART_ROLES, so the layout stays
# the one home. Three distinct refusals: file missing, no sysroot= string found
# (a witness that the grep itself works), and a different partition number.
# The FIRST sysroot= string in the file is the one compared; a stray later one
# is not looked at.
# ---------------------------------------------------------------------------
arca_uboot_root_ok() {
    local file="${1:-}" entry want="" got num
    for entry in $ARCA_PART_ROLES; do
        [ "${entry#*:}" = "root" ] && want="${entry%%:*}"
    done
    if [ -z "$want" ]; then
        echo "arca-identify: ARCA_PART_ROLES has no root entry" >&2
        return 1
    fi
    if [ ! -f "$file" ]; then
        echo "arca-identify: $file: not found" >&2
        return 1
    fi
    got=$(grep -a -o 'sysroot=/dev/mmcblk0p[0-9]* rw' "$file" | head -1)
    if [ -z "$got" ]; then
        echo "arca-identify: $file has no 'sysroot=/dev/mmcblk0pN rw' string - not a u-boot.img from uboot/build.sh" >&2
        return 1
    fi
    num="${got#sysroot=/dev/mmcblk0p}"
    num="${num%% *}"
    if [ "$num" != "$want" ]; then
        echo "arca-identify: $file boots root=/dev/mmcblk0p$num; this card layout has its root on p$want - rebuild U-Boot with uboot/build.sh" >&2
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# arca_part_dev DISK N
#
# Echo the device node of DISK's partition N.
#
# Two naming schemes, and the difference is not cosmetic: /dev/sdf + 6 is
# /dev/sdf6, but /dev/mmcblk0 + 6 is /dev/mmcblk0p6 and /dev/mmcblk06 does not
# exist. The rule the kernel uses is "insert a 'p' when the disk name already
# ends in a digit", which covers mmcblk, nvme and loop as well as sd/hd.
# ---------------------------------------------------------------------------
arca_part_dev() {
    local disk="$1" n="$2"
    [ -n "$disk" ] && [ -n "$n" ] || return 1
    case "$disk" in
        *[0-9]) echo "${disk}p${n}" ;;
        *)      echo "${disk}${n}" ;;
    esac
}

# ---------------------------------------------------------------------------
# arca_offline_path BASE DEVPATH
#
# Map a device-absolute path onto the right one of the two offline mounts.
#
# LONGEST prefix wins, and it must match on a component boundary.  Both matter:
# role "root" has device path "/", which prefixes everything, and /home/rootless
# must not be mistaken for something under /home/root.
# ---------------------------------------------------------------------------
arca_offline_path() {
    local base="$1" dev="$2" entry role dp best_role="root" best_dp="/" rest
    [ -n "$base" ] || return 1
    case "$dev" in /*) ;; *) return 1 ;; esac
    base="${base%/}"

    for entry in $ARCA_ROLE_DEVICE_PATH; do
        role="${entry%%:*}"
        dp="${entry#*:}"
        [ "$dp" = "/" ] && continue
        case "$dev" in
            "$dp"|"$dp"/*)
                if [ "${#dp}" -gt "${#best_dp}" ]; then best_role="$role"; best_dp="$dp"; fi
                ;;
        esac
    done

    if [ "$best_role" = "root" ]; then
        echo "$base/root$dev"
    else
        rest="${dev#$best_dp}"
        echo "$base/$best_role$rest"
    fi
}

# ---------------------------------------------------------------------------
# arca_offline_base_ok BASE
#
# The guard.  Refuses every base that would make device-absolute paths resolve
# to the dev host's own filesystem: unprefixed, /etc, /opt and /usr/lib are THIS
# host's.  Every offline writer calls it before its first write.
# ---------------------------------------------------------------------------
arca_offline_base_ok() {
    local base="$1" norm

    if [ -z "$base" ]; then
        echo "arca-identify: refusing an EMPTY base — the rules would resolve to this host's /etc, /opt and /usr/lib" >&2
        return 1
    fi

    # Collapse repeated slashes and a trailing "/." so that "//" and "/." are
    # recognised as "/" rather than sneaking past a string comparison.
    norm=$(printf '%s' "$base" | sed -e 's:/\{2,\}:/:g' -e 's:/\.$:/:' -e 's:\(.\)/$:\1:')
    if [ "$norm" = "/" ] || [ -z "$norm" ]; then
        echo "arca-identify: refusing base '$base' — '/' is this host's root, not a mounted card" >&2
        return 1
    fi

    case "$base" in
        *..*) echo "arca-identify: refusing base '$base' — it contains '..'" >&2; return 1 ;;
    esac

    if [ ! -d "$base" ]; then
        echo "arca-identify: refusing base '$base' — not a directory" >&2
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# arca_check_card_mounts BASE
#
# Sanity-check a mounted card: p3 (root) must look like our rootfs, and the other one
# (the log partition, p2) must NOT. Echoes each problem; returns 1 if there were any.
#
# The negative half is the half that earns its keep. Getting the partition order
# wrong — mounting p2 where p3 was expected — is the one mistake that makes every
# later path resolve under the wrong tree, and it would otherwise be invisible
# until the install landed in the wrong place.
# ---------------------------------------------------------------------------
arca_check_card_mounts() {
    local base="$1" entry role bad=0

    if ! arca_is_rootfs "$base/root"; then
        echo "  $base/root does not look like our Arca rootfs (no /$ARCA_ROOTFS_OURS, or a required file missing)"
        bad=$((bad + 1))
    fi

    for entry in $ARCA_PART_ROLES; do
        role="${entry#*:}"
        [ "$role" = "root" ] && continue
        if [ ! -d "$base/$role" ]; then
            echo "  $base/$role is not mounted"
            bad=$((bad + 1))
        elif arca_is_rootfs "$base/$role"; then
            echo "  $base/$role looks like a ROOTFS — the partitions are in the wrong order"
            bad=$((bad + 1))
        fi
    done

    [ "$bad" -eq 0 ]
}
