/*
 * audio_mix_test — the mix bus, driven by hand
 *
 * `common/audio.c` mixes every sound on one bus and delivers it through
 * `common/audio_out.c`'s never-reset continuous stream, serviced by a per-frame
 * `audio_pump()` — the library's only playback mode.  Everything about it that is
 * arithmetic is host-tested (`tests/audio_gen_test.c`, groups I/J/K).  What no host
 * can answer is whether two sounds at once are AUDIBLE as two sounds on a 20 mm
 * speaker that sums L + R, and how short a tone can be and still be heard.  Both
 * need an ear at the panel, so this is the tool for that trip.
 *
 * What each row is for:
 *
 *   controls   LIM / LVL / STOP.  STOP is `audio_interrupt()`, "silence every
 *              voice" — note it cannot un-write what is already inside the device
 *              (one lead, ~139 ms).
 *              ⚠️ **LIMIT is the second negative control, added after the first
 *              panel session.** With `clip` at 15402 the operator heard mixed
 *              sounds as *"a distorted square wave from an overdriven
 *              amplifier"* — three voices at `AUDIO_PEAK` sum to 54000 against
 *              int16's 32767.  `LIMIT: SOFT` is the fix (a knee at one voice's
 *              peak, asymptotic to `AUDIO_MIX_CEIL`); `LIMIT: HARD` restores the
 *              rejected clamp so the difference can be heard rather than argued.
 *   tones      DRONE is 3 s at 220 Hz, `440 3s` is 3 s at 440 Hz, and the other
 *              three are 200 ms at 440 / 880 / 1760 Hz.  Tap a long one, then tap
 *              the others while it runs.  The pitches are far apart on purpose:
 *              "one tone or two" must not depend on the listener keeping count.
 *              ⚠️ **The two 3 s pads exist for a TIMBRE question, which a 200 ms
 *              blip cannot answer.** Defect 3's decisive test is whether ONE voice
 *              at `AUDIO_PEAK` sounds like a clean sine — the operator compared the
 *              device against a phone signal generator and heard a square wave —
 *              and that comparison needs a tone that sustains.  `440 3s` alone is
 *              one voice through a limiter whose knee is `AUDIO_PEAK` exactly, so
 *              it is byte-identical to the unlimited path: if it still sounds
 *              square, **the limiter is exonerated** and the fault is upstream of
 *              the mix entirely.  `440 3s` + DRONE is the same question for a
 *              two-voice sum, sustained long enough to compare.
 *   canned     the four sounds every game uses, unchanged signatures.  SUCCESS
 *              and FAIL are three notes each, i.e. three voices with start
 *              offsets — if either sounds like a CHORD rather than an arpeggio,
 *              the offsets are broken.  CHORD deliberately plays three notes
 *              together, so there is something to compare to.
 *   ms row     the minimum audible tone.  Same 880 Hz tone at 5 / 10 / 20 / 40 /
 *              60 / 100 ms.  Walk up the row and note the shortest one you can
 *              hear.  Nothing clamps a tone's length; what the old floor really
 *              was is ../SYSTEM_ANALYSIS.md#34-audio gotcha 6.  Each stimulus is
 *              chosen by the operator, so it is self-identifying by construction —
 *              no marker clicks needed.
 *   sample row `WAV 1` / `WAV 2` / `W STOP` / `SFX` / `INH 622` — the sample voice,
 *              **this row is the experiment it exists for.** Four causes of
 *              the two-voice harshness are refuted and the survivor is this speaker
 *              on sustained pure sine PAIRS. Every other
 *              pad here is a synthesised sine, so nothing above this row can test
 *              that. Tap `WAV 1`, let the bed settle, then tap `SFX` over it: same
 *              bus, same limiter, same delivery, different waveform. If that is
 *              clean while `440 3s` + `880` is not, the answer is "effects should be
 *              samples" and the question closes as a product decision.
 *              ⚠️ **`W STOP` is not the top row's `STOP`** — it releases only the
 *              bed, so an effect over it keeps sounding.
 *              ⚠️ **`INH 622` is here because every tone pad above is an OCTAVE of
 *              every other** (220/440/880/1760), so the tool could not make an
 *              inharmonic pair and `CHORD` was the only substitute. 622 against 440
 *              is within 0.03 % of √2 — the tritone, minimum harmonic coincidence.
 *              The two music files are hand-copied, not in the repo, so
 *              "cannot open" is a deployment fact rather than a code fault.
 *
 * ⚠️ **Two of this tool's INSTRUMENTS were lying and both are fixed (2026-08-20).
 * The first invalidated a recorded refutation, which is the expensive kind:**
 *
 *   - **`CHORD` was an arpeggio.** Three back-to-back `audio_tone()` calls are
 *     consecutive statements, so `AUDIO_TONE_CHAIN_MS`'s recency gate read them as
 *     one motif and chained each behind the last. A refutation says "HARD vs SOFT was
 *     inaudible, therefore clipping is refuted" — and that A/B was judged with this
 *     pad, which never put two voices on the bus at once. It is now three
 *     `audio_mix_add()` calls at delay 0.
 *   - **The limiter label printed the tool's own variable.** `audio_mix_init()` sets
 *     `AUDIO_MIX_HARD` and the tool's bool started false, so the pad read `LIM:
 *     SOFT` over a HARD bus, the log agreed with it, and the first tap "turned SOFT
 *     on" by setting HARD. There is no local copy any more: the pad, the log and
 *     the toggle all read `audio_mix_get_limit()`. **An A/B tool must read its own
 *     toggles back from the library.**
 *
 * ⚠️ **The five row offsets are DERIVED from `SCREEN_SAFE_HEIGHT`, not written
 * down** — `rows_layout()`, whose comment carries what a heavily-inset panel pays
 * the shortfall with and why. A fifth row is what made that necessary: the agreed
 * offsets end at 406 and at the 48/48 inset cap the safe height is 384.
 *
 * The readout shows live voices and five counters: `clip` (samples the int16
 * store could not hold — ⚠️ **must be 0 with LIMIT: SOFT**, that is the check
 * that the limiter is engaged), `lim` (samples the soft knee bent — expected to
 * be large, not a fault), `starve` (pumps that found the ring dry with audio
 * still owed — **each one is an audible gap, and it attributes crackle to PACING
 * rather than to mixing**), `lost` (frames the device refused after the voices had
 * already advanced past them) and `drop` (sounds refused by a full bus).
 *
 * ⚠️ **`clip == 0` is NOT evidence of a clean mix** — it proves int16 did not
 * overflow, and the soft limiter guarantees that by construction.  The limiter
 * waveshapes the sum instead, which IS harmonic distortion, and this tool once
 * read PASS on every counter while the operator heard every sum as "a big
 * distortion".  `lim` is the number to read
 * for that: large `lim` means the sum is being bent, whatever `clip` says.
 *
 * ⚠️ **Two of this tool's own numbers were WRONG until 2026-08-16, and both made
 * a panel session harder to trust than the code it was judging:**
 *
 *   - **`lead` was `AUDIO_PUMP_LEAD_MS`, a constant.**  The library floors the
 *     lead at whole device periods, so the panel displayed 80 ms while the pump
 *     held ~139 — and the worst-frame warning was coloured against the same wrong
 *     number.  It now reads `audio_pump_lead()`/`audio_pump_period()` and prints
 *     the arithmetic (`139 ms (3x46)`), or `lead ?` before the first pump has
 *     measured anything.  **An A/B tool must report what the library did.**
 *   - **Every tap went to `stderr` with no `fopen` anywhere**, so from the
 *     launcher tile the log went nowhere at all and the claim that taps were
 *     recorded was false.  `main()` now `freopen()`s `stderr` onto
 *     `MIX_LOG_PATH` — which captures the library's own stderr lines in the same
 *     file, in order, with no library change.
 *
 * `worst frame` restarts on a STOP tap, because a 2474 ms first frame at boot
 * contention is not a property of the mix bus and must not sit on the panel
 * looking like one — tap STOP once the panel has settled, then measure.
 *
 * CPU is the other open question (mixing on a 600 MHz core with no FPU-friendly
 * sin()).  Measure it from another shell while sound is playing:
 *   ssh root@<ip> "top -b -n 2 | grep audio_mix_test"
 *
 * Run on device (the log needs no redirect — the tool opens it itself):
 *   /opt/games/audio_mix_test /dev/fb0 /dev/input/touchscreen0
 *   ssh root@<ip> cat /tmp/mix.log
 *
 * Build (from native_apps/).  ⚠️ `common/audio_out.c` is not optional: `audio.h`
 * includes `audio_out.h` and every `Audio` embeds an `AudioOut`, so the link fails
 * without it — which is the right failure.  `build-and-deploy.sh` gets it from
 * `$COMMON_OBJ`; this line is for building the tool by hand, with that script's
 * soft-float compiler and ALSA flags.
 *   arm-linux-gnueabi-gcc -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp \
 *     -O2 -I. -DAUDIO_OUT_HAVE_ALSA -Iarm-deps-softfp/usr/include tests/audio_mix_test.c \
 *     common/audio.c common/audio_gen.c common/audio_out.c common/audio_wav.c \
 *     common/touch_input.c \
 *     common/framebuffer.c common/font.c common/hardware.c common/common.c \
 *     common/config.c common/highscore.c common/keyboard.c \
 *     -o build/audio_mix_test -lm -Larm-deps-softfp/usr/lib -lasound
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

#include "../common/audio.h"
#include "../common/audio_gen.h"
#include "../common/touch_input.h"
#include "../common/framebuffer.h"
#include "../common/hardware.h"
#include "../common/common.h"

static volatile bool running = true;
static void sig_handler(int s) { (void)s; running = false; }

/** Where the tap log and the library's own stderr lines both land.
 *
 * ⚠️ **`stderr` was not a log.**  This tool is normally started from the launcher
 * tile, whose child inherits an init script's stderr and therefore throws it away;
 * there was no `fopen` anywhere in the file, so "every tap is logged" was false for
 * every session that mattered.  Redirecting the STREAM rather than opening a
 * private `FILE *` is deliberate: `audio.c`'s refusals and its `bus closed`
 * counter line also write `stderr`, and this way they and the taps interleave in
 * one file, in order, with no change to the library. */
