#!/bin/bash
#
# arca_release_test.sh — regression for release.sh: the `rm -rf "$OUT"` guard and
#                      the git provenance the bundle's NOTICE rests on.
#
# Host-only, no device, no card, no root, no network, no `gh`. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_release_test.sh"
#
# ── Why the script is COPIED and never sourced ──────────────────────────────
#
# release.sh defines its own ok()/info()/warn()/err(), and err() exits. Sourcing
# it would end this suite on the first refusal and overwrite the helpers below.
# So every case runs the shipped file as a program — in a temp tree, because
# SCRIPT_DIR comes from $0 and the script cd's there and sources lib/arca-bundle.sh
# and lib/arca-release.sh under `set -e` BEFORE it parses a single argument. A copy
# of the bare script dies before reading `--out`.
#
# ── Why `rm` is replaced by a tripwire ──────────────────────────────────────
#
# The guard's whole job is to refuse before the `rm -rf`. Asserting "it exited
# non-zero" cannot see the difference between a refusal and a script that wiped
# the directory and then failed for some later reason — and the defect this suite
# exists for accepted `--out /usr` straight into that `rm -rf`. So group A puts a
# fake `rm` first on PATH: it records its argv and exits 1, which aborts
# release.sh at its own `set -e`. An empty log is the assertion. That is also what
# makes a root spelling safe to exercise against a PRE-FIX copy of the script on
# this host — the guard being measured is exactly the one that would let / through.
#
# The shipped script is not modified to make this work: `rm -rf "$OUT"` is the
# only `rm` in it (measured), so the shim is a precise tripwire and nothing else.
#
# ── What each group is for ──────────────────────────────────────────────────
#
#   A  the --out guard. Every refusal asserted three ways: non-zero exit, an empty
#      tripwire log, and the message. Plus the decoy file survives — "it exited
#      non-zero" is not the claim, "it did not delete that" is.
#   B  the two paths that must be ACCEPTED, which is the half a guard suite
#      normally cannot reach: an empty directory and a re-stage over a real
#      bundle. Both fall through into the four-component ARM build loop, so this
#      group runs against a STUB component (see the hole named below). It ends in
#      a floor — a bundle that staged nothing would pass every other case here.
#   C  GIT_DIRTY, which is what the publish preflight refuses on and what
#      bundle.info records. C1 and C4 are the controls: the check must be able to
#      say "clean", or C2/C3 pass against a check hardwired to say "dirty".
#   D  the host gate: that release.sh runs it once, obeys its verdict, and skips it only
#      under --skip-tests.
#   E  --source-offer / --source-out / --image: the offer script is run under the release's
#      own tag, before the build and after the gate; NOTICE names the file only when it is
#      made; a publish attaches bundle, source file, checksum and image to one release.
#      The offer script and gh are stubs. E0 (stage without the flag) and E7c (a publish
#      without it is refused, nothing run) are the controls.
#      RELEASE_SH=<path> grades another copy; the E sweep is the second half of
#      tests/measure_release_sabotage.sh (ARCA_SAB_COPIES_ONLY=1 runs just that half).
#
# ── Seen failing ────────────────────────────────────────────────────────────
#
# `tests/measure_release_sabotage.sh` is the sweep, and every case in it produces
# failures. Measured 2026-09-07, against a green baseline of 40:
#
#   the pre-fix release.sh entire (git show ba7eded^)   11 failed
#   --out normalisation dropped                          3 failed  cases A3/A4/A5
#   the ownership check dropped                          4 failed  cases A6/A7/A10/A13
#   the quoted-glob guard restored                       30 failed
#   the --cached dirty check dropped                     2 failed  cases C3/C5
#   the post-build --cached half dropped                 1 failed  case C5 alone
#   release.sh replaced by `exit 1`                     32 failed
#   release.sh replaced by `exit 0`                     27 failed
#
# The last two are controls on THIS file rather than on release.sh. `exit 1`
# refuses everything, so a suite asserting only "it failed" would go green on it;
# `exit 0` deletes nothing and succeeds, so a suite reading only the exit status
# would go green on that. Both must fail wholesale, and do.
#
# ⚠️ Note what the sweep does NOT move: A11/A12/A14, the "and the decoy's file is
# still there" cases, keep passing under the ownership-check sabotage. The
# tripwire is why — it aborts the script in place of the `rm -rf`, so the decoy
# survives a guard that has been removed entirely. Those cases witness that the
# tripwire's own claim is true, not that the guard refused; the case above each of
# them is what carries the refusal.
#
# ── The hole, named ─────────────────────────────────────────────────────────
#
# Group B's acceptance is asserted against a stub component script, never against
# a real four-component ARM build. release.sh has no --no-build flag, so nothing
# here can see whether native_apps, vnc_client, scummvm-arca and usb_host
# actually honour `--bundle <dir>`; what it sees is that release.sh calls them
# with exactly those two arguments and consumes the bundle they leave behind.

# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# RELEASE_SH=<path> runs every case against another copy of release.sh: that is how a
# sabotaged copy is graded (tests/measure_release_sabotage.sh), so the shipped file is never edited.
RELEASE_SH="${RELEASE_SH:-$REPO_DIR/release.sh}"
# shellcheck source=../lib/product.sh
. "$REPO_DIR/lib/product.sh"   # PRODUCT_STEM: the tarball is named from it

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'

PASS=0
FAIL=0

ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }

# assert_eq <want> <got> <description>
assert_eq() {
    if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$1', got '$2')"; fi
}
exists() { if [ -e "$1" ] || [ -L "$1" ]; then ok "$2"; else bad "$2 — missing: $1"; fi; }

if [ ! -f "$RELEASE_SH" ]; then
    echo -e "  ${RED}✗${NC} no release.sh at $RELEASE_SH"
    exit 1
fi

# WSL's own filesystem, not /mnt/c: this builds and deletes directory trees, and
# DrvFs is slow enough that a file-by-file copy of a few scripts is the only kind
# worth doing across the boundary.
TMP=$(mktemp -d /tmp/arca-release-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT INT TERM

# ── the tripwire ────────────────────────────────────────────────────────────
SHIM="$TMP/shim"
mkdir -p "$SHIM"
cat > "$SHIM/rm" <<'RMSHIM'
#!/bin/bash
# Not an rm. Records its argv and fails, which aborts release.sh at its own
# `set -e` — so a guard that let a path through is visible as a LOGGED path
# instead of as a deleted tree.
printf '%s\n' "$*" >> "$ARCA_RM_LOG"
exit 1
RMSHIM
chmod +x "$SHIM/rm"

# ── the temp tree ───────────────────────────────────────────────────────────
# Only what release.sh sources, file by file. Never `cp -a` the repo: the card
# captures and scummvm/ are gigabytes, and a copy of a source INTO the repo is
# what once made another suite fail.
#
# The stub is the whole reason a tree is built rather than the repo used: it
# stands in for `<comp>/build-and-deploy.sh --bundle <dir>`, which is otherwise a
# four-component ARM cross-build.
build_tree() {
    local t="$1"
    mkdir -p "$t/lib" "$t/native_apps" "$t/tests"
    cp "$RELEASE_SH"                   "$t/release.sh"
    cp "$REPO_DIR/lib/arca-bundle.sh"  "$t/lib/arca-bundle.sh"
    cp "$REPO_DIR/lib/arca-release.sh" "$t/lib/arca-release.sh"
    # release.sh names its tarball and release title from lib/product.sh, which reads the
    # repo's os-release file: both ride along or the script dies before parsing an argument.
    cp "$REPO_DIR/lib/product.sh"      "$t/lib/product.sh"
    mkdir -p "$t/rootfs/board/arca"
    cp "$REPO_DIR/rootfs/board/arca/os-release" "$t/rootfs/board/arca/os-release"
    cat > "$t/tests/run-all.sh" <<'STUBGATE'
#!/bin/bash
# Stub for the host gate. release.sh invokes exactly:
#   bash "$SCRIPT_DIR/tests/run-all.sh"
# The real gate takes ~190 s and needs gcc, shellcheck and a git checkout, none
# of which this temp tree has — so a stub, for the same reason the component
# build script is stubbed. What group D asserts is that release.sh CALLS it and
# obeys its exit code, and the verdict is dialled in per case with
# ARCA_STUB_GATE_RC. The marker is how a case proves --skip-tests did not call it.
[ -n "${ARCA_STUB_GATE_MARKER:-}" ] && echo called >> "$ARCA_STUB_GATE_MARKER"
echo "stub host gate: exiting ${ARCA_STUB_GATE_RC:-0}"
exit "${ARCA_STUB_GATE_RC:-0}"
STUBGATE
    cat > "$t/native_apps/build-and-deploy.sh" <<'STUBCOMP'
#!/bin/bash
# Stub. release.sh invokes exactly:
#   ( cd <comp> && bash ./build-and-deploy.sh --bundle <OUT_ABS> )
# so the argument shape is asserted here, where a change to it fails loudly
# instead of staging an empty bundle.
set -e
[ $# -eq 2 ]          || { echo "stub: expected 2 args, got $#: $*" >&2; exit 1; }
[ "$1" = "--bundle" ] || { echo "stub: expected --bundle, got '$1'" >&2; exit 1; }
[ -d "$2" ]           || { echo "stub: --bundle is not a directory: $2" >&2; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../lib/arca-bundle.sh
. "$HERE/../lib/arca-bundle.sh"
echo "stub payload" > "$HERE/payload.bin"
arca_bundle_init "$2" native_apps
arca_bundle_add  "$2" native_apps 0755 "$HERE/payload.bin" /opt/games/stub_game
arca_bundle_finish "$2" native_apps >/dev/null
if [ -n "${ARCA_ORDER_LOG:-}" ]; then echo component >> "$ARCA_ORDER_LOG"; fi
echo "stub: staged 1 file"
STUBCOMP
    # Stub for rootfs/make-source-offer.sh. The real one is a 0.7 GB assembly that reads
    # caches this temp tree does not have, so what is asserted is how release.sh CALLS it
    # (argv, order) and what it does with the file it leaves. It writes the file name the
    # real script writes: <stem>-source-<tag>[-dev].tar plus .sha256, -dev when the
    # tree is dirty, measured with the same two git commands.
    mkdir -p "$t/rootfs"
    cat > "$t/rootfs/make-source-offer.sh" <<'STUBOFFER'
#!/bin/bash
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../lib/product.sh
. "$HERE/lib/product.sh"
if [ -n "${ARCA_OFFER_LOG:-}" ]; then printf '%s\n' "$*" >> "$ARCA_OFFER_LOG"; fi
if [ -n "${ARCA_ORDER_LOG:-}" ]; then echo offer >> "$ARCA_ORDER_LOG"; fi
[ "${ARCA_STUB_OFFER_RC:-0}" = 0 ] || { echo "stub offer: failing as told" >&2; exit "$ARCA_STUB_OFFER_RC"; }
tag="" out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --tag) tag="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --image) shift 2 ;;
        *) shift ;;
    esac
done
dev=""
if ! git -C "$HERE" diff --quiet 2>/dev/null || ! git -C "$HERE" diff --cached --quiet 2>/dev/null; then dev="-dev"; fi
[ -z "${ARCA_STUB_OFFER_NOFILE:-}" ] || exit 0
mkdir -p "$out"
echo "stub source" > "$out/${PRODUCT_STEM}-source-${tag}${dev}.tar"
echo "stub sha256  ${PRODUCT_STEM}-source-${tag}${dev}.tar" > "$out/${PRODUCT_STEM}-source-${tag}${dev}.tar.sha256"
STUBOFFER
}

TREE="$TMP/tree"
build_tree "$TREE"

# ── the canary ──────────────────────────────────────────────────────────────
# Laid out like a real root and deliberately OUTSIDE every --out used below, so a
# guard failure has somewhere visible to land instead of only showing up as a
# passing test.
CANARY="$TMP/canary"
mkdir -p "$CANARY/etc" "$CANARY/usr/lib"
echo "root:x:0:0" > "$CANARY/etc/shadow"
echo "so"         > "$CANARY/usr/lib/libX11.so.6"
canary_md5() { (cd "$CANARY" && find . -type f -exec md5sum {} + | LC_ALL=C sort | md5sum); }
CANARY_MD5=$(canary_md5)

RM_LOG="$TMP/rm.log"
OUT_TXT=""
ST=0

# run_guard <tree> [args...]   — `rm` is the tripwire; the log is truncated first.
run_guard() {
    local t="$1"; shift
    : > "$RM_LOG"
    OUT_TXT=$(cd "$t" && ARCA_RM_LOG="$RM_LOG" PATH="$SHIM:$PATH" \
        bash "$t/release.sh" --stage-only --component native_apps "$@" 2>&1)
    ST=$?
}

# run_full <tree> [args...]    — the real thing, real rm, no tripwire.
run_full() {
    local t="$1"; shift
    OUT_TXT=$(cd "$t" && bash "$t/release.sh" --stage-only --component native_apps "$@" 2>&1)
    ST=$?
}

# refuse <out-value> <message-substring> <description>
# ⚠️ The tripwire is checked BEFORE the message: a script that wiped the tree and
# then died also exits non-zero, and that is the defect, not the fix.
refuse() {
    local outv="$1" rx="$2" desc="$3"
    run_guard "$TREE" --out "$outv"
    if [ "$ST" -eq 0 ]; then
        bad "$desc — release.sh exited 0"
    elif [ -s "$RM_LOG" ]; then
        bad "$desc — it reached 'rm $(tr '\n' ' ' < "$RM_LOG")'"
    elif printf '%s\n' "$OUT_TXT" | grep -qF -e "$rx"; then
        ok "$desc"
    else
        bad "$desc — refused, but no line contained '$rx'"
        printf '%s\n' "$OUT_TXT" | tail -5 | sed 's/^/        /'
    fi
}

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "A. release.sh --out — the guard on the rm -rf"
echo "   (rm is a tripwire here, so a root spelling that got through is logged, not run)"
# ═══════════════════════════════════════════════════════════════════════════

# A0 is the control on the harness. If the tripwire cannot fire, every refusal
# below is unfalsifiable — so it is fired deliberately, with an --out the guard
# must ACCEPT.
mkdir -p "$TMP/tripwire-probe"
run_guard "$TREE" --out "$TMP/tripwire-probe"
if [ -s "$RM_LOG" ]; then
    ok "A0 the tripwire fires on an ACCEPTED --out — group A can see a guard that let one through"
else
    bad "A0 the tripwire never fired on an accepted --out — every refusal below is unfalsifiable"
fi

refuse ""     "--out needs a value"          "A1 an empty --out is refused at parse time"
refuse "/"    "resolves to this host's root" "A2 --out / is refused"
refuse "//"   "resolves to this host's root" "A3 --out // is refused (normalises to /)"
refuse "///"  "resolves to this host's root" "A4 --out /// is refused"
refuse "/."   "resolves to this host's root" "A5 --out /. is refused"

# ⚠️ Its own case, because it normalises to '/.' and not to '/': it is caught by
# the OWNERSHIP check, a different line of the guard from the four above.
refuse "/.//" "is not empty and is not a staged bundle" \
    "A6 --out /.// is refused by the ownership check, not the root check"

refuse "/usr" "is not empty and is not a staged bundle" \
    "A7 --out /usr is refused — the reported defect"

refuse "../x"        "path containing '..'" "A8 --out ../x is refused"
refuse "$TMP/a/../b" "path containing '..'" "A9 .. anywhere in --out is refused, not just at the front"

# A decoy: a non-empty directory that is not a bundle. The assertion is that the
# FILE SURVIVES, not merely that the command failed.
DECOY="$TMP/decoy"
mkdir -p "$DECOY/sub"
echo "do not delete me" > "$DECOY/precious.txt"
echo "nor me"           > "$DECOY/sub/also.txt"
refuse "$DECOY" "is not empty and is not a staged bundle" \
    "A10 a non-empty non-bundle directory is refused"
exists "$DECOY/precious.txt" "A11 and the decoy's file is still there"
exists "$DECOY/sub/also.txt" "A12 and so is the one a level down"

# Half a bundle is not a bundle: root/ without manifest.d/ must still refuse.
HALF="$TMP/half"
mkdir -p "$HALF/root/opt"
echo "x" > "$HALF/root/opt/thing"
refuse "$HALF" "is not empty and is not a staged bundle" \
    "A13 root/ without manifest.d/ is not a bundle"
exists "$HALF/root/opt/thing" "A14 and its file survives"

# An existing regular file, not a directory.
echo "not a dir" > "$TMP/afile"
refuse "$TMP/afile" "exists and is not a directory" "A15 --out pointing at a regular file is refused"
exists "$TMP/afile" "A16 and the file survives"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "B. the two paths that must be ACCEPTED"
echo "   (against a stub component — see the hole named in this file's header)"
# ═══════════════════════════════════════════════════════════════════════════

EMPTY="$TMP/accept-empty"
mkdir -p "$EMPTY"
run_full "$TREE" --out "$EMPTY"
if [ "$ST" -eq 0 ]; then
    ok "B1 an EMPTY existing directory is accepted"
else
    bad "B1 an EMPTY existing directory is accepted — exited $ST"
    printf '%s\n' "$OUT_TXT" | tail -8 | sed 's/^/        /'
fi
exists "$EMPTY/root/opt/games/stub_game"    "B2 the stub's artifact is staged under root/"
exists "$EMPTY/manifest.d/native_apps.list" "B3 and its manifest was written"
exists "$EMPTY/NOTICE"                      "B4 NOTICE is written"
exists "$EMPTY/manifest.d/bundle.info"      "B5 bundle.info is written"

# The device path is $ARCA_BUNDLE_STAMP, and there is one spelling of it. Read it
# from the library rather than repeating it here, so a move of the stamp fails
# this case instead of silently passing against a stale literal.
STAMP=$(sed -n 's/^ARCA_BUNDLE_STAMP=//p' "$REPO_DIR/lib/arca-bundle.sh" | head -1 | tr -d '"')
if [ -n "$STAMP" ]; then
    ok "B6 the stamp's device path was read from lib/arca-bundle.sh ($STAMP)"
else
    bad "B6 could not read ARCA_BUNDLE_STAMP from lib/arca-bundle.sh — B7 is meaningless"
fi
exists "$EMPTY/root$STAMP" "B7 and the same bytes are staged as an installable artifact"

assert_eq "native_apps" "$(sed -n 's/^components=//p' "$EMPTY/manifest.d/bundle.info" | tr -d ' ')" \
    "B8 bundle.info names the component, and not the reserved meta manifest"

# ⚠️ The floor. A bundle that staged nothing satisfies every refusal case above
# and most of the structural ones.
B_ENTRIES=$(grep -c . "$EMPTY/manifest.d/native_apps.list" 2>/dev/null || true)
if [ "${B_ENTRIES:-0}" -ge 1 ]; then
    ok "B9 the run staged $B_ENTRIES file(s) — it did not pass by staging nothing"
else
    bad "B9 the run staged ${B_ENTRIES:-0} files, so every structural case above is vacuous"
fi

TARS=$(find "$TREE/build" -maxdepth 1 -name "${PRODUCT_STEM}-apps-*.tar.gz" 2>/dev/null | grep -c . || true)
if [ "${TARS:-0}" -ge 1 ]; then
    ok "B10 the tarball landed under the tree's own build/"
else
    bad "B10 no tarball under $TREE/build"
fi
if printf '%s\n' "$OUT_TXT" | grep -qF "Staged only"; then
    ok "B11 --stage-only ran to the end and published nothing"
else
    bad "B11 --stage-only did not reach its own closing message"
fi

# THE case the ownership check must not break: re-staging over a real bundle.
run_full "$TREE" --out "$EMPTY"
if [ "$ST" -eq 0 ]; then
    ok "B12 a re-stage over a real staged bundle is accepted"
else
    bad "B12 a re-stage over a real staged bundle is accepted — exited $ST"
    printf '%s\n' "$OUT_TXT" | tail -8 | sed 's/^/        /'
fi
exists "$EMPTY/manifest.d/native_apps.list" "B13 and the bundle is there after the re-stage"

# A path that does not exist at all.
run_full "$TREE" --out "$TMP/accept-fresh/deeper"
if [ "$ST" -eq 0 ]; then
    ok "B14 a --out that does not exist yet is created"
else
    bad "B14 a --out that does not exist yet is created — exited $ST"
fi
exists "$TMP/accept-fresh/deeper/manifest.d/native_apps.list" "B15 and staged into"

assert_eq "$CANARY_MD5" "$(canary_md5)" \
    "B16 nothing outside the --out directories was touched by any case so far"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "C. GIT_DIRTY — what the publish preflight refuses on, and what bundle.info records"
echo "   (--stage-only skips the preflight, so bundle.info's commit= line is the readout)"
# ═══════════════════════════════════════════════════════════════════════════

GTREE="$TMP/gtree"
build_tree "$GTREE"
(
    cd "$GTREE" || exit 1
    git init -q .
    git config user.email "test@example.invalid"
    git config user.name  "arca_release_test"
    # ⚠️ An A/B must change exactly one thing, and without this it changes two.
    # A staged run writes build/<stem>-apps-*.tar.gz and the stub's payload
    # INSIDE this fixture repo, so `git add -A` below would stage them too and
    # the next run's regenerated tarball would then read as a worktree change.
    # C4 passed by accident for that reason before this file existed: measured,
    # it went dirty under a sabotage that has nothing to do with it.
    printf 'build/\npayload.bin\n' > .gitignore
    echo "tracked" > marker.txt
    git add -A
    git commit -qm "fixture"
) > "$TMP/git-setup.log" 2>&1

if [ -d "$GTREE/.git" ]; then
    ok "C0 the fixture repo was created"
else
    bad "C0 the fixture repo was created — every case in C below is meaningless"
    sed 's/^/        /' "$TMP/git-setup.log"
fi

# dirty_of <out-dir>  -> "" for clean, " (dirty)" for dirty, read from bundle.info
dirty_of() {
    run_full "$GTREE" --out "$1"
    if [ "$ST" -ne 0 ]; then echo "STAGE-FAILED-$ST"; return; fi
    sed -n 's/^commit=[0-9a-f][0-9a-f]*//p' "$1/manifest.d/bundle.info"
}

# ⚠️ C1 is the control. Without it, C2 and C3 pass against a check hardwired to
# say "dirty" — which is exactly what deleting its first half would produce.
assert_eq "" "$(dirty_of "$TMP/g-clean")" \
    "C1 a clean tree stamps commit= with no marker"

echo "changed" >> "$GTREE/marker.txt"
assert_eq " (dirty)" "$(dirty_of "$TMP/g-worktree")" \
    "C2 an UNSTAGED modification is dirty"

git -C "$GTREE" add -A > /dev/null 2>&1
# `git diff --quiet` alone exits 0 here — it compares the worktree against the
# INDEX. This is THE case: `git add -A` then `--tag` would publish a release whose
# NOTICE offers source that is in no commit.
assert_eq " (dirty)" "$(dirty_of "$TMP/g-staged")" \
    "C3 a STAGED-but-uncommitted modification is dirty"

git -C "$GTREE" commit -qm "second" > /dev/null 2>&1
assert_eq "" "$(dirty_of "$TMP/g-committed")" \
    "C4 and committing it reads clean again — the check can still say clean"

# The other half of the same defect: the post-build re-check needs both forms too,
# or a component build that modified a tracked file publishes anyway. It is
# unreachable with --stage-only, so this one is asserted against the shipped text.
#
# ⚠️ Which part of the count is the harness: the bare string `diff --cached
# --quiet` appears THREE times in release.sh, because the comment explaining the
# fix quotes it. The `-C "$SCRIPT_DIR"` is what distinguishes the two call sites
# from the prose about them — measured, this case read 3 before the pattern was
# anchored on it.
# shellcheck disable=SC2016  # the literal $SCRIPT_DIR is the point: it is what
# distinguishes the two call sites from the comment quoting them.
BOTH=$(grep -cF 'git -C "$SCRIPT_DIR" diff --cached --quiet' "$RELEASE_SH" || true)
assert_eq "2" "${BOTH:-0}" \
    "C5 both dirty checks in the shipped script compare against HEAD as well as the index"

assert_eq "$CANARY_MD5" "$(canary_md5)" \
    "C6 the canary is still intact at the end of the run"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "D. the host gate — that release.sh runs it, and obeys it"
echo "   (the gate is stubbed; its verdict is dialled in per case)"
# ═══════════════════════════════════════════════════════════════════════════
# ⚠️ This group exists because the gate's own placement in release.sh was wrong
# once, and this suite is what found it. Inserted before the --out refusals, the
# call preempted them: in this temp tree the gate is reached before any --out
# case, so a non-zero verdict aborted release.sh and groups A and B reported 30
# failures with A0 declaring every refusal below it unfalsifiable. The gate now
# sits after every local refusal and immediately before the first build, and
# D1/D2 are what will notice if it ever moves back.
GATE_MARK="$TMP/gate-called.log"

: > "$GATE_MARK"
ARCA_STUB_GATE_MARKER="$GATE_MARK" ARCA_STUB_GATE_RC=1 \
    run_full "$TREE" --out "$TMP/d-fail"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "host gate FAILED"; then
    ok "D1 a gate verdict of 1 refuses the release"
else
    bad "D1 a gate verdict of 1 must refuse the release — exit $ST"
    printf '%s\n' "$OUT_TXT" | tail -4 | sed 's/^/        /'
fi
assert_eq "0" "$([ -d "$TMP/d-fail" ] && find "$TMP/d-fail" -type f | wc -l | tr -d ' ' || echo 0)" \
    "D1b and stages no file"

: > "$GATE_MARK"
ARCA_STUB_GATE_MARKER="$GATE_MARK" ARCA_STUB_GATE_RC=2 \
    run_full "$TREE" --out "$TMP/d-harness"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "could not judge"; then
    ok "D2 a gate verdict of 2 refuses as HARNESS ERROR, not as a failed test"
else
    bad "D2 a gate verdict of 2 must refuse with 'could not judge' — exit $ST"
    printf '%s\n' "$OUT_TXT" | tail -4 | sed 's/^/        /'
fi

: > "$GATE_MARK"
ARCA_STUB_GATE_MARKER="$GATE_MARK" ARCA_STUB_GATE_RC=0 \
    run_full "$TREE" --out "$TMP/d-pass"
assert_eq "0" "$ST" "D3 a gate verdict of 0 lets the release proceed"
assert_eq "1" "$(grep -c called "$GATE_MARK" || true)" \
    "D3b and the gate was called exactly once"

# ⚠️ The marker is the assertion, not the exit status: --skip-tests would still
# exit 0 with a gate that ran and passed, so "it did not run" needs a witness.
: > "$GATE_MARK"
ARCA_STUB_GATE_MARKER="$GATE_MARK" ARCA_STUB_GATE_RC=1 \
    run_full "$TREE" --out "$TMP/d-skip" --skip-tests
assert_eq "0" "$ST" "D4 --skip-tests proceeds even with a gate that would fail"
assert_eq "0" "$(grep -c called "$GATE_MARK" || true)" \
    "D4b and the gate was never invoked at all"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "E. the source offer — --source-offer, --source-out, --image, and the NOTICE that names the file"
echo "   (rootfs/make-source-offer.sh and gh are stubs; what is asserted is how release.sh calls them)"
# ═══════════════════════════════════════════════════════════════════════════
# The offer is GPLv2 section 3(a) source for the card image, offered from the place the image
# is published and under its tag. What can go wrong here is a release that carries the image and
# not the source, a NOTICE that offers a file the release lacks, or a tag that differs between
# the three. E0 and E7c are the controls: a stage without --source-offer must behave as before
# and a publish without it must run nothing, or the cases between them pass against a script
# that always runs the offer.
DAY=$(date -u '+%Y%m%d')
IMG="$TMP/card.img"; echo "pretend image" > "$IMG"
OFFLOG="$TMP/offer.log"; ORDLOG="$TMP/order.log"; GHLOG="$TMP/gh.log"; GHNOTES="$TMP/gh-notes.log"
GHSHIM="$TMP/ghshim"
mkdir -p "$GHSHIM"
cat > "$GHSHIM/gh" <<'GHSTUB'
#!/bin/bash
# Not gh. Records its argv and the release notes it was handed, and answers `release view`.
printf '%s\n' "$*" >> "$ARCA_GH_LOG"
prev=""
for a in "$@"; do
    if [ "$prev" = "--notes-file" ]; then cat "$a" >> "$ARCA_GH_NOTES"; fi
    prev="$a"
done
if [ "$1 $2" = "release view" ]; then echo "https://example.invalid/releases/$3"; fi
exit 0
GHSTUB
chmod +x "$GHSHIM/gh"

# A clean, pushed tree, so a publish passes the preflight: a fixture repo with a fake remote.
PTREE="$TMP/ptree"
build_tree "$PTREE"
(
    cd "$PTREE" || exit 1
    git init -q .
    git config user.email "test@example.invalid"
    git config user.name  "arca_release_test"
    printf 'build/\npayload.bin\n' > .gitignore
    git add -A
    git commit -qm "fixture"
    git remote add origin git@github.com:acme/widget.git
    git update-ref refs/remotes/origin/main HEAD
) > "$TMP/git-setup-p.log" 2>&1
if [ -n "$(git -C "$PTREE" branch -r --contains HEAD 2>/dev/null)" ]; then
    ok "E00 the publish fixture is clean and its commit is on a (fake) remote branch"
else
    bad "E00 the publish fixture has no remote branch — the publish cases below are meaningless"
fi

# run_e <tree> [args...]  — gh is the stub; the env the caller prefixes reaches the stubs.
run_e() {
    local t="$1"; shift
    OUT_TXT=$(cd "$t" && PATH="$GHSHIM:$PATH" ARCA_OFFER_LOG="$OFFLOG" ARCA_ORDER_LOG="$ORDLOG" \
        ARCA_GH_LOG="$GHLOG" ARCA_GH_NOTES="$GHNOTES" ARCA_STUB_GATE_MARKER="$GATE_MARK" \
        bash "$t/release.sh" "$@" 2>&1)
    ST=$?
}
reset_logs() { : > "$OFFLOG"; : > "$ORDLOG"; : > "$GHLOG"; : > "$GHNOTES"; : > "$GATE_MARK"; }
# notice_names <bundle dir> : the source file NOTICE names, or empty
notice_names() { grep -o "${PRODUCT_STEM}-source-[A-Za-z0-9._-]*\.tar" "$1/NOTICE" | head -n 1; }

# E0: no --source-offer: the offer is not run, and NOTICE does not name a file it lacks
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e0"
assert_eq "0" "$ST" "E0 a release without --source-offer still stages"
assert_eq "0" "$(grep -c . "$OFFLOG")" "E0b and the offer script was never run"
assert_eq "" "$(notice_names "$TMP/e0")" "E0c and NOTICE names no source file"
if grep -qF "without a source file" "$TMP/e0/NOTICE"; then
    ok "E0d and NOTICE says the bundle was staged without one"
else
    bad "E0d NOTICE should say the bundle was staged without a source file"
fi

# E1: --source-offer with --stage-only
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e1" --source-offer --source-out "$TMP/srcout1"
assert_eq "0" "$ST" "E1 --source-offer stages"
assert_eq "1" "$(grep -c . "$OFFLOG")" "E1b and the offer script ran exactly once"
for want in "--tag $DAY" "--allow-dev" "--out $TMP/srcout1"; do
    if grep -qF -e "$want" "$OFFLOG"; then ok "E1c the offer was called with '$want'"; else bad "E1c the offer was not called with '$want': $(cat "$OFFLOG")"; fi
done
exists "$TMP/srcout1/${PRODUCT_STEM}-source-${DAY}.tar" "E1d the source file is where --source-out said"
assert_eq "${PRODUCT_STEM}-source-${DAY}.tar" "$(notice_names "$TMP/e1")" \
    "E1e NOTICE names exactly that file, under the same tag as the bundle tarball"
if grep -qF "${PRODUCT_STEM}-source-${DAY}.tar.sha256" "$TMP/e1/NOTICE"; then
    ok "E1f and its .sha256"
else
    bad "E1f NOTICE does not name the .sha256"
fi
exists "$PTREE/build/${PRODUCT_STEM}-apps-${DAY}.tar.gz" "E1g and the bundle tarball carries the same tag stem"
assert_eq "1" "$(grep -c called "$GATE_MARK" || true)" \
    "E1h and the host gate ran (so 'the gate did not run' in E4b/E6b is a real reading)"

# E2: the offer comes before the components build
assert_eq "offer component" "$(tr '\n' ' ' < "$ORDLOG" | sed 's/ $//')" \
    "E2 the offer is assembled before the first component builds"

# E3: a dirty tree: the file written is -dev, and NOTICE names that one (TREE is not a git repo,
# which release.sh reads as dirty)
reset_logs
run_e "$TREE" --stage-only --component native_apps --out "$TMP/e3" --source-offer --source-out "$TMP/srcout3"
assert_eq "0" "$ST" "E3 --source-offer on a dirty tree stages"
NAMED="$(notice_names "$TMP/e3")"
case "$NAMED" in *-dev.tar) ok "E3b NOTICE names the -dev file of a development build" ;; *) bad "E3b expected a -dev name, got '$NAMED'" ;; esac
exists "$TMP/srcout3/$NAMED" "E3c and that file exists"

