# scummvm-arca — authoring guide

Rules and traps for working on the ScummVM backend port. Deep reference (engine tables, memory
budget, bug history, optimisation log) is in [`SCUMMVM_DEV.md`](SCUMMVM_DEV.md). Device facts are
in [`../SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md). Open work is in
[`../IMPROVEMENT_PLAN.md`](../IMPROVEMENT_PLAN.md).

## The source-of-truth trap — read this first

The real source lives in **`../scummvm/backends/platform/arca/`** (an untracked upstream
checkout). `backend-files/` in this repo is the *version-controlled copy*.

```bash
bash manage-scummvm-changes.sh sync     # scummvm/ edits  -> backend-files/   (do this after editing)
bash manage-scummvm-changes.sh restore  # backend-files/ -> scummvm/
```

Consequences you must respect:

- Editing `backend-files/` alone changes nothing that gets built. Either edit in `../scummvm/`
  and `sync`, or edit here and `restore`.
- `backend-files/README.md` is copied **from** the upstream tree and copied back by `sync`.
  Anything you write there is upstream-facing and will be overwritten if the upstream copy
  differs. Do not put Arca project documentation in it.
- Restoring files from version control breaks `make`'s timestamp logic — the existing `.o` looks
  newer than the restored source, so you get a silently stale binary. `build-and-deploy.sh`
  handles this by `touch`ing restored sources and deleting the matching `.o`. If you copy files
  by hand, do the same.

## Build

```bash
./build-and-deploy.sh <ip>            # the supported path (all|clean|configure|build|strip|deploy|set-default|info)
```

`build_arm_deps()` cross-compiles zlib and libpng into `arm-deps-softfp/` and is idempotent. It runs on
every code path — do not add a shortcut that skips it.

**Warning baseline: two, both pre-existing** (recorded 2026-08-03) — `oss-mixer.cpp:298`
unused-result, and a `getaddrinfo` static-link note from timidity. `arca.o` itself is clean, and
`native_apps/` is at zero, so treat any *third* warning as yours. The `oss-mixer.cpp` one is a real
defect and is open work in [`../IMPROVEMENT_PLAN.md`](../IMPROVEMENT_PLAN.md).

**The `check-arm-safe.sh` gate runs from inside `strip_binary()`, ahead of the in-place `strip`** —
that is the only moment an unstripped ScummVM binary exists, and the gate is meaningless on a stripped
one. Never move it after the strip, and never allowlist offsets: `base/version.o` re-embeds the build
date on every link, so every address after it moves between builds of identical source. Why a stripped
binary cannot be gated, with the byte-level measurement: `../IMPROVEMENT_PLAN.md`.

**`clean` is the only clean path — do not add a second one.** A root-level `clean.sh` used to exist
and was deleted 2026-08-03: it was this tree's clean script with no
shebang and no `cd`, so from the repo root its `find . -name '*.o' -delete` reached
`native_apps/build/` and the `usb_host/linux-4.14.52/` kernel objects, and `-name '*.d'` matched
*directories* in the extracted rootfs under `partitions/`. `clean_build()` already does strictly
more than it did — `make clean` inside the tree, plus `native_apps/common/*.o`, which is the one
that actually bites (a stale x86 `.o` there fails the cross-build with "file format not
recognized").

**Never trust `config.mk`; check the artifact.** A stale `USE_PNG = 1` from an earlier configure
once made `make` compile `image/png.cpp` with no `libpng.a` on disk, failing on `png.h`. Test for
`arm-deps-softfp/lib/libpng.a`, not for the flag.

Ubuntu Focal WSL cannot do armhf multiarch (`dpkg --add-architecture armhf` fails — the standard
mirrors carry no armhf), which is why dependencies are built from source rather than installed.
To add one, extend `build_arm_deps()` following the zlib/libpng pattern and drop the matching
`--disable-*` from configure. Changing dependencies requires a full `clean` + `configure` + `make`.

When building libpng with `-mfpu=neon`, pass `-DPNG_ARM_NEON_OPT=0` — the NEON assembly files are
not part of the manual build and the linker will fail without it.

## Critical: never use `--whole-archive` with `-lpthread`

`build-and-deploy.sh` appends plain `-lpthread` (`oss-mixer.cpp` starts the service thread; with none
the link fails on `pthread_create`/`pthread_join`, measured 2026-09-01). Never `--whole-archive`; the
crash record and its status: `SYSTEM_ANALYSIS.md#62-never-use---whole-archive-with--lpthread`.

## Verifying that a link did what you asked

⚠️ **The deployed `scummvm` is `stripped`, so `grep -a` for an internal symbol answers zero whether or not
it was linked.** Asking it about `audio_out_open` reads exactly like "the object never made it in", which
is a false negative dressed as a measurement. What does answer:

- the build log's own lines — `C ../native_apps/common/<name>.o` per object, then `LINK scummvm`;
- `md5sum` of the freshly built binary against the deployed one;
- the build date ScummVM draws on its launcher screen, which dates the *running* binary;
- what it prints — the shared device half logs its `plughw:N,0` open on the first mixer init.

⚠️ **A glibc symbol is not an instrument either**: `grep -ac clock_gettime64` is nonzero in every working
binary on the device, the native apps included.

## Architecture

```
ScummVM Core -> OSystem_Arca
  |- ArcaGraphicsManager -> /dev/fb0   (RGB565, double-buffered)
  |- ArcaEventSource     -> /dev/input/event*  (touch, keyboard, mouse, gamepad)
  |- OssMixerManager           -> audio_out.c -> ALSA plughw:N,0 (22050 Hz mono) -> TWL4030 or USB DAC
  \- Default managers (timer, events, saves, filesystem)
```

**This backend runs the framebuffer at 16bpp RGB565**, not 32bpp. It calls `fb_set_bpp(...,16)`
on startup. Screenshots must be decoded with `--bpp 16`. Native apps run 32bpp and reset it on
launch, so the mode depends entirely on which app last ran.

**It links its own copy of `native_apps/common/touch_input.o` and `framebuffer.o`** (see
`backend-files/configure.patch`). The build refreshes them, but a *deployed* ScummVM keeps
whatever it was built with — redeploy ScummVM after changing touch or framebuffer code in
`native_apps/common/`.

**The backend is an ordinary `fb_init()` client, bezel included.** It has no bezel logic of its
own: `_fb->width`/`_fb->height` are the logical (visible) screen. Everything that indexes
`_fb->back_buffer` must use `fbWidth()`/`fbHeight()` as the stride and bounds — the buffer is
logical-sized (e.g. 800×450×2 bytes at 16bpp), so a hardcoded 800-pixel stride writes past the
allocation.

## Content and the overlay go in the touch-safe rectangle

`fbWidth()`/`fbHeight()` are the stride and bounds of the back buffer and **nothing else** — they are
not where content goes. The digitizer saturates before the panel edge, so a band at each end of the
visible surface is drawable but **not pressable** (~19 px top / ~16 px bottom / ~6 px each side on
RW09; measured at runtime by `touch_input.c`, `0` until a panel's edge reach has been swept). Neither
a game nor the GUI theme can be audited for which of its pixels have to be reachable, so:

| What | Rectangle | Set by |
|---|---|---|
| **overlay** — launcher, GMM, virtual keyboard | `safeWidth()` × `safeHeight()` at `safeLeft()`/`safeTop()`, **always** | `getOverlayWidth/Height()`, `_overlaySurface.create()`, the composite loop in `updateScreen()`, `drawCursor()`'s overlay branch |
| **game picture** | the safe rect by default; the whole surface when `rwFullContentArea()` | `getScalingInfo()` — the single chokepoint |
| gesture corners, overlay touch coordinates | safe rect, **always** | `arca-events.cpp` via `safeRect()`, which reads `getSafeRect()` |
| virtual cursor (USB mouse, gamepad stick) | wherever the current content is — safe rect in the GUI, the *picture* in game mode | `cursorBounds()` in `arca-events.cpp` |

The overlay surface **must** be exactly `getOverlayWidth()` × `getOverlayHeight()`: `ThemeEngine` and
the virtual keyboard `grabOverlay()` into a surface sized from those and `copyRectToOverlay()` it
straight back, so a mismatch corrupts every dialog backdrop.

`getScalingInfo()` returns offsets that already include the content rect's origin, which is why
`transformCoordinates()`'s game branch and `drawCursor()`'s game branch need no knowledge of any of
this. Keep it that way — one chokepoint, not three.

**Ordering, and the trap it fixes:** `SCREEN_SAFE_*` is only correct after both `fb_init()` and
`touch_init()`. `ArcaEventSource` is constructed *before* the framebuffer exists, so
`initSize()` calls `initFramebuffer()` and then **`syncScreenGeometry()` immediately**, before
sizing the overlay surface — that call is what republishes the geometry and with it the measured
inset. Moving it back below the surface creation silently sizes the overlay from the pre-bezel
defaults.

`ARCA_CONTENT_AREA=visible` (one-off) or `arca_content_area=visible` in `scummvm.ini` opts the
**picture** out. It deliberately does not move the overlay or any gesture zone; an option that could
strand the launcher's bottom button row is a footgun, not a feature.

## The config file is at one absolute path

`OSystem_Arca::getDefaultConfigFileName()` returns **`/opt/games/scummvm.ini`**. The base
`OSystem` returns the bare relative name `"scummvm.ini"`, which `Common::FSNode` resolves against the
process's **current directory** — and the cwd differs per launch method, because the init script does
not `cd` and `app_launcher` `execl()`s without `chdir()`. That gave RW09 three config files
(`/scummvm.ini` from boot, `/home/root/scummvm.ini` from SSH, `/opt/games/scummvm.ini` from a `cd`),
settings that did not follow the user between launch methods, and an "is the setting being ignored?"
report that was really an edit to the wrong file (fixed 2026-08-03).
`OSystem_POSIX` solves this with an absolute `$HOME/.config` path, but this backend derives from
`ModularGraphicsBackend`, not from it. `/opt/games` is the right home: next to the binary, the icons
and the game data, writable, and independent of `$HOME`, which the init script does not set.

**Backend defaults belong in `initBackend()`, and must be flushed.** ConfMan only persists keys that
were actually *set* — `registerDefault()` is not written to the file either — so a key the backend
merely reads with `hasKey()` never appears in `scummvm.ini`, and the option is undiscoverable from the
device. `initBackend()` therefore writes `arca_content_area=safe` on first
run, behind `!ConfMan.hasKey()` so a user's `visible` is never overwritten. Use **`setAndFlush`**, not
`set`: `quit()` calls `exit(0)` and bypasses ScummVM's normal shutdown flush, so a plain `set()` can be
lost on the one exit path this device actually takes. `ConfMan` is loaded (`base/main.cpp:478`) well
before `initBackend()` (`:572`), and both the read and the write go through
`createConfigReadStream`/`createConfigWriteStream`, hence through the override above — so the default
lands in the same one file.

**Saves default to `/home/root/data/scummvm-saves` (on the root partition p3, so a reflash wipes it; it is in the `backup.sh` set).**
`backend-files/arca.cpp` passes it as the `DefaultSaveFileManager` constructor argument, which is ConfMan's default, so a
`savepath` in `scummvm.ini` still wins. Deployed to `.188`; a real save made at the panel is **unverified**. Before it, the backend had
no savepath, so saves landed in the cwd (`/` at boot, `/home/root` from SSH) [inferred from source; a save was found in `/home/root`].

Numbers and method: [`../SYSTEM_ANALYSIS.md#33-touch`](../SYSTEM_ANALYSIS.md#33-touch).

## Audio

The device facts are documented in
[`../SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md#34-audio). The backend-specific consequences:

- **Mono request, 22050 Hz.** Mono halves OPL synthesis load, but it is a REQUEST: `hw:0,0` is
  stereo-only, and `fillFromMixer()` honours whatever count is granted — a fill that ignored the
  grant would play at the wrong speed. ScummVM's mixer downmixes automatically.
- **Use the GRANTED rate for `_outputRate`** — `audio_out_rate()` after the open, never the request;
  a wrong `_outputRate` makes OPL run at the wrong tempo.
- **The device half is not this file's.** The ALSA open, the negotiation and read-backs, the ring
  query, the silence prefill and the `EAGAIN` retry all live in `common/audio_out.{c,h}`; this is an
  adapter. ⚠️ **The wall-clock deadline and the emergency second write were DELETED, not moved** —
  the thread paces off `audio_out_service_interval_us()`, and `oss-mixer.cpp:45-58` says why.
- **No forced small period** — `audio_out.c` requests 2048 x 16 frames; the ring is the jitter buffer.
- **`SCHED_OTHER`, never `SCHED_RR`.** An RT audio thread starves the main thread on this single
  core and you get a black screen.

Quickest audio test: KQ3 `intro`, which starts music immediately.

## Debugging

```bash
ssh root@<ip> 'ARCA_DEBUG=1 /opt/games/scummvm'   # touch circles + touch-state logging
ssh root@<ip> '/opt/games/scummvm > /tmp/scummvm.log 2>&1'
```

`WARNING` goes to stderr and appears immediately; stdout is block-buffered, so redirect both.
`top -H` gives per-thread CPU.

## Gesture navigation

Triple-tap in a corner: bottom-right = Global Main Menu, bottom-left = virtual keyboard,
top-right = Enter. The 80 px corner zones are gesture-only — taps there are suppressed from the
game. Emit a synthetic `LBUTTONUP` when an overlay opens or closes, or SCI games get stuck
walking.

## Rendering

Software only. The optimisation log (O1–O12, CPU 80 % -> 32 %) is in
[`SCUMMVM_DEV.md`](SCUMMVM_DEV.md#optimization-backlog). The reusable techniques — palette LUTs,
precomputed source-column tables, row deduplication, skipping `fb_swap` on unchanged frames — are
summarised in [`../CLAUDE.md`](../CLAUDE.md).

**The DSS hardware scaler is reachable from sysfs and was measured and rejected on image quality** —
software nearest-neighbour is the shipped path, with no config key, no settings row and no backend branch
for a hardware arm. Numbers and the eye verdicts: [`SYSTEM_ANALYSIS.md#32-display`](../SYSTEM_ANALYSIS.md#32-display).
What a backend change would meet: `hasFeature()` returns true for `kFeatureCursorPalette` only and
`beginGFXTransaction`/`endGFXTransaction` are no-ops, so there is **no runtime mode-change plumbing**; and
nothing bridges `/opt/games/scummvm.ini` and `arca_config.conf` (the one exception is the hand-declared
`config_audio_device_stored()`, `oss-mixer.h`:34).

**The output rectangle is one isotropic scale, pillarboxed, and a function of runtime numbers.**
`getScalingInfo()`:163-171 takes the smaller of `rectW*256/w` and `rectH*256/h` and centres the picture in
the content rect (strips memset black at :455-478), so the axes are always equal — 320×200 is *not*
stretched 2.5×/2.4×. **Measured** on `.188` (bezel `15 13 0 0`, touch insets L3 R10 T14 B17): scale 538,
**672×420 at panel (60,29)**. A 15/15 bezel with zero insets would give 720×450 at (40,0) and no real unit
is that. ⚠️ **A non-black bounding box of an `fb0` grab is not the rect**: the mouse cursor (written at
output resolution after the scale, `drawCursor()`:585-588) and a stray 16-px speck on rows 478-479 widen
it, and KQ2 paints black at the top and bottom of its own surface. The columns pin the rect, the rows
cannot. To capture a real frame with no touch: stop the launcher, `/opt/games/scummvm kq2` over SSH, one
16bpp page off `/dev/fb0`, then `fb_to_game_ppm.py` at the repo root (`--self-test` needs no device).
⚠️ **Verify exactly one `scummvm` PID before and after the grab** — a capture with two engines fighting
over `fb0` was discarded, from one `nohup` launch, cause unexplained.

## Leaving a game, and why `quit()` keeps its `exit(0)`

**Do not replace `OSystem_Arca::quit()`'s `exit(0)` with a `_quit` flag.** It is not on the
game-exit path and never was: across the whole ScummVM tree the only caller of `OSystem::quit()` is
`common/recorderfile.cpp`, and `OSystem_SDL::quit()` does `destroy(); exit(0);` too. Exiting the
process here is *correct* on this device — it is how the init script's respawn hands the panel back to
`app_launcher`.

What decides whether leaving a game returns to the ScummVM launcher is the launcher loop in
`base/main.cpp:832`, which `break`s out — ending the process — when a game returns `kNoError`, no
return-to-launcher was requested, and neither `kFeatureNoQuit` nor `gui_return_to_launcher_at_exit` is
set. That is upstream's default on every platform, so "it returns to the launcher on Ubuntu" was a
desktop config with that Global Options box ticked, not a backend difference. `initBackend()` sets
`gui_return_to_launcher_at_exit` on first run (fixed 2026-08-03).

**`kFeatureNoQuit` satisfies the same condition and is a trap.** It also hides the `Quit` button on the
ScummVM launcher (`gui/launcher.cpp:264`) *and* the in-game global menu (`engines/dialogs.cpp:90`), and
loops `launcherDialog()` until a game starts. Quitting ScummVM is the only route back to `app_launcher`
and the native games, so that traps the user inside ScummVM. Never set it here.

## 32-bit target

`sizeof(long) == 4`. Baseline all timing to a start timestamp captured at init; never multiply a
raw `tv_sec` by 1000 or 1000000. `getMillis()` does the multiply in `uint32` since 2026-08-03, so it
wraps cleanly at 49.7 days rather than overflowing signed `time_t` at 24.85 — **built, deployed and
verified on RW09 the same day**. All six `getMillis()` consumers
compute `now - _last` in `uint32` and are wrap-safe; keep it that way.
