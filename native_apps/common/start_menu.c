/*
 * start_menu.c — the arcade start menu.  Contract and timing: start_menu.h.
 *
 * ⚠️ No integer divide or modulo on a run-time value anywhere in here: the
 * Cortex-A8 has no divide instruction, and the two quotients the blink needs
 * (ms / 1000, blink / 6) are taken by multiplying with a fixed-point
 * reciprocal instead (sm_div1000, sm_mod6 — both exact over all of uint32_t,
 * and checked against real division by tests/start_menu_test.c).  Every
 * other `/ 2` below is a shift by a constant.
 */
#include "start_menu.h"
#include "common.h"
#include <stdio.h>
#include <string.h>

#define SM_TITLE_SCALE  4
#define SM_TITLE_TOP    50      /* title's top edge below SCREEN_VISIBLE_TOP */
#define SM_ENTRY_SCALE  3
#define SM_PITCH_MAX    52      /* row pitch = touch target height */
#define SM_PITCH_MIN    36      /* never shrink a target below this */
#define SM_ROW_PAD      24      /* per side, beyond the widest drawn string */
#define SM_ROW_MIN_W    280
#define SM_BRACKET_GAP  12      /* between a bracket and the label */
#define SM_COLOR_GREY   RGB(90, 90, 90)

static const char *const sm_player_values[] = { "1 PLAYER", "2 PLAYERS" };

/* ── Divide-free arithmetic ────────────────────────────────────────────── */

/* floor(e / 1000): 274877907 = ceil(2^38 / 1000), and its error
 * (56 / 2^38 per unit) stays below 1/1000 for every 32-bit e, so the
 * floor is exact. */
static uint32_t sm_div1000(uint32_t e)
{
    return (uint32_t)(((uint64_t)e * 274877907u) >> 38);
}

/* i mod 6: 0xAAAAAAAB = ceil(2^34 / 6), error 2 / 2^34 per unit, below 1/6
 * for every 32-bit i. */
static uint32_t sm_mod6(uint32_t i)
{
    uint32_t q = (uint32_t)(((uint64_t)i * 0xAAAAAAABu) >> 34);
    return i - q * 6u;
}

/* ── Pure decisions ────────────────────────────────────────────────────── */

void start_menu_phase(uint32_t elapsed_ms, bool *marker_on, uint32_t *blink_idx)
{
    uint32_t idx = sm_div1000(elapsed_ms);
    uint32_t in  = elapsed_ms - idx * (uint32_t)SM_BLINK_MS;
    if (marker_on) *marker_on = in < SM_BLINK_ON_MS;
    if (blink_idx) *blink_idx = idx;
}

/* Edge-detected: a blink pings the first time its on-phase is SEEN, not the
 * first time its on-phase starts — a loop that sleeps 100 ms between updates
 * still catches every 500 ms on-phase.  A blink whose on-phase fell entirely
 * inside a stall is never seen and never pinged (no catch-up), and a blink
 * already seen never pings twice. */
bool start_menu_sound_allowed(uint32_t idle_ms)
{
    return idle_ms < SM_SOUND_IDLE_MS;
}

bool start_menu_ping_due(bool marker_on, uint32_t blink_idx, uint32_t idle_ms,
                         uint32_t *seen_on_idx)
{
    /* Past the idle cutoff the latch is left alone, so the first input
     * afterwards can ping the blink that is lit at that moment. */
    if (!start_menu_sound_allowed(idle_ms)) return false;
    if (!marker_on || blink_idx == *seen_on_idx) return false;
    *seen_on_idx = blink_idx;
    return sm_mod6(blink_idx) < SM_PING_RUN;
}

/* floor(x / 15) for x < 2^28: 2290649225 = ceil(2^35 / 15), error 7 / 2^35
 * per unit, below 1/15 while x < 2^35 / 105.  Fed sm_div1000()'s quotient,
 * at most 4294967. */
