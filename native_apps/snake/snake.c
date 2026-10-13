/*
 * Snake Game - Native C Implementation
 * Optimized for 300MHz ARM with touchscreen
 * No browser overhead - direct framebuffer rendering
 * Features: LED effects, screen transitions
 * Supports keyboard, gamepad, and touch input via unified gamepad module
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

#define GRID_SIZE 20
#define MAX_SNAKE_LENGTH 400
#define INITIAL_SPEED 150000  // microseconds per frame

/* Difficulty scales the step interval, never game.speed itself: game.speed
 * stays the HARD interval (the game as it always played, 150 ms falling to
 * 50) and the sleep is game.speed * SPEED_Q8[d] >> 8.  EASY is half the
 * speed (interval x2), NORMAL three quarters (interval x4/3, 341/256 =
 * 1.332).  Multiply-and-shift because the Cortex-A8 has no divide. */
enum { DIFF_EASY, DIFF_NORMAL, DIFF_HARD, DIFF_COUNT };
static const char *const DIFF_NAMES[DIFF_COUNT] = { "EASY", "NORMAL", "HARD" };
static const int SPEED_Q8[DIFF_COUNT] = { 512, 341, 256 };

typedef enum {
    SCREEN_MENU,
    SCREEN_PLAYING,
    SCREEN_PAUSED,
    SCREEN_GAME_OVER
} GameScreen;

typedef enum {
    DIR_UP,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT
} Direction;

typedef struct {
    int x;
    int y;
} Point;

typedef struct {
    Point body[MAX_SNAKE_LENGTH];
    int length;
    Direction direction;
    Direction next_direction;
} Snake;

typedef struct {
    Point position;
    bool active;
} Food;

typedef struct {
    int score;
    int high_score;
    bool game_over;
    bool paused;
    int speed;
} GameState;

// LED effect state
typedef struct {
    bool active;
    int type;  // 0=none, 1=food, 2=grow, 3=death
    uint32_t start_time;
    int step;
} LEDEffect;

// Global variables
Framebuffer fb;
TouchInput touch;
GamepadManager gamepad;
InputState input;
Snake snake;
Food food;
GameState game;
LEDEffect led_effect;
int cell_size;
int grid_offset_x;
int grid_offset_y;
bool running = true;
GameScreen current_screen = SCREEN_MENU;
HighScoreTable hs_table;
static GameOverScreen gos;
Audio audio;

// Start menu (../common/start_menu.h): START, DIFFICULTY, EXIT
StartMenu menu;
int menu_start_idx, menu_diff_idx, menu_exit_idx;
int difficulty = DIFF_NORMAL;

/* Attract-cycle DEMO: the snake steers itself on the menu screen, silent and
 * LED-dark, at NORMAL speed, keeps no score and never reaches game over. */
static bool demo_running = false;
static bool demo_done = false;          /* demo_over already reported; stop stepping */
static uint32_t demo_next_ms = 0;
#define DEMO_MAX_LENGTH 20              /* the demo ends early at this length */
static bool demo_on(void) { return demo_running; }

// UI Buttons
Button menu_button;
Button exit_button;
ModalDialog pause_dialog;

// Function prototypes
void init_game();
void reset_game();
void spawn_food();
void update_snake();
void draw_playing_field();
void draw_game();
void handle_input();
void signal_handler(int sig);
void start_led_effect(int type);
void update_led_effects();

void signal_handler(int sig) {
    running = false;
}

void start_led_effect(int type) {
    led_effect.active = true;
    led_effect.type = type;
    led_effect.start_time = get_time_ms();
    led_effect.step = 0;
}

