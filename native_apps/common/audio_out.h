#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

/**
 * audio_out — the output device half, and the ONE stream in this repo.
 *
 * The device is opened once, configured once, prefilled with silence and then
 * never reset and never reconfigured until shutdown.  That is the whole point:
 * a click is a stream transition, and every canned sound used to be a full stop
 * and start of the DAI (`audio.c`'s per-sound ring reset).  Native ALSA clicks
 * at a transition too, so no userspace API avoids it — only not stopping the
 * stream does.
 *
 * This is not a design.  `scummvm-arca/backend-files/oss-mixer.cpp` has
 * run exactly this — one open, prefill, deadline pacing, an arithmetic-shift
 * attenuation immediately before `write()`, no reset — on this device since
 * 2026-08-03, and Full Throttle's video, MIDI, speech and effects all play well
 * (operator, 2026-08-18).  This file is that architecture moved into shared
 * code, so there is ONE implementation to maintain when the next emulator port
 * arrives rather than a third copy of open/configure/prefill/pace/attenuate.
 *
 * Everything arithmetic stays in `audio_gen.c` and is unmodified: the lead, the
 * frame counts, the write loop, the mono→interleaved expansion.  What is here is
 * the four things that need a device — configure-and-read-back, how much room the
 * queue has, the write, and the drain — plus the two policies over
 * `audio_write_frames()` that this file's two write modes need.
 *
 * ── ONE `AudioOut` PER PROCESS ────────────────────────────────────────────────
 *
 * ⚠️ A second concurrent open of the device is refused *Device or resource busy*
 * (measured, ../SYSTEM_ANALYSIS.md#34-audio), so `audio_out_open*()` refuses a
 * second LIVE instance itself rather than letting the driver produce a confusing
 * EBUSY halfway through an init.  Sequential open/close pairs are fine, which is
 * what `control_panel` relies on: its one `Audio` is the Audio page's
 * (`control_panel/audio_page.c`), and the page closes it before it launches
 * `audio_mix_test`, which opens its own.
 *
 * ── THE THREADING CONTRACT ───────────────────────────────────────────────────
 *
 * There is no mutex in here, deliberately, because `native_apps` links no
 * pthread at all — static ARM plus pthread is the `clock_gettime64` →
 * SIGSEGV-before-`main()` scar (../CLAUDE.md) — and a `SCHED_RR` audio thread
 * starves this single 600 MHz core to a black screen.  So:
 *
 *   - `audio_out_open*()`, `audio_out_close()`, `audio_out_set_fill()` and
 *     `audio_out_set_shift()` are for ONE thread, and it must be the same thread
 *     for all of them.
 *   - `audio_out_service()` may run on a DIFFERENT thread from that one (ScummVM
 *     opens on its main thread and services from its audio thread), but only ever
 *     one thread at a time, and never concurrently with `set_fill`/`close`.
 *   - `audio_out_write()` — mode 2 — must run on the servicing thread, or on the
 *     owning thread while nothing is servicing.  It is refused outright while a
 *     fill callback is installed, which is what makes the common case safe by
 *     construction rather than by discipline.
 *   - The fill callback must not call back into any `audio_out_*` function.
 *   - The accessors are reads of one word and safe from anywhere; they are
 *     diagnostics, not synchronisation.
 *
 * ── THE TWO WRITE MODES, AND WHY BOTH ────────────────────────────────────────
 *
 * ⚠️ A service-driven library alone would SILENTLY MUTE two shipped Settings
 * tabs.  `control_panel.c` plays its speaker test tones
 * with **no render loop at all** — init, tone, `usleep`, tone, close — so nothing
 * would ever call `audio_out_service()` and the tones would sit in a callback
 * that is never invoked.  Measured objectively, not inferred.
 *
 *   mode 1, SERVICED    `audio_out_service()` from a render loop or an audio
 *                       thread.  Targets a lead; never sleeps.
 *   mode 2, SYNCHRONOUS `audio_out_write()` pushes a whole buffer with a bounded
 *                       blocking policy.  Costs whole-buffer CPU, which is free
 *                       on a static UI, and the stream still never resets.
 *
 * Both go through the same never-reset stream, and `audio_out_close()` DRAINS in
 * either mode — bounded — or the queued tail is thrown away at exit.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "audio_gen.h"
#include "audio_eq.h"
#include "audio_enh.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

/** What the driver's free-space report over-states the queue by, as a fraction
 *  of one period.
 *
 * ⚠️ **The device's free-space report OVER-STATES what is still queued, and this
 * is measured, not inferred.** On `.188` 2026-08-18 the OSS shim's `GETOSPACE`
 * disagreed with the kernel's own `buffer_size − avail` read at the same instant
 * by 2650–2670 frames on every `RUNNING` sample — ~60 ms at 44100, ~1.3 of the
 * shim's 2048-frame period.  That single number explains the whole interval sweep:
 * a NOMINAL 139 ms lead is ~79 ms of real audio, which is why a 66 ms service
 * interval survives with ~11 ms to spare and a 100 ms one starves ~2.5×/s.
 *
 * It is expressed as a fraction of the PERIOD because periods are the only unit
 * the shim moves in, and its period is 2048 frames at every rate and channel
 * count tested.  ⚠️ The mechanism — how much is the staged period and how much
 * a stale `hw_ptr` — is NOT established, so treat the fraction as the shape of
 * the measurement rather than as a law.
 *
 * ⚠️ **It is deliberately NOT subtracted from the in-flight figure.** Subtracting
 * it would make the library write more, deepening the queue and the onset latency
 * to fix a starvation the measurement says does not happen at the cadence the
 * library asks for.  It is used in exactly two places, both of which would
 * otherwise be a nominal number presented as audio: the service-interval ceiling
 * below, and the close-time drain's stop condition.  */
