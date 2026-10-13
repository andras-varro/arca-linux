# native_apps — authoring guide

Guidance for writing code in this directory. Device facts live in
[`../SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md); open bugs and planned work live in
[`../IMPROVEMENT_PLAN.md`](../IMPROVEMENT_PLAN.md); what each app *is* lives in
[`README.md`](README.md). This file is only about **how to write correct code here**.

`native_apps/` = a shared C library (`common/`) + games + on-device tools + the launcher.

## Build

```bash
./build-and-deploy.sh                          # build only
./build-and-deploy.sh <ip>                     # build + deploy
./build-and-deploy.sh <ip> set-default         # + make it the boot app
```

`set-default` is the only mode this script accepts. Cleanup and boot-service install live in
`../commissioning/provision.sh`.

All targets compile with `-Wall -Wextra -Wno-unused-parameter` and the tree is at **zero warnings** —
keep it there, so a new warning means a new problem. Not `-Werror`, so a warning will not block a
deploy; that is your job. Every build then runs `./check-arm-safe.sh` (root `../CLAUDE.md` for why);
it also takes one binary: `./check-arm-safe.sh <path>`.

Every app links `$COMMON_OBJ` = `framebuffer.o touch_input.o hardware.o common.o highscore.o
keyboard.o audio.o audio_gen.o audio_out.o audio_wav.o config.o`; games add `gamepad.o input_scan.o input_slots.o` (`GAMEPAD_OBJ`; **every binary naming `COMMON_OBJ` must also name `GAMEPAD_OBJ`**, because `common.o` / `keyboard.o` call into it); some add `ui_layout.o ppm.o
logger.o`; `control_panel` also links `GAMEPAD_OBJ` and `ui_focus.o`; the two tools that measure the touch mapping (`control_panel`, `touch_raw`) add
`$CALIB_OBJ` = `touch_calib.o`. Add new objects to `build-and-deploy.sh`. `audio_gen.o` is not
optional — `audio.c` calls into it for every frame count, byte count, envelope and write.

**A new *binary* goes in `GAMES_BINARIES` and nowhere else** (root `../CLAUDE.md` for what that array drives). Do not add a second list.

**The deploy verifies itself.** After `chmod`, every executable is md5-compared against `build/` and a
mismatch is fatal with a per-file diff — a truncated scp, a full filesystem and a surviving process
holding an old inode otherwise all look like a clean deploy. `./build-and-deploy.sh <ip>` validates the
IP and the mode *before* compiling anything, and `cd`s to its own directory, so it can be invoked by path.

## The common library

| Module | Use it for | Never do this instead |
|---|---|---|
| `framebuffer.c` | double-buffered draw, `fb_swap_rect()` (band-only present, byte-identical to `fb_swap()` inside the rect; `tests/fb_rotate_test.c`), sprite blit, the `SCREEN_VISIBLE_*` / `SCREEN_SAFE_*` macros | writing `/dev/fb0` yourself |
| `touch_input.c` | touch events, the raw→panel→logical map, publishing the touch inset | reading evdev directly |
| `touch_calib.c` | measuring that map: targets, fit, verdict, edge sweep, reach→inset, sanity gate, backup | a second copy of the fit or the sweep |
| `gamepad.c`, `pointer.c` | **all** input: touch + USB keyboard/mouse + up to 4 pads → abstract buttons; `gamepad_poll()` is any-device, `gamepad_player(gm, slot)` one P1..P4 slot (each pad, and each keyboard with all its nodes, keyed by `input_slots.c`; a mouse node that also has a full keyboard (BT keyboard+touchpad) owns a slot as a keyboard, `mouse_is_keyboard` in `gamepad.c`; other mice and touch are in no slot) (`InputState.mouse_dx/dy`; a click shorter than one poll is kept). `pointer.c` is the mouse pointer of the launcher and the Control Panel (`pointer_init/update/invalidate/paint`; pure `pointer_model_init/step`, `pointer_route_touch`; `pointer_test.c`): a 12x19 arrow over a saved copy, only the old and new rects go out via `fb_swap_rect`, so a still mouse paints nothing (launcher idle 4 ticks / 30 s, 0.1 %, same as without it). The left click is written into the app's `TouchState`, so it acts on release over the same target everywhere; it shows on motion, hides on touch or a nav key, starts hidden at the `SCREEN_SAFE` centre, a click hides the focus ring, a finger beats the mouse; wheel/right/middle unused; ⚠️ **after a full-screen run or a child app, drain the pointer together with the touch/keys (`pointer_drain`, `common/pointer.c`)**, or the click that started the run replays | per-app evdev scanning; a per-app cursor |
| `input_scan.c`, `input_slots.c` | the evdev scan: classify, open every node of a kind, skip held nodes, rescan by calling again (`gamepad.c`, `control_panel`, `vnc_client` and ScummVM all call it). `input_slots.c` (pure, `input_slots_test.c`; called by `gamepad.c`) is the P1..P4 table: a device keeps its slot across a disconnect, by the identity rule in its header | a fourth copy of the classifier or scan loop |
| `hardware.c` | LEDs, backlight, non-blocking `LedPulse` | writing `/sys/class/leds/*`, or a `usleep()` LED loop |
| `common.c` | buttons (`button_rect()` / `toggle_hit_rect()` are the one hit-box arithmetic: draw, hit-test and focus all derive from them), `ModalDialog` (focus index `modal_dialog_set_focus()` / `modal_dialog_focus_step()`: the active button is always highlighted and all four arrows step prev/next with wrap; default -1 leaves games' pause dialogs unchanged), `GameOverScreen`, safe-area screens, `acquire_instance_lock()`; `Button`/`ToggleSwitch` carry `disabled` and a disabled one refuses all input itself (touch, tap, press, keyboard hit-test) and draws grey, so set `.disabled` from state | hand-rolled widgets; per-use guards, hand-rolled greying or early returns in a page |
| `ui_layout.c`, `ui_flow.c` | grid/list layout; `ui_flow_place` wraps buttons into rows, `ui_label_fits` is the ink-width fit rule (used by the Input and Display pages; host test `tests/ui_flow_test.c`) | manual pixel arithmetic |
| `icon_grid.c` | the paged icon-tile grid (layout + receipt, tile, paging, hit-test, 96 px icon load) — the launcher's, and the home screen of any tile menu; `icon_grid_nav()` / `icon_grid_nav_exit()` are the one keyboard/pad navigation for both (reading order across pages, Left/Right wrap last<->first, Up from a top row reaches the exit X, which is `top_bar.c`'s; `icon_grid_layout()` takes the bar's `TOP_BAR_GRID_RESERVE` as `top_reserve`); **`IconGridFocus`** (`icon_grid_focus_init/_dir/_frame/_land`, pure, `icon_grid_focus_test.c`) is the one ring-visibility/selection state of both homes: a touch press hides the ring and keeps the selection, the first nav key reveals without moving, one direction per frame (U>D>L>R), a touch page flip re-anchors so Enter never acts on an unseen tile, Enter with the ring hidden only reveals, returning to home lands on the tile used. Both homes act on touch **release** over the same tile; Esc/Backspace/pad Select-Start on home = leave (launcher power dialog, Control Panel quits); ring only, no tile highlight, no key-hint line; the mouse acts through `pointer.c` | a second tile grid or a per-app `focus_shown`; the launcher's frame was md5-identical before and after the extraction |
| `top_bar.c` | the one bar across the top of the launcher and every Control Panel page: five sections (back `<`, message tap target, centred title, CPU % + clock, exit X; each optional). It starts at `SCREEN_VISIBLE_TOP` and **deliberately reaches into the untouchable band**: `top_bar_bottom()` = `max(VISIBLE_TOP+44, SAFE_TOP+40)`, so 40 px of every target is pressable at any inset. Pure geometry (`top_bar_compute`, `top_bar_hit`; host test `tests/top_bar_test.c`), the 1/s info refresh (`top_bar_info_due/_sample`) and `top_bar_draw`; the focus ring (`top_bar_ring_rect`) is the caller's. Re-run `top_bar_layout()` after `fb_init()` + `touch_init()` and after any `fb_set_bezel()`. Games do not carry it. The message comes from `status_line.c` (NOT CALIBRATED, whose tap opens `control_panel input`). `top_bar_info_sample` calls `tzset()` before each `localtime_r()` because a long-running process caches the zone at its first call (measured on `.188`: without it the launcher stayed on the old zone after `/etc/localtime` was swapped; host test `tests/top_bar_test.c`) | a hand-drawn BACK/exit button or title strip, or bezel arithmetic for the top edge |
| `ui_focus.c` | pure (no framebuffer, no widget type): spatial focus `ui_focus_move()` (nearest centre in the pressed direction, wraps at the edges), the synthetic tap `UiTap` (press frame then release frame at the focused rect's centre) and `ui_hold_progress()` hold timing, `ui_chord_button()`/`ui_hold_either()` (the mouse tester's two-button hold); linked by `control_panel` and `vnc_client` (the pad hold-Select and Settings focus); tests `ui_focus_test.c`, `icon_grid_nav_test.c` | a per-page focus walk, or a page that reads keys to activate its own widgets |
| `start_menu.c` | the arcade start menu and attract cycle (MENU, DEMO, SCORES); per-game entries; the pure core is `start_menu_step` (`tests/start_menu_test.c`) | a loop that skips OR-ing `start_menu_needs_redraw()` into its redraw test: the marker freezes |
| `audio.c` | beeps, tones, streaming, the per-frame mix pump | opening `/dev/dsp` yourself |
| `audio_gen.c` | the audio logic with no device in it: frame/byte arithmetic, the tone envelope, the one gliding oscillator, the mix bus, mono→interleaved, the frame-aligned write loop | a second sine loop, a `frames * 4` with the channel count spelled into the constant, or an audio thread |
| `audio_wav.c` | the one streaming RIFF reader: chunk walk, `(L+R)/2` downmix, the `AudioVoiceFill` adapter | assuming a 44-byte header, or loading a whole file to play it |
| `config.c` | `/opt/games/rw_config.conf`, and a game's sound set | ad-hoc config files |
| `keyboard.c` | on-screen keyboard (ALPHA / ALPHANUM / FULL / NUMERIC) | — |
| `highscore.c`, `ppm.c`, `logger.c` | scores, icons, logging | — |

`keyboard_enter(fb, touch, gm, "Title", buf, max_len, KB_LAYOUT_ALPHA)` — `buf` must hold `max_len + 1` bytes. Pad / keyboard
drive a focus frame (D-pad or arrows move, JUMP / ACTION press, BACK deletes, PAUSE = OK); touch hides it, the next key shows it.

**`fb_draw_text()` does not interpret `'\n'`.** A newline takes the unprintable-character branch and just
advances 6·scale px, so a multi-line string renders as one long line. Anything with embedded newlines must
be split per line by the caller — `sm_draw_block()` in `start_menu.c` does this.

The game draws nothing of its own on the start menu (`common/start_menu.h`): `start_menu_draw()` lays out and draws
the entries, `start_menu_draw_scores_page(fb, title, table)` the one scores page (attract and game over share it), and
`start_menu_set_warning()` adds the amber "needs a controller and none is connected" block (`'\n'`-split by `sm_draw_block`,
the one splitter). PAUSE (Esc / pad Start) exits like BACK; `start_menu_reopen(m, now)` returns to the menu from a game.
In every game, pause EXIT / BACK / an on-screen exit return to the start menu; only the menu's EXIT leaves for the launcher.

`ModalDialog`: `modal_dialog_init()` → `modal_dialog_set_button()` per button → `modal_dialog_draw()` after
all other content but before `fb_swap()` → `modal_dialog_update()` returns `MODAL_ACTION_BTN0..BTN3` or
`-1`. Two buttons render side-by-side; 1, 3 or 4 stack. While a dialog is active, route **all** input to it
and skip every other handler. **Pad and keyboard enter through `modal_dialog_input(dlg, &in, cancel)`**
(before `modal_dialog_update()`, after `modal_dialog_set_focus(dlg, 0)` on open; test `tests/modal_dialog_input_test.c`).

## App lifecycle

```c
int main(int argc, char *argv[]) {
    /* 0. Line-buffer stdout — at boot it is a LOG FILE, not a tty */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 1. Parse args (fb_device, touch_device) */
    /* 2. Singleton guard */
    int lock_fd = acquire_instance_lock("my_app");
    if (lock_fd < 0) return 1;

    /* 3. Signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 4. Hardware init + apply configured backlight */
    hw_init();
    hw_set_backlight(100);      /* 100 = 100% OF THE CONFIGURED MAX, not raw 100 */

    /* 5. Audio (non-fatal if no output device opens) */
    Audio audio;  audio_init(&audio);

    /* 6. Framebuffer BEFORE touch - touch_init() reads the screen dims fb_init() sets.
     *    Pin the depth first: /dev/fb0 keeps whatever the last process set. */
    fb_set_bpp(fb_device, 32);
    Framebuffer fb;    fb_init(&fb, fb_device);
    TouchInput  touch; touch_init(&touch, touch_device);

    /* 7. gamepad_init() BEFORE registering any TouchRegion - it memsets the manager */
    GamepadManager gp; gamepad_init(&gp);
    /* ...register touch regions here... */

    /* 8. Main loop */
    while (running) { /* ... */ }

    /* 9. Cleanup, reverse order */
    hw_leds_off();
    hw_set_backlight(100);      /* restore for the next app */
    audio_close(&audio);
    gamepad_close(&gp);
    touch_close(&touch);
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    fb_close(&fb);
    return 0;
}
```

Three rules in there are load-bearing, and every one has been violated in shipped code:

- ⚠️ **`setvbuf(stdout, NULL, _IOLBF, 0)` FIRST, or a printf receipt never arrives.** At boot stdout is
  `/var/log/roomwizard/app_stdout.log`, so glibc block-buffers 4 KB: `compute_grid_layout()`'s layout
  receipt was absent from that log while `grep -ac 'launcher: safe'` on the deployed binary returned 1.
  `common/logger.c` line-buffers its OWN file, which is why this reads as a missing printf rather than a
  buffering one. The seven games, `app_launcher` and `control_panel` have it; ⚠️ the OTHER tools do not.
- **`fb_init()` before `touch_init()`** — `touch_init()` reads `screen_base_width/height`, which
  `fb_init()` sets. Reversed, portrait mode silently gets 800×480 instead of 480×800.
- **`gamepad_init()` before registering `TouchRegion`s** — it `memset`s the manager and zeroes
  `touch_region_count`. Nothing in the tree registers any, so the rule has no live example; get it
  right in the app that revives it.

**`app_launcher` launches children with `argv[0]` = `exec_path`, not the manifest's `name=`.** The display
name is for the grid, not the process table. Why a wrong `argv[0]` costs a session, and why `comm`-based
killing was never affected: [`../SYSTEM_ANALYSIS.md#53-app-launcher-and-manifests`](../SYSTEM_ANALYSIS.md#53-app-launcher-and-manifests).

