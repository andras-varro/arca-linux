#!/bin/bash
#
# release.sh — build every component, stage one bundle, publish it as a GitHub
#              release.
#
# Usage:
#   ./release.sh --stage-only [--out <dir>]        # build + stage + tar, no network
#   ./release.sh --tag <tag>  [--out <dir>]        # the above, then gh release create
#   ./release.sh --stage-only --component native_apps
#   ./release.sh --help
#
# ── Why this exists ─────────────────────────────────────────────────────────
#
# The build is the slowest and most environment-bound step in the whole flow:
# WSL, an ARM cross-compiler, and a ScummVM link that takes ~1m35s–2m20s and
# deletes native_apps/common/*.o twice on the way.  All of that is pure overhead
# whenever the source has not changed, and it is a hard barrier for anyone who
# wants to put apps on a device without first reproducing the toolchain.  The
# artifacts suit distribution unusually well: vnc_client ships -static, and the
# dynamic binaries link only the device's own soft-float userspace (glibc 2.31, libstdc++,
# libasound.so.2), which its stock rootfs provides — so there is nothing to ship beside them.
#
# commissioning/commission-offline.sh is the first non-developer consumer — an offline
# commissioner has no toolchain to fall back on, so a bundle IS its only source
# of binaries.
#
# ── What is NOT published, and why ──────────────────────────────────────────
#
#   * Device config.  Never.  /etc/hosts carries a mapping that exists to be
#     removed and vnc_client.conf carries a plaintext
#     VNC password.  A glob that swept up *.conf would publish exactly what those
#     two exist to have removed.  Each component's --bundle decides; this script
#     re-checks below.
#   * The vendor image or any part of it — a third party's copyright, and in
#     particular the vendor uImage-system and boot chain on p1.  The manifest
#     check below is the negative control for that rule, exactly parallel to the
#     config one.
#
# ── usb_host IS published ───────────────────────────────────────────────────
#
# Everything it installs is on p6: its build artifacts are bundled like any
# others, and its device scripts are device-files/provision-rules.conf's `usb`
# group.  No installer writes p1.

#
# ── Not byte-reproducible ───────────────────────────────────────────────────
#
# base/version.o re-embeds the build date on every link, so two releases from an
# identical tree differ.  The md5 manifest is therefore GENERATED per release by
# lib/arca-bundle.sh, never asserted against a known-good set.
#
# ── gh ──────────────────────────────────────────────────────────────────────
#
# `gh` 2.86.0 is installed in WSL (from the release .deb — focal's apt has no `gh`
# and the snap links against a glibc newer than 2.31).  --stage-only stays the
# network-free path and produces a tarball commissioning/commission-offline.sh
# --bundle <file> takes directly.
#
# ⚠️ Every gh call below passes --repo, and it is NOT optional.  `origin` is an SSH
# host alias, and gh does not merely fail to guess the owner from it — it refuses
# the repository outright: "none of the git remotes configured for this repository
# point to a known GitHub host".  So the publish step derives owner/repo itself.
# Whoever publishes still needs `gh auth` for an account with write access.

set -e
_START_SECONDS=$(date +%s)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# shellcheck source=lib/arca-bundle.sh
. "$SCRIPT_DIR/lib/arca-bundle.sh"
# shellcheck source=lib/arca-release.sh
# Sourced for arca_release_repo alone — the one owner/repo derivation, shared with
# the fetch side so a fork publishes to and fetches from the same place.
. "$SCRIPT_DIR/lib/arca-release.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓ $*${NC}"; }
info() { echo -e "${YELLOW}  → $*${NC}"; }
warn() { echo -e "${BLUE}  ! $*${NC}"; }
err()  { echo -e "${RED}  ✗ $*${NC}" >&2; exit 1; }

