#!/bin/bash
# shellcheck source-path=SCRIPTDIR
# build-and-deploy.sh — deploy the retro station's libretro cores, system
# definitions and libretro system-dir config to a unit: /home/root/data/retro/cores,
# /home/root/data/retro/systems (the *.sys files) and /home/root/data/retro/system (the
# files under retro/system/, relative paths kept).
#
# ⚠️ An image and a release carry every core EXCEPT the ones in IMAGE_EXCLUDE (below), which
# stay deployable from source over SSH. This script writes only
# /home/root/data/retro/{cores,systems,system} on the unit. It never writes content/ or roms/,
# and it deletes nothing except symlinks in cores/.
#
# Usage:
#   retro/build-and-deploy.sh <ip> [--no-build] [--no-pgo] [core ...]
#   retro/build-and-deploy.sh --bundle <dir> [--no-build] [--no-pgo]
#   retro/build-and-deploy.sh --help
#     <ip>          unit IPv4 address; deploys as root over SSH key auth
#     --no-build    skip the build; deploy what is already in the out dir
#     --no-pgo      build vice plain -O2 even if a profile exists (see pgo-vice.sh)
#     core ...      build only these (default: all). Ids: ep128emu-core fuse fceumm vice mame2003-plus snes9x2002 snes9x2005 pcsx_rearmed uae4arm dosbox-pure picodrive
#     --bundle <dir>  build (unless --no-build), then stage the shipped cores, their .sys files and
#                   their system-dir files under <dir>/root/ with a declared-mode manifest, for
#                   release.sh and rootfs/commission-offline.sh. No device, no SSH. Leaves out
#                   IMAGE_EXCLUDE.
#
# Runs in WSL, because the cores are cross-built there:
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && retro/build-and-deploy.sh <ip>"
#
# Steps: build (retro/build-cores.sh, unless --no-build) -> check that every core=
# named in retro/systems/*.sys exists in the out dir -> SSH gate -> replace the test
# symlinks in cores/ (they point into retro-bench; only the links are removed, never
# their targets) -> copy every *_libretro.so from the out dir, every *.sys, and every
# file under retro/system/ (mkdir -p for its directories) -> compare md5 on the unit
# against the host.
#
# Out dir: $RETRO_OUT, default $HOME/retro-build/out (build-cores.sh's default). A
# core-restricted build leaves the other cores in the out dir, and they are deployed too.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
OUT="$(realpath -m "${RETRO_OUT:-$HOME/retro-build/out}")"
REMOTE_DIR=/home/root/data/retro

# ── what an image / release does NOT carry ──────────────────────────────────
# THE one list. Each entry is <build id>:<core file>:<system-dir subfolder or empty>. These cores
# embed ROMs with no redistribution grant (VICE: Commodore ROMs; Enterprise ep128emu-core: EXOS and
# friends, rights holder unknown; Fuse: Spectrum ROMs, verdict UNCLEAR; reasons in retro/README.md).
# --bundle drops the core, every .sys whose core= names it, and the system-dir subfolder. The SSH
# deploy path below ignores this list and deploys everything built, for a user who has the ROMs.
IMAGE_EXCLUDE=(
    "ep128emu-core:ep128emu_core_libretro.so:ep128emu"
    "fuse:fuse_libretro.so:"
    "vice:vice_x64_libretro.so:"
)

# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=../lib/arca-bundle.sh
. "$REPO_ROOT/lib/arca-bundle.sh"

usage() {
    cat <<'EOF'
Usage: retro/build-and-deploy.sh <ip> [--no-build] [--no-pgo] [core ...]
       retro/build-and-deploy.sh --bundle <dir> [--no-build] [--no-pgo]
       retro/build-and-deploy.sh --help

  <ip>          Unit IPv4 address (deploys as root over SSH key auth).
  --no-build    Skip the build; deploy what is already in the out dir
                (default ~/retro-build/out, or $RETRO_OUT).
  --no-pgo      Build vice plain -O2 even if a PGO profile exists (retro/pgo-vice.sh).
  core ...      Build only these cores (default: all). Ids: ep128emu-core fuse
                fceumm vice mame2003-plus snes9x2002 snes9x2005 pcsx_rearmed uae4arm dosbox-pure picodrive. Not allowed with --no-build.

  --bundle <dir>  Stage the shipped cores, their .sys files and system-dir files under
                <dir>/root/ with a declared-mode manifest (what release.sh and
                rootfs/commission-offline.sh read). Builds first unless --no-build.
                No device needed.

Enterprise (ep128emu-core), Spectrum (fuse) and C64 (vice) embed ROMs without a
redistribution grant: --bundle leaves them out (IMAGE_EXCLUDE in this script); the
deploy to a unit below always includes them. Writes only /home/root/data/retro/cores,
/home/root/data/retro/systems and /home/root/data/retro/system on the unit; never
content/ or roms/.

Run it in WSL: wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && retro/build-and-deploy.sh <ip>"
EOF
}

ts()  { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "[$(date '+%H:%M:%S')] ✗ $*" >&2; exit 1; }

