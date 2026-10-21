#!/bin/bash
# Measure that eq_panel_test can FAIL.  Copies only; the tree is untouched.
# eq_panel.c is NEW, so "seen failing against the pre-change source" has no
# meaning; this breaks a copy in single ways instead and counts the failures.
# Run from native_apps/ in WSL.  Each control must report >= 1 failure, and the
# unmodified copy 0.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="${EQ_PANEL_SCRATCH:-/mnt/c/work/rw-scratch/jfq_sab}"
rm -rf "$W"; mkdir -p "$W"
cd "$HERE" || exit 1
read -r -a OTHERS <<< "common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"

run() {   # name, sed expression ("" = control)
    cp "$HERE/common/eq_panel.c" "$W/eq_panel.c"
    [ -n "$2" ] && sed -i "$2" "$W/eq_panel.c"
    if [ -n "$2" ] && cmp -s "$HERE/common/eq_panel.c" "$W/eq_panel.c"; then
        echo "SKIP $1: sed matched nothing"; bad=1; return
    fi
    gcc -w -I common -o "$W/t" tests/eq_panel_test.c "$W/eq_panel.c" "${OTHERS[@]}" -lm -lpthread \
        || { echo "$1: build failed"; bad=1; return; }
    out="$("$W/t" 2>&1)"; n=$(printf '%s\n' "$out" | grep -c '^FAIL')
    echo "$1: $n failing checks"
    if [ -z "$2" ]; then [ "$n" -eq 0 ] || bad=1; else [ "$n" -ge 1 ] || bad=1; fi
}
bad=0
run "control (unmodified)"        ""
run "dB sign flipped"             's|return EQ_MAX_DB - (2 \* EQ_MAX_DB \* pos + range / 2) / range;|return -EQ_MAX_DB + (2 * EQ_MAX_DB * pos + range / 2) / range;|'
run "cancel does not restore"     '/k == EQK_BACK/s|memcpy(p->gains, p->orig, sizeof p->gains); ||'
run "Up/Down swapped"             's|(k == EQK_UP ? 1 : -1)|(k == EQK_UP ? -1 : 1)|'
run "band left not clamped"       's|p->band = clampi(p->band - 1, 0, EQ_BANDS - 1);|p->band = p->band - 1;|'
run "band_at always first"        's|if (d < bd) { bd = d; best = b; }|if (0) { bd = d; best = b; }|'
run "preset key does not apply"   's|^            memcpy(p->gains, g, sizeof p->gains);||'
run "store drops other keys"      's|config_load(&cfg);||'
run "config never a preset name"  's|if (m >= 0) snprintf(buf|if (0) snprintf(buf|'
run "DONE restores the old gains" 's|if (p->btn_focus == 1) return EQP_DONE;|if (p->btn_focus == 1) { memcpy(p->gains, p->orig, sizeof p->gains); return EQP_DONE; }|'
run "open does not clamp"         's|p->gains\[i\] = clampi(gains\[i\], -EQ_MAX_DB, EQ_MAX_DB);|p->gains[i] = gains[i];|'
exit $bad
