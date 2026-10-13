#!/bin/bash
#
# commissioning/commission-offline.sh — the IMAGE STEP: install a release bundle and the
#                         provision plan into a card's partitions, mounted by the caller.
#
# The only caller is rootfs/make-card-image.sh --bundle, which loop-mounts the four
# partitions of a card IMAGE it is building (our own Buildroot root on p6, marker
# etc/arca-rootfs) and hands them over with --base. Nothing here reads a
# terminal, scans a disk or mounts anything; it needs no root of its own and no network.
# A unit already in service is updated online with commissioning/provision.sh.
#
# Usage:
#   ./commissioning/commission-offline.sh --bundle <file.tar.gz|dir> --base <dir> [options]
#   ./commissioning/commission-offline.sh --bundle <b> --base <dir> --dry-run
#
#   --bundle <path>     A release tarball from `./release.sh --stage-only`, or a staged
#                       bundle directory. THE source of binaries: this host has no
#                       toolchain to fall back on.
#   --base <dir>        REQUIRED. The card, already mounted as <dir>/{root,data,log}.
#   --dry-run           Print every resolved absolute path and change nothing.
#   --no-<group>        Skip one group of the provision plan (see --help).
#   --arm-check=skip    Proceed with UNVERIFIED binaries when the ARM objdump is
#                       absent. Read what it prints before you use it.
#
# REFUSED, by name and with a non-zero exit, because the card-update path they belonged to
# is gone and a flag accepted and ignored reads as a choice honoured: --no-clean,
# --keep-<group>, --delete-factory, --unattended, --disk, --release, --ssh-auth=*,
# --sshd-only.
#
# ── What this script does NOT reimplement ──────────────────────────────────
#
# Nothing here restates a decision that lives somewhere else:
#
#   which mount is which       lib/arca-identify.sh, by content and by POSITION, never
#                              by UUID. p1 is not reachable from there at all.
#   what to install            device-files/provision-rules.conf — the boot scripts,
#                              the rc*.d links and the config fix-ups, shared with
#                              commissioning/provision.sh so the two cannot drift.
#                              Modes are DECLARED there, never read off disk.
#   which binaries             the bundle's own manifest (lib/arca-bundle.sh). Modes are
#                              DECLARED there too.
#
# Host name, password and ssh identity are not set here: they are whatever the caller's
# p6 already carries.

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=../lib/arca-identify.sh
. "$REPO_ROOT/lib/arca-identify.sh"
# shellcheck source=../lib/arca-provision.sh
. "$REPO_ROOT/lib/arca-provision.sh"
# shellcheck source=../lib/arca-bundle.sh
. "$REPO_ROOT/lib/arca-bundle.sh"

DEVICE_FILES="$REPO_ROOT/device-files"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓ $*${NC}"; }
info() { echo -e "${YELLOW}  → $*${NC}"; }
warn() { echo -e "${BLUE}  ! $*${NC}"; }
err()  { echo -e "${RED}  ✗ $*${NC}" >&2; cleanup_and_exit 1; }

BUNDLE=""
BASE=""
DRY=""
NO_PROV_GROUPS=""
ARM_CHECK="require"
# Set when the ARM gate could not judge everything it was given, so the closing
# summary reports it as a caveat rather than under a green tick.
ARM_TRUSTED=0

# State the exit path needs. Set before anything can fail, and used by err() above —
# which is why it is declared up here even though nothing reads it yet.
TMPROOT=""

cleanup_and_exit() {
    local code="${1:-0}"
    [ -n "$TMPROOT" ] && rm -rf "$TMPROOT"
    exit "$code"
}
trap 'cleanup_and_exit 1' INT TERM

