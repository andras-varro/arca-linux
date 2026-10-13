# vnc_client — authoring guide

Rules and traps for working on the VNC client. User-facing usage, configuration and
troubleshooting are in [`README.md`](README.md). Device facts are in
[`../SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md). Open work is in
[`../IMPROVEMENT_PLAN.md`](../IMPROVEMENT_PLAN.md).

A lightweight libvncclient viewer that renders a remote desktop to the framebuffer and forwards
touch, USB keyboard and USB mouse back to the server.

## Build and deploy

```bash
./build-deps.sh                       # once: cross-builds zlib, libjpeg-turbo, LibVNCClient into deps/
./build-and-deploy.sh <ip>            # build + deploy (also: set-default, run)
```

**Use `build-and-deploy.sh`, not `make deploy`.** The Makefile's deploy target copies only the
binary and the config; it skips the `.app` manifest and PPM icon, so the app silently disappears
from the launcher grid. The Makefile is fine for compiling.

**Do not add a boot init script.** Boot is owned by `/etc/init.d/arca-app`, which respawns
whatever `/opt/arca/default-app` points at. `./build-and-deploy.sh <ip> set-default` is the
supported way to make this the boot app.

`vnc_client.conf` is gitignored because it holds a plaintext password; `vnc_client.conf.example`
is the tracked template. The deploy script installs it mode 0600 and will not overwrite an
existing config on the device.

## Framebuffer: this component runs 16bpp

`vnc_client.c` calls `fb_set_bpp(..., 16)` and the renderer writes `uint16_t` RGB565 directly
into the back buffer. Two consequences:

- **Screenshots need `--bpp 16`.** Native apps run 32bpp; the mode depends on which app ran last.
- **Do not call the `native_apps/common` draw helpers from this component.** This component draws by
  hand-writing `uint16_t`, and mixing the two is how the 16bpp overflow got in: `fb_clear()` and
  `fb_draw_pixel()` once wrote 4 bytes per pixel unconditionally, so a single `fb_draw_text()` call at
  16bpp overflowed the back buffer by 768 KB. ⚠️ **Measured 2026-08-15: they are bpp-aware now** —
  `fb_clear()` sizes its `memset` in bytes and packs 565 otherwise, `fb_draw_pixel()` goes through
  `fb_store(…, FB_IS_16BPP(fb))`. So the overflow itself is fixed; what has *not* happened is anything
  calling them at 16bpp. The restriction stands until something exercises that path on the panel.

## Screen size is runtime, not 800×480

`fb_init()` shrinks the drawing surface to the rectangle the bezel leaves visible — **800×450**
at the shipped `15 15 0 0` margins, and whatever the config says otherwise. So the back buffer
is `fb->width × fb->height`, and that is also the stride for every `uint16_t` index into it.

- **Never use a compile-time screen dimension.** `PANEL_MAX_WIDTH` / `PANEL_MAX_HEIGHT` in
  `config.h` bound the *physical panel* and exist only to size the fixed `src_x_lut` /
  `temp_row` arrays. They are not the screen.
- **`fb->screen_size` is not the back-buffer size.** It is the physical mmap size
  (`line_length × panel height`). Clearing the back buffer with it overruns the allocation by
  48 KB at 16bpp. Use `fb->back_buffer_size` — that is what `vnc_renderer_clear_screen()` does.
- **This component never compensates for the bezel itself**, and must not start: `fb_swap()`
  already places the logical surface at `fb->view_x/view_y`. Letterboxing against 480 instead
  of 450 is what put a 16 px black bar at the top of the remote desktop and wrote 30 rows past
  the back buffer.
- **Layout anchored to the bottom must be computed from `SCREEN_SAFE_BOTTOM`**, and the draw path
  and the hit-test path must call the *same* helper — see `settings_act_btn_y()`, `kp_btn_y()` and
  `fkp_panel_h()` in `vnc_settings.c`. A constant in one and a helper in the other draws
  buttons where they cannot be tapped.

## Everything here goes in the touch-safe rectangle

The digitizer saturates before the panel edge, so a band at each end of the *visible* surface is
drawable but **not pressable** (~19 px top / ~16 px bottom / ~6 px each side on RW09; measured at
runtime, `0` until a panel's edge reach has been swept). `native_apps` splits its call sites between
`SCREEN_VISIBLE_*` and `SCREEN_SAFE_*` by asking whether each thing is *seen* or *pressed*.

**That audit is impossible here.** The remote desktop is not ours: it may put a taskbar on its
bottom row, and we have no way to know. So the only safe assumption is that *all* of it must be
pressable, and the whole component works in `SCREEN_SAFE_*`:

| What | Rectangle | Where |
|---|---|---|
| the remote desktop's letterbox | `vnc_content_rect()` — the safe rect, or the whole surface if the user opted out | `vnc_renderer.c` |
| exit gesture zone, its progress bar | safe rect, always | `vnc_input.c`, `vnc_client.c` |
| reconnect button row | safe rect, always | `vnc_client.c` `reconnect_ui()` |
| settings screen, both keypads | safe rect, always | `vnc_settings.c` layout helpers |
| stride and bounds of the back buffer | `fb->width` / `fb->height`, unchanged | everywhere |

`content_area = safe | visible` in the config (default `safe`) opts the **picture** out and hands it
the whole visible surface, trading reachability for ~11 % more pixels on a 1080p desktop. It
deliberately does **not** move anything in the table's "always" rows: an option that could strand
the SAVE button is a footgun, not a feature. Read it once in `load_config_file()` and publish it with
`vnc_content_set_full()`; `vnc_settings.c`'s config writer rewrites the whole file, so it must emit
the key or SAVE silently resets the user's choice.

It is also the settings screen's **CONTENT** row (seventh, `TOGGLE` button). Two things that row
depends on:

- The renderer keeps the flag in a **static** that only `vnc_renderer_set_remote_size()` reads, and
  that runs once per session — so both `SETTINGS_SAVE` sites in `vnc_client.c` must re-publish with
  `vnc_content_set_full()` before reconnecting, or the row appears to do nothing until a restart.
- A seventh row does not fit a fixed 52 px pitch (that is why the control was config-only until
  2026-08-03). `settings_row_pitch()` divides the space between `FIRST_ROW_Y` and the status line,
  capped at the old 52 — so **row height is runtime**, like everything bottom-anchored, and the draw
  path and the hit-test path must both go through `settings_row_y()` / `settings_row_h()`.

**Ordering:** `SCREEN_SAFE_*` is only correct after **both** `fb_init()` and `touch_init()`, in that
order — which is what `run_vnc_client()` already does. The bottom-anchored gap constants
(`ACT_BTN_BOTTOM_GAP`, `KP_BOTTOM_GAP`) are now *visual* gaps only; they used to reserve 20 px by
hand for the dead band, and reserving it twice pushes the status line onto the last settings row.

Numbers and method: [`../SYSTEM_ANALYSIS.md#33-touch`](../SYSTEM_ANALYSIS.md#33-touch).

## Shared code

Links `framebuffer.o`, `touch_input.o`, `hardware.o`, `config.o`, `logger.o`, `input_scan.o` and `ui_focus.o` from
`../native_apps/common/`. It has **no evdev scanner of its own**: `vnc_input.c` calls `input_scan()`
(`common/input_scan.h`), which classifies by `gamepad.c`'s rules, opens every node of a kind up to a cap
and skips held nodes — a rescan is another call. It does not link `gamepad.o`: that opens its own nodes, keyboards
and mice included, which would double-open the ones this node set holds, so `vnc_input.c` reads the pad
nodes itself (kind `INPUT_KIND_PAD`, codes from `/etc/input_config.conf`) and `vnc_pad.c` holds the pure mapping.
`gamepad.c` and the ScummVM backend call the same scanner, so a change to the classification rules lands in all three.

## Network robustness

The remote end is untrusted input. One known gap remains:

- **Validate server-supplied geometry.** `vnc_malloc_fb()` and the scaling setup divide by and
  allocate from the server's announced width/height with no bounds check — a zero dimension is a
  SIGFPE. The bilinear X-LUT also overflows `int` for very wide desktops, producing a negative
  source index and an out-of-bounds read.

Two that are fixed, both worth not undoing:

- **Session teardown goes through `vnc_client_destroy()`, never bare `rfbClientCleanup()`.** Cleanup
  does not free `client->frameBuffer` — `vnc_malloc_fb()` owns it — so every reconnect leaked a full
  framebuffer, ~8.3 MB against a 1080p host on a 234 MB device — OOM after ~25 reconnects.
- **Dead peers are detected by TCP keepalive, not by an application timeout.**
  `vnc_enable_keepalive()` runs after `rfbInitClient()` (no socket before that) and sets idle 20 s /
  3 probes / 10 s apart, so a silent TCP death surfaces in ~50 s — inside the 60 s hardware watchdog
  — as a `WaitForMessage() < 0`, which the existing "connection lost" branch already handles.
  ⚠️ **Do not add "break after N seconds with no server message."** Steady-state update requests are
  *incremental*, so a static remote desktop legitimately sends nothing for minutes; a silence timeout
  would disconnect exactly the healthy wall-dashboard case this component exists for.

## Rendering

Bilinear downscale (typically 2.4:1) plus BGR→RGB565 conversion, with partial updates driven by
the server's dirty rectangles. Techniques carried over from the ScummVM backend: precomputed
bilinear X-LUT (no per-pixel division), border-only clearing, row deduplication via an
L1-resident temp row (~57 % of scaled rows are duplicates), 16bpp to halve `fb_swap` cost, and a
frame-rate-capped present.

The 24bpp and 32bpp conversion paths disagree about R/B byte order, and anything other than 3 or
4 bytes per pixel leaves `temp_row` unfilled but still blits it. Currently unreachable because
non-32bpp server formats are rejected up front — a trap if that guard is ever relaxed.

## Watchdog

This app takes over the screen for long periods. The hardware watchdog is a 60 s timer; a
fullscreen app that stops feeding it gets the device hard-reset. If the app crashes without
cleanup, expect a reboot within ~60 s.

## Input

Touch maps to pointer events; USB keyboard maps through a keysym table; USB mouse gets 3-tier
acceleration (<3 px 1:1, 3–10 px 2×, >10 px 4×) — the same constants as `common/gamepad.c` and
the ScummVM backend — three copies of one table, which is the argument for extracting a shared layer.

Hotplug: the `/dev/input` fingerprint is checked every 1 s and devices are rescanned only on a change. Configuration is
`/etc/input_config.conf`, documented once in
[`../native_apps/README.md`](../native_apps/README.md#input-configuration).

`release_remote_buttons()` runs before every exit (corner hold, pad Select hold, keyboard Esc hold), so a drag that enters the exit zone leaves no remote button down.

**Pad** (`vnc_pad.c`, pure; `native_apps/tests/vnc_pad_test.c`); the button mapping is in [`README.md`](README.md#usb-game-pad). Motion is integrated by elapsed time
(`vnc_pad_motion`: a gap over `VNC_PAD_MAX_STEP_MS` counts as that long, a sub-pixel carry is kept, full-deflection speed is
two thirds of the remote width per second clamped to `VNC_PAD_MIN_SPEED`..`VNC_PAD_MAX_SPEED`, d-pad is `VNC_PAD_DPAD_NORM`);
A / B are held levels (so a drag works), a shoulder press is a one-shot wheel pulse, and Select held `UI_HOLD_EXIT_MS` (`UiHold`)
opens Settings, like the 3 s corner hold and a held keyboard Esc (`vnc_esc_event` / `vnc_esc_hold_exit`; a short tap is sent to the remote on release, the remote never sees Esc held). A physical keyboard types into the open Settings keypads only (pure `vnc_key_char`, `vnc_kp_key`, `vnc_kp_insert` in `vnc_pad.c`; host test `native_apps/tests/vnc_pad_test.c`); the reconnect screen shares Settings' input (`vnc_nav_open`/`vnc_nav_poll`) and drawing (`vnc_draw_focus_ring`/`vnc_draw_pointer` in `vnc_settings.c`), so pad, keyboard and mouse work there (operator-verified); a pointer move repaints the whole screen.

## Settings GUI

`vnc_settings.c/h` is a settings screen with a full alphanumeric keypad; all fields are
editable. It opens its own keyboard, mouse and pad nodes while open (the session closed its nodes; a mouse gives a drawn RGB565 arrow, left click = tap on release, a press and release inside one 33 ms poll is lost). Focus targets come from the
same geometry as the tap hit-test; the ring shows on the first nav key and a touch hides it; A / Enter / Space tap through
`ui_tap_begin`, B / Esc is Back / Cancel; buttons held on entry and Select are ignored. The full-keypad action row is one
shared table. It writes the config with no `fchmod`, so the file lands 0644 with the password in
cleartext, and it renders the password in plain text on a wall-mounted panel while editing. Fix
both if you touch this code.

## 32-bit target

`sizeof(long) == 4`. `get_ticks_ms()` in `vnc_input.c` currently does `tv.tv_sec * 1000`, which
is signed overflow; it works only because callers use it as a `uint32_t` difference. Baseline to
a start timestamp captured at init instead.
