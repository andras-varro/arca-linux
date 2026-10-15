#!/bin/bash
#
# lib/arca-release.sh — fetch a published release bundle from GitHub.
#
# SOURCED, not executed:   . "$REPO_ROOT/lib/arca-release.sh"
#
# ── Why this is a separate file from arca-bundle.sh ────────────────────────────
#
# arca-bundle.sh is the bundle LAYOUT, and it is deliberately network-free: it is
# sourced by four component build scripts on the write side and by the offline
# commissioner on the read side, and neither has any business opening a socket.
# This file is the one library in lib/ that does, so "does this code touch the
# network" is answerable by looking at the source list.
#
# It is also THIN on purpose.  deploy-all.sh --from-bundle and
# rootfs/commission-offline.sh --bundle both already accept either a
# tarball or an unpacked directory, install from the manifest and verify md5.  So
# a fetch is: resolve, download, verify, hand over the path.  Nothing here
# installs anything, and nothing here touches a device.
#
# ── ⚠️ Every gh call passes --repo, and it is NOT optional ───────────────────
#
# gh cannot resolve this repository from its own remote.  `origin` is an SSH host
# ALIAS (git@github.com-personal:owner/repo.git), and gh does not merely fail to
# guess the owner from it — it refuses the repository outright with "none of the
# git remotes configured for this repository point to a known GitHub host".
# Measured, not anticipated.  arca_release_repo below is the one derivation; it is
# what release.sh publishes through, so a fork fetches from the fork.
#
# ⚠️ It reads the remote URL and NOTHING ELSE from git.  `git ls-remote` and every
# other network git operation dies here with "Could not resolve hostname
# github.com-personal": the alias lives in the WINDOWS ssh config and these
# scripts run under WSL.  `remote get-url` is local, so it works.  Do not add a
# network git call to this file.
#
# ── ⚠️ sha256, never md5 ─────────────────────────────────────────────────────
#
# The bundle's own manifest.d/*.md5 proves the bundle is internally consistent
# once it is unpacked — it says nothing about whether the bytes that arrived are
# the bytes GitHub served, and nothing is signed.  The one authenticity check
# available is the published asset digest, and GitHub reports that as
# `sha256:…` — on `gh release view --json assets` AND on the unauthenticated REST
# API (measured 2026-09-01, both).  So a fetched asset is verified with
# sha256sum.  Comparing the manifest's md5 against that digest compares two
# different algorithms and can never agree.
#
# ── ⚠️ gh prints NO progress, so this file does ──────────────────────────────
#
# Measured 2026-09-01: `gh release download` moved all 131,434,339 bytes of
# v1.0.0 with ZERO bytes on stdout and zero on stderr.  The bundle is ~126 MB,
# most of it uncompressed mono music beds, so on any ordinary link a silent
# download is minutes of a dead terminal — which reads as a hang, and gets
# Ctrl-C'd.  _arca_release_progress polls the partial file against the size the
# resolve step already knows and prints one updating line.  It wraps curl too,
# rather than relying on curl's own bar, so both transports look the same.
#
# ── ⚠️ No jq ─────────────────────────────────────────────────────────────────
#
# jq is NOT installed in this WSL (measured 2026-09-01) and cannot be assumed on
# a commissioning host either.  gh has its own --jq built in, which is why the gh
# path can ask for exactly the five fields it wants; the curl fallback parses with
# grep and sed, and refuses rather than guessing when a release carries more than
# one tarball (see arca_release_resolve).
#
# ── ⚠️ stdout carries the ANSWER and nothing else ────────────────────────────
#
# Callers do `path="$(arca_release_fetch …)"`, which runs this in a subshell — so
# every diagnostic, every progress line and every warning goes to stderr, and any
# global set in here is discarded.  Same trap as arca_ssh_probe (lib/CLAUDE.md);
# here it is avoided by having no globals to lose.

