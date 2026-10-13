#ifndef AUDIO_GEN_H
#define AUDIO_GEN_H

/**
 * audio_gen — the audio logic that has no device in it.
 *
 * Everything here is a pure function of its arguments: no fd, no ioctl, no
 * clock, no sysfs.  That is the whole point — it is the half of `audio.c` a
 * host regression can reach (`tests/audio_gen_test.c`).
 *
 * The device half — open/configure/write/close plus the GPIO12 amp poke —
 * stays in `audio.c`.
 *
 * Two rules this file exists to enforce:
 *
 *   1. THE CHANNEL COUNT IS AN ARGUMENT, never a literal.  The device grants it
 *      (`hw:0,0` grants exactly 2 — measured, ../SYSTEM_ANALYSIS.md#34-audio);
 *      a caller that assumes is only accidentally right.  Every byte count here
 *      derives from a channel count handed in.
 *   2. A WRITE NEVER STOPS MID-FRAME.  The generator is mono and the device is
 *      interleaved stereo, so half a frame handed to the kernel swaps L and R
 *      for the rest of the stream — permanently, with no mono path underneath
 *      to absorb it.  `audio_write_frames()` is the only place that decides
 *      when to stop, and it stops on frame boundaries or says it could not.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

/** Bytes per sample: S16_LE everywhere, on both the OSS and the ALSA path. */
#define AUDIO_BYTES_PER_SAMPLE   2

/** Full-scale sample magnitude, and the unity denominator of a voice volume.
 *
 *  ⚠️ **A voice's loudness is a FRACTION of full scale, not an absolute number**,
 *  and that is `Audio::MixerImpl`'s shape rather than an invention here:
 *  `scummvm/audio/rate.cpp:122` renders `out = (in * vol) / 256` with `vol`
 *  composed from a global 0–256 and a per-channel 0–255
 *  (`scummvm/audio/mixer.cpp:628-651`), sums with a saturating add, and leaves
 *  ONE attenuation for the device stage.  That chain has driven this speaker
 *  since 2026-08-03, which is why this library copies it instead of designing a
 *  level law of its own. */
#define AUDIO_FULL_SCALE         32767
#define AUDIO_VOL_UNITY          256

/** The two knobs, and they do DIFFERENT jobs — do not collapse them.
 *
 *  `AUDIO_VOICE_VOL` is HEADROOM: `n` voices reach the int16 clamp when
 *  `n * vol > AUDIO_VOL_UNITY`, so 96 fits two and change, 64 fits four.
 *  ⚠️ **The master shift is NOT headroom** — it divides the sum after the clamp
 *  has already decided what survives, so lowering the volume and raising the
 *  shift are not interchangeable even though they cancel acoustically.
 *
 *  `AUDIO_MASTER_SHIFT` is this SPEAKER'S CEILING, spent once per device in
 *  `audio_out` — the same `>>1` `scummvm-arca/backend-files/oss-mixer.cpp`
 *  applies immediately before `write()`.  At 96 and 1 a lone voice reaches 6144
 *  after it, which is the peak a 440 Hz sine is measured clean at here
 *  (../SYSTEM_ANALYSIS.md#34-audio); 18000 was measured as a near-square.
 *  ⚠️ Both are runtime-settable and the ladder pad on `tests/audio_mix_test` is
 *  what chooses them — a constant here is a default, not a verdict. */
#define AUDIO_VOICE_VOL          96
#define AUDIO_MASTER_SHIFT       1

/** Default per-voice amplitude before the master shift — derived, so the volume
 *  above is the only place the level is decided.  Every call site that wants
 *  "a normal tone" passes this; the runtime knob is `audio_set_volume()`. */
#define AUDIO_PEAK               ((AUDIO_FULL_SCALE * AUDIO_VOICE_VOL) / AUDIO_VOL_UNITY)