# E4..E7: refusals, all raised before the host gate and before anything is staged
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e4" --image "$IMG"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF -e "--image needs --source-offer"; then
    ok "E4 --image without --source-offer is refused"
else
    bad "E4 --image without --source-offer must be refused — exit $ST"
fi
assert_eq "000" "$(grep -c . "$GATE_MARK")$(grep -c . "$OFFLOG")$([ -e "$TMP/e4" ] && echo 1 || echo 0)" \
    "E4b and nothing ran: no gate, no offer, no staging directory"
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e5" --source-offer --image "$TMP/no-such.img"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "no such image file"; then
    ok "E5 an --image that is not a file is refused"
else
    bad "E5 a missing --image must be refused — exit $ST"
fi
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e6" --source-offer --source-out "$PTREE/offer-out"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "inside the repository"; then
    ok "E6 --source-out inside the repository is refused"
else
    bad "E6 --source-out inside the repository must be refused — exit $ST"
fi
assert_eq "00" "$(grep -c . "$GATE_MARK")$(grep -c . "$OFFLOG")" "E6b and before the host gate, not after it"
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e7" --source-offer --source-out "$TMP/nope/deeper"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "does not exist"; then
    ok "E7 a --source-out whose parent is missing is refused"
else
    bad "E7 a --source-out whose parent is missing must be refused — exit $ST"