#define MIX_LOG_PATH  "/tmp/mix.log"

/** ⚠️ freopen() CLOSES the stream before it opens the target, so a failure leaves
 *  `stderr` closed — writes then vanish silently rather than crashing.  Say so on
 *  stdout, which an SSH launch can still see, instead of assuming /tmp is
 *  writable. */
static void open_log(void)
{
    if (freopen(MIX_LOG_PATH, "a", stderr)) {
        setvbuf(stderr, NULL, _IONBF, 0);   /* a session that ends in SIGKILL must
                                             * still have its lines on disk */
        /* Local wall-clock time, so this log lines up with /var/log/messages. */
        char hms[9] = "??:??:??";
        time_t t = time(NULL);
        struct tm tm;
        if (localtime_r(&t, &tm)) strftime(hms, sizeof(hms), "%H:%M:%S", &tm);
        fprintf(stderr, "\n=== audio_mix_test session start === %s\n", hms);
    } else {
        printf("audio_mix_test: cannot open %s — taps will not be logged\n",
               MIX_LOG_PATH);
    }
}

/* ── pads ────────────────────────────────────────────────────────────────── */

typedef enum {
    ACT_LIMIT, ACT_LEVEL, ACT_STOP,
    ACT_DRAW,                       /* the redraw A/B — see REDRAW_LIT_MS     */
    ACT_TONE,                       /* uses freq/ms */
    ACT_BEEP, ACT_BLIP, ACT_SUCCESS, ACT_FAIL, ACT_CHORD,
    ACT_MUSIC,                      /* uses `path` — the looping bed          */
    ACT_MUSIC_STOP,
    ACT_SFX                         /* uses `path` — one recorded effect      */
} Action;

/* ── the sample material, and why these files ────────────────────────────────
 *
 * All five measured on `.188` 2026-08-20: **mono / 44100 / 16-bit**, which is
 * what `hw:0,0` grants, so nothing here is resampled or downmixed and a rate
 * refusal in the log means something changed rather than something is unsupported.
 *
 * ⚠️ **The two music files are hand-copied and are not in the repo** — they
 * survive a re-commission of the same card (nothing deletes `/opt/sound`) but not
 * a fresh card. If a pad refuses with "cannot open",
 * that is the first thing to check, not a code fault.
 *
 * ⚠️ **`asl_success.wav` is the pad that answers the actual question.**
 * Everything else on this panel is a synthesised sine, and the surviving
 * hypothesis for the two-voice harshness is this speaker on sustained pure sine
 * PAIRS (four other causes are refuted by measurement). Sampled
 * material over sampled material shares the bus, the limiter and the delivery
 * with the sine case and differs only in the waveform, so if SFX-over-bed is
 * clean the answer is "effects should be samples" and the question closes.
 */
#define MUSIC1_PATH  "/opt/sound/officerunner1-mono.wav"
#define MUSIC2_PATH  "/opt/sound/officerunner2-mono.wav"
#define SFX_PATH     "/opt/sound/asl_success.wav"

/* ── the level ladder ─────────────────────────────────────────────────────────
 *
 * ⚠️ **Quietest FIRST, and the tool starts on the quietest rung rather than on the
 * shipped default.**  Every level ladder this project has walked so far ran
 * loud-to-quiet, and that direction biases adaptation: after a distorted rung a
 * clean one sounds *quiet*, and after a clean one a distorted rung sounds *loud*.
 * Starting at the bottom makes the honest direction the only one the pad offers.
 *
 * ⚠️ **Two SEPARATE questions per rung — "can you hear it?" and "is it clean?"**
 * Conflating them has already cost a session: *"all work"* was read as *"all
 * clean"* and a whole discriminator was built on it (`tests/CLAUDE.md`).
 *
 * The volume is the only thing that moves.  The master shift stays at ScummVM's
 * `>>1` — it is the DEVICE stage, one per speaker, and moving both at once would
 * make the walk a two-variable comparison.  The acoustic peak in the label is
 * `peak >> shift`, i.e. what actually reaches the amplifier.
 */
typedef struct { int vol; const char *note; } LevelRung;
static const LevelRung ladder[] = {
    {  24, "quietest"  },
    {  48, ""          },
    {  96, "default"   },   /* AUDIO_VOICE_VOL — one voice at the measured-clean 6144 */
    { 144, ""          },
    { 192, "ScummVM"   },   /* MixerImpl's own default arithmetic, then >>1 */
    { 256, "full"      },
};
#define LADDER_RUNGS ((int)(sizeof(ladder) / sizeof(ladder[0])))

typedef struct {
    Button      btn;
    Action      act;
    int         freq;
    int         ms;
    const char *path;               /* ACT_MUSIC / ACT_SFX only */
} Pad;

/** The pad table's size, and it is the EXACT count in use: 4 controls + 5 tones +
 *  5 canned + 6 ms + 5 sample = 25.
 *
 * ⚠️ **This was 26 with 22 used, and adding a five-pad row silently produced a
 * row that did not exist** — `pad_add()` returned NULL past the cap and said
 * nothing, so the buttons were simply absent and nothing in the code looked
 * wrong. It is exact rather than padded on purpose: a spare slot restores the
 * silence for the next row. If you add pads, raise this AND check the log line
 * below, which is the only thing that can tell you the cap was hit. */