void update_led_effects() {
    if (!led_effect.active) return;
    
    uint32_t current_time = get_time_ms();
    uint32_t elapsed = current_time - led_effect.start_time;
    
    switch (led_effect.type) {
        case 1:  // Food effect - quick green flash (100ms)
            if (elapsed < 100) {
                hw_set_leds(HW_LED_COLOR_GREEN);
            } else {
                hw_set_leds(HW_LED_COLOR_OFF);
                led_effect.active = false;
            }
            break;
            
        case 2:  // Grow effect - rainbow cycle (150ms)
            if (elapsed < 150) {
                int length_mod = snake.length % 6;
                switch (length_mod) {
                    case 0: hw_set_leds(100, 0); break;    // Red
                    case 1: hw_set_leds(100, 50); break;   // Orange
                    case 2: hw_set_leds(100, 100); break;  // Yellow
                    case 3: hw_set_leds(0, 100); break;    // Green
                    case 4: hw_set_leds(50, 100); break;   // Cyan-ish
                    case 5: hw_set_leds(100, 30); break;   // Red-orange
                }
            } else {
                hw_set_leds(HW_LED_COLOR_OFF);
                led_effect.active = false;
            }
            break;
            
        case 3:  // Death effect - red pulsing (3 pulses, 600ms total)
            {
                int pulse_num = elapsed / 200;  // Each pulse is 200ms
                int pulse_phase = elapsed % 200;
                
                if (pulse_num < 3) {
                    if (pulse_phase < 100) {
                        hw_set_red_led(100);  // On for 100ms
                    } else {
                        hw_set_red_led(0);    // Off for 100ms
                    }
                } else {
                    hw_set_red_led(0);
                    led_effect.active = false;
                }
            }
            break;
    }
}

