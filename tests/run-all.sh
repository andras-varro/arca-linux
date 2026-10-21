#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run-all.sh — the host gate: every test this repo can run without a device.
#
# WHY THIS EXISTS
#   Until this script, nothing ran more than one suite.  The twelve shell
#   suites in tests/ each had to be invoked by hand, the host-gcc
#   regressions in native_apps/tests/ carried their build line in a comment
#   and nothing anywhere executed them, and no build or deploy path ran any
#   of them.  A change could reach a device having been graded by nobody.
#
# WHAT IT RUNS  (three phases, one data table each — see the tables below)
#   1. shell suites        tests/*_test.sh plus doc_check.sh
#   2. host C regressions  native_apps/tests/*_test.c, host gcc, no device
#   3. shellcheck          every tracked *.sh, in two tiers (see PHASE 3)
#
# MEASURED COST, this host, 2026-09-08, warm cache:
#   phase 1  185 s   — arca_ssh_test.sh alone is 121 s (it starts a real sshd
#                      and probes TEST-NET-1, so most of that is timeouts);
#                      doc_check.sh 36 s; the other ten total 28 s
#   phase 2   14 s   — ten builds plus ten runs
#   phase 3   10 s
#   Total    ~3.5 min, which is less than one ScummVM rebuild.
#
# SCOPE
#   --scope=deploy  omits doc_check.sh, which grades documentation and cannot
#                   affect a deployed byte.  This is what deploy-all.sh runs.
#   --scope=all     everything.  The default, and what release.sh runs, since
#                   a release bundle ships the documentation too.
#
# EXIT
#   0  every subject passed
#   1  a subject FAILED — do not deploy
#   2  HARNESS ERROR: the gate could not judge.  Never used for "a test
#      failed".  Raised when a subject list comes back empty, when a
#      *_test.c on disk is in neither table, when a required tool is absent,
#      or when a suite itself exits 2.
#
# A SKIP IS NOT A PASS.  commission_offline_test.sh exits 0 when it cannot
# get root, and a runner that grades on exit status alone would report that
# as green having tested nothing.  Skips are counted and named separately,
# and the summary prints the command that would cover them.
#
# NEGATIVE CONTROLS: --self-test.  Each drives this script and asserts the exit
# code it must produce; the run itself says how many there are.  Every one has
# been seen to fire, and two of them first passed VACUOUSLY — see the notes at
# controls 5 and 7.  The four overrides they use (ARCA_SUITE_DIR, ARCA_CTEST_DIR,
# ARCA_SC_BASELINE, ARCA_SC_FILES) exist for that and nothing else.
#
# WHAT THIS GATE CANNOT SEE
#   Anything that needs the panel.  No screen is drawn, no touch is read, no
#   sound is heard, and no binary here is the ARM binary that ships.  It
#   cannot tell you an app looks right; it can only tell you the logic under
#   it still computes what it used to.  The first-screen smoke harness that
#   would close part of that gap needs a device and is not in this file.
#
# Run it:  bash tests/run-all.sh            (from anywhere)
#          bash tests/run-all.sh --list     (the subject tables, no run)
#          bash tests/run-all.sh --self-test
# ---------------------------------------------------------------------------
set -u

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SELF/.." && pwd)"

if [ -t 1 ]; then
    RED=$'\033[0;31m'; GRN=$'\033[0;32m'; YEL=$'\033[1;33m'
    CYA=$'\033[0;36m'; BLD=$'\033[1m';    RST=$'\033[0m'
else
    RED=''; GRN=''; YEL=''; CYA=''; BLD=''; RST=''
fi

pass_n=0; fail_n=0; skip_n=0; harness_n=0
FAILED_LIST=(); SKIPPED_LIST=(); HARNESS_LIST=()

ok()      { printf '  %s✓%s %s\n' "$GRN" "$RST" "$1"; }
bad()     { printf '  %s✗%s %s\n' "$RED" "$RST" "$1"; }
skipmsg() { printf '  %s—%s %s\n' "$YEL" "$RST" "$1"; }
head2()   { printf '\n%s%s%s\n' "$BLD" "$1" "$RST"; }
note()    { printf '    %s\n' "$1"; }

# ── PHASE 1 table: the shell suites ────────────────────────────────────────
# name|scope.  Every tests/*_test.sh on disk must appear here, and so must
# doc_check.sh; a suite that is added and not classified is a HARNESS ERROR,
# because a gate that silently stops covering a subject is worse than no
# gate.  The measure_*.sh sabotage sweeps are deliberately absent: they are
# instruments for grading a suite, they take minutes, and several print
# counts rather than a verdict.  They are run by hand when a suite changes.
SUITE_ROWS=(
    "check_arm_safe_test.sh|deploy"
    "commission_offline_test.sh|deploy"
    "provision_online_test.sh|deploy"
    "arca_bundle_ssh_test.sh|deploy"
    "arca_identify_test.sh|deploy"
    "card_layout_test.sh|deploy"
    "arca_provision_test.sh|deploy"
    "arca_release_test.sh|deploy"
    "arca_ssh_test.sh|deploy"
    "arca_state_test.sh|deploy"
    "arca_backup_test.sh|deploy"
    "product_sh_test.sh|deploy"
    "arca_source_offer_test.sh|deploy"
    "arca_kernel_tarball_test.sh|deploy"
    "arca_hostname_test.sh|deploy"
    "arca_growroot_test.sh|deploy"
    "dump_boot_regs_test.sh|deploy"
    "setup_build_env_test.sh|deploy"
    "smoke_first_screen_test.sh|deploy"
    "arca_app_test.sh|deploy"
    "doc_check.sh|docs"
)

