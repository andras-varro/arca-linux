# ScummVM Arca Backend

Custom ScummVM backend for the Steelcase RoomWizard embedded device. Runs classic point-and-click adventure games on 800×480 framebuffer with touch input.

## Quick Start

### Build
```bash
./build-and-deploy.sh               # build only;  <ip> to deploy too;  --help lists the commands
```

`build-and-deploy.sh` is the only maintained recipe. It configures with `--host=arm-linux-gnueabi`,
`CC`/`CXX` carrying `-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp`, zlib/libpng from
its own `arm-deps-softfp/`, and links **dynamically** against the device's glibc, `libstdc++` and
`libasound` — the alsa-lib headers come from `../native_apps/build-alsa-lib.sh`, and
`-DAUDIO_OUT_HAVE_ALSA` goes on `DEFINES` (an ARM build without it stops at `#error`).

> **Note:** `build-and-deploy.sh` auto-restores backend files from `backend-files/` into the ScummVM build tree before each build, so manual `manage-scummvm-changes.sh restore` is no longer needed.

### Deploy
```bash
scp scummvm root@192.168.50.73:/opt/games/
ssh root@192.168.50.73 'chmod +x /opt/games/scummvm'
```

ScummVM appears in the launcher through its `.app` manifest (`args=none`: it opens the framebuffer and input devices itself).

### Touch Calibration

Run once, on the device screen: **Control Panel → Input → `CALIBRATE`**.
(The standalone `/opt/games/unified_calibrate` binary was folded into that wizard and removed.)

- Tap 11 targets, 3 times each; a per-axis least-squares fit over the **interior** targets maps
  raw → panel, and a check screen shows the reach and per-axis verdict before anything is applied
- Writes both lines of `/etc/touch_calibration.conf` — loaded automatically by ScummVM and all
  native apps
- **Redeploy ScummVM after any change to `native_apps/common/touch_input.c`**: it links its own
  copy, so a deployed binary keeps whatever it was built with

### Screen area

Once the panel's touch reach has been measured (the wizard's `REACH` step), ScummVM keeps both the
game picture and its own GUI inside the part of the screen a finger can actually reach — the
digitizer saturates a little before the panel edge, so a band at each end is visible but not
pressable, and a game's verb bar or the launcher's button row must not land there.

The GUI (launcher, global menu, virtual keyboard) always stays inside that rectangle. The **game
picture** can be given the whole screen back if you prefer the extra size to the reachability:

```bash
ARCA_CONTENT_AREA=visible /opt/games/scummvm     # one run
```

To make it permanent, set `arca_content_area = visible` under `[scummvm]` in
**`/opt/games/scummvm.ini`** — one absolute path, regardless of how ScummVM was started. The key is
written out with its `safe` default on first run, so it is already in the file and you are editing a
line rather than guessing a name. The environment variable still takes precedence over it. Before the
panel has been swept, `safe` and `visible` are identical.

(Until 2026-08-03 the config path was resolved against the *working directory*, so the boot launcher
used `/scummvm.ini`, an SSH shell in `$HOME` wrote `/home/root/scummvm.ini`, and editing the wrong one
looked like the setting being ignored. Both strays are gone — `initBackend()` resolves the path
against `/opt/games` now, and `scummvm-arca/CLAUDE.md` has the reasoning.)

### Games

**No game data ships with this project** — not in a release bundle and not in a card image; commercial
titles are copyrighted, and the build and image scripts refuse ScummVM config and saves. Bring your
own: copy a game's files to a directory on the unit (e.g. `scp -r <game>/ root@<ip>:/home/root/data/games/<game>/`),
then add it from ScummVM's launcher with **Add Game…**.

Free, legally downloadable games — Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the
Temptress, Drascula, DreamWeb, Soltys and others — are listed on **<https://www.scummvm.org/games/>**.
The default build has the `sky` and `queen` engines, so the first two run as-is; the others need an
engine from a later batch (`ENGINE_BATCH=1 ./build-and-deploy.sh …`; the lists are in that script). Prefer the floppy versions: a few MiB
each, against 66–230 MiB for the CD ones.

