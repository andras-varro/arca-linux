#ifndef AUDIO_H
#define AUDIO_H

/**
 * Audio Library for Arca native games
 *
 * Hardware path:
 *   TWL4030 HiFi DAC  →  HandsfreeL/R class-D amp  →  SPKR1
 *   Kernel interface:  ALSA through libasound, `plughw:N,0` (card N is /dev/dspN)
 *   Amp enable:        GPIO12 sysfs (active HIGH)
 *
 * Prerequisites:
 *   /etc/init.d/audio-enable must have run to configure the ALSA
 *   HandsfreeL/R mixer path.  This is installed automatically by
 *   build-and-deploy.sh with the 'permanent' flag.
 *   audio_init() still drives GPIO12 directly so it works even when
 *   called from a dev shell before the boot service is active.
 *
 * Typical use:
 *   Audio audio;
 *   if (audio_init(&audio) == 0) {
 *       audio_beep(&audio);          // UI click sound
 *       audio_success(&audio);       // level-up fanfare
 *       audio_close(&audio);
 *   }
 *
 * If /dev/dsp cannot be opened audio_init() returns -1 and sets
 * audio.available = false.  All playback functions are silent no-ops
 * in that state, so games work without audio hardware.
 *
 * ⚠️ **There is ONE playback mode: the continuous stream.**  audio_init() opens
 * `common/audio_out.c`'s never-reset stream and puts the mix bus on it, so every
 * sound is a mixer voice and nothing reaches the device until audio_pump() services
 * it — once per frame from the render loop (see the mix-bus block below).  The old
 * per-sound path (this file's own fd, a SNDCTL_DSP_RESET before every canned sound,
 * one sound at a time) is gone; `audio_out.c` is the only opener of the device.
 */

#include <stdint.h>
#include <stdbool.h>

#include "audio_gen.h"
#include "audio_out.h"
#include "audio_wav.h"

/**
 * One streaming sample voice: the file, its read-ahead, and its bus handle.
 *
 * ⚠️ **A struct rather than five `music_*` fields, because there are TWO of
 * these and the policy must not be written twice.**  A bed and an effect differ
 * only in which instance they name and whether they loop — refusing with no stream,
 * refusing a rate mismatch, refusing while the previous one still sounds, and
 * arming the release instead of cutting are one code path in `audio.c`.  The
 * moment that was two code paths, the bed and the effect could drift apart in
 * exactly the way `audio_gen.h`'s full-bus rule exists to prevent.
 *
 * `slot` is -1 when idle and is set by `level_defaults()`, which every entry
 * point runs — a zeroed `slot` is slot 0, i.e. somebody else's voice.
 *
 * ⚠️ **`buf` and `wav` are the mixer's `ctx`, so this struct must not be copied
 * or moved while a voice is live**: `audio_mix_add_sample()` stores the pointers.
 * Nothing copies an `Audio`; this is the reason not to start.
 */
typedef struct {
    AudioWav wav;             /**< the open file; `wav.f` NULL when closed       */
    AudioWavStream stream;    /**< an fd source instead (audio_music_start_fd); `streaming` says which is live */
    bool     streaming;       /**< this voice pulls from `stream`, not `wav`      */
    int16_t *buf;             /**< read-ahead scratch, allocated on first start  */
    long     buf_frames;      /**< its size — how often the SD read is entered   */
    int      slot;            /**< mix-bus slot, -1 when this voice is idle      */
    uint32_t gen;             /**< its generation, so a REUSED slot is not cut   */
    bool     held;            /**< paused by audio_music_pause(): the voice is
                               *   released but `wav` stays OPEN at its position,
                               *   which is what a resume re-arms over.  ⚠️ Not
                               *   the same as "idle": an idle voice has no file  */
} AudioSampleVoice;

/* ── Recorded clips in RAM: what the four canned sounds are MADE of ──────────
 *
 * ⚠️ **A clip is RAM-resident because of the CURSOR, not because of the memory**
 * (operator, 2026-08-20).  There is one `AudioSampleVoice` for effects and its
 * `AudioWav` *is* the live voice's `ctx`, so `audio_sfx_play()` must refuse a
 * second tap while the first sounds — which for a game firing brick hits in
 * bursts is a refusal on nearly every hit.  A clip held in RAM can be handed
 * to several voices at once because each gets its own read position, and that
 * costs no mixer change: `audio_mix_add_sample()` already takes an
 * `AudioVoiceFill`, so a clip is just a different `fill`.
 *
 * The bed does NOT become one of these.  It streams, and that is measured rather
 * than assumed: a cold read of the 3.9 MB bed is 0.369 s, so RAM-loading it would
 * buy a startup stall to replace a path that has been heard clean.
 */

/** One loaded effect, shared by every voice playing it.  `pcm` is the whole file
 *  as mono 16-bit at the device rate — the mixer's own format, so a trigger is a
 *  `memcpy` and nothing converts anything at play time. */
typedef struct {
    int16_t *pcm;             /**< NULL until loaded, and NULL again on a miss   */
    long     frames;          /**< what `pcm` holds                              */
    bool     tried;           /**< a load was ATTEMPTED.  ⚠️ A miss is permanent
                               *   for the process on purpose: the files can
                               *   legitimately be absent, and retrying per
                               *   trigger would put one refusal per tap into
                               *   /var/log/arca/app_stdout.log            */
    bool     reload;          /**< the path changed under it: `pcm` is the OLD
                               *   file's and must be freed before the next load.
                               *   ⚠️ Deferred to the next TRIGGER rather than done
                               *   in audio_fx_set_path(), because a voice may be
                               *   sounding this `pcm` right now and the mixer
                               *   holds the pointer — freeing there is a
                               *   use-after-free, not a silence               */
} AudioClip;

