/*
 * jv_ui.c — the Ludkino screens.  Draw and focus only; jv_core decides.  See jv_ui.h.
 * Every pressable widget of a screen is a rectangle in a UiZone, so touch (press and release on the same widget) and
 * pad / keyboard (ui_zone pane cycle) reach one dispatch function, act().
 */
#include "jv_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jf_login.h"
#include "jv_app.h"
#include "keyboard.h"
#include "font.h"

#define BG      RGB(16, 18, 26)
#define PANEL   RGB(30, 36, 52)
#define BTN_BG  RGB(40, 64, 104)
#define BTN_HL  RGB(70, 104, 160)
#define DIM     RGB(140, 150, 170)
#define RING    RGB(255, 220, 60)
#define RED_BG  RGB(90, 40, 40)

/* zone numbers: the bar is always 0, the screen's main widgets 1, then the button rows */
enum { Z_BAR = 0, Z_MAIN, Z_B2, Z_B3, Z_N };
/* action ids of the main zones */
enum { A_SIGNIN = 3, A_EXIT = 4, A_REUSE = 5, A_SEARCH = 6, A_AUDIO = 0, A_SUB = 1, A_PLAY = 0, A_FROM = 1, A_CAST = 2, A_SIMILAR = 3 };

static bool in_rect(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static void zone_add(UiZone *z, UiRect r, int id) { if (z->n < UZ_BUTTONS) { z->r[z->n] = r; z->id[z->n] = id; z->n++; } }
static bool bp(const InputState *in, int id) { return in && in->buttons[id].pressed; }

static int emit(JvEvent *out, int *n, int max, JvEventType type)
{
    if (*n >= max) return -1;
    memset(&out[*n], 0, sizeof out[*n]);
    out[*n].type = type;
    return (*n)++;
}

static void mmss(char *o, size_t cap, long s)
{
    if (s >= 3600) snprintf(o, cap, "%ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
    else snprintf(o, cap, "%ld:%02ld", s / 60, s % 60);
}

/* ── list source ───────────────────────────────────────────────────────── */

static bool is_grid(const JvCore *c) { return c->screen == JV_SCR_LIST && (c->kind == JV_K_MOVIES || c->kind == JV_K_SHOWS); }
static bool lv_screen(const JvCore *c)
{
    return (c->screen == JV_SCR_LIST && !is_grid(c)) || c->screen == JV_SCR_SERIES || c->screen == JV_SCR_SEASON ||
           c->screen == JV_SCR_CAST || c->screen == JV_SCR_DETAILS;
}
static int item_count(const JvCore *c) { return c->list ? c->list->n : 0; }

static int src_count(void *ctx)
{
    JvUi *ui = ctx;
    const JvCore *c = ui->core;
    if (c->screen == JV_SCR_DETAILS) return ui->nlines;
    if (c->screen == JV_SCR_CAST) return c->detail.n_people;
    return item_count(c);
}

static void item_texts(JvUi *ui, const JvItem *it, const char **title, char *det, size_t dcap)
{
    char *t = ui->rowtxt[ui->rowtxt_i++ & 31];
    char rt[16] = "";
    det[0] = '\0';
    snprintf(t, 64, "%.63s", it->b.name);
    if (it->type == JV_EPISODE) {
        snprintf(t, 64, "%d  %.50s", it->episode_no, it->b.name);
        if (it->b.runtime_s > 0) mmss(rt, sizeof rt, it->b.runtime_s);
        snprintf(det, dcap, "%s%s%s", it->b.played ? "SEEN " : "", rt, it->b.position_s > 0 ? " >" : "");
    } else if (it->type == JV_SEASON) {
        if (it->b.unplayed > 0) snprintf(det, dcap, "%d UNWATCHED", it->b.unplayed);
    } else if (it->type == JV_PERSON) snprintf(det, dcap, "PERSON");
    else if (it->type == JV_GENRE) snprintf(det, dcap, "GENRE");
    else {
        if (it->b.runtime_s > 0) mmss(rt, sizeof rt, it->b.runtime_s);
        char pos[16] = "";
        if (it->b.position_s > 0) { char p[12]; mmss(p, sizeof p, it->b.position_s); snprintf(pos, sizeof pos, " > %s", p); }
        if (it->b.year > 0) { char tmp[64]; snprintf(tmp, sizeof tmp, "%d  %s%s", it->b.year, rt, pos); snprintf(det, dcap, "%.39s", tmp); }
        else snprintf(det, dcap, "%s%s", rt, pos);
    }
    *title = t;
}

static void src_row(void *ctx, int index, LvRow *out)
{
    JvUi *ui = ctx;
    const JvCore *c = ui->core;
    static char det[40];
    if (c->screen == JV_SCR_DETAILS) { out->title = ui->lines[index]; out->glyph = LV_GLYPH_BLANK; return; }
    if (c->screen == JV_SCR_CAST) {
        const JvPerson *p = &c->detail.people[index];
        out->title = p->name; out->detail = p->role; out->glyph = LV_GLYPH_BLANK;
        return;
    }
    const JvItem *it = &c->list->items[index];
    item_texts(ui, it, &out->title, det, sizeof det);
    out->detail = det;
    out->glyph = LV_GLYPH_BLANK;
    const char *id, *tag;
    jv_item_art(it, &id, &tag);
    if (id[0] && ui->art_thumb) { jf_art_request_ex(ui->art_thumb, id, tag); out->icon = jf_art_get_ex(ui->art_thumb, id, tag); }
}

/* ── details text ──────────────────────────────────────────────────────── */

static void add_wrapped(JvUi *ui, const char *s, int width)
{
    while (*s && ui->nlines < JV_UI_LINES) {
        while (*s == ' ') s++;
        int len = (int)strlen(s), cut = len;
        if (cut > width) {
            cut = width;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (cut == 0) cut = width;
        }
        int k = cut < JV_UI_LINE_W - 1 ? cut : JV_UI_LINE_W - 1;
        memcpy(ui->lines[ui->nlines], s, (size_t)k);
        ui->lines[ui->nlines][k] = '\0';
        ui->nlines++;
        s += cut;
    }
}

static void build_lines(JvUi *ui)
{
    const JvDetail *d = &ui->core->detail;
    char t[96];
    int w = (ui->L.text.w - LV_BAR_W - 20) / 12;   /* scale-2 glyphs are 12 px wide */
    if (w > JV_UI_LINE_W - 1) w = JV_UI_LINE_W - 1;
    ui->nlines = 0;
    add_wrapped(ui, d->it.b.name, w);
    char rt[16] = "";
    if (d->it.b.runtime_s > 0) mmss(rt, sizeof rt, d->it.b.runtime_s);
    snprintf(t, sizeof t, "%d  %s  %s", d->it.b.year, rt, d->rating);
    if (d->it.b.year > 0 || rt[0]) add_wrapped(ui, t, w);
    if (d->genres[0]) add_wrapped(ui, d->genres, w);
    add_wrapped(ui, d->overview, w);
    snprintf(ui->detail_for, sizeof ui->detail_for, "%s", d->it.b.id);
}

/* ── init / free ───────────────────────────────────────────────────────── */

void jv_ui_init(JvUi *ui, Framebuffer *fb, TouchInput *touch, GamepadManager *gm, const JvCore *core, const char *art_dir)
{
    memset(ui, 0, sizeof *ui);
    ui->fb = fb; ui->touch = touch; ui->gm = gm; ui->core = core;
    JvExtents e;
    jv_ui_extents_live(&e);
    jv_ui_layout(&ui->L, &e);
    top_bar_init(&ui->bar, true, true);
    ui->bar.title = JF_APP_TITLE;
    ui->bar.title_color = COLOR_WHITE;
    ui->bar.title_cut = true;
    top_bar_layout(&ui->bar);
    LvSource src = { src_count, src_row, ui };
    lv_init(&ui->lv, src, ui->L.list, 40);
    ui->art_grid = jf_art_create(art_dir, JV_UI_ART_GRID, 20);
    ui->art_thumb = jf_art_create(art_dir, JV_UI_ART_THUMB, 24);
    ui->art_poster = jf_art_create(art_dir, JV_UI_ART_POSTER, 2);
    ui->last_screen = (JvScreen)-1;
    ui->press_zone = ui->press_id = -1;
    touch_guard_init(&ui->guard);
    ui->zf.shown = true;
    ui->dirty = true;
}

void jv_ui_free(JvUi *ui)
{
    lv_free(&ui->lv);
    jf_art_destroy(ui->art_grid); jf_art_destroy(ui->art_thumb); jf_art_destroy(ui->art_poster);
    ui->art_grid = ui->art_thumb = ui->art_poster = NULL;
    jf_wipe(ui->fields, sizeof ui->fields);
}

void jv_ui_art_server(JvUi *ui, const char *host, int port, bool tls, const char *client, const char *device,
                      const char *device_id, const char *token)
{
    JfArt *a[3] = { ui->art_grid, ui->art_thumb, ui->art_poster };
    for (int i = 0; i < 3; i++) if (a[i]) {
        jf_art_set_tls_ex(a[i], tls);
        jf_art_set_server_ex(a[i], host, port, client, device, device_id, token);
    }
}

void jv_ui_art_poll(JvUi *ui, long long now_ms)
{
    JfArt *a[3] = { ui->art_grid, ui->art_thumb, ui->art_poster };
    unsigned g = 0;
    for (int i = 0; i < 3; i++) if (a[i]) { jf_art_poll_ex(a[i], now_ms); g += jf_art_generation_ex(a[i]); }
    if (g != ui->seen_art) { ui->seen_art = g; ui->dirty = true; }
}

void jv_ui_mark_dirty(JvUi *ui) { ui->dirty = true; }
bool jv_ui_take_dirty(JvUi *ui) { bool d = ui->dirty; ui->dirty = false; return d; }

bool jv_ui_view(const JvUi *ui, int *top, int *cursor)
{
    const JvCore *c = ui->core;
    if (c->screen != ui->last_screen) return false;
    if (is_grid(c)) { *top = ui->gtop * JV_UI_COLS; *cursor = ui->gcur; return true; }
    if (!lv_screen(c) || c->screen == JV_SCR_DETAILS || c->screen == JV_SCR_CAST) return false;
    *top = ui->lv.top; *cursor = ui->lv.cursor;
    return true;
}

/* ── zones: what is present and pressable, per screen ──────────────────── */

typedef struct { const char *label; int scale; } Lbl;

static int home_tiles(const JvCore *c, int ids[JV_UI_TILES])
{
    int n = 0;
    if (c->home_resume_n > 0) ids[n++] = JV_TILE_RESUME;
    if (c->home_next_n > 0) ids[n++] = JV_TILE_NEXTUP;
    if (c->home_movies) ids[n++] = JV_TILE_MOVIES;
    if (c->home_shows) ids[n++] = JV_TILE_SHOWS;
    ids[n++] = A_SEARCH;
    return n;
}

static bool signin_can(const JvUi *ui) { return ui->fields[0][0] && ui->fields[1][0]; }
static bool can_play(const JvCore *c) { return c->detail_ready && c->detail.msid[0]; }
static bool resumable(const JvCore *c) { return c->detail.it.b.position_s > 0; }

/* details action row: the present buttons in order PLAY / RESUME, FROM START, CAST, SIMILAR */
static int act_ids(const JvCore *c, int ids[JV_UI_ACT])
{
    int n = 0;
    if (can_play(c)) { ids[n++] = A_PLAY; if (resumable(c)) ids[n++] = A_FROM; }
    if (c->detail_ready && c->detail.n_people > 0) ids[n++] = A_CAST;
    if (c->detail_ready) ids[n++] = A_SIMILAR;
    return n;
}

static int grid_slots(const JvUi *ui)
{
    int left = item_count(ui->core) - ui->gtop * JV_UI_COLS;
    return left < 0 ? 0 : left > JV_UI_CELLS ? JV_UI_CELLS : left;
}

static int build_zones(JvUi *ui, UiZone z[UZ_ZONES])
{
    const JvCore *c = ui->core;
    const JvLayout *L = &ui->L;
    memset(z, 0, UZ_ZONES * sizeof z[0]);
    if (ui->bar.has_back) zone_add(&z[Z_BAR], top_bar_ring_rect(&ui->bar.L, TB_BACK), 0);
    if (ui->bar.has_exit) zone_add(&z[Z_BAR], top_bar_ring_rect(&ui->bar.L, TB_EXIT), 1);
    switch (c->screen) {
    case JV_SCR_SIGNIN:
        for (int i = 0; i < JV_UI_FIELDS; i++) zone_add(&z[Z_MAIN], L->field[i], i);
        if (signin_can(ui)) zone_add(&z[Z_B2], L->signin, A_SIGNIN);
        if (c->reuse_available) zone_add(&z[Z_B2], L->reuse, A_REUSE);
        zone_add(&z[Z_B2], L->signexit, A_EXIT);
        break;
    case JV_SCR_HOME: {
        int ids[JV_UI_TILES], n = home_tiles(c, ids);
        for (int i = 0; i < n; i++) zone_add(&z[Z_MAIN], L->tile[i], ids[i]);
        break;
    }
    case JV_SCR_PLAYER: break;
    case JV_SCR_DETAILS: {
        z[Z_MAIN].lv = &ui->lv;
        if (c->detail_ready && c->detail.n_audio > 1) zone_add(&z[Z_B2], L->trk[0], A_AUDIO);
        if (c->detail_ready && c->detail.n_subs > 0) zone_add(&z[Z_B2], L->trk[1], A_SUB);
        int ids[JV_UI_ACT], n = act_ids(c, ids);
        for (int i = 0; i < n; i++) zone_add(&z[Z_B3], L->act[i], ids[i]);
        break;
    }
    default:
        if (is_grid(c)) { int n = grid_slots(ui); for (int i = 0; i < n; i++) zone_add(&z[Z_MAIN], L->cell[i], i); }
        else {
            z[Z_MAIN].lv = &ui->lv;
            if (c->screen == JV_SCR_LIST && c->kind == JV_K_SEARCH) zone_add(&z[Z_B2], L->sbtn, A_SEARCH);
        }
        break;
    }
    return Z_N;
}

/* ── screen changes ────────────────────────────────────────────────────── */

static UiRect list_rect(const JvUi *ui)
{
    const JvCore *c = ui->core;
    if (c->screen == JV_SCR_DETAILS) return ui->L.text;
    return (c->screen == JV_SCR_LIST && c->kind == JV_K_SEARCH) ? ui->L.list_s : ui->L.list;
}

static void begin_screen(JvUi *ui)
{
    const JvCore *c = ui->core;
    if (c->screen == JV_SCR_PLAYER && ui->last_screen != JV_SCR_PLAYER) {   /* entering: the overlay starts up, for the auto-hide time */
        ui->ov_shown = ui->ov_fresh = ui->bands_dirty = true;
        ui->ov_ring = false; ui->ov_focus = -1;
        memset(&ui->sig, 0, sizeof ui->sig);
        ui->eq_up = false;
    } else if (c->screen != JV_SCR_PLAYER) ui->ov_shown = ui->eq_up = false;
    ui->last_screen = c->screen;
    ui->last_kind = c->kind;
    bool shown = ui->zf.shown;
    uz_begin_cycle(&ui->zf, Z_MAIN);
    ui->zf.shown = shown;
    ui->bar.has_back = c->screen != JV_SCR_SIGNIN && c->screen != JV_SCR_HOME;
    ui->bar.has_exit = true;
    top_bar_layout(&ui->bar);
    ui->gtop = ui->gcur = 0;
    ui->press_zone = ui->press_id = -1;
    ui->detail_for[0] = '\0';
    ui->nlines = 0;
    if (c->screen == JV_SCR_SIGNIN) {
        if (!ui->fields[0][0]) snprintf(ui->fields[0], sizeof ui->fields[0], "%s", c->signin_server);
        if (!ui->fields[1][0]) snprintf(ui->fields[1], sizeof ui->fields[1], "%s", c->signin_user);
        snprintf(ui->pre_server, sizeof ui->pre_server, "%s", c->signin_server);
        snprintf(ui->pre_user, sizeof ui->pre_user, "%s", c->signin_user);
    }
    if (lv_screen(c)) {
        ui->lv.rect = list_rect(ui);
        lv_reset(&ui->lv);
        if (c->screen == JV_SCR_DETAILS) { ui->lv.row_h = 28; ui->lv.scale = 2; }
        else { ui->lv.row_h = 44; ui->lv.scale = 2; }
    }
    ui->dirty = true;
}

static void restore_view(JvUi *ui)
{
    const JvCore *c = ui->core;
    if (c->restore_seq != ui->seen_restore) { ui->seen_restore = c->restore_seq; ui->restore_pend = true; }
    if (!ui->restore_pend) return;
    if (!is_grid(c) && !(lv_screen(c) && c->screen != JV_SCR_DETAILS)) { ui->restore_pend = false; return; }
    int cnt = item_count(c);
    bool more = c->list && (c->list->pending || c->busy);
    if (cnt <= c->restore_cursor && more) return;
    ui->restore_pend = false;
    if (is_grid(c)) {
        ui->gtop = c->restore_top / JV_UI_COLS;
        ui->gcur = c->restore_cursor < cnt ? c->restore_cursor : (cnt > 0 ? cnt - 1 : 0);
        if (ui->gtop * JV_UI_COLS >= cnt) ui->gtop = 0;
    } else {
        lv_sync(&ui->lv);
        ui->lv.top = c->restore_top; ui->lv.cursor = c->restore_cursor; ui->lv.cursor_dragged = false;
        lv_sync(&ui->lv);
    }
    ui->dirty = true;
}

static void drain_after_keyboard(JvUi *ui, Pointer *ptr)
{
    InputState scratch;
    memset(&scratch, 0, sizeof scratch);
    for (int i = 0; i < 3; i++) { gamepad_poll(ui->gm, &scratch, 0, 0, false); usleep(20000); }
    touch_poll(ui->touch);
    if (ptr) { pointer_drain(ptr); pointer_invalidate(ptr); }
    ui->dirty = true;
}

static void edit_field(JvUi *ui, Pointer *ptr, int i)
{
    static const char *title[JV_UI_FIELDS] = { "SERVER HOST OR HOST:PORT", "USER", "PASSWORD" };
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s", ui->fields[i]);
    int rc = i == 2 ? keyboard_enter_masked(ui->fb, ui->touch, ui->gm, title[i], tmp, 63, KB_LAYOUT_FULL)
                    : keyboard_enter(ui->fb, ui->touch, ui->gm, title[i], tmp, 63, KB_LAYOUT_FULL);
    if (rc == KB_RESULT_OK) snprintf(ui->fields[i], sizeof ui->fields[i], "%s", tmp);
    jf_wipe(tmp, sizeof tmp);
    drain_after_keyboard(ui, ptr);
}

static void do_search(JvUi *ui, Pointer *ptr, JvEvent *out, int *n, int max)
{
    char term[64];
    snprintf(term, sizeof term, "%.63s", ui->core->search_term);
    int rc = keyboard_enter(ui->fb, ui->touch, ui->gm, "SEARCH", term, 63, KB_LAYOUT_FULL);
    drain_after_keyboard(ui, ptr);
    if (rc != KB_RESULT_OK || !term[0]) return;
    int k = emit(out, n, max, JV_EV_SEARCH);
    if (k >= 0) snprintf(out[k].term, sizeof out[k].term, "%s", term);
}

/* ── one dispatch for touch and pad ────────────────────────────────────── */

static void act(JvUi *ui, int zone, int id, Pointer *ptr, JvEvent *out, int *n, int max)
{
    const JvCore *c = ui->core;
    if (zone == Z_BAR) { emit(out, n, max, id == 0 ? JV_EV_BACK : JV_EV_QUIT); return; }
    switch (c->screen) {
    case JV_SCR_SIGNIN:
        if (zone == Z_MAIN) edit_field(ui, ptr, id);
        else if (id == A_SIGNIN && signin_can(ui)) {
            int k = emit(out, n, max, JV_EV_SIGNIN);
            if (k >= 0) {
                snprintf(out[k].server, sizeof out[k].server, "%s", ui->fields[0]);
                snprintf(out[k].user, sizeof out[k].user, "%s", ui->fields[1]);
                snprintf(out[k].password, sizeof out[k].password, "%s", ui->fields[2]);
                jf_wipe(ui->fields[2], sizeof ui->fields[2]);
            }
        } else if (id == A_REUSE) emit(out, n, max, JV_EV_REUSE_LOGIN);
        else if (id == A_EXIT) emit(out, n, max, JV_EV_QUIT);
        break;
    case JV_SCR_HOME:
        if (id == A_SEARCH) do_search(ui, ptr, out, n, max);
        else { int k = emit(out, n, max, JV_EV_HOME); if (k >= 0) out[k].i = id; }
        break;
    case JV_SCR_PLAYER: break;   /* the player has its own input, player_input() */
    case JV_SCR_DETAILS:
        if (zone == Z_B2) emit(out, n, max, id == A_AUDIO ? JV_EV_AUDIO_NEXT : JV_EV_SUB_NEXT);
        else if (zone == Z_B3) {
            if (id == A_PLAY || id == A_FROM) {
                int k = emit(out, n, max, JV_EV_PLAY);
                if (k >= 0) out[k].i = id == A_PLAY && resumable(c) ? c->detail.it.b.position_s * 1000 : 0;
            } else emit(out, n, max, id == A_CAST ? JV_EV_CAST_OPEN : JV_EV_SIMILAR);
        }
        break;
    default:
        if (zone == Z_B2) do_search(ui, ptr, out, n, max);
        else if (zone == Z_MAIN && is_grid(c)) {
            int k = emit(out, n, max, JV_EV_ACTIVATE);
            if (k >= 0) { out[k].i = ui->gtop * JV_UI_COLS + id; ui->gcur = out[k].i; }
        }
        break;
    }
}

static void overlay_input(JvUi *ui, const TouchState *ts, const InputState *in, uint32_t now, JvEvent *out, int *n, int max)
{
    const JvCore *c = ui->core;
    if (!ui->dlg_up || ui->dlg_buttons != c->overlay_buttons) {
        int cnt = 1, k = 0;
        if (c->overlay_buttons & JV_BTN_RETRY) cnt++;
        modal_dialog_init(&ui->dlg, JF_APP_TITLE, c->overlay_text, cnt);
        if (c->overlay_buttons & JV_BTN_RETRY) modal_dialog_set_button(&ui->dlg, k++, "RETRY", BTN_BG, COLOR_WHITE);
        modal_dialog_set_button(&ui->dlg, k, "CLOSE", RED_BG, COLOR_WHITE);
        modal_dialog_set_focus(&ui->dlg, 0);
        modal_dialog_show(&ui->dlg);
        ui->dlg_up = true; ui->dlg_buttons = c->overlay_buttons; ui->dirty = true;
    }
    int a = modal_dialog_input(&ui->dlg, in, ui->dlg.button_count - 1);
    if (a < 0) a = modal_dialog_update(&ui->dlg, ts->x, ts->y, ts->pressed || ts->held, now);
    if (ts->pressed || ts->released || bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN) || bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT))
        ui->dirty = true;
    if (a >= 0) {
        bool retry = (c->overlay_buttons & JV_BTN_RETRY) && a == 0;
        emit(out, n, max, retry ? JV_EV_RETRY : JV_EV_CLOSE_ERROR);
        ui->dlg_up = false; ui->dirty = true;
    }
}

