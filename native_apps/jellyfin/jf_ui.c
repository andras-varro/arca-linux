/*
 * jf_ui.c — the Jellyfin screens.  Draw and focus only; jf_core decides.
 * See jf_ui.h.
 */
#include "jf_ui.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "keyboard.h"

#define BG        RGB(16, 18, 26)
#define PANEL     RGB(30, 36, 52)
#define BTN_BG    RGB(40, 64, 104)
#define BTN_HL    RGB(70, 104, 160)
#define DIM       RGB(140, 150, 170)
#define RING      RGB(255, 220, 60)

#define BANNER_H  18
#define GAP        6
#define FIELD_H   52

/* ── Layout (pure) ─────────────────────────────────────────────────────── */

static UiRect rect_of(int x, int y, int w, int h) { UiRect r = { x, y, w, h }; return r; }

void jf_ui_layout(JfLayout *L, const JfExtents *e)
{
    memset(L, 0, sizeof *L);
    int sl = e->safe_l, sw = e->safe_r - e->safe_l;
    int bb = top_bar_bottom(e->vis_t, e->safe_t);
    int y0 = bb + 3;
    L->bar_bottom = bb;
    L->banner = rect_of(sl, y0, sw, BANNER_H);

    int top = y0 + BANNER_H + 2;
    int rowy = e->safe_b - JF_UI_BTN_H - 4;
    L->list = rect_of(sl, top, sw, rowy - 4 - top);
    L->list_full = rect_of(sl, top, sw, e->safe_b - 4 - top);

    int w3 = (sw - 2 * GAP) / 3;
    for (int i = 0; i < JF_UI_ROW3; i++)
        L->row3[i] = rect_of(sl + i * (w3 + GAP), rowy, w3, JF_UI_BTN_H);
    int w5 = (sw - 4 * GAP) / 5;
    for (int i = 0; i < JF_UI_ROW5; i++)
        L->row5[i] = rect_of(sl + i * (w5 + GAP), rowy, w5, JF_UI_BTN_H);

    for (int i = 0; i < JF_UI_FIELDS; i++)
        L->field[i] = rect_of(sl, top + 4 + i * (FIELD_H + GAP), sw, FIELD_H);
    int wb = (sw - GAP) / 2;
    int by = top + 4 + JF_UI_FIELDS * (FIELD_H + GAP) + 8;
    L->signin = rect_of(sl, by, wb, FIELD_H);
    L->signexit = rect_of(sl + wb + GAP, by, wb, FIELD_H);

    L->info = rect_of(sl, top + 6, sw, 120);
    L->progress = rect_of(sl + 20, top + 6 + 120 + 10, sw - 40, 14);
}

void jf_ui_extents_live(JfExtents *e)
{
    e->vis_l = SCREEN_VISIBLE_LEFT;  e->vis_t = SCREEN_VISIBLE_TOP;
    e->vis_r = SCREEN_VISIBLE_RIGHT; e->vis_b = SCREEN_VISIBLE_BOTTOM;
    e->safe_l = SCREEN_SAFE_LEFT;    e->safe_t = SCREEN_SAFE_TOP;
    e->safe_r = SCREEN_SAFE_RIGHT;   e->safe_b = SCREEN_SAFE_BOTTOM;
}

/* ── List source ───────────────────────────────────────────────────────── */

static bool is_list_screen(JfScreen s)
{
    return s == JF_SCR_ARTISTS || s == JF_SCR_ALBUMS || s == JF_SCR_TRACKS || s == JF_SCR_SEARCH;
}

static int src_count(void *ctx)
{
    const JfUi *ui = ctx;
    if (ui->app->screen == JF_SCR_QUEUE) return ui->app->queue_n;
    if (is_list_screen(ui->app->screen) && ui->app->list) return ui->app->list->n;
    return 0;
}