#define AUDIO_OUT_OSPACE_SLACK_NUM   13
#define AUDIO_OUT_OSPACE_SLACK_DEN   10

/** Retry interval for the two blocking policies, in microseconds.  The serviced
 *  policy's is **0** — see `audio_out_service()`. */
#define AUDIO_OUT_PREFILL_WAIT_US    1000
#define AUDIO_OUT_SYNC_WAIT_US       5000

/** Waits mode 2 is allowed beyond the ones its own length justifies.  The bound
 *  is derived from the buffer being written (see `audio_out_write()`), so a long
 *  tone is not truncated; this is only the floor for a short one. */
#define AUDIO_OUT_SYNC_WAIT_FLOOR    64

/** Poll interval while draining at close, in microseconds. */
#define AUDIO_OUT_DRAIN_WAIT_US      2000

/* ── The device, behind a vtable ─────────────────────────────────────────────
 *
 * Injectable for exactly one reason: it is what lets `tests/audio_out_test.c`
 * drive this file on the host with **no fd in the test at all** — a simulated
 * ring that can be told to over-report, to stall, to accept half a frame, or to
 * grant a channel count nobody asked for.  The one real implementation lives in
 * `audio_out.c` behind `audio_out_open_alsa()` and is
 * the only device code in the repo below this header.
 *
 * A backend's `space` or `write` that fails because the device is gone returns
 * -1 with errno `ENODEV` (and `*again` false); the generic layer turns that into
 * `audio_out_device_lost()`.
 */

typedef struct {
    long period_frames;  /**< one device period (the ALSA period size)       */
    long ring_frames;    /**< total capacity                                  */
    long in_flight;      /**< frames the device still holds — see the slack
                          *   constant above before believing this number     */
    long space;          /**< frames it will accept right now                 */
} AudioOutSpace;

/**
 * `open` must report what the device GRANTED, never what was asked for.
 *
 * ⚠️ Reading the granted **bits** back is not optional and closes a latent hole:
 * `oss-mixer.cpp` already warns when the device is not 16-bit and `audio.c`
 * never looked, so a device that quietly granted another width would have
 * produced noise with no diagnostic anywhere.  All three read-backs go through
 * this one call so no client can forget one.
 */
typedef struct {
    int  (*open)(void *ctx, int rate_req, int channels_req,
                 int *rate_granted, int *bits_granted, int *channels_granted);
    int  (*space)(void *ctx, int frame_bytes, AudioOutSpace *sp);
    ssize_t (*write)(void *ctx, const void *buf, size_t nbytes, bool *again);
    void (*wait)(void *ctx, int usec);
    void (*close)(void *ctx);
} AudioOutDev;

/**
 * Fill `frames` interleaved frames of `channels` channels into `buf`.
 *
 * ⚠️ **The buffer arrives ZEROED and a short fill is legal**: return the frames
 * actually produced and the remainder plays as the silence that is already
 * there.  `audio_mix_render()` deliberately does not touch the buffer on a silent
 * bus, which is exactly how stale scratch would otherwise leak into the stream.
 *
 * ⚠️ `channels` is an argument because the device grants it and no caller may
 * assume it (../CLAUDE.md, `audio_gen.h`).  The native client renders mono and
 * expands with `audio_interleave()`; ScummVM's hands `buf` straight to
 * `MixerImpl::mixCallback()`, which is why this speaks device frames rather than
 * mono samples.
 *
 * Must not call any `audio_out_*` function.
 */
typedef long (*AudioOutFill)(void *ctx, int16_t *buf, long frames, int channels);

/* ── The stream ─────────────────────────────────────────────────────────── */