# ── PHASE 2 table: the host C regressions ──────────────────────────────────
# name|cflags|sources.  Copied from each file's own header build line and
# kept in step with it; the two include conventions (-I common and -I.) are
# real differences, not drift.
# All run from native_apps/, all write -o build/<name>, which check-arm-safe.sh
# already skips and labels "host-compiled tests" — a count that tracks whatever
# build/ happens to hold, so it is not written here.
CTEST_ROWS=(
    "audio_alsa_lost_test|-I common -DAUDIO_OUT_HAVE_ALSA -I tests/alsa_stub -Wl,--wrap=clock_gettime -Wl,--wrap=opendir|common/audio_out.c common/audio_eq.c common/audio_gen.c"
    "audio_bed_test|-I common|common/audio_bed.c common/config.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c"
    "audio_carry_test|-I. -Wl,--wrap=audio_out_open_resolved|common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/config.c"
    "audio_eq_test|-I common|common/audio_eq.c"
    "audio_gen_test|-I common|common/audio_gen.c"
    "audio_gain_test|-I common|common/audio_gen.c"
    "audio_out_test|-I common|common/audio_out.c common/audio_eq.c common/audio_gen.c"
    "audio_sample_test|-I.|common/audio_wav.c common/audio_gen.c"
    "audio_stream_test|-I.|common/audio_wav.c common/audio_gen.c"
    "audio_stereo_test|-I common|common/audio_gen.c"
    "audio_stream_stereo_test|-I.|common/audio_wav.c common/audio_gen.c"
    "audio_music_stereo_test|-I. -Wl,--wrap=audio_out_open_resolved|common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/config.c"
    "audio_stream_fd_test|-I. -Wl,--wrap=audio_out_open_resolved|common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/config.c"
    "audio_tone_test|-I.|common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/config.c"
    "bt_ctl_test|-I.|control_panel/bt_ctl.c"
    "button_latch_test|-I common|common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "bezel_scale_test|-I common|common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "config_test|-I common|common/config.c"
    "cpu_load_test|-I.|control_panel/cpu_load.c"
    "soc_temp_test|-I.|control_panel/soc_temp.c"
    "start_menu_test|-I common|common/start_menu.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "gameover_flow_test|-I common|common/start_menu.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "modal_dialog_input_test|-I common|common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "mon_ring_test|-I.|sysmon/mon_ring.c"
    "overtemp_test|-I.|sysmon/mon_ring.c"
    "blank_decide_test|-I common|"
    "mon_graph_test|-I.|control_panel/mon_graph.c"
    "evdev_resync_test|-I common|common/gamepad.c common/input_slots.c common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "fb_rotate_test|-O2 -I common|common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "framebuffer_bpp_test|-I common|common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "font_pixel_diff_test|-I common|common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "font_test|-I common|common/font.c"
    "utf8_test|-I common|"
    "gamepad_announce_test|-I common|common/gamepad.c common/input_slots.c common/input_scan.c common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "gamepad_latch_test|-I common|common/gamepad.c common/input_slots.c common/input_scan.c common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "gamepad_rescan_hold_test|-I common|common/gamepad.c common/input_slots.c common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "gamepad_slots_test|-I common|common/gamepad.c common/input_slots.c common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "gradient_test|-I common|common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "icons_test|-I common|common/icons.c common/framebuffer.c common/font.c common/hardware.c common/config.c common/touch_input.c"
    "http_test|-I common -fsanitize=undefined|common/http.c"
    "input_config_test|-I common|common/input_scan.c"
    "input_scan_test|-I common|common/input_scan.c"
    "input_slots_test|-I common|common/input_slots.c"
    "json_test|-I common -fsanitize=undefined|common/json.c"
    "jf_api_test|-I common -I jellyfin -fsanitize=undefined|jellyfin/jf_api.c jellyfin/jf_model.c jellyfin/jf_url.c common/http.c common/json.c common/logger.c"
    "jf_core_test|-I common -I jellyfin -fsanitize=undefined|jellyfin/jf_core.c jellyfin/jf_model.c jellyfin/jf_url.c jellyfin/jf_login.c common/config.c common/json.c"
    "jf_login_test|-I common -I jellyfin|jellyfin/jf_login.c jellyfin/jf_url.c common/config.c"
    "jf_ui_layout_test|-I. -I common -I jellyfin|jellyfin/jf_ui.c jellyfin/jf_art.c common/icons.c common/jf_img.c common/logger.c common/ui_focus.c jellyfin/jf_core.c jellyfin/jf_url.c jellyfin/jf_model.c jellyfin/jf_login.c common/json.c common/http.c common/list_view.c common/top_bar.c common/icon_grid.c common/ppm.c common/pointer.c common/status_line.c control_panel/cpu_load.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "jf_model_test|-I common -I jellyfin -fsanitize=undefined|jellyfin/jf_model.c common/json.c"
    "jf_genre_test|-I common -I jellyfin -fsanitize=undefined|jellyfin/jf_model.c common/json.c"
    "jf_url_test|-I common -I jellyfin|jellyfin/jf_url.c"
    "jf_art_test|-I common -I jellyfin -I tests -fsanitize=undefined,address -fno-sanitize=shift|common/jf_img.c common/http.c jellyfin/jf_art.c jellyfin/jf_url.c common/logger.c"
    "icon_grid_focus_test|-I common|common/icon_grid.c common/ppm.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "icon_grid_nav_test|-I common|common/icon_grid.c common/ppm.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "icon_grid_nav_test|-I common|common/icon_grid.c common/ppm.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "keyboard_symbols_test|-I common|common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "keyboard_order_test|-I common|common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "launcher_args_test|-I. -Dmain=app_launcher_main_unused|common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/common.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/config.c common/gamepad.c common/input_slots.c common/input_scan.c common/ppm.c common/icon_grid.c common/logger.c common/product.c common/pointer.c common/start_menu.c common/status_line.c common/top_bar.c control_panel/cpu_load.c"
    "logger_tz_test|-I common|common/logger.c"
    "list_view_test|-I common|common/list_view.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "status_line_test|-I.|common/status_line.c control_panel/cpu_load.c"
    "top_bar_test|-I. -I common|common/top_bar.c common/icon_grid.c common/ppm.c common/status_line.c control_panel/cpu_load.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "pointer_test|-I common|common/pointer.c common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "ppm_test|-I common|common/ppm.c"
    "product_test|-I common|common/product.c"
    "text_truncate_test|-I common|common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c"
    "touch_calib_test|-I common|common/touch_calib.c common/touch_input.c common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "remote_tap_test|-I common|common/remote_tap.c common/touch_calib.c common/touch_input.c common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "touch_map_test|-I common|common/touch_input.c common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "ui_focus_test|-I common|common/ui_focus.c"
    "ui_flow_test|-I common|common/ui_flow.c"
    "ui_scale_test|-I common|common/framebuffer.c common/font.c common/hardware.c common/config.c"
    "cp_page_name_test|-I.|control_panel/cp_page_name.c"
    "sys_settings_test|-I.|control_panel/sys_settings.c"
    "cp_exec_test|-I.|control_panel/cp_exec.c"
    "backup_parse_test|-I.|control_panel/backup_parse.c"
    "usb_watch_test|-I.|common/usb_watch.c"
    "usb_bus_test|-I.|control_panel/usb_bus.c"
    "log_filter_test|-I.|control_panel/log_filter.c"
    "term_keymap_test|-I.|terminal/term_keymap.c"
    "file_manager_test|-I.|file_manager/fm_core.c"
    "vnc_pad_test|-I common -I ../vnc_client|../vnc_client/vnc_pad.c common/ui_focus.c common/input_scan.c"
)