/* ── player overlay: input ─────────────────────────────────────────────── */

#define PH_BACK  9      /* hit ids beyond the nine pbtn: the title row's stop, the progress track, the bare picture */
#define PH_TRACK 10
#define PH_PIC   11
#define PH_SHOW  12     /* a press while the overlay was hidden: its release shows it */
#define PH_NONE  (-1)
#define OV_HIDE_MS 4000
#define TRK_NOTE_MS 2500   /* how long the title row names a just-cycled track */

static void ov_show(JvUi *ui, uint32_t now)
{
    if (!ui->ov_shown) { ui->ov_shown = true; ui->bands_dirty = true; }
    ui->ov_last = now;
}

static void ov_hide(JvUi *ui)
{
    if (ui->ov_shown) { ui->ov_shown = false; ui->bands_dirty = true; }
}

static int ph_hit(const JvUi *ui, int x, int y)
{
    const JvLayout *L = &ui->L;
    for (int i = 0; i < JV_UI_PBTN; i++) if (in_rect(L->pbtn[i], x, y)) return i;
    if (in_rect(L->tb_back, x, y)) return PH_BACK;
    if (in_rect(L->ptrack, x, y)) return PH_TRACK;
    if (in_rect(L->pic, x, y)) return PH_PIC;
    return PH_NONE;
}

