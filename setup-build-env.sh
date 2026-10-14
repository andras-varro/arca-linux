#!/bin/bash
# setup-build-env.sh — the one home for this repo's HOST build prerequisites.
#
# Two delivery modes, and only one of them has a toolchain. DELIVERY: someone clones the
# repo, puts a card in a reader, answers a few questions, puts the card back, and the
# device works — they may never build anything, and commissioning/commission-offline.sh
# serves them. DEVELOPMENT: we build and deploy onto an already-clean device, which is
# what deploy-all.sh does. This script exists to make the second reachable on a fresh
# machine, from one package set in one place.
#
# ⚠️ It installs HOST packages only. The CROSS-COMPILED dependencies already install
# themselves and are deliberately not listed here: scummvm-arca/build-and-deploy.sh's
# build_arm_deps fetches and builds zlib + libpng into scummvm-arca/arm-deps-softfp/,
# native_apps/build-alsa-lib.sh builds alsa-lib into native_apps/arm-deps-softfp/, and
# vnc_client/build-deps.sh does zlib / libjpeg-turbo / LibVNCServer into vnc_client/deps/.
# All three are idempotent and none needs sudo, so none belongs in an apt line.
#
# ⚠️ THE SHELL YOU RUN THIS IN IS PART OF THE MEASUREMENT. Every tool probed below is
# absent from Git Bash and present in WSL, so a sweep run in the wrong shell reports a host
# with no toolchain at all — a reading that has already been mistaken for a hard blocker on
# all building. This script refuses to run outside a Linux shell rather than print that.
#
# Run:  wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./setup-build-env.sh"
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "  ${GREEN}✓${NC} $*"; }
miss() { echo -e "  ${RED}✗${NC} $*"; }
info() { echo -e "  ${YELLOW}→${NC} $*"; }
warn() { echo -e "  ${BLUE}!${NC} $*"; }
hdr()  { echo ""; echo -e "${YELLOW}$*${NC}"; }

usage() {
    cat <<'USAGE'
Usage: ./setup-build-env.sh [--install-deps] [--scummvm] [--group <g>]...

Reports which host build prerequisites are missing, and offers to install them.
Run it from WSL (or any Linux shell); it refuses under Git Bash, where every one
of these tools is absent and the report would be about the shell, not the host.

  (no flags)       Probe and report. Offers to install only if stdin is a TTY.
  --install-deps   Install without asking. For scripted use, or a fresh clone.
  --scummvm        Also set up the upstream ScummVM tree, which is not an apt
                   package: clone it, check out the commit pinned in scummvm-arca/upstream-commit, and restore our
                   backend files. Needed only to build scummvm-arca.
  --group <g>      Probe only these groups; repeatable. Default: all four.
                     core    every component needs these
                     decode  python3 + PIL, for fb565_to_png.py screenshots
                     kmod    usb_host kernel modules only
                     check   shellcheck, for the host suites and the pre-deploy gate
  -h, --help       This text.

Exit 0 = every selected prerequisite is present. 1 = something is still missing.
2 = refused (wrong shell, bad argument, or not a Debian/Ubuntu host).

The measured inventory of this project's dev host, and the shell each claim was
measured in, live in COMMISSIONING.md -> "The dev host".
USAGE
}

DO_INSTALL=0
DO_SCUMMVM=0
SEL_GROUPS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --install-deps) DO_INSTALL=1 ;;
        --scummvm)      DO_SCUMMVM=1 ;;
        --group)        shift
                        [ "$#" -gt 0 ] || { echo "--group needs a value" >&2; exit 2; }
                        SEL_GROUPS+=("$1") ;;
        -h|--help)      usage; exit 0 ;;
        *)              echo "Unknown argument: $1" >&2; echo "" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
# ⚠️ `check` is in the default set, not opt-in. shellcheck is the tool a pre-deploy gate
# needs, and a gate that lands on a host without it cannot run — so it is a build
# prerequisite in the same sense the cross-compiler is, even though nothing links against it.
[ "${#SEL_GROUPS[@]}" -gt 0 ] || SEL_GROUPS=(core decode kmod check)

# ── the shell gate ──────────────────────────────────────────────────────────
# ⚠️ Not cosmetic, and it is why this script has an exit code of its own for it. In Git
# Bash, `gcc`, `arm-linux-gnueabihf-*`, `sfdisk`, `shellcheck` and `strings` all probe
# absent because they genuinely are not installed there — so the report reads "this host
# cannot build anything" about a host that builds everything, from WSL. Refuse instead,
# and name the shell that counts.
HOST_KERNEL="$(uname -s 2>/dev/null || echo unknown)"
case "$HOST_KERNEL" in
    Linux) ;;
    *)  hdr "Wrong shell"
        miss "uname says '$HOST_KERNEL', not Linux."
        echo ""
        echo "  Every tool this script probes is absent from Git Bash / MSYS and present"
        echo "  in WSL, so the report you would get here is about the shell, not the host."
        echo "  Re-run it there:"
        echo ""
        echo "    wsl.exe -e bash -lc \"cd /mnt/c/work/roomwizard && ./setup-build-env.sh\""
        echo ""
        exit 2 ;;