static uint32_t sm_div15(uint32_t x)
{
    return (uint32_t)(((uint64_t)x * 2290649225u) >> 35);
}

/* i mod 3: 0xAAAAAAAB = ceil(2^33 / 3), error 1 / 2^33 per unit. */
static uint32_t sm_mod3(uint32_t i)
{
    uint32_t q = (uint32_t)(((uint64_t)i * 0xAAAAAAABu) >> 33);
    return i - q * 3u;
}

/* The phase SM_ATTRACT_MS-long slot n of the idle time falls in: slot 0 is
 * the menu that went idle, then DEMO, SCORES, MENU (or SCORES, MENU without
 * a demo) repeating.  The clock is the idle time, moved on by a demo that
 * ended early.  floor(t / 15000) as floor(floor(t / 1000) / 15). */
SmAttract start_menu_attract_at(uint32_t idle_ms, bool has_demo)
{
    uint32_t n = sm_div15(sm_div1000(idle_ms));
    if (n == 0) return SM_ATTRACT_MENU;
    n -= 1;
    if (has_demo) {
        uint32_t r = sm_mod3(n);
        return r == 0 ? SM_ATTRACT_DEMO : r == 1 ? SM_ATTRACT_SCORES : SM_ATTRACT_MENU;
    }
    return (n & 1u) ? SM_ATTRACT_MENU : SM_ATTRACT_SCORES;
}

/* ── State helpers ─────────────────────────────────────────────────────── */

static void sm_restart_blink(StartMenu *m, uint32_t now)
{
    m->t0 = now;
    m->seen_on_idx = SM_NO_BLINK;
    m->marker_on = true;
    m->dirty = true;
}

static bool sm_selectable(const StartMenu *m, int i)
{
    return i >= 0 && i < m->count && !m->entries[i].disabled;
}

/* Next selectable entry from `from` in dir (+1/-1), wrapping; `from` itself
 * when nothing else is.  A plain index walk, not ui_focus_move(): the entries
 * are one column in list order, so the spatial search would find the same
 * neighbour, and it knows nothing about disabled entries. */
static int sm_walk(const StartMenu *m, int from, int dir)
{
    int i = from;
    for (int k = 0; k < m->count; k++) {
        i += dir;
        if (i < 0) i = m->count - 1;
        if (i >= m->count) i = 0;
        if (sm_selectable(m, i)) return i;
    }
    return from;
}

static void sm_set_warning(StartMenu *m, const char *w, bool players)
{
    char up[sizeof(m->warning)];
    up[0] = '\0';
    if (w && w[0]) text_to_uppercase(up, w, sizeof(up));
    if (strcmp(up, m->warning) != 0) {
        memcpy(m->warning, up, sizeof(up));
        m->dirty = true;
    }
    m->warn_is_players = players && up[0];
}

/* Cycle a CHOICE by dir, skipping values switched off, wrapping.  Returns
 * whether the value changed.  A players choice that cannot reach 2 says why. */
static bool sm_cycle(StartMenu *m, int idx, int dir)
{
    StartMenuEntry *e = &m->entries[idx];
    if (e->kind != SM_CHOICE || e->nvalues < 2) return false;
    int v = e->value;
    for (int k = 1; k < e->nvalues; k++) {
        v += dir;
        if (v < 0) v = e->nvalues - 1;
        if (v >= e->nvalues) v = 0;
        if (!(e->value_off & (1u << v))) break;
    }
    if (v == e->value || (e->value_off & (1u << v))) {
        if (idx == m->players_idx) sm_set_warning(m, SM_WARN_NEED_P2, true);
        return false;
    }
    e->value = v;
    m->dirty = true;
    return true;
}