typedef struct {
    const AudioOutDev *dev;
    void       *dev_ctx;
    bool        is_open;
    bool        device_lost;   /**< a write failed ENODEV — see the accessor   */

    /* Which /dev/dsp* node `audio_out_open_default()` opened (one of the resolver's
     * static strings, NULL behind any other opener), and the replug probe's
     * state — see audio_out_usb_returned().  All three reset on every open. */
    const char *open_path;
    uint32_t    reprobe_last_ms;
    bool        better_seen;   /**< a better sink was usable at the previous probe */

    /* What the device GRANTED.  Never what was requested. */
    int         rate;
    int         bits;
    int         channels;
    int         frame_bytes;
    bool        bits_warned;   /**< a non-16-bit grant was reported once       */

    /* Geometry, re-derived on every service because it is a MEASUREMENT: 0
     * until one has been taken, never a fallback to the requested constant. */
    long        period_frames;
    long        ring_frames;
    long        lead_frames;

    /* Post-fill attenuation, as a shift count.  See audio_out_set_shift(). */
    int         shift;

    /* System equaliser, applied before the attenuation. See audio_out_set_eq(). */
    EqParams    eq_p;
    EqState     eq_s;

    /* System enhancer, applied after the equaliser. See audio_out_set_enh(). */
    EnhParams   enh_p;
    EnhState    enh_s;

    /* Consecutive all-zero frames fed through attenuate(), and whether the DSP is
     * currently skipped for them (state reset on entry).  See attenuate(). */
    long        silent_frames;
    bool        dsp_idle;

    AudioOutFill fill;
    void        *fill_ctx;
    const char  *fill_owner;

    int16_t    *buf;           /**< interleaved device scratch, grown once     */
    long        buf_frames;

    /* Diagnostics.  Each means ONE thing — see the accessors. */
    uint32_t    starved;
    uint32_t    lost;
    uint32_t    misaligned;
    uint32_t    sink_errors;
    uint32_t    refused;
    uint32_t    services;
    uint32_t    drain_waits;
    long        last_frames;
} AudioOut;

/**
 * Open a device behind an arbitrary vtable, configure it, read the grant back,
 * derive the lead from the device's own period and prefill that much silence.
 *
 * Returns 0, or -1 if the device could not be opened, granted a nonsensical
 * channel count, or another `AudioOut` is already live in this process.
 *
 * ⚠️ The prefill BLOCKS, and it is the one place in this file that is allowed to.
 * Without it the first sound is written into an empty ring and the first
 * scheduling hiccup drains it before the DAC has started.
 */
int  audio_out_open(AudioOut *out, const AudioOutDev *dev, void *dev_ctx,
                    int rate_req, int channels_req);

/**
 * The same through libasound, on the ALSA PCM `pcm` (e.g. `"plughw:0,0"`):
 * non-blocking, interleaved S16_LE, rate and channels negotiated with the
 * `_near` calls and read back, a period near 2048 frames and a buffer near
 * sixteen of them — requested, never forced — and a start threshold of one
 * period so the silent prefill is what starts the stream.
 *
 * ⚠️ **`channels_req` is a REQUEST and the grant may differ, so no caller may
 * assume it was honoured.** `hw:0,0` is stereo-only; `plughw` may grant the
 * request and convert, but nothing guarantees it — and nothing below refuses the
 * mismatch, because the design is to conform to the grant. Read
 * `audio_out_channels()` back and honour it in the fill callback; a mono fill
 * handed a 2-channel grant plays at double speed. The speaker sums L + R, so 2
 * is also the louder of the two (measured).
 *
 * Compiled only with `-DAUDIO_OUT_HAVE_ALSA`, which both device build paths set
 * explicitly; on the host this is a stub that returns -1 (an ARM build without
 * the define refuses to compile).  Deliberately not detected with
 * `__has_include`: a host that happens to have libasound's headers must not
 * start needing `-lasound` at link time.
 */
int  audio_out_open_alsa(AudioOut *out, const char *pcm,
                         int rate_req, int channels_req);

/**
 * The opener every client calls: the device `path` (from
 * `audio_out_device_path()`) through ALSA, on the PCM `audio_out_device_pcm()`
 * names for it.
 */
int  audio_out_open_default(AudioOut *out, const char *path,
                            int rate_req, int channels_req);

/** The backend `audio_out_open_default()` opens through — always `"alsa"` — for
 *  the one log line each client writes per open. */
const char *audio_out_backend_name(void);

/** What a failed ALSA call means, as a pure function of its negative errno.
 *  Outside every guard so the host test reaches each branch with no libasound. */
typedef enum {
    AO_ERR_AGAIN,    /**< -EAGAIN: the ring is full, retry later          */
    AO_ERR_XRUN,     /**< -EPIPE: underrun, recover with a prepare         */
    AO_ERR_SUSPEND,  /**< -ESTRPIPE: suspended, resume or prepare          */
    AO_ERR_LOST,     /**< -ENODEV / -EBADFD: the device is gone — reopen   */
    AO_ERR_OTHER     /**< anything else: an ordinary sink error            */
} AudioOutErr;

AudioOutErr audio_out_alsa_classify(int err);

/**
 * Whether a space query or write on this open stream failed because the device
 * is GONE (`ENODEV`) — a USB DAC unplugged under a running stream.  Set in the generic
 * layer from whatever errno the backend left, so any backend reaches it;
 * cleared by the next open.  Nothing here reopens: the stream's owner polls this
 * and closes and reopens through its own path, which re-resolves the device.
 */