esac

# ── the package set: one table, one home ────────────────────────────────────
# group|kind|probe|package
#
# `kind` is here because these cannot all be probed the same way:
#   cmd   command -v is enough.
#   run   the tool must be EXECUTED. ⚠️ `command -v python3` SUCCEEDS in a shell where the
#         interpreter does not exist — on Windows it resolves to the App Execution Alias, a
#         real file that prints "Python was not found" and fails. The shell gate above makes
#         that particular case unreachable, but the rule is free to keep and any
#         wrapper-shimmed tool can do the same thing.
#   file  a -dev package ships headers and no binary, so there is nothing to run.
#   py    an importable module rather than a program.
#
# ⚠️ binutils-arm-linux-gnueabihf is named EXPLICITLY even though the gcc package pulls it
# in. commissioning/commission-offline.sh needs arm-linux-gnueabihf-objdump on a delivery
# host that has no compiler at all, and a missing objdump there is a refusal rather than a
# pass — so the package has to be nameable on its own.
#
# The arm-linux-gnueabi (soft-float ABI) pair builds DYNAMIC binaries: the device's own
# userspace is soft-float (loader /lib/ld-linux.so.3), so a hard-float binary can never load
# its libc, libasound or libstdc++ — so native_apps and ScummVM build with it. The gnueabihf
# set stays for vnc_client's hard-float -static build, usb_host's kernel modules and
# devmem_write, and the objdump check-arm-safe.sh and commission-offline.sh run.
packages() {
    # PACKAGES_FILE exists only so the regression can drive the probe loop over a fixture
    # table. Nothing in the shipped path sets it.
    if [ -n "${PACKAGES_FILE:-}" ]; then cat "$PACKAGES_FILE"; return; fi
    cat <<'EOF'
core|cmd|arm-linux-gnueabihf-gcc|gcc-arm-linux-gnueabihf
core|cmd|arm-linux-gnueabihf-g++|g++-arm-linux-gnueabihf
core|cmd|arm-linux-gnueabihf-objdump|binutils-arm-linux-gnueabihf
core|cmd|arm-linux-gnueabi-gcc|gcc-arm-linux-gnueabi
core|cmd|arm-linux-gnueabi-g++|g++-arm-linux-gnueabi
core|cmd|gcc|build-essential
core|cmd|make|build-essential
core|cmd|cmake|cmake
core|cmd|wget|wget
core|cmd|tar|tar
core|cmd|bzip2|bzip2
core|cmd|dash|dash
core|cmd|autoreconf|autoconf
core|cmd|aclocal|automake
core|cmd|libtoolize|libtool
core|cmd|gdbus-codegen|libglib2.0-dev-bin
decode|run|python3 --version|python3
decode|py|PIL|python3-pil
kmod|cmd|bc|bc
kmod|file|/usr/include/openssl/opensslv.h|libssl-dev
kmod|cmd|bison|bison
kmod|cmd|flex|flex
kmod|cmd|mkimage|u-boot-tools
kmod|cmd|dtc|device-tree-compiler
check|cmd|shellcheck|shellcheck
EOF
}

selected() {
    local g
    for g in "${SEL_GROUPS[@]}"; do [ "$g" = "$1" ] && return 0; done
    return 1
}

# ⚠️ The `run` branch deliberately leaves $2 unquoted: the spec IS a command line
# ("python3 --version"), and word-splitting is how it becomes argv. The disable sits in
# front of the whole function rather than that one branch, because a directive in front of a
# single `case` branch is SC1124 — and shellcheck then fails to parse the file and exits 1
# having checked NOTHING, which reads exactly like a clean run that found one problem.
# shellcheck disable=SC2086
probe() {
    case "$1" in
        cmd)  command -v "$2" >/dev/null 2>&1 ;;
        run)  $2 >/dev/null 2>&1 ;;
        file) [ -f "$2" ] ;;
        py)   python3 -c "import $2" >/dev/null 2>&1 ;;
        *)    return 1 ;;
    esac
}

# ── probe ───────────────────────────────────────────────────────────────────
hdr "Host build prerequisites — measured in $HOST_KERNEL $(uname -r 2>/dev/null)"
info "groups: ${SEL_GROUPS[*]}"
echo ""

MISSING=()
have=0
lack=0
seen=0
while IFS='|' read -r group kind spec pkg; do
    [ -n "${group:-}" ] || continue
    selected "$group" || continue
    seen=$((seen + 1))
    if probe "$kind" "$spec"; then
        ok "$(printf '%-34s' "$spec")$group"
        have=$((have + 1))
    else
        miss "$(printf '%-34s' "$spec")$group — $pkg"
        lack=$((lack + 1))
        case " ${MISSING[*]:-} " in
            *" $pkg "*) ;;
            *) MISSING+=("$pkg") ;;
        esac
    fi
done < <(packages)

