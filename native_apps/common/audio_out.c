#include "audio_out.h"
#include "audio_eq.h"
#include "audio_enh.h"
#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>

/* ── The device half, and nothing else ───────────────────────────────────────
 *
 * Every frame count, byte count, envelope and write decision in here comes from
 * `audio_gen.c`, which has no device in it and is host-tested.  What is left is
 * the four things that genuinely need a device — configure-and-read-back, how
 * much room the queue has, the write, and the drain — behind a vtable so a host
 * regression can drive all of it with no fd.
 *
 * ⚠️ **There is no ring-reset ioctl anywhere below this line, and that is the
 * fix.** The steady-state count must be exactly zero, which is a grep-checkable
 * number rather than a rule to remember; the pattern is in
 * `tests/audio_out_test.c`'s header, written so that it cannot match its own
 * documentation.
 */

/* ── Write policies: two, over the one loop in audio_gen.c ───────────────────
 *
 * `audio_write_frames()` is still the only code that decides when to stop, and it
 * stops on a frame boundary or reports that it could not.  These are POLICIES over
 * it, not loops — the rule that four hand-rolled EAGAIN loops were collapsed into
 * (`native_apps/CLAUDE.md`), and it now has to hold across this library's clients
 * as well as inside one directory, or an emulator port satisfies it while adding
 * two loops in a second file.
 */

/** ⚠️ **The prefill BLOCKS and must still be BOUNDED.** It is the one call in
 *  this file allowed to sleep, and an unlimited `max_waits` against a device that
 *  will not take the prefill hangs `audio_out_open()` forever — measured, by
 *  `tests/audio_out_test.c` group D hanging the suite the first time it drove a
 *  device with a byte budget.  So its bound is derived per call from the length
 *  of what it is writing, exactly like mode 2's: see `blocking_policy()`. */

/** ⚠️ **The serviced policy's wait is 0, and that is load-bearing.** It is called
 *  from a render loop or an audio thread that believes the call is free, so it may
 *  not block — and `stop_on_again` alone does not achieve that: mid-frame
 *  `audio_write_frames()` ignores the stop condition and waits up to
 *  AUDIO_ALIGN_TRIES times, which at a 1000 µs interval is 4 ms of sleep inside a
 *  function documented as never sleeping.  `dsp_wait`-style waits already no-op at
 *  `usec <= 0`, so 0 makes the realignment a spin of four retries instead. */
static const AudioWritePolicy AOPOL_SERVICE = { 0, 0, true };

/* Mode 2's policy is built per call, because its bound is derived from the
 * length of what it is being asked to write — see `sync_policy()`. */

/* ── One per process ─────────────────────────────────────────────────────────
 *
 * ⚠️ A second concurrent open is refused *Device or resource busy* by the driver
 * itself (measured, ../SYSTEM_ANALYSIS.md#34-audio).  Catching it here turns a
 * confusing half-finished init into a named refusal, and makes the rule the
 * header states testable on the host.  Sequential open/close pairs are fine.
 */
static int g_live = 0;
static const char *eq_path = NULL;   /* NULL = CONFIG_FILE_PATH */

void audio_out_set_eq_path(const char *path) { eq_path = path; }

/* ── Small helpers ──────────────────────────────────────────────────────── */

static ssize_t out_sink_write(void *ctx, const void *buf, size_t nbytes, bool *again)
{
    AudioOut *out = (AudioOut *)ctx;
    ssize_t r = out->dev->write(out->dev_ctx, buf, nbytes, again);
    /* ⚠️ Read errno HERE, immediately after the backend returned: push() only
     * learns "sink_error" after audio_write_frames() has possibly made more calls
     * and push() itself has printed, and either can overwrite it.  One check in
     * the generic layer covers every backend, including the host test's fake. */
    if (r < 0 && !*again && errno == ENODEV) out->device_lost = true;
    return r;
}

static void out_sink_wait(void *ctx, int usec)
{
    AudioOut *out = (AudioOut *)ctx;
    if (out->dev->wait) out->dev->wait(out->dev_ctx, usec);
}

static AudioSink sink_of(AudioOut *out)
{
    AudioSink s;
    s.write = out_sink_write;
    s.wait  = out_sink_wait;
    s.ctx   = out;
    return s;
}

/** Interleaved device scratch, grown once and kept.  A service runs every frame,
 *  so a malloc/free pair per call is the one allocation worth removing. */
static int16_t *scratch(AudioOut *out, long frames)
{
    if (frames <= 0 || out->channels <= 0) return NULL;
    if (out->buf && out->buf_frames >= frames) return out->buf;

    size_t n = (size_t)frames * (size_t)out->channels * sizeof(int16_t);
    int16_t *nb = (int16_t *)realloc(out->buf, n);
    if (!nb) return NULL;
    out->buf        = nb;
    out->buf_frames = frames;
    return nb;
}

/** How many frames of the in-flight report are the device over-stating.  Used
 *  only where a nominal figure would otherwise be presented as real audio: the
 *  service-interval ceiling and the drain's stop condition. */
static long ospace_slack(long period_frames)
{
    if (period_frames <= 0) return 0;
    return period_frames * AUDIO_OUT_OSPACE_SLACK_NUM / AUDIO_OUT_OSPACE_SLACK_DEN;
}

/**
 * Waits a blocking policy needs to see `frames` all the way out.
 *
 * Derived rather than constant: the device drains at hardware rate, so a buffer
 * longer than the ring needs about its own duration in waits.  A fixed bound would
 * truncate a long tone, and an unlimited one would hang a UI on a wedged device —
 * `AUDIO_MAX_TONE_MS` is 30 s, so both failures are reachable from one call site.
 */
static int derive_max_waits(int rate, long frames, int wait_us)
{
    long per_wait_ms = wait_us / 1000;
    if (per_wait_ms < 1) per_wait_ms = 1;

    long ms = audio_ms_for_frames(rate, frames);
    if (ms < 0) ms = 0;

    long n = ms / per_wait_ms + AUDIO_OUT_SYNC_WAIT_FLOOR;
    if (n > 100000) n = 100000;          /* a wedged device, not a long tone */
    return (int)n;
}

static AudioWritePolicy blocking_policy(int rate, long frames, int wait_us)
{
    AudioWritePolicy p;
    p.wait_us       = wait_us;
    p.max_waits     = derive_max_waits(rate, frames, wait_us);
    p.stop_on_again = false;
    return p;
}

/** The requested lead in ms; 0 = AUDIO_PUMP_LEAD_MS.  Process-wide on purpose:
 *  audio_out_open() zeroes the struct, one AudioOut is live per process, and
 *  audio_init() opens it internally, so an app has no handle to set a field on
 *  before the prefill sizes itself.  See audio_out_set_lead_ms(). */
static int g_lead_ms_req = 0;

void audio_out_set_lead_ms(int ms)
{
    g_lead_ms_req = (ms > 0) ? ms : 0;
}

/** The lead is a MEASUREMENT taken from the device's own period, so it is
 *  re-derived from every geometry read rather than cached from the request. */
static void derive_geometry(AudioOut *out, const AudioOutSpace *sp)
{
    int lead_ms = (g_lead_ms_req > 0) ? g_lead_ms_req : AUDIO_PUMP_LEAD_MS;
    out->period_frames = sp->period_frames;
    out->ring_frames   = sp->ring_frames;
    out->lead_frames   = audio_pump_lead_frames(
                             audio_frames_for_ms(out->rate, lead_ms),
                             sp->period_frames, AUDIO_PUMP_LEAD_PERIODS,
                             sp->ring_frames);
}

/** True when every sample is zero.  An OR-reduction rather than a flag from the
 *  fill: the fill is one opaque callback shared with ScummVM, and the sum over
 *  ~12k int16 costs a fraction of one biquad stage, so no new contract is needed. */
static bool all_silent(const int16_t *buf, long samples)
{
    int16_t acc = 0;
    for (long i = 0; i < samples; i++) acc |= buf[i];
    return acc == 0;
}

/** The device attenuation stage — see audio_attenuate() in audio_gen.c, which is
 *  the one implementation of it and the reason a shift rather than a multiply.
 *
 *  ⚠️ The stream is never idle (an idle one underruns), so a silent app still
 *  feeds it a lead of zeros every frame, and the equaliser and enhancer over
 *  those zeros was ~11 % CPU on a stopped player.  Zeros in give zeros out once
 *  the filters have rung down, so after SILENT_HANG_MS of consecutive all-zero
 *  buffers the DSP is skipped.  300 ms is many time constants of the slowest
 *  stage (the low shelf and the bass section settle in tens of ms), and the state
 *  is RESET on entering the skip, so what the first sound afterwards meets is a
 *  fresh filter and never a stale tail.  The master shift of zeros is zeros, so
 *  the output is unchanged by the skip. */