usage() {
    cat <<USAGE
Usage: $0 --bundle <file.tar.gz|dir> --base <dir> [options]

The image step of rootfs/make-card-image.sh --bundle. Asks nothing, mounts nothing,
needs no network. A unit in service is updated with commissioning/provision.sh.

  --bundle <path>    Release tarball or staged bundle directory (REQUIRED)
  --base <dir>       The card, already mounted as <dir>/{root,data,log}
                     (REQUIRED). p6 must carry our root's marker, etc/arca-rootfs.
  --dry-run          Resolve and print everything, change nothing.
  --no-<group>       Skip one group of the provision plan. Groups:
                     $(arca_provision_optional_groups)
                     --no-mdns leaves <name>.local unresolvable. The reason for each
                     is in device-files/provision-rules.conf.
  --arm-check=skip   Install binaries this host cannot verify. Say why to
                     yourself first; the message it replaces explains the risk.
  --help

Refused with an error, being retired with the card-update path: --no-clean,
--keep-<group>, --delete-factory, --unattended, --disk, --release, --ssh-auth=*,
--sshd-only.

p1 holds mlo, u-boot.bin, ctrlblock.bin and uImage-system, and is never mounted
or written.
USAGE
    exit 1
}

# retired FLAG REASON — print and exit 1. Not usage(): the operator named something
# specific, and the answer is about that.
retired() {
    echo "$1: removed. $2"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundle)         BUNDLE="${2:-}"; [[ -n "$BUNDLE" ]] || { echo "--bundle needs a value"; usage; }; shift 2 ;;
        --base)           BASE="${2:-}";   [[ -n "$BASE" ]]   || { echo "--base needs a value"; usage; };   shift 2 ;;
        --dry-run)        DRY=1; shift ;;
        --arm-check=skip) ARM_CHECK="skip"; shift ;;
        --no-clean)       retired "$1" "This script never cleans: it installs into our own root, which carries no vendor stack." ;;
        --keep-*|--delete-factory)
                          retired "$1" "There is no clean here, so there is nothing to keep or delete." ;;
        --unattended)     retired "$1" "It is always unattended now: this script asks nothing." ;;
        --disk|--disk=*)  retired "${1%%=*}" "It no longer finds or mounts a card; the caller mounts the four partitions and passes --base." ;;
        --release|--release=*)
                          retired "${1%%=*}" "It no longer fetches a release; pass the tarball with --bundle." ;;
        --ssh-auth=*|--sshd-only)
                          retired "$1" "sshd_config is the root image's overlay alone (key-only); this script never edits it." ;;
        --no-*)
            g="${1#--no-}"
            case " $(arca_provision_optional_groups) " in
                *" $g "*) NO_PROV_GROUPS="$NO_PROV_GROUPS $g" ;;
                *) echo "Unknown provision group: $g"; echo "  --no- accepts: $(arca_provision_optional_groups)"; exit 1 ;;
            esac
            shift ;;
        --help|-h) usage ;;
        *) echo "Unknown option: $1"; echo ""; usage ;;
    esac
done

[[ -n "$BUNDLE" ]] || { echo "A bundle is required — this host has no toolchain to fall back on."; echo ""; usage; }
[[ -e "$BUNDLE" ]] || { echo "No such bundle: $BUNDLE"; exit 1; }
[[ -n "$BASE" ]] || { echo "--base is required: the caller mounts the card and names it."; echo ""; usage; }

echo ""
echo "════════════════════════════════════════"
echo " Arca image step"
echo "════════════════════════════════════════"
[[ -z "$DRY" ]] || warn "DRY RUN — every path is resolved and printed; nothing is written."

# ── 1. the mounted card ─────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────────────"
echo " 1. The card"
echo "────────────────────────────────────────"

BASE="${BASE%/}"
[[ -d "$BASE" ]] || err "$BASE is not a directory"
# ⚠️ The negative half is the half that earns its keep: a rootfs where p2 was
# expected means the partitions are in the wrong order, which would make every
# path below resolve under the wrong tree.
if ! CHECK="$(arca_check_card_mounts "$BASE")"; then
    echo "$CHECK"
    err "the three mounts do not look right"
