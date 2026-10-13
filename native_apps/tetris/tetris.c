/*
 * Tetris Game - Native C Implementation
 * Optimized for 300MHz ARM with touchscreen
 * Touch controls: Tap left/right to move, tap center to rotate, swipe down to drop
 * Supports keyboard, gamepad, and touch input via unified gamepad module
 * Keyboard/gamepad features DAS (Delayed Auto Shift) for left/right movement
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/common.h"
#include "../common/hardware.h"
#include "../common/highscore.h"
#include "../common/audio.h"
#include "../common/audio_bed.h"
#include "../common/gamepad.h"
#include "../common/start_menu.h"

#define BOARD_WIDTH 10
#define BOARD_HEIGHT 20
#define NUM_TETROMINOS 7

/* Board frame: fb_draw_rect() puts a BOARD_BORDER-thick outline this far
 * outside the cells on every side, so the vertical budget has to include it. */
#define BOARD_BORDER   2
#define BOARD_GAP_TOP  6   /* clearance between the button row and the frame */

/* Gravity in MILLISECONDS per row, never in frames.  The main loop sleeps
 * FRAME_DELAY_IDLE_US (100 ms) on any iteration it did not redraw, so a frame
 * counter made the fall rate depend on how busy the renderer was: measured on
 * the panel at roughly one row every 5-6 seconds, and it sped up while a key
 * was held. */
#define DROP_BASE_MS        800   /* level 1 */
#define DROP_LEVEL_STEP_MS   60   /* subtracted per level above 1 */
#define DROP_MIN_MS         120   /* floor, however high the level gets */
#define DROP_SOFT_MS         50   /* while DOWN is held */

typedef enum {
    SCREEN_MENU,
    SCREEN_PLAYING,
    SCREEN_PAUSED,
    SCREEN_GAME_OVER
} GameScreen;

typedef enum {
    PIECE_I, PIECE_O, PIECE_T, PIECE_S, PIECE_Z, PIECE_J, PIECE_L
} PieceType;

typedef struct {
    int x, y;
    PieceType type;
    int rotation;
} Piece;

typedef struct {
    int board[BOARD_HEIGHT][BOARD_WIDTH];
    Piece current;
    Piece next;
    int score;
    int high_score;
    int lines_cleared;
    int level;
    bool game_over;
    bool paused;
    int drop_interval_ms;      /* ms per row at the current level */
    uint32_t last_drop_ms;     /* when the piece last moved down */
} GameState;

// Tetromino shapes (4 rotations each)
const int tetrominos[NUM_TETROMINOS][4][4][4] = {
    // I piece
    {
        {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
        {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}},
        {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}}
    },
    // O piece
    {
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}
    },
    // T piece
    {
        {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}}
    },
    // S piece
    {
        {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}},
        {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}},
        {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}}
    },
    // Z piece
    {
        {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}},
        {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}}
    },
    // J piece
    {
        {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}}
    },
    // L piece
    {
        {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{1,0,0,0},{0,0,0,0}},
        {{1,1,0,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}}
    }
};

const uint32_t piece_colors[NUM_TETROMINOS] = {
    RGB(0, 255, 255),   // I - Cyan
    RGB(255, 255, 0),   // O - Yellow
    RGB(128, 0, 128),   // T - Purple
    RGB(0, 255, 0),     // S - Green
    RGB(255, 0, 0),     // Z - Red
    RGB(0, 0, 255),     // J - Blue
    RGB(255, 165, 0)    // L - Orange
};

/* DAS (Delayed Auto Shift) constants */
#define DAS_INITIAL_DELAY_MS 200   /* ms before auto-repeat starts */
#define DAS_REPEAT_INTERVAL_MS 50  /* ms between auto-repeat moves */

/* DAS state for a single direction */
typedef struct {
    bool active;           /* Currently in DAS for this direction */
    uint32_t hold_start;   /* Timestamp when key was first held */
    uint32_t last_repeat;  /* Timestamp of last auto-repeat fire */
} DASState;

// Global variables
Framebuffer fb;
TouchInput touch;
GamepadManager gamepad;
InputState input;
GameState game;
int cell_size;
int board_offset_x;
int board_offset_y;
bool running = true;
bool portrait_mode = false;
int last_touch_x = -1;
int last_touch_y = -1;
Button menu_button;
Button exit_button;
ModalDialog pause_dialog;
GameScreen current_screen = SCREEN_MENU;
// Start menu (../common/start_menu.h): START, EXIT (tetris has no difficulty setting)
StartMenu menu;
int menu_start_idx, menu_exit_idx;
HighScoreTable hs_table;
static GameOverScreen gos;
Audio audio;
static DASState das_left  = {false, 0, 0};
static DASState das_right = {false, 0, 0};
static bool soft_drop_active = false;
/* Set whenever the game is not in play, so update_game() rebaselines the
 * gravity clock on the way back in instead of owing a row. */
