#!/bin/bash
# Measure that audio_enh_test can FAIL.  Copies only; the tree is untouched.
# audio_enh.c is NEW, so there is no pre-fix source; this breaks a COPY in single
# ways and counts the failures.  Run from native_apps/ in WSL.  Each control must
# report >= 1 failure, and the unmodified copy 0.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="${AUDIO_ENH_SCRATCH:-/mnt/c/work/rw-scratch/enh2_sab}"
rm -rf "$W"; mkdir -p "$W"
cp "$HERE/common/audio_enh.h" "$HERE/common/audio_enh.c" "$HERE/tests/audio_enh_test.c" "$W/"

run() {   # name, sed expression ("" = control)
    cp "$HERE/common/audio_enh.c" "$W/audio_enh.c"
    [ -n "$2" ] && sed -i "$2" "$W/audio_enh.c"
    if [ -n "$2" ] && cmp -s "$HERE/common/audio_enh.c" "$W/audio_enh.c"; then
        echo "SKIP $1: sed matched nothing"; bad=1; return
    fi
    ( cd "$W" && gcc -w -I. -o t audio_enh_test.c audio_enh.c -lm ) || { echo "$1: build failed"; bad=1; return; }
    out="$(cd "$W" && ./t 2>&1)"; n=$(printf '%s\n' "$out" | grep -c '^FAIL')
    echo "$1: $n failing checks"
    if [ -z "$2" ]; then [ "$n" -eq 0 ] || bad=1; else [ "$n" -ge 1 ] || bad=1; fi
}
bad=0
run "control (unmodified)"        ""
run "bypass not exact (off ignored)" 's|if (!e.on \|\| rate <= 0) return;|if (rate <= 0) return;|'
run "all-zero amounts not bypass"  's|if (!p->hb \&\& !p->db \&\& !p->ht) return;|p->hb = 1;|'
run "peak guard removed"           's|if (!bad) break;|break;|'
run "guard scales dry too"         's|ol  = dl + |ol  = ((dl * guard) >> 15) + |'
run "bass shaper bypassed (no harmonics)" 's|int32_t z = shape_cubic(a, p->hb_drive);|int32_t z = a;|'
run "mono sum replaced by left"    's|int32_t m  = (xl + xr) >> 1;|int32_t m  = xl;|'
run "treble high-pass is low-pass" 's|biquad(&p->ht_hp, rate, 5000.0 - 20.0 \* e.ht_rng, 1);|biquad(\&p->ht_hp, rate, 5000.0 - 20.0 * e.ht_rng, 0);|'
run "drum band collapsed"          's|biquad(&p->db_hi, rate, 200.0 + 4.0 \* e.db_rng, 0);|biquad(\&p->db_hi, rate, 50.0 + 1.0 * e.db_rng, 0);|'
run "range ignored (hb drive)"     's|256 + (768 \* e.hb_rng) / 100|256|'
run "parse clamp dropped"          's|enh_clamp(&e);\n    \*out|XX|; s|    \*out = e;|    *out = (AudioEnh){ (int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], (int)v[5], (int)v[6] };|'
exit $bad
