#!/bin/bash
#
# arca.sh — the front door: one menu over the bring-up paths
#
# This script implements NOTHING of its own. Every item shells out to the
# existing script with arguments, so anything that works today keeps working
# exactly as it did, and those scripts stay non-interactive when called directly:
#
#   commissioning/provision.sh           update a running unit, over SSH, ends in a reboot
#   deploy-all.sh                        over SSH, per-component
#   rootfs/make-card-image.sh            a whole card image, offline (runs commission-offline.sh)
#
# Why a composition layer and not one merged script: the three phases have
# genuinely different connection models, and the cleanup in Phase 2 touches paths
# spread across FOUR partitions that only a booted kernel assembles into one tree,
# while rootfs/make-card-image.sh mounts all four partitions of an image itself
# (commission-offline.sh maps every absolute path onto them); the
# SSH phases stay as the verified development loop. There are three further
# reasons in COMMISSIONING.md ("why these are separate").
#
# What this script does add is the two things missing between the phases: a
# wait_for_ssh so the operator is not guessing when a rebooted device is back,
# and a single place that knows the phases run in order.
#
# Every child is invoked as `bash <script>`, not `./<script>`. A clone can land
# without the executable bit — the mode lives in the git index, so one bad commit
# breaks every fresh clone — and `./` then fails at the point of use with
# "Permission denied". Nothing is in doubt about which interpreter to use; all
# three are #!/bin/bash. deploy-all.sh already does the same for the
# per-component scripts it discovers.
#
# Usage:
#   ./arca.sh          # interactive menu
#   bash arca.sh       # ... if this file itself lost its +x
#   ./arca.sh --help

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=lib/arca-ssh.sh
. "$SCRIPT_DIR/lib/arca-ssh.sh"

# ── colour helpers (same vocabulary as the three child scripts) ──────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓ $*${NC}"; }
info() { echo -e "${YELLOW}  → $*${NC}"; }
warn() { echo -e "${BLUE}  ! $*${NC}"; }
err()  { echo -e "${RED}  ✗ $*${NC}"; }
hdr()  { echo ""; echo -e "${CYAN}════════════════════════════════════════${NC}";
         echo -e "${CYAN} $*${NC}";
         echo -e "${CYAN}════════════════════════════════════════${NC}"; }

usage() {
    cat <<'USAGE'
Usage: ./arca.sh

Interactive front door for Arca bring-up. Menu-driven; it takes no
arguments of its own beyond --help, and reimplements none of the flags of the
scripts it calls. To script a step, call that script directly:

  ./commissioning/provision.sh <target> [flags]         update a unit (ssh, reboots)
  ./commissioning/provision.sh <target> --hostname NAME name only, no reboot
  ./deploy-all.sh <target> [component]                  deploy (ssh)
  ./rootfs/make-card-image.sh --mlo <f> --uboot <f> --kernel <f> --bundle <b> <rootfs.tar> <out.img>   card image
  ./setup-build-env.sh [--install-deps] [--scummvm]     host build prerequisites

Full guide: COMMISSIONING.md
USAGE
}

case "${1:-}" in
    ""       ) ;;
    -h|--help) usage; exit 0 ;;
    *        ) err "Unknown argument: $1"; echo ""; usage; exit 1 ;;
esac

