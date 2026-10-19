#include "audio_gen.h"

#include <math.h>
#include <string.h>

/* ── Frame and byte arithmetic ───────────────────────────────────────────── */

int audio_frame_bytes(int channels)
{
    if (channels <= 0) return 0;
    return channels * AUDIO_BYTES_PER_SAMPLE;
}

long audio_frames_for_ms(int rate, int duration_ms)
{
    if (rate <= 0 || duration_ms <= 0) return 0;
    if (duration_ms > AUDIO_MAX_TONE_MS) duration_ms = AUDIO_MAX_TONE_MS;
    /* 64-bit on purpose: sizeof(long) == 4 on this target, and the product of a
     * rate and a duration in ms leaves 32 bits at ~48.7 s. */
    long long frames = ((long long)rate * (long long)duration_ms) / 1000;
    return (long)frames;
}

long audio_bytes_for_frames(long frames, int channels)
{
    int fb = audio_frame_bytes(channels);
    if (frames <= 0 || fb == 0) return 0;
    return frames * (long)fb;
}

long audio_ms_for_frames(int rate, long frames)
{
    if (rate <= 0 || frames <= 0) return 0;
    /* 64-bit for the same reason as above: frames * 1000 leaves 32 bits at
     * ~2.1 M frames, which is only ~49 s of audio — a ring size, not an
     * unreachable one. */
    long long ms = ((long long)frames * 1000) / (long long)rate;
    return (long)ms;
}

/* ── Clock arithmetic, without a clock ───────────────────────────────────── */

uint32_t audio_ms_from_timeval(long tv_sec, long tv_usec)
{
    uint32_t sec = (uint32_t)(tv_sec & 0x3FFFFF);
    return sec * 1000U + (uint32_t)(tv_usec / 1000);
}

uint32_t audio_flush_wait_ms(uint32_t now, uint32_t end, uint32_t cap)
{
    if (end <= now) return 0;
    uint32_t wait = end - now;
    if (wait > cap) wait = cap;
    return wait;
}

/* ── Tone envelope ──────────────────────────────────────────────────────── */

static long clamp_edge_frames(int rate, long frames, int ms)
{
    if (rate <= 0 || frames <= 0) return 0;
    long n = (long)rate * ms / 1000;      /* ms is a small literal; no overflow */
    if (n > frames / 2) n = frames / 2;
    if (n < 0) n = 0;
    return n;
}

long audio_attack_frames(int rate, long frames)
{
    return clamp_edge_frames(rate, frames, AUDIO_ATTACK_MS);
}

long audio_release_frames(int rate, long frames)
{
    return clamp_edge_frames(rate, frames, AUDIO_RELEASE_MS);
}

double audio_tone_env(long i, long frames, long attack, long release)
{
    if (attack > 0 && i < attack)
        return (double)i / (double)attack;
    if (release > 0 && i >= frames - release)
        return (double)(frames - 1 - i) / (double)release;
    return 1.0;
}