static bool drop_clock_stale = true;
/* LED flourishes (game start, game over), advanced once per frame by the main loop. */
static LedPulse led_pulse;
/* Attract-cycle DEMO: an AI plays on the real board while the start menu is
 * on its DEMO page (current_screen stays SCREEN_MENU, so the music bed -
 * serviced from SCREEN_PLAYING only - stays off).  Silent, LED-dark, no high
 * score, never SCREEN_GAME_OVER; demo_sync() resets the board on entry and
 * START resets it again, so nothing of the demo reaches a real game. */
static bool demo_running = false;
#define DEMO_STEP_MS 70          /* one AI action (turn, shift or one row down) per step */
static uint32_t demo_next_ms = 0;
static bool demo_planned = false;   /* target below is valid for the current piece */
static int demo_target_rot = 0, demo_target_x = 0;

// Function prototypes
void init_game();
void reset_game();
void spawn_piece(Piece *piece);
bool check_collision(Piece *piece, int dx, int dy, int new_rotation);
void lock_piece();
void clear_lines();
void update_game();
void draw_playing_field();
void draw_game();
void handle_input();
void signal_handler(int sig);

void signal_handler(int sig) {
    running = false;
}

void init_game() {
    portrait_mode = fb.portrait_mode;
    
    // Calculate board dimensions.
    //
    // The board is drawn, never pressed — touch control is X-thresholds either
    // side of it plus a full-width bottom band — so it may use the whole
    // VISIBLE screen vertically.  It only has to clear the SAFE-anchored
    // MENU/EXIT row, because the SCORE/LVL line now lives in the VISIBLE band
    // *above* that row rather than in a HUD strip below it.  Deriving the
    // top from the row instead of a 55 px literal is also what stopped the well
    // hanging off the bottom edge: the old literal was smaller than the
    // row itself, so the buttons sat on the board and the board ran long.
    int board_top = LAYOUT_MENU_BTN_Y + BTN_MENU_HEIGHT + BOARD_GAP_TOP;
    int available_h = SCREEN_VISIBLE_BOTTOM - board_top - 2 * BOARD_BORDER;

    cell_size = available_h / BOARD_HEIGHT;

    // In portrait mode, allow larger cells (up to what fits); in landscape cap at 30
    if (!portrait_mode && cell_size > 30) cell_size = 30;

    int board_h = BOARD_HEIGHT * cell_size;
    int board_w = BOARD_WIDTH * cell_size;

    // Center board horizontally on the visible screen
    board_offset_x = SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - board_w) / 2;

    // Center board vertically in available space below HUD
    board_offset_y = board_top + (available_h - board_h) / 2;
    
    hs_init(&hs_table, "tetris");
    hs_load(&hs_table);
    game.high_score = hs_table.count > 0 ? hs_table.entries[0].score : 0;

    // Initialize buttons — LAYOUT_* is SCREEN_SAFE_*-anchored, so the row moves
    // down with the measured touch inset instead of losing its top rows to it.
    button_init(&menu_button, LAYOUT_MENU_BTN_X, LAYOUT_MENU_BTN_Y,
                BTN_MENU_WIDTH, BTN_MENU_HEIGHT, "",
                BTN_MENU_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&exit_button, LAYOUT_EXIT_BTN_X, LAYOUT_EXIT_BTN_Y,
                BTN_EXIT_WIDTH, BTN_EXIT_HEIGHT,
                "",
                BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    /* Attract cycle MENU -> DEMO -> SCORES, drawn by draw_game(). */
    start_menu_init(&menu, "TETRIS",
                    "L/R: MOVE   UP/A: ROTATE\n"
                    "DOWN: SOFT DROP   X: HARD DROP\n"
                    "TAP LEFT/RIGHT/CENTER/BOTTOM", get_time_ms());
    menu_start_idx = start_menu_add_action(&menu, "START");
    menu_exit_idx  = start_menu_add_action(&menu, "EXIT");
    start_menu_select(&menu, menu_start_idx, get_time_ms());
    start_menu_set_attract(&menu, true);
    modal_dialog_init(&pause_dialog, "PAUSED", NULL, 2);
    modal_dialog_set_button(&pause_dialog, 0, "RESUME", BTN_COLOR_PRIMARY, COLOR_WHITE);
    modal_dialog_set_button(&pause_dialog, 1, "EXIT", BTN_COLOR_DANGER, COLOR_WHITE);
    
    reset_game();
}

