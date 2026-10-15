#!/bin/bash
#
# commission_offline_test.sh — regression for rootfs/commission-offline.sh's VERIFY pass
#
# Host-only. No SD card and no device: the card is the synthetic tree from
# tests/make-fake-card.sh, mounted nowhere and handed over with --base.
#
#   wsl.exe -u root -e bash -lc "cd /mnt/c/work/roomwizard && tests/commission_offline_test.sh"
#
# Needs root: run without it, a step that reaches for sudo stalls a plain `wsl.exe -e bash -lc`
# on `sudo: a password is required`, and the gate counts a non-root run as a skip, so
# `-u root` above is the one way to run it.
# Needs a staged bundle: ./release.sh --stage-only [--component
# native_apps] leaves one in build/release.
#
# ── Why every case here is a SABOTAGE ──────────────────────────────────────
#
# The verify pass is eight checks that all report success on a good install, and a
# check that cannot fail is decoration. So each case breaks exactly one thing and
# asserts that (a) the right check fires and (b) the tool exits non-zero. The
# happy path is case 1 and is the control for all of them.
#
# Section 4 is the one exception and is the mirror image: the --no-usb skip must
# exit ZERO and still say which flag skipped it, so it uses expect_says rather
# than expect_fires; section 6 mixes both (refusals fire, the no-prompt run says). Section 0
# is neither — it is the structural check on the fixture tree
# itself.
#
# The sabotages run against COPIES — a copy of the bundle, and a copy of just the
# scripts rootfs/commission-offline.sh reads (not the repo, which carries 4 GB of card
# images). SCRIPT_DIR is derived from the script's own location, so a copied tree
# is a genuinely different installation.
#
# ⚠️ One thing is NOT controlled here, deliberately: a binary that really does
# contain an sdiv. This host's compiler will not emit one for Cortex-A8, so there
# is no way to build the positive case without hand-assembling it —
# native_apps/check-arm-safe.sh carries that reasoning and SYSTEM_ANALYSIS.md#61-cortex-a8-has-no-hardware-integer-divide
# the two ways to get a wrong answer out of the gate. What IS controlled is both
# ways the gate can lie by omission: a bundle with zero ARM binaries (2g) and a
# host with no arm objdump (2j, 2k), where the gate must REFUSE rather than pass.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUNDLE_SRC="$REPO_DIR/build/release"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }

if [ "$(id -u)" -ne 0 ]; then
    echo -e "  ${YELLOW}skip${NC}  needs root — run it with wsl.exe -u root"
    exit 0
fi
if [ ! -d "$BUNDLE_SRC" ]; then
    echo -e "  ${RED}✗${NC} no staged bundle at $BUNDLE_SRC"
    echo "     build one:  ./release.sh --stage-only --component native_apps"
    exit 1
fi

