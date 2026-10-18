#!/bin/bash
#
# lib/arca-state.sh — the one gate a card image has against key material.
#
# SOURCED, not executed:   . "$SCRIPT_DIR/lib/arca-state.sh"
#
# A card image carries NONE of a unit's per-unit state: rootfs/make-card-image.sh builds one
# generic image from our own parts. What the image builder takes from this file is
# arca_state_ssh_keys_deny, below. WHAT a unit's state is lives elsewhere: the item files
# <component>/backup.d/*.item, read on the unit by device-files/arca-backup.
#
# Paths are relative to /, never absolute, and contain no whitespace.

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