/* A bottom-bar button or the title row's back. */
static void p_press(JvUi *ui, int id, JvEvent *out, int *n, int max)
{
    int k;
    switch (id) {
    case PB_SEEK_BACK: case PB_SEEK_FWD:
        k = emit(out, n, max, JV_EV_SEEK_STEP);
        if (k >= 0) out[k].i = id == PB_SEEK_BACK ? -1 : 1;
        break;
    case PB_PLAY: emit(out, n, max, JV_EV_PAUSE); break;
    case PB_STOP: emit(out, n, max, JV_EV_STOP); break;       /* STOP forgets the film; the title row's "<" only leaves it */
    case PH_BACK: emit(out, n, max, JV_EV_BACK); break;
    case PB_EQ: ui->acts |= JV_UI_ACTB_EQ_OPEN; break;
    case PB_VOL_DN: ui->acts |= JV_UI_ACTB_VOL_DN; break;
    case PB_VOL_UP: ui->acts |= JV_UI_ACTB_VOL_UP; break;
    case PB_AUDIO: emit(out, n, max, JV_EV_AUDIO_NEXT); ui->trk_note = 1; ui->trk_t = ui->p_now; break;
    case PB_SUBS: emit(out, n, max, JV_EV_SUB_NEXT); ui->trk_note = 2; ui->trk_t = ui->p_now; break;
    default: break;
    }
    ui->bands_dirty = true;
}

