#!/usr/bin/env bash
# doc_check.sh — documentation invariants that a host can check.
#
# Host-only: no device, no card, no root, no cross-compiler. Run it from WSL:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/doc_check.sh > out.txt 2>&1"
#
# Groups, each with its own negative control (`--self-test` runs them all):
#
#   A  Every markdown anchor in the repo resolves: a cross-file `<doc>.md#<frag>` and a
#      same-file `(#<frag>)` alike must name a slugified heading that exists. Moving or
#      retitling a section is otherwise silent — ~120 of these point into
#      SYSTEM_ANALYSIS.md from .sh, .c, .py and .conf files, which no markdown linter reads.
#
#   B  No file outside IMPROVEMENT_PLAN.md cites a plan ID that resolves to nothing there.
#      Two citation shapes are scanned: qualified (`IMPROVEMENT_PLAN.md B19`) and the bare
#      parenthetical tag (`(B19)`, `see B19`) that a qualified one decays into.
#
#   C  Extraction receipts. One row per fact moved OUT of IMPROVEMENT_PLAN.md by the
#      documentation cleanup: a distinctive token, the file that must now hold it, and
#      optionally the file it must have LEFT. Run it before a deletion to confirm the
#      destination has the fact, and after to confirm it survived.
#
#   D  Per-document size ceilings, counted in NON-BLANK lines. Every addition that grew
#      these documents to 891, 2287 and 2214 lines was individually justified, so judging
#      each addition on its merits does not bound the total — only a ceiling does, because
#      it is arithmetic rather than judgement and therefore survives a session that has
#      forgotten the rule. Blank lines are excluded so that deleting whitespace cannot pay
#      for an overage — a trade that passes a line-counting gate and breaks the markdown.
#
#   E  Structural markdown health: every scanned .md ends in a newline, and no column-0
#      paragraph sits directly after an indented bullet (CommonMark folds it into the
#      bullet). Both render as plausible markdown, so neither is visible in a diff.
#
# ── Why every scan here skips this file ───────────────────────────────────────
# A gate that documents the pattern it searches for matches its own documentation.
# Measured four times on this script and its sibling doc, each time inflating the number
# a run reported. So: all three scans skip `doc_check.sh`, coverage of it comes from the
# `--self-test` fixture, and anywhere a *doc* has to describe one of these shapes it
# writes `<placeholder>` brackets, which no scan here can match.
#
# ── Why group B is "cites an ID at all" and not "resolves to nothing" ──────────
# IMPROVEMENT_PLAN.md is a work QUEUE: an entry is executed and then DELETED outright, and
# `git log --grep` does not rescue the ID either (B3k and B13c return zero commits). So a
# citation is a reference with a scheduled expiry date, and the reason it was carrying dies
# with the entry. The rule is therefore absolute — no file but IMPROVEMENT_PLAN.md may cite a
# plan ID, resolving or not. A comment states its own reason; naming the file is still fine.
#
# ⚠️ This inverts the earlier rule, which required every cited ID to RESOLVE. That version was
# self-defeating: it made deleting a closed entry fail until its citations were rewritten, so
# the cheapest way to stay green was to keep dead entries alive. The gate was holding the queue
# open. Counting citations instead means a deletion can never break this group.
#
# This file is exempt, and has to be: it documents the shapes it searches for.
#
# ⚠️ The bare form is why the qualified scan alone was not enough. Measured 2026-08-15: with
# every qualified dangling citation fixed and the gate reporting a clean zero, 41 bare ones
# were still in the tree — `(B13g)`, `(B3k)`, `see B28`. A gate that passes on a repo which
# still violates its invariant is worse than no gate.
# ⚠️ The bare scan has a PERMANENT collision class, and a live heading was masking it: function
# keys. F1-F12 are plan IDs here AND key names, so `Save/load dialog (F5)` in a ScummVM table
# matched the bare scan and resolved silently while that heading existed. Deleting that entry
# (2026-09-03) turned one shipped key binding into a dangling citation. The tree was fixed by
# rewording that key row, not by excluding the file; narrowing the scan is filed as a chore.
#
# ⚠️ Both scans SKIP this file. Its header discusses IDs by writing them out, and a scan over
# its own source counted those as findings — three times, in three different shapes, each one
# inflating the number the run reported. Coverage of this file comes from the self-test
# fixture instead, which is a throwaway tree that cannot be mistaken for a real result.
#
# ⚠️ Known blind spots, both of which under-count rather than over-count:
#   - the scanner is line-based, so a qualified citation list broken across two comment
#     lines (`… C11,` / `# C12)`) has only its first ID seen;
#   - the bare scan matches only the parenthetical/`see`-prefixed shapes, because a bare
#     `B2` in running prose is indistinguishable from a register name — which is exactly
#     what `vnc_client/deps/`'s vendored libjpeg-turbo is full of (pruned below).
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLAN_FILE="IMPROVEMENT_PLAN.md"

fail_total=0