static void mmss(char *out, size_t cap, int s)
{
    if (s < 0) s = 0;
    snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

static void src_row(void *ctx, int index, LvRow *out)
{
    JfUi *ui = ctx;
    const JfApp *app = ui->app;
    const JfItem *it;
    if (app->screen == JF_SCR_QUEUE) {
        if (index < 0 || index >= app->queue_n) return;
        it = &app->queue[index];
    } else {
        if (!app->list || index < 0 || index >= app->list->n) return;
        it = &app->list->items[index];
    }
    char *d = ui->rowtxt[ui->rowtxt_i];
    ui->rowtxt_i = (ui->rowtxt_i + 1) % 32;
    d[0] = '\0';
    if (it->type == JF_ITEM_TRACK) mmss(d, sizeof ui->rowtxt[0], it->runtime_s);
    out->title = it->name;
    out->detail = d;
    out->glyph = it->type == JF_ITEM_TRACK ? LV_GLYPH_FILE : LV_GLYPH_FOLDER;
    if (app->screen == JF_SCR_QUEUE && index == app->queue_cur) out->glyph = LV_GLYPH_UP;
}

/* ── Setup ─────────────────────────────────────────────────────────────── */

static void style(Button *b, UiRect r, const char *text, int scale)
{
    button_init_full(b, r.x, r.y, r.w, r.h, text, BTN_BG, COLOR_WHITE, BTN_HL, scale);
    b->max_text_width = r.w - 12;
}

void jf_ui_init(JfUi *ui, Framebuffer *fb, TouchInput *touch, GamepadManager *gm, const JfApp *app)
{
    memset(ui, 0, sizeof *ui);
    ui->fb = fb; ui->touch = touch; ui->gm = gm; ui->app = app;
    JfExtents e;
    jf_ui_extents_live(&e);
    jf_ui_layout(&ui->L, &e);

    top_bar_init(&ui->bar, true, true);
    ui->bar.title = "JELLYFIN";
    ui->bar.title_color = COLOR_WHITE;
    ui->bar.title_cut = true;
    top_bar_layout(&ui->bar);

    LvSource src = { src_count, src_row, ui };
    lv_init(&ui->lv, src, ui->L.list, 40);

    static const char *t3[JF_UI_ROW3] = { "SEARCH", "NOW PLAYING", "PLAY ALL" };
    static const char *t5[JF_UI_ROW5] = { "NEXT", "VOL -", "VOL +", "QUEUE", "BACK" };
    for (int i = 0; i < JF_UI_ROW3; i++) style(&ui->row3[i], ui->L.row3[i], t3[i], 2);
    for (int i = 0; i < JF_UI_ROW5; i++) style(&ui->row5[i], ui->L.row5[i], t5[i], 2);
    for (int i = 0; i < JF_UI_FIELDS; i++) style(&ui->field[i], ui->L.field[i], "", 2);
    style(&ui->signin, ui->L.signin, "SIGN IN", 3);
    style(&ui->signexit, ui->L.signexit, "EXIT", 3);

    ui->last_screen = (JfScreen)-1;
    ui->show_ring = true;
    ui->dirty = true;
}

void jf_ui_free(JfUi *ui)
{
    lv_free(&ui->lv);
    jf_wipe(ui->fields, sizeof ui->fields);
}

void jf_ui_set_played(JfUi *ui, int played_ms)
{
    if (played_ms / 1000 != ui->played_ms / 1000 && ui->app->screen == JF_SCR_NOWPLAYING)
        ui->dirty = true;
    ui->played_ms = played_ms;
}

void jf_ui_mark_dirty(JfUi *ui) { ui->dirty = true; }

bool jf_ui_take_dirty(JfUi *ui)
{
    bool d = ui->dirty || button_take_dirty();
    ui->dirty = false;
    return d;
}

/* ── Input ─────────────────────────────────────────────────────────────── */

static int emit(JfEvent *out, int *n, int max, JfEventType type)
{
    if (*n >= max) return -1;
    memset(&out[*n], 0, sizeof out[*n]);
    out[*n].type = type;
    return (*n)++;
}

/* Keys and the pointer were read by the blocking keyboard; whatever it left must
 * not act on the screen underneath. */
static void drain_after_keyboard(JfUi *ui, Pointer *ptr)
{
    InputState scratch;
    memset(&scratch, 0, sizeof scratch);
    for (int i = 0; i < 3; i++) {
        gamepad_poll(ui->gm, &scratch, 0, 0, false);
        usleep(20000);
    }
    touch_poll(ui->touch);
    if (ptr) { pointer_drain(ptr); pointer_invalidate(ptr); }
    ui->dirty = true;
}

static void edit_field(JfUi *ui, Pointer *ptr, int i)
{
    static const char *title[JF_UI_FIELDS] = { "SERVER HOST OR HOST:PORT", "USER", "PASSWORD" };
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s", ui->fields[i]);
    int rc = i == 2
        ? keyboard_enter_masked(ui->fb, ui->touch, ui->gm, title[i], tmp, 63, KB_LAYOUT_FULL)
        : keyboard_enter(ui->fb, ui->touch, ui->gm, title[i], tmp, 63, KB_LAYOUT_FULL);
    if (rc == KB_RESULT_OK) snprintf(ui->fields[i], sizeof ui->fields[i], "%s", tmp);
    jf_wipe(tmp, sizeof tmp);
    drain_after_keyboard(ui, ptr);
}

static void do_search(JfUi *ui, Pointer *ptr, JfEvent *out, int *n, int max)
{
    char term[96] = "";
    int rc = keyboard_enter(ui->fb, ui->touch, ui->gm, "SEARCH", term, 40, KB_LAYOUT_ALPHANUM);
    drain_after_keyboard(ui, ptr);
    if (rc != KB_RESULT_OK) return;
    int k = emit(out, n, max, JF_EV_SEARCH);
    if (k >= 0) snprintf(out[k].term, sizeof out[k].term, "%s", term);
}

static bool bp(const InputState *in, int id) { return in && in->buttons[id].pressed; }

static void begin_screen(JfUi *ui)
{
    const JfApp *app = ui->app;
    ui->last_screen = app->screen;
    ui->zone = JF_ZONE_LIST;
    ui->focus = 0;
    ui->dirty = true;
    ui->bar.has_back = app->screen != JF_SCR_ARTISTS && app->screen != JF_SCR_SIGNIN;
    ui->bar.has_exit = true;
    top_bar_layout(&ui->bar);
    if (app->screen == JF_SCR_SIGNIN) {
        if (!ui->fields[0][0]) snprintf(ui->fields[0], sizeof ui->fields[0], "%s", app->login.server);
        if (!ui->fields[1][0]) snprintf(ui->fields[1], sizeof ui->fields[1], "%s", app->login.user);
    }
    if (app->screen == JF_SCR_QUEUE || is_list_screen(app->screen)) {
        ui->lv.rect = app->screen == JF_SCR_QUEUE ? ui->L.list_full : ui->L.list;
        lv_reset(&ui->lv);
        if (app->screen == JF_SCR_QUEUE && app->queue_n > 0) lv_move_to(&ui->lv, app->queue_cur);
    }
    if (app->screen == JF_SCR_NOWPLAYING) { ui->zone = JF_ZONE_ROW; ui->focus = 0; }
}

/* Which row3 slots exist on this screen. */
static bool row3_on(const JfApp *app, int i)
{
    if (i == 0) return true;
    if (i == 1) return app->queue_n > 0;
    return app->screen == JF_SCR_TRACKS || app->screen == JF_SCR_SEARCH;
}

static int row3_step(const JfApp *app, int cur, int dir)
{
    for (int k = 1; k <= JF_UI_ROW3; k++) {
        int j = (cur + dir * k + 3 * JF_UI_ROW3) % JF_UI_ROW3;
        if (row3_on(app, j)) return j;
    }
    return cur;
}

static void row3_act(JfUi *ui, Pointer *ptr, int i, JfEvent *out, int *n, int max)
{
    if (i == 0) do_search(ui, ptr, out, n, max);
    else if (i == 1) emit(out, n, max, JF_EV_OPEN_NOWPLAYING);
    else emit(out, n, max, JF_EV_PLAY_ALL);
}

static void row5_act(JfUi *ui, int i, JfEvent *out, int *n, int max)
{
    (void)ui;
    static const JfEventType t[JF_UI_ROW5] = { JF_EV_NEXT, JF_EV_VOL_DOWN, JF_EV_VOL_UP,
                                               JF_EV_OPEN_QUEUE, JF_EV_BACK };
    emit(out, n, max, t[i]);
}

static void overlay_input(JfUi *ui, const TouchState *ts, const InputState *in, uint32_t now,
                          JfEvent *out, int *n, int max)
{
    const JfApp *app = ui->app;
    if (!ui->dlg_up || ui->dlg_buttons != app->overlay_buttons) {
        int cnt = 0;
        if (app->overlay_buttons & JF_BTN_RETRY) cnt++;
        cnt++;   /* CLOSE is always there */
        modal_dialog_init(&ui->dlg, "JELLYFIN", app->overlay_text, cnt);
        int k = 0;
        if (app->overlay_buttons & JF_BTN_RETRY)
            modal_dialog_set_button(&ui->dlg, k++, "RETRY", BTN_BG, COLOR_WHITE);
        modal_dialog_set_button(&ui->dlg, k, "CLOSE", RGB(90, 40, 40), COLOR_WHITE);
        modal_dialog_set_focus(&ui->dlg, 0);
        modal_dialog_show(&ui->dlg);
        ui->dlg_up = true;
        ui->dlg_buttons = app->overlay_buttons;
        ui->dirty = true;
    }
    int cancel = ui->dlg.button_count - 1;
    int a = modal_dialog_input(&ui->dlg, in, cancel);
    if (a < 0) a = modal_dialog_update(&ui->dlg, ts->x, ts->y, ts->pressed || ts->held, now);
    if (ts->pressed || ts->released || in->buttons[BTN_ID_UP].pressed || in->buttons[BTN_ID_DOWN].pressed ||
        in->buttons[BTN_ID_LEFT].pressed || in->buttons[BTN_ID_RIGHT].pressed) ui->dirty = true;
    if (a >= 0) {
        bool retry = (app->overlay_buttons & JF_BTN_RETRY) && a == 0;
        emit(out, n, max, retry ? JF_EV_RETRY : JF_EV_CLOSE_ERROR);
        ui->dlg_up = false;
        ui->dirty = true;
    }
}

int jf_ui_input(JfUi *ui, const TouchState *ts, const InputState *in, Pointer *ptr,
                uint32_t now, JfEvent *out, int max)
{
    const JfApp *app = ui->app;
    int n = 0;

    if (app->screen != ui->last_screen) begin_screen(ui);
    if (app->banner_seq != ui->seen_banner) { ui->seen_banner = app->banner_seq; ui->dirty = true; }

    if (app->overlay) {
        overlay_input(ui, ts, in, now, out, &n, max);
        return n;
    }
    ui->dlg_up = false;

    bool pad_nav = bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN) || bp(in, BTN_ID_LEFT) ||
                   bp(in, BTN_ID_RIGHT) || bp(in, BTN_ID_JUMP) || bp(in, BTN_ID_ACTION);
    if (ts->pressed) ui->show_ring = false;
    if (pad_nav) ui->show_ring = true;
    if (ts->pressed || ts->released || pad_nav) ui->dirty = true;

    bool go_back = bp(in, BTN_ID_PAUSE) || bp(in, BTN_ID_BACK);
    if (ts->pressed) {
        TbHit h = top_bar_hit(&ui->bar.L, ts->x, ts->y);
        ui->back_press = h == TB_BACK;
        ui->exit_press = h == TB_EXIT;
    }
    if (ts->released) {
        TbHit h = top_bar_hit(&ui->bar.L, ts->x, ts->y);
        if (ui->exit_press && h == TB_EXIT) emit(out, &n, max, JF_EV_QUIT);
        else if (ui->back_press && h == TB_BACK) emit(out, &n, max, JF_EV_BACK);
        ui->back_press = ui->exit_press = false;
        if (n) return n;
    }

    JfScreen sc = app->screen;
    bool act = bp(in, BTN_ID_ACTION) || bp(in, BTN_ID_JUMP);

    if (sc == JF_SCR_SIGNIN) {
        UiRect rs[5] = { ui->L.field[0], ui->L.field[1], ui->L.field[2], ui->L.signin, ui->L.signexit };
        UiDir dir = UI_DIR_DOWN;
        bool mv = false;
        if (bp(in, BTN_ID_UP))    { dir = UI_DIR_UP;    mv = true; }
        if (bp(in, BTN_ID_DOWN))  { dir = UI_DIR_DOWN;  mv = true; }
        if (bp(in, BTN_ID_LEFT))  { dir = UI_DIR_LEFT;  mv = true; }
        if (bp(in, BTN_ID_RIGHT)) { dir = UI_DIR_RIGHT; mv = true; }
        if (mv) ui->focus = ui_focus_move(rs, 5, ui->focus, dir);
        ui->signin.disabled = !ui->fields[0][0] || !ui->fields[1][0];
        int pick = -1;
        for (int i = 0; i < JF_UI_FIELDS; i++) if (button_check_tap(&ui->field[i], ts, now)) pick = i;
        if (button_check_tap(&ui->signin, ts, now)) pick = 3;
        if (button_check_tap(&ui->signexit, ts, now)) pick = 4;
        if (act && pick < 0) pick = ui->focus;
        if (go_back) emit(out, &n, max, JF_EV_QUIT);
        if (pick >= 0 && pick < JF_UI_FIELDS) edit_field(ui, ptr, pick);
        else if (pick == 3 && !ui->signin.disabled) {
            int k = emit(out, &n, max, JF_EV_SIGNIN);
            if (k >= 0) {
                snprintf(out[k].server, sizeof out[k].server, "%s", ui->fields[0]);
                snprintf(out[k].user, sizeof out[k].user, "%s", ui->fields[1]);
                snprintf(out[k].password, sizeof out[k].password, "%s", ui->fields[2]);
                jf_wipe(ui->fields[2], sizeof ui->fields[2]);
            }
        } else if (pick == 4) emit(out, &n, max, JF_EV_QUIT);
        return n;
    }

    if (sc == JF_SCR_NOWPLAYING) {
        if (go_back) emit(out, &n, max, JF_EV_BACK);
        if (bp(in, BTN_ID_LEFT))  emit(out, &n, max, JF_EV_VOL_DOWN);
        if (bp(in, BTN_ID_RIGHT)) emit(out, &n, max, JF_EV_VOL_UP);
        if (bp(in, BTN_ID_UP))   ui->focus = (ui->focus + JF_UI_ROW5 - 1) % JF_UI_ROW5;
        if (bp(in, BTN_ID_DOWN)) ui->focus = (ui->focus + 1) % JF_UI_ROW5;
        if (bp(in, BTN_ID_JUMP))   emit(out, &n, max, JF_EV_NEXT);
        else if (bp(in, BTN_ID_ACTION)) row5_act(ui, ui->focus, out, &n, max);
        for (int i = 0; i < JF_UI_ROW5; i++)
            if (button_check_tap(&ui->row5[i], ts, now)) row5_act(ui, i, out, &n, max);
        return n;
    }

    /* list screens and the queue */
    bool queue = sc == JF_SCR_QUEUE;
    bool has_row = !queue;
    int count = src_count(ui);
    if (go_back) emit(out, &n, max, JF_EV_BACK);

    const InputState *lin = NULL;
    if (ui->zone == JF_ZONE_LIST) {
        lin = in;
        if (bp(in, BTN_ID_DOWN) && (count == 0 || ui->lv.cursor >= count - 1) && has_row) {
            ui->zone = JF_ZONE_ROW; ui->focus = 0; lin = NULL; ui->dirty = true;
        } else if (bp(in, BTN_ID_UP) && ui->lv.cursor <= 0) {
            ui->zone = JF_ZONE_BAR; ui->bar_focus = ui->bar.has_back ? 0 : 1; lin = NULL; ui->dirty = true;
        }
    }
    ui->lv.active = ui->zone == JF_ZONE_LIST;
    lv_sync(&ui->lv);
    LvEvent ev = lv_input(&ui->lv, ts, lin, now);
    if (ev != LV_EV_NONE) ui->dirty = true;
    bool pick = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE;
    if (ui->zone == JF_ZONE_LIST && lin && bp(lin, BTN_ID_JUMP)) pick = true;
    if (pick && ui->lv.cursor >= 0 && ui->lv.cursor < count) {
        int k = emit(out, &n, max, queue ? JF_EV_QUEUE_JUMP : JF_EV_ACTIVATE);
        if (k >= 0) out[k].i = ui->lv.cursor;
    }

    if (!queue && app->list && ui->zone == JF_ZONE_LIST && count > 0 && ui->lv.cursor >= count - 6 &&
        count < app->list->total && !app->list->full && !app->list->pending && !app->busy)
        emit(out, &n, max, JF_EV_NEED_MORE);

    if (ui->zone == JF_ZONE_ROW && has_row) {
        if (!row3_on(app, ui->focus)) ui->focus = 0;
        if (bp(in, BTN_ID_LEFT))  ui->focus = row3_step(app, ui->focus, -1);
        if (bp(in, BTN_ID_RIGHT)) ui->focus = row3_step(app, ui->focus, 1);
        if (bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN)) ui->zone = JF_ZONE_LIST;
        else if (act) row3_act(ui, ptr, ui->focus, out, &n, max);
    } else if (ui->zone == JF_ZONE_BAR) {
        if ((bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT)) && ui->bar.has_back) ui->bar_focus ^= 1;
        if (bp(in, BTN_ID_DOWN)) ui->zone = JF_ZONE_LIST;
        else if (act) emit(out, &n, max, ui->bar_focus == 0 ? JF_EV_BACK : JF_EV_QUIT);
    }
    if (has_row) {
        for (int i = 0; i < JF_UI_ROW3; i++)
            if (row3_on(app, i) && button_check_tap(&ui->row3[i], ts, now)) row3_act(ui, ptr, i, out, &n, max);
    }
    return n;
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