# ---------------------------------------------------------------------------
# arca_release_repo [REPO_DIR]
#
# Print "owner/repo" derived from REPO_DIR's origin remote.  Derived rather than
# hardcoded, so a fork fetches from and publishes to the fork.
#
# Strips any scheme, any user@host prefix, and the trailing .git — which handles
# all three forms this repo has been cloned as: the SSH host alias
# git@github.com-personal:owner/repo.git, plain git@github.com:owner/repo.git,
# and https://github.com/owner/repo.
# ---------------------------------------------------------------------------
arca_release_repo() {
    local dir="${1:-.}" url repo
    url="$(git -C "$dir" remote get-url origin 2>/dev/null || true)"
    [ -n "$url" ] || { echo "arca_release_repo: no origin remote in $dir" >&2; return 1; }
    repo="$(echo "$url" | sed -e 's#^[a-z+]*://##' -e 's#^[^/@]*@##' -e 's#^[^:/]*[:/]##' -e 's#\.git$##')"
    case "$repo" in
        */*) echo "$repo" ;;
        *)   echo "arca_release_repo: could not derive owner/repo from origin ('$url')" >&2; return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# arca_release_cache_dir [REPO_DIR]
#
# Where fetched tarballs are kept.  One implementation so the front doors name
# one location, exactly as build/release is the one staging location.  build/ is
# gitignored, so a 126 MB cache never becomes a commit.
# ---------------------------------------------------------------------------
arca_release_cache_dir() {
    local dir="${1:-.}"
    if [ -n "${ARCA_RELEASE_CACHE:-}" ]; then echo "$ARCA_RELEASE_CACHE"; else echo "$dir/build/release-cache"; fi
}

# ---------------------------------------------------------------------------
# arca_release_resolve REPO [TAG]
#
# Print one tab-separated line for the bundle asset of TAG (or of the latest
# release when TAG is empty or "latest"):
#
#     <tag>\t<asset-name>\t<size-bytes>\t<sha256>\t<download-url>
#
# gh first, because it is the only path that works for a private repo; the
# unauthenticated REST API second, which is enough for this repo because it is
# public.  gh is skipped rather than trusted when it is present but not logged
# in — an unauthenticated gh fails on a public repo where plain curl succeeds.
#
# ⚠️ Refuses a release with no digest, and refuses one carrying more than one
# tarball.  An asset that cannot be verified is not a safer install than no
# install, and picking one of several tarballs silently would put a different set
# of components on the device than the operator asked for.  GitHub's auto-made
# "Source code (tar.gz)" is not an asset, so it does not trip the second rule.
# ---------------------------------------------------------------------------
arca_release_resolve() {
    local repo="$1" tag="${2:-}" line count digest
    [ -n "$repo" ] || { echo "arca_release_resolve: no repo" >&2; return 1; }
    [ "$tag" = "latest" ] && tag=""

    line=""
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        # shellcheck disable=SC2086  # $tag must vanish entirely when empty: `gh
        # release view ""` is an error, while no argument at all means "latest".
        line="$(gh release view $tag --repo "$repo" \
                    --json tagName,assets \
                    --jq '.tagName as $t | .assets[]
                          | select(.name | endswith(".tar.gz"))
                          | "\($t)\t\(.name)\t\(.size)\t\(.digest)\t\(.url)"' 2>/dev/null || true)"
    fi

    if [ -z "$line" ]; then
        command -v curl >/dev/null 2>&1 || {
            echo "arca_release_resolve: neither an authenticated gh nor curl is available" >&2
            return 1
        }
        local api json flat chunks url name size rtag
        if [ -n "$tag" ]; then api="https://api.github.com/repos/$repo/releases/tags/$tag"
        else                   api="https://api.github.com/repos/$repo/releases/latest"; fi
        json="$(curl -fsSL -H "Accept: application/vnd.github+json" "$api" 2>/dev/null || true)"
        [ -n "$json" ] || {
            echo "arca_release_resolve: no release ${tag:-latest} on $repo (and gh could not be used)" >&2
            return 1
        }
        # Flatten, then split on commas so each scalar field lands on its own
        # line.  Splitting on braces would not work: every asset object nests an
        # `uploader` object, so a brace split cuts one asset into two pieces and
        # separates its name from its digest.
        flat="$(printf '%s' "$json" | tr -d '\n')"
        chunks="$(printf '%s' "$flat" | tr ',' '\n')"
        rtag="$(printf '%s\n' "$chunks"   | grep -m1 -o '"tag_name": *"[^"]*"' | sed 's/^[^:]*: *"//; s/"$//')"
        url="$(printf '%s\n' "$chunks"    | grep -o '"browser_download_url": *"[^"]*\.tar\.gz"' | sed 's/^[^:]*: *"//; s/"$//')"
        digest="$(printf '%s\n' "$chunks" | grep -o '"digest": *"sha256:[0-9a-f]\{64\}"' | sed 's/.*sha256:\([0-9a-f]\{64\}\).*/\1/')"
        size="$(printf '%s\n' "$chunks"   | grep -o '"size": *[0-9]\{1,\}' | sed 's/[^0-9]*//')"
        count="$(printf '%s\n' "$url" | grep -c . || true)"
        [ "$count" = "1" ] || {
            echo "arca_release_resolve: ${tag:-latest} on $repo carries $count tarball assets." >&2
            echo "  Without jq this fallback cannot pair a digest to one of several assets." >&2
            echo "  Install gh and 'gh auth login', or download the one you want by hand." >&2
            return 1
        }
        # One asset, so the single digest and size found belong to it.
        name="${url##*/}"
        line="$(printf '%s\t%s\t%s\t%s\t%s' "$rtag" "$name" "$size" "$digest" "$url")"
    fi

    count="$(printf '%s\n' "$line" | grep -c . || true)"
    [ "$count" = "1" ] || {
        echo "arca_release_resolve: expected one .tar.gz asset on ${tag:-latest}, found $count" >&2
        printf '%s\n' "$line" | cut -f2 | sed 's/^/    /' >&2
        return 1
    }

    # sha256: stripped here so callers compare bare hex against sha256sum.
    digest="$(printf '%s' "$line" | cut -f4)"
    digest="${digest#sha256:}"
    case "$digest" in
        *[^0-9a-f]*|"") digest="" ;;
        ????????????????????????????????????????????????????????????????) ;;
        *) digest="" ;;
    esac
    [ -n "$digest" ] || {
        echo "arca_release_resolve: ${tag:-latest} publishes no sha256 digest for" \
             "$(printf '%s' "$line" | cut -f2) — nothing to verify a download against." >&2
        echo "  Refusing rather than installing unverified ARM binaries." >&2
        return 1
    }
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "$(printf '%s' "$line" | cut -f1)" \
        "$(printf '%s' "$line" | cut -f2)" \
        "$(printf '%s' "$line" | cut -f3)" \
        "$digest" \
        "$(printf '%s' "$line" | cut -f5)"
}

