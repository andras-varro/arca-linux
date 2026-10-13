# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this is

**Arca** (the codename used in code and paths; the product name is **Ludbox Linux**) repurposes the **Steelcase RoomWizard** — a wall-mounted meeting-room display — into a
games/apps platform. The device is an embedded Linux box: **TI OMAP3503** ARM Cortex-A8 @ 600 MHz,
234 MB RAM, 800×480 framebuffer, projected-capacitive touch, kernel 4.14.52, SysVinit. No GPU, no
DSP — all rendering is software. Display is legacy **omapfb/omapdss**; there is no DRM/KMS.

**There is no local app to run.** Everything is cross-compiled on the dev host and deployed over SSH
to a physical device (reference unit `192.168.50.73`, aka RW09). Verifying a change means deploying it
and looking at the panel — framebuffer screenshots via `fb565_to_png.py`.

**Kernel policy: we build our own 4.14.52 image; a mainline 5.x/6.x port stays out.** It builds from the
device's own config (asking Steelcase for source is ruled out and unnecessary) with `kernel/build-image.sh`,
which applies `kernel/patches/` and `kernel/dts/`. Touch works as a loadable `.ko` (`kernel/drivers/cy8ctmg120_ts`, measured on `.188`), not built in — `kernel/README.md`, "What we patch" table, Touch row.

### One fact, one home

**If something appears in two documents, the other copy is stale.** Four top-level docs with disjoint
jobs:

| Doc | Answers | Read it |
|---|---|---|
| `SYSTEM_ANALYSIS.md` | *What is true about this device?* — silicon, boot chain, OS, subsystems, traps | Before touching anything hardware-related |
| `HARDWARE.md` | *What is on the board?* — parts, connectors, headers, the enclosure, the teardown photos | Opening a unit, probing a header, or wondering what `J5` is |
| `IMPROVEMENT_PLAN.md` | *What should we do about it?* — open bug + feature backlog with `file:line` | **Before starting work**, so you don't rediscover a known bug |
| `CLAUDE.md` (this file) | *What must I know before my first edit?* | Loaded every session |

`IMPROVEMENT_PLAN.md` holds **open work only**. Closed items are deleted, so an entry ID has a
scheduled expiry built in: ⚠️ **nothing outside that file may cite one.** A comment or a document
carries its own reason in full, and no ID sits beside it; `tests/doc_check.sh` group B counts any
citation, resolving or not.

`README.md` is the annotated walkthrough of every script; `COMMISSIONING.md` the operator-facing
bring-up guide and the argument for its phase split; `LICENSE.md` the per-file licence record.

**Adding to, correcting or closing out any of these goes through `/doc-update`.** It carries the order of
operations that gets skipped: classify *before* opening a file, ask "is this reference **here**?" of the
*other* documents, write rule + measurement + identifier, delete the closed thing, then one gate run and
one commit. ⚠️ **`./tests/doc_check.sh` group D is a per-document line ceiling** — an addition that puts a
file over it is paid for by a deletion or by raising the ceiling *in the same commit*, never absorbed.

**Directory-scoped `CLAUDE.md` files load automatically when you work there.** They describe their own
code only — device facts belong in `SYSTEM_ANALYSIS.md`, open work in `IMPROVEMENT_PLAN.md`. Link,
don't copy.

| file | covers |
|---|---|
| `native_apps/CLAUDE.md` | the rendering loop, the input model, the touch model, the screen rectangles — the deepest of them |
| `lib/CLAUDE.md` | the sourced shell libraries: SSH gate, plan compilers, bundle layout, the p1 rules |
| `commissioning/CLAUDE.md` | the bring-up scripts, the consent gate, offline verification, host naming |
| `device-files/CLAUDE.md` | what is installed verbatim, and how to author the two rules files |
| `tests/CLAUDE.md` | the host regressions, what each cannot see, sabotage-harness discipline |
| `scummvm-arca/CLAUDE.md`, `vnc_client/CLAUDE.md` | those ports |
| `rootfs/CLAUDE.md` | our own p6 root filesystem: Buildroot tree, ABI, rc links, the generic card image |
| `kernel/CLAUDE.md` | our 4.14.52 image: its patches, DT changes, build script and missing drivers — notes in `kernel/README.md` |

