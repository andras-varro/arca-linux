# Native Apps for Arca

Native C apps and tools for the Steelcase RoomWizard (600MHz ARMv7, 800×480 framebuffer). Direct framebuffer rendering — no browser, no overhead.

See [CLAUDE.md](CLAUDE.md) for how to write code here, and [../IMPROVEMENT_PLAN.md](../IMPROVEMENT_PLAN.md) for open work.

## Table of Contents

1. [Apps & Tools](#apps--tools)
2. [Input Support](#input-support)
3. [Build & Deploy](#build--deploy-cross-compile-from-wsl)
4. [App Launcher](#app-launcher)
5. [App Manifests](#app-manifests)
6. [Permanent App Mode](#permanent-app-mode-boot)
7. [Resources](#resources)

---

## Apps & Tools

| Binary | Type | Controls / Notes |
|---|---|---|
| `snake` | Game | Tap / arrow keys / D-pad to steer |
| `frogger` | Game | Tap / arrow keys / D-pad to hop |
| `tetris` | Game | Tap / keys / D-pad to move, rotate, drop (DAS auto-repeat) |
| `pong` | Game | Touch-drag / keys / analog stick for paddle |
| `brick_breaker` | Game | Touch / mouse / keys / analog stick for paddle — pause menu toggles TEST MODE (explosive/power-up levels) |
| `samegame` | Game | Touch / mouse cursor + keyboard navigation |
| `platformer` | Game | Touch / keys / gamepad — reference input implementation; pause menu toggles TRAINING (10 lives, +1 per 50 coins) |
| `app_launcher` | Launcher | Visual grid launcher — keyboard/mouse/gamepad nav, auto-starts on boot |
| `control_panel` | Tool | **Unified hardware app** — the one you want; `control_panel <page>` opens one page directly (over SSH: `/etc/init.d/arca-app stop`, then `/opt/games/control_panel /dev/fb0 /dev/input/event0 system`, then `start`; pages `audio display led usb bluetooth input network monitor system information`, case-insensitive; `--list-pages` prints them). Home icon grid; tabs: Settings, Tests, Display; pages: LED, Monitor, Network, Information, USB, System (SSH mode read-only, clock, date/time editor) |
| `theremin` | Toy | "Tap-a-Theremin" — touch-controlled tone generator |
| `touch_raw` | Tool | Digitizer reach: no calibration, no bezel — live crosshair + interior-only fit (hidden) |

Tools marked *hidden* have no manifest, so they get no launcher tile but remain runnable over SSH.
USB testing lives in the `control_panel` Input page. The system `/usr/sbin/watchdog` daemon handles the
hardware watchdog.

`touch_raw` needs the framebuffer at 32 bpp and asserts that itself — run `fbset -depth 32` first if ScummVM or
`vnc_client` left it at 16. Stop the launcher before running it (`/etc/init.d/arca-app stop`), and start it
again afterwards. The calibrated finger trail lives in `control_panel` Input -> MULTI-TOUCH (drawn for the primary contact only;
80 px grid, trail, RAW/CAL/LIN readouts; also logs to `/tmp/touch_trace.log`).

### Control Panel

[`control_panel/control_panel.c`](control_panel/control_panel.c) holds every hardware setting
and test behind an icon grid, one page per icon (`control_panel/*_page.c`):

| Page | What it does |
|---|---|
| **Audio** | Audio on/off, music/effects, output device, chime test, Mix Bus Test; saved on each change. |
| **Display** | Backlight, portrait toggle, the VISIBLE/EDGES/TOUCHABLE readout, BACKLIGHT RAMP, TEST PATTERNS and SCREEN EDGES. |
| **LED** | Enable, brightness and the six LED tests, each full-screen. |
| **USB** | The device list, RESCAN and port recovery. |
| **Input** | Touch CALIBRATE (the wizard that writes both lines of `/etc/touch_calibration.conf`, see below), DIAGNOSTIC, MULTI-TOUCH, RESET GEOMETRY, and keyboard, mouse and gamepad testers that enable only when such a device is present. |
| **Network**, **Monitor**, **Information** | Read-only system pages; Information also holds RESET DEFAULTS. |

#### The calibration wizard

CALIBRATE (Input page) refuses to run in portrait; the Display page's portrait toggle carries a
"CALIBRATE IN LANDSCAPE" note.

CALIBRATE runs a five-step wizard that writes **both** lines of
`/etc/touch_calibration.conf`. Every step runs with the bezel zeroed on the full 800×480 panel,
so a drawn pixel is a panel pixel:

| Step | What you do |
|---|---|
| `TAP` | Tap 11 targets, 3 times each. They sit well inside the edges on purpose. |
| `CHECK` | Read the fitted range, the reach, a **per-axis** verdict and the edge-probe residuals. `ACCEPT` / `REDO` / `RESET`. |
| `EDGES` | Raise each margin until its yellow line clears the plastic, against a numbered 2 px ladder. |
| `REPORT` | Visible rectangle vs touchable rectangle, with the per-edge gap spelled out. |
| `CONFIRM` | The new mapping goes live for 20 s. Press `KEEP THESE` or it reverts on its own. |

SCREEN EDGES (Display page) jumps straight to `EDGES` for a margins-only tweak. `RESET` puts both lines back to
the hardware `EVIOCGABS` range and the default margins — the escape hatch if a calibration ever
leaves the screen hard to press.

**Nothing is written until `CONFIRM`,** the previous file is copied to `.bakN` first, and every
button before that step is hit-tested through the *old* calibration, so a bad fit can never leave
you unable to press the button that rejects it.

---

## Input Support

All native apps share a unified input system built on [`gamepad.h`](common/gamepad.h) / [`gamepad.c`](common/gamepad.c). This library provides a single polling API that unifies four input sources — **touch**, **USB keyboard**, **USB mouse**, and **USB gamepad** — into a common abstract button model.

### Abstract Button Model

Every input device maps to the same set of abstract buttons, allowing games to be written against a single API:

| Button ID | Purpose |
|-----------|---------|
| `BTN_ID_UP` | Direction up / navigate up |
| `BTN_ID_DOWN` | Direction down / navigate down |
| `BTN_ID_LEFT` | Direction left / navigate left |
| `BTN_ID_RIGHT` | Direction right / navigate right |
| `BTN_ID_JUMP` | Primary action (jump / select) |
| `BTN_ID_RUN` | Secondary action (run / sprint) |
| `BTN_ID_ACTION` | Tertiary action (interact / confirm) |
| `BTN_ID_PAUSE` | Pause game |
| `BTN_ID_BACK` | Back / exit to launcher |

### Common Controls

| Action | Keyboard | Gamepad | Purpose |
|--------|----------|---------|---------|
| Up | Arrow Up / W | D-pad Up / Left Stick | Direction / Navigate |
| Down | Arrow Down / S | D-pad Down / Left Stick | Direction / Navigate |
| Left | Arrow Left / A | D-pad Left / Left Stick | Direction / Navigate |
| Right | Arrow Right / D | D-pad Right / Left Stick | Direction / Navigate |
| Jump/Select | Space | A (South) | Primary action |
| Run | Shift | B (East) | Secondary action |
| Action | Enter | X (West) | Tertiary action |
| Pause | Escape | Start | Pause game |
| Back/Exit | Backspace | Select | Exit to launcher |

### Mouse Support

USB mice provide direct cursor control with **3-tier acceleration**:

- **Slow** (< 3 px movement) — 1:1 pixel mapping for precision
- **Medium** (3–10 px) — 2× multiplier
- **Fast** (> 10 px) — 4× multiplier

Mouse sensitivity is configurable via `/etc/input_config.conf`.

On the two home screens (`app_launcher`, `control_panel`) the mouse moves an on-screen arrow and the **left button** is a touch, acting on release; wheel, right and middle buttons do nothing there. A game pad does **not** move that arrow — it moves the focus ring (D-pad / stick), **A** activates, and Select / Start on the home screen leaves (`common/pointer.c`, `pointer_update`, reads only `mouse_*`; called at `app_launcher.c:744`, `control_panel.c:964`).

### Gamepad Button Mapping

Gamepad button mapping is configurable to support clone/third-party controllers that may report different button codes than standard Xbox/PlayStation layouts. Remap buttons in `/etc/input_config.conf` (see [Configuration](#input-configuration) below).

### Per-App Input Matrix

| App | Touch | Keyboard | Mouse | Gamepad | Notes |
|-----|:-----:|:--------:|:-----:|:-------:|-------|
| Snake | ✅ | ✅ | — | ✅ | Arrow keys / D-pad for direction |
| Frogger | ✅ | ✅ | — | ✅ | Arrow keys / D-pad for hopping |
| Tetris | ✅ | ✅ | — | ✅ | DAS auto-repeat, hard drop, rotate |
| Pong | ✅ | ✅ | — | ✅ | Analog stick proportional paddle |
| Brick Breaker | ✅ | ✅ | ✅ | ✅ | Mouse/analog for paddle, full control |
| SameGame | ✅ | ✅ | ✅ | ✅ | Mouse cursor + hover highlight |
| Platformer | ✅ | ✅ | — | ✅ | Reference implementation |
| App Launcher | ✅ | ✅ | ✅ | ✅ | Grid nav + Enter/A select, 500ms post-launch cooldown |
| USB Test | ✅ | ✅ | ✅ | ✅ | Device diagnostic visualizer |

### USB Hotplug

All apps scan `/dev/input/event*` for newly connected USB devices (a `/dev/input` fingerprint check **every 1 s**, rescanning only on a change), so an app never
holds a stale device handle: whenever a node appears, the app picks it up within seconds and no restart
is needed.

⚠️ **That covers the app layer only, and it cannot create a node that the kernel never made.** Measured
2026-08-13 on `.188`: MUSB powers the port **only for a device that is attached when the driver probes**.
Boot with the port empty and it stays unpowered for that whole boot — anything plugged in afterwards is
never even given VBUS, so there is nothing for the rescan to find. Once the port *is* live, an
unplug/replug re-enumerates normally (measured across a 95 s gap), and then the rescan works exactly as
advertised.

The only recovery reachable from userspace is a driver re-probe:

```sh
/etc/init.d/usb-host recover      # plug the device in FIRST, then run this
```

⚠️ **Do not use `echo host > …/musb-hdrc.0.auto/mode`.** Earlier versions of this file recommended it. It
is a **silent no-op** on this SoC — `omap2430_ops` has no `.set_mode`, so the store returns success having
done nothing. `mode` does not show a live port (it reads `a_idle` with a pad enumerated and working), but a
`b_*` reading means the port died after `VBUS_ERROR` even while devices are still listed; the control panel's
RESCAN recovers on it. Otherwise the reading that distinguishes live from dead is `$MUSB/vbus`.

Detail: [`../SYSTEM_ANALYSIS.md#36-usb`](../SYSTEM_ANALYSIS.md#36-usb); it is tracked as open work in
[`../IMPROVEMENT_PLAN.md`](../IMPROVEMENT_PLAN.md). Until there is an automatic fix, plug
peripherals in **before** you power the unit, or run the `recover` command above.

### Input Configuration

`/etc/input_config.conf` is read by one parser (`input_config_parse_line()` in `common/input_scan.c`) for every native app, `vnc_client` and the ScummVM backend. One `key=value` per line; `#` lines, blank lines, empty values, unknown keys and out-of-range numbers are skipped, so every key is optional.

| Key | Accepted | Default |
|---|---|---|
| `mouse_sensitivity` | float, 0.1 < v < 20 | 1.5 |
| `mouse_acceleration` | float multiplier, 0.1 < v < 20 | 2.0 |
| `mouse_low_threshold` / `mouse_high_threshold` | int, 0–99 / 1–499 (pixels per event) | 3 / 15 |
| `gamepad_deadzone` | int percent of half-range, 0–100 | 25 |
| `gamepad_btn_jump` `_run` `_action` `_pause` `_back` `_north` `_tl` `_tr` | any int, evdev button code (xpad layout) | 304 305 308 315 314 307 310 311 |
| `gamepad_hat_x` / `_hat_y`, `gamepad_stick_lx` `_ly` `_rx` `_ry` | any int, evdev axis code | 16 17, 0 1 3 4 |

`_north`, `_tl`, `_tr` are used by ScummVM only and the right stick by native apps only.

---

## Build & Deploy (cross-compile from WSL)

```bash
cd native_apps

# Build only
./build-and-deploy.sh

# Build + deploy binaries + manifests
./build-and-deploy.sh 192.168.50.53

# Build + deploy + set app launcher as default boot app
./build-and-deploy.sh 192.168.50.53 set-default
```

## App Launcher

The `app_launcher` provides a visual grid interface for launching apps:

- Scans `/opt/arca/apps/*.app` manifest files
- Displays apps as coloured icon tiles in a 3×2 grid
- Supports PPM icons or auto-generated letter tiles
- Touch tile to launch, edge touch for pagination
- The red X top right (or Back / Escape) opens Shut down / Reboot / Cancel — the only shutdown and reboot control on the device
- Re-scans manifests after each app exits (picks up new deployments)
- Respawns automatically via the init script if it crashes

## App Manifests

Each app is registered via a `.app` manifest in `/opt/arca/apps/`:

```ini
name=Snake
exec=/opt/games/snake
icon=/opt/arca/icons/snake.ppm
args=fb,touch
```

Fields:
- `name` — Display name (required)
- `exec` — Absolute path to executable (required)
- `icon` — Path to PPM P6 icon file (optional, auto letter-tile if absent)
- `args` — Argument mode: `fb,touch` (default), `fb`, `touch`, or `none`

The script cross-compiles all binaries, uploads them to `/opt/games/`, and sets permissions.

To rebuild a single app, run `./build-and-deploy.sh` — it is fast and always links the
correct object set. Hand-rolled single-file compile lines go stale as `common/` grows
and will fail to link.

---

## Permanent App Mode (boot)

```bash
./build-and-deploy.sh 192.168.50.73 set-default
```

This writes `/opt/arca/default-app`; the init service respawns whatever it points at.
Installing the service itself is done once by `../commissioning/provision.sh`.

Or manually: `ssh root@<ip> '/etc/init.d/arca-app start|stop|status'`

## Resources

- **Device / SSH setup:** [COMMISSIONING.md](../COMMISSIONING.md)
- **Hardware specs:** [SYSTEM_ANALYSIS.md](../SYSTEM_ANALYSIS.md)
- **ScummVM backend:** [scummvm-arca/SCUMMVM_DEV.md](../scummvm-arca/SCUMMVM_DEV.md)