void reset_game() {
    memset(game.board, 0, sizeof(game.board));
    game.score = 0;
    game.lines_cleared = 0;
    game.level = 1;
    game.game_over = false;
    game.paused = false;
    game.drop_interval_ms = DROP_BASE_MS;
    game.last_drop_ms = get_time_ms();
    /* The game-over flourish outlives the game-over screen now that it does not
     * block, so cancel it rather than flash red into the new round. */
    hw_led_pulse_stop(&led_pulse);

    spawn_piece(&game.current);
    spawn_piece(&game.next);
}

void spawn_piece(Piece *piece) {
    piece->type = rand() % NUM_TETROMINOS;
    piece->rotation = 0;
    piece->x = BOARD_WIDTH / 2 - 2;
    piece->y = 0;
}

bool check_collision(Piece *piece, int dx, int dy, int new_rotation) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (tetrominos[piece->type][new_rotation][y][x]) {
                int board_x = piece->x + x + dx;
                int board_y = piece->y + y + dy;
                
                if (board_x < 0 || board_x >= BOARD_WIDTH || 
                    board_y >= BOARD_HEIGHT) {
                    return true;
                }
                
                if (board_y >= 0 && game.board[board_y][board_x]) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Rotate the current piece into `new_rotation`, nudging it if the rotated form
 * does not fit where it stands — a wall kick.
 *
 * Without one, a rotation whose cells land outside the board or inside settled
 * blocks is simply refused, and the classic casualty is the I-piece: stood
 * vertically hard against the right wall, all four cells of its horizontal form
 * are off-board, so it can never be turned back flat for the rest of the game.
 *
 * Offsets are tried in order: in place, then one and two cells left/right (two
 * is what an I-piece needs, since its rotation centre sits one cell in from the
 * end), then the same nudges one row up — a floor kick, which is what frees a
 * piece resting on the stack.  `check_collision()` permits negative board_y and
 * `lock_piece()` skips those cells, so an upward kick is representable.  If no
 * offset fits, the rotation is refused exactly as it was before. */
static bool try_rotate(int new_rotation) {
    static const int kick_dx[] = { 0, -1,  1, -2,  2,  0, -1,  1 };
    static const int kick_dy[] = { 0,  0,  0,  0,  0, -1, -1, -1 };

    for (unsigned k = 0; k < sizeof(kick_dx) / sizeof(kick_dx[0]); k++) {
        if (check_collision(&game.current, kick_dx[k], kick_dy[k], new_rotation))
            continue;
        game.current.x += kick_dx[k];
        game.current.y += kick_dy[k];
        game.current.rotation = new_rotation;
        return true;
    }
    return false;
}

void lock_piece() {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (tetrominos[game.current.type][game.current.rotation][y][x]) {
                int board_y = game.current.y + y;
                int board_x = game.current.x + x;
                if (board_y >= 0 && board_y < BOARD_HEIGHT) {
                    game.board[board_y][board_x] = game.current.type + 1;
                }
            }
        }
    }
    
    clear_lines();
    
    game.current = game.next;
    spawn_piece(&game.next);
    
    if (check_collision(&game.current, 0, 0, game.current.rotation)) {
        if (demo_running) {
            /* The demo topped out: no game over, no score, no sound.  The
             * widget returns to its menu; the next demo/START resets the board. */
            game.game_over = true;
            start_menu_demo_over(&menu);
            return;
        }
        game.game_over = true;
        current_screen = SCREEN_GAME_OVER;
        // Initialize unified game over screen with level info
        char info_line[64];
        snprintf(info_line, sizeof(info_line), "LEVEL %d", game.level);
        gameover_init(&gos, &fb, game.score, NULL, info_line,
                      "TETRIS", &hs_table, &touch, &gamepad);
        /* Red pulse + fail sound.  Non-blocking: this runs inside update_game(),
         * so the 3 x 200 ms on/off loop that used to be here froze the panel for
         * 1.2 s with no touch poll and no redraw — and it ran *before* the
         * game-over screen was ever drawn, so the player stared at the old
         * playfield and any tap made during it was discarded. */
        hw_led_pulse_start(&led_pulse, LED_RED, 3, 200, get_time_ms());
        /* ⚠️ gameover, not fail: tetris has no LIVES, so a blocked spawn ends the
         * RUN and audio_fail() is the lost-a-life sound (common/audio.h's enum).
         * It fires INSTEAD of fail, never after — fx_gameover is 1.19 s against
         * fail's 350 ms and the two would sum on the bus. */
        audio_gameover(&audio);
    }
}

