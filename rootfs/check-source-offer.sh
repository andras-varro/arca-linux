#!/bin/bash
# check-source-offer.sh - Grade a source offer assembled by make-source-offer.sh.
#
# Usage: rootfs/check-source-offer.sh <offer-dir | offer.tar> [--rootfs-tar <tar> | --tree <dir>] [--p1-dir <dir>]
#
# Also sourced by make-source-offer.sh, which calls check_source_offer on its staging
# directory before it writes the tar.
#
# What it grades:
#   1. the files every offer carries are present;
#   2. every SOURCES.csv row names a member that exists, is non-empty, and has the sha256
#      the row records;
#   3. every row's hash or commit equals the one the component's own build script pins,
#      read out of the repo archive INSIDE the offer, not from a copy in this script;
#   4. every package in Buildroot's manifest.csv has source in legal-info/sources/ and
#      legal-info.sha256 verifies what is there;
#   5. with --rootfs-tar or --tree (and --p1-dir): every ELF, shared object and kernel
#      module on the image, and every boot file on p1, resolves to a source row, either
#      through Buildroot's packages-file-list.txt or through the repo's
#      rootfs/source-map.tsv. A file that resolves to nothing fails.
#
# Exit 0 pass, 1 an item failed, 2 the check could not judge (bad usage, an offer with
# nothing in it, a pin check that inspected nothing).

# pin_hex <file> <needle> <digits>: the first hex string of exactly that many digits on a
# line containing the needle. Empty output means the file does not pin it.
pin_hex() {
    grep -F -- "$2" "$1" 2>/dev/null | grep -owE "[0-9a-f]{$3}" | head -n 1
}

_cso_usage() {
    cat <<'USAGE'
Usage: rootfs/check-source-offer.sh <offer-dir | offer.tar> [--rootfs-tar <tar> | --tree <dir>] [--p1-dir <dir>]

Grades a source offer written by rootfs/make-source-offer.sh: members present and
hashed, hashes equal to the pins in the build scripts, Buildroot's legal-info complete,
and (with an image given) every program on the image mapped to a source row.
Exit 0 pass, 1 a check failed, 2 could not judge. Reads only; writes nothing but a
temporary directory.
USAGE
}