#define SILENT_HANG_MS 300

static void attenuate(AudioOut *out, int16_t *buf, long samples)
{
    long frames = (out->channels > 0) ? samples / out->channels : 0;

    if (all_silent(buf, samples)) {
        /* Tested BEFORE adding this buffer, so at least SILENT_HANG_MS of zeros
         * have gone through the filters by the time one is skipped. */
        if (out->silent_frames >= audio_frames_for_ms(out->rate, SILENT_HANG_MS)) {
            if (!out->dsp_idle) {
                eq_state_reset(&out->eq_s);
                enh_state_reset(&out->enh_s);
                out->dsp_idle = true;
            }
            return;     /* the buffer is already zeros, and so is the shifted one */
        }
        if (out->silent_frames < LONG_MAX - frames) out->silent_frames += frames;
    } else {
        out->silent_frames = 0;
        out->dsp_idle      = false;
    }

    /* The equaliser first, then the enhancer, on the full-scale data; the master shift after. */
    if (out->eq_p.n > 0 && out->channels > 0)
        eq_process(&out->eq_s, &out->eq_p, buf, frames, out->channels);
    if (out->enh_p.active && out->channels > 0)
        enh_process(&out->enh_s, &out->enh_p, buf, frames, out->channels);
    audio_attenuate(buf, samples, out->shift);
}

/** One write of `frames` interleaved frames under `pol`, with the two faults
 *  that are never ignorable counted rather than returned. */
static long push(AudioOut *out, const int16_t *buf, long frames,
                 const AudioWritePolicy *pol)
{
    AudioSink sink = sink_of(out);
    AudioWriteResult res;
    audio_write_frames(&sink, buf, frames, out->channels, pol, &res);

    if (res.misaligned) {
        out->misaligned++;
        fprintf(stderr, "audio_out: a partial frame is in the device "
                        "(%ld of %ld bytes) — channels may be swapped\n",
                res.bytes_written, audio_bytes_for_frames(frames, out->channels));
    }
    if (res.sink_error) out->sink_errors++;

    out->last_frames = res.frames_written;
    if (res.frames_written < frames)
        out->lost += (uint32_t)(frames - res.frames_written);
    return res.frames_written;
}

/* ── Open ───────────────────────────────────────────────────────────────── */

int audio_out_open(AudioOut *out, const AudioOutDev *dev, void *dev_ctx,
                   int rate_req, int channels_req)
{
    if (!out || !dev || !dev->open || !dev->space || !dev->write || !dev->close)
        return -1;

    /* Zero first, so a caller that ignores the return value still holds a struct
     * every entry point reads as closed. */
    memset(out, 0, sizeof(*out));

    if (g_live > 0) {
        fprintf(stderr, "audio_out: refused — one AudioOut is already open in "
                        "this process (a second concurrent open is EBUSY)\n");
        return -1;
    }

    out->dev     = dev;
    out->dev_ctx = dev_ctx;

    int rate = 0, bits = 0, channels = 0;
    if (dev->open(dev_ctx, rate_req, channels_req, &rate, &bits, &channels) != 0) {
        out->dev = NULL;
        return -1;
    }

    /* ⚠️ The GRANT is the only number the byte arithmetic may use.  A struct
     * filled from the request instead is the failure mode this library exists to
     * remove: a 0-channel byte count is 0, i.e. silently mute. */
    out->rate     = (rate > 0) ? rate : rate_req;
    out->bits     = bits;
    out->channels = channels;

    out->frame_bytes = audio_frame_bytes(out->channels);
    if (out->frame_bytes <= 0 || out->rate <= 0) {
        fprintf(stderr, "audio_out: device granted rate=%d channels=%d — unusable\n",
                out->rate, out->channels);
        dev->close(dev_ctx);
        out->dev = NULL;
        return -1;
    }

    /* Reading the width back closes a hole `audio.c` had and `oss-mixer.cpp`
     * already warned about: a device that quietly granted another width produces
     * noise with no diagnostic anywhere.  Warned, not refused — 16 is what every
     * measured configuration grants, so an unexpected width is a surprise to
     * report rather than a reason to leave the panel silent. */
    if (out->bits != AUDIO_BYTES_PER_SAMPLE * 8) {
        out->bits_warned = true;
        fprintf(stderr, "audio_out: device granted %d-bit samples, expected %d — "
                        "the stream will be written as S16_LE anyway\n",
                out->bits, AUDIO_BYTES_PER_SAMPLE * 8);
    }

    /* The equaliser curve for this stream, from the config key a settings change
     * wrote; designed for the GRANTED rate, never the requested one. */
    {
        int g[EQ_BANDS];
        eq_config_read(eq_path ? eq_path : CONFIG_FILE_PATH, g);
        eq_design(&out->eq_p, out->rate, g);
        AudioEnh en;
        enh_config_read(eq_path ? eq_path : CONFIG_FILE_PATH, &en);
        enh_design(&out->enh_p, out->rate, &en);
        if (out->enh_p.active) {
            char ev[64];
            audio_enh_format(&en, ev, sizeof(ev));
            fprintf(stderr, "audio_out: enhancer on (%s) at %d Hz\n", ev, out->rate);
        }
    }

    out->is_open = true;
    g_live++;

    /* Geometry, then the prefill.  Both need the grant, which is why neither can
     * be done before this point. */
    AudioOutSpace sp;
    memset(&sp, 0, sizeof(sp));
    if (dev->space(dev_ctx, out->frame_bytes, &sp) == 0)
        derive_geometry(out, &sp);

    if (out->lead_frames > 0) {
        int16_t *buf = scratch(out, out->lead_frames);
        if (buf) {
            memset(buf, 0, (size_t)out->lead_frames * (size_t)out->channels
                            * sizeof(int16_t));
            AudioWritePolicy pol = blocking_policy(out->rate, out->lead_frames,
                                                   AUDIO_OUT_PREFILL_WAIT_US);
            push(out, buf, out->lead_frames, &pol);
        }
        /* ⚠️ The prefill's frames are not "lost" audio — they are the silence the
         * stream is meant to start with — so do not let push()'s accounting read
         * as a defect on the very first write. */
        out->lost        = 0;
        out->last_frames = 0;
    }

    return 0;
}

/* ── Close, with a bounded drain ────────────────────────────────────────── */

void audio_out_close(AudioOut *out)
{
    if (!out) return;

    if (out->is_open && out->dev) {
        /* ⚠️ Drain, or the queued tail is discarded at exit — which on the two
         * Settings tabs is most of the tone they just played, because nothing
         * services them and the whole tone is still inside the device.
         *
         * The stop condition is the free-space slack rather than zero: the
         * device's in-flight figure over-reports by ~1.3 periods, so it never
         * reaches zero and a wait-for-zero loop would always run to its bound.
         * The bound itself is the ring's own duration plus a period — the queue
         * cannot be longer than the ring, so anything past that is a wedged
         * device, not a tail. */
        long budget_ms = audio_ms_for_frames(out->rate,
                                             out->ring_frames + out->period_frames);
        long max_polls = (budget_ms > 0)
                         ? (budget_ms * 1000) / AUDIO_OUT_DRAIN_WAIT_US + 1
                         : 0;
        long floor_frames = ospace_slack(out->period_frames);

        out->drain_waits = 0;
        for (long i = 0; i < max_polls; i++) {
            AudioOutSpace sp;
            memset(&sp, 0, sizeof(sp));
            if (out->dev->space(out->dev_ctx, out->frame_bytes, &sp) != 0) break;
            if (sp.in_flight <= floor_frames) break;
            out->drain_waits++;
            if (out->dev->wait) out->dev->wait(out->dev_ctx, AUDIO_OUT_DRAIN_WAIT_US);
        }

        out->dev->close(out->dev_ctx);
        g_live--;
        if (g_live < 0) g_live = 0;
    }

    free(out->buf);
    out->buf        = NULL;
    out->buf_frames = 0;
    out->is_open    = false;
    out->fill       = NULL;
    out->fill_ctx   = NULL;
    out->fill_owner = NULL;
    out->dev        = NULL;
    out->dev_ctx    = NULL;
}

