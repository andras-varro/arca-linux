#!/bin/bash
# Measure that audio_eq_test can FAIL.  Copies only; the tree is untouched.
# audio_eq.c is NEW, so "seen failing against the pre-change source" has no
# meaning; this breaks a copy in single ways instead and counts the failures.
# Run from native_apps/ in WSL.  Each control must report >= 1 failure, and the
# unmodified copy 0.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="${AUDIO_EQ_SCRATCH:-/mnt/c/work/rw-scratch/jfe_sab}"
rm -rf "$W"; mkdir -p "$W"
cp "$HERE/common/audio_eq.h" "$HERE/common/audio_eq.c" "$HERE/tests/audio_eq_test.c" "$W/"

run() {   # name, sed expression ("" = control)
    cp "$HERE/common/audio_eq.c" "$W/audio_eq.c"
    [ -n "$2" ] && sed -i "$2" "$W/audio_eq.c"
    if [ -n "$2" ] && cmp -s "$HERE/common/audio_eq.c" "$W/audio_eq.c"; then
        echo "SKIP $1: sed matched nothing"; bad=1; return
    fi
    ( cd "$W" && gcc -w -I. -o t audio_eq_test.c audio_eq.c -lm ) || { echo "$1: build failed"; bad=1; return; }
    out="$(cd "$W" && ./t 2>&1)"; n=$(printf '%s\n' "$out" | grep -c '^FAIL')
    echo "$1: $n failing checks"
    if [ -z "$2" ]; then [ "$n" -eq 0 ] || bad=1; else [ "$n" -ge 1 ] || bad=1; fi
}
bad=0
run "control (unmodified)"        ""
run "feedback a1 sign swapped"     's|e->a1 = q28(c\[3\]);|e->a1 = q28(-c[3]);|'
run "saturation dropped"           's|if (v > SIG_MAX) return SIG_MAX;||; s|if (v < SIG_MIN) return SIG_MIN;||; s|if (o >  32767) o =  32767;||; s|if (o < -32768) o = -32768;||'
run "state shared across channels" 's|EqHist \*h = &s->h\[ch\]\[bd\];|EqHist *h = \&s->h[0][bd];|'
run "rate ignored"                 's|rbj(b, rate, g, c);|rbj(b, 44100, g, c);|'
run "flat not bypassed"            's|if (g == 0 \|\| rate <= 0|if (rate <= 0|'
run "headroom pregain dropped"     's|if (p->n > 0 \&\& maxb > 0) {|if (0) {|'
run "Nyquist guard dropped"        's|BAND_HZ\[b\] \* 20 >= rate \* 9|0|'
exit $bad
