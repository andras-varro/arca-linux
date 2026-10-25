/*
 * audio_out_test.c — the one output stream, on the host, with no fd in it.
 *
 * `common/audio_out.c` is the device half of the audio library: it opens the
 * device once, reads the GRANT back, prefills silence, and then never resets and
 * never reconfigures until shutdown.  That is the click's fix — the click is a
 * stream transition, and every canned sound used to be a full stop and start.
 * This drives all of it through the injectable `AudioOutDev` vtable, so the whole
 * file is reachable on the host and no test here opens anything.
 *
 * GROUP A IS THE NEGATIVE CONTROL and it drives the OLD idiom, transcribed from
 * the shipped file (`audio.c` as of 2026-08-18, 710 lines):
 *
 *   A1  a ring reset before EVERY sound.  `audio_flush()` (audio.c:206-215) fired
 *       it and re-ran `configure_dsp()`, and `play_sequence()` (:679) called it
 *       before each canned sound — so a game with N sounds performs N stream
 *       transitions, which is what the operator hears as "every time there is a
 *       sound, there is a click".
 *   A2  the ~60 ms floor.  ⚠️ **MODELLED, not measured here**: the fake device
 *       swallows a start-up run of frames after each transition, which is the
 *       mechanism `audio.c:196-199` attributes the rule to.  A 30 ms tone through
 *       the old idiom is therefore inaudible and the same tone through the stream
 *       is not.  The frame count is a stand-in for ~50 ms of DAC pipeline start-up;
 *       nothing in the tree ever clamped 60 ms, it is prose only.
 *
 * Group B is the GRANT: every byte count in the library derives from what the
 * device granted, never from what was asked for, and a 0-channel grant is refused
 * rather than turned into a 0-byte write.  Group C is the lead, in whole device
 * periods.  Group D is the serviced mode, including the two counters that mean
 * one thing each.  Group E is the single fill callback and the loud refusal that
 * keeps two writers impossible.  Group F is the attenuation, swept over every
 * int16 at shift 0 and shift 1 — `-1 >> 1 == -1` is why it is a shift and not a
 * gain multiply, and it is what keeps ScummVM bit-identical to what was verified
 * on the panel.  Group G is the synchronous mode the two Settings tabs need,
 * which have no render loop at all.  Group H is the bounded drain.  Group I is
 * the published service ceiling, which is derived from REAL audio rather than the
 * nominal lead.  Group J is one stream per process.  Group K is which device.
 * Group L is what a failed write MEANS: the ALSA backend's error classification,
 * the device-lost flag an unplug sets, and the OSS-node → ALSA-PCM mapping — all
 * pure or vtable-driven, so a host with no libasound reaches every branch.
 * Group M is the gate in front of the replug probe: when a stream on the panel
 * speaker looks for a USB card that has come back.  Group N is the choice list a
 * settings page offers: which entries exist with the hardware that is attached,
 * what a saved choice SHOWS as, and the order a press cycles them in.
 *
 * ⚠️ **This file is NEW, so "seen failing against the pre-change source" cannot
 * mean compiling it against an older `audio_out.c` — there is none.**  The
 * equivalent evidence is `measure_audio_out_sabotage.sh`, which breaks one stated
 * rule at a time in a COPY and counts what notices.  Measured 2026-08-18:
 *
 *     sabotage                                      failed  caught by
 *      1  prefill dropped, the stream starts empty     5     A3b D6 D7 D8 D14
 *      2  scratch not zeroed before the fill           1     D4b
 *      3  service fills the free space, not the lead   8     D1 D5 D6 D7 D8 D11 …
 *      4  lead from the ms constant, not the period    8     C2 C3 C4 C5 D5 D11 …
 *      5  channels from the request, not the grant     8     B2 B3 B4 B5 B6 B7 …
 *                                                            then SIGFPE
 *      6  attenuation as a divide, not a shift         1     F2
 *      7  mode 2 not refused against a callback        1     E4
 *      8  drain waits for zero, not the slack          1     H4
 *      9  serviced policy given a non-zero wait        1     D13c
 *     10  CONTROL: audio_out_starved() returns 0       1     D10
 *
 * ⚠️ **Case 10 is the control and its count is the one to read first**: a single
 * lying accessor must fail exactly ONE check in ONE group.  A cascade there would
 * mean the suite is coupled and every other count above is inflated.
 *
 * ⚠️ **Three stanzas read 0 on the first run and only one of the three was telling
 * the truth, so reading the sweep is part of running it:**
 *
 *   - **case 5 was a LOST BUFFER, not a pass.**  `out=$(…)` makes the child's
 *     stdout a pipe, libc switches to full buffering, and the SIGFPE discarded
 *     eight `FAIL:` lines it had already printed.  The sweep now line-buffers the
 *     child and reports the signal — without both, a sabotage that CRASHES this
 *     suite is indistinguishable from one the suite cannot see.
 *   - **case 2 was a hole in this file.**  `D4` ran after three SILENT services,
 *     so the scratch was already zero and it passed with the `memset` deleted.
 *     Split into `D4a` (dirty the scratch first) and `D4b` (the real check).
 *   - **case 9 was a hole in this file.**  Nothing separated a wait from a SLEEP,
 *     so the load-bearing `wait_us == 0` was untested.  The fake now counts
 *     `waits_slept` only at `usec > 0`, and `D13b`/`D13c` drive the one path that
 *     can break it: a permanent mid-frame stall, where the policy's stop condition
 *     deliberately does not apply.
 *
 * Case 1 still fails four pacing checks in group D, but only as a side-effect of
 * the queue being empty; `A3b` is the check that states the property itself.
 *
 * What HAS been seen failing, on 2026-08-18, is three real defects — two in the
 * suite and one in the library, all found by running it:
 *
 *   - ⚠️ **the prefill policy was UNBOUNDED and hung `audio_out_open()` forever**
 *     against a device that would not take the prefill (`max_waits = 0` means
 *     unlimited, and `stop_on_again` is false because a prefill is worth waiting
 *     for).  The suite did not report it — it hung, which is a test result.  The
 *     fix derives the bound from the buffer's own length; `D12b` is the case.
 *   - the one-stream-per-process guard turned every later group's open into a
 *     refusal, because group D re-opened six times without closing.  A cascade of
 *     28 failures from one leak.
 *   - two expectations were wrong rather than the code: a transition count that
 *     included an open that never happened, and a drain asserted to reach zero
 *     when it deliberately stops at the over-report slack.
 *
 * Needs no sudo, no device and no network.
 *
 * Build (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/audio_out_test \
 *       tests/audio_out_test.c common/audio_out.c common/audio_gen.c -lm && \
 *   ./build/audio_out_test
 *
 * The zero that must stay zero is checked by grep rather than by a case here,
 * because it is an absence: the steady-state path performs no ring reset at all.
 *   grep -c 'SNDCTL_DSP_RE[S]ET' native_apps/common/audio_out.c   -> 0
 * The bracket is deliberate — an unbracketed pattern would match this line and
 * the gate would count its own documentation.
 */

#define _DEFAULT_SOURCE   /* mkdtemp, for group Q */
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "../common/audio_gen.h"
#include "../common/audio_out.h"

static int failures = 0;
static int checks   = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
    else       { printf("  ok:   %s\n", what); }
}

/* ── A device whose behaviour the test dictates ──────────────────────────────
 *
 * A simulated queue, plus the two things about the real one that no derived
 * number can stand in for: it can OVER-REPORT what it is holding (measured
 * 2650–2670 frames on `.188` 2026-08-18, ~1.3 of its 2048-frame period), and it
 * can swallow a run of frames after a transition, which is the mechanism the
 * ~60 ms minimum-tone rule is attributed to.
 */

#define FAKE_CAP  (1 << 19)

typedef struct {
    /* What it grants, regardless of what is asked for. */
    int  grant_rate, grant_bits, grant_channels;
    bool fail_open;

    int  fb;                /* frame bytes — the test sets this to match       */
    long period, ring;      /* frames                                          */
    long qbytes;            /* bytes it is holding                             */
    long over_report;       /* frames it ADDS to in_flight and hides from space */
    bool drain_all;         /* every space() empties the queue first            */
    long drain_per_space;   /* otherwise, frames drained per space() call       */
    bool fail_space;

    int  chunk;             /* max bytes taken per write (0 = whatever is asked) */
    long accept_upto;       /* -1 = unlimited; else total bytes it will ever take */
    bool hard_error;        /* stall as a real error rather than "full"          */
    int  hard_errno;        /* the errno a hard error leaves (0 = EIO)           */

    long startup_frames;    /* frames swallowed after a transition (MODELLED)    */
    long swallow_left;

    /* Observations. */
    int  opens, closes, transitions, spaces, writes, waits, waits_slept;
    long audible;
    uint8_t got[FAKE_CAP];
    long len;
} Fake;

static void fake_transition(Fake *f)
{
    f->transitions++;
    f->swallow_left = f->startup_frames;
    f->qbytes       = 0;
}

static int fake_open(void *ctx, int rate_req, int ch_req,
                     int *rate, int *bits, int *ch)
{
    Fake *f = (Fake *)ctx;
    if (f->fail_open) return -1;
    f->opens++;
    fake_transition(f);
    /* A grant of 0 means "the read-back failed"; the library must not turn that
     * into a 0-byte write.  Anything else is what the device chose, and it is
     * deliberately allowed to differ from the request. */
    *rate = f->grant_rate ? f->grant_rate : rate_req;
    *bits = f->grant_bits;
    *ch   = f->grant_channels;
    (void)ch_req;
    return 0;
}

static int fake_space(void *ctx, int frame_bytes, AudioOutSpace *sp)
{
    Fake *f = (Fake *)ctx;
    f->spaces++;
    if (f->fail_space || frame_bytes <= 0) {
        errno = f->hard_errno ? f->hard_errno : EIO;
        return -1;
    }

    if (f->drain_all) f->qbytes = 0;
    else {
        f->qbytes -= f->drain_per_space * f->fb;
        if (f->qbytes < 0) f->qbytes = 0;
    }

    long ring_bytes = f->ring * f->fb;
    long held       = f->qbytes / f->fb;

    sp->period_frames = f->period;
    sp->ring_frames   = f->ring;
    /* in_flight + space == ring always, which is the shape of the real ioctl:
     * over-reporting the queue is under-reporting the free space. */
    sp->in_flight     = held + f->over_report;
    sp->space         = (ring_bytes - f->qbytes) / f->fb - f->over_report;
    if (sp->space < 0) sp->space = 0;
    return 0;
}

