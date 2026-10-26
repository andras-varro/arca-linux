#!/bin/bash
# Profile-guided build of the VICE C64 libretro core, end to end.
#
# Steps: (1) cross-build retro_bench from this tree; (2) build vice instrumented
# (build-cores.sh --pgo-gen, -O2 -fprofile-generate); (3) on the unit: stop the
# launcher, run the instrumented core for N frames on every training pair with
# retro_bench (one process at a time, GCOV_PREFIX=/tmp/...), pull the .gcda files
# back, clean up, start the launcher once; (4) store the profile in
# $RETRO_PGO_DIR (default ~/retro-build/pgo/vice) and rebuild vice with
# -fprofile-use into --out. Plain `retro/build-and-deploy.sh <ip>` then picks the
# profile up by itself (`--no-pgo` turns that off), so deploy afterwards.
#
# Training data is copyrighted game content and stays on the unit: a pair is two
# paths ON THE UNIT, the content file and a frontend state file saved in it
# (states/c64/<content>.s1). Nothing of either is copied to the host. The profile
# and the resulting .so are build artifacts and stay outside the repo.
#
# Usage (WSL):
#   retro/pgo-vice.sh <ip> --pair CONTENT:STATE [--pair ...] [--frames N] [--opts K=V,...]
#                          [--work DIR] [--out DIR]
#     --pair      content and state, both paths on the unit, separated by ':' (so neither
#                 may contain one, nor a single quote); a relative path is under
#                 /home/root/data/retro.
#     --frames N  frames per pair (default 3000, ~64 ms each when instrumented)
#     --opts      core options for the run (default: drive emulation off, 11025 Hz,
#                 FastSID, i.e. what retro/systems/c64.sys sets plus the frontend's SID)
#     --work DIR  build area, as build-cores.sh (default ~/retro-build); it must be the
#                 same for the instrumented and the final build, which this script ensures
#     --out DIR   where the PGO core goes (default ~/retro-build/out)
# Refuses to run, touching nothing, if a `retro` process is already running on the unit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
REMOTE_DIR=/home/root/data/retro
TMP=/tmp/pgo-vice

ts()  { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2; exit 1; }
usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

[ $# -ge 1 ] || { usage >&2; exit 1; }
case "$1" in -h|--help) usage; exit 0 ;; -*) die "first argument must be the unit IP" ;; esac
IP=$1; shift
FRAMES=3000
OPTS="vice_drive_true_emulation=disabled,vice_sound_sample_rate=11025,vice_sid_engine=FastSID"
WORK="$HOME/retro-build"
OUT="$HOME/retro-build/out"
PAIRS=()
while [ $# -gt 0 ]; do
    case $1 in
        --pair)   PAIRS+=("${2:?--pair needs CONTENT:STATE}"); shift 2 ;;
        --frames) FRAMES=${2:?}; shift 2 ;;
        --opts)   OPTS=${2:?}; shift 2 ;;
        --work)   WORK=${2:?}; shift 2 ;;
        --out)    OUT=${2:?}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done
[ ${#PAIRS[@]} -gt 0 ] || die "need at least one --pair CONTENT:STATE"
case $FRAMES in ''|*[!0-9]*) die "--frames must be a number" ;; esac
for p in "${PAIRS[@]}"; do
    case $p in *:*) ;; *) die "pair '$p' is not CONTENT:STATE" ;; esac
    case $p in *\'*) die "pair '$p' contains a single quote" ;; esac
done

