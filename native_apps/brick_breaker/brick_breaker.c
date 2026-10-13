/*
 * Brick Breaker — Native C Implementation for Arca
 *
 * Features:
 *   - Touch-controlled paddle (drag to move, tap to launch)
 *   - Keyboard, gamepad, and mouse support via unified gamepad module
 *   - 10 levels with varied brick patterns and increasing difficulty
 *   - 9 power-ups in opposing pairs with permanent stacking behavior
 *   - Gradient bricks, glowing ball, ~30 FPS rendering on a fixed 1/30 s game tick
 *   - High-score leaderboard, audio feedback, LED effects
 *
 * Hardware: 800×480 framebuffer, resistive touchscreen, OSS audio, LEDs
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
#include "../common/gamepad.h"
#include "../common/start_menu.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  Constants
 * ══════════════════════════════════════════════════════════════════════════ */

/* Play area.
 *
 * The playfield is drawn and collided against, never pressed (the paddle
 * follows touch X only), so it is the whole VISIBLE rectangle: the ball's
 * ceiling is the top edge of the screen. The menu/exit button row is pressable
 * and therefore sits at SCREEN_SAFE_TOP, overlaid on the field together with
 * the score; the bricks start just below that row (HEADER_BOTTOM), and the
 * header band above them is the space the ball can get "behind" the bricks in.
 * Nothing here adds the touch inset to a drawn edge. */
#define AREA_X      SCREEN_VISIBLE_LEFT
#define AREA_Y      SCREEN_VISIBLE_TOP
#define AREA_W      SCREEN_VISIBLE_WIDTH
#define AREA_H      (SCREEN_VISIBLE_BOTTOM - AREA_Y)
#define HEADER_BOTTOM (LAYOUT_MENU_BTN_Y + BTN_MENU_HEIGHT)
#define BRICK_TOP   (HEADER_BOTTOM + 5)         /* first brick row's top edge, less BRICK_PAD */

/* Paddle */
#define PADDLE_Y        (AREA_Y + AREA_H - 28)
#define PADDLE_HEIGHT   14
#define PADDLE_BASE_W   100
#define PADDLE_KB_SPEED 8       /* px per tick for keyboard/d-pad (240 px/s) */
#define PADDLE_MAX_ANALOG 10    /* max px per tick from analog stick (300 px/s) */

/* Stepped paddle widths: index 0=narrow(-2), 1=small(-1), 2=normal(0), 3=large(+1), 4=larger(+2) */
#define PADDLE_LEVELS 5
static const int PADDLE_WIDTHS[PADDLE_LEVELS] = { 40, 70, 100, 140, 180 };

/* Stepped ball speed multipliers: 0=very slow(-2), 1=slow(-1), 2=normal(0), 3=fast(+1), 4=very fast(+2) */
#define SPEED_LEVELS 5
static const float SPEED_MULTS[SPEED_LEVELS] = { 0.55f, 0.75f, 1.0f, 1.25f, 1.5f };

/* Game clock.  Every speed and frame count in this file is per TICK — one
 * fixed 1/30 s step of game_tick() — not per drawn frame.  update_game() runs
 * as many ticks as elapsed get_time_ms() time calls for, so the values below
 * (authored as px/frame at the nominal 30 fps loop) keep their feel at any
 * frame rate: multiply by 30 for px/s.  A fixed step rather than a variable dt
 * keeps every authored constant and tick-counted timer valid unchanged — the
 * paddle speeds, BALL_*_SPEED, POWERUP_SPEED, particle life and gravity,
 * shake_frames, pending_exp_timer and clear_cooldown. */
#define TICK_S           (1.0f / 30.0f)  /* one game_tick(), seconds */
#define MAX_FRAME_DT     0.1f   /* longest gap one frame may integrate, seconds */
#define MAX_TICKS_FRAME  4      /* tick cap per frame; the remainder is dropped */

/* Ball */
#define BALL_RADIUS     6
#define BALL_SUBSTEP_PX 6.0f    /* longest single ball move, px (= BALL_RADIUS) */
#define MAX_BALLS       8
#define BALL_BASE_SPEED 4.0f
#define BALL_MAX_SPEED  11.0f
#define BALL_SPEED_INC  0.25f  /* Increased from 0.15f for faster progression */

/* Bricks -- compile-time maximums (for array sizing) and orientation presets */
#define BRICK_COLS_LANDSCAPE  12
#define BRICK_COLS_PORTRAIT   8    /* fewer cols in portrait for wider, more playable bricks */
#define BRICK_ROWS_BASE       8    /* base max rows (landscape) -- level defs use this */
#define BRICK_MAX_COLS        12
#define BRICK_MAX_ROWS        16
#define BRICK_PAD       3
#define BRICK_H         20
#define MAX_BRICKS      (BRICK_MAX_COLS * BRICK_MAX_ROWS)  /* 192 */

/* Runtime brick grid dimensions (set by init_brick_layout after fb_init) */
static int brick_cols = BRICK_COLS_LANDSCAPE;
static int brick_rows = BRICK_ROWS_BASE;

/* Power-ups */
#define MAX_POWERUPS    8
#define POWERUP_SIZE    18
#define POWERUP_SPEED   2
#define POWERUP_CHANCE  30      /* percent */

/* Lives and levels */
#define STARTING_LIVES  5
#define MAX_LEVELS      10

/* Particles */
#define MAX_PARTICLES   150

/* Fireball trail */
#define TRAIL_LEN       6

/* ══════════════════════════════════════════════════════════════════════════
 *  Types
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    SCREEN_MENU,
    SCREEN_PLAYING,
    SCREEN_PAUSED,
    SCREEN_LEVEL_COMPLETE,
    SCREEN_GAME_OVER
} GameScreen;

typedef enum {
    BRICK_NORMAL = 0,      /* Regular brick */
    BRICK_INDESTRUCTIBLE,  /* Can only be destroyed with fireball */
    BRICK_BONUS,           /* Gives bonus points (2x) */
    BRICK_EXPLOSIVE        /* Destroys adjacent bricks when hit */
} BrickType;

typedef enum {
    PU_WIDEN,        /* W - make paddle wider (one step) */
    PU_NARROW,       /* N - make paddle narrower (one step) */
    PU_FIREBALL,     /* F - activate fireball mode */
    PU_NORMALBALL,   /* B - deactivate fireball, return to normal */
    PU_SPEED_UP,     /* > - speed up ball (one step) */
    PU_SLOW_DOWN,    /* < - slow down ball (one step) */
    PU_EXTRA_LIFE,   /* + - gain one life */
    PU_LOSE_LIFE,    /* - - lose one life (but NOT if on last life!) */
    PU_MULTIBALL,    /* M - spawn extra balls */
    NUM_POWERUP_TYPES
} PowerUpType;

typedef struct {
    float x, y;
    float dx, dy;
    /* `base_speed` is the ball's own speed — the level multiplier plus every
     * per-brick BALL_SPEED_INC — and deliberately excludes the SPEED UP / SLOW
     * DOWN multiplier.  `speed` is the effective one, always *derived* from the
     * pair by ball_apply_speed().  Keeping the multiplier out of the stored
     * value is the whole point: it used to be re-applied to a figure that
     * already contained it, so the effects compounded and SLOW DOWN from +2 to
     * +1 made the ball faster (1.5 * 1.25). */
    float base_speed;
    float speed;
    bool  active;
    bool  stuck;     /* stuck on paddle until launched */
    bool  fireball;
    /* Fireball visual trail (circular buffer of recent positions) */
    float trail_x[TRAIL_LEN], trail_y[TRAIL_LEN];
    int   trail_idx;  /* next write position in circular buffer */
} Ball;

/* An indestructible brick's health is negative on purpose: it must never be
 * reachable by the `health--` a normal hit does.  So "destroyed" is
 * health == 0 *exactly* — a `health <= 0` test swallows the indestructible ones
 * too, which is what made them invisible and non-colliding from level 5 up, and
 * left both their bounce path and their stripe rendering as dead code.
 * Ask this predicate, never the sign. */
#define BRICK_HEALTH_INDESTRUCTIBLE (-1)

typedef struct {
    int  x, y, w, h;
    int  health;     /* 0 = destroyed, BRICK_HEALTH_INDESTRUCTIBLE = indestructible */
    int  max_health;
    BrickType type;  /* brick special type */
    int  color_index; /* row index for colorful rendering */
    uint32_t color_top;
    uint32_t color_bot;
} Brick;

static inline bool brick_is_destroyed(const Brick *b) { return b->health == 0; }

typedef struct {
    float x, y;
    PowerUpType type;
    bool  active;
} PowerUp;

typedef struct {
    float x, y;
    float dx, dy;
    int   life;     /* ticks remaining */
    uint32_t color;
} Particle;

typedef struct {
    int paddle_level;    /* -2 to +2, starts at 0 (index into PADDLE_WIDTHS is paddle_level + 2) */
    int speed_level;     /* -2 to +2, starts at 0 (index into SPEED_MULTS is speed_level + 2) */
    int fireball;        /* 0 or 1 */
} ActiveEffects;

typedef struct {
    int         score;
    int         lives;
    int         level;        /* 1-based */
    int         bricks_left;
    int         paddle_w;
    float       paddle_x;    /* centre X */
    Ball        balls[MAX_BALLS];
    int         ball_count;
    Brick       bricks[MAX_BRICKS];
    int         brick_count;
    PowerUp     powerups[MAX_POWERUPS];
    Particle    particles[MAX_PARTICLES];
    ActiveEffects fx;
    GameScreen  screen;
    bool        ball_launched;
    /* Screen shake */
    float       shake_x, shake_y;       /* current offset to apply to rendering */
    int         shake_frames;           /* ticks remaining */
    float       shake_intensity;        /* current max amplitude in pixels */

    /* Level clear animation cooldown */
    int         clear_cooldown;         /* ticks remaining before LEVEL_COMPLETE transition */

    /* Deferred explosion chain */
    int         pending_exp[MAX_BRICKS]; /* brick indices waiting to detonate */
    int         pending_exp_timer[MAX_BRICKS]; /* ticks until each detonation */
    int         pending_exp_count;
} GameState;

/* ══════════════════════════════════════════════════════════════════════════
 *  Globals
 * ══════════════════════════════════════════════════════════════════════════ */

static Framebuffer  fb;
static TouchInput   touch;
static GamepadManager gamepad_mgr;
static InputState   gp_input;
static Audio        audio;
static GameState    game;
static HighScoreTable hs;
static bool         running = true;
static uint32_t     frame_time_ms;  /* updated each frame */
static bool test_mode = false;     /* special test levels: the menu's EASY, or --test */

/* Start menu (../common/start_menu.h): START, DIFFICULTY, EXIT.  EASY is the
 * old TEST mode (level 1 all explosive bricks, level 2+ every brick drops a
 * power-up); NORMAL is the game as it always played.  Scores are recorded in
 * both, as TEST always did. */
enum { DIFF_EASY, DIFF_NORMAL, DIFF_COUNT };
static const char *const DIFF_NAMES[DIFF_COUNT] = { "EASY", "NORMAL" };
static StartMenu menu;
static int menu_start_idx, menu_diff_idx, menu_exit_idx;
/* Attract-cycle DEMO: runs while game.screen is SCREEN_MENU and the menu is on
 * its DEMO page.  Paddle AI, silent and LED-dark, no score kept, never a game
 * over: the demo ends (start_menu_demo_over) when the first ball is lost.  The
 * screen stays SCREEN_MENU throughout, so the music bed (SCREEN_PLAYING only)
 * stays off and update_game()'s play clock is not involved. */