/** One playing instance of a clip: the clip, a cursor into it, and the scratch
 *  the mixer pulls through.  ⚠️ **`buf` and `pos` are the mixer's `ctx`, so a
 *  live voice's `AudioClipVoice` must not be moved** — same rule, same reason as
 *  `AudioSampleVoice` above. */
typedef struct {
    const AudioClip *clip;    /**< what it is playing; NULL when never used      */
    long     pos;             /**< THE per-trigger cursor — see the note above   */
    int16_t *buf;             /**< mixer scratch, allocated on first use, kept    */
    long     buf_frames;      /**< its size                                      */
    int      slot;            /**< mix-bus slot, -1 when idle                     */
    uint32_t gen;             /**< its generation, so a REUSED slot is not read   */
} AudioClipVoice;

/** The canned sounds, and the order everything here indexes them in. */
typedef enum {
    AUDIO_FX_BEEP = 0,        /**< audio_beep()    — UI click, tile place        */
    AUDIO_FX_BLIP,            /**< audio_blip()    — item collected             */
    AUDIO_FX_SUCCESS,         /**< audio_success() — level up, milestone        */
    AUDIO_FX_FAIL,            /**< audio_fail()    — lost life                  */
    AUDIO_FX_GAMEOVER,        /**< audio_gameover() — the run is over, distinct
                               *   from FAIL because losing ONE life and losing
                               *   the game were the same sound and a player
                               *   could not tell them apart (operator, 2026-08-22) */
    /* ── The six DURING-PLAY names ────────────────────────────────────────
     * ⚠️ **These are not a design; they are the mapping the operator already
     * wrote down.**  `../sounds/prompts.md`'s table names the game event each
     * file was RECORDED for, and every one of these six was sourced for a site
     * that went on calling raw `audio_tone()` — so the panel played a generated
     * pitch while the sourced file sat on the device unreferenced.  Reported by
     * ear on `.188` 2026-08-23: "the brick thud, explosion and break plays the
     * stock ping, not the effect".  ⚠️ No gate could see it — the assets gate
     * measures a file's FORMAT and its ENERGY, never whether anything plays it;
     * `../check-sound-assets.sh` gained the claim check with this change. */
    AUDIO_FX_KNOCK,           /**< audio_knock()   — ball off the paddle/bat    */
    AUDIO_FX_THUD,            /**< audio_thud()    — a hit that did NOT destroy */
    AUDIO_FX_TICK,            /**< audio_tick()    — a brick destroyed          */
    AUDIO_FX_SPARKLE,         /**< audio_sparkle() — a bonus / rare reward      */
    AUDIO_FX_BURST,           /**< audio_burst()   — an explosion, chain blast  */
    AUDIO_FX_JUMP,            /**< audio_jump()    — a jump / stomp             */
    AUDIO_FX_COUNT
} AudioFxId;

/** How many clip triggers can sound at once.  Bounded by AUDIO_MAX_VOICES (8)
 *  anyway; 4 leaves room for the bed and a chained tone beside them. */
#define AUDIO_CLIP_VOICES        4

/** Mixer scratch per clip voice, in frames.  ⚠️ Small on purpose: the doc on
 *  `audio_mix_add_sample()` says size this at a device period *because* it sets
 *  how often the SD read is entered — and a clip's `fill` is a `memcpy` from RAM
 *  with no read behind it, so the only cost of a small buffer is more memcpys. */
#define AUDIO_CLIP_VOICE_BUF_FRAMES  1024

/** Longest file the clip loader will accept, in frames — 4 s at 44100.
 *  ⚠️ **This is the guard that stops a MUSIC path being RAM-loaded**: the config
 *  keys below are strings, and `fx_fail=/opt/sound/officerunner1-mono.wav` would
 *  otherwise malloc 7.8 MB inside a tap. */
#define AUDIO_CLIP_MAX_FRAMES    176400L

/** Room for a clip path from config.  Device paths are `/opt/sound/fx_fail.wav`. */
#define AUDIO_FX_PATH_MAX        128