void init_game() {
    // Calculate grid dimensions.  The grid starts below the MENU/EXIT row, which
    // is SCREEN_SAFE_*-anchored, so its top must move with the measured touch
    // inset. Identical to the old 80 / fb.height - 120 when the inset is 0.
    grid_offset_y = LAYOUT_MENU_BTN_Y + BTN_MENU_HEIGHT + 20;
    int usable_width = fb.width - 40;
    int usable_height = SCREEN_VISIBLE_BOTTOM - grid_offset_y - 40;
    cell_size = (usable_width < usable_height ? usable_width : usable_height) / GRID_SIZE;
    grid_offset_x = (fb.width - (GRID_SIZE * cell_size)) / 2;

    // Initialize buttons — LAYOUT_* is SCREEN_SAFE_*-anchored, so the row moves
    // down with the measured touch inset instead of losing its top rows to it.
    button_init(&menu_button, LAYOUT_MENU_BTN_X, LAYOUT_MENU_BTN_Y,
                BTN_MENU_WIDTH, BTN_MENU_HEIGHT, "",
                BTN_MENU_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    button_init(&exit_button, LAYOUT_EXIT_BTN_X, LAYOUT_EXIT_BTN_Y,
                BTN_EXIT_WIDTH, BTN_EXIT_HEIGHT, "",
                BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR);
    /* Attract cycle: MENU -> DEMO -> SCORES, the DEMO drawn by draw_game(). */
    start_menu_init(&menu, "SNAKE",
                    "D-PAD/ARROWS: MOVE\n"
                    "EAT FOOD TO GROW", get_time_ms());
    menu_start_idx = start_menu_add_action(&menu, "START");
    menu_diff_idx  = start_menu_add_choice(&menu, "DIFFICULTY", DIFF_NAMES, DIFF_COUNT, DIFF_NORMAL);
    menu_exit_idx  = start_menu_add_action(&menu, "EXIT");
    start_menu_select(&menu, menu_start_idx, get_time_ms());
    start_menu_set_attract(&menu, true);
    modal_dialog_init(&pause_dialog, "PAUSED", NULL, 2);
    modal_dialog_set_button(&pause_dialog, 0, "RESUME", BTN_COLOR_PRIMARY, COLOR_WHITE);
    modal_dialog_set_button(&pause_dialog, 1, "EXIT", BTN_COLOR_DANGER, COLOR_WHITE);
    
    // Initialize LED effect state
    led_effect.active = false;
    led_effect.type = 0;
    
    hs_init(&hs_table, "snake");
    hs_load(&hs_table);
    game.high_score = hs_table.count > 0 ? hs_table.entries[0].score : 0;

    /* No virtual D-pad: handle_input() already hops the snake relative to the
     * head, so TouchRegions would be a redundant second input path — and the
     * four that used to be registered here covered the grid's top/bottom halves
     * *and* its left/right halves, so every in-grid tap asserted two directions
     * at once.  Deleted 2026-08-03; prefer a tap relative to the controlled
     * object over a pad. */

    reset_game();
}

/* Back to the start menu from play, pause or the game-over flow: drop any
 * running LED effect and park the LEDs, reset the round, and reopen the menu
 * (selection and difficulty kept).  The music bed follows current_screen. */
static void return_to_menu(void) {
    demo_running = false;
    led_effect.active = false;
    hw_leds_off();
    reset_game();
    game.high_score = hs_table.count > 0 ? hs_table.entries[0].score : 0;
    start_menu_reopen(&menu, get_time_ms());
    current_screen = SCREEN_MENU;
}

void reset_game() {
    // Initialize snake in the middle
    snake.length = 3;
    snake.direction = DIR_RIGHT;
    snake.next_direction = DIR_RIGHT;
    
    int start_x = GRID_SIZE / 2;
    int start_y = GRID_SIZE / 2;
    
    for (int i = 0; i < snake.length; i++) {
        snake.body[i].x = start_x - i;
        snake.body[i].y = start_y;
    }
    
    game.score = 0;
    game.game_over = false;
    game.paused = false;
    game.speed = INITIAL_SPEED;
    
    spawn_food();
}

void spawn_food() {
    bool valid = false;
    
    while (!valid) {
        food.position.x = rand() % GRID_SIZE;
        food.position.y = rand() % GRID_SIZE;
        
        valid = true;
        for (int i = 0; i < snake.length; i++) {
            if (snake.body[i].x == food.position.x && snake.body[i].y == food.position.y) {
                valid = false;
                break;
            }
        }
    }
    
    food.active = true;
}

void update_snake() {
    if (game.game_over || game.paused) return;
    
    // Update direction
    snake.direction = snake.next_direction;
    
    // Calculate new head position
    Point new_head = snake.body[0];
    
    switch (snake.direction) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
    }
    
    // Check wall collision
    if (new_head.x < 0 || new_head.x >= GRID_SIZE ||
        new_head.y < 0 || new_head.y >= GRID_SIZE) {
        if (demo_on()) {   /* the demo is never a game: no game over, no sound */
            demo_done = true;
            start_menu_demo_over(&menu);
            return;
        }
        game.game_over = true;
        current_screen = SCREEN_GAME_OVER;
        gameover_init(&gos, &fb, game.score, NULL, NULL, "SNAKE", &hs_table, &touch, &gamepad);
        /* ⚠️ gameover, not fail: snake has no LIVES, so a collision ends the RUN
         * and audio_fail() is the lost-a-life sound (../common/audio.h's enum).
         * It fires INSTEAD of fail, never after — fx_gameover is 1.19 s against
         * fail's 350 ms and the two would sum on the bus. */
        audio_gameover(&audio);
        start_led_effect(3);  // Start death effect
        return;
    }

    // Check self collision
    for (int i = 0; i < snake.length; i++) {
        if (snake.body[i].x == new_head.x && snake.body[i].y == new_head.y) {
            if (demo_on()) {
                demo_done = true;
                start_menu_demo_over(&menu);
                return;
            }
            game.game_over = true;
            current_screen = SCREEN_GAME_OVER;
            gameover_init(&gos, &fb, game.score, NULL, NULL, "SNAKE", &hs_table, &touch, &gamepad);
            audio_gameover(&audio);   /* the RUN, not a life — see wall collision */
            start_led_effect(3);  // Start death effect
            return;
        }
    }
    
    /* The cell the tail is about to leave.  If this move eats food we extend
     * into it, so it must be read before the body shifts over it. */
    Point vacated_tail = snake.body[snake.length - 1];

    // Move snake body
    for (int i = snake.length - 1; i > 0; i--) {
        snake.body[i] = snake.body[i - 1];
    }
    snake.body[0] = new_head;
    
    // Check food collision
    if (new_head.x == food.position.x && new_head.y == food.position.y) {
        game.score += 10;
        /* ⚠️ No audio_interrupt() before an effect — on the mix bus it means
         * "stop ALL voices", so this 120 ms two-note motif discarded a fanfare
         * that was still playing.  The rule and the measurement that produced it
         * (brick_breaker by ear, `.188` 2026-08-20) are in ../CLAUDE.md →
         * Mixing.  No counter sees this — a voice stopped early is not `lost`,
         * `drop` or `clip`.  The two tones still chain into one motif: that is
         * AUDIO_TONE_CHAIN_MS, not the interrupt. */
        if (!demo_on()) {
            audio_tone(&audio, 1800, 60);  // ti-
            audio_tone(&audio, 2400, 60);  // -ti
            start_led_effect(1);  // Start food effect (non-blocking)
        }
        
        if (snake.length < MAX_SNAKE_LENGTH) {
            /* The shift above only wrote body[1..length-1], so body[length] is
             * still whatever an earlier, longer tick left there — {0,0} on the
             * very first grow, because the struct is zero-initialised.  Just
             * incrementing length therefore published a detached cell at the
             * grid origin: it was drawn for one tick, counted by the
             * self-collision test, and ended the game if the head stepped on it.
             * The new segment belongs in the
             * cell the tail just vacated, which is what growing physically is.
             * It self-healed on the next tick's shift, which is why this looked
             * like a one-frame rendering glitch rather than a fatal one. */
            snake.body[snake.length] = vacated_tail;
            snake.length++;
            if (!demo_on()) start_led_effect(2);  // Start grow effect (non-blocking)
        }
        // Increase speed slightly
        if (game.speed > 50000) {
            game.speed -= 5000;
        }
        spawn_food();
        if (demo_on() && snake.length >= DEMO_MAX_LENGTH) {
            demo_done = true;
            start_menu_demo_over(&menu);
        }
    }
}