# ── the components a release carries ────────────────────────────────────────
# Order matters only in that native_apps writes /opt/arca/default-app and
# the app_launcher every other component's tile is reached through.
#
# usb_host is here as of 2026-08-08. Its --bundle needs
# usb_host/device_config and usb_host/modules/*.ko, both gitignored build
# artifacts; on a fresh clone it refuses with the one command that fetches the
# config from any unit.
RELEASE_COMPONENTS=(native_apps vnc_client scummvm-arca usb_host bluetooth)

usage() {
    echo "Usage: $0 --stage-only [--out <dir>] [--component <name>]..."
    echo "       $0 --tag <tag>  [--out <dir>] [--component <name>]..."
    echo ""
    echo "  --stage-only      Build, stage and tar. No network, no gh."
    echo "  --skip-tests      Stage without running the host gate first. The"
    echo "                    gate (tests/run-all.sh, ~190 s) otherwise blocks"
    echo "                    the release if any suite fails."
    echo "  --tag <tag>       The above, then publish with 'gh release create <tag>'."
    echo "  --out <dir>       Where to stage (default: build/release)."
    echo "  --component <n>   Only this component; repeatable. Default: all of"
    echo "                    ${RELEASE_COMPONENTS[*]}"
    echo "  --notes <file>    Release notes for gh (default: generated)."
    echo ""
    echo "Components: ${RELEASE_COMPONENTS[*]}"
    exit 1
}

TAG=""
OUT="build/release"
STAGE_ONLY=0
SKIP_TESTS=0
NOTES_FILE=""
COMPONENTS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stage-only) STAGE_ONLY=1; shift ;;
        --skip-tests) SKIP_TESTS=1; shift ;;
        --tag)        TAG="${2:-}";  [[ -n "$TAG" ]]  || { echo "--tag needs a value"; usage; }; shift 2 ;;
        --out)        OUT="${2:-}";  [[ -n "$OUT" ]]  || { echo "--out needs a value"; usage; }; shift 2 ;;
        --notes)      NOTES_FILE="${2:-}"; [[ -f "$NOTES_FILE" ]] || { echo "no such notes file: ${2:-}"; usage; }; shift 2 ;;
        --component)
            [[ -n "${2:-}" ]] || { echo "--component needs a value"; usage; }
            # Validated against the list rather than passed through: an unknown
            # name would otherwise stage nothing and produce an empty bundle that
            # installs cleanly and delivers no apps.
            found=0
            for c in "${RELEASE_COMPONENTS[@]}"; do [[ "$c" == "$2" ]] && found=1; done
            [[ $found -eq 1 ]] || { echo "Unknown component: $2"; echo ""; usage; }
            COMPONENTS+=("$2"); shift 2 ;;
        --help|-h)    usage ;;
        *)            echo "Unknown option: $1"; echo ""; usage ;;
    esac
done