/* ── The one fill callback ──────────────────────────────────────────────── */

int audio_out_set_fill(AudioOut *out, AudioOutFill fill, void *ctx,
                       const char *owner)
{
    if (!out || !out->is_open) return -1;
    out->fill       = fill;
    out->fill_ctx   = ctx;
    out->fill_owner = fill ? (owner ? owner : "unnamed") : NULL;
    return 0;
}

const char *audio_out_fill_owner(const AudioOut *out)
{
    return (out && out->fill) ? out->fill_owner : NULL;
}

void audio_out_set_shift(AudioOut *out, int shift)
{
    if (!out) return;
    if (shift < 0)  shift = 0;
    if (shift > 15) shift = 15;
    out->shift = shift;
}

void audio_out_set_eq(AudioOut *out, const int gains_db[EQ_BANDS])
{
    if (!out || !gains_db) return;
    /* Design into a local first so the live params change in one struct copy
     * between two buffers; the history stays. */
    EqParams np;
    eq_design(&np, out->rate, gains_db);
    out->eq_p = np;
}

void audio_out_get_eq(const AudioOut *out, int gains_db[EQ_BANDS])
{
    for (int b = 0; b < EQ_BANDS; b++)
        gains_db[b] = out ? out->eq_p.gains_db[b] : 0;
}

void audio_out_set_enh(AudioOut *out, const AudioEnh *e)
{
    if (!out || !e) return;
    EnhParams np;
    enh_design(&np, out->rate, e);
    out->enh_p = np;
}

void audio_out_get_enh(const AudioOut *out, AudioEnh *e)
{
    if (!e) return;
    memset(e, 0, sizeof(*e));
    if (out) *e = out->enh_p.cfg;
}

/* ── Mode 1: serviced ───────────────────────────────────────────────────── */

long audio_out_service(AudioOut *out)
{
    if (!out || !out->is_open || !out->dev) return -1;

    AudioOutSpace sp;
    memset(&sp, 0, sizeof(sp));
    if (out->dev->space(out->dev_ctx, out->frame_bytes, &sp) != 0) {
        /* A gone device usually fails HERE first — the free-space query precedes
         * every write — and a service that never reaches a write would otherwise
         * never learn the device is lost. */
        if (errno == ENODEV) out->device_lost = true;
        out->refused++;
        return -1;
    }
    out->services++;
    derive_geometry(out, &sp);

    /* ⚠️ On a stream that is never allowed to go idle, a dry queue is ALWAYS a
     * fault — one audible gap each — so this is unconditional.  It is also the
     * number that separates "the mixer is wrong" from "the loop that feeds it was
     * too slow", which is a distinction no derived figure can make. */
    if (sp.in_flight <= 0) out->starved++;

    long want = audio_pump_frames(out->lead_frames, sp.in_flight, sp.space,
                                  out->lead_frames);
    if (want <= 0) {
        out->last_frames = 0;
        return 0;
    }

    int16_t *buf = scratch(out, want);
    if (!buf) {
        out->refused++;
        return -1;
    }

    /* ⚠️ Zeroed before every fill.  A short fill is legal and `audio_mix_render()`
     * deliberately does not touch the buffer on a silent bus, so without this the
     * previous service's samples would be re-written as this one's tail. */
    long samples = want * (long)out->channels;
    memset(buf, 0, (size_t)samples * sizeof(int16_t));

    if (out->fill)
        out->fill(out->fill_ctx, buf, want, out->channels);

    attenuate(out, buf, samples);

    return push(out, buf, want, &AOPOL_SERVICE);
}

/* ── Mode 2: synchronous ────────────────────────────────────────────────── */

long audio_out_write(AudioOut *out, const int16_t *mono, long frames)
{
    if (!out || !out->is_open || !out->dev) return -1;
    if (!mono || frames <= 0) return -1;

    /* ⚠️ Refused LOUDLY.  A quiet refusal here is the failure the one-callback
     * design exists to prevent: two writers interleaving frames into a stream
     * that has no mono path underneath to absorb a swap. */
    if (out->fill) {
        out->refused++;
        fprintf(stderr, "audio_out: synchronous write refused — \"%s\" owns the "
                        "fill callback (remove it first)\n",
                out->fill_owner ? out->fill_owner : "a client");
        return -1;
    }

    int16_t *buf = scratch(out, frames);
    if (!buf) return -1;

    long bytes = audio_bytes_for_frames(frames, out->channels);
    if (bytes <= 0 || audio_interleave(mono, frames, out->channels, buf) != bytes)
        return -1;

    attenuate(out, buf, frames * (long)out->channels);

    AudioWritePolicy pol = blocking_policy(out->rate, frames, AUDIO_OUT_SYNC_WAIT_US);
    return push(out, buf, frames, &pol);
}

/* ── The service ceiling ────────────────────────────────────────────────── */

long audio_out_service_interval_us(const AudioOut *out)
{
    if (!out || out->lead_frames <= 0 || out->rate <= 0) return 0;

    /* Real audio, not the nominal lead: subtract what the device over-states. */
    long real_frames = out->lead_frames - ospace_slack(out->period_frames);
    if (real_frames <= 0) return 0;

    /* Half a period of margin.  The measured sweep left ~11 ms at a 66 ms
     * interval, so the margin is what keeps the published figure inside the
     * region that was seen working rather than at its edge. */
    long usable = real_frames - out->period_frames / 2;
    if (usable <= 0) usable = real_frames;

    long ms = audio_ms_for_frames(out->rate, usable);
    return (ms > 0) ? ms * 1000 : 0;
}

/* ── Accessors ──────────────────────────────────────────────────────────── */

int  audio_out_rate(const AudioOut *out)        { return out ? out->rate     : 0; }
int  audio_out_channels(const AudioOut *out)    { return out ? out->channels : 0; }
int  audio_out_bits(const AudioOut *out)        { return out ? out->bits     : 0; }
bool audio_out_is_open(const AudioOut *out)     { return out ? out->is_open  : false; }

long audio_out_lead(const AudioOut *out)        { return out ? out->lead_frames   : 0; }
long audio_out_period(const AudioOut *out)      { return out ? out->period_frames : 0; }
long audio_out_last_frames(const AudioOut *out) { return out ? out->last_frames   : 0; }

uint32_t audio_out_starved(const AudioOut *out)     { return out ? out->starved     : 0; }
uint32_t audio_out_lost(const AudioOut *out)        { return out ? out->lost        : 0; }
uint32_t audio_out_misaligned(const AudioOut *out)  { return out ? out->misaligned  : 0; }
uint32_t audio_out_sink_errors(const AudioOut *out) { return out ? out->sink_errors : 0; }
uint32_t audio_out_refused(const AudioOut *out)     { return out ? out->refused     : 0; }
uint32_t audio_out_services(const AudioOut *out)    { return out ? out->services    : 0; }
uint32_t audio_out_drain_waits(const AudioOut *out) { return out ? out->drain_waits : 0; }
bool audio_out_device_lost(const AudioOut *out)
{
    return out && out->is_open && out->device_lost;
}

/* ── What a failed ALSA call means ───────────────────────────────────────────
 * Pure, and outside every guard, so the host test reaches the unplug branch
 * with no libasound.  Accepts either sign; ALSA hands back negative errnos. */
AudioOutErr audio_out_alsa_classify(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case EAGAIN:   return AO_ERR_AGAIN;
    case EPIPE:    return AO_ERR_XRUN;
    case ESTRPIPE: return AO_ERR_SUSPEND;
    /* EBADFD is the state a PCM is left in once its card has gone — every call
     * after the first ENODEV answers it, so it is the same "reopen" case.
     * ENOTCONN and ESHUTDOWN are the socket-shaped forms of the same loss: a
     * BlueALSA PCM is a socket to bluealsad, and which errno a dropped A2DP
     * transport surfaces as is not measured — so every plausible one reopens. */
    case ENODEV:
    case EBADFD:
    case ENOTCONN:
    case ESHUTDOWN: return AO_ERR_LOST;
    default:       return AO_ERR_OTHER;
    }
}

