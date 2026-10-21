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
    int ctop = top + JF_UI_CAT_H + 4;
    L->list_root = rect_of(sl, ctop, sw, rowy - 4 - ctop);
    L->cat_prev = rect_of(sl, top, JF_UI_CAT_W, JF_UI_CAT_H);
    L->cat_next = rect_of(sl + sw - JF_UI_CAT_W, top, JF_UI_CAT_W, JF_UI_CAT_H);
    L->cat_label = rect_of(sl + JF_UI_CAT_W + GAP, top, sw - 2 * (JF_UI_CAT_W + GAP), JF_UI_CAT_H);

    int w3 = (sw - 2 * GAP) / 3;
    for (int i = 0; i < JF_UI_ROW3; i++)
        L->row3[i] = rect_of(sl + i * (w3 + GAP), rowy, w3, JF_UI_BTN_H);
    int w5 = (sw - 4 * GAP) / 5;
    for (int i = 0; i < JF_UI_ABAR; i++)
        L->abar[i] = rect_of(sl + i * (w5 + GAP), rowy, w5, JF_UI_BTN_H);
    L->save = rect_of(sl, rowy, sw, JF_UI_BTN_H);
    for (int i = 0; i < JF_UI_MIXBTN; i++)
        L->mixb[i] = rect_of(sl + i * ((sw - GAP) / 2 + GAP), rowy, (sw - GAP) / 2, JF_UI_BTN_H);
    int w4 = (sw - 3 * GAP) / 4;
    for (int i = 0; i < JF_UI_NPMIX; i++)
        L->np_mix[i] = rect_of(sl + i * (w4 + GAP), rowy - GAP - JF_UI_BTN_H, w4, JF_UI_BTN_H);
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
    L->seek = rect_of(L->progress.x, L->progress.y - 10, L->progress.w, 34);

    /* mini player: a strip right above the button row; the lists shrink by its height + a gap */
    int mini_y = rowy - 4 - JF_UI_MINI_H;
    L->list_mini = L->list;      L->list_mini.h = mini_y - 4 - L->list.y;
    L->list_root_mini = L->list_root; L->list_root_mini.h = mini_y - 4 - L->list_root.y;
    L->mini = rect_of(sl, mini_y, sw, JF_UI_MINI_H);
    int wmb = 88;
    for (int i = 0; i < JF_UI_MINI; i++)
        L->mini_btn[i] = rect_of(sl + sw - (JF_UI_MINI - i) * wmb - (JF_UI_MINI - 1 - i) * GAP, mini_y, wmb, JF_UI_MINI_H);
    L->mini_title = rect_of(sl, mini_y, L->mini_btn[0].x - GAP - sl, JF_UI_MINI_H);
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
    if (ui->app->screen == JF_SCR_MIX) return ui->app->mix.opt_n;
    if (is_list_screen(ui->app->screen) && ui->app->list) return ui->app->list->n;
    return 0;
}

