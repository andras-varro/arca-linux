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
#   arca_is_card_disk   LAYOUT. The partition table, which the vendor's own
#                     partitioning produced identically on every unit and which
#                     U-Boot depends on — `root=/dev/mmcblk0p6` is compiled into
#                     u-boot.bin with no saveenv, so p6 IS the rootfs by
#                     position and cannot be renumbered. See
#                     SYSTEM_ANALYSIS.md#42-partitions.
#   arca_card_partitions
#                     POSITION. Which partition holds which of the three trees
#                     that are mounted. Also by position, for the same reason:
#                     /etc/fstab names /dev/mmcblk0p{2,3,7} literally. Nothing on
#                     the device consumes a UUID at all.
#
# Both are readable without mounting anything new and without writing anything.

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

# ── Layout fingerprint ──────────────────────────────────────────────────────
#
# start,size in 512-byte sectors, from `sfdisk -d`. p1 p2 p3 p5 p6 are
# byte-identical on every unit measured. p4 (the extended container) and p7
# (swap) are NOT pinned: they absorb the difference in physical card size, and
# two cards of the same nominal 4 GB differ there (6586650 vs 6891885 sectors of
# p4). Pinning them would reject a genuine Arca for being a slightly
# different card. p5 stays in the table, unused, so the table still matches.
ARCA_LAYOUT="1:63,144522 2:144585,514080 3:658665,498015 5:1156743,2939832 6:4096638,2008062"

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
# arca_find_rootfs
#
# Echo the mount point of every mounted Arca rootfs, one per line.
# Callers decide what 0, 1 or many means.
#
# "/" is EXCLUDED unconditionally. Commissioning is an offline, card-in-reader
# operation, so the live root is never the intended target — and a content scan,
# unlike the UUID lookup it replaces, could otherwise select the dev host's own
# root and rewrite its /etc/shadow. Excluding it makes that impossible rather
# than unlikely. To act on a live root deliberately, set ROOTFS=/ by hand.
# ---------------------------------------------------------------------------
arca_find_rootfs() {
    local target fstype
    # Fed by a here-doc rather than a pipe so the loop body runs in THIS shell:
    # a `findmnt | while read` would put it in a subshell, which matters as soon
    # as anyone adds a variable assignment to the body.
    while read -r target fstype; do
        [ -n "$target" ] || continue
        [ "$target" = "/" ] && continue
        case "$fstype" in
            ext2|ext3|ext4) ;;
            *) continue ;;
        esac
        arca_is_rootfs "$target" && echo "$target"
    done <<EOF