fi

# E7b: a publish without --source-offer is refused, and nothing ran: the bundle would be
# published without the source of what it ships. --stage-only is not affected: the cases
# above it stage without --tag.
reset_logs
run_e "$PTREE" --tag v1.4 --out "$TMP/e7b" --component native_apps
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF -e "--tag needs --source-offer"; then
    ok "E7b a publish without --source-offer is refused"
else
    bad "E7b a publish without --source-offer must be refused — exit $ST"
fi
assert_eq "0000" "$(grep -c . "$GATE_MARK")$(grep -c . "$OFFLOG")$(grep -c . "$GHLOG")$([ -e "$TMP/e7b" ] && echo 1 || echo 0)" \
    "E7c and nothing ran: no gate, no offer, no gh call, no staging directory"

# E8: the gate comes first: a failing gate means the offer is never assembled
reset_logs
ARCA_STUB_GATE_MARKER="$GATE_MARK" ARCA_STUB_GATE_RC=1 \
    run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e8" --source-offer --source-out "$TMP/srcout8"
if [ "$ST" -ne 0 ]; then ok "E8 a failing gate refuses the release"; else bad "E8 a failing gate must refuse"; fi
assert_eq "0" "$(grep -c . "$OFFLOG")" "E8b and the offer was not assembled"