typedef struct {
    int      sample_rate;     /**< Negotiated sample rate (read back, typ. 44100) */
    int      channels;        /**< Negotiated channel count (READ BACK, not a
                               *   literal).  Every byte count derives from this;
                               *   see audio_gen.h.  Never 0 on an open device.  */
    bool     available;       /**< true iff the stream opened: every entry point
                               *   that adds a voice gates on this alone          */
    /* ── streaming theremin state: ONE oscillator, in audio_gen.h ── */
    AudioOsc osc;             /**< phase + frequency glide + amplitude ramp        */
    bool     streaming;       /**< true while streaming audio chunks               */
    /* ── the mix bus: the only sound source, armed by audio_init() ── */
    AudioMixer mix;           /**< the voices every sound becomes                  */
    int16_t *pump_buf;        /**< render scratch, allocated once on first pump    */
    long     pump_buf_frames; /**< its size                                       */
    uint32_t pump_starved;    /**< pumps that found the ring DRY with audio still
                               *   owed — one audible gap each, and the number
                               *   that separates "the mixer is wrong" from "the
                               *   render loop was too slow to feed it"           */
    uint32_t pump_lost;       /**< frames rendered (so the voices advanced past
                               *   them) that the device did not take — silent
                               *   data loss, mirrored from audio_out_lost()       */
    long     pump_lead;       /**< the lead audio_pump() LAST TARGETED, in frames,
                               *   0 until a pump has read the device's period.
                               *   ⚠️ Recorded because it is derived from fragsize
                               *   and is NOT AUDIO_PUMP_LEAD_MS — a diagnostic
                               *   that printed the constant said 80 ms while the
                               *   library was holding ~139                        */
    long     pump_period;     /**< the device period the lead was rounded to, in
                               *   frames.  Reported beside the lead so the panel
                               *   shows the arithmetic, not just its result        */
    /* ── the continuous stream: the whole device half lives in audio_out.c ── */
    AudioOut out;             /**< the one never-reset stream, opened by
                               *   audio_init(); audio_out_is_open() says so       */
    bool     osc_stream;      /**< the theremin owns the fill callback             */
    bool     reopening;       /**< the device was lost; audio_pump() retries the
                               *   open, at most once per AUDIO_REOPEN_MS          */
    uint32_t reopen_last_ms;  /**< when the last reopen was attempted             */
    /* ── the level, and it is TWO knobs doing different jobs (audio_gen.h) ── */
    int      vol;             /**< per-voice volume, 0..AUDIO_VOL_UNITY.  Every
                               *   tone, voice and theremin amplitude derives from
                               *   it through audio_voice_peak(), so there is one
                               *   place to change how loud this process is        */
    int      master_shift;    /**< the device stage, applied by audio_out, which
                               *   holds its own copy (ScummVM has no `Audio`)     */
    int      last_tone_slot;  /**< the voice audio_tone() added last, and its
                               *   generation.  ⚠️ A later tone queues behind THIS
                               *   voice's tail, never behind the whole bus's:
                               *   see audio_mix_voice_pending()                    */
    uint32_t last_tone_gen;
    uint32_t last_tone_ms;    /**< when that tone was ISSUED, not when it ends.
                               *   ⚠️ Chaining is only correct for notes meant as
                               *   one motif, so it is gated on this being RECENT
                               *   (AUDIO_TONE_CHAIN_MS in audio.c) — without the
                               *   gate an unrelated tap inherited a 3 s drone's
                               *   tail and mixing became a queue                   */
    /* ── recorded PCM: one bed and one effect, both streamed ────────────────
     * TWO instances of one mechanism.  The bed is the LONGEST voice on the bus,
     * the effect the shortest — the pair audio_gen.h's refuse-never-steal rule
     * was written for.
     */
    AudioSampleVoice music;   /**< the bed: long, usually looping                 */
    AudioSampleVoice sfx;     /**< one-shot recorded effect, over the bed         */
    const AudioFdIo *fd_io;   /**< how audio_music_start_fd() reads its fd; NULL = read()/close() (audio_set_fd_io) */
    bool     music_gain_set;        /**< audio_music_set_gain() has been called           */
    int      music_gain_total_mdb;  /**< its total after the cap, applied to every music start */
    /* ── the canned sounds' CONTENT: four clips, four voices ────────────────
     * ⚠️ Loaded lazily, on the first trigger of each name — not at init, because
     * a game that never fails should not pay for fx_fail, and because a device
     * with no sound files must still make sounds (the note tables below).
     */
    AudioClip      fx[AUDIO_FX_COUNT];      /**< the loaded PCM, one per name    */
    AudioClipVoice fxv[AUDIO_CLIP_VOICES];  /**< the concurrent triggers          */
    char           fx_path[AUDIO_FX_COUNT][AUDIO_FX_PATH_MAX];
                              /**< where each name's clip lives.  Defaults set by
                               *   audio_init()/audio_init_unchecked(); an EMPTY
                               *   string means "use the note table", which is how
                               *   a config file switches one name back to tones  */
    /* ── the two operator toggles from the games menu ────────────────────────
     * ⚠️ **Both default TRUE and only audio_init() lowers them**, which is what
     * keeps audio_init_unchecked() a real bypass: a hardware speaker test must
     * make a noise on a device whose operator has silenced the games.
     * `audio_enabled` remains the master and is checked before either — see
     * audio_init().  These gate the LIBRARY entry points — audio_tone(),
     * audio_fx_play() and audio_sfx_play() for effects, audio_music_start() for the
     * bed — so a game needs no code of its own to honour them.
     * ⚠️ **audio_stream_start() is deliberately NOT gated.** Its only caller is
     * `theremin/theremin.c` (Tap-a-Theremin), an instrument whose entire purpose
     * is to make a noise on demand; a games-menu toggle that silenced it would leave
     * an app that does nothing.
     */
    bool     music_on;        /**< `music_enabled`   config key, default true      */
    bool     effects_on;      /**< `effects_enabled` config key, default true      */
} Audio;

/**
 * Initialise audio subsystem.
 *  - Honours the `audio_enabled` config setting
 *  - Opens the continuous stream (`audio_out_open_default()`: GPIO12, the
 *    device, the backend and the read-back all live there) and puts the mix bus
 *    on it as the stream's fill, so the struct is ready to pump on return
 * Returns 0 on success, -1 if hardware unavailable (game may continue).
 */
int  audio_init(Audio *audio);