⚠️ **The vanilla kernel tree is the authority for every subsystem the vendor did not patch, and
reading it beats theorising from sysfs.** The USB-enumeration hunt cost most of a session to three
mechanisms inferred from `/sys` — a mode write, an OTG timeout, runtime PM — all three refuted, two after
being written into the docs as fact. Ten minutes in `drivers/usb/musb/` said which sysfs writes are
**silent no-ops on this SoC** (`omap2430_ops` has no `.set_mode`; nothing reads `a_wait_bcon`) and named
the real mechanism. **If a driver's sysfs surface is behaving inexplicably, read the driver.**

⚠️ **A conclusion read out of the driver is still a hypothesis about the device, and that hunt produced
two false ones in one afternoon.** Both came from correct source reading applied one step too far. A code
path that keeps VBUS up *given an established session* was written down as "leave an adapter plugged in
and the port stays alive" — the operator refuted it in three minutes with a hub. A guard that skips the
`SESSION` bit *when VBUS still reads valid* was written down as the cause of a failed rebind — the next
transcript had VBUS off and the rebind still failed. **Check what the path you just read assumes about
the state it starts in, and say "measured" or "inferred" in the sentence you write it down in.**

## Working style

**Delegate the reading.** The docs are long and the sources are large (`control_panel.c` is ~3700 lines
— do not trust a line count you are carrying from an earlier session, measure it). Answering "where is
X / which call sites do Y / does this pattern hold across all seven games" by reading files into the
main context burns the budget the actual edit and its verification need. Spin up subagents for that —
one per independent question, in parallel — and keep only their conclusions. Do the edits, the build and
the on-device verification in the main thread.

**A recorded cause and a prescribed fix are both hypotheses.** `open, confirmed` in
`IMPROVEMENT_PLAN.md` means the *symptom* reproduced — nothing more. The cause written beside it and the
fix suggested under it were both written by someone who had not yet done the work, and both have been
wrong more than once, each time refutable by a single `grep`. So: reproduce the symptom, then find the
cause yourself. **When an entry says "compare with X", read X.** And before setting a flag, read every
use of it: the first mechanism that *would* produce the symptom is not automatically the right fix.

**Give every new check a negative control, and ask which part of the count is the harness.** A gate that
reports a number can be wrong in both directions, and this repo has hit both. **If a fix is supposed to
drive a number to zero, check that it reaches zero.** Detail and the standing harness rules:
`tests/CLAUDE.md`.

**Write the failing version first.** There is no CI, so a test that has only ever been seen passing is
not evidence that it can fail. Compile it against the pre-fix source and count the failures.

**Say what you could not verify.** "Latent, not reproducible from this host" and "reported as *I think
it works*" are the honest forms. Do not promote a hedge to a confirmation.

**Editing the long docs.** Anchor an `Edit` on text you intend to **keep**, and expect to fix inbound
links whenever you retitle a heading. **`./tests/doc_check.sh` group A is the check** — every
`<doc>.md#<fragment>` in the repo, including the ~120 in `.sh`/`.c`/`.py`/`.conf` comments that no
markdown linter reads. The IDE's markdownlint catches the same defect inside one file for free:
**`MD051/link-fragments` fires on exactly the anchors you just broke**. (`MD060` table warnings fire on
every table in the repo — that is the established style, not something you introduced.) A long multi-line `old_string` can
fail to match for no visible reason; fall back to `head -N` / `cat <<'EOF'` / `tail -n +M`, which also
preserves LF. ⚠️ **Never bulk-edit a source file with a python script** — it rewrites the whole file's
line endings. Use `Edit`, or `sed -i`.

## Build & deploy

Everything builds with the ARM cross-compiler and deploys over SSH. There is **no CI**, but
`./tests/run-all.sh` is the host gate — every test that needs no device, plus `shellcheck` — and
`deploy-all.sh` and `release.sh` both **run it and refuse to build if it fails**; `--skip-tests` opts
out. Authoring and running rules: `tests/CLAUDE.md`. `--help` on any script is current; `README.md` has
the annotated walkthrough.

