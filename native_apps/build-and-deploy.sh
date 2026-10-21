#!/bin/bash
# Build all native games and optionally deploy to a Arca device.
#
# Usage:
#   ./build-and-deploy.sh                      # build only
#   ./build-and-deploy.sh <ip>                 # build + deploy binaries
#   ./build-and-deploy.sh <ip> set-default     # build + deploy + set as default app
#   ./build-and-deploy.sh --bundle <dir>       # build + stage into an offline bundle
#
# System setup (init service, audio, time-sync) is handled
# separately by provision.sh.  Run that once before deploying for the first time.
#
# --bundle stages the same artifacts this script would scp, under
# <dir>/root/<device-path>, with a declared-mode manifest — the layout
# ../release.sh publishes and ../rootfs/commission-offline.sh installs
# from.  It always builds first: this component's
# targets take well under a minute, so there is no reason for a
# stage-what-is-already-there mode and therefore no way to bundle a stale
# binary.  (ScummVM, whose link is ~2 minutes, does have one.)

# A source-path directive, not a disable: it ENABLES analysis of the four libraries
# sourced below.  shellcheck 0.7.0 resolves a `source=` path against its OWN working
# directory rather than the script's, and the host gate runs it from the repo root,
# where neither app-manifests.sh nor ../lib/arca-ssh.sh exists.  It is file-wide ONLY
# here, before the first command: below `set -e` it scopes to the next command alone
# and all four SC1091 findings come back.  Measured both ways.
# ⚠️ Do not begin any of these explanatory lines with the checker's own name — a
# comment opening that way is parsed AS a directive, and the parse error voids the
# whole file's analysis (SC1072/SC1073), which reads as a suspiciously clean run.
# shellcheck source-path=SCRIPTDIR
set -e
_START_SECONDS=$(date +%s)

# Work from this script's own directory, so it can be invoked by path.  Every
# source, build and icon path below is relative (common/*.c, build/, tests/,
# ./check-arm-safe.sh); without this the first compile dies with
# "common/framebuffer.c: No such file or directory" and the script only ever
# worked because deploy-all.sh wraps it in a subshell cd.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# The .app manifests are data in one file, written locally, and copied by BOTH
# the deploy path and --bundle.  They used to be nine heredocs inside an
# `ssh … <<REMOTE` block, so they existed only when a device was reachable and
# ../rootfs/commission-offline.sh had no way to produce the same bytes.
# shellcheck source=app-manifests.sh
. "$SCRIPT_DIR/app-manifests.sh"
# shellcheck source=sound-sets.sh
. "$SCRIPT_DIR/sound-sets.sh"

# The two shared libraries every path needs. The SSH gate because every deploy goes
# through it; ../lib/arca-bundle.sh because --bundle stages through it AND a deploy
# clears the provenance stamp through it, so it can no longer be sourced in one branch.
# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=../lib/arca-bundle.sh
. "$REPO_ROOT/lib/arca-bundle.sh"
# shellcheck source=../lib/product.sh
. "$REPO_ROOT/lib/product.sh"
# shellcheck source=../lib/arca-identify.sh
. "$REPO_ROOT/lib/arca-identify.sh"
# shellcheck source=../lib/arca-provision.sh
. "$REPO_ROOT/lib/arca-provision.sh"

# ── argument shapes ─────────────────────────────────────────────────────────
# Two, and they do not mix: `--bundle <dir>` needs no device, and every deploy
# form needs one.  Parsed before anything else so a typo cannot reach the build.
BUNDLE_DIR=""
if [[ "${1:-}" == "--bundle" ]]; then
    BUNDLE_DIR="${2:-}"
    DEVICE_IP=""
    MODE=""
    if [[ -z "$BUNDLE_DIR" ]]; then
        echo "--bundle requires a directory"
        echo ""
        echo "Usage: $0 --bundle <dir>"
        exit 1
    fi
    if [[ -n "${3:-}" ]]; then
        echo "Unexpected argument after --bundle <dir>: $3"
        exit 1
    fi
else
    DEVICE_IP="${1:-}"
    MODE="${2:-}"
fi
DEVICE="root@${DEVICE_IP}"
GAMES_DIR="/opt/games"

# ── colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "[$(date '+%H:%M:%S')] ${GREEN}  ✓ $*${NC}"; }
info() { echo -e "[$(date '+%H:%M:%S')] ${YELLOW}  → $*${NC}"; }
warn() { echo -e "[$(date '+%H:%M:%S')] ${BLUE}  ! $*${NC}"; }
err()  { echo -e "[$(date '+%H:%M:%S')] ${RED}  ✗ $*${NC}"; exit 1; }

# ── argument validation ─────────────────────────────────────────────────────
# Validate BEFORE building.  A bad first argument used to surface only at the
# first ssh, i.e. after every target had already been compiled.
usage() {
    echo "Usage: $0 [<ip>] [set-default]"
    echo "       $0 --bundle <dir>"
    echo ""
    echo "  <ip>          Device IPv4 address; omit to build without deploying"
    echo "  set-default   Also make app_launcher the boot app"
    echo "  --bundle <dir>  Build, then stage every artifact under <dir>/root/"
    echo "                  with a declared-mode manifest. No device needed."
    echo ""
    echo "Examples:"
    echo "  $0                          # build only"
    echo "  $0 192.168.50.73            # build + deploy"
    echo "  $0 192.168.50.73 set-default"
    echo "  $0 --bundle /tmp/arca-bundle"
    exit 1
}

IPV4_RE='^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$'
if [[ -n "$DEVICE_IP" ]] && [[ ! "$DEVICE_IP" =~ $IPV4_RE ]]; then
    echo "Not an IPv4 address: $DEVICE_IP"
    echo ""
    usage
fi

# set-default is the only mode this script accepts; anything else was silently
# ignored, so a typo deployed without doing what you asked.
case "$MODE" in
    ""|set-default) ;;
    *) echo "Unknown mode: $MODE"; echo ""; usage ;;
esac

# ── 1. cross-compiler check ─────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo " $PRODUCT_NAME Build + Deploy"
echo "════════════════════════════════════════"
info "Started — $(date '+%Y-%m-%d %H:%M:%S')"