void clear_lines() {
    int lines = 0;
    
    for (int y = BOARD_HEIGHT - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BOARD_WIDTH; x++) {
            if (game.board[y][x] == 0) {
                full = false;
                break;
            }
        }
        
        if (full) {
            lines++;
            // Move all lines above down
            for (int yy = y; yy > 0; yy--) {
                for (int x = 0; x < BOARD_WIDTH; x++) {
                    game.board[yy][x] = game.board[yy - 1][x];
                }
            }
            // Clear top line
            for (int x = 0; x < BOARD_WIDTH; x++) {
                game.board[0][x] = 0;
            }
            y++; // Check this line again
        }
    }
    
    if (lines > 0) {
        game.lines_cleared += lines;
        game.score += (lines * lines * 100);  // Bonus for multiple lines
        game.level = 1 + game.lines_cleared / 10;
        game.drop_interval_ms = DROP_BASE_MS - (game.level - 1) * DROP_LEVEL_STEP_MS;
        if (game.drop_interval_ms < DROP_MIN_MS) game.drop_interval_ms = DROP_MIN_MS;
        
        // LED + audio effects for line clears (the demo is silent and dark)
        if (demo_running) {
            /* no effects */
        } else if (lines == 4) {
            // Tetris! Fanfare + yellow flash
            hw_set_leds(HW_LED_COLOR_YELLOW);
            audio_success(&audio);  // Ascending arpeggio (~440ms)
            hw_leds_off();
        } else if (lines >= 2) {
            // Multi-line clear
            hw_set_led(LED_GREEN, 100);
            audio_blip(&audio);     // Short blip (~60ms)
            hw_leds_off();
        } else {
            // Single line
            hw_set_led(LED_GREEN, 100);
            audio_blip(&audio);     // Short blip (~60ms)
            hw_leds_off();
        }
    }
}

/* Placement score for `p` already dropped to its resting row: lines cleared
 * minus aggregate height, holes and bumpiness (integer weights, no divide). */
static int demo_eval(const Piece *p) {
    int tmp[BOARD_HEIGHT][BOARD_WIDTH];
    memcpy(tmp, game.board, sizeof(tmp));
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (tetrominos[p->type][p->rotation][y][x]) {
                int by = p->y + y;
                if (by < 0) return -1000000;   /* sticks out of the top */
                tmp[by][p->x + x] = 1;
            }

    /* Remove full rows (compact downward) and count them. */
    int out[BOARD_HEIGHT][BOARD_WIDTH];
    memset(out, 0, sizeof(out));
    int lines = 0, dst = BOARD_HEIGHT - 1;
    for (int y = BOARD_HEIGHT - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BOARD_WIDTH; x++)
            if (!tmp[y][x]) { full = false; break; }
        if (full) { lines++; continue; }
        memcpy(out[dst--], tmp[y], sizeof(out[0]));
    }

    int agg = 0, holes = 0, bump = 0, prev_h = 0;
    for (int x = 0; x < BOARD_WIDTH; x++) {
        int h = 0;
        for (int y = 0; y < BOARD_HEIGHT; y++) {
            if (out[y][x]) {
                if (!h) h = BOARD_HEIGHT - y;
            } else if (h) {
                holes++;
            }
        }
        agg += h;
        if (x > 0) bump += (h > prev_h) ? h - prev_h : prev_h - h;
        prev_h = h;
    }
    return lines * 760 - agg * 510 - holes * 356 - bump * 184;
}

/* Choose the best rotation x column for the current piece. */
static void demo_plan(void) {
    int best = -2000000;
    demo_target_rot = game.current.rotation;
    demo_target_x = game.current.x;
    for (int rot = 0; rot < 4; rot++) {
        for (int x = -2; x < BOARD_WIDTH; x++) {
            Piece p = game.current;
            p.x = x; p.y = 0; p.rotation = rot;
            if (check_collision(&p, 0, 0, rot)) continue;
            while (!check_collision(&p, 0, 1, rot)) p.y++;
            int s = demo_eval(&p);
            if (s > best) { best = s; demo_target_rot = rot; demo_target_x = x; }
        }
    }
    demo_planned = true;
}

/* One AI action per DEMO_STEP_MS: turn, then shift, then fall a row at a time. */
static void demo_step(void) {
    if (game.game_over) return;          /* topped out; waiting for the widget */
    uint32_t now = get_time_ms();
    if ((uint32_t)(now - demo_next_ms) < DEMO_STEP_MS) return;
    demo_next_ms = now;

    if (!demo_planned) demo_plan();

    if (game.current.rotation != demo_target_rot) {
        if (!try_rotate((game.current.rotation + 1) & 3))
            demo_target_rot = game.current.rotation;   /* blocked: settle for this one */
    } else if (game.current.x != demo_target_x) {
        int dx = (demo_target_x > game.current.x) ? 1 : -1;
        if (check_collision(&game.current, dx, 0, game.current.rotation))
            demo_target_x = game.current.x;
        else
            game.current.x += dx;
    } else if (!check_collision(&game.current, 0, 1, game.current.rotation)) {
        game.current.y++;
    } else {
        demo_planned = false;
        lock_piece();
    }
}

/* The demo runs while SCREEN_MENU is on the attract cycle's DEMO page.
 * Entering it gives a fresh board. */
