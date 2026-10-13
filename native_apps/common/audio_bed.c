/*
 * audio_bed.c — the music-bed playlist and its four states.  See audio_bed.h
 * for what a game owns and what this file owns; the rules are the ones that
 * shipped in platformer.c and were verified on `.188` (2026-08-22).
 */
#include "audio_bed.h"

#include <stdio.h>
#include <string.h>

void audio_bed_init(AudioBed *bed, Audio *audio, const char *tag)
{
    if (!bed) return;
    memset(bed, 0, sizeof(*bed));
    bed->audio = audio;
    bed->state = AUDIO_BED_IDLE;
    snprintf(bed->tag, sizeof bed->tag, "%s", tag ? tag : "game");

    /* The set file is the ONE home for this game's paths — nothing is derived
     * here, so an absent file is an empty playlist rather than a fallback to a
     * convention.  ⚠️ Reported at INFO volume and not as an error: a device
     * without the file is a device without music, which is a state the off
     * switch already made legal. */
    Config cfg;
    if (config_load_sound_set(&cfg, bed->tag) < 0) {
        bed->disabled = true;
        printf("%s: no sound set at %s/%s%s — no music bed, effects continue\n",
               bed->tag, CONFIG_SOUND_SET_DIR, bed->tag, CONFIG_SOUND_SET_EXT);
        return;
    }
    audio_bed_set_playlist(bed, &cfg);
}

void audio_bed_set_playlist(AudioBed *bed, Config *cfg)
{
    if (!bed || !cfg) return;

    /* Walk the whole cap rather than a passed-in count: the file decides how many
     * tracks there are, and a gap ends nothing.  ⚠️ `config_get` is asked for
     * NULL rather than a derived default, which is what makes ABSENT and
     * PRESENT-BUT-EMPTY two different answers — only the second is the off
     * switch, and conflating them would silence a game whose track 1 line was
     * merely deleted. */
    for (int i = 0; i < AUDIO_BED_MAX_TRACKS; i++) {
        char key[CONFIG_KEY_LEN];
        if (i == 0) snprintf(key, sizeof key, "%s_music", bed->tag);
        else        snprintf(key, sizeof key, "%s_music%d", bed->tag, i + 1);

        const char *p = config_get(cfg, key, NULL);
        if (!p) continue;                  /* no such line; a gap is skipped */
        if (i == 0 && !p[0]) {
            /* ⚠️ Track 1 PRESENT AND EMPTY means silence for the whole game,
             * deliberately: it preserves `<tag>_music=` as the off switch it
             * already was, and it is the one thing an operator can write in the
             * file to mean "this game, no music". */
            bed->disabled = true;
            printf("%s: music bed disabled by %s (%s=)\n",
                   bed->tag, cfg->filepath, key);
            return;
        }
        if (!p[0]) continue;               /* a later empty line: also a gap */
        snprintf(bed->track[bed->count], sizeof bed->track[0], "%s", p);
        bed->count++;
    }

    if (bed->count == 0) {
        /* A file that exists and names nothing. audio_bed_service() would return
         * early on count==0 anyway; say it once so the log distinguishes this
         * from a file that is simply absent. */
        bed->disabled = true;
        printf("%s: sound set %s names no music — no music bed, effects continue\n",
               bed->tag, cfg->filepath);
        return;
    }
    printf("%s: music playlist — %d track(s), first %s\n",
           bed->tag, bed->count, bed->track[0]);
}

void audio_bed_service(AudioBed *bed, bool want_play, bool want_hold)
{
    if (!bed || !bed->audio || bed->disabled || bed->count == 0) return;

    switch (bed->state) {
    case AUDIO_BED_IDLE: {
        if (!want_play || audio_music_active(bed->audio)) break;
        /* Walk the playlist from the cursor, skipping entries that already
         * failed.  ⚠️ Bounded by ONE lap: without the counter a playlist whose
         * every entry is missing would spin here for the life of the process. */
        int tried = 0;
        while (tried < bed->count && bed->failed[bed->next]) {
            bed->next = (bed->next + 1) % bed->count;
            tried++;
        }
        if (tried >= bed->count) {          /* every track refused — stop asking */
            bed->disabled = true;
            break;
        }
        int t = bed->next;
        /* ⚠️ Loops only when it is the WHOLE playlist.  With two or more tracks each
         * plays once and ends, and the PLAYING arm below sends the bed back here for
         * the next — a looping track never ended, so a single-level game like
         * SameGame played track 1 for the whole session (operator, 2026-09-29). */
        if (audio_music_start(bed->audio, bed->track[t], bed->count == 1)) {
            bed->state = AUDIO_BED_PLAYING;
            bed->next  = (t + 1) % bed->count;      /* the NEXT fresh start */
            break;
        }
        /* A missing file, a rate mismatch or a bus that never came up are all
         * permanent for THIS TRACK, and retrying per frame would fill
         * /var/log/arca/app_stdout.log with one refusal per 33 ms.
         * ⚠️ Per track rather than per process, so one bad path in the config
         * does not silence the tracks that are fine — and the MUSIC toggle being
         * off is reported as itself rather than as a missing file. */
        bed->failed[t] = true;
        if (!audio_music_enabled(bed->audio)) {
            bed->disabled = true;           /* one line, not one per track */
            printf("%s: music is off (music_enabled=false) — effects and play continue\n",
                   bed->tag);
        } else {
            printf("%s: no music bed at %s — effects and play continue\n",
                   bed->tag, bed->track[t]);
        }
        break;
    }

    case AUDIO_BED_PLAYING:
        if (want_play) {
            /* The track ended (the next one starts), a lone looping track ran out its
             * 200 passes (~2.4 h), or audio_interrupt() cleared the bus:
             * re-arm from IDLE rather than stay silent for the session. */
            if (!audio_music_active(bed->audio)) bed->state = AUDIO_BED_IDLE;
        } else if (want_hold) {
            if (audio_music_pause(bed->audio)) bed->state = AUDIO_BED_HELD;
        } else {
            audio_music_stop(bed->audio);
            bed->state = AUDIO_BED_STOPPING;
        }
        break;

    case AUDIO_BED_HELD:
        if (want_hold) break;
        if (!want_play) { bed->state = AUDIO_BED_STOPPING; break; } /* died out, or quit */
        if (audio_music_active(bed->audio)) break;                  /* release still fading */
        if (audio_music_resume(bed->audio)) { bed->state = AUDIO_BED_PLAYING; break; }
        /* ⚠️ IDLE, not disabled: a refused resume loses the held file, but the
         * playlist is intact and the next level can start a fresh voice.  Marking
         * the track failed here would silence a game for one recoverable hiccup. */
        bed->state = AUDIO_BED_IDLE;
        printf("%s: music bed could not resume — starting fresh at the next level\n",
               bed->tag);
        break;

    case AUDIO_BED_STOPPING:
        if (!audio_music_active(bed->audio)) bed->state = AUDIO_BED_IDLE;
        break;
    }
}