bool audio_out_device_lost(const AudioOut *out);

/**
 * Resolve the device and open it through `audio_out_open_default()` — the opener
 * `audio.c` and ScummVM's mixer both call, at start-up and on every reopen.
 *
 * ⚠️ **A USB card that is present but will not open is REFUSED until it is
 * unplugged**, and this falls back to the panel speaker in the same call.  Without
 * that, a move back to a replugged card that fails would leave the stream closed
 * and every retry would resolve to the same card again: silence for the session,
 * the very thing the fallback exists to prevent.  The refusal is cleared the first
 * time card 1 is seen absent, so the next plug is tried afresh.  A Bluetooth sink
 * that will not open is refused the same way, until the ACL-link signature
 * changes (a reconnect); the fallback then resolves USB, then onboard.
 *
 * `*path_out` (may be NULL) receives the path that opened — a `/dev/dsp*` node, or
 * `"bluealsa-pin"` / `"bluealsa"` for the pinned / any Bluetooth sink.  Returns 0
 * or -1.
 */
int  audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                             const char **path_out);

/** How often, at most, a stream not on its preference's best sink looks again. */
#define AUDIO_OUT_REPROBE_MS 1000

/**
 * Whether `candidate` outranks `open_path` in the one order every preference is
 * a subsequence of: Bluetooth > USB > onboard.  Pure.  A move only ever goes UP
 * it — a sink that merely differs (the current one flickering in a probe) never
 * pulls a live stream sideways or down; losing the current sink is the
 * device-lost path's job, not this one's.
 */
bool audio_out_sink_better(const char *candidate, const char *open_path);

/**
 * The gate in front of the "better sink returned" probe, as a pure function: TRUE
 * when a stream open on `open_path` should look again now.  Only when the
 * preference allows something that outranks `open_path` (`"usb"`: USB over
 * onboard; `"auto"` and `"bluetooth"`: Bluetooth over both; never `"onboard"` or
 * an unrecognised value), and only once `AUDIO_OUT_REPROBE_MS` has passed since
 * `last_ms`.  Unsigned subtraction, so the millisecond clock wrapping does not
 * stop the probe.
 *
 * Split out so the host test reaches every branch: the wrapper below also needs
 * a card-1 node and a Bluetooth sink, which no host has.
 */
bool audio_out_reprobe_due(const char *pref, const char *open_path,
                           uint32_t now_ms, uint32_t last_ms);
/** The same gate with the pin in it: `pinned` (a headset pinned and in force)
 *  makes the pinned headset the best Bluetooth sink, so a stream on any other
 *  sink keeps looking for it.  audio_out_reprobe_due() is this with `false`. */
bool audio_out_reprobe_due_pin(const char *pref, bool pinned, const char *open_path,
                               uint32_t now_ms, uint32_t last_ms);

/**
 * Whether a better sink — a replugged USB card, a newly connected Bluetooth A2DP
 * sink — should take this stream.  Named for the first of them; ScummVM's mixer
 * calls it by this name.  Call it on every service; it costs a clock read unless
 * the gate above opens, and then one `access()` on card 1's node plus
 * `audio_out_bt_present()`'s cheap gate — a PCM open only inside that gate's
 * probe window.
 *
 * TRUE only when the better sink was usable at TWO consecutive probes (~1 s
 * apart), because a node or a transport can appear before it will open, and only
 * when it is not refused (see audio_out_open_resolved()).  It logs the move; the
 * owner then closes the stream and reopens through its own path — this reopens
 * nothing, exactly as with audio_out_device_lost().
 */
bool audio_out_usb_returned(AudioOut *out);

/* ── Bluetooth presence: a cheap kernel gate, then a windowed PCM probe ──────
 *
 * The operator keeps a Bluetooth keyboard and pad connected permanently, so "is
 * there a link" is not "is there an audio sink", and opening the BlueALSA PCM on
 * every look is not cheap.  The kernel lists one `hciN:H` entry per ACL link in
 * /sys/class/bluetooth; a small signature of those names is read on every look,
 * and only when it CHANGES does a probe window of AUDIO_OUT_BT_WINDOW_MS open,
 * inside which the real probe (open `plug:bluealsa` non-blocking, close) runs at
 * most once per AUDIO_OUT_BT_PROBE_MS — an A2DP transport comes up a few seconds
 * after its ACL link.  The first look in a process probes once if any link
 * exists.  Outside the window the last probe's answer stands.
 */
#define AUDIO_OUT_BT_WINDOW_MS 10000
#define AUDIO_OUT_BT_PROBE_MS  1000

typedef struct {
    bool     have_sig;         /**< a signature has been seen at all            */
    bool     window;           /**< the probe window is open                    */
    uint32_t sig;              /**< the last signature seen                     */
    uint32_t window_start_ms;
    uint32_t last_probe_ms;
} AudioOutBtGate;

