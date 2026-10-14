#!/bin/bash
#
# arca-kernel-src.sh — refuse a kernel source tarball whose sha256 is not the pinned one.
#
# Sourced, never executed:   . "$REPO_DIR/lib/arca-kernel-src.sh"
#
#   arca_check_kernel_tarball <tarball> [<hash file>]
#
# The pin has one home: the `sha256  <hex>  <basename>` line in rootfs/package-hashes/linux-headers.hash
# (the default hash file). Nothing here carries the hex. The check is run BEFORE `tar xf`, so a
# corrupt or substituted tarball is never unpacked into a build tree.
#
# Returns 0 only when the tarball exists, its pin line exists, the pin is a well-formed sha256, and
# the computed digest equals it. Every other case returns 1 with a message on stderr: a missing
# file, a missing or ambiguous pin line, a malformed pin, a digest that could not be computed, or a
# mismatch. An empty answer from any step is a failure, never a pass.

arca_check_kernel_tarball() {
    local tarball="${1:-}" hashfile="${2:-}"
    local repo name pins expected sum_line got

    if [ -z "$tarball" ]; then
        echo "arca_check_kernel_tarball: no tarball given" >&2
        return 1
    fi
    if [ -z "$hashfile" ]; then
        repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
        hashfile="$repo/rootfs/package-hashes/linux-headers.hash"
    fi
    if [ ! -f "$tarball" ]; then
        echo "arca_check_kernel_tarball: missing file: $tarball" >&2
        return 1
    fi
    if [ ! -r "$hashfile" ]; then
        echo "arca_check_kernel_tarball: pin file not readable: $hashfile" >&2
        return 1
    fi

    name="$(basename "$tarball")"
    # Exactly one pin line for this name. Zero is "no pin", two is ambiguous; both refuse.
    pins="$(tr -d '\r' < "$hashfile" | awk -v n="$name" '$1 == "sha256" && $3 == n { print $2 }')"
    case "$pins" in
        "") echo "arca_check_kernel_tarball: no sha256 pin for $name in $hashfile -- refusing to extract" >&2
            return 1 ;;
        *$'\n'*) echo "arca_check_kernel_tarball: more than one sha256 pin for $name in $hashfile -- refusing" >&2
            return 1 ;;
    esac
    expected="$pins"
    if [ "${#expected}" -ne 64 ] || [ -n "$(printf '%s' "$expected" | tr -d '0-9a-f')" ]; then
        echo "arca_check_kernel_tarball: pin for $name is not a lowercase sha256 hex in $hashfile" >&2
        return 1
    fi

    if ! sum_line="$(sha256sum -- "$tarball")"; then
        echo "arca_check_kernel_tarball: sha256sum failed on $tarball" >&2
        return 1
    fi
    got="${sum_line%% *}"
    if [ "${#got}" -ne 64 ] || [ -n "$(printf '%s' "$got" | tr -d '0-9a-f')" ]; then
        echo "arca_check_kernel_tarball: sha256sum printed no digest for $tarball -- refusing" >&2
        return 1
    fi

    if [ "$got" != "$expected" ]; then
        echo "arca_check_kernel_tarball: sha256 MISMATCH for $tarball" >&2
        echo "  expected: $expected  (from $hashfile)" >&2
        echo "  got:      $got" >&2
        echo "  the file is not the pinned source; delete it and re-run to re-download" >&2
        return 1
    fi
    return 0
}
