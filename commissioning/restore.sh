#!/bin/bash
#
# commissioning/restore.sh — write a commissioning/backup.sh archive back to a unit.
#
# Usage: commissioning/restore.sh <ip|host> <backup.tar.gz> [--dry-run]
#
# Refused before anything touches the unit: an archive that does not list, holds
# an absolute or '..' path, or holds anything outside lib/arca-state.sh's BACKUP set
# (arca_state_backup_foreign) — so a hand-built archive cannot write /etc/shadow.
#
# Then, on the unit: stop the app through /etc/init.d/arca-app stop (the one
# implementation of "stop what is running"; never a killall), extract over / with
# BusyBox tar -o (--no-same-owner: every file lands root-owned, which is what the
# unit runs as; one captured default-app was 1000/1000), put the modes sshd insists
# on back (/home/root/.ssh 700, authorized_keys 600, host private keys 600), and
# start the app again.
#
# What it does not do: restart sshd, bluetoothd or the network. A restored host
# key, pairing, host name or time zone takes effect at the next reboot, and the
# receipt says so. --dry-run is read-only on the unit: it says which paths would be
# overwritten and which created.
# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
    cat <<'USAGE'
Usage: commissioning/restore.sh <ip|host> <backup.tar.gz> [--dry-run]

Writes a commissioning/backup.sh archive back to the unit: stops the app, extracts
over / (owners not restored), fixes SSH modes, starts the app. Only paths in the
BACKUP set of lib/arca-state.sh are accepted.

  --dry-run    list what would be written (overwrite / new); change nothing
  -h, --help   this text
USAGE
}

DRY=0
ARGS=()
for a in "$@"; do
    case "$a" in
        -h|--help) usage; exit 0 ;;
        --dry-run) DRY=1 ;;
        -*) usage >&2; exit 2 ;;
        *) ARGS+=("$a") ;;
    esac
done
[ "${#ARGS[@]}" -eq 2 ] || { usage >&2; exit 2; }
TARGET="${ARGS[0]}"
ARCHIVE="${ARGS[1]}"

die() { echo "FAIL: $*" >&2; exit 1; }

# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=../lib/arca-state.sh
. "$REPO_ROOT/lib/arca-state.sh"

[ -f "$ARCHIVE" ] || die "no archive $ARCHIVE"
CONTENTS=$(tar -tzf "$ARCHIVE") || die "$ARCHIVE does not list as a .tar.gz"
[ -n "$CONTENTS" ] || die "$ARCHIVE is empty"
if UNSAFE=$(printf '%s\n' "$CONTENTS" | arca_state_unsafe_paths); then :; else
    die "$ARCHIVE holds absolute or '..' paths; refusing:
$UNSAFE"
fi
if FOREIGN=$(printf '%s\n' "$CONTENTS" | arca_state_backup_foreign); then :; else
    die "$ARCHIVE holds paths outside the backup set (lib/arca-state.sh); refusing:
$FOREIGN"
fi
if printf '%s\n' "$CONTENTS" | grep -q '[[:space:]]'; then
    die "$ARCHIVE holds a name with whitespace; refusing"
fi
FILES=$(printf '%s\n' "$CONTENTS" | grep -v '/$')
NFILES=$(printf '%s\n' "$FILES" | grep -c .)

DEVICE="root@$TARGET"
arca_ssh_gate "$DEVICE" || die "cannot continue without SSH to $DEVICE"
rssh() { ssh -o ConnectTimeout=10 "$DEVICE" "$@"; }

if [ "$DRY" -eq 1 ]; then
    echo "== dry run: $ARCHIVE -> $DEVICE (nothing is changed)"
    # The names go over stdin, one per line; the unit only tests for existence.
    CHECK=$(cat <<'REMOTE'
while IFS= read -r f; do
    if [ -e "/$f" ] || [ -L "/$f" ]; then echo "  overwrite /$f"; else echo "  new       /$f"; fi
done
REMOTE
)
    printf '%s\n' "$FILES" | rssh "$CHECK" || die "existence check on $DEVICE"
    echo "would: /etc/init.d/arca-app stop; extract $NFILES files over / (tar -o);"
    echo "       fix /home/root/.ssh (700/600) and /home/root/data/ssh host keys (600); arca-app start"
    exit 0
fi

echo "== stopping the app"
rssh '/etc/init.d/arca-app stop' < /dev/null || die "arca-app stop failed; nothing restored"

echo "== extracting $NFILES files over /"
if ! gzip -dc "$ARCHIVE" | rssh 'cd / && tar -xof -'; then
    echo "FAIL: extraction on $DEVICE failed; the unit may be partly restored." >&2
    echo "Re-run this script, or reflash. Starting the app again." >&2
    rssh '/etc/init.d/arca-app start' < /dev/null
    exit 1
fi

echo "== modes"
FIX=$(cat <<'REMOTE'
[ ! -d /home/root/.ssh ] || chmod 700 /home/root/.ssh
[ ! -f /home/root/.ssh/authorized_keys ] || chmod 600 /home/root/.ssh/authorized_keys
if [ -d /home/root/data/ssh ]; then
    chmod 700 /home/root/data/ssh
    for k in /home/root/data/ssh/ssh_host_*_key; do [ ! -f "$k" ] || chmod 600 "$k"; done
    for k in /home/root/data/ssh/ssh_host_*_key.pub; do [ ! -f "$k" ] || chmod 644 "$k"; done
fi
sync
REMOTE
)
rssh "$FIX" < /dev/null || echo "WARNING: fixing SSH modes on $DEVICE failed; sshd may refuse the keys" >&2

echo "== starting the app"
rssh '/etc/init.d/arca-app start' < /dev/null || echo "WARNING: arca-app start failed" >&2

echo "restored:  $NFILES files from $ARCHIVE to $DEVICE"
echo "reboot the unit for the host name, time zone, SSH host keys and Bluetooth pairings to take effect."