static void demo_sync(void) {
    bool want = (current_screen == SCREEN_MENU &&
                 start_menu_attract(&menu) == SM_ATTRACT_DEMO);
    if (want && !demo_running) {
        demo_running = true;       /* before reset_game(): spawn/LED paths see the demo */
        reset_game();
        demo_planned = false;
        demo_next_ms = get_time_ms();
    }
    demo_running = want;
}

void update_game() {
    if (demo_running) {
        drop_clock_stale = true;   /* a real game rebaselines its gravity clock */
        demo_step();
        return;
    }
    // Only process game logic when actually playing (Issue #9)
    if (current_screen != SCREEN_PLAYING || game.game_over || game.paused) {
        /* Not playing: the gravity clock stops, and is rebaselined on the way
         * back in so a long pause does not owe the player a row.  Doing it here
         * rather than at each of the six "current_screen = SCREEN_PLAYING"
         * sites means a seventh one cannot forget to. */
        drop_clock_stale = true;
        return;
    }

    /* Gravity off a wall-clock delta, so the fall rate is independent of how
     * long the last frame took and of whether the loop took the idle sleep.
     * uint32_t subtraction is wrap-safe. */
    uint32_t now = get_time_ms();
    if (drop_clock_stale) {
        drop_clock_stale = false;
        game.last_drop_ms = now;
        return;
    }

    int interval = soft_drop_active ? DROP_SOFT_MS : game.drop_interval_ms;
    if ((uint32_t)(now - game.last_drop_ms) < (uint32_t)interval) return;

    /* One row per call, whatever the delta: never replay a backlog. */
    game.last_drop_ms = now;

    if (!check_collision(&game.current, 0, 1, game.current.rotation)) {
        game.current.y++;
        if (soft_drop_active)
            game.score += 1;  // Soft drop bonus
    } else {
        lock_piece();
    }
}

/* Helper: process DAS for a direction. Returns true if a move should fire this frame. */
static bool das_update(DASState *das, bool held, bool pressed, uint32_t now) {
    if (pressed) {
        /* First press — always fire, start DAS timer */
        das->active = true;
        das->hold_start = now;
        das->last_repeat = now;
        return true;
    }
    if (!held) {
        /* Released — cancel DAS */
        das->active = false;
        return false;
    }
    /* Key is still held — check for auto-repeat */
    if (!das->active) return false;
    uint32_t hold_duration = now - das->hold_start;
    if (hold_duration >= DAS_INITIAL_DELAY_MS) {
        uint32_t since_repeat = now - das->last_repeat;
        if (since_repeat >= DAS_REPEAT_INTERVAL_MS) {
            das->last_repeat = now;
            return true;
        }
    }
    return false;
}

/* Back to the start menu from play, pause or game over: drop the play-time
 * state (LED flourish, pause flag, gravity clock) so the menu looks as on
 * first launch.  The bed follows current_screen in the main loop. */
static void return_to_menu(void) {
    demo_running = false;
    hw_led_pulse_stop(&led_pulse);
    hw_leds_off();
    game.paused = false;
    drop_clock_stale = true;
    start_menu_reopen(&menu, get_time_ms());
    current_screen = SCREEN_MENU;
}