/** The ACL-link signature of a /sys/class/bluetooth-shaped directory: 0 when it
 *  holds no `hciN:H` entry (or does not exist), otherwise an order-independent
 *  hash of those names.  A parameter so a host test can hand it a fixture. */
uint32_t audio_out_bt_acl_signature(const char *sysfs_dir);

/** The decision, pure: given the current signature and the clock, is a PCM probe
 *  due now?  Updates `g`; `*changed` (may be NULL) reports a signature change —
 *  the moment a Bluetooth refusal is cleared.  See the block comment for the
 *  rule; `tests/audio_out_test.c` group P is its specification. */
bool audio_out_bt_probe_due(AudioOutBtGate *g, uint32_t sig, uint32_t now_ms,
                            bool *changed);

/** Whether a Bluetooth A2DP sink is connected, by the gate above.  Cheap enough
 *  for every frame of a settings page: a readdir, and a PCM open only inside a
 *  probe window.  A probe that finds the sink BUSY counts as present. */
bool audio_out_bt_present(void);

/* ── Which device ───────────────────────────────────────────────────────────
 *
 * ⚠️ **One home for "which `/dev/dsp*`", and every opener in the tree resolves
 * through it** — this file's ALSA opener, `audio.c`, and ScummVM's mixer. There used to be a `DSP_DEVICE` macro in each of the first
 * two; a seam in only one of them left every app opening the panel speaker at
 * startup and falling back to it on error.
 *
 * The preference is process-global on purpose: `audio_open()` memsets `Audio`,
 * so a field on the struct would be cleared by the call that needs to read it.
 * Set it once, before the first open; it is honoured by every later open.
 *
 * These four are OUTSIDE this file's ALSA guard, so a host build with no
 * libasound still links them and `tests/audio_out_test.c` drives the
 * resolution with no sound card present.
 */

/** Set the preference: `"onboard"`, `"usb"`, `"bluetooth"` or `"auto"`. NULL or empty reads as `"auto"`;
 *  an unrecognised value resolves as `"onboard"` (see audio_out_device_for). Truncated
 *  past 15 characters. */
void        audio_out_set_device_pref(const char *pref);

/** What was last set — the preference, NOT the resolved device. */
const char *audio_out_device_pref(void);

/**
 * Pin the Bluetooth sink to one headset: `"AA:BB:CC:DD:EE:FF"` (config key
 * `audio_bt_addr`, written by the control panel's USE FOR AUDIO).  NULL, empty
 * or malformed unpins.  Set beside the preference, before the first open; every
 * opener that sets the preference sets this too.
 *
 * ⚠️ **A preference, not a filter, in force under `"bluetooth"` and `"auto"`.**
 * It adds a tier at the top of Bluetooth: the path `"bluealsa-pin"`, PCM
 * `bluealsa:DEV=<addr>,PROFILE=a2dp`, then `"bluealsa"` (`plug:bluealsa`,
 * BlueALSA's most recently connected sink) for any other headset, then USB and
 * onboard.  A stream on a lower tier keeps probing for the pinned headset
 * (audio_out_reprobe_due_pin()) and moves back when it returns.
 */
void        audio_out_set_bt_addr(const char *addr);
/** What was last set and accepted: the address, or "" when unpinned. */
const char *audio_out_bt_addr(void);
/** Exactly `XX:XX:XX:XX:XX:XX`, hex digits either case, nothing after. */
bool        audio_out_bt_addr_valid(const char *addr);
/** The pinned BlueALSA PCM name as a pure function of preference and address:
 *  the pinned form into `buf` when `pref` is `"bluetooth"` or `"auto"` and `addr`
 *  is valid, else (or if `buf` is too small) the literal `"plug:bluealsa"`. */
const char *audio_out_bt_pcm_for(const char *pref, const char *addr,
                                 char *buf, size_t n);
/** Whether a SAVED choice + address prefers the device `dev_addr`: BLUETOOTH or
 *  AUTO and the same valid address, compared case-insensitively.  A settings
 *  page offers USE FOR AUDIO while this is false. */
bool        audio_out_bt_is_pinned(int saved_choice, const char *saved_addr,
                                   const char *dev_addr);

/** Whether ALSA card 1's OSS node is present and writable right now. */
bool        audio_out_usb_present(void);

/**
 * The device the next open will use, resolved fresh on every call so unplugging
 * a DAC between opens is picked up.
 *
 * ⚠️ **Both non-onboard settings fall back to the panel speaker when card 1 is
 * absent** — a games panel gone mute with no explanation is worse than one on
 * the wrong speaker. `"auto"` falls back silently (unplugging is expected);
 * `"usb"` was an explicit request, so it reports the fallback once on stderr.
 * `"auto"` puts a connected Bluetooth sink first; `"bluetooth"` with none
 * resolves as `"auto"`'s remainder (USB, else onboard), reported once.  The
 * Bluetooth sink is the path `"bluealsa"`, not a node.
 */
const char *audio_out_device_path(void);