# Files named *_test.c that are NOT host regressions: ARM tools that
# native_apps/build-and-deploy.sh cross-compiles and deploys, and device-only
# diagnostics built by hand from their own headers.  Listing them is what lets
# an unclassified newcomer raise a HARNESS ERROR instead of being quietly
# ignored.
CTEST_NOT_HOST=(
    "audio_mix_test"    # ARM, built by native_apps/build-and-deploy.sh
    "ch_test"           # device only
)

SCOPE="all"
ONLY=""
SUITE_DIR="${ARCA_SUITE_DIR:-$REPO_ROOT/tests}"
CTEST_DIR="${ARCA_CTEST_DIR:-$REPO_ROOT/native_apps/tests}"
SC_BASELINE="${ARCA_SC_BASELINE:-$REPO_ROOT/tests/shellcheck-baseline.txt}"
LOGDIR=""

usage() {
    sed -n '2,60p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --scope=*)   SCOPE="${1#*=}" ;;
        --only=*)    ONLY="${1#*=}" ;;
        --list)      ONLY="list" ;;
        --self-test) ONLY="selftest" ;;
        -h|--help)   usage ;;
        *) printf 'run-all.sh: unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

case "$SCOPE" in
    all|deploy|docs) : ;;
    *) printf 'run-all.sh: --scope must be all, deploy or docs\n' >&2; exit 2 ;;
esac

in_scope() { [ "$SCOPE" = "all" ] || [ "$1" = "$SCOPE" ]; }

# ── --list ─────────────────────────────────────────────────────────────────
if [ "$ONLY" = "list" ]; then
    head2 "shell suites (${#SUITE_ROWS[@]})"
    for r in "${SUITE_ROWS[@]}"; do printf '  %-32s %s\n' "${r%%|*}" "${r#*|}"; done
    head2 "host C regressions (${#CTEST_ROWS[@]})"
    for r in "${CTEST_ROWS[@]}"; do printf '  %s\n' "${r%%|*}"; done
    head2 "syntax-checked ARM apps (1)"
    printf '  %s\n' control_panel
    head2 "*_test.c that are NOT host regressions (${#CTEST_NOT_HOST[@]})"
    for r in "${CTEST_NOT_HOST[@]}"; do printf '  %s\n' "$r"; done
    exit 0
fi