/**
 * The same, WITHOUT the config gate.
 *
 * For a hardware *test* that must be able to drive the speaker even when the
 * user has switched audio off in config — `control_panel` has
 * such a page, and it and a since-deleted settings tool both used to hand-roll the open, the three ioctls
 * and the GPIO12 poke themselves.  That duplication is what this exists to
 * remove; the bypass itself is deliberate, because audio_init() would make the
 * test obey the very setting it exists to test.
 *
 * ⚠️ Any other caller wants audio_init().  A struct filled by hand instead of
 * by one of these two leaves `channels` at 0 and goes SILENTLY mute.
 *
 * ⚠️ The bypass is of the `audio_enabled` gate ONLY.  The output DEVICE preference
 * (`audio_device`) IS honoured, read from the saved config by this call, because a
 * speaker test that ignores it drives the wrong hardware and then reports a
 * verdict about the device it never opened.  A caller therefore does NOT set the
 * preference itself — doing so before this call is overwritten, and both callers
 * that ever tried simply forgot, which is why it moved in here.
 */
int  audio_init_unchecked(Audio *audio);

/**
 * audio_init_unchecked(), opening on `pref` instead of the SAVED `audio_device`.
 *
 * For a settings screen whose device choice is not saved yet: its speaker test
 * must drive the device the screen shows, not the one on disk.  `pref == NULL`
 * means the saved one — audio_init_unchecked() is exactly that call, so the two
 * share one body.  Same gate bypass, same overwrite of any earlier preference.
 */
int  audio_init_unchecked_pref(Audio *audio, const char *pref);

/**
 * Drain the stream (bounded), close it and release resources.
 * Safe to call even if audio_init() failed.
 */
void audio_close(Audio *audio);

/**
 * Stop every voice on the mix bus.
 *
 * ⚠️ **It does not reset the device** — that reset is precisely what makes mixing
 * impossible — so up to one lead (~139 ms) of already-written tail still sounds.
 * ⚠️ **Never put it before an effect.** The seven games no longer call it at all —
 * every mention left in them is a comment saying not to.  It stops the OTHER voices,
 * so an `audio_interrupt(); audio_tone();` pair does not mean "replace what is
 * playing": it cuts whatever else was sounding, which for a game effect is a defect
 * and not the intent the pair used to carry.
 */
void audio_interrupt(Audio *audio);

/**
 * Play a sine-wave tone through SPKR1.
 *
 * Does NOT write and does NOT block: it adds a voice to the mix bus and
 * audio_pump() delivers it, summed with whatever else is sounding.
 *
 * @param freq_hz     Frequency 20–8000 Hz
 * @param duration_ms Duration in milliseconds (clamped to AUDIO_MAX_TONE_MS)
 */
void audio_tone(Audio *audio, int freq_hz, int duration_ms);

/* ── The mix bus and its per-frame service ─────────────────────────────────
 *
 * Two sounds at once needs userspace to hold the audio and hand the device
 * small pieces of it, because you cannot mix into a buffer the kernel already
 * has.  This library does that from the render loop — never a thread: static
 * ARM plus pthread is the SIGSEGV-before-main() scar (../CLAUDE.md).
 *
 * ⚠️ **It is not optional.**  audio_init() opens the stream with the bus as its
 * fill, so every sound waits in the mixer until something services it.  An app
 * owes two lines beside audio_init():
 *
 *     audio_init(&audio);
 *     while (running) {
 *         ...
 *         audio_pump(&audio);                 // once per frame, next to fb_swap()
 *         usleep((drew || audio_pump_active(&audio)) ? FRAME_DELAY_ACTIVE_US
 *                                                    : FRAME_DELAY_IDLE_US);
 *     }
 *
 * ⚠️ **That last line matters.** The stream keeps only one lead (~139 ms on the
 * OSS shim) inside the device and must be serviced within
 * audio_cont_service_interval_us(), so a loop that drops to FRAME_DELAY_IDLE_US
 * (100 ms) starves it and you hear a gap.  audio_pump_active() is the same idiom
 * as gameover_needs_redraw(): the component is asked, because it is the only
 * thing that knows it still owes the device frames.  `check-audio-pacing.sh`
 * gates both lines in every app that calls audio_init().
 */

/** Service the continuous stream: render the bus and write whatever the lead is
 *  short by.  Call once per frame.  A no-op when the stream is not open. */
void audio_pump(Audio *audio);

/** True while the stream is open — see the frame-pacing note above.  ⚠️ True
 *  whatever the bus is doing: a silent bus still writes silence, and an idle
 *  stream is the transition (and the click) the stream exists to remove. */
bool audio_pump_active(const Audio *audio);

/** The system equaliser on this stream (audio_eq.h), live and click-free; a forward to audio_out_set_eq() on
 *  the Audio's own stream.  Safe to call when the stream failed to open (then it does nothing). */
void audio_set_eq(Audio *audio, const int gains_db[EQ_BANDS]);
void audio_get_eq(const Audio *audio, int gains_db[EQ_BANDS]);
/** The enhancer (audio_enh.h) on this stream, live; forwards to audio_out_set_enh()/get. */
void audio_set_enh(Audio *audio, const AudioEnh *e);
void audio_get_enh(const Audio *audio, AudioEnh *e);

/** Hold for `ms` while SERVICING the bus, for a caller with no render loop.
 *
 * ⚠️ **A tone lives in the MIXER until something pumps it, and `audio_close()`
 * does not pump.**  `audio_out_close()`'s drain waits on the device's own
 * `in_flight` and never calls the fill, so it rescues only what is already
 * *inside* the device — and a tone nobody pumped is not, so it is lost.  This is
 * the frame-pacing clause above, applied to the one shape it does not cover: a
 * hardware test that plays a tone, waits, and closes, with no loop to hang an
 * `audio_pump()` on.
 *
 * With no stream open (audio disabled, or the open failed) it is a plain
 * `usleep()`.  It does NOT poll touch — a loop that must stay responsive keeps
 * its own `audio_pump()` beside its own poll instead.
 *
 * One implementation for the same reason `audio_init_unchecked()` is one: this
 * was going to be a four-line pump loop copied into two settings tools,
 * `control_panel.c` among them, and a copy like that goes silently wrong the moment the
 * service ceiling moves. */
