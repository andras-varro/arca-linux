/*
 * gameover_flow_test.c — the game-over flow's phase decision, on the host.
 *
 * gameover_phase_next() takes the time in the phase, whether an input edge
 * arrived and what the score / table say, and answers which phase to be in:
 * no framebuffer, no audio, no touch.  Every group asserts a CHANGE of phase
 * (or the refusal of one), so a flow that never advances, or advances on the
 * press that ended the game, fails it.
 *
 * Build (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/gameover_flow_test \
 *       tests/gameover_flow_test.c common/start_menu.c common/common.c \
 *       common/framebuffer.c common/font.c common/touch_input.c common/hardware.c \
 *       common/config.c common/highscore.c common/keyboard.c common/audio.c \
 *       common/audio_gen.c common/audio_out.c common/audio_wav.c \
 *       common/gamepad.c common/input_slots.c common/input_scan.c -lm && \
 *   ./build/gameover_flow_test
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../common/common.h"
#include "../common/start_menu.h"

static int failures = 0;
static int checks   = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
    else       { printf("  ok:   %s\n", what); }
}

#define SHOW   GAMEOVER_PHASE_SHOW
#define NAME   GAMEOVER_PHASE_NAME
#define SCORES GAMEOVER_PHASE_SCORES
#define DONE   GAMEOVER_PHASE_DONE

/* cur, ms, input, qualifies, has_table, name_done */
static GameOverPhase nx(GameOverPhase cur, uint32_t ms, bool in, bool q, bool t, bool nd)
{
    return gameover_phase_next(cur, ms, in, q, t, nd);
}

static void group_constants(void)
{
    printf("1. constants\n");
    check(GAMEOVER_SHOW_MS == 3000u && GAMEOVER_ARM_MS == 1000u, "SHOW 3000 ms, ARM 1000 ms");
    check(GAMEOVER_SCORES_MS == SM_ATTRACT_MS, "SCORES holds as long as the start menu's SCORES page");
}

static void group_show(void)
{
    printf("2. SHOW\n");
    check(nx(SHOW, 0, false, false, true, false) == SHOW, "t=0 no input stays");
    check(nx(SHOW, 2999, false, true, true, false) == SHOW, "t=2999 no input stays");
    check(nx(SHOW, 3000, false, false, true, false) == SCORES, "t=3000 leaves (no qualify -> SCORES)");
    check(nx(SHOW, 3000, false, true, true, false) == NAME, "t=3000 qualifying -> NAME");
    check(nx(SHOW, 0, true, true, true, false) == SHOW, "input at t=0 ignored (the press that ended the game)");
    check(nx(SHOW, 999, true, true, true, false) == SHOW, "input at t=999 ignored (still arming)");
    check(nx(SHOW, 1000, true, false, true, false) == SCORES, "input at t=1000 skips (no qualify -> SCORES)");
    check(nx(SHOW, 1500, true, true, true, false) == NAME, "input at t=1500 skips, qualifying -> NAME");
}

static void group_null_table(void)
{
    printf("3. NULL table (no name entry, no scores page)\n");
    check(nx(SHOW, 2999, false, false, false, false) == SHOW, "SHOW holds to 3000");
    check(nx(SHOW, 3000, false, false, false, false) == DONE, "SHOW then DONE after 3000");
    check(nx(SHOW, 1200, true, false, false, false) == DONE, "input after arming -> DONE");
    check(nx(SHOW, 3000, false, true, false, false) == DONE, "qualifies is moot without a table");
}

static void group_name(void)
{
    printf("4. NAME\n");
    check(nx(NAME, 0, false, true, true, false) == NAME, "stays while the name is not done");
    check(nx(NAME, 20000, true, true, true, false) == NAME, "neither time nor input leave it");
    check(nx(NAME, 0, false, true, true, true) == SCORES, "name done -> SCORES");
    check(nx(SHOW, 3000, false, true, true, true) == SCORES, "SHOW never re-enters NAME once done");
}

static void group_scores(void)
{
    printf("5. SCORES\n");
    check(nx(SCORES, 0, false, true, true, true) == SCORES, "t=0 stays");
    check(nx(SCORES, 14999, false, true, true, true) == SCORES, "t=14999 stays");
    check(nx(SCORES, 15000, false, true, true, true) == DONE, "t=15000 -> DONE");
    check(nx(SCORES, 200, true, false, true, false) == DONE, "any input ends it early");
    check(nx(DONE, 0, false, true, true, true) == DONE, "DONE is final");
}

static void group_init(void)
{
    printf("6. init\n");
    GameOverScreen g;
    gameover_init(&g, NULL, 42, NULL, NULL, NULL, NULL, NULL, NULL);
    check(g.phase == SHOW && g.pending_draw, "starts in SHOW owing a frame");
    check(strcmp(g.title, "GAME OVER") == 0, "default title");
    check(strcmp(g.game_title, "HIGH SCORES") == 0, "default scores heading");
    gameover_init(&g, NULL, 1, "you win!", "level 3", "snake", NULL, NULL, NULL);
    check(strcmp(g.title, "YOU WIN!") == 0 && strcmp(g.game_title, "SNAKE") == 0,
          "title and game title uppercased");
    bool all_held = g.was_touching;
    for (int b = 0; b < BTN_ID_COUNT; b++) all_held = all_held && g.prev_held[b];
    check(all_held, "edges seeded as held: the press that ended the game is no edge");
}

int main(void)
{
    group_constants();
    group_show();
    group_null_table();
    group_name();
    group_scores();
    group_init();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