# ── the one piece of real logic: waiting for a device ────────────────────────
# Lives here rather than in the three scripts because it is only needed BETWEEN
# phases — a device that has just been powered on, or has just been rebooted by
# commissioning/provision.sh. Polling SSH itself (not ping) is deliberate: ping answers
# while sshd is still starting, which is exactly the window that produces a
# confusing "Cannot reach <ip>" from the next phase.
#
# ⚠️ It polls on `down` and STOPS on `auth`. A unit that answers and refuses our key
# is up: waiting the rest of the timeout cannot change the answer, and the old
# version spent the full 180 s doing exactly that before reporting "did not answer
# SSH" about a device that had answered every time. On that
# state it hands over to the gate, which offers to install a key.
wait_for_ssh() {
    local target="$1" timeout="${2:-180}" waited=0
    info "Waiting for SSH on $target (up to ${timeout}s)..."
    while [ "$waited" -lt "$timeout" ]; do
        if arca_ssh_probe "root@$target" -o StrictHostKeyChecking=no >/dev/null 2>&1; then
            ok "$target is up (after ${waited}s)"
            return 0
        fi
        if [ "$ARCA_SSH_LAST_STATE" = "auth" ]; then
            echo ""
            ok "$target is up (after ${waited}s) — but it refused this host's key"
            arca_ssh_gate "root@$target" -o StrictHostKeyChecking=no && return 0
            return 1
        fi
        sleep 5
        waited=$((waited + 5))
        printf '.'
    done
    echo ""
    err "$target did not answer SSH within ${timeout}s"
    return 1
}

# Ask for a target once and remember it for the rest of the session.
TARGET=""
ask_target() {
    local prompt="Device IP or host name"
    [ -n "$TARGET" ] && prompt="$prompt [$TARGET]"
    local reply
    read -r -p "$prompt: " reply
    reply="${reply:-$TARGET}"
    if [ -z "$reply" ]; then
        err "No target given."
        return 1
    fi
    TARGET="$reply"
    return 0
}

pause() {
    echo ""
    read -r -p "Press Enter to return to the menu... " _
}

confirm() {
    local reply
    read -r -p "$1 [y/N]: " reply
    case "$reply" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# ── Image a card ────────────────────────────────────────────────────────────
# A composition like everything else here: it execs rootfs/make-card-image.sh, which builds
# the whole image file and itself runs commissioning/commission-offline.sh over the
# partitions it mounts. Nothing is written to a physical card by this item.
do_image_card() {
    hdr "6. Build a card IMAGE (our own root, bundle installed, offline)"
    cat <<'PRE'
  Builds an image file of the whole card: our boot files on p1, our Buildroot root on p6,
  empty p2/p3/p5, the release bundle installed and verified. The same image for every unit:
  no vendor file, no SSH key, no host name. You write the file to a card of 4 GB or more
  yourself and the unit boots working. Updating a unit that is already running is item 2.

  Before continuing:

    - this must run as root on the native WSL filesystem (not /mnt/c)
    - the boot files are MLO and u-boot.img from uboot/build.sh, and a uImage kernel
      from kernel/build-image.sh
    - the root filesystem tar is what rootfs/build-rootfs.sh left in ~/br-rw-out
    - you have a bundle: ./release.sh --stage-only leaves one in build/release
    - the kernel modules are in ~/rw-kmods (cy8ctmg120_ts.ko is required)

  It never touches a block device.
PRE
    echo ""
    local bundle mlo uboot kernel tar out
    read -r -p "  Bundle (tarball or directory) [build/release]: " bundle
    bundle="${bundle:-build/release}"
    if [ ! -e "$SCRIPT_DIR/$bundle" ] && [ ! -e "$bundle" ]; then
        err "No such bundle: $bundle"
        info "Build one first:  ./release.sh --stage-only"
        return 1
    fi
    [ -e "$bundle" ] || bundle="$SCRIPT_DIR/$bundle"
    read -r -p "  MLO [$HOME/rw-uboot-out/MLO]: " mlo
    mlo="${mlo:-$HOME/rw-uboot-out/MLO}"
    [ -f "$mlo" ] || { err "No such file: $mlo"; return 1; }
    read -r -p "  u-boot.img [$HOME/rw-uboot-out/u-boot.img]: " uboot
    uboot="${uboot:-$HOME/rw-uboot-out/u-boot.img}"
    [ -f "$uboot" ] || { err "No such file: $uboot"; return 1; }
    read -r -p "  Kernel (uImage): " kernel
    [ -f "$kernel" ] || { err "No such file: $kernel"; return 1; }
    read -r -p "  Root filesystem tar [$HOME/br-rw-out/rootfs.tar]: " tar
    tar="${tar:-$HOME/br-rw-out/rootfs.tar}"
    [ -f "$tar" ] || { err "No such file: $tar"; return 1; }
    read -r -p "  Output image file: " out
    [ -n "$out" ] || { err "An output file is required."; return 1; }
    echo ""
    confirm "Build $out?" || { warn "Skipped."; return 0; }
    echo ""
    sudo bash "$SCRIPT_DIR/rootfs/make-card-image.sh" --mlo "$mlo" --uboot "$uboot" --kernel "$kernel" \
        --bundle "$bundle" "$tar" "$out" \
        || err "Building the image failed."
}

# ── Update a booted device ─────────────────────────────────────────────────────────────────
do_setup_menu() {
    while true; do
        hdr "2. Set up a booted device (ssh)"
        cat <<'MENU'
  a) Update this unit               backup, install the plan, reboot
  b) Update DRY RUN                 --dry-run      (prints the plan; changes nothing)
  c) Set host name only             --hostname NAME          (no reboot)
  d) Device status                  --status                 (read-only)
  q) Back
