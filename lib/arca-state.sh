#!/bin/bash
#
# lib/arca-state.sh — which of a unit's files are PER-UNIT STATE, and the one gate a card
# image has against key material.
#
# SOURCED, not executed:   . "$SCRIPT_DIR/lib/arca-state.sh"
#
# ONE home for the BACKUP list: everything that makes a unit THIS unit — settings,
# calibration, pairings, high scores, SSH identity, and the operator's own private config
# (the VNC password, ScummVM's ini and saves). Read by commissioning/backup.sh;
# commissioning/restore.sh writes it back.
#
# A card image carries NONE of it: rootfs/make-card-image.sh builds one generic image from our
# own parts, and a unit's state travels only through backup.sh and restore.sh. What the
# image builder takes from this file is arca_state_ssh_keys_deny, below.
#
# ── Where ScummVM's saves go ────────────────────────────────────────────────
#
# Our backend's default savepath is home/root/data/scummvm-saves (p2; backend-files/
# arca.cpp). Binaries built before it had none, and a save then landed in the
# process's CURRENT DIRECTORY: "/" when booted, /home/root from an SSH shell — which
# is why kq2.000 turned up loose in /home/root. Saves have no fixed name (<target>.NNN
# for AGI, <target>.sNN for SCUMM), so the backup also takes every regular file
# directly in those two directories (ARCA_STATE_LOOSE_DIRS), for units with old saves.
#
# arca_state_backup_foreign is the restore side's gate: every entry outside the BACKUP
# set, so restore.sh cannot be made to write /etc/shadow from a hand-built archive.
#
# Paths are relative to /, never absolute, and contain no whitespace.

# The BACKUP set. Exact paths, file or directory (a directory carries everything under
# it); globs are expanded by the device's shell.
ARCA_STATE_BACKUP=(
    etc/hostname
    etc/hosts
    etc/timezone
    etc/localtime
    etc/touch_calibration.conf
    'etc/touch_calibration.conf.bak*'
    etc/input_config.conf
    opt/games/arca_config.conf
    opt/arca/default-app
    opt/vnc_client/vnc_client.conf
    opt/games/scummvm.ini
    var/lib/alsa/asound.state
    var/lib/bluetooth
    var/lib/bluealsa
    home/root/.ssh
    'home/root/data/*.hig'
    home/root/data/ssh
    home/root/data/scummvm-saves
)

# Directories whose loose REGULAR files are backed up (ScummVM saves; see above).
# "." is / itself. uImage-* in /home/root is a staged kernel, not state.
ARCA_STATE_LOOSE_DIRS=(home/root .)

# arca_state_lister <backup>
#   Prints a POSIX sh script (it runs under BusyBox ash on the unit) that lists,
#   one per line and relative to /, the members of the set that EXIST there. A
#   symlink such as etc/localtime is listed as itself (-L), so tar archives the
#   link. Exit 2 on an unknown set.
arca_state_lister() {
    local set="$1" paths d
    case "$set" in
        backup) paths="${ARCA_STATE_BACKUP[*]}" ;;
        *) echo "arca_state_lister: unknown set '$set' (backup)" >&2; return 2 ;;
    esac
    echo 'cd / || exit 1'
    # Unquoted on purpose: the globs in the list expand on the unit. A glob that
    # matches nothing stays literal and fails the -e test.
    printf 'for f in %s; do\n' "$paths"
    cat <<'REMOTE'
    if [ -e "$f" ] || [ -L "$f" ]; then echo "$f"; fi
done
REMOTE
    for d in "${ARCA_STATE_LOOSE_DIRS[@]}"; do
        if [ "$d" = . ]; then printf 'for f in *; do\n'; else printf 'for f in %s/*; do\n' "$d"; fi
        cat <<'REMOTE'
    case "$f" in home/root/uImage-*) continue ;; esac
    if [ -f "$f" ] && [ ! -L "$f" ]; then echo "$f"; fi
