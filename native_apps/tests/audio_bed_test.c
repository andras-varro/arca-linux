/*
 * audio_bed_test — the SOUND SET's resolution rules, on the host
 *
 * A game's music paths used to be DERIVED in C from a stem and a
 * count each game passed to audio_bed_init(); they now live in one place, the
 * per-game set file that native_apps/sound-sets.sh generates, and the C side
 * derives nothing.  That moved four rules out of "obvious" and into "asserted":
 *
 *   A  the playlist is the file's, in the file's order, and slot 0 has no number
 *   B  "<tag>_music=" PRESENT AND EMPTY is the whole-game off switch
 *   C  "<tag>_music" ABSENT is NOT the off switch — the later tracks still play
 *   D  a gap in the numbering ends nothing; a file naming nothing is silence
 *   E  AUDIO_BED_MAX_TRACKS bounds the walk, and the overflow key is ignored
 *   F  config_load_sound_set() builds the path the generator installs to
 *
 * ⚠️ E is also what pins the key NUMBERING, and it is the only group that can.
 * Measured by sabotage: shifting the numbered key from `i + 1` to `i` leaves
 * groups A, C and D all PASSING, because their files use consecutive keys and the
 * shift merely skips a `_music1` that was never there.  Only a file that reaches
 * the cap notices.  So if E is ever weakened, the numbering loses its only test.
 *
 * ⚠️ B and C are ONE line apart in audio_bed.c and mean opposite things.  Before
 * this change both spellings reached the same `config_get(..., def)` call with a
 * derived default behind it, so "absent" could not be distinguished from "empty"
 * at all — conflating them now silences a game whose track-1 line was merely
 * deleted, and nothing on the panel says which of the two happened.  Group C is
 * the only thing in the tree that can see that.
 *
 * ⚠️ This drives audio_bed_set_playlist(), not audio_bed_init(): the latter reads
 * CONFIG_SOUND_SET_DIR, which is /opt/arca/soundsets and does not exist on
 * a dev host.  The split is deliberate and audio_bed.h says so.  bed_load() below
 * reproduces exactly what audio_bed_init() does around the call, and group F
 * asserts the path half separately — so the two together cover the function.
 *
 * ⚠️ audio_bed_service() is NOT covered here.  Every one of its transitions calls
 * audio_music_*(), which needs /dev/dsp; it stays ear-only on the panel.
 *
 * Build and run (host gcc — build-and-deploy.sh does not run this):
 *
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/audio_bed_test tests/audio_bed_test.c \
 *       common/audio_bed.c common/config.c \
 *       common/audio.c common/audio_gen.c common/audio_out.c common/audio_wav.c \
 *       -lm && ./build/audio_bed_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_bed.h"

static int failures = 0;
static int checks   = 0;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("    FAIL: %s\n", what); }
}

/* The temp set file every group writes.  Under the repo rather than /tmp: Git
 * Bash's /tmp and WSL's /tmp are different filesystems and WSL's does not survive
 * between wsl.exe calls (../CLAUDE.md). */
#define SET_PATH "build/audio_bed_test.set"

static void write_set(const char *body)
{
    FILE *f = fopen(SET_PATH, "w");
    if (!f) { printf("    FAIL: cannot write " SET_PATH "\n"); failures++; return; }
    fputs(body, f);
    fclose(f);
}

/* What audio_bed_init() does around audio_bed_set_playlist(), with the one path
 * substituted.  Kept in step with it by hand — group F is what catches the path
 * itself drifting. */
static void bed_load(AudioBed *bed, const char *tag, const char *body)
{
    write_set(body);
    memset(bed, 0, sizeof(*bed));
    bed->state = AUDIO_BED_IDLE;
    snprintf(bed->tag, sizeof bed->tag, "%s", tag);

    Config cfg;
    config_init_path(&cfg, SET_PATH);
    if (config_load(&cfg) < 0) { bed->disabled = true; return; }
    audio_bed_set_playlist(bed, &cfg);
}

