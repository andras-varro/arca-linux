#!/bin/bash
# Reproducible build of the libretro cores we run on the Cortex-A8 device.
#
# Each core: fresh clone (or reuse of its own work-dir clone), checkout of the
# pinned commit, `git clean -fdx`, apply retro/patches/<core>.patch if present,
# cross-build with the ARM soft-float toolchain, strip into the out dir, then
# gate the result with native_apps/check-arm-safe.sh (hard zero sdiv/udiv).
#
# Runs inside WSL. Never builds in ~/retro-src (other workers' clones).
#
# Usage:
#   retro/build-cores.sh [--out DIR] [--work DIR] [--pgo-gen|--no-pgo] [core ...]
#     --out DIR   output directory for the stripped .so files (default ~/retro-build/out)
#     --work DIR  clone/build area, one subdirectory per core (default ~/retro-build)
#     core ...    any of: ep128emu-core fuse fceumm vice mame2003-plus snes9x2002 snes9x2005 pcsx_rearmed uae4arm dosbox-pure (default: all)
#     --pgo-gen   vice only: instrumented build (-fprofile-generate), used by retro/pgo-vice.sh
#     --no-pgo    vice: ignore the profile, build plain -O2
#   A vice build is profile-guided (-fprofile-use) when $RETRO_PGO_DIR (default
#   ~/retro-build/pgo/vice, written by retro/pgo-vice.sh) holds .gcda files, else plain -O2.
#
# Pins are the HEAD commits of the hand-made clones in ~/retro-src, each on
# its upstream libretro branch.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd -P)"

# ── Table: core key, upstream URL, pinned commit, extra make args, output .so ──
CORES=(ep128emu-core fuse fceumm vice mame2003-plus snes9x2002 snes9x2005 pcsx_rearmed uae4arm dosbox-pure)

declare -A CORE_URL=(
    [ep128emu-core]=https://github.com/libretro/ep128emu-core
    [fuse]=https://github.com/libretro/fuse-libretro
    [fceumm]=https://github.com/libretro/libretro-fceumm
    [vice]=https://github.com/libretro/vice-libretro
    [mame2003-plus]=https://github.com/libretro/mame2003-plus-libretro
    [snes9x2002]=https://github.com/libretro/snes9x2002
    [snes9x2005]=https://github.com/libretro/snes9x2005
    [pcsx_rearmed]=https://github.com/libretro/pcsx_rearmed
    [uae4arm]=https://github.com/libretro/uae4arm-libretro
    [dosbox-pure]=https://codeberg.org/schelling/dosbox-pure
)
declare -A CORE_COMMIT=(
    [ep128emu-core]=6af5d03de220e6effcd0ffcd99e0fb8e8c77cc9e
    [fuse]=e997e2bc32c888348f862f69f2c53babfedf7791
    [fceumm]=ac0d00a7fc01166f84c7bac01d1084ebfd2b865e
    [vice]=f63b56688f3133a2bb17499db9eebb7ab81df5f5
    [mame2003-plus]=505733a2df272a0eaa46c143628bd30d6fafb232
    [snes9x2002]=6ffbf9ef4f0063e1f1b78a40d10c50fc52f2524c
    [snes9x2005]=a79dfe9047e7fec58808aefe48ad2bf499c7af11
    [pcsx_rearmed]=c8816799b50388e61cfe237fe2cdbb7d8175f20a
    [uae4arm]=26f444f01771f0dfa2fa652906ff8222e92e476e
    [dosbox-pure]=d9c1356a6376fc35bd9a900b46c4666b3f4500e6
)
# Extra make variables beyond platform=unix and the toolchain.
declare -A CORE_MAKE_EXTRA=(
    [ep128emu-core]=""
    [fuse]=""
    [fceumm]=""
    [vice]="EMUTYPE=x64"
    [mame2003-plus]=""
    [snes9x2002]=""
    [snes9x2005]=""
    [pcsx_rearmed]="-f Makefile.libretro"
    [uae4arm]="-f Makefile.libretro HAVE_NEON=1 USE_PICASSO96=1 MORE_CFLAGS=-DARMV6T2 CPU_FLAGS=-marm CAPS= EXTRA_INCLUDES=-I./deps"
    [dosbox-pure]="STRIPCMD=true"
)
declare -A CORE_SO=(
    [ep128emu-core]=ep128emu_core_libretro.so
    [fuse]=fuse_libretro.so
    [fceumm]=fceumm_libretro.so
    [vice]=vice_x64_libretro.so
    [mame2003-plus]=mame2003_plus_libretro.so
    [snes9x2002]=snes9x2002_libretro.so
    [snes9x2005]=snes9x2005_libretro.so
    [pcsx_rearmed]=pcsx_rearmed_libretro.so
    [uae4arm]=uae4arm_libretro.so
    [dosbox-pure]=dosbox_pure_libretro.so
)