/* The focus ring runs over the nine buttons (0..8) and the title row's stop (9). */
static void p_focus_rects(const JvUi *ui, UiRect r[JV_UI_PBTN + 1])
{
    for (int i = 0; i < JV_UI_PBTN; i++) r[i] = ui->L.pbtn[i];
    r[JV_UI_PBTN] = ui->L.tb_back;
}

static void p_focus_set(JvUi *ui, int f)
{
    if (f >= 0 && f != ui->ov_focus) { ui->ov_focus = f; ui->bands_dirty = true; }
}

static void p_keys(JvUi *ui, const InputState *in, uint32_t now, JvEvent *out, int *n, int max)
{
    if (!in) return;
    bool seek_l = bp(in, BTN_ID_LEFT), seek_r = bp(in, BTN_ID_RIGHT);
    bool upd = bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN), cyc = bp(in, BTN_ID_NEXT) || bp(in, BTN_ID_PREV);
    if (bp(in, BTN_ID_JUMP)) {                      /* Space / pad A: pause here, not "activate" */
        emit(out, n, max, JV_EV_PAUSE);
        ov_show(ui, now);
    } else if (seek_l || seek_r) {
        int k = emit(out, n, max, JV_EV_SEEK_STEP);
        if (k >= 0) out[k].i = seek_l ? -1 : 1;
        ov_show(ui, now);
    } else if (bp(in, BTN_ID_ACTION)) {             /* Enter */
        if (!ui->ov_shown || ui->ov_focus < 0) { ov_show(ui, now); ui->ov_ring = true; p_focus_set(ui, PB_PLAY); }
        else p_press(ui, ui->ov_focus, out, n, max);
    } else if (upd || cyc) {
        bool was = ui->ov_shown;
        ov_show(ui, now);
        ui->ov_ring = true;
        if (!was || ui->ov_focus < 0) p_focus_set(ui, PB_PLAY);
        else if (upd) {
            UiRect r[JV_UI_PBTN + 1];
            p_focus_rects(ui, r);
            int f = ui_focus_move(r, JV_UI_PBTN + 1, ui->ov_focus, bp(in, BTN_ID_UP) ? UI_DIR_UP : UI_DIR_DOWN);
            p_focus_set(ui, f);
        } else {                                    /* Tab / RB / LB step along the row (Left and Right seek) */
            int f = ui->ov_focus >= JV_UI_PBTN ? PB_PLAY : ui->ov_focus;
            f += bp(in, BTN_ID_NEXT) ? 1 : JV_UI_PBTN - 1;
            if (f >= JV_UI_PBTN) f -= JV_UI_PBTN;
            p_focus_set(ui, f);
        }
        ui->bands_dirty = true;
    } else if (bp(in, BTN_ID_PAUSE) || bp(in, BTN_ID_CANCEL) || bp(in, BTN_ID_BACK)) {   /* Esc / pad B: hide, else stop */
        if (ui->ov_shown) ov_hide(ui);
        else emit(out, n, max, JV_EV_STOP);
    }
    if (ui->ov_shown && (seek_l || seek_r || upd || cyc || bp(in, BTN_ID_ACTION) || bp(in, BTN_ID_JUMP))) ui->ov_last = now;
}