void handle_input() {
    touch_poll(&touch);
    TouchState state = touch_get_state(&touch);
    uint32_t current_time = get_time_ms();

    // Poll gamepad/keyboard/touch through unified API
    gamepad_poll(&gamepad, &input, state.x, state.y, state.pressed);

    /* Hot-plug check: rescans only when /dev/input changed or a device went away */
    gamepad_tick(&gamepad, current_time);

    /* Start menu — ahead of the BACK rule below, so BACK during the SCORES
     * page is swallowed like any input there, and on the menu itself arrives
     * as SM_EXIT.  The widget keeps its own edges. */
    if (current_screen == SCREEN_MENU) {
        int r = start_menu_update(&menu, &input, state.x, state.y,
                                  state.pressed || state.held,
                                  &gamepad, &audio, current_time);
        if (r == SM_EXIT || r == menu_exit_idx) {
            fb_fade_out(&fb);
            running = false;
        } else if (r == menu_start_idx) {
            demo_running = false;
            reset_game();
            game.high_score = hs_table.count > 0 ? hs_table.entries[0].score : 0;
            current_screen = SCREEN_PLAYING;
            /* Non-blocking: a usleep() here delayed the first frame of play
             * by 100 ms from inside handle_input(). */
            hw_led_pulse_start(&led_pulse, LED_GREEN, 1, 100, current_time);
        } else {
            demo_sync();
        }
        return;
    }

    // BTN_BACK during play / pause returns to the start menu; GAME OVER's
    // flow owns its input, so BACK there cannot skip a high-score name
    if (input.buttons[BTN_ID_BACK].pressed && current_screen != SCREEN_GAME_OVER) {
        return_to_menu();
        return;
    }

    // Game over screen — gameover_update() runs in the draw phase
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
    
    // Touch game controls
    if (state.pressed && !game.game_over && !game.paused) {
        int tx = state.x;
        int ty = state.y;
        
        // Game controls
        int board_right = board_offset_x + BOARD_WIDTH * cell_size;
        
        if (ty > (int)fb.height - 80) {
            // Bottom — hard drop (check this FIRST for both modes)
            while (!check_collision(&game.current, 0, 1, game.current.rotation)) {
                game.current.y++;
                game.score += 2;
            }
            /* ⚠️ No audio_interrupt() before an effect — on the mix bus it means
             * "stop ALL voices", so a 60 ms move or place-down tone discards a
             * fanfare that is still playing.  All eight sites in this file
             * dropped theirs; the rule and the measurement that produced it
             * (brick_breaker by ear, `.188` 2026-08-20) are in ../CLAUDE.md →
             * Mixing.  No counter sees this — a voice stopped early is not
             * `lost`, `drop` or `clip`, so it is judged by ear.
             * ⚠️ The audible change here is not only the fanfare surviving: a
             * DAS-repeated left/right no longer cuts its own predecessor, so
             * fast moves overlap rather than replace.  A full bus REFUSES and
             * counts `drop`; it never steals a voice, so the worst case is a
             * missing click, not a truncated one. */
            audio_tone(&audio, 500, 60);
            audio_tone(&audio, 250, 70);
            lock_piece();
        } else if (portrait_mode) {
            // Portrait touch zones: wide center (60%) for rotation, narrow edges (20%) + margins for movement
            int board_width_px = BOARD_WIDTH * cell_size;
            int left_zone_edge = board_offset_x + board_width_px / 5;       // 20% from left board edge
            int right_zone_edge = board_offset_x + board_width_px * 4 / 5;  // 20% from right board edge
            
            if (tx < left_zone_edge) {
                // Left 20% of board or outside left edge — move left
                if (!check_collision(&game.current, -1, 0, game.current.rotation)) {
                    game.current.x--;
                    audio_tone(&audio, 880, 60);
                }
            } else if (tx > right_zone_edge) {
                // Right 20% of board or outside right edge — move right
                if (!check_collision(&game.current, 1, 0, game.current.rotation)) {
                    game.current.x++;
                    audio_tone(&audio, 880, 60);
                }
            } else {
                // Center 60% of board — rotate
                try_rotate((game.current.rotation + 1) % 4);
            }
        } else {
            // Landscape touch zones (existing logic)
            if (tx < board_offset_x - 10) {
                // Left side — move left
                if (!check_collision(&game.current, -1, 0, game.current.rotation)) {
                    game.current.x--;
                    audio_tone(&audio, 880, 60);
                }
            } else if (tx > board_right + 10) {
                // Right side — move right
                if (!check_collision(&game.current, 1, 0, game.current.rotation)) {
                    game.current.x++;
                    audio_tone(&audio, 880, 60);
                }
            } else {
                // Center — rotate
                try_rotate((game.current.rotation + 1) % 4);
            }
        }
        
        last_touch_x = tx;
        last_touch_y = ty;
    }

    // Gamepad/keyboard controls (only while playing and not paused)
    if (!game.game_over && !game.paused && current_screen == SCREEN_PLAYING) {
        // Left/Right with DAS (Delayed Auto Shift)
        if (das_update(&das_left, input.buttons[BTN_ID_LEFT].held,
                       input.buttons[BTN_ID_LEFT].pressed, current_time)) {
            if (!check_collision(&game.current, -1, 0, game.current.rotation)) {
                game.current.x--;
                audio_tone(&audio, 880, 60);
            }
        }
        if (das_update(&das_right, input.buttons[BTN_ID_RIGHT].held,
                       input.buttons[BTN_ID_RIGHT].pressed, current_time)) {
            if (!check_collision(&game.current, 1, 0, game.current.rotation)) {
                game.current.x++;
                audio_tone(&audio, 880, 60);
            }
        }

        // Rotate: BTN_UP or BTN_JUMP (pressed edge only)
        if (input.buttons[BTN_ID_UP].pressed ||
            input.buttons[BTN_ID_JUMP].pressed) {
            try_rotate((game.current.rotation + 1) % 4);
        }

        // Counter-clockwise rotate: BTN_RUN (optional)
        if (input.buttons[BTN_ID_RUN].pressed) {
            try_rotate((game.current.rotation + 3) % 4);
        }

        // Soft drop: BTN_DOWN held accelerates drop
        soft_drop_active = input.buttons[BTN_ID_DOWN].held;

        // Hard drop: BTN_ACTION (pressed edge only)
        if (input.buttons[BTN_ID_ACTION].pressed) {
            while (!check_collision(&game.current, 0, 1, game.current.rotation)) {
                game.current.y++;
                game.score += 2;
            }
            audio_tone(&audio, 500, 60);
            audio_tone(&audio, 250, 70);
            lock_piece();
        }
    }
}

