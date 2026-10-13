/*
 * Pong Game - Native C Implementation
 * One player vs AI, or two players (P2 on the right paddle)
 * Touch, keyboard, gamepad, and mouse input supported
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
#include "../common/audio.h"
#include "../common/audio_bed.h"
#include "../common/gamepad.h"
#include "../common/start_menu.h"

#define PADDLE_WIDTH 15
#define PADDLE_HEIGHT 80
#define BALL_SIZE 12
#define WINNING_SCORE 11
/* Motion runs on elapsed time (frame_dt, seconds), not frames: every speed below
 * is px per SECOND.  They were authored as px/frame at the nominal 30 fps and
 * are that figure x 30, so the feel at 30 fps is unchanged. */
#define PADDLE_SPEED 180.0f      /* keyboard/d-pad; authored 6 px/frame */
#define PADDLE_MAX_ANALOG 240.0f /* analog stick at full deflection; authored 8 px/frame */
#define BALL_START_SPEED 255.0f  /* authored 8.5 px/frame; 5.0 took ~7s to cross the playfield */
#define BALL_SPEEDUP 1.05f       /* per paddle hit, on the primary axis */
#define BALL_ENGLISH 90.0f       /* cross-axis px/s added per hit at a paddle end; authored 3 px/frame */
#define AI_SPEED_BASE 90.0f      /* AI paddle px/s = AI_SPEED_BASE + AI_SPEED_PER_LEVEL * difficulty; */
#define AI_SPEED_PER_LEVEL 30.0f /*   authored 3 + difficulty px/frame */
#define AI_JITTER_MS 33.3f       /* aim-error re-roll period; it was re-rolled every 30 fps frame */
#define MAX_DT 0.1f              /* longest step one frame may integrate, seconds */
/* The paddle test is a position test with no sweep, and the ball (12 px) and
 * paddle (15 px) are thin: no single integration step may move the ball further
 * than this, or it passes through a paddle at a low frame rate. */
#define BALL_MAX_SUBSTEP_PX 6.0f
#define BALL_MAX_SUBSTEPS 32

typedef enum {
    SCREEN_MENU,
    SCREEN_PLAYING,
    SCREEN_PAUSED,
    SCREEN_GAME_OVER
} GameScreen;

typedef struct {
    float x, y;
    float vx, vy;
} Ball;

typedef struct {
    float y;
    int score;
} Paddle;

typedef struct {
    Paddle player;
    Paddle ai;
    Ball ball;
    bool game_over;
    bool paused;
    int winner;  // 0 = none, 1 = player, 2 = AI
    int difficulty;  // 1 = easy, 2 = medium, 3 = hard
} GameState;

// Global variables
Framebuffer fb;
TouchInput touch;
GamepadManager gamepad;
InputState input;
GameState game;
bool running = true;
Audio audio;
int play_area_width;
int play_area_height;
int offset_x;
int offset_y;
Button menu_button;
Button exit_button;
ModalDialog pause_dialog;
GameScreen current_screen = SCREEN_MENU;

// Start menu (../common/start_menu.h): START, PLAYERS, DIFFICULTY, EXIT
static StartMenu menu;
static int menu_start_idx, menu_players_idx, menu_diff_idx, menu_exit_idx;
static const char *const DIFF_NAMES[3] = { "EASY", "NORMAL", "HARD" };
static bool two_player = false;     // P2 drives the right paddle instead of the AI
static bool demo_running = false;   // attract-cycle DEMO: both paddles AI, silent, no score kept
#define DEMO_POINTS 5               // the demo ends early at this many points in total
static bool demo_on(void) { return demo_running; }

static const char *player_label(void) { return demo_running ? "AI" : two_player ? "P1" : "YOU"; }
static const char *ai_label(void)     { return demo_running ? "AI" : two_player ? "P2" : "AI"; }
bool portrait_mode = false;
static HighScoreTable hs_table;
static GameOverScreen gos;
/* LED flourishes (game start, match won), advanced once per frame by the main loop. */
static LedPulse led_pulse;

/* The play clock.  handle_input() measures frame_dt once per main-loop
 * iteration; every transition into play calls play_clock_restart(), so the
 * first frame of play does not integrate the time spent on a menu, a pause or a
 * blocking name entry. */
static uint32_t last_ms;
static float frame_dt;
static float ai_jitter[2];      /* current aim error per AI paddle (0 = left, 1 = right), px */
static float ai_jitter_ms[2];   /* time since it was last re-rolled */

static void play_clock_restart(void) {
    last_ms = get_time_ms();
    frame_dt = 0.0f;
}

// Function prototypes
void init_game();
void reset_game();
void reset_ball();
void update_game();
static void update_ai_paddle(Paddle *pad, int who, float dt);
void draw_game();
void handle_input();
void signal_handler(int sig);
static void enter_game_over(void);

void signal_handler(int sig) {
    running = false;
}

