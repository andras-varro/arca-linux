#!/bin/bash
#
# lib/arca-bundle.sh — the bundle layout that release.sh stages and publishes and
#                    that rootfs/commission-offline.sh installs.
#
# SOURCED, not executed:   . "$REPO_ROOT/lib/arca-bundle.sh"
#
# ── Layout ──────────────────────────────────────────────────────────────────
#
#   <dir>/root/<device-path>             the file, at its device path minus the
#                                        leading /  — so root/opt/games/snake is
#                                        installed to /opt/games/snake
#   <dir>/manifest.d/<component>.list    "<mode> <device-path>", one per line
#   <dir>/manifest.d/<component>.md5     "<md5>  <device-path>", one per line
#   <dir>/manifest.d/bundle.info         tag, build date, component list
#   <dir>/NOTICE                         licence obligations (ScummVM is GPL-3.0-or-later)
#
# ── ⚠️ A bundle carries BUILT ARTIFACTS ONLY, and that is a decision ──────────
#
# Deliberate, decided 2026-09-01: no device scripts go in.  A holder of nothing but
# the tarball therefore gets xpad.ko/joydev.ko/ff-memless.ko/devmem_write and nothing
# that loads them at boot, and gets no arca-app either.
# That is not a gap to fix.  rootfs/commission-offline.sh is the only consumer
# that installs those, it runs from a clone, so it has device-files/ beside it either
# way — a self-sufficient tarball would buy a second copy of them and a second path
# for an exec= to drift along.
#
# ⚠️ release.sh's refusal to publish CONFIG and VENDOR FIRMWARE is a different rule
# and stays whatever happens here: those keep a plaintext password and 5 MB of
# vendor kernel out of a public release.  A device script is neither.
# ── ⚠️ Modes are DECLARED, never read off disk ───────────────────────────────
#
# This repo lives on /mnt/c, which is DrvFs 9p: it reports every file
# -rwxrwxrwx and silently discards chmod (CLAUDE.md → "Working from this host").
# So `stat -c %a` here is a constant, not a measurement, and a bundle that
# derived its modes from disk would declare 777 for everything — then the
# installer would either apply that (wrong) or ignore it (and ship a launcher
# binary with no +x, which is the one failure that cannot be reproduced on this
# host at all).  Hence: the caller states the mode, and .list is the authority.
#
# ── Why md5 is a separate file from .list ────────────────────────────────────
#
# The caveat: base/version.o re-embeds the build date on every link, so releases
# are not byte-reproducible and the md5 list must be GENERATED per release rather
# than asserted against a known-good set.  Keeping it out of .list means the
# declared modes — which ARE stable — can be diffed between releases without the
# checksums making every line differ.
#
# ── The reserved "meta" component ────────────────────────────────────────────
#
# manifest.d/bundle.info records the tag, commit and build date.  release.sh also
# stages it AS AN ARTIFACT, at /opt/arca/bundle.info, so that a unit can
# answer "what am I running?" — before that, nothing on any device recorded it.
#
# An artifact needs a manifest, manifests are per-component, and this file belongs
# to no component.  Hence a reserved name.  arca_bundle_components EXCLUDES it, for
# two reasons: every caller of that function is telling an operator what the bundle
# contains, and a fifth entry there is a component that does not exist; and
# release.sh calls it to fill bundle.info's own `components=` line, which would
# otherwise depend on whether the stamp had been staged yet.
ARCA_BUNDLE_META_COMPONENT=meta

# The stamp's DEVICE path, in one place because four scripts need it and they do
# opposite things with it: release.sh stages it as an artifact, both installers
# write and md5-verify it as an ordinary manifest entry, and every SOURCE deploy
# removes it.  A literal repeated across those would let the writer and the remover
# drift onto different paths, which reads on the device as "the stamp survived a
# source deploy".
ARCA_BUNDLE_STAMP=/opt/arca/bundle.info

# ---------------------------------------------------------------------------
# arca_bundle_init DIR COMPONENT
#
# Prepare DIR and start (or restart) COMPONENT's manifests.  Truncating here
# rather than appending is deliberate: a re-run of a component's --bundle must
# not leave the previous run's entries behind, or a binary that was renamed stays
# in the manifest and the installer fails verification on a file nobody ships.
# ---------------------------------------------------------------------------
arca_bundle_init() {
    local dir="$1" comp="$2"
    [ -n "$dir" ]  || { echo "arca_bundle_init: no directory" >&2; return 1; }
    [ -n "$comp" ] || { echo "arca_bundle_init: no component name" >&2; return 1; }
    mkdir -p "$dir/root" "$dir/manifest.d" || return 1
    : > "$dir/manifest.d/$comp.list"
    : > "$dir/manifest.d/$comp.md5"
}