fi
# Our own root only (arca_is_rootfs requires the marker too; this names it in the refusal). A vendor tree
# would have its software left running beside ours, since nothing cleans one any more.
[[ -f "$BASE/root/$ARCA_ROOTFS_OURS" ]] \
    || err "$BASE/root has no /$ARCA_ROOTFS_OURS — this installs into OUR root filesystem (rootfs/) only"
ok "p6 is our root ($(head -1 "$BASE/root/$ARCA_ROOTFS_OURS")); p2/p3 are not rootfs trees"
arca_is_rootfs_writable "$BASE/root" || err "$BASE/root is mounted read-only"

# ── 2. the bundle ───────────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────────────"
echo " 2. The bundle"
echo "────────────────────────────────────────"

if [[ -d "$BUNDLE" ]]; then
    BUNDLE_DIR="$(cd "$BUNDLE" && pwd)"
    info "Staged bundle directory: $BUNDLE_DIR"
else
    TMPROOT=$(mktemp -d /tmp/arca-bundle.XXXXXX)
    BUNDLE_DIR="$TMPROOT/bundle"
    mkdir -p "$BUNDLE_DIR"
    info "Unpacking $BUNDLE"
    tar -xzf "$BUNDLE" -C "$BUNDLE_DIR" || err "could not unpack $BUNDLE"
fi

if ! CHECK="$(arca_bundle_check "$BUNDLE_DIR")"; then
    echo "$CHECK"
    err "the bundle is not self-consistent — do not install it"
fi
ok "Every manifest entry is staged, and every staged file is in a manifest"

[[ -f "$BUNDLE_DIR/manifest.d/bundle.info" ]] && sed 's/^/    /' "$BUNDLE_DIR/manifest.d/bundle.info"
BUNDLE_FILES=$(arca_bundle_entries "$BUNDLE_DIR" | grep -c . || true)
[[ "$BUNDLE_FILES" -gt 0 ]] || err "the bundle contains no files at all"
info "$BUNDLE_FILES file(s), components: $(arca_bundle_components "$BUNDLE_DIR" | tr '\n' ' ')"