static void ring(JfUi *ui, UiRect r)
{
    fb_draw_rect(ui->fb, r.x - 2, r.y - 2, r.w + 4, r.h + 4, RING);
    fb_draw_rect(ui->fb, r.x - 3, r.y - 3, r.w + 6, r.h + 6, RING);
}

static void draw_banner(JfUi *ui)
{
    const JfApp *app = ui->app;
    UiRect b = ui->L.banner;
    if (app->banner[0]) fb_draw_text(ui->fb, b.x + 4, b.y + 2, app->banner, RING, 2);
    if (app->busy) fb_draw_text(ui->fb, b.x + b.w - 8 - text_measure_width("LOADING", 2), b.y + 2, "LOADING", DIM, 2);
}

static void draw_signin(JfUi *ui)
{
    static const char *label[JF_UI_FIELDS] = { "SERVER", "USER", "PASSWORD" };
    for (int i = 0; i < JF_UI_FIELDS; i++) {
        char star[64];
        const char *v = ui->fields[i];
        if (i == 2) {
            size_t k = strlen(v);
            memset(star, '*', k);
            star[k] = '\0';
            v = star;
        }
        snprintf(ui->field[i].text, sizeof ui->field[i].text, "%s: %s", label[i], v);
        button_draw(ui->fb, &ui->field[i]);
    }
    ui->signin.disabled = !ui->fields[0][0] || !ui->fields[1][0];
    button_draw(ui->fb, &ui->signin);
    button_draw(ui->fb, &ui->signexit);
    if (ui->show_ring) {
        UiRect rs[5] = { ui->L.field[0], ui->L.field[1], ui->L.field[2], ui->L.signin, ui->L.signexit };
        if (ui->focus >= 0 && ui->focus < 5) ring(ui, rs[ui->focus]);
    }
}