/* ── Which device, and the amp that belongs to one of them ──────────────────
 * ONE home for "which /dev/dsp*", and it sits OUTSIDE the ALSA guard below on
 * purpose.  Three callers resolve through here — this file's opener,
 * audio.c (which sets the preference and logs the path), and ScummVM's mixer —
 * and the ALSA backend is the only part inside that guard, so a host build with
 * no libasound must still link the resolution.  That is also what lets
 * tests/audio_out_test.c drive it with no sound card present.  The OSS nodes
 * are still what is resolved: /dev/dspN existing is how a card is seen, and
 * audio_out_device_pcm() maps the node to its ALSA PCM.
 *
 * ⚠️ The preference is a file-static, not a field on AudioOut.  audio.c sets
 * it before audio_open(), and audio_open() memsets the struct — a field would
 * be cleared by the very call that needs to read it.  One audio device per
 * process is a fact about the hardware, so one static is the honest shape.
 */

#define AUDIO_DEV_ONBOARD "/dev/dsp"      /* TWL4030, the panel speaker */
#define AUDIO_DEV_USB     "/dev/dsp1"     /* ALSA card 1, a USB DAC     */
/* Not a node: the path-space name of the Bluetooth A2DP sink, which BlueALSA
 * serves as an ALSA PCM with no OSS node behind it.  audio_out_device_pcm()
 * maps it; nothing ever open()s it. */
#define AUDIO_DEV_BT      "bluealsa"
/* The pinned headset, when one is set: ranked above AUDIO_DEV_BT, which is then
 * "any other connected sink".  Also a name, not a node. */
#define AUDIO_DEV_BT_PIN  "bluealsa-pin"
#define AUDIO_BT_PCM      "plug:bluealsa"
#define AUDIO_BT_SYSFS    "/sys/class/bluetooth"

/** "onboard" | "usb" | "bluetooth" | "auto".  Defaults to auto: a Bluetooth sink
 *  when one is connected, else a USB DAC when one is plugged in, else the panel
 *  speaker. */
static char audio_dev_pref[16] = "auto";

/* The pinned headset (audio_out.h, audio_out_set_bt_addr) and the PCM name it
 * makes, "" while no pin is in force.  bt_pin_pcm is rebuilt by the two setters
 * ONLY: g_alsa.name keeps a pointer to it for the life of a stream, so a lazy
 * rebuild per call would rename a stream out from under its own "is this our
 * stream" check. */
static char audio_bt_addr[18] = "";
static char bt_pin_pcm[64]    = "";
static void bt_pcm_rebuild(void);

void audio_out_set_device_pref(const char *pref)
{
    if (!pref || !*pref) pref = "auto";
    snprintf(audio_dev_pref, sizeof(audio_dev_pref), "%s", pref);
    bt_pcm_rebuild();
}

bool audio_out_bt_addr_valid(const char *addr)
{
    if (!addr) return false;
    /* `pos` counts 0,1,2 per octet: no `%` on a core with no divide. */
    for (int i = 0, pos = 0; i < 17; i++, pos = (pos == 2) ? 0 : pos + 1) {
        char c = addr[i];
        if (pos == 2) { if (c != ':') return false; continue; }
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f'))) return false;
    }
    return addr[17] == '\0';
}

const char *audio_out_bt_pcm_for(const char *pref, const char *addr,
                                 char *buf, size_t n)
{
    /* The pin is a PREFERENCE inside the Bluetooth tier, in force under both
     * preferences that can pick Bluetooth ("bluetooth" and "auto"); it narrows
     * nothing, because AUDIO_DEV_BT (any connected sink) still ranks below it.
     * The address is validated, not just non-empty, because it is spliced into
     * an ALSA device string whose ',' and '=' are syntax.
     *
     * ⚠️ `bluealsa:DEV=…`, NOT `plug:bluealsa:DEV=…`: pcm.bluealsa is already
     * `type plug` (device-files/20-bluealsa.conf), and the outer form would hand
     * "bluealsa:DEV=…,PROFILE=a2dp" to pcm.plug's argument parser, which splits
     * at '=' and ',' (alsa-lib conf.c parse_args) and rejects the result. */
    bool pin = pref && (strcmp(pref, "bluetooth") == 0 || strcmp(pref, "auto") == 0) &&
               audio_out_bt_addr_valid(addr);
    if (!pin || !buf || n == 0) return AUDIO_BT_PCM;
    int w = snprintf(buf, n, "bluealsa:DEV=%s,PROFILE=a2dp", addr);
    return (w > 0 && (size_t)w < n) ? buf : AUDIO_BT_PCM;
}

const char *audio_out_device_pref(void)
{
    return audio_dev_pref;
}

bool audio_out_usb_present(void)
{
    return access(AUDIO_DEV_USB, W_OK) == 0;
}

/** Say a fallback from an explicit request once per process, not per resolve. */
static void say_fallback_once(bool *said, const char *wanted, const char *used)
{
    if (*said) return;
    fprintf(stderr, "audio_out: %s requested but absent — using %s\n", wanted, used);
    *said = true;
}

const char *audio_out_device_for(const char *pref, bool usb_present, bool bt_present)
{
    return audio_out_device_for_bt(pref, usb_present, false, bt_present);
}

const char *audio_out_device_for_bt(const char *pref, bool usb_present,
                                    bool bt_pin_present, bool bt_any_present)
{
    bool want_usb = (pref && strcmp(pref, "usb")       == 0);
    bool want_bt  = (pref && strcmp(pref, "bluetooth") == 0);
    bool prefer   = (pref && strcmp(pref, "auto")      == 0);

    if (!want_usb && !want_bt && !prefer) return AUDIO_DEV_ONBOARD;
    /* The Bluetooth tier, in its own order: the pinned headset, then any other
     * connected sink. */
    if ((want_bt || prefer) && bt_pin_present) return AUDIO_DEV_BT_PIN;
    if ((want_bt || prefer) && bt_any_present) return AUDIO_DEV_BT;
    const char *rest = usb_present ? AUDIO_DEV_USB : AUDIO_DEV_ONBOARD;

    /* ⚠️ Every non-onboard setting falls back rather than opening a sink that is
     * not there: a games panel that has gone mute with no explanation is worse
     * than one on the wrong speaker.  "auto" means unplugging is expected and
     * falls back silently; "usb" and "bluetooth" were explicit requests that
     * could not be honoured, so each says so once.  "bluetooth" falls back the
     * way "auto" does once Bluetooth is out of the running: USB, else onboard. */
    static bool said_usb = false, said_bt = false;
    if (want_usb && !usb_present) say_fallback_once(&said_usb, AUDIO_DEV_USB, rest);
    if (want_bt)                  say_fallback_once(&said_bt, AUDIO_BT_PCM, rest);
    return rest;
}

/** Position in the one order every preference is a subsequence of. */
static int sink_rank(const char *path)
{
    if (!path) return -1;
    if (strcmp(path, AUDIO_DEV_BT_PIN) == 0) return 3;
    if (strcmp(path, AUDIO_DEV_BT)  == 0) return 2;
    if (strcmp(path, AUDIO_DEV_USB) == 0) return 1;
    return 0;
}

bool audio_out_sink_better(const char *candidate, const char *open_path)
{
    if (!candidate || !open_path) return false;
    return sink_rank(candidate) > sink_rank(open_path);
}

/* The settings page's list (audio_out.h).  An entry is listed only while the
 * hardware it needs is attached: `needs_usb` / `needs_bt`. */
static const struct {
    const char *name;
    const char *label;
    bool        needs_usb;
    bool        needs_bt;
} audio_out_choices[AUDIO_OUT_CHOICE_COUNT] = {
    [AUDIO_OUT_CHOICE_ONBOARD] = { "onboard",   "ONBOARD",   false, false },
    [AUDIO_OUT_CHOICE_USB]     = { "usb",       "USB",       true,  false },
    [AUDIO_OUT_CHOICE_BT]      = { "bluetooth", "BLUETOOTH", false, true  },
    [AUDIO_OUT_CHOICE_AUTO]    = { "auto",      "AUTO",      false, false },
};

static int choice_valid(int c)
{
    return (c >= 0 && c < AUDIO_OUT_CHOICE_COUNT) ? c : AUDIO_OUT_CHOICE_ONBOARD;
}

const char *audio_out_choice_name(int choice)
{
    return audio_out_choices[choice_valid(choice)].name;
}

const char *audio_out_choice_label(int choice)
{
    return audio_out_choices[choice_valid(choice)].label;
}

int audio_out_choice_of(const char *name)
{
    for (int c = 0; name && c < AUDIO_OUT_CHOICE_COUNT; c++)
        if (strcmp(name, audio_out_choices[c].name) == 0) return c;
    return AUDIO_OUT_CHOICE_ONBOARD;
}