# ── ARM safety, on the DOWNLOADED binaries ──────────────────────────────────
#
# A binary nobody built on the spot is exactly what check-arm-safe.sh is for: the
# Cortex-A8 has no hardware integer divide, and an sdiv/udiv INSTRUCTION dies with
# SIGILL — blank screen, no output, no log, indistinguishable from "the app didn't
# start". So the gate runs here too, over the bundle rather than over build/.
#
# ⚠️ The count is asserted, not the exit status alone. check-arm-safe.sh skips
# non-ARM files and then reports success — "no hardware divide in 0 binaries" is a
# pass over nothing, and that is the failure this block exists to make impossible.
info "Checking the bundle's ARM binaries..."
ELF_LIST="$TMPROOT/elf.list"
[[ -n "$TMPROOT" ]] || { TMPROOT=$(mktemp -d /tmp/arca-bundle.XXXXXX); ELF_LIST="$TMPROOT/elf.list"; }
: > "$ELF_LIST"
while read -r _mode dev; do
    [[ -n "$dev" ]] || continue
    f="$BUNDLE_DIR/root$dev"
    [[ -f "$f" ]] || continue
    # ELF magic read directly. `file` is not guaranteed present and a .app or a
    # .ppm must not be handed to objdump — skipping what a tool cannot inspect,
    # rather than passing it through, is what keeps the count honest.
    [[ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" == "7f454c46" ]] || continue
    printf '%s\n' "$f" >> "$ELF_LIST"
done < <(arca_bundle_entries "$BUNDLE_DIR")

ELF_COUNT=$(grep -c . "$ELF_LIST" || true)
[[ "$ELF_COUNT" -gt 0 ]] || err "the bundle contains no ELF binaries — it cannot be an Arca app bundle"

OBJDUMP="${OBJDUMP:-arm-linux-gnueabihf-objdump}"
if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    echo ""
    echo -e "${RED}  ╔════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}  ║  $OBJDUMP IS NOT INSTALLED.${NC}"
    echo -e "${RED}  ║${NC}"
    echo -e "${RED}  ║  $ELF_COUNT ARM binaries in this bundle were NOT CHECKED for the${NC}"
    echo -e "${RED}  ║  Cortex-A8 hardware-divide instruction. If one carries an sdiv or${NC}"
    echo -e "${RED}  ║  udiv, the app dies with SIGILL the moment it is tapped: black${NC}"
    echo -e "${RED}  ║  screen, no output, no log, nothing in dmesg you would look for.${NC}"
    echo -e "${RED}  ║${NC}"
    echo -e "${RED}  ║  Fix it:   sudo apt install binutils-arm-linux-gnueabihf${NC}"
    echo -e "${RED}  ║  Or run:   native_apps/check-arm-safe.sh <files>  on a build host${NC}"
    echo -e "${RED}  ║  Or force: --arm-check=skip${NC}"
    echo -e "${RED}  ╚════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    [[ "$ARM_CHECK" == "skip" ]] || err "refusing to install unverified ARM binaries"
    warn "--arm-check=skip given: installing $ELF_COUNT UNVERIFIED binaries"
    ARM_VERIFIED="NOT CHECKED — $OBJDUMP absent"
    ARM_TRUSTED=1
else
    # ⚠️ NOT xargs: it maps any command exit of 1–125 onto its own 123, which
    # erases the difference between "a real hit" (1) and "could not judge" (2) —
    # the whole distinction this block turns on. mapfile handles both things xargs
    # was here for: an arbitrarily long list, and paths containing spaces.
    #
    # ⚠️ Exit 2 is NOT a failure and must not be treated as one. It means some of
    # the bundle's binaries are stripped, and the gate refuses to invent a verdict
    # for those — objdump reads Thumb-2 as ARM without a symbol table and reports
    # divides that are not in the file. scummvm and
    # vnc_client both ship stripped, so every full bundle takes this path; treating
    # 2 as fatal refused all of them, and --arm-check=skip could not override it
    # because that flag lives in the objdump-absent branch above.
    ARM_LOG="$TMPROOT/arm-check.log"
    mapfile -t ARM_TARGETS < "$ELF_LIST"
    if bash "$REPO_ROOT/native_apps/check-arm-safe.sh" "${ARM_TARGETS[@]}" \
             > "$ARM_LOG" 2>&1; then
        arm_rc=0
    else
        arm_rc=$?
    fi
    cat "$ARM_LOG"

    # The counts come off the gate's own machine-readable last line, so this block
    # cannot disagree with what the gate printed directly above it.
    ARM_SUM="$(grep -o 'ARM-SUMMARY .*' "$ARM_LOG" | tail -1)"
    ARM_OK="$(printf '%s' "$ARM_SUM"  | sed -n 's/.*checked=\([0-9]*\).*/\1/p')"
    ARM_UNV="$(printf '%s' "$ARM_SUM" | sed -n 's/.*unverified=\([0-9]*\).*/\1/p')"

    case "$arm_rc" in
    0)  ARM_VERIFIED="$ELF_COUNT binaries, hard zero" ;;
    2)  echo ""
        echo -e "${YELLOW}  ╔════════════════════════════════════════════════════════════════╗${NC}"
        echo -e "${YELLOW}  ║  ${ARM_UNV:-some} OF ${ELF_COUNT} BUNDLED BINARIES COULD NOT BE CHECKED.${NC}"
        echo -e "${YELLOW}  ║${NC}"
        echo -e "${YELLOW}  ║  They are stripped, and the hardware-divide check is meaningless${NC}"
        echo -e "${YELLOW}  ║  on a stripped binary — it would report divides that are not in${NC}"
        echo -e "${YELLOW}  ║  the file. Refusing on that basis would refuse every bundle that${NC}"
        echo -e "${YELLOW}  ║  contains scummvm or vnc_client, so the install continues.${NC}"
        echo -e "${YELLOW}  ║${NC}"
        echo -e "${YELLOW}  ║  What that costs you: if one of those ${ARM_UNV:-n} binaries does carry an${NC}"
        echo -e "${YELLOW}  ║  sdiv/udiv, it will SIGILL when tapped — blank screen, no log.${NC}"
        echo -e "${YELLOW}  ║  A bundle from this repo's release.sh was gated at BUILD time, on${NC}"
        echo -e "${YELLOW}  ║  the unstripped artifact, which is the only sound moment. A bundle${NC}"
        echo -e "${YELLOW}  ║  from anywhere else is taken on trust here.${NC}"
        echo -e "${YELLOW}  ╚════════════════════════════════════════════════════════════════╝${NC}"
        echo ""
        ARM_VERIFIED="${ARM_OK:-?} verified, ${ARM_UNV:-?} stripped and TAKEN ON TRUST"
        ARM_TRUSTED=1 ;;
    *)  err "a bundled binary would SIGILL on this device — do not install it" ;;
    esac