/**
 * The resolution itself, as a pure function of the three inputs.
 *
 * ⚠️ **This exists so the CARD-PRESENT branch is reachable from a host test.**
 * `audio_out_device_path()` is this function applied to the stored preference
 * and the two presence readers, and no host has `/dev/dsp1` or a sink — so a group that
 * could only call the wrapper would pass identically against a resolver that
 * ignored its argument and always answered `/dev/dsp`. Splitting the decision
 * out is what makes that sabotage fail. It is not a test-only hook: the wrapper
 * has no logic of its own left to disagree with.
 */
const char *audio_out_device_for(const char *pref, bool usb_present, bool bt_present);
/** The same with the Bluetooth tier split: the pinned headset present
 *  (`"bluealsa-pin"`), else any sink present (`"bluealsa"`), under `"bluetooth"`
 *  and `"auto"`.  audio_out_device_for() is this with no pinned headset. */
const char *audio_out_device_for_bt(const char *pref, bool usb_present,
                                    bool bt_pin_present, bool bt_any_present);

/* ── The choices a settings page offers ─────────────────────────────────────
 *
 * One table, in the order a press cycles them, so a new output is
 * one row here plus its presence input rather than an edit in every page.  All
 * pure: the CALLER passes what is attached (`audio_out_usb_present()`,
 * `audio_out_bt_present()`), so a
 * host test reaches the card-present branch with no card.
 *
 * ⚠️ **"Shown" is display only, and the saved choice is never rewritten by it.**
 * A saved "usb" with no DAC already plays onboard and moves to the DAC when it
 * returns (`audio_out_usb_returned()`); the page shows AUTO meanwhile because
 * that is what is happening, but persisting AUTO would silently lose the
 * operator's explicit choice.  Keep the saved and shown indices apart.
 */
typedef enum {
    AUDIO_OUT_CHOICE_ONBOARD = 0,
    AUDIO_OUT_CHOICE_USB,
    AUDIO_OUT_CHOICE_BT,
    AUDIO_OUT_CHOICE_AUTO,
    AUDIO_OUT_CHOICE_COUNT
} AudioOutChoice;

/** The persisted config value ("onboard" | "usb" | "bluetooth" | "auto").  Out of range reads
 *  as onboard. */
const char *audio_out_choice_name(int choice);
/** Upper-case button text ("ONBOARD" | "USB" | "BLUETOOTH" | "AUTO").  Out of range: onboard. */
const char *audio_out_choice_label(int choice);
/** The entry a config value names; NULL or unrecognised is ONBOARD — the same
 *  answer audio_out_device_for() gives it, so a page cannot show what no opener
 *  would do. */
int  audio_out_choice_of(const char *name);
/** Whether the entry is in the list with this hardware attached. */
bool audio_out_choice_available(int choice, bool usb_present, bool bt_present);
/** What a saved choice shows as: itself if available, else AUTO. */
int  audio_out_choice_shown(int saved, bool usb_present, bool bt_present);
/** The next AVAILABLE entry after `shown`, in table order, wrapping. */
int  audio_out_choice_next(int shown, bool usb_present, bool bt_present);

/** The ALSA PCM for an OSS node `audio_out_device_path()` returned: `/dev/dsp`
 *  → `plughw:0,0`, `/dev/dsp1` → `plughw:1,0` (OSS minor N is ALSA card N on
 *  this device), `"bluealsa"` → `plug:bluealsa` (BlueALSA's default device =
 *  the most recently connected sink, A2DP; `plug` so ScummVM's mono and odd
 *  rates are converted) or, pinned, `audio_out_bt_pcm_for()`'s form.  Anything
 *  else maps onboard, the resolver's own fallback.
 *  `plughw`, not `hw`, so a rate or channel count the card lacks is converted
 *  rather than refused. */
const char *audio_out_device_pcm(const char *path);

/** Whether `audio_out_device_path()` currently resolves to the panel speaker.
 *  ⚠️ GPIO12 belongs to that device alone — see `audio_out_enable_amp()`. */
bool        audio_out_device_is_onboard(void);

/**
 * Drive GPIO12 HIGH to unmute the TWL4030 speaker amplifier (SPKR1).
 *
 * ⚠️ **Guard every call on `audio_out_device_is_onboard()`.** This is card 0's
 * amp and means nothing to a USB DAC; poking it while the DAC is the sink
 * unmutes a speaker nothing is feeding. Exposed rather than kept private
 * because `audio.c` had a byte-identical copy of it.
 */
void        audio_out_enable_amp(void);

/**
 * Drain what is queued — bounded — then close.  Safe on a struct that was never
 * opened or whose open failed.
 *
 * The bound is the ring's own duration plus a period, and the stop condition is
 * the free-space slack rather than zero, because the device's in-flight figure
 * never reaches zero while it is over-reporting.  So a drained stream returns
 * immediately and a full one costs at most one ring.
 */
void audio_out_close(AudioOut *out);