# Toolchain and link mode: soft-float ABI, DYNAMIC against the device's own soft-float
# glibc userspace — the same settings as scummvm-arca/build-and-deploy.sh, whose
# comment carries the loader/glibc mechanism and why armv7-a is still divide-free.  The
# flags ride inside CC so every compile and link line below gets them.
#
# The dynamic link is what lets common/audio_out.c play through ALSA: an explicit
# define, the alsa-lib headers and -lasound on every link that carries audio_out.o.
# The libasound linked against is built by build-alsa-lib.sh into its own per-ABI
# prefix (arm-deps-softfp) and never deployed — the device's own libasound.so.2 is
# the runtime.  A hard-float binary could not load that soft-float library at all.
TC=arm-linux-gnueabi
ARMFLAGS="-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp"
ALSA_PREFIX="$SCRIPT_DIR/arm-deps-softfp"
if [ ! -e "$ALSA_PREFIX/usr/lib/libasound.so" ]; then
    info "alsa-lib not built yet — running build-alsa-lib.sh"
    bash "$SCRIPT_DIR/build-alsa-lib.sh"
fi
AUDIO_CFLAGS=(-DAUDIO_OUT_HAVE_ALSA "-I$ALSA_PREFIX/usr/include")
AUDIO_LIBS=("-L$ALSA_PREFIX/usr/lib" -lasound)

# libvterm, for the terminal app and for nothing else: its -I/-L/-lvterm ride on
# that one compile line below, never in CC or AUDIO_*.  Static (libvterm.a, built
# by build-libvterm.sh into the same per-ABI prefix as alsa-lib) so nothing of it
# is deployed.  ⚠️ The artifacts are tested, not a stamp or a flag: a half-finished
# build-libvterm.sh leaves no libvterm.a and the link would otherwise fail late.
VTERM_PREFIX="$SCRIPT_DIR/arm-deps-softfp"
if [ ! -f "$VTERM_PREFIX/usr/lib/libvterm.a" ] || [ ! -f "$VTERM_PREFIX/usr/include/vterm.h" ]; then
    info "libvterm not built yet — running build-libvterm.sh"
    bash "$SCRIPT_DIR/build-libvterm.sh"
fi
[ -f "$VTERM_PREFIX/usr/lib/libvterm.a" ] || err "libvterm.a is missing after build-libvterm.sh"
VTERM_CFLAGS=("-I$VTERM_PREFIX/usr/include")
VTERM_LIBS=("-L$VTERM_PREFIX/usr/lib" -lvterm)
CC="$TC-gcc $ARMFLAGS"

# Warning flags applied to every compile line.  These are advisory only — the build
# does not use -Werror, because ~30k lines of C were written with warnings off and a
# hard failure would block every deploy.  Capture the output and work the list down:
#   ./build-and-deploy.sh 2>&1 | grep -E 'warning:' | sort | uniq -c | sort -rn
WARN=(-Wall -Wextra -Wno-unused-parameter)

if ! command -v "$TC-gcc" &>/dev/null; then
    err "ARM cross-compiler not found. Install every host prerequisite with\n  setup-build-env.sh, at the repo root — it carries the one package set."
fi
info "Compiler: $($CC --version | head -1)"
echo ""


# ── 2. build ─────────────────────────────────────────────────────────────────
mkdir -p build

step() { echo "[$1] $2..."; }

step " 1/35" "framebuffer";  $CC "${WARN[@]}" -O2 -c common/framebuffer.c    -o build/framebuffer.o
$CC "${WARN[@]}" -O2 -c common/font.c           -o build/font.o
step " 2/35" "touch_input";  $CC "${WARN[@]}" -O2 -c common/touch_input.c    -o build/touch_input.o
step " 3/35" "touch_calib";  $CC "${WARN[@]}" -O2 -c common/touch_calib.c    -o build/touch_calib.o
step " 4/35" "hardware";     $CC "${WARN[@]}" -O2 -c common/hardware.c        -o build/hardware.o
step " 5/35" "common";       $CC "${WARN[@]}" -O2 -c common/common.c          -o build/common.o
step " 6/35" "highscore";    $CC "${WARN[@]}" -O2 -c common/highscore.c       -o build/highscore.o
step " 7/35" "keyboard";     $CC "${WARN[@]}" -O2 -c common/keyboard.c        -o build/keyboard.o
step " 8/35" "ui_layout";    $CC "${WARN[@]}" -O2 -c common/ui_layout.c       -o build/ui_layout.o
                              $CC "${WARN[@]}" -O2 -c common/ui_flow.c         -o build/ui_flow.o
step " 9/35" "audio";        $CC "${WARN[@]}" -O2 -c common/audio.c           -o build/audio.o
step "10/35" "audio_gen";    $CC "${WARN[@]}" -O2 -c common/audio_gen.c       -o build/audio_gen.o
                              $CC "${WARN[@]}" -O2 -c common/audio_eq.c        -o build/audio_eq.o
                              $CC "${WARN[@]}" -O2 -c common/audio_enh.c       -o build/audio_enh.o
step "11/35" "audio_out";    $CC "${WARN[@]}" -O2 "${AUDIO_CFLAGS[@]}" -c common/audio_out.c       -o build/audio_out.o
step "12/35" "audio_wav";    $CC "${WARN[@]}" -O2 -c common/audio_wav.c       -o build/audio_wav.o
step "13/35" "http";         $CC "${WARN[@]}" -O2 -c common/http.c            -o build/http.o
step "14/35" "json";         $CC "${WARN[@]}" -O2 -c common/json.c            -o build/json.o
step "15/35" "audio_bed";    $CC "${WARN[@]}" -O2 -c common/audio_bed.c       -o build/audio_bed.o
step "16/35" "ppm";          $CC "${WARN[@]}" -O2 -c common/ppm.c             -o build/ppm.o
                              $CC "${WARN[@]}" -O2 -c common/product.c         -o build/product.o
                              $CC "${WARN[@]}" -O2 -c common/icon_grid.c       -o build/icon_grid.o
                              $CC "${WARN[@]}" -O2 -I. -c common/top_bar.c         -o build/top_bar.o
                              $CC "${WARN[@]}" -O2 -c common/ui_focus.c        -o build/ui_focus.o
                              $CC "${WARN[@]}" -O2 -c common/list_view.c       -o build/list_view.o
                              $CC "${WARN[@]}" -O2 -c common/pointer.c         -o build/pointer.o
                              $CC "${WARN[@]}" -O2 -c common/start_menu.c      -o build/start_menu.o