/** Longest tone this library will render.  A longer request is a caller bug:
 *  clamped here rather than overflowed, because `(long)rate * duration_ms` is a
 *  32-bit multiply on this target and 44100 × 49 000 exceeds INT32_MAX. */
#define AUDIO_MAX_TONE_MS        30000

/** Attack and release of a plain tone, in ms — removes the click at each end. */
#define AUDIO_ATTACK_MS          10
#define AUDIO_RELEASE_MS         20

/** Frequency smoothing factor per sample (higher = faster glide). */
#define AUDIO_FREQ_SMOOTH        0.15

/** Amplitude ramp per sample for the streaming fade-in. */
#define AUDIO_AMP_RAMP           0.0005

/** Extra attempts `audio_write_frames()` makes to finish a partial frame after
 *  its stop condition has already fired.  Bounded, because the alternative to
 *  giving up is hanging: 4 × the policy's wait is the worst case. */
#define AUDIO_ALIGN_TRIES        4

/** Voices the mix bus can hold at once.  A request past this is REFUSED and
 *  counted, never allowed to steal a playing voice — see `audio_mix_add()`. */
#define AUDIO_MAX_VOICES         8

/** Target queue depth the pump keeps inside the device, in ms.
 *
 *  This is the whole latency budget of the mix bus: audio already handed to the
 *  kernel cannot be mixed into, so a newly triggered sound starts at most this
 *  late.  It is also the only cushion against a slow render-loop iteration, so
 *  it must exceed a couple of frames at `FRAME_DELAY_ACTIVE_US` (33 ms). */
#define AUDIO_PUMP_LEAD_MS       80

/** Most audio one `audio_pump()` call will render, in ms.  Bounds the work a
 *  single render-loop iteration can be made to do. */
#define AUDIO_PUMP_CAP_MS        80

/** Whole device periods the pump must stay ahead by, at minimum.
 *
 * ⚠️ **A lead in MILLISECONDS is not enough, and this is measured.** The OSS shim
 * hands ALSA whole periods only — `appl_ptr` on `.188` moves 2048 → 4096 → 6144
 * and never lands between — while `GETOSPACE` counts the partial period it is
 * still staging.  So an 80 ms lead against a 2048-frame (46 ms) period is 1.7
 * periods, of which ALSA can *play* one: it drains that, the next is not complete
 * yet, and it goes `XRUN`.  The shim's recovery **discards** what was staged, so
 * every ~120 ms a crack, sounds cut short, and a tone chopped 8×/s that reads as
 * distortion.  Measured on `.188` 2026-08-15 by polling
 * `/proc/asound/card0/pcm0p/sub0/status` while keepalive ran; the operator
 * independently counted "20–25 cracks" in a 3 s drone, i.e. the same cadence.
 *
 * Three is the smallest depth that leaves a whole spare period *while* one is
 * being staged and one is playing.  ⚠️ **The lead is also the latency ceiling** —
 * at a 46 ms period this buys ~139 ms, which is the price of the OSS shim's
 * period size.  Native ALSA grants `period_size=1024` (23 ms, measured), so that
 * price is theoretically recoverable — see ../SYSTEM_ANALYSIS.md#34-audio, which
 * records why the port is not planned. */
#define AUDIO_PUMP_LEAD_PERIODS  3

/**
 * The lead the pump should actually target, in frames.
 *
 * PURE: the caller measures `period_frames` off the device (the granted ALSA period)
 * and passes it in.  `period_frames <= 0` means
 * nothing was measured, and then the ms figure is all there is to go on.
 *
 * Rounds UP to a whole period, because a target between two periods is what
 * produced the XRUN cycle above, and caps at half the ring so a deep lead on a
 * small buffer cannot ask for more cushion than the device can hold.
 */
long audio_pump_lead_frames(long lead_ms_frames, long period_frames,
                            int min_periods, long ring_frames);

