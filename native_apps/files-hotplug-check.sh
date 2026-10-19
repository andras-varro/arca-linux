#!/bin/bash
#
# files-hotplug-check.sh — unattended Files plug/pull check on a device.
#
#   ./native_apps/files-hotplug-check.sh <ip> [--out DIR]
#
# DEVICE ONLY. Starts the remote-tap daemon (arca_tap), opens Files from the
# launcher, and grades three frames of /dev/fb0:
#   plugged        Files with a stick in (right pane = the stick)
#   pulled         after a simulated pull (sd delete): the frame must DIFFER
#   plugged-again  after a simulated plug (scsi host scan): the right pane must differ from
#                  "pulled" and its 5th folder-list row ("USB usb0") must appear
# plus device state: the sd node gone after the pull, /media/usb0 mounted again
# after the plug. file_manager logs nothing about rescans, so the verdict is
# framebuffer evidence (md5 of the right-pane crop, stable over two captures; the
# top bar clock and CPU % are outside it), not a log line. Files falls back to its
# folder list after a pull, so "plugged-again" cannot equal "plugged".
#
# Test-only env: FH_SKIP_PULL=1 skips the pull, so "pulled" must fail the check
# (negative control: the script exits 1 with "FAIL: Files did not change").
#
# Leaves the unit as found: arca_tap stop, stick re-plugged if still gone, and a
# remote_tap=1 line removed again only if this script added it.
# Exit: 0 PASS; 1 FAIL; 2 usage, unreachable device or precondition missing.
# Tap coordinates (Files tile, launcher page 1) were measured on .115 2026-10-18.

FH_TILE_X=617
FH_TILE_Y=163
FH_CONF=/opt/games/arca_config.conf

fh_usage() {
    cat <<'USAGE'
Usage: native_apps/files-hotplug-check.sh <ip> [--out DIR]

Opens Files through arca_tap, pulls and re-plugs the USB stick by sysfs, and
checks the framebuffer changed on the pull and returned on the plug.
  --out DIR   keep the three PNGs/raw captures there (default: mktemp -d)
Needs a stick inserted and mounted, and WSL/Linux python3+PIL for the PNGs.
Exit: 0 PASS, 1 FAIL, 2 usage or precondition.
USAGE
}

HOST=""; OUT=""; ADDED_CONF=0; TAP_ON=0; SD=""; SCAN=""; TMPD=""
case "${1:-}" in -h|--help) fh_usage; exit 0 ;; ""|-*) fh_usage >&2; exit 2 ;; esac
HOST="$1"; shift
case "$HOST" in *@*) ;; *) HOST="root@$HOST" ;; esac
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="${2:-}"; shift 2 || { fh_usage >&2; exit 2; } ;;
        *) fh_usage >&2; exit 2 ;;
    esac
done