TMP=$(mktemp -d /tmp/arca-comm-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT INT TERM

# A copy of only what rootfs/commission-offline.sh reads. `cp -a` on device-files/ so the
# init scripts keep their bytes — that copy is also what brings arca-app and the
# other files the provision plan installs from there; nothing here needs the 4 GB card
# images.
REPO="$TMP/repo"
mkdir -p "$REPO/native_apps" "$REPO/rootfs" "$REPO/lib"
for f in rootfs/commission-offline.sh \
         lib/arca-identify.sh lib/product.sh lib/arca-provision.sh lib/arca-bundle.sh; do
    cp "$REPO_DIR/$f" "$REPO/$f"
done
cp -a "$REPO_DIR/device-files" "$REPO/device-files"
cp "$REPO_DIR/native_apps/check-arm-safe.sh" "$REPO/native_apps/"
# lib/product.sh reads the product name from here (the one home for it).
mkdir -p "$REPO/rootfs/board/arca" && cp "$REPO_DIR/rootfs/board/arca/os-release" "$REPO/rootfs/board/arca/"

# run <bundle-dir> <repo-dir> [extra args...]  -> stdout+stderr in $OUT, status in $ST
OUT=""; ST=0
run() {
    local bundle="$1" repo="$2"; shift 2
    bash "$SCRIPT_DIR/make-fake-card.sh" "$TMP/card" >/dev/null || return 1
    set +e
    OUT=$(bash "$repo/rootfs/commission-offline.sh" \
              --bundle "$bundle" --base "$TMP/card" "$@" 2>&1 < /dev/null)
    ST=$?
    set -e
}

# expect_fires <regex> <description>   — the run must have failed, with that message
expect_fires() {
    if [ "$ST" -eq 0 ]; then
        bad "$2 — the tool exited 0"
    elif printf '%s\n' "$OUT" | grep -qE "$1"; then
        ok "$2"
    else
        bad "$2 — exited $ST but no line matched /$1/"
        printf '%s\n' "$OUT" | grep -E '✗|FAIL' | sed 's/^/        /' | head -5
    fi
}

# expect_says <regex> <description>   — the run must have SUCCEEDED, and said it.
#
# The mirror of expect_fires, for the skip paths. Both halves are load-bearing and
# they are different bugs: a skip that exits non-zero turns a deliberate opt-out
# into a failed commissioning, and a skip that goes unmentioned leaves the operator
# believing a step ran. Asserting only the exit code would pass a silent skip.
expect_says() {
    if [ "$ST" -ne 0 ]; then
        bad "$2 — the tool exited $ST, but a deliberate skip must still succeed"
        printf '%s\n' "$OUT" | grep -E '✗|FAIL' | sed 's/^/        /' | head -5
    elif printf '%s\n' "$OUT" | grep -qE "$1"; then
        ok "$2"
    else
        bad "$2 — exited 0 but no line matched /$1/"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "0. the fixture tree covers everything the tool sources"
# ═══════════════════════════════════════════════════════════════════════════
#
# The negative control for the copy list above. A library sourced LAZILY — its `.`
# inside a branch that every --base case skips — is invisible to running: one such
# omission went unnoticed for a whole session. So a suite that only ever runs
# --base cannot discover a missing library by running; it has to look.
#
# Reads the COPIED scripts, not the repo's, so the thing asserted is the tree the
# cases below actually execute.
#
# ⚠️ Scans EVERY script in the fixture, not just commission-offline.sh. The first
# version of this check read that one file, and thereby missed a library one of the
# other scripts sourced (a copied tree has to cover every script the list is FOR).
#
# The pattern keys on the "/lib/" path component rather than on $REPO_ROOT, so a
# caller using $SCRIPT_DIR/../lib is covered too.
#
# ⚠️ The EAGER shape, measured 2026-09-06 on a library since dropped: unlike a lazy
# source, its `.` sits near the top of commission-offline.sh, before any argument is
# looked at — so every case in sections 1-4 died on it and the suite read 7 passed /
# 30 failed. That shape is worth recognising: when only section 0 survives, the
# fixture is what is broken, not the tool, and 0b names the file. A near-total red
# here is a copy-list symptom until 0b says otherwise.
SRC_LINES=$(grep -hoE '\.[[:space:]]+"[^"]*/lib/[^"]+"' \
                "$REPO"/rootfs/*.sh "$REPO"/lib/*.sh \
            | sed 's|.*/\(lib/[^"]*\)"|\1|' | sort -u)
SRC_N=$(printf '%s\n' "$SRC_LINES" | grep -c . )
# Ask which part of the count is the harness: a grep whose pattern has rotted
# matches nothing and every per-file case below then passes over an empty list.
if [ "$SRC_N" -ge 3 ]; then
    ok "0a the source-line grep found $SRC_N libraries (>= 3)"
else
    bad "0a the source-line grep found only $SRC_N libraries — the pattern has rotted"
fi
while read -r f; do
    [ -n "$f" ] || continue
    if [ -f "$REPO/$f" ]; then
        ok "0b sourced by a fixture script and present in the fixture: $f"
    else
        bad "0b sourced by a fixture script but MISSING from the fixture: $f"
    fi
done <<< "$SRC_LINES"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "1. the happy path — the control for every case below"
# ═══════════════════════════════════════════════════════════════════════════

BUNDLE="$TMP/bundle"
cp -a "$BUNDLE_SRC" "$BUNDLE"
run "$BUNDLE" "$REPO"
if [ "$ST" -eq 0 ]; then
    ok "1a a good bundle installs and verifies clean"
else
    bad "1a a good bundle installs and verifies clean (exit $ST)"
    printf '%s\n' "$OUT" | tail -20 | sed 's/^/        /'
fi
for want in 'md5: all' '\+x: all' '\.app: all' 'default-app:' 'n: all .* /bin/sh' \
            'boot links resolve'; do
    if printf '%s\n' "$OUT" | grep -qE "$want"; then
        ok "1b every verify check ran: /$want/"
    else
        bad "1b a verify check did not run: /$want/"
    fi
done
# The usb group is ON by default, so its two links must be among the ones checked.
# Named explicitly because the generic 'boot links resolve' above still matches when
# they are silently left out — which is how the gap survived.
if printf '%s\n' "$OUT" | grep -qE 'boot links resolve .*S89.*S90'; then
    ok "1c the default run checks the usb group's boot links too (S89, S90)"
else
    bad "1c the default run checks the usb group's boot links too (S89, S90)"
fi
# Same for the bluetooth group, also on by default.
if printf '%s\n' "$OUT" | grep -qE 'boot links resolve .*S91'; then
    ok "1d the default run checks the bluetooth group's boot link too (S91)"
else
    bad "1d the default run checks the bluetooth group's boot link too (S91)"
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "2. sabotage: one check each"
# ═══════════════════════════════════════════════════════════════════════════

# ── md5. Change a staged file's bytes and leave the manifest alone: this is a
# truncated or corrupted write, which is what a failing card looks like.
B="$TMP/b-md5"; cp -a "$BUNDLE_SRC" "$B"
printf 'corrupted\n' >> "$B/root/opt/arca/apps/snake.app"
run "$B" "$REPO"
expect_fires 'md5 mismatch' "2a a corrupted staged file is caught by md5"

# ── the executable bit. The plausible defect: an installer that trusts the
# archive's modes instead of the DECLARED ones. On /mnt/c that bug is invisible
# (DrvFs reports 0777 and discards chmod); on real ext4 the check is a
# measurement, which is the whole reason it is in the offline tool and not here.
B="$TMP/b-nochmod"; cp -a "$BUNDLE_SRC" "$B"
R="$TMP/repo-nochmod"; cp -a "$REPO" "$R"
sed -i 's/^    chmod "\$mode" "\$dest"$/    : "no chmod — deliberately broken copy"/' \
    "$R/rootfs/commission-offline.sh"
grep -q 'no chmod — deliberately broken' "$R/rootfs/commission-offline.sh" \
    || bad "2b harness: the chmod sabotage did not apply"
# The bundle's staged files must not already be executable, or the check would
# pass for the wrong reason — cp -a from /mnt/c carries 0777.
find "$B/root" -type f -exec chmod 0644 {} +
run "$B" "$R"
expect_fires 'declared mode .* not executable' "2b an installer that skips chmod is caught by the +x check"

# ── .app exec=. A manifest naming a binary that is not there renders a launcher
# tile that does nothing when tapped, which looks exactly like broken touch.
# The .md5 line is regenerated so that ONLY this check fires.
B="$TMP/b-app"; cp -a "$BUNDLE_SRC" "$B"
sed -i 's|^exec=.*|exec=/opt/games/no_such_game|' "$B/root/opt/arca/apps/snake.app"
newmd5=$(md5sum "$B/root/opt/arca/apps/snake.app" | cut -d' ' -f1)
sed -i "s|^[0-9a-f]*  /opt/arca/apps/snake.app$|$newmd5  /opt/arca/apps/snake.app|" \
    "$B/manifest.d/native_apps.md5"
run "$B" "$REPO"
expect_fires 'exec=/opt/games/no_such_game is not installed' \
    "2c a manifest naming an absent binary is caught"

# ── default-app. /etc/init.d/arca-app respawns whatever this names, so a
# wrong value is a device that boots to a black screen and stays there.
B="$TMP/b-default"; cp -a "$BUNDLE_SRC" "$B"
printf '/opt/arca/not_a_launcher\n' > "$B/root/opt/arca/default-app"
newmd5=$(md5sum "$B/root/opt/arca/default-app" | cut -d' ' -f1)
sed -i "s|^[0-9a-f]*  /opt/arca/default-app$|$newmd5  /opt/arca/default-app|" \
    "$B/manifest.d/native_apps.md5"
run "$B" "$REPO"
expect_fires "default-app is '/opt/arca/not_a_launcher'" \
    "2d a default-app that names nothing installed is caught"

# ── dash -n. A parse error in an init script does not fail at install time; it
# fails at boot, on a device with no serial console.
#
# ⚠️ What `dash -n` does NOT catch is a BASHISM. `[[ -n "$x" ]]` parses fine —
# dash reads `[[` as a command name — so it passes the check and then fails at
# boot with "[[: not found". Measured while writing this case, which is why the
# sabotage below is a real syntax error instead. Catching bashisms needs
# the ShellCheck binary (0.7.0, installed here), which this suite does not invoke.
#
# ⚠️ And a comment whose FIRST word is `shellcheck` is read as a DIRECTIVE rather
# than as prose: a malformed one aborts the parse of the WHOLE file (SC1073/SC1072)
# and the tool exits 1 having checked nothing at all. This very paragraph did that
# until 2026-09-06 — a wrap had put the word at the start of a line. Keep it off
# column 2 when you mean the program, or wiring the tool in here reports a red gate
# with no findings behind it.
R="$TMP/repo-parse"; cp -a "$REPO" "$R"
printf 'if [ -n "$server" ]; then\n  echo unterminated\n' >> "$R/device-files/time-sync"
run "$BUNDLE" "$R"
expect_fires '\-n failed' "2e a parse error in a /bin/sh init script is caught"

# ── CRLF. BusyBox rejects `#!/bin/sh\r` with a misleading "no such file or
# directory" and the device boots to a black screen with no obvious cause. This
# is why .gitattributes pins device-files/** to eol=lf.
R="$TMP/repo-crlf"; cp -a "$REPO" "$R"
sed -i 's/$/\r/' "$R/device-files/audio-enable"
run "$BUNDLE" "$R"
expect_fires 'CRLF shebang' "2f a CRLF shebang is caught"

# ── the ARM gate's zero-artifact case. "no hardware divide in 0 binaries" is a
# pass over nothing, and it is the failure the plan warns about by name.
B="$TMP/b-noelf"; mkdir -p "$B/root/opt/arca/apps" "$B/manifest.d"
printf 'name=X\nexec=/opt/games/x\nicon=\nargs=\n' > "$B/root/opt/arca/apps/x.app"
printf '0644 /opt/arca/apps/x.app\n' > "$B/manifest.d/only.list"
printf '%s  /opt/arca/apps/x.app\n' \
    "$(md5sum "$B/root/opt/arca/apps/x.app" | cut -d' ' -f1)" > "$B/manifest.d/only.md5"
run "$B" "$REPO"
expect_fires 'no ELF binaries' "2g a bundle with no ARM binaries is refused, not silently passed"

# ── the ARM gate when the toolchain is absent. THE case that matters most: it must
# say so loudly rather than report a pass over zero artifacts. Simulated through
# the OBJDUMP override, because uninstalling binutils to test this is absurd.
set +e
OUT=$(OBJDUMP=definitely-no-such-objdump bash "$REPO/rootfs/commission-offline.sh" \
          --bundle "$BUNDLE" --base "$TMP/card" 2>&1); ST=$?
set -e
if [ "$ST" -ne 0 ] && printf '%s\n' "$OUT" | grep -q 'IS NOT INSTALLED' \
   && printf '%s\n' "$OUT" | grep -q 'refusing to install unverified'; then
    ok "2j a missing arm objdump refuses loudly, naming the count it did not check"
else
    bad "2j a missing arm objdump refuses loudly (exit $ST)"
fi

bash "$SCRIPT_DIR/make-fake-card.sh" "$TMP/card" >/dev/null
set +e
OUT=$(OBJDUMP=definitely-no-such-objdump bash "$REPO/rootfs/commission-offline.sh" \
          --bundle "$BUNDLE" --base "$TMP/card" --arm-check=skip 2>&1); ST=$?
set -e
if [ "$ST" -eq 0 ] && printf '%s\n' "$OUT" | grep -q 'NOT CHECKED'; then
    ok "2k --arm-check=skip proceeds, and the summary still says NOT CHECKED"
else
    bad "2k --arm-check=skip proceeds, and the summary still says NOT CHECKED (exit $ST)"
fi

# ── the bundle's own structural check, in both directions.
B="$TMP/b-unstaged"; cp -a "$BUNDLE_SRC" "$B"
rm -f "$B/root/opt/games/snake"
run "$B" "$REPO"
expect_fires 'not self-consistent' "2h a manifest entry with no staged file is refused"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "3. the card itself"
# ═══════════════════════════════════════════════════════════════════════════

# Wrong partition order: p2 mounted where p3 was expected. The one mistake that
# makes every later path resolve under the wrong tree.
bash "$SCRIPT_DIR/make-fake-card.sh" "$TMP/card" >/dev/null
mv "$TMP/card/root" "$TMP/card/.r"; mv "$TMP/card/log" "$TMP/card/root"; mv "$TMP/card/.r" "$TMP/card/log"
set +e
OUT=$(bash "$REPO/rootfs/commission-offline.sh" \
          --bundle "$BUNDLE" --base "$TMP/card" 2>&1); ST=$?
set -e
expect_fires 'do not look right|does not look like' "3a the four mounts in the wrong order are refused"

# A base that is not a card at all.
mkdir -p "$TMP/notacard"
set +e
OUT=$(bash "$REPO/rootfs/commission-offline.sh" \
          --bundle "$BUNDLE" --base "$TMP/notacard" 2>&1); ST=$?
set -e
expect_fires 'do not look right|does not look like' "3b a directory that is not a card is refused"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "4. --no-usb, and the removed --no-usb-power"
# ═══════════════════════════════════════════════════════════════════════════
#
# The tool no longer touches p1 at all, so --no-usb-power is gone with the p1
# patch it opted out of. A stale invocation must be REFUSED, not swallowed: a
# flag accepted and ignored reads as a choice honoured.

run "$BUNDLE" "$REPO" --no-usb-power
expect_fires 'Unknown provision group: usb-power' \
    "4a the removed --no-usb-power is refused by name, not silently accepted"

# ── --no-usb: the whole group goes, the run still succeeds, and it says so.
run "$BUNDLE" "$REPO" --no-usb
expect_says 'USB HOST MODE was skipped \(--no-usb\)' \
    "4b --no-usb succeeds and the closing summary names the flag"
# And the boot-link check must drop S89/S90 rather than fail over links nobody asked
# to install. This is the half of that check that could turn an opt-out into a
# verification failure, so it is asserted from the opt-out side.
#
# ⚠️ Asserted as "the line exists AND does not name S89", not as a match on the
# short list: commission-offline.sh's ok() appends ${NC}, so nothing can be anchored
# with `$`, and unanchored `boot links resolve \(S28, S29, S99\)` is a PREFIX of the
# default run's own message — it would pass whether or not the exclusion works.
_bl=$(printf '%s\n' "$OUT" | grep -E 'boot links resolve' | head -1)
if [ -n "$_bl" ] && ! printf '%s\n' "$_bl" | grep -q 'S89'; then
    ok "4c --no-usb drops S89/S90 from the boot-link check instead of failing on them"
else
    bad "4c --no-usb drops S89/S90 from the boot-link check instead of failing on them"
    printf '%s\n' "$OUT" | grep -E 'boot link' | sed 's/^/        /' | head -5
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "6. the image step takes no prompt and refuses the vendor-era flags"
# ═══════════════════════════════════════════════════════════════════════════
#
# commission-offline.sh is the installer rootfs/make-card-image.sh --bundle runs
# against partitions it has mounted; updates to a unit in service go online
# through provision.sh. A flag from the retired card-update path must be REFUSED
# by name: one accepted and ignored reads as a choice honoured, and --no-clean in
# particular would otherwise be a no-op that sounds like safety.
#
# Each refusal runs with a mounted fixture and a good bundle, so the only thing
# that can make it fail is the flag.

for flag in --no-clean --unattended --delete-factory --keep-factory --keep-sweeps \
            "--disk /dev/null" "--release latest"; do
    read -ra fa <<< "$flag"; run "$BUNDLE" "$REPO" "${fa[@]}"
    expect_fires "(^|[^-])${flag%% *}(=[^ ]*)?: removed" "6a the retired flag ${flag%% *} is refused by name, exit non-zero"
done

# The witness that the refusal is the flag's and not a general failure: the very
# same invocation without it succeeds (case 1a), and the fixture is untouched.
bash "$SCRIPT_DIR/make-fake-card.sh" "$TMP/card" >/dev/null
_sum0=$(find "$TMP/card" -type f -o -type l | sort | md5sum)
set +e
OUT=$(bash "$REPO/rootfs/commission-offline.sh" --bundle "$BUNDLE" --base "$TMP/card" --no-clean 2>&1 < /dev/null); ST=$?
set -e
if [ "$ST" -ne 0 ] && [ "$(find "$TMP/card" -type f -o -type l | sort | md5sum)" = "$_sum0" ]; then
    ok "6b a refused flag writes nothing to the card"
else
    bad "6b a refused flag writes nothing to the card (exit $ST)"
fi

# Never prompts: stdin is /dev/null and no flag asks for that. A prompt would see
# EOF and either refuse (the old backup question) or hang a terminal.
bash "$SCRIPT_DIR/make-fake-card.sh" "$TMP/card" >/dev/null
set +e
OUT=$(timeout 120 bash "$REPO/rootfs/commission-offline.sh" --bundle "$BUNDLE" --base "$TMP/card" < /dev/null 2>&1); ST=$?
set -e
if [ "$ST" -eq 0 ] && ! printf '%s\n' "$OUT" | grep -qiE 'backup\?|\(yes/no\)|\(y/n\)|\[y/N\]'; then
    ok "6c </dev/null runs to completion with no prompt"
else
    bad "6c </dev/null runs to completion with no prompt (exit $ST)"
    printf '%s\n' "$OUT" | tail -5 | sed 's/^/        /'
fi
# And nothing in the script can ask: no bare `read` (a read from a file is not a prompt).
if grep -nE "^[[:space:]]*read[[:space:]]" "$REPO/rootfs/commission-offline.sh" | grep -v "<" | grep -q .; then
    bad "6d commission-offline.sh contains a terminal read"
else
    ok "6d commission-offline.sh contains no terminal read"
fi

# A root without our marker (a vendor tree, or p2 mounted as p3) is not ours to install
# into: with no clean, the vendor stack would keep running beside the bundle.
bash "$SCRIPT_DIR/make-fake-card.sh" "$TMP/card" >/dev/null
rm -f "$TMP/card/root/etc/arca-rootfs"
printf 'SteelCase RW20 Embedded Platform (Yocto) 3.1.4 \\n \\l\n' > "$TMP/card/root/etc/issue"
set +e
OUT=$(bash "$REPO/rootfs/commission-offline.sh" --bundle "$BUNDLE" --base "$TMP/card" 2>&1 < /dev/null); ST=$?
set -e
expect_fires 'etc/arca-rootfs' "6e a root without our marker is refused, naming the marker"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "════════════════════════════════════════"
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $TOTAL total"
if [ "$TOTAL" -lt 36 ]; then
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
