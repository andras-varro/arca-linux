# ScummVM Engine Addition Plan — Arca

> **Target:** Steelcase RoomWizard II — ARM Cortex-A8 600 MHz, 182 MB RAM, no GPU, 800×480 framebuffer, Linux 4.14  
> **Current working engines (8):** scumm, scumm-7-8, he, agi, sci, agos, sky, queen  
> **Last updated:** 2026-03-23

---

## 1. Strategy Overview

### Why One-at-a-Time?

Previous attempts to enable all ScummVM engines simultaneously caused a **segfault before `main()`** — the binary crashed during static initialization, before any of our code executed. Root causes included:

- **Binary size explosion** — the stripped binary exceeded what the device could load/relocate
- **Static initializers** — many engines register plugin constructors via `REGISTER_PLUGIN_STATIC()`, which run before `main()`. A single bad initializer crashes the entire binary with no diagnostic output
- **RAM exhaustion** — the device has only 182 MB total; kernel + services consume ~80 MB, leaving ~100 MB for ScummVM

With a segfault-before-main there is **no log output, no backtrace, no way to identify the culprit** when multiple engines are added at once. The only reliable approach is:

```
Add ONE engine → Build → Deploy → Test → If OK, proceed → If crash, revert & investigate
```

### Build Script Integration

The `build-and-deploy.sh` script supports an `ENGINE_BATCH` variable (values 1–5) that controls which group of engines is enabled:

| `ENGINE_BATCH` | Description |
|:-:|---|
| 0 | Base engines only (current 8) |
| 1 | Base + Batch 1 (zero-dep, small) |
| 2 | Base + Batch 1–2 (zero-dep, larger) |
| 3 | Base + Batch 1–3 (highres/16-bit) |
| 4 | Base + Batch 1–4 (built-in features) |
| 5 | Base + Batch 1–5 (external libs) |

Within each batch, engines are added **one at a time** during testing. The batch variable is for convenience after a batch is fully validated.

### Test Procedure (per engine)

1. Add `--enable-engine=<name>` to the configure flags
2. Build in WSL: `./build-and-deploy.sh build`
3. Record stripped binary size
4. Deploy: `./build-and-deploy.sh deploy`
5. SSH to device, verify: `/opt/games/scummvm --list-engines | grep <engine>`
6. If it starts without segfault → ✅ proceed to next engine
7. If segfault → ❌ revert, investigate, file notes
8. Commit after each successful batch

---

## 2. Engine Addition Queue

### Status Legend

| Symbol | Meaning |
|:-:|---|
| ⬜ | Untested |
| ✅ | Works — engine loads, binary starts |
| ❌ | Failed — segfault or build error |
| 🔧 | Fixed — failed initially, resolved |
| ⏭️ | Skipped — intentionally deferred |

---

### Batch 1 — Zero Dependencies (smallest/simplest first)

These engines have no external library requirements and are among the smallest in code size. Ideal for establishing a baseline and confirming the incremental approach works.

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | lure | `--enable-engine=lure` | Lure of the Temptress | None | ⬜ | |
| 2 | drascula | `--enable-engine=drascula` | Drascula: The Vampire Strikes Back | None | ⬜ | |
| 3 | touche | `--enable-engine=touche` | Touché: The Adventures of the Fifth Musketeer | None | ⬜ | |
| 4 | teenagent | `--enable-engine=teenagent` | Teen Agent | None | ⬜ | |
| 5 | tucker | `--enable-engine=tucker` | Bud Tucker in Double Trouble | None | ⬜ | |
| 6 | hugo | `--enable-engine=hugo` | Hugo trilogy | None | ⬜ | |
| 7 | draci | `--enable-engine=draci` | Drací Historie | None | ⬜ | |
| 8 | plumbers | `--enable-engine=plumbers` | Plumbers Don't Wear Ties | None | ⬜ | |
| 9 | supernova | `--enable-engine=supernova` | Mission Supernova 1 & 2 | None | ⬜ | |
| 10 | efh | `--enable-engine=efh` | Escape from Hell | None | ⬜ | |
| 11 | cge | `--enable-engine=cge` | Sołtys | None | ⬜ | |
| 12 | cge2 | `--enable-engine=cge2` | Sfinx | None | ⬜ | |
| 13 | dreamweb | `--enable-engine=dreamweb` | DreamWeb | None | ⬜ | |
| 14 | bbvs | `--enable-engine=bbvs` | Beavis and Butt-Head in Virtual Stupidity | None | ⬜ | |
| 15 | cine | `--enable-engine=cine` | Future Wars, Operation Stealth | None | ⬜ | |
| 16 | cruise | `--enable-engine=cruise` | Cruise for a Corpse | None | ⬜ | |
| 17 | lab | `--enable-engine=lab` | The Labyrinth of Time | None | ⬜ | |
| 18 | parallaction | `--enable-engine=parallaction` | Nippon Safes Inc., The Big Red Adventure | None | ⬜ | |