# -n: never let ssh swallow our stdin.
rssh() { timeout "${FH_TIMEOUT:-40}" ssh -n -o ConnectTimeout=5 "$HOST" "$@"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pre()  { echo "ERROR: $*" >&2; exit 2; }

cleanup() {
    trap - EXIT INT TERM
    echo "  cleanup"
    if [ "$TAP_ON" = 1 ]; then rssh "/opt/games/arca_tap stop" >/dev/null 2>&1 \
        || echo "  WARNING: arca_tap stop failed - run it by hand" >&2; fi
    if [ -n "$SCAN" ] && ! rssh "mountpoint -q /media/usb0" >/dev/null 2>&1; then
        rssh "echo '- - -' > $SCAN" >/dev/null 2>&1
        rssh "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do mountpoint -q /media/usb0 && exit 0; sleep 1; done; exit 1" \
            >/dev/null 2>&1 || echo "  WARNING: stick not remounted on /media/usb0" >&2
    fi
    if [ "$ADDED_CONF" = 1 ]; then rssh "sed -i '/^remote_tap=1\$/d' $FH_CONF" >/dev/null 2>&1; fi
    [ -z "$TMPD" ] || rm -rf "$TMPD"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ── preflight ──
rssh true >/dev/null 2>&1 || pre "cannot ssh to $HOST"
[ -n "$OUT" ] || OUT=$(mktemp -d)
mkdir -p "$OUT" || pre "cannot create $OUT"
TMPD=$(mktemp -d)
rssh "[ -x /opt/games/arca_tap ]" || pre "/opt/games/arca_tap missing"
rssh "fbset | grep geometry" 2>/dev/null | grep -q ' 800 480 800 480 32' || pre "fb0 is not 800x480 32bpp"

# The first USB disk: sdX whose sysfs path runs through a hostN.
# shellcheck disable=SC2016  # expands on the device
SD=$(rssh 'for d in /sys/block/sd?; do [ -e $d ] && { echo ${d##*/}; break; }; done' 2>/dev/null)
[ -n "$SD" ] || pre "no USB disk (/sys/block/sd?) on the unit"
HN=$(rssh "readlink -f /sys/block/$SD/device" 2>/dev/null | sed -n 's|.*/\(host[0-9]*\)/.*|\1|p')
[ -n "$HN" ] || pre "cannot find the scsi host of $SD"
SCAN=/sys/class/scsi_host/$HN/scan
rssh "mountpoint -q /media/usb0" || pre "stick not mounted on /media/usb0"
echo "  disk $SD on $HN"

if ! rssh "grep -q '^remote_tap=1\$' $FH_CONF"; then
    rssh "echo remote_tap=1 >> $FH_CONF" || pre "cannot enable remote_tap"
    ADDED_CONF=1; echo "  remote_tap=1 added (removed again at the end)"
fi

# crop_md5 <raw> <y0> <y1> <x0> <x1>: md5 of a rectangle of the 800x480 32bpp frame.
# The top bar (CPU %, clock) changes by itself, so only the pane area is graded.
crop_md5() {
    local y
    for ((y = $2; y < $3; y++)); do
        dd if="$1" skip=$((y * 3200 + $4 * 4)) count=$((($5 - $4) * 4)) iflag=skip_bytes,count_bytes bs=4096 2>/dev/null
    done | md5sum | cut -d' ' -f1
}
# Right pane (x 400..790, y 70..345) and its 5th row (y 262..300): a USB stick adds
# a "USB usb0" row to the folder list there; a pull removes it.
pane_md5() { crop_md5 "$1" 70 345 400 790; }
row_md5()  { crop_md5 "$1" 262 300 400 790; }

# stable_capture <name>: capture /dev/fb0 until two successive pane md5s agree (tearing).
stable_capture() {
    local f="$OUT/$1.raw" prev="" cur i
    # shellcheck disable=SC2034
    for i in 1 2 3 4 5; do
        rssh "cat /dev/fb0" > "$f" 2>/dev/null
        [ "$(wc -c < "$f")" = 1536000 ] || fail "capture $1 is not 1536000 bytes"
        cur=$(pane_md5 "$f")
        [ "$cur" = "$prev" ] && { echo "$cur"; return 0; }
        prev=$cur
    done
    fail "no stable frame for $1 after 5 captures"
}

png() {
    local f="$OUT/$1"
    python3 -c 'import PIL' >/dev/null 2>&1 \
        && python3 "$(dirname "${BASH_SOURCE[0]}")/../fb565_to_png.py" "$f.raw" "$f.png" >/dev/null 2>&1 \
        && echo "  $f.png" || echo "  $f.raw (no PNG: python3+PIL missing)"
}

# ── Files up ──
TAP_ON=1  # stop even on a half-failed start
rssh "/opt/games/arca_tap start" >/dev/null 2>&1 || fail "arca_tap start failed"
rssh "/opt/games/arca_tap status" 2>&1 | grep -qi running || echo "  note: arca_tap status did not say running"
rssh "/opt/games/arca_tap tap $FH_TILE_X $FH_TILE_Y" >/dev/null 2>&1 || fail "tap failed"
rssh "sleep 5; pidof file_manager" >/dev/null 2>&1 || fail "file_manager is not running after tapping the Files tile"

M1=$(stable_capture fh-1-plugged)
echo "  plugged        $M1"

# ── pull ──
if [ "${FH_SKIP_PULL:-}" = 1 ]; then
    echo "  (FH_SKIP_PULL=1: pull skipped)"
else
    rssh "sync; echo 1 > /sys/block/$SD/device/delete" || fail "pull command failed"
    rssh "sleep 1; [ ! -e /dev/$SD ]" || fail "/dev/$SD still exists after the pull"
fi
rssh "sleep 4; pidof file_manager" >/dev/null 2>&1 || fail "file_manager died after the pull"
M2=$(stable_capture fh-2-pulled)
echo "  pulled         $M2"
[ "$M1" != "$M2" ] || fail "Files did not change after the pull (pane identical to plugged)"

# ── plug ──
rssh "echo '- - -' > $SCAN" || fail "plug (scan) command failed"
rssh "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do mountpoint -q /media/usb0 && exit 0; sleep 1; done; exit 1" \
    || fail "/media/usb0 not mounted within 15 s of the plug"
rssh "sleep 4; pidof file_manager" >/dev/null 2>&1 || fail "file_manager died after the plug"
M3=$(stable_capture fh-3-plugged-again)
echo "  plugged-again  $M3"
for n in fh-1-plugged fh-2-pulled fh-3-plugged-again; do png "$n"; done
R2=$(row_md5 "$OUT/fh-2-pulled.raw"); R3=$(row_md5 "$OUT/fh-3-plugged-again.raw")
[ "$M3" != "$M2" ] || fail "Files did not change after the plug (pane identical to pulled)"
[ "$R3" != "$R2" ] || fail "no USB row appeared in the folder list after the plug"
echo "PASS: Files changed on the pull and gained the USB row on the plug (captures in $OUT)"