bool audio_out_choice_available(int choice, bool usb_present, bool bt_present)
{
    if (choice < 0 || choice >= AUDIO_OUT_CHOICE_COUNT) return false;
    return (!audio_out_choices[choice].needs_usb || usb_present) &&
           (!audio_out_choices[choice].needs_bt  || bt_present);
}

int audio_out_choice_shown(int saved, bool usb_present, bool bt_present)
{
    if (saved < 0 || saved >= AUDIO_OUT_CHOICE_COUNT) return AUDIO_OUT_CHOICE_ONBOARD;
    return audio_out_choice_available(saved, usb_present, bt_present)
               ? saved : AUDIO_OUT_CHOICE_AUTO;
}

int audio_out_choice_next(int shown, bool usb_present, bool bt_present)
{
    int c = choice_valid(shown);
    /* A wrap by comparison, not `%`: this file runs on a core with no divide. */
    for (int i = 0; i < AUDIO_OUT_CHOICE_COUNT; i++) {
        if (++c == AUDIO_OUT_CHOICE_COUNT) c = 0;
        if (audio_out_choice_available(c, usb_present, bt_present)) return c;
    }
    return AUDIO_OUT_CHOICE_AUTO;   /* unreachable while AUTO needs nothing */
}

/** Card 1 was present and would not open — see audio_out_open_resolved().
 *  Process-global for the preference's reason: one audio device per process. */
static bool usb_refused = false;

/** Is card 1 there AND usable?  Seeing it absent clears a refusal, so the next
 *  plug is a fresh card. */
static bool usb_usable(void)
{
    bool present = audio_out_usb_present();
    if (!present) usb_refused = false;
    return present && !usb_refused;
}

/* ── Bluetooth presence (audio_out.h has the rule) ───────────────────────── */

uint32_t audio_out_bt_acl_signature(const char *sysfs_dir)
{
    DIR *d = sysfs_dir ? opendir(sysfs_dir) : NULL;
    if (!d) return 0;
    uint32_t sum = 0, mix = 0, n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* One `hciN:H` entry per ACL link on 4.14; `hciN` alone is the controller. */
        if (strncmp(e->d_name, "hci", 3) != 0 || !strchr(e->d_name, ':')) continue;
        uint32_t h = 2166136261u;                       /* FNV-1a */
        for (const char *p = e->d_name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        sum += h;                                       /* readdir order is not ours: */
        mix ^= h * 2654435761u;                         /* combine commutatively      */
        n++;
    }
    closedir(d);
    if (n == 0) return 0;
    uint32_t sig = (sum ^ (mix << 7) ^ (mix >> 25)) + n;
    return sig ? sig : 1;                               /* 0 is reserved for "none" */
}

bool audio_out_bt_probe_due(AudioOutBtGate *g, uint32_t sig, uint32_t now_ms,
                            bool *changed)
{
    if (changed) *changed = false;
    if (!g) return false;

    if (!g->have_sig || sig != g->sig) {
        bool first = !g->have_sig;
        g->have_sig = true;
        g->sig      = sig;
        g->window   = false;
        if (changed) *changed = true;
        if (sig == 0) return false;          /* no link: nothing a probe could find */
        /* The first look probes once and opens no window: a link that predates
         * the process is the keyboard nobody is connecting right now. */
        if (!first) {
            g->window          = true;
            g->window_start_ms = now_ms;
        }
        g->last_probe_ms = now_ms;
        return true;
    }
    if (!g->window) return false;
    if ((uint32_t)(now_ms - g->window_start_ms) >= AUDIO_OUT_BT_WINDOW_MS) {
        g->window = false;
        return false;
    }
    if ((uint32_t)(now_ms - g->last_probe_ms) < AUDIO_OUT_BT_PROBE_MS) return false;
    g->last_probe_ms = now_ms;
    return true;
}

static uint32_t probe_now_ms(void);
/** The real probe: defined by the backend below (the host stub answers false). */
static bool bt_probe_pcm(const char *pcm);

/* Two answers per probe, one per Bluetooth tier, and a refusal for each: the
 * pinned headset (bt_pin_pcm) and any connected sink (plug:bluealsa). */
static AudioOutBtGate bt_gate;
static bool bt_pin_seen    = false;   /* the last probe's answers                   */
static bool bt_any_seen    = false;
static bool bt_pin_refused = false;   /* a sink that would not open: until a change */
static bool bt_any_refused = false;

/* The gate, then on a due probe the pinned name first.  "Any" is asked only
 * while the pin is absent, because only then is it consulted — and that keeps
 * the probe off plug:bluealsa while the pinned stream is live on what may be
 * the same transport.  Absent, the pinned DEV= name fails fast (ENODEV, 60 ms
 * measured on .188), so it costs one short open per probe inside the window. */
static void bt_refresh(void)
{
    bool changed = false;
    uint32_t sig = audio_out_bt_acl_signature(AUDIO_BT_SYSFS);
    bool due = audio_out_bt_probe_due(&bt_gate, sig, probe_now_ms(), &changed);
    if (changed) {
        bt_pin_refused = bt_any_refused = false;
        if (sig == 0) bt_pin_seen = bt_any_seen = false;
    }
    if (due) {
        bt_pin_seen = bt_pin_pcm[0] && bt_probe_pcm(bt_pin_pcm);
        bt_any_seen = bt_pin_seen || bt_probe_pcm(AUDIO_BT_PCM);
    }
}

bool audio_out_bt_present(void)
{
    bt_refresh();
    return bt_pin_seen || bt_any_seen;
}

void audio_out_set_bt_addr(const char *addr)
{
    snprintf(audio_bt_addr, sizeof(audio_bt_addr), "%s",
             audio_out_bt_addr_valid(addr) ? addr : "");
    bt_pcm_rebuild();
}

const char *audio_out_bt_addr(void)
{
    return audio_bt_addr;
}

/* A new name is a different sink: the cached presence answer and any refusal
 * were about the old one.  Forgetting the signature makes the next
 * audio_out_bt_present() a first look, which probes the new name once. */
static void bt_pcm_rebuild(void)
{
    char next[sizeof(bt_pin_pcm)];
    const char *name = audio_out_bt_pcm_for(audio_dev_pref, audio_bt_addr,
                                            next, sizeof(next));
    if (strcmp(name, AUDIO_BT_PCM) == 0) name = "";     /* no pin in force */
    if (strcmp(name, bt_pin_pcm) == 0) return;
    snprintf(bt_pin_pcm, sizeof(bt_pin_pcm), "%s", name);
    bt_gate.have_sig = false;
    bt_pin_seen    = bt_any_seen    = false;
    bt_pin_refused = bt_any_refused = false;
}

bool audio_out_bt_is_pinned(int saved_choice, const char *saved_addr,
                            const char *dev_addr)
{
    return (saved_choice == AUDIO_OUT_CHOICE_BT ||
            saved_choice == AUDIO_OUT_CHOICE_AUTO) &&
           audio_out_bt_addr_valid(saved_addr) &&
           audio_out_bt_addr_valid(dev_addr) &&
           strcasecmp(saved_addr, dev_addr) == 0;
}

const char *audio_out_device_path(void)
{
    /* Bluetooth is asked only under a preference that can pick it: the probe
     * can open a PCM, and "onboard" / "usb" would throw the answer away. */
    bool can_bt = strcmp(audio_dev_pref, "auto") == 0 ||
                  strcmp(audio_dev_pref, "bluetooth") == 0;
    bool usb = usb_usable();
    if (can_bt) bt_refresh();
    return audio_out_device_for_bt(audio_dev_pref, usb,
                                   can_bt && bt_pin_seen && !bt_pin_refused,
                                   can_bt && bt_any_seen && !bt_any_refused);
}

/* The ALSA name for the same device.  OSS minor N is ALSA card N here, because
 * the OSS nodes are the kernel's emulation over those very cards.  Only the
 * paths the resolver can return are named; anything else is onboard, the same
 * fallback it has. */
const char *audio_out_device_pcm(const char *path)
{
    if (path && strcmp(path, AUDIO_DEV_USB) == 0) return "plughw:1,0";
    if (path && strcmp(path, AUDIO_DEV_BT_PIN) == 0)
        return bt_pin_pcm[0] ? bt_pin_pcm : AUDIO_BT_PCM;
    if (path && strcmp(path, AUDIO_DEV_BT)  == 0) return AUDIO_BT_PCM;
    return "plughw:0,0";
}