void audio_hold_serviced(Audio *audio, int ms);

/** Voices occupying a slot right now (0..AUDIO_MAX_VOICES). */
int  audio_pump_voices(const Audio *audio);

/** Samples the summed bus could not store in an int16.
 *
 * ⚠️ **Under the default soft limiter this must read 0**, because the curve
 * asymptotes below full scale — so this is the negative control for the limiter
 * being engaged at all, not a loudness gauge.  It read **15402** on `.188`
 * 2026-08-15 with the hard clamp, which a panel heard as an overdriven square
 * wave. */
uint32_t audio_pump_clipped(const Audio *audio);

/** Samples the soft knee bent — the sum was louder than one voice can be.
 *  Expected to be large whenever sounds overlap; it is a level indicator, not a
 *  fault. */
uint32_t audio_pump_limited(const Audio *audio);

/** Pumps that found the ring dry while voices still owed audio: one audible gap
 *  each.  ⚠️ **This is the number that tells a mixing defect from a PACING one** —
 *  the stream holds only one lead, so any render-loop iteration longer than that
 *  starves the device however correct the mix is. */
uint32_t audio_pump_starved(const Audio *audio);

/** Frames the bus rendered — advancing its voices past them — that the device
 *  refused to take.  The service never blocks, so a full ring drops them; they
 *  are gone, and a non-zero count is a discontinuity in the waveform. */
uint32_t audio_pump_lost(const Audio *audio);

/** The lead the pump LAST TARGETED, in frames, and the device period it was
 *  rounded up to — both 0 until a pump call has read the period off the device.
 *
 * ⚠️ **A diagnostic must report what the library did, not what the header asked
 * for.**  `AUDIO_PUMP_LEAD_MS` is a *request*: `audio_pump_lead_frames()` floors
 * it at AUDIO_PUMP_LEAD_PERIODS whole periods and rounds up, which on the OSS
 * shim's 46 ms period makes the effective lead ~139 ms rather than 80.
 * `tests/audio_mix_test` printed the constant and so displayed `lead 80 ms` while
 * the library held nearly twice that — and it coloured its worst-frame warning
 * against the same wrong number.  Use these; `audio_ms_for_frames()` converts.
 *
 * They are a *measurement*, so they stay at 0 rather than falling back to the
 * constant: "not yet measured" and "80 ms" are different claims. */
long audio_pump_lead(const Audio *audio);
long audio_pump_period(const Audio *audio);

/** Choose how the summed bus leaves the mixer: AUDIO_MIX_HARD (the default, and
 *  ScummVM's saturating add) or AUDIO_MIX_SOFT, this repo's knee, kept so a panel
 *  can A/B them. */
void audio_pump_set_limit(Audio *audio, int mode);

/**
 * The per-voice volume, 0..AUDIO_VOL_UNITY — how loud this process is, as a
 * fraction of full scale.
 *
 * ⚠️ **This is the HEADROOM knob and `audio_set_master_shift()` is the speaker
 * knob; they cancel acoustically but are not interchangeable.** `n` voices reach
 * the clamp when `n * vol > AUDIO_VOL_UNITY`, so halving the volume buys twice
 * the polyphony while halving the shift buys none.  Setting the volume also
 * re-derives the soft knee, without which a lone tone would be bent by a limiter
 * still knee'd at a louder amplitude.
 *
 * ⚠️ Takes effect on voices added AFTER it: a sounding voice keeps the amplitude
 * it was created with, which is what stops a ladder pad from stepping the tone
 * it is being judged on.  `audio_get_volume()` reads it back for a log line —
 * from the library, never from a pad's label.
 */
void audio_set_volume(Audio *audio, int vol);
int  audio_get_volume(const Audio *audio);

/**
 * The device attenuation stage, in bits — `1` is ScummVM's `>>1`.
 *
 * Applied by `audio_out` to everything the stream writes. ⚠️ An arithmetic shift,
 * never a multiply: `audio_attenuate()` carries the reason.
 */
void audio_set_master_shift(Audio *audio, int shift);
int  audio_get_master_shift(const Audio *audio);

/** Sounds refused because all AUDIO_MAX_VOICES slots were busy.  A full bus
 *  never steals a playing voice; see audio_gen.h. */
uint32_t audio_pump_dropped(const Audio *audio);

/* ── The continuous stream ──────────────────────────────────────────────────
 *
 * The click fix, and the only mode.  A SNDCTL_DSP_RESET before every sound made
 * every game sound a full stream stop and start, and every boundary a DAI
 * teardown that clicks — the operator's *"every time there is a sound, there is
 * a click"*.  `common/audio_out.c` is the device half that never resets, and
 * audio_init() opens it.  Three things follow, all measured or derived:
 *
 *   - **The mix bus is the only sound source**, because a continuous stream needs
 *     something to fill it every service — silence included.
 *   - **audio_pump() is the service call** and audio_pump_active() is always true
 *     while the stream is open; the ceiling for how often is
 *     audio_cont_service_interval_us().
 *   - ⚠️ **audio_tone() defaults its delay to the preceding tone's TAIL while that
 *     tone is recent**, or `tetris.c`'s and `snake.c`'s back-to-back two-note
 *     motifs turn into dyads — a mix bus does not serialise them the way a kernel
 *     ring did.
 */

