#!/bin/bash
# Measure that launcher_args_test can FAIL, one targeted defect per group.
#
# There is no pre-fix source for a new test, so "seen passing" is not evidence
# it can fail.  Each case below copies native_apps/app_launcher/app_launcher.c
# to a scratch tree, breaks parse_args() in the copy in exactly one way, shadows
# the real file with -I over the copy, rebuilds the test and reports whether the
# group that defect targets went red.
#
# Case 0 is the control on the harness itself: the same rig with an untouched
# copy must be GREEN.  Without it a rig that failed to shadow, or failed to
# compile, would report every group "caught" or every group "not caught" for a
# reason that has nothing to do with the assertions.
#
# Group F can only fail by CRASHING — its assertion is that parse_args(NULL)
# does not dereference — so the verdict reads the exit status as well as the
# FAIL lines, and stdbuf line-buffers the run because a crash otherwise takes
# libc's buffered output with it and a caught sabotage reads as an undetected
# one.
#
# Restores are file copies from a pristine scratch copy.  git is never invoked:
# a checkout here would destroy whatever uncommitted work is being measured.
# Nothing is written inside the repo.
set -u

# Both files travel together, so the test is beside this script wherever the
# pair lives.  Resolved before the cd, because the cd is what makes -I. mean
# native_apps.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd /mnt/c/work/roomwizard/native_apps || exit 1

CFLAGS=(-Wall -Wextra -Wno-unused-parameter)
SRCS=(common/framebuffer.c common/touch_input.c common/hardware.c common/common.c
      common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c
      common/audio_out.c common/audio_wav.c common/config.c common/gamepad.c
      common/input_slots.c common/input_scan.c common/ppm.c common/icon_grid.c
      common/logger.c common/pointer.c common/start_menu.c common/status_line.c common/top_bar.c control_panel/cpu_load.c)

W=$(mktemp -d /tmp/launcherargs.XXXXXX)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/pristine"
cp app_launcher/app_launcher.c "$W/pristine/app_launcher.c"

not_caught=0

# run <group-letter> <description> [sabotage command…]
run() {
    local g="$1" name="$2"
    shift 2
    local out rc gn total verdict

    rm -rf "$W/c"
    mkdir -p "$W/c/app_launcher"
    cp "$W/pristine/app_launcher.c" "$W/c/app_launcher/app_launcher.c"

    if [ "$#" -gt 0 ]; then
        if ! ( cd "$W/c/app_launcher" && "$@" ); then
            printf '%-3s %-46s %s\n' "$g" "$name" "SABOTAGE DID NOT APPLY"
            not_caught=$((not_caught + 1))
            return
        fi
        if diff -q "$W/c/app_launcher/app_launcher.c" \
                   "$W/pristine/app_launcher.c" >/dev/null; then
            printf '%-3s %-46s %s\n' "$g" "$name" "NO-OP EDIT - pattern rotted"
            not_caught=$((not_caught + 1))
            return
        fi
    fi

    # "$W/c" ahead of "." is the whole mechanism: the test includes
    # "app_launcher/app_launcher.c" and this is what makes that the broken copy.
    if ! gcc "${CFLAGS[@]}" -I "$W/c" -I. \
             -Dmain=app_launcher_main_unused -o "$W/t" \
             "$HERE/launcher_args_test.c" "${SRCS[@]}" -lm 2>"$W/cc.log"; then
        printf '%-3s %-46s %s\n' "$g" "$name" \
               "DID NOT COMPILE ($(head -1 "$W/cc.log"))"
        not_caught=$((not_caught + 1))
        return
    fi

    out=$(stdbuf -oL timeout 60 "$W/t" 2>&1)
    rc=$?
    gn=$(printf '%s\n' "$out" | grep -c "^  FAIL $g ")
    total=$(printf '%s\n' "$out" | grep -c '^  FAIL ')

    if [ "$g" = "0" ]; then
        # Inverted: the control must be clean, or every verdict below is void.
        if [ "$rc" -eq 0 ] && [ "$total" -eq 0 ]; then
            verdict="CONTROL OK - pristine copy is GREEN"
        else
            verdict="CONTROL BROKEN - rc=$rc, $total FAIL (rig is void)"
            not_caught=$((not_caught + 1))
        fi
    elif [ "$gn" -gt 0 ]; then
        verdict="caught ($gn FAIL in $g, $total total, rc=$rc)"
    elif [ "$rc" -ge 124 ]; then
        verdict="caught by crash/timeout rc=$rc ($total FAIL printed first)"
    else
        verdict="NOT CAUGHT ($total FAIL elsewhere, rc=$rc)"
        not_caught=$((not_caught + 1))
    fi

    printf '%-3s %-46s %s\n' "$g" "$name" "$verdict"
    printf '%s\n' "$out" | grep "^  FAIL $g " | sed 's/^ */        /' | head -3
}

printf '%-3s %-46s %s\n' "grp" "defect applied to the COPY" "verdict"

run 0 "no sabotage at all (control on this harness)"

run A "swapped enum: ARG_FB = 2, ARG_TOUCH = 1" \
    sed -i -e 's/ARG_FB  *= 1,/ARG_FB       = 2,/' \
           -e 's/ARG_TOUCH  *= 2,/ARG_TOUCH    = 1,/' app_launcher.c

run B "exact match instead of strstr (tokenised)" \
    sed -i -e 's/strstr(s, "fb")/strcmp(s, "fb") == 0/' \
           -e 's/strstr(s, "touch")/strcmp(s, "touch") == 0/' app_launcher.c

run C 'the "none" test becomes a substring too' \
    sed -i 's/strcmp(s, "none") == 0/strstr(s, "none") != NULL/' app_launcher.c

run D "no fallback: an unmatched string is ARG_NONE" \
    sed -i 's/return m ? m : ARG_FB_TOUCH;/return m;/' app_launcher.c

# Two edits, so it needs a function: _GNU_SOURCE has to be defined ahead of
# every libc header for strcasestr to be declared at all.
sab_case_insensitive() {
    sed -i '1i #define _GNU_SOURCE 1' app_launcher.c &&
    sed -i -e 's/strcmp(s, "none")/strcasecmp(s, "none")/' \
           -e 's/strstr(s, "fb")/strcasestr(s, "fb")/' \
           -e 's/strstr(s, "touch")/strcasestr(s, "touch")/' app_launcher.c
}
run E "matching becomes case-INsensitive" sab_case_insensitive

run F "the NULL guard is dropped from the || test" \
    sed -i 's/if (!s || !\*s)/if (!*s)/' app_launcher.c

if [ "$not_caught" -eq 0 ]; then
    echo "every group caught, and the control was green"
else
    echo "$not_caught case(s) NOT caught - strengthen those assertions"
fi
exit $((not_caught > 0))
