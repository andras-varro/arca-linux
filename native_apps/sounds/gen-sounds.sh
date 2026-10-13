#!/bin/bash
#
# gen-sounds.sh — rebuild the stock sound effects from fx_gen.c.
#
# ⚠️ **RETIRED, AND IT OVERWRITES THE SHIPPED EFFECTS.** The ten fx_*.wav are SOURCED files now, not this
# generator's output: its gate enforced "broadband and transient", the operator heard the result as white
# noise on the panel and on a PC, and the property that actually matters is energy inside the speaker's
# usable band rather than spectral flatness. Running this replaces every sourced clip with generated noise
# and the loss is silent. See prompts.md for how the shipped set is authored.
#
# HOST ONLY. No device, no cross-compiler, no listen.
#
# ⚠️ **The self-test runs FIRST and a failure stops the script.** Its gate makes
# "broadband and transient" a checked property, and a gate that has only ever been
# seen passing is not evidence (../../CLAUDE.md → *Working style*). If its four
# controls stop coming out 1-pass / 3-reject, nothing it writes can be trusted.
# ⚠️ That gate is also why this script is retired: the checked property was the
# wrong one, so the numbers were green and the sounds were unusable.
#
# fx_gen is deterministic by construction (a fixed xorshift32 seed per effect,
# never rand()), so what it writes is reviewable — but it no longer matches the
# committed .wav files, and a clean `git status` after a run is NOT expected now.
# The effects this script installs are deployed by ../build-and-deploy.sh.
# Run from WSL (this host has no gcc in Git Bash — root ../../CLAUDE.md):
#   wsl.exe -e bash -lc "cd /mnt/c/work/arca/native_apps/sounds && ./gen-sounds.sh"

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CC="${CC:-gcc}"
BIN="${TMPDIR:-/tmp}/fx_gen"

usage() {
    cat <<'USAGE'
gen-sounds.sh — rebuild native_apps/sounds/fx_*.wav from fx_gen.c

  ./gen-sounds.sh              build fx_gen, run its gate self-test, regenerate
  ./gen-sounds.sh --check      self-test only; writes nothing, touches no .wav
  ./gen-sounds.sh --help       this text

Output is mono / 44100 / 16-bit — the mix bus's internal format, so nothing at
runtime resamples. Deterministic: re-running with no source change leaves every
file byte-identical.
USAGE
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    --check)   CHECK_ONLY=1 ;;
    "")        CHECK_ONLY=0 ;;
    *)         echo "gen-sounds.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
esac

command -v "$CC" >/dev/null 2>&1 || {
    echo "gen-sounds.sh: no '$CC' on PATH." >&2
    echo "  ⚠️ If this is Git Bash, that is expected — the host toolchain is in WSL." >&2
    exit 1
}

echo "== building fx_gen (host $CC) =="
"$CC" -O2 -Wall -Wextra -o "$BIN" "$HERE/fx_gen.c" -lm

echo
echo "== the gate, against its own controls =="
# ⚠️ Not piped: a pipeline reports the LAST command's status, so `| tail` would
# report PASS on a broken gate (root ../../CLAUDE.md).
"$BIN" --self-test

if [ "$CHECK_ONLY" = "1" ]; then
    echo
    echo "gen-sounds.sh: --check only, no files written"
    exit 0
fi

echo
echo "== rendering the stock set into $HERE =="
"$BIN" "$HERE"

echo
echo "== what a reader sees (channels 0001 @22, rate 0000ac44 @24, bits 0010 @34) =="
od -t x1 -N 48 "$HERE/fx_click.wav" | sed 's/^/  /'

echo
echo "== md5 =="
( cd "$HERE" && md5sum fx_*.wav | sed 's/^/  /' )

echo
echo "gen-sounds.sh: done. A clean 'git status' here means nothing changed, which"
echo "               is the expected result of a re-run."