/* 2 PLAYERS needs a device in slot P1 AND slot P2, re-read every frame. */
static void sm_sync_players(StartMenu *m, int mask)
{
    if (m->players_idx < 0) return;
    StartMenuEntry *e = &m->entries[m->players_idx];
    bool p2 = (mask & 3) == 3;
    unsigned off = p2 ? 0u : 2u;
    if (e->value_off != off) { e->value_off = off; m->dirty = true; }
    if (!p2 && e->value == 1) {
        e->value = 0;
        m->dirty = true;
        sm_set_warning(m, SM_WARN_NEED_P2, true);
    } else if (p2 && m->warn_is_players) {
        sm_set_warning(m, NULL, false);
    }
}

/* What the row shows, uppercased: ACTION its label, CHOICE "LABEL: VALUE",
 * or the value alone when the label is empty (the players choice). */
static void sm_entry_text(const StartMenuEntry *e, char *out, size_t n)
{
    char val[48];
    if (e->kind == SM_ACTION) { snprintf(out, n, "%s", e->label); return; }
    text_to_uppercase(val, e->values[e->value], sizeof(val));
    if (e->label[0]) snprintf(out, n, "%s: %s", e->label, val);
    else             snprintf(out, n, "%s", val);
}

static int sm_block_height(const char *text, int scale)
{
    if (!text[0]) return 0;
    int lines = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') lines++;
    return lines * text_measure_height(scale) + (lines - 1) * WELCOME_LINE_GAP;
}

/* ── Layout: computed once, the rectangles serve draw and hit test ─────── */

static void sm_layout(StartMenu *m)
{
    int th2 = text_measure_height(2);
    m->title_y = SCREEN_VISIBLE_TOP + SM_TITLE_TOP;   /* below the bezel band */
    int y = m->title_y + text_measure_height(SM_TITLE_SCALE) + WELCOME_BLOCK_GAP;

    m->sub_y = y;
    if (m->subtitle[0]) y += th2 + WELCOME_BLOCK_GAP;
    m->inst_y = y;
    if (m->instructions[0])
        y += sm_block_height(m->instructions, WELCOME_INST_SCALE) + WELCOME_BLOCK_GAP;

    /* The warning line is reserved whether or not one is showing, so a
     * warning that appears (P2 left) never moves a target under a finger. */
    int bottom = SCREEN_SAFE_BOTTOM - (th2 + WELCOME_BLOCK_GAP);
    int pitch  = SM_PITCH_MAX;
    while (pitch > SM_PITCH_MIN && y + m->count * pitch > bottom) pitch -= 2;
    int list_h = m->count * pitch;
    int top = y + (bottom - y - list_h) / 2;
    if (top < y) top = bottom - list_h;         /* crowded: the targets win over text */
    if (top < SCREEN_SAFE_TOP) top = SCREEN_SAFE_TOP;

    /* One width for every row, from the widest string any of them can show. */
    int w = 0;
    for (int i = 0; i < m->count; i++) {
        StartMenuEntry tmp = m->entries[i];
        int nv = tmp.kind == SM_CHOICE ? tmp.nvalues : 1;
        for (int v = 0; v < nv; v++) {
            char s[96];
            tmp.value = v;
            sm_entry_text(&tmp, s, sizeof(s));
            int sw = text_measure_width(s, SM_ENTRY_SCALE);
            if (sw > w) w = sw;
        }
    }
    w += 2 * (text_measure_width(">", SM_ENTRY_SCALE) + SM_BRACKET_GAP + SM_ROW_PAD);
    if (w < SM_ROW_MIN_W) w = SM_ROW_MIN_W;
    if (w > SCREEN_SAFE_WIDTH) w = SCREEN_SAFE_WIDTH;

    for (int i = 0; i < m->count; i++) {
        m->rect[i].x = LAYOUT_CENTER_X(w);
        m->rect[i].y = top + i * pitch;
        m->rect[i].w = w;
        m->rect[i].h = pitch;
    }
    m->warn_y = top + list_h + WELCOME_LINE_GAP;
    m->laid_out = true;
}

