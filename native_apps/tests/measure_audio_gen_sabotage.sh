#!/bin/bash
# Measure that audio_gen_test's level, limiter and voice-tail groups can FAIL.
# Copies only; the tree is untouched.  ⚠️ Cases 2 and 5 rotted once already, when
# AUDIO_MIX_CEIL became derived and the default limiter became HARD — a stanza
# that no longer matches prints "NO-OP EDIT" rather than "0 failed", which is the
# whole reason that guard exists.  Read the output; do not just count it.
cd /mnt/c/work/arca/native_apps || exit 1
W=$(mktemp -d /tmp/lim.XXXXXX)
HARNESS_ERR=0
run() {
  local name="$1"; shift
  rm -rf "$W/c"; mkdir -p "$W/c"
  cp common/audio_gen.c common/audio_gen.h "$W/c/"
  ( cd "$W/c" && "$@" ) || { echo "$name: SABOTAGE DID NOT APPLY"; HARNESS_ERR=1; return; }
  if ! diff -q "$W/c/audio_gen.c" common/audio_gen.c >/dev/null || \
     ! diff -q "$W/c/audio_gen.h" common/audio_gen.h >/dev/null; then :; else
     echo "$name: NO-OP EDIT — pattern rotted"; HARNESS_ERR=1; return; fi
  gcc -Wall -Wextra -Wno-unused-parameter -I "$W/c" -o "$W/t" \
      tests/audio_gen_test.c "$W/c/audio_gen.c" -lm 2>"$W/cc.log" || {
      echo "$name: did not compile ($(head -1 "$W/cc.log"))"; return; }
  # ⚠️ stdbuf, because a sabotage that CRASHES loses everything libc had buffered
  # into the pipe, and then a caught sabotage reads as an undetected one.
  out=$(stdbuf -oL timeout 60 "$W/t" 2>&1); rc=$?
  n=$(printf '%s\n' "$out" | grep -c 'FAIL:')
  if [ "$rc" -ge 124 ]; then echo "$name: $n failed, then TIMED OUT/SIGNALLED (rc=$rc)"
  else echo "$name: $n failed"; fi
  printf '%s\n' "$out" | grep 'FAIL:' | sed 's/^/      /' | head -6
}
run "1 knee below one voice's peak" \
    sed -i 's/#define AUDIO_MIX_KNEE           AUDIO_PEAK/#define AUDIO_MIX_KNEE           (AUDIO_PEAK * 2 \/ 3)/' audio_gen.h
run "2 derived ceiling outside int16 (3x the knee)" \
    sed -i 's|int32_t ceil_at = knee + knee \* 44 / 100;|int32_t ceil_at = knee * 3;|' audio_gen.c
run "3 no knee at all: y = K + span*u (linear, unbounded)" \
    sed -i 's|span \* (u / (1.0 + u))|span * u|' audio_gen.c
run "4 counters swapped: the knee counts as a clip" \
    sed -i 's/if (m->limit == AUDIO_MIX_HARD) m->clipped++;/if (m->limit == AUDIO_MIX_HARD) m->limited++;/' audio_gen.c
run "5 default mode is SOFT again, not ScummVM's clamped add" \
    sed -i 's/m->limit = AUDIO_MIX_HARD;     \/\* clampedAdd/m->limit = AUDIO_MIX_SOFT;     \/* clampedAdd/' audio_gen.c
run "6 truncate instead of round (the lost LSB at the knee)" \
    sed -i 's/int32_t v = (int32_t)(out + 0.5);/int32_t v = (int32_t)out;/' audio_gen.c
run "7 soft path ignores the sign of the sum" \
    sed -i 's/return (acc < 0) ? -v : v;/return v;/' audio_gen.c
run "8 volume denominator is 255, not 256 (ScummVM has BOTH — the easy slip)" \
    sed -i 's|/ AUDIO_VOL_UNITY;|/ (AUDIO_VOL_UNITY - 1);|' audio_gen.c
run "9 the voice peak is not clamped to full scale" \
    sed -i 's/if (p > AUDIO_FULL_SCALE) p = AUDIO_FULL_SCALE;//' audio_gen.c
run "10 attenuation is a DIVIDE, not a shift (-1/2 == 0)" \
    sed -i 's|buf\[i\] = (int16_t)(buf\[i\] >> shift);|buf[i] = (int16_t)(buf[i] / (1 << shift));|' audio_gen.c
run "11 the knee setter ignores its argument" \
    sed -i 's/m->knee = (knee > 0) ? knee : AUDIO_MIX_KNEE;/m->knee = AUDIO_MIX_KNEE;/' audio_gen.c
run "12 every voice gets generation 1 — slot reuse becomes invisible" \
    sed -i 's/vo->gen    = ++m->gen_seq;/vo->gen    = 1;/' audio_gen.c
run "13 the voice tail ignores the generation it was asked about" \
    sed -i 's/if (!vo->active || vo->gen != gen) return 0;/if (!vo->active) return 0;/' audio_gen.c
# ⚠️ This one must edit the RETURN, not the `long left = …` line: that expression
# appears in audio_mix_pending() too, and sed replaced both — making the bus tail
# call itself, which SIGSEGVed the suite before it printed a single FAIL.  A
# crashing sabotage reads exactly like an undetected one unless the harness says
# "rc=139", which is why it does.
run "14 THE DEFECT RESTORED: a voice's tail is the whole bus's tail" \
    sed -i 's|return (left > 0) ? left : 0;|return audio_mix_pending(m);|' audio_gen.c
rm -rf "$W"
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