void audio_render_tone(int rate, int freq_hz, int peak, int16_t *mono, long frames)
{
    if (!mono || frames <= 0 || rate <= 0 || freq_hz <= 0) return;

    long attack  = audio_attack_frames(rate, frames);
    long release = audio_release_frames(rate, frames);

    double phase_step = 2.0 * M_PI * (double)freq_hz / (double)rate;
    double phase      = 0.0;

    for (long i = 0; i < frames; i++) {
        double env = audio_tone_env(i, frames, attack, release);
        mono[i] = (int16_t)((double)peak * env * sin(phase));
        phase += phase_step;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
}

/* ── The one gliding oscillator ─────────────────────────────────────────── */

void audio_osc_init(AudioOsc *o, int rate, double freq_hz, int peak)
{
    memset(o, 0, sizeof(*o));
    o->rate        = rate;
    o->phase       = 0.0;
    o->freq        = freq_hz;
    o->target_freq = freq_hz;
    o->amp         = 0.0;          /* GLIDE ramps it up; FADE_OUT holds it */
    o->freq_smooth = AUDIO_FREQ_SMOOTH;
    o->amp_ramp    = AUDIO_AMP_RAMP;
    o->peak        = peak;
}

void audio_osc_render(AudioOsc *o, AudioOscMode mode, int16_t *mono, long frames)
{
    if (!o || !mono || frames <= 0 || o->rate <= 0) return;

    for (long i = 0; i < frames; i++) {
        double env = 1.0;

        if (mode == AUDIO_OSC_GLIDE) {
            o->freq += (o->target_freq - o->freq) * o->freq_smooth;
            if (o->amp < 1.0) {
                o->amp += o->amp_ramp;
                if (o->amp > 1.0) o->amp = 1.0;
            }
        } else {
            /* Fade out over exactly this call.  Frequency and amplitude are
             * held: a glide or a ramp during a fade-out fights the fade. */
            env = 1.0 - (double)i / (double)frames;
        }

        double phase_step = 2.0 * M_PI * o->freq / (double)o->rate;
        mono[i] = (int16_t)((double)o->peak * env * o->amp * sin(o->phase));

        o->phase += phase_step;
        if (o->phase > 2.0 * M_PI) o->phase -= 2.0 * M_PI;
    }
}

/* ── Mono → interleaved: the one conversion point ───────────────────────── */

long audio_interleave(const int16_t *mono, long frames, int channels, int16_t *out)
{
    if (!mono || !out || frames <= 0 || channels <= 0) return 0;

    for (long i = 0; i < frames; i++) {
        int16_t s = mono[i];
        for (int c = 0; c < channels; c++)
            out[i * channels + c] = s;
    }
    return audio_bytes_for_frames(frames, channels);
}

/* ── The frame-aligned write loop ───────────────────────────────────────── */

void audio_write_frames(const AudioSink *sink, const void *buf, long frames,
                        int channels, const AudioWritePolicy *pol,
                        AudioWriteResult *res)
{
    if (!res) return;
    memset(res, 0, sizeof(*res));

    int fb = audio_frame_bytes(channels);
    if (!sink || !sink->write || !buf || !pol || frames <= 0 || fb == 0) return;

    const uint8_t *p = (const uint8_t *)buf;
    long total = frames * (long)fb;
    long done  = 0;
    int  mid_waits = 0;      /* consecutive waits with a partial frame pending */
    bool stop  = false;

    while (done < total && !stop) {
        bool again = false;
        ssize_t r = sink->write(sink->ctx, p + done, (size_t)(total - done), &again);

        if (r > 0) {
            done += (long)r;
            mid_waits = 0;
            continue;
        }
        if (r == 0) {                 /* took nothing and did not say why */
            stop = true;
            continue;
        }
        if (again) {
            /* The policy's stop conditions apply on frame boundaries only.  Mid
             * frame there is no safe stop: the device is holding half a frame
             * and every sample after it lands in the wrong channel. */
            if ((done % fb) == 0) {
                if (pol->stop_on_again) { stop = true; continue; }
                if (pol->max_waits > 0 && res->waits >= pol->max_waits) { stop = true; continue; }
            } else if (mid_waits >= AUDIO_ALIGN_TRIES) {
                /* ⚠️ Mid frame the policy does not apply — but "not forever"
                 * still does.  An unlimited policy against a permanently full
                 * sink hangs the render loop otherwise, which is worse than the
                 * channel swap it is trying to avoid. */
                stop = true;
                continue;
            } else {
                mid_waits++;
            }
            res->waits++;
            if (sink->wait) sink->wait(sink->ctx, pol->wait_us);
            continue;
        }
        res->sink_error = true;
        stop = true;
    }

    /* Whatever stopped us, do not leave a partial frame in the device. */
    if ((done % fb) != 0) {
        for (int tries = 0; tries < AUDIO_ALIGN_TRIES && (done % fb) != 0; tries++) {
            long need = fb - (done % fb);
            bool again = false;
            ssize_t r = sink->write(sink->ctx, p + done, (size_t)need, &again);
            if (r > 0) {
                done += (long)r;
                continue;
            }
            res->waits++;
            if (sink->wait) sink->wait(sink->ctx, pol->wait_us);
        }
        if ((done % fb) != 0) res->misaligned = true;
    }

    res->bytes_written  = done;
    res->frames_written = done / fb;
}

/* ── The mix bus ────────────────────────────────────────────────────────────
 * Sample-major on purpose: the accumulator is one int32 in a register, so there
 * is no scratch buffer to size and the sum cannot depend on slot order.  The
 * inner loop over eight slots costs a test per slot per sample even when one
 * voice is live; the sin() calls are the real cost and there is one per ACTIVE
 * voice, which is the irreducible price of mixing.
 */

void audio_mix_init(AudioMixer *m, int rate)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->rate  = rate;
    m->limit = AUDIO_MIX_HARD;     /* clampedAdd — see AudioMixLimit */
    m->knee  = AUDIO_MIX_KNEE;
}