/** The mix bus as an `audio_out` fill: render mono, expand to the granted
 *  channel count.  audio_init() installs this itself, so no app calls it — it is
 *  exported for two callers that are not apps:
 *
 *   - a host regression that drives the REAL continuous path with a file-backed
 *     `AudioOutDev` instead of `/dev/dsp` (`tests/audio_path_dump.c`).  ⚠️ This
 *     is the point: a test that hand-transcribes the chain out of
 *     `audio_gen.c` primitives never covers the
 *     DELIVERY — the per-service chunking and the bus state carried across
 *     chunks.  A test that re-implements the fill would have the same hole.
 *   - a ScummVM adapter, which becomes another fill beside this one.
 */
long audio_cont_fill_mix(void *ctx, int16_t *buf, long frames, int channels);

/** How often audio_pump() must be called on the continuous stream, in
 *  microseconds, or 0 when nothing has measured the device yet.  Derived from
 *  REAL audio rather than the nominal lead — see audio_out.h. */
long audio_cont_service_interval_us(const Audio *audio);

/* ── Convenience sounds ────────────────────────────────────────────────────
 * Each adds one voice per note, each offset by the notes before it — so
 * `audio_success()` is an ascending arpeggio and not a chord, and it mixes with
 * whatever else is sounding instead of discarding it.  None of them waits.
 *
 * ⚠️ **Each is backed by a recorded CLIP when one is configured and loads, and
 * that is the AUDIBILITY fix**.  Every one of the four note tables is a
 * sustained pure tone, and this speaker rolls off sharply below ~700 Hz and is
 * inaudible below ~300 at viewing distance (../SYSTEM_ANALYSIS.md#34-audio):
 * every sound ever reported clear is ≥ 880 Hz and every one reported faint is
 * ≤ 500 Hz — `audio_fail()`'s 392/330/262 Hz included, reported *still not
 * audible under a music bed* on 2026-08-21.  The four stock clips are broadband
 * and land inside the passband (`fx_fail` sweeps 1800→420 Hz), so the fix is one
 * of CONTENT: no pitch is guessed, no level is raised — and there is no headroom
 * to raise it into anyway, the first non-zero `clip` count (126 samples) came
 * from that same session — 126 of ~18.7 M samples, a music bed and an effect
 * summing past full scale, so the LEVEL is settled and any future "make it
 * louder" is a content change, not a level one.
 *
 * The note table is the FALLBACK, not the legacy: a device with no sound files
 * still makes every one of these sounds.
 */

/** The speaker-test chime: 880 Hz 200 ms, then 1320 Hz 200 ms starting 250 ms
 *  in.  Both voices are queued at once with start delays, so it returns at once
 *  and the caller's audio_pump() plays it — for a screen that holds the bus
 *  open, not one that opens and closes it around each press.  No clip. */
void audio_test_chime(Audio *audio);

/** Short 880 Hz blip (~80 ms)  — UI click, tile place, button press */
void audio_beep(Audio *audio);

/** Short 1320 Hz blip (~60 ms) — item collected, food eaten           */
void audio_blip(Audio *audio);

/** C5→E5→G5 ascending arpeggio (~440 ms) — score milestone, level up */
void audio_success(Audio *audio);

/** G4→E4→C4 descending tone  (~600 ms)   — ONE life lost, error       */
void audio_fail(Audio *audio);

/** Descending 4-note fall (~680 ms) — the RUN is over, not one life.
 *  ⚠️ Its note table is deliberately ABOVE the knee (1046/880/740 Hz) unlike
 *  audio_fail()'s, so the fallback is audible on this speaker without the clip.
 *  Clip: fx_gameover, the loudest-in-band file of the set. */
void audio_gameover(Audio *audio);

/* ── The six during-play effects ─────────────────────────────────────────────
 *
 * Same shape as the five above: try the clip, fall back to ONE note.  ⚠️ **Every
 * fallback pitch here was RAISED when the site got a name** — the five
 * `brick_breaker` sites played 600/800/1000/1200/1500 Hz and the first two are at
 * or below the speaker's ~700 Hz knee ([§3.4](../SYSTEM_ANALYSIS.md#34-audio)).
 * The rule this repo already wrote for the moment a raw `audio_tone()` site gets
 * a name is *retune the spec, never the threshold*, so the notes are lifted to
 * 880 Hz and above while KEEPING the author's pitch ORDER — paddle < thud <
 * burst < tick < sparkle — which is the part a player actually reads.
 */

/** ~880 Hz, 30 ms — the ball off a paddle or bat.  Clip: fx_knock.
 *  ⚠️ **The weakest-measured file of the eleven drives the most FREQUENT event**
 *  (−21.66 dBFS in-band at a −3.05 dB band delta, `../check-sound-assets.sh`).
 *  That is the argument for re-sourcing it, and it is not a reason to leave the
 *  site on a 600 Hz tone the panel cannot radiate at all. */
void audio_knock(Audio *audio);

/** ~988 Hz, 20 ms — a hit that did NOT destroy its target.  Clip: fx_thud. */
void audio_thud(Audio *audio);

/** ~1319 Hz, 25 ms — a brick destroyed, a block cleared.  Clip: fx_tick. */
void audio_tick(Audio *audio);