---

### Batch 2 — Zero Dependencies (larger/more popular)

These engines are still dependency-free but are larger codebases or support more games. Binary size growth should be watched more carefully here.

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | kyra | `--enable-engine=kyra` | Legend of Kyrandia 1–3, Lands of Lore | None | ⬜ | |
| 2 | sword1 | `--enable-engine=sword1` | Broken Sword: Shadow of the Templars | None | ⬜ | |
| 3 | sword2 | `--enable-engine=sword2` | Broken Sword II: The Smoking Mirror | None | ⬜ | |
| 4 | tinsel | `--enable-engine=tinsel` | Discworld 1 & 2 | None | ⬜ | |
| 5 | gob | `--enable-engine=gob` | Gobliiins series, Bargon Attack, others | None | ⬜ | |
| 6 | saga | `--enable-engine=saga` | Inherit the Earth, I Have No Mouth | None | ⬜ | |
| 7 | tsage | `--enable-engine=tsage` | Ringworld, Blue Force | None | ⬜ | |
| 8 | made | `--enable-engine=made` | Return to Zork, Rodney's Funscreen | None | ⬜ | |
| 9 | mads | `--enable-engine=mads` | Rex Nebular, Dragonsphere | None | ⬜ | |
| 10 | toon | `--enable-engine=toon` | Toonstruck | None | ⬜ | |
| 11 | sherlock | `--enable-engine=sherlock` | The Case of the Serrated Scalpel, Rose Tattoo | None | ⬜ | |
| 12 | access | `--enable-engine=access` | Amazon: Guardians of Eden, Martian Memorandum | None | ⬜ | |
| 13 | kingdom | `--enable-engine=kingdom` | Kingdom: The Far Reaches | None | ⬜ | |
| 14 | mm | `--enable-engine=mm` | Might and Magic series | None | ⬜ | |
| 15 | voyeur | `--enable-engine=voyeur` | Voyeur | None | ⬜ | |
| 16 | hypno | `--enable-engine=hypno` | Wetlands, Spiderman (Sinister Six) | None | ⬜ | |
| 17 | chewy | `--enable-engine=chewy` | Chewy: Esc from F5 | None | ⬜ | |
| 18 | gnap | `--enable-engine=gnap` | U.F.O.s | None | ⬜ | |
| 19 | illusions | `--enable-engine=illusions` | Duckman, Beavis and Butt-Head (another) | None | ⬜ | |

---

### Batch 3 — Need Highres or 16-bit (already available)

These engines require 16-bit color or higher resolution support, both of which the Arca backend already provides (800×480, RGB565/RGB888). No new libraries needed, but these engines tend to be more complex.

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | griffon | `--enable-engine=griffon` | The Griffon Legend | 16-bit color | ⬜ | |
| 2 | neverhood | `--enable-engine=neverhood` | The Neverhood | 16-bit color | ⬜ | |
| 3 | composer | `--enable-engine=composer` | Magic Tales series | 16-bit color | ⬜ | |
| 4 | mortevielle | `--enable-engine=mortevielle` | Mortville Manor | 16-bit color | ⬜ | |
| 5 | toltecs | `--enable-engine=toltecs` | 3 Skulls of the Toltecs | 16-bit color | ⬜ | |
| 6 | prince | `--enable-engine=prince` | The Prince and the Coward | 16-bit color | ⬜ | |
| 7 | hadesch | `--enable-engine=hadesch` | Hades Challenge | 16-bit color | ⬜ | |
| 8 | pink | `--enable-engine=pink` | Peril of the Pink Panther | 16-bit color | ⬜ | |
| 9 | private | `--enable-engine=private` | Private Eye | 16-bit color | ⬜ | |
| 10 | asylum | `--enable-engine=asylum` | Sanitarium | 16-bit color | ⬜ | |
| 11 | cryomni3d | `--enable-engine=cryomni3d` | Versailles 1685 | 16-bit color | ⬜ | |
| 12 | dragons | `--enable-engine=dragons` | Blazing Dragons | 16-bit color | ⬜ | |
| 13 | hopkins | `--enable-engine=hopkins` | Hopkins FBI | 16-bit color | ⬜ | |
| 14 | tony | `--enable-engine=tony` | Tony Tough | 16-bit color | ⬜ | |
| 15 | ngi | `--enable-engine=ngi` | Full Pipe | 16-bit color | ⬜ | |
| 16 | bladerunner | `--enable-engine=bladerunner` | Blade Runner | 16-bit, CPU-heavy | ⬜ | May be slow at 600 MHz |
| 17 | pegasus | `--enable-engine=pegasus` | The Journeyman Project: Pegasus Prime | 16-bit color | ⬜ | |
| 18 | freescape | `--enable-engine=freescape` | Driller, Dark Side, etc. | 16-bit color | ⬜ | |
| 19 | mtropolis | `--enable-engine=mtropolis` | Muppet Treasure Island, Obsidian | 16-bit, complex | ⬜ | Large engine |
| 20 | trecision | `--enable-engine=trecision` | Starship Titanic (Trecision) | 16-bit color | ⬜ | |
| 21 | adl | `--enable-engine=adl` | Hi-Res Adventure series | Highres | ⬜ | |
| 22 | vcruise | `--enable-engine=vcruise` | Reah, Schizm | 16-bit color | ⬜ | |