WORK=$(realpath -m "$WORK"); OUT=$(realpath -m "$OUT")
PGO_DIR="$(realpath -m "${RETRO_PGO_DIR:-$HOME/retro-build/pgo/vice}")"
GEN_OUT="$WORK/pgo/gen-out"
DEVICE="root@$IP"
case "$PGO_DIR/$GEN_OUT/" in "$REPO_ROOT"/*) die "profile and work dirs must be outside the repo" ;; esac

# shellcheck source-path=SCRIPTDIR
# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
arca_ssh_gate "$DEVICE" || die "cannot continue without SSH to $DEVICE"
# shellcheck disable=SC2029
[ -z "$(ssh "$DEVICE" 'pidof retro')" ] || die "a retro process is running on $IP (someone is playing); not touching it"

# ── 1. bench, 2. instrumented core ──────────────────────────────────────────
mkdir -p "$WORK/pgo"
ts "building retro_bench"
arm-linux-gnueabi-gcc -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp -O2 \
    "$SCRIPT_DIR/tools/retro_bench.c" -o "$WORK/pgo/retro_bench" -ldl || die "retro_bench build failed"
bash "$REPO_ROOT/native_apps/check-arm-safe.sh" "$WORK/pgo/retro_bench" >"$WORK/pgo/bench.armsafe.log" 2>&1 || true
ts "building the instrumented vice"
bash "$SCRIPT_DIR/build-cores.sh" --work "$WORK" --out "$GEN_OUT" --pgo-gen vice || die "instrumented build failed"

# ── 3. train on the unit ────────────────────────────────────────────────────
ts "copying bench and instrumented core to $DEVICE:$TMP"
# shellcheck disable=SC2029
ssh "$DEVICE" "rm -rf $TMP; mkdir -p $TMP" || die "cannot create $TMP"
scp -q "$WORK/pgo/retro_bench" "$GEN_OUT/vice_x64_libretro.so" "$DEVICE:$TMP/" || die "scp failed"

STOPPED=0
restore() {
    if [ "$STOPPED" = 1 ]; then
        STOPPED=0
        ssh "$DEVICE" '/etc/init.d/arca-app start' >/dev/null 2>&1 || echo "WARNING: could not start arca-app on $IP" >&2
        ts "arca-app started"
    fi
}
trap restore EXIT

ts "stopping arca-app"
STOPPED=1
ssh "$DEVICE" '/etc/init.d/arca-app stop' >/dev/null 2>&1 || true
# shellcheck disable=SC2029
[ -z "$(ssh "$DEVICE" 'pidof respawn.sh app_launcher retro')" ] || die "launcher or retro still running after stop"

n=0
for p in "${PAIRS[@]}"; do
    n=$((n + 1))
    content=${p%%:*}; state=${p#*:}
    case $content in /*) ;; *) content="$REMOTE_DIR/$content" ;; esac
    case $state in /*) ;; *) state="$REMOTE_DIR/$state" ;; esac
    ts "pair $n/${#PAIRS[@]}: $FRAMES frames  $(basename "$content")"
    # One process at a time; the run ends by itself. GCOV_PREFIX keeps the .gcda off the read-only-ish tree.
    # shellcheck disable=SC2029
    ssh "$DEVICE" "cd $REMOTE_DIR && GCOV_PREFIX=$TMP/gcov RETRO_SYS=$REMOTE_DIR/system RETRO_STATE='$state' RETRO_OPTS='$OPTS' $TMP/retro_bench $TMP/vice_x64_libretro.so '$content' $FRAMES" \
        >"$WORK/pgo/run$n.log" 2>&1 || die "training run $n failed, see $WORK/pgo/run$n.log"
    grep -E 'unser|frames run' "$WORK/pgo/run$n.log" || true
    grep -q 'unser [0-9]* -> 1' "$WORK/pgo/run$n.log" || die "state of pair $n did not load (see $WORK/pgo/run$n.log)"
done

# ── 4. fetch the profile ────────────────────────────────────────────────────
SRC="$WORK/vice"
ts "fetching the profile"
rm -rf "$PGO_DIR"; mkdir -p "$PGO_DIR"
# shellcheck disable=SC2029
ssh "$DEVICE" "cd $TMP/gcov$SRC && tar cf - ." | tar xf - -C "$PGO_DIR" || die "could not fetch the .gcda tree"
count=$(find "$PGO_DIR" -name '*.gcda' | wc -l)
[ "$count" -gt 0 ] || die "no .gcda files came back"
ts "$count .gcda files in $PGO_DIR"
# shellcheck disable=SC2029
ssh "$DEVICE" "rm -rf $TMP" || true
restore
trap - EXIT

# ── 5. final build ──────────────────────────────────────────────────────────
ts "building vice with the profile into $OUT"
RETRO_PGO_DIR="$PGO_DIR" bash "$SCRIPT_DIR/build-cores.sh" --work "$WORK" --out "$OUT" vice || die "profile build failed"
ts "done: deploy with retro/build-and-deploy.sh $IP (or --no-build to deploy $OUT as is)"
