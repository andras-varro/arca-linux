#!/bin/bash
# Measure that audio_sample_test's groups can FAIL.  Copies only; the tree is
# untouched — ⚠️ never restore a sabotage with git, it would destroy whatever fix
# is still uncommitted beside it.
#
# ⚠️ Read the output, do not count it.  A stanza whose pattern has rotted prints
# "NO-OP EDIT" rather than "0 failed", and "0 failed" from a real edit is a HOLE
# in the suite, not a pass.  Case 3 is the one that found one: nothing in the
# suite reached the odd-size chunk pad until a fixture was added for it.
cd /mnt/c/work/arca/native_apps || exit 1
W=$(mktemp -d /tmp/samp.XXXXXX)
HARNESS_ERR=0
run() {
  local name="$1"; shift
  rm -rf "$W/c"; mkdir -p "$W/c/common"
  cp common/audio_wav.c common/audio_wav.h common/audio_gen.c common/audio_gen.h "$W/c/common/"
  ( cd "$W/c/common" && "$@" ) || { echo "$name: SABOTAGE DID NOT APPLY"; HARNESS_ERR=1; return; }
  if diff -q "$W/c/common/audio_wav.c" common/audio_wav.c >/dev/null && \
     diff -q "$W/c/common/audio_gen.c" common/audio_gen.c >/dev/null; then
     echo "$name: NO-OP EDIT — pattern rotted"; HARNESS_ERR=1; return; fi
  gcc -Wall -Wextra -Wno-unused-parameter -I "$W/c" -o "$W/t" \
      tests/audio_sample_test.c "$W/c/common/audio_wav.c" "$W/c/common/audio_gen.c" \
      -lm 2>"$W/cc.log" || {
      echo "$name: did not compile ($(head -1 "$W/cc.log"))"; return; }
  # ⚠️ stdbuf, because a sabotage that CRASHES loses everything libc had buffered
  # into the pipe, and then a caught sabotage reads as an undetected one.
  out=$(stdbuf -oL timeout 60 "$W/t" 2>&1); rc=$?
  n=$(printf '%s\n' "$out" | grep -c 'FAIL ')
  if [ "$rc" -ge 124 ]; then echo "$name: $n failed, then TIMED OUT/SIGNALLED (rc=$rc)"
  else echo "$name: $n failed"; fi
  printf '%s\n' "$out" | grep 'FAIL ' | sed 's/^/      /' | head -5
}

# ── the defect this whole file exists for ──────────────────────────────────
run "1 the 44-byte assumption: skip the walk, seek straight to 44" \
    sed -i 's|} else if (!memcmp(hdr, "data", 4)) {|} else if (0) {|' audio_wav.c
run "2 data found but the 8-byte chunk header is not skipped (off by 8)" \
    sed -i 's|data_pos   = ftell(f);|data_pos   = ftell(f) - 8;|' audio_wav.c
run "3 the RIFF odd-size pad byte is dropped from the skip" \
    sed -i 's|if (fseek(f, (long)sz + ((long)sz \& 1), SEEK_CUR) != 0) break;|if (fseek(f, (long)sz, SEEK_CUR) != 0) break;|' audio_wav.c

# ── the reader ─────────────────────────────────────────────────────────────
run "4 an over-claiming header is believed over the file" \
    sed -i 's|data_bytes = end - data_pos;||' audio_wav.c
run "5 the downmix takes the left channel instead of averaging" \
    sed -i 's|(((int32_t)frame\[0\] + (int32_t)frame\[1\]) >> 1)|(int32_t)frame[0]|' audio_wav.c
run "6 a drained non-looping reader wraps anyway" \
    sed -i 's|if (!w->loop) break;||' audio_wav.c
run "7 the loop counter never increments" \
    sed -i 's|w->loops++;||' audio_wav.c
run "8 rewind does not reset pos, so a wrap reads past the end" \
    sed -i 's|if (fseek(w->f, w->data_pos, SEEK_SET) == 0) w->pos = 0;|(void)fseek(w->f, w->data_pos, SEEK_SET);|' audio_wav.c
run "9 16-bit is not checked, so an 8-bit file is read as PCM" \
    sed -i 's|bits != 16|bits < 8|' audio_wav.c

# ── the voice on the bus ───────────────────────────────────────────────────
run "10 the buffer is refilled every frame, losing the unread tail" \
    sed -i 's|if (vo->buf_pos >= vo->buf_len \&\& !vo->drained) {|if (1) {|' audio_gen.c
run "11 the sample voice never advances pos, so it never ends" \
    sed -i 's|                if (++vo->pos >= vo->frames) vo->active = false;|                (void)0;|' audio_gen.c
run "12 a full bus STEALS slot 0 instead of refusing" \
    sed -i 's|if (vo->active) continue;|if (vo->active \&\& i + 1 < AUDIO_MAX_VOICES) continue;|' audio_gen.c
run "13 the refusal is not counted" \
    sed -i 's|m->dropped++;||' audio_gen.c
run "14 release CUTS the voice instead of arming the envelope" \
    sed -i 's|vo->frames = end;|vo->active = false;|' audio_gen.c
run "15 release ignores the generation, so a stale handle stops a live voice" \
    sed -i 's|if (!vo->active \|\| vo->gen != gen) return false;   /\* freed, or reused \*/|if (!vo->active) return false;|' audio_gen.c
run "16 the sample voice skips the envelope, so the bed starts with a step" \
    sed -i 's|acc += (int32_t)((((long)s \* (long)vo->peak) >> 15) \* env);|acc += (int32_t)(((long)s * (long)vo->peak) >> 15);|' audio_gen.c

rm -rf "$W"
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