```bash
./setup-build-env.sh                # host prerequisites; --install-deps, --scummvm
./tests/run-all.sh                  # the host gate; --list, --scope=deploy, --self-test
./arca.sh                     # front door: a menu over everything below
./deploy-all.sh <ip>                # build + deploy everything (native_apps first)
./deploy-all.sh <ip> <component>    # one component;  --list  to see them
./commissioning/provision.sh <ip>   # update a unit: backup, install, reboot; --dry-run, --status
./release.sh --stage-only           # build all components + stage one offline bundle + tar
cd native_apps && ./build-and-deploy.sh [<ip>] [set-default]
```

**Units run only our own root filesystem** (`rootfs/`); a card comes from `rootfs/make-card-image.sh`, and
`provision.sh` updates a running unit, taking a `backup.sh` archive before its first write. Neither path
writes a unit's p1 (the card-image builder formats the p1 of an image file). `set-default` is the only mode `native_apps/build-and-deploy.sh` accepts. The boot service is
installed **only** by the provision rules — never by a component script. All of it:
`commissioning/CLAUDE.md`.

**Components** (each a subdir with a `build-and-deploy.sh`): `native_apps` (C games + launcher +
tools), `scummvm-arca` (ScummVM backend port), `vnc_client`, `usb_host` (USB host-mode
enablement + Xbox controller modules), `bluetooth` (BlueZ, BT modules, dongle firmware, boot start).

**Toolchain:** `./setup-build-env.sh` is the one home for the host package set — it probes what is
missing, prints the exact `apt` line before running it, and `--scummvm` also clones the gitignored
upstream ScummVM tree. ScummVM additionally needs **WSL Ubuntu 20.04+**. ⚠️ **`native_apps` cross-builds
one dependency, alsa-lib** (`build-alsa-lib.sh`, never deployed); ScummVM builds `arm-deps-softfp/` (zlib + libpng).

**`native_apps/` has no `Makefile`.** `native_apps/build-and-deploy.sh` (soft-float cross-compiler, dynamic) is
the only build path. A **new** binary goes in `GAMES_BINARIES` there and nowhere else — that one array
drives the upload, the remote `chmod +x` and the md5 verification. Details: `native_apps/CLAUDE.md`.

**`.app` manifests: one generator per component, on disk.** The ten `native_apps` manifests are data in
`native_apps/app-manifests.sh`; `vnc_client` and `scummvm-arca` each write theirs from **one**
heredoc into a file. They used to be `cat > … << APP` blocks inside an `ssh "$DEVICE" bash <<'REMOTE'`
heredoc, i.e. they existed only when a device was reachable — so the offline installer could not
produce the same bytes without a second copy, and an `exec=` path drifting between the two renders a
launcher tile that does nothing when tapped. **Write the manifest to a file, then copy it**; never emit
one from inside an `ssh` heredoc again.

### Redeploy scope by changed file

All three components link objects from `native_apps/common/`, so what you touched decides how much has
to go out. A *deployed* binary keeps whatever it was built with, and a stale one does not error — it
misparses. When in doubt, over-deploy — the failure mode is silent.