step "17/35" "logger";       $CC "${WARN[@]}" -O2 -c common/logger.c          -o build/logger.o
step "18/35" "config";       $CC "${WARN[@]}" -O2 -c common/config.c          -o build/config.o
step "19/35" "gamepad";      $CC "${WARN[@]}" -O2 -c common/gamepad.c         -o build/gamepad.o
                              $CC "${WARN[@]}" -O2 -c common/input_scan.c      -o build/input_scan.o
                              $CC "${WARN[@]}" -O2 -c common/input_slots.c     -o build/input_slots.o

# gamepad.c finds its devices through input_scan.c (classifier + event* walk),
# so every binary that links gamepad.o links it too, and input_slots.o (which
# player slot each device holds) rides along — name GAMEPAD_OBJ, never
# build/gamepad.o alone.  control_panel links it too: keyboard/pad focus.
# start_menu.o (the games' start menu) rides here because it reads
# gamepad_player_mask() and every game links this list — one place, so a new
# game cannot miss it.  It needs only COMMON_OBJ besides, which every linker of
# GAMEPAD_OBJ also names; it uses ui_focus.h's UiRect type but no ui_focus.o
# function, so control_panel's own build/ui_focus.o stays the only copy linked.
# ⚠️ COMMON_OBJ needs this list too: common.o's game-over draws the shared
# scores page (start_menu.o) and keyboard.o's name entry polls the pad
# (gamepad.o), so every binary naming COMMON_OBJ also names GAMEPAD_OBJ.
GAMEPAD_OBJ=(build/gamepad.o build/input_scan.o build/input_slots.o build/start_menu.o)

COMMON_OBJ=(build/framebuffer.o build/font.o build/touch_input.o build/hardware.o
            build/common.o build/highscore.o build/keyboard.o
            build/audio.o build/audio_gen.o build/audio_eq.o build/audio_enh.o build/audio_out.o
            build/audio_wav.o build/audio_bed.o build/config.o)

# audio_gen.o rides with audio.o and is not optional: audio.c calls into it for
# every frame count, every byte count, the envelope and the write loop.  Both are
# in COMMON_OBJ, so every binary that links the library gets them.
#
# audio_out.o joined them in Phase 2 and is not optional either: `audio.h`
# includes `audio_out.h` and every `Audio` embeds an `AudioOut`, so the link
# fails without it rather than degrading — which is the right failure.  ⚠️ The
# redeploy price differs per object: vnc_client (its Makefile's SRCS) links none of
# them, but ScummVM links audio_out.o and audio_gen.o through configure.patch, so
# a change to either of those goes out with ScummVM too (../CLAUDE.md, the table).
#
# audio_wav.o joined in Phase 8 and is not optional either, for a DIFFERENT and
# less obvious reason than audio_out.o's: `audio.h` includes `audio_wav.h` and
# every `Audio` embeds two `AudioSampleVoice`, so the STRUCT is complete without
# the object and only `audio_music_start()`/`audio_sfx_play()` reference its code.
# A binary that never plays a file therefore links fine — but audio.o always
# calls audio_wav_open/close/fill, so in practice the link fails without it,
# which is the right failure.  It has no device in it at all (plain stdio), so it
# is also the one audio object a host test needs no shim to reach.
#
# audio_bed.o joined in Phase 5 ⑤ and IS optional in the link sense — nothing in
# `audio.h` reaches it and a binary with no bed never calls it, so it costs those
# binaries only its bytes.  It is in COMMON_OBJ anyway rather than named per game:
# a bed is now three lines in any game (`common/audio_bed.h`), and a per-game
# object list is exactly the kind of second place a new game silently misses.

# touch_calib.o is NOT in COMMON_OBJ: only the two tools that measure the touch
# mapping need it, and there is no reason to carry the target table into eight
# games.  Both of them must link it, though — it is the one place the fit lives.
CALIB_OBJ="build/touch_calib.o"

step "20/35" "snake";        $CC "${WARN[@]}" -O2 snake/snake.c             "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/snake         -lm "${AUDIO_LIBS[@]}"
step "21/35" "tetris";       $CC "${WARN[@]}" -O2 tetris/tetris.c           "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/tetris        -lm "${AUDIO_LIBS[@]}"
step "22/35" "pong";         $CC "${WARN[@]}" -O2 pong/pong.c               "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/pong          -lm "${AUDIO_LIBS[@]}"

step "23/35" "brick_breaker"
$CC "${WARN[@]}" -O2 brick_breaker/brick_breaker.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/brick_breaker -lm "${AUDIO_LIBS[@]}"

step "24/35" "samegame"
$CC "${WARN[@]}" -O2 samegame/samegame.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/samegame -lm "${AUDIO_LIBS[@]}"

step "25/35" "frogger"
$CC "${WARN[@]}" -O2 frogger/frogger.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/frogger -lm "${AUDIO_LIBS[@]}"

step "26/35" "platformer"
$CC "${WARN[@]}" -O2 platformer/platformer.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/platformer -lm "${AUDIO_LIBS[@]}"

step "27/35" "app_launcher"
$CC "${WARN[@]}" -O2 -I. app_launcher/app_launcher.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" build/ppm.o build/icon_grid.o build/top_bar.o build/pointer.o build/logger.o build/product.o common/status_line.c control_panel/cpu_load.c -o build/app_launcher -lm "${AUDIO_LIBS[@]}"

step "28/35" "theremin"
$CC "${WARN[@]}" -O2 -I. \
  theremin/theremin.c \
  "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" build/logger.o build/ppm.o \
  -o build/theremin -lm "${AUDIO_LIBS[@]}"