/**
 * Install, replace or (with `fill == NULL`) remove the fill callback.
 *
 * ⚠️ **The one installed callback is what makes two writers impossible by
 * construction**, and it needs no reset to switch — which is the whole reason
 * the theremin and the mix bus can share one never-reset stream.  `owner` is a
 * static string naming the installer; it exists so the layer above can keep its
 * refusal LOUD.  A swap that went quiet would let `audio_tone()` during the
 * theremin enqueue into a mixer nobody renders, and the sound would simply
 * vanish.
 *
 * ⚠️ A caller with a release to write — `audio_stream_stop()`'s 20 ms
 * `AUDIO_OSC_FADE_OUT` — must remove its fill FIRST and write the fade after,
 * because `audio_out_write()` is refused while any fill is installed.  The price
 * is that the fade lands one lead (~139 ms) behind the finger, with the
 * oscillator at full amplitude until then; that is accepted, because the only way
 * to cut the lead short is a reset, and the reset is the click.
 *
 * Returns 0, or -1 if the stream is not open.
 */
int  audio_out_set_fill(AudioOut *out, AudioOutFill fill, void *ctx,
                        const char *owner);

/** Who installed the current callback, or NULL if none is installed. */
const char *audio_out_fill_owner(const AudioOut *out);

/**
 * Attenuate by `shift` bits immediately before every write, post-fill.
 *
 * ⚠️ **An arithmetic SHIFT, never a gain multiply.** `-1 >> 1 == -1`, so
 * ScummVM's existing `>>1` is reproduced bit for bit; any rounding multiply
 * changes bits and the port would no longer be the thing that was verified on
 * the panel.  `shift = 0` is the identity and is what the native path uses,
 * because loudness is held IDENTICAL in this change — the level question is a
 * separate ear-verified follow-up, and confounding it with the stream change is
 * a mistake this work has already paid for twice.
 *
 * ⚠️ And this is not "one acoustic ceiling for every client": §3.4 measured that
 * a single global scalar is the wrong SHAPE, because the clean ceiling falls with
 * pitch.  At `shift = 0` it is a parameter, nothing more.
 */
void audio_out_set_shift(AudioOut *out, int shift);

/**
 * The system equaliser (audio_eq.h): 60/250/1k/4k/12k Hz, integer dB in
 * -EQ_MAX_DB..+EQ_MAX_DB.  audio_out_open() reads the `eq` key of the config
 * file (EQ_CONFIG_KEY, eq=g1,g2,g3,g4,g5 or a preset name) so a change made in
 * settings applies to the next app; this setter changes the curve of the OPEN
 * stream between two buffers (history kept, no click from a swap), for the
 * in-process panel.  It does not write the config; the caller does.  All
 * zeros is a bit-exact bypass.  Same single-thread rule as set_shift.
 */
void audio_out_set_eq(AudioOut *out, const int gains_db[EQ_BANDS]);
/** The gains in force (zeros when closed). */
void audio_out_get_eq(const AudioOut *out, int gains_db[EQ_BANDS]);
/** Override the config file the open reads (tests); NULL restores the default. */
void audio_out_set_eq_path(const char *path);

/**
 * The system enhancer (audio_enh.h), after the equaliser.  audio_out_open()
 * reads the `enh` key of the same config file (ENH_CONFIG_KEY,
 * enh=on,hb,hbr,db,dbr,ht,htr; missing or garbled = off).  The setter changes
 * the OPEN stream live (history kept); it does not write the config.  Off, or
 * all amounts 0, is a bit-exact bypass.  Same single-thread rule as set_eq.
 */
void audio_out_set_enh(AudioOut *out, const AudioEnh *e);
/** The settings in force (all off when closed). */
void audio_out_get_enh(const AudioOut *out, AudioEnh *e);

/**
 * Render one service's worth and write it.  Returns frames written, or -1.
 *
 * ⚠️ **It never sleeps and never spawns a thread.** Its write policy's wait is
 * **0**, which is what makes that claim true rather than nearly true: a 1000 µs
 * wait would let `audio_write_frames()`'s bounded mid-frame realignment spend
 * `AUDIO_ALIGN_TRIES` × 1 ms inside a call the render loop believes is free.
 *
 * ⚠️ **It targets the LEAD; it never fills the free space.** An empty 743 ms ring
 * will happily accept 743 ms of audio, and then the next sound plays three
 * quarters of a second late.
 *
 * ⚠️ **The return value is the pacing signal.** A caller advancing a fixed
 * per-buffer deadline while this writes a VARIABLE frame count has two pacing
 * models and neither bounds the queue — `oss-mixer.cpp`'s deadline is exactly
 * that shape.  Advance by what was written, or drop the deadline.
 *
 * ⚠️ **A silent bus still writes silence**, which is the entire fix: an idle
 * stream is a stream transition, and a transition is the click.  So this returns
 * a positive count on a silent bus, and `0` only when the queue is already at the
 * lead.
 */
long audio_out_service(AudioOut *out);