static void draw_row3(JfUi *ui)
{
    for (int i = 0; i < JF_UI_ROW3; i++) {
        if (!row3_on(ui->app, i)) continue;
        button_draw(ui->fb, &ui->row3[i]);
        if (ui->show_ring && ui->zone == JF_ZONE_ROW && ui->focus == i) ring(ui, ui->L.row3[i]);
    }
}

static void draw_nowplaying(JfUi *ui)
{
    const JfApp *app = ui->app;
    Framebuffer *fb = ui->fb;
    const JfItem *it = jf_now(app);
    UiRect in = ui->L.info;
    char buf[128];
    if (it) {
        lv_fit(buf, sizeof buf, it->name, in.w - 8, 3);
        fb_draw_text(fb, in.x + 4, in.y, buf, COLOR_WHITE, 3);
        lv_fit(buf, sizeof buf, it->artist, in.w - 8, 2);
        fb_draw_text(fb, in.x + 4, in.y + 40, buf, DIM, 2);
        lv_fit(buf, sizeof buf, it->album, in.w - 8, 2);
        fb_draw_text(fb, in.x + 4, in.y + 66, buf, DIM, 2);
    }
    const char *state = app->play_state == JF_PLAY_FETCHING ? "LOADING..."
                      : app->play_state == JF_PLAY_END ? "END OF QUEUE" : "";
    if (state[0]) fb_draw_text(fb, in.x + 4, in.y + 94, state, RING, 2);

    UiRect p = ui->L.progress;
    int total = it ? it->runtime_s : 0;
    int sec = ui->played_ms / 1000;
    if (app->play_state != JF_PLAY_PLAYING) sec = 0;
    fb_fill_rect(fb, p.x, p.y, p.w, p.h, PANEL);
    if (total > 0) {
        int fill = sec >= total ? p.w : (int)((long)p.w * sec / total);
        fb_fill_rect(fb, p.x, p.y, fill, p.h, BTN_HL);
    }
    fb_draw_rect(fb, p.x, p.y, p.w, p.h, DIM);
    char a[16], z[16];
    mmss(a, sizeof a, sec);
    mmss(z, sizeof z, total);
    snprintf(buf, sizeof buf, "%s / %s", a, z);
    fb_draw_text(fb, p.x, p.y + p.h + 8, buf, COLOR_WHITE, 2);
    if (app->volume_step == 0) snprintf(buf, sizeof buf, "MUTE");
    else snprintf(buf, sizeof buf, "VOLUME %d/%d", app->volume_step, JF_VOLUME_MAX);
    fb_draw_text(fb, p.x + p.w - text_measure_width(buf, 2), p.y + p.h + 8, buf, COLOR_WHITE, 2);

    for (int i = 0; i < JF_UI_ROW5; i++) {
        button_draw(fb, &ui->row5[i]);
        if (ui->show_ring && ui->focus == i) ring(ui, ui->L.row5[i]);
    }
}