MENU
        echo ""
        local choice
        read -r -p "Choice: " choice
        case "$choice" in
            a) ask_target || { pause; continue; }
               bash "$SCRIPT_DIR/commissioning/provision.sh" "$TARGET" || err "Setup failed."
               pause ;;
            b) ask_target || { pause; continue; }
               bash "$SCRIPT_DIR/commissioning/provision.sh" "$TARGET" --dry-run \
                   || err "Dry run failed."
               pause ;;
            c) ask_target || { pause; continue; }
               local name
               read -r -p "New host name (single label, e.g. rw09): " name
               if [ -n "$name" ]; then
                   bash "$SCRIPT_DIR/commissioning/provision.sh" "$TARGET" --hostname "$name" \
                       || err "Could not set the host name."
               else
                   warn "No name given; skipped."
               fi
               pause ;;
            d) ask_target || { pause; continue; }
               bash "$SCRIPT_DIR/commissioning/provision.sh" "$TARGET" --status || err "Status failed."
               pause ;;
            back|q|Q|"") return 0 ;;
            *) err "Not a choice: $choice"; pause ;;
        esac
    done
}

# ── Deploy ─────────────────────────────────────────────────────────────────
# Three sources of binaries, and the default is the one that changes no meaning
# for anyone who was already pressing Enter here: build from source. The other
# two are pure delegation — deploy-all.sh already accepts a local bundle and a
# release tag, and the DOWNLOAD lives in lib/arca-release.sh behind that script, not
# here. This file opens no sockets of its own.
do_deploy() {
    hdr "3. Deploy apps (ssh)"
    info "Discovered components:"
    bash "$SCRIPT_DIR/deploy-all.sh" --list
    echo ""
    ask_target || return 0
    cat <<'SRC'

  Where should the binaries come from?

    s) build them from source   needs the cross-compiler and WSL; ScummVM is
                                a ~2 min link          (default — press Enter)
    b) a local bundle           build/release, from ./release.sh --stage-only.
                                Seconds instead of a rebuild, and it puts the
                                bytes you already TESTED on the device.
    r) a published release      downloaded from GitHub, sha256-checked against
                                the digest it publishes, and cached — so the
                                second unit costs no second download.

  b and r install binaries and NOTHING ELSE: a bundle carries no config, by
  construction. Touch calibration, the host name and the VNC password come from
  item 2, exactly as they do after a from-source deploy.