void audio_mix_set_limit(AudioMixer *m, int mode)
{
    if (m) m->limit = (mode == AUDIO_MIX_HARD) ? AUDIO_MIX_HARD : AUDIO_MIX_SOFT;
}

void audio_mix_set_knee(AudioMixer *m, int knee)
{
    if (!m) return;
    m->knee = (knee > 0) ? knee : AUDIO_MIX_KNEE;
}

int audio_voice_peak(int vol)
{
    if (vol <= 0) return 1;                       /* silent is a caller bug */
    long p = ((long)AUDIO_FULL_SCALE * vol) / AUDIO_VOL_UNITY;
    if (p > AUDIO_FULL_SCALE) p = AUDIO_FULL_SCALE;
    if (p < 1)               p = 1;
    return (int)p;
}

void audio_attenuate(int16_t *buf, long samples, int shift)
{
    if (!buf || samples <= 0 || shift <= 0) return;
    if (shift > 15) shift = 15;
    for (long i = 0; i < samples; i++) buf[i] = (int16_t)(buf[i] >> shift);
}

int32_t audio_mix_limit(int32_t acc, int mode)
{
    return audio_mix_limit_at(acc, mode, AUDIO_MIX_KNEE);
}

int32_t audio_mix_limit_at(int32_t acc, int mode, int32_t knee)
{
    if (mode == AUDIO_MIX_HARD) {
        if (acc >  32767) return  32767;
        if (acc < -32768) return -32768;
        return acc;
    }

    int32_t mag = (acc < 0) ? -acc : acc;      /* acc is a sum of int16-range
                                                * voices, so this cannot be the
                                                * INT32_MIN that would overflow */
    if (knee <= 0) knee = AUDIO_MIX_KNEE;
    int32_t ceil_at = knee + knee * 44 / 100;  /* the measured 1.44×, derived so
                                                * a ceiling can never sit under
                                                * its own knee */
    if (ceil_at > AUDIO_FULL_SCALE) ceil_at = AUDIO_FULL_SCALE;
    if (ceil_at <= knee)            return (mag > knee) ? ((acc < 0) ? -knee : knee) : acc;
    if (mag <= knee) return acc;

    /* u/(1+u) is bounded by 1 and has slope 1 at u = 0, so the curve meets the
     * identity at the knee in both value and slope — no step, no corner. */
    double span = (double)(ceil_at - knee);
    double u    = (double)(mag - knee) / span;
    double out  = (double)knee + span * (u / (1.0 + u));

    /* Rounded, not truncated.  With truncation the first sample past the knee
     * lands back ON the knee — the curve's slope is 1 there, so the whole first
     * LSB of bend is lost — and the group L slope check measures exactly that. */
    int32_t v = (int32_t)(out + 0.5);
    return (acc < 0) ? -v : v;
}