static ssize_t fake_write(void *ctx, const void *buf, size_t n, bool *again)
{
    Fake *f = (Fake *)ctx;
    f->writes++;

    if (f->accept_upto >= 0 && f->len >= f->accept_upto) {
        if (f->hard_error) {
            *again = false;
            errno  = f->hard_errno ? f->hard_errno : EIO;
            return -1;
        }
        *again = true;
        return -1;
    }

    long room = f->ring * f->fb - f->qbytes;
    if (room <= 0) { *again = true; return -1; }

    long take = (long)n;
    if (take > room) take = room;
    if (f->chunk > 0 && take > f->chunk) take = f->chunk;
    if (f->accept_upto >= 0 && f->len + take > f->accept_upto)
        take = f->accept_upto - f->len;
    if (take <= 0) { *again = true; return -1; }
    if (f->len + take > FAKE_CAP) take = FAKE_CAP - f->len;
    if (take <= 0) { *again = true; return -1; }

    memcpy(f->got + f->len, buf, (size_t)take);
    f->len    += take;
    f->qbytes += take;

    /* The modelled DAC: whole frames only, and the run after a transition is
     * never heard. */
    long frames = take / f->fb;
    if (f->swallow_left >= frames) { f->swallow_left -= frames; }
    else { f->audible += frames - f->swallow_left; f->swallow_left = 0; }

    return (ssize_t)take;
}

static void fake_wait(void *ctx, int usec)
{
    Fake *f = (Fake *)ctx;
    f->waits++;
    /* ⚠️ Counting a wait and counting a SLEEP are different measurements, and only
     * the second one can test "service() never sleeps".  Every real backend's wait
     * (`alsa_wait()`, `dsp_wait`) is a no-op at `usec <= 0`, and the serviced policy
     * passes 0 — so the call still arrives here and must not be counted as sleep.
     * Without this split the non-zero-wait sabotage passed the whole suite. */
    if (usec > 0) f->waits_slept++;
    /* A wait is when the hardware makes progress, so the queue drains here too —
     * otherwise a bounded blocking policy could never finish and the drain loop
     * would always run to its bound. */
    if (!f->drain_all) {
        f->qbytes -= f->drain_per_space * f->fb;
        if (f->qbytes < 0) f->qbytes = 0;
    }
}

static void fake_close(void *ctx) { ((Fake *)ctx)->closes++; }

static const AudioOutDev FAKE_DEV = {
    fake_open, fake_space, fake_write, fake_wait, fake_close
};

/** 44100 / 2 channels / 2048-frame period / 16-period ring — the configuration
 *  `native_apps` is actually granted (measured on `.188`). */
static void fake_reset(Fake *f)
{
    memset(f, 0, sizeof(*f));
    f->grant_rate     = 44100;
    f->grant_bits     = 16;
    f->grant_channels = 2;
    f->fb             = 2 * AUDIO_BYTES_PER_SAMPLE;
    f->period         = 2048;
    f->ring           = 32768;
    f->accept_upto    = -1;
    f->drain_all      = true;
}

/* ── Fills the test installs ────────────────────────────────────────────── */

typedef struct {
    int16_t value;      /* every sample takes this value                      */
    long    produce;    /* frames to claim; -1 = all of them                  */
    int     calls;
    long    last_frames;
    int     last_channels;
    bool    poison;     /* write a sentinel into the frames it does NOT claim  */
    long    seq;        /* running counter for the shift sweep                */
} FillCtl;

static long fill_const(void *ctx, int16_t *buf, long frames, int channels)
{
    FillCtl *c = (FillCtl *)ctx;
    c->calls++;
    c->last_frames   = frames;
    c->last_channels = channels;

    long claim = (c->produce < 0 || c->produce > frames) ? frames : c->produce;
    for (long i = 0; i < claim * channels; i++) buf[i] = c->value;
    if (c->poison)
        for (long i = claim * channels; i < frames * channels; i++) buf[i] = 999;
    return claim;
}

/** Emits successive int16 values so one sweep can cover the whole range. */
static long fill_ramp(void *ctx, int16_t *buf, long frames, int channels)
{
    FillCtl *c = (FillCtl *)ctx;
    c->calls++;
    c->last_frames = frames;
    for (long i = 0; i < frames * channels; i++)
        buf[i] = (int16_t)(int32_t)(-32768 + ((c->seq++) & 0xFFFF));
    return frames;
}

/* ── Group V helpers: the system volume, through mode 2 ─────────────────────
 *
 * audio_out_write() is the path that needs no fill callback, so a test hands it
 * a mono buffer and reads the left channel back from the fake's byte log. */
static Fake vf;

/** Opens a stream, sets `step` (skipped when `set` is false), writes `n` mono
 *  samples (n <= 4096), returns the left channel in `outl`.  Returns frames
 *  written, or -1 when the open failed. */
static long vol_run(bool set, int step, const int16_t *in, long n, int16_t *outl)
{
    AudioOut o;
    fake_reset(&vf);
    if (audio_out_open(&o, &FAKE_DEV, &vf, 44100, 2) != 0) return -1;
    if (set) audio_out_set_volume_step(&o, step);
    long before = vf.len;
    long w = audio_out_write(&o, in, n);
    for (long i = 0; i < n; i++) {
        int16_t s;
        memcpy(&s, vf.got + before + i * vf.fb, sizeof(s));
        outl[i] = s;
    }
    audio_out_close(&o);
    return w;
}

/** Writes a one-line config fixture and points the open at it; returns the
 *  step the open then holds, or -2 if the open failed. */
static int vol_from_file(const char *text)
{
    char path[] = "/tmp/audio_out_vol_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -2;
    if (text) { FILE *fp = fdopen(fd, "w"); fputs(text, fp); fclose(fp); }
    else close(fd);
    audio_out_set_eq_path(path);
    AudioOut o;
    fake_reset(&vf);
    int r = -2;
    if (audio_out_open(&o, &FAKE_DEV, &vf, 44100, 2) == 0) {
        r = audio_out_get_volume_step(&o);
        audio_out_close(&o);
    }
    audio_out_set_eq_path(NULL);
    unlink(path);
    return r;
}

/** A fill that touches nothing at all — `audio_mix_render()` on a silent bus. */
static long fill_silent(void *ctx, int16_t *buf, long frames, int channels)
{
    FillCtl *c = (FillCtl *)ctx;
    (void)buf; (void)frames; (void)channels;
    c->calls++;
    return 0;
}

/* ── Buffer predicates ──────────────────────────────────────────────────── */

static bool all_zero(const uint8_t *b, long from, long to)
{
    for (long i = from; i < to; i++) if (b[i]) return false;
    return true;
}

static long count_nonzero_samples(const uint8_t *b, long bytes)
{
    const int16_t *s = (const int16_t *)b;
    long n = 0;
    for (long i = 0; i < bytes / 2; i++) if (s[i]) n++;
    return n;
}

/* ── Group A: the shipped idiom, transcribed ─────────────────────────────────
 *
 * Transcribed, not called: `audio_flush()` (audio.c:206-215) resets the ring and
 * re-runs `configure_dsp()`, and `play_sequence()` (:679) calls it before every
 * canned sound.  `audio_tone()` (:490-524) then writes the whole tone.
 */
static long old_play_tone(Fake *f, int rate, int freq, int ms, int16_t *mono,
                          int16_t *ilv)
{
    fake_transition(f);                       /* audio_flush(): reset + configure */
    long frames = audio_frames_for_ms(rate, ms);
    audio_render_tone(rate, freq, AUDIO_PEAK, mono, frames);
    audio_interleave(mono, frames, f->fb / AUDIO_BYTES_PER_SAMPLE, ilv);
    bool again = false;
    long bytes = frames * f->fb;
    long done  = 0;
    while (done < bytes) {
        ssize_t r = fake_write(f, (const uint8_t *)ilv + done, (size_t)(bytes - done),
                               &again);
        if (r <= 0) break;
        done += r;
    }
    return frames;
}

/* ── main ───────────────────────────────────────────────────────────────── */

#define RATE 44100