// Draw the playing field (board, falling piece, HUD) — used as background
// for PLAYING, PAUSED, and GAME_OVER screens
void draw_playing_field() {
    /* HUD: SCORE and LEVEL on ONE line, in the VISIBLE band above the
     * SAFE-anchored MENU/EXIT row.  The band is drawable, just not
     * pressable, and a status row is exactly what it is for; stacking the two
     * strings below SAFE_TOP put LEVEL underneath the buttons, which are drawn
     * after it.  Horizontally the line sits in the gap between the two buttons,
     * so it cannot collide with either — and when the band is shorter than the
     * text (uncalibrated panel, inset 0) it drops into the row's own vertical
     * centre, which is empty between the buttons. */
    char score_text[32];
    char level_text[32];
    snprintf(score_text, sizeof(score_text), "SCORE:%d", game.score);
    snprintf(level_text, sizeof(level_text), "LVL:%d", game.level);

    const int hud_scale = 2;
    const int hud_gap   = 24;
    int hud_h  = text_measure_height(hud_scale);
    int band_h = LAYOUT_MENU_BTN_Y - SCREEN_VISIBLE_TOP;
    int hud_y  = (band_h >= hud_h)
                 ? SCREEN_VISIBLE_TOP + (band_h - hud_h) / 2
                 : LAYOUT_MENU_BTN_Y + (BTN_MENU_HEIGHT - hud_h) / 2;

    int score_w = text_measure_width(score_text, hud_scale);
    int level_w = text_measure_width(level_text, hud_scale);
    int hud_cx  = (LAYOUT_MENU_BTN_X + BTN_MENU_WIDTH + LAYOUT_EXIT_BTN_X) / 2;
    int hud_x   = hud_cx - (score_w + hud_gap + level_w) / 2;

    fb_draw_text(&fb, hud_x, hud_y, score_text, COLOR_WHITE, hud_scale);
    fb_draw_text(&fb, hud_x + score_w + hud_gap, hud_y, level_text, COLOR_CYAN, hud_scale);

    // Draw menu and exit buttons
    if (!demo_running) draw_menu_button(&fb, &menu_button);
    if (!demo_running) draw_exit_button(&fb, &exit_button);

    // Draw board border
    fb_draw_rect(&fb, board_offset_x - BOARD_BORDER, board_offset_y - BOARD_BORDER,
                 BOARD_WIDTH * cell_size + 2 * BOARD_BORDER,
                 BOARD_HEIGHT * cell_size + 2 * BOARD_BORDER, COLOR_WHITE);
    
    // Draw locked pieces
    for (int y = 0; y < BOARD_HEIGHT; y++) {
        for (int x = 0; x < BOARD_WIDTH; x++) {
            if (game.board[y][x]) {
                int px = board_offset_x + x * cell_size;
                int py = board_offset_y + y * cell_size;
                uint32_t color = piece_colors[game.board[y][x] - 1];
                fb_fill_rect(&fb, px + 1, py + 1, cell_size - 2, cell_size - 2, color);
            }
        }
    }
    
    // Draw current piece
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (tetrominos[game.current.type][game.current.rotation][y][x]) {
                int px = board_offset_x + (game.current.x + x) * cell_size;
                int py = board_offset_y + (game.current.y + y) * cell_size;
                uint32_t color = piece_colors[game.current.type];
                fb_fill_rect(&fb, px + 1, py + 1, cell_size - 2, cell_size - 2, color);
            }
        }
    }
    
    // Draw next piece preview
    if (portrait_mode) {
        // Portrait: draw NEXT preview to the left of the exit button (avoids overlap)
        int preview_size = cell_size / 3;
        if (preview_size < 6) preview_size = 6;
        int preview_block_w = 4 * preview_size;
        int next_x = exit_button.x - preview_block_w - 8;
        int next_y = exit_button.y + (BTN_EXIT_HEIGHT - 4 * preview_size) / 2;
        int label_w = text_measure_width("NEXT:", 1);
        fb_draw_text(&fb, next_x - label_w - 4, next_y, "NEXT:", COLOR_WHITE, 1);
        
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (tetrominos[game.next.type][0][y][x]) {
                    int px = next_x + x * preview_size;
                    int py = next_y + y * preview_size;
                    uint32_t color = piece_colors[game.next.type];
                    fb_fill_rect(&fb, px, py, preview_size - 1, preview_size - 1, color);
                }
            }
        }
        
        // Draw controls hint below the board (centered on play area)
        int hint_y = board_offset_y + BOARD_HEIGHT * cell_size + 10;
        if (demo_running)   /* hidden MENU button's place: scale 2 is clipped under the board */
            text_draw_centered(&fb, menu_button.x + menu_button.width / 2,
                               menu_button.y + menu_button.height / 2,
                               "DEMO", COLOR_CYAN, 2);
        else
            text_draw_centered(&fb, board_offset_x + (BOARD_WIDTH * cell_size) / 2, hint_y,
                              "L/R: MOVE  CENTER: ROTATE  BOTTOM: DROP",
                              RGB(100, 100, 100), 1);
    } else {
        // Landscape: side panel to the right of the board
        int next_x = board_offset_x + BOARD_WIDTH * cell_size + 20;
        int next_y = board_offset_y + 20;
        fb_draw_text(&fb, next_x, next_y - 20, "NEXT:", COLOR_WHITE, 2);
        
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (tetrominos[game.next.type][0][y][x]) {
                    int px = next_x + x * (cell_size / 2);
                    int py = next_y + y * (cell_size / 2);
                    uint32_t color = piece_colors[game.next.type];
                    fb_fill_rect(&fb, px, py, cell_size / 2 - 1, cell_size / 2 - 1, color);
                }
            }
        }
        
        // Draw controls hint
        if (demo_running) {
            fb_draw_text(&fb, 10, fb.height - 50, "DEMO", COLOR_CYAN, 2);
        } else {
            fb_draw_text(&fb, 10, fb.height - 60, "L/R: MOVE", RGB(100, 100, 100), 1);
            fb_draw_text(&fb, 10, fb.height - 45, "CENTER: ROTATE", RGB(100, 100, 100), 1);
            fb_draw_text(&fb, 10, fb.height - 30, "BOTTOM: DROP", RGB(100, 100, 100), 1);
        }
    }
}