long audio_mix_render(AudioMixer *m, int16_t *mono, long frames)
{
    if (!m || !mono || frames <= 0) return 0;
    if (audio_mix_pending(m) <= 0)  return 0;

    for (long i = 0; i < frames; i++) {
        int32_t acc = 0;

        for (int vi = 0; vi < AUDIO_MAX_VOICES; vi++) {
            AudioVoice *vo = &m->v[vi];
            if (!vo->active) continue;

            if (vo->delay > 0) {        /* still waiting its turn */
                vo->delay--;
                continue;
            }

            /* The gain ramp: `peak` walks to `peak_goal` by at most `peak_step` per
             * sample, so a live gain change is a ~20 ms slope and not a click.  One
             * compare per voice per sample while settled; no divide. */
            if (vo->peak != vo->peak_goal) {
                int d = vo->peak_goal - vo->peak;
                if (d > 0) vo->peak += (d > vo->peak_step) ? vo->peak_step : d;
                else       vo->peak -= (-d > vo->peak_step) ? vo->peak_step : -d;
            }

            double env = audio_tone_env(vo->pos, vo->frames, vo->attack, vo->release);

            if (vo->kind == AUDIO_VOICE_SAMPLE) {
                /* Pull the next block only when the last one is spent, so `fill`
                 * — and the SD read behind it — is entered once per buffer
                 * rather than once per frame.  buf/buf_pos/buf_len are voice
                 * state like `pos` and `phase`, which is what keeps the
                 * incremental-render property: N then M frames pull the same
                 * bytes in the same order as one N+M render. */
                if (vo->buf_pos >= vo->buf_len && !vo->drained) {
                    long got = vo->fill(vo->ctx, vo->buf, vo->buf_cap);
                    vo->buf_len = (got > 0) ? got : 0;
                    vo->buf_pos = 0;
                    if (vo->buf_len < vo->buf_cap) vo->drained = true;
                }
                if (vo->buf_pos >= vo->buf_len) {
                    /* The source ran dry early — end the voice here rather than
                     * padding with silence it never contained.  A short fill is
                     * legal, so this is a normal exit, not an error. */
                    vo->active = false;
                    continue;
                }
                int16_t s = vo->buf[vo->buf_pos++];

                /* `>> 15`, not `/ AUDIO_FULL_SCALE`.  ⚠️ Cortex-A8 has no
                 * hardware divide, so a constant divide here would be an
                 * `__aeabi_idiv` call per sample per voice; the shift is exact
                 * and the 1/32768-vs-1/32767 difference is a third of an LSB at
                 * full scale.  Same reasoning as `audio_attenuate()`. */
                acc += (int32_t)((double)audio_voice_scale(s, vo->peak) * env);

                if (++vo->pos >= vo->frames) vo->active = false;
                continue;
            }

            acc += (int32_t)((double)vo->peak * env * sin(vo->phase));

            vo->phase += vo->phase_step;
            if (vo->phase > 2.0 * M_PI) vo->phase -= 2.0 * M_PI;

            /* The voice frees itself the instant its last sample is rendered,
             * so the slot is reusable on this same call — not one pump later. */
            if (++vo->pos >= vo->frames) vo->active = false;
        }

        /* One limiter, after the whole sum, so slot order cannot change it.
         * Each counter keeps ONE meaning: `clipped` is "the int16 range could not
         * hold this", which only the hard clamp can produce, and `limited` is
         * "the knee bent it".  Counting both into one number would have hidden
         * the very distinction this change turns on. */
        int32_t out = audio_mix_limit_at(acc, m->limit, m->knee);
        if (out != acc) {
            if (m->limit == AUDIO_MIX_HARD) m->clipped++;
            else                            m->limited++;
        }

        /* And a store the int16 cannot lie about.  Unreachable under SOFT — the
         * curve asymptotes to AUDIO_MIX_CEIL — which is exactly why `clipped`
         * reaching 0 is the measurement that the limiter is really engaged. */
        if      (out >  32767) { out =  32767; m->clipped++; }
        else if (out < -32768) { out = -32768; m->clipped++; }
        mono[i] = (int16_t)out;
    }

    return frames;
}

/* First-fit over the eight slots, lowest index wins.  ⚠️ ONE claim path for both
 * voice kinds, so the refuse-never-steal rule and the `dropped` counter cannot
 * drift apart between them — a second copy of this loop is how a sample voice
 * would quietly acquire a different full-bus policy from a tone. */
static AudioVoice *claim_slot(AudioMixer *m, int *slot)
{
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        AudioVoice *vo = &m->v[i];
        if (vo->active) continue;

        memset(vo, 0, sizeof(*vo));
        vo->active = true;
        vo->gen    = ++m->gen_seq;       /* never 0, so 0 can mean "no voice" */
        *slot = i;
        return vo;
    }

    /* Every slot busy.  Refuse and COUNT — never steal, see audio_gen.h. */
    m->dropped++;
    return NULL;
}