int main(void)
{
    AudioBed bed;

    printf("audio_bed_test — sound-set resolution\n\n");

    /* ── A: the playlist is the file's, in the file's order ────────────────── */
    printf("  A  playlist from the file, in order\n");
    bed_load(&bed, "platformer",
             "platformer_music=/opt/sound/officerunner1-mono.wav\n"
             "platformer_music2=/opt/sound/officerunner2-mono.wav\n"
             "platformer_music3=/opt/sound/officerunner3-mono.wav\n");
    check(!bed.disabled, "A: three named tracks must not disable the bed");
    check(bed.count == 3, "A: count must be 3");
    check(strcmp(bed.track[0], "/opt/sound/officerunner1-mono.wav") == 0,
          "A: slot 0 comes from the UNNUMBERED key");
    check(strcmp(bed.track[2], "/opt/sound/officerunner3-mono.wav") == 0,
          "A: slot 2 comes from _music3");
    check(bed.next == 0, "A: the cursor starts at 0");

    /* A negative control for A: another game's keys in the same file must be
     * invisible, or the tag prefix is not doing anything. */
    bed_load(&bed, "platformer", "snake_music=/opt/sound/snake1-mono.wav\n");
    check(bed.disabled && bed.count == 0,
          "A-control: a DIFFERENT tag's key must not be picked up");

    /* ── B: present-and-empty track 1 is the off switch ────────────────────── */
    printf("  B  the unnumbered key, present and empty, is the off switch\n");
    bed_load(&bed, "snake",
             "snake_music=\n"
             "snake_music2=/opt/sound/snake2-mono.wav\n");
    check(bed.disabled, "B: an empty track 1 must disable the whole bed");
    check(bed.count == 0, "B: and must not collect the later track");

    /* ── C: ABSENT track 1 is NOT the off switch ───────────────────────────── */
    printf("  C  the unnumbered key ABSENT is a gap, not the off switch\n");
    bed_load(&bed, "snake",
             "snake_music2=/opt/sound/snake2-mono.wav\n"
             "snake_music3=/opt/sound/snake3-mono.wav\n");
    check(!bed.disabled, "C: a missing track-1 LINE must not disable the bed");
    check(bed.count == 2, "C: the two later tracks must both be collected");
    check(strcmp(bed.track[0], "/opt/sound/snake2-mono.wav") == 0,
          "C: slot 0 must hold the first track that EXISTS");

    /* ── D: a middle gap, and a file naming nothing ────────────────────────── */
    printf("  D  a middle gap is skipped; a file naming nothing is silence\n");
    bed_load(&bed, "pong",
             "pong_music=/opt/sound/pong1-mono.wav\n"
             "pong_music4=/opt/sound/pong4-mono.wav\n");
    check(!bed.disabled && bed.count == 2, "D: a gap in the middle ends nothing");
    check(strcmp(bed.track[1], "/opt/sound/pong4-mono.wav") == 0,
          "D: the track after the gap packs down to slot 1");

    bed_load(&bed, "pong", "# a comment and nothing else\n\n");
    check(bed.disabled && bed.count == 0,
          "D: a set file that names no music must disable the bed");

    bed_load(&bed, "pong", "pong_music2=\npong_music3=\n");
    check(bed.disabled && bed.count == 0,
          "D: later keys all empty is also silence, via count==0");

    /* ── E: the cap bounds the walk ─────────────────────────────────────────── */
    printf("  E  AUDIO_BED_MAX_TRACKS bounds the walk\n");
    {
        char body[2048]; size_t n = 0;
        n += (size_t)snprintf(body + n, sizeof body - n, "frogger_music=/opt/sound/f1.wav\n");
        /* Keys _music2 .. _music<MAX+1>: the last one is one PAST the cap. */
        for (int i = 2; i <= AUDIO_BED_MAX_TRACKS + 1; i++)
            n += (size_t)snprintf(body + n, sizeof body - n,
                                  "frogger_music%d=/opt/sound/f%d.wav\n", i, i);
        bed_load(&bed, "frogger", body);
    }
    check(bed.count == AUDIO_BED_MAX_TRACKS,
          "E: count must stop at AUDIO_BED_MAX_TRACKS");
    check(strcmp(bed.track[AUDIO_BED_MAX_TRACKS - 1], "/opt/sound/f8.wav") == 0,
          "E: the last collected track is the key AT the cap, not the overflow key");

    /* E-control: one BELOW the cap must not be clipped, or E would pass on a
     * loop that stopped early for an unrelated reason. */
    {
        char body[2048]; size_t n = 0;
        n += (size_t)snprintf(body + n, sizeof body - n, "frogger_music=/opt/sound/f1.wav\n");
        for (int i = 2; i <= AUDIO_BED_MAX_TRACKS - 1; i++)
            n += (size_t)snprintf(body + n, sizeof body - n,
                                  "frogger_music%d=/opt/sound/f%d.wav\n", i, i);
        bed_load(&bed, "frogger", body);
    }
    check(bed.count == AUDIO_BED_MAX_TRACKS - 1,
          "E-control: MAX-1 tracks must all be collected");

    /* ── F: the install path the generator writes to ────────────────────────── */
    printf("  F  config_load_sound_set builds the installed path\n");
    {
        Config cfg;
        /* No such file: the return is -1 and that is the point — the PATH is what
         * is being asserted, and it is set before the open is attempted. */
        int rc = config_load_sound_set(&cfg, "brick_breaker");
        check(rc < 0, "F: an absent set file must report -1, not succeed");
        check(strcmp(cfg.filepath,
                     CONFIG_SOUND_SET_DIR "/brick_breaker" CONFIG_SOUND_SET_EXT) == 0,
              "F: the path is <CONFIG_SOUND_SET_DIR>/<tag><CONFIG_SOUND_SET_EXT>");

        /* A NULL tag must not build a path containing "(null)" — it reads the
         * shared set instead. */
        config_load_sound_set(&cfg, NULL);
        check(strcmp(cfg.filepath,
                     CONFIG_SOUND_SET_DIR "/default" CONFIG_SOUND_SET_EXT) == 0,
              "F: a NULL tag reads the default set");
        config_load_sound_set(&cfg, "");
        check(strcmp(cfg.filepath,
                     CONFIG_SOUND_SET_DIR "/default" CONFIG_SOUND_SET_EXT) == 0,
              "F: an EMPTY tag reads the default set too");
    }

    /* ── G: a path too long for CONFIG_VAL_LEN is truncated, never overrun ─── */
    printf("  G  an over-long path is bounded\n");
    {
        char body[512];
        snprintf(body, sizeof body, "tetris_music=/opt/sound/");
        size_t n = strlen(body);
        for (size_t i = 0; i < 200 && n < sizeof body - 2; i++) body[n++] = 'x';
        body[n++] = '\n'; body[n] = '\0';
        bed_load(&bed, "tetris", body);
    }
    check(bed.count == 1, "G: an over-long path is still a track");
    check(strlen(bed.track[0]) < CONFIG_VAL_LEN,
          "G: and it is bounded by CONFIG_VAL_LEN");

    unlink(SET_PATH);

    printf("\n%s  %d check(s), %d failure(s)\n",
           failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