bool audio_out_device_is_onboard(void)
{
    return strcmp(audio_out_device_path(), AUDIO_DEV_ONBOARD) == 0;
}

#define GPIO12_DIRECTION  "/sys/class/gpio/gpio12/direction"
#define GPIO12_VALUE      "/sys/class/gpio/gpio12/value"

void audio_out_enable_amp(void)
{
    FILE *f;
    f = fopen(GPIO12_DIRECTION, "w");
    if (f) { fputs("out", f); fclose(f); }
    f = fopen(GPIO12_VALUE, "w");
    if (f) { fputs("1",   f); fclose(f); }
}

/* ── The ALSA backend ────────────────────────────────────────────────────────
 *
 * The one device backend.  Compiled only with an explicit -DAUDIO_OUT_HAVE_ALSA,
 * which both device build paths pass together with libasound's include and link
 * flags.  Not detected with __has_include: a host that merely has libasound's
 * headers installed would start failing to link every host regression that
 * compiles this file.  Without the define the host gets a stub that refuses to
 * open, which is all the host regressions need — they drive audio_out_open()
 * through their own AudioOutDev.
 *
 * The context is one static, not a field on AudioOut: one stream per process is
 * already enforced (g_live), and a static keeps <alsa/asoundlib.h> out of the
 * header every client includes.
 */

/** HH:MM:SS local wall-clock time, for the lines that must be matched against
 *  /var/log/messages (syslog stamps local time); every other line is unstamped. */
static const char *wall_hms(char buf[9])
{
    time_t t = time(NULL);
    struct tm tm;
    tzset();    /* follow a zone change made by another process; see logger.c */
    if (!localtime_r(&t, &tm) || strftime(buf, 9, "%H:%M:%S", &tm) == 0)
        snprintf(buf, 9, "??:??:??");
    return buf;
}

#ifdef AUDIO_OUT_HAVE_ALSA

#include <alsa/asoundlib.h>

/* Requests, not demands: the grant is read back and used.  Forcing tiny periods
 * is what trades a jitter buffer for latency, and the pacing depends on the
 * jitter buffer: constraining the ring is what removed it when the kernel's OSS
 * emulation was the backend.  The values are what that OSS shim granted on its
 * own for every rate and channel count measured, 2048 x 16 (743 ms at 44100 —
 * NOT the ~506 ms this repo believed for months): the lead is three periods, and at
 * 1024 frames it fell to 92 ms and SameGame's slow frames starved the stream
 * 71 times in one session where the OSS lead of ~139 ms had covered them. */
#define ALSA_PERIOD_REQ   2048
#define ALSA_PERIODS_REQ  16

typedef struct {
    snd_pcm_t         *pcm;
    const char        *name;
    int                frame_bytes;
    snd_pcm_uframes_t  period;
    snd_pcm_uframes_t  buffer;
    bool               lost_said;
    unsigned           write_xruns;   /* underruns met by writei — counted nowhere else */
    unsigned           hard_fails;    /* consecutive failed space/write calls          */
    unsigned long      written;       /* frames writei accepted, this stream (wraps)   */
    unsigned long      consumed_last; /* written - in_flight at the last progress      */
    uint32_t           progress_ms;   /* monotonic ms of that progress                 */
    bool               progress_set;  /* consumed_last/progress_ms hold a baseline     */
    bool               stalled;       /* reported once; every later query is lost too  */
} AlsaCtx;

static AlsaCtx g_alsa;

/** Hand a dead stream up as ENODEV — the one errno the generic layer turns into
 *  audio_out_device_lost(), which is what makes the owner reopen. */
static int alsa_gone(AlsaCtx *a, int err)
{
    if (!a->lost_said) {
        char hms[9];
        fprintf(stderr, "audio_out: %s is gone (%s) at %s\n", a->name, snd_strerror(err),
                wall_hms(hms));
        a->lost_said = true;
    }
    errno = ENODEV;
    return -1;
}

/** An XRUN or a suspend is recovered in place; a lost device is reported once
 *  and handed up as ENODEV.  Returns 0 if the caller may retry.
 *
 *  ⚠️ A recovery that itself FAILS is a lost device too, not an error to count.
 *  On a card the stub and the measurements agree that prepare fails only once
 *  the card is gone (-ENODEV, already LOST); on a BlueALSA PCM whose transport
 *  dropped, what prepare answers is unmeasured, and handing anything but ENODEV
 *  up would leave the stream erroring every service with no reopen — silence. */
static int alsa_recover(AlsaCtx *a, int err)
{
    int rc;
    switch (audio_out_alsa_classify(err)) {
    case AO_ERR_XRUN:
        if ((rc = snd_pcm_prepare(a->pcm)) >= 0) return 0;
        return alsa_gone(a, rc);
    case AO_ERR_SUSPEND:
        if (snd_pcm_resume(a->pcm) >= 0) return 0;
        if ((rc = snd_pcm_prepare(a->pcm)) >= 0) return 0;
        return alsa_gone(a, rc);
    case AO_ERR_LOST:
        return alsa_gone(a, err);
    default:
        errno = (err < 0) ? -err : EIO;
        return -1;
    }
}

/** A failure the generic layer would only count.  Consecutive ones past
 *  ALSA_HARD_FAILS_LOST are escalated to a lost device: an EIO/EPIPE that
 *  recurs on every service is a stall, and a reopen is the only way out of it.
 *  Any success resets the run. */
#define ALSA_HARD_FAILS_LOST 8
static int alsa_hard_fail(AlsaCtx *a)
{
    if (errno == ENODEV) return -1;
    if (++a->hard_fails >= ALSA_HARD_FAILS_LOST) return alsa_gone(a, -errno);
    return -1;
}