#define MAX_PADS 25
static Pad  pads[MAX_PADS];
static int  pad_count = 0;

static Pad *pad_add(Action act, const char *label, int x, int y, int w, int h,
                    uint32_t colour, int scale)
{
    if (pad_count >= MAX_PADS) {
        /* ⚠️ Loud, because the silent version is indistinguishable from a layout
         * bug: the pad is not drawn, not hit-tested and not in `pad_count`. */
        fprintf(stderr, "audio_mix_test: MAX_PADS (%d) EXCEEDED — the pad \"%s\" "
                        "does not exist and cannot be tapped\n", MAX_PADS, label);
        return NULL;
    }
    Pad *p = &pads[pad_count++];
    memset(p, 0, sizeof(*p));
    p->act = act;
    button_init_full(&p->btn, x, y, w, h, label,
                     colour, COLOR_WHITE, BTN_HIGHLIGHT_COLOR, scale);
    return p;
}

/* Lay `count` pads out evenly across the touch-safe width.  Buttons are
 * hit-tested, so they belong in SCREEN_SAFE_*, never in the visible band. */
static void row_geom(int count, int index, int gap, int *x, int *w)
{
    int total = SCREEN_SAFE_WIDTH - 2 * gap;
    int cell  = total / count;
    *x = SCREEN_SAFE_LEFT + gap + index * cell;
    *w = cell - gap;
}

/* ── the vertical layout, DERIVED ────────────────────────────────────────────
 *
 * ⚠️ **The five row offsets are computed from `SCREEN_SAFE_HEIGHT`, not written
 * down, and that is what makes a fifth row safe on a unit nobody has swept.**
 * The agreed target — rows at 72/136/200/274/348 with heights 54/54/64/64/58 and
 * gaps of 10 — ends at 406 and needs a safe height of ~410. `.188` has it
 * (`reach 0 4082 0 4095`, i.e. its digitizer reaches the hardware limit
 * vertically, so ~418), but at `FB_TOUCH_INSET_MAX` on both edges
 * `SCREEN_SAFE_HEIGHT` is only **384** and the same offsets overflow the
 * touchable rectangle by 22 px — a bottom row that is drawn and cannot be
 * pressed, which is precisely the failure the two-rectangle split exists to
 * prevent (../CLAUDE.md → *Screen edges*).
 *
 * So the shortfall is paid for in a fixed order, and the order is a judgement
 * about tapping rather than about pixels:
 *
 *   1. **gaps first, down to GAP_MIN** — whitespace between rows costs nothing to
 *      lose, and a pad that shrank is measurably harder to hit than one that moved.
 *   2. **then the TALLEST row, one pixel at a time** — so the 64 px rows give
 *      before the 54 px ones and the row heights converge rather than one row
 *      collapsing.
 *   3. **never below ROW_H_MIN**, at which point it gives up and lets the bottom
 *      row sit outside the safe area. ⚠️ That is a real outcome, not a
 *      theoretical one, and it is REPORTED (`rows: …`) on the log's first line
 *      rather than silently rendered — a bottom row 6 px outside the touchable
 *      band looks completely normal on a screenshot.
 */
#define ROWS_TOP       72   /* below the voice meter at SCREEN_SAFE_TOP+56, h 8 */
#define ROWS_BOTTOM_M   4   /* keep the last row off the very last safe pixel   */
#define ROW_COUNT       5
#define ROW_GAP_NOM    10
#define ROW_GAP_MIN     4
#define ROW_H_MIN      44   /* ~60x40 is the comfortable minimum (../CLAUDE.md) */

static const int row_nom_h[ROW_COUNT] = { 54, 54, 64, 64, 58 };
static int row_y[ROW_COUNT];
static int row_h[ROW_COUNT];
static int row_gap;

static void rows_layout(void)
{
    int nom = 0;
    for (int i = 0; i < ROW_COUNT; i++) { row_h[i] = row_nom_h[i]; nom += row_nom_h[i]; }

    int avail = SCREEN_SAFE_HEIGHT - ROWS_TOP - ROWS_BOTTOM_M;

    row_gap = ROW_GAP_NOM;
    while (row_gap > ROW_GAP_MIN && nom + row_gap * (ROW_COUNT - 1) > avail) row_gap--;

    int over = nom + row_gap * (ROW_COUNT - 1) - avail;
    while (over > 0) {
        int tallest = 0;
        for (int i = 1; i < ROW_COUNT; i++) if (row_h[i] > row_h[tallest]) tallest = i;
        if (row_h[tallest] <= ROW_H_MIN) break;      /* nothing left to give */
        row_h[tallest]--;
        over--;
    }

    int y = SCREEN_SAFE_TOP + ROWS_TOP;
    for (int i = 0; i < ROW_COUNT; i++) { row_y[i] = y; y += row_h[i] + row_gap; }

    /* The receipt.  `over` is the pixels the safe area could NOT absorb, so a
     * non-zero value means the bottom row is drawable and not pressable. */
    fprintf(stderr, "mix: rows safe_h=%d avail=%d gap=%d h=%d/%d/%d/%d/%d "
                    "y=%d/%d/%d/%d/%d bottom=%d over=%d\n",
            SCREEN_SAFE_HEIGHT, avail, row_gap,
            row_h[0], row_h[1], row_h[2], row_h[3], row_h[4],
            row_y[0] - SCREEN_SAFE_TOP, row_y[1] - SCREEN_SAFE_TOP,
            row_y[2] - SCREEN_SAFE_TOP, row_y[3] - SCREEN_SAFE_TOP,
            row_y[4] - SCREEN_SAFE_TOP,
            row_y[4] + row_h[4] - SCREEN_SAFE_TOP, over);
    if (over > 0)
        fprintf(stderr, "mix: ⚠ row 5 overflows the touch-safe area by %d px — "
                        "it is drawn but may not be pressable\n", over);
}

/* ── state ───────────────────────────────────────────────────────────────── */

typedef struct {
    /* ⚠️ **There is no `hard` field here any more, and that is the fix.** The
     * limiter position was a tool-local bool printed as if it were a
     * measurement: `audio_mix_init()` sets AUDIO_MIX_HARD while a zeroed bool
     * reads SOFT, so both the pad and `/tmp/mix.log` said "SOFT" over a HARD bus
     * for a whole panel session (measured 2026-08-19). Everything now reads
     * `audio_mix_get_limit()`. Do not reintroduce a mirror of a library state
     * that has a getter. */
    int  rung;              /* index into `ladder` — the LEVEL under test.  Starts
                             * at 0, the quietest, so the walk runs upwards       */
    int  voices;
    uint32_t clipped;
    uint32_t limited;
    uint32_t starved;
    uint32_t lost;
    uint32_t dropped;
    bool music_on;          /* the bed still owes frames — asked of the library  */
    long music_loops;       /* times it has WRAPPED, which is the number the loop
                             * seam is judged against: "I heard the join" is only
                             * actionable beside a wrap count                    */
    long lead_frames;       /* what the LIBRARY targeted, 0 = not measured yet */
    long period_frames;     /* the device period it was rounded up to           */
    uint32_t max_gap;       /* longest gap between two loop iterations, ms —
                             * restarted by a STOP tap (see the header)          */
    bool quiet_redraw;      /* DRAW PAD: a press repaints only its own pad       */
    char last[40];
} View;