static bool demo_running = false;
static int  demo_launch_wait;          /* ticks left before the AI launches the ball */
static uint32_t demo_last_ms;          /* the demo's own frame clock, as play_last_ms */
static float    demo_acc;              /* the demo's tick accumulator, as tick_acc */
#define DEMO_LAUNCH_TICKS 45           /* 1.5 s at 30 ticks/s */
#define DEMO_PADDLE_SPEED 5.0f         /* px per tick: slower than the 8 of a key, so it can miss */
static bool demo_on(void) { return demo_running; }
static float ball_base_speed = BALL_BASE_SPEED;  /* runtime speed, adjusted for screen orientation */
static GameOverScreen gos;  /* unified game over screen */
/* The play clock (update_game).  play_clock_live is false whenever the last
 * update_game() call was outside SCREEN_PLAYING, so the first playing frame
 * after the start menu, a pause, a level change or a restart re-baselines
 * instead of integrating the time spent away. */
static bool     play_clock_live;
static uint32_t play_last_ms;
static float    tick_acc;
/* Analog stick + d-pad paddle velocity, px per tick.  handle_input() samples
 * it once per frame (zero outside play) and every tick of that frame applies
 * it, so the paddle moves the same distance per second at any frame rate. */
static float    paddle_vel;
/* Power-up / lost-ball LED flashes, advanced once per frame by the main loop. */
static LedPulse     fx_pulse;

/* UI buttons */
static Button btn_menu, btn_exit;
static Button btn_restart;
static Button btn_next_level;
static ModalDialog pause_dialog;

/* ── colour palette ──────────────────────────────────────────────────── */
#define BG_COLOR        RGB(8, 12, 30)
#define HUD_COLOR       RGB(200, 200, 200)
#define HEART_COLOR     RGB(255, 60, 80)
#define PADDLE_COLOR    RGB(0, 220, 255)
#define PADDLE_GLOW     RGB(0, 100, 140)
#define BALL_COLOR      RGB(255, 255, 255)
#define BALL_FIRE       RGB(255, 140, 0)

/* Brick colour sets per health [top, bottom] */
static const uint32_t brick_colors[][2] = {
    { RGB(80, 220, 80),   RGB(40, 140, 40)  },  /* health 1 — green */
    { RGB(255, 255, 80),  RGB(180, 160, 30) },  /* health 2 — yellow */
    { RGB(255, 160, 50),  RGB(180, 90, 20)  },  /* health 3 — orange */
    { RGB(255, 60, 60),   RGB(160, 30, 30)  },  /* health 4 — red */
    { RGB(180, 100, 255), RGB(120, 50, 180) },  /* health 5 — purple */
    { RGB(0, 200, 255),   RGB(0, 120, 180)  },  /* health 6 — cyan */
};

/* Row-based color palette for single-hit bricks (rainbow gradient) */
static const uint32_t ROW_COLORS[][2] = {
    { RGB(255, 60, 60),   RGB(180, 30, 30) },   /* 0: Red */
    { RGB(255, 140, 40),  RGB(180, 80, 20) },   /* 1: Orange */
    { RGB(255, 220, 40),  RGB(180, 160, 20) },  /* 2: Yellow */
    { RGB(80, 220, 80),   RGB(40, 140, 40) },   /* 3: Green */
    { RGB(40, 200, 200),  RGB(20, 130, 130) },  /* 4: Teal */
    { RGB(60, 140, 255),  RGB(30, 80, 180) },   /* 5: Blue */
    { RGB(160, 100, 255), RGB(100, 50, 180) },  /* 6: Purple */
    { RGB(255, 100, 200), RGB(180, 50, 130) },  /* 7: Pink */
};
#define NUM_ROW_COLORS 8

/* Special brick colors */
#define BRICK_INDESTRUCTIBLE_TOP  RGB(80, 80, 80)
#define BRICK_INDESTRUCTIBLE_BOT  RGB(50, 50, 50)
#define BRICK_BONUS_TOP           RGB(255, 215, 0)
#define BRICK_BONUS_BOT           RGB(200, 160, 0)
#define BRICK_EXPLOSIVE_TOP       RGB(255, 100, 0)
#define BRICK_EXPLOSIVE_BOT       RGB(180, 50, 0)

/* Power-up visual config (color, letter) per type */
static const struct { uint32_t color; char symbol; const char *name; } pu_info[] = {
    [PU_WIDEN]      = { RGB(80, 220, 80),   'W', "WIDEN"   },
    [PU_NARROW]     = { RGB(255, 60, 60),   'N', "NARROW"  },
    [PU_FIREBALL]   = { RGB(255, 140, 0),   'F', "FIRE"    },
    [PU_NORMALBALL] = { RGB(200, 200, 200), 'B', "NORMAL"  },
    [PU_SPEED_UP]   = { RGB(0, 200, 255),   '>', "SPD UP"  },
    [PU_SLOW_DOWN]  = { RGB(255, 255, 80),  '<', "SPD DN"  },
    [PU_EXTRA_LIFE] = { RGB(255, 100, 200), '+', "LIFE+"   },
    [PU_LOSE_LIFE]  = { RGB(180, 60, 180),  '-', "LIFE-"   },
    [PU_MULTIBALL]  = { RGB(60, 140, 255),  'M', "MULTI"   },
};

/* ══════════════════════════════════════════════════════════════════════════
 *  Level definitions
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    PAT_STANDARD,
    PAT_CHECKERBOARD,
    PAT_PYRAMID,
    PAT_DIAMOND,
    PAT_BORDER,
    PAT_STRIPES,
} BrickPattern;

typedef struct {
    int         rows;
    int         health[BRICK_ROWS_BASE];  /* per-row starting health (base/landscape layout) */
    BrickPattern pattern;
    float       speed_mult;          /* ball speed multiplier */
} LevelDef;

static const LevelDef levels[MAX_LEVELS] = {
    { 5, {1,1,1,1,1,0,0,0},             PAT_STANDARD,     1.0f },
    { 5, {1,1,2,1,1,0,0,0},             PAT_STANDARD,     1.0f },
    { 6, {2,1,2,1,1,1,0,0},             PAT_CHECKERBOARD, 1.05f },
    { 6, {2,2,1,2,1,1,0,0},             PAT_PYRAMID,      1.1f },
    { 7, {2,2,2,2,1,1,1,0},             PAT_DIAMOND,      1.15f },
    { 7, {3,2,2,2,1,1,1,0},             PAT_BORDER,       1.2f },
    { 7, {3,2,2,2,2,1,1,0},             PAT_STRIPES,      1.25f },
    { 8, {3,3,2,2,2,1,1,1},             PAT_CHECKERBOARD, 1.3f },
    { 8, {3,3,3,3,2,2,1,1},             PAT_PYRAMID,      1.35f },
    { 8, {4,4,3,3,2,2,1,1},             PAT_DIAMOND,      1.4f },
};

/* ══════════════════════════════════════════════════════════════════════════
 *  Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void signal_handler(int sig) { (void)sig; running = false; }

static float randf(void) { return (float)rand() / (float)RAND_MAX; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ── Particles ───────────────────────────────────────────────────────── */

/* Called after fb_init() to compute brick_cols and brick_rows from screen size.
 * In landscape (800x480): brick_cols=12, brick_rows=8 (unchanged from original)
 * In portrait  (480x800): brick_cols=8  (wider bricks), brick_rows=~14 (fill top 45%) */
static void init_brick_layout(void) {
    if (fb.portrait_mode) {
        brick_cols = BRICK_COLS_PORTRAIT;
        /* Calculate rows to fill ~45% of the play area height */
        int available_brick_height = (int)(AREA_H * 0.45f);
        brick_rows = available_brick_height / (BRICK_H + BRICK_PAD);
        if (brick_rows < BRICK_ROWS_BASE) brick_rows = BRICK_ROWS_BASE;
        if (brick_rows > BRICK_MAX_ROWS) brick_rows = BRICK_MAX_ROWS;
    } else {
        brick_cols = BRICK_COLS_LANDSCAPE;
        brick_rows = BRICK_ROWS_BASE;
    }
    printf("Brick layout: %dx%d (portrait=%d, AREA_H=%d)\n",
           brick_cols, brick_rows, fb.portrait_mode, AREA_H);
}

static void spawn_particles(float cx, float cy, uint32_t color, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < MAX_PARTICLES; j++) {
            if (game.particles[j].life <= 0) {
                Particle *p = &game.particles[j];
                p->x = cx;
                p->y = cy;
                p->dx = (randf() - 0.5f) * 6.0f;
                p->dy = (randf() - 0.5f) * 6.0f;
                p->life = 12 + (rand() % 10);
                p->color = color;
                break;
            }
        }
    }
}

static void update_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &game.particles[i];
        if (p->life <= 0) continue;
        p->x += p->dx;
        p->y += p->dy;
        p->dy += 0.15f;  /* gravity */
        p->life--;
    }
}

static void draw_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &game.particles[i];
        if (p->life <= 0) continue;
        int px = (int)p->x;
        int py = (int)p->y;
        uint8_t alpha = (uint8_t)(255 * p->life / 22);
        fb_draw_pixel_alpha(&fb, px, py, p->color, alpha);
        fb_draw_pixel_alpha(&fb, px + 1, py, p->color, alpha);
        fb_draw_pixel_alpha(&fb, px, py + 1, p->color, alpha);
        fb_draw_pixel_alpha(&fb, px + 1, py + 1, p->color, alpha);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Brick layout
 * ══════════════════════════════════════════════════════════════════════════ */

static bool pattern_has_brick(BrickPattern pat, int row, int col, int rows, int cols) {
    switch (pat) {
    case PAT_STANDARD:
        return true;
    case PAT_CHECKERBOARD:
        return (row + col) % 2 == 0;
    case PAT_PYRAMID: {
        int bricks_in_row = cols - row * 2;
        if (bricks_in_row <= 0) return false;
        int start = (cols - bricks_in_row) / 2;
        return col >= start && col < start + bricks_in_row;
    }
    case PAT_DIAMOND: {
        int mid_row = rows / 2;
        int dist = abs(row - mid_row);
        int half = (cols / 2) - dist;
        if (half < 1) half = 1;
        int start = cols / 2 - half;
        int end   = cols / 2 + half;
        return col >= start && col < end;
    }
    case PAT_BORDER:
        return row == 0 || row == rows - 1 || col == 0 || col == cols - 1;
    case PAT_STRIPES:
        return col % 3 != 1;  /* every 3rd column gap */
    }
    return true;
}