SRC
    echo ""
    local src
    read -r -p "Source [s]: " src
    case "${src:-s}" in
        s|S|source|"")
            local comp
            read -r -p "Component (Enter = all): " comp
            echo ""
            if [ -n "$comp" ]; then
                bash "$SCRIPT_DIR/deploy-all.sh" "$TARGET" "$comp" || err "Deploy failed."
            else
                bash "$SCRIPT_DIR/deploy-all.sh" "$TARGET" || err "Deploy failed."
            fi ;;
        b|B|bundle)
            local bundle
            read -r -p "Bundle (tarball or directory) [build/release]: " bundle
            bundle="${bundle:-build/release}"
            if [ ! -e "$SCRIPT_DIR/$bundle" ] && [ ! -e "$bundle" ]; then
                err "No such bundle: $bundle"
                info "Build one first:  ./release.sh --stage-only"
                return 1
            fi
            [ -e "$bundle" ] || bundle="$SCRIPT_DIR/$bundle"
            echo ""
            bash "$SCRIPT_DIR/deploy-all.sh" --from-bundle "$bundle" "$TARGET" \
                || err "Deploy failed." ;;
        r|R|release)
            local tag
            read -r -p "Release tag [latest]: " tag
            echo ""
            bash "$SCRIPT_DIR/deploy-all.sh" --from-release "${tag:-latest}" "$TARGET" \
                || err "Deploy failed." ;;
        *)  err "Not a choice: $src" ;;
    esac
}

# ── Status ──────────────────────────────────────────────────────────────────
do_status() {
    hdr "4. Device status (read-only)"
    ask_target || return 0
    bash "$SCRIPT_DIR/commissioning/provision.sh" "$TARGET" --status || err "Status failed."
}

# ── First boot of an imaged card ────────────────────────────────────────────
# The only item that waits, and the reason wait_for_ssh exists: the gap between
# writing a card image (item 6) and the unit answering on the network.
do_full() {
    hdr "5. First boot of an imaged card: boot -> wait -> deploy"
    cat <<'PRE'
  For a card written from item 6's image. The image already carries our root and
  the release bundle, so there is no setup phase; this waits for the unit to
  answer ssh, then offers to deploy components on top.
PRE
    echo ""
    confirm "Start?" || { warn "Cancelled."; return 0; }

    hdr "Boot the device"
    cat <<'PRE'
  Now:
    1. put the written card in the Arca
    2. connect Ethernet and power it on
PRE
    echo ""
    read -r -p "Press Enter once the device is powered on... " _
    echo ""
    ask_target || return 1
    wait_for_ssh "$TARGET" 300 || return 1

    hdr "Deploy"
    confirm "Build and deploy all components to $TARGET?" \
        || { warn "Stopping before deploy."; return 0; }
    bash "$SCRIPT_DIR/deploy-all.sh" "$TARGET" || { err "Deploy failed."; return 1; }

    hdr "Done"
    ok "Booted and deployed: $TARGET"
}

# ── this host, not a device ─────────────────────────────────────────────────
# The only menu item that touches no device and no card. It is here because "I cloned
# the repo and nothing builds" is the first thing a fresh machine hits, and the one
# package set lives in one script rather than in six hand-written apt lines.
do_setup_build_env() {
    hdr "Host build prerequisites"
    local extra=()
    confirm "Also set up the upstream ScummVM tree? (a large clone; only scummvm-arca needs it)" \
        && extra+=(--scummvm)
    bash "$SCRIPT_DIR/setup-build-env.sh" "${extra[@]+"${extra[@]}"}"
}

# ── main menu ───────────────────────────────────────────────────────────────
while true; do
    hdr "Arca"
    [ -n "$TARGET" ] && info "Target: $TARGET"
    cat <<'MENU'
  2) Update a booted device      ssh; backup, update, reboot
  3) Deploy apps                 ssh; source, bundle or release
  5) First boot of an imaged card boot, wait for ssh, deploy

  6) Build a card image          offline; our root + bundle, one boot (deliver a unit)

  4) Device status               read-only
  7) Host build prerequisites    this machine; no device, no card
  q) Quit
MENU
    echo ""
    read -r -p "Choice: " CHOICE || { echo ""; exit 0; }
    case "$CHOICE" in
        2) do_setup_menu ;;
        3) do_deploy; pause ;;
        4) do_status; pause ;;
        5) do_full; pause ;;
        6) do_image_card; pause ;;
        7) do_setup_build_env; pause ;;
        q|Q|quit|exit) echo ""; ok "Bye."; exit 0 ;;
        "") ;;
        *) err "Not a choice: $CHOICE"; pause ;;
    esac
done
