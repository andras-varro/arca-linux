# ScummVM Arca - Development Guide

## Status: ✅ GUI + touch + keyboard + mouse + gamepad + audio (OPL/AdLib music + SFX) working

**Binary:** dynamically linked (soft-float; the device's glibc, `libstdc++` and `libasound`) → `/opt/games/scummvm` on device  
**Version:** ScummVM 2.8.1pre with custom Arca backend  
**Build env:** WSL Ubuntu-20.04, `arm-linux-gnueabi-g++` 9.4, `--enable-vkeybd`

---

## Quick Build & Deploy

```bash
cd scummvm-arca && ./build-and-deploy.sh 192.168.50.73
```

The configure flags, the softfp `CC`/`CXX` and the ALSA link it uses: [`README.md`](README.md) → *Build*.

First-time extras:
```bash
# Scaled virtual keyboard pack
scp ../scummvm-arca/vkeybd_arca.zip root@192.168.50.73:/opt/games/
# To regenerate: python3 ../scummvm-arca/make_vkeybd_scaled.py vkeybd_small_source.zip ../scummvm-arca/vkeybd_arca.zip 2
```

Sync backend source to version control — see [`README.md`](README.md#backend-files) or use:
```bash
bash manage-scummvm-changes.sh sync     # scummvm/ edits → backend-files/
bash manage-scummvm-changes.sh restore  # backend-files/ → scummvm/
```

---

## Architecture

```
ScummVM Core → OSystem_Arca
  ├── ArcaGraphicsManager  → /dev/fb0 (800×480 RGB565, double-buffered)
  ├── ArcaEventSource      → /dev/input/event* (touch, keyboard, mouse w/ cursor, gamepad)
  ├── OssMixerManager            → audio_out.c → ALSA plughw:N,0 (22050 Hz mono requested) → TWL4030 or USB DAC
  └── Default managers (timer, events, saves, filesystem)
```

Backend files: [`backends/platform/arca/`](../scummvm/backends/platform/arca/) — `arca.cpp/h`, `arca-graphics.cpp/h`, `arca-events.cpp/h`  
Mixer adapter: [`backends/mixer/oss/oss-mixer.cpp/h`](../scummvm/backends/mixer/oss/oss-mixer.cpp)

---

## Gesture Navigation

| Gesture | Zone | Action |
|---|---|---|
| Triple-tap | Bottom-right (x>720, y>400) | Global Main Menu (Ctrl+F5) |
| Triple-tap | Bottom-left (x<80, y>400) | Virtual Keyboard |
| Triple-tap | Top-right (x>720, y<80) | Enter key |

Corner zones are gesture-only — all taps in 80px corners are suppressed from the game. `_waitForRelease` blocks new touch until finger lifts. Overlay-transition guard emits synthetic LBUTTONUP when GMM opens/closes to prevent "stuck walking" in SCI games.

---

## Audio

**Signal path:** OssMixerManager → `audio_out.c` → ALSA `plughw:N,0` (22050 Hz mono S16_LE requested; `plughw` converts) → card 0 TWL4030 DAC1 → HandsfreeL/R → SPKR1, or card 1 a USB DAC  
**Amp enable:** GPIO12 HIGH (set by `/etc/init.d/audio-enable` at boot)  
**Hardware audio details:** See [`SYSTEM_ANALYSIS.md#34-audio`](../SYSTEM_ANALYSIS.md#34-audio)

**Design choices that are still this file's:**
- **Mono output** — single speaker; eliminates stereo/mono mismatch bugs; halves all audio-thread work
- **22050 Hz** — halves OPL synthesis load vs 44100
- **2048-frame period** — 93 ms at 22050 Hz (`ALSA_PERIOD_REQ` in `audio_out.c`)
- **SCHED_OTHER** — SCHED_RR starved main thread on single-core ARM; the multi-period ring absorbs jitter
- **50% volume attenuation** — `audio_out_set_shift(&_out, 1)`, the old `>>1`; the speaker distorts at full scale

**The device half is `native_apps/common/audio_out.{c,h}`, not this file.** The `plughw:N,0` open (card
chosen by the `audio_device` key), the hw/sw params and their read-backs, the ring query, the silence
prefill, the `EAGAIN` and XRUN handling and the fallback to the panel speaker when a USB DAC is unplugged
all live there; `oss-mixer.cpp` keeps only the mixer, the fill and the service thread — the class and
file names are ScummVM's. Two of the old choices were
**deleted rather than moved** — the fixed wall-clock deadline (the thread paces off
`audio_out_service_interval_us()` instead) and the emergency anti-underrun second write.
`oss-mixer.cpp:45-58` records why for both.

**Verified at the panel 2026-09-01, after that move:** Full Throttle plays correctly, audio and all,
and King's Quest 2's AdLib synthesis *and* its shore-wave sample both play as expected — two engines
and two synthesis paths, operator unhedged on both. ScummVM on ALSA was heard again on `.188` 2026-09-29.

### Mono mixer and the granted rate

`MixerImpl(_outputRate, false, _samples)` — a mono mixer, requesting 1 channel (a stereo grant is
widened in `fillFromMixer()`), at the rate `audio_out` reports **granted** on the first open. A
`MixerImpl`'s rate is fixed at construction, so a reopen granted a different rate warns instead: a
mismatch plays OPL at the wrong tempo (half speed if `_outputRate` is 2× the real rate). ScummVM's mixer
downmixes DualOPL2/OPL3/iMUSE sources to mono itself. The OSS shim's ioctl bugs that first forced this
design are in [`SYSTEM_ANALYSIS.md#34-audio`](../SYSTEM_ANALYSIS.md#34-audio), gotcha 3
(`native_apps/tests/ch_test.c` is its evidence).

---

## Debug Mode

```bash
ssh root@192.168.50.73 'ARCA_DEBUG=1 /opt/games/scummvm'
```

Enables touch feedback circles and TOUCH_NONE→PRESSED debug logging. Read once at startup (`getenv`), cached — zero runtime overhead when off.

**Debugging tips:**
- KQ3 `intro` plays music immediately — quickest audio test
- `> /tmp/scummvm.log 2>&1` — WARNING lines go to stderr immediately (stdout is block-buffered)
- `top -H` shows per-thread CPU

---

## Bug Fix History

Condensed log of issues found and fixed. Full debugging context is in git history.

| Bug | Root Cause | Fix | Files |
|---|---|---|---|
| OPL music = noise fragment | `mixCallback(buf, _samples)` passed frame count instead of byte count | Pass `bufBytes` = samples × bytes-per-frame | `oss-mixer.cpp` |
| OPL music half-speed | ALSA OSS shim ignores stereo → L/R consumed as separate mono frames | Switched to mono mixer entirely | `oss-mixer.cpp` |
| Black screen (post-audio fix) | SCHED_RR audio thread starved main thread on single-core ARM | Removed SCHED_RR, use SCHED_OTHER | `oss-mixer.cpp` |
| Black screen (persistent) | 32-bit `long` overflow in frame-rate cap (`timeval` delta from epoch) | Init timing baseline to current time, not epoch | `arca-graphics.cpp` |
| Audio "bru-bru" stuttering | Blocking `write()` stalls for 506 ms ALSA HW period | O_NONBLOCK + wall-clock pacing + no SETFRAGMENT | `oss-mixer.cpp` |
| OPL still half-speed after mono fix | `_outputRate` may not match actual device rate (set-ioctl output unreliable) | Read back actual rate with `SOUND_PCM_READ_RATE`; use that for `_outputRate` | `oss-mixer.cpp` |
| Audio breakup after extended play | Ring buffer draining below safe level (wall-clock pacing drift / XRUN) | Pre-fill ring with 3 silence buffers; GETOSPACE monitoring; emergency refill on near-empty | `oss-mixer.cpp` |
| **Input not working (12 sub-bugs)** | Multiple issues across event system, graphics manager, and build system | See breakdown below | multiple |

### Input Bug Fixes (12 sub-bugs)

Keyboard, mouse (with cursor), and gamepad all verified working in launcher and in-game (EcoQuest, Full Throttle).

| Sub-bug | Fix | Files |
|---|---|---|
| Keymapper disabled | `allowMapping()` returns `true` — enables keyboard/gamepad mapping | `arca-events.cpp` |
| Mouse movement lost on click | Flush pending mouse movement before emitting button events | `arca-events.cpp` |
| In-game mouse events ignored | `getScreenChangeID()` returns incrementing counter (was returning 0) | `arca-graphics.cpp` |
| Cursor not visible/tracking | Cursor position synced from event manager in `updateScreen()` | `arca-graphics.cpp` |
| Cursor coordinates wrong in-game | Cursor position scaled to framebuffer coordinates in game mode | `arca-graphics.cpp` |
| Cursor palette broken | `setFeatureState(kFeatureCursorPalette)` properly implemented | `arca-graphics.cpp` |
| Stale `.o` files after backend changes | `build-and-deploy.sh` auto-restores backend files with `touch` + stale `.o` cleanup | `build-and-deploy.sh` |

**32-bit ARM lesson:** `sizeof(long) == 4` — never compute `(now.tv_sec - epoch_0) * 1000000L`. Always init timing baselines to current time.

---

## Build & Cross-Compilation Learnings

Hard-won lessons from build and runtime failures. These are non-obvious pitfalls specific to the Steelcase RoomWizard cross-compilation environment.

### `png.h: No such file or directory` (March 2026)

**Symptom:** `image/png.cpp:28:10: fatal error: png.h: No such file or directory` when building via `./deploy-all.sh <IP>`

**Root cause:** The `deploy` command path in `build-and-deploy.sh` ran `build_scummvm()` → `make` without calling `build_arm_deps()` first. The `arm-deps/` directory with cross-compiled zlib and libpng didn't exist. However, `config.mk` from a prior configure still had `USE_PNG = 1`, so make tried to compile `image/png.cpp` which `#include <png.h>`.

**Why it wasn't caught before:** The `all` command path calls `build_arm_deps()` → `configure_build()` → `build_scummvm()` in order. The `deploy` path was a shortcut that skipped dependency building.

**Fix:**
- Added `build_arm_deps` call at the start of `build_scummvm()` — runs on every code path, idempotent (skips if `libpng.a` exists)
- Strengthened staleness check to verify `$ARM_DEPS_PREFIX/lib/libpng.a` exists on disk, not just that `config.mk` says `USE_PNG = 1`

**Lesson:** Generated config files can become stale. Always verify the actual build artifacts exist, not just config flags.

### SIGSEGV before `main()` — glibc 2.31 static pthread vs kernel 4.14.52 (March 2026)

**Symptom:** ScummVM crashes immediately with `Segmentation fault (core dumped)`. Even `scummvm --version` crashes. No log file created. `dmesg` shows:
```
PC is at 0x40
LR is at 0x130415d
r0 : ffffffda                     ← -38 = -ENOSYS
ISA ThumbEE
```

Static hard-float build (`arm-linux-gnueabihf-g++` 9.4.0, glibc 2.31) on kernel 4.14.52. Cause not
proven; the status and the hypothesis are in `SYSTEM_ANALYSIS.md#62-never-use---whole-archive-with--lpthread`.
The recorded fix (plain `-lpthread`) was not applied to the build script until 2026-09-01.

---

## Optimization Backlog

**Baseline:** CPU ~80% → **32%** after all optimizations (LSL5 gameplay, OPL music + animation)

| # | Optimization | Status | Notes |
|---|---|---|---|
| O1 | Precomputed `_palette32[256]` LUT | done | 1 indexed load replaces 7 ops/pixel |
| O2 | Precomputed `srcXtab[]` + row pointer lifting | done | Eliminates per-pixel division |
| O3 | Border-only clear (skip overwritten pixels) | done | |
| O4 | Cached `rwSystem()` global | done | Eliminates `dynamic_cast` per poll |
| O5 | Dead code removal | done | |
| O6 | Right-click fix (LBUTTONUP→RBUTTONDOWN sequence) | done | Correctness, no CPU impact |
| O7 | Skip `fb_swap` on unchanged frames | done | Menu CPU 35%→15% |
| O8 | 16bpp RGB565 framebuffer | done | Halves write bandwidth |
| O9 | OMAP3 DSS hardware scaler | **not used — low return (operator, 2026-10-04)** | Reachable from sysfs, but always filtered and rejected at the panel; software nearest-neighbour is sharp and cheap. [`SYSTEM_ANALYSIS.md#32-display`](../SYSTEM_ANALYSIS.md#32-display). |
| O10 | NEON `vst1q_u16` 8-pixel blit | done | |
| O11 | Row deduplication (L1-cache tempRow) | done | 57% of scaled rows are dupes |
| O12 | Mono mixer | done | Halves audio-thread work |

---

## Limitations & Open Issues

- **Single-touch only** — right-click = long-press 500 ms
- **PNG crash ("No PNG support compiled!")** — **Fixed.** Clicking the thumbnails/grid-view icon in the launcher would crash with "No PNG support compiled!" because the ARM cross-compilation sysroot lacked `libpng`. The `./configure` script silently disables PNG when it cannot find `libpng` for the target architecture. **Fix:** the build script now automatically cross-compiles zlib 1.3.1 and libpng 1.6.43 from source into a local `arm-deps/` prefix directory (idempotent — skips if already built), then passes `--with-zlib-prefix` and `--with-png-prefix` to `./configure`. No manual package installation needed; Ubuntu Focal WSL doesn't support armhf multiarch (`dpkg --add-architecture armhf` fails because standard mirrors don't carry armhf). After changing dependencies, a full rebuild is required (`clean` + `configure` + `make`).
- **Software rendering only** — no GPU. (The DSS overlay scaler is deliberately unused: [`SYSTEM_ANALYSIS.md#32-display`](../SYSTEM_ANALYSIS.md#32-display).)
- **No MIDI** — NullMidiDriver; future option: software synth (FluidSynth or similar)
- **OPL tempo** — **Confirmed correct on device** (operator verdict 2026-08-31; not a reference-recording comparison against a known good capture). What makes it right is the mono mixer plus the read-back `_outputRate`, both recorded under *Audio* above.
- **Exit game returns to launcher on Ubuntu but exits ScummVM on the Steelcase RoomWizard** — **Fixed 2026-08-03**, and the cause recorded here was wrong. It is not `quit()`: `OSystem_SDL::quit()` does `destroy(); exit(0);` too, and nothing in the game-exit path calls `quit()` at all (the only caller in the tree is `common/recorderfile.cpp`). The decision is `base/main.cpp:832`, which `break`s out of the launcher loop when a game returns `kNoError` and neither `kFeatureNoQuit` nor `gui_return_to_launcher_at_exit` is set — upstream's default on every platform, so the Ubuntu difference was a desktop config with that Global Options checkbox on, not a backend difference. `initBackend()` now sets `gui_return_to_launcher_at_exit` on first run. **Do not "fix" `quit()`'s `exit(0)`** — it is correct here; exiting the process is how the init script's respawn returns the panel to `app_launcher`. **Not `kFeatureNoQuit` either**: it hides the Quit button on both the launcher and the global main menu, which would trap the user inside ScummVM with no way back to the native games.
- **Wireframe green UI (missing theme data)** — **Fixed.** The build-and-deploy script now automatically deploys `scummremastered.zip` and `gui-icons.dat` from `gui/themes/` to `/opt/games/` on the device. If you still see the green wireframe UI, re-run `./build-and-deploy.sh <ip>` to redeploy the theme files.

## ScummVM Engine Availability on the Steelcase RoomWizard

### Currently Enabled
The build enables 8 base engines by default (`ENGINE_BATCH=0` in `build-and-deploy.sh`): scumm, scumm-7-8, he, agi, sci, agos, sky, queen. Engines with unmet library dependencies are skipped by configure. See [`ENGINE_ADDITION_PLAN.md`](ENGINE_ADDITION_PLAN.md) for adding more.

### 16-bit Color Support (USE_RGB_COLOR)
The Arca backend was added to the 16-bit backend whitelist via [`configure.patch`](backend-files/configure.patch). This enables `USE_RGB_COLOR` which is required by ~30 engines. After building, verify with:
```bash
grep USE_RGB_COLOR scummvm/config.h
```
If not defined, ~17 engines will be silently skipped (ADL, Blade Runner, Blazing Dragons, Freescape, Griffon Legend, Grim, Hopkins FBI, Hypno/UFOs, mTropolis, NGI, Pegasus Prime, Tony Tough, Trecision, V-Cruise, Riven, Ultima IV/VIII).

### Skipped Engines — Missing Library Dependencies

| Library to Cross-Compile | Engines Unlocked | Priority |
|--------------------------|-----------------|----------|
| **libjpeg** | Groovie 2, Myst 3, Myst ME, Wintermute, Titanic, Glk (partial) — 7 engines | High |
| **FreeType2** | Buried in Time, Z-Vision, Petka, Stark (partial), Glk (partial) — 6 engines | Medium |
| **Lua 5.x** | HDB, Sword25 (partial), Ultima VI — 4 engines | Medium |
| **libvorbis** (remove `--disable-vorbis`) | Nancy Drew, Stark (partial) — 3 engines | Low |
| **libmad** (remove `--disable-mad`) | AGS, Titanic (partial) — 2 engines | Low |
| **libtheora** | Sword25 (partial) — 2 engines | Low |

To add a library, extend `build_arm_deps()` in [`build-and-deploy.sh`](build-and-deploy.sh) following the zlib/libpng pattern, and remove any `--disable-*` flags from configure.

### WIP/Unstable Engines (can be force-enabled)

These engines have all their dependencies met but are marked `build-by-default=no`. They can be force-enabled by adding `--enable-engine=<name>` to the configure call:

Avalanche, Chamber, Cryo/Lost Eden, DM/Dungeon Master, ICB/In Cold Blood, Immortal, Last Express, Lilliput, MacVenture, MADS V2, Carmen Sandiego (Mohawk sub), Mutation of JB, Sludge, Star Trek, Ultima I, WAGE

**Note**: Playground 3d and TestBed are testing/debug engines, not games.

### Cannot Be Enabled (require OpenGL GPU)

Hpl1, Watchmaker — require OpenGL which is not available on the Steelcase RoomWizard hardware.

### Build Artifacts Note
When compiling libpng with `-mfpu=neon`, add `-DPNG_ARM_NEON_OPT=0` to disable libpng's NEON assembly optimizations (the assembly files are not compiled in our manual build, causing linker errors). See [`SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md#63-cross-compiled-dependencies-must-be-built-from-source) for details.

## Memory Budget

| Component | Size |
|---|---|
| Binary | ~14 MB |
| Game surface | ~1.2 MB |
| Framebuffer (double-buffered) | ~3 MB |
| Audio mix buffer (mono) | ~4 KB |
| **Total** | **~18 MB** of 184 MB available |