/** ~1319 Hz, 40 ms — a start menu calling for attention (start_menu.c pings
 *  on the brackets' blink).  The tick's pitch, which the panel radiates, held
 *  longer so it reads as a call rather than a hit.  A plain audio_tone(): no
 *  clip, so the EFFECTS toggle is audio_tone()'s own gate. */
void audio_ping(Audio *audio);

/** ~1568 Hz, 30 ms — a bonus brick, a rare reward.  Clip: fx_sparkle, which has
 *  the FLATTEST band delta of the set (−0.05 dB). */
void audio_sparkle(Audio *audio);

/** ~1175 Hz, 40 ms — an explosion or a chain detonation.  Clip: fx_burst. */
void audio_burst(Audio *audio);

/** ~1046 Hz, 40 ms — a jump or a stomp.  Clip: fx_jump.
 *  ⚠️ `platformer`'s jump called `audio_beep()` (fx_click, a UI tap) while
 *  `fx_jump.wav` — sourced for "jump / stomp" — reached nothing. */
void audio_jump(Audio *audio);

/**
 * Play one canned name's CLIP, with no note-table fallback.  Returns true iff a
 * voice was added.
 *
 * The four functions above are `audio_fx_play()` then the notes if it said no, so
 * this is only for a caller that wants to know which one it got — a test, or a
 * pad measuring the clip against the tones.  Loads the file on first use.
 *
 * ⚠️ Refused (false, quietly) when there is no stream (`available` false) — the
 * caller then falls to its notes, which audio_tone() refuses the same way.
 */
bool audio_fx_play(Audio *audio, AudioFxId id);

/**
 * Point one canned name at a different clip file, or at "" for the note table.
 *
 * The path a game names in its own config arrives here.  ⚠️ **The clip loaded
 * under the old path is freed at the next TRIGGER, not here**, because a voice
 * may be sounding it and the mixer holds the pointer — freeing under a live
 * voice is a use-after-free, not a silence.  So a swap takes effect on the next
 * trigger, and if the old clip is still sounding at that moment the trigger
 * falls back to the note table for that one tap.  Passing the path already set
 * is a no-op, so this is safe to call every frame.
 */
void audio_fx_set_path(Audio *audio, AudioFxId id, const char *path);

/* ── Recorded PCM: a music bed and a sample effect ──────────────────────────
 *
 * A sample voice is an `AudioVoice` on the mix bus like any tone.  Every refusal
 * below is LOUD: silently doing nothing would read on the panel as "the file is
 * broken", which is the wrong repair.
 *
 * ⚠️ **They REFUSE a rate mismatch: there is no resampler here and there is not
 * going to be one.**  Every file we have is
 * 44100 / mono / 16-bit — measured on `.188` 2026-08-20 for the three
 * `/opt/sound/asl_*.wav`, and on the committed bytes 2026-08-22 for all 24
 * `native_apps/music/` beds (RIFF `fmt ` chunk: PCM, 1 channel, 44100, 16) —
 * and so is what `hw:0,0` grants.  Guessing a rate is a pitch bug that sounds
 * like a bad recording, which is the hardest kind of audio fault to attribute.
 *
 * The read is a PULL from the render loop (`common/audio_wav.c` →
 * `audio_wav_fill`), entered once per read-ahead buffer rather than once per
 * frame, so `audio_gen.c` still has no fd and this file still owns every one.
 */

/**
 * Start the music bed from `path`, looping if asked.
 *
 * Returns true if a voice was added.  Refused — with a reason on `stderr` — when
 * the file will not open, its rate is not the device's, the bus is full, or a
 * previous bed is still sounding (and quietly when there is no stream at all).
 *
 * ⚠️ **`loop` does not mean "forever": it means a large finite total**, because
 * `audio_mix_add_sample()` needs a real `total_frames` for `audio_mix_pending()`
 * and the self-free to behave (see `audio_gen.h`).  AUDIO_MUSIC_LOOP_PASSES in
 * `audio.c` sets it, and the total is clamped so the frame count cannot overflow
 * a 32-bit `long`.
 */
bool audio_music_start(Audio *audio, const char *path, bool loop);

/**
 * Play a WAV arriving on `fd` (a socket, a pipe): `channels` (1 or 2) of
 * 16-bit at the granted rate, anything else refused; header sizes of 0xFFFFFFFF
 * mean until EOF.  The fd is
 * made O_NONBLOCK and the call never waits; `audio_pump()` pulls from it.  Rules
 * (pre-roll ~1 s, silence on underrun, EOF ends the track) are in `audio_wav.h`.
 * Takes ownership of `fd` only when it returns true (closed on the next start, or
 * by `audio_close()`).  A bad header that has not arrived yet is logged later and
 * ends the track.  Not pausable.  `audio_music_stream_buffered()` is the status.
 * A stereo bed on a mono device is folded to (L+R)>>1 by the mix; on a stereo
 * device L and R stay apart (`audio_cont_fill_mix`).
 */
bool audio_music_start_fd(Audio *audio, int fd, int channels);
/** Route later audio_music_start_fd() streams through `io` (a TLS socket's reader and
 *  closer; NULL = plain read()/close(), the default).  Call after audio_init(). */
void audio_set_fd_io(Audio *audio, const AudioFdIo *io);
long audio_music_stream_buffered(const Audio *audio);
long audio_music_stream_underruns(const Audio *audio);

/**
 * Set the music voice's gain: `norm_mdb` (a track's normalisation gain) plus
 * `user_mdb` (the volume setting), both milli-dB, summed and capped by
 * `audio_gain_total_mdb()`.  A live voice slews to it in ~20 ms; with no music
 * voice sounding it is remembered and applied to the NEXT start (file, stream
 * or resume).  The effect is relative to the `audio_set_volume()` level.
 */