# ── Toolchain (same as ~/retro-src/env.sh, kept here so the script is self-contained)
TC=arm-linux-gnueabi
ARMFLAGS="-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp"
CC="$TC-gcc $ARMFLAGS"
CXX="$TC-g++ $ARMFLAGS"
AR="$TC-ar"
STRIP="$TC-strip"

usage() {
    sed -n "2,22p" "$0" | sed 's/^# \{0,1\}//'
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

OUT="$HOME/retro-build/out"
WORK="$HOME/retro-build"
PGO_MODE=auto   # auto: use the profile if there is one; gen: instrument; off: never
PGO_DIR="${RETRO_PGO_DIR:-$HOME/retro-build/pgo/vice}"
SEL=()
while [ $# -gt 0 ]; do
    case $1 in
        --out)  OUT=${2:?--out needs a directory}; shift 2 ;;
        --work) WORK=${2:?--work needs a directory}; shift 2 ;;
        --pgo-gen) PGO_MODE=gen; shift ;;
        --no-pgo)  PGO_MODE=off; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            [[ -n "${CORE_URL[$1]:-}" ]] || die "unknown core '$1' (known: ${CORES[*]})"
            SEL+=("$1"); shift ;;
    esac
done
if [ ${#SEL[@]} -eq 0 ]; then
    SEL=("${CORES[@]}")
fi

if [ "$PGO_MODE" = gen ] && { [ ${#SEL[@]} -ne 1 ] || [ "${SEL[0]}" != vice ]; }; then die "--pgo-gen needs exactly the core: vice"; fi
command -v "$TC-gcc" >/dev/null || die "$TC-gcc not found (run inside WSL with the ARM toolchain)"
command -v git >/dev/null || die "git not found"

OUT=$(realpath -m "$OUT")
WORK=$(realpath -m "$WORK")

# Guards (before anything is created): outputs never inside the repo; never build in the hand-made clones.
case "$OUT/" in "$REPO"/*) die "--out must not be inside the repo ($REPO)" ;; esac
case "$WORK/" in "$REPO"/*) die "--work must not be inside the repo ($REPO)" ;; esac
case "$WORK/" in "$HOME/retro-src"/*) die "--work must not be ~/retro-src (other workers' clones)" ;; esac

mkdir -p "$OUT" "$WORK"

JOBS=$(nproc)

build_core() {
    local core=$1
    local url=${CORE_URL[$core]}
    local commit=${CORE_COMMIT[$core]}
    local so=${CORE_SO[$core]}
    local extra=${CORE_MAKE_EXTRA[$core]}
    local src="$WORK/$core"
    local patch="$REPO/retro/patches/$core.patch"
    local dest="$OUT/$so"
    local blog="$WORK/$core.build.log"
    local glog="$WORK/$core.armsafe.log"

    echo "== $core  $url @ $commit"

    if [ -e "$src" ]; then
        [ -d "$src/.git" ] || die "$src exists and is not a git clone; remove it or pick --work"
        [ "$(git -C "$src" remote get-url origin)" = "$url" ] || die "$src has a different origin"
        if ! git -C "$src" cat-file -e "$commit^{commit}" 2>/dev/null; then
            git -C "$src" fetch -q origin
        fi
        git -C "$src" checkout -f -q "$commit"
    else
        git clone -q "$url" "$src"
        git -C "$src" checkout -f -q "$commit"
    fi
    git -C "$src" clean -fdxq
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || die "$core: HEAD is not $commit"

    if [ -f "$patch" ]; then
        git -C "$src" apply "$patch"
        echo "   applied $(basename "$patch")"
    else
        echo "   no patch (retro/patches/$core.patch absent)"
    fi

    # Profile-guided vice. The flags ride on CC/CXX (compile and link both see them; gen needs
    # -fprofile-generate at link time or dlopen fails on __gcov_indirect_call). The profile is a
    # tree of .gcda files relative to the source root; gcc finds them next to the objects, so
    # they are copied in after the clean + patch, and the tree must sit at the path the
    # instrumented build used (--work). A source that no longer matches the profile is a gcc error.
    local cc=$CC cxx=$CXX
    if [ "$core" = vice ]; then
        local have=0
        if [ -d "$PGO_DIR" ] && [ -n "$(find "$PGO_DIR" -name '*.gcda' -print -quit)" ]; then have=1; fi
        if [ "$PGO_MODE" = gen ]; then
            cc="$CC -fprofile-generate"; cxx="$CXX -fprofile-generate"
            echo "   PGO: instrumented build (-fprofile-generate)"
        elif [ "$PGO_MODE" = auto ] && [ "$have" = 1 ]; then
            cp -R "$PGO_DIR"/. "$src"/
            cc="$CC -fprofile-use -fprofile-correction -Wno-missing-profile"
            cxx="$CXX -fprofile-use -fprofile-correction -Wno-missing-profile"
            echo "   PGO: -fprofile-use, profile $PGO_DIR"
        else
            echo "   PGO: none (plain -O2)"
        fi
    fi
    local make_vars=("platform=unix" "CC=$cc" "CXX=$cxx" "AR=$AR")

    # shellcheck disable=SC2086  # $extra is a space-separated list of VAR=value words
    (cd "$src" && make -j"$JOBS" "${make_vars[@]}" $extra) >"$blog" 2>&1 \
        || die "$core: make failed, see $blog"
    [ -f "$src/$so" ] || die "$core: $so not produced, see $blog"

    # The gate reads symbols (Thumb vs ARM). Some Makefiles link with -s (ep128emu does),
    # so the gate runs on every object the build produced, not on the .so.
    local objs=()
    mapfile -d '' objs < <(find "$src" -name '*.o' -print0)
    [ ${#objs[@]} -gt 0 ] || die "$core: no object files produced"
    if ! bash "$REPO/native_apps/check-arm-safe.sh" "${objs[@]}" >"$glog" 2>&1 \
        || ! grep -q 'unverified=0 bad=0' "$glog"; then
        cat "$glog" >&2
        die "$core: ARM gate FAILED or unverified (see $glog), nothing copied to $OUT"
    fi

    rm -f "$dest"
    "$STRIP" --strip-unneeded -o "$dest" "$src/$so"

    local bytes
    bytes=$(stat -c %s "$dest")
    printf 'RESULT %-16s built  arm-safe PASS  %9s bytes  %s\n' "$core" "$bytes" "$dest"

    if [ "$core" = vice ]; then
        echo "WARNING: $dest embeds Commodore ROMs. It is for on-device testing only; never ship it." >&2
    fi
}

for c in "${SEL[@]}"; do
    build_core "$c"
done

echo "done: ${#SEL[@]} core(s) in $OUT"
