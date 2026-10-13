/*
 * Frogger Game - Native C Implementation
 * Optimized for 300MHz ARM with touchscreen
 * No browser overhead - direct framebuffer rendering
 * Features: LED effects, screen transitions, high scores
 * Supports keyboard, gamepad, and touch input via unified gamepad module
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
#include "../common/logger.h"
#include "../common/gamepad.h"
#include "../common/start_menu.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NUM_ROWS          13
#define NUM_GOALS         5
#define HUD_HEIGHT        70
#define MAX_OBJECTS_PER_LANE 6
#define NUM_LANE_CONFIGS  10
/* Animation lengths run on elapsed time (update_game's dt), not frames.  They
 * were authored as 8 and 12 frames at the nominal 30 fps. */
#define HOP_DURATION_MS   267.0f  /* hop animation */
#define TIMER_MAX         30.0f   /* seconds per life attempt */
#define DEATH_ANIM_MS     400.0f  /* death splash before respawn / game over */
#define INITIAL_LIVES     3
#define HOP_COOLDOWN_MS   200

/* HUD metrics.  The lives icons are 12 px squares on an 18 px pitch; the timer
 * bar sits between the MENU and EXIT buttons with this much clearance either
 * side of it. */
#define LIFE_ICON_SIZE    12
#define LIFE_ICON_PITCH   18
#define TIMER_BAR_HEIGHT  12
#define TIMER_BAR_GAP     10

/* ─── Color palette ─────────────────────────────────────────────────────── */

/* Environment */
#define COLOR_GRASS_DARK    RGB(34, 120, 34)
#define COLOR_GRASS_LIGHT   RGB(50, 160, 50)
#define COLOR_WATER_DARK    RGB(20, 50, 140)
#define COLOR_WATER_LIGHT   RGB(40, 80, 180)
#define COLOR_WATER_WAVE    RGB(60, 110, 200)
#define COLOR_ROAD_DARK     RGB(50, 50, 55)
#define COLOR_ROAD_LINE     RGB(200, 200, 60)

/* Frog */
#define COLOR_FROG_BODY     RGB(30, 180, 30)
#define COLOR_FROG_DARK     RGB(20, 120, 20)
#define COLOR_FROG_BELLY    RGB(150, 220, 100)
#define COLOR_FROG_EYE_W    RGB(255, 255, 255)
#define COLOR_FROG_EYE_B    RGB(0, 0, 0)

/* Vehicles */
#define COLOR_CAR_BLUE      RGB(40, 80, 200)
#define COLOR_CAR_YELLOW    RGB(220, 200, 40)
#define COLOR_TRUCK_PURPLE  RGB(120, 40, 160)
#define COLOR_TRUCK_ORANGE  RGB(220, 120, 30)
#define COLOR_RACE_CAR_CLR  RGB(255, 60, 60)
#define COLOR_WHEEL         RGB(30, 30, 30)
#define COLOR_WINDOW        RGB(150, 200, 240)

/* River objects */
#define COLOR_LOG_DARK      RGB(100, 60, 20)
#define COLOR_LOG_LIGHT     RGB(140, 90, 40)
#define COLOR_LOG_BARK      RGB(80, 45, 15)
#define COLOR_TURTLE_SHELL  RGB(50, 100, 50)
#define COLOR_TURTLE_DARK   RGB(30, 70, 30)
#define COLOR_TURTLE_HEAD   RGB(80, 140, 80)

/* Goal */
#define COLOR_LILYPAD       RGB(30, 140, 50)
#define COLOR_LILYPAD_LIGHT RGB(60, 180, 80)
#define COLOR_GOAL_FROG     RGB(80, 200, 80)

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    SCREEN_MENU,
    SCREEN_PLAYING,
    SCREEN_PAUSED,
    SCREEN_GAME_OVER
} GameScreen;

typedef enum {
    LANE_ROAD,
    LANE_RIVER
} LaneType;

typedef enum {
    OBJ_CAR,
    OBJ_TRUCK,
    OBJ_RACE_CAR,
    OBJ_LOG_SHORT,
    OBJ_LOG_MED,
    OBJ_LOG_LONG,
    OBJ_TURTLE_2,
    OBJ_TURTLE_3
} ObjectType;

typedef enum {
    DEATH_VEHICLE,
    DEATH_WATER,
    DEATH_TIMEOUT,
    DEATH_OFFSCREEN
} DeathType;

typedef struct {
    ObjectType type;
    float x;
    int width_cells;
    uint32_t color;
} LaneObject;

typedef struct {
    int row;
    LaneType type;
    int direction;   /* -1 = left, +1 = right */
    float cells_per_s;  /* level-1 speed, unsigned; see lane_speed_px_s() */
    LaneObject objects[MAX_OBJECTS_PER_LANE];
    int object_count;
} Lane;

typedef struct {
    int col, row;
    float ride_offset;
    int target_col, target_row;
    float hop_progress;
    bool hopping;
    bool alive;
    int facing;  /* 0=up, 1=down, 2=left, 3=right */
} Frog;

typedef struct {
    int score, lives, level;
    bool goals_reached[NUM_GOALS];
    int goals_filled;
    float timer;
    int highest_row;
} GameStateData;

typedef struct {
    int row;
    LaneType type;
    ObjectType obj_type;
    int direction;
    float cells_per_s;
    int obj_width_cells;
    int spacing_cells;
    uint32_t color;
} LaneConfig;

typedef struct {
    bool active;
    int type;   /* 0=none, 1=goal, 2=death, 3=level_complete */
    uint32_t start_time;
} LEDEffect;

/* ═══════════════════════════════════════════════════════════════════════════
 * Global Variables (project convention)
 * ═══════════════════════════════════════════════════════════════════════════ */

Framebuffer fb;
TouchInput touch;
GamepadManager gamepad;
InputState input;
Audio audio;
HighScoreTable hs_table;
static GameOverScreen gos;
bool running = true;
GameScreen current_screen = SCREEN_MENU;

/* Start menu (../common/start_menu.h): START, DIFFICULTY, EXIT.  Difficulty
 * scales every lane's speed on top of the level ramp; NORMAL is the game as
 * it always played, EASY a quarter slower, HARD a quarter faster. */
enum { DIFF_EASY, DIFF_NORMAL, DIFF_HARD, DIFF_COUNT };
static const char *const DIFF_NAMES[DIFF_COUNT] = { "EASY", "NORMAL", "HARD" };
static const float DIFF_SPEED[DIFF_COUNT] = { 0.75f, 1.0f, 1.25f };
static StartMenu menu;
static int menu_start_idx, menu_diff_idx, menu_exit_idx;
static int difficulty = DIFF_NORMAL;

/* Attract-cycle DEMO: SCREEN_MENU while the widget is on its DEMO page.  The AI
 * plays the real game state at NORMAL speed, silent and LED-dark, keeps no
 * score and never reaches SCREEN_GAME_OVER; it ends when all lives are lost. */
static bool demo_running = false;
static bool demo_on(void) { return demo_running; }

/* Every sound the gameplay makes goes through these, so the demo is silent. */
static Audio *sfx_audio(void) { return &audio; }
static void sfx_beep(void)     { if (!demo_on()) audio_beep(sfx_audio()); }
static void sfx_success(void)  { if (!demo_on()) audio_success(sfx_audio()); }
static void sfx_fail(void)     { if (!demo_on()) audio_fail(sfx_audio()); }
static void sfx_gameover(void) { if (!demo_on()) audio_gameover(sfx_audio()); }

static Lane lanes[NUM_LANE_CONFIGS];
static Frog frog;
static GameStateData state;
static LEDEffect led_effect;

static float death_anim_ms = 0;   /* elapsed play time since kill_frog() */
static bool death_anim_active = false;
static bool game_over_pending = false;
static uint32_t current_frame = 0;
static uint32_t last_frame_ms = 0;
static uint32_t last_hop_ms = 0;