---

### Batch 4 — Need Built-in Features (Lua, Bink, TinyGL, PNG)

These engines require ScummVM built-in subsystems to be compiled in. No external cross-compiled libraries, but the features add binary size and complexity.

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | hdb | `--enable-engine=hdb` | Hyperspace Delivery Boy | Lua (built-in) | ⬜ | |
| 2 | mohawk | `--enable-engine=mohawk` | Myst, Riven (base), Living Books | Built-in codecs | ⬜ | |
| 3 | ultima | `--enable-engine=ultima` | Ultima IV–VIII, Ultima Underworld | Built-in, large | ⬜ | Very large engine |
| 4 | twine | `--enable-engine=twine` | Twine interactive fiction | Built-in | ⬜ | |
| 5 | grim | `--enable-engine=grim` | Grim Fandango, Escape from Monkey Island | TinyGL (software 3D) | ⬜ | ⚠️ CPU-heavy — TinyGL software rendering at 600 MHz will be very slow. May be unplayable. |
| 6 | director | `--enable-engine=director` | Hundreds of Director/Shockwave titles | Very large codebase | ⬜ | ⚠️ May need to skip — enormous binary size increase, high RAM usage |

---

### Batch 5 — Need Cross-compiled Libraries

These engines require external libraries that must be cross-compiled for the ARM toolchain and linked statically. Each sub-batch groups engines by their shared library dependency.

#### Sub-batch 5a — libjpeg

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | groovie | `--enable-engine=groovie` | The 7th Guest, The 11th Hour, Clandestiny | libjpeg | ⬜ | |
| 2 | wintermute | `--enable-engine=wintermute` | Many WME games (J.U.L.I.A., etc.) | libjpeg | ⬜ | Large engine |

#### Sub-batch 5b — freetype2

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | buried | `--enable-engine=buried` | The Journeyman Project 2: Buried in Time | freetype2 | ⬜ | |
| 2 | zvision | `--enable-engine=zvision` | Zork: Grand Inquisitor, Zork Nemesis | freetype2 | ⬜ | |
| 3 | petka | `--enable-engine=petka` | Red Comrades series | freetype2 | ⬜ | |

#### Sub-batch 5c — Tremor (integer Vorbis)

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | nancy | `--enable-engine=nancy` | Nancy Drew series | tremor/vorbis | ⬜ | |

#### Sub-batch 5d — libmad (MP3 decoding)

| # | Engine | Configure Flag | Games Enabled | Dependencies | Status | Notes |
|--:|--------|---------------|---------------|:------------:|:------:|-------|
| 1 | ags | `--enable-engine=ags` | Adventure Game Studio games (thousands) | libmad | ⬜ | Very large engine |
| 2 | titanic | `--enable-engine=titanic` | Starship Titanic | libmad | ⬜ | |

---

## 3. Testing Procedure

### Per-Engine Checklist

```bash
# Step 1: Enable the engine
# Edit build-and-deploy.sh, add: --enable-engine=<engine_name>

# Step 2: Build (in WSL)
./build-and-deploy.sh build

# Step 3: Record binary size
ls -lh /path/to/scummvm   # note size in tracking table

# Step 4: Deploy to device
./build-and-deploy.sh deploy

# Step 5: Verify engine is listed
ssh root@<ip> '/opt/games/scummvm --list-engines' | grep <engine_name>

# Step 6: Verify no segfault at startup
ssh root@<ip> '/opt/games/scummvm --list-engines'
# If this completes without segfault, the engine loads OK

# Step 7: Check RAM usage
ssh root@<ip> 'ps aux | grep scummvm'  # note RSS column

# Step 8: Mark status
# ✅ if OK → proceed to next engine
# ❌ if segfault → revert the --enable-engine line, investigate
```

### Per-Batch Completion

After all engines in a batch are validated:

1. Commit the updated `build-and-deploy.sh` with all batch engines enabled
2. Update this document — mark all statuses
3. Record final binary size and RAM baseline for the batch
4. Tag the commit: `engines-batch-N-complete`

### Regression Testing

After adding each new batch, verify that previously-working engines still function:

```bash
ssh root@<ip> '/opt/games/scummvm --list-engines' | wc -l
# Should equal: 8 (base) + all previously added engines + new engine
```

---

## 4. Tracking Table

Use this table to record measurements as engines are added:

| # | Engine | Batch | Binary Size | Binary Delta | RAM at Startup (RSS) | Status | Notes |
|--:|--------|:-----:|------------|-------------|---------------------|:------:|-------|
| — | *base (8 engines)* | 0 | ___ MB | — | ___ MB | ✅ | Baseline |
| 1 | lure | 1 | ___ MB | +___ KB | ___ MB | ⬜ | |
| 2 | drascula | 1 | ___ MB | +___ KB | ___ MB | ⬜ | |
| 3 | touche | 1 | ___ MB | +___ KB | ___ MB | ⬜ | |
| ... | ... | ... | ... | ... | ... | ... | ... |

> **Tip:** After deploying, get binary size with `ls -l /opt/games/scummvm` and RSS with:
> ```bash
> /opt/games/scummvm --list-engines > /dev/null &
> PID=$!; sleep 2; grep VmRSS /proc/$PID/status; kill $PID
> ```

---

## 5. Known Risks

### Binary Size

- The stripped ScummVM binary must stay **under ~50 MB** to load reliably on the device
- Each engine adds roughly 200 KB–5 MB depending on complexity
- Monitor with `ls -l` after each build; if approaching 45 MB, consider stopping
- The `director` and `ags` engines are the largest and may push past limits alone

### Static Initializers (Segfault-Before-Main)

- ScummVM's static plugin system uses `REGISTER_PLUGIN_STATIC()` macros that create global constructors
- These run before `main()`, meaning a crash in any one of them kills the process silently
- The one-at-a-time approach is specifically designed to isolate these failures
- If a segfault occurs: revert the last engine, confirm recovery, then investigate the specific engine

### Memory (RAM)

- Total device RAM: 182 MB
- Kernel + services at idle: ~80 MB
- **Target:** ScummVM RSS at startup (before loading a game) must stay **under 100 MB**
- Some games will use more RAM at runtime — this is per-game and acceptable as long as startup works
- The `grim` engine (TinyGL) and `wintermute` engine are known heavy RAM users

### Linker Flags (--whole-archive / -lpthread)

- A previous build issue involved `--whole-archive` pulling in all symbols from all libraries, causing massive binary bloat and unresolved symbols
- The current build uses the **non-whole-archive** approach — do NOT reintroduce `--whole-archive`
- The `-lpthread` must be linked but not via whole-archive; verify with:
  ```bash
  arm-linux-gnueabihf-readelf -d scummvm | grep NEEDED
  ```

### CPU Performance

- At 600 MHz with no GPU, engines that rely on software rendering will be slow
- `grim` (TinyGL 3D), `bladerunner` (complex scenes), and `mtropolis` (video-heavy) are the main concerns
- These may technically build and load but be **unplayable** — mark as ✅ for loading, note performance in comments

---

## 6. Cannot Enable (Hardware Limitations)

The following engines require **OpenGL GPU hardware** which the Steelcase RoomWizard does not have. They cannot be enabled regardless of binary size or RAM:

| Engine | Reason | Games |
|--------|--------|-------|
| hpl1 | Requires OpenGL GPU | Penumbra: Overture |
| watchmaker | Requires OpenGL GPU | The Watchmaker |

These are **permanently excluded** from the build and should not be attempted.

---

## 7. Quick Reference — Current Configure Line

Base engines (always enabled):

```bash
--enable-engine=scumm \
--enable-engine=scumm_7_8 \
--enable-engine=he \
--enable-engine=agi \
--enable-engine=sci \
--enable-engine=agos \
--enable-engine=sky \
--enable-engine=queen
```

Add new engines below this block, one line per engine, in batch order.

---

## 8. Estimated Timeline

| Batch | Engines | Estimated Time | Cumulative |
|:-----:|:-------:|:--------------:|:----------:|
| 1 | 18 | 1–2 sessions | 1–2 sessions |
| 2 | 19 | 1–2 sessions | 2–4 sessions |
| 3 | 22 | 2–3 sessions | 4–7 sessions |
| 4 | 6 | 1–2 sessions | 5–9 sessions |
| 5 | 7 | 2–3 sessions (includes cross-compiling libs) | 7–12 sessions |

> A "session" is roughly one build-deploy-test cycle per engine, assuming no failures. Failures add investigation time.