/** Where the summed bus stops being linear, and where it asymptotes.
 *
 * ⚠️ **These bound the SOFT knee only, and SOFT is no longer the default** — the
 * bus clamps like `Audio::MixerImpl` does (`clampedAdd`, `scummvm/audio/rate.cpp:127`),
 * with no curve, because that is the chain measured to sound right on this
 * speaker.  The knee stays reachable through `audio_mix_set_limit()` so the
 * rejected shape is an A/B on the same panel rather than a claim.
 *
 * ⚠️ **The knee TRACKS the voice amplitude and is not a constant** — one voice
 * must be BYTE-IDENTICAL through the limiter (the property `audio_render_tone()`
 * parity depends on), and the amplitude is now a runtime volume.  These two are
 * therefore the defaults for `audio_mix_set_knee()`, which every volume change
 * must re-derive; the ceiling keeps the measured 1.44× ratio to the knee, which
 * is what makes the curve bounded BELOW full scale so `clipped` can reach
 * exactly 0 under SOFT. */
#define AUDIO_MIX_KNEE           AUDIO_PEAK
#define AUDIO_MIX_CEIL           (AUDIO_MIX_KNEE + AUDIO_MIX_KNEE * 44 / 100)

/* ── Frame and byte arithmetic ───────────────────────────────────────────── */

/** Bytes in one interleaved frame.  0 for a nonsensical channel count. */
int  audio_frame_bytes(int channels);

/** Frames in `duration_ms` at `rate`, computed 64-bit and clamped to
 *  AUDIO_MAX_TONE_MS.  0 for a non-positive rate or duration. */
long audio_frames_for_ms(int rate, int duration_ms);

/** Bytes for `frames` interleaved frames of `channels` channels. */
long audio_bytes_for_frames(long frames, int channels);

/** The inverse of `audio_frames_for_ms()`, and it exists so a DIAGNOSTIC can
 *  report the lead the pump actually took rather than the constant it asked for.
 *  ⚠️ It does **not** clamp: `audio_frames_for_ms()` clamps because a caller's
 *  *request* may be nonsense, while a frame count handed back by the device is a
 *  measurement — clamping it would hide exactly the surprise worth reporting.
 *  0 for a non-positive rate or frame count. */
long audio_ms_for_frames(int rate, long frames);

/* ── Clock arithmetic, without a clock ───────────────────────────────────── */

/**
 * `struct timeval` → ms, as `time_now_ms()` computes it.
 *
 * Only the low 22 bits of the seconds are used, so the result peaks at
 * 4,194,303,999 against UINT32_MAX 4,294,967,295 — it does NOT overflow.  It
 * wraps every ~48.5 days instead, and `audio_flush_wait_ms()`'s cap is what
 * bounds the consequence of a wrap to one short wait.
 */
uint32_t audio_ms_from_timeval(long tv_sec, long tv_usec);

/** How long to wait for the current sound to finish: 0 if it already has,
 *  otherwise `end - now` capped at `cap`.  The cap is load-bearing — across a
 *  clock wrap `end - now` is nonsense, and this is what makes it harmless. */
uint32_t audio_flush_wait_ms(uint32_t now, uint32_t end, uint32_t cap);

/* ── Tone envelope ──────────────────────────────────────────────────────── */

/** Attack/release lengths in frames, each clamped to half the tone so a short
 *  tone still has both, and both 0 for a 1-frame tone (no divide by zero). */
long audio_attack_frames(int rate, long frames);
long audio_release_frames(int rate, long frames);

/** Envelope at sample `i`: 0 → 1 over the attack, 1, then 1 → 0 over the
 *  release.  Continuous at both joins, including when they meet at frames/2. */
double audio_tone_env(long i, long frames, long attack, long release);

/** Render `frames` MONO samples of a fixed-frequency tone with that envelope. */
void audio_render_tone(int rate, int freq_hz, int peak, int16_t *mono, long frames);

/* ── The one gliding oscillator ─────────────────────────────────────────── */