int audio_mix_add(AudioMixer *m, int freq_hz, int duration_ms,
                  int delay_ms, int peak)
{
    if (!m || m->rate <= 0 || freq_hz <= 0 || duration_ms <= 0) return -1;
    if (peak <= 0) return -1;          /* caller bug, not a full bus */

    long frames = audio_frames_for_ms(m->rate, duration_ms);
    if (frames <= 0) return -1;
    long delay = (delay_ms > 0) ? audio_frames_for_ms(m->rate, delay_ms) : 0;

    int slot = -1;
    AudioVoice *vo = claim_slot(m, &slot);
    if (!vo) return -1;

    vo->kind       = AUDIO_VOICE_TONE;
    vo->phase      = 0.0;
    vo->phase_step = 2.0 * M_PI * (double)freq_hz / (double)m->rate;
    vo->peak       = peak;
    vo->peak_goal  = peak;
    vo->peak_step  = 1;
    vo->delay      = delay;
    vo->frames     = frames;
    vo->pos        = 0;
    vo->attack     = audio_attack_frames(m->rate, frames);
    vo->release    = audio_release_frames(m->rate, frames);
    return slot;
}

int audio_mix_add_sample(AudioMixer *m, AudioVoiceFill fill, void *ctx,
                         int16_t *buf, long buf_frames,
                         long total_frames, int peak)
{
    if (!m || m->rate <= 0 || !fill)  return -1;
    if (!buf || buf_frames <= 0)      return -1;
    if (total_frames <= 0)            return -1;
    if (peak <= 0)                    return -1;   /* caller bug, not a full bus */

    int slot = -1;
    AudioVoice *vo = claim_slot(m, &slot);
    if (!vo) return -1;

    vo->kind    = AUDIO_VOICE_SAMPLE;
    vo->fill    = fill;
    vo->ctx     = ctx;
    vo->buf     = buf;
    vo->buf_cap = buf_frames;
    vo->buf_len = 0;                   /* pulled on the first rendered frame */
    vo->buf_pos = 0;
    vo->drained = false;
    vo->peak    = peak;
    vo->peak_goal = peak;
    vo->peak_step = 1;
    vo->delay   = 0;
    vo->frames  = total_frames;
    vo->pos     = 0;
    /* The same envelope a tone gets, and it is not cosmetic: a bed that starts at
     * its first sample's amplitude steps the bus, which is the click.  The
     * release is what `audio_mix_release_voice()` later arms. */
    vo->attack  = audio_attack_frames(m->rate, total_frames);
    vo->release = audio_release_frames(m->rate, total_frames);
    return slot;
}

bool audio_mix_release_voice(AudioMixer *m, int slot, uint32_t gen)
{
    if (!m || slot < 0 || slot >= AUDIO_MAX_VOICES || gen == 0) return false;
    AudioVoice *vo = &m->v[slot];
    if (!vo->active || vo->gen != gen) return false;   /* freed, or reused */

    long release = (vo->release > 0) ? vo->release : 1;
    long end     = vo->pos + release;
    if (end < vo->frames) {
        vo->frames = end;
        /* `audio_tone_env()` reads the release off the END of `frames`, so
         * shortening `frames` is what makes the ramp start HERE.  `release` is
         * left alone: it is the ramp's length, not its position. */
    }
    return true;
}

int audio_mix_get_limit(const AudioMixer *m)
{
    return m ? m->limit : AUDIO_MIX_HARD;
}

void audio_mix_stop_all(AudioMixer *m)
{
    if (!m) return;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) m->v[i].active = false;
}

int audio_mix_active(const AudioMixer *m)
{
    if (!m) return 0;
    int n = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) if (m->v[i].active) n++;
    return n;
}

long audio_mix_pending(const AudioMixer *m)
{
    if (!m) return 0;
    long worst = 0;
    for (int i = 0; i < AUDIO_MAX_VOICES; i++) {
        const AudioVoice *vo = &m->v[i];
        if (!vo->active) continue;
        long left = vo->delay + (vo->frames - vo->pos);
        if (left > worst) worst = left;
    }
    return worst;
}

