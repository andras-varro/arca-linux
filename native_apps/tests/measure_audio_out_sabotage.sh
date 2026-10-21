#!/bin/bash
# Measure that audio_out_test can FAIL.  Copies only; the tree is untouched.
#
# `audio_out.c` is NEW, so "seen failing against the pre-change source" has no
# older file to compile against — this sweep is that evidence instead.  Each
# stanza breaks exactly one rule the library states in its header and reports how
# many of the suite's checks notice.  A count of 0 is a HOLE IN THE SUITE, not a
# clean bill of health: three stanzas below found one, and the checks that close
# them were added because this ran.
#
# Structure differs from measure_audio_gen_sabotage.sh in one way only: the test
# includes "../common/audio_out.h", a path relative to its own directory, which
# -I cannot override.  So the copy reproduces the two directories rather than one
# flat dir, or the suite would compile against the REAL header while linking the
# sabotaged .c — a mismatch that reads as a pass.
cd /mnt/c/work/arca/native_apps || exit 1
W=$(mktemp -d /tmp/aout.XXXXXX)
HARNESS_ERR=0
run() {
  local name="$1"; shift
  rm -rf "$W/c"; mkdir -p "$W/c/common" "$W/c/tests"
  cp common/audio_out.c common/audio_out.h common/audio_gen.c common/audio_gen.h common/audio_eq.c common/audio_eq.h common/audio_enh.c common/audio_enh.h common/config.h "$W/c/common/"
  cp tests/audio_out_test.c "$W/c/tests/"
  ( cd "$W/c/common" && "$@" ) || { echo "$name: SABOTAGE DID NOT APPLY"; HARNESS_ERR=1; return; }
  if ! diff -q "$W/c/common/audio_out.c" common/audio_out.c >/dev/null || \
     ! diff -q "$W/c/common/audio_out.h" common/audio_out.h >/dev/null || \
     ! diff -q "$W/c/common/audio_gen.c" common/audio_gen.c >/dev/null; then :; else
     echo "$name: NO-OP EDIT — pattern rotted"; HARNESS_ERR=1; return; fi
  gcc -Wall -Wextra -Wno-unused-parameter -o "$W/t" \
      "$W/c/tests/audio_out_test.c" "$W/c/common/audio_out.c" \
      "$W/c/common/audio_gen.c" "$W/c/common/audio_eq.c" "$W/c/common/audio_enh.c" -lm 2>"$W/cc.log" || {
      echo "$name: did not compile ($(head -1 "$W/cc.log"))"; return; }
  # ⚠️ `stdbuf -oL` and the exit code are both load-bearing, and their absence
  # cost a wrong answer here on 2026-08-18.  `out=$(…)` makes stdout a PIPE, so
  # libc switches to full buffering and a sabotage that CRASHES loses every line
  # it had already printed — the sweep then reports "0 failed", which reads as
  # "the suite does not catch this" when the suite in fact caught it and died.
  # Line-buffer the child, and report a signal or a timeout as the result it is.
  out=$(timeout 60 stdbuf -oL -eL "$W/t" 2>&1); local rc=$?
  n=$(printf '%s\n' "$out" | grep -c 'FAIL:')
  case $rc in
    0|1) echo "$name: $n failed" ;;
    124) echo "$name: $n failed, then TIMED OUT — a hang is a result" ;;
    *)   echo "$name: $n failed, then DIED rc=$rc — a crash is a result" ;;
  esac
  printf '%s\n' "$out" | grep 'FAIL:' | sed 's/^/      /' | head -6
}

# 1  The open no longer prefills, so the stream starts EMPTY — i.e. it starts with
#    the transition the whole design exists to remove.
run "1 prefill dropped (the stream starts empty)" \
    sed -i 's/if (out->lead_frames > 0) {/if (out->lead_frames < 0) {/' audio_out.c

# 2  Stale scratch leaks into the tail of a short fill.  `audio_mix_render()`
#    deliberately does not touch the buffer on a silent bus, so the previous
#    service's samples would be re-written as this one's.
run "2 scratch not zeroed before the fill" \
    sed -i 's|memset(buf, 0, (size_t)samples \* sizeof(int16_t));|/* sabotage: not zeroed */;|' audio_out.c

# 3  Writes the free space instead of the lead: an empty 743 ms ring is accepted
#    whole, and then the next sound plays three quarters of a second late.
run "3 service fills the FREE SPACE, not the lead" \
    sed -i 's|long want = audio_pump_frames|long want = sp.space;\n    if (0) want = audio_pump_frames|' audio_out.c

# 4  Lead from AUDIO_PUMP_LEAD_MS instead of whole device periods — 3528 frames at
#    44100, which is not a period multiple and is what XRUNed.
run "4 lead from the ms constant, not the PERIOD" \
    sed -i 's|out->lead_frames   = audio_pump_lead_frames(|out->lead_frames = audio_frames_for_ms(out->rate, AUDIO_PUMP_LEAD_MS);\n    if (0) out->lead_frames = audio_pump_lead_frames(|' audio_out.c

# 5  Channel count from the REQUEST rather than the read-back.  This is the
#    silently-mute failure the library exists to remove.
run "5 channels from the request, not the grant" \
    sed -i 's|out->channels = channels;|out->channels = channels_req;|' audio_out.c

# 6  Attenuation as a rounding-free integer DIVIDE rather than an arithmetic
#    shift: -1/2 == 0 but -1 >> 1 == -1, so ScummVM stops being bit-identical.
run "6 attenuation as a multiply/divide, not a shift" \
    sed -i 's|buf\[i\] = (int16_t)(buf\[i\] >> shift);|buf[i] = (int16_t)(buf[i] / (1 << shift));|' audio_gen.c

# 7  Mode 2 no longer refuses against an installed callback: two writers
#    interleaving frames into a stream with no mono path underneath.
run "7 mode 2 not refused against a callback" \
    sed -i 's|if (out->fill) {|if (0) {|' audio_out.c

# 8  The drain waits for ZERO instead of the over-report slack, so every process
#    exit costs the bound rather than the tail.
run "8 drain waits for zero, not the slack" \
    sed -i 's|long floor_frames = ospace_slack(out->period_frames);|long floor_frames = 0;|' audio_out.c

# 9  The serviced policy gets a non-zero wait, so a call documented as never
#    sleeping sleeps up to AUDIO_ALIGN_TRIES times inside a render loop.
run "9 serviced policy given a non-zero wait" \
    sed -i 's|AOPOL_SERVICE = { 0, 0, true };|AOPOL_SERVICE = { 1000, 0, true };|' audio_out.c

# 10 HARNESS CONTROL.  One accessor lies, and nothing else changes.  Exactly ONE
#    group and exactly ONE check must fail: if this reports a cascade the suite is
#    coupled, and if it reports 0 the counters are not individually observed.
run "10 CONTROL: audio_out_starved() always returns 0" \
    sed -i 's|return out ? out->starved     : 0;|return 0;|' audio_out.c

rm -rf "$W"
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