/**
 * Streaming oscillator: phase accumulator + frequency glide + amplitude ramp.
 *
 * This is ONE implementation of what `audio.c` had in three places (the
 * stream prefill and the stream chunk were identical; the fade-out was a
 * variant that deliberately holds frequency and amplitude still).  The variant
 * is a MODE, not a second copy — deleting it would delete the fade.
 */
typedef enum {
    AUDIO_OSC_GLIDE    = 0,  /**< smooth freq toward target, ramp amp up to 1  */
    AUDIO_OSC_FADE_OUT = 1   /**< hold freq and amp, envelope 1 → 0 over the call */
} AudioOscMode;

typedef struct {
    int    rate;         /**< frames per second                               */
    double phase;        /**< radians, wrapped to [0, 2π)                     */
    double freq;         /**< current frequency (Hz)                          */
    double target_freq;  /**< glide target (Hz)                               */
    double amp;          /**< current amplitude, 0..1                         */
    double freq_smooth;  /**< glide factor per sample                         */
    double amp_ramp;     /**< amplitude ramp per sample                       */
    int    peak;         /**< peak amplitude at amp == 1                      */
} AudioOsc;

/** Start at `freq_hz` with amplitude 0 (so GLIDE fades in) and phase 0. */
void audio_osc_init(AudioOsc *o, int rate, double freq_hz, int peak);

/**
 * Render `frames` MONO samples, advancing the oscillator's state.
 *
 * Split calls are identical to one long call — that is what lets the caller
 * write in chunks of whatever the ring will take without a seam.
 */
void audio_osc_render(AudioOsc *o, AudioOscMode mode, int16_t *mono, long frames);

/* ── Mono → interleaved: the one conversion point ───────────────────────── */

/**
 * Duplicate `frames` mono samples into `channels` interleaved channels.
 *
 * The hardware is mono and the interface is not, so this is where the two meet
 * and nothing above it needs to know.  `SPKR1` sees L + R (measured), so
 * duplicating is correct and is the loudest of the options.
 *
 * `out` must hold `audio_bytes_for_frames(frames, channels)` bytes.  Returns
 * that byte count, or 0 if it could not.
 */
long audio_interleave(const int16_t *mono, long frames, int channels, int16_t *out);

/* ── The frame-aligned write loop ───────────────────────────────────────── */

/**
 * Where the bytes go.  `write` returns bytes accepted, or -1; on -1 it sets
 * `*again` when the sink was merely full (EAGAIN) rather than broken.  `wait`
 * may be NULL, and is the only thing in this file that is allowed to block.
 */
typedef struct {
    ssize_t (*write)(void *ctx, const void *buf, size_t nbytes, bool *again);
    void    (*wait)(void *ctx, int usec);
    void     *ctx;
} AudioSink;

typedef struct {
    int  wait_us;        /**< passed to `wait` between retries                */
    int  max_waits;      /**< <= 0 for unlimited                              */
    bool stop_on_again;  /**< give up at the first full sink instead of waiting */
} AudioWritePolicy;

typedef struct {
    long frames_written; /**< whole frames the sink took                      */
    long bytes_written;  /**< always frames_written * frame_bytes ...          */
    bool misaligned;     /**< ... unless this is true: a partial frame is in
                          *   the device and L/R are now swapped.  A fault to
                          *   report, never to ignore.                        */
    bool sink_error;     /**< the sink failed for a reason other than "full"  */
    int  waits;          /**< how many times `wait` was called                */
} AudioWriteResult;

/**
 * Write `frames` interleaved frames, stopping only on a frame boundary.
 *
 * `stop_on_again` and `max_waits` are honoured at frame boundaries only: mid
 * frame it keeps going (up to AUDIO_ALIGN_TRIES further attempts) because
 * stopping there is not a lesser evil, it is a permanent channel swap.
 */
void audio_write_frames(const AudioSink *sink, const void *buf, long frames,
                        int channels, const AudioWritePolicy *pol,
                        AudioWriteResult *res);