void audio_music_set_gain(Audio *audio, int norm_mdb, int user_mdb);

/** The total set by `audio_music_set_gain()` after the cap (0 until it is called). */
int  audio_music_get_gain_mdb(const Audio *audio);

/**
 * Stop the bed at the end of its release, not now.
 *
 * ⚠️ **It does NOT close the file or free the buffer**, and it must not: the
 * envelope walks the voice down to 0 over the following frames and the mixer
 * pulls from this exact `ctx` until it gets there.  Cutting instead would step
 * the summed bus by the bed's whole instantaneous amplitude — an audible click.
 * `audio_music_active()` reads false once the fade has finished, and that is
 * when a restart is accepted.
 */
void audio_music_stop(Audio *audio);

/** True while the bed still owes frames — read from the MIXER by (slot,
 *  generation), not from a flag of this file's own, so a bed that
 *  audio_interrupt() cleared out from under it reads false rather than stuck. */
bool audio_music_active(const Audio *audio);

/**
 * Hold the bed where it is, so a resume continues the track instead of restarting it.
 *
 * Arms the same release as `audio_music_stop()` — a bed is never cut, because
 * stepping the summed bus by its whole instantaneous amplitude is a click — but
 * keeps the file OPEN at its read position, which is what `audio_music_resume()`
 * re-arms a voice over.  Returns true if a live bed was held.
 *
 * ⚠️ **The pump must keep running after this call**, or the release never
 * finishes and the resume is refused: the mixer advances by frames RENDERED, so
 * a game that stops servicing the bus while "paused" has paused the fade too.
 *
 * ⚠️ **A resume can skip up to one read-ahead buffer (~93 ms).** Frames the
 * mixer had already pulled out of the file but not yet rendered are gone with the
 * voice; `wav.pos` counts what was READ, not what was heard.  Inaudible in a bed
 * and wrong for anything where sample-exact continuation matters.
 */
bool audio_music_pause(Audio *audio);

/**
 * Re-arm the held bed from where it stopped.  Returns true if a voice was added.
 *
 * Refused — with a reason on `stderr` — when nothing is held, when the previous
 * release has not finished (`audio_music_active()` is still true), or when the bus
 * is off or full.  ⚠️ **It re-opens NOTHING**: the ctx handed to the mixer is the
 * same still-open `AudioWav`, which is why the track continues rather than
 * retriggering, and why `audio_music_stop()`'s promise not to close the file is
 * load-bearing rather than an implementation detail.
 */
bool audio_music_resume(Audio *audio);

/**
 * Play one recorded effect over whatever else is sounding. Returns true if a
 * voice was added; same refusals as the bed, plus one of its own.
 *
 * ⚠️ **A second tap is refused while the first effect is still sounding**, because
 * there is ONE `AudioSampleVoice` for effects and its `AudioWav` is the live
 * voice's `ctx`: rewinding it under the mixer would make the effect jump rather
 * than retrigger.  This is the pad that answers the real question —
 * sampled material over sampled material, on a speaker that makes two sustained
 * sines harsh.
 */
bool audio_sfx_play(Audio *audio, const char *path);

/**
 * The two games-menu toggles, as read by audio_init().
 *
 * ⚠️ **Ask these to explain a SILENCE, never to decide whether to make a sound** —
 * the library already refuses; a game that also checks is a second place the rule
 * can drift. What they are for is the log line and the state machine: `platformer`'s
 * bed uses `audio_music_enabled()` so a bed that is OFF prints "disabled" rather
 * than "no music bed at <path>", which is a different fault to chase later.
 *
 * ⚠️ Both read TRUE on a struct from audio_init_unchecked(), by design — see the
 * fields' comment in the struct above.  Both also read TRUE when `audio_enabled` is
 * false, because that gate short-circuits before them and `available` is what says so.
 */
bool audio_music_enabled(const Audio *audio);
bool audio_effects_enabled(const Audio *audio);

/* ── Streaming (theremin) API ──────────────────────────────────────────────
 * For continuous pitch-gliding audio driven by a touch loop.
 *
 * It runs on the same continuous stream audio_init() opened, by swapping the
 * stream's fill from the mix bus to one gliding oscillator and back — no reset
 * and no chunk loop of its own.  Its only caller is `theremin/theremin.c`.
 */

/**
 * Begin streaming — takes over the fill callback with one gliding oscillator.
 *
 * ⚠️ Refused LOUDLY if the mix bus still has voices pending: a swap that went
 * quiet would leave those voices enqueued into a mixer nobody renders.
 */
void audio_stream_start(Audio *audio, int freq_hz);

/**
 * Service the stream — call every frame while the user is touching.
 * Non-blocking; equivalent to audio_pump() with the oscillator installed.
 */
void audio_stream_chunk(Audio *audio);

/**
 * Update the target frequency for smooth glissando.
 * Call whenever touch position changes.
 */
void audio_stream_set_freq(Audio *audio, int freq_hz);

/**
 * Stop streaming — removes the oscillator, then APPENDS its 20 ms fade-out so
 * the tone has a release rather than a cut.
 *
 * ⚠️ **The release lands one lead behind the finger** (~139 ms on the OSS shim),
 * because a continuous stream cannot un-write what is already queued and the
 * whole point is not to reset the ring.  That is a property of the design, not a
 * bug to fix here; the alternative is the click.
 */
void audio_stream_stop(Audio *audio);

#endif /* AUDIO_H */