# ── --self-test ────────────────────────────────────────────────────────────
# Each control drives THIS script and asserts the exit code it must produce.
# A control that stops firing is a hole in the gate, so a control that does
# not fire fails the self-test rather than being reported as a curiosity.
if [ "$ONLY" = "selftest" ]; then
    ME="${BASH_SOURCE[0]}"
    T="$(mktemp -d)"
    trap 'rm -rf "$T"' EXIT
    sc_pass=0; sc_fail=0
    control() { # description | expected-exit | command…
        local desc="$1" want="$2"; shift 2
        local got=0
        "$@" >"$T/out" 2>&1 || got=$?
        if [ "$got" = "$want" ]; then
            sc_pass=$((sc_pass+1)); ok "$desc (exit $got)"
        else
            sc_fail=$((sc_fail+1)); bad "$desc — wanted exit $want, got $got"
            sed -n '1,6p' "$T/out" | sed 's/^/      /'
        fi
    }
    head2 "run-all.sh --self-test: negative controls"

    mkdir -p "$T/empty_suites" "$T/empty_ctests"
    control "1 an empty suite directory is a HARNESS ERROR, not a pass" 2 \
        env ARCA_SUITE_DIR="$T/empty_suites" bash "$ME" --only=suites --scope=deploy

    mkdir -p "$T/decoy_ctests"
    : > "$T/decoy_ctests/zz_decoy_test.c"
    control "2 an unclassified *_test.c is a HARNESS ERROR" 2 \
        env ARCA_CTEST_DIR="$T/decoy_ctests" bash "$ME" --only=ctests

    mkdir -p "$T/failing"
    printf '#!/bin/bash\necho "3 passed, 1 failed"\nexit 1\n' > "$T/failing/arca_identify_test.sh"
    # One stub per table row, read from the table itself: a hand-kept list here rotted (five rows
    # were missing) and turned controls 3 and 5 into harness errors for a reason unrelated to them.
    for row in "${SUITE_ROWS[@]}"; do
        n="${row%%|*}"
        [ "$n" = arca_identify_test.sh ] && continue
        printf '#!/bin/bash\necho "1 passed, 0 failed"\n' > "$T/failing/$n"
    done
    printf '#!/bin/bash\necho ok\n' > "$T/failing/doc_check.sh"
    control "3 a suite that exits 1 fails the gate" 1 \
        env ARCA_SUITE_DIR="$T/failing" bash "$ME" --only=suites --scope=deploy

    cp -a "$T/failing" "$T/harness"
    printf '#!/bin/bash\necho "HARNESS ERROR"\nexit 2\n' > "$T/harness/arca_identify_test.sh"
    control "4 a suite that exits 2 is a HARNESS ERROR, not a failure" 2 \
        env ARCA_SUITE_DIR="$T/harness" bash "$ME" --only=suites --scope=deploy

    cp -a "$T/failing" "$T/skipping"
    # ⚠️ The fixture COLOURS the word, exactly as commission_offline_test.sh
    # does. A plain-text "skip" here is a fixture cleaner than reality, and it
    # made this control pass while the real suite's skip was being counted as a
    # pass — measured 2026-09-08. The ESC[1;33m prefix is the whole point.
    printf '#!/bin/bash\nprintf "  \\033[1;33mskip\\033[0m  needs root\\n"\nexit 0\n' \
        > "$T/skipping/arca_identify_test.sh"
    control "5 a suite that skips passes the gate but is NOT counted as passed" 0 \
        env ARCA_SUITE_DIR="$T/skipping" bash "$ME" --only=suites --scope=deploy
    if grep -q 'skipped: arca_identify_test.sh' "$T/out"; then
        sc_pass=$((sc_pass+1)); ok "5b   …and the summary names it as skipped"
    else
        sc_fail=$((sc_fail+1)); bad "5b   the skip was absorbed into the pass count"
    fi

    # ── Phase 2 had no skip detection at all until 2026-09-08: exit 0 was an
    # unconditional pass, so a C regression that could not reach its subject
    # reported as having tested it.  These two controls are what stop that
    # coming back.  The fixture is a whole miniature native_apps tree, because
    # the rows compile relative to the subject directory's parent: one stub .c
    # per CTEST_ROWS name, and an empty file for every source any row names, so
    # every row builds and only the stubs decide the verdicts.
    # ⚠️ The skipping stub COLOURS the word, for the same reason control 5 does.
    # ⚠️ Quoted heredocs, not printf: bash printf turns the \\n inside a C string
    # literal into a REAL newline, which does not compile.  Control 10 is what
    # caught that — without it, control 8 passed on a fixture whose other 14
    # rows were all failing to build.
    mkdir -p "$T/ct/tests" "$T/ct/build"
    cat > "$T/ct_stub.c" <<'CTSTUB'
#include <stdio.h>
#undef main   /* launcher_args_test's row passes -Dmain=..., which renames this */
int main(void) { printf("1 passed, 0 failed\n"); return 0; }
CTSTUB
    for row in "${CTEST_ROWS[@]}"; do
        cname="${row%%|*}"
        cp "$T/ct_stub.c" "$T/ct/tests/$cname.c"
        for src in $(printf '%s' "$row" | cut -d'|' -f3); do
            mkdir -p "$T/ct/$(dirname "$src")"; : > "$T/ct/$src"
        done
    done
    cp -a "$T/ct" "$T/ct_skip"
    cat > "$T/ct_skip/tests/gamepad_announce_test.c" <<'CTSKIP'
#include <stdio.h>
#undef main
int main(void) { printf("  \033[1;33mskip\033[0m  no /dev/uinput\n"); return 0; }
CTSKIP
    control "8 a C regression that skips passes the gate but is NOT counted as passed" 0 \
        env ARCA_CTEST_DIR="$T/ct_skip/tests" bash "$ME" --only=ctests
    if grep -q 'skipped: gamepad_announce_test' "$T/out"; then
        sc_pass=$((sc_pass+1)); ok "8b   …and the summary names it as skipped"
    else
        sc_fail=$((sc_fail+1)); bad "8b   the skip was absorbed into the pass count"
    fi

    cp -a "$T/ct" "$T/ct_undec"
    cat > "$T/ct_undec/tests/gamepad_announce_test.c" <<'CTUNDEC'
#include <stdio.h>
#undef main
int main(void) { printf("Refusing to grade\n"); return 2; }
CTUNDEC
    control "9 a C regression that exits 2 is a HARNESS ERROR, not a failure" 2 \
        env ARCA_CTEST_DIR="$T/ct_undec/tests" bash "$ME" --only=ctests

    # And the control for the control: the same fixture with no stub replaced
    # must be plain green, or controls 8 and 9 are measuring the fixture.
    control "10 the unmodified phase-2 fixture passes" 0 \
        env ARCA_CTEST_DIR="$T/ct/tests" bash "$ME" --only=ctests

    # ⚠️ The other direction, which fired for real: three of these tests name a
    # CASE with the word "skipped", so a detector matching the word alone
    # reported three PASSING tests as skipped.  This stub passes AND says the
    # word, and must be counted as a pass.
    cp -a "$T/ct" "$T/ct_wordy"
    cat > "$T/ct_wordy/tests/config_test.c" <<'CTWORDY'