[[ ${#COMPONENTS[@]} -gt 0 ]] || COMPONENTS=("${RELEASE_COMPONENTS[@]}")

if [[ $STAGE_ONLY -eq 0 && -z "$TAG" ]]; then
    echo "Give either --stage-only or --tag <tag>."
    echo ""
    usage
fi
if [[ $STAGE_ONLY -eq 1 && -n "$TAG" ]]; then
    err "--stage-only and --tag are mutually exclusive"
fi

echo ""
echo "════════════════════════════════════════"
echo " Arca release"
echo "════════════════════════════════════════"
info "Components: ${COMPONENTS[*]}"
info "Staging to: $OUT"
[[ -n "$TAG" ]] && info "Tag:        $TAG"

# ── git provenance ──────────────────────────────────────────────────────────
# Measured here rather than beside the bundle metadata that consumes it, because
# the publish preflight below needs it and the whole point of the preflight is to
# run before the component loop.  These read the working tree's git state only —
# no build artifact, no $OUT_ABS.
GIT_REV="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
# The full SHA as well, for gh --target only.  GitHub's API takes a branch name or
# a 40-character SHA for target_commitish and rejects an abbreviated one outright:
# a short rev there fails the whole publish with "HTTP 422 Validation Failed:
# Release.target_commitish is invalid", after the build and the staging are done.
GIT_REV_FULL="$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY=""
# ⚠️ BOTH forms are needed, and the second was missing.  `git diff --quiet` compares
# the working tree against the INDEX, so it exits 0 while a modification sits STAGED
# — measured in a throwaway repo, where `git add -A` then `git diff --quiet` exits 0
# and `git diff --cached --quiet` exits 1.  Staged-and-uncommitted is the normal
# shape of a tree mid-commit, so `git add -A && ./release.sh --tag …` would publish a
# release whose NOTICE offers source that is in no commit.  `--cached` compares the
# index against HEAD and is what sees it.
if ! git -C "$SCRIPT_DIR" diff --quiet 2>/dev/null \
   || ! git -C "$SCRIPT_DIR" diff --cached --quiet 2>/dev/null; then
    GIT_DIRTY=" (dirty)"
fi

# ── publish preflight ───────────────────────────────────────────────────────
# Every refusal `--tag` can raise from LOCAL state is raised here, before the first
# component builds.  These checks used to sit after the tarball, so `--tag` on a
# host without gh, or on an unpushed commit, spent a full four-component rebuild to
# print a message it could have printed in a second.
#
# ⚠️ Deliberately network-free, and that is the boundary for what belongs here: gh's
# presence, origin's URL and the local remote-tracking refs are all readable
# offline.  `gh auth status` and "does this tag already exist" are NOT hoisted —
# they would let a flaky network refuse a release that is otherwise fine — and both
# still refuse at `gh release create` below, after the build.
if [[ $STAGE_ONLY -eq 0 ]]; then
    command -v gh >/dev/null 2>&1 \
        || err "gh is not installed — install it, or use --stage-only. Nothing was built."

    # gh resolves owner/repo from the remote URL and CANNOT resolve this one:
    # `origin` is an SSH host alias (git@github.com-personal:owner/repo.git), and
    # gh rejects the whole repository with "none of the git remotes configured for
    # this repository point to a known GitHub host".  Measured, not anticipated —
    # every gh call here therefore needs an explicit --repo.
    #
    # ⚠️ The derivation itself lives in lib/arca-release.sh and there is ONE of it,
    # because the fetch side needs exactly the same answer: a fork must fetch from
    # the fork it published to.  Do not write a second copy here.
    GH_REPO="$(arca_release_repo "$SCRIPT_DIR" 2>/dev/null || true)"
    case "$GH_REPO" in
        */*) ;;
        *)   err "could not derive owner/repo from origin
     ('$(git -C "$SCRIPT_DIR" remote get-url origin 2>/dev/null)').
     gh cannot resolve it either, so pass a tarball to 'gh release create --repo
     <owner>/<repo>' by hand. Nothing was built." ;;
    esac

    # A tag whose commit is not on the remote makes the bundle's NOTICE false.
    # NOTICE carries a GPL written offer — "the complete corresponding source ...
    # available from the repository this release was published from" — for ScummVM
    # (GPLv3+), LibVNCClient (GPLv2+) and three kernel modules (GPL-2.0-only).  An
    # unpushed or dirty tree does not satisfy that offer, and the failure is silent:
    # gh would tag the remote default branch's HEAD instead, so the release would
    # name a commit that builds different binaries.
    [[ -z "$GIT_DIRTY" ]] || err "the working tree is dirty, so the source for these
     binaries is not in any commit — and NOTICE offers exactly that source. Commit
     first. Nothing was built."
    if [[ -z "$(git -C "$SCRIPT_DIR" branch -r --contains HEAD 2>/dev/null)" ]]; then
        err "HEAD ($GIT_REV) is on no remote branch, so the corresponding source
     NOTICE offers is unpublished. Push first. Nothing was built."
    fi
    ok "Preflight: gh present, $GH_REPO, $GIT_REV pushed and clean"
fi

# ── stage ───────────────────────────────────────────────────────────────────
# A whole-directory wipe, because a bundle assembled on top of a previous one can
# carry a file no manifest names — which arca_bundle_check then reports, but only
# after the operator has already been told the release is ready.
#
# Guarded rather than trusted: $OUT comes from the command line and this is an
# `rm -rf`.  The guard refuses root, a path
# containing `..`, a non-directory, and a non-empty directory that is not a bundle.
#
# Normalise before comparing, so that "//" and "/." cannot spell root past a
# string test — arca_offline_base_ok does exactly this, for exactly this reason,
# and both spellings pass an un-normalised comparison.
OUT_NORM="$(printf '%s' "$OUT" | sed -e 's:/\{2,\}:/:g' -e 's:/\.$:/:' -e 's:\(.\)/$:\1:')"
case "$OUT_NORM" in
    ""|"/") err "refusing to stage into '$OUT' — that resolves to this host's root" ;;
esac
[[ "$OUT" == *".."* ]] && err "refusing to stage into a path containing '..': $OUT"
if [[ -e "$OUT" && ! -d "$OUT" ]]; then
    err "$OUT exists and is not a directory"
fi

# ⚠️ This is the check that makes the `rm -rf` below safe, and it is the one that
# was missing.  Containment under a base is not available here, because --out is
# legitimately anywhere the operator wants to stage.  So the guard is
# OWNERSHIP: an existing $OUT must be something this script made —
# a previous bundle, or an empty directory — and `--out /usr` is refused rather
# than destroyed.
#
# What was here before listed ""|"/"|"/*", and a QUOTED "/*" in a case pattern is
# the literal two-character string, not a glob.  So of those three patterns only
# "/" could ever fire: "" is already refused when --out is parsed, and a bare /*
# is expanded by the caller's shell long before this sees it.
if [[ -d "$OUT" && -n "$(ls -A "$OUT" 2>/dev/null)" ]]; then
    if [[ ! -d "$OUT/root" || ! -d "$OUT/manifest.d" ]]; then
        err "refusing to 'rm -rf' '$OUT' — it is not empty and is not a staged bundle
     (no root/ and no manifest.d/). Stage into a new or empty directory, or remove
     that one yourself if you did mean it."
    fi
fi
rm -rf "$OUT"
mkdir -p "$OUT"
OUT_ABS="$(cd "$OUT" && pwd)"

# ── the host gate, immediately before the first component builds ────────────
#
# ⚠️ Placement is load-bearing and was wrong once. This must come AFTER every
# local refusal — --out '/', the non-empty-directory ownership check, the
# publish preflight — and BEFORE the first build. Put earlier, it preempts
# those refusals: arca_release_test.sh runs this script in a temp tree with no
# tests/ directory, so `bash …/run-all.sh` returned 127, the gate "failed",
# and release.sh exited before reaching any --out case. That suite went 40/0
# to 10/30, its own A0 control reporting every refusal below it as
# unfalsifiable. Measured 2026-09-08, by this gate on its first real run.
#
# --scope defaults to all, not deploy: a release bundle carries the
# documentation, so doc_check.sh is in scope here even though deploy-all.sh
# omits it. Measured 2026-09-08 on this host: ~190 s, against a four-component
# ARM cross-build that costs several minutes more.
#
# ⚠️ Exit 2 means the gate could not judge, not that a test failed. Shipping a
# bundle nothing could grade is worse than shipping one with a red test, so
# both stop here.
if [[ $SKIP_TESTS -eq 1 ]]; then
    warn "host gate SKIPPED (--skip-tests) — this bundle has been graded by nobody"
else
    info "Host gate: tests/run-all.sh  (~190 s; --skip-tests to bypass)"
    gate_rc=0
    bash "$SCRIPT_DIR/tests/run-all.sh" || gate_rc=$?
    case "$gate_rc" in
        0) : ;;
        2) err "host gate could not judge (exit 2) — fix the harness. Nothing was built." ;;
        *) err "host gate FAILED — refusing to stage a release. Override with --skip-tests." ;;
    esac
fi

for comp in "${COMPONENTS[@]}"; do
    echo ""
    echo "────────────────────────────────────────"
    echo " $comp"
    echo "────────────────────────────────────────"
    [[ -x "$SCRIPT_DIR/$comp/build-and-deploy.sh" || -f "$SCRIPT_DIR/$comp/build-and-deploy.sh" ]] \
        || err "$comp/build-and-deploy.sh not found"

    # `bash <script>`, never ./<script>: a fresh clone can land without the
    # executable bit and /mnt/c cannot even show whether it has one (CLAUDE.md).
    ( cd "$SCRIPT_DIR/$comp" && bash ./build-and-deploy.sh --bundle "$OUT_ABS" ) \
        || err "$comp --bundle failed"
done

# ── bundle metadata ─────────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────────────"
echo " Bundle"
echo "────────────────────────────────────────"

{
    echo "tag=${TAG:-untagged}"
    echo "built=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "commit=${GIT_REV}${GIT_DIRTY}"
    echo "components=$(arca_bundle_components "$OUT_ABS" | tr '\n' ' ')"
} > "$OUT_ABS/manifest.d/bundle.info"

# ── the same bytes, installed on the device ─────────────────────────────────
# Until this existed, no unit could answer "what am I running?": bundle.info lived
# in manifest.d/, which the installers read and never write, so the tag and commit
# reached the operator's terminal and nothing else.  Staging it as an ordinary
# bundle artifact is what puts it on the device — arca_bundle_entries drives BOTH
# installers (arca_bundle_install_ssh for --from-bundle/--from-release, and
# commission-offline.sh's put() loop), so one manifest line covers every bundle
# path and gets the file md5-verified on each of them for free.
#
# ⚠️ Same file, not a second rendering of the same facts: a stamp that disagreed
# with the bundle it shipped in would be worse than no stamp at all.
#
# ⚠️ $ARCA_BUNDLE_META_COMPONENT, not one of the four real components. Manifests are
# per-component and this file belongs to none of them; the reserved name is what
# keeps it out of every "Components:" line — see lib/arca-bundle.sh.
arca_bundle_init "$OUT_ABS" "$ARCA_BUNDLE_META_COMPONENT"
arca_bundle_add  "$OUT_ABS" "$ARCA_BUNDLE_META_COMPONENT" 0644 \
    "$OUT_ABS/manifest.d/bundle.info" "$ARCA_BUNDLE_STAMP" \
    || err "could not stage the provenance stamp"
arca_bundle_finish "$OUT_ABS" "$ARCA_BUNDLE_META_COMPONENT" >/dev/null

# ── NOTICE: the two licence obligations a published bundle carries ──────────
cat > "$OUT_ABS/NOTICE" <<'NOTICE'
Arca app bundle
=====================

This bundle contains statically linked ARM binaries for Arca
(TI OMAP3503, Cortex-A8, kernel 4.14.52).  It contains NO vendor firmware and NO
device configuration.

ScummVM
-------
The `scummvm` binary is a build of ScummVM, which is licensed under the GNU
General Public License, version 3 or later.  (Measured, not assumed: the upstream
tree's COPYING is GPLv3 and its source headers read "either version 3 of the
License, or (at your option) any later version".)  The complete corresponding
source for this binary is the upstream ScummVM tree plus this project's backend
port in `scummvm-arca/backend-files/`; both are available from the
repository this release was published from.  Written offers for the source are
honoured for the lifetime of the release.

Three data files travel with it and are ALSO GPLv3+, not this project's own:
`scummremastered.zip` and `gui-icons.dat` are staged verbatim out of the ScummVM
tree, and `vkeybd_arca.zip` is a 2x scaled derivative of ScummVM's
`vkeybd_small.zip`.  Same corresponding-source offer.

vnc_client
----------
Links LibVNCClient (GPLv2+), zlib (zlib licence) and libjpeg-turbo
(IJG / BSD-3-Clause).  Same corresponding-source offer as above.

native_apps
-----------
This project's own code (MIT — see LICENSE.md), statically linked against glibc
(LGPL-2.1+).  `terminal` also contains libvterm 0.3.3, linked statically and
licensed under the MIT license: Copyright (c) 2008 Paul Evans.

Audio assets  ⚠️ NOT FOR COMMERCIAL USE
---------------------------------------
The sound effects installed to /opt/sound as `fx_*.wav` (eleven files) are AI-
generated audio from https://elevenlabs.io, produced on a FREE account, which
does not grant commercial use.  They are the only content in this bundle that is
narrower than its code licence: THIS BUNDLE MAY NOT BE USED OR REDISTRIBUTED
COMMERCIALLY unless those eleven files are removed or replaced.  Removing them is
safe — every game falls back to its built-in note tables.

ATTRIBUTION, REQUIRED — this credit must travel with the bundle:

    Sound effects generated with ElevenLabs — elevenlabs.io.

Both obligations were confirmed by ElevenLabs and relayed by the author on
2026-08-23, and they are the whole of what is established: non-commercial use,
plus that credit.  See LICENSE.md, which is the repo-level half of this notice
and must agree with it.

The music beds installed to /opt/sound as `<stem><n>-mono.wav` (one set per game)
are AI-generated and are not this project's composition.  All of them are from
musely.ai and ARE royalty-free for commercial use per that generator's FAQ, with
no attribution requirement — so they are NOT a restriction on this bundle.  Two
limits: the FAQ asserts clearance but not ownership, so they cannot be licensed
onward as this project's own, and every bed carries the string "made with suno" in
its WAV metadata, which is consistent with musely.ai being a Suno front end and is
not a separate licence claim.  LICENSE.md is the repo-level half of this and must
agree with it.  Removing any bed is safe — a game whose configured bed is missing
plays its effects and carries on.

usb_host
--------
`devmem_write` is this project's own code (MIT).

⚠️ `ff-memless.ko`, `joydev.ko` and `xpad.ko` are LINUX KERNEL MODULES and are
licensed under the GNU General Public License, version 2 (GPL-2.0-only), as is
the kernel they are built against.  WRITTEN OFFER FOR SOURCE: the complete
corresponding source is the unmodified upstream Linux 4.14.52 tree, as published
at

    https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.14.52.tar.xz

together with the kernel configuration and build script in this project's
repository at `usb_host/build-kernel-modules.sh`.  No kernel source was modified;
the three modules are upstream drivers compiled for this device's kernel
configuration, which is read off the device itself.  This offer is honoured for
the lifetime of the release.

bluetooth
---------
`bluetoothd`, `bluetoothctl` and `libbluetooth.so.3` are BlueZ 5.66, licensed
under the GNU General Public License, version 2 or later (GPL-2.0-or-later, per
the SPDX lines of the sources they are built from; the tree's COPYING is GPLv2).
WRITTEN OFFER FOR SOURCE: the complete corresponding source is the unmodified
upstream release

    https://www.kernel.org/pub/linux/bluetooth/bluez-5.66.tar.xz

together with the build script in this project's repository at
`bluetooth/build-bluez.sh`.

`bluealsa`, `bluealsa-aplay` and the ALSA plugins libasound_module_pcm_bluealsa.so
and libasound_module_ctl_bluealsa.so are BlueALSA 4.3.1, licensed under the MIT
license: Copyright (c) 2016-2023 Arkadiusz Bokowy.  The same licence covers its
dbus policy and ALSA configuration file installed under /etc.  `bluealsa` also
contains the SBC codec library, sbc 2.0 (LGPL-2.1-or-later), linked statically.
WRITTEN OFFER FOR SOURCE (and relinking, LGPL-2.1 section 6): the complete
corresponding source is the unmodified upstream releases

    https://github.com/arkq/bluez-alsa/archive/refs/tags/v4.3.1.tar.gz
    https://www.kernel.org/pub/linux/bluetooth/sbc-2.0.tar.xz

together with the build script in this project's repository at
`bluetooth/build-bluealsa.sh`, which rebuilds both.

⚠️ The modules under /lib/modules/4.14.52/bt are LINUX KERNEL MODULES, GPL-2.0-only.
Their source is the same upstream Linux 4.14.52 tree as above, with this project's
patches in `kernel/patches/` and `kernel/patches-modules/` (two of them change
btusb.c and btrtl.c) and the build script `kernel/build-bt-modules.sh`.  This
offer is honoured for the lifetime of the release.

`rtl8761cu_fw.bin` and `rtl8761cu_config.bin` in /lib/firmware/rtl_bt are Realtek
firmware from linux-firmware, redistributed unmodified under the Realtek licence
installed beside them as `LICENCE.rtlwifi_firmware.txt`:

    Copyright (c) 2010, Realtek Semiconductor Corporation. All rights reserved.
    Redistribution in binary form, without modification, is permitted; the full
    conditions and disclaimer are in that file.

Not included
------------
No device configuration is published, deliberately: /etc/hosts carries a
host-name mapping that exists in order to be removed, and vnc_client.conf carries
a plaintext VNC password.  Create the latter on the device after installing.

No vendor firmware is published either.  In particular this bundle does NOT
contain `uImage-system` or any other file from the boot partition.
NOTICE

# ── structural + policy checks on what was staged ───────────────────────────
info "Checking bundle structure..."
if ! CHECK_OUT="$(arca_bundle_check "$OUT_ABS")"; then
    echo "$CHECK_OUT"
    err "bundle is not self-consistent"
fi
ok "Every manifest entry is staged, and every staged file is in a manifest"

# The publish-no-configs rule, enforced rather than trusted.  Each component's
# --bundle already declines to stage its config; this is the negative control
# that would catch a future component that forgot.
info "Checking that no device config was staged..."
CONFIG_HITS="$(arca_bundle_entries "$OUT_ABS" | awk '{print $2}' | grep -E '\.conf$|/etc/hosts$|/etc/hostname$|arca_config|touch_calibration|input_config' || true)"
if [[ -n "$CONFIG_HITS" ]]; then
    echo "$CONFIG_HITS" | sed 's/^/    /'
    err "a release must publish binaries only, never device config"
fi
ok "No config files staged"

# The never-publish-the-vendor-kernel rule, enforced rather than trusted, and the
# reason a bundle stays 5.2 MB smaller.  usb_host/.gitignore already calls
# uImage-system* "Copyrighted device-specific files"; this repo is meant to be
# published, so a component that decided to stage one anyway must not get past
# here.
#
# Matched on the BASENAME, not on a path: p1 is not a bundle path at all, so a
# staged copy would arrive at some invented location like /opt/arca/ — the
# hazard is the bytes, wherever they are put.
info "Checking that no vendor firmware was staged..."
FIRMWARE_HITS="$(arca_bundle_entries "$OUT_ABS" | awk '{print $2}' \
    | grep -E '(^|/)(uImage[^/]*|mlo|MLO|u-boot[^/]*|ctrlblock[^/]*)$' || true)"
if [[ -n "$FIRMWARE_HITS" ]]; then
    echo "$FIRMWARE_HITS" | sed 's/^/    /'
    err "a release must never publish the vendor kernel or boot chain — it is a third
     party's copyright"
fi
ok "No vendor firmware staged"

FILE_COUNT="$(arca_bundle_entries "$OUT_ABS" | grep -c . || true)"
info "$FILE_COUNT file(s) staged:"
arca_bundle_components "$OUT_ABS" | while read -r c; do
    printf '    %-22s %s file(s)\n' "$c" "$(grep -c . "$OUT_ABS/manifest.d/$c.list")"
done

# ── tar ─────────────────────────────────────────────────────────────────────
# --owner/--group/--numeric-owner so the archive does not carry whoever built it,
# and so two builds differ only in the artifacts.  Modes inside the tar are NOT
# the authority — manifest.d/*.list is (see lib/arca-bundle.sh); on /mnt/c every file
# reads 0777 and the tar would say so.
TARBALL="$SCRIPT_DIR/build/arca-apps-${TAG:-$(date -u '+%Y%m%d')}.tar.gz"
mkdir -p "$(dirname "$TARBALL")"
info "Creating $TARBALL"
tar -czf "$TARBALL" \
    --owner=root --group=root --numeric-owner \
    -C "$OUT_ABS" root manifest.d NOTICE
ok "Tarball: $(du -h "$TARBALL" | cut -f1)"

md5sum "$TARBALL" | sed 's/^/    /'

# ── publish ─────────────────────────────────────────────────────────────────
if [[ $STAGE_ONLY -eq 1 ]]; then
    echo ""
    ok "Staged only — nothing was published."
    echo ""
    echo "  Install it offline, with no network and no toolchain:"
    echo "    ./commissioning/commission-offline.sh --bundle $TARBALL"
    echo ""
    echo "  Or publish it:"
    echo "    $0 --tag <tag>"
else
    echo ""
    echo "────────────────────────────────────────"
    echo " Publishing"
    echo "────────────────────────────────────────"
    info "Publishing to $GH_REPO"

    # Re-measured, because the preflight above ran on the PRE-build tree.  A
    # component build that modified a TRACKED file would otherwise publish a tag
    # whose commit does not contain the source these binaries were built from —
    # exactly the falsehood the preflight's dirty check exists to prevent.
    #
    # ⚠️ No build step in this repo is known to do that; every build output is
    # gitignored.  So this is a guard against a future one, not a recorded failure,
    # and it is here rather than hoisted because it can only be measured now.
    # ⚠️ Both forms, for the reason given beside the preflight's own check: the
    # worktree-vs-index comparison alone cannot see a staged change.
    if ! git -C "$SCRIPT_DIR" diff --quiet 2>/dev/null \
       || ! git -C "$SCRIPT_DIR" diff --cached --quiet 2>/dev/null; then
        err "a component build modified a
     tracked file, so HEAD no longer matches the source for these binaries. The
     tarball is already built: $TARBALL"
    fi

    if [[ -z "$NOTES_FILE" ]]; then
        NOTES_FILE="$(mktemp)"
        {
            echo "Statically linked ARM binaries for Arca."
            echo ""
            echo "Install offline, with no toolchain:"
            echo ""
            echo '```'
            echo "./commissioning/commission-offline.sh --bundle $(basename "$TARBALL")"
            echo '```'
            echo ""
            echo "Built from ${GIT_REV}${GIT_DIRTY}. Components:"
            arca_bundle_components "$OUT_ABS" | sed 's/^/- /'
            echo ""
            echo "See NOTICE inside the tarball for licence obligations (ScummVM is GPLv3+)."
        } > "$NOTES_FILE"
    fi

    info "gh release create $TAG"
    # --target pins the tag to the commit the binaries were actually built from.
    # Without it gh tags the remote default branch's HEAD, which is a different
    # commit whenever anything landed on main after this build started.
    gh release create "$TAG" "$TARBALL" \
        --repo "$GH_REPO" \
        --target "$GIT_REV_FULL" \
        --title "Arca apps $TAG" \
        --notes-file "$NOTES_FILE" \
        || err "gh release create failed — the tarball is still at $TARBALL"
    ok "Published $TAG"
    gh release view "$TAG" --repo "$GH_REPO" --json url --jq .url 2>/dev/null | sed 's/^/    /'
fi

_ELAPSED=$(( $(date +%s) - _START_SECONDS ))
echo ""
printf "  Total time: %dm%02ds\n" $((_ELAPSED / 60)) $((_ELAPSED % 60))
echo ""