static void mmss(char *out, size_t cap, int s)
{
    if (s < 0) s = 0;
    snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

/* The MIX ticks live in app state (JfApp.mix.opt[i].on); the ListView draws its own mark bar from a bitmap.  Each
 * frame, copy the one into the other.  lv_key(LV_KEY_MARK) only toggles the cursor row, so move the cursor to each
 * row that disagrees, toggle it, and put the cursor back.  Rows past opt_n end up unmarked. */
static void mix_sync_marks(JfUi *ui)
{
    ListView *lv = &ui->lv;
    const JfApp *app = ui->app;
    int saved = lv->cursor;
    for (int i = 0; i < lv->count; i++) {
        bool want = i < app->mix.opt_n && i < JF_MIX_OPT_MAX && app->mix.opt[i].on;
        if (lv_is_marked(lv, i) == want) continue;
        lv->cursor = i;
        lv_key(lv, LV_KEY_MARK);
    }
    lv->cursor = saved;
}

static void src_row(void *ctx, int index, LvRow *out)
{
    JfUi *ui = ctx;
    const JfApp *app = ui->app;
    const JfItem *it;
    if (app->screen == JF_SCR_MIX) {
        if (index < 0 || index >= app->mix.opt_n || index >= JF_MIX_OPT_MAX) return;
        /* The tick is not drawn here: mix_sync_marks() mirrors it into the ListView mark, which draws the bar. */
        out->title = app->mix.opt[index].label;
        out->glyph = LV_GLYPH_BLANK;
        return;
    }
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
    if (it->type == JF_ITEM_TRACK) {
        /* the plain album's track list repeats its album artist on every row; every other list (a compilation shows
         * per-track artists through the helper) names the artist so a mixed list is readable */
        bool album_list = app->screen == JF_SCR_TRACKS && app->ctx[JF_LIST_TRACKS].sub == JF_SUB_PLAIN;
        jf_row_detail(it, jf_row_shows_artist(album_list, it), d, sizeof ui->rowtxt[0]);
    }
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
    static const char *t5[JF_UI_ROW5] = { "PAUSE", "STOP", "NEXT", "QUEUE", "BACK" };
    for (int i = 0; i < JF_UI_ROW3; i++) style(&ui->row3[i], ui->L.row3[i], t3[i], 2);
    for (int i = 0; i < JF_UI_ROW5; i++) style(&ui->row5[i], ui->L.row5[i], t5[i], 2);
    static const char *ta[JF_UI_ABAR] = { "PLAY", "PLAY NEXT", "ADD QUEUE", "MIX", "INSTANT MIX" };
    static const char *tm[JF_UI_NPMIX] = { "VOL -", "VOL +", "MIX", "INSTANT MIX" };
    static const char *tmini[JF_UI_MINI] = { "PAUSE", "VOL -", "VOL +" };
    for (int i = 0; i < JF_UI_MINI; i++) style(&ui->mini_b[i], ui->L.mini_btn[i], tmini[i], 2);
    for (int i = 0; i < JF_UI_ABAR; i++) style(&ui->abar[i], ui->L.abar[i], ta[i], 2);
    for (int i = 0; i < JF_UI_NPMIX; i++) style(&ui->np_mix[i], ui->L.np_mix[i], tm[i], 2);
    style(&ui->save, ui->L.save, "SAVE AS PLAYLIST", 2);
    style(&ui->mixb[0], ui->L.mixb[0], "START", 2);
    style(&ui->mixb[1], ui->L.mixb[1], "CANCEL", 2);
    for (int i = 0; i < JF_UI_FIELDS; i++) style(&ui->field[i], ui->L.field[i], "", 2);
    style(&ui->signin, ui->L.signin, "SIGN IN", 3);
    style(&ui->signexit, ui->L.signexit, "EXIT", 3);
    style(&ui->cat_btn[0], ui->L.cat_prev, "<", 3);
    style(&ui->cat_btn[1], ui->L.cat_next, ">", 3);

    ui->last_screen = (JfScreen)-1;
    touch_guard_init(&ui->guard);
    ui->last_cat = app->cat;
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
    int rc = keyboard_enter(ui->fb, ui->touch, ui->gm, "SEARCH", term, 40, KB_LAYOUT_FULL);
    drain_after_keyboard(ui, ptr);
    if (rc != KB_RESULT_OK) return;
    int k = emit(out, n, max, JF_EV_SEARCH);
    if (k >= 0) snprintf(out[k].term, sizeof out[k].term, "%s", term);
}

/* The queue's name is the core's to offer; the keyboard edits it, and an empty or cancelled entry sends nothing. */
static void do_save(JfUi *ui, Pointer *ptr, JfEvent *out, int *n, int max)
{
    char name[96];
    snprintf(name, sizeof name, "%s", ui->app->queue_name);
    int rc = keyboard_enter(ui->fb, ui->touch, ui->gm, "PLAYLIST NAME", name, 80, KB_LAYOUT_FULL);
    drain_after_keyboard(ui, ptr);
    if (rc != KB_RESULT_OK || !name[0]) return;
    int k = emit(out, n, max, JF_EV_SAVE_PLAYLIST);
    if (k >= 0) snprintf(out[k].term, sizeof out[k].term, "%s", name);
}

static bool bp(const InputState *in, int id) { return in && in->buttons[id].pressed; }

/* The mini player rides on the browse screens while a queue exists. */
static bool mini_wanted(const JfApp *app)
{
    return app->queue_n > 0 && is_list_screen(app->screen);
}

static const char *mini_play_label(const JfApp *app)
{
    return app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING ? "PAUSE" : "PLAY";
}

static UiRect list_rect_for(const JfUi *ui, JfScreen s)
{
    bool m = ui->mini_on;
    if (s == JF_SCR_ARTISTS) return m ? ui->L.list_root_mini : ui->L.list_root;
    return m ? ui->L.list_mini : ui->L.list;
}

static void begin_screen(JfUi *ui)
{
    const JfApp *app = ui->app;
    ui->last_screen = app->screen;
    ui->abar_up = false;
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
    if (app->screen == JF_SCR_QUEUE || app->screen == JF_SCR_MIX || is_list_screen(app->screen)) {
        ui->mini_on = mini_wanted(app);
        ui->lv.rect = list_rect_for(ui, app->screen);
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
    static const JfEventType t[JF_UI_ROW5] = { JF_EV_PAUSE, JF_EV_STOP, JF_EV_NEXT,
                                               JF_EV_OPEN_QUEUE, JF_EV_BACK };
    emit(out, n, max, t[i]);
}

static void abar_act(int i, int row, JfEvent *out, int *n, int max);

/* The now-playing upper row: VOL -, VOL +, MIX, INSTANT MIX (of the playing track). */
static void np_act(int i, JfEvent *out, int *n, int max)
{
    if (i == 0) emit(out, n, max, JF_EV_VOL_DOWN);
    else if (i == 1) emit(out, n, max, JF_EV_VOL_UP);
    else abar_act(i + 1, -1, out, n, max);   /* MIX = abar 3, INSTANT MIX = abar 4 */
}

/* The five buttons of the action bar, on list row `row` (-1 = the playing track, NOW PLAYING's MIX pair). */
static void abar_act(int i, int row, JfEvent *out, int *n, int max)
{
    static const JfEventType t[JF_UI_ABAR] = { JF_EV_ACTIVATE, JF_EV_PLAY_NEXT, JF_EV_ADD_QUEUE, JF_EV_MIX, JF_EV_INSTANT_MIX };
    int k = emit(out, n, max, t[i]);
    if (k >= 0) out[k].i = row;
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

bool jf_ui_view(const JfUi *ui, int *top, int *cursor)
{
    JfScreen s = ui->app->screen;
    if (s != ui->last_screen) return false;
    if (s != JF_SCR_QUEUE && s != JF_SCR_MIX && !is_list_screen(s)) return false;
    *top = ui->lv.top;
    *cursor = ui->lv.cursor;
    return true;
}

/* After a BACK the list is put back where it was when the entry was opened.  A re-aimed list is empty at first, so this waits
 * until the rows are there (or no more are coming); lv_sync() clamps what a shorter list cannot hold. */
static void restore_view(JfUi *ui)
{
    const JfApp *app = ui->app;
    if (app->restore_seq != ui->seen_restore) {
        ui->seen_restore = app->restore_seq;
        ui->restore_pend = true;
    }
    if (!ui->restore_pend) return;
    bool lv_screen = app->screen == JF_SCR_QUEUE || app->screen == JF_SCR_MIX || is_list_screen(app->screen);
    if (!lv_screen) { ui->restore_pend = false; return; }
    lv_sync(&ui->lv);
    bool more_coming = app->list && (app->list->pending || app->busy);
    if (ui->lv.count <= app->restore_cursor && more_coming) return;
    ui->restore_pend = false;
    ui->lv.top = app->restore_top;
    ui->lv.cursor = app->restore_cursor;
    ui->lv.cursor_dragged = false;
    lv_sync(&ui->lv);
    ui->dirty = true;
}

int jf_ui_input(JfUi *ui, const TouchState *ts, const InputState *in, Pointer *ptr,
                uint32_t now, JfEvent *out, int max)
{
    const JfApp *app = ui->app;
    int n = 0;

    if (app->screen != ui->last_screen) {
        begin_screen(ui);
        touch_guard_screen_changed(&ui->guard, now);
    }
    /* Whatever finger was down when the screen changed (a release, a bounce, a dropped contact that resumes) must not press
     * a widget of the new screen: until the panel has been quiet for a moment the widgets see no touch at all. */
    TouchState gated;
    if (!touch_guard_pass(&ui->guard, ts && (ts->pressed || ts->held), now)) {
        memset(&gated, 0, sizeof gated);
        ts = &gated;
    }
    restore_view(ui);
    /* a queue can start or empty on an open screen: the list makes room for the strip, or takes it back */
    if (is_list_screen(app->screen) && mini_wanted(app) != ui->mini_on) {
        ui->mini_on = mini_wanted(app);
        ui->lv.rect = list_rect_for(ui, app->screen);
        lv_sync(&ui->lv);
        ui->dirty = true;
    }
    if (app->banner_seq != ui->seen_banner) { ui->seen_banner = app->banner_seq; ui->dirty = true; }

    if (app->overlay) {
        overlay_input(ui, ts, in, now, out, &n, max);
        return n;
    }
    ui->dlg_up = false;

    /* The INSTANT MIX question owns every input while it is up (OK / CANCEL; Esc, B and Select cancel). */
    if (app->confirm != JF_CONFIRM_NONE) {
        if (!ui->cdlg_up) {
            char msg[128], nm[64];
            lv_fit(nm, sizeof nm, app->confirm_seed.name, 8 * 6 * 2, 2);
            snprintf(msg, sizeof msg, "REPLACE QUEUE WITH INSTANT MIX OF %s?", nm);
            modal_dialog_init(&ui->cdlg, "INSTANT MIX", msg, 2);
            modal_dialog_set_button(&ui->cdlg, 0, "OK", BTN_BG, COLOR_WHITE);
            modal_dialog_set_button(&ui->cdlg, 1, "CANCEL", RGB(90, 40, 40), COLOR_WHITE);
            modal_dialog_set_focus(&ui->cdlg, 1);
            modal_dialog_show(&ui->cdlg);
            ui->cdlg_up = true;
            ui->dirty = true;
        }
        int a = modal_dialog_input(&ui->cdlg, in, 1);
        if (a < 0) a = modal_dialog_update(&ui->cdlg, ts->x, ts->y, ts->pressed || ts->held, now);
        if (ts->pressed || ts->released || bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT) || bp(in, BTN_ID_UP) ||
            bp(in, BTN_ID_DOWN)) ui->dirty = true;
        if (a >= 0) {
            emit(out, &n, max, a == 0 ? JF_EV_CONFIRM : JF_EV_BACK);
            ui->cdlg_up = false;
            ui->dirty = true;
        }
        return n;
    }
    ui->cdlg_up = false;

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
        /* focus 0..4 the row5 buttons (PAUSE, STOP, NEXT, QUEUE, BACK), 5..8 the row above (VOL -, VOL +, MIX,
         * INSTANT MIX); Up / Down walk all nine.  Start / Esc and Space pause, Select / Backspace go back. */
        enum { NP_N = JF_UI_ROW5 + JF_UI_NPMIX };
        if (bp(in, BTN_ID_BACK)) emit(out, &n, max, JF_EV_BACK);
        if (bp(in, BTN_ID_PAUSE) || bp(in, BTN_ID_JUMP)) emit(out, &n, max, JF_EV_PAUSE);
        if (bp(in, BTN_ID_LEFT))  emit(out, &n, max, JF_EV_VOL_DOWN);
        if (bp(in, BTN_ID_RIGHT)) emit(out, &n, max, JF_EV_VOL_UP);
        if (bp(in, BTN_ID_UP))   ui->focus = (ui->focus + NP_N - 1) % NP_N;
        if (bp(in, BTN_ID_DOWN)) ui->focus = (ui->focus + 1) % NP_N;
        if (bp(in, BTN_ID_ACTION)) {
            if (ui->focus < JF_UI_ROW5) row5_act(ui, ui->focus, out, &n, max);
            else np_act(ui->focus - JF_UI_ROW5, out, &n, max);
        }
        for (int i = 0; i < JF_UI_ROW5; i++)
            if (button_check_tap(&ui->row5[i], ts, now)) row5_act(ui, i, out, &n, max);
        for (int i = 0; i < JF_UI_NPMIX; i++)
            if (button_check_tap(&ui->np_mix[i], ts, now)) np_act(i, out, &n, max);
        /* seek: a finger on the bar shows the target, lifting it sends the seek; a tap is a jump */
        const JfItem *cur = jf_now(app);
        int total_ms = cur ? cur->runtime_s * 1000 : 0;
        UiRect sk = ui->L.seek, pr = ui->L.progress;
        if (ts->pressed && total_ms > 0 && ts->x >= sk.x && ts->x < sk.x + sk.w && ts->y >= sk.y && ts->y < sk.y + sk.h)
            ui->seeking = true;
        if (ui->seeking) {
            int dx = ts->x - pr.x;
            if (dx < 0) dx = 0;
            if (dx > pr.w) dx = pr.w;
            ui->seek_ms = (int)((long long)total_ms * dx / pr.w);
            ui->dirty = true;
            if (ts->released) {
                int k = emit(out, &n, max, JF_EV_SEEK);
                if (k >= 0) out[k].i = ui->seek_ms;
                ui->seeking = false;
            } else if (!ts->held && !ts->pressed) ui->seeking = false;
        }
        return n;
    }

    if (sc == JF_SCR_MIX) {
        /* Tick rows are the list; START / CANCEL are the button row (focus 0 / 1).  While the run is going the
         * rows are locked and START is off; CANCEL (BACK) is what the core turns into "stop the run". */
        bool locked = app->mix.phase == JF_MIX_INDEX || app->mix.phase == JF_MIX_POOL;
        int cnt = app->mix.opt_n;
        if (go_back) emit(out, &n, max, JF_EV_BACK);
        ui->mixb[0].disabled = locked;
        const InputState *lin = NULL;
        if (ui->zone == JF_ZONE_LIST) {
            lin = in;
            if (bp(in, BTN_ID_DOWN) && (cnt == 0 || ui->lv.cursor >= cnt - 1)) {
                ui->zone = JF_ZONE_ROW; ui->focus = locked ? 1 : 0; lin = NULL; ui->dirty = true;
            } else if (bp(in, BTN_ID_UP) && ui->lv.cursor <= 0) {
                ui->zone = JF_ZONE_BAR; ui->bar_focus = ui->bar.has_back ? 0 : 1; lin = NULL; ui->dirty = true;
            }
        }
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        mix_sync_marks(ui);
        LvEvent ev = lv_input(&ui->lv, ts, lin, now);
        if (ev != LV_EV_NONE) ui->dirty = true;
        bool pick = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE;
        if (ui->zone == JF_ZONE_LIST && lin && bp(lin, BTN_ID_JUMP)) pick = true;
        if (pick && !locked && ui->lv.cursor >= 0 && ui->lv.cursor < cnt) {
            int k = emit(out, &n, max, JF_EV_MIX_TOGGLE);
            if (k >= 0) out[k].i = ui->lv.cursor;
        }
        if (ui->zone == JF_ZONE_ROW) {
            if (locked) ui->focus = 1;
            else if (bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT)) ui->focus ^= 1;
            if (bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN)) ui->zone = JF_ZONE_LIST;
            else if (act) emit(out, &n, max, ui->focus == 0 ? JF_EV_MIX_START : JF_EV_BACK);
        } else if (ui->zone == JF_ZONE_BAR) {
            if ((bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT)) && ui->bar.has_back) ui->bar_focus ^= 1;
            if (bp(in, BTN_ID_DOWN)) ui->zone = JF_ZONE_LIST;
            else if (act) emit(out, &n, max, ui->bar_focus == 0 ? JF_EV_BACK : JF_EV_QUIT);
        }
        if (!locked && button_check_tap(&ui->mixb[0], ts, now)) emit(out, &n, max, JF_EV_MIX_START);
        if (button_check_tap(&ui->mixb[1], ts, now)) emit(out, &n, max, JF_EV_BACK);
        return n;
    }

    /* list screens and the queue */
    bool queue = sc == JF_SCR_QUEUE;
    bool has_row = queue ? app->queue_n > 0 : true;   /* the queue's one button is SAVE AS PLAYLIST */

    /* The action bar owns focus while the core has it up: a rising edge puts focus on PLAY, its fall
     * (BACK, another intent, a re-aim) returns focus to the list.  Left / Right walk the five buttons,
     * ENTER acts, Up / Down dismiss it and go back to the list; touch on a row retargets it as ever. */
    if (app->action_bar && !ui->abar_up) { ui->zone = JF_ZONE_ABAR; ui->focus = 0; ui->dirty = true; }
    if (!app->action_bar && ui->zone == JF_ZONE_ABAR) { ui->zone = JF_ZONE_LIST; ui->dirty = true; }
    ui->abar_up = app->action_bar;
    if (ui->zone == JF_ZONE_ABAR) {
        if (bp(in, BTN_ID_LEFT))  ui->focus = (ui->focus + JF_UI_ABAR - 1) % JF_UI_ABAR;
        if (bp(in, BTN_ID_RIGHT)) ui->focus = (ui->focus + 1) % JF_UI_ABAR;
        if (bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN)) { emit(out, &n, max, JF_EV_DISMISS); ui->zone = JF_ZONE_LIST; }
        else if (act) abar_act(ui->focus, app->action_row, out, &n, max);
    }
    if (app->action_bar) {
        for (int i = 0; i < JF_UI_ABAR; i++)
            if (button_check_tap(&ui->abar[i], ts, now)) abar_act(i, app->action_row, out, &n, max);
    }
    int count = src_count(ui);
    if (go_back) emit(out, &n, max, JF_EV_BACK);

    if (sc == JF_SCR_ARTISTS) {
        /* Left / Right are free in the list zone (the list takes only Up / Down); the row and the bar
         * zones keep them for moving along their buttons. */
        int step = 0;
        if (ui->zone == JF_ZONE_LIST) step = bp(in, BTN_ID_RIGHT) - bp(in, BTN_ID_LEFT);
        if (button_check_tap(&ui->cat_btn[0], ts, now)) step = -1;
        if (button_check_tap(&ui->cat_btn[1], ts, now)) step = 1;
        if (step) { int k = emit(out, &n, max, JF_EV_CATEGORY); if (k >= 0) out[k].i = step; }
        if (app->cat != ui->last_cat) { ui->last_cat = app->cat; lv_reset(&ui->lv); ui->dirty = true; }
    }

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
    if (ui->zone == JF_ZONE_ABAR) pick = ev == LV_EV_TAP;   /* the pad does not select a row under the bar */
    if (pick && ui->lv.cursor >= 0 && ui->lv.cursor < count) {
        int k = emit(out, &n, max, queue ? JF_EV_QUEUE_JUMP : JF_EV_SELECT);
        if (k >= 0) out[k].i = ui->lv.cursor;
    }

    if (!queue && app->list && ui->zone == JF_ZONE_LIST && count > 0 && ui->lv.cursor >= count - 6 &&
        count < app->list->total && !app->list->full && !app->list->pending && !app->busy)
        emit(out, &n, max, JF_EV_NEED_MORE);

    if (ui->zone == JF_ZONE_ROW && has_row && queue) {
        if (bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN)) ui->zone = JF_ZONE_LIST;
        else if (act) do_save(ui, ptr, out, &n, max);
    } else if (ui->zone == JF_ZONE_ROW && has_row) {
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
    if (queue && has_row) {
        if (button_check_tap(&ui->save, ts, now)) do_save(ui, ptr, out, &n, max);
    } else if (has_row && !app->action_bar) {
        for (int i = 0; i < JF_UI_ROW3; i++)
            if (row3_on(app, i) && button_check_tap(&ui->row3[i], ts, now)) row3_act(ui, ptr, i, out, &n, max);
    }
    if (mini_wanted(app)) {
        static const JfEventType mt[JF_UI_MINI] = { JF_EV_PAUSE, JF_EV_VOL_DOWN, JF_EV_VOL_UP };
        for (int i = 0; i < JF_UI_MINI; i++)
            if (button_check_tap(&ui->mini_b[i], ts, now)) emit(out, &n, max, mt[i]);
        UiRect t = ui->L.mini_title;
        if (ts && ts->released && ts->x >= t.x && ts->x < t.x + t.w && ts->y >= t.y && ts->y < t.y + t.h)
            emit(out, &n, max, JF_EV_OPEN_NOWPLAYING);
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
    else if (app->list && app->screen != JF_SCR_ARTISTS && app->list->total > 0) {
        /* the server's count, so a ranked first page is not read as the whole answer */
        char c[32];
        const char *what = app->screen == JF_SCR_SEARCH ? "RESULTS" : app->screen == JF_SCR_ALBUMS ? "ALBUMS" : "TRACKS";
        snprintf(c, sizeof c, "%d %s", app->list->total, what);
        fb_draw_text(ui->fb, b.x + 4, b.y + 2, c, DIM, 2);
    }
    if (app->busy) fb_draw_text(ui->fb, b.x + b.w - 8 - text_measure_width("LOADING", 2), b.y + 2, "LOADING", DIM, 2);
    if (app->screen == JF_SCR_MIX && !app->banner[0] &&
        (app->mix.phase == JF_MIX_INDEX || app->mix.phase == JF_MIX_POOL)) {
        char c[32];
        snprintf(c, sizeof c, "MIXING... %d", app->mix.found_n);
        fb_draw_text(ui->fb, b.x + 4, b.y + 2, c, RING, 2);
    }
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
    if (ui->app->action_bar) {   /* the bar takes the button row's place */
        for (int i = 0; i < JF_UI_ABAR; i++) {
            button_draw(ui->fb, &ui->abar[i]);
            if (ui->show_ring && ui->zone == JF_ZONE_ABAR && ui->focus == i) ring(ui, ui->L.abar[i]);
        }
        return;
    }
    for (int i = 0; i < JF_UI_ROW3; i++) {
        if (!row3_on(ui->app, i)) continue;
        button_draw(ui->fb, &ui->row3[i]);
        if (ui->show_ring && ui->zone == JF_ZONE_ROW && ui->focus == i) ring(ui, ui->L.row3[i]);
    }
}

/* The root strip: [<]  CATEGORY  count  [>]. */
static void draw_category(JfUi *ui)
{
    const JfApp *app = ui->app;
    char t[176];
    if (app->list && app->list->total > 0) snprintf(t, sizeof t, "%s  %d", app->crumb, app->list->total);
    else snprintf(t, sizeof t, "%s", app->crumb);
    UiRect r = ui->L.cat_label;
    int w = text_measure_width(t, 3);
    fb_draw_text(ui->fb, r.x + (r.w - w) / 2, r.y + (r.h - 21) / 2, t, COLOR_WHITE, 3);
    button_draw(ui->fb, &ui->cat_btn[0]);
    button_draw(ui->fb, &ui->cat_btn[1]);
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
    const char *state = app->play_state == JF_PLAY_FETCHING ? "LOADING..." : app->play_state == JF_PLAY_PAUSED ? "PAUSED"
                      : app->play_state == JF_PLAY_STOPPED ? "STOPPED"
                      : app->play_state == JF_PLAY_END ? "END OF QUEUE" : "";
    if (state[0]) fb_draw_text(fb, in.x + 4, in.y + 94, state, RING, 2);

    UiRect p = ui->L.progress;
    int total = it ? it->runtime_s : 0;
    int sec = ui->played_ms / 1000;
    if (app->play_state != JF_PLAY_PLAYING && app->play_state != JF_PLAY_PAUSED) sec = 0;
    if (ui->seeking) sec = ui->seek_ms / 1000;   /* the would-be position under the finger */
    fb_fill_rect(fb, p.x, p.y, p.w, p.h, PANEL);
    if (total > 0) {
        int fill = sec >= total ? p.w : (int)((long)p.w * sec / total);
        fb_fill_rect(fb, p.x, p.y, fill, p.h, ui->seeking ? RING : BTN_HL);
    }
    fb_draw_rect(fb, p.x, p.y, p.w, p.h, DIM);
    char a[16], z[16];
    mmss(a, sizeof a, sec);
    mmss(z, sizeof z, total);
    snprintf(buf, sizeof buf, "%s / %s", a, z);
    fb_draw_text(fb, p.x, p.y + p.h + 8, buf, ui->seeking ? RING : COLOR_WHITE, 2);
    if (app->volume_step == 0) snprintf(buf, sizeof buf, "MUTE");
    else snprintf(buf, sizeof buf, "VOLUME %d/%d", app->volume_step, JF_VOLUME_MAX);
    fb_draw_text(fb, p.x + p.w - text_measure_width(buf, 2), p.y + p.h + 8, buf, COLOR_WHITE, 2);

    snprintf(ui->row5[0].text, sizeof ui->row5[0].text, "%s", mini_play_label(app));
    for (int i = 0; i < JF_UI_NPMIX; i++) {
        button_draw(fb, &ui->np_mix[i]);
        if (ui->show_ring && ui->focus == JF_UI_ROW5 + i) ring(ui, ui->L.np_mix[i]);
    }
    for (int i = 0; i < JF_UI_ROW5; i++) {
        button_draw(fb, &ui->row5[i]);
        if (ui->show_ring && ui->focus == i) ring(ui, ui->L.row5[i]);
    }
}

static void draw_mini(JfUi *ui)
{
    const JfApp *app = ui->app;
    Framebuffer *fb = ui->fb;
    UiRect m = ui->L.mini_title;
    fb_fill_rect(fb, m.x, m.y, m.w, m.h, PANEL);
    const JfItem *it = jf_now(app);
    if (it) {
        char t[192], f[160];
        if (it->artist[0]) snprintf(t, sizeof t, "%s - %s", it->name, it->artist);
        else snprintf(t, sizeof t, "%s", it->name);
        lv_fit(f, sizeof f, t, m.w - 12, 2);
        fb_draw_text(fb, m.x + 6, m.y + (m.h - 14) / 2, f, COLOR_WHITE, 2);
    }
    snprintf(ui->mini_b[0].text, sizeof ui->mini_b[0].text, "%s", mini_play_label(app));
    for (int i = 0; i < JF_UI_MINI; i++) button_draw(fb, &ui->mini_b[i]);
}

void jf_ui_draw(JfUi *ui)
{
    const JfApp *app = ui->app;
    Framebuffer *fb = ui->fb;
    fb_clear(fb, BG);

    const char *title = "JELLYFIN";
    switch (app->screen) {
    case JF_SCR_ALBUMS: case JF_SCR_TRACKS: case JF_SCR_SEARCH: case JF_SCR_MIX:
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
    case JF_SCR_ARTISTS:
        if (mini_wanted(app)) draw_mini(ui);
        draw_category(ui);
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, app->busy ? "" : "NOTHING HERE");
        draw_row3(ui);
        break;
    case JF_SCR_QUEUE:
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, "QUEUE IS EMPTY");
        if (app->queue_n > 0) {
            button_draw(fb, &ui->save);
            if (ui->show_ring && ui->zone == JF_ZONE_ROW) ring(ui, ui->L.save);
        }
        break;
    case JF_SCR_MIX:
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        mix_sync_marks(ui);
        lv_draw(fb, &ui->lv, "NOTHING TO TICK");
        ui->mixb[0].disabled = app->mix.phase == JF_MIX_INDEX || app->mix.phase == JF_MIX_POOL;
        for (int i = 0; i < JF_UI_MIXBTN; i++) {
            button_draw(fb, &ui->mixb[i]);
            if (ui->show_ring && ui->zone == JF_ZONE_ROW && ui->focus == i) ring(ui, ui->L.mixb[i]);
        }
        break;
    default:
        if (mini_wanted(app)) draw_mini(ui);
        ui->lv.active = ui->zone == JF_ZONE_LIST;
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, app->busy ? "" : "NOTHING HERE");
        draw_row3(ui);
        break;
    }
    if (app->confirm != JF_CONFIRM_NONE && ui->cdlg_up) modal_dialog_draw(&ui->cdlg, fb);
    if (app->overlay && ui->dlg_up) modal_dialog_draw(&ui->dlg, fb);
}