## Status

✅ Fully functional — GUI, touch, keyboard, mouse, gamepad, audio (OPL/AdLib music + SFX), virtual keyboard all working. All input verified with EcoQuest and Full Throttle.
See [SCUMMVM_DEV.md](SCUMMVM_DEV.md) for architecture, audio design, and optimization history.

## Input Devices

All four input methods work **simultaneously** — users can freely mix touch, keyboard, mouse, and gamepad at any time. Input handling is implemented in [`arca-events.cpp`](backend-files/arca-events.cpp) / [`arca-events.h`](backend-files/arca-events.h).

### Touch

Existing behaviour preserved:
- Tap = left click, long-press = right click
- Corner gestures for ScummVM shortcuts
- Bezel-aware coordinate mapping (uses `/etc/touch_calibration.conf`)

### USB Keyboard

Full key mapping for adventure game interaction:

| Key | ScummVM Action |
|-----|----------------|
| F5 | Open save/load dialog |
| Ctrl+F5 | Global Main Menu (GMM) |
| Space | Skip cutscene |
| Escape | Skip dialog / cancel |
| Enter | Confirm selection |
| Arrow keys | Navigate menus / move cursor |
| Letters/Numbers | Type save game names, in-game input |

All standard keys are forwarded — letters, numbers, F-keys, arrows, modifiers, and punctuation.

### USB Mouse

Direct cursor control with 3-tier acceleration (same as native apps):
- Left click = left click, right click = right click. **Middle click opens the Global Main Menu**
  (it is synthesised into Ctrl+F5, `backend-files/arca-events.cpp:821`) — it is not forwarded as
  a middle click, because ScummVM games do not use one and a menu needs a mouse-only way in
- Precise point-and-click gameplay — ideal for adventure games
- Mouse sensitivity configurable via `/etc/input_config.conf`

### USB Gamepad

D-pad and analog stick control the on-screen cursor. Full button mapping:

| Gamepad Button | ScummVM Action |
|----------------|----------------|
| A (South) | Left click |
| B (East) | Right click |
| X (West) | Skip cutscene (Space) |
| Y (North) | Escape / cancel |
| Start | Global Main Menu (Ctrl+F5) |
| Select | Save/load dialog — F5 |
| L Bumper | Skip dialog text |
| R Bumper | Skip dialog text |
| D-pad / Left Stick | Move cursor |

### Configuration

Mouse sensitivity and gamepad button mapping are configurable via `/etc/input_config.conf` (shared with native apps — see [native_apps/README.md](../native_apps/README.md#input-configuration)).

### Auto-Detection

Devices are auto-detected by watching the `/dev/input` fingerprint every 1 s and rescanning on a change. USB hotplug is fully supported — plug in a controller mid-game and it works immediately.

## Backend Files

Version-controlled in [`backend-files/`](backend-files/), synced to/from `../scummvm/backends/platform/arca/`:

| File | Purpose |
|---|---|
| `arca.cpp/h` | Main backend, VKB, feature flags |
| `arca-graphics.cpp/h` | 800×480 framebuffer, bezel-aware scaling, overlay compositing |
| `arca-events.cpp/h` | Touch + keyboard + mouse + gamepad input, state machine, corner gestures |
| `oss-mixer.cpp/h` | Audio adapter: mixes ScummVM into `../native_apps/common/audio_out.c`, which plays through ALSA (`plughw:N,0`) |
| `module.mk` | Build configuration |
| `configure.patch` | ScummVM configure modifications |

Sync after editing:
```bash
bash manage-scummvm-changes.sh sync     # scummvm/ edits → backend-files/
bash manage-scummvm-changes.sh restore  # backend-files/ → scummvm/
bash manage-scummvm-changes.sh status   # check configuration
```
