#!/bin/bash
# Gate: the sound sets, the games and the committed beds all agree.
#
# Run from build-and-deploy.sh, beside check-arm-safe.sh, check-audio-pacing.sh
# and check-sound-assets.sh.  Standalone:
#     ./check-bed-files.sh
#
# ── WHY THIS EXISTS ─────────────────────────────────────────────────────────
# A missing bed is refused in SILENCE.  audio_bed_service() marks the track
# failed, prints one line into a log nobody reads at play time, and the game
# plays its effects with no music: exactly what a game with no bed at all sounds
# like.  Nothing else in the tree can see the difference.
#
# The paths live in ONE home — sound-sets.sh's ARCA_SOUND_SETS —
# and the C side derives nothing, so this gate no longer reads a convention out
# of a call site.  It reads the table, and checks FIVE things:
#
#   1. every path the table names exists in music/            FAIL
#   2. every audio_bed_init() tag has a table row             FAIL  (silent no-music)
#   3. a row's track count fits AUDIO_BED_MAX_TRACKS          FAIL  (silently ignored)
#   4. CONFIG_SOUND_SET_DIR/_EXT agree with ARCA_SOUND_SET_DIR/_EXT   FAIL
#   5. every committed bed reaches some row                   WARN
#      every table row reaches some game                      WARN
#
# ⚠️ (4) is the one with no other symptom: a mismatch installs seven set files
# that nothing ever opens, and all seven games simply have no music.  Both sides
# are READ here rather than restated, so this check cannot itself drift.
#
# ⚠️ (2) FAILS where the old gate WARNED-AND-EXITED-0.  Measured 2026-08-24: the
# 5-argument→1-argument signature change made the old regex match nothing, and it
# printed `games=0 want=0 missing=0 orphan=0` and exited 0 — it went blind and
# reported that as success, with 24 beds that should all have read as orphans.  A
# gate that finds no callers at all has lost its subject and must say so.
#
# ⚠️ WHAT THIS CANNOT SEE: whether the DEVICE has the files.  It reads the repo's
# music/ directory and the generator's table, which is what build-and-deploy.sh
# uploads; a device whose /opt/sound or /opt/arca/soundsets was hand-managed
# can still be missing one.  It also cannot see whether a bed SOUNDS right, and it
# is a TEXT match, so a commented-out audio_bed_init() counts as a game — which is
# how its own negative control is built (a throwaway .c under the tree naming a tag
# with no table row makes it exit 1, no shipped file touched).
# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
cd "$(dirname "$0")" || exit 1

# shellcheck source=sound-sets.sh
. ./sound-sets.sh

MUSIC_DIR="music"
n_games=0; n_missing=0; n_want=0; n_rows=0; n_fail=0