| Changed | Redeploy |
|---|---|
| an app's own source, `common/common.c`, `common/gamepad.c`, `common/pointer.c` | `native_apps` |
| `common/audio.c`, `common/audio_wav.c`, `common/audio_bed.c`, `common/audio.h` | `native_apps` only — **measured**: neither `vnc_client` (`Makefile` `SRCS`) nor ScummVM links `audio.o`; ScummVM reaches the device half through `audio_out.o` and never the mix-bus client layer |
| `common/logger.c`, `common/ui_focus.c`, `common/product.c` | `native_apps` + `vnc_client` — **measured** for `ui_focus.c`, and the same for `product.c`: `vnc_client/Makefile` `SRCS` links them, `configure.patch` does not name them |
| `common/framebuffer.c`, `common/touch_input.c`, `common/hardware.c`, `common/config.c`, `common/input_scan.c`, `common/overtemp.h`, `common/blank_decide.h` (read by `hardware.c`), `native_apps/sysmon/mon_ring.h` (both read by `framebuffer.c`) | **all three** — `./deploy-all.sh <ip>`; ScummVM is the slow one. ⚠️ **Measured from `scummvm-arca/backend-files/configure.patch`, which is the list** — it appends each of these `.o` to ScummVM's `OBJS`, so a header in that chain counts too |
| `common/audio_out.c`, `common/audio_gen.c` (+ `audio_out.h`, `audio_gen.h`) | `native_apps` + **ScummVM** — ⚠️ **two, not three: measured 2026-09-09**, `vnc_client/Makefile`'s `SRCS` names neither, and no `audio_out` symbol appears anywhere in that tree. They are on ScummVM's `OBJS` via `configure.patch`, which is why the row above cannot speak for them |
| anything in `device-files/` (`arca-app`, the rules files, …) | neither — **only** `./commissioning/provision.sh <ip>`, which ends in a reboot (or a new `make-card-image.sh` image, offline) |
| the four **`usb`-group** device files (`usb-host`, `enable-usb-host.sh`, `xpad-modules`, `usb-audio-modules`) | either of the above, **or** `cd usb_host && ./build-and-deploy.sh <ip>` — it compiles the `usb` group itself and, unlike them, needs no reboot. Same for the **`bluetooth`** group (`bluetooth`, `bluetooth.conf`, `bluetooth-main.conf`, `bluetooth-input.conf`) with `cd bluetooth && ./build-and-deploy.sh <ip>` |
| `usb_host/devmem_write.c`, `build-kernel-modules.sh` | `cd usb_host && ./build-and-deploy.sh <ip>` |

## Working from this host — Windows, WSL and the tools

The repo lives on `c:\work\roomwizard`; **everything that compiles or decodes lives in WSL.** These are
tool-level traps rather than device facts, and each has cost real time.

- **WSL is not just for the cross-compiler.** Git Bash has no host `gcc` and no usable `python3`, so
  the host regressions (`native_apps/tests/*_test.c`) and every `fb565_to_png.py` decode need WSL too.
  Invoke as `wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard/<component> && ./build-and-deploy.sh <ip>"`.
- ⚠️ **`command -v python3` succeeds in Git Bash and the interpreter does not exist.** It resolves to
  the Windows App Execution Alias — a real file that prints *"Python was not found"* and fails. Test by
  running `python3 --version`, not by looking it up. WSL's python3 has `PIL`, which the framebuffer
  decoder needs.
- ⚠️ **Never measure a prerequisite from Git Bash — the answer is "absent" for everything.** `gcc`,
  `arm-linux-gnueabihf-*`, `sfdisk` and `gh` are all present in WSL and all absent there, so a
  `command -v` sweep run in the wrong shell reports a host with no toolchain at all. That happened
  (2026-08-06): it was recorded as fact and made a plan entry read as a hard blocker on all building.
  **State which shell a prerequisite claim was measured in**, and measure with `wsl.exe -e bash -lc`.
  The measured inventory lives in `COMMISSIONING.md` → *The dev host*.
- ⚠️ **`strings` is one of those absences, and it fails *silently* into a wrong answer about the
  device.** `strings <device-binary> 2>/dev/null | grep -q <option>` in Git Bash prints nothing —
  because `strings` does not exist, not because the option is missing — so it reads as "the vendor's
  binary lacks that feature". Measured 2026-08-14: it said the shipped `aplay` had no
  `--dump-hw-params`, which is the flag that let a whole audio-probing phase skip the cross-build.
  **Use `grep -a` on the binary**, which works in both shells, and never route a capability question
  about a device binary through a tool whose absence looks like a negative result.
- ⚠️ **`grep -c $'\r'` in Git Bash reports every line of the file, whatever its line endings.** The
  `$'\r'` degrades to an **empty pattern**, which matches every line — so a CRLF check written that way
  says a clean LF file is entirely CRLF, and says it about every file you pass. Measured 2026-08-16:
  `SYSTEM_ANALYSIS.md` came back `1871` of 1871 lines, and `0` by two other methods. Use
  `awk 'index($0,"\r")>0'`, and cross-check with `cat -A | grep -c '\^M\$'`. Same family as the `strings`
  trap above: a tool whose failure looks like a measurement.