#include <stdio.h>
#undef main
int main(void) {
    printf("  ok   comments and blanks are skipped\n");
    printf("1 passed, 0 failed\n");
    return 0;
}
CTWORDY
    control "11 a PASSING test that says \"skipped\" in a case name is not a skip" 0 \
        env ARCA_CTEST_DIR="$T/ct_wordy/tests" bash "$ME" --only=ctests
    if grep -q 'skipped: config_test' "$T/out"; then
        sc_fail=$((sc_fail+1)); bad "11b   its case name was read as a skip"
    else
        sc_pass=$((sc_pass+1)); ok "11b   …and it is not named as skipped"
    fi

    mkdir -p "$T/scfiles"
    printf '#!/bin/bash\n# shellcheck disable-typo=SC2086\ntrue\n' > "$T/scfiles/broken.sh"
    : > "$T/sc_empty_baseline.txt"
    control "6 a malformed shellcheck directive fails tier 1" 1 \
        env ARCA_SC_BASELINE="$T/sc_empty_baseline.txt" ARCA_SC_FILES="$T/scfiles/broken.sh" \
            bash "$ME" --only=shellcheck

    mkdir -p "$T/scratchet"
    # A quoted heredoc, not printf with $1 in single quotes: the unquoted "$1"
    # below is the fixture that must yield two SC2086s, and writing it this
    # way means there is nothing to suppress in this file either.
    # ⚠️ Positional parameters, NOT a local like f=x.  Measured: shellcheck
    # 0.7.0 emits NOTHING for `f=x; echo $f`, because it can prove the value
    # safe — so a fixture built that way is clean and this control passes
    # vacuously.  That is how control 7 first failed to fire.
    cat > "$T/scratchet/ratchet.sh" <<'RATCHET'
#!/bin/bash
echo $1
echo $2
RATCHET
    printf '%s SC2086 1\n' "$T/scratchet/ratchet.sh" > "$T/sc_low_baseline.txt"
    control "7 a finding count above the baseline fails tier 2" 1 \
        env ARCA_SC_BASELINE="$T/sc_low_baseline.txt" ARCA_SC_FILES="$T/scratchet/ratchet.sh" \
            bash "$ME" --only=shellcheck

    printf '\n%s%d controls fired, %d did not%s\n' "$BLD" "$sc_pass" "$sc_fail" "$RST"
    [ "$sc_fail" -eq 0 ] && { printf '%srun-all.sh --self-test: PASS%s\n' "$GRN" "$RST"; exit 0; }
    printf '%srun-all.sh --self-test: FAIL%s\n' "$RED" "$RST"; exit 1
fi

# ── preflight ──────────────────────────────────────────────────────────────
LOGDIR="$(mktemp -d)"
trap 'rm -rf "$LOGDIR"' EXIT

harness() { harness_n=$((harness_n+1)); HARNESS_LIST+=("$1"); bad "HARNESS ERROR: $1"; }

# ── PHASE 1: the shell suites ──────────────────────────────────────────────
phase_suites() {
    head2 "PHASE 1  shell suites  (scope: $SCOPE)"

    # A suite present on disk and absent from the table would be silently
    # dropped, which is the failure mode a gate must never have.
    local on_disk d
    on_disk="$(cd "$SUITE_DIR" 2>/dev/null && for f in ./*_test.sh; do [ -e "$f" ] && printf '%s\n' "${f#./}"; done | sort)"
    if [ -z "$on_disk" ]; then
        harness "no *_test.sh found in $SUITE_DIR — the subject list is empty"
        return
    fi
    for d in $on_disk; do
        case " ${SUITE_ROWS[*]} " in
            *"$d|"*) : ;;
            *) harness "$d is on disk but in no table row — classify it (deploy or docs)" ;;
        esac
    done

    local row name scope path rc t0 t1 log plain verdict
    for row in "${SUITE_ROWS[@]}"; do
        name="${row%%|*}"; scope="${row#*|}"
        in_scope "$scope" || continue
        path="$SUITE_DIR/$name"
        if [ ! -f "$path" ]; then
            harness "$name is in the table but not on disk at $path"
            continue
        fi
        log="$LOGDIR/$name.log"
        t0=$(date +%s)
        rc=0
        # stdin from /dev/null: arca_provision_test's ssh stub runs `cat` on fd 0
        # on purpose, and under a caller whose stdin never reaches EOF (a tool
        # or background shell) it blocked until the 420 s cap -- measured.
        timeout 420 bash "$path" </dev/null >"$log" 2>&1 || rc=$?
        t1=$(date +%s)
        # ⚠️ Match on a DECOLOURISED copy, never on the raw log. The suites
        # colour their own words, so commission_offline_test.sh emits
        # ESC[1;33mskip ESC[0m — the character before "skip" is `m`, so a
        # pattern like (^|[^a-z])skip cannot match it. Measured 2026-09-08:
        # that is exactly how this gate first reported a skipped suite as a
        # PASS, while a self-test control using a plain-text fixture said the
        # detection worked. A fixture cleaner than reality proves nothing.
        plain="$LOGDIR/$name.plain"
        sed 's/\x1b\[[0-9;]*m//g' "$log" | tr -d '\000' > "$plain"
        verdict="$(grep -aoE '[0-9]+ passed, [0-9]+ failed' "$plain" | tail -1)"
        case "$rc" in
        0)  # exit 0 with a skip banner is NOT a pass — see the header.
            if [ -z "$verdict" ] && grep -qiE '(^|[^[:alnum:]])skip' "$plain"; then
                skip_n=$((skip_n+1)); SKIPPED_LIST+=("$name")
                skipmsg "$(printf '%-32s %4ss  SKIPPED — %s' "$name" "$((t1-t0))" \
                    "$(grep -iE '(^|[^[:alnum:]])skip' "$plain" | head -1 | cut -c1-64)")"
            else
                pass_n=$((pass_n+1))
                ok "$(printf '%-32s %4ss  %s' "$name" "$((t1-t0))" "$verdict")"
            fi ;;
        2)  harness "$name exited 2 — it could not judge; see its own output"
            sed -n '$p' "$log" | tr -d '\000' | sed 's/^/    /' ;;
        124) fail_n=$((fail_n+1)); FAILED_LIST+=("$name")
            bad "$(printf '%-32s %4ss  TIMED OUT at 420 s — a hang is a result' "$name" "$((t1-t0))")" ;;
        *)  fail_n=$((fail_n+1)); FAILED_LIST+=("$name")
            bad "$(printf '%-32s %4ss  exit %s  %s' "$name" "$((t1-t0))" "$rc" "$verdict")"
            grep -aE '(FAIL|✗)' "$log" | tr -d '\000' | head -6 | sed 's/^/    /' ;;
        esac
    done
}

