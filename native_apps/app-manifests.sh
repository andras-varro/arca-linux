#!/bin/bash
#
# app-manifests.sh — the ONE generator for native_apps' .app launcher manifests.
#
# SOURCED, not executed:   . "$SCRIPT_DIR/app-manifests.sh"
#
# ── Why this is its own file ─────────────────────────────────────────────────
#
# The nine manifests used to be nine `cat > … << APP` heredocs inside an
# `ssh "$DEVICE" bash <<'REMOTE'` block in build-and-deploy.sh, i.e. they only
# existed as a side effect of having a reachable device.  The offline installer
# (../commissioning/commission-offline.sh) has no device and must write
# byte-identical manifests, so a second copy of that heredoc was the obvious
# move and the wrong
# one: `exec=` paths and `args=` values would drift between the two, and a
# manifest whose `exec=` names a binary that is not there renders a launcher tile
# that does nothing when tapped.
#
# So: the manifests are DATA here, written locally by rw_write_app_manifests, and
# both the deploy path and `--bundle` copy those same files.
#
# ── Format ──────────────────────────────────────────────────────────────────
#
# INI, read by app_launcher's manifest scanner (SYSTEM_ANALYSIS.md#53-app-launcher-and-manifests):
#
#     name=Snake
#     exec=/opt/games/snake
#     icon=/opt/roomwizard/icons/snake.ppm
#     args=fb,touch
#
# `args` is one of fb,touch / fb / touch / none — or empty, which control_panel
# uses to say "take no argument at all".
#
# One record per line below: basename|name|exec|icon|args
# The separator is `|` because a display name legitimately contains a space
# ("Brick Breaker", "Office Runner") and must not be split on it.

RW_APP_MANIFESTS='
snake|Snake|/opt/games/snake|/opt/roomwizard/icons/snake.ppm|fb,touch
tetris|Tetris|/opt/games/tetris|/opt/roomwizard/icons/tetris.ppm|fb,touch
pong|Pong|/opt/games/pong|/opt/roomwizard/icons/pong.ppm|fb,touch
brick_breaker|Brick Breaker|/opt/games/brick_breaker|/opt/roomwizard/icons/brick_breaker.ppm|fb,touch
samegame|SameGame|/opt/games/samegame|/opt/roomwizard/icons/samegame.ppm|fb,touch
frogger|Frogger|/opt/games/frogger|/opt/roomwizard/icons/frogger.ppm|fb,touch
platformer|Office Runner|/opt/games/platformer|/opt/roomwizard/icons/platformer.ppm|fb,touch
theremin|Tap-a-Theremin|/opt/games/theremin|/opt/roomwizard/icons/theremin.ppm|fb,touch
terminal|Terminal|/opt/games/terminal|/opt/roomwizard/icons/terminal.ppm|fb,touch
control_panel|Control Panel|/opt/games/control_panel|/opt/roomwizard/icons/control_panel.ppm|
'

# Manifests this component used to install and no longer does.  Swept on deploy
# so a device that saw an older build does not keep a tile for a tool that was
# folded into control_panel's tabs, or for an app that was renamed — the launcher
# would render it and the tap would exec a binary that is gone.
# device_tools is the old name of control_panel (binary, manifest, icon).
# audio_mix_test lost its tile, not its binary: MIX BUS TEST on control_panel's
# Audio page launches it.
RW_APP_MANIFESTS_RETIRED='hardware_test hardware_config calibrate usb_test hardware_diag audio_touch_test device_tools audio_mix_test'

# ---------------------------------------------------------------------------
# rw_write_app_manifests DIR
#
# Write every manifest above into DIR as <basename>.app.  DIR is created.
# Echoes nothing on success; the caller reports.  Returns non-zero if a manifest
# the table names did not appear — which is also the only way this can report at
# all, for the reason spelled out at sound-sets.sh's rw_write_sound_sets.  It
# counts the TABLE's files, not `*.app`: build/ is never cleaned, so a retired
# manifest left behind by an older checkout must not read as a failure here.
#
# Mode is NOT set here.  /mnt/c is DrvFs and discards chmod (CLAUDE.md), so a
# mode set on this host is unobservable and unreliable; 0644 for these is
# DECLARED in the bundle manifest and applied by whoever installs them.
# ---------------------------------------------------------------------------
rw_write_app_manifests() {
    local dir="$1" line base name exec icon args
    [ -n "$dir" ] || { echo "rw_write_app_manifests: no directory given" >&2; return 1; }
    mkdir -p "$dir" || return 1

    printf '%s\n' "$RW_APP_MANIFESTS" | while IFS='|' read -r base name exec icon args; do
        [ -n "$base" ] || continue
        {
            printf 'name=%s\n' "$name"
            printf 'exec=%s\n' "$exec"
            printf 'icon=%s\n' "$icon"
            printf 'args=%s\n' "$args"
        } > "$dir/$base.app"
    done

    local base_wanted want=0 got=0
    for base_wanted in $(rw_app_manifest_names); do
        want=$((want + 1))
        [ -f "$dir/$base_wanted.app" ] && got=$((got + 1))
    done
    if [ "$want" -ne "$got" ]; then
        echo "rw_write_app_manifests: wrote $got manifest(s) into $dir, expected $want" >&2
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# rw_app_manifest_names
#
# Echo each manifest's basename, one per line.  Used to build the bundle's
# declared-mode manifest and to cross-check `exec=` targets at install time.
# ---------------------------------------------------------------------------
rw_app_manifest_names() {
    printf '%s\n' "$RW_APP_MANIFESTS" | while IFS='|' read -r base _rest; do
        [ -n "$base" ] && echo "$base"
    done
}