# ---------------------------------------------------------------------------
# arca_release_sha256 FILE
#
# Print FILE's sha256, or fail loudly if no tool can compute one.  A missing
# hasher must not read as a passing check — it is the same shape as the `strings`
# trap in CLAUDE.md, where an absent tool answered a question about the device.
# ---------------------------------------------------------------------------
arca_release_sha256() {
    local f="$1"
    if   command -v sha256sum >/dev/null 2>&1; then sha256sum "$f" | cut -d' ' -f1
    elif command -v shasum    >/dev/null 2>&1; then shasum -a 256 "$f" | cut -d' ' -f1
    else echo "arca_release_sha256: no sha256sum and no shasum — cannot verify a download" >&2; return 1
    fi
}

# ---------------------------------------------------------------------------
# _arca_release_progress FILE EXPECTED_BYTES PID
#
# One updating line on stderr while PID is alive.  See the header: gh prints
# nothing at all, and a silent multi-minute download reads as a hang.
# ---------------------------------------------------------------------------
_arca_release_progress() {
    local f="$1" want="$2" pid="$3" have pct
    while kill -0 "$pid" 2>/dev/null; do
        have="$(stat -c %s "$f" 2>/dev/null || echo 0)"
        if [ "${want:-0}" -gt 0 ] 2>/dev/null; then
            pct=$(( have * 100 / want ))
            printf '\r      %4d MB of %4d MB (%3d%%)' \
                   $(( have / 1048576 )) $(( want / 1048576 )) "$pct" >&2
        else
            printf '\r      %4d MB' $(( have / 1048576 )) >&2
        fi
        sleep 1
    done
    printf '\r%-44s\r' " " >&2
}