# ---------------------------------------------------------------------------
# arca_bundle_add DIR COMPONENT MODE LOCAL_FILE DEVICE_PATH
#
# Stage one file.  Refuses rather than warns on every input it cannot make
# correct, because a bundle that is silently missing a file installs cleanly and
# leaves a device with a launcher tile that does nothing.
# ---------------------------------------------------------------------------
arca_bundle_add() {
    local dir="$1" comp="$2" mode="$3" src="$4" dev="$5" dest

    [ -f "$src" ] || { echo "arca_bundle_add: no such file: $src" >&2; return 1; }

    # A 3- or 4-digit octal mode. Anything else is a typo, and a typo here is
    # applied verbatim by the installer's chmod.
    case "$mode" in
        [0-7][0-7][0-7]|[0-7][0-7][0-7][0-7]) ;;
        *) echo "arca_bundle_add: '$mode' is not a 3- or 4-digit octal mode" >&2; return 1 ;;
    esac

    # The device path must be absolute and must not be able to escape root/.
    # `..` is the whole reason this is checked: root/opt/../../etc/shadow is a
    # perfectly ordinary-looking manifest line that writes outside the tree.
    case "$dev" in
        /*) ;;
        *)  echo "arca_bundle_add: device path must be absolute: $dev" >&2; return 1 ;;
    esac
    case "$dev" in
        *//*|*/./*|*/../*|*/..) echo "arca_bundle_add: unsafe device path: $dev" >&2; return 1 ;;
    esac

    dest="$dir/root${dev}"
    mkdir -p "$(dirname "$dest")" || return 1
    cp "$src" "$dest" || return 1

    printf '%s %s\n' "$mode" "$dev" >> "$dir/manifest.d/$comp.list"
    printf '%s  %s\n' "$(md5sum "$src" | cut -d' ' -f1)" "$dev" >> "$dir/manifest.d/$comp.md5"
}

# ---------------------------------------------------------------------------
# arca_bundle_finish DIR COMPONENT
#
# Sort both manifests and echo the file count.  Sorted so that two bundles built
# from the same tree differ only where the artifacts differ — an unsorted list
# reorders whenever a glob's directory order changes, which makes every diff
# useless.
# ---------------------------------------------------------------------------
arca_bundle_finish() {
    local dir="$1" comp="$2" f
    for f in "$dir/manifest.d/$comp.list" "$dir/manifest.d/$comp.md5"; do
        [ -f "$f" ] || continue
        LC_ALL=C sort -k2 -o "$f" "$f"
    done
    grep -c . "$dir/manifest.d/$comp.list" 2>/dev/null || echo 0
}