static int alsa_open(void *ctx, int rate_req, int channels_req,
                     int *rate_granted, int *bits_granted, int *channels_granted)
{
    AlsaCtx *a = (AlsaCtx *)ctx;
    int err;

    /* ⚠️ GPIO12 is the TWL4030's speaker amp and belongs to card 0 alone.  Poking
     * it while a USB DAC is the sink unmutes a speaker nothing is feeding, so it
     * is decided from the PCM actually being opened, not done unconditionally. */
    if (strcmp(a->name, audio_out_device_pcm(AUDIO_DEV_ONBOARD)) == 0)
        audio_out_enable_amp();

    /* ⚠️ Non-blocking is not an optimisation.  A blocking write stalls for a full
     * hardware period once the queue fills, which turns every subsequent sound
     * into a late one; the write policies follow the queue at real-time pace. */
    err = snd_pcm_open(&a->pcm, a->name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "audio_out: cannot open %s: %s\n", a->name, snd_strerror(err));
        a->pcm = NULL;
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned int rate = (unsigned int)rate_req, ch = (unsigned int)channels_req;
    snd_pcm_uframes_t period = ALSA_PERIOD_REQ;
    snd_pcm_uframes_t buffer = ALSA_PERIOD_REQ * ALSA_PERIODS_REQ;
    int dir = 0;

    if ((err = snd_pcm_hw_params_any(a->pcm, hw)) < 0 ||
        (err = snd_pcm_hw_params_set_access(a->pcm, hw,
                                            SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(a->pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(a->pcm, hw, &rate, &dir)) < 0 ||
        (err = snd_pcm_hw_params_set_channels_near(a->pcm, hw, &ch)) < 0)
        goto fail;
    /* Geometry is a request: a refusal here leaves the device's own choice,
     * which the read-back below reports. */
    dir = 0;
    snd_pcm_hw_params_set_period_size_near(a->pcm, hw, &period, &dir);
    snd_pcm_hw_params_set_buffer_size_near(a->pcm, hw, &buffer);
    if ((err = snd_pcm_hw_params(a->pcm, hw)) < 0) goto fail;

    /* The GRANT, read back from the installed parameters — never the request. */
    dir = 0;
    snd_pcm_hw_params_get_rate(hw, &rate, &dir);
    snd_pcm_hw_params_get_channels(hw, &ch);
    dir = 0;
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);

    /* ⚠️ Refuse a ring longer than two seconds rather than play into it.  The
     * geometry above is a request whose refusal is tolerated, and plug:bluealsa
     * offers buffers up to ~2e8 frames: measured on .188, speaker-test left to
     * the PCM's own choice took the maximum and played nothing audible.  A
     * refused open falls back like any other (audio_out_open_resolved()). */
    if ((unsigned long)buffer > 2UL * (unsigned long)rate) {
        fprintf(stderr, "audio_out: %s granted a %lu-frame ring at %u Hz — refused\n",
                a->name, (unsigned long)buffer, rate);
        err = -EINVAL;
        goto fail;
    }

    /* Start at one period queued, so the prefill's silence is what starts the
     * stream — and what restarts it after an XRUN's prepare. */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    if ((err = snd_pcm_sw_params_current(a->pcm, sw)) < 0 ||
        (err = snd_pcm_sw_params_set_start_threshold(a->pcm, sw, period)) < 0 ||
        (err = snd_pcm_sw_params(a->pcm, sw)) < 0)
        goto fail;

    a->period      = period;
    a->buffer      = buffer;
    a->frame_bytes = (int)ch * AUDIO_BYTES_PER_SAMPLE;
    a->lost_said   = false;
    a->write_xruns = 0;
    a->hard_fails  = 0;
    a->written     = 0;
    a->progress_set = false;
    a->stalled     = false;

    *rate_granted     = (int)rate;
    *bits_granted     = snd_pcm_format_width(SND_PCM_FORMAT_S16_LE);
    *channels_granted = (int)ch;

    fprintf(stderr, "audio_out: %s open (alsa), granted %u Hz %d-bit %u ch, "
                    "period %lu, buffer %lu frames (requested %d Hz %d ch)\n",
            a->name, rate, *bits_granted, ch, (unsigned long)period,
            (unsigned long)buffer, rate_req, channels_req);
    return 0;

fail:
    fprintf(stderr, "audio_out: cannot configure %s: %s\n", a->name, snd_strerror(err));
    snd_pcm_close(a->pcm);
    a->pcm = NULL;
    return -1;
}

/* ⚠️ A PCM can freeze with every call succeeding.  Log-measured on a BlueALSA
 * A2DP PCM: avail kept answering, the ring held the lead, nothing was consumed,
 * so the pump asked for nothing every service, no error ever reached
 * device_lost, and the stream sat silent on the pinned headset with no reopen.
 * Progress is consumption, `written - in_flight`, which a write cannot move.
 *
 * 2000 ms: a running ring with a period queued is consumed every period (46 ms
 * at the 2048-frame grant, 44100 Hz), and the granted ring is 743 ms, so this is
 * ~43 periods and ~2.7 ring lengths of nothing played.  It is also no shorter
 * than the longest ring alsa_open() accepts (2 s), so a ring of any accepted
 * size would have drained fully in it.  Not measured: the burst size of
 * BlueALSA's own reads, assumed far below a second. */
#define ALSA_STALL_MS 2000

static uint32_t stall_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return audio_ms_from_timeval((long)ts.tv_sec, (long)(ts.tv_nsec / 1000));
}

/** True once a ring that is OWED playback has not been consumed for
 *  ALSA_STALL_MS.  Owed: at least the start threshold (one period) queued, and
 *  not paused — so a ring below the threshold, which ALSA does not start, is
 *  never a stall, and a service gap the device played through is progress. */
static bool alsa_stalled(AlsaCtx *a, long in_flight)
{
    uint32_t now = stall_now_ms();
    unsigned long consumed = a->written - (unsigned long)in_flight;
    bool owed = in_flight >= (long)a->period &&
                snd_pcm_state(a->pcm) != SND_PCM_STATE_PAUSED;
    if (!owed || !a->progress_set || consumed != a->consumed_last) {
        a->consumed_last = consumed;
        a->progress_ms   = now;
        a->progress_set  = true;
        return false;
    }
    return (uint32_t)(now - a->progress_ms) >= ALSA_STALL_MS;
}

/** A stalled PCM is refused like one that would not open — the same flags,
 *  cleared by the same return (a Bluetooth link change, a USB replug), so the
 *  reopen takes the next tier instead of the stalled sink, and the pinned
 *  headset still wins the stream back when it reconnects.  Onboard has no
 *  tier below it and is simply reopened. */
static void alsa_refuse_stalled(const char *pcm)
{
    if (bt_pin_pcm[0] && strcmp(pcm, bt_pin_pcm) == 0)            bt_pin_refused = true;
    else if (strcmp(pcm, AUDIO_BT_PCM) == 0)                      bt_any_refused = true;
    else if (strcmp(pcm, audio_out_device_pcm(AUDIO_DEV_USB)) == 0) usb_refused   = true;
}

static int alsa_space(void *ctx, int frame_bytes, AudioOutSpace *sp)
{
    AlsaCtx *a = (AlsaCtx *)ctx;
    if (!a->pcm || frame_bytes <= 0) return -1;
    if (a->stalled) return alsa_gone(a, -ETIMEDOUT);

    /* ⚠️ snd_pcm_avail(), never snd_pcm_avail_update().  Measured on this
     * device's alsa-lib with a USB card unplugged under a live plughw stream:
     * avail_update does not sync with the hardware, and from the disconnect on it
     * returned a frozen positive count with no error for 25 s, while avail
     * returned -ENODEV from the first call.  That frozen count reads as a full
     * ring, so the pump asks for nothing, writei — whose EBADFD was the only
     * other report of the loss — is never reached, and the stream goes silent
     * with no reopen.  avail syncs first, which also removes avail_update's lag
     * (measured up to ~1.8k frames, under one period, on a healthy stream). */
    snd_pcm_sframes_t avail = snd_pcm_avail(a->pcm);
    if (avail < 0) {
        if (alsa_recover(a, (int)avail) != 0) return alsa_hard_fail(a);
        avail = snd_pcm_avail(a->pcm);
        if (avail < 0) {
            if (alsa_recover(a, (int)avail) == 0) errno = EIO;  /* errno + one report */
            return alsa_hard_fail(a);
        }
    }

    long ring = (long)a->buffer;
    if (avail > ring) avail = ring;   /* an unreported XRUN reads past the ring */
    if (alsa_stalled(a, ring - (long)avail)) {
        fprintf(stderr, "audio_out: %s stalled — no progress for %u ms (avail=%ld state=%s)\n",
                a->name, (unsigned)(stall_now_ms() - a->progress_ms), (long)avail,
                snd_pcm_state_name(snd_pcm_state(a->pcm)));
        a->stalled = true;
        alsa_refuse_stalled(a->name);
        return alsa_gone(a, -ETIMEDOUT);
    }
    sp->period_frames = (long)a->period;
    sp->ring_frames   = ring;
    sp->space         = (long)avail;
    sp->in_flight     = ring - (long)avail;
    return 0;
}

static ssize_t alsa_write(void *ctx, const void *buf, size_t nbytes, bool *again)
{
    AlsaCtx *a = (AlsaCtx *)ctx;
    if (!a->pcm || a->frame_bytes <= 0) { errno = EBADF; return -1; }

    /* ALSA moves whole frames only, so no partial frame can ever be left in the
     * device and audio_write_frames()' realignment is never entered. */
    snd_pcm_uframes_t frames = (snd_pcm_uframes_t)(nbytes / (size_t)a->frame_bytes);
    if (frames == 0) return 0;

    /* One retry after a recovered XRUN or suspend: a freshly prepared ring is
     * empty, so a second failure is not another underrun. */
    for (int attempt = 0; attempt < 2; attempt++) {
        snd_pcm_sframes_t r = snd_pcm_writei(a->pcm, buf, frames);
        if (r >= 0) {
            a->hard_fails = 0;
            a->written   += (unsigned long)r;
            return (ssize_t)r * a->frame_bytes;
        }
        if (audio_out_alsa_classify((int)r) == AO_ERR_AGAIN) {
            *again = true;
            errno  = EAGAIN;
            return -1;
        }
        /* ⚠️ The service's dry-queue count only sees a ring found empty at the
         * space query; one that runs dry between that query and this write is
         * recovered here and retried, and is audible all the same — so say it. */
        if (audio_out_alsa_classify((int)r) == AO_ERR_XRUN && ++a->write_xruns <= 200)
            fprintf(stderr, "audio_out: %s underran at the write (%u this stream)\n",
                    a->name, a->write_xruns);
        if (alsa_recover(a, (int)r) != 0) return alsa_hard_fail(a);
    }
    errno = EIO;
    return alsa_hard_fail(a);
}

/* usleep, not snd_pcm_wait(): the serviced policy's wait of 0 must stay a
 * no-op, and snd_pcm_wait() can return early, which the blocking policies'
 * wait-count bound does not expect. */