/**
 * Mode 2: push `frames` MONO samples with a bounded blocking policy.
 *
 * For a caller with no render loop and no audio thread.  Returns the frames the
 * device took, or -1.  Refused — loudly, and counted — while a fill callback is
 * installed, because that is the two-writer case the callback exists to make
 * impossible.
 *
 * The blocking bound is derived from the buffer's own duration, so a long tone
 * waits as long as a long tone needs and only a permanently wedged device is
 * given up on.  It is a bound rather than an unlimited wait because the
 * alternative to giving up is hanging a UI.
 */
long audio_out_write(AudioOut *out, const int16_t *mono, long frames);

/**
 * The longest a caller may go between services, in microseconds.
 *
 * ⚠️ **Derived from REAL audio, not from the nominal lead.** The device's
 * in-flight figure over-reports (see `AUDIO_OUT_OSPACE_SLACK_NUM`), so the
 * nominal lead the arithmetic targets is ~1.3 periods more than the audio that
 * actually exists.  This subtracts that and keeps half a period of margin.  At
 * 44100 with the shim's 2048-frame period and a 3-period lead it comes out under
 * the 66 ms that was measured to survive with ~11 ms to spare, and well under the
 * 100 ms that starves ~2.5×/s.
 *
 * ⚠️ **This is why `audio_pump_active()` stays in the frame-pacing decision.**
 * `FRAME_DELAY_IDLE_US` is 100 000 — above this figure at every configuration
 * measured — so a render loop that drops to idle while the stream is live starves
 * it, and 100 ms alone is enough: the composite row showed a 107 ms worst frame
 * added nothing to the damage.
 *
 * 0 until a service or an open has measured the geometry.
 */
long audio_out_service_interval_us(const AudioOut *out);

/* ── Accessors: what the device GRANTED, and what the library DID ────────── */

int  audio_out_rate(const AudioOut *out);
int  audio_out_channels(const AudioOut *out);
/** The granted sample width.  16 everywhere measured; anything else is warned
 *  about once and reported here, because a silent width surprise is noise with
 *  no diagnostic. */
int  audio_out_bits(const AudioOut *out);
bool audio_out_is_open(const AudioOut *out);

/** Ask for a deeper (or shallower) lead than AUDIO_PUMP_LEAD_MS, in ms; 0 restores
 *  the default.  PROCESS-WIDE and read by the next geometry derivation, so call it
 *  BEFORE the open (the prefill is sized from it) — a stream already open picks it
 *  up at its next service, without a re-prefill.  The result still goes through
 *  audio_pump_lead_frames(): floored at AUDIO_PUMP_LEAD_PERIODS periods, rounded UP
 *  to a whole period, capped at half the ring the device granted — read the
 *  outcome from audio_out_lead(), never from this figure.  The price of a deep lead
 *  is latency: audio already queued plays out, so a volume, EQ or track change is
 *  heard one lead late.  Only Ludfin sets it; every other client keeps today's. */
void audio_out_set_lead_ms(int ms);

/** The lead the last service TARGETED (the EFFECTIVE value, override included),
 *  in frames, and the device period it was
 *  rounded up to.  Both 0 until measured — "not yet measured" and "the constant"
 *  are different claims, and a diagnostic that printed
 *  `AUDIO_PUMP_LEAD_MS` once displayed 80 ms while the library held ~139. */
long audio_out_lead(const AudioOut *out);
long audio_out_period(const AudioOut *out);
/** Frames the last service or write handed to the device. */
long audio_out_last_frames(const AudioOut *out);

/** Services that found the queue DRY.  ⚠️ On a stream that is never allowed to
 *  go idle, dry is ALWAYS a fault — one audible gap each — and it is what
 *  separates a pacing problem from a mixing one. */
uint32_t audio_out_starved(const AudioOut *out);
/** Frames rendered — so the client's voices advanced past them — that the device
 *  refused.  They are gone, not deferred, and the waveform has a step where they
 *  were.  The serviced policy may not block, so this is the price of that; it is
 *  counted so the price is measured rather than assumed zero. */
uint32_t audio_out_lost(const AudioOut *out);
/** Writes that left a PARTIAL FRAME in the device.  ⚠️ Never ignorable: with an
 *  interleaved grant and no mono path underneath, half a frame swaps L and R for
 *  the rest of the stream, permanently. */
uint32_t audio_out_misaligned(const AudioOut *out);
/** Writes that failed for a reason other than "the queue is full". */
uint32_t audio_out_sink_errors(const AudioOut *out);
/** Calls refused: a mode-2 write against an installed callback, or a service
 *  with no geometry.  Counted so a silently mute client is diagnosable. */
uint32_t audio_out_refused(const AudioOut *out);
/** Services that got as far as looking at the device. */
uint32_t audio_out_services(const AudioOut *out);
/** Poll iterations the last close spent draining. */
uint32_t audio_out_drain_waits(const AudioOut *out);

#endif /* AUDIO_OUT_H */