done
REMOTE
    done
}

# arca_state_unsafe_paths
#   stdin: a tar listing (tar -tf). Prints every entry that is absolute or has a
#   '..' component. Exit 1 if it printed any, 0 if none.
arca_state_unsafe_paths() {
    local e found=1
    while IFS= read -r e; do
        case "/$e/" in
            //*|*/../*) printf '%s\n' "$e"; found=0 ;;
        esac
    done
    [ "$found" -ne 0 ]
}

# _arca_state_norm ENTRY — strip a leading "./" and a trailing "/".
_arca_state_norm() {
    local e="$1"
    while [ "${e#./}" != "$e" ]; do e="${e#./}"; done
    e="${e%/}"
    printf '%s' "$e"
}

# arca_state_ssh_keys_deny
#   stdin: a tar listing (tar -tf), or `find . ! -type d` output. Prints every entry
#   that is SSH key material: anything inside a '.ssh' directory (including a non-directory
#   .ssh symlink), any file named authorized_keys*, SSH host private keys (ssh_host_*_key),
#   and user SSH private keys (id_rsa, id_ecdsa, id_ed25519, id_dsa) anywhere. Directory
#   entries (trailing '/') are not material, so the rootfs's empty home/root/.ssh passes.
#   Exit 1 if it printed any, 0 if none.
#   Used by make-card-image.sh, which must put no key of any operator or unit on the
#   image it builds.
#   The caller must check tar's own exit status: an empty listing is clean.
arca_state_ssh_keys_deny() {
    local e found=1
    while IFS= read -r e; do
        [ -n "$e" ] || continue
        case "$e" in */) continue ;; esac
        case "/$e" in
            */.ssh/*) printf '%s\n' "$e"; found=0 ;;
            */authorized_keys*) printf '%s\n' "$e"; found=0 ;;
            */ssh_host_*_key) printf '%s\n' "$e"; found=0 ;;
            */id_rsa) printf '%s\n' "$e"; found=0 ;;
            */id_ecdsa) printf '%s\n' "$e"; found=0 ;;
            */id_ed25519) printf '%s\n' "$e"; found=0 ;;
            */id_dsa) printf '%s\n' "$e"; found=0 ;;
            */.ssh) printf '%s\n' "$e"; found=0 ;;
        esac
    done
    [ "$found" -ne 0 ]
}

# _arca_state_backup_member ENTRY — 0 if a tar entry belongs to the BACKUP set.
# Patterns are globs by design, hence the unquoted case patterns.
# shellcheck disable=SC2254
_arca_state_backup_member() {
    local raw="$1" e p d
    case "/$raw/" in //*|*/../*) return 1 ;; esac
    e="$(_arca_state_norm "$raw")"
    case "$e" in ''|.) return 0 ;; esac
    for p in "${ARCA_STATE_BACKUP[@]}"; do
        case "$e" in $p|$p/*) return 0 ;; esac
        case "$raw" in */) case "$p" in "$e"/*) return 0 ;; esac ;; esac
    done
    # A loose file: a non-directory entry directly inside one of the loose dirs.
    case "$raw" in */) return 1 ;; esac
    for d in "${ARCA_STATE_LOOSE_DIRS[@]}"; do
        if [ "$d" = . ]; then
            case "$e" in */*) ;; *) return 0 ;; esac
        else
            case "$e" in "$d"/*/*) ;; "$d"/*) return 0 ;; esac
        fi
    done
    return 1
}

# arca_state_backup_foreign
#   stdin: a tar listing. Prints every entry outside the BACKUP set, unsafe ones
#   included — what commissioning/restore.sh refuses to write. Exit 1 if it printed
#   any, 0 if none.
arca_state_backup_foreign() {
    local e found=1
    while IFS= read -r e; do
        [ -n "$e" ] || continue
        if ! _arca_state_backup_member "$e"; then printf '%s\n' "$e"; found=0; fi
    done
    [ "$found" -ne 0 ]
}