fi

# ── 3. install ──────────────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────────────"
echo " 3. Install"
echo "────────────────────────────────────────"

# put MODE DEVICE_PATH SOURCE
#
# One writer for everything installed, so the mode is applied in exactly one
# place. The mode is DECLARED by the caller and never read off disk: /mnt/c is
# DrvFs, reports every file 0777 and discards chmod, so a mode derived from the
# source here would be a constant rather than a measurement (CLAUDE.md).
INSTALLED=()
put() {
    local mode="$1" dev="$2" src="$3" dest
    dest=$(arca_offline_path "$BASE" "$dev") || { err "cannot resolve $dev"; }
    case "$dest/" in
        "${BASE%/}"/*) ;;
        *) err "$dev resolved to $dest, outside $BASE — refusing" ;;
    esac
    if [[ -n "$DRY" ]]; then
        printf '  would write   %-5s %s\n' "$mode" "$dest"
        return 0
    fi
    mkdir -p "$(dirname "$dest")"
    cp "$src" "$dest"
    chmod "$mode" "$dest"
    INSTALLED+=("$mode|$dev|$dest")
}

# ── 3a. the provision plan: boot scripts, links, config edits ─────────
#
# ⚠️ The decisions are NOT here. Every file, link, mode and config edit lives in
# device-files/provision-rules.conf with a reason per entry, read by BOTH this
# script and commissioning/provision.sh, so the two cannot drift.
# They HAD drifted: the online path removed stale rc*.d links before relinking and
# this one did not, so a card carrying an old S50arca-app came out of offline
# commissioning with two links to one init script at two priorities.
PROV_RULES="$DEVICE_FILES/provision-rules.conf"
[[ -f "$PROV_RULES" ]] || err "missing $PROV_RULES"
if ! PCHECK="$(arca_provision_validate "$PROV_RULES" "$REPO_ROOT")"; then
    echo "$PCHECK"
    err "device-files/provision-rules.conf does not validate — refusing to install"
fi

PROV_GROUPS="base"
for g in $(arca_provision_optional_groups); do
    case " $NO_PROV_GROUPS " in
        *" $g "*) ;;
        *) PROV_GROUPS="$PROV_GROUPS $g" ;;
    esac
done
[[ -n "$NO_PROV_GROUPS" ]] && info "Skipping:$NO_PROV_GROUPS"

PROV_PLAN="$TMPROOT/provision.plan"
[[ -n "$TMPROOT" ]] || { TMPROOT=$(mktemp -d /tmp/arca-bundle.XXXXXX); PROV_PLAN="$TMPROOT/provision.plan"; }
arca_provision_plan "$PROV_RULES" "$PROV_GROUPS" > "$PROV_PLAN" \
    || err "could not compile the provision plan"
info "Provision plan: $(arca_provision_plan_summary "$PROV_PLAN")"

if ! ARCA_PROVISION_DRY="$DRY" arca_provision_apply_offline "$BASE" "$PROV_PLAN" "$REPO_ROOT"; then
    err "the provision step failed"
fi

# Feed the plan's declared modes into the +x measurement below. Reading them from
# the plan rather than having the executor export an array keeps one authority for
# "what mode was declared" — the data file.
if [[ -z "$DRY" ]]; then
    while IFS=$'\t' read -r pkind pmode ptarget psrc; do
        case "$pkind" in install) ;; *) continue ;; esac
        pdest=$(arca_offline_path "$BASE" "$ptarget") || continue
        INSTALLED+=("$pmode|$ptarget|$pdest")
    done < "$PROV_PLAN"
fi
ok "Boot scripts, boot links and the config fix-ups done"

# ── 3b. the bundle ─────────────────────────────────────────────────────────
info "Bundle: $BUNDLE_FILES file(s)"
while read -r mode dev; do
    [[ -n "$dev" ]] || continue
    put "$mode" "$dev" "$BUNDLE_DIR/root$dev"
done < <(arca_bundle_entries "$BUNDLE_DIR")
ok "Installed"

# ── 4. verify ────────────────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────────────"
echo " 4. Verify"
echo "────────────────────────────────────────"

if [[ -n "$DRY" ]]; then
    warn "Dry run: nothing was written, so there is nothing to verify."
    cleanup_and_exit 0
fi

VBAD=0
vfail() { VBAD=$((VBAD + 1)); echo -e "${RED}  ✗ $*${NC}"; }

# ── md5, against the bundle's own manifest, reading the INSTALLED file ─────
# The point is the bytes that landed on the card, not the bytes that were staged:
# a truncated write on a full or failing card is exactly what this catches, and it
# reports success just as loudly if you check the source instead.
MD5_CHECKED=0
for m in "$BUNDLE_DIR"/manifest.d/*.md5; do
    [[ -f "$m" ]] || continue
    while read -r want dev; do
        [[ -n "$dev" ]] || continue
        dest=$(arca_offline_path "$BASE" "$dev")
        if [[ ! -f "$dest" ]]; then
            vfail "not installed: $dev"
            continue
        fi
        got=$(md5sum "$dest" | cut -d' ' -f1)
        MD5_CHECKED=$((MD5_CHECKED + 1))
        [[ "$got" == "$want" ]] || vfail "md5 mismatch: $dev (want $want, got $got)"
    done < "$m"
done
if [[ "$MD5_CHECKED" -eq "$BUNDLE_FILES" ]]; then
    ok "md5: all $MD5_CHECKED installed file(s) match the bundle manifest"
else
    vfail "md5 checked $MD5_CHECKED of $BUNDLE_FILES files — the manifests do not cover the bundle"
fi

# ── the executable bit ─────────────────────────────────────────────────────
# Real ext4 honours it, unlike /mnt/c, so this is a MEASUREMENT here and could
# not be one on the dev host. A missing +x on app_launcher is the failure that
# cannot be reproduced from Windows at all.
XCOUNT=0
for entry in "${INSTALLED[@]}"; do
    mode="${entry%%|*}"; rest="${entry#*|}"; dev="${rest%%|*}"; dest="${rest##*|}"
    case "$mode" in
        *[1357]|*[1357][0-9]|*[1357][0-9][0-9]) ;;   # owner-execute set
        *) continue ;;
    esac
    XCOUNT=$((XCOUNT + 1))
    [[ -x "$dest" ]] || vfail "declared mode $mode but not executable on the card: $dev"
done
[[ "$VBAD" -eq 0 ]] && ok "+x: all $XCOUNT file(s) declared executable are executable"

# ── every .app's exec= exists and is executable ────────────────────────────
# A manifest whose exec= names a binary that is not there renders a launcher tile
# that does nothing when tapped — which looks exactly like a broken touch panel.
APPS_DIR="$BASE/root/opt/arca/apps"
APPCOUNT=0
EXECS=""
if [[ -d "$APPS_DIR" ]]; then
    for a in "$APPS_DIR"/*.app; do
        [[ -f "$a" ]] || continue
        APPCOUNT=$((APPCOUNT + 1))
        e=$(sed -n 's/^exec=//p' "$a" | head -1 | tr -d '\r')
        if [[ -z "$e" ]]; then
            vfail "$(basename "$a"): no exec= line"
            continue
        fi
        EXECS="$EXECS $e"
        d=$(arca_offline_path "$BASE" "$e")
        if [[ ! -f "$d" ]]; then
            vfail "$(basename "$a"): exec=$e is not installed"
        elif [[ ! -x "$d" ]]; then
            vfail "$(basename "$a"): exec=$e is installed but not executable"
        fi
        # icon= too: a tile with no icon renders, but as a hole in the grid.
        i=$(sed -n 's/^icon=//p' "$a" | head -1 | tr -d '\r')
        if [[ -n "$i" ]]; then
            di=$(arca_offline_path "$BASE" "$i")
            [[ -f "$di" ]] || vfail "$(basename "$a"): icon=$i is not installed"
        fi
    done
fi
if [[ "$APPCOUNT" -eq 0 ]]; then
    vfail "no .app manifests were installed — the launcher would render an empty grid"
else
    ok ".app: all $APPCOUNT manifest(s) name an installed, executable binary"
fi

# ── default-app names one of them ──────────────────────────────────────────
DEFAULT_APP_FILE="$BASE/root/opt/arca/default-app"
if [[ ! -f "$DEFAULT_APP_FILE" ]]; then
    vfail "no /opt/arca/default-app — /etc/init.d/arca-app would start nothing"
else
    da=$(head -1 "$DEFAULT_APP_FILE" | tr -d ' \t\r\n')
    dad=$(arca_offline_path "$BASE" "$da")
    if [[ ! -x "$dad" ]]; then
        vfail "default-app is '$da', which is not installed or not executable"
    else
        ok "default-app: $da (installed, executable)"
    fi
    # It should be the launcher, or at least something with a tile — a default-app
    # that no manifest mentions boots straight into one game with no way back.
    case " $EXECS $da " in
        *" $da "*) ;;
        *) warn "default-app '$da' is in no .app manifest — nothing returns to a launcher grid" ;;
    esac
fi

# ── dash -n every /bin/sh script written ───────────────────────────────────
# A parse error or a CRLF in an init script does not fail at install time: it
# fails at boot, on a device with no serial console. `dash` is the closest thing
# this host has to BusyBox ash.
#
# ⚠️ It catches parse errors and CRLF, NOT bashisms. `[[ -n "$x" ]]` parses fine
# under dash — `[[` is read as a command name — so it passes here and fails at
# boot with "[[: not found". Measured while writing
# tests/commission_offline_test.sh case 2e. Catching that needs shellcheck, which
# IS installed in this WSL (0.7.0, re-measured 2026-09-06) but is not run here.
SHCHECK="dash"
command -v dash >/dev/null 2>&1 || SHCHECK="sh"
SHCOUNT=0
for entry in "${INSTALLED[@]}"; do
    dest="${entry##*|}"
    [[ -f "$dest" ]] || continue
    read -r shebang < "$dest" 2>/dev/null || continue
    case "$shebang" in
        '#!/bin/sh'*|'#! /bin/sh'*) ;;
        *) continue ;;
    esac
    SHCOUNT=$((SHCOUNT + 1))
    case "$shebang" in
        *$'\r'*) vfail "CRLF shebang (BusyBox rejects it as 'not found'): ${entry#*|}" ;;
    esac
    "$SHCHECK" -n "$dest" 2>/dev/null || vfail "$SHCHECK -n failed: ${entry#*|}"
done
if [[ "$SHCOUNT" -eq 0 ]]; then
    vfail "no /bin/sh scripts were checked — the init scripts should have been among them"
else
    ok "$SHCHECK -n: all $SHCOUNT /bin/sh script(s) parse"
fi

# ── the boot links resolve ─────────────────────────────────────────────────
LINKBAD=0
BOOT_LINKS=("$BASE/root/etc/rc5.d/S28time-sync" "$BASE/root/etc/rc5.d/S29audio-enable" \
            "$BASE/root/etc/rc5.d/S99arca-app")
LINK_NAMES="S28, S29, S99"
# ⚠️ The usb group's two links are asserted too, but only when the group ran.
# --no-usb leaves them uninstalled on purpose, so an unconditional list would
# report a failure for a deliberate omission; and leaving them out entirely was a
# real gap — a dangling rc5.d link is skipped in SILENCE
# at boot, which is the exact class of defect this check exists for, and these two
# are the only boot links this project installs that were never covered.
case " $NO_PROV_GROUPS " in
    *" usb "*) ;;
    *) BOOT_LINKS+=("$BASE/root/etc/rc5.d/S89xpad-modules" \
                    "$BASE/root/etc/rc5.d/S90usb-host")
       LINK_NAMES="$LINK_NAMES, S89, S90" ;;
esac
case " $NO_PROV_GROUPS " in
    *" bluetooth "*) ;;
    *) BOOT_LINKS+=("$BASE/root/etc/rc5.d/S91bluetooth")
       LINK_NAMES="$LINK_NAMES, S91" ;;
esac
for l in "${BOOT_LINKS[@]}"; do
    [[ -L "$l" ]] || { vfail "missing boot link: ${l#$BASE/root}"; LINKBAD=1; continue; }
    # A relative link resolves against its own directory, so test it from there —
    # `[ -e "$link" ]` from the wrong cwd is a dangling-link false positive.
    ( cd "$(dirname "$l")" && [ -e "$(readlink "$l")" ] ) \
        || { vfail "dangling boot link: ${l#$BASE/root} -> $(readlink "$l")"; LINKBAD=1; }
done
[[ "$LINKBAD" -eq 0 ]] && ok "boot links resolve ($LINK_NAMES)"

echo ""
if [[ "$VBAD" -gt 0 ]]; then
    echo -e "${RED}  ✗ $VBAD verification failure(s) — do NOT use this image until they are understood${NC}"
    cleanup_and_exit 1
fi

echo "════════════════════════════════════════"
echo " Done"
echo "════════════════════════════════════════"
# A green tick on a partial answer is the thing this whole entry is about, so the
# marker follows what was actually established, not merely that we got this far.
if [[ "$ARM_TRUSTED" == 1 ]]; then
    warn "ARM safety: $ARM_VERIFIED"
else
    ok "ARM safety: $ARM_VERIFIED"
fi
ok "$BUNDLE_FILES bundled file(s) installed and md5-verified on the card"
echo ""
echo "  The caller unmounts the four trees. On the first boot of a unit made from"
echo "  this image, in this order — each one is cheap and rules out the next:"
echo "    1. it comes up as a launcher grid, not a black screen"
echo "    2. ssh root@$(head -1 "$BASE/root/etc/hostname" 2>/dev/null | tr -d ' \t\r\n').local   (or find it in the DHCP leases)"
echo "    3. tap a game; it plays and exiting returns to the grid"
echo "    4. sound: Control Panel -> Audio, or Tap-a-Theremin"
echo "    5. touch: Control Panel -> Display -> CALIBRATE TOUCH — the one step"
echo "       that still needs the panel, because it is per-unit."
echo ""
# USB host mode lives entirely on p6 — the usb-group boot scripts and the
# controller modules — so a bundle delivers it outright.
case " $NO_PROV_GROUPS " in
    *" usb "*)
        echo "  USB HOST MODE was skipped (--no-usb): no /etc/init.d/usb-host and no"
        echo "  controller modules. To add it later, from a machine with the ARM"
        echo "  toolchain:   cd usb_host && ./build-and-deploy.sh <ip>"
        ;;
    *)
        echo "    6. USB: plug an Xbox controller in and check it appears —"
        echo "         /etc/init.d/usb-host status   and   lsusb"
        echo "       The budget is 100 mA, so a controller needs a POWERED hub."
        ;;
esac
echo ""
cleanup_and_exit 0
