#!/bin/bash
# Measure that every group of touch_map_test can FAIL.
#
# touch_map_test.c is a NEW test, so there is no pre-fix revision of
# common/touch_input.c to compile it against — the only way to show it can go
# red is to break the subject on purpose.  Each case copies touch_input.c and
# touch_input.h to a scratch tree, applies ONE targeted defect to the COPY,
# rebuilds the test against that copy, and reports which assertion groups turned
# red.  The repo is never written to and git is never used: the pristine source
# is restored by file copy, because a measurement loop that restores with git
# destroys the uncommitted fix it is measuring.
#
# A sabotage that no longer matches its line prints DID-NOT-APPLY or NO-OP
# rather than "0 red", which is the difference between a suite with a hole and a
# harness with a rotted pattern.  Every case also declares the group it is aimed
# at, so a case that goes red somewhere else — a rotted replacement hitting a
# different segment — shows up as MISMATCH instead of passing for the wrong
# reason.  Read the output; do not just count it.
#
#   wsl.exe -e bash -lc /mnt/c/work/rw-scratch/c6/measure_touch_map_sabotage.sh
set -u

REPO=/mnt/c/work/arca/native_apps
TEST=/mnt/c/work/rw-scratch/c6/touch_map_test.c
WORK=/mnt/c/work/rw-scratch/c6/sab

cd "$REPO" || exit 1
rm -rf "$WORK"
mkdir -p "$WORK/pristine" || exit 1
cp common/touch_input.c common/touch_input.h "$WORK/pristine/" || exit 1

caught=""
HARNESS_ERR=0

run() {
    name=$1
    want=$2
    shift 2
    rm -rf "$WORK/c"
    mkdir -p "$WORK/c"
    cp "$WORK/pristine/touch_input.c" "$WORK/pristine/touch_input.h" "$WORK/c/"
    if ! ( cd "$WORK/c" && "$@" ); then
        printf '  %-58s SABOTAGE DID NOT APPLY\n' "$name"
        HARNESS_ERR=1; return
    fi
    if diff -q "$WORK/c/touch_input.c" "$WORK/pristine/touch_input.c" >/dev/null &&
       diff -q "$WORK/c/touch_input.h" "$WORK/pristine/touch_input.h" >/dev/null; then
        printf '  %-58s NO-OP EDIT — pattern rotted\n' "$name"
        HARNESS_ERR=1; return
    fi
    if ! gcc -Wall -Wextra -Wno-unused-parameter -I "$WORK/c" -I common \
            -o "$WORK/t" "$TEST" "$WORK/c/touch_input.c" \
            common/framebuffer.c common/hardware.c common/config.c -lm \
            2>"$WORK/cc.log"; then
        printf '  %-58s DID NOT COMPILE: %s\n' "$name" "$(head -1 "$WORK/cc.log")"
        HARNESS_ERR=1; return
    fi
    # stdbuf, because a sabotage that CRASHES loses whatever libc had buffered
    # into the pipe, and then a caught sabotage reads as an undetected one.
    out=$(stdbuf -oL timeout 60 "$WORK/t" 2>&1)
    rc=$?
    n=$(printf '%s\n' "$out" | grep -c '^  FAIL ')
    red=$(printf '%s\n' "$out" | sed -n 's/^  FAIL \([A-Z]\).*/\1/p' | sort -u | tr -d '\n')
    verdict="caught"
    case "$red" in
        *"$want"*) ;;
        *) verdict="MISMATCH — aimed at $want" ;;
    esac
    if [ "$n" -eq 0 ]; then
        verdict="NOT CAUGHT"
    fi
    if [ "$rc" -ge 124 ]; then
        verdict="$verdict, then TIMED OUT/SIGNALLED rc=$rc"
    elif [ "$rc" -eq 0 ]; then
        verdict="NOT CAUGHT — the suite still exits 0"
    fi
    printf '  %-58s %2d red in [%s] %s\n' "$name" "$n" "$red" "$verdict"
    caught="$caught$red"
}

echo "sabotage sweep over common/touch_input.c (copies only)"