void jf_ui_draw(JfUi *ui)
{
    const JfApp *app = ui->app;
    Framebuffer *fb = ui->fb;
    fb_clear(fb, BG);

    const char *title = "JELLYFIN";
    switch (app->screen) {
    case JF_SCR_ARTISTS: case JF_SCR_ALBUMS: case JF_SCR_TRACKS: case JF_SCR_SEARCH:
        if (app->crumb[0]) title = app->crumb;
        break;
    case JF_SCR_NOWPLAYING: title = "NOW PLAYING"; break;
    case JF_SCR_QUEUE: title = "QUEUE"; break;
    default: break;
    }
    ui->bar.title = title;
    top_bar_draw(fb, &ui->bar);
    if (ui->show_ring && ui->zone == JF_ZONE_BAR && !app->overlay)
        top_bar_draw_focus(fb, &ui->bar.L, ui->bar_focus == 0 ? TB_BACK : TB_EXIT);

    draw_banner(ui);
    switch (app->screen) {
    case JF_SCR_SIGNIN:
        draw_signin(ui);
        break;
    case JF_SCR_NOWPLAYING:
        draw_nowplaying(ui);
        break;
    case JF_SCR_QUEUE:
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, "QUEUE IS EMPTY");
        break;
    default:
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, app->busy ? "" : "NOTHING HERE");
        draw_row3(ui);
        break;
    }
    if (app->overlay && ui->dlg_up) modal_dialog_draw(&ui->dlg, fb);
}