# ── PHASE 2: the host C regressions ────────────────────────────────────────
phase_ctests() {
    head2 "PHASE 2  host C regressions  (host gcc, no device)"

    command -v gcc >/dev/null 2>&1 || { harness "gcc is absent — this must run in WSL, not Git Bash"; return; }

    local on_disk d stem
    on_disk="$(cd "$CTEST_DIR" 2>/dev/null && for f in ./*_test.c; do [ -e "$f" ] && printf '%s\n' "${f#./}"; done | sed 's|\.c$||' | sort)"
    if [ -z "$on_disk" ]; then
        harness "no *_test.c found in $CTEST_DIR — the subject list is empty"
        return
    fi
    for stem in $on_disk; do
        case " ${CTEST_ROWS[*]} ${CTEST_NOT_HOST[*]} " in
            *"$stem|"*|*" $stem "*) : ;;
            *) harness "$stem.c is in neither table — a host regression, an ARM app, or a device diagnostic?" ;;
        esac
    done

    # Nothing below runs if the tables and the tree disagree: a wrong subject
    # list makes every number after it meaningless.
    [ "$harness_n" -eq 0 ] || return

    # The tree the rows compile in is the PARENT of the subject directory, so
    # that ARCA_CTEST_DIR redirects the build as well as the scan.  Without this
    # the self-test can point phase 2 at a fixture and still compile the real
    # sources, which is how a control for this phase would pass while measuring
    # the repo instead of the fixture.
    local na
    na="$(dirname "$CTEST_DIR")"
    mkdir -p "$na/build"
    local row name cflags srcs rc t0 t1 log
    for row in "${CTEST_ROWS[@]}"; do
        name="${row%%|*}"
        cflags="$(printf '%s' "$row" | cut -d'|' -f2)"
        srcs="$(printf '%s' "$row" | cut -d'|' -f3)"
        log="$LOGDIR/$name.log"
        t0=$(date +%s); rc=0
        # shellcheck disable=SC2086  # cflags and srcs are word lists by design
        ( cd "$na" && gcc -Wall -Wextra -Wno-unused-parameter $cflags \
              -o "build/$name" "tests/$name.c" $srcs -lm ) >"$log" 2>&1 || rc=$?
        if [ "$rc" -ne 0 ]; then
            t1=$(date +%s); fail_n=$((fail_n+1)); FAILED_LIST+=("$name (build)")
            bad "$(printf '%-32s %4ss  BUILD FAILED' "$name" "$((t1-t0))")"
            grep -E 'error:|fatal error' "$log" | head -4 | sed 's/^/    /'
            continue
        fi
        rc=0
        run="$LOGDIR/$name.run"
        ( cd "$na" && timeout 180 "./build/$name" ) >"$run" 2>&1 || rc=$?
        cat "$run" >>"$log"
        t1=$(date +%s)
        # A SKIP IS NOT A PASS here either.  Phase 1 has had this since a
        # skipping suite was counted green; phase 2 did not, and exit 0 was an
        # unconditional pass — so a C regression that cannot reach its subject
        # (gamepad_announce_test needs /dev/uinput, which is root-only on this
        # host) would have been reported as having tested it.  Two details that
        # are easy to get wrong: decolourise first, because the tests colour
        # their own words and the byte before "skip" is then `m`, which a
        # pattern anchored on a non-alphanumeric cannot match; and read the RUN
        # output only, not $log, which also holds the compiler's — a warning
        # quoting a source comment must not be able to mark a test skipped.
        # ⚠️ The pattern is ANCHORED, and that is the whole design: a phase-2
        # test signals a skip by printing `skip` as the FIRST token on a line,
        # the way its banner does.  Matching the word anywhere reported three
        # PASSING tests as skipped — they name cases "a middle gap is skipped",
        # "the RIFF pad byte is skipped", "comments and blanks are skipped" —
        # which understates coverage exactly as badly as absorbing a real skip
        # overstates it.  Nor can a verdict line be used as the guard the way
        # phase 1 does: these tests print four different shapes
        # (`PASSED (0 failures)`, `PASSED 24 check(s), 0 failure(s)`,
        # `63 checks, 0 failures`) and none is the `N passed, M failed` phase 1
        # greps for, so that test is vacuously true here.
        plain="$LOGDIR/$name.plain"
        sed 's/\x1b\[[0-9;]*m//g' "$run" | tr -d '\000' > "$plain"
        case "$rc" in
        0)   if grep -qiE '^[[:space:]]*skip([^[:alnum:]]|$)' "$plain"; then
                 skip_n=$((skip_n+1)); SKIPPED_LIST+=("$name")
                 skipmsg "$(printf '%-32s %4ss  SKIPPED — %s' "$name" "$((t1-t0))" \
                     "$(grep -iE '^[[:space:]]*skip([^[:alnum:]]|$)' "$plain" | head -1 | cut -c1-64)")"
             else
                 pass_n=$((pass_n+1)); ok "$(printf '%-32s %4ss' "$name" "$((t1-t0))")"
             fi ;;
        2)   harness "$name exited 2 — it could not judge; see its own output"
             grep -aE '(Refusing|refuse|could not|NOTHING)' "$plain" | head -3 | sed 's/^/    /' ;;
        124) fail_n=$((fail_n+1)); FAILED_LIST+=("$name")
             bad "$(printf '%-32s %4ss  TIMED OUT at 180 s — a hang is a result' "$name" "$((t1-t0))")" ;;
        *)   fail_n=$((fail_n+1)); FAILED_LIST+=("$name")
             bad "$(printf '%-32s %4ss  exit %s' "$name" "$((t1-t0))" "$rc")"
             grep -aE '(FAIL|✗)' "$log" | head -6 | sed 's/^/    /' ;;
        esac
    done
}