# ── 4. the two sides of the install path ────────────────────────────────────
# Both READ, never restated.  A quoted #define and a quoted shell assignment.
c_dir="$(sed -nE 's/^#define[[:space:]]+CONFIG_SOUND_SET_DIR[[:space:]]+"([^"]*)".*/\1/p' common/config.h)"
c_ext="$(sed -nE 's/^#define[[:space:]]+CONFIG_SOUND_SET_EXT[[:space:]]+"([^"]*)".*/\1/p' common/config.h)"
if [ -z "$c_dir" ] || [ -z "$c_ext" ]; then
    echo -e "${RED}  ✗ could not read CONFIG_SOUND_SET_DIR/_EXT from common/config.h${NC}"
    n_fail=$((n_fail + 1))
elif [ "$c_dir" != "$ARCA_SOUND_SET_DIR" ] || [ "$c_ext" != "$ARCA_SOUND_SET_EXT" ]; then
    echo -e "${RED}  ✗ install path disagrees: C says $c_dir/<tag>$c_ext, sound-sets.sh says $ARCA_SOUND_SET_DIR/<tag>$ARCA_SOUND_SET_EXT${NC}"
    n_fail=$((n_fail + 1))
fi

# The cap, read from the header for the same reason.
max_tracks="$(sed -nE 's/^#define[[:space:]]+AUDIO_BED_MAX_TRACKS[[:space:]]+([0-9]+).*/\1/p' common/audio_bed.h)"
[ -n "$max_tracks" ] || { echo -e "${RED}  ✗ could not read AUDIO_BED_MAX_TRACKS${NC}"; n_fail=$((n_fail + 1)); max_tracks=8; }

# ── the game side: one tag per audio_bed_init() call ────────────────────────
tags_c="$(grep -rhoE 'audio_bed_init\(&bed, &audio, "[^"]+"' --include='*.c' . 2>/dev/null \
          | sed -E 's/.*"([^"]+)"$/\1/' | sort -u || true)"
tags_tbl="$(arca_sound_set_tags | sort -u)"

if [ -z "$tags_c" ]; then
    echo "BED-SUMMARY games=0 want=0 missing=0 orphan=0 rows=$(printf '%s\n' "$tags_tbl" | grep -c . )"
    echo -e "${RED}  ✗ No audio_bed_init() call found — this gate has lost its subject${NC}"
    exit 1
fi

# ── 1 + 3: each row's paths exist, and the row fits the cap ─────────────────
claimed=""
while IFS='|' read -r tag name stem count; do
    [ -n "$tag" ] || continue
    n_rows=$((n_rows + 1))
    if [ "$count" -gt "$max_tracks" ]; then
        echo -e "${RED}  ✗ $tag asks for $count track(s); AUDIO_BED_MAX_TRACKS is $max_tracks — the rest are silently ignored${NC}"
        n_fail=$((n_fail + 1))
    fi
    miss=""
    for p in $(arca_sound_set_paths "$tag"); do
        b="$(basename "$p")"
        n_want=$((n_want + 1))
        if [ -f "$MUSIC_DIR/$b" ]; then claimed="$claimed $b"
        else miss="$miss $b"; n_missing=$((n_missing + 1)); fi
    done
    printf "  %-16s %d track(s) as %s<n>%s%s\n" "$tag" "$count" "$stem" "$ARCA_SOUND_BED_SUFFIX" \
        "$([ -n "$miss" ] && echo "  MISSING:$miss")"
done <<< "$(printf '%s\n' "$ARCA_SOUND_SETS")"

# ── 2 + 5: the two directions between games and rows ────────────────────────
no_row=""
for t in $tags_c; do
    n_games=$((n_games + 1))
    case " $(echo $tags_tbl) " in *" $t "*) ;; *) no_row="$no_row $t";; esac
done
no_game=""
for t in $tags_tbl; do
    case " $(echo $tags_c) " in *" $t "*) ;; *) no_game="$no_game $t";; esac
done

# ── 5: beds that reach nothing ──────────────────────────────────────────────
# ⚠️ Counted against the CLAIMED list rather than against a per-game name, so a
# file claimed by two games counts once.
n_orphan=0; orphans=""
if ls "$MUSIC_DIR"/*.wav >/dev/null 2>&1; then
    for f in "$MUSIC_DIR"/*.wav; do
        b="$(basename "$f")"
        case " $claimed " in *" $b "*) ;; *) n_orphan=$((n_orphan + 1)); orphans="$orphans $b";; esac
    done
fi

echo "BED-SUMMARY games=$n_games rows=$n_rows want=$n_want missing=$n_missing orphan=$n_orphan"
[ -n "$no_game" ] && echo -e "${YELLOW}  ! set row(s) reaching no game:$no_game${NC}"
[ "$n_orphan" -gt 0 ] && echo -e "${YELLOW}  ! $n_orphan committed bed(s) reach no game:$orphans${NC}"
if [ -n "$no_row" ]; then
    echo -e "${RED}  ✗ game(s) with no sound-set row, so no set file and no music:$no_row${NC}"
    n_fail=$((n_fail + 1))
fi
if [ "$n_missing" -gt 0 ]; then
    echo -e "${RED}  ✗ Music beds: $n_missing file(s) a set names are not in $MUSIC_DIR/${NC}"
    n_fail=$((n_fail + 1))
fi
[ "$n_fail" -gt 0 ] && exit 1
echo -e "${GREEN}  ✓ Music beds: $n_want file(s) for $n_games game(s), all present${NC}"
exit 0