/* ── The mix bus ────────────────────────────────────────────────────────────
 *
 * You cannot mix into a buffer the kernel already holds: a sound written whole
 * leaves a second one arriving too late to be summed.  So userspace holds the
 * audio and writes it incrementally — `audio_tone()` only adds a voice here, and
 * whoever services next renders it.
 *
 * It is a PUMP, not a thread.  `native_apps` links no pthread of its own, and a
 * pthread link done wrong is how you get `clock_gettime64` → `-ENOSYS` →
 * SIGSEGV before `main()` (../CLAUDE.md).  So `audio.c` renders from this bus
 * once per frame, next to `fb_swap()`.
 *
 * Everything here is still pure: no fd, no ioctl, no clock.  Time is counted in
 * FRAMES the caller has asked for, so the whole bus — summing, clipping, voice
 * lifetime, the arpeggio delays — is host-testable (`tests/audio_gen_test.c`).
 */

/**
 * One sounding tone.  Renderable INCREMENTALLY: `pos` and `phase` are all the
 * state, so N + M frames are identical to one N+M-frame render, which is what
 * lets the pump write whatever the device will take this frame without a seam.
 *
 * `delay` is what keeps a canned arpeggio an arpeggio.  `audio_success()` is
 * three notes, and three voices all starting now is a CHORD — a different sound
 * at 45 call sites.  Offsetting each note by the ones before it preserves what
 * the panel already hears while still mixing with everything else.
 */
/**
 * A voice that plays RECORDED mono PCM instead of a sine.
 *
 * ⚠️ **The fill is a PULL callback, and that is the whole point.** `audio_gen.c`
 * opens no fd, calls no ioctl and reads no clock — the property that lets the
 * whole mixer be host-tested and byte-compared. A `read()` in here would take
 * that away, so the file half stays in `audio.c` and reaches the bus through
 * this callback. Same shape as `AudioOutFill` (`audio_out.h`), minus `channels`:
 * the bus is mono and interleaving is strictly downstream of it.
 *
 * Contract, matching `AudioOutFill`'s so the two read as siblings:
 *   - Produce up to `frames` mono samples in `dst`; return how many you really
 *     produced. **A short fill is legal** and means "the source ran dry".
 *   - Do not re-enter the mixer.
 *   - `dst` is NOT pre-zeroed — a short fill's tail is whatever was there, and
 *     the bus reads only the returned count.
 */
typedef long (*AudioVoiceFill)(void *ctx, int16_t *dst, long frames);

/** What a voice is made of.  TONE is 0 so `memset`-zeroing a slot still yields
 *  the voice kind every existing caller creates. */
typedef enum {
    AUDIO_VOICE_TONE   = 0,  /**< sin(phase) — a generated tone            */
    AUDIO_VOICE_SAMPLE = 1   /**< PCM pulled through an AudioVoiceFill       */
} AudioVoiceKind;

typedef struct {
    bool     active;      /**< false = free slot                               */
    uint32_t gen;         /**< bumped on every add — see audio_mix_voice_gen() */
    double   phase;       /**< radians, wrapped to [0, 2π)                     */
    double   phase_step;  /**< 2π · freq / rate                                */
    int      peak;        /**< this voice's amplitude — the per-voice gain      */
    long     delay;       /**< frames of silence still owed before it sounds    */
    long     frames;      /**< total length                                    */
    long     pos;         /**< frames sounded so far (0..frames)               */
    long     attack;      /**< envelope, in frames — same curve as a plain tone */
    long     release;

    /* ── AUDIO_VOICE_SAMPLE only.  Zero for a tone, which is what `memset`
     *    already leaves behind, so no existing add path has to say so. ── */
    int            kind;     /**< AudioVoiceKind                               */
    AudioVoiceFill fill;     /**< pull callback — see above                     */
    void          *ctx;      /**< opaque; the CALLER owns it and must outlive
                              *   the voice.  There is no teardown hook on any
                              *   of the four release sites (self-free,
                              *   stop_all, init, slot reuse), so ownership
                              *   cannot live down here.  Poll the voice with
                              *   `audio_mix_voice_pending()` to learn it went. */
    int16_t       *buf;      /**< CALLER-owned scratch.  `audio_gen.c` has no
                              *   allocator and gains none for this.           */
    long           buf_cap;  /**< frames `buf` holds                            */
    long           buf_len;  /**< frames currently in `buf`                     */
    long           buf_pos;  /**< frames of `buf` already rendered              */
    bool           drained;  /**< the fill returned short: no more will come    */
} AudioVoice;