# ── PHASE 2b: ARM apps with no host regression, syntax-checked ─────────────
# The ARM build is the only thing that links control_panel, so a compile error
# in it passed every phase above.  This compiles each of its sources with
# -fsyntax-only on the host gcc.  The source list is NOT written here: it is
# read from the `control_panel` step of native_apps/build-and-deploy.sh, the
# one place that names them, so a page added there is covered here.  Warnings
# do not fail it, matching the ARM build (WARN=-Wall -Wextra, no -Werror); the
# count is printed so a rise is visible.  ARCA_APPS_DIR redirects the tree, for
# the negative control.
phase_apps() {
    head2 "PHASE 2b  control_panel syntax check  (host gcc, no device)"
    command -v gcc >/dev/null 2>&1 || { harness "gcc is absent — this must run in WSL, not Git Bash"; return; }
    local na="${ARCA_APPS_DIR:-$REPO_ROOT/native_apps}" script srcs s rc=0 t0 t1 log
    script="$na/build-and-deploy.sh"
    [ -f "$script" ] || { harness "$script is missing — the source list has no home"; return; }
    srcs="$(awk '/^step "[0-9]+\/[0-9]+" "control_panel"/ {f=1; next} f && /\$CC/ {print; exit}' "$script" \
            | tr ' ' '\n' | grep -E '^control_panel/[A-Za-z0-9_]+\.c$')"
    if [ -z "$srcs" ]; then
        harness "no control_panel/*.c found on the control_panel step of $script"
        return
    fi
    log="$LOGDIR/control_panel.log"; : > "$log"
    t0=$(date +%s)
    for s in $srcs; do
        [ -f "$na/$s" ] || { harness "$s is named by the build script and absent on disk"; return; }
        ( cd "$na" && gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -I. "$s" ) >>"$log" 2>&1 || rc=1
    done
    t1=$(date +%s)
    local n_src nw
    n_src="$(printf '%s\n' "$srcs" | wc -l | tr -d ' ')"
    nw="$(grep -c 'warning:' "$log" || true)"
    if [ "$rc" -eq 0 ]; then
        pass_n=$((pass_n+1)); ok "$(printf '%-32s %4ss  %s sources, %s warning(s)' control_panel "$((t1-t0))" "$n_src" "$nw")"
    else
        fail_n=$((fail_n+1)); FAILED_LIST+=("control_panel (syntax)")
        bad "$(printf '%-32s %4ss  COMPILE FAILED' control_panel "$((t1-t0))")"
        grep -E 'error:|fatal error' "$log" | head -4 | sed 's/^/    /'
    fi
}

# ── PHASE 3: shellcheck ────────────────────────────────────────────────────
# Two tiers, because the repo carries a backlog of pre-existing findings that are
# not this gate's to fix, and a gate that demanded zero would simply be disabled.
# ⚠️ The size of that backlog is NOT written here: tests/shellcheck-baseline.txt is
# its one home, and a copy in prose goes stale silently.  It read "319" while the
# baseline summed to 316 — measure with awk '{s+=$3} END{print s, NR}' on the file.
#
#   TIER 1, a hard zero:  every finding at error: severity, plus every SC11xx
#     code.  SC11xx is the family that means shellcheck could not do its job
#     — and one of them, SC1124, is measured to make the tool exit 1 having
#     analysed NOTHING in that file, which reads exactly like a clean run
#     that found one small problem.  A voided analysis must never be a pass.
#
#   TIER 2, a ratchet:  tests/shellcheck-baseline.txt records one row per
#     (file, code) with its current count.  A count going UP, or a new
#     (file, code) pair, FAILS.  A count going down passes and says so.  The
#     backlog stays visible as open work instead of being suppressed with
#     disable= directives in shipped source, which is also how this gate
#     avoids adding SC1124 risk of its own.
phase_shellcheck() {
    head2 "PHASE 3  shellcheck"

    command -v shellcheck >/dev/null 2>&1 || { harness "shellcheck is absent — it is in setup-build-env.sh's default group set"; return; }
    [ -f "$SC_BASELINE" ] || { harness "baseline missing: $SC_BASELINE"; return; }

    local files
    if [ -n "${ARCA_SC_FILES:-}" ]; then
        files="$ARCA_SC_FILES"
    else
        files="$(cd "$REPO_ROOT" && git ls-files -- '*.sh')"
    fi
    if [ -z "$files" ]; then
        harness "no shell scripts to check — git ls-files returned nothing"
        return
    fi
    note "$(printf '%s scripts, baseline %s rows' "$(printf '%s\n' "$files" | wc -l | tr -d ' ')" "$(grep -c '[^[:space:]]' "$SC_BASELINE" | tr -d ' ')")"

    local raw="$LOGDIR/sc.txt"
    # shellcheck disable=SC2086  # $files is a newline list of paths, no spaces
    ( cd "$REPO_ROOT" && shellcheck -f gcc $files ) >"$raw" 2>&1

    # TIER 1
    local t1_err t1_11
    t1_err="$(grep -c ' error: ' "$raw" || true)"
    t1_11="$(grep -cE '\[SC11[0-9]{2}\]' "$raw" || true)"
    if [ "$t1_err" -eq 0 ] && [ "$t1_11" -eq 0 ]; then
        pass_n=$((pass_n+1)); ok "tier 1  hard zero: no error-severity finding, no SC11xx"
    else
        fail_n=$((fail_n+1)); FAILED_LIST+=("shellcheck tier 1")
        bad "tier 1  $t1_err error-severity finding(s), $t1_11 SC11xx — an SC11xx VOIDS that file's analysis"
        grep -E ' error: |\[SC11[0-9]{2}\]' "$raw" | head -8 | sed 's/^/    /'
    fi

    # TIER 2
    local cur="$LOGDIR/sc-tally.txt" regress=0
    sed -E 's/^([^:]+):[0-9]+:[0-9]+: [a-z]+: .*\[(SC[0-9]+)\]$/\1 \2/' "$raw" \
        | grep -E '^[^ ]+ SC[0-9]+$' | sort | uniq -c \
        | awk '{print $2" "$3" "$1}' | sort > "$cur"
    local f c n base
    while read -r f c n; do
        [ -z "${f:-}" ] && continue
        base="$(awk -v f="$f" -v c="$c" '$1==f && $2==c {print $3; exit}' "$SC_BASELINE")"
        base="${base:-0}"
        if [ "$n" -gt "$base" ]; then
            regress=$((regress+1))
            note "$RED+$RST $f $c: $base in the baseline, $n now"
        fi
    done < "$cur"
    local improved
    improved="$(awk 'NR==FNR{now[$1" "$2]=$3; next} {b=$3; k=$1" "$2; nn=(k in now)?now[k]:0; if (nn<b) c++} END {print c+0}' "$cur" "$SC_BASELINE")"
    if [ "$regress" -eq 0 ]; then
        pass_n=$((pass_n+1))
        ok "tier 2  ratchet: no (file, code) count above the baseline"
        [ "$improved" -gt 0 ] && note "$improved baseline row(s) now lower — refresh with: bash tests/run-all.sh --only=shellcheck-baseline"
    else
        fail_n=$((fail_n+1)); FAILED_LIST+=("shellcheck tier 2")
        bad "tier 2  ratchet: $regress (file, code) pair(s) above the baseline — see the + rows above"
        note "To see them: cd $REPO_ROOT && shellcheck -f gcc \$(git ls-files -- '*.sh')"
    fi
}

