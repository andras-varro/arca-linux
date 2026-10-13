#!/bin/bash
#
# commissioning/backup.sh — copy a unit's per-unit state to this host.
#
# Usage: commissioning/backup.sh <ip|host> [<out.tar.gz>]
#
# Takes the BACKUP set of lib/arca-state.sh — settings, touch calibration, input
# mapping, Bluetooth pairings and audio state, high scores, the SSH host keys and
# authorized_keys, the host name and time zone, and the operator's private config
# (the VNC password, ScummVM's ini and saves) — for an update, upgrade or reflash.
# commissioning/restore.sh puts it back.
#
# READ-ONLY on the unit: the list is built there (only files that exist), BusyBox
# tar streams it over ssh stdout, and this host compresses it (the unit has no
# gzip). Default output: backups/<hostname>-<YYYYmmdd-HHMMSS>.tar.gz in the repo,
# which .gitignore keeps out of git.
#
# ⚠️ The archive holds SECRETS — the VNC password in plaintext and the SSH host
# private keys. The script chmods it 600, which DrvFs (/mnt/c, c:\) silently
# ignores: on this host the file is as readable as the directory it lands in.
# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
    cat <<'USAGE'
Usage: commissioning/backup.sh <ip|host> [<out.tar.gz>]

Copies the unit's per-unit state (lib/arca-state.sh, BACKUP set) to this host.
Read-only on the unit. Default output: backups/<hostname>-<YYYYmmdd-HHMMSS>.tar.gz.
The archive holds secrets (VNC password, SSH host keys); keep it private.
Restore with: commissioning/restore.sh <ip|host> <archive> [--dry-run]

  -h, --help   this text
USAGE
}

case "${1:-}" in -h|--help) usage; exit 0 ;; esac
if [ $# -lt 1 ] || [ $# -gt 2 ]; then usage >&2; exit 2; fi
case "$1" in -*) usage >&2; exit 2 ;; esac
TARGET="$1"
OUT="${2:-}"

die() { echo "FAIL: $*" >&2; exit 1; }

# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=../lib/arca-state.sh
. "$REPO_ROOT/lib/arca-state.sh"

command -v gzip > /dev/null || die "gzip not installed on this host"
if [ -n "$OUT" ]; then
    case "$OUT" in *.tar.gz|*.tgz) ;; *) die "output must end in .tar.gz or .tgz: $OUT" ;; esac
    [ ! -e "$OUT" ] || die "$OUT exists; refusing to overwrite a backup"
fi

DEVICE="root@$TARGET"
arca_ssh_gate "$DEVICE" || die "cannot continue without SSH to $DEVICE"
rssh() { ssh -o ConnectTimeout=10 "$DEVICE" "$@" < /dev/null; }

UNIT=$(rssh 'head -n 1 /etc/hostname 2>/dev/null' | tr -cd 'A-Za-z0-9-')
[ -n "$UNIT" ] || UNIT="unit-$(printf '%s' "$TARGET" | tr -c 'A-Za-z0-9.-' '_')"
if [ -z "$OUT" ]; then
    mkdir -p "$REPO_ROOT/backups" || die "mkdir backups"
    OUT="$REPO_ROOT/backups/$UNIT-$(date +%Y%m%d-%H%M%S).tar.gz"
    [ ! -e "$OUT" ] || die "$OUT exists"
fi

LIST=$(rssh "$(arca_state_lister backup)") || die "listing the backup set on $DEVICE"
[ -n "$LIST" ] || die "the unit has none of the backup set's files; nothing to back up"
if printf '%s\n' "$LIST" | grep -q '[[:space:]]'; then
    die "a state file name contains whitespace:
$(printf '%s\n' "$LIST" | grep '[[:space:]]')"
fi
if UNSAFE=$(printf '%s\n' "$LIST" | arca_state_unsafe_paths); then :; else
    die "the unit listed unsafe paths:
$UNSAFE"
fi

PART="$OUT.part"
trap 'rm -f "$PART"' EXIT INT TERM
# The list is expanded here on purpose: these names come from the unit's own lister,
# and were checked for whitespace and unsafe paths above.
if ! rssh "cd / && tar -cf - $(printf '%s\n' "$LIST" | tr '\n' ' ')" | gzip -9 > "$PART"; then
    die "tar on $DEVICE or gzip here failed; nothing written"
fi
chmod 600 "$PART" 2>/dev/null || true
CONTENTS=$(tar -tzf "$PART") || die "the archive does not list; nothing written"
NFILES=$(printf '%s\n' "$CONTENTS" | grep -vc '/$')
[ "$NFILES" -gt 0 ] || die "the archive holds no files; nothing written"
mv -- "$PART" "$OUT" || die "mv $PART $OUT"
trap - EXIT INT TERM

echo "== backup of $DEVICE ($UNIT)"
printf '%s\n' "$LIST" | sed 's|^|  /|'
echo "archive:   $OUT"
echo "size:      $(wc -c < "$OUT") bytes, $NFILES files ($(printf '%s\n' "$LIST" | wc -l) listed paths)"
echo "sha256:    $(sha256sum "$OUT" | awk '{print $1}')"
echo "⚠️  holds secrets (VNC password, SSH host keys); chmod 600 is a no-op on DrvFs."