## Pixel format — pin it, and never assume it

`/dev/fb0`'s depth is **global mutable state** and which component runs at which depth is
[`../SYSTEM_ANALYSIS.md#32-display`](../SYSTEM_ANALYSIS.md#32-display). Two authoring rules, and the
codebase once broke both:

- **Every app calls `fb_set_bpp(dev, 32)` before `fb_init()`.** `grep -n 'fb_set_bpp\|fb_init(&' */*.c
  tests/*.c` is the check, and every `fb_init(&` must have an `fb_set_bpp` immediately above it.
- **The primitives dispatch on `fb->bytes_per_pixel`.** Four helpers in `framebuffer.c` (`fb_pack565` /
  `fb_unpack565` / `fb_store` / `fb_load`) are the only code that knows the format; **the API is
  unchanged, callers always pass RGB888**. If you add a primitive, go through `fb_store()`/`fb_load()` —
  do not index `back_buffer` as `uint32_t`, which at 16bpp overruns the back buffer by exactly 2×.

Two subtleties worth not rediscovering: a sprite's **colour key is compared in the source's 32-bit
space, before packing** (two RGB888 colours can share one RGB565 word, so a key tested after packing
turns opaque pixels transparent), and `fb_unpack565` **replicates high bits rather than dividing**,
because a `/31` in the alpha inner loop is a call into `__aeabi_uidiv` on this core.

Covered by `tests/framebuffer_bpp_test.c` (host gcc, build line in its header): guard bytes after a
16bpp back buffer turn an overflow into an assertion, every primitive is driven over the whole surface
*including its last pixel*, and the same sweep runs again at 32bpp so a fix cannot regress the depth
every app actually uses.

## Hardware API rules

1. **LEDs: `hw_set_led()` / `hw_set_leds()`.** Never write `/sys/class/leds/` directly — these
   respect the `led_enabled` and `led_brightness` config.
2. **Backlight: `hw_set_backlight()`.** It scales by the configured `backlight_brightness`
   percentage, so `hw_set_backlight(100)` means "the user's chosen maximum". The one exception is a settings slider *previewing* a value — it is choosing that percentage, so scaling by the outgoing one shows the wrong brightness. `hw_set_backlight_raw()` is for that, and it exists so a preview does not carry its own copy of the sysfs path: two did, both naming a node that does not exist here.
3. **Call `hw_set_backlight(100)` at both startup and exit.** Startup so the app matches the user's preference; exit so the next app inherits a sane value. `snake.c` misses the exit call and `app_launcher.c` misses both — do not copy them.
4. **`hw_leds_off()` deliberately bypasses config** and always writes 0, so cleanup works even when LEDs are disabled.
5. Direct sysfs writes are acceptable **only** in test functions that intentionally exercise raw hardware.
6. **Screen blanking is already in `common/`; an app adds nothing.** `hw_blank_poll()` runs from `fb_swap`, `touch_poll` and `gamepad_poll`, and every input wakes the panel (the hat, and a stick only beyond its calibrated dead zone, `blank_abs_is_input` in `common/blank_decide.h`); the waking touch is swallowed there. A video or remote app opts out with `hw_blank_set_enabled(false)` after `fb_init` (`vnc_client.c`); a loop that blocks over 5 s without polling restarts the idle clock and restores the panel.

**Config cache is per-process.** `hardware.c` caches config on the first `hw_set_*()` call to avoid
re-reading during animations. A `fork()`/`exec()` child gets a fresh cache, so launched apps always see
current settings — but a long-running parent (the launcher) must call `hw_reload_config()` after a child
that may have changed settings exits. Config changes never propagate into a running process on their own.

## Rendering: dirty flag + adaptive sleep

600 MHz, no GPU. Unconditional 60 fps redraws burn 40 %+ CPU; tracking `needs_redraw`, drawing only when
set and sleeping longer when idle takes a static UI from ~40 % to under 5 %. Static UI (launchers, menus,
settings): draw only on state change. Games: dirty-flag start-menu/paused/game-over, render every gameplay frame.

```c
bool needs_redraw = true;               /* first frame always draws */

while (running) {
    int old_selected = state.selected;

    touch_poll(&touch);
    gamepad_poll(&gp);
    /* ...handle input, update state... */

    if (state.selected != old_selected /* || other visible change */)
        needs_redraw = true;

    bool drew = needs_redraw;           /* capture BEFORE clearing */
    if (needs_redraw) {
        draw_screen(&state);
        fb_swap(&fb);
        needs_redraw = false;
    }

    usleep(drew ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
}
```

The `bool drew` matters: testing `needs_redraw` in the `usleep` *after* clearing it inside the `if` always
yields the idle delay, pinning the app to 10 fps — a shipped bug. Capture it first, or clear the flag last.

**An app that repaints only on change must OR `button_take_dirty()` into its redraw test** — a `Button`'s look
change is the widget's to report (semantics: the `common.h` comment); without it a button stays yellow.
**A static screen repaints only when something changed**: a full 800x480 clear+redraw+swap costs ~15-17 ms CPU, so frame rate alone sets cost (measured: Snake ~10 % at ~7 fps, Frogger 51 % at 30 fps; idle `keyboard_enter`/input testers fell from 45-70 % to <= 2 % once gated, paced at `FRAME_DELAY_ACTIVE_US`).

**A component whose `update()` both draws and reads input has to tell the caller when it still needs
frames** — the loop's dirty flag sees only what the loop can see. `gameover_update(gos, in)` is the arcade flow
SHOW (3 s) → NAME (only if `hs_qualifies`) → SCORES (15 s) → `GAMEOVER_ACTION_MENU` (pure step: `gameover_phase_next`), and
its timed phases need frames without any input. It exposes `gameover_needs_redraw(gos, in)`, ORed into the flag
(`if (current_screen == SCREEN_GAME_OVER && gameover_needs_redraw(&gos, &in)) needs_redraw = true;`). Corollaries:

- **Do not fix it in the caller.** An unconditional redraw while `SCREEN_GAME_OVER` cures one game, leaves the
  others broken and pins a static overlay to 30 fps.
- **The transition frame's own press lands in front of the new screen's buttons.** `TouchState.pressed` is a
  rising edge surviving until the next `touch_poll()` and a draw-then-check-input component draws before it
  reads, so it must ignore input on its first drawn frame (`GameOverScreen.armed`); SHOW also ignores input for 1 s.
- **But do not make every state fall through — NAME deliberately keeps its `return`.**
  `hs_enter_name(fb, touch, gm, buf, score)` is a **blocking** keyboard that repaints and swaps the framebuffer itself, so
  drawing the next page in the same call composites it over the keyboard's last frame; it leaves `pending_draw` set and
  takes one clean frame.

**And the predicate must cover pending *input*, not just a pending draw.** If the component reads its buttons
inside the draw path, a frame the loop declines to run is also **an input event it never sees** — which
shipped as a game-over screen drawn correctly and completely unresponsive, only killable over SSH. Do not
rely on the caller having an input-activity `else` branch; a pure visible-state diff sees nothing in a tap on
an overlay. Three grounds, each load-bearing:

| Ground | Why |
|---|---|
| `pending_draw` | the multi-frame machine owes the screen a frame |
| `ts.pressed \|\| ts.held` | there is input to act on, and only a drawn frame lets us act on it |
| any button's `was_pressed` | `button_check_press()` clears that latch only on a frame where the button is **not** touched. At `FRAME_DELAY_IDLE_US` a press and its release can both land in one `touch_poll()`, so there may be no `held`/`released` frame at all — and then the *next* press is silently eaten. Ask the buttons, not the touch state |

Idle still costs nothing: with no finger down and nothing latched all three are false and the static overlay
produces no frames. `FRAME_DELAY_ACTIVE_US` = 33 333 (~30 fps) and `FRAME_DELAY_IDLE_US` = 100 000 (~10 fps)
are in `common/common.h` — use them, don't hardcode `usleep()` values. Reference: `app_launcher/app_launcher.c`.

**The only `usleep()` in a game is the one at the bottom of the main loop.** Anything else stops the world:
no `touch_poll()`, no `gamepad_poll()`, no redraw. Two shapes, both with a replacement:

| Shape | Was | Use instead |
|---|---|---|
| "flash an LED for N ms" | `hw_set_led(); usleep(); hw_leds_off();` — up to 1.2 s in tetris and pong | `LedPulse` + `hw_led_pulse_start/update/stop()` in `hardware.c` |
| "hold this screen for N ms" | `while (...) { draw; fb_swap; usleep; }` — samegame's 300–1500 ms pre-game-over hold | a state in the app's existing animation machine (`ANIM_GAMEOVER_DELAY`) |

Three things about that, learned from doing it:

- **`hw_led_pulse_update()` takes `now_ms` as an argument rather than calling `get_time_ms()`**, because
  `vnc_client` links `hardware.c` but **not** `common.c` — same reasoning as `gamepad_poll()`'s touch
  coordinate, and it makes the pulse host-testable. `hw_blink_led()`/`hw_pulse_led()` are still blocking:
  correct for exactly one case, the exit flourish before `running = false`.
- **The render-loop shape is the one that hides a dead button** — a screen held without polling leaves MENU
  and EXIT dead. As an animation state it costs nothing: the dirty flag already forces frames on
  `anim_state != ANIM_NONE`, and `handle_input()` checks the buttons *before* it gates the grid on the
  animation — check that order if you add one.
- **A pulse outlives the state that started it, so whoever can leave that state must cancel it** —
  `reset_game()` / `init_level()` call `hw_led_pulse_stop()`, or a game-over flourish flashes into the next
  round. If a code path writes the LED directly, stop the pulse first or the next update re-lights it.

**Game logic works in its own units — cells or fractions of the playfield — and speeds are those units per
SECOND of elapsed time (`get_time_ms()` delta), never pixels per frame; only drawing converts to pixels.** Frogger
scaled speed by `width/800` but laid out from the height, so portrait ran ~3.4× slow in cells; tetris' gravity counter ran ~3× slow while idle, and any per-frame motion slows whenever a frame costs more.
Every game is time-based (px/s = old px/frame × 30), in one of two idioms: variable dt plus substeps against tunnelling (frogger, pong), or a fixed tick
accumulator with constants per tick, at most 4 ticks per frame (platformer at 1/20 s, brick_breaker at 1/30 s). Both re-baseline the clock on entering play, so pause/resume never jumps.

**Derive state; don't accumulate it, and don't let a marker mean two things.** One shape produced three game
bugs: a multiplier re-applied to a figure that already contained it (so **SLOW DOWN made the ball faster**), a
sentinel every other site read as something else (`health = -1` for "indestructible" against `health <= 0`
"destroyed", so the brick had no collision), and a published slot nothing wrote (`length++` exposing a
`body[length]` the shift never fills). Each fix made the derived value derived rather than patching the failing
site: **one writer**, everyone else reads from it (`ball_apply_speed()` is the only writer of `Ball.speed` and
reads a separate `base_speed`; `brick_is_destroyed()` is the only test). Two things make the class quick to
confirm and safe to fix: **the dead code proves the diagnosis** — brick_breaker had a bounce path and a
diagonal-stripe renderer for indestructible bricks, neither reachable, and deliberate-looking unreachable code
tells you which value is being misread — and **when a clamp moves, ask what it was protecting**:
`BALL_MAX_SPEED` stayed on the *effective* speed because 11 px/tick is what bounds the substeps that stop the ball tunnelling through
a brick. At effect level 0 the emitted behaviour must come out byte-identical; that is what preserves the
default feel.

## Coordinates, dimensions, portrait

- **Never hardcode 800, 480, or 400.** Use `fb.width` / `fb.height`, `screen_base_width` /
  `screen_base_height`, and the `SCREEN_VISIBLE_*` / `SCREEN_SAFE_*` / `LAYOUT_*` macros. Hardcoding
  `400` as the centre-X has been the single most common bug in this codebase. Pick between the two
  screen rectangles by asking whether the thing has to be *pressed* or only *seen* — see *Screen edges*.
- **Compute layout at init, not compile time.** Replace `#define`d grid/tile constants with
  runtime values computed after `fb_init()`. See `compute_grid_layout()` in `app_launcher.c`
  and `init_brick_layout()` in `brick_breaker.c`.
- **Scale element counts to available area** (`count = space / (size + padding)`), don't fix
  them for one orientation.
- **Touch and drawing must share a coordinate space.** Call `touch_set_screen_size()` with the
  virtual dimensions. A mismatch produces the "cursor runs ahead of the finger" symptom.
- **Guard game logic by state** — ball physics, AI and scoring must not run on the start menu
  page.

Portrait mode (flag file `/opt/games/portrait.mode`):

- `fb_init()` swaps width/height and rotates the safe margins. `fb_swap()` does a 90° CCW
  rotated copy; landscape is a straight `memcpy`. `touch /opt/games/portrait.mode` for one run is how a portrait layout receipt is captured without tapping.
- Touch rotates **after** the raw→panel curve, in `scale_coordinates()`.
- Use `fb.portrait_mode` to branch only when the layout must differ fundamentally (3×2 vs 2×3);
  otherwise let the runtime dimensions do the work.
- **Calibrate in landscape.** Calibration in portrait is not supported.

Surface smaller than the panel (`fb_init(fb, device)` takes any node and accepts whatever geometry it finds):

- **Design pixels convert by the surface:panel ratio.** `fb_scale_ui_px()` / `fb_ui_px_x()` / `fb_ui_px_y()`
  in `common/framebuffer.c`; `fb_apply_viewport()` also publishes `screen_true_panel_width`/`_height`,
  separate from the surface-valued `screen_panel_*`. ⚠️ **The ratio is surface:panel, never a fixed
  800×480 reference** — portrait is 480×800 at full resolution and a fixed reference shrinks every control
  there to 60 %. Converted: the `BTN_*_WIDTH`/`_HEIGHT` and `LAYOUT_*` constants in `common/common.h` and
  `FB_TOUCH_INSET_MAX`; bit-exact no-op at both shipped geometries. `tests/ui_scale_test.c` **group 8
  drives the wrappers and is not optional**: measured, a fixed-reference wrapper and a transposed one each
  fail group 8 and nothing else.
- **Bezel margins are panel pixels**: `fb_init()` reads the panel's true size from the display mode
  timings and converts them with the pure `fb_scale_bezel_to_surface()` (rounds down; `tests/bezel_scale_test.c`).
- **Touch on a scaled node needs no divide** — measured: the published dims are surface dims, so stage 1
  already tracks the surface. Dividing breaks 12 assertions in `tests/touch_map_test.c` group J (one
  calibration over 800×480, 400×240, 200×120).
- **Not converted, measured:** `common/common.c` has zero `fb_ui_px_*()` calls, so `gameover_init()` and
  `modal_dialog_draw()` mix scaled and flat units (about 40 layout literals; `button_draw()`'s `- 20`
  icon inset is at 51 call sites); the icon stroke floors (3 px bars) cannot scale down; text is an
  integer multiplier with no rung below 1, so a scaled label can overrun its button (`"PLAY AGAIN"` is
  120 px against a scaled 110) and `text_measure_width()` over-measures by 33 % (8 px/6 px font-width
  confusion at `common.c:92` and `:131`). Per-game fixed bands and `TILE_SIZE`/`PADDLE_*` constants are
  unconverted too; `platformer.c` is the most expensive.

## Screen edges — two rectangles, both handled by the library

The plastic bezel hides a band of panel pixels at top and bottom, and **apps never compensate for it** —
`fb_init()` sizes the drawing surface to the visible rectangle and `fb_swap()` places it on the panel,
leaving the hidden bands black. Never subtract a bezel margin yourself; that double-corrects. The geometry,
the margin defaults and where they are set:
[`../SYSTEM_ANALYSIS.md#32-display`](../SYSTEM_ANALYSIS.md#32-display). `fb_set_bezel()` changes them on a
live framebuffer and **resizes the back buffer**, so re-run any layout computed from `SCREEN_SAFE_*`
afterwards — see `rebuild_ui()` in `control_panel.c`.

**Visible is not the same as touchable.** The digitizer's reading saturates *before* the physical
panel edge — badly on Y, mildly and calibration-dependently on X — so a band at each end is drawable
but unpressable. Hence two macro families:

| Macro family | Means | Use for |
|---|---|---|
| `fb.width` / `fb.height` | drawing surface | all drawing calls, unchanged |
| `SCREEN_VISIBLE_*` | the full logical screen | backgrounds, borders, titles, status/score rows, game playfields, read-only info pages |
| `SCREEN_SAFE_*` | visible **∩** touchable | buttons, toggles, tab bars, touch grids, anything hit-tested |

The band's measured extent, the per-unit inset digits, why a non-zero X inset is the model working rather
than a fault, and where to read the live numbers on the device are all
[`../SYSTEM_ANALYSIS.md#33-touch`](../SYSTEM_ANALYSIS.md#33-touch) — which also records why two earlier
revisions of this rule got it wrong in *opposite* directions. **Treat every one of those numbers as
per-unit and per-calibration.** What belongs here is what an app must do about them:

- The inset is **measured at runtime, never hardcoded**: `touch_input.c`'s `publish_safe_area()` pushes the
  four raw edge extremes through the production `scale_coordinates()` and calls `fb_set_touch_inset()`. It
  is capped at `FB_TOUCH_INSET_MAX` (48 px) with a loud warning, and is `0` on an unswept panel.
- `SCREEN_SAFE_*` is only correct **after** `touch_init()`. Compute layout after both `fb_init()` and
  `touch_init()` — the documented lifecycle anyway — and re-run it after any `fb_set_bezel()`.
- The band **stays fully drawable and is good screen area** — a status bar, score row, title or background
  belongs there. Do not shrink the drawing surface or grow the bezel to make visible == touchable. **This
  is a native_apps policy, not a device-wide one:** `scummvm-roomwizard` and `vnc_client` display
  third-party content nobody can audit for what must be pressable, so they confine the *content rectangle
  itself* to `SCREEN_SAFE_*` and leave the band black. See their own `CLAUDE.md` files.
- Not every playfield can move: Tetris' board and Brick Breaker's play area are drawn and collided
  against but never pressed (touch is X-only), so they use `SCREEN_VISIBLE_*`; SameGame's grid *is*
  tapped cell-by-cell, so it stays inside `SCREEN_SAFE_*`.
- **A top button row is `LAYOUT_MENU_BTN_X/Y` + `LAYOUT_EXIT_BTN_X/Y`, never a literal `10, 10`** — and
  **the reserve below it derives from the row**, not from a second literal. A playfield starting under the
  row is `LAYOUT_MENU_BTN_Y + BTN_MENU_HEIGHT + <gap>`, and its height comes off
  `SCREEN_VISIBLE_BOTTOM`, not `fb.height - <sum of the gaps>`. A literal offset loses its top rows to the
  inset; a literal reserve leaves the row on the playfield once it moves; a literal reserve *smaller* than
  the row draws the buttons on the playfield *and* runs it off the bottom (tetris, `SAFE_TOP + 55` against
  a row occupying `+10 .. +60`). If the row lives inside a fixed-height HUD band, **the band has to grow
  with it** (`frogger.c`'s `hud_height = SCREEN_SAFE_TOP + HUD_HEIGHT`). Count the **frame** too: a
  `fb_draw_rect()` outline sits *outside* the content rect on every side, so its thickness belongs in the
  vertical budget, from the same named constant the drawing uses. Where a drawn rect and a hit-test
  describe the same target, compute both from **one** helper (`icon_grid.c`'s exit button, which draws it and reports the tap).
  `grep -n 'button_init(&[a-z_]*, *[0-9]'` is the check. **When you replace a literal, choose the
  expression that is byte-identical to it at inset 0** — then an uncalibrated panel is provably unaffected
  and the diff can only move pixels on a panel that has been swept, which is what makes this class of
  change safe in bulk without re-testing every unit.
- **A capped row is positioned from the count you draw, not the count you have.** `frogger.c` computes
  `lives_shown = min(lives, 5)` and lays out from `lives_shown * LIFE_ICON_PITCH` — right. Positioning
  from `game_lives * 16` while drawing `min(game_lives, 5)` agrees at 3 lives and diverges the moment
  something grants more, which stranded five icons mid-HUD when Office Runner's training mode gave 10.
  Cap first, lay out from the capped number, and **say what the cap hid** — one icon plus `x10`, never
  five icons meaning ten. `brick_breaker` still truncates silently at nine — an open bug, not the model.
- **Put a status row in the band above the button row, not below it.** HUD text is only *seen*, so it
  belongs in `SCREEN_VISIBLE_TOP` — the band the two-rectangle split exists to keep usable. Stacking it
  under `SCREEN_SAFE_TOP` put tetris' `LVL` and frogger's lives icons **behind** the buttons, which are
  drawn after them. The pattern both games use, and why it is safe on an uncalibrated panel:

  ```c
  int band_h = LAYOUT_MENU_BTN_Y - SCREEN_VISIBLE_TOP;   /* == inset + 10 */
  int row_y  = (band_h >= text_h)
               ? SCREEN_VISIBLE_TOP + (band_h - text_h) / 2   /* in the band */
               : LAYOUT_MENU_BTN_Y + (BTN_MENU_HEIGHT - text_h) / 2;  /* level with the row */
  ```

  At inset 0 the band is 10 px and cannot hold 14 px of text, so the row drops level with the buttons —
  safe only because **horizontally it is confined to the gap between MENU and EXIT**
  (`LAYOUT_MENU_BTN_X + BTN_MENU_WIDTH` → `LAYOUT_EXIT_BTN_X`). Lay it out from a *measured* total width
  so it stays centred as the numbers grow. That gap is also where a full-width bar goes: frogger's timer
  bar once spanned the grid and covered the bottom of both buttons.

Minimum comfortable touch target is about 60×40 px, with the 200 ms `BTN_DEBOUNCE_MS`.

## Touch model

Two stages, in `scale_coordinates()`:

```text
raw -> panel    piecewise-linear, 3 segments, knots at panel dim/4 and 3*dim/4  [calibration]
                  raw_min    .. knot_lo  ->  panel      0 .. dim/4    outer
                  knot_lo    .. knot_hi  ->  panel  dim/4 .. 3*dim/4  fitted interior
                  knot_hi    .. raw_max  ->  panel 3*dim/4 .. dim-1   outer
     -> rotate 90° CCW if portrait
     -> logical  = panel - view_origin                                      [bezel viewport]
     -> clamp to the logical screen
```

`touch_map_axis_panel()` is the single implementation, and `touch_calib_predict_*()` delegate to it
so a predicted reach can never disagree with the live mapping. Stage 2 is the exact inverse of what
`fb_swap()` does, which is what keeps touch and drawing in one coordinate system.

**The panel is linear, and hard-clipped at raw 0 and 4095** — the model, the per-edge dead band and the
refuted "~8.8 vs ~9.9 raw/px" claim: [`../SYSTEM_ANALYSIS.md#33-touch`](../SYSTEM_ANALYSIS.md#33-touch).

Calibration therefore has two parts. Least squares over 11 targets × 3 taps gives the interior **line**,
and **only targets far from the ends enter it** — ≥100 px on X, ≥80 px on Y
(`touch_calib_interior_x/y()`). Then `touch_calib_curve_from_fit()` turns that line into the curve that
ships: knots on the line at panel ¼ and ¾, and the endpoints **exactly where the line puts them**
(`c->v0 = f->in0`, `c->v1 = f->in1`) — so with the endpoints unclamped the stored three-segment curve *is*
a straight line, and the host regression asserts **zero** deviation. The three-segment format is kept
anyway, because changing line 1's field count is the one edit that silently breaks a component linking a
stale `touch_input.o`. File format is in `../SYSTEM_ANALYSIS.md`.

⚠️ **Never clamp a fitted endpoint into `0..4095`** — a correct fit legitimately extrapolates outside the
emittable range (the reference Y fit is `-279..4382`), because the line reaches raw 0/4095 before the panel
edge. The measurement that settles it, and what the clamp did to the cursor:
[`../SYSTEM_ANALYSIS.md#33-touch`](../SYSTEM_ANALYSIS.md#33-touch). Both that clamp and `touch_input.c`'s
legacy-migration `clamp_to_hw()` are deleted; `overshoot_lo/hi` survives as *reporting only*. Never
reintroduce either. And ⚠️ **the sanity gate is not "reject outside `0..4095`"** — that is the same bug in
gate form. It requires `2 × overlap(fit, hw) ≥ max(fit_span, hw_span)`, which **accepts** the measured-good
`-279..4382` and rejects a skewed `0..60000`.

**`common/touch_calib.c` is the only implementation of the fit.** It holds the target set, the interior
masks, the per-axis verdict, the reach calculation, the edge-sweep accumulator (`TouchCalibSweep`,
`touch_calib_sweep_*`), the sanity gate and the `.bakN` backup. `control_panel`'s calibration wizard (`control_panel/touch_wizard.c`) and the
`touch_raw` diagnostic both link it — which is what lets the diagnostic validate the code the wizard
actually calibrates with. Do not write a second copy; there were three of the fit and two of the sweep, and
they drifted.

After changing it, run the host-side regression. It replays the reference capture's 11 target medians and
asserts the interior fit still lands on `X 17..4084` / `Y -279..4382`, that the derived curve reproduces
that line with **zero** deviation, that the endpoints are **not** clamped (`Y v0 -279 / v1 4382`), and that
the per-axis verdict reports the real dead band (`Y: sensor saturates 28 px inside the low edge, 30 inside
the high`). It is host gcc, so `build-and-deploy.sh` does not run it:

```bash
gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/touch_calib_test \
    tests/touch_calib_test.c common/touch_calib.c common/touch_input.c \
    common/framebuffer.c common/hardware.c common/config.c -lm && ./build/touch_calib_test
```

**Rules that are easy to get wrong:**

| Rule | Why |
|---|---|
| Fit calibration against **panel** coordinates, not logical ones | Targets are drawn in logical space, so add `fb->view_x/view_y` and pass `screen_panel_width/height` as the dim before calling `touch_fit_axis_range()`. Otherwise the bezel is baked into line 1 and stage 2 subtracts it again. |
| **Never clamp a fitted endpoint into the emittable raw range** | The sensor clips before the panel edge, so a correct line legitimately passes raw 0/4095 inside the panel. Clamping fabricates a steeper outer segment and the cursor runs ahead of the finger. Endpoints outside `0..4095` are the measurement, not an error. |
| Derive a slope from the multi-target fit, never from two adjacent near-edge samples | ±80 raw of tap noise over a 10–20 px baseline is nonsense: `touch_raw`'s old INSET report printed "raw reached at panel 594, 614, 817, −4" on a 480-row panel. Near-edge taps are only good for locating *where* the reading goes flat, which is a comparison, not a division. |
| Validate with `touch_map_raw()`, not a private copy of the maths | It runs the production path, so the summary screen cannot drift from what apps actually see. |
| Call `touch_set_screen_size()` (or `touch_set_viewport()`) after any `fb_set_bezel()` | The `TouchInput` caches the panel size and viewport origin. |
| Extend the config file with new **keyword-tagged trailing lines**, never by changing line 1's arity | An old parser reads lines 1–2 positionally and ignores what it does not recognise, so a tagged line 3 is back-compatible by construction. This is how the measured edge reach was added. |
| A UI whose buttons sit on a screen that is itself **sampling edge touches** must keep them clear of all four edge bands, **on both axes** | An edge-sweep collector treats a finger anywhere in the outer sixth of an axis as a sample for that edge. `WIZ_REACH`'s CANCEL at the normal button row (`y=384..440`) is inside the BOTTOM band (`y > H*5/6 == 400`), so pressing CANCEL would record its own tap as a bottom extreme. Its row sits at `y=258`, `x` in `[200,610]` — clear on both axes. `touch_raw` hit the same trap and solved it the same way; if you move either row, re-check both. |
| Report the touch-safe area by reading `screen_touch_inset_*`, never by re-deriving the mapping | `display_touchable_rect()` prints the same globals `SCREEN_SAFE_*` resolves to, so the number on screen cannot disagree with what layouts actually got. It used to re-run the arithmetic from a `TouchInput *`, which is a second copy that can drift; it now takes no `TouchInput *` and returns nothing. |
| No affine / rotation / skew term | Solves a problem the hardware does not have; the extra freedom lets one bad tap rotate the whole mapping. Piecewise-linear *per axis* is fine and is what ships — each axis stays an independent monotone 1-D curve, which is the shape the hardware actually has. |
| No bilinear / 4-corner offsets | Same, and the file format persists four raw values per axis and nothing else, so they would be silently discarded. |
| Knots live at panel dim/4 and 3\*dim/4, and nowhere else | They are implicit in the file format (`TOUCH_KNOT_LO/HI`). Moving them silently reinterprets every stored calibration. |

`touch_enable_calibration()` is a **no-op** — the flag is written but never read, so calibration cannot be
turned off. **ScummVM and vnc_client each link their own copy of `touch_input.o`**, so redeploy **both**
after changing touch code and especially after changing the calibration file format; the silent-misparse
mechanism is in root `../CLAUDE.md` and the forensics, with the `(piecewise)` discriminator that identifies
a stale binary, in [`../SYSTEM_ANALYSIS.md#33-touch`](../SYSTEM_ANALYSIS.md#33-touch).

## control_panel — a static page registry

**`control_panel` is the one place for settings and tests; a page is a module, not a binary.** Home is a paged
icon grid (`common/icon_grid.c`; 3x2 landscape, 2x3 portrait); each tile opens a `CpPage`
(`control_panel/cp_page.h`): `load`, `layout` (rects plus the `control_panel: <page> stack fits` receipt),
`enter`, `leave`, `draw`, `input` (idle, redraw or fullscreen), `run_fullscreen`, `reset_defaults`, and an
optional `busy()` that holds the active frame rate (the Audio page, only while a chime plays). A page file exports one `const CpPage`; adding a page is
one file plus one row of `home_pages[]`, and `control_panel.c` holds no per-page code. Shared drawing is `cp_ui.h`;
icons come from `gen_cp_icons.py`. Settings "apps" never become launcher tiles; heavy tools (Mix Bus Test,
`touch_raw`) stay child processes launched from their page, and Tap-a-Theremin stays an app with its own tile.

- **Every change saves at once; there is no SAVE.** RESET DEFAULTS (Information page) asks through `cp_confirm()`
  first, backs the config up to `<path>.bak-YYYYmmdd-HHMMSS` (`O_EXCL`; on failure nothing is reset), then runs
  `config_clear` and every page's `reset_defaults`; touch calibration is untouched. The dialog's wording
  ("(TOUCH CALIBRATION IS KEPT)" in brackets) is the operator's and stays although `config_clear` wipes every key.
- **Exit and navigation:** the shared top bar's red X on the home grid only exits (Shutdown / Reboot live in `app_launcher`'s X dialog);
  on a page the bar carries BACK `<` on the left (the X is gone) so a double tap cannot leave and quit. Pages post a status message with
  `cp_status()` (6 s) that takes the page title's place in the bar while it shows (green or orange, cut with ".." if too wide), and reach the wizard or `touch_raw` through `cp_run_touch_tool()`, which reloads geometry
  and calls `rebuild_ui` itself — `main()` does not rebuild after a page's `run_fullscreen`. **Keyboard and pad:** a page lists its widgets in the optional `CpPage.focusables`, from the same `button_rect()` / `toggle_hit_rect()` its hit-test uses; arrows move a focus ring, Enter/Space activate it as a synthetic tap, so **no page's `input()` changes**; Esc/Backspace cancel a dialog or go back, and do nothing on home. `focus_drain_keys()` runs after every full-screen run so an Esc typed there is not BACK here; the Input testers repaint only their hold-to-exit band with `fb_swap_rect()`.
- **`enter()` only marks work pending**, so the page paints first. The USB page's opening scan of an empty port
  (or one whose musb `mode` reads `b_*`, `usb_port_dead()`) queues one port re-probe (`RECOVER_TRIES=1`, measured 6.0 s on .188): ~6 s on every open while the socket
  is empty, kept by operator decision; RESCAN keeps three attempts. Disabled controls refuse input in the widget (`Widget.disabled`).
- **Rejected:** one executable per page (init respawns the launcher, so the return path breaks, and every tap re-inits fb, touch and config). `dlopen`'d pages are deferred — an ABI version field refused on mismatch,
  exported helpers and `check-arm-safe.sh` over the `.so` would all be required.
- **`control_panel <page>` opens one page directly, so a page can be captured over SSH** (`cp_page_find()` in `control_panel/cp_page_name.c`, host test `tests/cp_page_name_test.c`). Names are the page-table names in lowercase (`audio display led usb bluetooth input network monitor information system`), case-insensitive, a unique prefix accepted; arguments starting with `/` (the launcher's fb/touch paths) are skipped; `--list-pages` and `--help` exist; an unknown name exits 2; BACK returns to the home grid. Capture: `/etc/init.d/roomwizard-app stop`, then `/opt/games/control_panel /dev/fb0 /dev/input/event0 monitor &`, then `cat /dev/fb0` (measured on `.188`, 2026-10-06). It reaches the page only, not a state inside it.

**The Log page and the terminal.** The Log page (`control_panel/log_page.c`) is read-only by design: no export, no delete, no login; longer reading stays with SSH or `commissioning/backup.sh`. The terminal (`native_apps/terminal/`, its own launcher tile) runs `/bin/login` in a pty, not a shell, and carries no security logic: a user gets what the account grants. **Operator-verified at the panel 2026-10-12:** the Log page's tabs and ERRORS ONLY filter; terminal login with the on-screen keyboard and with a USB keyboard, and exit. **Not verified:** the terminal's toolbar keys, the pad route, `vi`/`top`, the `xterm` terminfo entry on our root `[inferred]`, and a launch from the Control Panel (not implemented).

**`rwmond` is the Monitor page's history source** (`native_apps/sysmon/rwmond.c`, `/opt/games/rwmond`, in `GAMES_BINARIES`): at nice 19 it samples CPU, memory and SoC temperature once a second and publishes a 120-sample ring at `/var/run/rwmond.ring` (tmpfs; text, atomic rename; format in `sysmon/mon_ring.h`). The page seeds its graph from the ring when it is at most 3 s old. `roomwizard-app stop` kills it with `/opt/games`, and `native_apps/build-and-deploy.sh` restarts it. The Monitor graphs' pure scaling lives in `control_panel/mon_graph.c` (host test `tests/mon_graph_test.c`); the layout receipt reads "N graphs WxH beside|under". `rwmond` also owns the over-temperature flag (latch 85/80 °C, `common/overtemp.h`, `red_led`) and links `common/hardware.c` and `config.c`; `fb_swap()`/`fb_swap_rect()` paint the red square via `fb_overtemp_poll()`, so **a loop that can idle without presenting must call `fb_overtemp_poll()` itself** (launcher and control_panel do). Test: `touch /var/run/rwmond.force_overtemp`. Measured on `.188`: 0.27 % CPU, VmRSS 1.1 MB. The RAM text row is total-free-buffers-cached while the graph is MemTotal-MemAvailable, so the two differ.

## Input

Use `gamepad.c` for everything; don't scan evdev per-app. Abstract buttons are `BTN_ID_UP..BTN_ID_BACK`,
configurable via `/etc/input_config.conf`. `.pressed` is a rising edge, `.held` is level. Prefer `.pressed`
for menu navigation and discrete actions, `.held` for continuous movement. **Canonical pad codes are `xpad`'s:**
`input_scan.c` decides a layout per pad at open (SEQUENTIAL = has `BTN_TL` and `BTN_TR2` but neither `BTN_SELECT` nor
`BTN_START`, i.e. the 8BitDo in BT X mode) and every pad reader (`gamepad.c`, ScummVM `pollGamepad`, the pad tester)
translates with `input_pad_key()` at read time; `EVIOCGKEY` levels are looked up by raw code, then translated;
defaults and `/etc/input_config.conf` stay in `xpad` codes. `KEY_MENU`→`BTN_MODE` is deliberately untranslated (no
consumer). The default `btn_action` is `BTN_WEST` (`0x134`), commented "X" but physical Y under `xpad`.

⚠️ **Any evdev reader that tracks a level (button, key, axis, touch contact) must handle `SYN_DROPPED`.** When the
kernel buffer overflows, `evdev.c` `__pass_event` drops the unread events and leaves `SYN_DROPPED` plus the newest one
[measured from source]; a lost release then reads as held until the next click. On `SYN_DROPPED` skip to the
end of that packet with `input_syn_drop_skip()` (`common/input_scan.h`) and re-read the level (`EVIOCGKEY` keys, `EVIOCGABS` axes, `EVIOCGMTSLOTS` MT slots).
Every evdev reader in the tree does; a new one must too. Checks: `tests/evdev_resync_test.c`, `vnc_pad_test.c` group 11. Keyboard, pad and MT paths: inferred from code, not observed on device.

**All three fields are pure outputs of `gamepad_poll()`** — it recomputes them from scratch every call, so
writing them from an app has no effect past the next poll. `held` is the OR of two kinds of source, and
that split is what stops a virtual D-pad latching:

| Source | Reports | Level lives in |
|---|---|---|
| gamepad buttons, D-pad hat, keyboard | press/release **events** | `GamepadManager.held_latched[slot][]` (per player slot) — must survive quiet frames, because a key-up may be hundreds of frames away |
| touch regions, analog stick | an absolute **position** | a per-frame array inside `gamepad_poll()`, rebuilt every call |

Both kinds used to write `.held` on the caller's `InputState` with only the first ever cleared, so **a
virtual D-pad latched on permanently**. If you add a source, decide which column it is in first: the naive
"clear `held` at the top of the poll" is what breaks the event-driven half. Two smaller holes closed with
it, both worth not reopening: `poll_gamepads()` **zeroes a slot's axes while no pad of it is open** (a stick unplugged
while deflected otherwise asserts its direction forever), and `gamepad_rescan()` clears `held_latched[]` (a
key held at unplug time never gets its key-up).

**A `TouchRegion` virtual D-pad is safe again, but still usually the wrong design.** Snake's four regions
*overlapped* — UP/DOWN the full-width top/bottom halves of the grid, LEFT/RIGHT the full-height left/right
halves, so every in-grid tap asserted two directions — and they duplicated the head-relative input path it
already had. So **no *app* calls `gamepad_set_touch_regions()`**; the only caller is
`tests/gamepad_latch_test.c`, and `gamepad_draw_touch_controls()` has **no** caller at all. Both are kept
as library surface; the latter's boxes never matched the regions they claimed to draw.

Prefer a **tap relative to the object being controlled** over a virtual pad — frogger hops the frog
towards wherever you tap in the playfield, which needs no regions, cannot latch, and makes the whole
playfield one target. Where that does not map (platformer needs simultaneous run + jump), say so on the
start menu with `start_menu_set_warning()` rather than shipping controls that do not work.

**What you can and cannot test from a script.** `CONFIG_INPUT_UINPUT` is unset in this kernel, so an
injector reports success and delivers nothing — a `tests/touch_inject.c` did exactly that and is
deleted; root `../CLAUDE.md` has the mechanism. But that is a limit on *the device*, not on the
code: `gamepad.c`'s own state machine is fully testable on the host, and `tests/gamepad_latch_test.c` does
it. `gamepad_poll()` takes the touch coordinate as a plain argument and its evdev sources are `read(2)` on
an fd — so a temp file of `struct input_event` assigned to `gm.gamepad_fd` drives the real
`poll_gamepad()`, returning each event and then 0 at EOF exactly as a quiet non-blocking evdev fd does.
Run that after touching input logic; what still needs the panel is only whether a game *feels* right.

**Ask a touch button with `button_check_tap()`, every frame.** `button_check_press()` derives the press
edge from `btn->was_pressed` and clears that latch on exactly one kind of call — one where
`currently_pressed` is **false**. So this shape, which shipped in `platformer.c` and `frogger.c`, fires
**once per process** and is dead afterwards:

```c
if (ts.pressed) {                                          /* WRONG */
    if (button_is_touched(&menu_button, ts.x, ts.y) &&
        button_check_press(&menu_button, true, now)) { … }  /* never called with false */
}
if (button_check_tap(&menu_button, &ts, now)) { … }         /* right — every frame */
```

Office Runner's hamburger menu died after its first use and the report came off the panel. Three things
make this class hard to see, and all three are why it needs a helper rather than care:

- **EXIT hides it.** An exit button only ever needs to fire once, because it ends the process — same
  defect, no symptom. In the report EXIT worked and MENU didn't, in the same HUD row.
- **Touch gameplay hides it.** Passing the *value* survives if players tap elsewhere constantly, because
  that is what produces the false call. A controller-only game has nothing to clear the latch.
- **The obvious fix keeps it.** Testing `button_is_touched()` alone on quiet frames does not clear it
  either: `touch_input.c` leaves `state.x/y` at the last touched point after release, so the coordinates
  stay inside the rect with no finger on the panel. The level signal has to come from `ts.pressed ||
  ts.held`, which is what `button_check_tap()` does.

`tests/button_latch_test.c` is the regression, and **group A drives the old idiom** so a green run cannot
mean the harness is blind. `grep -rn 'button_is_touched(&[a-z_]*, *[a-z.]*x, *[a-z.]*y) *&&' --include=*.c`
is the tree-wide check.

**Every app with a game loop calls `gamepad_tick(&mgr, now_ms)` once per frame, after `gamepad_poll()`.** Every `INPUT_SIG_CHECK_MS` (1 s) it compares the `/dev/input` fingerprint (`input_node_sig()`, opens nothing) and calls
`gamepad_rescan()` only on a change, or at once after a read returned ENODEV/EBADF; an app that omits it never re-detects a device. The read loop
is `while (read(fd, &ev, sizeof(ev)) == sizeof(ev))` — a pad that leaves the bus (unplugged, or a wireless
one idling out) leaves a stale fd that fails forever, which is why that error arms the rescan. The rescan
clears `held_latched[]` (a direction held at unplug is not stuck on) then `seed_latched_levels()` re-reads the held levels, so a key held across the rescan keeps its level.

⚠️ **`scan_devices()` announces a CHANGE, never a poll, and a new print in it must keep that shape.**
Because the rescan closes every device first, the `fd < 0` that guards each announcement is always true by
the time the scan runs, so an unconditional `printf` there is one line per device per rescan — measured on
`.188` as 1720 identical `found gamepad` lines in one session, in the log that a no-microphone audio
verification has to read. `announce_found()` / `announce_lost()` in `common/gamepad.c` compare against a
remembered `"<name> at <path>"` per slot, which is why those three fields are the ones `gamepad_close()`
must **not** clear. `tests/gamepad_announce_test.c` is the regression; it needs a real evdev node, so read
[`../tests/CLAUDE.md`](../tests/CLAUDE.md) before assuming a green run measured anything.

⚠️ **The rescan fixes a stale fd, not a dead port — and the two are complementary.** It re-`open()`s
`/dev/input/event0..31` from scratch, so it finds a *new* node but cannot create one, and a unit booted with
an empty port has no node for this poll to find for the rest of that boot. A recovery needs **both** halves:
something to make the device enumerate, and this poll to pick it up without a relaunch. Do not read the 5 s
poll as "unplug/replug recovers", and read
[`../SYSTEM_ANALYSIS.md#36-usb`](../SYSTEM_ANALYSIS.md#36-usb) — for what the other half is and which sysfs
writes are no-ops — before building anything on it.

Every app should handle `BTN_ID_BACK` as "exit / back" — `fb_fade_out()` then `running = false`, as
`frogger.c` does. A game that doesn't can leave its game-over screen with no way out at all.

## 32-bit target

`sizeof(long) == 4` — root `../CLAUDE.md` for the rule. A duration times a sample rate is the same trap:
`(long)44100 * 49000` leaves 32 bits, which is why `audio_frames_for_ms()` computes in `long long` and
clamps.

⚠️ **A host regression cannot *observe* any of this — this host's `long` is 64 bits.** The overflow has
to be **modelled**, by truncating the 64-bit product to `int32_t` the way armhf does; a test that simply
writes the shipped expression passes on the host and says nothing about the target. `tests/audio_gen_test.c`
group A does it that way and labels it.

## Audio: the generator is separate from the device

(`tests/audio_gen_test.c`). The device half — ALSA via libasound, the device resolver, the GPIO12 amp — is
`common/audio_out.c`, the continuous stream and **the only playback path**: `audio_init()` opens it, and
if it fails returns -1 and the app is silent, with no fallback. `audio.c` keeps the config gate and the
client API and **consumes `audio_gen` for everything else**. It includes no device header, so it links on
host gcc as it stands (`tests/audio_tone_test.c`), where `audio_out.c` builds its refusing ALSA stub.
Four rules live there:

- **The channel count is an argument, never a literal.** `hw:0,0` is stereo-only and the speaker sums L + R
  (both measured, [`../SYSTEM_ANALYSIS.md#34-audio`](../SYSTEM_ANALYSIS.md#34-audio)), so the generator is
  mono and single-sample and `audio_interleave()` the one conversion point; a `frames * 4` with the 4 spelled
  out is only accidentally right. The open reads the GRANTED count back and uses it.
- **ALSA (`plughw:N,0` for `/dev/dspN`) is the only backend, compiled only under `-DAUDIO_OUT_HAVE_ALSA`, which
  both device build paths set — never `__has_include`** (reason and the period request's: comments in
  `audio_out.c`). An ARM build without the define stops at an `#error` rather than shipping silent.
- ⚠️ **A write must never stop mid-frame.** Half a frame handed to the kernel swaps L and R for the rest of the
  stream, permanently, and a stereo-only interface has no mono path underneath to absorb it.
  `audio_write_frames()` is the only code that decides when to stop: on frame boundaries, or it reports
  `misaligned`. Its mid-frame retry is bounded by `AUDIO_ALIGN_TRIES` whatever the caller's policy — an
  unlimited policy against a full sink hangs the render loop, which is worse than the swap. Every write
  goes through it under a **named `AudioWritePolicy`** (`audio_out.c`). Add a policy, never a loop.
- **`audio_out` routes to a Bluetooth A2DP sink (choice `bluetooth`).** Presence is gated on the `/sys/class/bluetooth`
  `hciN:H` link set, with the `plug:bluealsa` probe at most once a second for 10 s after that set changes (ENODEV and the
  buffer trap: [`../SYSTEM_ANALYSIS.md#36-usb`](../SYSTEM_ANALYSIS.md#36-usb)). A newly connected sink takes the stream
  after two sightings, a lost link reopens down the order, a grant ring
  over 2 s is refused. **ScummVM's BT path is not verified**. **A pin:** config key `audio_bt_addr`,
  written only by `cp_audio_set_output`, opens `bluealsa:DEV=<addr>,PROFILE=a2dp` (not `plug:bluealsa:DEV=` — `20-bluealsa.conf` already
  defines `pcm.bluealsa` as type plug and alsa-lib's parser rejects the nesting, inferred from source). Tier order under
  BLUETOOTH and AUTO: pinned headset (`bluealsa-pin`) > any BT sink (`plug:bluealsa`) > USB DAC > onboard, from the pure
  `audio_out_device_for_bt()` (`audio_out_device_for` wraps it); the open fails fast. The probe
  opens `plug:bluealsa` only while the pin is absent; with the stream on another sink, `audio_out_reprobe_due_pin()`
  keeps re-probing and the two-sighting move returns it. OUT cycling keeps the pin; only USE FOR AUDIO on another headset or REMOVE of the pinned one changes it.
- **The fade-out is a MODE of the one oscillator, not a second copy.** `AUDIO_OSC_FADE_OUT` holds frequency and
  amplitude still, so deleting it while collapsing the duplicated generators deletes the fade.
  `AUDIO_OSC_GLIDE` reproduces the old stream generator byte for byte, and split calls equal one long call —
  which lets a caller write whatever the ring will take without a seam.

### Sound assets: sourced, gated, and two ways they fail without saying so

`sounds/fx_*.wav` are **sourced files**, not generated — `sounds/fx_gen.c` and `gen-sounds.sh` are
superseded and ⚠️ **running `gen-sounds.sh` overwrites them.** `sounds/prompts.md` is the authoring record.
⚠️ **They are also the one thing in this repo that is not free for any use** — `../LICENSE.md` is the home
for that, and it is a release concern, not an authoring one.

**`./check-sound-assets.sh` is the gate, and `build-and-deploy.sh` runs it** — format plus in-band energy,
with two opposed sine controls it validates itself against before judging a file. Run it after touching any
asset rather than reasoning about the numbers; it blocks on an unusable file and warns on a weak one.
⚠️ **It cannot hear whether an effect sounds like its name** — in-band energy is audibility, not identity,
and the retired generated set would have PASSED it. That half stays ear-only, on the panel.

- ⚠️ **A wrong sample rate is refused in SILENCE.** `clip_load()` in `common/audio.c` rejects any clip whose
  rate is not the granted 44100, logs one line, and the convenience sound falls back to its note table — so
  the game sounds **unchanged** and reads as "the new asset did not work" rather than as a format error.
  Measured: a sourced effect arrived 48000 Hz / stereo and would have changed nothing on the panel. Every
  asset is **mono / 44100 / 16-bit PCM**.
- ⚠️ **Nothing under ~90 ms.** The mixer puts a 10 ms attack and 20 ms release on every sample voice, each
  clamped to half the clip (`clamp_edge_frames()` in `common/audio_gen.c`), so a 28 ms effect is almost
  entirely envelope and its transient is gone.
- **The content rule is IN-BAND ENERGY, not spectral shape.** This speaker rolls off sharply below its knee
  ([`../SYSTEM_ANALYSIS.md#34-audio`](../SYSTEM_ANALYSIS.md#34-audio)), so an effect whose energy sits under
  it is not quiet, it is absent. ⚠️ **Broadband is not the requirement**: chasing flatness instead cost a
  whole set that measured correct and sounded like noise.
- ⚠️ **The MUSIC / EFFECTS toggles are enforced in the LIBRARY, so never re-check them in a game.**
  `music_enabled` / `effects_enabled` (one writer: `control_panel`'s Audio page, under `audio_enabled`) are
  read once by `audio_init()` and gate `audio_tone()`, `audio_fx_play()`, `audio_sfx_play()` and
  `audio_music_start()` — a second check in a call site is a second place the rule drifts.
  `audio_music_enabled()` / `audio_effects_enabled()` exist to explain a silence in a log line, not to
  decide whether to make a sound. ⚠️ **Both read TRUE on a struct from `audio_init_unchecked()`**,
  deliberately: that bypass is what lets a hardware speaker test make a noise on a silenced device.

- ⚠️ **Every effect's path is already overridable per name — do NOT build a second mechanism for it.**
  `fx_config_apply()` in `common/audio.c` reads one `fx_<name>=` key per `AudioFxId` from
  `/opt/games/rw_config.conf` over `FX_DEFAULT_PATH`: an absent key keeps the stock clip, a **present but
  empty** one selects the note table. A per-GAME variant was declined.

### Mixing: the per-frame pump

Two sounds at once needs userspace to hold the audio and hand the device small pieces of it — you cannot mix
into a buffer the kernel already has. `audio_pump()` does that **from the render loop, never a thread**:
static ARM plus pthread is the `clock_gettime64` SIGSEGV-before-`main()` scar (`../CLAUDE.md`). Two lines:

```c
audio_init(&audio);                           /* opens the stream — the only playback path */
while (running) {
    /* ... */
    audio_pump(&audio);                       /* once per frame, beside fb_swap() */
    usleep((drew || audio_pump_active(&audio)) ? FRAME_DELAY_ACTIVE_US
                                               : FRAME_DELAY_IDLE_US);
}
```

These rules, each of which is a way to get this wrong:

- ⚠️ **The lead is measured in DEVICE PERIODS, never in milliseconds alone.** `audio_pump_lead_frames()` takes
  the period off the device, floors the lead at `AUDIO_PUMP_LEAD_PERIODS` (3) of them and rounds **up** (why
  three, and what a shorter lead sounds like: [§3.4](../SYSTEM_ANALYSIS.md#34-audio) gotcha 5). ⚠️ **The lead
  is also the latency ceiling** — ~139 ms on the shim's 46 ms period; a client asking 23 ms buys that back.
- ⚠️ **The level is `Audio::MixerImpl`'s, and its TWO knobs do different jobs.** A voice's loudness is a
  *fraction* of full scale — `audio_voice_peak(vol)`, ScummVM's `out = (in * vol) / 256` — and
  `AUDIO_PEAK` is derived from `AUDIO_VOICE_VOL`, not a second place to set it. **`vol` is HEADROOM**:
  `n` voices reach the int16 clamp when `n * vol > AUDIO_VOL_UNITY`. **`AUDIO_MASTER_SHIFT` is this
  SPEAKER'S ceiling**, spent once per device in `audio_out` — the same `>>1` ScummVM applies before
  `write()`, and `audio_attenuate()` is the one implementation. ⚠️ **They are not interchangeable
  even though they cancel acoustically**: the shift divides after the clamp has decided what survives,
  so at a fixed acoustic level a lower `vol` with a smaller shift has strictly more headroom. Set them
  with `audio_set_volume()` / `audio_set_master_shift()`; a lone sine's clean peak on this speaker is
  measured, and 18000 is a near-square ([§3.4](../SYSTEM_ANALYSIS.md#34-audio)).
- ⚠️ **`AUDIO_MIX_HARD` is `clampedAdd`, what `audio_mix_init()` sets, and — since 2026-08-20 — what every
  app actually runs** (`level_defaults()`; `tests/audio_tone_test.c` group F). At the shipped `vol` it adds
  no nonlinearity to the ≤ 2 voices a game sums, where `AUDIO_MIX_SOFT` — this repo's own knee, still on
  the panel's `LIM` pad — bends every two-voice sum. ⚠️ **A knee is only correct while it tracks the voice
  amplitude**: one voice must pass byte-identical, so every volume change re-derives it
  (`audio_mix_set_knee()`), and a stale knee *below* the amplitude bends a lone tone. ⚠️ **And
  `clip == 0` is NOT evidence of a clean mix** — it proves int16 did not overflow, which a bounded
  limiter guarantees by construction, and a gate read PASS while the sound was destroyed.
- ⚠️ **The pump targets a LEAD; it never writes into the free space** — an empty ring would accept
  its whole 743 ms (`ALSA_PERIOD_REQ` × `ALSA_PERIODS_REQ` at 44100) and put the next sound that late.
- ⚠️ **The counters are the diagnosis, and each means ONE thing.** `clip` (int16 could not hold it), `lim`
  (the knee bent it — expected under SOFT, not a fault), `starve` (the ring was dry with audio still owed:
  **one audible gap each, pacing not mixing**), `lost` (refused after render), `drop` (full bus). Read them.
- ⚠️ **`audio_pump_active()` must be in the frame-pacing decision.** A loop dropping to
  `FRAME_DELAY_IDLE_US` (100 ms) mid-sound starves the lead and you hear a gap, which reads as a mixing
  defect rather than a pacing one. Ask the library for the ceiling with
  `audio_cont_service_interval_us()`, and ask the component whether it needs frames, exactly as with
  `gameover_needs_redraw(gos, in)`.
- **A voice carries a `delay`, and `audio_success()` depends on it**: simultaneous voices are a *chord*, and
  the canned sounds are note tables plus one sequencer. `audio_interrupt()` resets no ring — a tail survives it.
- ⚠️ **`audio_tone()` chains onto the preceding tone only while that tone is RECENT** —
  `AUDIO_TONE_CHAIN_MS`, half a frame. Unbounded, a tap inherited a 3 s drone's tail and taps stacked
  behind each *other* (last of four at 3800 ms, `tests/audio_tone_test.c` group C); at 0, tetris' and
  snake's two-note motifs collapse into dyads. The tail is ONE voice's, by (slot, generation) — the bus's
  (`audio_mix_pending()`) made the same queue. The ~23 interrupt-then-tone sites are unaffected: tail 0.
- **A full bus refuses and counts (`audio_pump_dropped()`); it never steals a voice.** The longest voice
  is the one a dropped blip must not cut — a streaming music bed, tens of seconds long.
- ⚠️ **A voice is a tone OR a SAMPLE, and the sample PULLS rather than reads.** `audio_mix_add_sample()`
  takes an `AudioVoiceFill` plus a caller-owned buffer, so `audio_gen.c` keeps its no-fd property and
  `audio_wav.c` owns the file — never put a `read()` in the mixer. Pass the **real** `data`-chunk length:
  `audio_mix_render()` skips a bus whose `audio_mix_pending()` is 0, so an under-reporting voice is never
  rendered, and `loop` is therefore a large FINITE total. Stop it with `audio_mix_release_voice()`, which
  shortens `frames` so the envelope fades it — clearing `active` cuts mid-cycle, which is a click.
  `audio.c` owns the fd: `audio_music_start(path, loop)`/`_stop()`/`_pause()`/`_resume()`/`_active()` and
  `audio_sfx_play(path)`, one mechanism used twice, refusing LOUDLY off the bus and on a rate mismatch.
  `tests/audio_sample_test.c`; `tests/audio_mix_test.c` row 5.
- **`audio_cont_fill_mix()` is exported for tests — drive it, never re-implement it.** `tests/audio_path_dump.c`
  renders the production fill through a file-backed `AudioOutDev` to a WAV — the bytes exonerated with no mic.
- ⚠️ **The theremin and the bus cannot both own the stream** — `audio_stream_start()` swaps the fill, so it
  refuses loudly while the bus still owes frames.
- ⚠️ **Every app is on the never-reset stream**, which is what drops the minimum audible tone from ~60 ms
  to 5 ms — `brick_breaker`'s 20–40 ms during-play tones were inaudible under per-sound ring resets.
  ⚠️ **`audio_pump()` goes OUTSIDE the `if (needs_redraw)` block** — a stream
  serviced only on frames that drew is a stream with gaps in it. ⚠️ **`snake` is the exception to the
  two-line shape**: its play sleep IS its step interval (`game.speed`, 150 ms falling to 50), so
  shortening it to feed the stream would make the snake faster — the wait is split into pieces of **half**
  `audio_cont_service_interval_us()` (a ceiling already carrying half a period of margin), and off the bus
  that returns 0, collapsing to one `usleep()`.
- ⚠️ **Never `audio_interrupt()` before an effect on the bus — it means "stop ALL voices".** The idiom cost
  nothing when the kernel ring held exactly one sound; on the bus a 20 ms effect throws away a 600 ms
  fanfare that is still playing, and `samegame`'s removal blip against a level-clear fanfare is the
  constructible case. ⚠️ **The measurement that first motivated the rule is REFUTED and the rule is not**.
  It is gone from all seven games, and **no counter sees it** — a
  voice stopped early is not `lost`, `drop` or `clip`.
- ⚠️ **The four canned sounds play a recorded CLIP when one is configured and their note table when not**,
  which is the audibility fix: every one of those tables is a sustained tone at or below the speaker's knee
  ([§3.4](../SYSTEM_ANALYSIS.md#34-audio)). A clip is loaded WHOLE into RAM and each trigger gets its own
  cursor, so unlike `audio_sfx_play()` it can retrigger — `AUDIO_CLIP_VOICES` of them, then it falls back to
  notes; *Sound assets*' envelope rule above applies to a clip unchanged. ⚠️ **`build-and-deploy.sh` must
  upload `sounds/fx_*.wav` to `/opt/sound` or the fix is inert.** ⚠️ **The same clip fired twice in ONE frame
  sums COHERENTLY — 2× amplitude**, where two tones partially cancel. `tests/audio_tone_test.c` group I,
  controls in `tests/measure_audio_tone_sabotage.sh`.
- ⚠️ **A blocking sub-loop IS a render loop and owes the same per-frame services** — the mixer advances by
  frames RENDERED, so a loop that draws its own screen and services nothing DEFERS the queued sound rather
  than dropping it, which is worse to diagnose. `ui_frame_service()` (`common/common.h`) is the call it
  owes; `audio_open()` registers `audio_pump()` on it. ⚠️ **A new loop that owns the screen must call it.**
  ⚠️ **The CALLER owes the mirror of that: a per-frame service the sub-loop's own screen change should
  trigger must run BEFORE the block that opens it.** `gameover_update()`'s NAME phase sits inside the redraw
  block, so a bed serviced after it never sees `SCREEN_GAME_OVER` and plays for the whole keyboard session —
  servicing the pump does not save it. Hence `audio_bed_service()` sits above that block in every game.
- ⚠️ **A bed is a state machine over `audio_music_active()`, never a flag of the game's own** — and it
  lives ONCE, in `common/audio_bed.{c,h}`, whose header carries the four states and the hold/resume rules:
  level-complete leaves `want_play` (so the bed releases and the next playlist track starts), a **death**
  is `want_hold` (so the fail effect plays over near-silence and a respawn continues the same track), and
  only game over stops it. A game supplies a **tag** and those two predicates, ⚠️ **never its own copy of
  the machine**. ⚠️ **The pump must keep running while a bed is held**, or no resume is accepted. ⚠️ **The
  paths are the SET FILE's and C derives none of them** — `/opt/roomwizard/soundsets/<tag>.sound`, whose
  one generator is `sound-sets.sh`; no file means no music, and `./check-bed-files.sh` gates both
  directions plus that the C and shell install paths agree. ⚠️ **A missing bed is NORMAL**: a refusal is
  permanent **per track** (`tests/audio_tone_test.c` G, H; resolution in `tests/audio_bed_test.c`).
- **`audio_close()` prints the counters — one line, from the library, for every app that ran a bus**, and
  it is what lets an operator’s own play session be measured with no mic — from the launcher it lands in
  `/var/log/roomwizard/app_stdout.log`. ⚠️ **Validate before believing a zero**: `kill -STOP` the game
  for a second, three times, and `starve` reads exactly 3. A dry queue is counted even while the bus is
  SILENT, so a small `starve` is not automatically audible.
- **`check-audio-pacing.sh` is the gate, and it runs from `build-and-deploy.sh`.** It enforces every
  per-frame obligation a converted game owes — the pump, the pacing, the bed serviced above the redraw block,
  and `audio_gameover()` in any game that has a game-over screen. ⚠️ **It reads text, not control flow**, and
  its own header is the authority on both what it checks and what it cannot see; `--self-test` carries a
  fixture control per check, and `starve` catches what the text cannot.
The clamp is a single one after the whole `int32` sum, so slot order cannot change the mix, and it
**counts** — `audio_pump_clipped()`. `tests/audio_mix_test.c` is the interactive tool for the panel
questions: its top row is **LIM | LVL | STOP | DRAW** (DRAW turns the per-press full redraw off; the tap line logs `redraw=`), so the rejected limiter (`SOFT`) and every level rung sit on
the same screen as the shipped shape and "the click is gone" can be A/B'd rather than believed; the stream
itself has no off switch to compare against any more. ⚠️ Its level ladder starts
on the QUIETEST rung and wraps — a loud-to-quiet walk biases adaptation.
⚠️ **An app lives in its own directory, never under `tests/`** — a file there gets read as a test
tool. `theremin/` (`Tap-a-Theremin`) is an **app**, not a diagnostic (it also measured the speaker's usable band, [§3.4](../SYSTEM_ANALYSIS.md#34-audio)).
`tests/audio_mix_test.c` (`Mix Bus Test`) still ships — no tile, launched from `control_panel`'s Audio
page — so nothing under `tests/` is automatically expendable: read `GAMES_BINARIES` before calling a file there a tool.
⚠️ **Never write prose saying 60 ms is a minimum tone length** — nothing clamps it; the floor was the
start-of-stream pop ([gotcha 6](../SYSTEM_ANALYSIS.md#34-audio)).

⚠️ **An `Audio` must be filled by `audio_init()` or `audio_init_unchecked()`, never by hand.** Two tabs used
to `memset` one and set three fields; when the struct gained `channels` that idiom left it at **0**, and a
0-channel byte count is 0 — **silently mute**, measured (`audio_bytes_for_frames(8820, 0)` = 0 against
35280 for 2 channels). `audio_init_unchecked()` bypasses the **`audio_enabled` gate and NOTHING else** — it
resolves `audio_device` itself, both callers having forgotten to and both speaker tests having played on the panel whatever the setting said. One grep, and its expected count in `common/` is
now a hard **zero** — the device path is no longer spelled at any opener:

```bash
grep -rn 'open(DSP_DEVICE\|open("/dev/dsp"' --include=*.c native_apps/ | grep -v arm-deps
```

⚠️ **`audio_out_device_path()` is the ONE home for which `/dev/dsp*` to open**, and
`common/audio_out.c`'s opener — the only one `common/` has — resolves through it, as does ScummVM's mixer — a
USB DAC is selectable, so a second spelling would be a seam only some apps could see. `enable_amp()`
lives there too, for the same reason and because GPIO12 belongs to the panel speaker alone. The
remaining hits are `tests/ch_test.c`, `tests/oss_diag.c`, `tests/oss_geom.c`, `tests/oss_keepalive.c`
and `tests/oss_play.c` — standalone OSS probes with no `Audio` at all, not in `build-and-deploy.sh`,
build lines in their own headers, and each pinned to the onboard device on purpose.

### Hold the bus for the screen, not for one press

**A screen that makes sound owns the bus for as long as it is up.** Open it before the loop (or on
entering the tab), queue sounds as voices — `audio_test_chime()` returns at once and `audio_pump()`
delivers it — and `audio_close()` on the exit paths only: `control_panel`'s Audio page is the example. The
wrong shape — `audio_init*` → tones → `audio_close()` inside one button handler — costs a stream open
**and** a stream stop per press, and freezes the UI while the handler holds the tones.

**Measured on `.188` 2026-09-05**, on that wrong shape, from `/var/log/roomwizard/app_stdout.log`: one
`control_panel` process logged fifteen `bus closed` lines — six at `services=28` and nine at
`services=80` — against one line per whole game session (`services=123` Tap-a-Theremin, `services=142` SameGame, the latter the only
`starve=1` seen). Every diagnostic line read `starve=0 lost=0 drop=0 lim=0 clip=0`, so the cost is the
transitions and not the mixing.

Both transitions are audible ([`../SYSTEM_ANALYSIS.md#34-audio`](../SYSTEM_ANALYSIS.md#34-audio)), which
is why a click is heard on the first press after a gap and again ~5 s after the last. §3.4 records the
stop-click with the power-down held *off*, whereas repeat presses at the default `pmdown_time` were
reported inaudible. Those two have never been listened to
together; do that before promising any screen is click-free.