# check_source_offer <offer dir> [--rootfs-tar F | --tree D] [--p1-dir D]
check_source_offer() {
    local offer="$1"; shift
    local rootfs_tar="" tree="" p1dir=""
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --rootfs-tar) rootfs_tar="$2"; shift 2 ;;
            --tree)       tree="$2"; shift 2 ;;
            --p1-dir)     p1dir="$2"; shift 2 ;;
            *) echo "check-source-offer: unknown argument $1" >&2; return 2 ;;
        esac
    done
    [ -d "$offer" ] || { echo "check-source-offer: $offer is not a directory" >&2; return 2; }

    local fails=0 tmp
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/rw-cso.XXXXXX")" || return 2
    _fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

    # 1. required files
    local f
    for f in README-SOURCE.txt SOURCES.csv BUILD-ENV.txt image-md5.txt buildroot/legal-info/manifest.csv \
             buildroot/legal-info/legal-info.sha256 licenses/LICENCE.rtlwifi_firmware.txt; do
        [ -s "$offer/$f" ] || _fail "missing or empty: $f"
    done
    if [ ! -s "$offer/SOURCES.csv" ]; then
        rm -rf "$tmp"; echo "check-source-offer: no SOURCES.csv: nothing to judge" >&2; return 2
    fi

    # 2. members and their hashes (the header line is skipped)
    local rows=0 comp member sum pin got
    while IFS=, read -r comp _ _ _ member _ sum _; do
        rows=$((rows + 1))
        if [ ! -s "$offer/$member" ]; then _fail "$comp: member $member is missing or empty"; continue; fi
        got="$(sha256sum "$offer/$member" | cut -d' ' -f1)"
        [ "$got" = "$sum" ] || _fail "$comp: $member has sha256 $got, SOURCES.csv says $sum"
    done < <(tail -n +2 "$offer/SOURCES.csv")
    [ "$rows" -gt 0 ] || { rm -rf "$tmp"; echo "check-source-offer: SOURCES.csv has no rows" >&2; return 2; }

    # 3. pins, read from the repo archive inside the offer. One pass collects the paths.
    local repo_tar
    repo_tar=""; for f in "$offer"/repo/*.tar.xz; do [ -f "$f" ] && { repo_tar="$f"; break; }; done
    if [ -z "$repo_tar" ]; then
        _fail "no repo archive under repo/"
    else
        local want_paths="*/rootfs/source-map.tsv" kind a b
        while IFS=, read -r _ _ _ _ member _ _ pin; do
            kind="${pin%%:*}"; a="${pin#*:}"; a="${a%%:*}"
            case "$kind" in grep|content) want_paths="$want_paths */$a" ;; esac
        done < <(tail -n +2 "$offer/SOURCES.csv")
        mkdir -p "$tmp/repo"
        # shellcheck disable=SC2086
        tar -xJf "$repo_tar" -C "$tmp/repo" --strip-components=1 --wildcards --no-wildcards-match-slash $want_paths 2>/dev/null || true
    fi
    local pinned=0 want
    while IFS=, read -r comp _ _ _ member _ sum pin; do
        kind="${pin%%:*}"; a="${pin#*:}"; b="${a#*:}"; a="${a%%:*}"
        got="$(sha256sum "$offer/$member" 2>/dev/null | cut -d' ' -f1)"
        case "$kind" in
            none) ;;
            grep)
                want="$(pin_hex "$tmp/repo/$a" "$b" 64)"
                if [ -z "$want" ]; then _fail "$comp: $a pins no sha256 near '$b'"
                elif [ "$want" != "$got" ]; then _fail "$comp: $member is $got, $a pins $want"
                else pinned=$((pinned + 1)); fi ;;
            dsc)
                want="$(pin_hex "$offer/$a" "$b" 64)"
                if [ -z "$want" ]; then _fail "$comp: $a lists no sha256 for $b"
                elif [ "$want" != "$got" ]; then _fail "$comp: $member is $got, $a lists $want"
                else pinned=$((pinned + 1)); fi ;;
            content)
                want="$(pin_hex "$tmp/repo/$a" "$b" 40)"
                got="$(tr -d '[:space:]' <"$offer/$member")"
                if [ -z "$want" ]; then _fail "$comp: $a pins no commit near '$b'"
                elif [ "$want" != "$got" ]; then _fail "$comp: $member holds $got, $a pins $want"
                else pinned=$((pinned + 1)); fi ;;
            *) _fail "$comp: unknown pin kind '$pin'" ;;
        esac
    done < <(tail -n +2 "$offer/SOURCES.csv")
    if [ "$pinned" -eq 0 ]; then
        rm -rf "$tmp"; echo "check-source-offer: no pin was checked: the pin check inspected nothing" >&2; return 2
    fi
    echo "pins checked: $pinned"

    # 4. Buildroot legal-info
    local li="$offer/buildroot/legal-info" pkg pver n=0
    if [ -s "$li/manifest.csv" ]; then
        while IFS=, read -r pkg pver _; do
            pkg="${pkg//\"/}"; pver="${pver//\"/}"
            [ "$pkg" = "PACKAGE" ] && continue
            n=$((n + 1))
            if [ -z "$(ls -A "$li/sources/$pkg-$pver" 2>/dev/null)" ]; then
                _fail "legal-info: no source for $pkg $pver"
            elif [ -f "$li/sources/$pkg-$pver/POINTER.txt" ]; then
                [ -e "$li/sources/$pkg-$pver/$(sed -n 's/^see //p' "$li/sources/$pkg-$pver/POINTER.txt")" ] \
                    || _fail "legal-info: the pointer for $pkg $pver leads nowhere"
            fi
        done <"$li/manifest.csv"
        [ "$n" -gt 0 ] || _fail "legal-info: manifest.csv lists no package"
        local line sumfile ok=0
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            sumfile="${line#*  }"
            if [ ! -e "$li/$sumfile" ]; then
                case "$sumfile" in
                    host-sources/*|host-licenses/*|host-manifest.csv|sources/linux-headers-*) ;;
                    *) _fail "legal-info.sha256 names $sumfile, which is not in the offer" ;;
                esac
            elif (cd "$li" && printf '%s\n' "$line" | sha256sum -c --quiet - >/dev/null 2>&1); then
                ok=$((ok + 1))
            else
                _fail "legal-info.sha256: $sumfile does not verify"
            fi
        done <"$li/legal-info.sha256"
        [ "$ok" -gt 0 ] || _fail "legal-info.sha256 verified nothing"
        echo "legal-info: $n packages, $ok files verified"
    fi

    # 5. the image
    if [ -n "$rootfs_tar$tree$p1dir" ]; then
        local root="$tmp/root" listing="$tmp/files.txt" intree="skeleton skeleton-init-common skeleton-init-sysv initscripts ifupdown-scripts urandom-scripts toolchain"
        mkdir -p "$root"
        if [ -n "$rootfs_tar" ]; then
            tar -xf "$rootfs_tar" -C "$root" 2>/dev/null || { _fail "cannot unpack $rootfs_tar"; }
        fi
        [ -z "$tree" ] || cp -a "$tree"/. "$root"/ 2>/dev/null || true
        [ -z "$p1dir" ] || { mkdir -p "$root/.p1"; cp -a "$p1dir"/. "$root/.p1"/ 2>/dev/null || true; }
        # every candidate: ELF magic or a .ko, regular files only; p1 files are all candidates
        : >"$listing"
        local p magic mapped count=0
        while IFS= read -r -d '' p; do
            case "$p" in
                *.ko) echo "${p#"$root"/}" >>"$listing" ;;
                *) magic="$(head -c 4 "$p" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
                   [ "$magic" = "7f454c46" ] && echo "${p#"$root"/}" >>"$listing" ;;
            esac
        done < <(find "$root" -type f -not -path "$root/.p1/*" -print0 2>/dev/null)
        [ ! -d "$root/.p1" ] || (cd "$root/.p1" && find . -type f | sed 's|^\./|p1/|') >>"$listing"
        count="$(wc -l <"$listing")"
        if [ "$count" -eq 0 ]; then
            _fail "the image given holds no program to map (nothing inspected)"
        fi
        local flist="$offer/buildroot/packages-file-list.txt" comps="" row_comp
        comps="$(tail -n +2 "$offer/SOURCES.csv" | cut -d, -f1 | tr '\n' ' ') buildroot"
        local mapfile="$tmp/repo/rootfs/source-map.tsv" unmapped=0 gl pk
        [ -s "$mapfile" ] || _fail "the repo archive carries no rootfs/source-map.tsv"
        while IFS= read -r p; do
            mapped=0
            # (a) the package that installed it
            if [ -s "$flist" ]; then
                pk="$(grep -F -m1 ",./$p" "$flist" | cut -d, -f1 || true)"
                if [ -n "$pk" ]; then
                    if grep -q "^\"$pk\"," "$li/manifest.csv" 2>/dev/null || [[ " $intree " == *" $pk "* ]]; then mapped=1; fi
                fi
            fi
            # (b) the table for what Buildroot did not build
            if [ "$mapped" -eq 0 ] && [ -s "$mapfile" ]; then
                while IFS=$'\t' read -r gl row_comp; do
                    case "$gl" in ""|\#*) continue ;; esac
                    # shellcheck disable=SC2254
                    case "$p" in $gl)
                        if [[ " $comps " == *" $row_comp "* ]]; then mapped=1; break; fi ;;
                    esac
                done <"$mapfile"
            fi
            if [ "$mapped" -eq 0 ]; then _fail "no source row for $p"; unmapped=$((unmapped + 1)); fi
        done <"$listing"
        echo "image: $count programs inspected, $unmapped unmapped"
    fi

    rm -rf "$tmp"
    if [ "$fails" -gt 0 ]; then echo "check-source-offer: $fails failure(s)"; return 1; fi
    echo "check-source-offer: PASS"
    return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -uo pipefail
    case "${1:-}" in
        ""|-h|--help) _cso_usage; [ -n "${1:-}" ] && exit 0; exit 2 ;;
    esac
    src="$1"; shift
    work=""
    if [ -f "$src" ]; then
        work="$(mktemp -d "${TMPDIR:-/tmp}/rw-cso-tar.XXXXXX")"
        tar -xf "$src" -C "$work" || { echo "check-source-offer: cannot unpack $src" >&2; rm -rf "$work"; exit 2; }
        for src in "$work"/*/; do [ -d "$src" ] && break; done
        src="${src%/}"
    fi
    check_source_offer "$src" "$@"
    rc=$?
    [ -z "$work" ] || rm -rf "$work"
    exit "$rc"
fi