/* Demo AI: greedy toward the food, never reversing, one-step lookahead that
 * refuses a wall or any body cell (the tail included, as update_snake does)
 * when another move is safe.  Ties keep the current heading. */
static void demo_steer(void) {
    static const int DX[4] = { 0, 0, -1, 1 };   /* UP, DOWN, LEFT, RIGHT */
    static const int DY[4] = { -1, 1, 0, 0 };
    int best = -1, best_dist = 0;
    for (int k = 0; k < 4; k++) {
        /* heading first, so a tie keeps it */
        int d = (k == 0) ? (int)snake.direction : (k - 1 >= (int)snake.direction ? k : k - 1);
        if (d == ((int)snake.direction ^ 1)) continue;   /* never reverse */
        int nx = snake.body[0].x + DX[d];
        int ny = snake.body[0].y + DY[d];
        if (nx < 0 || nx >= GRID_SIZE || ny < 0 || ny >= GRID_SIZE) continue;
        bool hit = false;
        for (int i = 0; i < snake.length; i++)
            if (snake.body[i].x == nx && snake.body[i].y == ny) { hit = true; break; }
        if (hit) continue;
        int dist = abs(nx - food.position.x) + abs(ny - food.position.y);
        if (best < 0 || dist < best_dist) { best = d; best_dist = dist; }
    }
    if (best >= 0) snake.next_direction = (Direction)best;
}

/* One demo step per NORMAL-speed interval; the main loop paces at the active
 * frame rate meanwhile.  game.speed * 341 >> 8 is the NORMAL interval in us. */
