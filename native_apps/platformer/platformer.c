/*
 * Office Runner — Mario-style side-scrolling platformer
 * Native C implementation for Arca embedded device (300 MHz ARM)
 *
 * Features:
 *   - Tile-based scrolling levels with camera system
 *   - Physics engine with coyote time, jump buffering, variable jump height
 *   - Procedurally drawn sprites (no PPM assets needed)
 *   - Enemy AI with stomp-kill mechanic
 *   - Gamepad, keyboard, and touch input via unified gamepad module
 *   - High scores, LED effects, audio feedback
 *   - Start menu, pause (modal dialog), level complete, game-over screens
 *
 * Follows project conventions established in frogger.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>
#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/common.h"
#include "../common/hardware.h"
#include "../common/highscore.h"
#include "../common/audio.h"
#include "../common/audio_bed.h"
#include "../common/config.h"
#include "../common/gamepad.h"
#include "../common/start_menu.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Tile System Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TILE_EMPTY      0
#define TILE_SOLID      1
#define TILE_PLATFORM   2
#define TILE_HAZARD     3
#define TILE_COIN       4
#define TILE_GOAL       5
#define TILE_SPAWN      6
#define TILE_ENEMY      7

#define TILE_SIZE       16
#define MAX_LEVEL_WIDTH  120
#define MAX_LEVEL_HEIGHT 30

/* ═══════════════════════════════════════════════════════════════════════════
 * Physics Constants
 *
 * Every speed, acceleration and frame count in this file is per TICK — one
 * fixed 1/20 s step of game_tick() — not per drawn frame.  update_game() runs
 * as many ticks as elapsed time calls for, so the values below (authored as
 * px/frame) keep their feel at any frame rate.  TICK_S is the pace: they were
 * tuned on a loop that drew well under its nominal 30 fps, and at 30 ticks/s
 * the game played ~1.8x faster than it had (operator, 2026-10-04).  A fixed
 * step rather than a variable dt keeps the jump arc exact: Euler with a
 * varying dt changes jump height with frame time, and tile collision and the
 * one-way platform test (feet_prev = y + h - vy) assume each step moved by
 * exactly vy.  The tick-counted timers are COYOTE_TIME, JUMP_BUFFER_TIME,
 * INVINCIBLE_TIME, DEATH_ANIM_FRAMES, the player and enemy anim_timer
 * periods, current_frame and the camera's 0.12 smoothing factor.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TICK_S           (1.0f / 20.0f)  /* one game_tick(), seconds: the pace */
#define MAX_FRAME_DT     0.1f   /* longest gap one frame may integrate, seconds */
#define MAX_TICKS_FRAME  4      /* tick cap per frame; the remainder is dropped */

#define GRAVITY          0.55f
#define JUMP_VELOCITY   -8.5f
#define JUMP_CUT         0.4f
#define MAX_FALL_SPEED   11.0f
#define WALK_SPEED       3.0f
#define RUN_SPEED        5.0f
#define ACCELERATION     0.5f
#define DECELERATION     0.3f
#define COYOTE_TIME      5
#define JUMP_BUFFER_TIME 6
#define STOMP_BOUNCE    -6.0f

/* ═══════════════════════════════════════════════════════════════════════════
 * Game Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_ENEMIES      20
#define MAX_LEVELS       3
#define INITIAL_LIVES    3
#define TRAINING_LIVES   10      /* Training mode: lives a game starts with */
#define TRAINING_COINS_PER_LIFE 50  /* Training mode: coins per extra life */
#define MAX_LIFE_ICONS   5       /* HUD draws this many, then falls back to "xN" */
#define HUD_HEIGHT       40
#define DEATH_ANIM_FRAMES 20
#define INVINCIBLE_TIME  60
#define COIN_SCORE       100
#define STOMP_SCORE      200
#define LEVEL_BONUS      1000
#define ENEMY_WALKER     0

/* ═══════════════════════════════════════════════════════════════════════════
 * Color Palette
 * ═══════════════════════════════════════════════════════════════════════════ */

#define COLOR_SKY_TOP       RGB(135, 206, 235)
#define COLOR_SKY_BOTTOM    RGB(100, 180, 220)
#define COLOR_GROUND        RGB(139, 90, 43)
#define COLOR_GROUND_TOP    RGB(76, 153, 0)
#define COLOR_GROUND_DARK   RGB(100, 65, 30)
#define COLOR_GROUND_LINE   RGB(80, 50, 20)
#define COLOR_PLATFORM_BG   RGB(160, 120, 80)
#define COLOR_PLATFORM_TOP  RGB(180, 140, 100)
#define COLOR_PLATFORM_LINE RGB(120, 85, 55)
#define COLOR_HAZARD_BG     RGB(200, 50, 30)
#define COLOR_HAZARD_STRIPE RGB(255, 200, 0)
#define COLOR_SKIN          RGB(255, 210, 170)
#define COLOR_HAIR          RGB(60, 40, 20)
#define COLOR_SHIRT         RGB(50, 100, 180)
#define COLOR_SHIRT_DARK    RGB(35, 70, 130)
#define COLOR_TIE           RGB(200, 40, 40)
#define COLOR_PANTS         RGB(50, 50, 60)
#define COLOR_SHOES         RGB(30, 30, 30)
#define COLOR_ENEMY_BODY    RGB(180, 180, 180)
#define COLOR_ENEMY_DARK    RGB(120, 120, 120)
#define COLOR_ENEMY_EYE     RGB(255, 60, 60)
#define COLOR_ENEMY_PAPER   RGB(240, 240, 230)
#define COLOR_COIN_OUTER    RGB(255, 200, 0)
#define COLOR_COIN_INNER    RGB(255, 230, 100)
#define COLOR_COIN_SHINE    RGB(255, 255, 200)
#define COLOR_FLAG_POLE     RGB(180, 180, 180)
#define COLOR_FLAG_CLOTH    RGB(0, 180, 0)
#define COLOR_HUD_BG        RGB(20, 20, 30)

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    SCREEN_MENU,
    SCREEN_PLAYING,
    SCREEN_PAUSED,
    SCREEN_LEVEL_COMPLETE,
    SCREEN_GAME_OVER
} GameScreen;

typedef enum {
    PSTATE_IDLE,
    PSTATE_RUNNING,
    PSTATE_JUMPING,
    PSTATE_FALLING,
    PSTATE_DYING
} PlayerState;

typedef struct {
    float x, y;
    float vx, vy;
    int width, height;
    bool on_ground;
    bool facing_right;
    int anim_frame;
    int anim_timer;
    PlayerState state;
    int lives;
    int coyote_frames;
    int jump_buffer;
    bool jump_held;
    float spawn_x, spawn_y;
    int death_timer;
    int invincible_timer;
} Player;

typedef struct {
    float x, y;
    float vx;
    int width, height;
    bool alive;
    bool facing_right;
    int anim_frame, anim_timer;
    int type;
} Enemy;

typedef struct {
    float x, y;
    float target_x, target_y;
    int level_width_px;
    int level_height_px;
} Camera;

typedef struct {
    bool active;
    int type;
    uint32_t start_time;
} LEDEffect;

/* ═══════════════════════════════════════════════════════════════════════════
 * Global State
 * ═══════════════════════════════════════════════════════════════════════════ */

static Framebuffer fb;
static TouchInput touch;
static Audio audio;
static GamepadManager gamepad;
static InputState input;
static HighScoreTable hs_table;
static GameOverScreen gos;
static bool running = true;
static GameScreen current_screen = SCREEN_MENU;

/* Start menu (../common/start_menu.h): START, DIFFICULTY, EXIT.  EASY is the
 * old TRAINING mode (TRAINING_LIVES, an extra life per TRAINING_COINS_PER_LIFE
 * coins); NORMAL is the game as it always played. */
enum { DIFF_EASY, DIFF_NORMAL, DIFF_COUNT };
static const char *const DIFF_NAMES[DIFF_COUNT] = { "EASY", "NORMAL" };
static StartMenu menu;
static int menu_start_idx, menu_diff_idx, menu_exit_idx;
static int difficulty = DIFF_NORMAL;

/* The attract cycle's DEMO: an AI runs the level 1 field on SCREEN_MENU while
 * the widget shows its DEMO page.  Silent, LED-dark, no score kept, never a
 * game over; it ends (start_menu_demo_over) on the AI's first death, the goal,
 * or when it is stuck.  Always a fresh NORMAL run (demo_sync). */
static bool demo_running = false;
static bool demo_on(void) { return demo_running; }
#define DEMO_STUCK_TICKS 40      /* ticks without forward progress before the demo gives up */
static float demo_last_x;
static int   demo_stuck;

static uint8_t level_tiles[MAX_LEVEL_HEIGHT][MAX_LEVEL_WIDTH];
static bool    coins_collected[MAX_LEVEL_HEIGHT][MAX_LEVEL_WIDTH];
static int     level_width, level_height;
static int     current_level;
static int     total_coins, collected_coins;

static Player  player;
static Enemy   enemies[MAX_ENEMIES];
static int     enemy_count;
static Camera  camera;

static int     score;
static int     game_lives;
/* Training mode: TRAINING_LIVES at game start, one more life per
 * TRAINING_COINS_PER_LIFE coins.  Toggled from the pause dialog; the credit
 * counts only while the mode is on, and is reset whenever it is switched on. */
static bool    training_mode;
static int     training_coin_credit;
static uint32_t current_frame;   /* game ticks, not drawn frames */
static uint32_t level_complete_timer;

/* The play clock (update_game).  play_clock_live is false whenever the last
 * update_game() call was outside SCREEN_PLAYING, so the first playing frame
 * after the start menu, a pause, a level change or a restart re-baselines
 * instead of integrating the time spent away. */
static bool     play_clock_live;
static uint32_t play_last_ms;
static float    tick_acc;
/* Jump edges sampled once per frame by gamepad_poll(), held here until exactly
 * one tick — the first to run — consumes them.  A frame that runs no tick
 * leaves them pending for the next frame's first tick. */
static bool     jump_press_pending;
static bool     jump_release_pending;
static LEDEffect led_effect;

static Button menu_button;
static Button exit_button;
static ModalDialog pause_dialog;

/* ═══════════════════════════════════════════════════════════════════════════
 * Forward Declarations (for functions referenced before definition)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void player_die(void);
static void draw_coin(int sx, int sy);
static void draw_goal(int sx, int sy);

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void signal_handler(int sig) {
    (void)sig;
    running = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LED Effects
 * ═══════════════════════════════════════════════════════════════════════════ */

static void start_led_effect(int type) {
    if (demo_on()) return;   /* the demo is LED-dark */
    led_effect.active = true;
    led_effect.type = type;
    led_effect.start_time = get_time_ms();
}