# ── the scanner ───────────────────────────────────────────────────────────────
# Files worth scanning, relative to $1. Prunes the three vendored trees that are not our
# source — scummvm/ and usb_host/linux-4.14.52/, which make a recursive walk from the repo
# root exceed a two-minute budget, and scummvm-icons/, an upstream icon set whose own
# .github templates would otherwise be judged by group E's markdown rules — plus the card
# captures. Measured 2026-08-19: scummvm-icons holds 5 scannable files and 0 anchors, so
# pruning it moves group A's count by nothing; it is pruned for E's sake, not A's.
# An extensionless file is scanned when it starts with a shebang: the device-files init
# scripts have no .sh, and nine plan-ID citations sat in them unseen (measured 2026-09-27).
# One batched awk reads the first lines: a fork per file cost ~40 min over ~5900 of them.
scan_files() {
    local root="$1"
    ( cd "$root" && find . \
        \( -name .git \
        -o -name scummvm \
        -o -name linux-4.14.52 \
        -o -name scummvm-icons \
        -o -name partitions \
        -o -name partitions.new \
        -o -name 'arca.new*' \
        -o -name HardwarePhotos \
        -o -name 'arm-deps*' \
        -o -name deps \
        \) -prune -o -type f \
        \( \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.sh' \
        -o -name '*.py' -o -name '*.md' -o -name '*.conf' \) -print \
        -o ! -name '*.*' -exec awk 'FNR == 1 { if (/^#!/) print FILENAME; nextfile }' {} + \) \
        | sed 's|^\./||' | sort )
}

# Plan IDs that have a heading in IMPROVEMENT_PLAN.md, one per line.
defined_ids() {
    grep -oE '^#{2,4} +[A-F][0-9]+[a-z]?\.' "$1/$PLAN_FILE" \
        | grep -oE '[A-F][0-9]+[a-z]?' | sort -u
}

# Every `IMPROVEMENT_PLAN.md <ID>` citation in the given files, as file:line:ID.
# Matches the two forms in the tree — a bare path in a code comment and a
# backticked path in markdown — plus comma- and slash-separated continuations
# (`F9, F10`, `B20/B25`). A `#fragment` after the path is an anchor, group A's job.
# The `.md` is optional because one site in the tree writes `IMPROVEMENT_PLAN B13g`.
cite_sites() {
    local root="$1"; shift
    ( cd "$root" && awk '
    {
        s = $0
        while (match(s, /IMPROVEMENT_PLAN(\.md)?`?[ \t]+`?[A-F][0-9]+[a-z]?/)) {
            st = RSTART; ln = RLENGTH
            hit = substr(s, st, ln)
            s   = substr(s, st + ln)
            if (match(hit, /[A-F][0-9]+[a-z]?$/))
                print FILENAME ":" FNR ":" substr(hit, RSTART, RLENGTH)
            while (match(s, /^(,[ ]*(and[ ]+)?|\/)`?[A-F][0-9]+[a-z]?/)) {
                st2 = RSTART; ln2 = RLENGTH
                seg = substr(s, st2, ln2)
                s   = substr(s, st2 + ln2)
                if (match(seg, /[A-F][0-9]+[a-z]?$/))
                    print FILENAME ":" FNR ":" substr(seg, RSTART, RLENGTH)
            }
        }
    }' "$@" )
}

# Bare plan-ID tags: the shape a qualified citation decays into once the filename is
# dropped. Only `(B19)`-style parentheticals and `see B19` / `is B19` prose are matched —
# a bare ID with no such marker cannot be told apart from a register or bitfield name.
#
# ⚠️ This scan skips THIS file. The header above discusses bare IDs by writing them out,
# and the self-test fixture is prose too, so a bare scan over its own source reports nine
# phantom findings — the second time this gate inflated its own number. The qualified scan
# still covers this file, via the interpolated fixture.
bare_sites() {
    local root="$1"; shift
    ( cd "$root" && awk '
    FILENAME ~ /doc_check\.sh$/ { next }
    {
        s = $0
        while (match(s, /(\(|see |is |was )[A-F][0-9]+[a-z]?([),.;: ]|$)/)) {
            st = RSTART; ln = RLENGTH
            hit = substr(s, st, ln)
            s   = substr(s, st + ln)
            if (match(hit, /[A-F][0-9]+[a-z]?/))
                print FILENAME ":" FNR ":" substr(hit, RSTART, RLENGTH)
        }
    }' "$@" )
}

# ── group A: every markdown anchor resolves ───────────────────────────────────
# GitHub's slug, measured against anchors already in this tree rather than assumed:
# heading text, lowercased, every character outside [a-z0-9 _-] deleted, then spaces
# turned into '-'. `### 5.2 As we run it — game mode` gives `52-as-we-run-it--game-mode`
# — the em-dash is deleted and leaves BOTH its spaces behind, so the double hyphen is
# correct. Backticks go and underscores stay: `### 4.5 Control block and \`boot_tracker\``
# gives `45-control-block-and-boot_tracker`.
#
# ⚠️ An ATX heading inside a fenced code block is not a heading. SYSTEM_ANALYSIS.md's
# recovery recipes are full of `# comment` lines inside ```bash fences — treating those as
# headings would let a dangling anchor resolve, which is a false PASS.
slugs_of() {
    awk '
    /^[ \t]*```/ { fence = !fence; next }
    fence        { next }
    /^#{1,6}[ \t]+/ {
        sub(/^#+[ \t]+/, ""); sub(/[ \t]+$/, "")
        s = tolower($0)
        gsub(/[^a-z0-9 _-]/, "", s)
        gsub(/ /, "-", s)
        print s
    }' "$1"
}

# Every anchor as file<TAB>line<TAB>link. Two shapes, both in the tree:
#   `path/to/doc.md#frag`  — bare in a code comment, or inside a markdown ]( ) target
#   `(#frag)`              — same-file, markdown only
# A fragment cannot contain `.`, because the slug rule deletes it — so excluding the dot is
# what stops the sentence-ending period of `(… doc.md#42-partitions).` being read as part of
# the fragment, which it was on the first run.
anchor_sites() {
    local root="$1"; shift
    ( cd "$root" && awk '
    FILENAME ~ /doc_check\.sh$/ { next }
    {
        s = $0
        while (match(s, /[A-Za-z0-9_.\/-]*\.md#[A-Za-z0-9_-]+/)) {
            print FILENAME "\t" FNR "\t" substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART + RLENGTH)
        }
        s = $0
        while (match(s, /\(#[A-Za-z0-9_-]+\)/)) {
            print FILENAME "\t" FNR "\t" substr(s, RSTART + 1, RLENGTH - 2)
            s = substr(s, RSTART + RLENGTH)
        }
    }' "$@" )
}

# Resolve a link path against the citing file's directory, collapsing . and ..
# ($1 = dirname of the citing file, "." at the repo root; $2 = the link's path part.)
norm_path() {
    local p="$2"
    [ "$1" = "." ] || p="$1/$2"
    printf '%s\n' "$p" | awk -F/ '{
        n = 0
        for (i = 1; i <= NF; i++) {
            if ($i == "." || $i == "") continue
            else if ($i == "..") { if (n > 0) n-- }
            else a[++n] = $i
        }
        out = ""
        for (i = 1; i <= n; i++) out = out (i > 1 ? "/" : "") a[i]
        print out
    }'
}

# ⚠️ The path resolves relative to the citing file and then, failing that, from the repo
# root — because most of these anchors are not links. A comment in `lib/arca-identify.sh` or a
# backticked pointer in `lib/CLAUDE.md` writes the doc's plain filename and means the one at
# the root; requiring `../` there would put path arithmetic into 25 human-readable comments
# to satisfy a checker. The invariant worth having is that the **fragment** names a real
# heading, and the root fallback keeps a filename typo caught (nothing of that name exists
# anywhere) while dropping the noise.
#
# ⚠️ One blind spot, taken deliberately: a fragment that is 3 or 6 hex digits is skipped as a
# CSS colour. `browser_games/README.md` documents its palette as `(#0a0e27)`, which is
# `(#<frag>)` to a scanner. A heading whose slug is six hex digits would go unchecked.
group_a() {
    local root="$1" quiet="${2:-}"
    local bad=0 total=0 f ln link tgt frag key cache
    local -a files=()
    mapfile -t files < <(scan_files "$root" | grep -v 'doc_check\.sh$')
    [ ${#files[@]} -eq 0 ] && { echo 0 > "$RESULT"; return 0; }

    cache="$(mktemp -d)"
    while IFS=$'\t' read -r f ln link; do
        frag="${link#*#}"
        [[ "$frag" =~ ^[0-9a-fA-F]{3}$|^[0-9a-fA-F]{6}$ ]] && continue
        total=$((total + 1))

        if [ "${link:0:1}" = "#" ]; then
            tgt="$f"
        else
            tgt="$(norm_path "$(dirname "$f")" "${link%%#*}")"
            [ -f "$root/$tgt" ] || tgt="$(norm_path "." "${link%%#*}")"
        fi

        key="$cache/$(printf '%s' "$tgt" | tr '/' '_')"
        if [ ! -e "$key" ]; then
            if [ -f "$root/$tgt" ]; then slugs_of "$root/$tgt" > "$key"
            else : > "$key"; : > "$key.missing"; fi
        fi

        if [ -e "$key.missing" ]; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  NO SUCH FILE  %s:%s  -> %s\n' "$f" "$ln" "$tgt"
        elif ! grep -qxF -- "$frag" "$key"; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  DANGLING      %s:%s  #%s is no heading in %s\n' "$f" "$ln" "$frag" "$tgt"
        fi
    done < <(anchor_sites "$root" "${files[@]}")

    rm -rf "$cache"
    [ -n "$quiet" ] || printf 'group A: %d anchors, %d resolve to nothing\n' "$total" "$bad"
    echo "$bad" > "$RESULT"
    [ "$bad" -eq 0 ]
}

# ── group B ───────────────────────────────────────────────────────────────────
group_b() {
    local root="$1" quiet="${2:-}"
    local ids_file hits_file dangling=0 total=0

    ids_file="$(mktemp)"; hits_file="$(mktemp)"
    defined_ids "$root" > "$ids_file"

    local -a files=()
    mapfile -t files < <(scan_files "$root" | grep -v "^$PLAN_FILE\$" | grep -v 'doc_check\.sh$')
    [ ${#files[@]} -eq 0 ] && { rm -f "$ids_file" "$hits_file"; return 0; }

    cite_sites "$root" "${files[@]}" > "$hits_file"
    bare_sites "$root" "${files[@]}" >> "$hits_file"
    total=$(wc -l < "$hits_file")

    while IFS= read -r hit; do
        local id="${hit##*:}"
        grep -qx "$id" "$ids_file" || dangling=$((dangling + 1))
        [ -n "$quiet" ] || printf '  %s\n' "$hit"
    done < "$hits_file"

    rm -f "$ids_file" "$hits_file"
    [ -n "$quiet" ] || printf 'group B: %d plan-ID citations outside %s, %d of them already resolving to nothing\n' \
        "$total" "$PLAN_FILE" "$dangling"
    echo "$total" > "$RESULT"
    [ "$total" -eq 0 ]
}

# ── group C: extraction receipts ──────────────────────────────────────────────
# Fields: TOKEN <TAB> file-that-must-contain-it <TAB> file-it-must-have-left (or "-")
#
# A row exists because the fact was unique to IMPROVEMENT_PLAN.md and that entry was cut.
# The `must have left` column is what makes this a *move* check rather than a copy check —
# "one fact, one home" is the rule, and a fact present in both files is the drift the whole
# cleanup exists to remove.
#
# ⚠️ What this group CANNOT see, all of which under-report:
#   - **A token can be present and the claim around it wrong.** This greps for a string; it
#     cannot read the sentence. Three of phase 2's 83 comments were stale about the tree
#     while their ID matched perfectly, which is the same failure one level down.
#   - **A fact extracted without a row here is invisible.** The table is hand-maintained, so
#     a clean run says "every receipt I was told about holds", never "nothing was lost".
#     That is why the ⚠️-warning survival census stays a per-phase manual control.
#   - **It cannot tell prose from a code fence or a see-also line.** A token parked in a
#     stray reference passes exactly like a token in the paragraph that explains it.
#   - **A token that is a substring of something else passes for free.** Prefer md5s, hex
#     addresses and `file.c:line` forms over words.
#
# ⚠️ **Every key here is a DURABLE token — an identifier, a filename, a constant, a measured
# number, a board designator — and 14 of them used to be prose phrases.** That was the
# gate's own advice being ignored by the gate's own table, and it cost two sessions: a
# legitimate rewording of a sentence reads as a lost fact, so `./tests/doc_check.sh` failed
# for a reason unrelated to the defect it exists to catch. Converted 2026-08-19, each new
# key measured present at its destination and absent from its source before it went in.
# ⚠️ One row has no true identifier available and is the weakest here: `forces 32bpp` stands
# in for "16bpp bands every gradient", because that claim has no constant, function or
# measurement attached to it — every identifier in its paragraph is shared with the source
# file, which would fire NOT MOVED. If you give that fact a number, re-key the row to it.
# ⚠️ And when you ADD a row, do not key it on a sentence. `grep -n` the destination for a
# `#define`, a function name, a `file.c:line` or a measured value in the same paragraph.
receipts() {
    # `RECEIPTS_FILE` exists only so --self-test can drive this group over a fixture table.
    if [ -n "${RECEIPTS_FILE:-}" ]; then cat "$RECEIPTS_FILE"; return; fi
    # shellcheck disable=SC2016
    cat <<'EOF'
source-path=SCRIPTDIR	tests/CLAUDE.md	IMPROVEMENT_PLAN.md
0x480AB060	usb_host/README.md	IMPROVEMENT_PLAN.md
oss_keepalive.c	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
clock_gettime64	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
743 ms	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
audio_interrupt	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
audio_stream_start	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
1486	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
22,317	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
button_check_tap	native_apps/CLAUDE.md	-
arca_provision_push_installs	lib/CLAUDE.md	-
TouchCalibSweep	native_apps/CLAUDE.md	SYSTEM_ANALYSIS.md
clamp_to_hw	native_apps/CLAUDE.md	SYSTEM_ANALYSIS.md
11.4 MB/s	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
12287	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
clampedAdd	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
stay public for as long as	LICENSE.md	IMPROVEMENT_PLAN.md
AUDIO_VOL_UNITY	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
0..60000	native_apps/CLAUDE.md	SYSTEM_ANALYSIS.md
touch_fit_axis_range	native_apps/CLAUDE.md	SYSTEM_ANALYSIS.md
FB_TOUCH_INSET_MAX	native_apps/CLAUDE.md	SYSTEM_ANALYSIS.md
0x7bf1	kernel/README.md	IMPROVEMENT_PLAN.md
jitterentropy_rng	kernel/README.md	IMPROVEMENT_PLAN.md
ic_id_table	kernel/README.md	IMPROVEMENT_PLAN.md
algif_skcipher	kernel/README.md	IMPROVEMENT_PLAN.md
926896a5	kernel/README.md	IMPROVEMENT_PLAN.md
static-only-last	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
screen_true_panel_width	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
fb_scale_bezel_to_surface	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
fb_to_game_ppm	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
kFeatureCursorPalette	scummvm-arca/CLAUDE.md	IMPROVEMENT_PLAN.md
drawCursor()	scummvm-arca/CLAUDE.md	IMPROVEMENT_PLAN.md
publish_safe_area	native_apps/CLAUDE.md	SYSTEM_ANALYSIS.md
X 10..4076	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
1,536,000	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
SYN_REPORT	CLAUDE.md	native_apps/CLAUDE.md
1000000L	CLAUDE.md	native_apps/CLAUDE.md
/dev/uinput	CLAUDE.md	native_apps/CLAUDE.md
15/15/0/0	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
SAFE AREA	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
+19	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
0 1020 3074 4095	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
forces 32bpp	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
XRGB8888	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
SPKR1	SYSTEM_ANALYSIS.md	native_apps/CLAUDE.md
35280	native_apps/CLAUDE.md	IMPROVEMENT_PLAN.md
RW29 1G-093	HARDWARE.md	SYSTEM_ANALYSIS.md
W180322	HARDWARE.md	SYSTEM_ANALYSIS.md
D9RMJ	HARDWARE.md	SYSTEM_ANALYSIS.md
`J1`	HARDWARE.md	SYSTEM_ANALYSIS.md
22.86	HARDWARE.md	SYSTEM_ANALYSIS.md
SLEEP_RQ	HARDWARE.md	SYSTEM_ANALYSIS.md
T1OUT	HARDWARE.md	SYSTEM_ANALYSIS.md
TP39	HARDWARE.md	SYSTEM_ANALYSIS.md
560-0540-0x	HARDWARE.md	SYSTEM_ANALYSIS.md
Top-Overwiev	HARDWARE.md	SYSTEM_ANALYSIS.md
bezel_with_touch_screen.jpg	HARDWARE.md	SYSTEM_ANALYSIS.md
does not reach the SoC	HARDWARE.md	IMPROVEMENT_PLAN.md
three full wraps	native_apps/common/audio_wav.h	IMPROVEMENT_PLAN.md
filter: unspecified	.gitattributes	IMPROVEMENT_PLAN.md
averages rather than sums	LICENSE.md	IMPROVEMENT_PLAN.md
18.7 M	native_apps/common/audio.h	IMPROVEMENT_PLAN.md
DURING playback	native_apps/common/audio.c	IMPROVEMENT_PLAN.md
1179	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
0d8c:0014	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
--no-build	tests/CLAUDE.md	IMPROVEMENT_PLAN.md
sudo: a password is required	tests/commission_offline_test.sh	IMPROVEMENT_PLAN.md
binutils-arm-linux-gnueabihf	setup-build-env.sh	IMPROVEMENT_PLAN.md
python3-pil	setup-build-env.sh	IMPROVEMENT_PLAN.md
run-all.sh	tests/CLAUDE.md	-
shellcheck-baseline.txt	tests/CLAUDE.md	-
harness-error channel	tests/arca_provision_test.sh	IMPROVEMENT_PLAN.md
musb_debug.h:38-41	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
omap2430.c:171-181	SYSTEM_ANALYSIS.md	IMPROVEMENT_PLAN.md
BUSYBOX_SHOW_OTHERS	rootfs/CLAUDE.md	IMPROVEMENT_PLAN.md
S01seedrng	rootfs/CLAUDE.md	IMPROVEMENT_PLAN.md
GLIBC_2.28	rootfs/CLAUDE.md	IMPROVEMENT_PLAN.md
config.c:65	rootfs/CLAUDE.md	IMPROVEMENT_PLAN.md
nothing from Steelcase on the card, at all	rootfs/CLAUDE.md	IMPROVEMENT_PLAN.md
EOF
}

group_c() {
    local root="$1" quiet="${2:-}"
    local bad=0 total=0 tok dest gone

    while IFS=$'\t' read -r tok dest gone; do
        [ -z "$tok" ] && continue
        total=$((total + 1))
        if [ ! -f "$root/$dest" ]; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  MISSING FILE  %s (receipt for %s)\n' "$dest" "$tok"
            continue
        fi
        if ! grep -qF -- "$tok" "$root/$dest"; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  NOT EXTRACTED %s  is absent from %s\n' "$tok" "$dest"
        fi
        [ "$gone" = "-" ] && continue
        if [ -f "$root/$gone" ] && grep -qF -- "$tok" "$root/$gone"; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  NOT MOVED     %s  still in %s as well\n' "$tok" "$gone"
        fi
    done < <(receipts)

    [ -n "$quiet" ] || printf 'group C: %d receipts, %d unsatisfied\n' "$total" "$bad"
    echo "$bad" > "$RESULT"
    [ "$bad" -eq 0 ]
}

# ── group D: per-document size ceilings ───────────────────────────────────────
# Fields: ceiling <TAB> path
#
# ⚠️ **It counts NON-BLANK lines.** Counting all lines made blank lines legal currency, and
# that is not a theoretical loophole — a 2-line overage paid for by deleting two blank lines
# passes the gate and silently breaks the markdown, because a column-0 paragraph that loses
# the blank line above it becomes a lazy continuation and is swallowed into the preceding
# indented bullet. Group E catches that shape now; counting non-blank lines removes the
# incentive at the source. Whitespace is not content, so it is free in both directions.
#
# The ceilings below were re-derived when the basis changed (2026-08-19), preserving each
# file's granted margin EXACTLY: new_cap = nonblank_today + (old_cap - total_today). No file
# gained or lost headroom in the conversion — SYSTEM_ANALYSIS.md was at 0 margin before and
# is at 0 margin after. So the raise history recorded below still reads correctly as the
# argument for each grant, even though its numbers were in the old unit.
#
# This group is not about tidiness. Every addition that grew these documents to 891,
# 2287 and 2214 lines was individually justified, so a week of cleanup was the price.
# Judging each addition on its merits does not bound the total; a ceiling does, because
# it is arithmetic rather than judgement and so it survives a session that has forgotten
# the rule. `.claude/skills/doc-update/SKILL.md` is the authoring half of the same rule.
#
# The ceilings are where the 2026-08-16 cleanup landed, plus room for ONE substantial
# addition. So a genuinely new measured fact does not fail the gate and a second one
# without a deletion does. Two legitimate responses when it fires, and only two:
#   1. PAY for the addition — delete what the document no longer needs, or move a block
#      to the document whose job it is, which is usually where it should have been.
#   2. RAISE the ceiling, in a commit that argues for it. Deliberate growth is fine;
#      what this catches is growth nobody decided on.
#
# ⚠️ A ceiling naming a file that is not there is a FAILURE, not a skip. A renamed or
# split document would otherwise be unguarded forever while it grew — and phase 5 split
# HARDWARE.md out of SYSTEM_ANALYSIS.md, so that is not hypothetical here.
# ⚠️ The authoring skill is on this list too. A rulebook that grows without limit is the
# thing it exists to prevent, and it has no other check on it.
# ⚠️ 2026-09-01, a NEW entry rather than a raise: README.md at 190, its measured non-blank count
# after the commit that added it. It was absent for no decided reason — the 2026-08-16 cleanup that
# set this group's basis did not cover it — and the commit adding it grew it 163 -> 190 non-blank
# documenting the front door and the release path, which is the growth shape this group exists to
# bound. It is the ONE entry deliberately granted zero margin from the start, and on a different
# argument from the zero-margin files above: README is NAVIGATIONAL. Its job is to point, and every
# new capability wants a mention in it, so the pressure that keeps it correct is that a new mention
# is paid for by trimming what a component README or CLAUDE.md now owns better. Raising it with an
# argument is the intended escape, not a defeat.
# ⚠️ 2026-09-23, two NEW entries: kernel/README.md at 100 and kernel/CLAUDE.md at 40, against
# measured non-blank counts of 79 and 26 in the commit that created them. Unlike README.md above,
# these get margin on purpose: kernel/README.md is the WORKING NOTES of an open effort (panel, touch,
# musb still to land), so it is expected to grow by a row per driver, and a ceiling with no room would
# only push those rows into SYSTEM_ANALYSIS.md, which is the wrong home. Device facts found there
# still move out to SYSTEM_ANALYSIS.md; only build, patch and method prose stays.
# ⚠️ tests/CLAUDE.md is 232 and native_apps/CLAUDE.md 629, both raised deliberately in the
# 2026-08-20 commit that landed F1 Phase 8's device half: `audio.c` gained a public
# `audio_music_*`/`audio_sfx_play()` surface and `common/audio_wav.c` became a COMMON_OBJ member,
# so the authoring guide's audio bullet had to name them (+3, and it corrected a $COMMON_OBJ list
# that had been missing `audio_out.o` since Phase 2). tests/CLAUDE.md's +9 is the sabotage-sweep
# rule that Phase 8 paid for the hard way: an assertion that rendered `left + 8` frames read
# TRAILING SILENCE and passed 56/56 with the envelope deleted, and only the 16-case sweep found it.
# Both raises were audited for a deletion first and neither file had one left.
# ⚠️ SYSTEM_ANALYSIS.md +2 (1850 to 1852), 2026-10-01: BlueALSA's D-Bus socket-path trap, the alsa-lib plugin/conf dirs
# and the SDP re-run rule are device facts with no other home; no deletion candidate was left in 3.6.
# ⚠️ SYSTEM_ANALYSIS.md is 1587 rather than 1581, raised in the 2026-08-20 commit that closed the
# two-voice distortion hunt. That hunt ran across four sessions and killed five candidate causes;
# what closed it is a DEVICE fact — a pre-mixed two-sine WAV through the vendor's own `aplay`, none
# of this repo's code in the path, distorting identically to our bus, while two broadband streams at
# the same level are clean. #34 is its only home, and it PAID for one line by deleting the
# "budget level per frequency" prescription the same measurement supersedes. Audited for more: the
# surrounding one-voice ladder is [n=1] measured data that no other document records.
# ⚠️ tests/CLAUDE.md was 223 rather than 214 before that, raised in the 2026-08-19 commit that
# added group E: this gate gained a fifth group, changed group D's counting basis and re-keyed
# 14 group-C receipts, and all three of those are documented in that file and nowhere else.
# One line was PAID (a see-also that pointed at a section two paragraphs up, plus a prune
# sentence folded into the one that already existed); the other nine are the raise. ⚠️ **No
# headroom is granted with either raise** — the next addition to those files pays by deleting.
# ⚠️ tests/CLAUDE.md was 250 rather than 235, in the old all-lines basis, because the commit
# that INTRODUCED these ceilings spent that file's whole headroom documenting this group — so the headroom was
# never actually granted. Raised deliberately, in that same commit, which is the escape
# hatch working as designed rather than an exception to it. Every other row still holds
# its original grant; check the margin before assuming a row is generous.
# ⚠️ IMPROVEMENT_PLAN.md is 1515 rather than 1500 because F1 defect 3's recorded cause was
# REFUTED and replacing it cost more lines than the wrong version had: a 4-row evidence
# table plus the five suspects that measurement killed, which is what stops the next
# session re-raising them. Raised to cover exactly that, and the ~20 lines of headroom are
# deliberately NOT restored. ⚠️ **That identified payment has now been SPENT** (2026-08-17): F1's
# Phase-3 design-rule bullets were verified at native_apps/CLAUDE.md → *Mixing*, receipted in group C
# below, and deleted in favour of a pointer — which is where this file's present margin came from.
# **The next addition needs a NEW way to pay; there is no queued one.**
# ⚠️ 2026-08-17, second raise, deliberate and argued: SYSTEM_ANALYSIS.md 1900 → 1930 and
# IMPROVEMENT_PLAN.md 1515 → 1545 (the figure is the measured
# post-edit line count, not an estimate -- the first attempt at this comment guessed 1535 and the
# gate caught it, which is the group working). F1 defect 3's "the OSS path corrupts it" cause was REFUTED by a
# controlled A/B, and replacing a wrong cause with a measured refutation costs more lines than the
# wrong version had — the same arithmetic as the 1500 → 1515 raise above, one fault later. The
# SYSTEM_ANALYSIS.md half is four new device facts, one of which (there is no microphone and the
# codec's loopbacks run capture → playback, so audio CANNOT be measured acoustically here) closes off
# a whole class of future investigation and is worth its lines several times over. ⚠️ **Neither raise
# leaves queued headroom: the next addition to either file pays by deleting.**
# ⚠️ 2026-08-17, THIRD raise, deliberate and argued: SYSTEM_ANALYSIS.md 1930 → 1940 and
# IMPROVEMENT_PLAN.md 1545 → 1580 (measured post-edit counts, not estimates). Two new measured
# device facts, both found in one evening at the panel. (1) B33 — a USB babble error leaves a printk
# loop that outlives unplugging and hard-resets the unit ~46 min later. It is worth its lines twice:
# it is a defect, and it is a MEASUREMENT CONTAMINANT, so every future on-device reading needs its
# one-command check. (2) The audible click at every sound is now a named mechanism with a runtime
# knob (pmdown_time = 5000) plus a stream-stop teardown, replacing a section that said only "the
# candidates that remain". F1's half also shrank where it could: the four refuted-mechanism bullets
# were compressed to three, keeping every measurement.
# ⚠️ **Identified future payment, not yet spendable:** when the continuous-stream fix lands, F1
# defect 3 loses BOTH its refuted-mechanism inventory and its onset question — that is where the
# next ~25 lines come from. Until then there is no queued headroom in either file.
# ✅ **That payment was COLLECTED 2026-08-20 and it came in far above its estimate**: F1's phase table
# lost its eight closed rows, the LIM A/B, the level derivation, the exoneration, the refuted-mechanism
# inventory and the onset question, and F19 lost the playback path it no longer owns —
# IMPROVEMENT_PLAN.md fell 1319 → ~1198 non-blank against a ceiling of 1321. ⚠️ **The ceiling was NOT
# lowered to match, deliberately**: F1 phase 5 (the games' sound sets) and phase 6 are unwritten, and
# lowering a ceiling onto a file that is mid-feature buys nothing and blocks the next honest addition.
# ⚠️ 2026-08-20, FOURTH raise, deliberate and argued: SYSTEM_ANALYSIS.md 1587 → 1592 (measured
# post-edit count). One measured device fact with no home anywhere in the docs — the card's sequential
# read speed, ~11.4 MB/s, against the 88.2 KB/s an uncompressed mono bed consumes. It is worth its five
# lines because it settles a CLASS of question rather than one: whether any asset can be streamed off
# the card instead of loaded into 234 MB of RAM. The same commit removes ~121 non-blank lines from
# IMPROVEMENT_PLAN.md, so the documentation shrinks by ~116 net — which is the argument this raise rests
# on, since per-file ceilings cannot themselves see that. ⚠️ **No headroom is granted: 1592 is the
# measurement, and the next addition to that file pays by deleting.**
# ⚠️ It counts non-blank LINES, which is a proxy for size and can still be gamed in both
# directions: a paragraph rewrapped to 200 characters passes while getting longer, and a
# table split across more rows fails while saying the same thing. The ~110-char wrap is a
# review rule this cannot see. Read the diff; do not let a green D stand in for that.
# 2026-08-20, native_apps/CLAUDE.md 629 → 660. F1 Phase 5 put the first two games on the continuous
# stream, and that landed FOUR authoring rules this file is the only home for: a game wants CONT rather
# than PUMP; `audio_pump()` lives outside the draw branch; `audio_interrupt()` before an effect discards a
# fanfare that is still playing (measured by ear, and NO counter sees it); and a blocking sub-loop is a
# render loop, so `keyboard_enter()` losing the sound queued before it is a rule and not a one-off. The
# same commit paid 6 lines by compressing two bullets whose facts are homed in §3.4 and F1, and it adds a
# gate (`check-audio-pacing.sh`) whose existence has to be discoverable from the doc that states the rule
# it enforces. ⚠️ 660 grants ~6 lines of headroom, not more: the file was already AT its ceiling when this
# started, which is what made the raise necessary rather than optional.
# ⚠️ 2026-08-21, FIFTH and SIXTH raises, both deliberate and argued, both spending that headroom and a
# little more. SYSTEM_ANALYSIS.md 1592 → 1597: one measured device fact with no possible home elsewhere —
# the speaker's USABLE BAND (sharp rolloff below ~700 Hz, inaudible below ~300 Hz at viewing distance). It
# earns five lines the way the card's read speed did, by settling a CLASS of question rather than one:
# which frequencies ANY effect may use. It also retires the pitch half of an unrun instrument script and
# explains every faintness report in the tree, so it removes future work rather than describing past work.
# native_apps/CLAUDE.md 660 → 665: F1 Phase 5 ① closing across all seven games landed two authoring facts
# this file is the only home for — `snake`'s play-sleep exception to the three-line pump shape (its sleep IS
# its step interval, so it is SPLIT rather than shortened), and that two files under `tests/` are shipped
# launcher tiles and therefore not expendable. The interrupt-before-an-effect rule was rewritten IN PLACE
# rather than appended to, since its original evidence is refuted and its rule is not.
# ⚠️ **No headroom is granted by either: 1597 and 665 are the measurements, and the next addition to
# either file pays by deleting.**
# ⚠️ 2026-08-21, FIFTH raise, deliberate and argued: IMPROVEMENT_PLAN.md 1321 → 1352 (measured
# post-edit count). F1 phase 5 gained a fifth part — three sound requests the operator made BY NAME after
# the first bed listen (stop the music between levels, per-level tracks, MUSIC/EFFECTS off on the games
# menu) — plus B35 and the closure detail of ③. The same commit collected ~50 non-blank lines by deleting
# the ① and ② reports now that both are closed and their rules live in native_apps/CLAUDE.md, and by
# compressing three superseded coverage paragraphs; the raise is what the requests cost NET of that.
# ⚠️ **No headroom is granted: 1352 is the measurement, and the next addition to that file pays by
# deleting.** ⚠️ And phase 5 is still mid-feature, so do not lower it back onto ④/⑤ landing.
# ⚠️ 2026-08-24, deliberate and argued: native_apps/CLAUDE.md 708 → 712 (measured post-edit).
# F1 Phase 5 ⑤ closed its last two ordering defects and both had shipped broken in ALL SEVEN games at
# once, which is the signature of a missing authoring rule rather than seven mistakes. The rule this file
# is the only home for is the CALLER's half of the blocking-sub-loop contract: a per-frame service that
# the sub-loop's own screen change should trigger has to run BEFORE the block that opens it, because
# `gameover_update()`'s name entry lives inside the redraw block and a bed serviced after it never sees
# the transition. Servicing the pump — the fix that closed row 6 — does not save it, which is exactly why
# the residual half survived a session. The same commit paid 2 lines by compressing the envelope fact
# (stated twice in this file) to a pointer and by dropping a prose COUNT from the gate bullet; the raise is
# the remaining 4. ⚠️ **No headroom is granted: 712 is the measurement, and the next addition pays by
# deleting.** The two new obligations are GATED (`check-audio-pacing.sh`, a fixture control each), so the
# doc states the rule and the gate enforces it — neither is load-bearing alone.
# ⚠️ 2026-08-23, SEVENTH raise, deliberate and argued: IMPROVEMENT_PLAN.md 1352 → 1362 (measured
# post-edit count). Phase 5 ⑤ gained one CLOSED item (brick_breaker's lost ball holds the bed) and two
# OPEN findings that came off one ear report: the bed is never told to STOP before a blocking sub-loop
# opens in all seven games (row 6's residual half — the pump was fixed, the ORDERING was not), and six of
# seven games never call audio_gameover(). The same commit paid ~12 lines by deleting ③'s anticipation of
# the moment a raw audio_tone() site gets a name — that moment arrived, so the rule now lives beside the
# code in common/audio.{c,h} — and by compressing ②'s refuted-diagnosis paragraph to its two standing
# "do not re-raise" claims. The raise is what the two new findings cost NET of that.
# ⚠️ **No headroom: 1362 is the measurement, and the next addition to that file pays by deleting.**
# ⚠️ 2026-08-21, SIXTH raise, deliberate and argued: native_apps/CLAUDE.md 665 → 668 (measured post-edit
# count). The clip bank is new library surface behind the four canned sounds (F1 phase 5 ③) and its rules
# are ones a caller gets wrong silently — a same-frame double trigger sums COHERENTLY, and the clips have
# to be DEPLOYED or the fix is inert.
# The 2026-08-22 raise pays for *Sound assets*: the effects became SOURCED rather than generated, which
# retires fx_gen.c's gate and puts two silent failure modes (a refused sample rate, a clip shorter than the
# mixer's own envelope) in the path of anyone adding one. Nothing in the file went stale to fund it.
# ⚠️ **No headroom is granted: 684 is the measurement.**
# ⚠️ 2026-08-22, SEVENTH raise, deliberate and argued: native_apps/CLAUDE.md 684 → 701 (measured post-edit
# count). F1 phase 5 ⑤ landed as a whole — the games-menu MUSIC/EFFECTS toggles, a music PLAYLIST, and a
# death that holds the bed rather than stopping it — and two of its rules have no other home because they
# are about how to CALL this directory's library rather than about the device. (1) The toggles are enforced
# in the library, so a game must NOT re-check them; `audio_music_enabled()` exists to explain a silence in a
# log line, and both toggles read TRUE through `audio_init_unchecked()` so a speaker test still works. (2)
# A bed's hold-versus-stop distinction IS the difference between "the music continues" and "the level
# restarts its music", which is a behaviour a future game will otherwise re-derive. It also lands
# `check-sound-assets.sh`, whose existence has to be discoverable from the doc stating the rule it enforces
# — the same argument as check-audio-pacing.sh two raises up. The same commit paid where it could: the
# *Sound assets* section absorbed the gate in place of its "ffprobe before deploying" line, and the bed
# bullet's "device-only files" claim was CORRECTED rather than appended to, since the beds are now committed
# and deployed. The other three docs did not grow; IMPROVEMENT_PLAN.md SHRANK by ~1 net while gaining all of
# ⑤'s and ③'s results, by compressing F19 to its open remainder and deleting two closed rationales.
# ⚠️ **No headroom is granted: 701 is the measurement, and the next addition pays by deleting.**
# ⚠️ 2026-08-22, EIGHTH raise, deliberate and argued, and it goes AGAINST the note above — read that as the
# cost being acknowledged rather than dodged: native_apps/CLAUDE.md 701 → 708 (measured post-edit count).
# One rule, and it is the rule that makes every OTHER receipt in this directory readable: stdout is a LOG
# FILE at boot, so glibc block-buffers it and a printf receipt does not arrive. It was measured the hard
# way — compute_grid_layout()'s layout receipt was absent from app_stdout.log while `grep -ac` on the
# deployed binary found the string in it — and the thing that makes it cost a session rather than a minute
# is that common/logger.c line-buffers its OWN file, so the logger's lines are all present and only the
# printf ones are missing, which reads as a missing printf. It has no other home: it is a rule about how to
# write main() in this directory, not a device fact. The commit paid what it could elsewhere rather than
# here — SYSTEM_ANALYSIS.md took the card's WRITE rate at net ZERO by compressing the read-rate paragraph it
# belongs beside, and the beds' `<game><n>-mono.wav` naming rule was deliberately NOT added to this file:
# it lives in native_apps/sound-sets.sh, the one home for it, and at both consumers of that table.
# ⚠️ **No headroom is granted: 708 is the measurement, and the next addition pays by deleting.**
# ⚠️ 2026-09-01, NINTH raise, deliberate and argued: lib/CLAUDE.md 214 → 232 (measured post-edit count).
# A new subsystem landed in this directory — lib/arca-release.sh, the release fetch — and its rules have no
# other home: they are about how to call these libraries, not device facts and not open work. The raise buys
# the four that cost real time to establish and would each be re-derived from scratch otherwise: gh release
# download prints NOTHING for a 125 MB asset (which is why the library carries its own progress), GitHub
# publishes sha256 while every manifest here is md5, there is exactly ONE owner/repo derivation because gh
# cannot resolve an SSH host alias at all, and the no-jq JSON parse must split on commas because an asset
# nests an uploader object. The same commit paid several times over, and NOT out of this file: two closed
# entries were DELETED from IMPROVEMENT_PLAN.md and a third compressed to its open remainder, taking that
# document ~1081 → ~1040 non-blank while it GAINED two newly-recorded open items. Net documentation change
# across the repo is ~23 lines SMALLER. Two stale claims were CORRECTED rather than appended to, per the
# overwrite rule: this file's "--tag has never run" (it has, v1.0.0) and arca-bundle.sh's "ScummVM is GPLv2+"
# (the tree is GPL-3.0-or-later).
# ⚠️ **No headroom is granted: 232 is the measurement, and the next addition pays by deleting.**
# ⚠️ 2026-09-01, a deliberate raise: SYSTEM_ANALYSIS.md 1599 -> 1610, and NOT paid for by a deletion.
# The addition is a measured capability rather than another paragraph about one: a peripheral whose DT
# node says `status = "disabled"` has no driver bound, so nothing contends for it and userspace can
# drive it outright — on UART3 that is one clock-gate bit, with no DTB patch and no reboot. That
# matters out of proportion to its length because "the node is disabled" had been read as
# "out of scope" for every peripheral, not just this one. The same commit
# CORRECTED rather than appended to the claim it falsifies ("`disabled` is the whole blocker", now
# scoped to the tty), and it paid elsewhere: the boot-path decision in the 802.15.4 entry, which the
# measurement demotes from gating to deferred, shrank 43 -> 34 non-blank. Net across the repo is +1 line.
# ⚠️ 2026-09-02, a second deliberate raise: SYSTEM_ANALYSIS.md 1610 -> 1616, also not paid for by a
# deletion. Two consecutive raises is a smell and this one is argued on scope rather than on content: the
# fleet is NOT on one firmware release, and the release decides the kernel. Every subsystem measurement in
# that document was taken on one release, so the addition does not describe a subsystem — it bounds the
# whole file, the way §1's [inferred]/[unverified]/[n=1] tags do. It was found the expensive way, by a
# commissioning run refusing p1 on a unit nobody had identified. The same commit CORRECTED rather than
# appended to the claim it falsifies (§5.1's bare `Linux 4.14.52`, now qualified by release) and qualified
# HARDWARE.md's `OS build` row in place at no line cost, so nothing else in the repo grew.
# ⚠️ 2026-09-03: both raises above are now over-paid. Closing the 802.15.4 work
# by operator decision deleted its whole plan entry, IMPROVEMENT_PLAN.md 1108 -> 1026 non-blank, so they came
# back from the file that had borrowed against them. No ceiling was LOWERED: a ceiling is a budget, not a
# measurement, and lowering one here would make the next legitimate addition pay twice.
# ⚠️ 2026-09-06, a third deliberate raise: SYSTEM_ANALYSIS.md 1616 -> 1640, and this one buys a whole new
# SECTION rather than a measurement. §8 Hardware policy states, for the first time anywhere in the repo,
# that the board is used as delivered and nothing is soldered. Before this the rule existed only as the
# operator's rulings on two separate items (the MADC analogue inputs, XBee wireless) and a reader had no
# way to find it — one of those two entries argued from "needs a reachable pad", which reads as expensive
# rather than out of scope. It is a scope policy, so §7 Kernel policy is the shape it copies and the
# neighbour it must be read against. It CORRECTED rather than appended twice: §3.11's closing sentence
# (which proposed the ruled-out half and pointed at a plan ID) and §3.4's stop-click bullet, whose scope
# limit was folded into the existing text at a cost of 4 lines rather than a new paragraph. IMPROVEMENT_PLAN.md
# paid for part of it -- one entry halved and another closed outright -- but into its own slack, not into this ceiling.
# ⚠️ 2026-09-07, raise, deliberate and argued: tests/CLAUDE.md 232 → 241 (measured post-edit
# count). `release.sh` gained its first test suite, and the one thing that file exists to record
# about a suite is what it structurally CANNOT see. Here that is unusually load-bearing in both
# directions: the acceptance half is asserted against a stub component because the script has no
# way to skip its four-component ARM build, AND the suite's `rm` is a PATH tripwire, which makes
# four of its own cases witnesses to the tripwire rather than to the guard. A reader who does not
# know the second would read a green sweep as covering more than it does. Nothing in the file went
# stale to fund it, and the holes list is the shortest section that could carry it.
# ⚠️ 2026-09-08, two raises in one commit, deliberate and argued: README.md 190 -> 202 and
# tests/CLAUDE.md 241 -> 249, both to the measured post-edit count. A single home for the host build
# prerequisites landed as setup-build-env.sh, and both files have a standing job that a new root-level
# script obliges them to do: README.md is the annotated walkthrough of EVERY script, and tests/CLAUDE.md
# records what each suite structurally cannot see. The tests/ half is load-bearing rather than
# descriptive -- that suite drives fixture tables instead of the real package set, because a green run
# against the real one proves only that this host is provisioned, and its sudo tripwire is on every case
# rather than only the two that assert on it. Neither number was compressed to fit: the alternative was
# to cut a measured claim, which is the failure this group exists to prevent. IMPROVEMENT_PLAN.md paid
# the repo back well over both raises in the same commit -- 696 -> 654 non-blank, the whole prerequisites
# entry deleted once its facts had homes in setup-build-env.sh -- but into its own slack, not into these
# ceilings. No ceiling was lowered, for the reason the 2026-09-03 note gives.
# ⚠️ 2026-09-08, three more raises in one commit, argued and operator-approved in advance:
# tests/CLAUDE.md 249 -> 288, README.md 202 -> 215, CLAUDE.md 337 -> 341, each to the measured
# post-edit count. The repo gained its first aggregate test runner, tests/run-all.sh, and the three
# files have standing jobs a new root-level entry point obliges them to do: tests/CLAUDE.md documents
# how to write and run what lives in tests/ AND what each suite structurally cannot see; README.md is
# the annotated walkthrough of EVERY script; root CLAUDE.md carried the claim "no CI, no test runner,
# no lint", of which only "no CI" survived, and a correction that did not name the gate and its
# --skip-tests override would leave the next session bypassing it unknowingly. The tests/ share is the
# large one and it is load-bearing rather than descriptive: four of its lines record traps measured
# while building the gate, two of them cases where a negative control passed VACUOUSLY because the
# fixture was cleaner than the thing it stood for. Nothing was compressed to fit — the alternative was
# to drop a measured claim, which is the failure this group exists to prevent. SYSTEM_ANALYSIS.md paid
# nothing and needed to: its copy of the same stale claim became a pointer at exactly equal length.
# ⚠️ 2026-09-08, one raise: SYSTEM_ANALYSIS.md 1640 -> 1657, to the measured post-edit count. The DSS
# overlay scaler went from [inferred] to measured on a device -- vid1 upscaling 400x240 to 800x480
# full-screen over the running app -- and the block that recorded it as untried was replaced by the
# working recipe. Three of the added lines are corrections of claims that were WRONG, not additions:
# the file asserted that input and output sizes are freely independent, when gfx (overlay0), the plane
# every app uses, has no scaler at all and rejects any width != out_width; input_size is read-only and
# tracks the framebuffer's mode rather than being written; and downscale is capped at 4x per axis. The
# rest is the reason the recipe was not obvious -- omapfb allocates vram for fb0 only, so fb1 starts
# size=0 and an overlay with paddr 0 is refused, which is why the plane looked unusable for months --
# plus the trap that no framebuffer screenshot can see an overlay, so this is the one screen check in
# the repo that a human has to make. Nothing was compressed to fit; the alternative was to drop a
# measured recipe and leave the next session re-deriving it. IMPROVEMENT_PLAN.md paid for its own F2
# rewrite out of its large standing slack, not into this ceiling.
#
# 2026-09-08: tests/CLAUDE.md 288 -> 314. The file was AT its ceiling with zero slack, and the phase-2
# work added three blocks that a gate cannot catch for itself. (1) "A skip is not a pass" was true of
# phase 1 only -- phase 2 graded exit 0 as a pass unconditionally, so a C regression that could not reach
# its subject reported as having tested it. (2) A phase-2 negative control needs a whole miniature
# native_apps tree, and three separate things make that fixture lie: the build tree has to follow the
# subject override, bash printf destroys a C string literal, and one row's -Dmain= renames a stub's entry
# point. All three were measured, and the control that caught them is the one asserting the unmodified
# fixture passes. (3) The instruction "run the gate as root to reach it" is WRONG on this host, measured
# the same day, and shipping it would have sent the next session down a path that grades nothing. Nothing
# was compressed to fit: the only way to pay 288 exactly was to delete other measured traps from the one
# file that is their home, which trades one blind spot for another. A fourth block was then needed for the
# skip CONVENTION itself: the detector is anchored on `skip` as a line's first token, because matching the
# word anywhere read three PASSING tests as skipped, and phase 1's verdict-line guard cannot be borrowed —
# these tests print four different verdict shapes and none is the one it greps for. 314 grants one line, not
# more. native_apps/CLAUDE.md absorbed its own addition inside its existing slack, and
# IMPROVEMENT_PLAN.md fell further, since the entry this work closed was deleted outright.
#
# ⚠️ 2026-09-10: SYSTEM_ANALYSIS.md 1657 -> 1681. USB audio moved from "one module away" to a
# shipping, hardware-proven subsystem, and §3.4 had no account of a second sound card at all: which
# four modules load it, what card 1 actually advertises, what the OSS shim reports back instead, and
# what the PIO costs. The file was audited for a deletion first and had none left — the addition is
# net of two paid for in place: §3.6's stale "a wired USB DAC is one module away" paragraph collapsed
# to a pointer, and the /dev/dsp1 minor was NOT copied here, because
# device-files/usb-audio-modules' own header already owns that number and the reason it is
# deterministic. The plan lost far more than 24 lines to the same change. 1681 is the measured
# post-edit count and grants NO headroom: the next addition to this file pays by deleting.
#
# ⚠️ 2026-09-11: SYSTEM_ANALYSIS.md 1681 -> 1744. §3.2 had no account of the DSS scaler's FILTER, and
# the eye A/B that ranked software nearest-neighbour above the hardware plane could not be explained
# without one -- so the next session would have re-derived it from the driver, which is what the last
# one spent an afternoon doing. Five facts, all measured in usb_host/linux-4.14.52 and none of them
# reachable from sysfs: a scaled overlay is always filtered with no filter-off bit, the coefficient
# table is picked by ratio alone, above 2x the driver deliberately substitutes a SOFTER kernel and says
# so in a comment, exact 2x reaches an identity phase but rings on its odd phases, and the tables are
# static const with no sysfs or ioctl path to them. Two more are what reclassified the plan entry as
# kernel-gated: the DSS is built IN rather than modular, so a coefficient patch is a whole image; and
# three overlays enumerate against only two framebuffers (NUM_FBS=2), so vid2 can never be funded from
# userspace at all. The same edit CORRECTED four standing claims in place rather than adding beside
# them: the upscale limit is 8x per axis and was recorded as "no upscale limit enforced at all";
# screen_width is a source row stride and was an unexplained blur suspect; input_size's read-only-ness
# had no citation; and the boot-time omap_vout DMA error was "uninvestigated" where it is now measured
# not to prevent sysfs-driven vid1 use at all. The file had no deletion left to make -- it was audited
# at the 1681 raise the day before -- and the coefficient VALUES are deliberately not copied into
# IMPROVEMENT_PLAN.md, which cross-references this section instead. The plan absorbed its own share
# inside its standing slack and FELL on net, because the withdrawn upscale switch took three bullets of
# design work with it. 1744 is the measured post-edit count and grants NO headroom.
# 2026-09-21 (same session, second raise): SYSTEM_ANALYSIS.md 1778 -> 1788, and the reason is a defect the
# first raise did not fix. The kernel-image question has been re-opened and re-settled repeatedly across
# sessions -- feasible, then an emergency, then feasible again -- because section 7 recorded only HAZARDS.
# Each one is individually true and individually warned about, so the next reader derives panic from the
# list and re-litigates whether an image is possible at all. The operator named this pattern directly. A
# standing RULING now opens the section: what is measured, what the first boot is expected to look like,
# and the single unknown that is actually open. That is a fact with no other home -- the plan is a queue
# and cannot hold a verdict. The same commit also lands a drop-table row whose edit had FAILED its anchor
# in the first raise: the board-file measurement (27 pinmux nodes, 11 _pins blocks, 13 pinctrl consumers,
# the audio binding) existed only as a pointer FROM IMPROVEMENT_PLAN.md TO a section 7 that did not
# contain it -- a broken pointer of exactly the kind the doc-update rules warn about, caught by the
# operator asking whether the thing had been written up.
# 2026-09-21 (same session, third raise): SYSTEM_ANALYSIS.md 1788 -> 1800 and HARDWARE.md 216 -> 224, both
# argued, and the cheaper homes were exhausted FIRST. The touch controller's register map moved from
# `[unverified]` hypothesis to a settled byte-level measurement against a live finger, which is the input
# that makes the kernel touch driver writable at all -- so it is a fact with exactly one home, section 3.3.
# Two of its three halves cost ZERO lines because they went into existing ONE-LINE table rows: the I2C
# bus-2 row absorbed "only responder on the bus, and a bare read returns register 0", and the section 7
# drop-table row absorbed both the closed 110->120 delta and the i2c-dev mechanism (`I2C_RDWR` carries the
# address per-message and never consults the busy list that makes `I2C_SLAVE` return -EBUSY), which is why
# the reader needs no unbind and no p1 write. Only the map itself, the 12-bit range that refutes the
# in-tree driver's hardcoded 759x465, the `0x0fff` flag mask and the quiescent pattern went into prose.
# A duplication sweep over nine candidate tokens found the new facts stated nowhere, and the one real
# overlap -- `Num_Touch` in both 3.3 and section 7 -- was reduced to one home while landing this.
# HARDWARE.md pays for a measured NEW CAPABILITY plus the trap that cost this session real time: the P4
# header is RS-232 and the operator's four cables are all USB-TTL, so the console looked blocked on a
# purchase. It is not -- `U27` pin 11 (`T1IN`) is the SoC's TX at logic level before the shifter, so a TTL
# adapter reads the boot log on ONE sense wire with ground taken from P4 pin 5 rather than fine pitch, and
# that log is the only diagnostic a FAILED boot leaves, there being no SSH to ask. Routing any of this to
# COMMISSIONING.md -- which carries no ceiling -- would have been free and would have been exactly the
# "well-written addition to the wrong file" the doc-update rules exist to prevent: a header pinout is
# HARDWARE.md's by classification. Neither number grants headroom; both are the measured post-edit count.
# 2026-09-24: SYSTEM_ANALYSIS.md 1800 -> 1801. The touch controller's per-electrode profile map (27 column
# and 15 row delta bytes, their fitted pitch, the 60 ms / 30 ms-out-of-phase update, and a stale in-range
# Y2) is what makes two-finger tracking possible at all, and it has one home, section 3.3. It replaced the
# bounding-box paragraph's `[inferred]` "registers 19..31" guess and paid for itself down to one line: the
# paragraph was rewritten in place (6 -> 8 lines) and the protocol paragraph lost a clause the bounding-box
# finding superseded. The measurement-free alternative was dropping the fit numbers, which are the claim.
# 2026-09-27: SYSTEM_ANALYSIS.md 1801 -> 1806. The device userspace is soft-float ABI (loader
# /lib/ld-linux.so.3), which is why a gnueabihf dynamic binary cannot run and what the whole dynamic-
# libasound route rests on; it has one home, section 6.3, with the measured toolchain recipe and the
# rootfs library inventory BlueZ will link against. The stale "native ALSA NOT PLANNED" verdict in 3.4 was
# rewritten in place with the measurement (9 -> 7 lines), paying 2 of the 7. Operator was AFK; flagged.
## 2026-09-21: SYSTEM_ANALYSIS.md 1744 -> 1778, authorised by the operator in the session that spent it.
# The file was AT its ceiling with zero slack and the kernel-image question moved from untried to measured,
# which is four separate facts a gate cannot see for itself. (1) A full image LINKS and packages, and the
# mkimage recipe is proven by a byte-identical round trip against the vendor image -- the recipe has to be
# written down or it is re-derived, and `-C none` is the part that is counter-intuitive enough to be got
# wrong twice. (2) The drop table was WRONG, not merely thin: it rated the dropped Sharp panel driver
# "cost: none -- reduces to a stock panel-dpi node", and the vendor DTB names a compatible string no
# vanilla driver claims, so the honest cost is a blank panel. A stale claim is corrected with a
# measurement, not deleted. (3) Two more dropped symbols had no row at all, one of them the only drop that
# can stop the image booting. (4) The module-vs-image line existed ONLY in IMPROVEMENT_PLAN.md, which is a
# queue that gets deleted -- a real one-fact-one-home gap, and the rule (decide from the device's own
# config; a `=y` driver is still evictable if its bus exposes unbind) is what the next session needs.
# Nothing was compressed to pay for it: the alternative was deleting measured traps from the file that is
# their home. IMPROVEMENT_PLAN.md and HARDWARE.md absorbed their share of this work into standing slack.
#
## 2026-10-01: native_apps/CLAUDE.md 716 -> 740, raised in the commit that closed the control-panel plan entry.
## 2026-10-01: SYSTEM_ANALYSIS.md 1806 -> 1826 and native_apps/CLAUDE.md 740 -> 759, raised in the commit that
# emptied the closed half of the DSS-overlay plan entry. SYSTEM_ANALYSIS.md gains the measured userspace-upscale
# result (CPU per frame, the eye A/B and its ruling, the instruments), which existed only in the plan; native_apps
# gains the reduced-surface conversion rules (surface:panel ratio, bezel scaling, touch needing no divide, the
# unconverted literals). The narrative, instrument-repair history and operator transcripts were deleted, not moved;
# the plan entry fell by over 300 lines but a ceiling is a budget, so it was not lowered.
## 2026-10-05: kernel/README.md 139 -> 145, raised in the commit that gave the closed Bluetooth hardening (BT_HS off and
# the three module backports, with the operator's pad-reconnect result) its home in the Bluetooth modules section; the
# plan entries for it were deleted, so the facts exist only there. The plan ceiling was not lowered, for the reason above.
## 2026-10-04: kernel/README.md 124 -> 139, raised in the commit that gave the software power-off test (patch-table row
# plus the staged-image procedure) and the Spectre finding their homes there, replaced the stale p1 rollback-image list
# and reflowed the vendor-kernel paragraph (-2). Receipt row `17243454` (the `.mod` rollback image) was deleted with
# the image list: the file it described does not exist on the unit. The plan keeps only what stays open.
## 2026-10-02: kernel/README.md 120 -> 124, raised in the commit that gave the two musb patches (bounded TX flush for a
# gone device; set_vbus timeout report) their patch-table rows with mechanism and measurement, and rewrote the
# manual image-install paragraph. The plan entries kept only what remains open; the plan ceiling was not lowered.
## 2026-10-01: kernel/README.md 100 -> 120, raised in the commit that moved the closed Bluetooth-module and
# image-boot facts out of the F17 and F101 plan entries: the 18-module set, the RTL8761CU dongle identity and
# firmware result, and the p1 rollback-image list now live there. The plan entries lost their history narrative
# and the done steps; the plan ceiling was not lowered, for the reason above.
#
# The entry carried the durable design rules of `control_panel` (the CpPage registry, save-at-once and the
# reset flow, the exit and navigation split, enter-marks-pending, the rejected one-binary-per-page shape) and
# the directory guide had no section for any of it. They moved here as one section; IMPROVEMENT_PLAN.md paid
# with the rest of that deleted entry, into its own slack rather than this ceiling.
#
# 2026-10-01: tests/CLAUDE.md 314 -> 324. The file sat at 313 and the shellcheck-directive traps (prose that voids
# a file, `source-path=SCRIPTDIR` placement, gate-shape measurement) moved in from a plan entry as nine lines;
# the plan paid with the rest of that entry and its ceiling was not lowered, for the reason above.
#
# 2026-10-01: SYSTEM_ANALYSIS.md 1826 -> 1834. The file sat at its ceiling; the avail/avail_update behaviour after a
# USB audio card is removed moved in from a closed plan entry as eight lines. The plan paid with that entry and its
# ceiling was not lowered, for the reason above.
# 2026-10-01: SYSTEM_ANALYSIS.md 1834 -> 1850. The 8BitDo Pro 2 radio identities and the DualShock-4 emulation measured
# on .188 are device facts and moved in from a plan entry as sixteen lines; the plan shed its superseded S/D-mode
# speculation, and its ceiling was not lowered, for the reason above.
# 2026-10-01: SYSTEM_ANALYSIS.md 1852 -> 1854 and native_apps/CLAUDE.md 759 -> 766. Measured Bluetooth audio facts (bluealsa
# open/buffer behaviour, bondable flag, HFP-AG reconnect) and the audio_out BT routing rules are new content with no stale
# text to cut; the plan shed its done steps, and its ceiling was not lowered, for the reason above.
# 2026-10-02: native_apps/CLAUDE.md 766 -> 771. The canonical pad-code rule (xpad codes, per-pad layout, translate at read time) is new
# content with no stale text to cut; its device-side facts went into the existing 8BitDo paragraph of SYSTEM_ANALYSIS.md.
# 2026-10-02: native_apps/CLAUDE.md 771 -> 775. The audio_bt_addr pin (PCM string, absent-pin fallback, why not plug:...DEV=) is new
# content with no stale text to cut; the plan shed the follow-up that asked for it.
# 2026-10-05: native_apps/CLAUDE.md 775 -> 780 and kernel/README.md 145 -> 160. The rule that every level-tracking evdev reader
# handles SYN_DROPPED is new content with no stale text to cut; kernel/README.md gained the measured ACTLR value (replacing the
# "unmeasured" sentence), the module-set rebuild rule after an image patch change (MODVERSIONS), and a refreshed p1 inventory.
# The plan shed five closed entries and its ceiling was not lowered, for the reason above.
# 2026-10-05: kernel/README.md 160 -> 180. The Mainline and omapfb section is new content (the measured upstream omapfb finding,
# the unproven list, gains and costs) that replaced a false claim held in the system analysis; no stale text in this file to cut.
# 2026-10-09: rootfs/CLAUDE.md is a NEW entry at 60, with its measured size 46 non-blank lines plus room for the card-image
# rules still to land; it holds the facts moved in from the closed root-filesystem plan entry (group C rows).
# 2026-10-14: commissioning/CLAUDE.md (ceiling 120) and COMMISSIONING.md were dissolved. rootfs/CLAUDE.md 60 -> 76 takes the image installer's
# rules and the host-name writer's rules (they live in rootfs/ now); SYSTEM_ANALYSIS.md 1854 -> 1859 takes the LED reading of a failed boot,
# which existed only in COMMISSIONING.md. lib/CLAUDE.md took the provision.sh rules into its own slack.
# 2026-10-15 (operator-approved): README.md became the USER guide and the old walkthrough moved, with history, to DEVELOPING.md (git mv).
# DEVELOPING.md gets a NEW entry at its measured 206 non-blank, its SSH steps replaced by a pointer to README.md (the old README.md ceiling was 215 against 208); the new README.md is 162
# non-blank and gets zero margin, same as the old one: a user guide that grows without a decision is the shape this group bounds.
#
#
ceilings() {
    # `CEILINGS_FILE` exists only so --self-test can drive this group over a fixture table.
    if [ -n "${CEILINGS_FILE:-}" ]; then cat "$CEILINGS_FILE"; return; fi
    cat <<'EOF'
1859	SYSTEM_ANALYSIS.md
1362	IMPROVEMENT_PLAN.md
224	HARDWARE.md
162	README.md
206	DEVELOPING.md
342	CLAUDE.md
780	native_apps/CLAUDE.md
232	lib/CLAUDE.md
106	device-files/CLAUDE.md
324	tests/CLAUDE.md
215	scummvm-arca/CLAUDE.md
162	vnc_client/CLAUDE.md
137	.claude/skills/doc-update/SKILL.md
180	kernel/README.md
40	kernel/CLAUDE.md
76	rootfs/CLAUDE.md
EOF
}

# Non-blank line count. A line of nothing but whitespace is not content, so it is neither
# chargeable nor spendable — see the basis note on group D above.
nonblank_lines() {
    grep -cve '^[[:space:]]*$' "$1" || true
}

group_d() {
    local root="$1" quiet="${2:-}"
    local bad=0 total=0 cap path n margin tight=999999 tightest=

    while IFS=$'\t' read -r cap path; do
        [ -z "$cap" ] && continue
        total=$((total + 1))
        if [ ! -f "$root/$path" ]; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  MISSING FILE  %s (ceiling %s) — renamed or split? the ceiling must follow it\n' \
                "$path" "$cap"
            continue
        fi
        n="$(nonblank_lines "$root/$path")"
        n=$((n))
        if [ "$n" -gt "$cap" ]; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  OVER CEILING  %s  %d non-blank lines, ceiling %d (+%d — delete as much, or raise it deliberately)\n' \
                "$path" "$n" "$cap" "$((n - cap))"
            continue
        fi
        margin=$((cap - n))
        if [ "$margin" -lt "$tight" ]; then tight="$margin"; tightest="$path"; fi
    done < <(ceilings)

    if [ -z "$quiet" ]; then
        printf 'group D: %d ceilings, %d over' "$total" "$bad"
        [ -n "$tightest" ] && printf ' (tightest margin: %s, %d lines left)' "$tightest" "$tight"
        printf '\n'
    fi
    echo "$bad" > "$RESULT"
    [ "$bad" -eq 0 ]
}

# ── group E: structural markdown health ───────────────────────────────────────
# Two defects that render as *plausible* markdown, so neither a reader skimming the diff
# nor GitHub's own view flags them — and both are produced by ordinary editing.
#
#   E1  A file with no trailing newline. `Edit`, `cat`, `head -N` splices and heredoc
#       rewrites all drop it, and the next append then lands on the last line rather than
#       after it — silently joining two paragraphs, or turning a heading into body text.
#       ⚠️ Found one live on the first run (2026-08-19), which is this check's positive
#       control on the real tree: a rule that has only ever been seen passing is not
#       evidence it can fail.
#
#   E2  A column-0 paragraph on the line directly after an INDENTED bullet. CommonMark's
#       lazy-continuation rule folds it into that bullet, so the paragraph disappears from
#       where the author put it and reappears inside a list item. This is the shape group
#       D's old all-lines basis actively rewarded: deleting the blank line above a
#       paragraph paid off a 1-line overage and broke the document, and the gate said PASS.
#       The basis change removes the incentive; this check removes the defect.
#
# ⚠️ Blind spots, all under-counting, all deliberate:
#   - a column-0 line beginning `*`, `+`, `-` or a number is read as a list marker, so a
#     paragraph opening with emphasis (`*Note:* …`) is not seen;
#   - a column-0 `|` table row after an indented bullet is lazily swallowed the same way
#     but is not flagged, because a table is not the reported defect;
#   - a bullet at column 0 followed by a column-0 paragraph is also lazy continuation, and
#     is out of scope — the reported defect is the INDENTED-bullet case, which is the one
#     that reads as correct.
# Fenced code blocks are skipped in both checks, for the same reason group A skips them:
# a `# comment` or an indented line inside a fence is not markdown structure.

# Every scanned markdown file, relative to $1.
md_files() {
    scan_files "$1" | grep '\.md$'
}

group_e() {
    local root="$1" quiet="${2:-}"
    local bad=0 total=0 f
    local -a files=()
    mapfile -t files < <(md_files "$root")
    [ ${#files[@]} -eq 0 ] && { echo 0 > "$RESULT"; return 0; }

    for f in "${files[@]}"; do
        total=$((total + 1))
        [ -s "$root/$f" ] || continue
        if [ "$(tail -c1 "$root/$f" | wc -l)" -eq 0 ]; then
            bad=$((bad + 1))
            [ -n "$quiet" ] || printf '  NO EOL AT EOF %s — the next append will join onto its last line\n' "$f"
        fi
    done

    local hits
    hits="$( cd "$root" && awk '
    FNR == 1 { fence = 0; prev_bullet = 0 }
    /^[ \t]*```/ { fence = !fence; prev_bullet = 0; next }
    fence { next }
    {
        if (prev_bullet && $0 ~ /^[^ \t#>|*+-]/ && $0 !~ /^[0-9]+[.)]/ && $0 !~ /^</)
            printf "  LAZY BULLET   %s:%d  column-0 paragraph after an indented bullet — blank line needed: %.48s\n", FILENAME, FNR, $0
        if ($0 ~ /^[ \t]+([*+-]|[0-9]+[.)])[ \t]/) prev_bullet = 1
        else if (prev_bullet && $0 ~ /^[ \t]+[^ \t]/) prev_bullet = 1
        else prev_bullet = 0
    }' "${files[@]}" )"
    if [ -n "$hits" ]; then
        bad=$((bad + $(printf '%s\n' "$hits" | wc -l)))
        [ -n "$quiet" ] || printf '%s\n' "$hits"
    fi

    [ -n "$quiet" ] || printf 'group E: %d markdown files, %d structural defects\n' "$total" "$bad"
    echo "$bad" > "$RESULT"
    [ "$bad" -eq 0 ]
}

# ── negative controls ─────────────────────────────────────────────────────────
# A gate that reports a number can be wrong in both directions, so both directions
# are controlled: a fabricated dangling citation MUST be caught, and a citation of a
# real ID in the same file MUST NOT be. The fixture is a throwaway tree, not this
# repo, so the control cannot be confused with a real finding.
self_test() {
    local t rc=0 seen
    t="$(mktemp -d)"
    mkdir -p "$t/sub"
    cat > "$t/$PLAN_FILE" <<'EOF'
# fixture
### C99. A defined item — open
### F1. Another defined item — open
EOF
    # One dangling, one defined, one anchor (group A's, must be ignored), and a
    # continuation list mixing both kinds.
    # ⚠️ Interpolated, NOT a quoted heredoc: a literal citation here would be found by
    # the scanner in this script's own source and counted as a real finding. That is the
    # harness inflating its own number, and it did — three phantom hits on first run.
    local p="$PLAN_FILE"
    cat > "$t/sub/probe.c" <<EOF
/* $p B99 — fabricated, must be caught. */
/* $p C99 — defined, must be caught TOO: the rule is now "no citation at all". */
/* $p#c99-a-defined-item--open — an anchor, not a citation. */
/* (../$p F1, D98). */
/* a bare dangling tag (B97), and a bare defined one (C99). */
/* prose form: see D96.  And a register name, B0, which must NOT be caught. */
EOF
    seen="$(group_b "$t" quiet; cat "$RESULT")"
    if [ "$seen" = "7" ]; then
        echo "self-test: PASS — group B caught 7 citations (B99, C99, F1, D98 qualified; B97, C99, D96 bare)," \
             "including the ones that RESOLVE; the anchor and the bare register name B0 ignored"
    else
        echo "self-test: FAIL — expected 7 citations, scanner reported $seen"
        rc=1
    fi

    # Group A, controlled in both directions: a heading that exists must not fire, a
    # fragment that names no heading must, a link to a file that does not exist must, and
    # — the case that would otherwise be a false PASS — a `#` line inside a fenced code
    # block must NOT count as a heading.
    cat > "$t/DOC.md" <<'EOF'
# Fixture Doc
## 2.4 Unpopulated and expansion
Resolving same-file link: [a](#24-unpopulated-and-expansion)
Dangling same-file link: [b](#no-such-section)
```bash
# 9.9 Fenced, not a heading
```
EOF
    cat > "$t/sub/anchors.md" <<'EOF'
[ok](../DOC.md#24-unpopulated-and-expansion)
[fenced](../DOC.md#99-fenced-not-a-heading)
[gone](../NOPE.md#anything)
Background: dark blue (#0a0e27) — a colour, not an anchor.
EOF
    seen="$(group_a "$t" quiet; cat "$RESULT")"
    if [ "$seen" = "3" ]; then
        echo "self-test: PASS — group A caught 3 (#no-such-section, the fenced heading, the missing file);" \
             "both good anchors, the hex colour and the root-fallback path from sub/ ignored"
    else
        echo "self-test: FAIL — group A expected 3 dangling, reported $seen"
        rc=1
    fi

    # Group C, controlled in BOTH directions: a satisfied receipt must not fire, an
    # unextracted one must, and a token still present at the source must — the last is
    # the copy-instead-of-move case, which a "destination has it" check alone passes.
    printf 'kept-token\n' > "$t/DEST.md"
    printf 'both-token\n' >> "$t/DEST.md"
    printf 'both-token\n' > "$t/SOURCE.md"
    local rf="$t/receipts.tsv"
    printf 'kept-token\tDEST.md\tSOURCE.md\n'   > "$rf"   # satisfied: at dest, not at source
    printf 'absent-token\tDEST.md\t-\n'        >> "$rf"   # must fire: never extracted
    printf 'both-token\tDEST.md\tSOURCE.md\n'  >> "$rf"   # must fire: copied, not moved
    seen="$(RECEIPTS_FILE="$rf" group_c "$t" quiet; cat "$RESULT")"
    if [ "$seen" = "2" ]; then
        echo "self-test: PASS — group C caught 2 (absent-token, both-token); the satisfied receipt ignored"
    else
        echo "self-test: FAIL — group C expected 2 unsatisfied, reported $seen"
        rc=1
    fi

    # Group D, controlled in FOUR directions: over ceiling must fire, under must not, a
    # ceiling naming a file that is not there must fire, and — the control on the counting
    # basis — a file that is over on TOTAL lines but under on NON-BLANK lines must NOT
    # fire. That last one is what makes deleting whitespace worthless as payment; without
    # it, the basis could silently revert to `wc -l` and every group-D result would still
    # look right. The missing-file case is the one a "count the lines and compare" check
    # passes by accident, and it is the case that matters after a split — the guard must
    # follow the document, not the old filename.
    printf 'a\nb\nc\nd\ne\n' > "$t/BIG.md"       # 5 non-blank against a ceiling of 3 — must fire
    printf 'a\nb\n'          > "$t/SMALL.md"     # 2 non-blank against a ceiling of 9 — must not
    printf 'a\n\n\nb\n\n   \nc\n\n\n' > "$t/BLANKS.md"  # 9 total, 3 non-blank, ceiling 5 — must not
    local cf="$t/ceilings.tsv"
    printf '3\tBIG.md\n'     > "$cf"
    printf '9\tSMALL.md\n'  >> "$cf"
    printf '5\tBLANKS.md\n' >> "$cf"
    printf '9\tGONE.md\n'   >> "$cf"
    seen="$(CEILINGS_FILE="$cf" group_d "$t" quiet; cat "$RESULT")"
    if [ "$seen" = "2" ]; then
        echo "self-test: PASS — group D caught 2 (BIG.md over, GONE.md missing); SMALL.md under ceiling" \
             "and BLANKS.md (9 total lines, 3 non-blank, ceiling 5) ignored"
    else
        echo "self-test: FAIL — group D expected 2, reported $seen"
        rc=1
    fi

    # Group E, controlled in both directions on BOTH of its checks: a file that ends in a
    # newline and separates its column-0 paragraphs with a blank line must not fire; a file
    # missing its final newline must, and a column-0 paragraph directly after an indented
    # bullet must. The two blind spots are asserted too — a column-0 BULLET after an
    # indented one is legal list continuation, and a fenced block's indented lines are not
    # structure — because both are shapes a naive version of this check fires on.
    mkdir -p "$t/e"
    cat > "$t/e/GOOD.md" <<'EOF'
# Fixture

- top bullet
  - indented bullet, wrapped
    over two lines

A column-0 paragraph, correctly separated by a blank line.

- bullet
- another column-0 bullet, which continues the list

```text
  - an indented bullet inside a fence
not a paragraph, because this is code
```

Done.
EOF
    printf '# No trailing newline\n\nBody.' > "$t/e/NOEOL.md"
    cat > "$t/e/LAZY.md" <<'EOF'
# Fixture

- top bullet
  - indented bullet
Swallowed into that bullet by lazy continuation.

Fine again.
EOF
    seen="$(group_e "$t" quiet; cat "$RESULT")"
    if [ "$seen" = "2" ]; then
        echo "self-test: PASS — group E caught 2 (NOEOL.md missing final newline, LAZY.md:5 lazy" \
             "continuation); GOOD.md's blank-separated paragraph, column-0 bullet and fenced block ignored"
    else
        echo "self-test: FAIL — group E expected 2 defects, reported $seen"
        rc=1
    fi

    rm -rf "$t"
    return $rc
}

# ── main ──────────────────────────────────────────────────────────────────────
RESULT="$(mktemp)"
trap 'rm -f "$RESULT"' EXIT

case "${1:-}" in
    --self-test)
        self_test || fail_total=1
        ;;
    --help|-h)
        sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        self_test || fail_total=$((fail_total + 1))
        echo
        echo "Markdown anchors that resolve to no heading:"
        group_a "$REPO" || fail_total=$((fail_total + 1))
        echo
        echo "Plan-ID citations outside $PLAN_FILE (the plan is a queue; none may exist):"
        group_b "$REPO" || fail_total=$((fail_total + 1))
        echo
        echo "Extraction receipts — each fact must be at its destination and gone from the source:"
        group_c "$REPO" || fail_total=$((fail_total + 1))
        echo
        echo "Document size ceilings — growth is paid for or decided on, never absorbed:"
        group_d "$REPO" || fail_total=$((fail_total + 1))
        echo
        echo "Structural markdown health — a trailing newline, and no lazily-swallowed paragraph:"
        group_e "$REPO" || fail_total=$((fail_total + 1))
        ;;
esac

echo
if [ "$fail_total" -eq 0 ]; then
    echo "doc_check: PASS"
else
    echo "doc_check: FAIL ($fail_total group(s))"
fi
exit "$fail_total"