static void create_bricks(void) {
    const LevelDef *lv = &levels[clampi(game.level - 1, 0, MAX_LEVELS - 1)];
    int brick_w = (AREA_W - BRICK_PAD * (brick_cols + 1)) / brick_cols;
    game.brick_count = 0;
    game.bricks_left = 0;
    
    /* Scale the level's row count proportionally to the available brick_rows.
     * In landscape: brick_rows == BRICK_ROWS_BASE (8), so scaled_rows == lv->rows (unchanged).
     * In portrait:  brick_rows is larger (~14), so levels get proportionally more rows. */
    int scaled_rows = lv->rows * brick_rows / BRICK_ROWS_BASE;
    if (scaled_rows > brick_rows) scaled_rows = brick_rows;
    if (scaled_rows < lv->rows) scaled_rows = lv->rows;  /* never fewer than base */

    for (int r = 0; r < scaled_rows && r < brick_rows; r++) {
        for (int c = 0; c < brick_cols; c++) {
            if (!pattern_has_brick(lv->pattern, r, c, scaled_rows, brick_cols))
                continue;

            /* Map this row back to the base health pattern.
             * Stretches the original health[] across the scaled row count. */
            int base_r = r * lv->rows / scaled_rows;
            if (base_r >= lv->rows) base_r = lv->rows - 1;

            Brick *b = &game.bricks[game.brick_count];
            b->x = AREA_X + BRICK_PAD + c * (brick_w + BRICK_PAD);
            b->y = BRICK_TOP + BRICK_PAD + r * (BRICK_H + BRICK_PAD);
            b->w = brick_w;
            b->h = BRICK_H;
            b->health = lv->health[base_r];
            b->max_health = b->health;
            b->color_index = r;
            
            /* Determine brick type - special bricks on higher levels
             * Level 3-6: 15% chance; Level 7+: 25% chance
             * Among special bricks, explosive is weighted ~50% */
            b->type = BRICK_NORMAL;
            int special_chance = (game.level >= 7) ? 25 : 15;
            if (game.level >= 3 && (rand() % 100) < special_chance) {
                int roll = rand() % 100;
                if (roll < 15 && game.level >= 5) {
                    /* 15% of specials: indestructible (level 5+) */
                    b->type = BRICK_INDESTRUCTIBLE;
                    b->health = BRICK_HEALTH_INDESTRUCTIBLE;
                    b->color_top = BRICK_INDESTRUCTIBLE_TOP;
                    b->color_bot = BRICK_INDESTRUCTIBLE_BOT;
                } else if (roll < 40) {
                    /* 25% of specials: bonus */
                    b->type = BRICK_BONUS;
                    b->color_top = BRICK_BONUS_TOP;
                    b->color_bot = BRICK_BONUS_BOT;
                } else {
                    /* ~60% of specials: explosive */
                    b->type = BRICK_EXPLOSIVE;
                    b->color_top = BRICK_EXPLOSIVE_TOP;
                    b->color_bot = BRICK_EXPLOSIVE_BOT;
                }
            } else {
                /* Normal brick - use health-based colors */
                int ci = clampi(b->health - 1, 0, 5);
                b->color_top = brick_colors[ci][0];
                b->color_bot = brick_colors[ci][1];
            }

            /* ── Test mode level overrides ────────────────────── */
            if (test_mode) {
                if (game.level == 1) {
                    /* Test level 1: ALL bricks are explosive */
                    b->type = BRICK_EXPLOSIVE;
                    b->health = 1;
                    b->max_health = 1;
                    b->color_top = BRICK_EXPLOSIVE_TOP;
                    b->color_bot = BRICK_EXPLOSIVE_BOT;
                } else if (game.level == 2) {
                    /* Test level 2: normal bricks, but power-ups guaranteed (handled in spawn_powerup) */
                    /* Keep normal brick type */
                }
            }

            game.brick_count++;
            /* Only count non-indestructible bricks for level completion */
            if (b->type != BRICK_INDESTRUCTIBLE) {
                game.bricks_left++;
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Effects helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Reset all active effects to default (called on ball lost / level complete) */
static void reset_effects(void) {
    game.fx.paddle_level = 0;
    game.fx.speed_level = 0;
    game.fx.fireball = 0;
}

/* Apply paddle width from current effect level */
static void apply_paddle_width(void) {
    int idx = clampi(game.fx.paddle_level + 2, 0, PADDLE_LEVELS - 1);
    game.paddle_w = PADDLE_WIDTHS[idx];
}

/* Get current speed multiplier from effect level */
static float get_speed_mult(void) {
    int idx = clampi(game.fx.speed_level + 2, 0, SPEED_LEVELS - 1);
    return SPEED_MULTS[idx];
}

/* Recompute a ball's effective speed from its base speed and the current effect
 * level, and re-scale its velocity to match.  This is the only writer of
 * b->speed, so the multiplier is applied exactly once no matter how many
 * power-ups have been collected.  BALL_MAX_SPEED clamps the *effective* speed,
 * as it always has — the 11 px/tick cap bounds the substeps update_balls()
 * splits a tick into against tunnelling, and that is a property of the speed
 * the ball actually travels at, not of the base. */
static void ball_apply_speed(Ball *b) {
    float sp = b->base_speed * get_speed_mult();
    if (sp > BALL_MAX_SPEED) sp = BALL_MAX_SPEED;
    b->speed = sp;
    float mag = sqrtf(b->dx * b->dx + b->dy * b->dy);
    if (mag > 0.01f) {
        b->dx = (b->dx / mag) * sp;
        b->dy = (b->dy / mag) * sp;
    }
}

/* Re-derive every ball's speed after the effect level changed.  Stuck balls are
 * included: a power-up can be caught while the next life's ball waits on the
 * paddle, and skipping it left that ball carrying the previous multiplier. */
static void normalize_ball_speeds(void) {
    for (int i = 0; i < game.ball_count; i++) {
        Ball *b = &game.balls[i];
        if (!b->active) continue;
        ball_apply_speed(b);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Ball management
 * ══════════════════════════════════════════════════════════════════════════ */

/* `base_speed` excludes the SPEED UP / SLOW DOWN multiplier — see the Ball
 * struct.  Callers pass dx/dy already scaled to the effective speed; the clamp
 * inside ball_apply_speed() re-scales them if the pair disagrees. */
static Ball *add_ball(float x, float y, float dx, float dy, float base_speed) {
    if (game.ball_count >= MAX_BALLS) return NULL;
    Ball *b = &game.balls[game.ball_count++];
    b->x = x; b->y = y;
    b->dx = dx; b->dy = dy;
    b->base_speed = base_speed;
    b->active = true;
    b->stuck = false;
    b->fireball = (game.fx.fireball != 0);
    ball_apply_speed(b);
    /* Initialize trail positions to starting position */
    for (int t = 0; t < TRAIL_LEN; t++) {
        b->trail_x[t] = x;
        b->trail_y[t] = y;
    }
    b->trail_idx = 0;
    return b;
}

static void reset_ball_on_paddle(void) {
    game.ball_count = 0;
    float start_x = game.paddle_x;
    float start_y = PADDLE_Y - BALL_RADIUS - 1;
    const LevelDef *lv = &levels[clampi(game.level - 1, 0, MAX_LEVELS - 1)];
    Ball *b = add_ball(start_x, start_y, 0, 0, ball_base_speed * lv->speed_mult);
    if (b) {
        b->stuck = true;
        /* Re-initialize trail to paddle position (add_ball already did this) */
    }
    game.ball_launched = false;
}

static void launch_ball(void) {
    if (game.ball_launched) return;
    for (int i = 0; i < game.ball_count; i++) {
        Ball *b = &game.balls[i];
        if (b->stuck) {
            float angle = -M_PI / 2.0f + (randf() - 0.5f) * 0.6f;
            b->dx = cosf(angle) * b->speed;
            b->dy = sinf(angle) * b->speed;
            b->stuck = false;
            game.ball_launched = true;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Power-up management
 * ══════════════════════════════════════════════════════════════════════════ */

static void spawn_powerup(float x, float y) {
    /* In test mode level 2+: every brick drops a power-up */
    if (!test_mode || game.level < 2) {
        if ((rand() % 100) >= POWERUP_CHANCE) return;
    }

    /* Weighted random selection among 9 types.
     * Positive (widen, fireball, slow_down, extra_life, multiball): ~13% each = 65%
     * Negative (narrow, normalball, speed_up, lose_life): ~8-9% each = 35%
     * Using weights summing to 100:
     *   PU_WIDEN=13, PU_NARROW=9, PU_FIREBALL=13, PU_NORMALBALL=9,
     *   PU_SPEED_UP=9, PU_SLOW_DOWN=13, PU_EXTRA_LIFE=13, PU_LOSE_LIFE=8, PU_MULTIBALL=13
     */
    static const struct { PowerUpType type; int weight; } pu_weights[] = {
        { PU_WIDEN,      13 },
        { PU_NARROW,      9 },
        { PU_FIREBALL,   13 },
        { PU_NORMALBALL,  9 },
        { PU_SPEED_UP,    9 },
        { PU_SLOW_DOWN,  13 },
        { PU_EXTRA_LIFE, 13 },
        { PU_LOSE_LIFE,   8 },
        { PU_MULTIBALL,  13 },
    };
    static const int total_weight = 100;

    int roll = rand() % total_weight;
    PowerUpType type = PU_WIDEN;  /* fallback */
    int cumulative = 0;
    for (int i = 0; i < (int)(sizeof(pu_weights) / sizeof(pu_weights[0])); i++) {
        cumulative += pu_weights[i].weight;
        if (roll < cumulative) {
            type = pu_weights[i].type;
            break;
        }
    }

    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (!game.powerups[i].active) {
            game.powerups[i] = (PowerUp){ x, y, type, true };
            return;
        }
    }
}

static void apply_powerup(PowerUpType type) {
    /* Non-blocking 50 ms green flash.  This used to be hw_set_led() here and
     * `usleep(50000); hw_leds_off();` at the end of the function — a sleep in an
     * update path, which freezes touch and rendering along with it. */
    if (!demo_on())   /* the demo is LED-dark and silent */
        hw_led_pulse_start(&fx_pulse, LED_GREEN, 1, 50, get_time_ms());

    switch (type) {
    case PU_WIDEN:
        if (game.fx.paddle_level < 2) game.fx.paddle_level++;
        apply_paddle_width();
        break;
    case PU_NARROW:
        if (game.fx.paddle_level > -2) game.fx.paddle_level--;
        apply_paddle_width();
        break;
    case PU_FIREBALL:
        game.fx.fireball = 1;
        for (int i = 0; i < game.ball_count; i++)
            game.balls[i].fireball = true;
        break;
    case PU_NORMALBALL:
        game.fx.fireball = 0;
        for (int i = 0; i < game.ball_count; i++)
            game.balls[i].fireball = false;
        break;
    case PU_SPEED_UP:
        if (game.fx.speed_level < 2) game.fx.speed_level++;
        normalize_ball_speeds();
        break;
    case PU_SLOW_DOWN:
        if (game.fx.speed_level > -2) game.fx.speed_level--;
        normalize_ball_speeds();
        break;
    case PU_EXTRA_LIFE:
        game.lives++;
        if (!demo_on()) audio_blip(&audio);
        break;
    case PU_LOSE_LIFE:
        if (game.lives > 1) game.lives--;
        break;
    case PU_MULTIBALL: {
        /* Spawn 2 new balls moving upward at different angles */
        int n = game.ball_count;
        for (int i = 0; i < n && game.ball_count < MAX_BALLS - 1; i++) {
            Ball *src = &game.balls[i];
            if (!src->active || src->stuck) continue;
            
            /* Spawn balls moving upward at -60° and -120° (30° left/right of vertical) */
            float angle1 = -M_PI / 2.0f - 0.5f;  /* upward-left */
            float angle2 = -M_PI / 2.0f + 0.5f;  /* upward-right */
            
            /* dx/dy from the effective speed, base_speed inherited as the base —
             * passing src->speed as the base would bake the current multiplier
             * in a second time. */
            add_ball(src->x, src->y,
                     cosf(angle1) * src->speed,
                     sinf(angle1) * src->speed,
                     src->base_speed);
            add_ball(src->x, src->y,
                     cosf(angle2) * src->speed,
                     sinf(angle2) * src->speed,
                     src->base_speed);
        }
        if (!demo_on()) audio_blip(&audio);
        break;
    }
    default:
        break;
    }

    /* Brief LED + beep */
    if (type != PU_MULTIBALL && type != PU_EXTRA_LIFE && !demo_on())
        audio_beep(&audio);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Game state management
 * ══════════════════════════════════════════════════════════════════════════ */

static void init_level(void) {
    /* Reset power-ups, effects, and screen shake */
    memset(game.powerups, 0, sizeof(game.powerups));
    memset(game.particles, 0, sizeof(game.particles));
    hw_led_pulse_stop(&fx_pulse);   /* no flash carried into the new level */
    game.shake_x = 0; game.shake_y = 0;
    game.shake_frames = 0; game.shake_intensity = 0;
    game.clear_cooldown = 0;
    game.pending_exp_count = 0;
    reset_effects();
    apply_paddle_width();
    game.paddle_x = AREA_X + AREA_W / 2;
    game.ball_count = 0;

    create_bricks();
    reset_ball_on_paddle();
}

static void reset_game(void) {
    game.score = 0;
    game.lives = STARTING_LIVES;
    game.level = 1;
    game.screen = SCREEN_PLAYING;
    init_level();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Physics / update
 * ══════════════════════════════════════════════════════════════════════════ */

/* Queue a brick for deferred detonation (chain explosion with delay) */
static void queue_deferred_explosion(int brick_idx, int delay_ticks) {
    if (game.pending_exp_count >= MAX_BRICKS) return;
    /* Don't queue if already pending */
    for (int i = 0; i < game.pending_exp_count; i++) {
        if (game.pending_exp[i] == brick_idx) return;
    }
    game.pending_exp[game.pending_exp_count] = brick_idx;
    game.pending_exp_timer[game.pending_exp_count] = delay_ticks;
    game.pending_exp_count++;
}

/* Process one deferred detonation: destroy the brick and its non-explosive neighbors,
 * queue any adjacent explosive bricks for further chain reaction */
static void detonate_brick(int brick_idx) {
    Brick *br = &game.bricks[brick_idx];
    if (brick_is_destroyed(br)) return;  /* Already destroyed (e.g., by ball in the meantime) */

    br->health = 0;
    if (br->type != BRICK_INDESTRUCTIBLE) {
        game.bricks_left--;
    }
    game.score += 15 * game.level;

    /* Explosion particles */
    spawn_particles((float)(br->x + br->w / 2),
                    (float)(br->y + br->h / 2),
                    BRICK_EXPLOSIVE_TOP, 6);
    spawn_powerup((float)(br->x + br->w / 2),
                  (float)(br->y + br->h / 2));

    /* Shake — logarithmic accumulation */
    if (game.shake_frames < 3) {
        game.shake_frames = 10;
        game.shake_intensity = 3.0f;
    } else {
        game.shake_frames += 3;
        float headroom = 8.0f - game.shake_intensity;
        if (headroom > 0.2f)
            game.shake_intensity += headroom * 0.3f;
        if (game.shake_frames > 30)
            game.shake_frames = 30;
    }

    /* Destroy adjacent bricks */
    for (int k = 0; k < game.brick_count; k++) {
        Brick *adj = &game.bricks[k];
        if (brick_is_destroyed(adj)) continue;
        if (k == brick_idx) continue;

        int adx = abs((adj->x + adj->w/2) - (br->x + br->w/2));
        int ady = abs((adj->y + adj->h/2) - (br->y + br->h/2));
        if (adx < br->w * 1.5f && ady < br->h * 1.5f) {
            if (adj->type == BRICK_INDESTRUCTIBLE) continue;

            if (adj->type == BRICK_EXPLOSIVE) {
                /* Chain: queue with delay for visual ripple */
                queue_deferred_explosion(k, 5);
            } else {
                /* Non-explosive neighbor: destroy immediately */
                adj->health = 0;
                game.bricks_left--;
                game.score += 10 * game.level;
                spawn_particles((float)(adj->x + adj->w / 2),
                               (float)(adj->y + adj->h / 2),
                               adj->color_top, 4);
                spawn_powerup((float)(adj->x + adj->w / 2),
                              (float)(adj->y + adj->h / 2));
            }
        }
    }

}

/* One substep of a free ball: move by `frac` of its per-tick velocity, then
 * walls, paddle, the lost-ball line and at most one brick.  update_balls() runs
 * ceil(speed / BALL_SUBSTEP_PX) of these per tick, so no single move is longer
 * than BALL_SUBSTEP_PX (= BALL_RADIUS) and the ball cannot jump a brick, its
 * corner or the paddle between two tests.  Every side effect below (score,
 * particles, power-up drop, audio, shake, chained explosions, speed-up) belongs
 * to one hit and fires once for it; a step that hits nothing does nothing. */
static void ball_substep(Ball *b, float frac, int paddle_left, int paddle_right) {
    b->x += b->dx * frac;
    b->y += b->dy * frac;

    /* Wall collisions */
    if (b->x - BALL_RADIUS < AREA_X) {
        b->x = AREA_X + BALL_RADIUS;
        b->dx = fabsf(b->dx);
    }
    if (b->x + BALL_RADIUS > AREA_X + AREA_W) {
        b->x = AREA_X + AREA_W - BALL_RADIUS;
        b->dx = -fabsf(b->dx);
    }
    if (b->y - BALL_RADIUS < AREA_Y) {
        b->y = AREA_Y + BALL_RADIUS;
        b->dy = fabsf(b->dy);
    }

    /* Paddle collision */
    if (b->dy > 0 &&
        b->y + BALL_RADIUS >= PADDLE_Y &&
        b->y - BALL_RADIUS <= PADDLE_Y + PADDLE_HEIGHT &&
        b->x >= paddle_left && b->x <= paddle_right) {

        b->y = PADDLE_Y - BALL_RADIUS;
        /* Angle depends on where ball hit the paddle */
        float hit = (b->x - paddle_left) / (float)game.paddle_w;  /* 0..1 */
        float angle = (hit - 0.5f) * 1.2f;  /* -0.6 .. +0.6 rad */
        /* Ensure ball always goes upward */
        float out_angle = -M_PI / 2.0f + angle;
        b->dx = cosf(out_angle) * b->speed;
        b->dy = sinf(out_angle) * b->speed;

        /* ⚠️ **No `audio_interrupt()` before an effect on the mix bus** —
         * these five sites had one, and on the bus it means "stop ALL
         * voices", so a brick hit threw away the 600 ms `audio_fail()`
         * fanfare a lost ball had just started.  Measured by ear on `.188`
         * 2026-08-20: the fanfare was audible when the game ENDED (no
         * further play to interrupt it) and inaudible when a life was lost
         * with balls remaining.  The idiom was free when the kernel ring
         * held exactly one sound; mixing is the whole point of the bus.
         * The four sites below dropped theirs for the same reason. */
        if (!demo_on()) audio_knock(&audio);
    }

    /* Bottom — ball lost */
    if (b->y - BALL_RADIUS > PADDLE_Y + PADDLE_HEIGHT + 20) {
        b->active = false;
    }

    /* Brick collisions */
    for (int j = 0; j < game.brick_count; j++) {
        Brick *br = &game.bricks[j];
        if (brick_is_destroyed(br)) continue;

        /* AABB check */
        if (b->x + BALL_RADIUS <= br->x || b->x - BALL_RADIUS >= br->x + br->w ||
            b->y + BALL_RADIUS <= br->y || b->y - BALL_RADIUS >= br->y + br->h)
            continue;

        /* Hit! Handle special brick types */
        bool brick_destroyed = false;
        
        if (br->type == BRICK_INDESTRUCTIBLE) {
            /* Indestructible bricks can only be destroyed by fireball */
            if (b->fireball) {
                br->health = 0;
                brick_destroyed = true;
                game.score += 50 * game.level;  /* Bonus for destroying indestructible */
            }
            /* Otherwise just bounce off */
        } else if (br->type == BRICK_BONUS) {
            /* Bonus brick - double points */
            br->health = 0;
            brick_destroyed = true;
            game.score += 20 * game.level;  /* 2x normal points */
            if (!demo_on()) audio_sparkle(&audio);   /* bonus brick — fx_sparkle */
        } else if (br->type == BRICK_EXPLOSIVE) {
            /* Explosive brick — immediate detonation + deferred chain */
            br->health = 0;
            brick_destroyed = true;
            game.score += 15 * game.level;
            
            /* Initial shake */
            game.shake_frames = 10;
            game.shake_intensity = 3.0f;
            
            /* Destroy adjacent non-explosive bricks immediately,
             * queue adjacent explosive bricks for deferred chain reaction */
            for (int k = 0; k < game.brick_count; k++) {
                if (k == j) continue;
                Brick *adj = &game.bricks[k];
                if (brick_is_destroyed(adj)) continue;
                
                int adx = abs((adj->x + adj->w/2) - (br->x + br->w/2));
                int ady = abs((adj->y + adj->h/2) - (br->y + br->h/2));
                if (adx < br->w * 1.5f && ady < br->h * 1.5f) {
                    if (adj->type == BRICK_INDESTRUCTIBLE) continue;
                    
                    if (adj->type == BRICK_EXPLOSIVE) {
                        /* Defer chain reaction — explodes 5 ticks later */
                        queue_deferred_explosion(k, 5);
                    } else {
                        /* Non-explosive neighbor: destroy now */
                        adj->health = 0;
                        game.bricks_left--;
                        game.score += 10 * game.level;
                        spawn_particles((float)(adj->x + adj->w / 2),
                                      (float)(adj->y + adj->h / 2),
                                      adj->color_top, 4);
                        spawn_powerup((float)(adj->x + adj->w / 2),
                                     (float)(adj->y + adj->h / 2));
                    }
                }
            }
            
            if (!demo_on()) audio_burst(&audio);
        } else {
            /* Normal brick */
            if (b->fireball) {
                br->health = 0;
                brick_destroyed = true;
            } else {
                br->health--;
                /* Update colours for new health */
                int ci = clampi(br->health - 1, 0, 5);
                br->color_top = brick_colors[ci][0];
                br->color_bot = brick_colors[ci][1];
                if (brick_is_destroyed(br)) {
                    brick_destroyed = true;
                }
            }
            game.score += 10 * game.level;
        }

        if (brick_destroyed) {
            if (br->type != BRICK_INDESTRUCTIBLE) {
                game.bricks_left--;
            }
            /* Fireball destruction: fire-colored particles, more of them */
            if (b->fireball) {
                /* Fire-colored particles for fireball destruction */
                uint32_t fc[] = {
                    RGB(255, 80, 0),
                    RGB(255, 200, 0),
                    RGB(255, 140, 0),
                    RGB(255, 50, 20)
                };
                for (int pi = 0; pi < 8; pi++) {
                    uint32_t pc = fc[rand() % 4];
                    spawn_particles((float)(br->x + br->w / 2),
                                    (float)(br->y + br->h / 2),
                                    pc, 1);
                }
            } else {
                spawn_particles((float)(br->x + br->w / 2),
                                (float)(br->y + br->h / 2),
                                br->color_top, 6);
            }
            spawn_powerup((float)(br->x + br->w / 2),
                          (float)(br->y + br->h / 2));
            if (!demo_on()) audio_tick(&audio);
        } else if (br->type != BRICK_INDESTRUCTIBLE) {
            if (!demo_on()) audio_thud(&audio);
        }

        /* Bounce (skip if fireball or hit indestructible without fireball) */
        if (!b->fireball || br->type == BRICK_INDESTRUCTIBLE) {
            float overlap_l = (b->x + BALL_RADIUS) - br->x;
            float overlap_r = (br->x + br->w) - (b->x - BALL_RADIUS);
            float overlap_t = (b->y + BALL_RADIUS) - br->y;
            float overlap_b = (br->y + br->h) - (b->y - BALL_RADIUS);
            float min_o = overlap_l;
            if (overlap_r < min_o) min_o = overlap_r;
            if (overlap_t < min_o) min_o = overlap_t;
            if (overlap_b < min_o) min_o = overlap_b;

            if (min_o == overlap_l || min_o == overlap_r)
                b->dx = -b->dx;
            else
                b->dy = -b->dy;
        }

        /* Speed up slightly — on base_speed, so the effect multiplier stays
         * a multiplier and is not accumulated into the ball's own speed. */
        if (b->base_speed < BALL_MAX_SPEED) {
            b->base_speed += BALL_SPEED_INC;
            ball_apply_speed(b);
        }

        break;  /* one brick per substep per ball */
    }
}

static void update_balls(void) {
    int paddle_left  = (int)game.paddle_x - game.paddle_w / 2;
    int paddle_right = paddle_left + game.paddle_w;

    for (int i = 0; i < game.ball_count; i++) {
        Ball *b = &game.balls[i];
        if (!b->active) continue;

        /* Stuck ball follows paddle */
        if (b->stuck) {
            b->x = game.paddle_x;
            b->y = PADDLE_Y - BALL_RADIUS - 1;
            /* Update trail to paddle position while stuck */
            for (int t = 0; t < TRAIL_LEN; t++) {
                b->trail_x[t] = b->x;
                b->trail_y[t] = b->y;
            }
            continue;
        }

        /* Push current position into trail buffer before moving — once per
         * tick, not per substep, so the trail spans the same time at any speed. */
        b->trail_x[b->trail_idx] = b->x;
        b->trail_y[b->trail_idx] = b->y;
        b->trail_idx = (b->trail_idx + 1) % TRAIL_LEN;

        /* b->speed is |(dx, dy)| (ball_apply_speed), capped at BALL_MAX_SPEED,
         * so this is 1 or 2 substeps.  The fraction is fixed for the tick: a
         * brick hit that speeds the ball up mid-tick lengthens the remaining
         * substep by at most BALL_SPEED_INC / n. */
        int n = (int)ceilf(b->speed * (1.0f / BALL_SUBSTEP_PX));
        if (n < 1) n = 1;
        float frac = 1.0f / (float)n;
        for (int s = 0; s < n && b->active; s++)
            ball_substep(b, frac, paddle_left, paddle_right);
    }

    /* Remove inactive balls */
    int alive = 0;
    for (int i = 0; i < game.ball_count; i++) {
        if (game.balls[i].active) {
            if (alive != i) game.balls[alive] = game.balls[i];
            alive++;
        }
    }
    game.ball_count = alive;
}

static void update_powerups(void) {
    int paddle_left  = (int)game.paddle_x - game.paddle_w / 2;
    int paddle_right = paddle_left + game.paddle_w;

    for (int i = 0; i < MAX_POWERUPS; i++) {
        PowerUp *pu = &game.powerups[i];
        if (!pu->active) continue;

        pu->y += POWERUP_SPEED;

        /* Caught by paddle */
        if (pu->y + POWERUP_SIZE / 2 >= PADDLE_Y &&
            pu->x >= paddle_left && pu->x <= paddle_right) {
            apply_powerup(pu->type);
            pu->active = false;
            continue;
        }

        /* Off screen */
        if (pu->y > AREA_Y + AREA_H + POWERUP_SIZE) {
            pu->active = false;
        }
    }
}

/* One fixed TICK_S step of everything that advances the game. */
static void game_tick(void) {
    /* Continuously apply paddle width from effect level */
    apply_paddle_width();

    if (demo_on()) {
        /* AI: launch after a short wait, then chase the lowest falling ball's x
         * at a limited speed (DEMO_PADDLE_SPEED < a key's 8 px/tick) so it is
         * not perfect and eventually misses, which ends the demo. */
        if (!game.ball_launched) {
            if (demo_launch_wait > 0) demo_launch_wait--;
            else launch_ball();
        }
        const Ball *tgt = NULL;
        for (int i = 0; i < game.ball_count; i++) {
            const Ball *b = &game.balls[i];
            if (!b->active) continue;
            if (!tgt || b->y > tgt->y) tgt = b;
        }
        if (tgt) {
            float diff = tgt->x - game.paddle_x;
            float step = clampf(diff, -DEMO_PADDLE_SPEED, DEMO_PADDLE_SPEED);
            game.paddle_x = clampf(game.paddle_x + step,
                                   AREA_X + game.paddle_w / 2.0f,
                                   AREA_X + AREA_W - game.paddle_w / 2.0f);
        }
        paddle_vel = 0.0f;   /* nothing but the AI moves the paddle */
    }

    if (paddle_vel != 0.0f)
        game.paddle_x = clampf(game.paddle_x + paddle_vel,
                               AREA_X + game.paddle_w / 2.0f,
                               AREA_X + AREA_W - game.paddle_w / 2.0f);

    update_balls();
    update_powerups();
    update_particles();

    /* Process deferred explosions (chain reaction with visual delay) */
    for (int i = 0; i < game.pending_exp_count; ) {
        game.pending_exp_timer[i]--;
        if (game.pending_exp_timer[i] <= 0) {
            int brick_idx = game.pending_exp[i];
            /* Remove from queue (swap with last) */
            game.pending_exp[i] = game.pending_exp[game.pending_exp_count - 1];
            game.pending_exp_timer[i] = game.pending_exp_timer[game.pending_exp_count - 1];
            game.pending_exp_count--;
            /* Detonate */
            detonate_brick(brick_idx);
            /* Don't increment i — swapped element needs checking */
        } else {
            i++;
        }
    }

    /* Screen shake update */
    if (game.shake_frames > 0) {
        game.shake_frames--;
        float t = game.shake_intensity * ((float)game.shake_frames / 30.0f);
        game.shake_x = ((rand() % 200) / 100.0f - 1.0f) * t;
        game.shake_y = ((rand() % 200) / 100.0f - 1.0f) * t;
        if (game.shake_frames <= 0) {
            game.shake_x = 0;
            game.shake_y = 0;
            game.shake_intensity = 0;
        }
    }

    /* All balls lost? (skip during level clear cooldown) */
    if (game.ball_count == 0 && game.ball_launched && game.clear_cooldown == 0) {
        if (demo_on()) {
            /* The demo ends with its first lost ball: no life, flash, sound,
             * score or game over.  The field is reset when the next demo starts. */
            start_menu_demo_over(&menu);
            return;
        }
        game.lives--;
        /* Non-blocking 300 ms red flash — the usleep() that was here froze the
         * panel for 300 ms on every lost ball. */
        hw_led_pulse_start(&fx_pulse, LED_RED, 1, 300, get_time_ms());

        /* Reset effects on ball lost */
        reset_effects();
        apply_paddle_width();

        if (game.lives <= 0) {
            /* ⚠️ **The RUN ending and ONE life ending must not be the same
             * noise** — that is the whole reason `audio_gameover()` exists, and
             * this game played `audio_fail()` for both, so a player could not
             * hear which had happened.  It fires INSTEAD of fail, not after it:
             * fx_gameover is 1.19 s against fail's 350 ms and the two would sum
             * on the bus. */
            audio_gameover(&audio);
            game.screen = SCREEN_GAME_OVER;
            {
                char info[64];
                snprintf(info, sizeof(info), "LEVEL %d", game.level);
                gameover_init(&gos, &fb, game.score, NULL, info,
                              "BRICK BREAKER", &hs, &touch, &gamepad_mgr);
            }
        } else {
            audio_fail(&audio);
            reset_ball_on_paddle();
        }
    }

    /* Level clear? — Start cooldown to let explosions/particles play out */
    if (game.bricks_left <= 0 && game.clear_cooldown == 0 && game.screen == SCREEN_PLAYING && game.pending_exp_count == 0) {
        game.clear_cooldown = 90;  /* ticks: 3 s */
        audio_success(&audio);
        hw_set_led(LED_GREEN, 100);
    }

    /* Level clear cooldown — keep rendering particles/shake, then transition */
    if (game.clear_cooldown > 0) {
        game.clear_cooldown--;
        if (game.clear_cooldown <= 0) {
            game.screen = SCREEN_LEVEL_COMPLETE;
            game.score += game.lives * 50;  /* bonus */
            reset_effects();
            apply_paddle_width();
            hw_led_pulse_stop(&fx_pulse);  /* so a pulse cannot re-light it */
            hw_leds_off();
        }
    }
}

/* Fixed-step accumulator: elapsed time (clamped to MAX_FRAME_DT) is banked in
 * tick_acc and spent in whole ticks, at most MAX_TICKS_FRAME per frame.  Hitting
 * the cap drops the remainder rather than carrying a debt into later frames.
 * Drawing stays once per frame, showing the state after the last tick.  Launch
 * is an edge handled in handle_input() (launch_ball() only sets a velocity), so
 * no input edge needs holding for a tick here. */
static void update_game(void) {
    if (demo_on()) {
        /* The demo runs on SCREEN_MENU with its own clock; the play clock is
         * left not-live so real play re-baselines on entry. */
        play_clock_live = false;
        uint32_t dnow = get_time_ms();
        float ddt = (float)(dnow - demo_last_ms) * 0.001f;
        if (ddt > MAX_FRAME_DT) ddt = MAX_FRAME_DT;
        demo_last_ms = dnow;
        demo_acc += ddt;
        int dsteps = 0;
        while (demo_acc >= TICK_S && dsteps < MAX_TICKS_FRAME) {
            game_tick();
            demo_acc -= TICK_S;
            dsteps++;
            /* game_tick() reported the lost ball to the menu: stop ticking. */
            if (game.ball_launched && game.ball_count == 0) break;
        }
        if (dsteps >= MAX_TICKS_FRAME) demo_acc = 0.0f;
        return;
    }

    if (game.screen != SCREEN_PLAYING) {
        play_clock_live = false;
        return;
    }

    uint32_t now = get_time_ms();
    if (!play_clock_live) {
        /* Entering play: start the clock here and run no tick this frame. */
        play_clock_live = true;
        play_last_ms    = now;
        tick_acc        = 0.0f;
        return;
    }

    float dt = (float)(now - play_last_ms) * 0.001f;
    if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;
    play_last_ms = now;
    tick_acc += dt;

    int steps = 0;
    while (tick_acc >= TICK_S && steps < MAX_TICKS_FRAME) {
        game_tick();
        tick_acc -= TICK_S;
        steps++;
        /* Level complete or game over: the rest of the frame's time is not play. */
        if (game.screen != SCREEN_PLAYING) break;
    }
    if (steps >= MAX_TICKS_FRAME) tick_acc = 0.0f;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Drawing
 * ══════════════════════════════════════════════════════════════════════════ */

static void draw_hud(void) {
    char buf[64];
    /* Everything here sits in the header band, between the two pressable
     * buttons, and is anchored to their edges rather than to the screen's. */
    int hud_left  = LAYOUT_MENU_BTN_X + BTN_MENU_WIDTH + 10;
    int hud_right = LAYOUT_EXIT_BTN_X - 10;

    /* Level - next to the menu button */
    snprintf(buf, sizeof(buf), "LV %d", game.level);
    fb_draw_text(&fb, hud_left, SCREEN_SAFE_TOP + 22, buf, HUD_COLOR, 1);
    if (test_mode) {
        fb_draw_text(&fb, hud_left, SCREEN_SAFE_TOP + 12, "TEST", RGB(255, 255, 0), 1);
    }

    /* Score (centred between buttons) */
    snprintf(buf, sizeof(buf), "%d", game.score);
    int sw = (int)strlen(buf) * 12;
    fb_draw_text(&fb, AREA_X + AREA_W / 2 - sw / 2, SCREEN_SAFE_TOP + 20, buf, HUD_COLOR, 2);

    /* Hearts for lives - right-aligned against the exit button. A heart spans
     * hx-4 .. hx+9, so the rightmost one's hx is hud_right - 9. */
    for (int i = 0; i < game.lives && i < 9; i++) {
        int hx = hud_right - 9 - i * 14;
        int hy = SCREEN_SAFE_TOP + 22;
        /* Smaller hearts */
        fb_fill_circle(&fb, hx, hy + 2, 4, HEART_COLOR);
        fb_fill_circle(&fb, hx + 5, hy + 2, 4, HEART_COLOR);
        fb_fill_rect(&fb, hx - 3, hy + 2, 11, 5, HEART_COLOR);
    }

    /* Active effect indicators — one row under the level, still in the header
     * band, so they cannot collide with the bricks below HEADER_BOTTOM. */
    int ex = hud_left;
    int ey = SCREEN_SAFE_TOP + 36;
    if (game.fx.paddle_level != 0) {
        snprintf(buf, sizeof(buf), "PAD%+d", game.fx.paddle_level);
        uint32_t color = game.fx.paddle_level > 0 ? RGB(80, 220, 80) : RGB(255, 60, 60);
        fb_draw_text(&fb, ex, ey, buf, color, 1);
        ex += text_measure_width(buf, 1) + 8;
    }
    if (game.fx.speed_level != 0) {
        snprintf(buf, sizeof(buf), "SPD%+d", game.fx.speed_level);
        uint32_t color = game.fx.speed_level < 0 ? RGB(255, 255, 80) : RGB(0, 200, 255);
        fb_draw_text(&fb, ex, ey, buf, color, 1);
        ex += text_measure_width(buf, 1) + 8;
    }
    if (game.fx.fireball) {
        fb_draw_text(&fb, ex, ey, "FIRE", RGB(255, 140, 0), 1);
    }
}

static void draw_bricks(void) {
    for (int i = 0; i < game.brick_count; i++) {
        Brick *b = &game.bricks[i];
        if (brick_is_destroyed(b)) continue;

        int bx = b->x;
        int by = b->y;

        /* Determine colors: use row-based rainbow for single-hit normal bricks */
        uint32_t ctop = b->color_top;
        uint32_t cbot = b->color_bot;
        if (b->type == BRICK_NORMAL && b->max_health == 1) {
            int ci = b->color_index % NUM_ROW_COLORS;
            ctop = ROW_COLORS[ci][0];
            cbot = ROW_COLORS[ci][1];
        }

        /* Gradient fill */
        fb_fill_rect_gradient(&fb, bx, by, b->w, b->h, ctop, cbot);

        /* Subtle highlight on top edge */
        fb_fill_rect_alpha(&fb, bx + 1, by, b->w - 2, 2,
                           COLOR_WHITE, 60);

        /* Bottom shadow */
        fb_fill_rect_alpha(&fb, bx + 1, by + b->h - 2, b->w - 2, 2,
                           COLOR_BLACK, 80);

        /* Special brick indicators */
        if (b->type == BRICK_INDESTRUCTIBLE) {
            /* Diagonal stripes pattern for indestructible */
            for (int s = 0; s < b->w + b->h; s += 6) {
                int x1 = bx + s;
                int y1 = by;
                int x2 = bx;
                int y2 = by + s;
                if (x1 < bx + b->w && y2 < by + b->h) {
                    fb_draw_line(&fb, x1, y1, x2, y2, RGB(120, 120, 120));
                }
            }
        } else if (b->type == BRICK_BONUS) {
            /* Star/sparkle effect for bonus bricks */
            int cx = bx + b->w / 2;
            int cy = by + b->h / 2;
            fb_draw_line(&fb, cx - 4, cy, cx + 4, cy, COLOR_WHITE);
            fb_draw_line(&fb, cx, cy - 4, cx, cy + 4, COLOR_WHITE);
        } else if (b->type == BRICK_EXPLOSIVE) {
            /* X pattern for explosive bricks */
            fb_draw_line(&fb, bx + 2, by + 2, bx + b->w - 2, by + b->h - 2, RGB(255, 255, 0));
            fb_draw_line(&fb, bx + b->w - 2, by + 2, bx + 2, by + b->h - 2, RGB(255, 255, 0));
        }

        /* Health number for multi-hit bricks */
        if (b->health > 1 && b->health < 100 && b->type == BRICK_NORMAL) {
            char num[8];
            snprintf(num, sizeof(num), "%d", b->health);
            int tw = (int)strlen(num) * 6;
            fb_draw_text(&fb, bx + b->w / 2 - tw / 2,
                         by + b->h / 2 - 3, num, COLOR_WHITE, 1);
        }
    }
}

static void draw_paddle(void) {
    int px = (int)game.paddle_x - game.paddle_w / 2;
    int py = PADDLE_Y;

    /* Glow under paddle */
    fb_fill_rect_alpha(&fb, px + 4, py + PADDLE_HEIGHT,
                       game.paddle_w - 8, 4, PADDLE_GLOW, 80);

    /* Main paddle body — gradient */
    fb_fill_rect_gradient(&fb, px, py, game.paddle_w, PADDLE_HEIGHT,
                          PADDLE_COLOR, PADDLE_GLOW);

    /* Highlight on top */
    fb_fill_rect_alpha(&fb, px + 2, py, game.paddle_w - 4, 2,
                       COLOR_WHITE, 100);
}

static void draw_balls(void) {
    for (int i = 0; i < game.ball_count; i++) {
        Ball *b = &game.balls[i];
        if (!b->active) continue;

        int bx = (int)b->x;
        int by = (int)b->y;
        uint32_t col = b->fireball ? BALL_FIRE : BALL_COLOR;

        /* Fiery trail for fireball balls (drawn BEFORE ball so ball is on top) */
        if (b->fireball && !b->stuck) {
            for (int t = 0; t < TRAIL_LEN; t++) {
                /* Read from oldest to newest in circular buffer.
                 * trail_idx points to next write = oldest entry.
                 * So oldest is at trail_idx, newest is at trail_idx - 1. */
                int idx = (b->trail_idx + t) % TRAIL_LEN;
                float age = (float)t / (float)(TRAIL_LEN - 1);  /* 0=oldest, 1=newest */

                /* Radius: oldest=1, newest=4, linearly interpolated */
                int radius = 1 + (int)(age * 3.0f);

                /* Color gradient: oldest=dark red, newest=bright yellow-orange */
                uint8_t r, g, bb_c;
                if (age < 0.5f) {
                    /* Dark red (180,30,0) -> orange (255,100,0) */
                    float f = age * 2.0f;
                    r = (uint8_t)(180 + f * (255 - 180));
                    g = (uint8_t)(30  + f * (100 - 30));
                    bb_c = 0;
                } else {
                    /* Orange (255,100,0) -> bright yellow-orange (255,200,50) */
                    float f = (age - 0.5f) * 2.0f;
                    r = 255;
                    g = (uint8_t)(100 + f * (200 - 100));
                    bb_c = (uint8_t)(0 + f * 50);
                }

                int tx = (int)b->trail_x[idx];
                int ty = (int)b->trail_y[idx];
                fb_fill_circle(&fb, tx, ty, radius, RGB(r, g, bb_c));
            }
        }

        /* Glow behind ball */
        fb_fill_circle(&fb, bx, by, BALL_RADIUS + 3,
                       b->fireball ? RGB(80, 40, 0) : RGB(40, 40, 60));

        /* Ball body */
        fb_fill_circle(&fb, bx, by, BALL_RADIUS, col);

        /* Specular highlight */
        fb_fill_circle(&fb, bx - 2, by - 2, 2, COLOR_WHITE);
    }
}

static void draw_powerups(void) {
    for (int i = 0; i < MAX_POWERUPS; i++) {
        PowerUp *pu = &game.powerups[i];
        if (!pu->active) continue;

        int px = (int)pu->x - POWERUP_SIZE / 2;
        int py = (int)pu->y - POWERUP_SIZE / 2;

        /* Box with border */
        fb_fill_rounded_rect(&fb, px, py, POWERUP_SIZE, POWERUP_SIZE, 4,
                             pu_info[pu->type].color);
        fb_draw_rounded_rect(&fb, px, py, POWERUP_SIZE, POWERUP_SIZE, 4,
                             COLOR_WHITE);

        /* Letter */
        char sym[2] = { pu_info[pu->type].symbol, 0 };
        fb_draw_text(&fb, px + POWERUP_SIZE / 2 - 3, py + POWERUP_SIZE / 2 - 4,
                     sym, COLOR_WHITE, 1);
    }
}

static void draw_play_area_border(void) {
    /* Subtle border around play area */
    fb_draw_rect(&fb, AREA_X - 1, AREA_Y - 1, AREA_W + 2, AREA_H + 2,
                 RGB(40, 50, 80));
}

static void draw_game_screen(void) {
    fb_clear(&fb, BG_COLOR);

    /* Screen shake offset — applied via framebuffer draw offset */
    fb_set_draw_offset(&fb, (int)game.shake_x, (int)game.shake_y);
    draw_play_area_border();
    draw_bricks();
    draw_paddle();
    draw_balls();
    draw_powerups();
    draw_particles();
    fb_clear_draw_offset(&fb);
    draw_hud();  /* HUD does NOT shake */

    if (demo_on()) {
        /* DEMO label between the buttons' band, under the score; no buttons */
        const char *lbl = "DEMO";
        fb_draw_text(&fb, AREA_X + AREA_W / 2 - text_measure_width(lbl, 1) / 2,
                     SCREEN_SAFE_TOP + 40, lbl, RGB(0, 220, 255), 1);
        return;
    }
    button_draw_menu(&fb, &btn_menu);
    button_draw_exit(&fb, &btn_exit);
    /* No in-game launch hint: the title screen already says how to launch
     * and move, and the operator found the line redundant. */
}

/* ── Overlay screens ─────────────────────────────────────────────────── */

/* The menu subtitle is the best score so far; blank before the first one.
 * Called at init, and again by anything that returns to the menu. */
static void menu_refresh_subtitle(void) {
    char buf[64];
    if (hs.count > 0)
        snprintf(buf, sizeof(buf), "HIGH SCORE: %d", hs.entries[0].score);
    else
        buf[0] = '\0';
    start_menu_set_subtitle(&menu, buf);
}

static void draw_paused(void) {
    draw_game_screen();  /* game as background */
    modal_dialog_draw(&pause_dialog, &fb);
}

static void draw_level_complete(void) {
    draw_game_screen();
    fb_fill_rect_alpha(&fb, 0, 0, fb.width, fb.height, COLOR_BLACK, 140);
    text_draw_centered(&fb, fb.width / 2, fb.height / 2 - 70,
                       "LEVEL COMPLETE!", RGB(0, 255, 100), 4);
    char buf[64];
    snprintf(buf, sizeof(buf), "SCORE: %d", game.score);
    text_draw_centered(&fb, fb.width / 2, fb.height / 2 - 20,
                       buf, COLOR_WHITE, 2);
    snprintf(buf, sizeof(buf), "BONUS: %d", game.lives * 50);
    text_draw_centered(&fb, fb.width / 2, fb.height / 2 + 10,
                       buf, COLOR_YELLOW, 2);
    button_draw(&fb, &btn_next_level);
}

/* draw_game_over() — removed: replaced by unified GameOverScreen component */

/* Leave a run (or the finished game-over flow) for the start menu, undoing what
 * START and play set up so the menu looks as on first launch.  test_mode needs
 * no reset: START re-reads it from the DIFFICULTY choice, which reopen keeps. */
static void return_to_menu(void) {
    modal_dialog_hide(&pause_dialog);
    hw_led_pulse_stop(&fx_pulse);
    hw_leds_off();
    reset_effects();
    apply_paddle_width();
    game.clear_cooldown = 0;
    game.pending_exp_count = 0;
    menu_refresh_subtitle();
    demo_running = false;
    start_menu_reopen(&menu, get_time_ms());
    game.screen = SCREEN_MENU;
}

/* The demo: SCREEN_MENU while the attract cycle is on its DEMO page.  Entering
 * it resets the field to a fresh level 1 at NORMAL (test_mode off); START
 * resets the game again, so nothing of the demo leaks into real play. */
static void demo_sync(void) {
    bool want = (game.screen == SCREEN_MENU &&
                 start_menu_attract(&menu) == SM_ATTRACT_DEMO);
    if (want && !demo_running) {
        test_mode = false;
        demo_running = true;          /* before reset_game(): nothing in it may sound */
        reset_game();
        game.screen = SCREEN_MENU;    /* reset_game() sets PLAYING; the demo stays on the menu screen */
        demo_launch_wait = DEMO_LAUNCH_TICKS;
        demo_last_ms = get_time_ms();
        demo_acc = 0.0f;
        paddle_vel = 0.0f;
    }
    demo_running = want;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Input handling
 * ══════════════════════════════════════════════════════════════════════════ */

static void handle_input(void) {
    touch_poll(&touch);
    TouchState st = touch_get_state(&touch);
    uint32_t now = get_time_ms();
    paddle_vel = 0.0f;   /* re-sampled below, and only while playing */

    /* Poll gamepad/keyboard/mouse through unified API */
    gamepad_poll(&gamepad_mgr, &gp_input, st.x, st.y, st.pressed);

    /* Hot-plug check: rescans only when /dev/input changed or a device went away */
    gamepad_tick(&gamepad_mgr, now);

    /* Start menu — ahead of the BACK rule, so BACK during the SCORES page is
     * swallowed by the widget and on the menu itself arrives as SM_EXIT. */
    if (game.screen == SCREEN_MENU) {
        int r = start_menu_update(&menu, &gp_input, st.x, st.y,
                                  st.pressed || st.held,
                                  &gamepad_mgr, &audio, now);
        if (r == SM_EXIT || r == menu_exit_idx) {
            running = false;
        } else if (r == menu_start_idx) {
            test_mode = (start_menu_value(&menu, menu_diff_idx) == DIFF_EASY);
            demo_running = false;
            reset_game();
            audio_beep(&audio);
        } else {
            demo_sync();
        }
        return;
    }

    /* BTN_BACK leaves the run for the start menu (only the menu's EXIT quits);
     * not on GAME OVER, whose flow owns its input — BACK must not lose a name. */
    if (gp_input.buttons[BTN_ID_BACK].pressed && game.screen != SCREEN_GAME_OVER) {
        return_to_menu();
        return;
    }

    switch (game.screen) {
    case SCREEN_MENU:
        break;   /* handled above */

    case SCREEN_PLAYING:
        /* Gamepad/keyboard: pause */
        if (gp_input.buttons[BTN_ID_PAUSE].pressed) {
            game.screen = SCREEN_PAUSED;
            modal_dialog_set_focus(&pause_dialog, 0);
            modal_dialog_show(&pause_dialog);
            return;
        }

        /* Exit (touch) */
        if (st.pressed) {
            if (button_check_press(&btn_exit, button_is_touched(&btn_exit, st.x, st.y), now)) {
                return_to_menu();
                return;
            }
            /* Menu → pause */
            if (button_check_press(&btn_menu, button_is_touched(&btn_menu, st.x, st.y), now)) {
                game.screen = SCREEN_PAUSED;
                modal_dialog_set_focus(&pause_dialog, 0);
                modal_dialog_show(&pause_dialog);
                return;
            }
        }

        /* ── Paddle movement input handling ──────────────────────────────
         * Multiple input methods coexist without fighting each other:
         *
         *   1. Touch:   absolute positioning — finger position = paddle pos
         *   2. Mouse:   delta-based — mouse movement adds to paddle position
         *   3. Analog:  velocity-based (axis_lx ±1000 → proportional speed)
         *   4. D-pad:   velocity-based (fixed PADDLE_KB_SPEED per tick)
         *
         * Only touch sets the paddle position absolutely.  All other inputs
         * add a delta (mouse, each frame) or a velocity (analog stick and
         * d-pad/keyboard, each tick), so they never fight each other or cause the paddle to
         * snap to an unexpected position when switching input methods.
         * ────────────────────────────────────────────────────────────── */

        /* Touch: absolute positioning (only when actively touching the screen) */
        if (st.held || st.pressed) {
            game.paddle_x = clampf((float)st.x,
                                   AREA_X + game.paddle_w / 2.0f,
                                   AREA_X + AREA_W - game.paddle_w / 2.0f);
        }

        /* Mouse: delta-based movement — track previous position and apply
         * the difference as velocity.  This prevents the paddle from
         * snapping to an absolute screen position when switching between
         * mouse and gamepad.  On first connection (prev == -1), we just
         * store the position without moving the paddle. */
        {
            static int prev_mouse_x = -1;
            if (gp_input.mouse_connected) {
                if (prev_mouse_x >= 0 && gp_input.mouse_x != prev_mouse_x) {
                    int delta = gp_input.mouse_x - prev_mouse_x;
                    game.paddle_x += (float)delta;
                    game.paddle_x = clampf(game.paddle_x,
                                           AREA_X + game.paddle_w / 2.0f,
                                           AREA_X + AREA_W - game.paddle_w / 2.0f);
                }
                prev_mouse_x = gp_input.mouse_x;
            } else {
                prev_mouse_x = -1;  /* Reset when mouse disconnects */
            }
            /* Mouse click to launch ball */
            if (gp_input.mouse_connected &&
                gp_input.mouse_left_pressed && !game.ball_launched) {
                launch_ball();
            }
        }

        /* Gamepad analog stick and d-pad/keyboard: velocity, not position.
         * axis_lx is normalized to ±1000 (0 when in dead zone), scaled up to
         * PADDLE_MAX_ANALOG px/tick; a held LEFT/RIGHT adds PADDLE_KB_SPEED
         * px/tick.  Only recorded here — game_tick() moves the paddle by it
         * once per tick, so the speed follows elapsed time, not frame count. */
        if (gp_input.axis_lx != 0)
            paddle_vel += (gp_input.axis_lx / 1000.0f) * PADDLE_MAX_ANALOG;
        if (gp_input.buttons[BTN_ID_LEFT].held)
            paddle_vel -= PADDLE_KB_SPEED;
        if (gp_input.buttons[BTN_ID_RIGHT].held)
            paddle_vel += PADDLE_KB_SPEED;

        /* Launch on tap or gamepad button */
        if (st.pressed && !game.ball_launched) {
            launch_ball();
        }
        if ((gp_input.buttons[BTN_ID_JUMP].pressed ||
             gp_input.buttons[BTN_ID_ACTION].pressed) && !game.ball_launched) {
            launch_ball();
        }
        break;

    case SCREEN_PAUSED: {
        /* Pad / keyboard: focus frame, A presses it, Start / Esc = RESUME */
        ModalDialogAction action = modal_dialog_input(&pause_dialog, &gp_input, 0);
        if (action == MODAL_ACTION_NONE)
            action = modal_dialog_update(&pause_dialog, st.x, st.y, st.pressed, now);
        if (action == MODAL_ACTION_BTN0) {
            /* Resume */
            game.screen = SCREEN_PLAYING;
            audio_beep(&audio);
        }
        if (action == MODAL_ACTION_BTN1) {
            /* Retire — trigger game over */
            game.screen = SCREEN_GAME_OVER;
            {
                char info[64];
                snprintf(info, sizeof(info), "LEVEL %d", game.level);
                gameover_init(&gos, &fb, game.score, NULL, info,
                              "BRICK BREAKER", &hs, &touch, &gamepad_mgr);
            }
            audio_gameover(&audio);   /* retiring ends the RUN — see the lost-ball site */
        }
        if (action == MODAL_ACTION_BTN2) {
            /* Exit: back to the start menu */
            return_to_menu();
        }
        break;
    }

    case SCREEN_LEVEL_COMPLETE:
        if (st.pressed) {
            if (button_check_press(&btn_next_level, button_is_touched(&btn_next_level, st.x, st.y), now)) {
                if (game.level < MAX_LEVELS) {
                    game.level++;
                    init_level();
                    game.screen = SCREEN_PLAYING;
                } else {
                    /* Beat all levels! */
                    game.screen = SCREEN_GAME_OVER;
                    {
                        char info[64];
                        snprintf(info, sizeof(info), "ALL %d LEVELS COMPLETE!", MAX_LEVELS);
                        gameover_init(&gos, &fb, game.score, "YOU WIN!", info,
                                      "BRICK BREAKER", &hs, &touch, &gamepad_mgr);
                    }
                }
                audio_beep(&audio);
            }
        }
        /* Gamepad: advance to next level with Jump/Action */
        if (gp_input.buttons[BTN_ID_JUMP].pressed ||
            gp_input.buttons[BTN_ID_ACTION].pressed) {
            if (game.level < MAX_LEVELS) {
                game.level++;
                init_level();
                game.screen = SCREEN_PLAYING;
            } else {
                game.screen = SCREEN_GAME_OVER;
                {
                    char info[64];
                    snprintf(info, sizeof(info), "ALL %d LEVELS COMPLETE!", MAX_LEVELS);
                    gameover_init(&gos, &fb, game.score, "YOU WIN!", info,
                              "BRICK BREAKER", &hs, &touch, &gamepad_mgr);
                }
            }
            audio_beep(&audio);
        }
        break;

    case SCREEN_GAME_OVER:
        break;   /* the game-over component reads its own input in the draw path */
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* Line-buffer stdout FIRST: at boot it is /var/log/arca/app_stdout.log,
     * not a tty, so glibc block-buffers 4 KB and audio_bed_init()'s playlist
     * receipt never arrives — which reads as a printf that was never reached.
     * common/logger.c line-buffers its own file, which is why only the printf
     * lines go missing (../CLAUDE.md → App lifecycle). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *fb_device = "/dev/fb0";
    const char *touch_device = "/dev/input/touchscreen0";

    /* Check for --test flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            test_mode = true;
            printf("TEST MODE ENABLED\n");
        }
    }

    if (argc > 1) fb_device = argv[1];
    if (argc > 2) touch_device = argv[2];

    // Singleton guard — prevent duplicate instances
    int lock_fd = acquire_instance_lock("brick_breaker");
    if (lock_fd < 0) {
        fprintf(stderr, "brick_breaker: another instance is already running\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Pin 32bpp — /dev/fb0 keeps whatever ran last (see fb_set_bpp). */
    fb_set_bpp(fb_device, 32);

    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, "Failed to initialise framebuffer\n");
        return 1;
    }
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, "Failed to initialise touch input\n");
        fb_close(&fb);
        return 1;
    }

    hw_init();
    hw_set_backlight(100);
    audio_init(&audio);

    /* The music bed: the playlist named by /opt/arca/soundsets/brick_breaker.sound,
     * with the four states and the hold/resume rules in common/audio_bed.c.
     * ⚠️ That file is the ONLY home for the paths — no set file means no music,
     * and brick_breaker_music present but EMPTY is the off switch. */
    AudioBed bed;
    audio_bed_init(&bed, &audio, "brick_breaker");
    srand(time(NULL));

    /* Gamepad init */
    gamepad_init(&gamepad_mgr);
    gamepad_set_mouse_bounds(&gamepad_mgr, fb.width, fb.height);

    /* Compute brick grid dimensions based on screen orientation */
    init_brick_layout();

    /* Calculate ball base speed based on paddle-to-brick distance.
     * distance / 150 px per tick crosses the gap straight up in 150 ticks
     * (5 s at 30 ticks/s), clamped to 4..8 px/tick (120..240 px/s).  The
     * 4.0 floor wins for any gap under 600 px — every landscape layout
     * (~200 px gap, ~50 ticks = ~1.7 s) — so only a taller portrait gap
     * raises the base. */
    {
        int lowest_brick_y = BRICK_TOP + BRICK_PAD  /* as create_bricks() */
                           + brick_rows * (BRICK_H + BRICK_PAD);
        int paddle_y = PADDLE_Y;
        int distance = paddle_y - lowest_brick_y;
        if (distance < 50) distance = 50;  /* safety floor */
        float speed = (float)distance / 150.0f;
        ball_base_speed = clampf(speed, 4.0f, 8.0f);
        printf("Ball base speed: %.2f (distance=%d, portrait=%d)\n",
               ball_base_speed, distance, fb.portrait_mode);
    }

    /* High score */
    hs_init(&hs, "brick_breaker");
    hs_load(&hs);

    /* UI buttons */
    button_init(&btn_menu, LAYOUT_MENU_BTN_X, LAYOUT_MENU_BTN_Y,
                BTN_MENU_WIDTH, BTN_MENU_HEIGHT, "",
                BTN_MENU_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&btn_exit, LAYOUT_EXIT_BTN_X, LAYOUT_EXIT_BTN_Y,
                BTN_EXIT_WIDTH, BTN_EXIT_HEIGHT, "",
                BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&btn_restart, fb.width / 2 - BTN_LARGE_WIDTH / 2,
                SCREEN_SAFE_BOTTOM - BTN_LARGE_HEIGHT - 30,
                BTN_LARGE_WIDTH, BTN_LARGE_HEIGHT, "PLAY AGAIN",
                BTN_RESTART_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&btn_next_level, fb.width / 2 - BTN_LARGE_WIDTH / 2,
                fb.height / 2 + 60, BTN_LARGE_WIDTH, BTN_LARGE_HEIGHT, "NEXT LEVEL",
                BTN_START_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);

    /* Pause dialog — 3-button modal */
    modal_dialog_init(&pause_dialog, "PAUSED", NULL, 3);
    modal_dialog_set_button(&pause_dialog, 0, "RESUME", BTN_COLOR_PRIMARY, COLOR_WHITE);
    modal_dialog_set_button(&pause_dialog, 1, "RETIRE", BTN_COLOR_WARNING, COLOR_WHITE);
    modal_dialog_set_button(&pause_dialog, 2, "EXIT", BTN_COLOR_DANGER, COLOR_WHITE);

    /* Start menu: no DEMO yet, so the attract cycle is MENU -> SCORES.
     * --test preselects EASY. */
    start_menu_init(&menu, "BRICK BREAKER",
                    "DRAG TO MOVE PADDLE\n"
                    "TAP TO LAUNCH BALL\n"
                    "CATCH POWER-UPS!", get_time_ms());
    menu_refresh_subtitle();
    menu_start_idx = start_menu_add_action(&menu, "START");
    menu_diff_idx  = start_menu_add_choice(&menu, "DIFFICULTY", DIFF_NAMES, DIFF_COUNT,
                                           test_mode ? DIFF_EASY : DIFF_NORMAL);
    menu_exit_idx  = start_menu_add_action(&menu, "EXIT");
    start_menu_select(&menu, menu_start_idx, get_time_ms());
    start_menu_set_attract(&menu, true);

    game.screen = SCREEN_MENU;
    frame_time_ms = get_time_ms();

    printf("Brick Breaker started!\n");

    /* ── main loop ──────────────────────────────────────────────────── */
    bool needs_redraw = true;
    while (running) {
        frame_time_ms = get_time_ms();

        GameScreen prev_screen = game.screen;
        handle_input();
        update_game();
        hw_led_pulse_update(&fx_pulse, get_time_ms());

        /* Dirty-flag: active gameplay always redraws; static screens on change */
        if (game.screen == SCREEN_PLAYING || demo_on()) {
            needs_redraw = true;  /* continuous rendering during play; the demo is gameplay */
        } else if (game.screen != prev_screen) {
            needs_redraw = true;  /* screen transition */
        } else {
            /* Static screens: redraw only on input activity */
            TouchState ts = touch_get_state(&touch);
            if (ts.pressed || ts.held) needs_redraw = true;
            for (int i = 0; i < BTN_ID_COUNT; i++) {
                if (gp_input.buttons[i].pressed) { needs_redraw = true; break; }
            }
        }

        /* The game-over component runs a multi-frame state machine (highscore
         * check, blocking name entry) and only draws once it reaches DISPLAY —
         * give it frames until it says it is settled, or the overlay never
         * appears without a tap. */
        if (game.screen == SCREEN_GAME_OVER && gameover_needs_redraw(&gos, &gp_input))
            needs_redraw = true;

        /* The start menu's blink and attract phase change with no input. */
        if (game.screen == SCREEN_MENU && start_menu_needs_redraw(&menu))
            needs_redraw = true;

        /* ⚠️ The bed is serviced ABOVE the redraw block, and the ORDER is
         * load-bearing rather than tidiness.  SCREEN_GAME_OVER's redraw calls
         * gameover_update(), whose name entry is a BLOCKING sub-loop that owns
         * the screen for as long as the player types — so a bed serviced *after*
         * the block never sees the transition and stays PLAYING for the whole
         * keyboard session, reported as "the end of game keeps playing the music
         * over the keyboard".  `want_play` was already SCREEN_PLAYING alone;
         * only the position was wrong.  Servicing first releases the voice, and
         * the sub-loop's own ui_frame_service() pumps the release envelope out
         * while it runs — so the music fades instead of cutting.  It stays
         * BEFORE audio_pump() for the original reason: a voice started on this
         * iteration is fed on the same one. */
        /* The ball parked on the paddle HOLDS the bed, which is `platformer`'s
         * death rule with this game's spelling for it (operator, 2026-08-23).
         * ⚠️ The term is in BOTH predicates, exactly as platformer's `dying` is:
         * clearing `want_play` is what releases the voice, and setting
         * `want_hold` is what keeps the FILE open at its read position — so the
         * relaunch RESUMES the same track mid-bar instead of restarting the
         * level's music, and the playlist cursor does not advance.  It also
         * covers the pre-launch wait at a level's start, so the bed begins when
         * play does rather than over a stationary ball. */
        bool bed_waiting = !game.ball_launched;
        audio_bed_service(&bed, game.screen == SCREEN_PLAYING && !bed_waiting,
                                game.screen == SCREEN_PAUSED || bed_waiting);

        if (needs_redraw) {
            switch (game.screen) {
            case SCREEN_MENU:
                if (start_menu_attract(&menu) == SM_ATTRACT_MENU)
                    start_menu_draw(&menu, &fb);
                else if (demo_on())
                    draw_game_screen();   /* the DEMO page: the real field, drawn by the real code */
                else
                    start_menu_draw_scores(&menu, &fb, &hs);
                break;
            case SCREEN_PLAYING:        draw_game_screen();    break;
            case SCREEN_PAUSED:         draw_paused();         break;
            case SCREEN_LEVEL_COMPLETE: draw_level_complete(); break;
            case SCREEN_GAME_OVER: {
                /* Draw game field as background (visible through semi-transparent overlay) */
                draw_game_screen();
                /* Update and draw the unified game over screen */
                TouchState go_st = touch_get_state(&touch);
                GameOverAction action = gameover_update(&gos, &fb,
                                                         go_st.x, go_st.y, go_st.pressed,
                                                         &gp_input);
                if (action == GAMEOVER_ACTION_MENU)
                    return_to_menu();
                break;
            }
            }
            fb_swap(&fb);
        }

        /* Service the stream on EVERY iteration, drawing or not — it holds one
         * lead (~139 ms on the OSS shim) and a skipped service is an audible
         * gap.  ⚠️ audio_pump_active() belongs in the pacing decision: it is
         * unconditionally true while the continuous stream is live, and
         * FRAME_DELAY_IDLE_US (100 ms) is well above the ~55 ms service ceiling
         * the library measures for itself (common/audio_out.h). */
        audio_pump(&audio);
        usleep((needs_redraw || audio_pump_active(&audio))
               ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
        needs_redraw = false;  /* reset for next frame */
    }

    /* Clean up */
    gamepad_close(&gamepad_mgr);
    touch_close(&touch);
    hw_leds_off();
    hw_set_backlight(100);
    audio_close(&audio);
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    fb_close(&fb);

    printf("Brick Breaker ended.\n");
    return 0;
}