int main(void)
{
    static int16_t mono[1 << 17];
    static int16_t ilv[1 << 18];
    static Fake f;
    FillCtl fc;
    AudioOut out;

    printf("\n=== A. the OLD idiom, transcribed (the defects must reproduce) ===\n");
    {
        /* ~50 ms of DAC pipeline start-up at 44100, MODELLED as swallowed frames. */
        const long startup = audio_frames_for_ms(RATE, 50);

        fake_reset(&f);
        f.startup_frames = startup;
        f.drain_all      = true;
        old_play_tone(&f, RATE, 880, 80, mono, ilv);
        old_play_tone(&f, RATE, 660, 80, mono, ilv);
        old_play_tone(&f, RATE, 523, 80, mono, ilv);
        check(f.transitions == 3,
              "A1 old idiom: three sounds cost three stream transitions — one "
              "click each, which is the operator's complaint");

        fake_reset(&f);
        f.startup_frames = startup;
        long want = old_play_tone(&f, RATE, 880, 30, mono, ilv);
        check(want > 0 && f.audible == 0,
              "A2 old idiom: a 30 ms tone after a reset is ENTIRELY swallowed by "
              "the modelled DAC start-up — the ~60 ms floor, reproduced");

        /* The same device, driven by the library instead. */
        fake_reset(&f);
        f.startup_frames = startup;
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0,
              "A3 the stream opens on the same fake device");
        check(f.len == audio_out_lead(&out) * f.fb && f.len > 0 &&
              all_zero(f.got, 0, f.len),
              "A3b and the open PREFILLS that lead with silence — a stream started "
              "empty starts with the very transition this design removes.  Added "
              "because the sabotage sweep caught a dropped prefill only through its "
              "pacing side-effects, which is a coincidence rather than a check");
        long t_after_open = f.transitions;
        for (int i = 0; i < 20; i++) audio_out_service(&out);
        fc.value = 4000; fc.produce = -1; fc.calls = 0; fc.poison = false; fc.seq = 0;
        audio_out_set_fill(&out, fill_const, &fc, "test");
        for (int i = 0; i < 20; i++) audio_out_service(&out);
        audio_out_set_fill(&out, NULL, NULL, NULL);
        for (int i = 0; i < 20; i++) audio_out_service(&out);
        check(f.transitions == t_after_open,
              "A4 the stream: 60 services and two callback swaps cost ZERO further "
              "transitions — the device is configured exactly once, at open");
        check(f.opens == 1,
              "A5 and the device is opened exactly once, so nothing reconfigured it");
        audio_out_close(&out);
    }

    printf("\n=== B. the GRANT, never the request ===\n");
    {
        fake_reset(&f);
        f.grant_rate = 22050; f.grant_channels = 1; f.fb = 1 * AUDIO_BYTES_PER_SAMPLE;
        check(audio_out_open(&out, &FAKE_DEV, &f, 44100, 2) == 0,
              "B1 a device granting 22050 mono against a 44100 stereo request opens");
        check(audio_out_rate(&out) == 22050 && audio_out_channels(&out) == 1,
              "B2 the library reports the GRANT, not the request");
        fc.value = 1000; fc.produce = -1; fc.calls = 0; fc.poison = false;
        audio_out_set_fill(&out, fill_const, &fc, "test");
        f.len = 0;
        long got = audio_out_service(&out);
        check(fc.last_channels == 1,
              "B3 the fill callback is TOLD the granted channel count — it is an "
              "argument, never a literal");
        check(got > 0 && f.len == got * 1 * AUDIO_BYTES_PER_SAMPLE,
              "B4 and every byte count follows the grant: one sample per frame");
        audio_out_close(&out);

        fake_reset(&f);
        f.grant_channels = 0;             /* the read-back failed */
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) != 0,
              "B5 a 0-channel grant is REFUSED — a 0-channel byte count is 0, i.e. "
              "silently mute, which is the failure this library exists to remove");
        check(!audio_out_is_open(&out) && f.closes == 1,
              "B6 and the refused open closes the device rather than leaking it");

        fake_reset(&f);
        f.grant_bits = 8;
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0 &&
              audio_out_bits(&out) == 8,
              "B7 a non-16-bit grant is reported rather than swallowed — the hole "
              "audio.c:85 left and oss-mixer.cpp:147-152 already warned about");
        audio_out_close(&out);

        fake_reset(&f);
        f.fail_open = true;
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) != 0 &&
              !audio_out_is_open(&out),
              "B8 a failed open leaves a struct every entry point reads as closed");
    }

    printf("\n=== C. the lead, in whole DEVICE PERIODS ===\n");
    {
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_period(&out) == 2048,
              "C1 the period is read off the device, not assumed");
        check(audio_out_lead(&out) == 3 * 2048,
              "C2 the lead is AUDIO_PUMP_LEAD_PERIODS whole periods (6144 frames, "
              "~139 ms at 44100) — NOT the 80 ms the ms constant asks for");
        check(audio_out_lead(&out) % audio_out_period(&out) == 0,
              "C3 and it lands on a period boundary, which is what a lead between "
              "two periods XRUNed for");
        audio_out_close(&out);

        /* ScummVM's configuration: 22050 mono, same 2048-frame period. */
        fake_reset(&f);
        f.grant_rate = 22050; f.grant_channels = 1; f.fb = AUDIO_BYTES_PER_SAMPLE;
        audio_out_open(&out, &FAKE_DEV, &f, 22050, 1);
        check(audio_out_lead(&out) == 3 * 2048,
              "C4 the same rule at 22050 mono gives 6144 frames = ~278 ms, which is "
              "within 2 ms of what oss-mixer.cpp already prefills and holds — one "
              "shared constant reproduces both clients");
        audio_out_close(&out);

        /* A device with a tiny ring: the lead may not exceed half of it. */
        fake_reset(&f);
        f.period = 256; f.ring = 1024;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == 512,
              "C5 a small ring caps the lead at half of it, so the stream is not "
              "asked for more cushion than the device can hold");
        audio_out_close(&out);

        /* The per-process lead override (Ludfin's deeper cushion).  The default
         * (checked above) must be unchanged by a set(0). */
        audio_out_set_lead_ms(0);
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == 3 * 2048,
              "C6 set_lead_ms(0) leaves the default: still 3 periods = 6144 frames");
        audio_out_close(&out);

        /* 200 ms = 8820 frames, between 4 and 5 periods: rounds UP to 5 = 10240,
         * under the 16384 cap.  A stream opened AFTER the set takes it, and the
         * open prefills that lead (not the default's). */
        audio_out_set_lead_ms(200);
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == 5 * 2048,
              "C7 set_lead_ms(200) is rounded UP to whole periods: 10240 frames");
        check(f.len == audio_out_lead(&out) * f.fb,
              "C7b and the open prefilled that deeper lead with silence");
        audio_out_close(&out);

        /* 350 ms (Ludfin) wants 15435 frames -> 8 periods = 16384 = the cap. */
        audio_out_set_lead_ms(350);
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == 8 * 2048,
              "C8 set_lead_ms(350) on the 32768-frame ring grants 16384 frames (~372 ms)");
        audio_out_close(&out);

        /* Far beyond the ring: clamped at half of it, never more. */
        audio_out_set_lead_ms(5000);
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == f.ring / 2,
              "C9 an absurd request is clamped to half the ring");
        audio_out_close(&out);

        /* A small ring caps it even lower. */
        audio_out_set_lead_ms(350);
        fake_reset(&f);
        f.period = 256; f.ring = 1024;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == 512,
              "C10 the cap also holds on a small ring with the override set");
        audio_out_close(&out);

        /* Back to 0 restores the default for the next open. */
        audio_out_set_lead_ms(0);
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_lead(&out) == 3 * 2048,
              "C11 set_lead_ms(0) after an override restores the default");
        audio_out_close(&out);
    }

    printf("\n=== D. mode 1: serviced, and it targets a LEAD ===\n");
    {
        /* A silent bus still writes.  This is the whole fix: an idle stream is a
         * stream transition, and a transition is the click. */
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        fc.calls = 0;
        audio_out_set_fill(&out, fill_silent, &fc, "silent");
        f.len = 0;
        long n = audio_out_service(&out);
        check(n == audio_out_lead(&out),
              "D1 on a silent bus the service writes exactly the lead — never 0 "
              "bytes, because a stream allowed to go idle is a transition");
        check(f.len == n * f.fb && all_zero(f.got, 0, f.len),
              "D2 and what it wrote is silence, all of it");
        check(fc.calls == 1,
              "D3 the fill callback was still asked, once");

        /* ⚠️ Stale scratch must not leak, and the ORDER below is the entire test.
         * A loud fill claiming EVERY frame runs first, so the scratch is full of
         * non-zero samples; only then does a fill claim 100 of them and touch
         * nothing else, which is exactly `audio_mix_render()` on a silent bus.
         * Until 2026-08-18 this ran after three SILENT services, so the scratch
         * was already zero and the check passed with the `memset` deleted —
         * measured, by the sabotage sweep reporting 0 for that stanza. */
        fc.value = 7000; fc.produce = -1; fc.poison = false; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "loud");
        f.len = 0;
        n = audio_out_service(&out);
        check(n > 0 && count_nonzero_samples(f.got, f.len) == n * 2,
              "D4a a fill claiming every frame fills every frame — which is what "
              "leaves the scratch dirty for D4b to find");

        fc.value = 7000; fc.produce = 100; fc.poison = false; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "half");
        f.len = 0;
        n = audio_out_service(&out);
        check(n > 100 && count_nonzero_samples(f.got, f.len) == 100 * 2,
              "D4b a SHORT fill's remainder is silence — the library zeroes the "
              "buffer before every fill, because audio_mix_render() deliberately "
              "does not touch it on a silent bus");

        /* Never fills the free space. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = true;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        f.len = 0;
        n = audio_out_service(&out);
        check(n == 6144,
              "D5 an EMPTY 32768-frame ring gets the lead, not 743 ms of audio — "
              "filling the space is what puts the next sound three quarters of a "
              "second late");

        /* Already at the lead: nothing owed. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 0;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        long before = f.len;
        n = audio_out_service(&out);
        check(n == 0 && f.len == before,
              "D6 a queue already at the lead is written nothing at all, and 0 is "
              "a legitimate return rather than an error");

        /* Partial drain: exactly the shortfall. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 500;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        f.len = 0;
        n = audio_out_service(&out);
        check(n == 500,
              "D7 a queue 500 frames short of the lead is written exactly 500 "
              "frames — lead minus in_flight, not the free space");

        /* The over-report must not change what is written: it is a property of
         * the device's report, and the library deliberately does not subtract it
         * from in_flight. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 500; f.over_report = 2662;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        f.len = 0;
        n = audio_out_service(&out);
        check(n == 0,
              "D8 a device over-reporting its queue by 1.3 periods is BELIEVED for "
              "pacing — the slack is spent on the published service ceiling and the "
              "drain, never on writing deeper and adding onset latency");

        /* Starvation: a dry queue on a never-idle stream is always a fault. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = true;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_starved(&out) == 0,
              "D9 a fresh stream has starved 0 times");
        audio_out_service(&out);
        audio_out_service(&out);
        check(audio_out_starved(&out) == 2,
              "D10 a queue found DRY counts a starve every time — one audible gap "
              "each, and the number that separates pacing from mixing");

        /* Lost frames: the device refuses and the fill has already advanced.
         * ⚠️ The byte budget is set AFTER the open, because a device that refuses
         * the PREFILL is a different case — and it is the one that hung this suite
         * the first time it was run, before the prefill policy was bounded. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = true;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        f.accept_upto = f.len + 4 * f.fb;
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        long lost0 = audio_out_lost(&out);
        n = audio_out_service(&out);
        check(n == 4 && audio_out_lost(&out) - lost0 == (uint32_t)(6144 - 4),
              "D11 frames the device refused are COUNTED as lost — the fill has "
              "advanced past them, so they are gone rather than deferred");
        check(audio_out_misaligned(&out) == 0,
              "D12 and it stopped on a frame boundary: no partial frame in the "
              "device, so L and R are not swapped for the rest of the stream");

        /* And the prefill is bounded on the same device. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 0; f.accept_upto = 12;
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0,
              "D12b a device that will not take the PREFILL still returns from "
              "open — the one blocking call in the file is bounded, and an "
              "unbounded one hung this suite before it was");
        audio_out_close(&out);

        /* A chunking device that takes half a frame at a time still leaves whole
         * frames behind. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = true; f.chunk = 3;      /* not a multiple of 4-byte frames */
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        f.len = 0;
        n = audio_out_service(&out);
        check(f.len % f.fb == 0 && audio_out_misaligned(&out) == 0,
              "D13 a device taking 3 bytes at a time is left holding whole frames "
              "only — audio_write_frames() is still the only code that stops");

        /* ⚠️ A device that stalls MID-FRAME and never recovers is the one place the
         * "service() never sleeps" claim can be broken, and nothing tested it: the
         * sabotage sweep gave the non-zero-wait policy 0 failures.  6 bytes at a
         * 4-byte frame leaves half a frame in the device, and mid-frame the policy's
         * stop condition does NOT apply — `audio_write_frames()` waits
         * AUDIO_ALIGN_TRIES times whatever the caller asked for, which at a 1000 us
         * interval is 4 ms of sleep inside a render loop. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = true;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        f.accept_upto = f.len + 6;             /* 1.5 frames, then EAGAIN forever */
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        /* The prefill is the one call in the library allowed to sleep, so its waits
         * are not the measurement — zero the counters after the open, not before. */
        f.waits = 0; f.waits_slept = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        n = audio_out_service(&out);
        check(n == 1 && audio_out_misaligned(&out) == 1,
              "D13b a permanent MID-FRAME stall is REPORTED, not retried forever: "
              "one whole frame written, the half frame counted, and the loop bounded "
              "by AUDIO_ALIGN_TRIES — hanging the render loop is worse than the "
              "channel swap it is trying to avoid");
        check(f.waits > 0 && f.waits_slept == 0,
              "D13c and the realignment it just did SPUN rather than slept — the "
              "serviced policy's wait_us is 0, which is what makes \"service() never "
              "sleeps\" true rather than nearly true");

        /* The return value is the pacing signal. */
        audio_out_close(&out);
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 700;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        long a = audio_out_service(&out);
        f.drain_per_space = 1300;
        long b = audio_out_service(&out);
        check(a == 700 && b == 1300 && a != b,
              "D14 service() returns a VARIABLE frame count, which is why a caller "
              "advancing a fixed per-buffer deadline has two pacing models and "
              "neither bounds the queue");
        audio_out_close(&out);

        fake_reset(&f);
        f.fail_space = true;
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0,
              "D15 a device whose geometry cannot be read still opens");
        check(audio_out_service(&out) == -1 && audio_out_refused(&out) >= 1,
              "D16 but a service against it is refused and counted, not silently "
              "written into nowhere");
        audio_out_close(&out);
    }

    printf("\n=== E. one fill callback, and the refusal stays LOUD ===\n");
    {
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_fill_owner(&out) == NULL,
              "E1 a fresh stream has no fill owner");
        fc.value = 1000; fc.produce = -1; fc.calls = 0;
        audio_out_set_fill(&out, fill_const, &fc, "mixbus");
        check(audio_out_fill_owner(&out) != NULL &&
              strcmp(audio_out_fill_owner(&out), "mixbus") == 0,
              "E2 the owner is named, so the layer above can keep its refusal loud "
              "instead of letting a swap go quiet");

        FillCtl fc2; memset(&fc2, 0, sizeof(fc2));
        fc2.value = 2000; fc2.produce = -1;
        audio_out_set_fill(&out, fill_const, &fc2, "theremin");
        long t = f.transitions;
        f.len = 0;
        audio_out_service(&out);
        check(fc.calls == 0 && fc2.calls == 1 && f.transitions == t,
              "E3 a swap replaces the writer with no reset and no reconfigure — "
              "which is what lets the theremin and the mix bus share one stream");

        long r0 = audio_out_refused(&out);
        audio_render_tone(RATE, 880, AUDIO_PEAK, mono, 1000);
        check(audio_out_write(&out, mono, 1000) == -1 &&
              audio_out_refused(&out) == r0 + 1,
              "E4 a synchronous write against an installed callback is REFUSED and "
              "counted — the two-writer case the one callback exists to prevent");

        audio_out_set_fill(&out, NULL, NULL, NULL);
        check(audio_out_fill_owner(&out) == NULL,
              "E5 removing the callback clears the owner");
        f.len = 0;
        check(audio_out_write(&out, mono, 1000) == 1000,
              "E6 and with nobody servicing, the synchronous write goes through");
        audio_out_close(&out);
    }

    printf("\n=== F. attenuation is a SHIFT, swept over every int16 ===\n");
    {
        for (int shift = 0; shift <= 1; shift++) {
            fake_reset(&f);
            f.period = 64; f.ring = 4096; f.drain_all = true;
            audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
            audio_out_set_shift(&out, shift);
            memset(&fc, 0, sizeof(fc));
            audio_out_set_fill(&out, fill_ramp, &fc, "ramp");

            long mismatches = 0, seen = 0;
            while (seen < 65536) {
                f.len = 0;
                long got = audio_out_service(&out);
                if (got <= 0) break;
                const int16_t *s = (const int16_t *)f.got;
                for (long i = 0; i < got * 2; i++) {
                    int16_t src = (int16_t)(int32_t)(-32768 + ((seen + i) & 0xFFFF));
                    int16_t exp = (int16_t)(src >> shift);
                    if (s[i] != exp) mismatches++;
                }
                seen += got * 2;
            }
            if (shift == 0)
                check(seen >= 65536 && mismatches == 0,
                      "F1 shift 0 is the IDENTITY over all 65536 int16 values — "
                      "which is what keeps loudness unchanged in this change");
            else
                check(seen >= 65536 && mismatches == 0,
                      "F2 shift 1 matches oss-mixer.cpp:241-244's `>>1` for all "
                      "65536 int16 values, so ScummVM stays bit-identical");
            audio_out_close(&out);
        }

        /* The two values a rounding multiply gets wrong. */
        check((int16_t)(-1 >> 1) == -1 && (int16_t)(-32768 >> 1) == -16384,
              "F3 the two values that separate a shift from a gain multiply: "
              "-1 >> 1 == -1 (a multiply rounds it to 0) and -32768 >> 1 == -16384");

        /* Applied post-fill, so the fill never sees it. */
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        audio_out_set_shift(&out, 1);
        fc.value = 8000; fc.produce = -1; fc.calls = 0; fc.poison = false;
        audio_out_set_fill(&out, fill_const, &fc, "const");
        f.len = 0;
        long n = audio_out_service(&out);
        const int16_t *s = (const int16_t *)f.got;
        check(n > 0 && s[0] == 4000 && s[n * 2 - 1] == 4000,
              "F4 the attenuation is a library stage AFTER the fill and immediately "
              "before the write, so a client renders at its own peak");
        audio_out_close(&out);
    }

    printf("\n=== G. mode 2: synchronous, for a caller with NO render loop ===\n");
    {
        /* A loop-less helper: init, tone, usleep,
         * tone, close.  Nothing would ever service it. */
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 2048;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        long frames = audio_frames_for_ms(RATE, 200);
        audio_render_tone(RATE, 880, AUDIO_PEAK, mono, frames);
        f.len = 0;
        long n = audio_out_write(&out, mono, frames);
        check(n == frames && f.len == frames * f.fb,
              "G1 a whole 200 ms tone reaches the device with NO service loop — "
              "otherwise both Settings speaker tests are silently mute");
        check(f.transitions == 1,
              "G2 and it cost no stream transition: the tone is written into the "
              "same never-reset stream");
        audio_out_close(&out);

        /* Longer than the ring: the bound is derived from the tone, so it is not
         * truncated. */
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 4096;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        frames = audio_frames_for_ms(RATE, 2000);        /* 2 s > the 743 ms ring */
        audio_render_tone(RATE, 440, AUDIO_PEAK, mono, frames);
        f.len = 0;
        n = audio_out_write(&out, mono, frames);
        check(n == frames,
              "G3 a tone LONGER than the ring is not truncated — mode 2's wait "
              "bound is derived from the buffer's own duration, not a constant");
        audio_out_close(&out);

        /* A wedged device: bounded, not hanging. */
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 0;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        f.accept_upto = f.len + 8;
        frames = audio_frames_for_ms(RATE, 100);
        audio_render_tone(RATE, 880, AUDIO_PEAK, mono, frames);
        n = audio_out_write(&out, mono, frames);
        check(n == 2 && audio_out_misaligned(&out) == 0,
              "G4 a permanently full device is GIVEN UP ON at a frame boundary — "
              "bounded, because the alternative to giving up is hanging a UI");
        audio_out_close(&out);

        fake_reset(&f);
        check(audio_out_write(&out, mono, 100) == -1,
              "G5 a synchronous write on a closed stream is refused, not a crash");
    }

    printf("\n=== H. close DRAINS, bounded ===\n");
    {
        /* A device that drains: the tail gets out. */
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 2048;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        long frames = audio_frames_for_ms(RATE, 200);
        audio_render_tone(RATE, 880, AUDIO_PEAK, mono, frames);
        audio_out_write(&out, mono, frames);
        long q_before = f.qbytes;
        audio_out_close(&out);
        long slack_bytes = 2048 * 13 / 10 * f.fb;
        check(q_before > slack_bytes && f.qbytes <= slack_bytes &&
              audio_out_drain_waits(&out) > 0,
              "H1 close waits for the queued tail to play out, down to the slack — "
              "without it, exiting discards most of what the Settings tab just "
              "played");
        check(f.closes == 1, "H2 and then it closes the device exactly once");

        /* A device that NEVER drains: bounded by the ring, not forever. */
        fake_reset(&f);
        f.drain_all = false; f.drain_per_space = 0;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        audio_render_tone(RATE, 880, AUDIO_PEAK, mono, 4096);
        audio_out_write(&out, mono, 4096);
        audio_out_close(&out);
        long ring_ms = audio_ms_for_frames(RATE, 32768 + 2048);
        check(audio_out_drain_waits(&out) > 0 &&
              (long)audio_out_drain_waits(&out) <=
                  (ring_ms * 1000) / AUDIO_OUT_DRAIN_WAIT_US + 1,
              "H3 a device that never drains costs at most one ring's duration — "
              "the queue cannot be longer than the ring, so anything past that is "
              "a wedged device rather than a tail");

        /* The stop condition is the slack, not zero — an over-reporting device
         * never reads empty, so a wait-for-zero loop would always run to its
         * bound at every process exit. */
        fake_reset(&f);
        f.drain_all = true; f.over_report = 2662;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        audio_out_close(&out);
        check(audio_out_drain_waits(&out) == 0,
              "H4 an EMPTY but over-reporting device drains in zero waits: the stop "
              "condition is the 1.3-period slack, because in_flight never reads 0");

        fake_reset(&f);
        memset(&out, 0, sizeof(out));
        audio_out_close(&out);
        check(f.closes == 0,
              "H5 closing a struct that was never opened touches no device");
    }

    printf("\n=== I. the published service ceiling, from REAL audio ===\n");
    {
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        long us = audio_out_service_interval_us(&out);
        check(us > 33333,
              "I1 the ceiling is above FRAME_DELAY_ACTIVE_US (33 ms), which the "
              "sweep measured safe with about a period of margin");
        check(us < 66000,
              "I2 and below the 66 ms that survived with only ~11 ms of true "
              "content left — the published figure sits inside the region that was "
              "seen working, not at its edge");
        check(us < 100000,
              "I3 and far below FRAME_DELAY_IDLE_US (100 ms), which starves ~2.5x/s "
              "ON ITS OWN — this is why audio_pump_active() must stay in the "
              "frame-pacing decision");
        long nominal_us = audio_ms_for_frames(RATE, audio_out_lead(&out)) * 1000;
        check(nominal_us > 130000 && us < nominal_us / 2,
              "I4 the nominal lead is ~139 ms and the ceiling is less than half of "
              "it — a nominal number is never published as if it were audio");
        audio_out_close(&out);

        fake_reset(&f);
        f.fail_space = true;
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        check(audio_out_service_interval_us(&out) == 0,
              "I5 with no geometry measured the ceiling is 0, not a constant — "
              "\"not measured\" and \"55 ms\" are different claims");
        audio_out_close(&out);
    }

    printf("\n=== J. one stream per process ===\n");
    {
        static Fake g;
        AudioOut a, b;
        fake_reset(&f);
        fake_reset(&g);
        check(audio_out_open(&a, &FAKE_DEV, &f, RATE, 2) == 0,
              "J1 the first stream opens");
        check(audio_out_open(&b, &FAKE_DEV, &g, RATE, 2) != 0 && g.opens == 0,
              "J2 a second CONCURRENT open is refused before it reaches the device "
              "— the driver answers EBUSY, and a named refusal beats a half-done "
              "init");
        audio_out_close(&a);
        check(audio_out_open(&b, &FAKE_DEV, &g, RATE, 2) == 0,
              "J3 but SEQUENTIAL open/close pairs are fine, which is what "
              "two sequential short-lived Audio objects rely on");
        audio_out_close(&b);
    }

    /* ── K. which device the next open will use ──────────────────────────────
     *
     * ⚠️ **What this group can and cannot see.** The resolver's inputs are the
     * stored preference and `access("/dev/dsp1", W_OK)`, and this host has no
     * `/dev/dsp1` — so every case below exercises the CARD-ABSENT half. That is
     * deliberate rather than a gap papered over: K1 asserts the absence first,
     * so if a host ever did have that node the group would fail loudly instead
     * of quietly grading a different mechanism. The card-PRESENT half is
     * verifiable only on a device with a DAC attached, by reading which node the
     * process holds open — it is not claimed here.
     *
     * The negative control is K2 against K5: an unset preference and an
     * explicitly-onboard one must both resolve onboard, while "usb" must take a
     * DIFFERENT code path to reach the same answer. Without K6 the whole group
     * would still pass on a resolver that ignored its argument entirely. */
    printf("\n=== K. device resolution (card-absent half; see the header) ===\n");
    {
        check(!audio_out_usb_present(),
              "K1 this host has no writable /dev/dsp1 — every case below is "
              "therefore the card-ABSENT branch, and says so");

        check(strcmp(audio_out_device_pref(), "auto") == 0,
              "K2 the default preference is auto, which with no card "
              "present must still resolve to the panel speaker (K3)");
        check(strcmp(audio_out_device_path(), "/dev/dsp") == 0 &&
              audio_out_device_is_onboard(),
              "K3 and it resolves to the panel speaker");

        audio_out_set_device_pref("usb");
        check(strcmp(audio_out_device_pref(), "usb") == 0,
              "K4 the preference round-trips — it reports what was SET, not "
              "what was resolved");
        check(strcmp(audio_out_device_path(), "/dev/dsp") == 0 &&
              audio_out_device_is_onboard(),
              "K5 but with no card it FALLS BACK to the panel speaker: a games "
              "panel gone mute with no explanation is worse than one on the "
              "wrong speaker");

        audio_out_set_device_pref("auto");
        check(strcmp(audio_out_device_path(), "/dev/dsp") == 0,
              "K6 auto with no card is onboard too");

        audio_out_set_device_pref(NULL);
        check(strcmp(audio_out_device_pref(), "auto") == 0,
              "K7 NULL reads as the auto default rather than clearing the string, so a "
              "caller with no config file still names a device");
        audio_out_set_device_pref("wharrgarbl");
        check(strcmp(audio_out_device_path(), "/dev/dsp") == 0,
              "K8 an unrecognised value resolves onboard — a typo in the config "
              "file must not silence the panel");

        audio_out_set_device_pref("usbusbusbusbusbusbusbusb");
        check(strlen(audio_out_device_pref()) == 15,
              "K9 an over-long preference is TRUNCATED, not written past the "
              "buffer — and truncation makes it unrecognised, so K8 covers "
              "where it lands");

        audio_out_set_device_pref("onboard");   /* leave the process as found */

        /* ── The card-PRESENT half ────────────────────────────────────────────
         * These drive `audio_out_device_for()` directly, which is why it exists:
         * without them the nine checks above all pass against a resolver that
         * ignores its argument and always answers /dev/dsp — measured, that
         * exact sabotage was green before this block was added. */
        check(strcmp(audio_out_device_for("usb", true, false), "/dev/dsp1") == 0,
              "K10 with a card present, usb resolves to it — the case no host "
              "can reach through the wrapper");
        check(strcmp(audio_out_device_for("auto", true, false), "/dev/dsp1") == 0,
              "K11 and so does auto, which is what auto MEANS");
        check(strcmp(audio_out_device_for("onboard", true, false), "/dev/dsp") == 0,
              "K12 but onboard stays onboard with a card present — an explicit "
              "choice is not overridden by hardware appearing");
        check(strcmp(audio_out_device_for("usb", false, false), "/dev/dsp") == 0 &&
              strcmp(audio_out_device_for("auto", false, false), "/dev/dsp") == 0,
              "K13 and both fall back with it absent — the same answers K5/K6 "
              "reach through the wrapper, now shown to depend on the argument");
        check(strcmp(audio_out_device_for(NULL, true, false), "/dev/dsp") == 0,
              "K14 a NULL preference is onboard even with a card present");
    }

    printf("\n=== L. what a failed write means (ALSA classification, device lost) ===\n");
    {
        /* The classification is what alsa_write() acts on, and it is pure so the
         * branch an unplug takes is reachable on a host with no libasound. */
        check(audio_out_alsa_classify(-EAGAIN) == AO_ERR_AGAIN,
              "L1 -EAGAIN is 'the ring is full', the policies' retry case");
        check(audio_out_alsa_classify(-EPIPE) == AO_ERR_XRUN,
              "L2 -EPIPE is an underrun, recovered by a prepare — not a lost device");
        check(audio_out_alsa_classify(-ESTRPIPE) == AO_ERR_SUSPEND,
              "L3 -ESTRPIPE is a suspend, recovered by resume/prepare");
        check(audio_out_alsa_classify(-ENODEV) == AO_ERR_LOST,
              "L4 -ENODEV is the device gone — the USB DAC unplugged");
        check(audio_out_alsa_classify(-ENOTCONN) == AO_ERR_LOST &&
              audio_out_alsa_classify(-ESHUTDOWN) == AO_ERR_LOST &&
              audio_out_alsa_classify(-EPIPE) == AO_ERR_XRUN,
              "L4b a socket-shaped loss (a BlueALSA transport gone) is LOST too; "
              "CONTROL: -EPIPE is still an XRUN");
        check(audio_out_alsa_classify(-EBADFD) == AO_ERR_LOST,
              "L5 -EBADFD is also gone: the state a PCM is left in after its "
              "card disappears, so it must reopen rather than retry forever");
        check(audio_out_alsa_classify(-EIO) == AO_ERR_OTHER,
              "L6 CONTROL: -EIO is an ordinary error, not a lost device — a "
              "classifier answering LOST for everything would reopen every glitch");

        /* The flag is set in the generic layer from the backend's errno, so the
         * fake reaches it with no backend code at all. */
        fake_reset(&f);
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0 &&
              !audio_out_device_lost(&out),
              "L7 a freshly opened stream is not lost");
        f.drain_all   = true;
        f.accept_upto = f.len;
        f.hard_error  = true;
        f.hard_errno  = EIO;
        audio_out_service(&out);
        check(audio_out_sink_errors(&out) > 0 && !audio_out_device_lost(&out),
              "L8 CONTROL: a write failing with EIO is a sink error and NOT a "
              "lost device — the flag means one thing");
        f.hard_errno  = ENODEV;
        audio_out_service(&out);
        check(audio_out_device_lost(&out),
              "L9 a write failing with ENODEV marks the device lost, which is "
              "what audio.c polls to reopen after an unplug");
        audio_out_close(&out);
        check(!audio_out_device_lost(&out),
              "L10 a closed stream is not lost — the flag belongs to one open");

        fake_reset(&f);
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0 &&
              !audio_out_device_lost(&out),
              "L11 and a reopen starts clean");
        audio_out_close(&out);
        check(!audio_out_device_lost(NULL), "L12 NULL is not lost");

        /* A gone device usually fails the free-space query first, and a service
         * that stops there never reaches a write. */
        fake_reset(&f);
        audio_out_open(&out, &FAKE_DEV, &f, RATE, 2);
        f.fail_space = true;
        f.hard_errno = EIO;
        audio_out_service(&out);
        check(!audio_out_device_lost(&out),
              "L12b CONTROL: a space query failing EIO is not a lost device");
        f.hard_errno = ENODEV;
        audio_out_service(&out);
        check(audio_out_device_lost(&out),
              "L12c a space query failing ENODEV marks the device lost, with no "
              "write attempted");
        f.fail_space = false;
        audio_out_close(&out);

        /* OSS minor N is ALSA card N on this device. */
        check(strcmp(audio_out_device_pcm("/dev/dsp"), "plughw:0,0") == 0,
              "L13 the onboard OSS node maps to ALSA card 0");
        check(strcmp(audio_out_device_pcm("/dev/dsp1"), "plughw:1,0") == 0,
              "L14 the USB OSS node maps to ALSA card 1");
        check(strcmp(audio_out_device_pcm(NULL), "plughw:0,0") == 0 &&
              strcmp(audio_out_device_pcm("/dev/null"), "plughw:0,0") == 0,
              "L15 anything else maps onboard, the same fallback the resolver has");
    }

    printf("\n=== M. the replug probe's gate (pure) ===\n");
    {
        /* audio_out_usb_returned() also needs card 1's node, which no host has,
         * so the decision in front of it is driven directly.  M1 is the case the
         * whole group exists for: a stream on the panel under "auto" must look
         * again, or a replugged DAC is never picked up. */
        check(audio_out_reprobe_due("auto", "/dev/dsp", 5000, 4000),
              "M1 on the panel under \"auto\", 1000 ms after the last probe: due");
        check(audio_out_reprobe_due("usb", "/dev/dsp", 5000, 4000),
              "M2 the same under \"usb\"");
        check(!audio_out_reprobe_due("auto", "/dev/dsp", 4999, 4000),
              "M3 999 ms after the last probe: not yet — the probe is rate-limited");
        check(!audio_out_reprobe_due("onboard", "/dev/dsp", 9000, 0),
              "M4 CONTROL: \"onboard\" never probes — the panel is what was asked for");
        check(!audio_out_reprobe_due("bogus", "/dev/dsp", 9000, 0),
              "M5 an unrecognised preference resolves onboard, so it never probes");
        check(!audio_out_reprobe_due("usb", "/dev/dsp1", 9000, 0) &&
              !audio_out_reprobe_due("auto", "bluealsa", 9000, 0) &&
              !audio_out_reprobe_due("bluetooth", "bluealsa", 9000, 0),
              "M6 CONTROL: a stream already on the best sink its preference "
              "allows never probes");
        check(audio_out_reprobe_due("auto", "/dev/dsp1", 9000, 0) &&
              audio_out_reprobe_due("bluetooth", "/dev/dsp", 9000, 0) &&
              audio_out_reprobe_due("bluetooth", "/dev/dsp1", 9000, 0),
              "M6b but a stream on USB or onboard under auto/bluetooth probes, "
              "because a Bluetooth sink outranks both");
        check(!audio_out_reprobe_due("auto", NULL, 9000, 0) &&
              !audio_out_reprobe_due(NULL, "/dev/dsp", 9000, 0),
              "M7 no open path (a stream from another opener) or no preference: never");
        /* 0x100 ms before the wrap plus 800 after it is 1056 ms elapsed. */
        check(audio_out_reprobe_due("auto", "/dev/dsp", 800u, 0xFFFFFF00u) &&
              !audio_out_reprobe_due("auto", "/dev/dsp", 700u, 0xFFFFFF00u),
              "M8 the millisecond clock wrapping between probes neither stops the "
              "probe nor fires it early");

        /* A stream opened behind the fake vtable has no open path, so the wrapper
         * must answer FALSE without touching the filesystem's answer at all. */
        fake_reset(&f);
        audio_out_set_device_pref("auto");
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0 &&
              !audio_out_usb_returned(&out),
              "M9 a stream with no open path is never moved");
        audio_out_close(&out);
        check(!audio_out_usb_returned(&out) && !audio_out_usb_returned(NULL),
              "M10 a closed stream, or NULL, is never moved");
    }

    printf("\n=== N. the output choice list (pure) ===\n");
    {
        /* The persisted strings are the config file's contract, so they are
         * asserted by value: a renamed entry would orphan every saved setting. */
        check(AUDIO_OUT_CHOICE_COUNT == 4 &&
              strcmp(audio_out_choice_name(AUDIO_OUT_CHOICE_ONBOARD), "onboard") == 0 &&
              strcmp(audio_out_choice_name(AUDIO_OUT_CHOICE_USB),     "usb")     == 0 &&
              strcmp(audio_out_choice_name(AUDIO_OUT_CHOICE_BT),  "bluetooth")   == 0 &&
              strcmp(audio_out_choice_name(AUDIO_OUT_CHOICE_AUTO),    "auto")    == 0,
              "N1 the table holds the four persisted values, in cycle order");
        check(audio_out_choice_of("usb")  == AUDIO_OUT_CHOICE_USB &&
              audio_out_choice_of("auto") == AUDIO_OUT_CHOICE_AUTO &&
              audio_out_choice_of("bogus") == AUDIO_OUT_CHOICE_ONBOARD &&
              audio_out_choice_of(NULL)   == AUDIO_OUT_CHOICE_ONBOARD,
              "N2 a name maps back to its entry; anything else is onboard, which "
              "is what audio_out_device_for() resolves it to");

        bool usb_ever = false;
        for (int c = 0; c < AUDIO_OUT_CHOICE_COUNT; c++)
            if (audio_out_choice_next(c, false, false) == AUDIO_OUT_CHOICE_USB) usb_ever = true;
        check(!usb_ever,
              "N3 with no USB DAC, a press never lands on USB — from ANY start, "
              "including USB itself");
        check(!audio_out_choice_available(AUDIO_OUT_CHOICE_USB, false, false) &&
              audio_out_choice_available(AUDIO_OUT_CHOICE_USB, true, false) &&
              audio_out_choice_available(AUDIO_OUT_CHOICE_ONBOARD, false, false) &&
              audio_out_choice_available(AUDIO_OUT_CHOICE_AUTO, false, false),
              "N4 USB is listed only while present; ONBOARD and AUTO always are");
        check(audio_out_choice_shown(AUDIO_OUT_CHOICE_USB, false, false) == AUDIO_OUT_CHOICE_AUTO,
              "N5 a saved USB with no DAC SHOWS as AUTO");
        check(audio_out_choice_shown(AUDIO_OUT_CHOICE_USB, true, false) == AUDIO_OUT_CHOICE_USB,
              "N6 CONTROL: a saved USB with the DAC present shows as USB");
        check(audio_out_choice_shown(AUDIO_OUT_CHOICE_ONBOARD, false, false) == AUDIO_OUT_CHOICE_ONBOARD &&
              audio_out_choice_shown(AUDIO_OUT_CHOICE_AUTO, true, false) == AUDIO_OUT_CHOICE_AUTO,
              "N7 an available saved choice shows as itself");
        check(audio_out_choice_next(AUDIO_OUT_CHOICE_ONBOARD, true, false) == AUDIO_OUT_CHOICE_USB &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_USB, true, false)     == AUDIO_OUT_CHOICE_AUTO &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_AUTO, true, false)    == AUDIO_OUT_CHOICE_ONBOARD,
              "N8 with USB present the cycle is onboard -> usb -> auto -> onboard");
        check(audio_out_choice_next(AUDIO_OUT_CHOICE_ONBOARD, false, false) == AUDIO_OUT_CHOICE_AUTO &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_AUTO, false, false)    == AUDIO_OUT_CHOICE_ONBOARD,
              "N9 with USB absent the cycle is onboard -> auto -> onboard");
        check(audio_out_choice_shown(-1, true, false) == AUDIO_OUT_CHOICE_ONBOARD &&
              audio_out_choice_shown(AUDIO_OUT_CHOICE_COUNT, true, false) == AUDIO_OUT_CHOICE_ONBOARD &&
              audio_out_choice_name(AUDIO_OUT_CHOICE_COUNT) != NULL &&
              audio_out_choice_label(-1) != NULL,
              "N10 an out-of-range index reads as onboard rather than indexing past the table");
    }

    printf("\n=== O. Bluetooth in the resolver and the choice list (pure) ===\n");
    {
        /* Every preference against every presence combination.  The expected
         * column is the design: auto = BT > USB > onboard, "bluetooth" with no
         * sink = auto's remainder, "usb" never picks BT, onboard is onboard. */
        static const struct { const char *pref; bool usb, bt; const char *want; } T[] = {
            { "onboard",   false, false, "/dev/dsp"  }, { "onboard",   true,  true,  "/dev/dsp"  },
            { "onboard",   false, true,  "/dev/dsp"  }, { "onboard",   true,  false, "/dev/dsp"  },
            { "usb",       false, false, "/dev/dsp"  }, { "usb",       true,  false, "/dev/dsp1" },
            { "usb",       false, true,  "/dev/dsp"  }, { "usb",       true,  true,  "/dev/dsp1" },
            { "auto",      false, false, "/dev/dsp"  }, { "auto",      true,  false, "/dev/dsp1" },
            { "auto",      false, true,  "bluealsa"  }, { "auto",      true,  true,  "bluealsa"  },
            { "bluetooth", false, false, "/dev/dsp"  }, { "bluetooth", true,  false, "/dev/dsp1" },
            { "bluetooth", false, true,  "bluealsa"  }, { "bluetooth", true,  true,  "bluealsa"  },
            { NULL,        true,  true,  "/dev/dsp"  }, { "bogus",     true,  true,  "/dev/dsp"  },
        };
        int bad = 0;
        for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
            const char *got = audio_out_device_for(T[i].pref, T[i].usb, T[i].bt);
            if (strcmp(got, T[i].want) != 0) {
                printf("    %s usb=%d bt=%d -> %s, want %s\n", T[i].pref ? T[i].pref : "(null)",
                       T[i].usb, T[i].bt, got, T[i].want);
                bad++;
            }
        }
        check(bad == 0, "O1 the resolver's order over all 18 preference x presence rows");
        check(strcmp(audio_out_device_pcm("bluealsa"), "plug:bluealsa") == 0 &&
              strcmp(audio_out_device_pcm("/dev/dsp1"), "plughw:1,0") == 0,
              "O2 the Bluetooth path names plug:bluealsa (plug converts mono and odd rates)");
        check(audio_out_sink_better("bluealsa", "/dev/dsp1") &&
              audio_out_sink_better("bluealsa", "/dev/dsp") &&
              audio_out_sink_better("/dev/dsp1", "/dev/dsp") &&
              !audio_out_sink_better("/dev/dsp1", "bluealsa") &&
              !audio_out_sink_better("/dev/dsp", "/dev/dsp") &&
              !audio_out_sink_better("bluealsa", "bluealsa"),
              "O3 a move goes only UP the BT > USB > onboard order, never sideways or down");

        bool bt_ever = false;
        for (int c = 0; c < AUDIO_OUT_CHOICE_COUNT; c++)
            for (int u = 0; u < 2; u++)
                if (audio_out_choice_next(c, u, false) == AUDIO_OUT_CHOICE_BT) bt_ever = true;
        check(!bt_ever, "O4 with no Bluetooth sink a press never lands on BLUETOOTH, from any "
                        "start and with or without USB");
        check(!audio_out_choice_available(AUDIO_OUT_CHOICE_BT, true, false) &&
              audio_out_choice_available(AUDIO_OUT_CHOICE_BT, false, true),
              "O5 BLUETOOTH is listed only while a sink is present, independent of USB");
        check(audio_out_choice_shown(AUDIO_OUT_CHOICE_BT, true, false) == AUDIO_OUT_CHOICE_AUTO &&
              audio_out_choice_shown(AUDIO_OUT_CHOICE_BT, false, true) == AUDIO_OUT_CHOICE_BT,
              "O6 a saved bluetooth with no sink SHOWS as AUTO; CONTROL: present shows itself");
        check(audio_out_choice_next(AUDIO_OUT_CHOICE_ONBOARD, true, true) == AUDIO_OUT_CHOICE_USB &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_USB, true, true)     == AUDIO_OUT_CHOICE_BT &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_BT, true, true)      == AUDIO_OUT_CHOICE_AUTO &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_AUTO, true, true)    == AUDIO_OUT_CHOICE_ONBOARD &&
              audio_out_choice_next(AUDIO_OUT_CHOICE_ONBOARD, false, true) == AUDIO_OUT_CHOICE_BT,
              "O7 with both present: onboard -> usb -> bluetooth -> auto; BT alone: onboard -> bluetooth");
        check(audio_out_choice_of("bluetooth") == AUDIO_OUT_CHOICE_BT &&
              strcmp(audio_out_choice_label(AUDIO_OUT_CHOICE_BT), "BLUETOOTH") == 0,
              "O8 the persisted name and the button label");
    }

    printf("\n=== P. the Bluetooth probe window (pure) ===\n");
    {
        AudioOutBtGate g;
        bool changed = false;

        memset(&g, 0, sizeof(g));
        check(!audio_out_bt_probe_due(&g, 0, 0, &changed),
              "P1 no ACL link at the first look: no PCM probe at all");

        memset(&g, 0, sizeof(g));
        check(audio_out_bt_probe_due(&g, 0x1234, 0, &changed),
              "P2 an ACL link at the first look: exactly one probe now");
        int late = 0;
        for (uint32_t t = 100; t <= 60000; t += 100)
            if (audio_out_bt_probe_due(&g, 0x1234, t, &changed)) late++;
        check(late == 0 && !changed,
              "P3 an UNCHANGED link set never probes again (a keyboard and pad held "
              "connected cost no PCM open)");

        /* A change opens the window: probes at most once per second, for
         * AUDIO_OUT_BT_WINDOW_MS, then nothing. */
        int in_win = 0, after = 0;
        uint32_t last = 0, min_gap = 0xFFFFFFFFu;
        bool first = audio_out_bt_probe_due(&g, 0x5678, 70000, &changed);
        check(first && changed, "P4 a changed link set probes at once and reports the change");
        last = 70000;
        for (uint32_t t = 70050; t <= 100000; t += 50) {
            if (!audio_out_bt_probe_due(&g, 0x5678, t, &changed)) continue;
            if (t - 70000 < AUDIO_OUT_BT_WINDOW_MS) in_win++; else after++;
            if (t - last < min_gap) min_gap = t - last;
            last = t;
        }
        printf("    window: %d probes after the first, %d after it closed, min gap %u ms\n",
               in_win, after, min_gap);
        check(in_win == (AUDIO_OUT_BT_WINDOW_MS / AUDIO_OUT_BT_PROBE_MS) - 1 &&
              min_gap >= AUDIO_OUT_BT_PROBE_MS,
              "P5 inside the window: one probe per second, never faster");
        check(after == 0, "P6 the window closes: no probe after it");

        check(!audio_out_bt_probe_due(&g, 0, 200000, &changed) && changed,
              "P7 every link gone: a change, and no probe (nothing to find)");

        /* The window arithmetic is unsigned, so it survives the ms clock wrap. */
        memset(&g, 0, sizeof(g));
        audio_out_bt_probe_due(&g, 1, 0xFFFFF000u, &changed);
        audio_out_bt_probe_due(&g, 2, 0xFFFFFC00u, &changed);
        check(audio_out_bt_probe_due(&g, 2, 0xFFFFFC00u + AUDIO_OUT_BT_PROBE_MS, &changed) &&
              !audio_out_bt_probe_due(&g, 2, 0xFFFFFC00u + AUDIO_OUT_BT_WINDOW_MS + 5000u,
                                      &changed),
              "P8 across the 32-bit ms wrap the window still probes, then closes");
    }

    printf("\n=== Q. the ACL signature reads /sys/class/bluetooth names ===\n");
    {
        char dir[] = "/tmp/ao_bt_XXXXXX", p[96];
        if (!mkdtemp(dir)) { check(false, "Q0 mkdtemp"); }
        else {
            snprintf(p, sizeof(p), "%s/hci0", dir);     mkdir(p, 0700);
            uint32_t none = audio_out_bt_acl_signature(dir);
            snprintf(p, sizeof(p), "%s/hci0:11", dir);  mkdir(p, 0700);
            uint32_t one = audio_out_bt_acl_signature(dir);
            snprintf(p, sizeof(p), "%s/hci0:12", dir);  mkdir(p, 0700);
            uint32_t two = audio_out_bt_acl_signature(dir);
            rmdir(p);
            snprintf(p, sizeof(p), "%s/hci0:13", dir);  mkdir(p, 0700);
            uint32_t swapped = audio_out_bt_acl_signature(dir);
            check(none == 0, "Q1 the controller alone (hci0) is no link: signature 0");
            check(one != 0 && two != 0 && one != two && swapped != two,
                  "Q2 each link set, including one link replaced by another, signs differently");
            check(audio_out_bt_acl_signature("/nonexistent/bt") == 0,
                  "Q3 no Bluetooth class directory at all reads as no link");
            snprintf(p, sizeof(p), "%s/hci0:11", dir); rmdir(p);
            snprintf(p, sizeof(p), "%s/hci0:13", dir); rmdir(p);
            snprintf(p, sizeof(p), "%s/hci0", dir);    rmdir(p);
            rmdir(dir);
        }
    }

    printf("\n=== R. USE FOR AUDIO pins the Bluetooth sink to one headset ===\n");
    {
        const char *A = "AA:BB:CC:DD:EE:FF", *B = "11:22:33:44:55:66";
        const char *PIN_A = "bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp";
        char buf[64];

        check(audio_out_bt_addr_valid(A) && audio_out_bt_addr_valid("aa:bb:cc:dd:ee:ff") &&
              audio_out_bt_addr_valid("0a:Bc:9F:00:e1:7D"),
              "R1 a BD address in either case is valid");
        const char *bad_addr[] = {
            NULL, "", "AA:BB:CC:DD:EE:F", "AA:BB:CC:DD:EE:FF0", "AA-BB-CC-DD-EE-FF",
            "GG:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF,PROFILE=sco", "AABB:CC:DD:EE:FF:",
        };
        int accepted = 0;
        for (size_t i = 0; i < sizeof(bad_addr) / sizeof(bad_addr[0]); i++)
            if (audio_out_bt_addr_valid(bad_addr[i])) {
                printf("    accepted: %s\n", bad_addr[i] ? bad_addr[i] : "(null)");
                accepted++;
            }
        check(accepted == 0, "R2 a malformed address, including one carrying ALSA ',' / '=' "
                             "syntax, is refused");

        check(strcmp(audio_out_bt_pcm_for("bluetooth", A, buf, sizeof(buf)), PIN_A) == 0,
              "R3 \"bluetooth\" + a valid address names that headset's PCM");
        check(strcmp(audio_out_bt_pcm_for("auto", A, buf, sizeof(buf)), PIN_A) == 0 &&
              strcmp(audio_out_bt_pcm_for("usb", A, buf, sizeof(buf)), "plug:bluealsa") == 0 &&
              strcmp(audio_out_bt_pcm_for(NULL, A, buf, sizeof(buf)), "plug:bluealsa") == 0,
              "R4 \"auto\" keeps the pin too (a preference, not a filter); \"usb\" never pins");
        check(strcmp(audio_out_bt_pcm_for("bluetooth", "", buf, sizeof(buf)), "plug:bluealsa") == 0 &&
              strcmp(audio_out_bt_pcm_for("bluetooth", "AA:BB", buf, sizeof(buf)), "plug:bluealsa") == 0 &&
              strcmp(audio_out_bt_pcm_for("bluetooth", A, buf, 20), "plug:bluealsa") == 0,
              "R5 unpinned, malformed, or a buffer too small: the unpinned PCM, never a torn name");
        const char *pin = audio_out_bt_pcm_for("bluetooth", A, buf, sizeof(buf));
        check(strncmp(pin, "plug:", 5) != 0 && strchr(pin, ',') == strrchr(pin, ','),
              "R6 the pinned name is not wrapped in plug: (pcm.bluealsa is already a plug, "
              "and plug's own argument parser would split it at ',' and '=')");

        check(audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_BT, A, A) &&
              audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_BT, A, "aa:bb:cc:dd:ee:ff"),
              "R7 BLUETOOTH pinned to this address is this headset's audio, either case");
        check(!audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_BT, A, B) &&
              !audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_BT, "", A),
              "R8 BLUETOOTH pinned elsewhere, or unpinned, still offers USE FOR AUDIO "
              "on this headset");
        check(!audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_USB, A, A) &&
              !audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_ONBOARD, A, A) &&
              audio_out_bt_is_pinned(AUDIO_OUT_CHOICE_AUTO, A, A),
              "R9 AUTO keeps the pin; USB and ONBOARD are nobody's pin, whatever is stored");

        audio_out_set_device_pref("bluetooth");
        audio_out_set_bt_addr(A);
        bool w1 = strcmp(audio_out_device_pcm("bluealsa-pin"), PIN_A) == 0 &&
                  strcmp(audio_out_device_pcm("bluealsa"), "plug:bluealsa") == 0 &&
                  strcmp(audio_out_bt_addr(), A) == 0;
        audio_out_set_device_pref("usb");
        bool w2 = strcmp(audio_out_device_pcm("bluealsa-pin"), "plug:bluealsa") == 0;
        audio_out_set_device_pref("auto");
        bool w3 = strcmp(audio_out_device_pcm("bluealsa-pin"), PIN_A) == 0;
        audio_out_set_bt_addr("AA:BB:CC:DD:EE:FF,PROFILE=sco");
        bool w4 = strcmp(audio_out_device_pcm("bluealsa-pin"), "plug:bluealsa") == 0 &&
                  audio_out_bt_addr()[0] == '\0';
        check(w1 && w2 && w3 && w4, "R10 the pinned tier's PCM follows both setters, in either "
                                    "order, \"bluealsa\" stays any sink, and a malformed "
                                    "address unpins");
        audio_out_set_bt_addr(NULL);                /* leave the process as found */
        audio_out_set_device_pref("onboard");
    }

    printf("\n=== S. the pin is a preference: pinned > any BT > USB > onboard (pure) ===\n");
    {
        /* The operator's two panel runs (2026-10-02, pinned WI-C310 plus a VT360
         * speaker) are rows here.  Under d949753 a pinned "bluetooth" with the
         * pinned headset off fell to the speaker although the VT360 was there,
         * and "auto" on the VT360 never went back to the returning WI-C310. */
        static const struct {
            const char *pref; bool usb, pin, any; const char *want;
        } T[] = {
            /* the BT tier's own order, under both preferences that reach it */
            { "bluetooth", false, true,  true,  "bluealsa-pin" },
            { "bluetooth", false, false, true,  "bluealsa"     },  /* run 1: VT360, not speaker */
            { "bluetooth", true,  false, false, "/dev/dsp1"    },
            { "bluetooth", false, false, false, "/dev/dsp"     },
            { "auto",      true,  true,  true,  "bluealsa-pin" },
            { "auto",      true,  false, true,  "bluealsa"     },
            { "auto",      true,  false, false, "/dev/dsp1"    },
            { "auto",      false, false, false, "/dev/dsp"     },
            { "auto",      false, true,  false, "bluealsa-pin" },
            /* CONTROL: preferences that never pick Bluetooth ignore both tiers */
            { "usb",       true,  true,  true,  "/dev/dsp1"    },
            { "usb",       false, true,  true,  "/dev/dsp"     },
            { "onboard",   true,  true,  true,  "/dev/dsp"     },
        };
        int bad = 0;
        for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
            const char *got = audio_out_device_for_bt(T[i].pref, T[i].usb, T[i].pin, T[i].any);
            if (strcmp(got, T[i].want) != 0) {
                printf("    %s usb=%d pin=%d any=%d -> %s, want %s\n", T[i].pref,
                       T[i].usb, T[i].pin, T[i].any, got, T[i].want);
                bad++;
            }
        }
        check(bad == 0, "S1 the resolver: pinned headset, then any BT sink, then USB, then onboard");
        check(strcmp(audio_out_device_for_bt("auto", true, false, true),
                     audio_out_device_for("auto", true, true)) == 0 &&
              strcmp(audio_out_device_for_bt("bluetooth", false, false, false),
                     audio_out_device_for("bluetooth", false, false)) == 0,
              "S2 CONTROL: with no pinned headset present the split resolver is the old one");

        check(audio_out_sink_better("bluealsa-pin", "bluealsa") &&
              audio_out_sink_better("bluealsa-pin", "/dev/dsp") &&
              !audio_out_sink_better("bluealsa", "bluealsa-pin") &&
              !audio_out_sink_better("bluealsa-pin", "bluealsa-pin"),
              "S3 a move to the pinned headset is UP from any other sink, and never away from it");

        check(audio_out_reprobe_due_pin("auto", true, "bluealsa", 9000, 0) &&
              audio_out_reprobe_due_pin("bluetooth", true, "bluealsa", 9000, 0) &&
              audio_out_reprobe_due_pin("bluetooth", true, "/dev/dsp", 9000, 0),
              "S4 on another sink with a pin in force the probe keeps looking (run 2: back "
              "from the VT360 to the WI-C310)");
        check(!audio_out_reprobe_due_pin("auto", false, "bluealsa", 9000, 0) &&
              !audio_out_reprobe_due_pin("auto", true, "bluealsa-pin", 9000, 0) &&
              !audio_out_reprobe_due_pin("onboard", true, "/dev/dsp", 9000, 0) &&
              !audio_out_reprobe_due_pin("auto", true, "bluealsa", 999, 0),
              "S5 CONTROL: unpinned any-sink, already pinned, onboard, and inside the rate "
              "limit all stay quiet");
    }
    printf("\n=== T. silence skips the DSP (after a hangover) ===\n");
    {
        const int gains[EQ_BANDS] = { 6, 6, 6, 6, 6 };
        const AudioEnh en = { 1, 60, 50, 60, 50, 60, 50 };
        const long hang = audio_frames_for_ms(RATE, 300);

        fake_reset(&f);
        memset(&fc, 0, sizeof(fc));
        check(audio_out_open(&out, &FAKE_DEV, &f, RATE, 2) == 0, "T0 opens");
        audio_out_set_eq(&out, gains);
        audio_out_set_enh(&out, &en);
        audio_out_set_fill(&out, fill_const, &fc, "t");
        const long lead = audio_out_lead(&out);

        /* Prime the filters with sound, then go silent. */
        fc.value = 8000; fc.produce = -1;
        for (int i = 0; i < 3; i++) audio_out_service(&out);
        fc.value = 0;

        /* A sentinel in the history shows whether a DSP call touched it: the
         * filters rewrite their history on every call, a skip leaves it alone. */
        int ran = 0, skipped_at = -1;
        bool zeros = true;
        long frames_dsp = 0;
        for (int i = 1; i <= 12; i++) {
            out.eq_s.h[0][0].y1 = 12345;
            long before = f.len;
            audio_out_service(&out);
            bool dsp_ran = (out.eq_s.h[0][0].y1 != 12345);
            if (dsp_ran) { ran++; frames_dsp += lead; }
            else if (skipped_at < 0) skipped_at = i;
            /* The hangover legitimately carries the filters' ring-down of the
             * sound before it; only a skipped fill has to be exactly zeros. */
            if (!dsp_ran && !all_zero(f.got, before, f.len)) zeros = false;
        }
        check(zeros, "T1 the output of every skipped fill is all zeros");
        check(ran >= 1 && frames_dsp >= hang,
              "T2 during the hangover the DSP still runs: at least 300 ms of zeros "
              "went through it");
        check(skipped_at > 0 && skipped_at == ran + 1,
              "T3 after the hangover the DSP is NOT called on silent fills");
        {
            long before = f.len;
            out.eq_s.h[0][0].y1 = 12345;
            audio_out_service(&out);
            check(out.eq_s.h[0][0].y1 == 12345 && all_zero(f.got, before, f.len),
                  "T3b and it stays skipped on the next silent fill");
        }

        /* Sound resumes: the DSP runs again, from a fresh state. */
        fc.value = 8000;
        long before = f.len;
        audio_out_service(&out);
        long n = f.len - before;
        check(out.eq_s.h[0][0].y1 != 12345 && n > 0 && !all_zero(f.got, before, f.len),
              "T4 a non-silent fill after a skip is processed by the DSP");

        /* The reference: the same loud buffer through a fresh EQ and enhancer. */
        {
            static int16_t ref[1 << 15];
            long frames = n / f.fb;
            for (long i = 0; i < frames * 2; i++) ref[i] = 8000;
            EqState es; EnhState hs;
            eq_state_reset(&es); enh_state_reset(&hs);
            eq_process(&es, &out.eq_p, ref, frames, 2);
            enh_process(&hs, &out.enh_p, ref, frames, 2);
            audio_attenuate(ref, frames * 2, out.shift);
            check(frames > 0 && frames * 2 <= (long)(sizeof(ref) / sizeof(ref[0])) &&
                  memcmp(ref, f.got + before, (size_t)n) == 0,
                  "T5 and its output equals a fresh-state DSP's — no stale tail from "
                  "before the skip");
        }
        audio_out_close(&out);
    }


    printf("\n=== V. the system volume: volume_step, 0 = mute, 3 dB a step, 10 = unity ===\n");
    {
        static int16_t vin[4096], vout[4096];
        bool ident = true, mono_up = true, nowrap = true, sign_ok = true, mute0 = true;

        /* V1: step 10 is a bit-exact passthrough over every int16, set or not. */
        for (int pass = 0; pass < 2; pass++) {
            ident = true;
            for (long base = -32768; base < 32768; base += 4096) {
                for (long i = 0; i < 4096; i++) vin[i] = (int16_t)(base + i);
                if (vol_run(pass == 1, 10, vin, 4096, vout) != 4096) ident = false;
                if (memcmp(vin, vout, sizeof(vin)) != 0) ident = false;
            }
            check(ident, pass ? "V1b step 10 set explicitly: all 65536 int16 values pass bit-exact"
                              : "V1a no volume set (default): all 65536 int16 values pass bit-exact");
        }

        /* V2: step 0 is silence, whatever the input. */
        for (long i = 0; i < 4096; i++) vin[i] = (int16_t)((i & 1) ? 32767 : -32768);
        vol_run(true, 0, vin, 4096, vout);
        for (long i = 0; i < 4096; i++) if (vout[i] != 0) mute0 = false;
        check(mute0, "V2 step 0 (mute) writes all zeros");

        /* V3: steps 1..9 are the stated dB, within the Q12 table's rounding. */
        for (long i = 0; i < 4096; i++) vin[i] = 10000;
        vol_run(true, 7, vin, 4096, vout);
        check(vout[100] >= 3546 && vout[100] <= 3550,
              "V3a step 7 is -9 dB: 10000 -> ~3548");
        vol_run(true, 1, vin, 4096, vout);
        check(vout[100] >= 445 && vout[100] <= 449,
              "V3b step 1 is -27 dB: 10000 -> ~447");
        {
            int prev = -1;
            bool up = true;
            for (int s = 0; s <= AUDIO_VOLUME_MAX; s++) {
                vol_run(true, s, vin, 4096, vout);
                if (vout[100] < prev) up = false;
                prev = vout[100];
            }
            check(up, "V3c output for a fixed input never falls as the step rises, 0..14");
        }
        {
            for (long i = 0; i < 4096; i++) vin[i] = 20000;
            vol_run(true, 9, vin, 4096, vout);
            check(vout[100] <= 20000 && vout[100] >= 13900,
                  "V3d steps below 10 never exceed the input (step 9: 20000 -> ~13950)");
        }

        /* V4: step 14 boosts +12 dB below the knee and never wraps above it. */
        for (long i = 0; i < 4096; i++) vin[i] = 4000;
        vol_run(true, 14, vin, 4096, vout);
        check(vout[100] >= 15920 && vout[100] <= 15930,
              "V4a step 14 is +12 dB below the limiter knee: 4000 -> ~15925");
        {
            const double amps[2] = { 32767.0, 16384.0 };
            for (int a = 0; a < 2; a++) {
                int peak = 0;
                for (long i = 0; i < 4096; i++)
                    vin[i] = (int16_t)(amps[a] * sin(2.0 * M_PI * (double)i / 64.0));
                vol_run(true, 14, vin, 4096, vout);
                for (long i = 0; i < 4096; i++) {
                    int m = vout[i] < 0 ? -vout[i] : vout[i];
                    if (m > peak) peak = m;
                    if (vin[i] > 0 && vout[i] < 0) nowrap = false;
                    if (vin[i] < 0 && vout[i] > 0) nowrap = false;
                    if (vin[i] == 0 && vout[i] != 0) sign_ok = false;
                }
                check(peak <= 32767 && peak > 22000,
                      a == 0 ? "V4b step 14 on a full-scale sine: peak within int16, boosted"
                             : "V4c step 14 on a half-scale sine: peak within int16, boosted");
            }
            check(nowrap && sign_ok, "V4d no sign flip (wrap) and no offset on either sine");
        }
        {
            /* Monotone in |input|, odd-symmetric, over the whole magnitude range. */
            int last_p = -1, last_n = 1;
            for (long k = 0; k < 4096; k++) vin[k] = (int16_t)(k * 8);        /* 0..32760 */
            vol_run(true, 14, vin, 4096, vout);
            for (long k = 0; k < 4096; k++) { if (vout[k] < last_p) mono_up = false; last_p = vout[k]; }
            for (long k = 0; k < 4096; k++) vin[k] = (int16_t)(-(k * 8));
            vol_run(true, 14, vin, 4096, vout);
            for (long k = 0; k < 4096; k++) { if (vout[k] > last_n) mono_up = false; last_n = vout[k]; }
            check(mono_up, "V4e step 14 is monotone in |input| on both polarities");
        }

        /* V5: the setter clamps, and a NULL stream is safe. */
        {
            AudioOut o;
            fake_reset(&vf);
            check(audio_out_open(&o, &FAKE_DEV, &vf, 44100, 2) == 0 &&
                  audio_out_get_volume_step(&o) == AUDIO_VOLUME_DEFAULT,
                  "V5a a fresh stream with no key holds the default step (10)");
            audio_out_set_volume_step(&o, -5);
            check(audio_out_get_volume_step(&o) == 0, "V5b step -5 clamps to 0");
            audio_out_set_volume_step(&o, 99);
            check(audio_out_get_volume_step(&o) == AUDIO_VOLUME_MAX, "V5c step 99 clamps to 14");
            audio_out_close(&o);
            audio_out_set_volume_step(NULL, 5);
            check(audio_out_get_volume_step(NULL) == AUDIO_VOLUME_DEFAULT,
                  "V5d NULL stream: set is a no-op, get reads the default");
        }

        /* V6: volume_step is read from the config file at open. */
        check(vol_from_file("volume_step=7\n") == 7, "V6a volume_step=7 is read at open");
        check(vol_from_file("eq=0,0,0,0,0\nvolume_step=0\n") == 0, "V6b volume_step=0 (mute) is read");
        check(vol_from_file("volume_step=99\n") == AUDIO_VOLUME_MAX, "V6c an out-of-range key clamps to 14");
        check(vol_from_file("eq=0,0,0,0,0\n") == AUDIO_VOLUME_DEFAULT, "V6d key absent: default 10");
        check(vol_from_file("volume_step=loud\n") == AUDIO_VOLUME_DEFAULT, "V6e garbled key: default 10");
        check(vol_from_file(NULL) == AUDIO_VOLUME_DEFAULT, "V6f empty file: default 10");
    }

    printf("\n%s  %d checks, %d failure(s)\n",
           failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