/* ── the redraw A/B ──────────────────────────────────────────────────────────
 *
 * A crack is heard on every press at 5-8 voices while clip=0 starve=0, and a
 * trace of the mix found no sample discontinuity on a press.  What a press DOES
 * change is the loop: it forces draw_screen() + fb_swap() — a full clear, every
 * label and a whole-surface copy into the mapped framebuffer — in the same
 * iteration as the audio_pump() that starts the new voice.
 *
 * `DRAW PAD` removes exactly that and nothing else.  A press, and the voice
 * start it causes, repaint ONLY the pressed pad (inverted for REDRAW_LIT_MS, then
 * normal), copied into the front buffer as one small rectangle by
 * present_rect() — no clear, no fb_swap().  Everything audio-side is untouched:
 * the same pump call in the same place, the same sleep decision.
 *
 * ⚠️ **The readout must not smuggle the full redraw back in.**  The READOUT_MS
 * timer used to call draw_screen() + fb_swap() in BOTH modes whenever the voice
 * count moved — so every voice start still produced a full redraw within 250 ms
 * in DRAW PAD, and an A/B that heard the crack "in both modes" never tested a
 * loop without full redraws.  In DRAW PAD the timer now repaints only the
 * readout band (title, two counter lines, the `last` line, the voice meter —
 * present_readout()) by the same present_rect() path.  DRAW FULL is unchanged:
 * a full redraw on the press AND on the count change.  In DRAW PAD the only full
 * redraws left are the first frame and a tap on the DRAW pad itself, so its own
 * label is always the truth.  Every tap logs `redraw=full|pad`, and the timer
 * logs how many of each kind happened and how long the worst took (the `mix: t=`
 * line) — a crack that coincides with `full_draws` not moving is not this. */
#define REDRAW_LIT_MS  150

/* Microsecond monotonic clock for the draw/pump timings.  get_time_ms() is
 * whole milliseconds off gettimeofday(), too coarse for a present_rect() and
 * steppable by the time sync.  uint32_t wraps every ~71 min; only differences
 * are ever taken, and unsigned subtraction survives the wrap (no `long` math —
 * it is 32 bits here). */
static uint32_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

/* Per-READOUT_MS-period draw statistics: counts are session totals (so a log
 * line shows whether any draw happened since the last one), maxima restart at
 * every tick. */
static struct {
    uint32_t full_draws, pad_draws;       /* session totals                       */
    uint32_t full_us_max, pad_us_max;     /* worst this period                    */
} dstat;

static void note_pad_us(uint32_t t0)
{
    uint32_t d = mono_us() - t0;
    dstat.pad_draws++;
    if (d > dstat.pad_us_max) dstat.pad_us_max = d;
}

/* Copy one logical rectangle of the back buffer onto the panel.  Landscape only:
 * in portrait fb_swap()'s rotation would be needed and there is no feedback. */
static void present_rect(Framebuffer *fb, int x, int y, int w, int h)
{
    if (fb->portrait_mode || !fb->double_buffering || fb->back_buffer == NULL) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width)  w = (int)fb->width  - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0) return;
    const size_t bpp = fb->bytes_per_pixel;
    const uint8_t *src = (const uint8_t *)fb->back_buffer;
    uint8_t *dst = (uint8_t *)fb->buffer;
    for (int r = 0; r < h; r++)
        memcpy(dst + (size_t)(y + r + fb->view_y) * fb->line_length
                   + (size_t)(x + fb->view_x) * bpp,
               src + ((size_t)(y + r) * fb->width + (size_t)x) * bpp,
               (size_t)w * bpp);
}

static void repaint_pad(Framebuffer *fb, Pad *p)
{
    uint32_t t0 = mono_us();
    button_draw(fb, &p->btn);
    present_rect(fb, p->btn.x, p->btn.y, p->btn.width, p->btn.height);
    note_pad_us(t0);
}