# Regenerate the baseline from the current tree.  Deliberately a separate
# mode: a gate that refreshes its own baseline as a side effect can never
# fail, which is the one thing a ratchet must be able to do.
phase_baseline() {
    command -v shellcheck >/dev/null 2>&1 || { harness "shellcheck is absent"; return; }
    local raw="$LOGDIR/sc.txt" files
    files="$(cd "$REPO_ROOT" && git ls-files -- '*.sh')"
    [ -n "$files" ] || { harness "git ls-files returned no scripts"; return; }
    # shellcheck disable=SC2086
    ( cd "$REPO_ROOT" && shellcheck -f gcc $files ) >"$raw" 2>&1
    sed -E 's/^([^:]+):[0-9]+:[0-9]+: [a-z]+: .*\[(SC[0-9]+)\]$/\1 \2/' "$raw" \
        | grep -E '^[^ ]+ SC[0-9]+$' | sort | uniq -c \
        | awk '{print $2" "$3" "$1}' | sort > "$SC_BASELINE.new"
    if [ ! -s "$SC_BASELINE.new" ]; then
        harness "the regenerated baseline is empty — refusing to write it"
        rm -f "$SC_BASELINE.new"
        return
    fi
    mv "$SC_BASELINE.new" "$SC_BASELINE"
    ok "baseline rewritten: $(grep -c '[^[:space:]]' "$SC_BASELINE" | tr -d ' ') rows"
    pass_n=$((pass_n+1))
}

# ── main ───────────────────────────────────────────────────────────────────
printf '%s%sArca host gate%s  —  scope %s, repo %s\n' "$BLD" "$CYA" "$RST" "$SCOPE" "$REPO_ROOT"

case "$ONLY" in
    "")                  phase_suites; phase_ctests; phase_apps; phase_shellcheck ;;
    suites)              phase_suites ;;
    ctests)              phase_ctests ;;
    apps)                phase_apps ;;
    shellcheck)          phase_shellcheck ;;
    shellcheck-baseline) phase_baseline ;;
    *) printf 'run-all.sh: --only must be suites, ctests, apps, shellcheck or shellcheck-baseline\n' >&2; exit 2 ;;
esac

head2 "SUMMARY"
printf '  %s%d passed%s, %s%d failed%s, %s%d skipped%s, %s%d harness error%s\n' \
    "$GRN" "$pass_n" "$RST" "$RED" "$fail_n" "$RST" \
    "$YEL" "$skip_n" "$RST" "$YEL" "$harness_n" "$RST"

if [ "$skip_n" -gt 0 ]; then
    printf '  %sskipped:%s %s\n' "$YEL" "$RST" "${SKIPPED_LIST[*]}"
    # ⚠️ NOT "re-run this as root". Measured 2026-09-08: as root, one suite exits 2,
    # a later phase then returns without printing a
    # row, and phase 3 dies because git calls the repo dubiously owned — 11
    # passed, 2 harness errors, exit 2. Root reaches FEWER subjects, so point at
    # the one skipped subject instead of at the whole gate.
    [ "$(id -u)" -ne 0 ] && note "each skipped subject names what it needs; run THAT one as root, not this gate"
fi
if [ "$harness_n" -gt 0 ]; then
    printf '  %sHARNESS ERROR%s — the gate could not judge. Nothing here is a verdict.\n' "$YEL" "$RST"
    exit 2
fi
if [ "$fail_n" -gt 0 ]; then
    printf '  %sfailed:%s %s\n' "$RED" "$RST" "${FAILED_LIST[*]}"
    printf '%shost gate: FAIL — do not deploy%s\n' "$RED" "$RST"
    exit 1
fi
if [ "$pass_n" -eq 0 ]; then
    printf '  %sHARNESS ERROR%s — nothing ran, and nothing running is not a pass.\n' "$YEL" "$RST"
    exit 2
fi
printf '%shost gate: PASS%s\n' "$GRN" "$RST"
exit 0