static void update_led_effects(void) {
    if (!led_effect.active) return;
    uint32_t elapsed = get_time_ms() - led_effect.start_time;

    switch (led_effect.type) {
    case 1: /* Coin: yellow 150ms */
        if (elapsed < 150) hw_set_leds(HW_LED_COLOR_YELLOW);
        else { hw_leds_off(); led_effect.active = false; }
        break;
    case 2: /* Stomp: green 200ms */
        if (elapsed < 200) hw_set_leds(HW_LED_COLOR_GREEN);
        else { hw_leds_off(); led_effect.active = false; }
        break;
    case 3: /* Death: red pulse */
    {
        int pulse = (int)(elapsed / 200);
        int phase = (int)(elapsed % 200);
        if (pulse < 3) hw_set_red_led(phase < 100 ? 100 : 0);
        else { hw_leds_off(); led_effect.active = false; }
    }
    break;
    case 4: /* Level complete */
    {
        int phase = (int)((elapsed / 150) % 2);
        if (elapsed < 800) {
            if (phase == 0) hw_set_leds(HW_LED_COLOR_GREEN);
            else hw_set_leds(HW_LED_COLOR_YELLOW);
        } else { hw_leds_off(); led_effect.active = false; }
    }
    break;
    default:
        led_effect.active = false;
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Every effect is silent in the demo (demo_on()). */
static void play_jump_sound(void)           { if (!demo_on()) audio_jump(&audio); }
static void play_coin_sound(void)           { if (!demo_on()) audio_blip(&audio); }
static void play_stomp_sound(void)          { if (!demo_on()) audio_beep(&audio); }
static void play_death_sound(void)          { if (!demo_on()) audio_fail(&audio); }
/* ⚠️ A DIFFERENT sound from losing one life (operator, 2026-08-22): the two were
 * both audio_fail(), so nothing told a player whether they had another go coming.
 * It fires at the END of the death animation, where the run is actually decided —
 * not in player_die(), which cannot yet know. */
static void play_game_over_sound(void)      { if (!demo_on()) audio_gameover(&audio); }
static void play_extra_life_sound(void)     { if (!demo_on()) audio_success(&audio); }
static void play_level_complete_sound(void) { if (!demo_on()) audio_success(&audio); }

/* ── The music bed ───────────────────────────────────────────────────────────
 * The first game to run one, and since 2026-08-22 the state machine behind it
 * lives in `common/audio_bed.c` — read that header for the playlist, the four
 * states and the hold/resume rules, all of which shipped here first.  What is
 * left in this file is the only part that cannot be shared: which of THIS game's
 * screens count as playing and which as a hold.
 *
 * The bed is a streaming sample voice on the mix bus, so it costs one file, one
 * read-ahead buffer and no thread; the effects above mix OVER it because a full
 * bus refuses a voice rather than stealing the longest one (../common/audio.h).
 *
 * ⚠️ **This game's bed files are `officerunner<n>-mono.wav`, not
 * `platformer<n>-mono.wav`** — the operator sourced sets for eight games on
 * 2026-08-22 and this one kept its own name, so the stem is passed separately
 * from the tag.  The config keys stay `platformer_music`, `platformer_music2` …
 */
/* ⚠️ Literals, not macros: `check-bed-files.sh` parses this call to prove the
 * files a game asks for exist, and it cannot resolve a macro. */

static AudioBed bed;

/* One transition per loop iteration, called ABOVE the redraw block and before
 * audio_pump() — the game-over redraw runs a BLOCKING name entry, so a bed
 * serviced after it plays through the whole keyboard session.
 *
 *  - ⚠️ **SCREEN_LEVEL_COMPLETE is not want_play.**  The bed used to play through
 *    the 2.5 s overlay and under audio_success().  Dropping the screen silences
 *    that overlay AND — because the release completes and IDLE then takes the
 *    next playlist entry — is the entire mechanism of the per-level track change.
 *  - ⚠️ **Dying is want_HOLD, not want_stop.**  A death holds the bed (so
 *    audio_fail() is heard over near-silence for the 20-frame animation) and a
 *    respawn RESUMES the same track mid-bar.  Only game over lets it go.
 */
static void bed_service(void) {
    bool dying = (player.state == PSTATE_DYING);
    audio_bed_service(&bed, current_screen == SCREEN_PLAYING && !dying,
                      current_screen == SCREEN_PAUSED ||
                      (current_screen == SCREEN_PLAYING && dying));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tile Queries
 * ═══════════════════════════════════════════════════════════════════════════ */

static int tile_at(int tx, int ty) {
    if (tx < 0 || tx >= level_width || ty < 0 || ty >= level_height)
        return TILE_EMPTY;
    return level_tiles[ty][tx];
}

static bool tile_is_solid(int tx, int ty) {
    return tile_at(tx, ty) == TILE_SOLID;
}

static bool tile_is_platform(int tx, int ty) {
    return tile_at(tx, ty) == TILE_PLATFORM;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Level Building Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void level_fill(int x1, int y1, int x2, int y2, uint8_t tile) {
    for (int y = y1; y <= y2 && y < MAX_LEVEL_HEIGHT; y++)
        for (int x = x1; x <= x2 && x < MAX_LEVEL_WIDTH; x++)
            if (x >= 0 && y >= 0)
                level_tiles[y][x] = tile;
}

static void level_platform(int x, int y, int len, uint8_t tile) {
    for (int i = 0; i < len && (x + i) < MAX_LEVEL_WIDTH; i++)
        if (x + i >= 0 && y >= 0 && y < MAX_LEVEL_HEIGHT)
            level_tiles[y][x + i] = tile;
}

static void level_coins(int x, int y, int count) {
    for (int i = 0; i < count && (x + i) < MAX_LEVEL_WIDTH; i++)
        if (x + i >= 0 && y >= 0 && y < MAX_LEVEL_HEIGHT)
            level_tiles[y][x + i] = TILE_COIN;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Level 1 — Tutorial (100 tiles wide)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_level_1(void) {
    level_width = 100;
    level_height = 30;
    memset(level_tiles, TILE_EMPTY, sizeof(level_tiles));
    memset(coins_collected, 0, sizeof(coins_collected));

    /* Ground rows 28-29 */
    level_fill(0, 28, 99, 29, TILE_SOLID);

    /* Gaps */
    level_fill(18, 28, 20, 29, TILE_EMPTY);
    level_fill(38, 28, 41, 29, TILE_EMPTY);
    level_fill(60, 28, 62, 29, TILE_EMPTY);
    level_fill(78, 28, 81, 29, TILE_EMPTY);

    /* Spawn */
    level_tiles[27][2] = TILE_SPAWN;

    /* Low platforms */
    level_platform(10, 25, 4, TILE_PLATFORM);
    level_platform(22, 25, 5, TILE_PLATFORM);
    level_platform(35, 24, 3, TILE_PLATFORM);
    level_platform(45, 25, 4, TILE_PLATFORM);
    level_platform(55, 24, 5, TILE_PLATFORM);
    level_platform(65, 25, 4, TILE_PLATFORM);
    level_platform(75, 24, 3, TILE_PLATFORM);
    level_platform(85, 25, 4, TILE_PLATFORM);

    /* Higher platforms */
    level_platform(14, 22, 3, TILE_PLATFORM);
    level_platform(30, 21, 4, TILE_PLATFORM);
    level_platform(50, 22, 3, TILE_PLATFORM);
    level_platform(70, 21, 4, TILE_PLATFORM);

    /* Step platforms over gaps */
    level_platform(17, 26, 2, TILE_PLATFORM);
    level_platform(20, 26, 2, TILE_PLATFORM);
    level_platform(37, 26, 2, TILE_PLATFORM);
    level_platform(41, 26, 2, TILE_PLATFORM);
    level_platform(59, 26, 2, TILE_PLATFORM);
    level_platform(62, 26, 2, TILE_PLATFORM);
    level_platform(77, 26, 2, TILE_PLATFORM);
    level_platform(81, 26, 2, TILE_PLATFORM);

    /* Ground coins */
    level_coins(5, 27, 3);
    level_coins(13, 27, 3);
    level_coins(24, 27, 3);
    level_coins(43, 27, 3);
    level_coins(53, 27, 3);
    level_coins(67, 27, 3);
    level_coins(84, 27, 3);
    level_coins(90, 27, 3);

    /* Platform coins */
    level_coins(11, 24, 2);
    level_coins(23, 24, 3);
    level_coins(36, 23, 2);
    level_coins(46, 24, 2);
    level_coins(56, 23, 3);
    level_coins(66, 24, 2);
    level_coins(76, 23, 2);
    level_coins(86, 24, 2);

    /* High platform coins */
    level_coins(15, 21, 2);
    level_coins(31, 20, 2);
    level_coins(51, 21, 2);
    level_coins(71, 20, 2);

    /* Enemies */
    level_tiles[27][15] = TILE_ENEMY;
    level_tiles[27][30] = TILE_ENEMY;
    level_tiles[27][48] = TILE_ENEMY;
    level_tiles[27][70] = TILE_ENEMY;
    level_tiles[27][88] = TILE_ENEMY;

    /* Goal */
    level_tiles[27][97] = TILE_GOAL;
    level_tiles[26][97] = TILE_GOAL;
    level_tiles[25][97] = TILE_GOAL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Level 2 — Intermediate (110 tiles wide)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_level_2(void) {
    level_width = 110;
    level_height = 30;
    memset(level_tiles, TILE_EMPTY, sizeof(level_tiles));
    memset(coins_collected, 0, sizeof(coins_collected));

    level_fill(0, 28, 109, 29, TILE_SOLID);

    /* Gaps */
    level_fill(12, 28, 15, 29, TILE_EMPTY);
    level_fill(25, 28, 29, 29, TILE_EMPTY);
    level_fill(40, 28, 44, 29, TILE_EMPTY);
    level_fill(55, 28, 58, 29, TILE_EMPTY);
    level_fill(68, 28, 73, 29, TILE_EMPTY);
    level_fill(85, 28, 89, 29, TILE_EMPTY);

    /* Hazard section */
    level_fill(48, 28, 52, 28, TILE_HAZARD);

    level_tiles[27][2] = TILE_SPAWN;

    /* Staircase */
    level_platform(10, 26, 3, TILE_PLATFORM);
    level_platform(15, 25, 3, TILE_PLATFORM);
    level_platform(19, 24, 3, TILE_PLATFORM);

    /* Bridge */
    level_platform(24, 26, 2, TILE_PLATFORM);
    level_platform(29, 26, 2, TILE_PLATFORM);

    /* Vertical challenge */
    level_platform(33, 25, 3, TILE_PLATFORM);
    level_platform(36, 22, 3, TILE_PLATFORM);
    level_platform(33, 19, 3, TILE_PLATFORM);

    /* Hazard bypass */
    level_platform(47, 25, 2, TILE_PLATFORM);
    level_platform(50, 23, 2, TILE_PLATFORM);
    level_platform(53, 25, 2, TILE_PLATFORM);

    /* More platforms */
    level_platform(58, 26, 3, TILE_PLATFORM);
    level_platform(62, 25, 3, TILE_PLATFORM);
    level_platform(66, 24, 3, TILE_PLATFORM);

    level_platform(69, 26, 2, TILE_PLATFORM);
    level_platform(72, 26, 2, TILE_PLATFORM);

    level_platform(76, 25, 4, TILE_PLATFORM);
    level_platform(82, 24, 3, TILE_PLATFORM);
    level_platform(86, 26, 2, TILE_PLATFORM);
    level_platform(89, 26, 2, TILE_PLATFORM);
    level_platform(92, 25, 5, TILE_PLATFORM);
    level_platform(100, 25, 4, TILE_PLATFORM);

    /* Coins */
    level_coins(5, 27, 4);
    level_coins(11, 25, 2);
    level_coins(16, 24, 2);
    level_coins(20, 23, 2);
    level_coins(34, 24, 2);
    level_coins(37, 21, 2);
    level_coins(34, 18, 2);
    level_coins(48, 24, 1);
    level_coins(51, 22, 1);
    level_coins(59, 25, 2);
    level_coins(63, 24, 2);
    level_coins(67, 23, 2);
    level_coins(77, 24, 3);
    level_coins(83, 23, 2);
    level_coins(93, 24, 4);
    level_coins(101, 24, 3);

    /* Enemies */
    level_tiles[27][8] = TILE_ENEMY;
    level_tiles[27][20] = TILE_ENEMY;
    level_tiles[27][35] = TILE_ENEMY;
    level_tiles[27][60] = TILE_ENEMY;
    level_tiles[27][76] = TILE_ENEMY;
    level_tiles[27][95] = TILE_ENEMY;
    level_tiles[24][34] = TILE_ENEMY;
    level_tiles[24][93] = TILE_ENEMY;

    level_tiles[27][107] = TILE_GOAL;
    level_tiles[26][107] = TILE_GOAL;
    level_tiles[25][107] = TILE_GOAL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Level 3 — Hard (115 tiles wide)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_level_3(void) {
    level_width = 115;
    level_height = 30;
    memset(level_tiles, TILE_EMPTY, sizeof(level_tiles));
    memset(coins_collected, 0, sizeof(coins_collected));

    /* Sparse ground with gaps */
    level_fill(0, 28, 9, 29, TILE_SOLID);
    level_fill(14, 28, 22, 29, TILE_SOLID);
    level_fill(28, 28, 35, 29, TILE_SOLID);
    level_fill(42, 28, 50, 29, TILE_SOLID);
    level_fill(57, 28, 62, 29, TILE_SOLID);
    level_fill(70, 28, 75, 29, TILE_SOLID);
    level_fill(82, 28, 88, 29, TILE_SOLID);
    level_fill(95, 28, 100, 29, TILE_SOLID);
    level_fill(107, 28, 114, 29, TILE_SOLID);

    /* Hazard pits */
    level_fill(10, 28, 13, 29, TILE_HAZARD);
    level_fill(36, 28, 41, 29, TILE_HAZARD);
    level_fill(63, 28, 69, 29, TILE_HAZARD);
    level_fill(89, 28, 94, 29, TILE_HAZARD);

    level_tiles[27][2] = TILE_SPAWN;

    /* Platforms */
    level_platform(8, 26, 2, TILE_PLATFORM);
    level_platform(11, 24, 2, TILE_PLATFORM);
    level_platform(14, 22, 3, TILE_PLATFORM);
    level_platform(19, 20, 3, TILE_PLATFORM);
    level_platform(15, 26, 2, TILE_PLATFORM);

    level_platform(23, 25, 2, TILE_PLATFORM);
    level_platform(26, 26, 2, TILE_PLATFORM);

    level_platform(35, 26, 2, TILE_PLATFORM);
    level_platform(38, 24, 2, TILE_PLATFORM);
    level_platform(41, 26, 2, TILE_PLATFORM);

    level_platform(44, 25, 4, TILE_PLATFORM);
    level_platform(46, 22, 3, TILE_PLATFORM);
    level_platform(44, 19, 4, TILE_PLATFORM);
    level_platform(48, 17, 3, TILE_PLATFORM);

    level_platform(52, 25, 2, TILE_PLATFORM);
    level_platform(55, 24, 2, TILE_PLATFORM);

    level_platform(62, 26, 2, TILE_PLATFORM);
    level_platform(65, 24, 2, TILE_PLATFORM);
    level_platform(68, 26, 2, TILE_PLATFORM);

    level_platform(73, 25, 3, TILE_PLATFORM);
    level_platform(77, 23, 3, TILE_PLATFORM);
    level_platform(80, 25, 2, TILE_PLATFORM);

    level_platform(88, 26, 2, TILE_PLATFORM);
    level_platform(91, 24, 2, TILE_PLATFORM);
    level_platform(94, 26, 2, TILE_PLATFORM);

    level_platform(98, 25, 3, TILE_PLATFORM);
    level_platform(102, 23, 3, TILE_PLATFORM);
    level_platform(105, 25, 2, TILE_PLATFORM);

    /* Coins */
    level_coins(4, 27, 3);
    level_coins(9, 25, 1);
    level_coins(12, 23, 1);
    level_coins(15, 21, 2);
    level_coins(20, 19, 2);
    level_coins(30, 27, 3);
    level_coins(36, 25, 1);
    level_coins(39, 23, 1);
    level_coins(45, 24, 2);
    level_coins(47, 21, 2);
    level_coins(45, 18, 3);
    level_coins(49, 16, 2);
    level_coins(53, 24, 1);
    level_coins(56, 23, 1);
    level_coins(58, 27, 3);
    level_coins(63, 25, 1);
    level_coins(66, 23, 1);
    level_coins(72, 27, 2);
    level_coins(74, 24, 2);
    level_coins(78, 22, 2);
    level_coins(84, 27, 3);
    level_coins(89, 25, 1);
    level_coins(92, 23, 1);
    level_coins(97, 27, 2);
    level_coins(99, 24, 2);
    level_coins(103, 22, 2);
    level_coins(109, 27, 3);

    /* Enemies */
    level_tiles[27][6] = TILE_ENEMY;
    level_tiles[27][17] = TILE_ENEMY;
    level_tiles[27][32] = TILE_ENEMY;
    level_tiles[27][46] = TILE_ENEMY;
    level_tiles[27][59] = TILE_ENEMY;
    level_tiles[27][73] = TILE_ENEMY;
    level_tiles[27][85] = TILE_ENEMY;
    level_tiles[27][98] = TILE_ENEMY;
    level_tiles[27][110] = TILE_ENEMY;
    level_tiles[24][45] = TILE_ENEMY;
    level_tiles[22][78] = TILE_ENEMY;

    level_tiles[27][112] = TILE_GOAL;
    level_tiles[26][112] = TILE_GOAL;
    level_tiles[25][112] = TILE_GOAL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Level Loading & Entity Spawning
 * ═══════════════════════════════════════════════════════════════════════════ */

static void spawn_enemies_from_tiles(void) {
    enemy_count = 0;
    total_coins = 0;
    collected_coins = 0;

    for (int y = 0; y < level_height; y++) {
        for (int x = 0; x < level_width; x++) {
            if (level_tiles[y][x] == TILE_ENEMY && enemy_count < MAX_ENEMIES) {
                Enemy *e = &enemies[enemy_count++];
                e->x = (float)(x * TILE_SIZE);
                e->y = (float)(y * TILE_SIZE);
                e->vx = 1.0f;
                e->width = TILE_SIZE - 2;
                e->height = TILE_SIZE - 2;
                e->alive = true;
                e->facing_right = (rand() % 2) ? true : false;
                if (!e->facing_right) e->vx = -e->vx;
                e->anim_frame = 0;
                e->anim_timer = 0;
                e->type = ENEMY_WALKER;
                level_tiles[y][x] = TILE_EMPTY;
            }
            if (level_tiles[y][x] == TILE_COIN)
                total_coins++;
        }
    }
}

static void reset_player_to_spawn(void) {
    player.x = player.spawn_x;
    player.y = player.spawn_y;
    player.vx = 0;
    player.vy = 0;
    player.on_ground = false;
    player.facing_right = true;
    player.anim_frame = 0;
    player.anim_timer = 0;
    player.state = PSTATE_IDLE;
    player.coyote_frames = 0;
    player.jump_buffer = 0;
    player.jump_held = false;
    player.death_timer = 0;
    player.invincible_timer = INVINCIBLE_TIME;
}

static void load_level(int level_num) {
    switch (level_num) {
    case 0: build_level_1(); break;
    case 1: build_level_2(); break;
    case 2: build_level_3(); break;
    default: build_level_1(); break;
    }

    /* Find spawn */
    player.spawn_x = 2.0f * TILE_SIZE;
    player.spawn_y = 27.0f * TILE_SIZE;
    for (int y = 0; y < level_height; y++) {
        for (int x = 0; x < level_width; x++) {
            if (level_tiles[y][x] == TILE_SPAWN) {
                player.spawn_x = (float)(x * TILE_SIZE);
                player.spawn_y = (float)(y * TILE_SIZE);
                level_tiles[y][x] = TILE_EMPTY;
            }
        }
    }

    player.width = 14;
    player.height = 28;
    player.lives = game_lives;

    spawn_enemies_from_tiles();
    reset_player_to_spawn();
    player.invincible_timer = 0;

    camera.level_width_px = level_width * TILE_SIZE;
    camera.level_height_px = level_height * TILE_SIZE;
    camera.x = player.x - (float)fb.width / 2.0f;
    camera.y = player.y - (float)fb.height / 2.0f;
    camera.target_x = camera.x;
    camera.target_y = camera.y;

    if (camera.x < 0) camera.x = 0;
    if (camera.y < 0) camera.y = 0;
    float max_cx = (float)(camera.level_width_px - (int)fb.width);
    float max_cy = (float)(camera.level_height_px - (int)fb.height);
    if (max_cx < 0) max_cx = 0;
    if (max_cy < 0) max_cy = 0;
    if (camera.x > max_cx) camera.x = max_cx;
    if (camera.y > max_cy) camera.y = max_cy;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Game Init
 * ═══════════════════════════════════════════════════════════════════════════ */

static void init_buttons(void) {
    button_init(&menu_button, LAYOUT_MENU_BTN_X, LAYOUT_MENU_BTN_Y,
                BTN_MENU_WIDTH, BTN_MENU_HEIGHT, "",
                BTN_MENU_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&exit_button, LAYOUT_EXIT_BTN_X, LAYOUT_EXIT_BTN_Y,
                BTN_EXIT_WIDTH, BTN_EXIT_HEIGHT, "",
                BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    modal_dialog_init(&pause_dialog, "PAUSED", NULL, 2);
    modal_dialog_set_button(&pause_dialog, 0, "RESUME", BTN_COLOR_PRIMARY, COLOR_WHITE);
    modal_dialog_set_button(&pause_dialog, 1, "EXIT", BTN_COLOR_DANGER, COLOR_WHITE);
}

static void reset_game(void) {
    score = 0;
    game_lives = training_mode ? TRAINING_LIVES : INITIAL_LIVES;
    training_coin_credit = 0;
    current_level = 0;
    current_frame = 0;
    level_complete_timer = 0;
    led_effect.active = false;
    load_level(current_level);
}

static void init_game(void) {
    init_buttons();
    hs_init(&hs_table, "platformer");
    hs_load(&hs_table);
    reset_game();

    /* Attract cycle: MENU -> DEMO -> SCORES, drawn by draw_all(). */
    start_menu_init(&menu, "OFFICE RUNNER",
                    "D-PAD: MOVE   A: JUMP   B: RUN\n"
                    "STOMP ENEMIES FROM ABOVE\n"
                    "COLLECT COINS   REACH THE FLAG", get_time_ms());
    menu_start_idx = start_menu_add_action(&menu, "START");
    menu_diff_idx  = start_menu_add_choice(&menu, "DIFFICULTY", DIFF_NAMES, DIFF_COUNT, DIFF_NORMAL);
    menu_exit_idx  = start_menu_add_action(&menu, "EXIT");
    start_menu_select(&menu, menu_start_idx, get_time_ms());
    start_menu_set_attract(&menu, true);

    /* No virtual D-pad TouchRegions and no on-screen controller overlay.
     * gamepad.c used to never clear a region's .held, so every zone latched on
     * first touch: the player ran in one direction forever and the overlay's
     * boxes stayed highlighted light-blue.  That latch is fixed now, but the
     * boxes were also drawn in different places from the regions that actually
     * received the taps.  This
     * game needs a real controller; draw_all() says so on the start menu
     * when none is connected, and the touch EXIT button still works either way.
     * Do not add regions back without fixing B2 first. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Collision Resolution
 * ═══════════════════════════════════════════════════════════════════════════ */

static void resolve_collisions_x(void) {
    int left_tile   = (int)player.x / TILE_SIZE;
    int right_tile  = (int)(player.x + player.width - 1) / TILE_SIZE;
    int top_tile    = (int)(player.y + 2) / TILE_SIZE;
    int bottom_tile = (int)(player.y + player.height - 2) / TILE_SIZE;

    for (int ty = top_tile; ty <= bottom_tile; ty++) {
        for (int tx = left_tile; tx <= right_tile; tx++) {
            if (tile_is_solid(tx, ty)) {
                if (player.vx > 0) {
                    player.x = (float)(tx * TILE_SIZE) - player.width;
                } else if (player.vx < 0) {
                    player.x = (float)((tx + 1) * TILE_SIZE);
                }
                player.vx = 0;
                return;
            }
        }
    }
}

static void resolve_collisions_y(void) {
    int left_tile   = (int)(player.x + 1) / TILE_SIZE;
    int right_tile  = (int)(player.x + player.width - 2) / TILE_SIZE;
    int top_tile    = (int)player.y / TILE_SIZE;
    int bottom_tile = (int)(player.y + player.height - 1) / TILE_SIZE;

    bool was_on_ground = player.on_ground;
    player.on_ground = false;

    for (int ty = top_tile; ty <= bottom_tile; ty++) {
        for (int tx = left_tile; tx <= right_tile; tx++) {
            int t = tile_at(tx, ty);

            if (t == TILE_SOLID) {
                if (player.vy > 0) {
                    player.y = (float)(ty * TILE_SIZE) - player.height;
                    player.vy = 0;
                    player.on_ground = true;
                } else if (player.vy < 0) {
                    player.y = (float)((ty + 1) * TILE_SIZE);
                    player.vy = 0;
                }
                return;
            }

            if (t == TILE_PLATFORM && player.vy > 0) {
                float platform_top = (float)(ty * TILE_SIZE);
                float feet_prev = player.y + player.height - player.vy;
                if (feet_prev <= platform_top + 2.0f) {
                    player.y = platform_top - player.height;
                    player.vy = 0;
                    player.on_ground = true;
                    return;
                }
            }
        }
    }

    /* Ground probe.  Standing on a surface, gravity sinks the feet less than
     * 1 px per tick, and the truncating bottom_tile above still files that in
     * the empty row — without this, on_ground flips every other tick and the
     * runner is in PSTATE_FALLING (drawn idle) half the time. */
    if (player.vy >= 0) {
        int   feet_row = (int)(player.y + player.height) / TILE_SIZE;
        float row_top  = (float)(feet_row * TILE_SIZE);
        if (player.y + player.height - row_top < 1.0f) {
            for (int tx = left_tile; tx <= right_tile; tx++) {
                int t = tile_at(tx, feet_row);
                if (t == TILE_SOLID ||
                    (t == TILE_PLATFORM &&
                     player.y + player.height - player.vy <= row_top + 2.0f)) {
                    player.y = row_top - player.height;
                    player.vy = 0;
                    player.on_ground = true;
                    return;
                }
            }
        }
    }

    if (was_on_ground && !player.on_ground && player.vy >= 0)
        player.coyote_frames = COYOTE_TIME;
}

static void check_tile_interactions(void) {
    int left_tile   = (int)(player.x + 2) / TILE_SIZE;
    int right_tile  = (int)(player.x + player.width - 3) / TILE_SIZE;
    int top_tile    = (int)(player.y + 2) / TILE_SIZE;
    int bottom_tile = (int)(player.y + player.height - 3) / TILE_SIZE;

    for (int ty = top_tile; ty <= bottom_tile; ty++) {
        for (int tx = left_tile; tx <= right_tile; tx++) {
            int t = tile_at(tx, ty);

            if (t == TILE_HAZARD) {
                player_die();
                return;
            }
            if (t == TILE_COIN && !coins_collected[ty][tx]) {
                coins_collected[ty][tx] = true;
                collected_coins++;
                score += COIN_SCORE;
                play_coin_sound();
                start_led_effect(1);
                /* Training mode: an extra life per TRAINING_COINS_PER_LIFE.
                 * The credit is a run counter, not collected_coins, which
                 * spawn_enemies_from_tiles() resets on every level load —
                 * per level it would almost never reach the threshold. */
                if (training_mode) {
                    training_coin_credit++;
                    if (training_coin_credit >= TRAINING_COINS_PER_LIFE) {
                        training_coin_credit -= TRAINING_COINS_PER_LIFE;
                        game_lives++;
                        player.lives = game_lives;
                        play_extra_life_sound();
                        start_led_effect(2);  /* after the coin's, so it wins */
                    }
                }
            }
            if (t == TILE_GOAL) {
                if (demo_on()) {   /* the demo reached the flag: done, no overlay */
                    start_menu_demo_over(&menu);
                    return;
                }
                current_screen = SCREEN_LEVEL_COMPLETE;
                level_complete_timer = get_time_ms();
                score += LEVEL_BONUS;
                play_level_complete_sound();
                start_led_effect(4);
                return;
            }
        }
    }

    if (player.y > (float)(camera.level_height_px + TILE_SIZE))
        player_die();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Death
 * ═══════════════════════════════════════════════════════════════════════════ */

static void player_die(void) {
    if (player.state == PSTATE_DYING) return;
    if (player.invincible_timer > 0) return;

    /* The demo's first death ends it: no dying state, no life lost, no game
     * over.  demo_over moves the widget to SCORES at its next update. */
    if (demo_on()) {
        start_menu_demo_over(&menu);
        return;
    }

    player.state = PSTATE_DYING;
    player.death_timer = DEATH_ANIM_FRAMES;
    player.vx = 0;
    player.vy = JUMP_VELOCITY * 0.6f;
    game_lives--;
    player.lives = game_lives;
    play_death_sound();
    start_led_effect(3);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Enemy Update
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_enemies(void) {
    for (int i = 0; i < enemy_count; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;

        e->anim_timer++;
        if (e->anim_timer >= 10) {
            e->anim_timer = 0;
            e->anim_frame = (e->anim_frame + 1) % 2;
        }

        e->x += e->vx;
        e->facing_right = (e->vx > 0);

        /* Wall check */
        int front_tx;
        if (e->vx > 0) front_tx = (int)(e->x + e->width) / TILE_SIZE;
        else front_tx = (int)(e->x) / TILE_SIZE;
        int foot_ty = (int)(e->y + e->height - 1) / TILE_SIZE;

        if (tile_is_solid(front_tx, foot_ty)) {
            e->vx = -e->vx;
            e->facing_right = (e->vx > 0);
        }

        /* Ledge check */
        int check_x;
        if (e->vx > 0) check_x = (int)(e->x + e->width) / TILE_SIZE;
        else check_x = (int)(e->x) / TILE_SIZE;
        int below_ty = foot_ty + 1;

        bool ground_ahead = tile_is_solid(check_x, below_ty) ||
                            tile_is_platform(check_x, below_ty);
        if (!ground_ahead && below_ty < level_height) {
            e->vx = -e->vx;
            e->facing_right = (e->vx > 0);
        }

        if (e->x < 0) { e->x = 0; e->vx = fabsf(e->vx); }
        if (e->x + e->width > camera.level_width_px) {
            e->x = (float)(camera.level_width_px - e->width);
            e->vx = -fabsf(e->vx);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Enemy Collisions
 * ═══════════════════════════════════════════════════════════════════════════ */

static void check_enemy_collisions(void) {
    if (player.state == PSTATE_DYING) return;

    /* Resolve every overlap before acting on any of them.
     *
     * The stomp test reads player.vy, and a stomp *writes* it (STOMP_BOUNCE is
     * negative — upward), so applying the bounce inside the loop made the second
     * of two overlapping enemies fail `vy > 0` and take the fatal branch: landing
     * on a stack killed the player.  A plain
     * `break` after the first stomp is not enough either — it leaves enemy #2
     * alive directly under the player's feet, and the bounce only clears the
     * overlap after ~3 ticks at 6.0 px/tick against a 22 px enemy, so the
     * player still dies on the next tick with vy already negative.
     *
     * So: judge each enemy against the velocity the player arrived with, kill
     * everything that was genuinely landed on, and only die if nothing was.  A
     * stomp beats a side-hit in the same tick. */
    const float arrival_vy = player.vy;
    bool stomped = false;
    bool struck  = false;

    for (int i = 0; i < enemy_count; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;

        if (player.x + player.width <= e->x || player.x >= e->x + e->width) continue;
        if (player.y + player.height <= e->y || player.y >= e->y + e->height) continue;

        float player_feet = player.y + player.height;
        float enemy_mid = e->y + (float)e->height / 2.0f;

        if (arrival_vy > 0 && player_feet < enemy_mid + 4.0f) {
            e->alive = false;
            score += STOMP_SCORE;
            stomped = true;
        } else {
            struck = true;
        }
    }

    if (stomped) {
        /* Once per tick, however many enemies went under the boots — two
         * play_stomp_sound() calls in one tick just cut each other off. */
        player.vy = STOMP_BOUNCE;
        player.on_ground = false;
        play_stomp_sound();
        start_led_effect(2);
    } else if (struck) {
        player_die();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Player Update
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_player(void) {
    if (player.state == PSTATE_DYING) {
        player.death_timer--;
        player.vy += GRAVITY;
        player.y += player.vy;
        if (player.death_timer <= 0) {
            if (game_lives <= 0) {
                current_screen = SCREEN_GAME_OVER;
                play_game_over_sound();
                gameover_init(&gos, &fb, score, NULL, NULL, "OFFICE RUNNER",
                              &hs_table, &touch, &gamepad);
            } else {
                reset_player_to_spawn();
            }
        }
        return;
    }

    if (player.invincible_timer > 0) player.invincible_timer--;
    if (player.coyote_frames > 0) player.coyote_frames--;
    if (player.jump_buffer > 0) player.jump_buffer--;

    /* Horizontal input */
    float target_speed = 0;
    bool moving = false;

    /* The demo's AI holds RIGHT and RUN; real input is ignored while it runs. */
    bool in_left  = !demo_on() && input.buttons[BTN_ID_LEFT].held;
    bool in_right = demo_on() || input.buttons[BTN_ID_RIGHT].held;
    bool in_run   = demo_on() || input.buttons[BTN_ID_RUN].held;

    if (in_left) {
        bool run = in_run;
        target_speed = -(run ? RUN_SPEED : WALK_SPEED);
        player.facing_right = false;
        moving = true;
    } else if (in_right) {
        bool run = in_run;
        target_speed = (run ? RUN_SPEED : WALK_SPEED);
        player.facing_right = true;
        moving = true;
    }

    if (moving) {
        if (fabsf(player.vx) < fabsf(target_speed)) {
            player.vx += (target_speed > 0 ? ACCELERATION : -ACCELERATION);
            if (fabsf(player.vx) > fabsf(target_speed))
                player.vx = target_speed;
        } else {
            if (player.vx > target_speed + 0.1f) player.vx -= DECELERATION;
            else if (player.vx < target_speed - 0.1f) player.vx += DECELERATION;
            else player.vx = target_speed;
        }
    } else {
        if (player.vx > DECELERATION) player.vx -= DECELERATION;
        else if (player.vx < -DECELERATION) player.vx += DECELERATION;
        else player.vx = 0;
    }

    /* Jump */
    if (jump_press_pending)
        player.jump_buffer = JUMP_BUFFER_TIME;

    bool can_jump = player.on_ground || player.coyote_frames > 0;
    if (player.jump_buffer > 0 && can_jump) {
        player.vy = JUMP_VELOCITY;
        player.on_ground = false;
        player.coyote_frames = 0;
        player.jump_buffer = 0;
        player.jump_held = true;
        play_jump_sound();
    }

    if (player.jump_held && jump_release_pending) {
        player.jump_held = false;
        if (player.vy < 0) player.vy *= JUMP_CUT;
    }
    if (player.on_ground) player.jump_held = false;

    /* Gravity */
    player.vy += GRAVITY;
    if (player.vy > MAX_FALL_SPEED) player.vy = MAX_FALL_SPEED;

    /* Move X + resolve */
    player.x += player.vx;
    if (player.x < 0) { player.x = 0; player.vx = 0; }
    if (player.x + player.width > camera.level_width_px) {
        player.x = (float)(camera.level_width_px - player.width);
        player.vx = 0;
    }
    resolve_collisions_x();

    /* Move Y + resolve */
    player.y += player.vy;
    resolve_collisions_y();

    check_tile_interactions();
    if (player.state == PSTATE_DYING) return;
    check_enemy_collisions();
    if (player.state == PSTATE_DYING) return;

    /* Animation: a four-pose walk cycle — stride, legs together, other stride,
     * legs together (see draw_player).  Each pose holds a few ticks, quicker
     * when running.  The together pose must be a pose of its own: picked by
     * tick parity it aliases against the frame rate and can freeze the legs. */
    player.anim_timer++;
    if (player.anim_timer >= (fabsf(player.vx) > WALK_SPEED ? 2 : 3)) {
        player.anim_timer = 0;
        player.anim_frame = (player.anim_frame + 1) % 4;
    }

    if (!player.on_ground)
        player.state = (player.vy < 0) ? PSTATE_JUMPING : PSTATE_FALLING;
    else if (fabsf(player.vx) > 0.5f)
        player.state = PSTATE_RUNNING;
    else
        player.state = PSTATE_IDLE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Camera Update
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_camera(void) {
    int sw = (int)fb.width;
    int sh = (int)fb.height;

    float dead_zone_x = (float)sw * 0.35f;
    float psx = player.x - camera.x;

    if (psx < dead_zone_x)
        camera.target_x = player.x - dead_zone_x;
    else if (psx > (float)sw - dead_zone_x)
        camera.target_x = player.x - ((float)sw - dead_zone_x);

    float dead_zone_y = (float)sh * 0.30f;
    float psy = player.y - camera.y;

    if (psy < dead_zone_y)
        camera.target_y = player.y - dead_zone_y;
    else if (psy > (float)sh - dead_zone_y)
        camera.target_y = player.y - ((float)sh - dead_zone_y);

    /* Smooth interpolation */
    camera.x += (camera.target_x - camera.x) * 0.12f;
    camera.y += (camera.target_y - camera.y) * 0.12f;

    /* Clamp */
    float max_cx = (float)(camera.level_width_px - sw);
    float max_cy = (float)(camera.level_height_px - sh);
    if (max_cx < 0) max_cx = 0;
    if (max_cy < 0) max_cy = 0;
    if (camera.x < 0) camera.x = 0;
    if (camera.y < 0) camera.y = 0;
    if (camera.x > max_cx) camera.x = max_cx;
    if (camera.y > max_cy) camera.y = max_cy;
}

/* Screen-space drop applied to the whole world (tiles, player, enemies) so
 * the floor sits on the bottom of the visible area.  Every level is
 * 30 tiles = 480 px tall: in landscape that fills the screen and this is 0,
 * so the layout is unchanged; in portrait (480x800) the camera clamps to
 * y = 0 and the level used to end at row 480, leaving the runner on a floor
 * in mid-air.  The rows above the level show the sky gradient.  Only applies
 * when the level is shorter than the screen — a taller one scrolls instead. */
static int world_offset_y(void) {
    int off = (int)SCREEN_VISIBLE_BOTTOM - camera.level_height_px;
    return off > 0 ? off : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Game Update
 * ═══════════════════════════════════════════════════════════════════════════ */

/* The demo's brain, once per tick before update_player(): the runner holds
 * RIGHT + RUN (update_player) and this decides the jump.  A running jump
 * (JUMP_VELOCITY -8.5, GRAVITY 0.55) stays up ~31 ticks, about 155 px at
 * RUN_SPEED and 65 px high, so a wall of up to four tiles, a gap of up to nine
 * and an enemy are all cleared by jumping within ~50 px of them.  Jumps only
 * from the ground; the button is never released early (full height). */
static void demo_ai_think(void) {
    if (player.state == PSTATE_DYING) return;

    /* Stuck against something it cannot clear: give up and let the cycle go on */
    if (player.x > demo_last_x + 0.25f) {
        demo_last_x = player.x;
        demo_stuck = 0;
    } else if (++demo_stuck >= DEMO_STUCK_TICKS) {
        start_menu_demo_over(&menu);
        return;
    }
    if (!player.on_ground) return;

    int fx = (int)(player.x + (float)player.width);              /* front edge, px */
    int fy = (int)(player.y + (float)player.height - 1.0f) / TILE_SIZE;   /* feet row */
    bool jump = false;

    /* Wall or hazard ahead (at body or foot level), or a hazard on the floor */
    for (int d = 4; d <= 52 && !jump; d += 16) {
        int tx = (fx + d) / TILE_SIZE;
        if (tile_is_solid(tx, fy) || tile_is_solid(tx, fy - 1) ||
            tile_at(tx, fy) == TILE_HAZARD || tile_at(tx, fy + 1) == TILE_HAZARD)
            jump = true;
    }

    /* Gap: no floor just ahead of the front foot */
    if (!jump) {
        int tx = (fx + 10) / TILE_SIZE;
        if (fy + 1 < level_height && !tile_is_solid(tx, fy + 1) && !tile_is_platform(tx, fy + 1))
            jump = true;
    }

    /* A live enemy on our level within 60 px ahead */
    for (int i = 0; i < enemy_count && !jump; i++) {
        const Enemy *e = &enemies[i];
        if (!e->alive) continue;
        if (e->y + (float)e->height <= player.y || e->y >= player.y + (float)player.height) continue;
        float ahead = e->x - (float)fx;
        if (ahead > -4.0f && ahead < 60.0f) jump = true;
    }

    if (jump) jump_press_pending = true;
}

/* One fixed TICK_S step of everything that advances the game. */
static void game_tick(void) {
    current_frame++;
    if (demo_on()) demo_ai_think();
    update_player();
    update_enemies();
    update_camera();
}

/* Fixed-step accumulator: elapsed time (clamped to MAX_FRAME_DT) is banked in
 * tick_acc and spent in whole ticks, at most MAX_TICKS_FRAME per frame.  Hitting
 * the cap drops the remainder rather than carrying a debt into later frames.
 * Drawing stays once per frame, showing the state after the last tick. */
static void update_game(void) {
    /* The demo runs on SCREEN_MENU, on the same clock and tick loop as play */
    if (current_screen != SCREEN_PLAYING && !demo_on()) {
        play_clock_live = false;
        return;
    }

    uint32_t now = get_time_ms();
    if (!play_clock_live) {
        /* Entering play: start the clock here, run no tick this frame, and drop
         * this frame's edges — they belong to whatever started or resumed play
         * (the start menu's JUMP, the pause toggle). */
        play_clock_live      = true;
        play_last_ms         = now;
        tick_acc             = 0.0f;
        jump_press_pending   = false;
        jump_release_pending = false;
        update_led_effects();
        return;
    }

    float dt = (float)(now - play_last_ms) * 0.001f;
    if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;
    play_last_ms = now;
    tick_acc += dt;

    if (!demo_on()) {   /* the demo's AI owns the jump */
        if (input.buttons[BTN_ID_JUMP].pressed)  jump_press_pending   = true;
        if (input.buttons[BTN_ID_JUMP].released) jump_release_pending = true;
    }

    int steps = 0;
    while (tick_acc >= TICK_S && steps < MAX_TICKS_FRAME) {
        game_tick();
        tick_acc -= TICK_S;
        steps++;
        /* Consumed by this frame's first tick and no other. */
        jump_press_pending   = false;
        jump_release_pending = false;
        /* Goal reached or game over: the rest of the frame's time is not play. */
        if (current_screen != SCREEN_PLAYING && !demo_on()) break;
    }
    if (steps >= MAX_TICKS_FRAME) tick_acc = 0.0f;

    update_led_effects();   /* runs on get_time_ms() itself, once per frame */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sprite Drawing — Tiles
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_single_tile(int tx, int ty, int sx, int sy) {
    int t = tile_at(tx, ty);

    /* Check if coin collected */
    if (t == TILE_COIN && coins_collected[ty][tx])
        return;

    switch (t) {
    case TILE_SOLID:
        /* Ground block: brown body with green top strip */
        fb_fill_rect(&fb, sx, sy, TILE_SIZE, TILE_SIZE, COLOR_GROUND);
        fb_fill_rect(&fb, sx, sy, TILE_SIZE, 3, COLOR_GROUND_TOP);
        /* Brick pattern */
        fb_draw_line(&fb, sx, sy + TILE_SIZE / 2, sx + TILE_SIZE, sy + TILE_SIZE / 2,
                     COLOR_GROUND_LINE);
        fb_draw_line(&fb, sx + TILE_SIZE / 2, sy + 3, sx + TILE_SIZE / 2, sy + TILE_SIZE / 2,
                     COLOR_GROUND_LINE);
        fb_draw_line(&fb, sx + TILE_SIZE / 4, sy + TILE_SIZE / 2,
                     sx + TILE_SIZE / 4, sy + TILE_SIZE, COLOR_GROUND_LINE);
        fb_draw_line(&fb, sx + TILE_SIZE * 3 / 4, sy + TILE_SIZE / 2,
                     sx + TILE_SIZE * 3 / 4, sy + TILE_SIZE, COLOR_GROUND_LINE);
        break;

    case TILE_PLATFORM:
        /* Thinner wooden platform */
        fb_fill_rect(&fb, sx, sy, TILE_SIZE, TILE_SIZE / 2 + 2, COLOR_PLATFORM_BG);
        fb_fill_rect(&fb, sx, sy, TILE_SIZE, 3, COLOR_PLATFORM_TOP);
        fb_draw_rect(&fb, sx, sy, TILE_SIZE, TILE_SIZE / 2 + 2, COLOR_PLATFORM_LINE);
        /* Wood grain */
        fb_draw_line(&fb, sx + 2, sy + 4, sx + TILE_SIZE - 2, sy + 4, COLOR_PLATFORM_LINE);
        break;

    case TILE_HAZARD:
        /* Red with yellow warning stripes */
        fb_fill_rect(&fb, sx, sy, TILE_SIZE, TILE_SIZE, COLOR_HAZARD_BG);
        for (int i = 0; i < TILE_SIZE; i += 6) {
            fb_fill_rect(&fb, sx + i, sy, 3, 3, COLOR_HAZARD_STRIPE);
            fb_fill_rect(&fb, sx + i + 3, sy + TILE_SIZE - 3, 3, 3, COLOR_HAZARD_STRIPE);
        }
        /* Spikes */
        for (int i = 2; i < TILE_SIZE - 2; i += 5) {
            fb_draw_line(&fb, sx + i, sy + 6, sx + i + 2, sy + 2, COLOR_HAZARD_STRIPE);
            fb_draw_line(&fb, sx + i + 2, sy + 2, sx + i + 4, sy + 6, COLOR_HAZARD_STRIPE);
        }
        break;

    case TILE_COIN:
        draw_coin(sx, sy);
        break;

    case TILE_GOAL:
        draw_goal(sx, sy);
        break;

    default:
        break;
    }
}

static void draw_coin(int sx, int sy) {
    int cx = sx + TILE_SIZE / 2;
    int cy = sy + TILE_SIZE / 2;
    int r = TILE_SIZE / 3;

    /* Animate: pulse size */
    int anim = (int)(current_frame / 8) % 4;
    if (anim == 1 || anim == 3) r -= 1;

    fb_fill_circle(&fb, cx, cy, r, COLOR_COIN_OUTER);
    fb_fill_circle(&fb, cx, cy, r - 2, COLOR_COIN_INNER);

    /* Dollar sign or shine */
    if (anim < 2) {
        fb_draw_line(&fb, cx, cy - r + 2, cx, cy + r - 2, COLOR_COIN_OUTER);
    }
    /* Sparkle */
    fb_draw_pixel(&fb, cx - 1, cy - r + 1, COLOR_COIN_SHINE);
}

static void draw_goal(int sx, int sy) {
    /* Flag pole */
    int pole_x = sx + TILE_SIZE / 2;
    fb_draw_line(&fb, pole_x, sy, pole_x, sy + TILE_SIZE, COLOR_FLAG_POLE);
    fb_draw_line(&fb, pole_x + 1, sy, pole_x + 1, sy + TILE_SIZE, COLOR_FLAG_POLE);

    /* Flag cloth (triangular) */
    int flag_top = sy + 2;
    int flag_h = TILE_SIZE / 2;
    for (int row = 0; row < flag_h; row++) {
        int w = flag_h - row;
        fb_fill_rect(&fb, pole_x + 2, flag_top + row, w, 1, COLOR_FLAG_CLOTH);
    }

    /* Pole ball top */
    fb_fill_circle(&fb, pole_x, sy, 2, COLOR_COIN_OUTER);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sprite Drawing — Player (Office Worker, 14x28)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_player_idle(int sx, int sy, bool right) {
    (void)right;
    int w = player.width;

    /* Head (circle) */
    fb_fill_circle(&fb, sx + w / 2, sy + 4, 5, COLOR_SKIN);
    /* Hair */
    fb_fill_rect(&fb, sx + w / 2 - 4, sy, 8, 3, COLOR_HAIR);

    /* Eyes */
    int eye_off = right ? 2 : -2;
    fb_draw_pixel(&fb, sx + w / 2 + eye_off - 1, sy + 3, COLOR_BLACK);
    fb_draw_pixel(&fb, sx + w / 2 + eye_off + 1, sy + 3, COLOR_BLACK);

    /* Body/shirt */
    fb_fill_rect(&fb, sx + 2, sy + 10, w - 4, 10, COLOR_SHIRT);
    /* Tie */
    fb_fill_rect(&fb, sx + w / 2 - 1, sy + 10, 2, 7, COLOR_TIE);

    /* Arms */
    fb_fill_rect(&fb, sx, sy + 11, 2, 7, COLOR_SHIRT_DARK);
    fb_fill_rect(&fb, sx + w - 2, sy + 11, 2, 7, COLOR_SHIRT_DARK);

    /* Pants */
    fb_fill_rect(&fb, sx + 3, sy + 20, 4, 5, COLOR_PANTS);
    fb_fill_rect(&fb, sx + w - 7, sy + 20, 4, 5, COLOR_PANTS);

    /* Shoes */
    fb_fill_rect(&fb, sx + 2, sy + 25, 5, 3, COLOR_SHOES);
    fb_fill_rect(&fb, sx + w - 7, sy + 25, 5, 3, COLOR_SHOES);
}

static void draw_player_running(int sx, int sy, bool right, int frame) {
    int w = player.width;

    /* Head */
    fb_fill_circle(&fb, sx + w / 2, sy + 4, 5, COLOR_SKIN);
    fb_fill_rect(&fb, sx + w / 2 - 4, sy, 8, 3, COLOR_HAIR);

    int eye_off = right ? 2 : -2;
    fb_draw_pixel(&fb, sx + w / 2 + eye_off - 1, sy + 3, COLOR_BLACK);
    fb_draw_pixel(&fb, sx + w / 2 + eye_off + 1, sy + 3, COLOR_BLACK);

    /* Body */
    fb_fill_rect(&fb, sx + 2, sy + 10, w - 4, 10, COLOR_SHIRT);
    fb_fill_rect(&fb, sx + w / 2 - 1, sy + 10, 2, 7, COLOR_TIE);

    /* Animated arms */
    if (frame == 0) {
        fb_fill_rect(&fb, sx - 1, sy + 11, 3, 6, COLOR_SHIRT_DARK);
        fb_fill_rect(&fb, sx + w - 2, sy + 13, 3, 6, COLOR_SHIRT_DARK);
    } else {
        fb_fill_rect(&fb, sx - 1, sy + 13, 3, 6, COLOR_SHIRT_DARK);
        fb_fill_rect(&fb, sx + w - 2, sy + 11, 3, 6, COLOR_SHIRT_DARK);
    }

    /* Animated legs */
    if (frame == 0) {
        fb_fill_rect(&fb, sx + 2, sy + 20, 4, 4, COLOR_PANTS);
        fb_fill_rect(&fb, sx + w - 5, sy + 21, 4, 3, COLOR_PANTS);
        fb_fill_rect(&fb, sx + 1, sy + 24, 5, 3, COLOR_SHOES);
        fb_fill_rect(&fb, sx + w - 6, sy + 24, 5, 3, COLOR_SHOES);
    } else {
        fb_fill_rect(&fb, sx + 2, sy + 21, 4, 3, COLOR_PANTS);
        fb_fill_rect(&fb, sx + w - 5, sy + 20, 4, 4, COLOR_PANTS);
        fb_fill_rect(&fb, sx + 1, sy + 24, 5, 3, COLOR_SHOES);
        fb_fill_rect(&fb, sx + w - 6, sy + 24, 5, 3, COLOR_SHOES);
    }
}

static void draw_player_jumping(int sx, int sy, bool right) {
    int w = player.width;

    /* Head */
    fb_fill_circle(&fb, sx + w / 2, sy + 4, 5, COLOR_SKIN);
    fb_fill_rect(&fb, sx + w / 2 - 4, sy, 8, 3, COLOR_HAIR);
    int eye_off = right ? 2 : -2;
    fb_draw_pixel(&fb, sx + w / 2 + eye_off - 1, sy + 3, COLOR_BLACK);
    fb_draw_pixel(&fb, sx + w / 2 + eye_off + 1, sy + 3, COLOR_BLACK);

    /* Body */
    fb_fill_rect(&fb, sx + 2, sy + 10, w - 4, 10, COLOR_SHIRT);
    fb_fill_rect(&fb, sx + w / 2 - 1, sy + 10, 2, 7, COLOR_TIE);

    /* Arms up */
    fb_fill_rect(&fb, sx, sy + 8, 2, 5, COLOR_SHIRT_DARK);
    fb_fill_rect(&fb, sx + w - 2, sy + 8, 2, 5, COLOR_SHIRT_DARK);

    /* Legs tucked */
    fb_fill_rect(&fb, sx + 2, sy + 20, 4, 4, COLOR_PANTS);
    fb_fill_rect(&fb, sx + w - 6, sy + 20, 4, 4, COLOR_PANTS);
    fb_fill_rect(&fb, sx + 1, sy + 24, 5, 3, COLOR_SHOES);
    fb_fill_rect(&fb, sx + w - 6, sy + 24, 5, 3, COLOR_SHOES);
}

static void draw_player_dying(int sx, int sy, int timer) {
    int w = player.width;
    int r = 3 + (DEATH_ANIM_FRAMES - timer) * 2;
    int cx = sx + w / 2;
    int cy = sy + player.height / 2;

    fb_fill_circle(&fb, cx, cy, r, COLOR_RED);
    if (r > 5) {
        fb_draw_line(&fb, cx - r/2, cy - r/2, cx + r/2, cy + r/2, COLOR_WHITE);
        fb_draw_line(&fb, cx + r/2, cy - r/2, cx - r/2, cy + r/2, COLOR_WHITE);
    }
}

static void draw_player_sprite(void) {
    int sx = (int)player.x - (int)camera.x;
    int sy = (int)player.y - (int)camera.y + world_offset_y();

    /* Off-screen cull */
    if (sx + player.width < -16 || sx > (int)fb.width + 16) return;
    if (sy + player.height < -16 || sy > (int)fb.height + 16) return;

    /* Dying */
    if (player.state == PSTATE_DYING) {
        draw_player_dying(sx, sy, player.death_timer);
        return;
    }

    /* Invincibility blink */
    if (player.invincible_timer > 0 && (current_frame / 3) % 2)
        return;

    switch (player.state) {
    case PSTATE_IDLE:
    case PSTATE_FALLING:
        draw_player_idle(sx, sy, player.facing_right);
        break;
    case PSTATE_RUNNING:
        if (player.anim_frame & 1)
            draw_player_idle(sx, sy, player.facing_right);
        else
            draw_player_running(sx, sy, player.facing_right, player.anim_frame >> 1);
        break;
    case PSTATE_JUMPING:
        draw_player_jumping(sx, sy, player.facing_right);
        break;
    default:
        draw_player_idle(sx, sy, player.facing_right);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sprite Drawing — Enemies
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_enemy_walker(Enemy *e, int sx, int sy) {
    int w = e->width, h = e->height;

    /* Body — printer/box shape */
    fb_fill_rect(&fb, sx + 1, sy + 2, w - 2, h - 4, RGB(180,180,180));
    fb_fill_rect(&fb, sx + 1, sy + 2, w - 2, 3, RGB(120,120,120));

    /* Paper sticking out top */
    fb_fill_rect(&fb, sx + 3, sy, w - 6, 4, RGB(240,240,230));
    if (e->anim_frame)
        fb_fill_rect(&fb, sx + 4, sy - 2, w - 8, 3, RGB(240,240,230));

    /* Eyes */
    int ex1 = sx + w / 3, ex2 = sx + w * 2 / 3;
    fb_fill_circle(&fb, ex1, sy + h / 2, 2, RGB(255,60,60));
    fb_fill_circle(&fb, ex2, sy + h / 2, 2, RGB(255,60,60));

    /* Mouth */
    fb_draw_line(&fb, sx + w / 3, sy + h * 2 / 3,
                 sx + w * 2 / 3, sy + h * 2 / 3, RGB(40,40,40));

    /* Feet */
    int fo = e->anim_frame ? 1 : 0;
    fb_fill_rect(&fb, sx + 2, sy + h - 2 - fo, 3, 2, RGB(120,120,120));
    fb_fill_rect(&fb, sx + w - 5, sy + h - 2 + fo - 1, 3, 2, RGB(120,120,120));
}

static void draw_enemies_all(void) {
    for (int i = 0; i < enemy_count; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;

        int sx = (int)e->x - (int)camera.x;
        int sy = (int)e->y - (int)camera.y + world_offset_y();

        if (sx + e->width < -16 || sx > (int)fb.width + 16) continue;
        if (sy + e->height < -16 || sy > (int)fb.height + 16) continue;

        draw_enemy_walker(e, sx, sy);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tile Rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_tiles(void) {
    int sx_start = (int)camera.x / TILE_SIZE;
    int sy_start = (int)camera.y / TILE_SIZE;
    int sx_end = sx_start + (int)fb.width / TILE_SIZE + 2;
    int sy_end = sy_start + (int)fb.height / TILE_SIZE + 2;
    int y_off = world_offset_y();

    if (sx_start < 0) sx_start = 0;
    if (sy_start < 0) sy_start = 0;
    if (sx_end > level_width) sx_end = level_width;
    if (sy_end > level_height) sy_end = level_height;

    for (int ty = sy_start; ty < sy_end; ty++) {
        for (int tx = sx_start; tx < sx_end; tx++) {
            if (level_tiles[ty][tx] != TILE_EMPTY) {
                int scrx = tx * TILE_SIZE - (int)camera.x;
                int scry = ty * TILE_SIZE - (int)camera.y + y_off;
                draw_single_tile(tx, ty, scrx, scry);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HUD Drawing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_hud(void) {
    fb_fill_rect(&fb, 0, 0, (int)fb.width, HUD_HEIGHT, RGB(20,20,30));
    draw_menu_button(&fb, &menu_button);
    draw_exit_button(&fb, &exit_button);

    char buf[48];
    snprintf(buf, sizeof(buf), "SCORE: %d", score);
    fb_draw_text(&fb, 90, 12, buf, COLOR_WHITE, 2);

    snprintf(buf, sizeof(buf), "LEVEL %d", current_level + 1);
    int tw = text_measure_width(buf, 2);
    fb_draw_text(&fb, (int)fb.width / 2 - tw / 2, 12, buf, COLOR_CYAN, 2);

    /* Lives — up to MAX_LIFE_ICONS icons, then one icon and a count.  The row
     * is positioned from what is actually drawn, never from game_lives: the old
     * "- game_lives * 16" start clamped to mid-screen at training mode's 10
     * lives, leaving 5 icons floating in a gap next to the EXIT button. */
    int icons = game_lives > 0 ? game_lives : 0;
    char more[16] = "";
    if (icons > MAX_LIFE_ICONS) {
        snprintf(more, sizeof(more), "x%d", icons);
        icons = 1;
    }
    int more_w = more[0] ? text_measure_width(more, 2) + 4 : 0;
    int lx = (int)fb.width - BTN_EXIT_WIDTH - 20 - (icons * 16 + more_w);
    if (lx < (int)fb.width / 2 + 60) lx = (int)fb.width / 2 + 60;
    for (int i = 0; i < icons; i++) {
        int ix = lx + i * 16;
        fb_fill_circle(&fb, ix + 6, 28, 5, COLOR_SHIRT);
        fb_fill_circle(&fb, ix + 6, 23, 3, COLOR_SKIN);
    }
    if (more[0])
        fb_draw_text(&fb, lx + icons * 16 + 4, 20, more, COLOR_WHITE, 2);

    /* Training badge, under the score: says the mode is on and how many more
     * coins buy the next life.  Scale 1 at y=28 clears the score (12..26) and
     * stays inside HUD_HEIGHT. */
    if (training_mode) {
        snprintf(buf, sizeof(buf), "TRAINING %d/%d",
                 training_coin_credit, TRAINING_COINS_PER_LIFE);
        fb_draw_text(&fb, 90, 28, buf, COLOR_YELLOW, 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main Draw Function
 * ═══════════════════════════════════════════════════════════════════════════ */

/* The demo: SCREEN_MENU while the attract cycle is on its DEMO page.  Entering
 * it starts a fresh NORMAL run (training off, level 1) and re-baselines the play
 * clock; leaving it drops the clock again.  START resets the game once more at
 * the selected difficulty, so nothing of the demo reaches real play. */
static void demo_sync(void) {
    bool want = (current_screen == SCREEN_MENU &&
                 start_menu_attract(&menu) == SM_ATTRACT_DEMO);
    if (want != demo_running) {
        play_clock_live = false;
        jump_press_pending = false;
        jump_release_pending = false;
        if (want) {
            demo_running = true;      /* before reset_game(): nothing of it may sound or light */
            training_mode = false;
            reset_game();
            demo_last_x = player.x;
            demo_stuck = 0;
        } else {
            demo_running = false;
            hw_leds_off();
            led_effect.active = false;
        }
    }
}

/* Back to the start menu from anywhere in play: drop the play-time state the
 * START path set up (LED effect, dialog, level, score, lives) so the menu looks
 * as on first launch.  Changing current_screen releases the music bed on the
 * next bed_service(); the play clock re-baselines on its own. */
static void return_to_menu(void) {
    demo_running = false;
    hw_leds_off();
    modal_dialog_hide(&pause_dialog);
    reset_game();
    start_menu_reopen(&menu, get_time_ms());
    current_screen = SCREEN_MENU;
}

static void draw_all(void) {
    /* Start menu, or the attract cycle's SCORES page (widget clears too) */
    if (current_screen == SCREEN_MENU) {
        /* This game has no touch controls, so say so up front rather than
         * shipping a virtual D-pad: the one this game had latched its zones on
         * and drew its boxes away from the regions that received the taps. */
        bool no_controller = !input.gamepad_connected && !input.keyboard_connected;
        start_menu_set_warning(&menu, no_controller
            ? "NO CONTROLLER DETECTED\nCONNECT A KEYBOARD OR GAMEPAD" : NULL);
        SmAttract ph = start_menu_attract(&menu);
        if (ph == SM_ATTRACT_MENU) {
            start_menu_draw(&menu, &fb);
            return;
        }
        if (ph == SM_ATTRACT_SCORES) {
            start_menu_draw_scores(&menu, &fb, &hs_table);
            return;
        }
        /* DEMO: the level below, drawn with the real draw code */
    }

    /* Sky background */
    fb_fill_rect_gradient(&fb, 0, 0, (int)fb.width, (int)fb.height,
                          RGB(135,206,235), RGB(100,180,220));

    /* Game world */
    draw_tiles();
    draw_enemies_all();
    draw_player_sprite();
    if (demo_on()) {   /* no HUD, no buttons: a label instead */
        text_draw_centered(&fb, (int)fb.width / 2, SCREEN_VISIBLE_TOP + 20, "DEMO", COLOR_CYAN, 2);
        return;
    }
    draw_hud();

    /* No touch-controls overlay — see init_game(). */

    /* Pause overlay */
    if (current_screen == SCREEN_PAUSED) {
        modal_dialog_draw(&pause_dialog, &fb);
        return;
    }

    /* Level complete overlay */
    if (current_screen == SCREEN_LEVEL_COMPLETE) {
        fb_fill_rect_alpha(&fb, 0, 0, (int)fb.width, (int)fb.height,
                           COLOR_BLACK, 140);
        char buf[64];
        snprintf(buf, sizeof(buf), "LEVEL %d COMPLETE!", current_level + 1);
        text_draw_centered(&fb, (int)fb.width / 2, (int)fb.height / 2 - 40,
                          buf, COLOR_WHITE, 3);
        snprintf(buf, sizeof(buf), "SCORE: %d   COINS: %d/%d",
                 score, collected_coins, total_coins);
        text_draw_centered(&fb, (int)fb.width / 2, (int)fb.height / 2 + 10,
                          buf, COLOR_YELLOW, 2);
        if (current_level < MAX_LEVELS - 1)
            text_draw_centered(&fb, (int)fb.width / 2, (int)fb.height / 2 + 50,
                              "GET READY...", COLOR_WHITE, 2);
        else
            text_draw_centered(&fb, (int)fb.width / 2, (int)fb.height / 2 + 50,
                              "YOU WIN! CONGRATULATIONS!", COLOR_GREEN, 2);
        return;
    }

    /* Game over */
    if (current_screen == SCREEN_GAME_OVER) {
        /* No touch_poll() here. handle_input() already polled this frame, and
         * touch_poll() clears TouchState.pressed at entry — a second poll ate the
         * press edge, so RESTART and EXIT could never fire. */
        TouchState ts = touch_get_state(&touch);
        /* `input` is the file-scope InputState handle_input() filled this frame. */
        GameOverAction act = gameover_update(&gos, &fb,
                                             ts.x, ts.y, ts.pressed, &input);
        if (act == GAMEOVER_ACTION_MENU)
            return_to_menu();
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Input Handling
 * ═══════════════════════════════════════════════════════════════════════════ */

static void handle_input(void) {
    touch_poll(&touch);
    TouchState ts = touch_get_state(&touch);
    gamepad_poll(&gamepad, &input, ts.x, ts.y, ts.pressed);
    uint32_t now = get_time_ms();

    /* Hot-plug check.  This game is controller-only, and it was once the one
     * app with no rescan at all: a pad that left the bus made read() fail
     * forever on a stale fd and nothing reopened it, so input stayed dead for
     * the life of the process — seen on the panel 2026-08-10, and why
     * relaunching the game fixed it.  gamepad_tick() rescans when /dev/input
     * changes or a read reports the device gone, and the rescan also clears
     * held_latched[], so a direction held at unplug time does not stay
     * asserted. */
    gamepad_tick(&gamepad, now);

    /* Start menu — ahead of the BACK rule below, so that BACK during the
     * SCORES page is swallowed like any other input there (the widget's
     * rule), and on the menu itself arrives as SM_EXIT.  The widget keeps
     * its own edges, so a button or finger still down from the screen
     * before acts only after a fresh press. */
    if (current_screen == SCREEN_MENU) {
        int r = start_menu_update(&menu, &input, ts.x, ts.y,
                                  ts.pressed || ts.held,
                                  &gamepad, &audio, now);
        if (r == SM_EXIT || r == menu_exit_idx) {
            fb_fade_out(&fb);
            running = false;
        } else if (r == menu_start_idx) {
            difficulty = start_menu_value(&menu, menu_diff_idx);
            if (difficulty < 0 || difficulty >= DIFF_COUNT) difficulty = DIFF_NORMAL;
            training_mode = (difficulty == DIFF_EASY);
            demo_running = false;
            play_clock_live = false;
            jump_press_pending = false;
            jump_release_pending = false;
            led_effect.active = false;
            reset_game();
            current_screen = SCREEN_PLAYING;
        } else {
            demo_sync();
        }
        return;
    }

    /* BTN_BACK returns to the start menu, whose own EXIT leaves the process —
     * not on GAME OVER, whose flow owns its input (BACK must not lose a name). */
    if (input.buttons[BTN_ID_BACK].pressed && current_screen != SCREEN_GAME_OVER) {
        return_to_menu();
        return;
    }

    switch (current_screen) {
    case SCREEN_MENU:
        break;   /* handled above; listed so -Wswitch stays quiet */

    case SCREEN_PLAYING:
        if (input.buttons[BTN_ID_PAUSE].pressed) {
            current_screen = SCREEN_PAUSED;
            modal_dialog_set_focus(&pause_dialog, 0);
            modal_dialog_show(&pause_dialog);
            return;
        }
        /* Both buttons are asked EVERY frame, quiet ones included — that is
         * what clears button_check_press()'s latch.  Guarding these on
         * ts.pressed made the call with `false` unreachable, so the hamburger
         * menu fired once per process and then died; EXIT hid it by only ever
         * needing to fire once.  Reported from the panel 2026-08-10, mechanism
         * in tests/button_latch_test.c. */
        if (button_check_tap(&exit_button, &ts, now)) {
            return_to_menu();
            return;
        }
        if (button_check_tap(&menu_button, &ts, now)) {
            current_screen = SCREEN_PAUSED;
            modal_dialog_set_focus(&pause_dialog, 0);
            modal_dialog_show(&pause_dialog);
            return;
        }
        break;

    case SCREEN_PAUSED:
        {
            /* Pad / keyboard: focus frame, A presses it, Start / Esc = RESUME */
            ModalDialogAction act = modal_dialog_input(&pause_dialog, &input, 0);
            if (act == MODAL_ACTION_NONE)
                act = modal_dialog_update(&pause_dialog,
                    ts.x, ts.y, ts.pressed, now);
            if (act == MODAL_ACTION_BTN0) {
                current_screen = SCREEN_PLAYING;
                break;
            }
            if (act == MODAL_ACTION_BTN1) {
                return_to_menu();
                break;
            }
        }
        break;

    case SCREEN_LEVEL_COMPLETE:
    {
        uint32_t elapsed = now - level_complete_timer;
        if (elapsed > 2500) {
            if (current_level < MAX_LEVELS - 1) {
                current_level++;
                load_level(current_level);
                current_screen = SCREEN_PLAYING;
            } else {
                current_screen = SCREEN_GAME_OVER;
                gameover_init(&gos, &fb, score, "YOU WIN!", NULL, "OFFICE RUNNER",
                              &hs_table, &touch, &gamepad);
            }
        }
    }
    break;

    case SCREEN_GAME_OVER:
        /* Handled in draw_all via gameover_update */
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main Entry Point
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* Line-buffer stdout: at boot this is a log FILE, not a tty, so audio_bed_init()'s
     * playlist line and every other diagnostic would sit in a 4 KB glibc buffer
     * instead of in /var/log/arca/app_stdout.log.  Same reason as
     * app_launcher.c's, measured the same day. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *fb_device    = "/dev/fb0";
    const char *touch_device = "/dev/input/touchscreen0";

    if (argc > 1) fb_device    = argv[1];
    if (argc > 2) touch_device = argv[2];

    /* Singleton guard */
    int lock_fd = acquire_instance_lock("platformer");
    (void)lock_fd;
    if (lock_fd < 0) {
        fprintf(stderr, "platformer: another instance is already running\n");
        return 1;
    }

    /* Signal handlers */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Hardware init */
    hw_init();
    hw_set_backlight(100);
    hw_leds_off();
    audio_init(&audio);
    audio_bed_init(&bed, &audio, "platformer");

    /* Framebuffer init */
    /* Pin 32bpp — /dev/fb0 keeps whatever ran last (see fb_set_bpp). */
    fb_set_bpp(fb_device, 32);

    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, "Failed to initialize framebuffer\n");
        return 1;
    }

    /* Touch init */
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, "Failed to initialize touch input\n");
        fb_close(&fb);
        return 1;
    }
    touch_set_screen_size(&touch, fb.width, fb.height);

    /* Gamepad init */
    gamepad_init(&gamepad);

    /* Seed RNG */
    srand(time(NULL));

    /* Initialize game */
    init_game();

    printf("Office Runner started! Press Ctrl+C to exit.\n");

    /* Main loop */
    bool needs_redraw = true;
    while (running) {
        GameScreen prev_screen = current_screen;
        handle_input();
        update_game();

        /* Dirty-flag: active gameplay always redraws; static screens only on changes */
        if (current_screen == SCREEN_PLAYING ||
            current_screen == SCREEN_LEVEL_COMPLETE || demo_on()) {
            needs_redraw = true;  /* scrolling, enemies, physics, timed overlay */
        } else if (current_screen != prev_screen) {
            needs_redraw = true;  /* screen transition */
        } else {
            /* Static screens: redraw only on input activity */
            TouchState ts = touch_get_state(&touch);
            if (ts.pressed || ts.held) needs_redraw = true;
            for (int i = 0; i < BTN_ID_COUNT; i++) {
                if (input.buttons[i].pressed) { needs_redraw = true; break; }
            }
        }

        /* The game-over component runs a multi-frame state machine (highscore
         * check, blocking name entry) and only draws once it reaches DISPLAY —
         * give it frames until it says it is settled, or the overlay never
         * appears without a tap. */
        if (current_screen == SCREEN_GAME_OVER && gameover_needs_redraw(&gos, &input))
            needs_redraw = true;

        /* The start menu's bracket blink and attract phase changes arrive
         * with no input; without this the blink freezes. */
        if (current_screen == SCREEN_MENU && start_menu_needs_redraw(&menu))
            needs_redraw = true;

        /* One bed transition, ABOVE the redraw block and before the pump.
         * ⚠️ The position is load-bearing: SCREEN_GAME_OVER's redraw runs
         * gameover_update()'s BLOCKING name entry, so a bed serviced after the
         * block stays PLAYING for the whole keyboard session.  Before the pump
         * so a voice started on this iteration is fed on the same one, and so
         * the release fade is rendered.  Full reason: brick_breaker.c's copy. */
        bed_service();

        if (needs_redraw) {
            draw_all();
            fb_swap(&fb);
        }

        /* Service the stream on EVERY iteration, drawing or not — it holds one
         * lead (~139 ms on the OSS shim) and a skipped service is an audible
         * gap.  ⚠️ audio_pump_active() belongs in the pacing decision: it is
         * unconditionally true while the continuous stream is live, and
         * FRAME_DELAY_IDLE_US (100 ms) is well above the ~55 ms service ceiling
         * the library measures for itself (../common/audio_out.h). */
        audio_pump(&audio);
        usleep((needs_redraw || audio_pump_active(&audio))
               ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
        needs_redraw = false;
    }

    /* Cleanup */
    hw_leds_off();
    hw_set_backlight(100);
    audio_close(&audio);
    gamepad_close(&gamepad);
    touch_close(&touch);
    fb_close(&fb);

    printf("Office Runner ended. Final score: %d\n", score);
    return 0;
}