$(findmnt -rno TARGET,FSTYPE 2>/dev/null)
EOF
    # Explicit, because the loop's status is that of the last arca_is_rootfs call:
    # a final non-match would otherwise return 1 and kill a caller running under
    # `set -e`. "No cards found" is a normal result, not an error.
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
# arca_can_read_partition_table DEVICE
#
# 0 if DEVICE's partition table can be read at all.
#
# Exists to separate two failures that arca_is_card_disk cannot distinguish,
# because both are `return 1`: "this is not an Arca card" and "this process
# cannot read the table". `sfdisk -d` needs root — block devices are
# brw-rw---- root:disk — so a non-root run on a genuine card reported
# "does not carry the Arca partition layout", which is a lie that sends the
# operator looking at the card instead of at the command. Measured 2026-08-05 on
# a real unit's card at /dev/mmcblk0.
#
# Also 1 when sfdisk is absent, which a caller should report as its own thing.
# ---------------------------------------------------------------------------
arca_can_read_partition_table() {
    local dev="$1"
    [ -n "$dev" ] || return 1
    command -v sfdisk >/dev/null 2>&1 || return 1
    sfdisk -d "$dev" >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# arca_is_card_disk DEVICE
#
# 0 if DEVICE (a whole disk or an image file) carries the Arca partition
# layout. Reads the partition table only — nothing is mounted and nothing is
# written, so this is safe to run against any disk on the host.
# ---------------------------------------------------------------------------
arca_is_card_disk() {
    local dev="$1" table entry num want got
    [ -n "$dev" ] || return 1
    table=$(sfdisk -d "$dev" 2>/dev/null) || return 1
    [ -n "$table" ] || return 1

    for entry in $ARCA_LAYOUT; do
        num="${entry%%:*}"
        want="${entry#*:}"
        # sfdisk -d prints, with the padding shown:
        #   /dev/sdf6 : start=  4096638, size=  2008062, type=83
        # so `start=` and its value are separate awk fields. Matched with a
        # regex over the whole line instead of by field index, because the
        # padding is not guaranteed and `type=` may or may not be followed by
        # `bootable`.
        #
        # The partition number is anchored on a non-digit: a bare /p?6$/ also
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
        [ "$got" = "$want" ] || return 1
    done
    return 0
}

# ---------------------------------------------------------------------------
# arca_find_card_disks
#
# Echo every whole disk on the host that carries the Arca layout, one
# "DEVICE SIZE" pair per line. Used only to make a "nothing is mounted" error
# actionable, so it is best-effort and prints nothing if lsblk is unavailable.
#
# The root disk is skipped, and by resolution rather than by name: arca_host_root_disk
# below is the one implementation of that resolution, shared with the mount path,
# because on this host every disk reports removable=0 including the root disk and
# a wsl --mount'ed card lands on the same /dev/sd? namespace as / — so a
# "removable only" filter rejects everything and a name filter rejects nothing.
# (CLAUDE.md, "Working from this host".)
# ---------------------------------------------------------------------------
arca_find_card_disks() {
    local rootdisk="" name size
    command -v lsblk >/dev/null 2>&1 || return 0
    rootdisk=$(arca_host_root_disk 2>/dev/null || true)

    while read -r name size; do
        [ -n "$name" ] || continue
        [ "$name" = "$rootdisk" ] && continue
        arca_is_card_disk "/dev/$name" && echo "/dev/$name $size"
    done <<EOF
$(lsblk -drno NAME,SIZE --nodeps 2>/dev/null)
EOF
    return 0   # see arca_find_rootfs: finding nothing is not an error
}

# ═══════════════════════════════════════════════════════════════════════════
# The three mounts, by POSITION
# ═══════════════════════════════════════════════════════════════════════════
#
# A booted Arca assembles three partitions into one tree (p7 is swap). Offline —
# a card in a reader — they are three separate mounts, and an offline tool that mounts
# only p6 sees /home/root/{data,log} as EMPTY directories, because they are mount
# points: the per-unit state is on p2 and the logs on p3. p5 stays in the partition
# table, unused: nothing mounts it, so it has no role here.
#
# ⚠️ ROLE IS BY POSITION, NEVER BY UUID OR BY CONTENT.
#
#   * Not UUID, for the reason at the top of this file: all of them differ per unit.
#   * Not content either, for p2/p3. p6 has a marker to recognise; the other two
#     do not have anything reliable, and a p3 that has been rotated is
#     indistinguishable from an empty p2. Guessing from content would mis-mount a
#     unit's partitions and the install would then run against the wrong tree.
#   * Position is what the device itself uses: /etc/fstab names
#     /dev/mmcblk0p{2,3,7} literally and U-Boot passes root=/dev/mmcblk0p6
#     compiled in, with no saveenv to change it. So position is not merely
#     convenient here, it is the device's own definition.
#
# p1 is deliberately NOT in this table. It carries mlo, u-boot.bin,
# ctrlblock.bin and uImage-system; leaving it untouched is what keeps a power
# cycle a free undo (SYSTEM_ANALYSIS.md#47-recovery). A caller cannot reach p1
# through these functions, which is a stronger guarantee than remembering not to.
# p4 (extended container), p5 (unused) and p7 (swap) are absent for the same
# reason: nothing to mount.
ARCA_PART_ROLES="6:root 2:data 3:log"

# Where each role is mounted on a RUNNING device. Used to translate a
# device-absolute path (which is how the install rules are written) into a path
# under the right offline mount.
ARCA_ROLE_DEVICE_PATH="root:/ data:/home/root/data log:/home/root/log"

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
# arca_card_partitions DISK
#
# Echo "<role> <partition-device>" for each of the three, one per line, root
# first. Pure name arithmetic over ARCA_PART_ROLES — it does not check that the
# nodes exist, because a caller may be working on an image file where they do
# not, and arca_is_card_disk has already established the layout.
# ---------------------------------------------------------------------------
arca_card_partitions() {
    local disk="$1" entry num role
    [ -n "$disk" ] || return 1
    for entry in $ARCA_PART_ROLES; do
        num="${entry%%:*}"
        role="${entry#*:}"
        echo "$role $(arca_part_dev "$disk" "$num")"
    done
}

# ---------------------------------------------------------------------------
# arca_role_device_path ROLE
#
# Echo the absolute path ROLE's tree has on a running device, or nothing for an
# unknown role.
# ---------------------------------------------------------------------------
arca_role_device_path() {
    local want="$1" entry
    for entry in $ARCA_ROLE_DEVICE_PATH; do
        [ "${entry%%:*}" = "$want" ] && { echo "${entry#*:}"; return 0; }
    done
    return 1
}

# ---------------------------------------------------------------------------
# arca_offline_path BASE DEVPATH
#
# Map a device-absolute path onto the right one of the three offline mounts.
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
# arca_host_root_disk
#
# Echo the whole disk this host boots from.
#
# Resolved, never guessed by name or by the removable flag. On the machine this
# was written on EVERY disk reports removable = 0 — including the root disk — and
# a `wsl --mount`ed card lands in the same /dev/sd? namespace as /, so a
# "removable media only" gate rejects everything and a name filter rejects
# nothing (CLAUDE.md → "Working from this host").
#
# `mount | grep ^/dev/sdX` is NOT a substitute: it never lists swap, so a disk
# whose only mounted piece is the swap partition looks unused.
# ---------------------------------------------------------------------------
arca_host_root_disk() {
    local src
    command -v lsblk >/dev/null 2>&1 || return 1
    src=$(findmnt -no SOURCE --target / 2>/dev/null) || return 1
    [ -n "$src" ] || return 1
    lsblk -rnso NAME "$src" 2>/dev/null | tail -1
}

# ---------------------------------------------------------------------------
# arca_is_host_root_disk DEVICE
#
# 0 if DEVICE is (or is a partition of) the disk this host boots from. The gate
# every write path checks before it does anything: writing to the dev host's own
# disk is the one mistake in this file with no undo.
#
# Returns 1 — "not the root disk" — when lsblk cannot answer at all, which is the
# permissive direction. That is deliberate: this function is a veto, and callers
# also require arca_is_card_disk to have said yes. An unanswerable lsblk on a disk
# that carries the exact Arca partition table is not the dev host.
# ---------------------------------------------------------------------------
arca_is_host_root_disk() {
    local dev="$1" rootdisk name
    [ -n "$dev" ] || return 1
    rootdisk=$(arca_host_root_disk) || return 1
    [ -n "$rootdisk" ] || return 1

    # Compare by resolved disk, not by string: /dev/sda, /dev/sda1 and
    # /dev/disk/by-id/… all have to match when the root disk is sda.
    name=$(lsblk -rnso NAME "$dev" 2>/dev/null | tail -1)
    [ -n "$name" ] || name="$(basename "$dev")"
    [ "$name" = "$rootdisk" ]
}

# ---------------------------------------------------------------------------
# arca_mount_card DISK BASE
#
# Mount all three of DISK's trees read-write under BASE/{root,data,log}
# and echo "<role> <mountpoint>" per line. Needs root.
#
# Refuses the host's own disk before mounting anything, rather than after: a
# read-write mount of the dev host's root is already a bad outcome even if
# nothing is written to it.
#
# Partial failure unwinds. A caller that got two of three mounts and proceeded
# would install into two trees and silently miss the third — a bundle path under
# /home/root/data landing in p6's empty mount point instead of on p2.
# ---------------------------------------------------------------------------
arca_mount_card() {
    local disk="$1" base="$2" role part mp
    [ -n "$disk" ] && [ -n "$base" ] || return 1

    if arca_is_host_root_disk "$disk"; then
        echo "arca_mount_card: $disk is this host's root disk — refusing" >&2
        return 1
    fi
    if ! arca_is_card_disk "$disk"; then
        echo "arca_mount_card: $disk does not carry the Arca partition layout" >&2
        return 1
    fi

    while read -r role part; do
        [ -n "$role" ] || continue
        mp="$base/$role"
        mkdir -p "$mp" || { arca_umount_card "$base"; return 1; }
        if ! mount "$part" "$mp" 2>/dev/null; then
            echo "arca_mount_card: could not mount $part on $mp" >&2
            arca_umount_card "$base"
            return 1
        fi
        echo "$role $mp"
    done <<EOF
$(arca_card_partitions "$disk")
EOF
    return 0
}

# ---------------------------------------------------------------------------
# arca_umount_card BASE
#
# Unmount whatever arca_mount_card put under BASE. Idempotent, and never an error:
# it is called from the failure path of arca_mount_card and from a trap, where a
# non-zero status would mask the real problem.
# ---------------------------------------------------------------------------
arca_umount_card() {
    local base="$1" entry role
    [ -n "$base" ] || return 0
    for entry in $ARCA_PART_ROLES; do
        role="${entry#*:}"
        mountpoint -q "$base/$role" 2>/dev/null && umount "$base/$role" 2>/dev/null
    done
    # root last is not required — they are independent filesystems, not
    # nested mounts — but sync is, before the operator pulls the card.
    sync
    return 0
}

# ---------------------------------------------------------------------------
# arca_check_card_mounts BASE
#
# Sanity-check a mounted card: p6 must look like our rootfs, and the other two
# must NOT. Echoes each problem; returns 1 if there were any.
#
# The negative half is the half that earns its keep. Getting the partition order
# wrong — mounting p2 where p6 was expected — is the one mistake that makes every
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