static void set_draw_label(Pad *draw_pad, const View *v)
{
    button_set_text(&draw_pad->btn, v->quiet_redraw ? "DRAW PAD" : "DRAW FULL");
    button_set_colors(&draw_pad->btn,
                      v->quiet_redraw ? RGB(150, 120, 30) : RGB(70, 80, 100),
                      COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
}

/** How often the counter line may force a full redraw.  ⚠️ Not cosmetic: with
 *  the soft limiter engaged `limited` increments on most samples, so a readout
 *  that redraws on every change redraws EVERY FRAME — and a full 800x450x4
 *  repaint plus fb_swap on this part is a plausible cause of the very starvation
 *  this tool is trying to attribute.  The voice count still redraws instantly
 *  under DRAW FULL; under DRAW PAD it rides on this timer and is repainted in
 *  place (see REDRAW_LIT_MS). */
#define READOUT_MS  250

static void set_toggle_labels(Pad *limit_pad, Pad *level_pad, const Audio *audio,
                              const View *v)
{
    char t[32];

    /* HARD is drawn as a WARNING, because it is the state the panel rejected.
     * The log line carries `limit=soft|hard` as its own field.
     *
     * ⚠️ **Read back from the LIBRARY, never from a tool flag.** This label said
     * SOFT over a HARD bus for a whole session, because `audio_mix_init()` sets
     * HARD and the tool's own bool started false. `audio_mix_get_limit()` exists
     * for exactly this; an A/B tool that prints its own intention is not an
     * instrument. */
    bool hard = (audio_mix_get_limit(&audio->mix) == AUDIO_MIX_HARD);
    snprintf(t, sizeof(t), "LIM: %s", hard ? "HARD" : "SOFT");
    button_set_text(&limit_pad->btn, t);
    button_set_colors(&limit_pad->btn,
                      hard ? BTN_COLOR_DANGER : BTN_COLOR_PRIMARY,
                      COLOR_WHITE, BTN_HIGHLIGHT_COLOR);

    /* ⚠️ The rung number comes from the tool, but the VOLUME comes from the
     * library — `audio_get_volume()`, not `ladder[rung].vol`.  A pad that printed
     * its own intention would have shown a level the library had clamped or
     * refused, and this file has already paid for a label that disagreed with the
     * device (a toggle position that was recalled rather than recorded). */
    snprintf(t, sizeof(t), "LVL %d/%d", v->rung + 1, LADDER_RUNGS);
    button_set_text(&level_pad->btn, t);
    button_set_colors(&level_pad->btn,
                      (audio_get_volume(audio) >= AUDIO_VOICE_VOL) ? BTN_COLOR_DANGER
                                                                   : BTN_COLOR_INFO,
                      COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
}

#define MIX_BG  RGB(10, 12, 20)

/* The readout band: everything above the pad rows that changes during play.
 * Its bottom is the voice meter's (SCREEN_SAFE_TOP+56, h 8) plus a margin that
 * stays short of ROWS_TOP, so an in-place repaint can never touch a pad. */
#define READOUT_BAND_BOTTOM  (SCREEN_SAFE_TOP + 66)

/* Draw the readout band's contents into the back buffer — no clear, no present.
 * draw_screen() and present_readout() both call it, so the two paths cannot
 * show different text. */
static void draw_readout(Framebuffer *fb, const View *v, int rate)
{
    /* Title and the two readout lines live in the visible band above the pads:
     * they are read, never pressed.  INFO_X clears "MIX BUS" at scale 3 —
     * 7 chars x 6 px x 3 plus the left margin — measured rather than guessed,
     * because the first version overlapped and printed "4100 Hz". */
    fb_draw_text(fb, SCREEN_SAFE_LEFT + 4, SCREEN_VISIBLE_TOP + 4,
                 "MIX BUS", RGB(255, 200, 80), 3);
    int info_x = SCREEN_SAFE_LEFT + 8 + text_measure_width("MIX BUS", 3) + 12;

    /* ⚠️ The lead comes off the LIBRARY and carries its arithmetic with it, and
     * "not measured yet" is printed as such rather than as the header's request —
     * the two are different claims and only one of them is a measurement. */
    char lead[40];
    long lead_ms = audio_ms_for_frames(rate, v->lead_frames);
    if (v->lead_frames > 0)
        snprintf(lead, sizeof(lead), "lead %ld ms (%dx%ld)", lead_ms,
                 AUDIO_PUMP_LEAD_PERIODS,
                 audio_ms_for_frames(rate, v->period_frames));
    else
        snprintf(lead, sizeof(lead), "lead ? (pump has not measured)");

    char line[144];
    snprintf(line, sizeof(line), "%d Hz  %s  %d voices  worst frame %lu ms",
             rate, lead, AUDIO_MAX_VOICES, (unsigned long)v->max_gap);
    fb_draw_text(fb, info_x, SCREEN_VISIBLE_TOP + 6,
                 line, (lead_ms > 0 && v->max_gap > (uint32_t)lead_ms)
                       ? RGB(255, 180, 60) : RGB(130, 140, 160), 1);

    /* ⚠️ `bed` carries the WRAP COUNT, not just on/off.  The loop is measured
     * SEAMLESS (../common/audio_wav.h), and "I heard the join" is only
     * actionable next to which wrap it was — the bed loops every ~44 s, so a
     * session produces several and they are otherwise indistinguishable. */
    snprintf(line, sizeof(line),
             "voices %d/%d  clip %lu  lim %lu  starve %lu  lost %lu  drop %lu  bed %s %ld",
             v->voices, AUDIO_MAX_VOICES,
             (unsigned long)v->clipped, (unsigned long)v->limited,
             (unsigned long)v->starved, (unsigned long)v->lost,
             (unsigned long)v->dropped,
             v->music_on ? "ON wraps" : "off wraps", v->music_loops);
    fb_draw_text(fb, info_x, SCREEN_VISIBLE_TOP + 20,
                 line, (v->clipped || v->starved || v->lost)
                       ? RGB(255, 180, 60) : RGB(130, 200, 140), 1);

    if (v->last[0])
        fb_draw_text(fb, info_x, SCREEN_VISIBLE_TOP + 34,
                     v->last, RGB(160, 160, 200), 1);

    /* Voice meter: one cell per slot, lit for as many as are sounding. */
    int meter_y = SCREEN_SAFE_TOP + 56;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        int cw = 22, cx = SCREEN_SAFE_LEFT + 4 + i * (cw + 4);
        uint32_t c = (i < v->voices) ? RGB(80, 230, 120) : RGB(35, 40, 50);
        fb_fill_rect(fb, cx, meter_y, cw, 8, c);
    }
}

static void draw_screen(Framebuffer *fb, Button *exit_btn, const View *v,
                        int rate)
{
    fb_clear(fb, MIX_BG);
    draw_readout(fb, v, rate);
    for (int i = 0; i < pad_count; i++) button_draw(fb, &pads[i].btn);
    draw_exit_button(fb, exit_btn);
}

/* DRAW PAD's readout refresh: clear and redraw only the readout band in the back
 * buffer and copy that band to the panel — no fb_clear(), no fb_swap().  The EXIT
 * button sits top-right inside the band, so it is redrawn whole after the fill
 * (the part of it below the band is untouched in both buffers). */
static void present_readout(Framebuffer *fb, Button *exit_btn, const View *v,
                            int rate)
{
    uint32_t t0 = mono_us();
    int h = READOUT_BAND_BOTTOM - SCREEN_VISIBLE_TOP;
    fb_fill_rect(fb, SCREEN_VISIBLE_LEFT, SCREEN_VISIBLE_TOP,
                 SCREEN_VISIBLE_WIDTH, h, MIX_BG);
    draw_readout(fb, v, rate);
    draw_exit_button(fb, exit_btn);
    present_rect(fb, SCREEN_VISIBLE_LEFT, SCREEN_VISIBLE_TOP,
                 SCREEN_VISIBLE_WIDTH, h);
    note_pad_us(t0);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *fb_dev    = (argc > 1) ? argv[1] : "/dev/fb0";
    const char *touch_dev = (argc > 2) ? argv[2] : "/dev/input/touchscreen0";

    int lock_fd = acquire_instance_lock("audio_mix_test");
    if (lock_fd < 0) return 1;

    open_log();

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    hw_init();
    hw_set_backlight(100);

    Audio audio;
    audio_init(&audio);                 /* non-fatal: the UI still works */

    /* fb before touch — touch_init() reads the dims fb_init() publishes. */
    fb_set_bpp(fb_dev, 32);
    Framebuffer fb;
    if (fb_init(&fb, fb_dev) < 0) {
        fprintf(stderr, "audio_mix_test: cannot open %s\n", fb_dev);
        audio_close(&audio); return 1;
    }

    /* The row layout and present_rect() are landscape-only (present_rect()
     * returns early in portrait), so refuse with a message instead of showing
     * a screen that never updates.  fb_swap() rotates the whole frame in
     * portrait, so this one full-frame swap is visible there.  At scale 2 the
     * longer line is 30 chars * 12 px = 360 px, inside the 480 px portrait width. */
    if (fb.portrait_mode) {
        fb_clear(&fb, COLOR_BLACK);
        text_draw_centered(&fb, (int)fb.width / 2, (int)fb.height / 2 - 20,
                           "MIX BUS TEST NEEDS LANDSCAPE", COLOR_YELLOW, 2);
        text_draw_centered(&fb, (int)fb.width / 2, (int)fb.height / 2 + 20,
                           "TURN PORTRAIT OFF AND RELAUNCH", COLOR_YELLOW, 2);
        fb_swap(&fb);
        sleep(3);
        fb_clear(&fb, COLOR_BLACK);
        fb_swap(&fb);
        fb_close(&fb);
        audio_close(&audio);
        return 0;
    }

    TouchInput touch;
    if (touch_init(&touch, touch_dev) < 0) {
        fprintf(stderr, "audio_mix_test: cannot open %s\n", touch_dev);
        fb_close(&fb); audio_close(&audio); return 1;
    }
    touch_set_screen_size(&touch, screen_base_width, screen_base_height);

    Button exit_btn;
    button_init_full(&exit_btn, LAYOUT_EXIT_BTN_X, SCREEN_SAFE_TOP + 4,
                     BTN_EXIT_WIDTH, BTN_EXIT_HEIGHT, "",
                     BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR, 2);

    /* ── rows.  Everything is laid out from SCREEN_SAFE_*, so it is right on a
     * calibrated panel and unchanged on one whose reach has never been swept.
     * ⚠️ **The five vertical offsets are DERIVED by rows_layout(), not written
     * here** — see its comment for what the shortfall on a heavily-inset panel is
     * paid for with, and why a fifth row is where that starts to matter. */
    int x, w, y;
    int gap = 6;
    rows_layout();

    y = row_y[0];
    /* Four controls, laid out across the whole row by row_geom() like every
     * other row — the continuous stream is the library's only mode, so there is
     * no device-half or pump toggle left to put beside them.  The fourth is the
     * redraw A/B (see REDRAW_LIT_MS), which changes the loop, not the audio. */
    row_geom(4, 0, gap, &x, &w);
    /* ⚠️ "LIM" with no value: set_toggle_labels() runs before the first draw and
     * reads the real position out of the library, so a literal here could only
     * ever be a lie waiting for a code path that skips that call. */
    Pad *limit_pad = pad_add(ACT_LIMIT,    "LIM",         x, y, w, row_h[0], BTN_COLOR_PRIMARY, 2);
    row_geom(4, 1, gap, &x, &w);
    Pad *level_pad = pad_add(ACT_LEVEL,    "LVL 1/6",     x, y, w, row_h[0], BTN_COLOR_INFO, 2);
    row_geom(4, 2, gap, &x, &w);
    pad_add(ACT_STOP, "STOP", x, y, w, row_h[0], BTN_COLOR_DANGER, 2);
    row_geom(4, 3, gap, &x, &w);
    Pad *draw_pad  = pad_add(ACT_DRAW,     "DRAW",        x, y, w, row_h[0], RGB(70, 80, 100), 2);

    y = row_y[1];
    /* ⚠️ Two SUSTAINED tones, because defect 3's decisive question is about
     * TIMBRE and a 200 ms blip cannot be compared with a signal generator.
     * `440 3s` is one voice at AUDIO_PEAK, which the soft limiter passes through
     * byte-identically (its knee IS AUDIO_PEAK) — so it separates "the limiter
     * distorts the sum" from "this device's 440 is not a sine at all".
     * ⚠️ **This row was 80 px tall and is now 54**, which is where most of row 5's
     * space came from: 54 is confirmed comfortable to tap and the other four rows
     * were never taller than 64. */
    static const struct { const char *l; int f, ms; } tones[5] = {
        { "DRONE 220", 220, 3000 }, { "440 3s", 440, 3000 },
        { "440",       440,  200 }, { "880",   880,  200 },
        { "1760",     1760,  200 }
    };
    for (int i = 0; i < 5; i++) {
        row_geom(5, i, gap, &x, &w);
        Pad *p = pad_add(ACT_TONE, tones[i].l, x, y, w, row_h[1],
                         (tones[i].ms >= 3000) ? RGB(120, 60, 160)
                                               : RGB(40, 90, 170), 2);
        if (p) { p->freq = tones[i].f; p->ms = tones[i].ms; }
    }

    y = row_y[2];
    static const struct { const char *l; Action a; } canned[5] = {
        { "BEEP", ACT_BEEP }, { "BLIP", ACT_BLIP }, { "SUCCESS", ACT_SUCCESS },
        { "FAIL", ACT_FAIL }, { "CHORD", ACT_CHORD }
    };
    for (int i = 0; i < 5; i++) {
        row_geom(5, i, gap, &x, &w);
        pad_add(canned[i].a, canned[i].l, x, y, w, row_h[2], RGB(45, 110, 90), 2);
    }

    y = row_y[3];
    static const int ms_row[6] = { 5, 10, 20, 40, 60, 100 };
    for (int i = 0; i < 6; i++) {
        char l[12]; snprintf(l, sizeof(l), "%dms", ms_row[i]);
        row_geom(6, i, gap, &x, &w);
        Pad *p = pad_add(ACT_TONE, l, x, y, w, row_h[3], RGB(150, 100, 30), 2);
        if (p) { p->freq = 880; p->ms = ms_row[i]; }
    }

    /* ── row 5: recorded PCM, and the one inharmonic tone ────────────────────
     *
     * ⚠️ **`INH 622` exists because every other tone pad is an OCTAVE of every
     * other one** — 220 / 440 / 880 / 1760 — so this tool could not make an
     * inharmonic pair at all, and the `CHORD` pad (523/659/784) was the only
     * substitute. 622 against 440 is 1.4136, within 0.03 % of √2: the tritone,
     * the interval whose harmonics coincide least (440x7 = 3080 against
     * 622x5 = 3110). Against DRONE 220 it is a tritone plus an octave. Sustained
     * at 3 s for the same reason the other two long pads are
     * (harmonic fusion is already refuted; this pad
     * is what lets that refutation be re-run without borrowing CHORD).
     *
     * ⚠️ **`W STOP` is not `STOP`.** The top row's STOP is `audio_interrupt()`
     * — every voice at once. This one releases only the bed, so an effect over it
     * keeps sounding: that difference is the whole point of a bed. */
    y = row_y[4];
    static const struct { const char *l; Action a; const char *path; int f, ms; uint32_t c; } srow[5] = {
        { "WAV 1",   ACT_MUSIC,      MUSIC1_PATH, 0,   0,    RGB(170, 90, 40)  },
        { "WAV 2",   ACT_MUSIC,      MUSIC2_PATH, 0,   0,    RGB(170, 90, 40)  },
        { "W STOP",  ACT_MUSIC_STOP, NULL,        0,   0,    RGB(120, 60, 60)  },
        { "SFX",     ACT_SFX,        SFX_PATH,    0,   0,    RGB(60, 140, 170) },
        { "INH 622", ACT_TONE,       NULL,        622, 3000, RGB(120, 60, 160) }
    };
    for (int i = 0; i < 5; i++) {
        row_geom(5, i, gap, &x, &w);
        Pad *p = pad_add(srow[i].a, srow[i].l, x, y, w, row_h[4], srow[i].c, 2);
        if (p) { p->path = srow[i].path; p->freq = srow[i].f; p->ms = srow[i].ms; }
    }

    /* ⚠️ The receipt for MAX_PADS.  pad_add() already complains per refused pad;
     * this says whether the table is exactly full, which is what the next person
     * adding a row needs to know. */
    fprintf(stderr, "mix: pads %d of %d\n", pad_count, MAX_PADS);

    View v; memset(&v, 0, sizeof(v));
    snprintf(v.last, sizeof(v.last), "one never-reset stream");
    /* ⚠️ Start on the QUIETEST rung, not on the shipped default: the walk has to
     * run quiet-to-loud, and a tool that begins in the middle cannot enforce it. */
    v.rung = 0;
    audio_set_volume(&audio, ladder[0].vol);
    set_toggle_labels(limit_pad, level_pad, &audio, &v);
    if (draw_pad) set_draw_label(draw_pad, &v);

    bool needs_redraw = true;
    Pad     *lit       = NULL;      /* DRAW PAD: the pad shown inverted, and until */
    uint32_t lit_until = 0;
    uint32_t last_readout = 0;
    uint32_t prev_now     = 0;
    /* DRAW PAD: something in the readout band is stale — `last` after a press,
     * the lead once measured — and waits for the READOUT_MS tick to be
     * repainted in place, so no readout paint lands in a press's iteration. */
    bool     readout_dirty = false;
    uint32_t shown_gap     = 0;         /* v.max_gap as last painted             */

    /* The timing receipt (the `mix: t=` line).  pump_gap is wall time between
     * the starts of two consecutive audio_pump() calls — the us-resolution
     * version of `worst frame`, restarted every tick rather than by STOP. */
    const uint32_t t_start   = get_time_ms();
    uint32_t last_pump_us    = 0;
    uint32_t pump_gap_max    = 0;       /* this period, us                       */
    uint32_t pump_gap_record = 0;       /* whole session, us                     */
    uint32_t draw_record     = 0;       /* whole session, us, either kind        */
    uint32_t logged_full = 0, logged_pad = 0;
    int      logged_voices = -1;

    while (running) {
        touch_poll(&touch);
        TouchState ts = touch_get_state(&touch);
        uint32_t   now = get_time_ms();

        /* ⚠️ The stream holds only its LEAD — the measured one, ~139 ms here, not
         * the 80 ms AUDIO_PUMP_LEAD_MS asks for — so ANY iteration longer than that
         * starves the device however correct the mix is.  Measuring the worst one is
         * what tells a pacing fault from a mixing fault, and it is the number
         * `starve` cannot give, because starve counts the symptom. */
        if (prev_now != 0 && (now - prev_now) > v.max_gap) v.max_gap = now - prev_now;
        prev_now = now;

        if (button_check_tap(&exit_btn, &ts, now)) running = false;

        for (int i = 0; i < pad_count && running; i++) {
            if (!button_check_tap(&pads[i].btn, &ts, now)) continue;
            Pad *p = &pads[i];
            /* The A/B: in DRAW PAD a press does NOT redraw the screen — the pad
             * is repainted alone after the switch below.  The DRAW pad's own tap
             * always redraws in full, so the label it flips is on the panel. */
            if (!v.quiet_redraw || p->act == ACT_DRAW) needs_redraw = true;

            /* ⚠️ Every tap logs the LEVEL STATE with it, read from the LIBRARY and
             * never from a pad's label: the first panel report of this tool could
             * not be diagnosed because a toggle position was recalled rather than
             * recorded.  The pad's own freq/ms go in the line too, not just `act` —
             * the whole point of the ms row is WHICH stimulus was silent, so the
             * record has to be self-identifying the same way the stimuli are.
             * `svc_us` is the service ceiling, the one number that turns "I heard a
             * gap" into a pacing verdict — compare it with `gapmax` on the same
             * line. */
            fprintf(stderr, "mix: tap act=%d pad=%s freq=%d ms=%d path=%s "
                            "svc_us=%ld "
                            "limit=%s redraw=%s vol=%d shift=%d acoustic=%d voices=%d "
                            "clip=%lu lim=%lu starve=%lu lost=%lu drop=%lu "
                            "bed=%d wraps=%ld "
                            "gapmax=%lu lead=%ldfr/%ldms period=%ldfr\n",
                    (int)p->act, p->btn.text, p->freq, p->ms,
                    p->path ? p->path : "-",
                    audio_cont_service_interval_us(&audio),
                    (audio_mix_get_limit(&audio.mix) == AUDIO_MIX_HARD) ? "hard" : "soft",
                    v.quiet_redraw ? "pad" : "full",
                    audio_get_volume(&audio), audio_get_master_shift(&audio),
                    audio_voice_peak(audio_get_volume(&audio))
                        >> audio_get_master_shift(&audio),
                    audio_pump_voices(&audio),
                    (unsigned long)audio_pump_clipped(&audio),
                    (unsigned long)audio_pump_limited(&audio),
                    (unsigned long)audio_pump_starved(&audio),
                    (unsigned long)audio_pump_lost(&audio),
                    (unsigned long)audio_pump_dropped(&audio),
                    (int)audio_music_active(&audio), audio.music.wav.loops,
                    (unsigned long)v.max_gap,
                    audio_pump_lead(&audio),
                    audio_ms_for_frames(audio.sample_rate,
                                        audio_pump_lead(&audio)),
                    audio_pump_period(&audio));

            switch (p->act) {
            case ACT_LIMIT: {
                /* Switchable while a drone runs: the two curves agree below the
                 * knee, so the change is inaudible on a quiet passage and obvious
                 * on a loud one — which is the comparison worth hearing.
                 *
                 * ⚠️ **The current position is READ, then flipped.** It used to
                 * flip a tool-local bool, which started false while the library
                 * started HARD — so the first tap "turned SOFT on" by setting
                 * HARD, and everything downstream printed the opposite of the truth. */
                bool now_hard = (audio_mix_get_limit(&audio.mix) == AUDIO_MIX_HARD);
                audio_pump_set_limit(&audio, now_hard ? AUDIO_MIX_SOFT : AUDIO_MIX_HARD);
                set_toggle_labels(limit_pad, level_pad, &audio, &v);
                bool is_hard = (audio_mix_get_limit(&audio.mix) == AUDIO_MIX_HARD);
                snprintf(v.last, sizeof(v.last), "limit %s",
                         is_hard ? "HARD (clamp at int16)" : "soft (knee 18000)");
                break;
            }
            case ACT_LEVEL: {
                /* ⚠️ Wraps back to the QUIETEST rather than reversing, so a second
                 * pass runs in the same direction as the first — a ladder walked up
                 * and then down is two different listening tasks, and the second one
                 * is the biased one this pad exists to avoid. */
                v.rung = (v.rung + 1) % LADDER_RUNGS;
                audio_set_volume(&audio, ladder[v.rung].vol);
                set_toggle_labels(limit_pad, level_pad, &audio, &v);
                /* The acoustic peak — after the device shift — is the number the ear
                 * is judging, and it is read back from the library. */
                int vol   = audio_get_volume(&audio);
                int shift = audio_get_master_shift(&audio);
                snprintf(v.last, sizeof(v.last), "vol %d peak %d%s%s",
                         vol, audio_voice_peak(vol) >> shift,
                         ladder[v.rung].note[0] ? " " : "", ladder[v.rung].note);
                break;
            }
            case ACT_STOP:
                audio_interrupt(&audio);
                /* ⚠️ Also restarts `worst frame` (see the header) — and prev_now
                 * with it, or the gap ACROSS this tap becomes the new worst. */
                v.max_gap = 0; prev_now = 0;
                snprintf(v.last, sizeof(v.last), "stop: voices cut, worst frame reset");
                break;
            case ACT_DRAW:
                /* Touches no audio state — that is the whole point of the A/B. */
                v.quiet_redraw = !v.quiet_redraw;
                set_draw_label(p, &v);
                lit = NULL;
                snprintf(v.last, sizeof(v.last), "redraw %s",
                         v.quiet_redraw ? "PAD only on a press" : "FULL on a press");
                break;
            case ACT_TONE:
                audio_tone(&audio, p->freq, p->ms);
                snprintf(v.last, sizeof(v.last), "tone %d Hz %d ms", p->freq, p->ms);
                break;
            case ACT_BEEP:    audio_beep(&audio);
                snprintf(v.last, sizeof(v.last), "beep 880 Hz 80 ms"); break;
            case ACT_BLIP:    audio_blip(&audio);
                snprintf(v.last, sizeof(v.last), "blip 1320 Hz 60 ms"); break;
            case ACT_SUCCESS: audio_success(&audio);
                snprintf(v.last, sizeof(v.last), "success: 3 notes, offset"); break;
            case ACT_FAIL:    audio_fail(&audio);
                snprintf(v.last, sizeof(v.last), "fail: 3 notes, offset"); break;
            case ACT_CHORD:
                /* ⚠️ **This pad was an ARPEGGIO and was recorded as a chord, and a
                 * refutation rests on it.** Three back-to-back `audio_tone()` calls
                 * are consecutive statements, i.e. microseconds apart, so
                 * AUDIO_TONE_CHAIN_MS's recency gate saw a motif and CHAINED them:
                 * note 2 started behind note 1's tail and note 3 behind note 2's.
                 * A refutation says "HARD vs SOFT was inaudible, therefore clipping is
                 * refuted", and that A/B was judged with this pad — which never put
                 * two voices on the bus at once and so could not distinguish two
                 * limiters. `audio_mix_add()` with delay 0 is what a chord is. */
                {
                    int peak = audio_voice_peak(audio_get_volume(&audio));
                    audio_mix_add(&audio.mix, 523, 400, 0, peak);
                    audio_mix_add(&audio.mix, 659, 400, 0, peak);
                    audio_mix_add(&audio.mix, 784, 400, 0, peak);
                    snprintf(v.last, sizeof(v.last), "chord: 3 voices, delay 0");
                }
                break;
            case ACT_MUSIC:
                /* ⚠️ Every refusal is the LIBRARY's and is already on stderr with its
                 * reason; what belongs here is a short version on the panel, because
                 * an operator holding a checklist cannot read /tmp/mix.log. */
                if (audio_music_start(&audio, p->path, true))
                    snprintf(v.last, sizeof(v.last), "bed: %s looping",
                             strrchr(p->path, '/') ? strrchr(p->path, '/') + 1 : p->path);
                else
                    snprintf(v.last, sizeof(v.last), "bed REFUSED - see /tmp/mix.log");
                break;
            case ACT_MUSIC_STOP:
                /* Releases the bed only — an effect over it keeps sounding, which is
                 * the difference between this and the top row's STOP. */
                audio_music_stop(&audio);
                snprintf(v.last, sizeof(v.last), "bed: release armed (fades out)");
                break;
            case ACT_SFX:
                if (audio_sfx_play(&audio, p->path))
                    snprintf(v.last, sizeof(v.last), "sfx: %s over the bus",
                             strrchr(p->path, '/') ? strrchr(p->path, '/') + 1 : p->path);
                else
                    snprintf(v.last, sizeof(v.last), "sfx REFUSED - see /tmp/mix.log");
                break;
            }

            /* DRAW PAD's only visible feedback: this pad, inverted, alone.  A
             * previously lit pad is restored first, so at most two small
             * rectangles move on a press and the screen is never cleared. */
            if (v.quiet_redraw && p->act != ACT_DRAW) {
                if (lit && lit != p) repaint_pad(&fb, lit);
                p->btn.visual_state = BTN_STATE_PRESSED;
                repaint_pad(&fb, p);
                lit = p;
                lit_until = now + REDRAW_LIT_MS;
                readout_dirty = true;   /* `last` changed; shown on the next tick */
            }
        }

        /* Restore the lit pad once its moment is over.  button_check_tap() has
         * already put its state back to NORMAL, so this repaints it plain. */
        if (lit && (int32_t)(now - lit_until) >= 0) {
            if (lit->btn.visual_state == BTN_STATE_PRESSED)
                lit->btn.visual_state = BTN_STATE_NORMAL;
            repaint_pad(&fb, lit);
            lit = NULL;
        }

        /* The pump, once per frame, exactly where a game would put it. */
        uint32_t pump_t = mono_us();
        if (last_pump_us != 0 && pump_t - last_pump_us > pump_gap_max)
            pump_gap_max = pump_t - last_pump_us;
        last_pump_us = pump_t;
        audio_pump(&audio);

        int      nv = audio_pump_voices(&audio);
        /* A voice starting or ending: draw now — except in DRAW PAD, where a
         * voice start IS the press, so the count waits for the READOUT_MS timer. */
        if (nv != v.voices && !v.quiet_redraw) {
            v.voices = nv;
            needs_redraw = true;
        }

        /* The effective lead only exists once a pump has read the device period,
         * so it appears mid-session rather than at startup — redraw when it does
         * (in DRAW PAD: in place, on the next tick). */
        long nlead = audio_pump_lead(&audio);
        if (nlead != v.lead_frames) {
            v.lead_frames   = nlead;
            v.period_frames = audio_pump_period(&audio);
            if (v.quiet_redraw) readout_dirty = true;
            else                needs_redraw  = true;
        }

        /* The counters move on almost every sample, so they are refreshed on a
         * timer rather than on change — see READOUT_MS.  In DRAW PAD every
         * readout change (count, counters, bed, `worst frame`, `last`) is
         * repainted in place; only DRAW FULL turns it into a full redraw. */
        bool tick = (uint32_t)(now - last_readout) >= READOUT_MS;
        bool paint_readout = false;
        if (tick) {
            uint32_t nc = audio_pump_clipped(&audio);
            uint32_t nl = audio_pump_limited(&audio);
            uint32_t ns = audio_pump_starved(&audio);
            uint32_t nf = audio_pump_lost(&audio);
            uint32_t nd = audio_pump_dropped(&audio);
            /* The bed's state rides on the same timer: `loops` moves once every
             * ~44 s and `music_on` only on a start or the end of a fade, so
             * neither needs a redraw of its own. */
            bool nm = audio_music_active(&audio);
            long nw = audio.music.wav.loops;
            if (nc != v.clipped || nl != v.limited || ns != v.starved ||
                nf != v.lost    || nd != v.dropped ||
                nm != v.music_on || nw != v.music_loops || nv != v.voices) {
                v.voices  = nv;
                v.clipped = nc; v.limited = nl; v.starved = ns;
                v.lost    = nf; v.dropped = nd;
                v.music_on = nm; v.music_loops = nw;
                if (v.quiet_redraw) readout_dirty = true;
                else                needs_redraw  = true;
            }
            if (v.quiet_redraw && v.max_gap != shown_gap) readout_dirty = true;
            paint_readout = v.quiet_redraw && readout_dirty && !needs_redraw;
            last_readout = now;
        }

        bool drew = needs_redraw || paint_readout;
        if (needs_redraw) {
            uint32_t t0 = mono_us();
            draw_screen(&fb, &exit_btn, &v, audio.sample_rate);
            fb_swap(&fb);
            uint32_t d = mono_us() - t0;
            dstat.full_draws++;
            if (d > dstat.full_us_max) dstat.full_us_max = d;
            needs_redraw  = false;
            readout_dirty = false;          /* the full frame carried it */
            shown_gap     = v.max_gap;
        } else if (paint_readout) {
            present_readout(&fb, &exit_btn, &v, audio.sample_rate);
            readout_dirty = false;
            shown_gap     = v.max_gap;
        }

        /* The timing receipt, at most one line per tick and only when it says
         * something: a draw of either kind since the last line, a new voice
         * count, or a pump gap / draw time worse than any before it this
         * session.  Times are measured in us and printed as ms to one decimal.
         * The *_max fields are this period's worst; the counts are totals.
         * ⚠️ Counters (clip/lim/starve/lost/drop) are on the tap line, not here —
         * `lim` moves on most samples and would make this a per-tick line. */
        if (tick) {
            uint32_t dmax = dstat.full_us_max > dstat.pad_us_max ? dstat.full_us_max
                                                                 : dstat.pad_us_max;
            bool new_gap  = pump_gap_max > pump_gap_record;
            bool new_draw = dmax > draw_record;
            if (dstat.full_draws != logged_full || dstat.pad_draws != logged_pad ||
                v.voices != logged_voices || new_gap || new_draw) {
                fprintf(stderr, "mix: t=%lu redraw=%s voices=%d full_draws=%lu "
                                "pad_draws=%lu full_ms_max=%lu.%lu pad_ms_max=%lu.%lu "
                                "pump_gap_ms_max=%lu.%lu%s%s\n",
                        (unsigned long)(now - t_start),
                        v.quiet_redraw ? "pad" : "full", v.voices,
                        (unsigned long)dstat.full_draws, (unsigned long)dstat.pad_draws,
                        (unsigned long)(dstat.full_us_max / 1000),
                        (unsigned long)(dstat.full_us_max % 1000 / 100),
                        (unsigned long)(dstat.pad_us_max / 1000),
                        (unsigned long)(dstat.pad_us_max % 1000 / 100),
                        (unsigned long)(pump_gap_max / 1000),
                        (unsigned long)(pump_gap_max % 1000 / 100),
                        new_gap ? " new_gap_max" : "", new_draw ? " new_draw_max" : "");
                logged_full   = dstat.full_draws;
                logged_pad    = dstat.pad_draws;
                logged_voices = v.voices;
            }
            if (new_gap)  pump_gap_record = pump_gap_max;
            if (new_draw) draw_record     = dmax;
            pump_gap_max = 0;
            dstat.full_us_max = dstat.pad_us_max = 0;
        }

        /* ⚠️ audio_pump_active() must be in this decision.  The stream keeps only
         * one lead inside the device, so a loop that idles at 100 ms starves it
         * and you hear a gap — and the gap would look like a mixing defect
         * rather than a pacing one. */
        usleep((drew || audio_pump_active(&audio)) ? FRAME_DELAY_ACTIVE_US
                                                   : FRAME_DELAY_IDLE_US);
    }

    hw_leds_off();
    hw_set_backlight(100);
    fb_fade_out(&fb);
    audio_close(&audio);
    touch_close(&touch);
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    fb_close(&fb);
    return 0;
}