- ⚠️ **A pipeline reports the LAST command's exit status, so `./tests/<gate>.sh | tail` says PASS when
  the gate failed.** Same family again, and it is the *gate* it lies about rather than a measurement.
  Read `${PIPESTATUS[0]}`, or redirect to a file and `Read` it — which is what `./tests/doc_check.sh`
  needs anyway: it takes 9 s in WSL and minutes in Git Bash (measured 2026-09-27), it emits NUL bytes that make the output unreadable
  (`| tr -d '\000'`), and its destination — like EVERY scratch file — must be **outside** the repo:
  group D counts a receipt inside; a scratch copy of a source made `arca_ssh_test` FAIL (measured).
- ⚠️ **Git Bash `sed` does not expand `\n` in a REPLACEMENT** — it inserts a literal `n`, silently, and
  the corruption lands in the middle of a line you are no longer looking at. Use `Edit` for any
  multi-line splice, and ⚠️ **never splice into the middle of an existing comment block**: the result
  parses and reads as though the second half's reasoning belongs to the first half's claim.
- **Git Bash `/tmp` and WSL `/tmp` are different filesystems.** Write captures somewhere under
  `c:\work\roomwizard` and both sides reach them. ⚠️ **WSL's `/tmp` also does not survive between
  `wsl.exe` calls** — the instance idles out and takes it with it, so a fixture staged in one call can
  be gone by the next. Stage and use it inside **one** `wsl.exe -e bash -lc`, or put it under the repo.
- **The Bash tool's working directory does not reliably persist between calls.** Use absolute paths.
- **Prefer Git Bash for `git`, but the LFS reason is gone — re-measured 2026-09-06.** `git-lfs` 2.9.2
  **is** installed in this WSL now, and `git status`/`git diff` over LFS-tracked `HardwarePhotos/**` both
  exit 0 there with no filter error. The old absolute prohibition rested on
  `git-lfs filter-process: git-lfs: not found`, which no longer happens. Git Bash stays the default only
  because that is where every recorded git measurement here was taken.
- ⚠️ **Do not ask `git grep` about a past revision — measured 2026-08-15 returning a false negative.**
  `git grep -n 'open("/dev/dsp"' HEAD -- native_apps` **exits 1** on a tree where two files at `HEAD`
  each contain that exact string (`git show HEAD:<file> | grep -c` says `1` for both). The mechanism was
  not found — a double quote in the pattern and a `(` in the pattern were each tested and neither
  reproduces it alone — so the rule is the symptom, not a theory: **`git show <rev>:<file> | grep` is
  the form to trust for a before/after count.** `git grep` against the *working tree* is fine, and
  plain `grep -rn` is fine; it is the `<rev>` form that silently answered "nothing here".
- ⚠️ **A recursive `grep -r` from the repo root can exceed a 120 s tool timeout** — `scummvm/`,
  `usb_host/linux-4.14.52/` and `vnc_client/deps/` are enormous. Use `Grep`, or scope the path.
- **No foreground `sleep`** — it is blocked. Use `run_in_background`, or put the sleep inside the remote
  command: `ssh root@<ip> 'sleep 3; …'` works, because the local command is `ssh`.
- ⚠️ **`wsl.exe … | tail -N` prints nothing until the command exits, which is indistinguishable from a
  hung WSL.** Redirect to a file inside the repo and `Read` it instead, and `timeout N` anything that
  could loop — a host regression that hangs is a *test result*, not a tool timeout.
- **A compound `ssh` command can be refused by the permission classifier.** Re-issue it as one plain
  single-purpose command. The same applies to a `bash -c` heredoc whose body contains apostrophes.
- **File modes are unobservable here.** `/mnt/c` is DrvFs 9p: it reports every file `-rwxrwxrwx` and
  silently discards `chmod`. A missing-`+x` bug can neither fire nor be demonstrated on this host.