static void demo_step(void) {
    if (!demo_on() || demo_done) return;
    uint32_t now = get_time_ms();
    if ((int32_t)(now - demo_next_ms) < 0) return;
    demo_next_ms = now + (uint32_t)((((long)game.speed * SPEED_Q8[DIFF_NORMAL]) >> 8) / 1000);
    demo_steer();
    update_snake();
}

/* The demo is SCREEN_MENU while the attract cycle is on its DEMO page.  Entering
 * it resets the field; START resets again, so nothing of the demo leaks. */
static void demo_sync(void) {
    bool want = (current_screen == SCREEN_MENU &&
                 start_menu_attract(&menu) == SM_ATTRACT_DEMO);
    if (want && !demo_running) {
        demo_done = false;
        reset_game();
        demo_next_ms = get_time_ms();
        led_effect.active = false;
        hw_leds_off();
    }
    demo_running = want;
}

void handle_input() {
    touch_poll(&touch);
    TouchState state = touch_get_state(&touch);
    uint32_t current_time = get_time_ms();

    // Poll gamepad/keyboard/touch through unified API
    gamepad_poll(&gamepad, &input, state.x, state.y, state.pressed);

    /* Hot-plug check: rescans only when /dev/input changed or a device went away */
    gamepad_tick(&gamepad, current_time);

    /* Start menu — ahead of the BACK rule below, so that BACK during the
     * SCORES page is swallowed like any other input there (the widget's
     * rule), and on the menu itself arrives as SM_EXIT.  The widget keeps
     * its own edges, so a button or finger still down from the screen
     * before acts only after a fresh press. */
    if (current_screen == SCREEN_MENU) {
        int r = start_menu_update(&menu, &input, state.x, state.y,
                                  state.pressed || state.held,
                                  &gamepad, &audio, current_time);
        if (r == SM_EXIT || r == menu_exit_idx) {
            fb_fade_out(&fb);
            running = false;
        } else if (r == menu_start_idx) {
            difficulty = start_menu_value(&menu, menu_diff_idx);
            if (difficulty < 0 || difficulty >= DIFF_COUNT) difficulty = DIFF_NORMAL;
            demo_running = false;
            reset_game();
            current_screen = SCREEN_PLAYING;
            /* Non-blocking: the hw_set_led() + usleep(100000) + hw_leds_off()
             * that was here froze the panel for 100 ms from inside
             * handle_input() — no touch poll, no redraw, and no audio_pump()
             * either, which is a starved stream as well as a dropped frame.
             * Effect 1 is this file's own 100 ms green flash, serviced by
             * update_led_effects() once per frame, so it is the same flash
             * without the freeze. */
            start_led_effect(1);
        } else {
            demo_sync();
        }
        return;
    }

    /* Game over: the shared flow owns all input (BACK included) and ends in
     * GAMEOVER_ACTION_MENU, handled in draw_game().  Ahead of the BACK rule so
     * the flow sees the press itself. */
    if (current_screen == SCREEN_GAME_OVER)
        return;

    // BTN_BACK during play/pause returns to the start menu
    if (input.buttons[BTN_ID_BACK].pressed) {
        return_to_menu();
        return;
    }
    
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
        
        // Touch controls for direction (relative to snake head)
        Point head = snake.body[0];
        int head_screen_x = grid_offset_x + head.x * cell_size + cell_size / 2;
        int head_screen_y = grid_offset_y + head.y * cell_size + cell_size / 2;
        
        int dx = state.x - head_screen_x;
        int dy = state.y - head_screen_y;
        if (abs(dx) > abs(dy)) {
            // Horizontal movement
            if (dx > 0 && snake.direction != DIR_LEFT) {
                snake.next_direction = DIR_RIGHT;
            } else if (dx < 0 && snake.direction != DIR_RIGHT) {
                snake.next_direction = DIR_LEFT;
            }
        } else {
            // Vertical movement
            if (dy > 0 && snake.direction != DIR_UP) {
                snake.next_direction = DIR_DOWN;
            } else if (dy < 0 && snake.direction != DIR_DOWN) {
                snake.next_direction = DIR_UP;
            }
        }
    }

    // Gamepad/keyboard direction input (use pressed edge, not held, to prevent rapid turns)
    if (input.buttons[BTN_ID_UP].pressed && snake.direction != DIR_DOWN) {
        snake.next_direction = DIR_UP;
    } else if (input.buttons[BTN_ID_DOWN].pressed && snake.direction != DIR_UP) {
        snake.next_direction = DIR_DOWN;
    } else if (input.buttons[BTN_ID_LEFT].pressed && snake.direction != DIR_RIGHT) {
        snake.next_direction = DIR_LEFT;
    } else if (input.buttons[BTN_ID_RIGHT].pressed && snake.direction != DIR_LEFT) {
        snake.next_direction = DIR_RIGHT;
    }
}