static void eq_input(JvUi *ui, const TouchState *ts, const InputState *in, uint32_t now)
{
    EqPanelEvent ev = eq_panel_input(&ui->eq, ts, in, now);
    if (ev != EQP_NONE) {
        memcpy(ui->eq_out, eq_panel_gains(&ui->eq), sizeof ui->eq_out);
        ui->enh_out = *eq_panel_enh(&ui->eq);
        ui->eq_apply = true;
        ui->eq_save = ev == EQP_DONE;
    }
    if (ev == EQP_DONE || ev == EQP_CANCEL) {
        ui->eq_up = false;
        ui->eq_closed = true;
        touch_guard_screen_changed(&ui->guard, now);   /* the finger that closed it must not press what is underneath */
    }
    if (eq_panel_take_dirty(&ui->eq) || !ui->eq_up) ui->dirty = true;
}

/* The PLAYER screen's whole input.  Touch acts on release over the widget the finger went down on. */
static void player_input(JvUi *ui, const TouchState *ts, const InputState *in, uint32_t now, JvEvent *out, int *n, int max)
{
    ui->p_now = now;
    const JvCore *c = ui->core;
    if (ui->eq_up) { eq_input(ui, ts, in, now); return; }
    if (ui->ov_fresh) { ui->ov_fresh = false; ui->ov_last = now; }
    if (ts->pressed) {
        ui->ov_ring = false;
        ui->press_id = ui->ov_shown ? ph_hit(ui, ts->x, ts->y) : PH_SHOW;
        if (ui->ov_focus >= 0 && ui->press_id >= 0) ui->bands_dirty = true;   /* the ring goes */
    }
    if (ui->ov_shown && (ts->pressed || ts->held)) ui->ov_last = now;
    if (ts->released) {
        int id = ui->press_id;
        ui->press_id = -1;
        if (id == PH_SHOW) ov_show(ui, now);
        else if (id >= 0 && ph_hit(ui, ts->x, ts->y) == id) {
            ui->ov_last = now;
            if (id == PH_PIC) ov_hide(ui);
            else if (id == PH_TRACK) {
                int k = emit(out, n, max, JV_EV_SEEK_TO);
                if (k >= 0) out[k].i = (int)jv_ui_progress_ms(&ui->L, ts->x, c->play.runtime_ms);
            } else p_press(ui, id, out, n, max);
        }
    }
    p_keys(ui, in, now, out, n, max);
    if (ui->trk_note && (uint32_t)(now - ui->trk_t) > TRK_NOTE_MS) { ui->trk_note = 0; ui->bands_dirty = true; }
    if (ui->ov_shown && c->play_state != JV_PLAY_PAUSED && (uint32_t)(now - ui->ov_last) > OV_HIDE_MS) ov_hide(ui);

    /* what the bands show: repaint them (and only them) when it changed */
    JvPSig s;
    memset(&s, 0, sizeof s);
    s.shown = ui->ov_shown;
    s.busy = c->busy;
    if (ui->ov_shown) {
        s.focus = ui->ov_ring ? ui->ov_focus : -1;
        s.state = c->play_state;
        s.vol = ui->vol_step; s.volmax = ui->vol_max;
        s.pos_s = (int)(jv_core_position(c) / 1000);
        s.dur_s = (int)(c->play.runtime_ms / 1000);
        s.clk = (int)(time(NULL) / 60);
        s.seek = c->seek_pending;
    }
    if (memcmp(&s, &ui->sig, sizeof s)) {
        if (s.busy != ui->sig.busy) ui->dirty = true;          /* BUFFERING... lives outside the bands: a full frame */
        if (s.shown) ui->bands_dirty = true;
        ui->sig = s;
    }
}