typedef struct {
    int        rate;
    AudioVoice v[AUDIO_MAX_VOICES];
    uint32_t   dropped;  /**< adds refused because every slot was busy       */
    uint32_t   clipped;  /**< samples the summed bus drove past int16 range   */
    uint32_t   limited;  /**< samples the soft knee had to bend              */
    int        limit;    /**< AudioMixLimit — how the sum leaves the bus      */
    int        knee;     /**< SOFT's knee; 0 means AUDIO_MIX_KNEE            */
    uint32_t   gen_seq;  /**< monotone, so a slot's identity survives reuse   */
} AudioMixer;

/**
 * What happens to a summed sample that is louder than one voice can be.
 *
 * ⚠️ **`HARD` is the DEFAULT now, and it is `clampedAdd`** — the saturating add
 * `Audio::MixerImpl` has always used (`scummvm/audio/rate.cpp:127`).  `SOFT` is
 * this repo's own knee, kept reachable because a rejected shape with no A/B
 * beside it is a claim rather than a measurement (`tests/audio_mix_test`'s LIM
 * toggle is that control).  ⚠️ Headroom is the VOLUME's job, not the limiter's:
 * at `AUDIO_VOICE_VOL` 96 two voices fit under the clamp and three do not.
 */
typedef enum {
    AUDIO_MIX_SOFT = 0,  /**< linear to the knee, then asymptotic to 1.44× it   */
    AUDIO_MIX_HARD = 1   /**< clamp at int16 — ScummVM's shape, the default     */
} AudioMixLimit;

/**
 * Map one summed sample to what the device should hear.  PURE — no mixer, no
 * counters, so a test can sweep it.
 *
 * `SOFT` is `y = K + (C-K)·u/(1+u)` with `u = (|x|-K)/(C-K)`: continuous in value
 * AND in slope at the knee (both are 1 there), monotone, odd-symmetric, and
 * bounded by `C` for any input — so it needs no clamp behind it and cannot wrap.
 * Below the knee it is the identity, which is what keeps one voice unchanged.
 */
int32_t audio_mix_limit(int32_t acc, int mode);

/**
 * The same map with the knee supplied, which is what the bus actually calls.
 * `knee <= 0` falls back to `AUDIO_MIX_KNEE`.  The ceiling is derived from the
 * knee at the measured 1.44× ratio rather than passed, so the two cannot be set
 * inconsistently — a ceiling below its knee would invert the curve.
 */
int32_t audio_mix_limit_at(int32_t acc, int mode, int32_t knee);

/** The amplitude a voice at `vol` renders at, before the master shift.
 *  `(AUDIO_FULL_SCALE * vol) / AUDIO_VOL_UNITY`, clamped to 1..full scale —
 *  `rate.cpp:122`'s arithmetic, integer and truncating like the original. */
int  audio_voice_peak(int vol);

/**
 * Attenuate `samples` int16s in place by `shift` bits — the device stage, and
 * the ONE implementation of it.  `audio_out` calls this immediately before every
 * `write()` and the pre-continuous path calls it before its own, so both reach
 * the speaker at the same level and an A/B between them changes only the path.
 *
 * ⚠️ **An arithmetic shift, never a gain multiply.** `-1 >> 1 == -1` and
 * `shift == 0` is the identity, so this is bit-identical to what ScummVM has
 * shipped since 2026-08-03; any rounding multiply changes bits and makes the
 * adapter's output a new thing to re-verify by ear.
 */