# ── arguments ───────────────────────────────────────────────────────────────
if [ $# -eq 0 ]; then usage >&2; exit 1; fi
BUNDLE_DIR=""
if [ "$1" = "--bundle" ]; then
    BUNDLE_DIR="${2:-}"
    [ -n "$BUNDLE_DIR" ] || { usage >&2; die "--bundle needs a directory"; }
    shift 2
    IP=""
else
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -*) usage >&2; die "first argument must be the unit IP, got: $1" ;;
    esac
    IP="$1"; shift
fi

NO_BUILD=0
CORES=()
BUILD_OPTS=()
for a in "$@"; do
    case "$a" in
        --no-build) NO_BUILD=1 ;;
        --no-pgo) BUILD_OPTS+=(--no-pgo) ;;
        -h|--help) usage; exit 0 ;;
        -*) usage >&2; die "unknown option: $a" ;;
        *) CORES+=("$a") ;;
    esac
done
if [ "$NO_BUILD" -eq 1 ] && [ "${#CORES[@]}" -gt 0 ]; then
    die "core ids select what to build; they have no meaning with --no-build"
fi

if [ -n "$BUNDLE_DIR" ] && [ "${#CORES[@]}" -gt 0 ]; then die "--bundle stages every shipped core; core ids are for the SSH deploy"; fi
DEVICE="root@$IP"

# ── 1. build ────────────────────────────────────────────────────────────────
if [ "$NO_BUILD" -eq 0 ]; then
    EXCEPT_OPTS=()
    if [ -n "$BUNDLE_DIR" ]; then
        for e in "${IMAGE_EXCLUDE[@]}"; do EXCEPT_OPTS+=(--except "${e%%:*}"); done
    fi
    ts "building ${CORES[*]:-all cores} into $OUT"
    bash "$SCRIPT_DIR/build-cores.sh" --out "$OUT" "${BUILD_OPTS[@]}" "${EXCEPT_OPTS[@]}" "${CORES[@]}" \
        || die "build-cores.sh failed; nothing deployed"
else
    ts "--no-build: deploying what is in $OUT"
fi