# Owns the calibration wizard (Touch tab), which is why it links CALIB_OBJ.
# The standalone unified_calibrate was folded into it and deleted — it was a
# second, independent copy of the same 9-tap fit, carrying the same defect.
step "29/35" "control_panel"
$CC "${WARN[@]}" -O2 -I. control_panel/control_panel.c common/eq_panel.c control_panel/cp_page_name.c control_panel/usb_bus.c control_panel/touch_wizard.c control_panel/led_page.c control_panel/monitor_page.c control_panel/cpu_load.c control_panel/soc_temp.c control_panel/mon_graph.c sysmon/mon_ring.c control_panel/info_page.c control_panel/network_page.c control_panel/usb_page.c control_panel/input_page.c control_panel/audio_page.c control_panel/display_page.c control_panel/bluetooth_page.c control_panel/system_page.c control_panel/log_page.c control_panel/log_filter.c control_panel/ssh_page.c control_panel/cp_exec.c control_panel/backup_page.c control_panel/backup_parse.c common/usb_watch.c file_manager/fm_core.c build/list_view.o control_panel/sys_settings.c control_panel/bt_ctl.c common/status_line.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" $CALIB_OBJ build/ui_layout.o build/ui_flow.o build/ui_focus.o build/ppm.o build/icon_grid.o build/top_bar.o build/pointer.o build/product.o -o build/control_panel -lm "${AUDIO_LIBS[@]}"

# Touch diagnostic. It was previously absent from this script, which is why a
# deployed copy of the old trace tool was stale (pre-bezel); that tool now lives in
# control_panel's multi-touch tester. A second, touch_inject, is gone
# entirely: it wrote synthetic events to /dev/input/event0, which is the OUTPUT
# path, so it announced success and delivered nothing to any reader. Injection
# needs /dev/uinput and this kernel has none — ../CLAUDE.md carries the rule.
step "30/35" "touch_raw"
$CC "${WARN[@]}" -O2 -I. tests/touch_raw.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" $CALIB_OBJ -o build/touch_raw -lm "${AUDIO_LIBS[@]}"
# arca_tap: scripted touch (tap/hold/swipe) through a uinput device; gated by the
# Input page's REMOTE TAP setting. Links like touch_raw: it inverts the calibration.
$CC "${WARN[@]}" -O2 -I. arca_tap/arca_tap.c common/remote_tap.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" $CALIB_OBJ -o build/arca_tap -lm "${AUDIO_LIBS[@]}"


# The mix bus, driven by hand.  Groups I/J/K of tests/audio_gen_test.c cover the
# arithmetic; whether two sounds are AUDIBLE as two, and whether the ~60 ms
# minimum-tone rule survives a stream that is never reset, need an ear at the panel.
step "31/35" "audio_mix_test"
$CC "${WARN[@]}" -O2 -I. tests/audio_mix_test.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/audio_mix_test -lm "${AUDIO_LIBS[@]}"

# The Monitor page's history daemon, started at boot by /etc/init.d/arcamond.
# From common/ it links only hardware.c (+ config.c), for the over-temperature
# red LED; it opens no framebuffer or input.
step "32/35" "arcamond"
$CC "${WARN[@]}" -O2 -I. sysmon/arcamond.c sysmon/mon_ring.c control_panel/cpu_load.c control_panel/soc_temp.c common/hardware.c common/config.c -o build/arcamond

# The terminal: /bin/login in a pty, drawn with libvterm.  The one binary with
# VTERM_CFLAGS / VTERM_LIBS; term_keymap.c is its pure key table (host test
# tests/term_keymap_test.c).  -lvterm goes after the objects that need it.
step "33/35" "terminal"
$CC "${WARN[@]}" -O2 -I. "${VTERM_CFLAGS[@]}" terminal/terminal.c terminal/term_keymap.c control_panel/sys_settings.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/terminal -lm "${VTERM_LIBS[@]}" "${AUDIO_LIBS[@]}"

# The file manager: two list_view panes over the data, games, audio and USB roots.
# fm_core.c is its screen-free half (host test tests/file_manager_test.c); the top
# bar, the pointer and the list widget are the shared objects built above.
step "34/35" "file_manager"
$CC "${WARN[@]}" -O2 -I. file_manager/file_manager.c file_manager/fm_core.c "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" build/list_view.o build/top_bar.o build/icon_grid.o build/ppm.o build/pointer.o build/ui_flow.o build/ui_focus.o control_panel/cpu_load.c common/status_line.c common/usb_watch.c -o build/file_manager -lm "${AUDIO_LIBS[@]}"

# jellyfin: music client.  jf_core/jf_api/jf_model/jf_url/jf_login are screen-free (host tests);
# jf_ui.c draws, jellyfin.c is the loop.  http.o and json.o come from the common block above.
step "35/35" "jellyfin"
$CC "${WARN[@]}" -O2 -I. -Icommon -Ijellyfin jellyfin/jellyfin.c common/eq_panel.c jellyfin/jf_ui.c jellyfin/jf_core.c jellyfin/jf_api.c jellyfin/jf_model.c jellyfin/jf_url.c jellyfin/jf_login.c jellyfin/jf_queue_store.c jellyfin/jf_art.c common/icons.c common/jf_img.c build/http.o build/json.o build/list_view.o build/top_bar.o build/icon_grid.o build/ppm.o build/pointer.o build/ui_flow.o build/ui_focus.o control_panel/cpu_load.c common/status_line.c common/logger.c build/product.o "${COMMON_OBJ[@]}" "${GAMEPAD_OBJ[@]}" -o build/jellyfin -lm "${AUDIO_LIBS[@]}"