static int sm_hit(const StartMenu *m, int x, int y)
{
    for (int i = 0; i < m->count; i++) {
        const UiRect *r = &m->rect[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return sm_selectable(m, i) ? i : -1;
    }
    return -1;
}

/* ── Setup ─────────────────────────────────────────────────────────────── */

void start_menu_init(StartMenu *m, const char *title, const char *instructions, uint32_t now)
{
    memset(m, 0, sizeof(*m));
    if (title)        text_to_uppercase(m->title, title, sizeof(m->title));
    if (instructions) text_to_uppercase(m->instructions, instructions, sizeof(m->instructions));
    m->players_idx = -1;
    m->pressed = -1;
    m->shown = true;
    /* Seeded as held / touching: the press that opened the menu, or a
     * button still down from the screen before, must be released and pressed
     * again before it counts — the house rule for every edge-driven widget. */
    for (int b = 0; b < BTN_ID_COUNT; b++) m->prev_held[b] = true;
    m->was_touching = true;
    m->last_input_ms = now;
    m->cycle_origin_ms = now;
    m->attract = SM_ATTRACT_MENU;
    sm_restart_blink(m, now);
}

/* Back on the MENU page from a game: everything start_menu_init() seeds that
 * is about time and input, but the entries, the selection and every choice
 * value stay as the player left them. */
void start_menu_reopen(StartMenu *m, uint32_t now)
{
    for (int b = 0; b < BTN_ID_COUNT; b++) m->prev_held[b] = true;
    m->was_touching = true;
    m->pressed = -1;
    m->shown = true;
    m->last_input_ms = now;
    m->cycle_origin_ms = now;
    m->demo_over = false;
    m->attract = SM_ATTRACT_MENU;
    sm_restart_blink(m, now);
}

void start_menu_set_attract(StartMenu *m, bool has_demo)
{
    m->has_demo = has_demo;
}

SmAttract start_menu_attract(const StartMenu *m)
{
    return m->attract;
}

void start_menu_demo_over(StartMenu *m)
{
    if (m->attract == SM_ATTRACT_DEMO) m->demo_over = true;
}

uint32_t start_menu_idle_ms(const StartMenu *m, uint32_t now)
{
    return now - m->last_input_ms;
}

void start_menu_set_subtitle(StartMenu *m, const char *subtitle)
{
    m->subtitle[0] = '\0';
    if (subtitle) text_to_uppercase(m->subtitle, subtitle, sizeof(m->subtitle));
    m->laid_out = false;
    m->dirty = true;
}

void start_menu_set_warning(StartMenu *m, const char *warning)
{
    sm_set_warning(m, warning, false);
}

static int sm_add(StartMenu *m, StartMenuKind kind, const char *label)
{
    if (m->count >= SM_MAX_ENTRIES) {
        fprintf(stderr, "start_menu: menu \"%s\" is full (%d entries), entry \"%s\" NOT ADDED\n", m->title, SM_MAX_ENTRIES, label ? label : "?");
        return -1;
    }
    StartMenuEntry *e = &m->entries[m->count];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    if (label) text_to_uppercase(e->label, label, sizeof(e->label));
    m->laid_out = false;
    m->dirty = true;
    return m->count++;
}

int start_menu_add_action(StartMenu *m, const char *label)
{
    return sm_add(m, SM_ACTION, label);
}

int start_menu_add_choice(StartMenu *m, const char *label,
                          const char *const *values, int nvalues, int initial)
{
    if (!values || nvalues < 1) return -1;
    int i = sm_add(m, SM_CHOICE, label);
    if (i < 0) return -1;
    StartMenuEntry *e = &m->entries[i];
    e->values  = values;
    e->nvalues = nvalues > SM_MAX_VALUES ? SM_MAX_VALUES : nvalues;
    e->value   = (initial >= 0 && initial < e->nvalues) ? initial : 0;
    return i;
}

int start_menu_players(StartMenu *m)
{
    int i = start_menu_add_choice(m, "", sm_player_values, 2, 0);
    if (i >= 0) {
        m->players_idx = i;
        m->entries[i].value_off = 2u;   /* until the first update reads the mask */
    }
    return i;
}

void start_menu_set_disabled(StartMenu *m, int idx, bool disabled)
{
    if (idx < 0 || idx >= m->count || m->entries[idx].disabled == disabled) return;
    m->entries[idx].disabled = disabled;
    if (disabled && idx == m->sel) m->sel = sm_walk(m, idx, 1);
    if (m->pressed == idx) m->pressed = -1;
    m->dirty = true;
}

void start_menu_select(StartMenu *m, int idx, uint32_t now)
{
    if (!sm_selectable(m, idx)) return;
    m->sel = idx;
    sm_restart_blink(m, now);
}

/* ── One frame ─────────────────────────────────────────────────────────── */

/* Activate the selected entry from a key: an ACTION returns its index, a
 * CHOICE steps forward. */
static int sm_activate(StartMenu *m)
{
    if (!sm_selectable(m, m->sel)) return SM_NONE;
    if (m->entries[m->sel].kind == SM_ACTION) return m->sel;
    sm_cycle(m, m->sel, 1);
    return SM_NONE;
}

int start_menu_step(StartMenu *m, const InputState *in, int tx, int ty, bool touching,
                    int player_mask, uint32_t now, bool *ping)
{
    int result = SM_NONE;
    if (ping) *ping = false;
    if (!m->laid_out) sm_layout(m);
    sm_sync_players(m, player_mask);

    /* Own edges from the level, so the seeded prev_held[] can refuse a
     * button that was already down.  `pressed` counts as held for this
     * frame: a press and release inside one poll still makes an edge. */
    bool edge[BTN_ID_COUNT];
    for (int b = 0; b < BTN_ID_COUNT; b++) {
        bool held = in && in->buttons[b].held;
        bool down = held || (in && in->buttons[b].pressed);
        edge[b] = down && !m->prev_held[b];
        m->prev_held[b] = held;
    }

    bool tpress = touching && !m->was_touching;
    bool trel   = !touching && m->was_touching;
    m->was_touching = touching;
    if (touching) { m->last_tx = tx; m->last_ty = ty; }

    /* The idle clock.  Input is any key edge or a finger on the glass; one
     * that arrives while DEMO or SCORES is on screen brings the menu back
     * and is spent doing so.  The edges above are already consumed and a
     * swallowed touch never sets `pressed`, so neither can act later. */
    bool input = touching;
    for (int b = 0; b < BTN_ID_COUNT; b++) input = input || edge[b];
    bool swallow = input && m->attract != SM_ATTRACT_MENU;
    if (input) { m->last_input_ms = now; m->cycle_origin_ms = now; m->demo_over = false; }
    uint32_t idle = now - m->last_input_ms;
    /* A demo that ended early moves the attract clock's origin back so that
     * now is the first instant of the SCORES slot after it; the sound clock
     * (idle) is untouched.  Wrap-safe: both are differences of uint32_t. */
    if (m->demo_over && m->attract == SM_ATTRACT_DEMO) {
        uint32_t slot = sm_div15(sm_div1000(now - m->cycle_origin_ms));
        m->cycle_origin_ms = now - (slot + 1u) * SM_ATTRACT_MS;
    }
    m->demo_over = false;
    SmAttract ph = start_menu_attract_at(now - m->cycle_origin_ms, m->has_demo);
    if (ph != m->attract) {
        m->attract = ph;
        m->pressed = -1;
        m->dirty = true;
        if (ph == SM_ATTRACT_MENU) { m->shown = true; sm_restart_blink(m, now); }
    }
    if (ph != SM_ATTRACT_MENU) return SM_NONE;   /* the game's screen, silent */

    if (swallow) {
        /* spent on bringing the menu back */
    } else if (tpress) {
        /* A real touch hides the brackets and keeps the selection — the
         * IconGridFocus rule; the first key afterwards only reveals them. */
        if (m->shown) { m->shown = false; m->dirty = true; }
        m->pressed = sm_hit(m, tx, ty);
        if (m->pressed >= 0) m->dirty = true;
    } else if (trel) {
        int idx = m->pressed;
        m->pressed = -1;
        /* Acts on release over the SAME entry it was pressed on; sliding off
         * cancels.  The release is judged where the finger was last seen. */
        if (idx >= 0) {
            m->dirty = true;
            if (sm_hit(m, m->last_tx, m->last_ty) == idx) {
                if (idx != m->sel) { m->sel = idx; sm_restart_blink(m, now); }
                if (m->entries[idx].kind == SM_ACTION) result = idx;
                else sm_cycle(m, idx, 1);
            }
        }
    } else if (!touching) {
        /* Keys: one per frame, a finger down beats them.  With the brackets
         * hidden, the first key reveals them and does nothing else. */
        bool act = edge[BTN_ID_JUMP] || edge[BTN_ID_ACTION];
        int  dir = edge[BTN_ID_UP] ? BTN_ID_UP : edge[BTN_ID_DOWN] ? BTN_ID_DOWN
                 : edge[BTN_ID_LEFT] ? BTN_ID_LEFT : edge[BTN_ID_RIGHT] ? BTN_ID_RIGHT : -1;
        /* PAUSE is Escape on a keyboard: back, as in the launcher and the
         * control panel — it once activated the selection, so Esc started. */
        if (edge[BTN_ID_BACK] || edge[BTN_ID_PAUSE]) {
            result = SM_EXIT;
        } else if ((act || dir >= 0) && !m->shown) {
            m->shown = true;
            sm_restart_blink(m, now);
        } else if (act) {
            result = sm_activate(m);
        } else if (dir == BTN_ID_UP || dir == BTN_ID_DOWN) {
            int to = sm_walk(m, m->sel, dir == BTN_ID_UP ? -1 : 1);
            if (to != m->sel) { m->sel = to; sm_restart_blink(m, now); }
        } else if (dir >= 0 && sm_selectable(m, m->sel)) {
            sm_cycle(m, m->sel, dir == BTN_ID_LEFT ? -1 : 1);
        }
    }

    /* Blink and ping last, so a selection moved this frame starts its blink 0
     * — and pings — in the same frame.  Hidden brackets neither blink nor ping. */
    if (m->shown) {
        bool on;
        uint32_t idx;
        start_menu_phase(now - m->t0, &on, &idx);
        if (on != m->marker_on) { m->marker_on = on; m->dirty = true; }
        if (start_menu_ping_due(on, idx, idle, &m->seen_on_idx) && ping) *ping = true;
    }
    return result;
}

int start_menu_update(StartMenu *m, const InputState *in, int tx, int ty, bool touching,
                      const GamepadManager *gm, Audio *audio, uint32_t now)
{
    bool ping = false;
    int mask = gm ? gamepad_player_mask(gm) : 1;
    int r = start_menu_step(m, in, tx, ty, touching, mask, now, &ping);
    if (ping) audio_ping(audio);
    return r;
}

/* Off the MENU page the game draws, so start_menu_draw() never runs to clear
 * the flag: it is consumed here instead, and only the page edge reports. */
bool start_menu_needs_redraw(StartMenu *m)
{
    bool d = m->dirty;
    if (m->attract != SM_ATTRACT_MENU) m->dirty = false;
    return d;
}

int start_menu_value(const StartMenu *m, int idx)
{
    if (idx < 0 || idx >= m->count || m->entries[idx].kind != SM_CHOICE) return -1;
    return m->entries[idx].value;
}

int start_menu_player_count(const StartMenu *m)
{
    return m->players_idx >= 0 ? m->entries[m->players_idx].value + 1 : 1;
}

/* ── Draw ──────────────────────────────────────────────────────────────── */

/* '\n'-split, each line centred: fb_draw_text() does not interpret '\n'. */
static void sm_draw_block(Framebuffer *fb, int cx, int y, const char *text,
                          uint32_t color, int scale)
{
    int lh = text_measure_height(scale);
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[128];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (len) text_draw_centered(fb, cx, y + lh / 2, line, color, scale);
        y += lh + WELCOME_LINE_GAP;
        if (!nl) break;
        p = nl + 1;
    }
}