# ---------------------------------------------------------------------------
# arca_release_fetch REPO TAG CACHE_DIR
#
# Print the path of a verified local copy of TAG's bundle tarball on stdout.
# Everything else — progress, warnings, what it decided — goes to stderr.
#
# Cached BY DIGEST: a file already at the cache path whose sha256 equals the
# published one is the same bytes GitHub would serve, so the download is skipped.
# That is what makes re-running a menu item, or deploying to a second unit, free
# rather than another 126 MB.  A cached file whose digest does NOT match is
# re-downloaded rather than trusted — it is a truncated earlier run, or a
# republished tag.
#
# Downloads to a temp name in the cache directory and moves it into place only
# after verification, so an interrupted run never leaves a short file where the
# next run would find it, and a mismatch never poisons the cache.
# ---------------------------------------------------------------------------
arca_release_fetch() {
    local repo="$1" tag="${2:-}" cache="$3"
    [ -n "$repo" ]  || { echo "arca_release_fetch: no repo" >&2; return 1; }
    [ -n "$cache" ] || { echo "arca_release_fetch: no cache directory" >&2; return 1; }

    local meta rtag name size want url dest tmp got dlpid
    meta="$(arca_release_resolve "$repo" "$tag")" || return 1
    rtag="$(printf '%s' "$meta" | cut -f1)"
    name="$(printf '%s' "$meta" | cut -f2)"
    size="$(printf '%s' "$meta" | cut -f3)"
    want="$(printf '%s' "$meta" | cut -f4)"
    url="$(printf  '%s' "$meta" | cut -f5)"

    echo "  → $repo $rtag: $name ($(( size / 1048576 )) MB)" >&2
    mkdir -p "$cache/$rtag" || return 1
    dest="$cache/$rtag/$name"

    if [ -f "$dest" ]; then
        got="$(arca_release_sha256 "$dest")" || return 1
        if [ "$got" = "$want" ]; then
            echo "  ✓ already downloaded and sha256 matches — not fetching again" >&2
            echo "    $dest" >&2
            printf '%s\n' "$dest"
            return 0
        fi
        echo "  ! cached copy does not match the published digest — re-downloading" >&2
    fi

    tmp="$dest.part"
    rm -f "$tmp"
    echo "  → downloading (progress below; gh itself prints none)" >&2
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        # --clobber because gh refuses an existing target outright (measured), and
        # --output so the temp name is ours rather than the asset's.
        gh release download "$rtag" --repo "$repo" --pattern "$name" \
           --output "$tmp" --clobber >/dev/null 2>&1 &
    else
        curl -fsSL -o "$tmp" "$url" &
    fi
    dlpid=$!
    _arca_release_progress "$tmp" "$size" "$dlpid"
    wait "$dlpid" || { rm -f "$tmp"; echo "  ✗ download failed: $url" >&2; return 1; }

    got="$(arca_release_sha256 "$tmp")" || { rm -f "$tmp"; return 1; }
    if [ "$got" != "$want" ]; then
        rm -f "$tmp"
        echo "  ✗ sha256 MISMATCH — the bytes that arrived are not what GitHub published." >&2
        echo "      expected $want" >&2
        echo "      got      $got" >&2
        echo "    The partial file was deleted. Nothing was installed." >&2
        return 1
    fi
    mv -f "$tmp" "$dest" || return 1

    # A fetch reached under `sudo commission-offline.sh --release` writes into the
    # operator's own build/ as root, and everything they run afterwards then needs
    # sudo to touch its own cache.  Same reasoning as arca_ssh_key_owner: hand it
    # back.  Failure is not fatal — the tarball is correct either way.
    if [ "$(id -u)" = "0" ] && [ -n "${SUDO_UID:-}" ]; then
        chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$dest" "$cache/$rtag" "$cache" 2>/dev/null || true
    fi

    echo "  ✓ downloaded and sha256 verified against the published digest" >&2
    echo "    $dest" >&2
    printf '%s\n' "$dest"
}