/* Grid dimensions (computed once at init) */
static int cell_size;
static int num_cols;
static int grid_width;
static int grid_offset_x;
static int grid_offset_y;
static int goal_cols[NUM_GOALS];

/* HUD band height, computed at init: HUD_HEIGHT plus the measured touch inset,
 * because the MENU/EXIT row inside it is SCREEN_SAFE_*-anchored.  The band has to
 * grow with the row — at only 70 px the timer bar already grazes the buttons, so
 * moving them without growing the band would put the bar through their middle and
 * push them into the playfield.  Equals HUD_HEIGHT when the inset is 0. */
static int hud_height;

/* UI Buttons */
Button menu_button;
Button exit_button;
ModalDialog pause_dialog;

/* ─── Lane configuration table ──────────────────────────────────────────── */

/* Lane speeds are in cells per second, so they hold in every orientation and at
 * any frame rate.  They were authored as px/frame at 28 px cells and 30 fps —
 * the cell compute_grid() gives on the 800x453 landscape screen with a
 * calibrated touch inset — and LANE_PXF converts that authored figure exactly,
 * keeping the original feel: cells/s = px/frame * 30 / 28. */
#define LANE_PXF(px_per_frame) ((px_per_frame) * 30.0f / 28.0f)

static const LaneConfig lane_configs[NUM_LANE_CONFIGS] = {
    /* River lanes (rows 1-5) */
    { 1,  LANE_RIVER, OBJ_LOG_LONG,   +1, LANE_PXF(0.8f), 4, 6, COLOR_LOG_LIGHT    },
    { 2,  LANE_RIVER, OBJ_TURTLE_3,   -1, LANE_PXF(0.6f), 3, 5, COLOR_TURTLE_SHELL },
    { 3,  LANE_RIVER, OBJ_LOG_MED,    +1, LANE_PXF(1.0f), 3, 5, COLOR_LOG_LIGHT    },
    { 4,  LANE_RIVER, OBJ_TURTLE_2,   -1, LANE_PXF(0.7f), 2, 6, COLOR_TURTLE_SHELL },
    { 5,  LANE_RIVER, OBJ_LOG_SHORT,  +1, LANE_PXF(0.9f), 2, 4, COLOR_LOG_LIGHT    },
    /* Road lanes (rows 7-11) */
    { 7,  LANE_ROAD,  OBJ_RACE_CAR,   -1, LANE_PXF(2.0f), 1, 8, COLOR_RACE_CAR_CLR },
    { 8,  LANE_ROAD,  OBJ_CAR,        +1, LANE_PXF(1.2f), 1, 6, COLOR_CAR_BLUE     },
    { 9,  LANE_ROAD,  OBJ_TRUCK,      -1, LANE_PXF(0.8f), 2, 7, COLOR_TRUCK_PURPLE },
    { 10, LANE_ROAD,  OBJ_CAR,        +1, LANE_PXF(1.4f), 1, 5, COLOR_CAR_YELLOW   },
    { 11, LANE_ROAD,  OBJ_TRUCK,      -1, LANE_PXF(0.9f), 2, 6, COLOR_TRUCK_ORANGE },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Function Prototypes
 * ═══════════════════════════════════════════════════════════════════════════ */

static void signal_handler(int sig);
static void init_game(void);
static void init_buttons(void);
static void init_lanes(void);
static void reset_game(void);
static void reset_frog_position(void);
static void compute_grid(void);
static void spawn_lane_objects(Lane *lane, const LaneConfig *cfg);
static void handle_input(void);
static void update_game(void);
static void update_lanes(float dt);
static void update_hop(float dt);
static void update_frog_ride(float dt);
static void update_timer(float dt);
static void update_death_animation(float dt);
static void check_road_collision(void);
static void check_river_collision(void);
static void check_goal_reached(void);
static void hop_frog(int drow, int dcol);
static void kill_frog(DeathType cause);
static void advance_level(void);
static int  lane_index_for_row(int row);
static void draw_all(void);
static void draw_playing_field(void);
static void draw_hud(void);
static void draw_timer_bar(void);
static void draw_life_icons(int x, int y);
static void draw_water_lane(int row);
static void draw_road_lane(int row);
static void draw_grass_lane(int row);
static void draw_goal_zone(void);
static void draw_frog_sprite(int screen_x, int screen_y);
static void draw_car(int sx, int sy, uint32_t color);
static void draw_truck(int sx, int sy, uint32_t color);
static void draw_race_car(int sx, int sy, uint32_t color);
static void draw_log_sprite(int sx, int sy, int width_cells);
static void draw_turtle_group(int sx, int sy, int count);
static void draw_death_splash(int screen_x, int screen_y, float elapsed_ms);
static void draw_lane_objects(int lane_idx);
static void start_led_effect(int type);
static void update_led_effects(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void signal_handler(int sig) {
    (void)sig;
    running = false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LED Effects (non-blocking, same pattern as snake.c)
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
    case 1: /* Goal reached: green flash 200ms */
        if (elapsed < 200)
            hw_set_leds(HW_LED_COLOR_GREEN);
        else {
            hw_leds_off();
            led_effect.active = false;
        }
        break;

    case 2: /* Death: red pulse 3x200ms */
    {
        int pulse = (int)(elapsed / 200);
        int phase = (int)(elapsed % 200);
        if (pulse < 3)
            hw_set_red_led(phase < 100 ? 100 : 0);
        else {
            hw_leds_off();
            led_effect.active = false;
        }
    }
    break;

    case 3: /* Level complete: green/yellow alternation 600ms */
    {
        int phase = (int)((elapsed / 150) % 2);
        if (elapsed < 600) {
            if (phase == 0) hw_set_leds(HW_LED_COLOR_GREEN);
            else hw_set_leds(HW_LED_COLOR_YELLOW);
        } else {
            hw_leds_off();
            led_effect.active = false;
        }
    }
    break;

    default:
        led_effect.active = false;
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Grid Computation
 * ═══════════════════════════════════════════════════════════════════════════ */

static void compute_grid(void) {
    hud_height = SCREEN_SAFE_TOP + HUD_HEIGHT;
    int available_height = (int)fb.height - hud_height;
    cell_size = available_height / NUM_ROWS;
    if (cell_size < 8) cell_size = 8;
    num_cols = (int)fb.width / cell_size;
    if (num_cols < NUM_GOALS + 2) num_cols = NUM_GOALS + 2;
    grid_width = num_cols * cell_size;
    grid_offset_x = ((int)fb.width - grid_width) / 2;
    grid_offset_y = hud_height;

    /* Compute goal slot positions */
    if (num_cols <= NUM_GOALS * 2) {
        for (int i = 0; i < NUM_GOALS; i++)
            goal_cols[i] = (i * num_cols + num_cols / 2) / NUM_GOALS;
    } else {
        int spacing = num_cols / (NUM_GOALS + 1);
        for (int i = 0; i < NUM_GOALS; i++)
            goal_cols[i] = spacing * (i + 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lane Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static int lane_index_for_row(int row) {
    for (int i = 0; i < NUM_LANE_CONFIGS; i++)
        if (lane_configs[i].row == row) return i;
    return -1;
}

static void spawn_lane_objects(Lane *lane, const LaneConfig *cfg) {
    int obj_px  = cfg->obj_width_cells * cell_size;
    int gap_px  = cfg->spacing_cells   * cell_size;
    int stride  = obj_px + gap_px;
    lane->object_count = 0;
    if (stride <= 0) return;

    for (float x = 0; x < grid_width + stride; x += (float)stride) {
        if (lane->object_count >= MAX_OBJECTS_PER_LANE) break;
        LaneObject *obj = &lane->objects[lane->object_count++];
        obj->type        = cfg->obj_type;
        obj->x           = x;
        obj->width_cells = cfg->obj_width_cells;
        obj->color       = cfg->color;
    }
}

static void init_lanes(void) {
    for (int i = 0; i < NUM_LANE_CONFIGS; i++) {
        const LaneConfig *cfg = &lane_configs[i];
        Lane *lane        = &lanes[i];
        lane->row         = cfg->row;
        lane->type        = cfg->type;
        lane->direction   = cfg->direction;
        lane->cells_per_s = cfg->cells_per_s;
        spawn_lane_objects(lane, cfg);
    }
}

/* Signed lane velocity in px/s at the current level.  The one home for the
 * level multiplier, shared by the objects (update_lanes) and the frog riding
 * them (update_frog_ride) so the two can never drift apart.  Cells/s times the
 * live cell_size: a speed in cells is the same in portrait and landscape. */
static float lane_speed_px_s(const Lane *lane) {
    float speed_mult = 1.0f + (state.level - 1) * 0.15f;
    if (speed_mult > 2.5f) speed_mult = 2.5f;
    speed_mult *= DIFF_SPEED[difficulty];
    return lane->cells_per_s * (float)cell_size * speed_mult * lane->direction;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Game Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static void init_buttons(void) {
    /* LAYOUT_* is SCREEN_SAFE_*-anchored, so the row moves down with the measured
     * touch inset instead of losing its top rows to it.  compute_grid() grows the
     * HUD band to match — see hud_height. */
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

static void reset_frog_position(void) {
    frog.row          = 12;
    frog.col          = num_cols / 2;
    frog.ride_offset  = 0;
    frog.hopping      = false;
    frog.alive        = true;
    frog.facing       = 0;
    frog.hop_progress = 0;
    frog.target_row   = frog.row;
    frog.target_col   = frog.col;
    state.timer       = TIMER_MAX;
    state.highest_row = 12;
}

static void reset_game(void) {
    state.score        = 0;
    state.lives        = INITIAL_LIVES;
    state.level        = 1;
    state.goals_filled = 0;
    for (int i = 0; i < NUM_GOALS; i++)
        state.goals_reached[i] = false;

    death_anim_active  = false;
    death_anim_ms      = 0;
    game_over_pending  = false;
    led_effect.active  = false;
    current_frame      = 0;

    init_lanes();
    reset_frog_position();
}

static void init_game(void) {
    compute_grid();
    init_buttons();
    hs_init(&hs_table, "frogger");
    hs_load(&hs_table);

    /* No virtual D-pad TouchRegions.  handle_input() already hops the
     * frog from a plain tap anywhere in the play area, relative to the frog's
     * own position, so the regions were a redundant second path: they made the
     * frog jump on its own, and back when gamepad.c never cleared a region's
     * .held they also latched the on-screen overlay permanently
     * highlighted.  The whole playfield is the tap target now.  ⚠️ The latch
     * itself is fixed — poll_touch() writes only the per-frame `derived` array —
     * so what still argues against regions here is the redundancy, not the latch. */

    /* Attract cycle MENU -> DEMO -> SCORES, drawn by draw_all(). */
    start_menu_init(&menu, "FROGGER",
                    "TAP AHEAD OF THE FROG TO HOP\n"
                    "OR USE A D-PAD / ARROW KEYS\n"
                    "CROSS ROAD AND RIVER\n"
                    "REACH THE LILY PADS", get_time_ms());
    menu_start_idx = start_menu_add_action(&menu, "START");
    menu_diff_idx  = start_menu_add_choice(&menu, "DIFFICULTY", DIFF_NAMES, DIFF_COUNT, DIFF_NORMAL);
    menu_exit_idx  = start_menu_add_action(&menu, "EXIT");
    start_menu_select(&menu, menu_start_idx, get_time_ms());
    start_menu_set_attract(&menu, true);

    reset_game();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Level Progression
 * ═══════════════════════════════════════════════════════════════════════════ */

static void advance_level(void) {
    state.level++;
    state.score += 500;

    for (int i = 0; i < NUM_GOALS; i++)
        state.goals_reached[i] = false;
    state.goals_filled = 0;

    init_lanes();
    reset_frog_position();

    start_led_effect(3);
    sfx_success();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Frog Movement
 * ═══════════════════════════════════════════════════════════════════════════ */

static void hop_frog(int drow, int dcol) {
    int new_row = frog.row + drow;
    int new_col = frog.col + dcol;

    if (new_col < 0 || new_col >= num_cols) return;
    if (new_row < 0 || new_row > 12) return;

    frog.target_row   = new_row;
    frog.target_col   = new_col;
    frog.hop_progress = 0.0f;
    frog.hopping      = true;
    frog.ride_offset  = 0;

    if      (drow < 0) frog.facing = 0;
    else if (drow > 0) frog.facing = 1;
    else if (dcol < 0) frog.facing = 2;
    else               frog.facing = 3;

    if (drow < 0 && new_row < state.highest_row) {
        state.score += 10;
        state.highest_row = new_row;
    }

    sfx_beep();
    last_hop_ms = get_time_ms();
}

static void update_hop(float dt) {
    if (!frog.hopping) return;
    frog.hop_progress += dt * 1000.0f / HOP_DURATION_MS;
    if (frog.hop_progress >= 1.0f) {
        frog.row          = frog.target_row;
        frog.col          = frog.target_col;
        frog.hop_progress = 0;
        frog.hopping      = false;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Object Movement
 * ═══════════════════════════════════════════════════════════════════════════ */

/* No substeps: dt is clamped to 0.1 s, so the fastest lane (2.14 cells/s at the
 * 2.5x level cap = 5.4 cells/s) moves at most 0.54 cells per update, and the
 * frog sits still while it is collision-tested.  An overlap can only be skipped
 * by a step longer than object + frog hitbox (>= 1 + 0.75 cells). */
static void update_lanes(float dt) {
    for (int i = 0; i < NUM_LANE_CONFIGS; i++) {
        Lane *lane = &lanes[i];
        float step = lane_speed_px_s(lane) * dt;

        for (int j = 0; j < lane->object_count; j++) {
            LaneObject *obj = &lane->objects[j];
            obj->x += step;
            int obj_w = obj->width_cells * cell_size;

            if (lane->direction > 0 && obj->x > grid_width)
                obj->x -= grid_width + obj_w;
            else if (lane->direction < 0 && obj->x + obj_w < 0)
                obj->x += grid_width + obj_w;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Frog Riding on River Objects
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_frog_ride(float dt) {
    if (frog.hopping) return;
    if (frog.row < 1 || frog.row > 5) return;

    int idx = lane_index_for_row(frog.row);
    if (idx < 0) return;

    frog.ride_offset += lane_speed_px_s(&lanes[idx]) * dt;

    while (frog.ride_offset >= cell_size) {
        frog.col++;
        frog.ride_offset -= cell_size;
    }
    while (frog.ride_offset <= -cell_size) {
        frog.col--;
        frog.ride_offset += cell_size;
    }

    float frog_pixel_x = frog.col * cell_size + frog.ride_offset;
    if (frog_pixel_x < -cell_size || frog_pixel_x > grid_width)
        kill_frog(DEATH_OFFSCREEN);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Collision Detection
 * ═══════════════════════════════════════════════════════════════════════════ */

static void check_road_collision(void) {
    if (frog.row < 7 || frog.row > 11) return;
    if (frog.hopping || !frog.alive) return;

    int idx = lane_index_for_row(frog.row);
    if (idx < 0) return;
    Lane *lane = &lanes[idx];

    int p = cell_size / 8;
    if (p < 1) p = 1;
    float frog_x = frog.col * cell_size + frog.ride_offset + p;
    float frog_w = cell_size - p * 2;

    for (int i = 0; i < lane->object_count; i++) {
        float obj_x = lane->objects[i].x;
        float obj_w = lane->objects[i].width_cells * cell_size;
        if (frog_x < obj_x + obj_w && frog_x + frog_w > obj_x) {
            kill_frog(DEATH_VEHICLE);
            return;
        }
    }
}

static void check_river_collision(void) {
    if (frog.row < 1 || frog.row > 5) return;
    if (frog.hopping || !frog.alive) return;

    int idx = lane_index_for_row(frog.row);
    if (idx < 0) return;
    Lane *lane = &lanes[idx];

    int p = cell_size / 8;
    if (p < 1) p = 1;
    float frog_x = frog.col * cell_size + frog.ride_offset + p;
    float frog_w = cell_size - p * 2;
    bool on_object = false;

    for (int i = 0; i < lane->object_count; i++) {
        float obj_x = lane->objects[i].x;
        float obj_w = lane->objects[i].width_cells * cell_size;
        if (frog_x < obj_x + obj_w && frog_x + frog_w > obj_x) {
            on_object = true;
            break;
        }
    }

    if (!on_object)
        kill_frog(DEATH_WATER);
}

static void check_goal_reached(void) {
    if (frog.row != 0 || frog.hopping || !frog.alive) return;

    for (int i = 0; i < NUM_GOALS; i++) {
        if (frog.col == goal_cols[i] && !state.goals_reached[i]) {
            state.goals_reached[i] = true;
            state.goals_filled++;
            state.score += 50;
            state.score += (int)state.timer * 10;

            sfx_success();
            start_led_effect(1);

            if (state.goals_filled >= NUM_GOALS)
                advance_level();
            else
                reset_frog_position();
            return;
        }
    }

    /* Landed on row 0 but NOT on a goal slot */
    kill_frog(DEATH_WATER);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Death and Respawn
 * ═══════════════════════════════════════════════════════════════════════════ */

static void kill_frog(DeathType cause) {
    (void)cause;  /* reserved for future death-type-specific animation */
    if (!frog.alive) return;
    frog.alive = false;
    state.lives--;

    start_led_effect(2);
    /* ⚠️ **The RUN ending and ONE life ending must not be the same noise** —
     * that is the whole reason audio_gameover() exists, and this site played
     * audio_fail() for both.  frogger is the only one of the seven games with
     * LIVES, and unlike platformer's player_die() it already knows which
     * happened — state.lives is decremented two lines up — so it branches HERE
     * rather than deferring to the end of the death animation.  It fires INSTEAD
     * of fail, never after: fx_gameover is 1.19 s against fail's 350 ms and the
     * two would sum on the bus.
     * Either sound lands over near-silence, because death_anim_active is the
     * bed's want_HOLD (see the main loop) for the whole animation. */
    if (state.lives <= 0)
        sfx_gameover();
    else
        sfx_fail();

    death_anim_ms     = 0;
    death_anim_active = true;

    if (state.lives <= 0)
        game_over_pending = true;
}

static void update_death_animation(float dt) {
    if (!death_anim_active) return;
    death_anim_ms += dt * 1000.0f;

    if (death_anim_ms >= DEATH_ANIM_MS) {
        death_anim_active = false;
        hw_leds_off();

        if (game_over_pending && demo_on()) {
            /* The demo is not a game: no GAME OVER, no score.  The frog stays
             * dead until the widget leaves the DEMO page. */
            game_over_pending = false;
            start_menu_demo_over(&menu);
        } else if (game_over_pending) {
            game_over_pending = false;
            current_screen = SCREEN_GAME_OVER;
            gameover_init(&gos, &fb, state.score, NULL, NULL, "FROGGER",
                          &hs_table, &touch, &gamepad);
        } else {
            reset_frog_position();
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Timer
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_timer(float dt) {
    if (!frog.alive || frog.hopping) return;
    state.timer -= dt;
    if (state.timer <= 0) {
        state.timer = 0;
        kill_frog(DEATH_TIMEOUT);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Input Handling
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Back to the start menu from play, pause or game over, undoing what START set up. */
static void return_to_menu(void) {
    led_effect.active = false;
    hw_leds_off();
    death_anim_active = false;
    game_over_pending = false;
    start_menu_reopen(&menu, get_time_ms());
    current_screen = SCREEN_MENU;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Attract-cycle DEMO: the AI
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEMO_LAND_S   (HOP_DURATION_MS / 1000.0f)   /* a hop lands this far ahead */
#define DEMO_STEP_S   0.08f    /* sample step: < 0.55 cell at the fastest lane, under any gap */

/* Where lane object `o` will be dt seconds from now (with the lane's wrap). */
static float demo_obj_x(const Lane *lane, const LaneObject *o, float dt) {
    float x = o->x + lane_speed_px_s(lane) * dt;
    float w = (float)(o->width_cells * cell_size);
    if (lane->direction > 0 && x > grid_width)    x -= grid_width + w;
    else if (lane->direction < 0 && x + w < 0)    x += grid_width + w;
    return x;
}

/* A road cell is clear for the whole window [t0,t1] (seconds from now). */
static bool demo_road_clear(const Lane *lane, int col, float t0, float t1) {
    int p = cell_size / 8;
    if (p < 1) p = 1;
    int m = cell_size / 5;
    float fx = (float)(col * cell_size + p);
    float fw = (float)(cell_size - p * 2);
    for (float t = t0; ; t += DEMO_STEP_S) {
        if (t > t1) t = t1;
        for (int i = 0; i < lane->object_count; i++) {
            float ox = demo_obj_x(lane, &lane->objects[i], t);
            float ow = (float)(lane->objects[i].width_cells * cell_size);
            if (fx < ox + ow + m && fx + fw + m > ox) return false;
        }
        if (t >= t1) break;
    }
    return true;
}

/* A river cell has a log/turtle well under the frog's centre at time t. */
static bool demo_river_held(const Lane *lane, int col, float t) {
    float cx = (float)(col * cell_size + cell_size / 2);
    float m  = (float)(cell_size / 4);
    for (int i = 0; i < lane->object_count; i++) {
        float ox = demo_obj_x(lane, &lane->objects[i], t);
        float ow = (float)(lane->objects[i].width_cells * cell_size);
        if (cx >= ox + m && cx <= ox + ow - m) return true;
    }
    return false;
}

/* Would a frog that starts hopping to (row,col) now be alive on landing and
 * for the short wait before its next hop? */
static bool demo_land_ok(int row, int col) {
    if (col < 0 || col >= num_cols || row < 0 || row > 12) return false;
    if (row == 0) {
        for (int i = 0; i < NUM_GOALS; i++)
            if (goal_cols[i] == col && !state.goals_reached[i]) return true;
        return false;
    }
    if (row == 6 || row == 12) return true;
    int idx = lane_index_for_row(row);
    if (idx < 0) return false;
    if (row >= 7) return demo_road_clear(&lanes[idx], col, DEMO_LAND_S, DEMO_LAND_S + 0.3f);
    return demo_river_held(&lanes[idx], col, DEMO_LAND_S);
}

/* The nearest goal slot not yet filled, as a column; -1 when none. */
static int demo_goal_col(int from) {
    int best = -1, bd = 1 << 20;
    for (int i = 0; i < NUM_GOALS; i++) {
        if (state.goals_reached[i]) continue;
        int d = abs(goal_cols[i] - from);
        if (d < bd) { bd = d; best = goal_cols[i]; }
    }
    return best;
}

/* One decision when the frog is idle: hop forward when the landing is safe,
 * otherwise wait, sidestep out of danger, or on the last river row line up
 * with a free lily pad.  No / or % on variables. */
static void demo_ai(uint32_t now) {
    if (!frog.alive || frog.hopping || death_anim_active) return;
    if (now - last_hop_ms < HOP_COOLDOWN_MS) return;

    int r = frog.row, c = frog.col;
    if (demo_land_ok(r - 1, c)) { hop_frog(-1, 0); return; }

    int idx = lane_index_for_row(r);
    bool danger = false;
    int prefer = 0;                       /* sidestep direction tried first */
    if (idx >= 0 && r >= 7 && r <= 11) {
        danger = !demo_road_clear(&lanes[idx], c, 0.0f, 0.3f);
    } else if (idx >= 0 && r >= 1 && r <= 5) {
        int d = lanes[idx].direction;     /* the log carries us this way */
        if ((d > 0 && c >= num_cols - 2) || (d < 0 && c <= 1)) { danger = true; prefer = -d; }
        else if (r == 1) {                /* line up under a free lily pad */
            int g = demo_goal_col(c);
            if (g > c) prefer = 1; else if (g >= 0 && g < c) prefer = -1;
            if (prefer && demo_land_ok(r, c + prefer)) { hop_frog(0, prefer); return; }
        }
    }
    if (!danger) return;                  /* safe where we stand: wait */

    if (prefer == 0) prefer = (c * 2 < num_cols) ? 1 : -1;
    if (demo_land_ok(r, c + prefer))  { hop_frog(0, prefer);  return; }
    if (demo_land_ok(r, c - prefer))  { hop_frog(0, -prefer); return; }
    if (r < 12 && demo_land_ok(r + 1, c)) { hop_frog(1, 0); return; }
}

/* SCREEN_MENU while the attract cycle is on its DEMO page.  Entering it resets
 * a fresh level at NORMAL speed; START resets the game again, so nothing of the
 * demo leaks into real play. */
static void demo_sync(void) {
    bool want = (current_screen == SCREEN_MENU &&
                 start_menu_attract(&menu) == SM_ATTRACT_DEMO);
    if (want && !demo_running) {
        difficulty = DIFF_NORMAL;
        demo_running = true;      /* before reset_game(): the demo is silent from its first frame */
        reset_game();
        last_frame_ms = get_time_ms();
        last_hop_ms   = last_frame_ms;
    }
    demo_running = want;
    if (!want && led_effect.active) led_effect.active = false;
}

static void handle_input(void) {
    touch_poll(&touch);
    TouchState ts = touch_get_state(&touch);
    uint32_t now = get_time_ms();

    // Poll gamepad/keyboard/touch through unified API
    gamepad_poll(&gamepad, &input, ts.x, ts.y, ts.pressed);

    /* Hot-plug check: rescans only when /dev/input changed or a device went away */
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
            demo_running = false;
            reset_game();
            current_screen = SCREEN_PLAYING;
            last_frame_ms  = get_time_ms();
            /* Non-blocking: effect 1 is this file's own green flash,
             * serviced by update_led_effects() once per frame. */
            start_led_effect(1);
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

    /* ── Game over screen (handled by gameover_update in draw) ────── */
    if (current_screen == SCREEN_GAME_OVER)
        return;

    /* ── Pause screen ────────────────────────────────────────────────── */
    if (current_screen == SCREEN_PAUSED) {
        // Pad / keyboard: focus frame, A presses it, Start / Esc = RESUME
        ModalDialogAction action = modal_dialog_input(&pause_dialog, &input, 0);
        if (action == MODAL_ACTION_NONE)
            action = modal_dialog_update(&pause_dialog,
                ts.x, ts.y, ts.pressed, now);
        if (action == MODAL_ACTION_BTN0) {
            current_screen = SCREEN_PLAYING;
            last_frame_ms  = get_time_ms();
            return;
        }
        if (action == MODAL_ACTION_BTN1) {
            return_to_menu();
            return;
        }
        return;
    }

    /* ── Playing screen ──────────────────────────────────────────────── */

    // Gamepad/keyboard: pause
    if (input.buttons[BTN_ID_PAUSE].pressed) {
        current_screen = SCREEN_PAUSED;
        modal_dialog_set_focus(&pause_dialog, 0);
        modal_dialog_show(&pause_dialog);
        return;
    }

    // Touch: exit and menu buttons.  Asked EVERY frame, quiet ones included —
    // that is what clears button_check_press()'s latch.  Behind an ts.pressed
    // guard the call with `false` is unreachable and the menu fires once per
    // process; frogger survived it only because a tap in the play area also
    // reaches the (false) case.  Office Runner, which has no touch gameplay,
    // did not — reported from the panel 2026-08-10, mechanism in
    // tests/button_latch_test.c.
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

    if (ts.pressed) {
        /* Direction input via touch (only in play area, with cooldown) */
        if (ts.y >= grid_offset_y && frog.alive && !death_anim_active) {
            if (now - last_hop_ms < HOP_COOLDOWN_MS) return;
            if (frog.hopping) return;

            int frog_sx = grid_offset_x + frog.col * cell_size
                          + cell_size / 2 + (int)frog.ride_offset;
            int frog_sy = grid_offset_y + frog.row * cell_size + cell_size / 2;

            int dx = ts.x - frog_sx;
            int dy = ts.y - frog_sy;
            int min_dist = cell_size / 2;
            if (abs(dx) < min_dist && abs(dy) < min_dist) return;

            if (abs(dx) > abs(dy)) {
                hop_frog(0, dx > 0 ? +1 : -1);
            } else {
                hop_frog(dy < 0 ? -1 : +1, 0);
            }
        }
    }

    // Gamepad/keyboard direction input (pressed edge, with hop cooldown)
    if (frog.alive && !death_anim_active && !frog.hopping) {
        if (now - last_hop_ms >= HOP_COOLDOWN_MS) {
            if (input.buttons[BTN_ID_UP].pressed) {
                hop_frog(-1, 0);
            } else if (input.buttons[BTN_ID_DOWN].pressed) {
                hop_frog(+1, 0);
            } else if (input.buttons[BTN_ID_LEFT].pressed) {
                hop_frog(0, -1);
            } else if (input.buttons[BTN_ID_RIGHT].pressed) {
                hop_frog(0, +1);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Game Update
 * ═══════════════════════════════════════════════════════════════════════════ */

static void update_game(void) {
    if (current_screen != SCREEN_PLAYING && !demo_on()) return;

    current_frame++;
    uint32_t now = get_time_ms();
    float dt = (now - last_frame_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_frame_ms = now;

    if (death_anim_active) {
        update_death_animation(dt);
        return;
    }
    if (demo_on()) {
        if (state.lives <= 0) return;   /* run over; waiting for the widget to leave DEMO */
        demo_ai(now);
    }

    update_lanes(dt);
    update_frog_ride(dt);
    update_hop(dt);
    update_timer(dt);

    if (!frog.hopping && frog.alive) {
        if (frog.row >= 7 && frog.row <= 11)
            check_road_collision();
        else if (frog.row >= 1 && frog.row <= 5)
            check_river_collision();
        else if (frog.row == 0)
            check_goal_reached();
    }

    update_led_effects();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sprite Drawing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_frog_sprite(int sx, int sy) {
    int cs = cell_size;
    int p  = cs / 8;  if (p < 1) p = 1;
    int cx = sx + cs / 2;
    int bw = cs - p * 3;
    int bh = cs - p * 3;
    int bx = sx + (cs - bw) / 2;
    int by = sy + (cs - bh) / 2 + p / 2;

    fb_fill_rounded_rect(&fb, bx, by, bw, bh, p, COLOR_FROG_BODY);
    fb_fill_rect(&fb, bx + p, by + bh / 3, bw - p * 2, bh / 3, COLOR_FROG_BELLY);

    int lw = p * 2; if (lw < 2) lw = 2;
    int lh = p;     if (lh < 1) lh = 1;
    fb_fill_rect(&fb, bx - lw / 2,            by + p,          lw, lh, COLOR_FROG_DARK);
    fb_fill_rect(&fb, bx + bw - lw / 2,       by + p,          lw, lh, COLOR_FROG_DARK);
    fb_fill_rect(&fb, bx - lw / 2,            by + bh - p * 2, lw, lh, COLOR_FROG_DARK);
    fb_fill_rect(&fb, bx + bw - lw / 2,       by + bh - p * 2, lw, lh, COLOR_FROG_DARK);

    int er = cs > 40 ? 3 : 2;
    int pr = er > 2 ? 2 : 1;
    int ey = by + p;
    int elx = cx - bw / 4;
    int erx = cx + bw / 4;
    fb_fill_circle(&fb, elx, ey, er, COLOR_FROG_EYE_W);
    fb_fill_circle(&fb, erx, ey, er, COLOR_FROG_EYE_W);
    fb_fill_circle(&fb, elx, ey, pr, COLOR_FROG_EYE_B);
    fb_fill_circle(&fb, erx, ey, pr, COLOR_FROG_EYE_B);
}

static void draw_car(int sx, int sy, uint32_t color) {
    int cs = cell_size;
    int p  = cs / 8; if (p < 1) p = 1;
    int bx = sx + p, by = sy + p * 2;
    int bw = cs - p * 2, bh = cs - p * 4;
    if (bw < 2 || bh < 2) return;

    fb_fill_rounded_rect(&fb, bx, by, bw, bh, p / 2, color);

    uint32_t dk = RGB(((color >> 16) & 0xFF) * 3 / 4,
                      ((color >>  8) & 0xFF) * 3 / 4,
                      ( color        & 0xFF) * 3 / 4);
    fb_fill_rect(&fb, bx + p, by + p, bw - p * 2, bh / 3, dk);
    if (bw > p * 4)
        fb_fill_rect(&fb, bx + p * 2, by + p + 1, bw - p * 4, bh / 4, COLOR_WINDOW);

    int wr = p > 2 ? p : 2;
    fb_fill_circle(&fb, bx + p * 2,      by + bh, wr, COLOR_WHEEL);
    fb_fill_circle(&fb, bx + bw - p * 2, by + bh, wr, COLOR_WHEEL);
}

static void draw_truck(int sx, int sy, uint32_t color) {
    int cs = cell_size;
    int p  = cs / 8; if (p < 1) p = 1;
    int tw = cs * 2 - p * 2, th = cs - p * 4;
    int tx = sx + p, ty = sy + p * 2;
    if (tw < 4 || th < 2) return;

    int cargw = tw * 2 / 3;
    int cargx = tx + tw - cargw;
    fb_fill_rounded_rect(&fb, cargx, ty, cargw, th, p / 2, color);

    uint32_t dk = RGB(((color >> 16) & 0xFF) * 2 / 3,
                      ((color >>  8) & 0xFF) * 2 / 3,
                      ( color        & 0xFF) * 2 / 3);
    for (int i = 1; i < 3; i++) {
        int lx = cargx + (cargw * i) / 3;
        fb_draw_line(&fb, lx, ty + 2, lx, ty + th - 2, dk);
    }

    int cabw = tw - cargw;
    uint32_t cc = RGB(((color >> 16) & 0xFF) * 3 / 4,
                      ((color >>  8) & 0xFF) * 3 / 4,
                      ( color        & 0xFF) * 3 / 4);
    fb_fill_rounded_rect(&fb, tx, ty, cabw + 2, th, p / 2, cc);
    if (cabw > p * 2)
        fb_fill_rect(&fb, tx + p, ty + p, cabw - p * 2, th / 2, COLOR_WINDOW);

    int wr = p > 2 ? p : 2;
    fb_fill_circle(&fb, tx + p * 2,         ty + th, wr, COLOR_WHEEL);
    fb_fill_circle(&fb, tx + cabw,           ty + th, wr, COLOR_WHEEL);
    fb_fill_circle(&fb, cargx + cargw / 3,   ty + th, wr, COLOR_WHEEL);
    fb_fill_circle(&fb, cargx + cargw * 2/3, ty + th, wr, COLOR_WHEEL);
}

static void draw_race_car(int sx, int sy, uint32_t color) {
    int cs = cell_size;
    int p  = cs / 8; if (p < 1) p = 1;
    int bx = sx + p, by = sy + cs / 3;
    int bw = cs - p * 2, bh = cs / 2;
    if (bw < 2 || bh < 2) return;

    fb_fill_rounded_rect(&fb, bx, by, bw, bh, p, color);
    fb_draw_thick_line(&fb, bx + bw / 2, by + 2, bx + bw / 2, by + bh - 2,
                       2, COLOR_WHITE);
    int wr = p > 2 ? p : 2;
    fb_fill_circle(&fb, bx + p,      by + bh, wr, COLOR_WHEEL);
    fb_fill_circle(&fb, bx + bw - p, by + bh, wr, COLOR_WHEEL);
}

static void draw_log_sprite(int sx, int sy, int width_cells) {
    int cs = cell_size;
    int p  = cs / 8; if (p < 1) p = 1;
    int lw = width_cells * cs;
    int lh = cs - p * 3;
    int lx = sx;
    int ly = sy + p + p / 2;
    if (lw < 2 || lh < 4) return;

    fb_fill_rounded_rect(&fb, lx, ly, lw, lh, lh / 2, COLOR_LOG_LIGHT);

    for (int i = 1; i <= 3; i++) {
        int line_y = ly + (lh * i) / 4;
        fb_draw_line(&fb, lx + p * 2, line_y, lx + lw - p * 2, line_y,
                     COLOR_LOG_BARK);
    }

    int cr = lh / 2 - 1; if (cr < 1) cr = 1;
    fb_fill_circle(&fb, lx + cr + 1,      ly + lh / 2, cr, COLOR_LOG_DARK);
    fb_fill_circle(&fb, lx + lw - cr - 1, ly + lh / 2, cr, COLOR_LOG_DARK);
    if (cr > 2) {
        fb_draw_circle(&fb, lx + cr + 1,      ly + lh / 2, cr / 2, COLOR_LOG_BARK);
        fb_draw_circle(&fb, lx + lw - cr - 1, ly + lh / 2, cr / 2, COLOR_LOG_BARK);
    }
}

static void draw_turtle_group(int sx, int sy, int count) {
    int cs = cell_size;
    int p  = cs / 8; if (p < 1) p = 1;

    for (int i = 0; i < count; i++) {
        int tx  = sx + i * cs;
        int tcx = tx + cs / 2;
        int tcy = sy + cs / 2;
        int sr  = cs / 2 - p * 2; if (sr < 2) sr = 2;

        fb_fill_circle(&fb, tcx, tcy, sr, COLOR_TURTLE_SHELL);
        fb_draw_line(&fb, tcx - sr / 2, tcy, tcx + sr / 2, tcy, COLOR_TURTLE_DARK);
        fb_draw_line(&fb, tcx, tcy - sr / 2, tcx, tcy + sr / 2, COLOR_TURTLE_DARK);
        fb_fill_circle(&fb, tcx - 1, tcy - 1, sr / 3, COLOR_TURTLE_HEAD);
        fb_fill_circle(&fb, tcx + sr - 1, tcy - sr / 2, p + 1, COLOR_TURTLE_HEAD);
    }
}

/* The splash grows by cs/8 per 33.3 ms (authored as per frame at 30 fps) and
 * reaches its full cell radius halfway through DEATH_ANIM_MS. */
static void draw_death_splash(int sx, int sy, float elapsed_ms) {
    int cs = cell_size;
    int cx = sx + cs / 2, cy = sy + cs / 2;
    int r  = cs / 4 + (int)(elapsed_ms * (30.0f / 1000.0f / 8.0f) * (float)cs);
    if (r > cs) r = cs;
    fb_fill_circle(&fb, cx, cy, r, COLOR_RED);
    fb_draw_line(&fb, cx - r / 2, cy - r / 2, cx + r / 2, cy + r / 2, COLOR_WHITE);
    fb_draw_line(&fb, cx + r / 2, cy - r / 2, cx - r / 2, cy + r / 2, COLOR_WHITE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Lane Background Drawing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_water_lane(int row) {
    int y = grid_offset_y + row * cell_size;
    int x = grid_offset_x;
    int w = grid_width;

    fb_fill_rect(&fb, x, y, w, cell_size, COLOR_WATER_DARK);

    int wy1 = y + cell_size / 3;
    int wy2 = y + cell_size * 2 / 3;
    int step = cell_size / 2; if (step < 4) step = 4;
    for (int wx = 0; wx < w; wx += step) {
        int off = ((int)(current_frame / 4) + wx / 8) % 6;
        fb_fill_rect(&fb, x + wx, wy1 + off - 3,
                     cell_size / 4, 2, COLOR_WATER_WAVE);
        fb_fill_rect(&fb, x + wx + cell_size / 4, wy2 - off + 3,
                     cell_size / 4, 2, COLOR_WATER_LIGHT);
    }
}

static void draw_road_lane(int row) {
    int y = grid_offset_y + row * cell_size;
    int x = grid_offset_x;
    int w = grid_width;

    fb_fill_rect(&fb, x, y, w, cell_size, COLOR_ROAD_DARK);

    int dash_y = y + cell_size / 2 - 1;
    int step = cell_size; if (step < 4) step = 4;
    for (int dx = 0; dx < w; dx += step)
        fb_fill_rect(&fb, x + dx + 2, dash_y, cell_size / 2 - 2, 2, COLOR_ROAD_LINE);
}

static void draw_grass_lane(int row) {
    int y = grid_offset_y + row * cell_size;
    int x = grid_offset_x;
    int w = grid_width;

    fb_fill_rect(&fb, x, y, w, cell_size, COLOR_GRASS_DARK);

    unsigned int seed = (unsigned int)(row * 1000);
    int dots = w / 8; if (dots < 1) dots = 1;
    for (int i = 0; i < dots; i++) {
        seed = seed * 1103515245u + 12345u;
        int ddx = (int)((seed >> 16) % (unsigned)w);
        seed = seed * 1103515245u + 12345u;
        int ddy = (int)((seed >> 16) % (unsigned)cell_size);
        fb_fill_rect(&fb, x + ddx, y + ddy, 2, 2, COLOR_GRASS_LIGHT);
    }
}

static void draw_goal_zone(void) {
    int y = grid_offset_y;
    int x = grid_offset_x;
    int w = grid_width;

    fb_fill_rect(&fb, x, y, w, cell_size, COLOR_WATER_DARK);

    for (int i = 0; i < NUM_GOALS; i++) {
        int px = grid_offset_x + goal_cols[i] * cell_size + cell_size / 2;
        int py = y + cell_size / 2;
        int pr = cell_size / 2 - 2; if (pr < 3) pr = 3;

        if (state.goals_reached[i]) {
            fb_fill_circle(&fb, px, py, pr, COLOR_LILYPAD);
            fb_fill_circle(&fb, px, py, pr / 2, COLOR_GOAL_FROG);
            fb_fill_circle(&fb, px - pr / 4, py - pr / 3, 2, COLOR_FROG_EYE_W);
            fb_fill_circle(&fb, px + pr / 4, py - pr / 3, 2, COLOR_FROG_EYE_W);
        } else {
            fb_fill_circle(&fb, px, py, pr, COLOR_LILYPAD);
            fb_draw_line(&fb, px, py, px - pr / 2, py - pr, COLOR_WATER_DARK);
            fb_draw_line(&fb, px, py, px + pr / 2, py - pr, COLOR_WATER_DARK);
            fb_fill_circle(&fb, px + 1, py + 1, pr / 3, COLOR_LILYPAD_LIGHT);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Draw Lane Objects
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_lane_objects(int lane_idx) {
    Lane *lane = &lanes[lane_idx];
    int y = grid_offset_y + lane->row * cell_size;

    for (int j = 0; j < lane->object_count; j++) {
        LaneObject *obj = &lane->objects[j];
        int ox = grid_offset_x + (int)obj->x;
        int ow = obj->width_cells * cell_size;

        if (ox + ow < grid_offset_x - cell_size ||
            ox > grid_offset_x + grid_width + cell_size)
            continue;

        switch (obj->type) {
        case OBJ_CAR:       draw_car(ox, y, obj->color);               break;
        case OBJ_TRUCK:     draw_truck(ox, y, obj->color);             break;
        case OBJ_RACE_CAR:  draw_race_car(ox, y, obj->color);          break;
        case OBJ_LOG_SHORT:
        case OBJ_LOG_MED:
        case OBJ_LOG_LONG:  draw_log_sprite(ox, y, obj->width_cells);  break;
        case OBJ_TURTLE_2:  draw_turtle_group(ox, y, 2);               break;
        case OBJ_TURTLE_3:  draw_turtle_group(ox, y, 3);               break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HUD Drawing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_timer_bar(void) {
    /* The bar lives in the gap BETWEEN the MENU and EXIT buttons, vertically
     * centred on the row.  It used to span the whole grid width at
     * hud_height - 18, which covered the bottom ~8 px of both buttons. */
    int bx = LAYOUT_MENU_BTN_X + BTN_MENU_WIDTH + TIMER_BAR_GAP;
    int bw = LAYOUT_EXIT_BTN_X - TIMER_BAR_GAP - bx;
    int bh = TIMER_BAR_HEIGHT;
    int by = LAYOUT_MENU_BTN_Y + (BTN_MENU_HEIGHT - bh) / 2;

    if (bw < 40) return;   /* no room between the buttons — skip it */

    fb_fill_rect(&fb, bx, by, bw, bh, RGB(40, 40, 40));

    float ratio = state.timer / TIMER_MAX;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int fill_w = (int)(bw * ratio);

    uint32_t color;
    if      (state.timer > 10.0f) color = COLOR_GREEN;
    else if (state.timer >  5.0f) color = COLOR_YELLOW;
    else                          color = COLOR_RED;

    fb_fill_rect(&fb, bx, by, fill_w, bh, color);
    fb_draw_rect(&fb, bx, by, bw, bh, COLOR_WHITE);
}

static void draw_life_icons(int x, int y) {
    for (int i = 0; i < state.lives && i < 5; i++) {
        int ix = x + i * LIFE_ICON_PITCH;
        fb_fill_circle(&fb, ix + 6, y + 6, 5, COLOR_FROG_BODY);
        fb_fill_circle(&fb, ix + 3, y + 3, 1, COLOR_FROG_EYE_W);
        fb_fill_circle(&fb, ix + 9, y + 3, 1, COLOR_FROG_EYE_W);
    }
}

static void draw_hud(void) {
    /* The band starts at the visible top (it is only drawn) and ends below the
     * SAFE-anchored button row.  Score, level and the lives icons sit in the
     * VISIBLE band ABOVE the row — none of them is pressable, and that band is
     * the screen area the two-rectangle split exists to keep usable.
     * Horizontally they stay in the gap between MENU and EXIT, so a short band
     * (uncalibrated panel, inset 0) puts them level with the buttons without
     * colliding with either. */
    fb_fill_rect(&fb, 0, 0, (int)fb.width, hud_height, RGB(20, 20, 30));
    if (!demo_on()) {          /* the demo shows no buttons */
        draw_menu_button(&fb, &menu_button);
        draw_exit_button(&fb, &exit_button);
    }

    const int hud_scale = 2;
    int hud_h  = text_measure_height(hud_scale);
    int band_h = LAYOUT_MENU_BTN_Y - SCREEN_VISIBLE_TOP;
    int row_y  = (band_h >= hud_h)
                 ? SCREEN_VISIBLE_TOP + (band_h - hud_h) / 2
                 : LAYOUT_MENU_BTN_Y - hud_h - 2;
    if (row_y < SCREEN_VISIBLE_TOP) row_y = SCREEN_VISIBLE_TOP;

    /* One row: SCORE, LVL, lives — laid out from a measured total width so it
     * stays centred in the gap whatever the numbers grow to. */
    char score_buf[32];
    char level_buf[32];
    bool narrow = ((int)fb.width < 600);
    snprintf(score_buf, sizeof(score_buf), narrow ? "SC:%d" : "SCORE: %d", state.score);
    snprintf(level_buf, sizeof(level_buf), narrow ? "LV:%d" : "LVL: %d", state.level);

    int lives_shown = state.lives > 5 ? 5 : state.lives;
    int lives_w = lives_shown * LIFE_ICON_PITCH;
    int score_w = text_measure_width(score_buf, hud_scale);
    int level_w = text_measure_width(level_buf, hud_scale);
    int gap     = narrow ? 12 : 24;

    int total_w = score_w + gap + level_w + (lives_w ? gap + lives_w : 0);
    int gap_lo  = LAYOUT_MENU_BTN_X + BTN_MENU_WIDTH + 8;
    int gap_hi  = LAYOUT_EXIT_BTN_X - 8;
    int x = gap_lo + (gap_hi - gap_lo - total_w) / 2;
    if (x < gap_lo) x = gap_lo;

    fb_draw_text(&fb, x, row_y, score_buf, COLOR_WHITE, hud_scale);
    x += score_w + gap;
    fb_draw_text(&fb, x, row_y, level_buf, COLOR_CYAN, hud_scale);
    if (lives_w) {
        x += level_w + gap;
        draw_life_icons(x, row_y + (hud_h - LIFE_ICON_SIZE) / 2);
    }

    /* Timer bar */
    draw_timer_bar();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Playing Field Drawing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_playing_field(void) {
    /* 1. Draw lane backgrounds */
    for (int row = 0; row < NUM_ROWS; row++) {
        if (row == 0) {
            draw_goal_zone();
        } else if (row >= 1 && row <= 5) {
            draw_water_lane(row);
        } else if (row == 6 || row == 12) {
            draw_grass_lane(row);
        } else if (row >= 7 && row <= 11) {
            draw_road_lane(row);
        }
    }

    /* 2. Draw lane objects */
    for (int i = 0; i < NUM_LANE_CONFIGS; i++)
        draw_lane_objects(i);

    /* 3. Draw frog */
    if (death_anim_active) {
        /* Death animation at frog's position */
        int fx, fy;
        if (frog.hopping) {
            float t = frog.hop_progress;
            fx = grid_offset_x + (int)(frog.col * cell_size * (1 - t) +
                                       frog.target_col * cell_size * t
                                       + frog.ride_offset);
            fy = grid_offset_y + (int)(frog.row * cell_size * (1 - t) +
                                       frog.target_row * cell_size * t);
        } else {
            fx = grid_offset_x + frog.col * cell_size + (int)frog.ride_offset;
            fy = grid_offset_y + frog.row * cell_size;
        }
        draw_death_splash(fx, fy, death_anim_ms);
    } else if (frog.alive) {
        int fx, fy;
        if (frog.hopping) {
            float t = frog.hop_progress;
            int from_x = grid_offset_x + frog.col * cell_size;
            int from_y = grid_offset_y + frog.row * cell_size;
            int to_x   = grid_offset_x + frog.target_col * cell_size;
            int to_y   = grid_offset_y + frog.target_row * cell_size;
            fx = from_x + (int)((to_x - from_x) * t);
            fy = from_y + (int)((to_y - from_y) * t);
            /* Arc: lift the frog during mid-hop */
            int arc = (int)(sinf(t * 3.14159f) * cell_size / 3);
            fy -= arc;
        } else {
            fx = grid_offset_x + frog.col * cell_size + (int)frog.ride_offset;
            fy = grid_offset_y + frog.row * cell_size;
        }
        draw_frog_sprite(fx, fy);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main Draw Function
 * ═══════════════════════════════════════════════════════════════════════════ */

static void draw_all(void) {
    /* ── Start menu, or the attract cycle's SCORES page (widget clears too) ── */
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

    /* Draw the playing field as background for PLAYING, PAUSED, GAME_OVER, DEMO */
    draw_hud();
    draw_playing_field();

    if (demo_on()) {
        const char *label = "DEMO";
        int lw = text_measure_width(label, 2);
        fb_draw_text(&fb, ((int)fb.width - lw) / 2, (int)fb.height - 28, label, COLOR_CYAN, 2);
        return;
    }

    /* ── Pause overlay ───────────────────────────────────────────────── */
    if (current_screen == SCREEN_PAUSED) {
        modal_dialog_draw(&pause_dialog, &fb);
        return;
    }

    /* ── Game over overlay ───────────────────────────────────────────── */
    if (current_screen == SCREEN_GAME_OVER) {
        TouchState go_ts = touch_get_state(&touch);
        GameOverAction action = gameover_update(&gos, &fb,
                                                go_ts.x, go_ts.y, go_ts.pressed,
                                                &input);
        if (action == GAMEOVER_ACTION_MENU)
            return_to_menu();
        return;
    }

    /* No virtual-controller overlay: the frog is driven by tapping the
     * playfield, not by an on-screen D-pad, and the overlay's boxes were never
     * where its TouchRegions were anyway. */

    /* Hint text at bottom */
    if (!input.gamepad_connected && !input.keyboard_connected)
        fb_draw_text(&fb, 10, (int)fb.height - 20,
                     "TAP AHEAD OF THE FROG TO HOP", RGB(100, 100, 100), 1);
    else
        fb_draw_text(&fb, 10, (int)fb.height - 20,
                     "ARROWS/D-PAD: HOP  ESC: PAUSE", RGB(100, 100, 100), 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main Entry Point
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* Line-buffer stdout FIRST: at boot it is /var/log/arca/app_stdout.log,
     * not a tty, so glibc block-buffers 4 KB and audio_bed_init()'s playlist
     * receipt never arrives — which reads as a printf that was never reached.
     * common/logger.c line-buffers its own file, which is why only the printf
     * lines go missing (../CLAUDE.md → App lifecycle). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *fb_device    = "/dev/fb0";
    const char *touch_device = "/dev/input/touchscreen0";

    if (argc > 1) fb_device    = argv[1];
    if (argc > 2) touch_device = argv[2];

    /* Singleton guard */
    int lock_fd = acquire_instance_lock("frogger");
    (void)lock_fd;  /* held open for process lifetime */
    if (lock_fd < 0) {
        fprintf(stderr, "frogger: another instance is already running\n");
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

    /* The music bed: the playlist named by /opt/arca/soundsets/frogger.sound,
     * with the four states and the hold/resume rules in common/audio_bed.c.
     * ⚠️ That file is the ONLY home for the paths — no set file means no music,
     * and frogger_music present but EMPTY is the off switch. */
    AudioBed bed;
    audio_bed_init(&bed, &audio, "frogger");

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

    printf("Frogger game started! Touch screen to play.\n");
    printf("Press Ctrl+C to exit.\n");

    /* ── Main game loop ────────────────────────────────────────────────── */
    bool needs_redraw = true;
    while (running) {
        GameScreen prev_screen = current_screen;
        handle_input();
        update_game();

        /* Dirty-flag: active gameplay always redraws; static screens only on changes */
        if (current_screen == SCREEN_PLAYING || demo_on()) {
            needs_redraw = true;  /* lanes scroll, water animates continuously; the demo is gameplay */
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
        audio_bed_service(&bed, current_screen == SCREEN_PLAYING, current_screen == SCREEN_PAUSED || (death_anim_active && !demo_on()));

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

    printf("Frogger game ended. Final score: %d\n", state.score);
    return 0;
}