int jv_ui_input(JvUi *ui, const TouchState *ts, const InputState *in, Pointer *ptr, uint32_t now, JvEvent *out, int max)
{
    const JvCore *c = ui->core;
    int n = 0;
    if (c->screen != ui->last_screen || c->kind != ui->last_kind) {
        bool grid_was = ui->last_kind == JV_K_MOVIES || ui->last_kind == JV_K_SHOWS;
        bool newscr = c->screen != ui->last_screen;
        if (newscr || grid_was || is_grid(c) || c->kind != ui->last_kind) { begin_screen(ui); touch_guard_screen_changed(&ui->guard, now); }
    }
    TouchState gated;
    if (!touch_guard_pass(&ui->guard, ts && (ts->pressed || ts->held), now)) { memset(&gated, 0, sizeof gated); ts = &gated; }
    restore_view(ui);
    if (c->screen == JV_SCR_SIGNIN &&
        (strcmp(c->signin_server, ui->pre_server) || strcmp(c->signin_user, ui->pre_user))) {   /* REUSE SAVED LOGIN prefilled */
        snprintf(ui->pre_server, sizeof ui->pre_server, "%s", c->signin_server);
        snprintf(ui->pre_user, sizeof ui->pre_user, "%s", c->signin_user);
        snprintf(ui->fields[0], sizeof ui->fields[0], "%s", c->signin_server);
        snprintf(ui->fields[1], sizeof ui->fields[1], "%s", c->signin_user);
        if (c->focus_password) { UiZone z[UZ_ZONES]; build_zones(ui, z); uz_focus(&ui->zf, z, Z_N, Z_MAIN, 2); }
        ui->dirty = true;
    }
    if (c->screen != JV_SCR_PLAYER && top_bar_info_due(&ui->bar, now) && top_bar_info_sample(&ui->bar) && !c->overlay && !ui->dlg_up) {   /* the bar's CPU and clock, once a second */
        /* Only the bar's text changed: repaint just it; the full frame only when the text moved another section. */
        UiRect ir;
        if (ui->dirty || !top_bar_info_present(ui->fb, &ui->bar, &ir)) ui->dirty = true;
        else if (ptr) { pointer_invalidate(ptr); pointer_paint(ptr, ui->fb); }
    }
    if (c->screen == JV_SCR_DETAILS && c->detail_ready && strcmp(ui->detail_for, c->detail.it.b.id)) {
        build_lines(ui); lv_sync(&ui->lv); ui->dirty = true;
    }
    if (c->screen == JV_SCR_DETAILS && c->detail_ready && ui->art_poster) {
        const char *id, *tag;
        jv_item_art(&c->detail.it, &id, &tag);
        if (id[0]) jf_art_request_ex(ui->art_poster, id, tag);
    }
    if (c->overlay) { overlay_input(ui, ts, in, now, out, &n, max); return n; }
    ui->dlg_up = false;
    if (c->screen == JV_SCR_PLAYER) { player_input(ui, ts, in, now, out, &n, max); return n; }

    bool pad_nav = bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN) || bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT) ||
                   bp(in, BTN_ID_JUMP) || bp(in, BTN_ID_ACTION) || bp(in, BTN_ID_NEXT) || bp(in, BTN_ID_PREV);
    if (ts->pressed) ui->zf.shown = false;
    if (pad_nav) ui->zf.shown = true;
    if (ts->pressed || ts->released || pad_nav) ui->dirty = true;

    UiZone z[UZ_ZONES];
    build_zones(ui, z);

    /* touch: a press and its release on the same widget act (the bar's own hit target reaches under the digitizer's dead band) */
    if (ts->pressed) {
        ui->press_zone = ui->press_id = -1;
        TbHit h = top_bar_hit(&ui->bar.L, ts->x, ts->y);
        if (h == TB_BACK && ui->bar.has_back) { ui->press_zone = Z_BAR; ui->press_id = 0; }
        else if (h == TB_EXIT && ui->bar.has_exit) { ui->press_zone = Z_BAR; ui->press_id = 1; }
        else for (int zi = Z_MAIN; zi < Z_N && ui->press_zone < 0; zi++)
            for (int b = 0; b < z[zi].n; b++) if (in_rect(z[zi].r[b], ts->x, ts->y)) { ui->press_zone = zi; ui->press_id = z[zi].id[b]; break; }
    }
    if (ts->released) {
        int pz = ui->press_zone, pid = ui->press_id;
        ui->press_zone = ui->press_id = -1;
        bool hit = false;
        if (pz == Z_BAR) { TbHit h = top_bar_hit(&ui->bar.L, ts->x, ts->y); hit = h == (pid == 0 ? TB_BACK : TB_EXIT); }
        else if (pz >= Z_MAIN) for (int b = 0; b < z[pz].n; b++) if (z[pz].id[b] == pid && in_rect(z[pz].r[b], ts->x, ts->y)) hit = true;
        if (hit) { act(ui, pz, pid, ptr, out, &n, max); return n; }
        if (is_grid(c)) {   /* the scroll buttons */
            int rows_total = (item_count(c) + JV_UI_COLS - 1) / JV_UI_COLS;
            if (in_rect(ui->L.gup, ts->x, ts->y) && ui->gtop > 0) { ui->gtop--; ui->dirty = true; }
            if (in_rect(ui->L.gdn, ts->x, ts->y) && ui->gtop + JV_UI_ROWS < rows_total) { ui->gtop++; ui->dirty = true; }
        }
    }

    /* grid: the pad scrolls a row when it presses past the first or last visible row */
    const InputState *zin = in;
    InputState kin;
    if (is_grid(c) && in && ui->zf.zone == Z_MAIN && z[Z_MAIN].n > 0) {
        int total_rows = (item_count(c) + JV_UI_COLS - 1) / JV_UI_COLS;
        int slot = ui->zf.id < 0 ? 0 : ui->zf.id, row = slot / JV_UI_COLS;
        int scroll = 0;
        if (bp(in, BTN_ID_DOWN) && row == JV_UI_ROWS - 1 && ui->gtop + JV_UI_ROWS < total_rows) scroll = 1;
        if (bp(in, BTN_ID_UP) && row == 0 && ui->gtop > 0) scroll = -1;
        if (scroll) {
            kin = *in;
            kin.buttons[BTN_ID_UP].pressed = kin.buttons[BTN_ID_DOWN].pressed = false;
            zin = &kin;
            ui->gtop += scroll;
            int cnt = grid_slots(ui);
            if (slot >= cnt) { UiZone z2[UZ_ZONES]; build_zones(ui, z2); uz_focus(&ui->zf, z2, Z_N, Z_MAIN, cnt - 1); }
            ui->dirty = true;
            build_zones(ui, z);
        }
    }

    UzOut zo;
    uz_input(&ui->zf, z, Z_N, zin, ts->pressed, &zo);
    if (zo.moved) ui->dirty = true;
    if (zo.back && c->screen != JV_SCR_HOME && c->screen != JV_SCR_SIGNIN) emit(out, &n, max, JV_EV_BACK);
    if (is_grid(c) && ui->zf.zone == Z_MAIN && ui->zf.id >= 0 && ui->zf.id < grid_slots(ui)) ui->gcur = ui->gtop * JV_UI_COLS + ui->zf.id;

    if (lv_screen(c)) {
        ui->lv.active = ui->zf.zone == Z_MAIN;
        ui->lv.ring_hidden = !ui->zf.shown;
        ui->lv.tap2_any = false;
        lv_sync(&ui->lv);
        LvEvent ev = lv_input(&ui->lv, ts, zo.list_in, now);
        if (ev != LV_EV_NONE) ui->dirty = true;
        int arow = ui->lv.cursor;
        bool go = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE;
        if (zo.activate_row >= 0) { arow = zo.activate_row; go = true; }
        if (go && arow >= 0 && arow < src_count(ui) && c->screen != JV_SCR_DETAILS) {
            int k = emit(out, &n, max, c->screen == JV_SCR_CAST ? JV_EV_CAST_ACTIVATE : JV_EV_ACTIVATE);
            if (k >= 0) out[k].i = arow;
        }
        if (c->screen != JV_SCR_CAST && c->screen != JV_SCR_DETAILS && c->list && ui->zf.zone == Z_MAIN && src_count(ui) > 0 &&
            ui->lv.cursor >= src_count(ui) - 6 && src_count(ui) < c->list->total && !c->list->full && !c->list->pending && !c->busy)
            emit(out, &n, max, JV_EV_NEED_MORE);
    } else if (is_grid(c) && c->list && ui->gcur >= item_count(c) - 2 * JV_UI_COLS && item_count(c) > 0 &&
               item_count(c) < c->list->total && !c->list->full && !c->list->pending && !c->busy)
        emit(out, &n, max, JV_EV_NEED_MORE);

    if (zo.activate >= 0 && !(ts->released && n)) act(ui, ui->zf.zone, zo.activate, ptr, out, &n, max);
    return n;
}

/* ── drawing ───────────────────────────────────────────────────────────── */

static void ring(JvUi *ui, UiRect r)
{
    fb_draw_rect(ui->fb, r.x - 2, r.y - 2, r.w + 4, r.h + 4, RING);
    fb_draw_rect(ui->fb, r.x - 3, r.y - 3, r.w + 6, r.h + 6, RING);
}

static void draw_btn(JvUi *ui, UiRect r, const char *text, int scale, uint32_t bg)
{
    Framebuffer *fb = ui->fb;
    char t[48];
    lv_fit(t, sizeof t, text, r.w - 12, scale);
    fb_fill_rect(fb, r.x, r.y, r.w, r.h, bg);
    fb_draw_rect(fb, r.x, r.y, r.w, r.h, BTN_HL);
    fb_draw_text(fb, r.x + (r.w - text_measure_width(t, scale)) / 2, r.y + (r.h - FONT_ROWS * scale) / 2, t, COLOR_WHITE, scale);
}

static void blit_box(JvUi *ui, const LvIcon *ic, UiRect box)
{
    if (!ic || !ic->pixels || ic->w <= 0 || ic->h <= 0) { fb_draw_rect(ui->fb, box.x, box.y, box.w, box.h, PANEL); return; }
    int x = box.x + (box.w - ic->w) / 2, y = box.y + (box.h - ic->h) / 2;
    fb_blit_sprite(ui->fb, ic->pixels, ic->w, 0, 0, x, y, ic->w, ic->h, 0xFFFFFFFF);
}

static void draw_banner(JvUi *ui)
{
    const JvCore *c = ui->core;
    UiRect b = ui->L.banner;
    if (c->banner[0]) fb_draw_text(ui->fb, b.x + 4, b.y + 2, c->banner, RING, 2);
    else if (c->screen == JV_SCR_LIST && c->list && c->list->total > 0) {
        char t[32];
        snprintf(t, sizeof t, "%d OF %d", c->list->n, c->list->total);
        fb_draw_text(ui->fb, b.x + 4, b.y + 2, t, DIM, 2);
    }
    if (c->busy) {      /* on the player screen busy is the stream being opened */
        const char *w = c->screen == JV_SCR_PLAYER ? "BUFFERING..." : "LOADING";
        fb_draw_text(ui->fb, b.x + b.w - 8 - text_measure_width(w, 2), b.y + 2, w, DIM, 2);
    }
}