void audio_attenuate(int16_t *buf, long samples, int shift);

/** Choose the limiter.  Takes effect on the next rendered sample; it changes
 *  only samples ABOVE the knee, so switching it mid-tone cannot step a quiet
 *  one and the panel can A/B it while a drone is running. */
void audio_mix_set_limit(AudioMixer *m, int mode);

/** Set SOFT's knee, which every volume change must re-derive from
 *  `audio_voice_peak()` — the knee's whole job is that ONE voice passes the
 *  limiter unchanged, and a knee left at a stale amplitude bends a lone tone.
 *  `knee <= 0` restores `AUDIO_MIX_KNEE`. */
void audio_mix_set_knee(AudioMixer *m, int knee);

/** Reset the bus to silence at `rate`.  Clears the diagnostics too. */
void audio_mix_init(AudioMixer *m, int rate);

/**
 * Add one tone voice.  Returns its slot, or -1.
 *
 * ⚠️ **A full bus REFUSES and counts (`dropped`); it never steals a voice.**
 * Stealing the oldest is what a synth does, and it is wrong here: the longest
 * voice on this bus is the one thing a dropped UI beep must not be allowed to
 * cut — background music is a 44 s voice, and a missing blip is far cheaper
 * than a chopped soundtrack.
 *
 * `peak <= 0` is a caller bug, refused WITHOUT counting a drop: `dropped` means
 * "the bus was full", which is the number worth watching.
 */
int  audio_mix_add(AudioMixer *m, int freq_hz, int duration_ms,
                   int delay_ms, int peak);

/**
 * Add one SAMPLE voice: recorded mono PCM pulled through `fill`.
 *
 * Returns its slot, or -1.  The full-bus rule above applies **unchanged, and
 * that is deliberate** — it was written for exactly this voice.  A music bed is
 * the longest voice on the bus, so a blip arriving into a full bus is refused
 * and counted rather than allowed to cut it.  One mechanism, not two.
 *
 * `total_frames` is the source's real length, so `pos`, `frames`,
 * `audio_mix_active()`, `audio_mix_pending()` and the self-free at
 * `pos >= frames` all behave exactly as they do for a tone. ⚠️ **Getting this
 * from the WAV's `data` chunk size rather than inventing a sentinel is what
 * keeps `audio_mix_render()`'s `audio_mix_pending() <= 0` early-out correct**:
 * a voice that under-reports its length is never rendered at all.  For material
 * meant to loop, pass the total you intend to play, not one pass.
 *
 * `buf` is CALLER-owned scratch of `buf_frames` frames, and `audio_gen.c` never
 * frees or reallocates it — this file has no allocator and gains none here.
 * It must outlive the voice, and so must `ctx`.  Size it at a device period or
 * more: it is refilled only when it runs dry, so it sets how often `fill` (and
 * therefore the SD read behind it) is entered.
 *
 * `peak <= 0`, a NULL `fill`, a NULL/short `buf` and `total_frames <= 0` are all
 * caller bugs, refused WITHOUT counting a drop — same split as above.
 */
int  audio_mix_add_sample(AudioMixer *m, AudioVoiceFill fill, void *ctx,
                          int16_t *buf, long buf_frames,
                          long total_frames, int peak);

/**
 * Ask ONE voice to stop at the end of its release, and free itself there.
 *
 * ⚠️ **Not the same thing as clearing `active`, and the difference is audible.**
 * A voice cut mid-cycle steps the summed bus by its whole instantaneous
 * amplitude, which is an audible click.  This shortens `frames` to
 * `pos + release` instead, so the existing envelope walks it down to exactly 0
 * and the slot frees itself in `audio_mix_render()` like any other finished
 * voice.
 *
 * Identified by `(slot, gen)` like `audio_mix_voice_pending()`, so a stale
 * handle whose slot has been reused cannot stop somebody else's voice.
 * Returns true if it found that voice and armed the release.
 */