- **`shellcheck` IS installed in this WSL — re-measured 2026-09-06, version 0.7.0.** Use it; `bash -n`
  and `dash -n` (the latter on anything carrying a `/bin/sh` shebang) remain the cheaper first pass.
- **Never run a ScummVM build concurrently with a `native_apps` build.**
  `scummvm-arca/build-and-deploy.sh` does `rm -f native_apps/common/*.o` at two points —
  deliberately, because a stale x86 `.o` there fails the cross-build with "file format not recognized".
  It takes ~1m35s–2m20s; wait it out.
- **Fixtures, card captures and what a test suite cannot observe from here:** `tests/CLAUDE.md`.

## Non-obvious constraints (things that will silently break)

- **You cannot script a touch interaction, and a tool that claims to will lie to you.** There is no
  `/dev/uinput`, and evdev's `write()` path is for *output* events — so a writer to
  `/dev/input/event0` **prints "injected successfully", exits 0, and delivers nothing to any reader.**
  One shipped here for months and is deleted, not fixed. Automated on-device verification stops at
  the **first** screen (`native_apps/smoke-first-screen.sh <ip>`). Past that, write a
  tap-by-tap checklist for a human. ⚠️ **This is a limit on the *device*, not on the code** — ask
  whether the thing needs the *kernel* or only needs *events*. Detail: `native_apps/CLAUDE.md`.
- ⚠️ **Cortex-A8 has no hardware integer divide.** A binary containing an `sdiv`/`udiv` *instruction*
  crashes instantly with SIGILL (exit 132) — blank screen, no output, no log, indistinguishable from
  "the app didn't start". Verify with `native_apps/check-arm-safe.sh`, which runs from all three
  component build scripts; the expected count is a **hard zero** and it is zero. Never pass a
  `-march`/`-mcpu` that implies idiv. Which flags are safe, and the two ways to get a wrong answer out
  of the gate: `SYSTEM_ANALYSIS.md#61-cortex-a8-has-no-hardware-integer-divide`.
- **Framebuffer bpp is per-app — confirm it before decoding a screenshot.** Every native app pins
  **32bpp XRGB8888**; **ScummVM and the VNC remote session run 16bpp RGB565**. Screenshot:
  `ssh root@<ip> cat /dev/fb0 > fb.raw`, then `python3 fb565_to_png.py fb.raw fb.png` (defaults to
  32bpp; `--bpp 16` for ScummVM/VNC). ⚠️ **Run `fbset | grep geometry` on the device and believe it** —
  an app killed mid-session leaves the panel in a mode nothing running asked for, and a 32bpp frame is
  exactly the size of two 16bpp pages, so file size never catches the mistake. Table and the
  RGB565-read-as-32bpp signature: `SYSTEM_ANALYSIS.md#32-display`.
- **Screen edges are the library's problem, not the app's**, and **drawable ≠ pressable**: there are two
  rectangles, `SCREEN_VISIBLE_*` (everything drawable) and `SCREEN_SAFE_*` (visible ∩ touchable). Never
  add your own bezel arithmetic; never hardcode the touch inset, which is per-unit and measured at
  runtime. Choosing per call site needs the hit-test, not the look — `native_apps/CLAUDE.md` →
  *Screen edges*. Numbers and method: `SYSTEM_ANALYSIS.md#33-touch`.
- **Touch coordinates must be captured before the `BTN_TOUCH` press event** (order: ABS_X, ABS_Y,
  BTN_TOUCH, SYN_REPORT), and `common/touch_input.c` maps a reading in **two stages** — raw 12-bit →
  **panel** pixel by a per-axis piecewise-linear curve, then panel → **logical** by subtracting the
  bezel viewport origin. Stage 1 is calibration, stage 2 is the bezel; a fit for stage 1 must be done
  in panel coordinates or the bezel is subtracted twice. The fit lives in `common/touch_calib.c` and
  nowhere else. ⚠️ **ScummVM and `vnc_client` each link their own `touch_input.o`, so redeploy every
  component after changing that file or the config format** — a stale binary does not error: its
  4-number `sscanf` succeeds on the first four of the eight values and touch silently collapses.
  Extend the config with keyword-tagged trailing lines; **never change line 1's field count.**
  Authoring rules: `native_apps/CLAUDE.md` → *Touch model*.