static void draw_grid(JvUi *ui)
{
    const JvCore *c = ui->core;
    int n = grid_slots(ui);
    for (int i = 0; i < n; i++) {
        UiRect r = ui->L.cell[i];
        const JvItem *it = &c->list->items[ui->gtop * JV_UI_COLS + i];
        const char *id, *tag;
        jv_item_art(it, &id, &tag);
        UiRect box = { r.x + (r.w - JV_UI_ART_GRID) / 2, r.y + 4, JV_UI_ART_GRID, JV_UI_ART_GRID };
        const LvIcon *ic = NULL;
        if (id[0] && ui->art_grid) { jf_art_request_ex(ui->art_grid, id, tag); ic = jf_art_get_ex(ui->art_grid, id, tag); }
        blit_box(ui, ic, box);
        char t[40];
        lv_fit(t, sizeof t, it->b.name, r.w - 8, 1);
        fb_draw_text(ui->fb, r.x + 4, box.y + box.h + 6, t, COLOR_WHITE, 1);
        if (it->b.year > 0) { char y[16]; snprintf(y, sizeof y, "%d", it->b.year); fb_draw_text(ui->fb, r.x + 4, box.y + box.h + 18, y, DIM, 1); }
        if (it->b.position_s > 0) fb_draw_text(ui->fb, box.x + 4, box.y + box.h - 12, ">", RING, 1);
        if (ui->zf.shown && ui->zf.zone == Z_MAIN && ui->zf.id == i) ring(ui, (UiRect){ box.x, box.y, box.w, box.h });
    }
    draw_btn(ui, ui->L.gup, "^", 2, BTN_BG);
    draw_btn(ui, ui->L.gdn, "v", 2, BTN_BG);
}

static void trk_label(const JvCore *c, bool audio, char *out, size_t cap)
{
    char s[64] = "OFF";
    int sel = audio ? c->sel_audio : c->sel_sub;
    if (audio ? sel >= 0 && sel < c->detail.n_audio : sel >= 0 && sel < c->detail.n_subs)
        jv_stream_label(s, sizeof s, audio ? &c->detail.audio[sel] : &c->detail.subs[sel], audio);
    snprintf(out, cap, "%s: %s", audio ? "AUDIO" : "SUBTITLES", s);
}

static void draw_details(JvUi *ui)
{
    const JvCore *c = ui->core;
    const JvLayout *L = &ui->L;
    if (!c->detail_ready) { fb_draw_text(ui->fb, L->text.x + 8, L->text.y + 8, "LOADING", DIM, 2); return; }
    const char *id, *tag;
    jv_item_art(&c->detail.it, &id, &tag);
    blit_box(ui, id[0] && ui->art_poster ? jf_art_get_ex(ui->art_poster, id, tag) : NULL, L->poster);
    lv_sync(&ui->lv);
    ui->lv.active = ui->zf.zone == Z_MAIN;
    ui->lv.ring_hidden = !ui->zf.shown;
    lv_draw(ui->fb, &ui->lv, "");
    char t[80];
    for (int i = 0; i < JV_UI_TRK; i++) {
        bool audio = i == 0;
        int cnt = audio ? c->detail.n_audio : c->detail.n_subs;
        if (cnt == 0) continue;
        trk_label(c, audio, t, sizeof t);
        draw_btn(ui, L->trk[i], t, 2, (audio ? cnt > 1 : true) ? BTN_BG : PANEL);
    }
    int ids[JV_UI_ACT], n = act_ids(c, ids);
    for (int i = 0; i < n; i++) {
        const char *lbl = "";
        char rb[32];
        switch (ids[i]) {
        case A_PLAY: if (resumable(c)) { mmss(t, sizeof t, c->detail.it.b.position_s); snprintf(rb, sizeof rb, "RESUME %.20s", t); lbl = rb; } else lbl = "PLAY"; break;
        case A_FROM: lbl = "FROM START"; break;
        case A_CAST: lbl = "CAST"; break;
        default: lbl = "SIMILAR"; break;
        }
        draw_btn(ui, L->act[i], lbl, 2, BTN_BG);
    }
}

static void draw_player_bands(JvUi *ui)
{
    if (!ui->ov_shown) return;
    const JvCore *c = ui->core;
    const JvLayout *L = &ui->L;
    Framebuffer *fb = ui->fb;
    char t[64];
    UiRect tb = L->top_band, bb = L->bot_band;

    fb_fill_rect(fb, tb.x, tb.y, tb.w, tb.h, BG);
    fb_fill_rect(fb, tb.x, tb.y + tb.h - 1, tb.w, 1, BTN_HL);
    draw_btn(ui, L->tb_back, "<", 3, RED_BG);
    char nm[64];
    if (ui->trk_note) trk_label(c, ui->trk_note == 1, nm, sizeof nm);
    else snprintf(nm, sizeof nm, "%s", c->detail_ready ? c->detail.it.b.name : "");
    lv_fit(t, sizeof t, nm, L->tb_title.w, 2);
    fb_draw_text(fb, L->tb_title.x, L->tb_title.y + (L->tb_title.h - FONT_ROWS * 2) / 2, t, COLOR_WHITE, 2);
    snprintf(t, sizeof t, "VOL %d/%d", ui->vol_step, ui->vol_max);
    fb_draw_text(fb, L->tb_vol.x + L->tb_vol.w - text_measure_width(t, 2), L->tb_vol.y + (L->tb_vol.h - FONT_ROWS * 2) / 2, t, DIM, 2);
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) snprintf(t, sizeof t, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else snprintf(t, sizeof t, "--:--");
    fb_draw_text(fb, L->tb_clock.x + L->tb_clock.w - text_measure_width(t, 2), L->tb_clock.y + (L->tb_clock.h - FONT_ROWS * 2) / 2, t, DIM, 2);

    fb_fill_rect(fb, bb.x, bb.y, bb.w, bb.h, BG);
    fb_fill_rect(fb, bb.x, bb.y, bb.w, 1, BTN_HL);
    long pos = jv_core_position(c), dur = c->play.runtime_ms;
    if (pos < 0) pos = 0;
    mmss(t, sizeof t, pos / 1000);
    fb_draw_text(fb, L->ppos.x, L->ppos.y + (L->ppos.h - FONT_ROWS * 2) / 2, t, c->seek_pending ? RING : COLOR_WHITE, 2);
    mmss(t, sizeof t, dur / 1000);
    fb_draw_text(fb, L->pdur.x + L->pdur.w - text_measure_width(t, 2), L->pdur.y + (L->pdur.h - FONT_ROWS * 2) / 2, t, DIM, 2);
    UiRect tr = L->ptrack;
    fb_fill_rect(fb, tr.x, tr.y, tr.w, tr.h, PANEL);
    int fw = jv_ui_progress_w(L, pos, dur);
    if (fw > 0) fb_fill_rect(fb, tr.x, tr.y, fw, tr.h, c->seek_pending ? RING : BTN_HL);
    fb_draw_rect(fb, tr.x, tr.y, tr.w, tr.h, DIM);

    static const char *lbl[JV_UI_PBTN] = { "<<", "PAUSE", ">>", "STOP", "EQ", "VOL-", "VOL+", "AUDIO", "SUBS" };
    for (int i = 0; i < JV_UI_PBTN; i++) {
        const char *s = lbl[i];
        if (i == PB_PLAY && c->play_state == JV_PLAY_PAUSED) s = "PLAY";
        draw_btn(ui, L->pbtn[i], s, 2, i == PB_STOP ? RED_BG : BTN_BG);
    }
    if (ui->ov_ring && ui->ov_focus >= 0 && ui->ov_focus <= JV_UI_PBTN)
        ring(ui, ui->ov_focus == JV_UI_PBTN ? L->tb_back : L->pbtn[ui->ov_focus]);
}
/* The PLAYER screen: the bands and the banner, never a clear (the picture is under them; the loop clears to black
 * and repaints it after a full swap). */