bool audio_mix_release_voice(AudioMixer *m, int slot, uint32_t gen);

/** Which limiter the bus is actually using, AUDIO_MIX_HARD or AUDIO_MIX_SOFT.
 *  ⚠️ **An A/B tool must read its toggle back from here rather than print its
 *  own variable.** `audio_mix_init()` sets HARD, so a tool whose local flag
 *  starts false displayed `SOFT` over a HARD bus and the log agreed with it —
 *  measured in `tests/audio_mix_test.c` 2026-08-19. */
int  audio_mix_get_limit(const AudioMixer *m);

/** Silence every voice at once.  What `audio_interrupt()` becomes when pumping:
 *  it cannot un-write audio already inside the device, so up to
 *  AUDIO_PUMP_LEAD_MS of tail survives it.  That is the price of not resetting
 *  the ring, which is the very thing that makes mixing impossible. */
void audio_mix_stop_all(AudioMixer *m);

/** Voices occupying a slot — sounding or still waiting out their delay. */
int  audio_mix_active(const AudioMixer *m);

/** Frames until the bus falls silent (delay + remaining length, worst voice).
 *  0 means there is nothing to render, which is how the pump knows to write
 *  nothing at all rather than a buffer of silence. */
long audio_mix_pending(const AudioMixer *m);

/** The generation stamp of the voice in `slot`, or 0 for an empty slot.
 *  Slots are reused within the same call, so a slot number alone does not name a
 *  voice; a caller that means "the one I just added" keeps the pair. */
uint32_t audio_mix_voice_gen(const AudioMixer *m, int slot);

/**
 * Frames until ONE named voice falls silent — 0 if that slot is empty or now
 * holds a different voice.
 *
 * ⚠️ **This exists because `audio_mix_pending()` is the wrong tail to queue
 * behind.** Defaulting a tone's delay to the whole bus's worst voice makes a
 * long one serialise every later sound behind it: a 3 s drone pushed six taps to
 * the far side of it on the panel, which is the opposite of mixing.  The tail
 * that keeps a two-note motif two notes is the tail of the PRECEDING TONE.
 */
long audio_mix_voice_pending(const AudioMixer *m, int slot, uint32_t gen);

/**
 * Render and sum `frames` MONO samples, advancing every voice.  Returns the
 * frames written, or 0 when the bus is silent — in which case `mono` is NOT
 * touched.
 *
 * The sum accumulates in `int32_t` and leaves the bus through
 * `audio_mix_limit()`, once, at the store — so the result does not depend on
 * which slot a voice happens to sit in.  Both outcomes count: `limited` for a
 * sample the soft knee bent, `clipped` for one the int16 store could not hold.
 * ⚠️ **Under `AUDIO_MIX_SOFT`, `clipped` must stay 0** — the curve is bounded by
 * `AUDIO_MIX_CEIL`, so a non-zero count there is a defect, not a loud sound.
 *
 * A single voice is BYTE-IDENTICAL to `audio_render_tone()` of the same tone —
 * so switching an app to the pump cannot change how one sound is heard.  That
 * survives the limiter because one voice cannot reach the knee.
 */
long audio_mix_render(AudioMixer *m, int16_t *mono, long frames);

/**
 * How many frames the pump should render this call.  Pure arithmetic; the
 * caller gets `in_flight`/`space` off the device and the two `_MS` constants
 * through `audio_frames_for_ms()`.
 *
 * ⚠️ **The pump targets a LEAD, it does not fill the ring.** An empty ~506 ms
 * OSS ring will happily accept 506 ms of audio, and then a tone triggered on
 * the next frame plays half a second late.  `lead` is therefore both the queue
 * depth and the latency ceiling, and this returns 0 whenever we are already
 * that far ahead.
 */
long audio_pump_frames(long lead, long in_flight, long space, long cap);

#endif /* AUDIO_GEN_H */