uint32_t audio_mix_voice_gen(const AudioMixer *m, int slot)
{
    if (!m || slot < 0 || slot >= AUDIO_MAX_VOICES) return 0;
    return m->v[slot].active ? m->v[slot].gen : 0;
}

long audio_mix_voice_pending(const AudioMixer *m, int slot, uint32_t gen)
{
    if (!m || slot < 0 || slot >= AUDIO_MAX_VOICES || gen == 0) return 0;
    const AudioVoice *vo = &m->v[slot];
    if (!vo->active || vo->gen != gen) return 0;   /* freed, or reused by another */
    long left = vo->delay + (vo->frames - vo->pos);
    return (left > 0) ? left : 0;
}

long audio_pump_lead_frames(long lead_ms_frames, long period_frames,
                            int min_periods, long ring_frames)
{
    if (lead_ms_frames < 0) lead_ms_frames = 0;
    if (period_frames <= 0 || min_periods <= 0) return lead_ms_frames;

    long want = lead_ms_frames;
    long floor_frames = period_frames * (long)min_periods;
    if (want < floor_frames) want = floor_frames;

    /* Round UP to a whole period: a target that lands between two periods is the
     * measured cause of the XRUN cycle this function exists to prevent. */
    long rem = want % period_frames;
    if (rem) want += period_frames - rem;

    /* Never ask for more cushion than half the ring.  A device with a tiny buffer
     * would otherwise be asked for a lead it can never hold, and the pump would
     * write into a full ring every single frame. */
    if (ring_frames > 0) {
        long cap = ring_frames / 2;
        if (cap < period_frames) cap = period_frames;   /* one period, or nothing works */
        if (want > cap) want = cap;
    }
    return want;
}

long audio_pump_frames(long lead, long in_flight, long space, long cap)
{
    if (lead <= 0 || space <= 0) return 0;
    if (in_flight < 0) in_flight = 0;   /* a nonsense read-back must not inflate */

    long want = lead - in_flight;
    if (want <= 0)            return 0;
    if (want > space)         want = space;
    if (cap > 0 && want > cap) want = cap;
    return want;
}

int audio_gain_total_mdb(int norm_mdb, int user_mdb)
{
    /* long long: two caller-supplied ints must not wrap before the clamp. */
    long long t = (long long)norm_mdb + (long long)user_mdb;
    if (t > AUDIO_GAIN_MAX_MDB) return AUDIO_GAIN_MAX_MDB;
    if (t < AUDIO_GAIN_MIN_MDB) return AUDIO_GAIN_MIN_MDB;
    return (int)t;
}

int audio_gain_peak(int base_peak, int total_mdb)
{
    if (base_peak <= 0 || total_mdb <= AUDIO_GAIN_MIN_MDB) return 0;
    if (total_mdb > AUDIO_GAIN_MAX_MDB) total_mdb = AUDIO_GAIN_MAX_MDB;
    /* One pow(), on a user action only, never per sample. */
    double p = (double)base_peak * pow(10.0, (double)total_mdb / 20000.0);
    if (p > (double)AUDIO_GAIN_PEAK_MAX) return AUDIO_GAIN_PEAK_MAX;
    return (int)(p + 0.5);
}

bool audio_mix_set_voice_peak(AudioMixer *m, int slot, uint32_t gen, int peak)
{
    if (!m || slot < 0 || slot >= AUDIO_MAX_VOICES || gen == 0 || peak < 0) return false;
    AudioVoice *vo = &m->v[slot];
    if (!vo->active || vo->gen != gen) return false;   /* freed, or reused */

    if (peak > AUDIO_GAIN_PEAK_MAX) peak = AUDIO_GAIN_PEAK_MAX;
    vo->peak_goal = peak;
    if (vo->pos == 0) {                /* nothing sounded yet: no click to avoid */
        vo->peak = peak;
        return true;
    }
    /* About 20 ms for the whole swing; the one divide here is per call, not per sample. */
    long ramp = (long)m->rate * 20 / 1000;
    if (ramp < 1) ramp = 1;
    long d = (long)peak - (long)vo->peak;
    if (d < 0) d = -d;
    long step = (d + ramp - 1) / ramp;
    vo->peak_step = (step < 1) ? 1 : (int)step;
    return true;
}