# ---------------------------------------------------------------------------
# arca_bundle_components DIR
#
# Echo the BUILD component names present in DIR, one per line.  The reserved
# $ARCA_BUNDLE_META_COMPONENT manifest is skipped — it carries the provenance stamp
# rather than any component's artifacts, and every caller of this function is
# reporting the bundle's contents to an operator.  arca_bundle_entries deliberately
# does NOT skip it: the installers must still write and verify that file.
# ---------------------------------------------------------------------------
arca_bundle_components() {
    local dir="$1" f name
    for f in "$dir"/manifest.d/*.list; do
        [ -f "$f" ] || continue
        name=$(basename "$f" .list)
        [ "$name" = "${ARCA_BUNDLE_META_COMPONENT:-meta}" ] && continue
        echo "$name"
    done
}

# ---------------------------------------------------------------------------
# arca_bundle_entries DIR
#
# Echo "<mode> <device-path>" for every component, one per line, sorted.
# The installer's single source of what to write and with which mode.
# ---------------------------------------------------------------------------
arca_bundle_entries() {
    local dir="$1"
    cat "$dir"/manifest.d/*.list 2>/dev/null | grep . | LC_ALL=C sort -k2
}

# ---------------------------------------------------------------------------
# arca_bundle_check DIR
#
# Structural check, run before any install and at the end of every stage: every
# manifest line names a file that is actually under root/, and every file under
# root/ is named by some manifest line.  Both directions matter — the first
# catches a manifest entry whose cp failed, the second catches a file staged by
# hand that nothing will chmod.
#
# Echoes each problem; returns 1 if there were any.
# ---------------------------------------------------------------------------
arca_bundle_check() {
    local dir="$1" bad=0 mode dev f rel

    [ -d "$dir/root" ]       || { echo "  missing $dir/root"; return 1; }
    [ -d "$dir/manifest.d" ] || { echo "  missing $dir/manifest.d"; return 1; }

    while read -r mode dev; do
        [ -n "$dev" ] || continue
        if [ ! -f "$dir/root$dev" ]; then
            echo "  manifest names a file that is not staged: $dev"
            bad=$((bad + 1))
        fi
    done <<EOF
$(arca_bundle_entries "$dir")
EOF

    # The reverse direction. Built as a here-doc-fed loop rather than a pipe so
    # `bad` survives (a `find | while` body runs in a subshell).
    local manifest_paths
    manifest_paths=$(arca_bundle_entries "$dir" | awk '{print $2}')
    while read -r f; do
        [ -n "$f" ] || continue
        rel="${f#$dir/root}"
        if ! printf '%s\n' "$manifest_paths" | grep -qxF "$rel"; then
            echo "  staged but in no manifest: $rel"
            bad=$((bad + 1))
        fi
    done <<EOF
$(find "$dir/root" -type f 2>/dev/null)
EOF

    [ "$bad" -eq 0 ]
}

# ---------------------------------------------------------------------------
# arca_bundle_install_ssh TARGET DIR
#
# Install a staged bundle onto a running device over SSH.  The SSH twin of
# rootfs/commission-offline.sh's install loop, for a device that is already
# up and reachable.
#
# ── Why this has to exist ───────────────────────────────────────────────────
#
# `deploy-all.sh <ip>` BUILDS: it runs each component's build-and-deploy.sh, which
# needs the ARM cross-compilers (arm-linux-gnueabi-gcc; arm-linux-gnueabihf-gcc for
# vnc_client).  The person being delivered to has no toolchain by
# definition — that is what a release bundle is for — so until this existed the
# offline card path was the ONLY way to put binaries on a unit, even for someone
# who already had SSH to it.
#
# ── ⚠️ Modes are DECLARED by the manifest, not carried by the tar ───────────
#
# The stream is unpacked first and every mode applied afterwards from the `.list`.
# It cannot be the other way round: the staging directory usually lives on /mnt/c,
# which reports every file 0777 and discards chmod, so tar preserves a mode that was
# never a measurement.  `.list` is the authority — the same rule arca_bundle_add
# enforces on the write side.
#
# One ssh for the payload rather than one scp per file: 46 files over 46 SSH
# handshakes to a 600 MHz device is minutes of key exchange.
#
# ── How this is testable without a device ───────────────────────────────────
#
# $ARCA_SSH replaces the ssh command and $ARCA_BUNDLE_ROOT prefixes every device path,
# both empty in production. Together they let tests/arca_bundle_ssh_test.sh run the
# real function against a directory on this host — the same approach
# arca_provision_online_script's $ARCA_PROVISION_ROOT takes, and for the same reason:
# an installer whose only test is "it worked on a device once" has no test.
# ---------------------------------------------------------------------------
arca_bundle_install_ssh() {
    local target="$1" dir="$2" mode dev want got n=0 bad=0 m
    local SSHC="${ARCA_SSH:-ssh}" P="${ARCA_BUNDLE_ROOT:-}"

    [ -n "$target" ] || { echo "  arca_bundle_install_ssh: no target"; return 1; }
    [ -d "$dir" ]    || { echo "  arca_bundle_install_ssh: no such bundle dir: $dir"; return 1; }

    # Both directions, before anything is written: no manifest entry without a
    # staged file, and no staged file without an entry.  The second is the one that
    # catches a file added by hand that nothing will ever chmod.
    if ! arca_bundle_check "$dir"; then
        echo "  the bundle is not self-consistent — refusing to install"
        return 1
    fi

    n=$(arca_bundle_entries "$dir" | grep -c . || true)
    [ "$n" -gt 0 ] || { echo "  the bundle names no files at all"; return 1; }
    echo "  installing $n file(s) from $(basename "$dir") to $target"

    # The payload, in one connection.  -p is deliberately absent on extract: the
    # modes in the stream are meaningless (see above) and the loop below is the
    # authority.
    if ! tar -C "$dir/root" -cf - . | $SSHC "$target" "mkdir -p '${P:-/}'; tar -xf - -C '${P:-/}'"; then
        echo "  the payload transfer failed"
        return 1
    fi

    # Modes, from the manifest, in one remote shell.
    arca_bundle_entries "$dir" | while read -r mode dev; do
        [ -n "$dev" ] || continue
        printf "chmod %s '%s%s' || echo 'CHMODFAIL %s'\n" "$mode" "$P" "$dev" "$dev"
    done | $SSHC "$target" "sh -s" > "$dir/.chmod.out" 2>&1 || bad=1
    if grep -q CHMODFAIL "$dir/.chmod.out" 2>/dev/null; then
        sed -n 's/^CHMODFAIL /  could not chmod: /p' "$dir/.chmod.out"
        bad=1
    fi
    rm -f "$dir/.chmod.out"

    # md5 every installed file against the manifest, on the device.  Same assertion
    # rootfs/commission-offline.sh makes on the card, and the reason a truncated transfer
    # is caught here rather than by a blank screen later.
    local remote_md5 checked=0
    remote_md5=$(arca_bundle_entries "$dir" | awk -v p="$P" '{print p $2}' \
                   | $SSHC "$target" "xargs -n 50 md5sum 2>/dev/null" \
                   | sed "s| $P| |; s|  $P|  |")
    for m in "$dir"/manifest.d/*.md5; do
        [ -f "$m" ] || continue
        while read -r want dev; do
            [ -n "$dev" ] || continue
            checked=$((checked + 1))
            got=$(printf '%s\n' "$remote_md5" | awk -v d="$dev" '$2 == d { print $1; exit }')
            if [ -z "$got" ]; then
                echo "  not installed: $dev"; bad=1
            elif [ "$got" != "$want" ]; then
                echo "  md5 mismatch: $dev (want $want, got $got)"; bad=1
            fi
        done < "$m"
    done
    if [ "$checked" -ne "$n" ]; then
        echo "  md5 covered $checked of $n file(s) — the manifests do not cover the bundle"
        bad=1
    fi

    # ⚠️ +x is a MEASUREMENT here and cannot be one on the dev host.  A missing +x on
    # app_launcher is a black screen at boot and is invisible on /mnt/c.
    #
    # The OWNER digit, extracted by position: in a 4-digit mode "0755" it is char 2,
    # in a 3-digit "755" it is char 1.  Testing the last digit instead would ask
    # about `other`, so a deliberate 0700 would be reported as non-executable and a
    # 0644 whose owner bit was wrong would slip through.
    local xlist xbad
    xlist=$(arca_bundle_entries "$dir" | awk -v p="$P" '
        {
            m = $1
            o = (length(m) == 4) ? substr(m, 2, 1) : substr(m, 1, 1)
            if (o % 2 == 1) print p $2
        }')
    if [ -n "$xlist" ]; then
        xbad=$(printf '%s\n' "$xlist" \
                 | $SSHC "$target" "while read -r p; do [ -x \"\$p\" ] || echo \"\$p\"; done")
        if [ -n "$xbad" ]; then
            printf '%s\n' "$xbad" | sed "s|^$P||; s|^|  declared executable but is not: |"
            bad=1
        fi
    fi

    [ "$bad" = 0 ] || return 1
    echo "  $n file(s) installed, md5 verified, +x verified"
    return 0
}

# ---------------------------------------------------------------------------
# arca_bundle_clear_stamp TARGET
#
# Remove the provenance stamp from TARGET.  The ONE writer for that, shared by
# every source-deploy path, because four copies of an `ssh rm -f` is four chances
# for one of them to be forgotten — which is exactly what happened: the whole-tree
# deploy cleared the stamp and a single component's build-and-deploy.sh did not, so
# a unit could carry a stamp naming a release whose bytes had been overwritten.
#
# Quiet on success, and it does NOT distinguish "removed" from "there was nothing
# there".  `rm -f` cannot, and the distinction has no consumer: an absent stamp
# means "this unit is not running a published release" either way.  A nonzero
# return therefore means the SSH call itself failed, which is the only case a
# caller has anything to say about.
#
# ⚠️ Call it BEFORE writing any files, not after.  Clearing early can only lose
# information — a stamp removed by a deploy that then fails understates what is on
# the unit.  Clearing late leaves a window in which fresh binaries sit under a
# stamp that already names the wrong bytes, and that window is a lie rather than a
# gap.  It is also why this is not gated on the deploy succeeding.
#
# $ARCA_SSH and $ARCA_BUNDLE_ROOT are honoured for the same reason
# arca_bundle_install_ssh honours them: tests/arca_bundle_ssh_test.sh runs the real
# function against a directory on this host.
# ---------------------------------------------------------------------------
arca_bundle_clear_stamp() {
    local target="$1"
    local SSHC="${ARCA_SSH:-ssh}" P="${ARCA_BUNDLE_ROOT:-}"

    [ -n "$target" ] || { echo "  arca_bundle_clear_stamp: no target"; return 1; }
    $SSHC "$target" "rm -f '$P$ARCA_BUNDLE_STAMP'"
}