- **Don't probe I2C bus 1.** `pv02_app 5` (the vendor light-sensor factory test) can hang the bus, and
  **bus 1 carries the PMIC** — `SYSTEM_ANALYSIS.md#39-i2c`. There is no light sensor to find.
- ⚠️ **A device can be running an older copy than the repo's until `commissioning/provision.sh` is
  re-run** — check `--status` (read-only, no reboot) **before reproducing anything against a device**, or
  you will draw conclusions about code the device is not running. The *hardware* watchdog
  (`/dev/watchdog`, 60 s) is fed by `/sbin/watchdog` from `rcS.d/S06watchdog` — keep it.
- **Audio via OSS `/dev/dsp` is buggy**: open with `O_NONBLOCK` and handle `EAGAIN`, attenuate ~50 %,
  and set SPEED→FMT→CHANNELS then read back with `SOUND_PCM_READ_*` because those ioctls reset each
  other. The hardware is permanently mono. `SYSTEM_ANALYSIS.md#34-audio`.
- **32-bit ARM:** `sizeof(long) == 4`. Never do `(now.tv_sec - 0) * 1000000L` — it overflows. Baseline
  timers to current time, not epoch 0.
- **Firmware / boot edits — you are root, and every write is allowed; know what each costs to undo.** A
  bad `mtd0` needs JTAG (dump it first); a bad p1 file needs a card reader (keep the vendor copy beside
  it); a kernel staged under a *new* filename is a free undo. Table: `SYSTEM_ANALYSIS.md#what-a-boot-path-write-costs-to-undo`. **Only our own kernel image
  is supported**, and nothing scripted writes `uImage-system`: installing it is a manual operator step
  (`kernel/README.md`), and the p1 rules are in `lib/CLAUDE.md`. The vendor-kernel byte-patch path is
  deleted; tag `last-vendor-kernel` is the last tree that ran on the vendor kernel, and `last-vendor-rootfs` the last that can commission the vendor root filesystem. Recovery:
  `SYSTEM_ANALYSIS.md#4-boot-chain-and-recovery`.

## Cross-component build rules

Full detail, measurements and the flags each component uses:
`SYSTEM_ANALYSIS.md#6-building-for-this-device`.

- ⚠️ Static links use plain `-lpthread`, never `--whole-archive`; crash record and its status (cause
  unproven): `SYSTEM_ANALYSIS.md#62-never-use---whole-archive-with--lpthread`.
- **Verify build artifacts on disk, not config flags.** Generated `config.mk`/`config.h` go stale. Test
  for the `.a`, not the flag.
- **Cross-compiled dependencies must be built from source** — this WSL cannot do armhf multiarch. With
  `-mfpu=neon`, libpng needs `-DPNG_ARM_NEON_OPT=0`.
- **No `SCHED_RR` audio thread.** On this single 600 MHz core an RT audio thread starves the main thread
  and you get a black screen.
- **The DSS overlay scaler is deliberately unused** (operator, 2026-10-04: low return; it always filters and
  the result was rejected at the panel) — apps draw to `fb0`. Facts: `SYSTEM_ANALYSIS.md#32-display`.
- Before optimising a software renderer, read
  `SYSTEM_ANALYSIS.md#65-software-rendering-techniques-that-paid-off`.

## Architecture

| Layer | Script | When |
|---|---|---|
| Front door (menu over everything below) | `arca.sh` | whenever you'd rather not remember the flags |
| Whole-card image; `--bundle` adds the no-prompt installer `commissioning/commission-offline.sh` | `rootfs/make-card-image.sh` | once per card, offline |
| Update a running unit (backup, install, reboot) | `commissioning/provision.sh` | per update, over SSH |
| Build + deploy all components | `deploy-all.sh` | per deploy |
| Per-component build/deploy/manifest | `*/build-and-deploy.sh` | per component |
| Build + stage + publish an offline bundle | `release.sh` | per release |
| App respawn loop at boot | `device-files/arca-app` | every boot |

