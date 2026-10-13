/*
 * High Score System — central component for Arca native games.
 *
 * Provides persistent top-5 leaderboards per game, touch-driven name entry
 * with an on-screen keyboard, and gold/silver/bronze ranking display.
 *
 * Files: /home/root/data/<game_name>.hig  (plain text, one "NAME SCORE\n" per line)
 */

#ifndef HIGHSCORE_H
#define HIGHSCORE_H

#include "framebuffer.h"
#include "touch_input.h"
#include "gamepad.h"

#define HS_MAX_ENTRIES  5
#define HS_NAME_LEN     11     /* 10 visible chars + '\0' */
#define HS_DATA_DIR     "/home/root/data"

typedef struct {
    char name[HS_NAME_LEN];
    int  score;
} HSEntry;

typedef struct {
    HSEntry entries[HS_MAX_ENTRIES];
    int     count;
    char    game_name[32];
} HighScoreTable;

/* Initialise table and associate a game name (used as the filename stem). */
void hs_init(HighScoreTable *t, const char *game_name);

/* Load entries from disk.  Silent on missing file. */
void hs_load(HighScoreTable *t);

/* Write entries to disk.  Silent on failure. */
void hs_save(const HighScoreTable *t);

/* Wipe all entries and rewrite (empty) file. */
void hs_reset(HighScoreTable *t);

/* Return the 0-based rank at which 'score' would be inserted,
   or -1 if it doesn't qualify for the top HS_MAX_ENTRIES.    */
int  hs_qualifies(const HighScoreTable *t, int score);

/* Insert name+score at the correct rank, dropping the lowest if full.
   Always call hs_qualifies() first.                              */
void hs_insert(HighScoreTable *t, const char *name, int score);

/* Draw the leaderboard table into 'fb' at position (x, y) in a box of
   width 'w'.  Returns the pixel height consumed.                  */
int  hs_draw(Framebuffer *fb, const HighScoreTable *t, int x, int y, int w);

/* Blocking UI: show an on-screen keyboard and let the player enter a name.
   Writes a NUL-terminated string of at most HS_NAME_LEN-1 chars into
   name_buf.  'score' is shown as context ("NEW HIGH SCORE: %d").
   gm (may be NULL = touch only) adds pad / keyboard control, see keyboard.h. */
void hs_enter_name(Framebuffer *fb, TouchInput *touch, GamepadManager *gm,
                   char *name_buf, int score);

/* Drain pending touch events after hs_enter_name to prevent ghost taps. */
void hs_drain_touches(TouchInput *touch);

#endif /* HIGHSCORE_H */