void start_menu_draw(StartMenu *m, Framebuffer *fb)
{
    if (!m->laid_out) sm_layout(m);
    fb_clear(fb, COLOR_BLACK);

    /* Text is only seen, so it centres on the VISIBLE screen; the rows are
     * pressed, so they were laid out in the SAFE one. */
    int cx = SCREEN_VISIBLE_LEFT + SCREEN_VISIBLE_WIDTH / 2;
    text_draw_centered(fb, cx, m->title_y + text_measure_height(SM_TITLE_SCALE) / 2,
                       m->title, COLOR_CYAN, SM_TITLE_SCALE);
    if (m->subtitle[0])
        sm_draw_block(fb, cx, m->sub_y, m->subtitle, COLOR_YELLOW, 2);
    if (m->instructions[0])
        sm_draw_block(fb, cx, m->inst_y, m->instructions, COLOR_WHITE, WELCOME_INST_SCALE);

    int bw = text_measure_width(">", SM_ENTRY_SCALE);
    for (int i = 0; i < m->count; i++) {
        const UiRect *r = &m->rect[i];
        char s[96];
        sm_entry_text(&m->entries[i], s, sizeof(s));
        uint32_t col = m->entries[i].disabled ? SM_COLOR_GREY
                     : i == m->pressed        ? COLOR_CYAN
                     : i == m->sel            ? BTN_COLOR_HIGHLIGHT
                     :                          COLOR_WHITE;
        int rcx = r->x + r->w / 2, rcy = r->y + r->h / 2;
        text_draw_centered(fb, rcx, rcy, s, col, SM_ENTRY_SCALE);
        if (i == m->sel && m->shown && m->marker_on) {
            int half = text_measure_width(s, SM_ENTRY_SCALE) / 2;
            text_draw_centered(fb, rcx - half - SM_BRACKET_GAP - bw / 2, rcy, ">", col, SM_ENTRY_SCALE);
            text_draw_centered(fb, rcx + half + SM_BRACKET_GAP + bw / 2, rcy, "<", col, SM_ENTRY_SCALE);
        }
    }

    if (m->warning[0])
        sm_draw_block(fb, cx, m->warn_y, m->warning, BTN_COLOR_WARNING, WELCOME_INST_SCALE);
    m->dirty = false;
}

/* The attract cycle's SCORES page: the menu's own title where it sits on the
 * MENU page, then the table (hs_draw() brings its own heading), narrower than
 * the screen so the rank and the score stay near the eye. */
void start_menu_draw_scores_page(Framebuffer *fb, const char *title, const HighScoreTable *t)
{
    fb_clear(fb, COLOR_BLACK);
    int cx = SCREEN_VISIBLE_LEFT + SCREEN_VISIBLE_WIDTH / 2;
    int title_y = SCREEN_VISIBLE_TOP + SM_TITLE_TOP;
    text_draw_centered(fb, cx, title_y + text_measure_height(SM_TITLE_SCALE) / 2,
                       title, COLOR_CYAN, SM_TITLE_SCALE);
    int w = SCREEN_VISIBLE_WIDTH - 40;
    if (w > 480) w = 480;
    int y = title_y + text_measure_height(SM_TITLE_SCALE) + 30;
    hs_draw(fb, t, LAYOUT_CENTER_X(w), y, w);
}

void start_menu_draw_scores(StartMenu *m, Framebuffer *fb, const HighScoreTable *t)
{
    start_menu_draw_scores_page(fb, m->title, t);
}
