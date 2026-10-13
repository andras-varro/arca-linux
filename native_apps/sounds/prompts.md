# Sourcing the eleven Arca sound effects

## Hard file spec — a file that misses this is REFUSED at load

| property | value | what happens otherwise |
|---|---|---|
| format | WAV, PCM | not parsed |
| channels | **mono** | stereo collapses to L+R in the amp anyway; width and panning are lost |
| sample rate | **44100 Hz exactly** | REFUSED with a log line, game silently falls back to the old beep — there is no resampler |
| bit depth | 16-bit | — |
| length | 90 ms - 400 ms (hard ceiling 4.0 s) | see the envelope note below |
| peak | normalise to about -0.3 dBFS | the mixer attenuates per voice |
| edges | must start and end at zero, >=2 ms fade out | a file that stops mid-swing clicks |

**Do not go below ~90 ms.** The mixer applies its own 10 ms attack and 20 ms release to every
clip, each clamped to half the clip length. At 55 ms that shapes 30 of 55 ms; at 28 ms the
envelope IS the whole sound. Longer files keep their own transient.

## Acoustic spec — the part a sound generator gets wrong

This is a small panel speaker, excursion-limited, measured by ear against a signal generator:

- **Nothing below 700 Hz.** Sharp rolloff below ~700 Hz; below 500 Hz hard to hear at the
  shipped level; below 300 Hz inaudible at viewing distance. An effect centred below 700 Hz is
  unusable *whatever its shape*. So: no bass, no thump, no boom, no sub, no low rumble.
  High-pass everything at 700 Hz before delivering it.
- **Safe centre is 1-4 kHz.** Everything shipped and confirmed audible is >=880 Hz, and the
  highest is 4.2 kHz. Nothing above ~5 kHz has been measured, so don't put the payload there.
- **No two sustained tones at once.** Two held sines intermodulate audibly - measured on a
  PRE-MIXED file through the vendor's player, so it is the speaker, not our code. No chords,
  no held harmony, no two-note intervals. An arpeggio (one note at a time) is fine.
- Loud low content distorts before any digital stage. Keep the energy up high and it stays clean.

## Shared prompt suffix - append to every prompt below

> Mono, 44.1 kHz, 16-bit WAV. Bright and thin, for a tiny wall-panel speaker: all energy
> between 1 kHz and 4 kHz, absolutely no bass or low-mid content below 700 Hz, no sub, no
> boom. Clean single-voice sound, no chords or held harmony. Sharp start, quick decay to
> silence. Dry - no reverb, no room tail.

## The eleven

| file | game event | length | prompt |
|---|---|---|---|
| `fx_click.wav` | menu tap, UI press (`audio_beep`) | ~100 ms | A short bright synthetic UI tap. A single clean high blip with a tiny click at the onset, like a hardware button on a handheld console. Pitched, not noisy. |
| `fx_pickup.wav` | coin, powerup collected (`audio_blip`) | ~120 ms | A cheerful two-note rising blip, one note then the next, like collecting a coin in an 8-bit platformer. Bright square-wave character, clearly pitched, ends clean. |
| `fx_success.wav` | level cleared (`audio_success`) | ~350 ms | A short triumphant rising arpeggio of three or four bright chiptune notes, played one at a time, never overlapping. Cheerful, clearly melodic, ends on the highest note. |
| `fx_fail.wav` | life lost (`audio_fail`) | ~350 ms | A descending "you lost" chiptune figure: two or three bright falling notes, one at a time, with a slight wobble. Clearly pitched and unmistakably sad. Must cut through background music. |
| `fx_knock.wav` | ball hits paddle | ~90 ms | A crisp mid-high pitched knock, like a table-tennis ball off a hard bat. Short, woody, a definite pitch to it, no low thud. |
| `fx_tick.wav` | brick destroyed | ~90 ms | A bright short glassy tick, like tapping a small glass with a fingernail. High and clean with a fast decay. |
| `fx_thud.wav` | brick hit but not destroyed | ~90 ms | A dull muted mid-pitched tap - the "nothing happened" version of a hit. Softer and slightly lower than a glassy tick but still well above bass, quick and unresonant. |
| `fx_sparkle.wav` | bonus brick | ~200 ms | A quick bright sparkle: three or four very short high shimmering notes rising fast, like a magic pickup in an arcade game. Twinkly, clean, one note at a time. |
| `fx_burst.wav` | explosive brick | ~250 ms | A small bright arcade explosion: a sharp filtered noise burst with a fast bright decay and a hint of pitch falling through it. Crackly and energetic but with NO low boom at all. |
| `fx_jump.wav` | jump / stomp | ~120 ms | A classic upward jump swoop - a single tone sliding quickly from mid to high pitch, bright and springy, like a platform-game hop. Clearly pitched, no noise. |
| `fx_gameover.wav` | the RUN is over, not one life (`audio_gameover`) | ~1.19 s | A classical game-over effect: three or four descending tones. ⚠️ **Recorded from the operator's recollection 2026-08-24, not the verbatim text**, and it also carried this file's standard speaker boilerplate — re-source against the measurements below rather than treating the wording as exact. |

⚠️ **`fx_gameover` is the only effect longer than the 400 ms guideline above** (1.19 s against a 4.0 s hard
ceiling) and that is fine: the ceiling is `AUDIO_CLIP_MAX_FRAMES`, and the guideline is about effects that
fire during play. This one fires once, when play has stopped. It also measures the LOUDEST in-band of the
set — −10.61 dBFS at a −0.93 dB band delta — which is why `AUDIO_FX_GAMEOVER` points at it rather than at
`fx_burst`. Re-measure with `../check-sound-assets.sh`, never by ear alone.

Deliver into `native_apps/sounds/` under exactly those filenames.