# E9/E10: the offer fails, or claims success and writes nothing: no component is built
reset_logs
ARCA_STUB_OFFER_RC=3 run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e9" --source-offer --source-out "$TMP/srcout9"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "make-source-offer.sh failed"; then
    ok "E9 a failing offer script refuses the release"
else
    bad "E9 a failing offer script must refuse — exit $ST"
fi
assert_eq "offer" "$(tr '\n' ' ' < "$ORDLOG" | sed 's/ $//')" "E9b and no component was built after it"
reset_logs
ARCA_STUB_OFFER_NOFILE=1 run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e10" --source-offer --source-out "$TMP/srcout10"
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT_TXT" | grep -qF "is not there"; then
    ok "E10 an offer script that wrote no file is not taken at its word"
else
    bad "E10 a missing source file must refuse — exit $ST"
fi

# E11: images are handed to the offer, by absolute path, once each
reset_logs
run_e "$PTREE" --stage-only --component native_apps --out "$TMP/e11" --source-offer --source-out "$TMP/srcout11" --image "$IMG"
assert_eq "0" "$ST" "E11 --image with --source-offer stages"
if grep -qF -e "--image $IMG" "$OFFLOG"; then ok "E11b the image reaches the offer script"; else bad "E11b the image did not reach the offer: $(cat "$OFFLOG")"; fi