static void alsa_wait(void *ctx, int usec)
{
    (void)ctx;
    if (usec > 0) usleep((useconds_t)usec);
}

/* The generic layer has already drained to the slack; nothing more here. */
static void alsa_close(void *ctx)
{
    AlsaCtx *a = (AlsaCtx *)ctx;
    if (a->pcm) { snd_pcm_close(a->pcm); a->pcm = NULL; }
}

/* The Bluetooth presence probe: open the sink's PCM and close it again.  Run
 * only inside audio_out_bt_present()'s window.  Our own live stream on it is
 * an answer without a second open, and EBUSY means a sink is there but held —
 * present either way, so a settings page never hides the sink being played. */
static bool bt_probe_pcm(const char *pcm)
{
    if (g_alsa.pcm && g_alsa.name && strcmp(g_alsa.name, pcm) == 0) return true;
    snd_pcm_t *p = NULL;
    /* A pinned headset that is not connected fails this open (ENODEV), so it
     * reads absent and the resolver tries the next tier: any connected sink. */
    int err = snd_pcm_open(&p, pcm, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err == 0) { snd_pcm_close(p); return true; }
    return err == -EBUSY;
}

static const AudioOutDev ALSA_DEV = {
    alsa_open, alsa_space, alsa_write, alsa_wait, alsa_close
};

int audio_out_open_alsa(AudioOut *out, const char *pcm, int rate_req, int channels_req)
{
    if (!out) return -1;
    if (!pcm || !*pcm) pcm = audio_out_device_pcm(NULL);
    /* Refuse before touching g_alsa: a second live open must not clobber the
     * first stream's context, and audio_out_open() would refuse it only after. */
    if (g_alsa.pcm) {
        memset(out, 0, sizeof(*out));
        fprintf(stderr, "audio_out: refused — an ALSA stream is already open\n");
        return -1;
    }
    memset(&g_alsa, 0, sizeof(g_alsa));
    g_alsa.name = pcm;
    return audio_out_open(out, &ALSA_DEV, &g_alsa, rate_req, channels_req);
}

#else  /* built without -DAUDIO_OUT_HAVE_ALSA: the host */

/* ⚠️ ALSA is the only device backend, so an ARM object without it is a build that
 * would ship silent — a ScummVM config.mk left over from before the ALSA appends
 * is exactly that.  Refuse it here rather than on the panel. */
#if defined(__arm__)
#error "audio_out.c needs -DAUDIO_OUT_HAVE_ALSA (and alsa-lib) on the device build"
#endif

int audio_out_open_alsa(AudioOut *out, const char *pcm, int rate_req, int channels_req)
{
    (void)pcm; (void)rate_req; (void)channels_req;
    if (out) memset(out, 0, sizeof(*out));
    fprintf(stderr, "audio_out: built without ALSA support\n");
    return -1;
}

/* No libasound, so no BlueALSA sink can be opened: never present. */
static bool bt_probe_pcm(const char *pcm)
{
    (void)pcm;
    return false;
}

#endif /* AUDIO_OUT_HAVE_ALSA */

/* ── The opener ─────────────────────────────────────────────────────────── */

int audio_out_open_default(AudioOut *out, const char *path,
                           int rate_req, int channels_req)
{
    return audio_out_open_alsa(out, audio_out_device_pcm(path),
                               rate_req, channels_req);
}

const char *audio_out_backend_name(void)
{
    return "alsa";
}

/* ── Reopen, fallback and replug ────────────────────────────────────────────
 * Both stream owners (audio.c and ScummVM's mixer) open through here and poll
 * audio_out_usb_returned(), so a DAC or a Bluetooth sink that comes and goes
 * is handled one way. */

/** The name the log lines use: the ALSA PCM the node maps to — the same name
 *  both owners print at open. */
static const char *device_label(const char *path)
{
    return audio_out_device_pcm(path);
}

/** Millisecond clock for the probe's rate limit; audio.c's time_now_ms() is the
 *  same arithmetic, audio_ms_from_timeval(). */
static uint32_t probe_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return audio_ms_from_timeval((long)tv.tv_sec, (long)tv.tv_usec);
}

/** The resolver's static string for `path`, so open_path compares by value. */
static const char *canonical_path(const char *path)
{
    if (path && strcmp(path, AUDIO_DEV_BT_PIN) == 0) return AUDIO_DEV_BT_PIN;
    if (path && strcmp(path, AUDIO_DEV_BT)  == 0) return AUDIO_DEV_BT;
    if (path && strcmp(path, AUDIO_DEV_USB) == 0) return AUDIO_DEV_USB;
    return AUDIO_DEV_ONBOARD;
}

static int open_on(AudioOut *out, const char *path, int rate_req, int channels_req)
{
    if (audio_out_open_default(out, path, rate_req, channels_req) != 0) return -1;
    out->open_path       = canonical_path(path);
    out->reprobe_last_ms = probe_now_ms();
    out->better_seen     = false;
    return 0;
}

int audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                            const char **path_out)
{
    /* ⚠️ A sink that will not open is refused, then the device is resolved
     * again and the next sink down tried — the pinned headset, any Bluetooth
     * sink, USB, then onboard, so at most four opens.  Onboard failing is the
     * end: nothing is below it. */
    for (int attempt = 0; attempt < 4; attempt++) {
        const char *path = audio_out_device_path();
        if (open_on(out, path, rate_req, channels_req) == 0) {
            if (path_out) *path_out = out->open_path;
            return 0;
        }
        if (strcmp(path, AUDIO_DEV_BT_PIN) == 0) {
            bt_pin_refused = true;
            fprintf(stderr, "audio_out: %s will not open — not using it until a "
                    "Bluetooth link changes\n", device_label(AUDIO_DEV_BT_PIN));
        } else if (strcmp(path, AUDIO_DEV_BT) == 0) {
            bt_any_refused = true;
            fprintf(stderr, "audio_out: %s will not open — not using it until a "
                    "Bluetooth link changes\n", device_label(AUDIO_DEV_BT));
        } else if (strcmp(path, AUDIO_DEV_USB) == 0) {
            usb_refused = true;
            fprintf(stderr, "audio_out: %s will not open — not using it until it "
                    "is replugged\n", device_label(AUDIO_DEV_USB));
        } else {
            return -1;
        }
    }
    return -1;
}

bool audio_out_reprobe_due(const char *pref, const char *open_path,
                           uint32_t now_ms, uint32_t last_ms)
{
    return audio_out_reprobe_due_pin(pref, false, open_path, now_ms, last_ms);
}

bool audio_out_reprobe_due_pin(const char *pref, bool pinned, const char *open_path,
                               uint32_t now_ms, uint32_t last_ms)
{
    if (!pref || !open_path) return false;
    /* The best sink this preference can ever resolve to.  With a pin in force
     * that is the pinned headset, so a stream on any other sink — another
     * headset included — keeps looking for it. */
    const char *best;
    if (strcmp(pref, "usb") == 0)                                   best = AUDIO_DEV_USB;
    else if (strcmp(pref, "auto") == 0 || strcmp(pref, "bluetooth") == 0)
        best = pinned ? AUDIO_DEV_BT_PIN : AUDIO_DEV_BT;
    else return false;
    if (!audio_out_sink_better(best, open_path)) return false;
    return (uint32_t)(now_ms - last_ms) >= AUDIO_OUT_REPROBE_MS;
}

bool audio_out_usb_returned(AudioOut *out)
{
    if (!out || !out->is_open || out->device_lost) return false;

    uint32_t now = probe_now_ms();
    if (!audio_out_reprobe_due_pin(audio_dev_pref, bt_pin_pcm[0] != '\0',
                                   out->open_path, now, out->reprobe_last_ms))
        return false;
    out->reprobe_last_ms = now;

    /* Two consecutive sightings: a node or a transport can exist before it will
     * open, and a move that fails there costs the sink a refusal until it is
     * unplugged or reconnected.  Only a move UP counts: the current sink
     * flickering out of a probe is not a reason to leave it. */
    const char *target = audio_out_device_path();
    bool better = audio_out_sink_better(target, out->open_path);
    bool before = out->better_seen;
    out->better_seen = better;
    if (!better || !before) return false;

    char hms[9];
    fprintf(stderr, "audio_out: %s is available — leaving %s at %s\n",
            device_label(target), device_label(out->open_path), wall_hms(hms));
    return true;
}