# ── 1b. --bundle: stage the shipped subset and stop ─────────────────────────
# The core file is a shipped core when it is not in IMAGE_EXCLUDE and a .sys names it;
# a .sys ships when its core ships; a system-dir file ships unless it sits under an
# excluded core's subfolder. snes9x2005 has no .sys, so it is built but not staged.
# The cores were gated for sdiv/udiv by build-cores.sh at build time, on the unstripped
# object; the stripped copy cannot be judged again.
if [ -n "$BUNDLE_DIR" ]; then
    ex_so=(); ex_dir=()
    for e in "${IMAGE_EXCLUDE[@]}"; do
        rest="${e#*:}"; ex_so+=("${rest%%:*}"); ex_dir+=("${rest#*:}")
    done
    is_excluded_so() { local x; for x in "${ex_so[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }

    arca_bundle_init "$BUNDLE_DIR" retro || die "could not prepare $BUNDLE_DIR"
    shopt -s nullglob
    n_sys=0
    declare -A staged_so=()
    for sys in "$SCRIPT_DIR"/systems/*.sys; do
        core=$(sed -n 's/^core=//p' "$sys" | tr -d '\r')
        [ -n "$core" ] || die "$(basename "$sys") has no core= line"
        is_excluded_so "$core" && { ts "image excludes $(basename "$sys") (core $core)"; continue; }
        [ -f "$OUT/$core" ] || die "$(basename "$sys") names $core, which is not in $OUT (build it: drop --no-build)"
        arca_bundle_add "$BUNDLE_DIR" retro 644 "$sys" "$REMOTE_DIR/systems/$(basename "$sys")" \
            || die "staging failed: $(basename "$sys")"
        n_sys=$((n_sys + 1))
        if [ -z "${staged_so[$core]:-}" ]; then
            staged_so[$core]=1
            arca_bundle_add "$BUNDLE_DIR" retro 755 "$OUT/$core" "$REMOTE_DIR/cores/$core" \
                || die "staging failed: $core"
        fi
    done
    [ "${#staged_so[@]}" -gt 0 ] || die "no shipped core staged"
    if [ -d "$SCRIPT_DIR/system" ]; then
        while IFS= read -r -d '' f; do
            rel="${f#"$SCRIPT_DIR"/system/}"
            skip=0
            for d in "${ex_dir[@]}"; do
                [ -n "$d" ] && case "$rel" in "$d"/*) skip=1 ;; esac
            done
            [ "$skip" -eq 0 ] || { ts "image excludes system/$rel"; continue; }
            arca_bundle_add "$BUNDLE_DIR" retro 644 "$f" "$REMOTE_DIR/system/$rel" \
                || die "staging failed: system/$rel"
        done < <(find "$SCRIPT_DIR/system" -type f -print0 | sort -z)
    fi
    ts "Staged $(arca_bundle_finish "$BUNDLE_DIR" retro) file(s): ${#staged_so[@]} core(s), $n_sys system file(s)"
    exit 0
fi

# ── 2. local checks ─────────────────────────────────────────────────────────
shopt -s nullglob
SOS=("$OUT"/*_libretro.so)
SYSS=("$SCRIPT_DIR"/systems/*.sys)
[ "${#SOS[@]}" -gt 0 ] || die "no *_libretro.so in $OUT"
[ "${#SYSS[@]}" -gt 0 ] || die "no *.sys in $SCRIPT_DIR/systems"
# system/ is the libretro system dir: every regular file under it, relative paths kept
SYS_DIR="$SCRIPT_DIR/system"
CFGS=()
if [ -d "$SYS_DIR" ]; then
    while IFS= read -r -d '' f; do CFGS+=("$f"); done < <(find "$SYS_DIR" -type f -print0 | sort -z)
fi
for sys in "${SYSS[@]}"; do
    core=$(sed -n 's/^core=//p' "$sys" | tr -d '\r')
    [ -n "$core" ] || die "$(basename "$sys") has no core= line"
    [ -f "$OUT/$core" ] || die "$(basename "$sys") names $core, which is not in $OUT (build it: drop --no-build)"
done

# ── 3. reachability ─────────────────────────────────────────────────────────
arca_ssh_gate "$DEVICE" || die "cannot continue without SSH to $DEVICE"

# ── 4. replace the test symlinks, then copy ─────────────────────────────────
# Only the links are removed (rm on a symlink never touches its target).
ts "removing symlinks in $REMOTE_DIR/cores (test copies in retro-bench are left alone)"
# shellcheck disable=SC2029  # $REMOTE_DIR is ours, expanded on purpose
ssh "$DEVICE" "mkdir -p $REMOTE_DIR/cores $REMOTE_DIR/systems; for f in $REMOTE_DIR/cores/*; do if [ -L \"\$f\" ]; then rm -f \"\$f\"; fi; done" \
    || die "could not prepare $REMOTE_DIR on $DEVICE"

ts "copying ${#SOS[@]} core(s), ${#SYSS[@]} system file(s) and ${#CFGS[@]} system-dir file(s)"
scp -q "${SOS[@]}" "$DEVICE:$REMOTE_DIR/cores/" || die "scp of cores failed"
scp -q "${SYSS[@]}" "$DEVICE:$REMOTE_DIR/systems/" || die "scp of systems failed"
if [ "${#CFGS[@]}" -gt 0 ]; then
    cfg_dirs=()
    for f in "${CFGS[@]}"; do
        cfg_dirs+=("$REMOTE_DIR/system/$(dirname "${f#"$SYS_DIR"/}")")
    done
    # shellcheck disable=SC2029  # the quoted dir list is ours, expanded on purpose
    ssh "$DEVICE" "mkdir -p $(printf '%q ' "${cfg_dirs[@]}")" || die "could not create system dirs on $DEVICE"
    for f in "${CFGS[@]}"; do
        rel="${f#"$SYS_DIR"/}"
        scp -q "$f" "$DEVICE:$REMOTE_DIR/system/$rel" || die "scp of $rel failed"
    done
fi

# ── 5. verify: md5 on the unit against the host ─────────────────────────────
want=$(cd "$OUT" && md5sum -- *_libretro.so | sort -k2)
# shellcheck disable=SC2029
got=$(ssh "$DEVICE" "cd $REMOTE_DIR/cores && md5sum -- *_libretro.so" | sort -k2) \
    || die "could not read md5 of cores on $DEVICE"
if [ "$want" != "$got" ]; then
    echo "host:"; echo "$want"; echo "unit:"; echo "$got"
    die "md5 mismatch between $OUT and $DEVICE:$REMOTE_DIR/cores"
fi

want_sys=$(cd "$SCRIPT_DIR/systems" && md5sum -- *.sys | sort -k2)
# shellcheck disable=SC2029
got_sys=$(ssh "$DEVICE" "cd $REMOTE_DIR/systems && md5sum -- *.sys" | sort -k2) \
    || die "could not read md5 of systems on $DEVICE"
[ "$want_sys" = "$got_sys" ] || die "md5 mismatch for systems on $DEVICE"

if [ "${#CFGS[@]}" -gt 0 ]; then
    # Only the files we ship: cores keep their own files (keymaps, hiscore.dat) in the same dir.
    cfg_list=$(cd "$SYS_DIR" && find . -type f | sort | tr '\n' ' ')
    # shellcheck disable=SC2086
    want_cfg=$(cd "$SYS_DIR" && md5sum -- $cfg_list)
    # shellcheck disable=SC2029
    got_cfg=$(ssh "$DEVICE" "cd $REMOTE_DIR/system && md5sum -- $cfg_list") \
        || die "could not read md5 of system dir on $DEVICE"
    [ "$want_cfg" = "$got_cfg" ] || die "md5 mismatch for system dir on $DEVICE"
fi

ts "deployed and verified: $(echo "$want" | wc -l) core(s), $(echo "$want_sys" | wc -l) system file(s), ${#CFGS[@]} system-dir file(s) on $DEVICE"