void draw_playing_field() {
    if (demo_on()) {
        text_draw_centered(&fb, fb.width / 2, SCREEN_VISIBLE_TOP + 20, "DEMO", COLOR_CYAN, 2);
    } else {
    // Draw HUD
    char score_text[32];
    snprintf(score_text, sizeof(score_text), "SCORE: %d", game.score);
    text_draw_centered(&fb, fb.width / 2, 28, score_text, COLOR_WHITE, 2);
    
    char high_text[32];
    snprintf(high_text, sizeof(high_text), "HIGH: %d", game.high_score);
    text_draw_centered(&fb, fb.width / 2, 53, high_text, COLOR_YELLOW, 2);
    
    // Draw menu and exit buttons
    draw_menu_button(&fb, &menu_button);
    draw_exit_button(&fb, &exit_button);
    }

    // Draw grid border
    fb_draw_rect(&fb, grid_offset_x - 2, grid_offset_y - 2,
                 GRID_SIZE * cell_size + 4, GRID_SIZE * cell_size + 4, COLOR_WHITE);
    
    // Draw snake
    for (int i = 0; i < snake.length; i++) {
        int x = grid_offset_x + snake.body[i].x * cell_size;
        int y = grid_offset_y + snake.body[i].y * cell_size;
        
        uint32_t color = (i == 0) ? COLOR_GREEN : RGB(0, 200, 0);
        fb_fill_rect(&fb, x + 1, y + 1, cell_size - 2, cell_size - 2, color);
    }
    
    // Draw food
    if (food.active) {
        int fx = grid_offset_x + food.position.x * cell_size;
        int fy = grid_offset_y + food.position.y * cell_size;
        fb_fill_circle(&fb, fx + cell_size / 2, fy + cell_size / 2, cell_size / 2 - 2, COLOR_RED);
    }
    
    // Draw controls hint
    if (demo_on()) return;
    if (!input.gamepad_connected && !input.keyboard_connected)
        fb_draw_text(&fb, 10, fb.height - 25, "TAP DIRECTION TO MOVE", RGB(100, 100, 100), 1);
    else
        fb_draw_text(&fb, 10, fb.height - 25, "ARROWS/D-PAD: MOVE  ESC: PAUSE", RGB(100, 100, 100), 1);
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

    // Clear screen
    fb_clear(&fb, COLOR_BLACK);
    
    // Draw the playing field as background (used by PLAYING, PAUSED, GAME_OVER)
    draw_playing_field();
    if (demo_on()) return;

    // Handle pause screen overlay
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
    
    // Allow custom devices
    if (argc > 1) fb_device = argv[1];
    if (argc > 2) touch_device = argv[2];
    
    // Singleton guard — prevent duplicate instances
    int lock_fd = acquire_instance_lock("snake");
    if (lock_fd < 0) {
        fprintf(stderr, "snake: another instance is already running\n");
        return 1;
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize hardware control
    hw_init();
    hw_set_backlight(100);
    hw_leds_off();  // Start with LEDs off
    audio_init(&audio);  // Initialize audio (non-fatal if unavailable)

    /* The music bed: the playlist named by /opt/arca/soundsets/snake.sound,
     * with the four states and the hold/resume rules in common/audio_bed.c.
     * ⚠️ That file is the ONLY home for the paths — no set file means no music,
     * and snake_music present but EMPTY is the off switch. */
    AudioBed bed;
    audio_bed_init(&bed, &audio, "snake");
    
    // Initialize framebuffer
    /* Pin 32bpp — /dev/fb0 keeps whatever ran last (see fb_set_bpp). */
    fb_set_bpp(fb_device, 32);

    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, "Failed to initialize framebuffer\n");
        return 1;
    }
    
    // Initialize touch input
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, "Failed to initialize touch input\n");
        fb_close(&fb);
        return 1;
    }
    
    // Set screen size for coordinate scaling
    touch_set_screen_size(&touch, fb.width, fb.height);
    
    // Seed random number generator
    srand(time(NULL));
    
    // Initialize game
    init_game();
    
    printf("Snake game started! Touch screen to play.\n");
    printf("Press Ctrl+C to exit.\n");
    
    // Gamepad init
    gamepad_init(&gamepad);

    // Game loop
    bool needs_redraw = true;
    while (running) {
        GameScreen prev_screen = current_screen;
        handle_input();
        if (current_screen == SCREEN_PLAYING)
            update_snake();
        demo_step();
        update_led_effects();

        /* Dirty-flag: active gameplay always redraws; static screens only on changes */
        if (current_screen == SCREEN_PLAYING || demo_on()) {
            needs_redraw = true;  /* snake moves every tick */
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

        /* Adaptive sleep: game.speed during play, idle polling on static screens.
         *
         * ⚠️ Snake is the one game whose play sleep IS its step interval — the
         * snake advances once per iteration, so game.speed (INITIAL_SPEED
         * 150 ms falling to 50, scaled up by the difficulty's SPEED_Q8 to as
         * much as 300 ms on EASY) cannot be shortened to service the stream
         * without making the snake faster.  It starves BEFORE it speeds up
         * rather than after: 150 ms is ~3x the ~55 ms service ceiling, so a
         * single usleep() here would run the device dry twice per step at
         * level 1 and stop doing so as the game got harder.  So the wait is
         * BROKEN INTO service-sized pieces instead of shortened — same step
         * interval, stream fed throughout.  audio_cont_service_interval_us()
         * is 0 when there is no continuous stream (audio disabled, or the
         * handover failed), and then this is one usleep() exactly as before.
         * ⚠️ HALF the published interval, not all of it: that figure is a
         * CEILING (it already carries half a period of margin — see
         * ../common/audio_out.h), so sleeping the whole of it and then pumping
         * puts the service at its edge.  Half lands snake at ~27 ms, the same
         * neighbourhood as the other six games' FRAME_DELAY_ACTIVE_US.
         * ⚠️ But HALF is a choice, never measured — neither the whole interval
         * nor any other divisor has been tried, and "it sounds right" is the whole
         * of the evidence.  Note the ceiling's own half-period margin guards period
         * granularity, not the per-iteration wakeup jitter this halving covers, so
         * the two margins are not obviously redundant. */
        long wait_us = (current_screen == SCREEN_PLAYING)
                     ? (((long)game.speed * SPEED_Q8[difficulty]) >> 8)
                     : ((needs_redraw || audio_pump_active(&audio))
                        ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
        long slice_us = audio_cont_service_interval_us(&audio) / 2;
        while (slice_us > 0 && wait_us > slice_us) {
            usleep((useconds_t)slice_us);
            wait_us -= slice_us;
            audio_pump(&audio);
        }
        usleep((useconds_t)wait_us);
        needs_redraw = false;
    }
    
    // Cleanup
    hw_leds_off();
    audio_close(&audio);
    gamepad_close(&gamepad);
    touch_close(&touch);
    fb_close(&fb);
    
    printf("Snake game ended. Final score: %d\n", game.score);
    return 0;
}