void init_game() {
    portrait_mode = fb.portrait_mode;
    
    play_area_width = fb.width - 40;
    offset_x = 20;
    // The play area starts below the MENU/EXIT row, which is SCREEN_SAFE_*-anchored,
    // so it must move with the measured touch inset rather than sit at a literal 80.
    // Identical to the old 80 / fb.height - 120 when the inset is 0.
    offset_y = LAYOUT_MENU_BTN_Y + BTN_MENU_HEIGHT + 20;
    play_area_height = SCREEN_VISIBLE_BOTTOM - offset_y - 40;
    
    game.difficulty = 2;
    
    // Initialize highscore
    hs_init(&hs_table, "pong");
    hs_load(&hs_table);
    
    // Initialize buttons — LAYOUT_* is SCREEN_SAFE_*-anchored, so the row moves
    // down with the measured touch inset instead of losing its top rows to it.
    button_init(&menu_button, LAYOUT_MENU_BTN_X, LAYOUT_MENU_BTN_Y,
                BTN_MENU_WIDTH, BTN_MENU_HEIGHT, "",
                BTN_MENU_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&exit_button, LAYOUT_EXIT_BTN_X, LAYOUT_EXIT_BTN_Y,
                BTN_EXIT_WIDTH, BTN_EXIT_HEIGHT, "",
                BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    start_menu_init(&menu, "PONG",
                    "TOUCH OR D-PAD: MOVE PADDLE\n"
                    "FIRST TO 11 WINS", get_time_ms());
    menu_start_idx   = start_menu_add_action(&menu, "START");
    menu_players_idx = start_menu_players(&menu);
    menu_diff_idx    = start_menu_add_choice(&menu, "DIFFICULTY", DIFF_NAMES, 3, 1);
    menu_exit_idx    = start_menu_add_action(&menu, "EXIT");
    start_menu_select(&menu, menu_start_idx, get_time_ms());
    start_menu_set_attract(&menu, true);
    modal_dialog_init(&pause_dialog, "PAUSED", NULL, 2);
    modal_dialog_set_button(&pause_dialog, 0, "RESUME", BTN_COLOR_PRIMARY, COLOR_WHITE);
    modal_dialog_set_button(&pause_dialog, 1, "EXIT", BTN_COLOR_DANGER, COLOR_WHITE);
    
    reset_game();
}

void reset_game() {
    if (portrait_mode) {
        // Paddles move horizontally in portrait — use .y field for X position
        game.player.y = play_area_width / 2 - PADDLE_HEIGHT / 2;
        game.ai.y = play_area_width / 2 - PADDLE_HEIGHT / 2;
    } else {
        game.player.y = play_area_height / 2 - PADDLE_HEIGHT / 2;
        game.ai.y = play_area_height / 2 - PADDLE_HEIGHT / 2;
    }
    game.player.score = 0;
    game.ai.score = 0;
    game.game_over = false;
    game.paused = false;
    game.winner = 0;
    /* The win flourish outlives the game-over screen now that it does not block,
     * so cancel it rather than flash into the new match. */
    hw_led_pulse_stop(&led_pulse);

    reset_ball();
}

void reset_ball() {
    game.ball.x = play_area_width / 2;
    game.ball.y = play_area_height / 2;
    
    float angle = ((rand() % 90) - 45) * M_PI / 180.0;
    float speed = BALL_START_SPEED;
    
    if (portrait_mode) {
        // Ball moves primarily up/down in portrait
        int direction = (rand() % 2) ? 1 : -1;
        game.ball.vx = sin(angle) * speed;      // Small horizontal component
        game.ball.vy = cos(angle) * speed * direction;  // Primary vertical movement
    } else {
        // Ball moves primarily left/right in landscape
        int direction = (rand() % 2) ? 1 : -1;
        game.ball.vx = cos(angle) * speed * direction;
        game.ball.vy = sin(angle) * speed;
    }
}

// Helper: initialize the unified game over screen when a match ends
//
// The win flourish lives here rather than at the four call sites, all of which
// followed this call with the same 3 x 200 ms on/off usleep() loop plus a sound.
// Two reasons: that loop ran inside update_game() and froze the panel for 1.2 s
// with no touch poll and no redraw — before the game-over screen had been drawn,
// so taps made during it were discarded — and the
// LED colour and the sound both follow game.winner, which is decided here.
static void enter_game_over(void) {
    char info[64];
    bool player_won = (game.winner == 1);
    if (two_player) {
        /* No high score for a head-to-head match: a NULL table skips the check. */
        snprintf(info, sizeof(info), "%d - %d", game.player.score, game.ai.score);
        gameover_init(&gos, &fb, game.player.score,
                      player_won ? "P1 WINS!" : "P2 WINS!", info, "PONG",
                      NULL, &touch, &gamepad);
    } else {
        if (player_won)
            snprintf(info, sizeof(info), "YOU WIN! %d - %d", game.player.score, game.ai.score);
        else
            snprintf(info, sizeof(info), "AI WINS %d - %d", game.ai.score, game.player.score);
        gameover_init(&gos, &fb, game.player.score,
                      player_won ? "YOU WIN!" : "AI WINS!", info, "PONG",
                      &hs_table, &touch, &gamepad);
    }

    hw_led_pulse_start(&led_pulse, player_won ? LED_GREEN : LED_RED,
                       3, 200, get_time_ms());
    if (player_won)
        audio_success(&audio);
    else
        /* ⚠️ gameover, not fail: this runs when the MATCH is decided, not when a
         * point is conceded — pong scores no sound for a lost rally at all, so
         * audio_fail() (the lost-a-LIFE sound, ../common/audio.h's enum) was
         * never the right name here.  INSTEAD of fail, never after: fx_gameover
         * is 1.19 s against fail's 350 ms and the two would sum on the bus. */
        audio_gameover(&audio);
}

static void update_ai_paddle(Paddle *pad, int who, float dt) {
    float step = (AI_SPEED_BASE + AI_SPEED_PER_LEVEL * game.difficulty) * dt;
    float target;
    float max_pos;
    
    if (portrait_mode) {
        // AI tracks ball X position (horizontal)
        target = game.ball.x - PADDLE_HEIGHT / 2;
        max_pos = play_area_width - PADDLE_HEIGHT;
    } else {
        // AI tracks ball Y position (vertical)
        target = game.ball.y - PADDLE_HEIGHT / 2;
        max_pos = play_area_height - PADDLE_HEIGHT;
    }
    
    /* The aim error is re-rolled on a timer, not per frame, so how much the AI
     * wobbles does not depend on the frame rate. */
    ai_jitter_ms[who] += dt * 1000.0f;
    if (ai_jitter_ms[who] >= AI_JITTER_MS) {
        ai_jitter_ms[who] = 0.0f;
        ai_jitter[who] = (game.difficulty < 3) ? (float)((rand() % 20) - 10) : 0.0f;
    }
    if (game.difficulty < 3) {
        target += ai_jitter[who];
    }

    /* Never step past the target: at a low frame rate one step can exceed the
     * +-5 px deadband and the paddle would oscillate around it. */
    if (pad->y < target - 5) {
        pad->y += fminf(step, target - pad->y);
    } else if (pad->y > target + 5) {
        pad->y -= fminf(step, pad->y - target);
    }
    
    if (pad->y < 0) pad->y = 0;
    if (pad->y > max_pos) pad->y = max_pos;
}

/* Game effects.  The demo runs silent and dark, and is never a match: it does
 * not end at WINNING_SCORE (update_game() ends it at DEMO_POINTS). */
static void sfx_tone(int hz, int ms) { if (!demo_on()) audio_tone(&audio, hz, ms); }
static void sfx_blip(void)           { if (!demo_on()) audio_blip(&audio); }
static void sfx_led(LEDColor c)      { if (!demo_on()) hw_set_led(c, 100); }
static bool match_won(int score)     { return !demo_on() && score >= WINNING_SCORE; }

/* One integration substep of sdt seconds.  Returns true when the ball hit a
 * paddle or a point was scored: the caller stops substepping for this frame, so
 * the LED and sound of a hit fire once per hit, and nothing integrates a ball
 * reset_ball() just served.  *wall_sounded caps the wall tone at one per frame. */
static bool ball_step(float sdt, bool *wall_sounded) {
    game.ball.x += game.ball.vx * sdt;
    game.ball.y += game.ball.vy * sdt;

    if (portrait_mode) {
        // === PORTRAIT MODE COLLISIONS ===
        
        // Ball collision with left/right walls (bounce)
        if (game.ball.x <= 0 || game.ball.x >= play_area_width - BALL_SIZE) {
            game.ball.vx = -game.ball.vx;
            game.ball.x = (game.ball.x <= 0) ? 0 : play_area_width - BALL_SIZE;
            /* ⚠️ No audio_interrupt() before an effect — on the mix bus it means
             * "stop ALL voices", so a 60 ms bounce discards a fanfare that is
             * still playing.  All six sites in this file dropped theirs; the
             * rule and the measurement that produced it (brick_breaker by ear,
             * `.188` 2026-08-20) are in ../CLAUDE.md → Mixing.  No counter sees
             * this — a voice stopped early is not `lost`, `drop` or `clip`. */
            if (!*wall_sounded) {
                sfx_tone(2000, 60);
                *wall_sounded = true;
            }
        }
        
        // Ball collision with player paddle (bottom)
        if (game.ball.y + BALL_SIZE >= play_area_height - PADDLE_WIDTH &&
            game.ball.x + BALL_SIZE >= game.player.y &&
            game.ball.x <= game.player.y + PADDLE_HEIGHT) {
            
            game.ball.vy = -game.ball.vy * BALL_SPEEDUP;
            game.ball.y = play_area_height - PADDLE_WIDTH - BALL_SIZE;
            
            float hit_pos = (game.ball.x + BALL_SIZE / 2 - game.player.y) / PADDLE_HEIGHT;
            game.ball.vx += (hit_pos - 0.5f) * BALL_ENGLISH;
            
            sfx_led(LED_GREEN);
            sfx_tone(440, 90);
            hw_leds_off();
            return true;
        }
        
        // Ball collision with AI paddle (top)
        if (game.ball.y <= PADDLE_WIDTH &&
            game.ball.x + BALL_SIZE >= game.ai.y &&
            game.ball.x <= game.ai.y + PADDLE_HEIGHT) {
            
            game.ball.vy = -game.ball.vy * BALL_SPEEDUP;
            game.ball.y = PADDLE_WIDTH;
            
            float hit_pos = (game.ball.x + BALL_SIZE / 2 - game.ai.y) / PADDLE_HEIGHT;
            game.ball.vx += (hit_pos - 0.5f) * BALL_ENGLISH;
            
            sfx_led(LED_RED);
            sfx_tone(440, 90);
            hw_leds_off();
            return true;
        }
        
        // Ball out top = player scores
        if (game.ball.y < 0) {
            game.player.score++;
            sfx_led(LED_GREEN);
            sfx_blip();
            hw_leds_off();
            
            if (match_won(game.player.score)) {
                game.game_over = true;
                game.winner = 1;
                current_screen = SCREEN_GAME_OVER;
                enter_game_over();   /* also starts the win pulse + sound */
            } else {
                reset_ball();
            }
            return true;
        }
        // Ball out bottom = AI scores
        else if (game.ball.y > play_area_height) {
            game.ai.score++;
            sfx_led(LED_RED);
            sfx_blip();
            hw_leds_off();

            if (match_won(game.ai.score)) {
                game.game_over = true;
                game.winner = 2;
                current_screen = SCREEN_GAME_OVER;
                enter_game_over();   /* also starts the win pulse + sound */
            } else {
                reset_ball();
            }
            return true;
        }
        
    } else {
        // === LANDSCAPE MODE COLLISIONS (existing code, unchanged) ===
        
        // Ball collision with top/bottom walls
        if (game.ball.y <= 0 || game.ball.y >= play_area_height - BALL_SIZE) {
            game.ball.vy = -game.ball.vy;
            game.ball.y = (game.ball.y <= 0) ? 0 : play_area_height - BALL_SIZE;
            if (!*wall_sounded) {
                sfx_tone(2000, 60);
                *wall_sounded = true;
            }
        }
        
        // Ball collision with player paddle (left)
        if (game.ball.x <= PADDLE_WIDTH &&
            game.ball.y + BALL_SIZE >= game.player.y &&
            game.ball.y <= game.player.y + PADDLE_HEIGHT) {
            
            game.ball.vx = -game.ball.vx * BALL_SPEEDUP;
            game.ball.x = PADDLE_WIDTH;
            
            float hit_pos = (game.ball.y + BALL_SIZE / 2 - game.player.y) / PADDLE_HEIGHT;
            game.ball.vy += (hit_pos - 0.5f) * BALL_ENGLISH;
            
            sfx_led(LED_GREEN);
            sfx_tone(440, 90);
            hw_leds_off();
            return true;
        }
        
        // Ball collision with AI paddle (right)
        if (game.ball.x + BALL_SIZE >= play_area_width - PADDLE_WIDTH &&
            game.ball.y + BALL_SIZE >= game.ai.y &&
            game.ball.y <= game.ai.y + PADDLE_HEIGHT) {
            
            game.ball.vx = -game.ball.vx * BALL_SPEEDUP;
            game.ball.x = play_area_width - PADDLE_WIDTH - BALL_SIZE;
            
            float hit_pos = (game.ball.y + BALL_SIZE / 2 - game.ai.y) / PADDLE_HEIGHT;
            game.ball.vy += (hit_pos - 0.5f) * BALL_ENGLISH;
            
            sfx_led(LED_RED);
            sfx_tone(440, 90);
            hw_leds_off();
            return true;
        }
        
        // Ball out of bounds - score
        if (game.ball.x < 0) {
            game.ai.score++;
            sfx_led(LED_RED);
            sfx_blip();
            hw_leds_off();
            
            if (match_won(game.ai.score)) {
                game.game_over = true;
                game.winner = 2;
                current_screen = SCREEN_GAME_OVER;
                enter_game_over();   /* also starts the win pulse + sound */
            } else {
                reset_ball();
            }
            return true;
        } else if (game.ball.x > play_area_width) {
            game.player.score++;
            sfx_led(LED_GREEN);
            sfx_blip();
            hw_leds_off();

            if (match_won(game.player.score)) {
                game.game_over = true;
                game.winner = 1;
                current_screen = SCREEN_GAME_OVER;
                enter_game_over();   /* also starts the win pulse + sound */
            } else {
                reset_ball();
            }
            return true;
        }
    }
    return false;
}

void update_game() {
    if (current_screen != SCREEN_PLAYING && !demo_on()) return;
    if (game.game_over || game.paused) return;

    float dt = frame_dt;

    /* Substep so no step moves the ball more than BALL_MAX_SUBSTEP_PX.  The
     * count is capped; at MAX_DT the cap still holds the limit up to
     * 6 * 32 / 0.1 = 1920 px/s, far past any rally's speed-up. */
    float travel = fmaxf(fabsf(game.ball.vx), fabsf(game.ball.vy)) * dt;
    int n = (int)ceilf(travel / BALL_MAX_SUBSTEP_PX);
    if (n < 1) n = 1;
    if (n > BALL_MAX_SUBSTEPS) n = BALL_MAX_SUBSTEPS;
    float sdt = dt / (float)n;

    bool wall_sounded = false;
    for (int i = 0; i < n; i++) {
        if (ball_step(sdt, &wall_sounded)) break;
    }

    if (demo_on()) {
        update_ai_paddle(&game.player, 0, dt);
        update_ai_paddle(&game.ai, 1, dt);
        /* The demo ends early once it has shown a few points; the scores stay
         * until the next demo resets the field. */
        if (game.player.score + game.ai.score >= DEMO_POINTS)
            start_menu_demo_over(&menu);
    } else if (!two_player) {
        update_ai_paddle(&game.ai, 1, dt);
    }
}

/* Touch drives a paddle to the finger (landscape: Y, portrait: X). */
static void paddle_to_touch(Paddle *p, int x, int y) {
    if (portrait_mode) {
        p->y = (x - offset_x) - PADDLE_HEIGHT / 2;
        if (p->y < 0) p->y = 0;
        if (p->y > play_area_width - PADDLE_HEIGHT) p->y = play_area_width - PADDLE_HEIGHT;
    } else {
        p->y = (y - offset_y) - PADDLE_HEIGHT / 2;
        if (p->y < 0) p->y = 0;
        if (p->y > play_area_height - PADDLE_HEIGHT) p->y = play_area_height - PADDLE_HEIGHT;
    }
}

/* Analog stick and d-pad/keyboard from `in` move a paddle; clamped to max_pos. */
static void paddle_from_pad(Paddle *p, const InputState *in, float max_pos) {
    // Analog stick: proportional speed from axis_ly (landscape) or axis_lx (portrait)
    int axis_val = portrait_mode ? in->axis_lx : in->axis_ly;
    if (axis_val != 0)
        p->y += (axis_val / 1000.0f) * PADDLE_MAX_ANALOG * frame_dt;

    // D-pad / keyboard: fixed speed movement (uses held for smooth continuous movement)
    if (portrait_mode) {
        if (in->buttons[BTN_ID_LEFT].held)  p->y -= PADDLE_SPEED * frame_dt;
        if (in->buttons[BTN_ID_RIGHT].held) p->y += PADDLE_SPEED * frame_dt;
    } else {
        if (in->buttons[BTN_ID_UP].held)    p->y -= PADDLE_SPEED * frame_dt;
        if (in->buttons[BTN_ID_DOWN].held)  p->y += PADDLE_SPEED * frame_dt;
    }

    if (p->y < 0) p->y = 0;
    if (p->y > max_pos) p->y = max_pos;
}

/* The demo: SCREEN_MENU while the attract cycle is on its DEMO page.  Entering
 * it resets the playfield; the real game is reset again on START, so nothing
 * of the demo leaks into it. */
static void demo_sync(void) {
    bool want = (current_screen == SCREEN_MENU &&
                 start_menu_attract(&menu) == SM_ATTRACT_DEMO);
    if (want && !demo_running) {
        game.difficulty = 2;
        reset_game();
        play_clock_restart();
    }
    demo_running = want;
}

/* Back to the start menu from play, pause or game over: undo what START set up
 * so the menu and its attract cycle behave as on first launch. */
static void return_to_menu(void) {
    two_player = false;
    demo_running = false;
    game.paused = false;
    hw_led_pulse_stop(&led_pulse);
    hw_leds_off();
    start_menu_reopen(&menu, get_time_ms());
    current_screen = SCREEN_MENU;
    play_clock_restart();
}

void handle_input() {
    touch_poll(&touch);
    TouchState state = touch_get_state(&touch);
    uint32_t current_time = get_time_ms();

    /* The one dt per frame, shared by the paddle input below and update_game(). */
    frame_dt = (current_time - last_ms) / 1000.0f;
    if (frame_dt > MAX_DT) frame_dt = MAX_DT;
    last_ms = current_time;

    // Poll gamepad/keyboard/touch through unified API
    gamepad_poll(&gamepad, &input, state.x, state.y, state.pressed);

    /* Hot-plug check: rescans only when /dev/input changed or a device went away */
    gamepad_tick(&gamepad, current_time);

    /* Start menu — ahead of the BACK rule below, so that BACK during the
     * SCORES/DEMO pages is swallowed like any other input there (the widget's
     * rule), and on the menu itself arrives as SM_EXIT.  The widget keeps its
     * own edges, so a button or finger still down from the screen before acts
     * only after a fresh press. */
    if (current_screen == SCREEN_MENU) {
        int r = start_menu_update(&menu, &input, state.x, state.y,
                                  state.pressed || state.held,
                                  &gamepad, &audio, current_time);
        if (r == SM_EXIT || r == menu_exit_idx) {
            fb_fade_out(&fb);
            running = false;
        } else if (r == menu_start_idx) {
            static const int DIFF_LEVEL[3] = { 1, 2, 3 };   /* EASY, NORMAL (as always), HARD */
            int d = start_menu_value(&menu, menu_diff_idx);
            if (d < 0 || d > 2) d = 1;
            game.difficulty = DIFF_LEVEL[d];
            two_player = (start_menu_player_count(&menu) == 2);
            demo_running = false;
            reset_game();
            current_screen = SCREEN_PLAYING;
            play_clock_restart();
            /* Non-blocking: a usleep() here delayed the first frame of play
             * by 100 ms from inside handle_input().
             * After reset_game(), which cancels any pending pulse. */
            hw_led_pulse_start(&led_pulse, LED_GREEN, 1, 100, current_time);
        } else {
            demo_sync();
        }
        return;
    }

    // BTN_BACK returns to the start menu — not on GAME OVER, whose flow
    // owns its input, so BACK there cannot skip a high-score name
    if (input.buttons[BTN_ID_BACK].pressed && current_screen != SCREEN_GAME_OVER) {
        return_to_menu();
        return;
    }

    // Game over screen — gameover_update() runs the flow in the draw phase
    if (current_screen == SCREEN_GAME_OVER)
        return;
    
    // Handle pause screen
    if (current_screen == SCREEN_PAUSED) {
        // Pad / keyboard: focus frame, A presses it, Start / Esc = RESUME
        ModalDialogAction action = modal_dialog_input(&pause_dialog, &input, 0);
        if (action == MODAL_ACTION_NONE)
            action = modal_dialog_update(&pause_dialog,
                state.x, state.y, state.pressed, current_time);
        if (action == MODAL_ACTION_BTN0) {
            current_screen = SCREEN_PLAYING;
            play_clock_restart();
            game.paused = false;
            return;
        }
        if (action == MODAL_ACTION_BTN1) {
            return_to_menu();
            return;
        }
        return;
    }
    
    // Playing screen — gamepad/keyboard: pause
    if (input.buttons[BTN_ID_PAUSE].pressed) {
        current_screen = SCREEN_PAUSED;
        game.paused = true;
        modal_dialog_set_focus(&pause_dialog, 0);
        modal_dialog_show(&pause_dialog);
        return;
    }

    // Playing screen - check menu and exit buttons (touch)
    if (state.pressed) {
        // Check exit button (top-right)
        bool exit_touched = button_is_touched(&exit_button, state.x, state.y);
        if (button_check_press(&exit_button, exit_touched, current_time)) {
            return_to_menu();
            return;
        }
        
        // Check menu button (top-left)
        bool menu_touched = button_is_touched(&menu_button, state.x, state.y);
        if (button_check_press(&menu_button, menu_touched, current_time)) {
            current_screen = SCREEN_PAUSED;
            game.paused = true;
            modal_dialog_set_focus(&pause_dialog, 0);
            modal_dialog_show(&pause_dialog);
            return;
        }
    }
    
    if (game.game_over || game.paused) return;

    float max_pos = portrait_mode ?
        (float)(play_area_width - PADDLE_HEIGHT) :
        (float)(play_area_height - PADDLE_HEIGHT);

    if (!two_player) {
        /* One player: touch anywhere, and every device at once, drive the left
         * (bottom, in portrait) paddle; the AI owns the other. */
        if (state.held || state.pressed)
            paddle_to_touch(&game.player, state.x, state.y);
        paddle_from_pad(&game.player, &input, max_pos);
    } else {
        /* Two players: P1 = slot 0 pad/keyboard, P2 = slot 1 (gamepad_player()
         * carries no touch).  The finger drives the paddle on its half of the
         * screen: left/right in landscape, bottom/top in portrait (P1 is the
         * bottom paddle there). */
        if (state.held || state.pressed) {
            bool p1_half = portrait_mode ? (state.y >= (int)(fb.height / 2))
                                         : (state.x < (int)(fb.width / 2));
            paddle_to_touch(p1_half ? &game.player : &game.ai, state.x, state.y);
        }
        paddle_from_pad(&game.player, gamepad_player(&gamepad, 0), max_pos);
        paddle_from_pad(&game.ai, gamepad_player(&gamepad, 1), max_pos);
    }
}

// Draw the playing field (paddles, ball, scores, borders) — used as
// background for PLAYING, PAUSED, and GAME_OVER screens.
static void draw_playing_field(void) {
    // Draw HUD scores — both at top with labels, using text_draw_centered
    if (portrait_mode) {
        // Portrait: AI score on left, Player score on right, both at top
        char ai_score[16];
        snprintf(ai_score, sizeof(ai_score), "%s: %d", ai_label(), game.ai.score);
        text_draw_centered(&fb, fb.width / 3, 35, ai_score, COLOR_RED, 3);
        
        char player_score[16];
        snprintf(player_score, sizeof(player_score), "%s: %d", player_label(), game.player.score);
        text_draw_centered(&fb, fb.width * 2 / 3, 35, player_score, COLOR_GREEN, 3);
    } else {
        // Landscape: player score left, AI score right
        char player_score[16];
        snprintf(player_score, sizeof(player_score), "%s: %d", player_label(), game.player.score);
        text_draw_centered(&fb, fb.width / 3, 35, player_score, COLOR_GREEN, 3);
        
        char ai_score[16];
        snprintf(ai_score, sizeof(ai_score), "%s: %d", ai_label(), game.ai.score);
        text_draw_centered(&fb, fb.width * 2 / 3, 35, ai_score, COLOR_RED, 3);
    }
    
    // Draw menu and exit buttons
    if (!demo_on()) draw_menu_button(&fb, &menu_button);
    if (!demo_on()) draw_exit_button(&fb, &exit_button);
    
    // Draw play area border
    fb_draw_rect(&fb, offset_x - 2, offset_y - 2,
                 play_area_width + 4, play_area_height + 4, COLOR_WHITE);
    
    if (portrait_mode) {
        // Portrait: horizontal center line
        for (int x = 0; x < play_area_width; x += 20) {
            fb_fill_rect(&fb, offset_x + x, offset_y + play_area_height / 2 - 2, 10, 4, RGB(128, 128, 128));
        }
        
        // Draw AI paddle (top, red) — horizontal paddle
        fb_fill_rect(&fb, offset_x + (int)game.ai.y, offset_y + 5,
                     PADDLE_HEIGHT, PADDLE_WIDTH, COLOR_RED);
        
        // Draw player paddle (bottom, green) — horizontal paddle
        fb_fill_rect(&fb, offset_x + (int)game.player.y,
                     offset_y + play_area_height - PADDLE_WIDTH - 5,
                     PADDLE_HEIGHT, PADDLE_WIDTH, COLOR_GREEN);
        
        // Draw controls hint (centered)
        if (demo_on()) {
        } else if (!input.gamepad_connected && !input.keyboard_connected)
            text_draw_centered(&fb, fb.width / 2, fb.height - 18,
                              "TOUCH TO MOVE PADDLE", RGB(100, 100, 100), 1);
        else
            text_draw_centered(&fb, fb.width / 2, fb.height - 18,
                              "D-PAD/STICK: MOVE  ESC: PAUSE", RGB(100, 100, 100), 1);
    } else {
        // Landscape: vertical center line
        for (int y = 0; y < play_area_height; y += 20) {
            fb_fill_rect(&fb, offset_x + play_area_width / 2 - 2, offset_y + y, 4, 10, RGB(128, 128, 128));
        }
        
        // Draw player paddle (left, green)
        fb_fill_rect(&fb, offset_x + 5, offset_y + (int)game.player.y,
                     PADDLE_WIDTH, PADDLE_HEIGHT, COLOR_GREEN);
        
        // Draw AI paddle (right, red)
        fb_fill_rect(&fb, offset_x + play_area_width - PADDLE_WIDTH - 5,
                     offset_y + (int)game.ai.y, PADDLE_WIDTH, PADDLE_HEIGHT, COLOR_RED);
        
        // Draw controls hint (centered)
        if (demo_on()) {
        } else if (!input.gamepad_connected && !input.keyboard_connected)
            text_draw_centered(&fb, fb.width / 2, fb.height - 18,
                              "TOUCH TO MOVE PADDLE", RGB(100, 100, 100), 1);
        else
            text_draw_centered(&fb, fb.width / 2, fb.height - 18,
                              "D-PAD/STICK: MOVE  ESC: PAUSE", RGB(100, 100, 100), 1);
    }

    // Draw ball (same for both orientations)
    fb_fill_circle(&fb, offset_x + (int)game.ball.x + BALL_SIZE / 2,
                   offset_y + (int)game.ball.y + BALL_SIZE / 2,
                   BALL_SIZE / 2, COLOR_WHITE);
}

void draw_game() {
    fb_clear(&fb, COLOR_BLACK);
    
    // Start menu, or the attract cycle's DEMO (the field, silent) and SCORES pages
    if (current_screen == SCREEN_MENU) {
        SmAttract ph = start_menu_attract(&menu);
        if (ph == SM_ATTRACT_MENU) {
            start_menu_draw(&menu, &fb);
            return;
        }
        if (ph == SM_ATTRACT_SCORES) {
            start_menu_draw_scores(&menu, &fb, &hs_table);
            return;
        }
        /* DEMO: the playing field below, drawn with the real draw code */
    }
    
    // Draw the playing field as background (used by PLAYING, PAUSED, GAME_OVER)
    draw_playing_field();

    if (demo_on()) {
        text_draw_centered(&fb, fb.width / 2, SCREEN_VISIBLE_TOP + 20, "DEMO", COLOR_CYAN, 2);
        return;
    }

    // Handle pause screen overlay
    if (current_screen == SCREEN_PAUSED) {
        modal_dialog_draw(&pause_dialog, &fb);
        return;
    }
    
    // Handle game over screen overlay (unified GameOverScreen component)
    if (current_screen == SCREEN_GAME_OVER) {
        TouchState go_st = touch_get_state(&touch);
        /* `input` is the file-scope InputState handle_input() filled this frame. */
        GameOverAction action = gameover_update(&gos, &fb,
                                                go_st.x, go_st.y, go_st.pressed,
                                                &input);
        if (action == GAMEOVER_ACTION_MENU)
            return_to_menu();
        return;
    }
}

int main(int argc, char *argv[]) {
    /* Line-buffer stdout FIRST: at boot it is /var/log/arca/app_stdout.log,
     * not a tty, so glibc block-buffers 4 KB and audio_bed_init()'s playlist
     * receipt never arrives — which reads as a printf that was never reached.
     * common/logger.c line-buffers its own file, which is why only the printf
     * lines go missing (../CLAUDE.md → App lifecycle). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *fb_device = "/dev/fb0";
    const char *touch_device = "/dev/input/touchscreen0";
    
    if (argc > 1) fb_device = argv[1];
    if (argc > 2) touch_device = argv[2];
    
    // Singleton guard — prevent duplicate instances
    int lock_fd = acquire_instance_lock("pong");
    if (lock_fd < 0) {
        fprintf(stderr, "pong: another instance is already running\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Pin 32bpp — /dev/fb0 keeps whatever ran last (see fb_set_bpp). */
    fb_set_bpp(fb_device, 32);

    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, "Failed to initialize framebuffer\n");
        return 1;
    }
    
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, "Failed to initialize touch input\n");
        fb_close(&fb);
        return 1;
    }
    
    // Initialize hardware control
    hw_init();
    hw_set_backlight(100);
    audio_init(&audio);  // Initialize audio (non-fatal if unavailable)

    /* The music bed: the playlist named by /opt/arca/soundsets/pong.sound,
     * with the four states and the hold/resume rules in common/audio_bed.c.
     * ⚠️ That file is the ONLY home for the paths — no set file means no music,
     * and pong_music present but EMPTY is the off switch. */
    AudioBed bed;
    audio_bed_init(&bed, &audio, "pong");
    
    srand(time(NULL));
    init_game();
    
    // Gamepad init
    gamepad_init(&gamepad);
    gamepad_set_mouse_bounds(&gamepad, fb.width, fb.height);

    printf("Pong game started!\n");
    
    /* ── main loop ───────────────────────────────────────────────────── */
    bool needs_redraw = true;
    while (running) {
        GameScreen prev_screen = current_screen;
        handle_input();
        update_game();
        hw_led_pulse_update(&led_pulse, get_time_ms());

        /* Dirty-flag: active gameplay always redraws; static screens on change */
        if (current_screen == SCREEN_PLAYING || demo_on()) {
            needs_redraw = true;  /* continuous rendering (ball + AI always moving; the demo is gameplay) */
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

        /* The start menu's bracket blink and attract page changes arrive with no
         * input; without this the blink freezes.  Off the MENU page it reports
         * once per page edge, so the demo (redrawn every frame above) is not
         * affected. */
        if (current_screen == SCREEN_MENU && start_menu_needs_redraw(&menu))
            needs_redraw = true;

        /* One bed transition, ABOVE the redraw block and before the pump.
         * ⚠️ The position is load-bearing: SCREEN_GAME_OVER's redraw runs
         * gameover_update()'s BLOCKING name entry, so a bed serviced after the
         * block stays PLAYING for the whole keyboard session.  Before the pump
         * so a voice started on this iteration is fed on the same one, and so
         * the release fade is rendered.  Full reason: brick_breaker.c's copy. */
        audio_bed_service(&bed, current_screen == SCREEN_PLAYING, current_screen == SCREEN_PAUSED);

        if (needs_redraw) {
            draw_game();
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
        needs_redraw = false;  /* reset for next frame */
    }
    
    gamepad_close(&gamepad);
    touch_close(&touch);
    hw_leds_off();
    hw_set_backlight(100);
    audio_close(&audio);
    fb_close(&fb);
    
    printf("Pong ended.\n");
    return 0;
}