static void draw_player(JvUi *ui)
{
    const JvCore *c = ui->core;
    const JvLayout *L = &ui->L;
    if (c->banner[0]) fb_draw_text(ui->fb, L->pbanner.x + 4, L->pbanner.y + 2, c->banner, RING, 2);
    if (c->busy) {
        const char *w = "BUFFERING...";
        fb_draw_text(ui->fb, L->pbanner.x + L->pbanner.w - 8 - text_measure_width(w, 2), L->pbanner.y + 2, w, DIM, 2);
    }
    draw_player_bands(ui);
    if (c->overlay && ui->dlg_up) modal_dialog_draw(&ui->dlg, ui->fb);
}

void jv_ui_draw(JvUi *ui)
{
    const JvCore *c = ui->core;
    Framebuffer *fb = ui->fb;
    const JvLayout *L = &ui->L;
    if (ui->eq_up) {
        fb_clear(fb, BG);
        eq_panel_draw(&ui->eq, fb);
        return;
    }
    if (c->screen == JV_SCR_PLAYER) { draw_player(ui); return; }
    fb_clear(fb, BG);
    const char *title = JF_APP_TITLE;
    if (c->screen != JV_SCR_HOME && c->screen != JV_SCR_SIGNIN && c->crumb[0]) title = c->crumb;
    if (c->screen == JV_SCR_CAST) title = "CAST";
    if (c->screen == JV_SCR_PLAYER) title = "PLAYER";
    ui->bar.title = title;
    top_bar_draw(fb, &ui->bar);
    if (ui->zf.shown && ui->zf.zone == Z_BAR && !c->overlay) top_bar_draw_focus(fb, &ui->bar.L, ui->zf.id == 0 ? TB_BACK : TB_EXIT);
    draw_banner(ui);
    ui->rowtxt_i = 0;
    switch (c->screen) {
    case JV_SCR_SIGNIN: {
        static const char *label[JV_UI_FIELDS] = { "SERVER", "USER", "PASSWORD" };
        for (int i = 0; i < JV_UI_FIELDS; i++) {
            char star[64], t[80];
            const char *v = ui->fields[i];
            if (i == 2) { size_t k = strlen(v); memset(star, '*', k); star[k] = '\0'; v = star; }
            snprintf(t, sizeof t, "%s: %s", label[i], v);
            draw_btn(ui, L->field[i], t, 2, BTN_BG);
        }
        if (signin_can(ui)) draw_btn(ui, L->signin, "SIGN IN", 3, BTN_BG);
        if (c->reuse_available) draw_btn(ui, L->reuse, "REUSE SAVED LOGIN", 2, BTN_BG);
        draw_btn(ui, L->signexit, "EXIT", 3, RED_BG);
        break;
    }
    case JV_SCR_HOME: {
        static const char *name[] = { "CONTINUE", "NEXT UP", "MOVIES", "SHOWS", "", "", "SEARCH" };
        int ids[JV_UI_TILES], n = home_tiles(c, ids);
        for (int i = 0; i < n; i++) draw_btn(ui, L->tile[i], name[ids[i]], 3, BTN_BG);
        break;
    }
    case JV_SCR_PLAYER: break;
    case JV_SCR_DETAILS: draw_details(ui); break;
    default:
        if (is_grid(c)) {
            if (!item_count(c)) fb_draw_text(fb, L->list.x + 8, L->list.y + 8, c->busy ? "" : "NO ITEMS", DIM, 2);
            draw_grid(ui);
        } else {
            lv_sync(&ui->lv);
            ui->lv.active = ui->zf.zone == Z_MAIN;
            ui->lv.ring_hidden = !ui->zf.shown;
            lv_draw(fb, &ui->lv, c->busy ? "" : "NO ITEMS");
            if (c->screen == JV_SCR_LIST && c->kind == JV_K_SEARCH) draw_btn(ui, L->sbtn, "NEW SEARCH", 2, BTN_BG);
        }
        break;
    }
    if (ui->zf.shown && ui->zf.zone != Z_BAR && !c->overlay) {
        UiZone z[UZ_ZONES];
        UiRect rr;
        int nz = build_zones(ui, z);
        if (!is_grid(c) && uz_ring(&ui->zf, z, nz, &rr)) ring(ui, rr);
        else if (is_grid(c) && ui->zf.zone != Z_MAIN && uz_ring(&ui->zf, z, nz, &rr)) ring(ui, rr);
    }
    if (c->overlay && ui->dlg_up) modal_dialog_draw(&ui->dlg, fb);
}

/* ── the player overlay's out-API ──────────────────────────────────────── */

bool jv_ui_overlay_shown(const JvUi *ui) { return ui->ov_shown; }
bool jv_ui_take_bands_dirty(JvUi *ui) { bool d = ui->bands_dirty; ui->bands_dirty = false; return d; }
void jv_ui_band_rects(const JvUi *ui, UiRect *top, UiRect *bot)
{
    if (top) *top = ui->L.top_band;
    if (bot) *bot = ui->L.bot_band;
}
unsigned jv_ui_take_actions(JvUi *ui) { unsigned a = ui->acts; ui->acts = 0; return a; }
void jv_ui_set_volume(JvUi *ui, int step, int max)
{
    if (step == ui->vol_step && max == ui->vol_max) return;
    ui->vol_step = step; ui->vol_max = max;
    if (ui->ov_shown) ui->bands_dirty = true;
}

/* The equaliser panel: opened by the loop on JV_UI_ACTB_EQ_OPEN with the gains in force, owns the screen and every input until
 * DONE or CANCEL.  The loop applies what jv_ui_eq_take() hands back and repaints the picture when jv_ui_eq_closed() says so. */
void jv_ui_eq_open(JvUi *ui, const int gains[EQ_BANDS], const AudioEnh *enh, uint32_t now)
{
    AudioEnh en;
    memset(&en, 0, sizeof en);
    if (enh) en = *enh;
    eq_panel_open_enh(&ui->eq, gains, &en, SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP, SCREEN_SAFE_RIGHT, SCREEN_SAFE_BOTTOM, now);
    ui->eq_up = true;
    ui->dirty = true;
    touch_guard_screen_changed(&ui->guard, now);
}
bool jv_ui_eq_take(JvUi *ui, int gains[EQ_BANDS], bool *save)
{
    if (!ui->eq_apply) return false;
    memcpy(gains, ui->eq_out, sizeof ui->eq_out);
    if (save) *save = ui->eq_save;
    ui->eq_apply = ui->eq_save = false;
    return true;
}
const AudioEnh *jv_ui_enh_out(const JvUi *ui) { return &ui->enh_out; }
bool jv_ui_eq_closed(JvUi *ui) { bool c = ui->eq_closed; ui->eq_closed = false; return c; }