run "A  axis_dims falls back to 640, not the panel 800" A \
    sed -i 's|\*phys_w = (w > 1) ? w : 800;|*phys_w = (w > 1) ? w : 640;|' touch_input.c
run "A  the linear span maps onto dim, not dim-1" A \
    sed -i 's|return (int)((raw - v0) \* last / span);|return (int)((raw - v0) * dim / span);|' touch_input.c
run "B  the interior segment rises by q2, not q2-q1" B \
    sed -i 's|(raw - k_lo) \* (q2 - q1) / (k_hi - k_lo)|(raw - k_lo) * q2 / (k_hi - k_lo)|' touch_input.c
run "B  the low knot sits at dim/5, not dim/4" B \
    sed -i 's|#define TOUCH_KNOT_LO(dim) ((dim) / 4)|#define TOUCH_KNOT_LO(dim) ((dim) / 5)|' touch_input.h
run "C  raw is not clamped down to raw_max_x" C \
    sed -i 's|if (rx > touch->raw_max_x) rx = touch->raw_max_x;||' touch_input.c
run "C  raw is not clamped down to raw_max_y" C \
    sed -i 's|if (ry > touch->raw_max_y) ry = touch->raw_max_y;||' touch_input.c
run "D  the logical low clamp on X is gone" D \
    sed -i 's|if (mx < 0) mx = 0;||' touch_input.c
run "D  the logical high clamp on Y is gone" D \
    sed -i 's|if (my >= touch->screen_height) my = touch->screen_height - 1;||' touch_input.c
run "D  the upper segment falls instead of rising" D \
    sed -i 's|return (int)(q2 + (raw - k_hi)|return (int)(q2 - (raw - k_hi)|' touch_input.c
run "E  a zero X span is not degenerate any more" E \
    sed -i 's|if (touch->raw_max_x - touch->raw_min_x <= 0) {|if (touch->raw_max_x - touch->raw_min_x < 0) {|' touch_input.c
run "E  the Y knots are not zeroed with the range" E \
    sed -i 's|touch->raw_knot_lo_y = touch->raw_knot_hi_y = 0;||' touch_input.c
run "E  the reset writes 4095 into raw_min_x too" E \
    sed -i 's|touch->raw_min_x = 0; touch->raw_max_x = 4095;|touch->raw_min_x = 4095; touch->raw_max_x = 4095;|' touch_input.c
run "F  the portrait rotation does not invert Y" F \
    sed -i 's|mx = phys_h - 1 - py;|mx = py;|' touch_input.c
run "F  the rotated Y comes from raw Y, not raw X" F \
    sed -i 's|my = px;|my = py;|' touch_input.c
run "G  the viewport X origin is not subtracted" G \
    sed -i 's|mx -= touch->view_x;||' touch_input.c
run "G  the viewport Y origin is ADDED" G \
    sed -i 's|my -= touch->view_y;|my += touch->view_y;|' touch_input.c
run "H  the X clamp is hardcoded to the panel 800" H \
    sed -i 's|if (mx >= touch->screen_width)  mx = touch->screen_width - 1;|if (mx >= 800)  mx = 799;|' touch_input.c
run "H  the Y clamp is hardcoded to the panel 480" H \
    sed -i 's|if (my >= touch->screen_height) my = touch->screen_height - 1;|if (my >= 480) my = 479;|' touch_input.c
run "I  the monotone guard ignores knot order" I \
    sed -i 's|if (!(v0 < k_lo \&\& k_lo < k_hi \&\& k_hi < v1)) {|if (!(v0 < k_lo \&\& k_hi < v1)) {|' touch_input.c

echo
echo "per-group result"
missed=0
for g in A B C D E F G H I; do
    case "$caught" in
        *"$g"*) printf '  %s  caught\n' "$g" ;;
        *) printf '  %s  NOT CAUGHT — that group is vacuous\n' "$g"; missed=$((missed + 1)) ;;
    esac
done

echo
if [ "$missed" -eq 0 ]; then
    echo "every group seen failing"
else
    echo "$missed group(s) never seen failing"
fi
rm -rf "$WORK/c" "$WORK/t" "$WORK/cc.log"
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing or did not compile"; exit 1; }
exit "$missed"