# ⚠️ Zero subjects probed must not read as success. A --group typo, or a table this loop
# failed to parse, would otherwise print "0 missing" and exit 0 about a host it never
# looked at — the same shape of lie the shell gate above exists to prevent.
if [ "$seen" -eq 0 ]; then
    echo ""
    miss "no prerequisites were probed at all — groups '${SEL_GROUPS[*]}' match nothing in the table"
    echo "    valid groups: core decode kmod check"
    echo ""
    exit 2
fi

echo ""
if [ "$lack" -eq 0 ]; then
    ok "$have of $seen present, 0 missing"
else
    warn "$have of $seen present, $lack missing — ${#MISSING[@]} package(s) would fix it"
fi

# ── install ─────────────────────────────────────────────────────────────────
install_missing() {
    # apt-only, with a clean refusal on anything else rather than a guess at the package
    # manager. A wrong dnf/pacman line that half-succeeds is worse than a refusal that
    # names the package set and lets the operator translate it.
    if ! command -v apt-get >/dev/null 2>&1; then
        hdr "Not a Debian/Ubuntu host"
        miss "apt-get not found, so nothing will be installed from here."
        echo ""
        echo "  Install the equivalents of these by hand:"
        printf '    %s\n' "${MISSING[@]}"
        echo ""
        return 2
    fi

    # Print the exact command before running it. Nothing is installed that the operator
    # has not seen spelled out first.
    hdr "Installing"
    echo "  sudo apt-get install -y ${MISSING[*]}"
    echo ""
    sudo apt-get install -y "${MISSING[@]}" \
        || { miss "apt-get failed — nothing further attempted"; return 1; }
    return 0
}

RC=0
if [ "$lack" -gt 0 ]; then
    if [ "$DO_INSTALL" -eq 1 ]; then
        install_missing || RC=$?
    elif [ -t 0 ]; then
        # ⚠️ TTY-only. A blocking `read` here would hang release.sh and deploy-all.sh, which
        # invoke the component scripts non-interactively; --install-deps is the scripted door.
        echo ""
        read -r -p "  Install the ${#MISSING[@]} missing package(s) now? [y/N]: " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) install_missing || RC=$? ;;
            *) info "Nothing installed. Re-run with --install-deps, or:"
               echo "    sudo apt-get install -y ${MISSING[*]}"
               RC=1 ;;
        esac
    else
        info "Not a TTY and --install-deps not given. Nothing installed:"
        echo "    sudo apt-get install -y ${MISSING[*]}"
        RC=1
    fi
fi

# ── the ScummVM half, which is not apt ──────────────────────────────────────
# ⚠️ This is the actual blocker on a fresh clone, and an installer that skipped it would
# not have solved the problem it exists to solve. The upstream tree at the repo root is
# gitignored (/scummvm/ in .gitignore), so a clone has no scummvm/ at all and
# scummvm-arca/build-and-deploy.sh refuses. vkeybd_arca.zip and scummvm.ppm ARE
# tracked, so those do come with the clone.
#
# Opt-in rather than part of the default run because it is a large clone over the network,
# and the other three components do not need it.
setup_scummvm() {
    hdr "ScummVM upstream tree"
    local dir="$REPO/scummvm"

    command -v git >/dev/null 2>&1 || { miss "git not found"; return 1; }

    if [ -d "$dir/.git" ]; then
        ok "present at $dir"
    else
        info "cloning ScummVM into $dir — this is large"
        echo "    git clone https://github.com/scummvm/scummvm.git $dir"
        git clone https://github.com/scummvm/scummvm.git "$dir" \
            || { miss "clone failed"; return 1; }
    fi

    # The commit is data in one file, shared with scummvm-arca/build-and-deploy.sh, which
    # refuses any other HEAD: the source offered with a card image must be what was built.
    local pin
    pin="$(sed -n 's/^\([0-9a-f]\{40\}\)$/\1/p' "$REPO/scummvm-arca/upstream-commit" 2>/dev/null | head -n 1)"
    [ -n "$pin" ] || { miss "no 40-hex commit in scummvm-arca/upstream-commit"; return 1; }
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$pin" ]; then
        info "git checkout --detach $pin"
        git -C "$dir" checkout --detach "$pin" || { miss "checkout of the pinned commit failed"; return 1; }
    fi
    ok "at the pinned commit $(git -C "$dir" rev-parse --short HEAD 2>/dev/null || echo '?')"

    # Our backend is not in upstream. `restore` copies backend-files/ into the tree and
    # sets the skip-worktree bits that keep our edits out of upstream's git status.
    info "restoring the Arca backend into the tree"
    ( cd "$REPO/scummvm-arca" && bash manage-scummvm-changes.sh restore ) \
        || { miss "manage-scummvm-changes.sh restore failed"; return 1; }
    ok "backend restored"
    return 0
}

if [ "$DO_SCUMMVM" -eq 1 ]; then
    setup_scummvm || RC=1
elif [ ! -d "$REPO/scummvm/.git" ]; then
    hdr "ScummVM upstream tree"
    warn "absent — scummvm-arca cannot build. Pass --scummvm to set it up."
    info "The other three components do not need it."
fi

echo ""
exit "$RC"