# Collect icon files from source dirs → build/icons/.  control_panel/icons/ holds
# the control panel's home-grid icons (gen_cp_icons.py); control_panel reads them
# from /opt/arca/icons/ like the launcher reads a manifest's icon=.
#
# ⚠️ Emptied first, as build/apps/ is below: build/ is never cleaned, and both
# directories are copied by GLOB — to the device and into --bundle — so an icon
# or a manifest this tree no longer has would otherwise ship from a leftover.
# The deploy's retired-name sweep runs after its copy; a bundle has no sweep.
mkdir -p build/icons
rm -f build/icons/*.ppm
ICON_COUNT=0
for ppm in *//*.ppm control_panel/icons/*.ppm; do
    [ -f "$ppm" ] || continue
    cp "$ppm" build/icons/
    ICON_COUNT=$((ICON_COUNT + 1))
done
[ $ICON_COUNT -gt 0 ] && echo "  Collected $ICON_COUNT icon(s) → build/icons/"

# Write the .app manifests locally, from app-manifests.sh's data.  Both the
# deploy path and --bundle copy these exact files, which is the point of having
# them on disk rather than in an ssh heredoc.
rm -f build/apps/*.app
arca_write_app_manifests build/apps
echo "  Wrote $(find build/apps -maxdepth 1 -type f -name '*.app' 2>/dev/null | wc -l) app manifest(s) → build/apps/"

# The per-game sound sets, from sound-sets.sh's data and for the same reason: that
# table is the ONE home for every game's music paths, and a path
# that drifted between this path and --bundle would install a game that plays
# nothing.  ⚠️ Checked rather than assumed — arca_write_sound_sets counts what it
# wrote, because its writer loop runs in a subshell and cannot report itself.
arca_write_sound_sets build/soundsets || err "could not write the sound sets"
echo "  Wrote $(find build/soundsets -maxdepth 1 -type f -name '*.sound' 2>/dev/null | wc -l) sound set(s) → build/soundsets/"

# ── deployed artifacts ──────────────────────────────────────────────────────
# ONE list, used for the build-size listing, the upload, the chmod, the md5
# verification and --bundle.  There used to be two (scp and chmod) and
# one binary was missing from the chmod one — it worked only because scp
# happens to carry the source file's mode.  A third copy for the md5 check would
# have recreated exactly that bug, and a fourth for
# --bundle would recreate it again.
GAMES_BINARIES=(snake tetris pong brick_breaker samegame frogger platformer
                theremin audio_mix_test control_panel arcamond
                touch_raw terminal file_manager arca_tap jellyfin)

echo ""
echo "Build sizes:"
ls -lh "${GAMES_BINARIES[@]/#/build/}" build/app_launcher \
    | awk '{printf "  %-24s %s\n", $9, $5}'
ok "Build complete ($(( $(date +%s) - _START_SECONDS ))s)"
echo ""

# ── 2b. ARM-safety gate ─────────────────────────────────────────────────────
# Cortex-A8 has no hardware integer divide; an sdiv/udiv means SIGILL (exit 132)
# with a blank screen and no output.  Runs on build-only too, so the problem is
# caught at the desk rather than on the wall.
if [[ -x ./check-arm-safe.sh ]]; then
    ./check-arm-safe.sh || err "ARM-safety check failed — refusing to deploy"
else
    warn "check-arm-safe.sh missing — skipping hardware-divide gate"
fi

# ── 2b2. Audio pacing gate ──────────────────────────────────────────────────
# A source check, not a binary one: an app that turns the mix bus on must also
# service it every iteration and keep audio_pump_active() in its frame pacing.
# Neither omission errors — the game runs and the sound has gaps in it — so this
# is arithmetic rather than a rule to remember.  Runs on build-only too, for the
# same reason as the gate above.
if [[ -x ./check-audio-pacing.sh ]]; then
    ./check-audio-pacing.sh || err "Audio pacing check failed — refusing to deploy"
else
    warn "check-audio-pacing.sh missing — skipping audio pacing gate"
fi
echo ""

# ── 2b3. Sound-asset gate ───────────────────────────────────────────────────
# An ASSET check, not a source one, and it exists because both of the ways a sound
# file fails on this device are SILENT: a rate the mixer does not grant is refused
# by clip_load() with the game falling back to its note table (so it sounds
# unchanged), and energy below the speaker's ~700 Hz knee is not quiet but absent.
# Neither is visible in a byte comparison, a screenshot or an md5.  It replaces
# fx_gen.c's retired spectral-flatness gate, which enforced the wrong property.
#
# ⚠️ **It does not block on a WEAK file, only on an unusable one**, and it skips
# itself when ffmpeg is absent — a build host without ffmpeg must still build.
if [[ -x ./check-sound-assets.sh ]]; then
    ./check-sound-assets.sh || err "Sound-asset check failed — refusing to deploy"
else
    warn "check-sound-assets.sh missing — skipping sound-asset gate"
fi
echo ""

# ── 2b4. Music-bed gate ─────────────────────────────────────────────────────
# The naming CONVENTION is load-bearing and lives in sound-sets.sh: that table is
# the ONE home for a bed path, so a mistyped stem or a count one too high there
# ships a game that plays its effects in silence — indistinguishable from a game
# with no bed.  This is also the only thing that counts beds reaching NO game,
# which is the state the tree was in while 18 of 24 were unreachable.
# Cheap: it reads the repo, no ffmpeg.
if [[ -x ./check-bed-files.sh ]]; then
    ./check-bed-files.sh || err "Music-bed check failed — refusing to deploy"
else
    warn "check-bed-files.sh missing — skipping music-bed gate"
fi
echo ""

# ── 2c. --bundle: stage instead of deploying ────────────────────────────────
# Deliberately AFTER the ARM-safety gate.  A bundle is published and then
# installed by someone with no toolchain, so it is the one artifact that must
# never carry an sdiv/udiv — the failure would surface on a wall-mounted panel as
# a blank screen with no output (../SYSTEM_ANALYSIS.md#61-cortex-a8-has-no-hardware-integer-divide).
if [[ -n "$BUNDLE_DIR" ]]; then
    echo "════════════════════════════════════════"
    echo " Staging bundle → $BUNDLE_DIR"
    echo "════════════════════════════════════════"

    arca_bundle_init "$BUNDLE_DIR" native_apps || err "could not prepare $BUNDLE_DIR"

    # 0755 for everything executable, 0644 for data.  Stated here, not read off
    # disk — /mnt/c reports 0777 for every file and discards chmod, so a mode
    # measured on this host is a constant (../CLAUDE.md, ../lib/arca-bundle.sh).
    for b in "${GAMES_BINARIES[@]}"; do
        arca_bundle_add "$BUNDLE_DIR" native_apps 0755 "build/$b" "$GAMES_DIR/$b" \
            || err "staging failed: $b"
    done
    arca_bundle_add "$BUNDLE_DIR" native_apps 0755 build/app_launcher /opt/arca/app_launcher \
        || err "staging failed: app_launcher"

    for f in build/apps/*.app; do
        [ -f "$f" ] || continue
        arca_bundle_add "$BUNDLE_DIR" native_apps 0644 "$f" "/opt/arca/apps/$(basename "$f")" \
            || err "staging failed: $f"
    done

    # The backup items (format: DEVELOPING.md#plug-in-file-formats): data files in
    # this tree, copied as they are by both paths.
    for f in backup.d/*.item; do
        [ -f "$f" ] || continue
        arca_bundle_add "$BUNDLE_DIR" native_apps 0644 "$f" "/opt/arca/backup.d/$(basename "$f")" \
            || err "staging failed: $f"
    done

    for f in build/icons/*.ppm; do
        [ -f "$f" ] || continue
        arca_bundle_add "$BUNDLE_DIR" native_apps 0644 "$f" "/opt/arca/icons/$(basename "$f")" \
            || err "staging failed: $f"
    done

    # The per-game sound sets.  ⚠️ Without these the games have NO MUSIC — the set
    # file is the only home for a bed path, so a bundle that omits them installs
    # seven games that play their effects in silence and log one line each.
    for f in build/soundsets/*.sound; do
        [ -f "$f" ] || continue
        arca_bundle_add "$BUNDLE_DIR" native_apps 0644 "$f" "$ARCA_SOUND_SET_DIR/$(basename "$f")" \
            || err "staging failed: $f"
    done

    # The effect clips, for the same reason the online path uploads them: without
    # them the canned sounds are note tables below the speaker's knee.  0644 —
    # data, read by audio.c, never executed.
    for f in sounds/fx_*.wav; do
        [ -f "$f" ] || continue
        arca_bundle_add "$BUNDLE_DIR" native_apps 0644 "$f" "/opt/sound/$(basename "$f")" \
            || err "staging failed: $f"
    done

    # The music beds, same destination and same reason — a bundle that installs
    # `platformer` without them installs a game whose configured bed is missing.
    #
    # ⚠️ These are Git LFS objects, and a clone made WITHOUT git-lfs leaves ~130-byte
    # POINTER TEXT files in their place.  Staging one of
    # those produces a bundle that installs cleanly and then plays no music at all —
    # `clip_load()`/the stream reader reject it, log one line and the game carries on.
    # So refuse it here, where the operator is still at a keyboard, rather than let it
    # travel.  The check is the pointer file's own first line, which is a documented
    # format, not the size — a size threshold would also reject a legitimately short
    # bed.
    for f in music/*.wav; do
        [ -f "$f" ] || continue
        if head -c 40 "$f" | grep -q 'version https://git-lfs'; then
            err "$f is an unfetched Git LFS pointer, not audio — run: git lfs install && git lfs pull"
        fi
        arca_bundle_add "$BUNDLE_DIR" native_apps 0644 "$f" "/opt/sound/$(basename "$f")" \
            || err "staging failed: $f"
    done

    # app_launcher is this component's boot target, and the only component that
    # has one — the launcher is what makes every other .app reachable.  0644:
    # /opt/arca/default-app is read, never executed.
    echo '/opt/arca/app_launcher' > build/default-app
    arca_bundle_add "$BUNDLE_DIR" native_apps 0644 build/default-app /opt/arca/default-app \
        || err "staging failed: default-app"

    ok "Staged $(arca_bundle_finish "$BUNDLE_DIR" native_apps) file(s)"
    echo ""
    exit 0
fi

# ── 3. deploy? ───────────────────────────────────────────────────────────────
if [[ -z "$DEVICE_IP" ]]; then
    echo "No IP supplied — build only. To deploy:"
    echo "  ./build-and-deploy.sh <ip>"
    echo "  ./build-and-deploy.sh <ip> set-default"
    exit 0
fi

echo "════════════════════════════════════════"
echo " Deploying to $DEVICE_IP"
echo "════════════════════════════════════════"

# Check SSH reachable
info "Testing SSH connection..."
# The shared gate (lib/arca-ssh.sh): "down" and "up but refusing our key" are different
# answers, and only the second one has a remedy worth offering.
arca_ssh_gate "$DEVICE" || err "Cannot continue without SSH to $DEVICE"
ok "SSH OK"

# These are locally built binaries, so any provenance stamp on the unit is about to
# stop being true. One shared writer (../lib/arca-bundle.sh), called before the first
# file goes out rather than after the last one.
arca_bundle_clear_stamp "$DEVICE" \
    || warn "could not clear /opt/arca/bundle.info — it may still name an older release"

# Verify system setup has been done: the respawn loop that provision installs is what
# starts the launcher this deploy is about to replace.
if ! ssh "$DEVICE" "[ -x /etc/init.d/arca-app ]" 2>/dev/null; then
    warn "System setup not detected on device."
    warn "Run provision.sh first:  ../provision.sh $DEVICE_IP"
    echo ""
    read -r -p "Continue deploying anyway? (y/n): " confirm
    [[ "$confirm" != "y" ]] && exit 1
fi

# Stop whatever is running (avoids "Text file busy" on scp).
#
# There is exactly one stop implementation and it lives on the device, in
# /etc/init.d/arca-app — it matches processes on their executable, so it
# also catches an app that app_launcher started.  Do NOT re-add a `killall` here:
# a per-script copy only knows the basenames whoever wrote it thought of, which
# is how a vnc_client survived a full deploy and repainted over the launcher.
info "Stopping running apps (device init script)..."
ssh "$DEVICE" 'if [ -x /etc/init.d/arca-app ]; then
    /etc/init.d/arca-app stop
else
    echo "  /etc/init.d/arca-app not installed - run ../provision.sh <ip>"
fi' || warn "stop reported a failure - a surviving process may hold the binaries"
ok "Launcher stopped"

# Ensure target directory exists
info "Ensuring target directories exist..."
ssh "$DEVICE" "mkdir -p $GAMES_DIR /var/log/arca"
ok "Target directories ready"

# ── deployed artifacts ──────────────────────────────────────────────────────
# GAMES_BINARIES is declared once, up with the build, so
# --bundle and this deploy path cannot ship different sets.

# Upload game binaries
info "Uploading game binaries → $GAMES_DIR/ (${#GAMES_BINARIES[@]} files)"
scp "${GAMES_BINARIES[@]/#/build/}" "$DEVICE:$GAMES_DIR/"
ok "Game binaries uploaded"

# Upload app launcher
info "Uploading app launcher → /opt/arca/"
scp build/app_launcher "$DEVICE:/opt/arca/"
ok "App launcher uploaded"

# Upload icons (if any)
if ls build/icons/*.ppm &>/dev/null; then
    info "Uploading icons → /opt/arca/icons/"
    ssh "$DEVICE" "mkdir -p /opt/arca/icons"
    scp build/icons/*.ppm "$DEVICE:/opt/arca/icons/"
    ok "Icons uploaded ($(ls build/icons/*.ppm | wc -l) file(s))"
fi

# Upload the generated effect clips → /opt/sound/
#
# ⚠️ Without these the four canned sounds fall back to their note tables, which
# is a working device that has NOT had the audibility fix — every one of those
# tones is at or below the speaker's knee.  So this is not an optional asset step.
#
# /opt/sound is where the music bed already lives and nothing in commissioning
# deletes that directory, so a re-commission does not take them away.  They
# are checked in (sounds/fx_*.wav — SOURCED files, not build output; ⚠️ running
# sounds/gen-sounds.sh overwrites them with generated noise), so
# unlike the music there is nothing to hand-copy.
if ls sounds/fx_*.wav &>/dev/null; then
    info "Uploading effect clips → /opt/sound/"
    ssh "$DEVICE" "mkdir -p /opt/sound"
    scp sounds/fx_*.wav "$DEVICE:/opt/sound/"
    ok "Effect clips uploaded ($(ls sounds/fx_*.wav | wc -l) file(s))"
fi

# Upload the music beds → /opt/sound/, but ONLY the ones that differ.
#
# ⚠️ Unlike the effects these are LARGE — 24 beds, 116.9 MB — so an unconditional
# scp adds all of that to every deploy of a component that is otherwise a few hundred
# KB of binaries.  They also never change: they are sourced files under LFS
# (tracked under LFS — see ../.gitattributes), not build output.  So compare md5 first and send
# nothing when the device already matches.  The md5 is the whole check — a size
# comparison would pass a truncated-then-padded file, and a timestamp comparison
# cannot be made to mean anything across the device's clock offset (../CLAUDE.md).
#
# ⚠️ **The glob is `music/*.wav`, deliberately not a per-game list.**  It was
# `music/music*.wav` while two beds existed and matched NOTHING the moment they were
# renamed to `officerunner{1,2}-mono.wav` — a name-gated list is a place a bed can be
# added and never travel, and the failure is silent at both ends (the game says "no
# music bed" once and plays on).  Cost of the general glob, measured 2026-08-22 on
# `.188`: **0.87 MB/s sustained** including the flush (9.8 MB = 4.6 s scp + 6.7 s
# `sync`), so a first deploy of all 24 is ~2¼ min and every later one is the md5 read.
#
# A game whose configured bed is missing plays its effects and carries on
# (`platformer`'s bed_service()), so this step failing is not fatal — but then the
# feature is silently absent, which is why it prints per file either way.
if ls music/*.wav &>/dev/null; then
    info "Checking music beds → /opt/sound/"
    ssh "$DEVICE" "mkdir -p /opt/sound"
    remote_md5="$(ssh "$DEVICE" "md5sum /opt/sound/*-mono.wav 2>/dev/null" || true)"
    for f in music/*.wav; do
        base="$(basename "$f")"
        # Same LFS-pointer refusal as the --bundle path: uploading a 130-byte
        # pointer gives the device a file the stream reader rejects in silence.
        if head -c 40 "$f" | grep -q 'version https://git-lfs'; then
            err "$f is an unfetched Git LFS pointer, not audio — run: git lfs install && git lfs pull"
        fi
        want="$(md5sum "$f" | cut -d' ' -f1)"
        have="$(echo "$remote_md5" | grep " /opt/sound/$base\$" | cut -d' ' -f1)"
        if [[ "$want" == "$have" ]]; then
            info "  $base — already current, skipped"
        else
            scp "$f" "$DEVICE:/opt/sound/" && ok "  $base uploaded"
        fi
    done
fi

# Every executable this script put on the device, as the device sees it.
DEPLOYED_EXECUTABLES=("${GAMES_BINARIES[@]/#/$GAMES_DIR/}" /opt/arca/app_launcher)

# Set permissions.  The chmod list is passed in as "$@" rather than written out
# again, so it cannot drift from what was uploaded.
info "Setting permissions..."
ssh "$DEVICE" bash -s -- "${#DEPLOYED_EXECUTABLES[@]}" "${DEPLOYED_EXECUTABLES[@]}" <<'REMOTE'
nexe="$1"; shift
chmod +x "${@:1:$nexe}"
shift "$nexe"

# Retired names: the binary that was folded into control_panel's Display tab,
# the theremin's old test-shaped name (its manifest goes with
# ARCA_APP_MANIFESTS_RETIRED, or the launcher shows two tiles), the retired
# launcher prototype, two icons no manifest names, and the control panel's old
# name device_tools (manifest via ARCA_APP_MANIFESTS_RETIRED), and the icon of
# audio_mix_test's retired tile (the binary stays: the Audio page runs it).
# The .hidden and .noargs marker files go by glob: nothing reads them, so
# nothing writes them.
# touch_inject is the leftover binary of a deleted evdev-write injector, which
# cannot inject on this device (no /dev/uinput).
# hardware_test, hardware_config, hardware_diag, usb_test and the backlight CLI
# are the diagnostic binaries control_panel replaced; their manifests go with
# ARCA_APP_MANIFESTS_RETIRED.
rm -f /opt/games/*.hidden /opt/games/*.noargs \
      /opt/games/touch_inject \
      /opt/games/hardware_test \
      /opt/games/hardware_config \
      /opt/games/hardware_diag \
      /opt/games/usb_test \
      /opt/games/backlight \
      /opt/games/game_selector \
      /opt/arca/icons/hardware_test.ppm \
      /opt/arca/icons/usb_test.ppm \
      /opt/games/unified_calibrate \
      /opt/games/audio_touch_test \
      /opt/arca/icons/audio_touch_test.ppm \
      /opt/games/device_tools \
      /opt/arca/icons/device_tools.ppm \
      /opt/arca/icons/audio_mix_test.ppm
REMOTE
ok "Permissions set"

# ── verify what landed ──────────────────────────────────────────────────────
# 19 executables were copied and made runnable with nothing checking that the
# bytes on the device are the bytes that were built.  A truncated scp, a full
# filesystem, or a surviving process still holding an old inode all look like a
# successful deploy otherwise.
info "Verifying deployed binaries (md5)..."
LOCAL_SUMS="$(
    for remote in "${DEPLOYED_EXECUTABLES[@]}"; do
        printf '%s  %s\n' "$remote" "$(md5sum "build/$(basename "$remote")" | cut -d' ' -f1)"
    done | sort
)"
REMOTE_SUMS="$(ssh "$DEVICE" md5sum "${DEPLOYED_EXECUTABLES[@]}" | awk '{print $2"  "$1}' | sort)"

if [[ "$LOCAL_SUMS" == "$REMOTE_SUMS" ]]; then
    ok "Verified ${#DEPLOYED_EXECUTABLES[@]}/${#DEPLOYED_EXECUTABLES[@]} binaries — md5 matches what was built"
else
    echo ""
    echo "  built (local)                        vs  deployed (device):"
    diff <(printf '%s\n' "$LOCAL_SUMS") <(printf '%s\n' "$REMOTE_SUMS") | sed 's/^/    /' || true
    echo ""
    err "md5 mismatch — what is on the device is NOT what was built.\n     A process may still hold an old binary: ssh $DEVICE /etc/init.d/arca-app status"
fi

# Deploy app manifests
#
# The manifests were WRITTEN to build/apps/ during the build, from
# app-manifests.sh's data, and are copied here.  They used to be nine
# `cat > … << APP` heredocs inside this ssh block, which meant
# ../rootfs/commission-offline.sh could not produce the same bytes
# without a second copy of them.
info "Installing app manifests..."
ssh "$DEVICE" "mkdir -p /opt/arca/apps"
scp build/apps/*.app "$DEVICE:/opt/arca/apps/"
ssh "$DEVICE" bash -s -- $ARCA_APP_MANIFESTS_RETIRED <<'REMOTE'
chmod 644 /opt/arca/apps/*.app

# Manifests this component used to install: their tools were folded into
# control_panel's tabs, and a stale manifest renders a tile whose exec= is gone.
for name in "$@"; do
    rm -f "/opt/arca/apps/$name.app"
done
REMOTE
ok "App manifests installed ($(ls build/apps/*.app | wc -l) file(s))"
echo ""

# Deploy the backup items: what the Control Panel's export/import page offers to save.
info "Installing backup items..."
ssh "$DEVICE" "mkdir -p /opt/arca/backup.d"
scp backup.d/*.item "$DEVICE:/opt/arca/backup.d/"
ssh "$DEVICE" "chmod 644 /opt/arca/backup.d/*.item"
ok "Backup items installed ($(find backup.d -name '*.item' | wc -l) file(s))"
echo ""

# The backup group of the provision plan: the engine /usr/sbin/arca-backup and the
# device-files/backup.d item files the Control Panel's Backup page reads. They are
# records in device-files/provision-rules.conf, so a unit that was only ever given
# component deploys still gets the page working. The steps are the usb group's
# (usb_host/build-and-deploy.sh); the decisions are not here.
info "Installing the backup group (provision-rules.conf)..."
PROV_RULES="$REPO_ROOT/device-files/provision-rules.conf"
[[ -f "$PROV_RULES" ]] || err "missing $PROV_RULES"
if ! PCHECK="$(arca_provision_validate "$PROV_RULES" "$REPO_ROOT")"; then
    echo "$PCHECK"; err "provision-rules.conf does not validate"
fi
BACKUP_PLAN=$(mktemp)
arca_provision_plan_component "$PROV_RULES" backup > "$BACKUP_PLAN" \
    || { rm -f "$BACKUP_PLAN"; err "could not compile the backup provision plan"; }
info "$(arca_provision_plan_summary "$BACKUP_PLAN")"
arca_provision_push_installs "$BACKUP_PLAN" "$REPO_ROOT" "$DEVICE" \
    || { rm -f "$BACKUP_PLAN"; err "could not copy the backup provision sources to the device"; }
ssh "$DEVICE" "cat > /tmp/arca-backup-plan" < "$BACKUP_PLAN"
arca_provision_online_script | ssh "$DEVICE" "cat > /tmp/arca-backup-provision.sh"
ssh "$DEVICE" "sh /tmp/arca-backup-provision.sh /tmp/arca-backup-plan; rc=\$?; rm -f /tmp/arca-backup-provision.sh /tmp/arca-backup-plan; exit \$rc" \
    || { rm -f "$BACKUP_PLAN"; err "the backup provision step failed on the device"; }
rm -f "$BACKUP_PLAN"
ok "backup group installed (arca-backup and its item files)"
echo ""

# Deploy the per-game sound sets
#
# Same shape as the manifests above and for the same reason: written to
# build/soundsets/ during the build from sound-sets.sh's data, copied here, and
# copied by --bundle.  ⚠️ These are not optional — the C side derives no bed
# path, so a device without them has seven games with no music.
info "Installing sound sets..."
ssh "$DEVICE" "mkdir -p $ARCA_SOUND_SET_DIR"
scp build/soundsets/*.sound "$DEVICE:$ARCA_SOUND_SET_DIR/"
ssh "$DEVICE" "chmod 644 $ARCA_SOUND_SET_DIR/*.sound"
ok "Sound sets installed ($(ls build/soundsets/*.sound | wc -l) file(s))"
echo ""

# ── 4. set-default mode? ────────────────────────────────────────────────────
if [[ "$MODE" == "set-default" ]]; then
    info "Setting app launcher as default app..."
    ssh "$DEVICE" "mkdir -p /opt/arca && echo '/opt/arca/app_launcher' > /opt/arca/default-app"
    ok "Default app → /opt/arca/app_launcher"
fi

# ── 5. restart launcher ─────────────────────────────────────────────────────
# If the init service is installed, restart it (re-creates respawn wrapper).
# Otherwise just tell the user how to start manually.
if ssh "$DEVICE" '[ -f /etc/init.d/arca-app ]' 2>/dev/null; then
    info "Restarting app launcher..."
    ssh "$DEVICE" '/etc/init.d/arca-app start' 2>&1 | grep -v '^$'
    ok "Launcher running"
    # arca-app stop ends every process running out of $GAMES_DIR, arcamond
    # included (it has to, or scp could not replace the binary), so restart it.
    ssh "$DEVICE" 'if [ -x /etc/init.d/arcamond ]; then /etc/init.d/arcamond start; fi' 2>&1 | grep -v '^$' || true
else
    echo ""
    echo "  To start app launcher:"
    echo "    ssh $DEVICE '/opt/arca/app_launcher'"
fi

_END_SECONDS=$(date +%s)
_ELAPSED=$((_END_SECONDS - _START_SECONDS))
printf "[$(date '+%H:%M:%S')] Total time: %dm%02ds\n" $((_ELAPSED / 60)) $((_ELAPSED % 60))
echo ""