void draw_game() {
    // Start menu, or the attract cycle's SCORES page (the widget clears too)
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

    fb_clear(&fb, COLOR_BLACK);
    
    // Draw the playing field as background (used by PLAYING, PAUSED, GAME_OVER)
    draw_playing_field();
    
    // Handle pause screen overlay (Issue #11: semi-transparent overlay)
    if (current_screen == SCREEN_PAUSED) {
        modal_dialog_draw(&pause_dialog, &fb);
        return;
    }
    
    // Handle game over screen overlay (unified GameOverScreen component)
    if (current_screen == SCREEN_GAME_OVER) {
        TouchState go_st = touch_get_state(&touch);
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
    int lock_fd = acquire_instance_lock("tetris");
    if (lock_fd < 0) {
        fprintf(stderr, "tetris: another instance is already running\n");
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

    /* The music bed: the playlist named by /opt/arca/soundsets/tetris.sound,
     * with the four states and the hold/resume rules in common/audio_bed.c.
     * ⚠️ That file is the ONLY home for the paths — no set file means no music,
     * and tetris_music present but EMPTY is the off switch. */
    AudioBed bed;
    audio_bed_init(&bed, &audio, "tetris");

    // Gamepad init
    gamepad_init(&gamepad);
    
    srand(time(NULL));
    init_game();
    
    printf("Tetris game started!\n");
    
    /* ── main loop ───────────────────────────────────────────────────── */
    bool needs_redraw = true;
    while (running) {
        GameScreen prev_screen = current_screen;

        /* Snapshot game state before processing (for change detection) */
        int prev_px = game.current.x, prev_py = game.current.y;
        int prev_rot = game.current.rotation, prev_score = game.score;

        handle_input();
        update_game();
        hw_led_pulse_update(&led_pulse, get_time_ms());

        /* Dirty-flag: detect what changed */
        if (current_screen == SCREEN_PLAYING) {
            /* Redraw only when piece moved, rotated, score changed, or drop tick */
            if (game.current.x != prev_px || game.current.y != prev_py ||
                game.current.rotation != prev_rot || game.score != prev_score)
                needs_redraw = true;
            /* Also redraw on any input activity (soft-drop held, etc.) */
            TouchState ts = touch_get_state(&touch);
            if (ts.pressed) needs_redraw = true;
            for (int i = 0; i < BTN_ID_COUNT; i++) {
                if (input.buttons[i].pressed || input.buttons[i].held) {
                    needs_redraw = true; break;
                }
            }
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

        /* Bracket blink and attract phase changes arrive with no input. */
        if (current_screen == SCREEN_MENU && start_menu_needs_redraw(&menu))
            needs_redraw = true;
        /* The demo is gameplay: redraw every frame. */
        if (demo_running) needs_redraw = true;

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
         * the library measures for itself (common/audio_out.h). */
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
    
    printf("Tetris ended. Final score: %d\n", game.score);
    return 0;
}