# E12: publishing: image, bundle, source file and its checksum go to ONE `gh release create`, under
# one tag, and the offer was not asked for a development build
reset_logs
run_e "$PTREE" --tag v1.2 --out "$TMP/e12" --source-offer --source-out "$TMP/srcout12" --image "$IMG" --component native_apps
assert_eq "0" "$ST" "E12 a publish with --source-offer and --image succeeds"
assert_eq "1" "$(grep -c '^release create v1.2 ' "$GHLOG")" "E12b one 'gh release create v1.2'"
for want in "${PRODUCT_STEM}-apps-v1.2.tar.gz" "$TMP/srcout12/${PRODUCT_STEM}-source-v1.2.tar " "$TMP/srcout12/${PRODUCT_STEM}-source-v1.2.tar.sha256" "$IMG"; do
    if grep '^release create' "$GHLOG" | grep -qF -e "$want"; then ok "E12c the release carries $(basename "$want")"; else bad "E12c the release does not carry '$want': $(cat "$GHLOG")"; fi
done
if grep -qF -e "--tag v1.2" "$OFFLOG" && ! grep -qF -e "--allow-dev" "$OFFLOG"; then
    ok "E12d the offer was built under the release's tag and is not a development build"
else
    bad "E12d expected '--tag v1.2' and no --allow-dev: $(cat "$OFFLOG")"
fi
assert_eq "${PRODUCT_STEM}-source-v1.2.tar" "$(notice_names "$TMP/e12")" "E12e NOTICE names the file the release carries"
if grep -qF "${PRODUCT_STEM}-source-v1.2.tar" "$GHNOTES" && grep -qF "card.img" "$GHNOTES"; then
    ok "E12f and the release notes name the source file and the image"
else
    bad "E12f the release notes should name both: $(cat "$GHNOTES")"
fi

assert_eq "$CANARY_MD5" "$(canary_md5)" "E14 the canary is still intact after the source-offer cases"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "════════════════════════════════════════"
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $TOTAL total"

# A file that silently ran zero cases reports success as loudly as one that ran
# all of them.
if [ "$TOTAL" -lt 30 ]; then
    echo -e "  ${RED}✗ only $TOTAL cases ran — the harness itself is broken${NC}"
    exit 1
fi
if [ "$FAIL" -gt 0 ]; then
    echo -e "  ${RED}✗ $FAIL failure(s)${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓ all $TOTAL cases passed${NC}"
echo ""
exit 0