`arca.sh` is a **composition layer with no logic of its own** except `wait_for_ssh` — which polls
**SSH, not ping**, because ping answers while `sshd` is still starting. Every item execs one of the
scripts below it, and all of them stay non-interactive when called directly.

⚠️ **Never identify a partition by filesystem UUID** — a UUID names one *card*, and two units on
identical firmware share none of their four. `lib/arca-identify.sh` is the one implementation: content
for a mounted rootfs, the partition table for a disk, **position** for which partition holds which
tree. ⚠️ **p1 is deliberately absent from `ARCA_PART_ROLES`, and a test asserts its absence**, so nothing
can reach `mlo`/`u-boot.bin`/`ctrlblock.bin` through those functions. ⚠️ **And a rootfs mounted offline
shows `/home/root/{data,log}` as EMPTY directories** — they are mount points for p2/p3 (p5 stays in
the table, unused and unmounted), so a tool that mounts only p6 can report success having touched
neither. All three:
`lib/CLAUDE.md`.

**A script's executable bit lives in the git index, so one bad commit breaks every fresh clone.** All
`*.sh` are `100755` there — check with `git ls-files -s -- '*.sh'` after adding one. Belt and braces:
`arca.sh` and `deploy-all.sh` invoke their children as `bash <script>`, never `./<script>`.

**Stopping what is running belongs to the init script, and it matches on the executable.**
`/etc/init.d/arca-app stop` is the only implementation; the three component scripts call it and
**must not carry a `killall` of their own** — a name-based rule cannot see the app that `app_launcher`
*started*, and that grandchild is normally the process holding `/dev/fb0`. Detail:
`device-files/CLAUDE.md`.

**App launcher + manifests.** The boot init script reads `/opt/arca/default-app` and respawns that
binary whenever it exits (so exiting a game returns to the launcher). `app_launcher` scans
`/opt/arca/apps/*.app` manifests (INI: `name=`, `exec=`, `icon=` PPM P6, `args=` one of
`fb,touch`/`fb`/`touch`/`none`) and renders them as a touch grid. Each component's deploy script writes
its own manifests + PPM icons — this is how projects plug into the launcher without a central registry.
`SYSTEM_ANALYSIS.md#53-app-launcher-and-manifests`.

**native_apps shared C library (`native_apps/common/`)** — every game and tool links these:
`framebuffer.c` (double-buffered 800×480, bpp-aware primitives), `gamepad.c` (**the** unified input
abstraction across touch + USB keyboard + mouse + Xbox pad — new apps should use this rather than
reading evdev directly), `touch_input.c`, `touch_calib.c`, `hardware.c`, `common.c`, `ui_layout.c`,
`audio.c`, `audio_gen.c`, `config.c`, `keyboard.c`, `highscore.c`, `ppm.c`, `logger.c`, `ui_focus.c`, `pointer.c`, `start_menu.c`. The
per-function table and what never to do instead: `native_apps/CLAUDE.md`.

ScummVM has its own evdev reader (`arca-events.cpp`, on the shared `input_scan.c`) but **not its own audio**: `oss-mixer.cpp` opens no `/dev/dsp` and drives the device through `common/audio_out.o` (`configure.patch`; measured by grep of `backend-files/`); it never links `audio.o`.
It shares the Cortex-A8 / OSS fixes through that library.

## Device paths worth knowing

- `/opt/games/` — native binaries · `/opt/arca/apps/` — `.app` manifests ·
  `/opt/arca/icons/` — PPM icons · `/opt/arca/default-app` — boot target
- `/dev/fb0` (framebuffer) · `/dev/input/event*` (evdev) ·
  `/sys/class/leds/{red_led,green_led,backlight}/brightness` (0–100)
- `/dev/dsp` (OSS audio, GPIO12 must be HIGH to unmute the speaker) · `/dev/watchdog` (60 s HW)
- `/etc/touch_calibration.conf` (touch curve + bezel) · `/etc/input_config.conf` (input mapping)
