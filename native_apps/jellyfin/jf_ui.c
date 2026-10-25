/*
 * jf_ui.c — the Ludfin screens.  Draw and focus only; jf_core decides.
 * See jf_ui.h.
 */
#include "jf_ui.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "icons.h"
#include "jf_app.h"
#include "jf_art.h"
#include "jf_fmt_icon.h"
#include "keyboard.h"
#include "font.h"
#include "audio_out.h"

#define BG        RGB(16, 18, 26)
#define PANEL     RGB(30, 36, 52)
#define BTN_BG    RGB(40, 64, 104)
#define BTN_HL    RGB(70, 104, 160)
#define DIM       RGB(140, 150, 170)
#define RING      RGB(255, 220, 60)

#define BANNER_H  18
#define GAP        6
#define FIELD_H   52
#define TAG_LINE_H   18   /* the band's top offset in jf_ui_layout: kept at the old scale-2 height so the band rect does not move */
#define TAG_FONT     1    /* the file-tag band is set in the small font */
#define TAG_LINE1_H  FONT_ROWS  /* a scale-1 line: its glyph rows */
#define TAG_PITCH    10   /* line to line in the file-tag band: one row of air under the glyphs */
#define TAG_WORK_MAX 2    /* most lines the WORK part wraps onto */
#define TAG_WITH_MAX 3    /* most lines the WITH (performers) part wraps onto */
#define TAG_AA_MAX   2    /* most lines the ALBUM ARTIST part wraps onto */
#define TAG_ROWS_MAX JF_TAG_LINES_MAX  /* conductor, work, performers, album artist, year and genres */
#define NP_MIN_W  72   /* narrowest now-playing button; 10 of them fit one row at 774 px or more */
#define QR_MIN_W  56   /* narrowest queue-view button; 11 of them fit one row at 676 px or more (65 px each on the unit's 780) */
#define NP_ICON    22   /* the now-playing glyphs: a square box in the button's middle */

/* ── Layout (pure) ─────────────────────────────────────────────────────── */

static UiRect rect_of(int x, int y, int w, int h) { UiRect r = { x, y, w, h }; return r; }

/* `count` buttons of at least `min_w` in one row across [sl, sl+sw), or in the fewest equal rows when the width cannot
 * hold them; the last row is the one at `rowy`, the others stack upward.  Returns the number of rows. */
static int row_layout(UiRect *out, int count, int min_w, int sl, int sw, int rowy)
{
    int per = (sw + GAP) / (min_w + GAP);
    if (per < 1) per = 1;
    if (per > count) per = count;
    int rows = (count + per - 1) / per;
    per = (count + rows - 1) / rows;
    int wn = (sw - (per - 1) * GAP) / per;
    for (int i = 0; i < count; i++) {
        int r = i / per, c = i - r * per;
        out[i] = rect_of(sl + c * (wn + GAP), rowy - (rows - 1 - r) * (JF_UI_BTN_H + GAP), wn, JF_UI_BTN_H);
    }
    return rows;
}

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
    L->list_set = rect_of(sl, top, sw, rowy + JF_UI_BTN_H - top);

    int w3 = (sw - 2 * GAP) / 3;
    for (int i = 0; i < JF_UI_ROW3; i++)
        L->row3[i] = rect_of(sl + i * (w3 + GAP), rowy, w3, JF_UI_BTN_H);
    int w4 = (sw - 3 * GAP) / 4;
    for (int i = 0; i < JF_UI_ROW4; i++)
        L->row4[i] = rect_of(sl + i * (w4 + GAP), rowy, w4, JF_UI_BTN_H);
    int w6 = (sw - 5 * GAP) / 6;
    for (int i = 0; i < JF_UI_ABAR; i++)
        L->abar[i] = rect_of(sl + i * (w6 + GAP), rowy, w6, JF_UI_BTN_H);
    for (int i = 0; i < JF_UI_MIXBTN; i++)
        L->mixb[i] = rect_of(sl + i * ((sw - GAP) / 2 + GAP), rowy, (sw - GAP) / 2, JF_UI_BTN_H);
    /* now playing and its queue view: one row of buttons, none narrower than the minimum; when the width does not hold
     * them all (portrait) they wrap into the fewest equal rows, stacked upward from the button row */
    row_layout(L->np, JF_UI_NP, NP_MIN_W, sl, sw, rowy);
    L->qrow_rows = row_layout(L->qrow, JF_UI_QROW, QR_MIN_W, sl, sw, rowy);
    L->list_q = L->list;
    L->list_q.h -= (L->qrow_rows - 1) * (JF_UI_BTN_H + GAP);

    for (int i = 0; i < JF_UI_FIELDS; i++)
        L->field[i] = rect_of(sl, top + 4 + i * (FIELD_H + GAP), sw, FIELD_H);
    int wb = (sw - GAP) / 2;
    int by = top + 4 + JF_UI_FIELDS * (FIELD_H + GAP) + 8;
    L->signin = rect_of(sl, by, wb, FIELD_H);
    L->signexit = rect_of(sl + wb + GAP, by, wb, FIELD_H);

    L->info = rect_of(sl, top + 6, sw, 120);
    L->progress = rect_of(sl + 20, top + 6 + 120 + 10, sw - 40, 14);
    L->seek = rect_of(L->progress.x, L->progress.y - 10, L->progress.w, 34);
    L->source = rect_of(L->info.x + 4, L->info.y + 110, L->info.w - 8, FONT_ROWS);   /* under the state line (y 94..108), above the seek bar (info bottom + 6) */
    /* the file-tag band: from under the time row to above the transport row, on the seek bar's margins */
    int np_top = L->np[0].y;
    for (int i = 1; i < JF_UI_NP; i++) if (L->np[i].y < np_top) np_top = L->np[i].y;
    int tag_y = L->progress.y + L->progress.h + 8 + TAG_LINE_H + GAP;
    L->tags = rect_of(L->progress.x, tag_y, L->progress.w, np_top - GAP - tag_y);

    /* mini player: a strip right above the button row; the lists shrink by its height + a gap */
    int mini_y = rowy - 4 - JF_UI_MINI_H;
    L->list_mini = L->list;
    L->list_mini.h = mini_y - 4 - L->list.y;
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
    if (ui->app->screen == JF_SCR_PLPICK) return ui->app->lists[JF_LIST_PLPICK].n + 1;   /* row 0 is + NEW PLAYLIST */
    if (ui->app->screen == JF_SCR_SETTINGS) {
        int ids[JF_SET_COUNT];
        if (ui->app->settings_page == JF_SETPAGE_LIBRARY) return 1 + ui->app->lists[JF_LIST_LIBS].n;
        return jf_settings_rows(ui->app, ids);
    }
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
/* The settings view's library page marks the chosen library the same way: row 0 is ALL LIBRARIES, row k the k-1'th library. */
static int chosen_lib_row(const JfApp *app)
{
    if (!app->login.library[0]) return 0;
    for (int k = 0; k < app->lists[JF_LIST_LIBS].n; k++)
        if (strcmp(app->lists[JF_LIST_LIBS].items[k].id, app->login.library) == 0) return k + 1;
    return 0;
}

static bool mark_want(const JfApp *app, int i)
{
    if (app->screen == JF_SCR_SETTINGS) return app->settings_page == JF_SETPAGE_LIBRARY && i == chosen_lib_row(app);
    if (app->screen == JF_SCR_PLPICK) return i == app->pick_sel + 1 && app->pick_sel >= 0;   /* the chosen playlist, one bar */
    return i < app->mix.opt_n && i < JF_MIX_OPT_MAX && app->mix.opt[i].on;
}

static void mix_sync_marks(JfUi *ui)
{
    ListView *lv = &ui->lv;
    const JfApp *app = ui->app;
    int saved = lv->cursor;
    for (int i = 0; i < lv->count; i++) {
        bool want = mark_want(app, i);
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
    if (app->screen == JF_SCR_PLPICK) {
        /* row 0 starts a new playlist; row k is the picker list's k-1'th playlist */
        out->glyph = LV_GLYPH_BLANK;
        if (index == 0) { out->title = "+ NEW PLAYLIST"; return; }
        if (index < 0 || index > app->lists[JF_LIST_PLPICK].n) return;
        out->title = app->lists[JF_LIST_PLPICK].items[index - 1].name;
        return;
    }
    if (app->screen == JF_SCR_SETTINGS) {
        char *t = ui->rowtxt[ui->rowtxt_i];
        ui->rowtxt_i = (ui->rowtxt_i + 1) % 32;
        out->glyph = LV_GLYPH_BLANK;
        out->title = t;
        t[0] = '\0';
        if (app->settings_page == JF_SETPAGE_LIBRARY) {
            if (index == 0) snprintf(t, sizeof ui->rowtxt[0], "ALL LIBRARIES");
            else if (index - 1 < app->lists[JF_LIST_LIBS].n) snprintf(t, sizeof ui->rowtxt[0], "%.47s", app->lists[JF_LIST_LIBS].items[index - 1].name);
            return;
        }
        int ids[JF_SET_COUNT];
        int cnt = jf_settings_rows(app, ids);
        if (index < 0 || index >= cnt) return;
        if (ids[index] == JF_SET_SIGNOUT) jf_ui_signout_label(app, t, sizeof ui->rowtxt[0]);
        else if (ids[index] == JF_SET_TAP) jf_ui_tap_label(app, t, sizeof ui->rowtxt[0]);
        else if (ids[index] == JF_SET_TAGS) jf_ui_tags_label(app, t, sizeof ui->rowtxt[0]);
        else {
            int r = chosen_lib_row(app);
            if (r == 0) snprintf(t, sizeof ui->rowtxt[0], "ALL LIBRARIES");
            else snprintf(t, sizeof ui->rowtxt[0], "LIBRARY: %.38s", app->lists[JF_LIST_LIBS].items[r - 1].name);
        }
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
    /* a track shows its format (FLAC, MP3, ...) when an icon file exists for its container; the
     * queue's current row keeps the play triangle instead */
    if (app->screen == JF_SCR_QUEUE && index == app->queue_cur) out->glyph = LV_GLYPH_PLAYING;
    else if (it->type == JF_ITEM_TRACK) out->icon = jf_fmt_icon_get(it->container);
}

/* ── Setup ─────────────────────────────────────────────────────────────── */

static int pick_count(void *ctx) { (void)ctx; return JF_CAT_COUNT; }

static const char *pick_name(void *ctx, int index) { (void)ctx; return jf_cat_name((JfCat)index); }

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
    ui->bar.title = JF_APP_TITLE;
    ui->bar.title_color = COLOR_WHITE;
    ui->bar.title_cut = true;
    top_bar_layout(&ui->bar);

    LvSource src = { src_count, src_row, ui };
    lv_init(&ui->lv, src, ui->L.list, 40);

    static const char *t3[JF_UI_ROW3] = { "SEARCH", "NOW PLAYING", "PLAY ALL" };
    for (int i = 0; i < JF_UI_ROW3; i++) style(&ui->row3[i], ui->L.row3[i], t3[i], 2);
    static const char *ta[JF_UI_ABAR] = { "PLAY", "PLAY NEXT", "ADD QUEUE", "MIX", "I-MIX", "+ PLAYLIST" };
    static const char *tn[JF_UI_NP] = { "", "", "", "", "QUEUE", "MIX", "I-MIX", "EQ", "VOL+", "VOL-" };
    static const char *tmini[JF_UI_MINI] = { "PAUSE", "VOL -", "VOL +" };
    for (int i = 0; i < JF_UI_MINI; i++) style(&ui->mini_b[i], ui->L.mini_btn[i], tmini[i], 2);
    for (int i = 0; i < JF_UI_MINI; i++) style(&ui->pmenu[i], ui->L.mini_btn[i], "", 2);
    /* a label whose ink at scale 2 would leave no margin in its button ("+ PLAYLIST" is 120 px in a 124 px one) drops to scale 1 */
    for (int i = 0; i < JF_UI_ABAR; i++)
        style(&ui->abar[i], ui->L.abar[i], ta[i], 6 * (int)strlen(ta[i]) * 2 <= ui->L.abar[i].w - 12 ? 2 : 1);
    for (int i = 0; i < JF_UI_ROW4; i++) {
        const char *t4 = i < JF_UI_ROW3 ? t3[i] : "+ PLAYLIST";
        style(&ui->row4[i], ui->L.row4[i], t4, 6 * (int)strlen(t4) * 2 <= ui->L.row4[i].w - 12 ? 2 : 1);
    }
    for (int i = 0; i < JF_UI_NP; i++) style(&ui->np[i], ui->L.np[i], tn[i], 2);
    static const char *tq[JF_UI_QROW] = { "", "", "", "", "SAVE", "CLR", "RND", "REMOVE", "EQ", "VOL+", "VOL-" };
    /* a label whose ink at scale 2 would leave no margin in its button (REMOVE in the 65 px buttons) drops to scale 1 */
    for (int i = 0; i < JF_UI_QROW; i++)
        style(&ui->qrow[i], ui->L.qrow[i], tq[i], 6 * (int)strlen(tq[i]) * 2 <= ui->L.qrow[i].w - 12 ? 2 : 1);
    style(&ui->mixb[0], ui->L.mixb[0], "START", 2);
    style(&ui->mixb[1], ui->L.mixb[1], "CANCEL", 2);
    style(&ui->plb[0], ui->L.mixb[0], "ADD", 2);
    style(&ui->plb[1], ui->L.mixb[1], "CLOSE", 2);
    for (int i = 0; i < JF_UI_FIELDS; i++) style(&ui->field[i], ui->L.field[i], "", 2);
    style(&ui->signin, ui->L.signin, "SIGN IN", 3);
    style(&ui->signexit, ui->L.signexit, "EXIT", 3);
    TpSource ps = { pick_count, pick_name, ui };
    tp_init(&ui->pick, ps);

    ui->last_screen = (JfScreen)-1;
    touch_guard_init(&ui->guard);
    ui->last_cat = app->cat;
    ui->zf.shown = true;
    ui->dirty = true;
}

void jf_ui_free(JfUi *ui)
{
    lv_free(&ui->lv);
    tp_free(&ui->pick);
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
    if (d) ui->info_dirty = false;   /* the full frame carries the new text */
    return d;
}

bool jf_ui_take_info(JfUi *ui, UiRect *r)
{
    if (!ui->info_dirty) return false;
    ui->info_dirty = false;
    if (top_bar_info_present(ui->fb, &ui->bar, r)) return true;   /* patched and presented */
    ui->dirty = true;                /* the text moved another section: the next frame draws it all */
    return false;
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

/* The list-over-buttons screens (PLPICK, MIX) as a ui_zone stack, top to bottom: the top bar (BACK 0, EXIT 1), the
 * list, the button row (the screen's first button 0, its second 1).  The first button is absent when it cannot act. */
enum { STACK_BAR = 0, STACK_LIST, STACK_ROW, STACK_N };

static bool mix_locked(const JfApp *app) { return app->mix.phase == JF_MIX_INDEX || app->mix.phase == JF_MIX_POOL; }

static void zone_add(UiZone *z, UiRect r, int id)
{
    z->r[z->n] = r;
    z->id[z->n] = id;
    z->n++;
}

static void stack_zones(JfUi *ui, UiZone z[UZ_ZONES], bool first_on)
{
    memset(z, 0, UZ_ZONES * sizeof z[0]);
    if (ui->bar.has_back) zone_add(&z[STACK_BAR], top_bar_ring_rect(&ui->bar.L, TB_BACK), 0);
    if (ui->bar.has_exit) zone_add(&z[STACK_BAR], top_bar_ring_rect(&ui->bar.L, TB_EXIT), 1);
    z[STACK_LIST].lv = &ui->lv;
    for (int i = first_on ? 0 : 1; i < JF_UI_MIXBTN; i++) zone_add(&z[STACK_ROW], ui->L.mixb[i], i);
}

/* Settings: the bar over the list, no button row. */
static void settings_zones(JfUi *ui, UiZone z[UZ_ZONES])
{
    memset(z, 0, UZ_ZONES * sizeof z[0]);
    if (ui->bar.has_back) zone_add(&z[STACK_BAR], top_bar_ring_rect(&ui->bar.L, TB_BACK), 0);
    if (ui->bar.has_exit) zone_add(&z[STACK_BAR], top_bar_ring_rect(&ui->bar.L, TB_EXIT), 1);
    z[STACK_LIST].lv = &ui->lv;
}

/* Now playing: three panes - the bar, the seek bar (one UZ_SLIDER button: Left / Right seek) and the ten buttons (one row, two in portrait). */
enum { NPZ_BAR = 0, NPZ_SEEK, NPZ_ROW, NPZ_N };

static void np_zones(JfUi *ui, UiZone z[UZ_ZONES])
{
    memset(z, 0, UZ_ZONES * sizeof z[0]);
    if (ui->bar.has_back) zone_add(&z[NPZ_BAR], top_bar_ring_rect(&ui->bar.L, TB_BACK), 0);
    if (ui->bar.has_exit) zone_add(&z[NPZ_BAR], top_bar_ring_rect(&ui->bar.L, TB_EXIT), 1);
    zone_add(&z[NPZ_SEEK], ui->L.seek, 0);
    z[NPZ_SEEK].flags = UZ_SLIDER;
    for (int i = 0; i < JF_UI_NP; i++) zone_add(&z[NPZ_ROW], ui->L.np[i], i);
}

/* SIGN IN is absent (not greyed) until the server and the user are filled. */
static bool signin_can(const JfUi *ui) { return ui->fields[0][0] && ui->fields[1][0]; }

/* Sign-in: three panes - the bar (EXIT only), the three fields (ids 0..2) and the buttons: SIGN IN (3, when present), EXIT (4). */
enum { SIZ_BAR = 0, SIZ_FIELDS, SIZ_BTN, SIZ_N };

static void signin_zones(JfUi *ui, UiZone z[UZ_ZONES])
{
    memset(z, 0, UZ_ZONES * sizeof z[0]);
    if (ui->bar.has_back) zone_add(&z[SIZ_BAR], top_bar_ring_rect(&ui->bar.L, TB_BACK), 0);
    if (ui->bar.has_exit) zone_add(&z[SIZ_BAR], top_bar_ring_rect(&ui->bar.L, TB_EXIT), 1);
    for (int i = 0; i < JF_UI_FIELDS; i++) zone_add(&z[SIZ_FIELDS], ui->L.field[i], i);
    if (signin_can(ui)) zone_add(&z[SIZ_BTN], ui->L.signin, 3);
    zone_add(&z[SIZ_BTN], ui->L.signexit, 4);
}

/* How the stack's list looks: active only while it holds focus, the yellow pad edge only after a key (a touch hides it). */
static void stack_lv_look(JfUi *ui)
{
    ui->lv.active = ui->zf.zone == STACK_LIST;
    ui->lv.ring_hidden = !ui->zf.shown;
    ui->lv.tap2_any = true;
}

/* The playlist action menu: the core raised the action bar on a playlist row (the track bar is the five-button one). */
static bool pmenu_up(const JfApp *app)
{
    return app->action_bar && app->list && app->action_row >= 0 && app->action_row < app->list->n &&
           app->list->items[app->action_row].type == JF_ITEM_PLAYLIST;
}

/* OPEN, DELETE and CANCEL; DELETE is absent (not greyed) when the policy gives no right, so two buttons share the strip */
static int pmenu_n(const JfApp *app) { return jf_can_delete(app) ? 3 : 2; }

/* Button j of the shown ones sits in the strip slot that keeps CANCEL at the right edge. */
static int pmenu_slot(const JfApp *app, int j) { return JF_UI_MINI - pmenu_n(app) + j; }

/* 0 OPEN, 1 DELETE, 2 CANCEL */
static int pmenu_kind(const JfApp *app, int j) { return pmenu_n(app) == 3 ? j : (j == 0 ? 0 : 2); }

/* The strip is up for the mini player (a queue exists) and for the playlist menu, which takes its place. */
static bool mini_wanted(const JfApp *app)
{
    return (app->queue_n > 0 || pmenu_up(app)) && is_list_screen(app->screen);
}

/* The mini player itself: its buttons are neither drawn nor hit-tested while the menu is on the strip. */
static bool mini_player(const JfApp *app)
{
    return mini_wanted(app) && !pmenu_up(app);
}


/* ADD exists only with a playlist chosen and nothing in flight: it is absent (not greyed) otherwise. */
bool jf_ui_plpick_add_shown(const JfApp *app)
{
    return app->pick_sel >= 0 && app->pick_sel < app->lists[JF_LIST_PLPICK].n &&
           !app->pick_wait && !app->dup_scan && !app->add_pending;
}

/* The duplicate question's message.  The name is cut to the dialog's width (20 glyphs at scale 2); the counts say how much
 * of the add is already there, and an unchecked add says so instead. */
void jf_ui_dup_title(const JfApp *app, char *buf, size_t cap)
{
    char nm[64];
    if (app->dup_unknown) { snprintf(buf, cap, "COULD NOT CHECK DUPLICATES - ADD?"); return; }
    lv_fit(nm, sizeof nm, app->add_name, 20 * 6 * 2, 2);
    if (app->dup_hits >= app->pick_n) snprintf(buf, cap, "ALREADY IN %s", nm);
    else snprintf(buf, cap, "%d OF %d ALREADY IN %s", app->dup_hits, app->pick_n, nm);
}
/* The tap-action settings row. */
void jf_ui_tap_label(const JfApp *app, char *buf, size_t cap)
{
    static const char *const nm[JF_TAP_COUNT] = { "REPLACE QUEUE", "ADD AND PLAY", "ADD ONLY" };
    int t = app->login.tap_action;
    snprintf(buf, cap, "TAP: %s", nm[t >= 0 && t < JF_TAP_COUNT ? t : JF_TAP_REPLACE]);
}

void jf_ui_tags_label(const JfApp *app, char *buf, size_t cap)
{
    snprintf(buf, cap, "FILE TAGS: %s", jf_tags_blocked(app) ? "SERVER REFUSED" : app->login.tags_on ? "ON" : "OFF");
}

/* The sign-out row: "SIGN OUT FROM <host>", the host of the address typed at sign-in, upcased.  The list view cuts a long row
 * with "..." at its width, so the host (the tail) is what gives way.  No usable address: plain "SIGN OUT". */
void jf_ui_signout_label(const JfApp *app, char *buf, size_t cap)
{
    char host[64];
    int port = 0;
    bool tls = false;
    if (!app->login.server[0] || jf_parse_server(app->login.server, host, sizeof host, &port, &tls) != JF_SERVER_OK) {
        snprintf(buf, cap, "SIGN OUT");
        return;
    }
    for (char *p = host; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    snprintf(buf, cap, "SIGN OUT FROM %s", host);
}

/* The new playlist's name: the keyboard asks (an empty or cancelled entry sends nothing); the core makes it from the picked ids. */
static void do_pick_new(JfUi *ui, Pointer *ptr, JfEvent *out, int *n, int max)
{
    char name[96] = "";
    int rc = keyboard_enter(ui->fb, ui->touch, ui->gm, "PLAYLIST NAME", name, 80, KB_LAYOUT_FULL);
    drain_after_keyboard(ui, ptr);
    if (rc != KB_RESULT_OK || !name[0]) return;
    int k = emit(out, n, max, JF_EV_PICK_NEW);
    if (k >= 0) snprintf(out[k].term, sizeof out[k].term, "%s", name);
}

static const char *mini_play_label(const JfApp *app)
{
    return app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING ? "PAUSE" : "PLAY";
}

static UiRect list_rect_for(const JfUi *ui, JfScreen s)
{
    if (s == JF_SCR_QUEUE) return ui->L.list_q;
    return ui->mini_on ? ui->L.list_mini : ui->L.list;
}

static void settings_sync(JfUi *ui);

static void begin_screen(JfUi *ui)
{
    const JfApp *app = ui->app;
    ui->last_screen = app->screen;
    ui->abar_up = false;
    ui->abar_tz = -1;
    ui->lv.ring_hidden = ui->lv.tap2_any = false;   /* the settings list keeps today's look; the zone screens set them every frame */
    bool shown = ui->zf.shown;   /* the ring stays as the last screen left it: a touch hid it, a key shows it */
    /* every screen is a pane cycle (Tab / LB-RB); now playing starts on its buttons, sign-in on its fields, the rest on the list */
    uz_begin_cycle(&ui->zf, app->screen == JF_SCR_NOWPLAYING ? NPZ_ROW : STACK_LIST);
    ui->zf.shown = shown;
    ui->dirty = true;
    ui->bar.has_back = app->screen != JF_SCR_ARTISTS && app->screen != JF_SCR_SIGNIN;
    ui->bar.has_exit = true;
    top_bar_layout(&ui->bar);
    if (app->screen == JF_SCR_SIGNIN) {
        if (!ui->fields[0][0]) snprintf(ui->fields[0], sizeof ui->fields[0], "%s", app->login.server);
        if (!ui->fields[1][0]) snprintf(ui->fields[1], sizeof ui->fields[1], "%s", app->login.user);
    }
    if (app->screen == JF_SCR_QUEUE || app->screen == JF_SCR_MIX || app->screen == JF_SCR_PLPICK || is_list_screen(app->screen)) {
        ui->mini_on = mini_wanted(app);
        ui->lv.rect = list_rect_for(ui, app->screen);
        lv_reset(&ui->lv);
        if (app->screen == JF_SCR_QUEUE && app->queue_n > 0) lv_move_to(&ui->lv, app->queue_cur);
    }
    if (app->screen == JF_SCR_SETTINGS) {
        ui->mini_on = false;
        ui->lv.rect = ui->L.list_set;
        ui->last_page = (JfSettingsPage)-1;
        settings_sync(ui);
    }
}

/* The settings page changed (or the screen just opened): the list starts over, on the chosen library when it is the library page. */
static void settings_sync(JfUi *ui)
{
    const JfApp *app = ui->app;
    if (app->settings_page == ui->last_page) return;
    ui->last_page = app->settings_page;
    lv_reset(&ui->lv);
    lv_sync(&ui->lv);
    if (app->settings_page == JF_SETPAGE_LIBRARY) lv_move_to(&ui->lv, chosen_lib_row(app));
    mix_sync_marks(ui);
    ui->dirty = true;
}

/* The button row: SEARCH, NOW PLAYING and, at the root, SETTINGS; below the root SEARCH, NOW PLAYING and (on track lists)
 * PLAY ALL.  The root's categories are picked from the header drop-down. */
static bool at_root(const JfApp *app) { return app->screen == JF_SCR_ARTISTS; }

/* The artist folder (albums, then tracks of other albums): the core queues its whole discography for PLAY ALL. */
static bool artist_folder(const JfApp *app) { return app->screen == JF_SCR_ALBUMS && app->ctx[JF_LIST_ALBUMS].sub == JF_SUB_PLAIN; }
/* A search result list that holds a track, an album or an artist: what PLAY ALL plays and ADD ALL TO PLAYLIST adds, albums and artists expanded to their tracks. */
/* A search result list that holds a track: the rows PLAY ALL plays and ADD ALL TO PLAYLIST adds. */
static bool search_has_tracks(const JfApp *app) { return jf_search_playable(app); }

/* An album's, a playlist's or a genre's tracks carry a fourth button, + PLAYLIST, and a search with a track, an album or an artist in it ADD ALL TO PLAYLIST; an artist's albums do not. */
static bool tracks4(const JfApp *app)
{
    if (app->screen == JF_SCR_SEARCH) return search_has_tracks(app);   /* ADD ALL TO PLAYLIST, over the tracks PLAY ALL plays */
    return app->screen == JF_SCR_TRACKS;
}

static int row_n(const JfApp *app) { return tracks4(app) ? JF_UI_ROW4 : JF_UI_ROW3; }

/* Which slots of the row exist on this screen. */
static bool row3_on(const JfApp *app, int i)
{
    if (i == 0) return true;
    if (i == 1) return app->queue_n > 0;
    if (i == 3) return tracks4(app);
    return at_root(app) || app->screen == JF_SCR_TRACKS || app->screen == JF_SCR_SEARCH || artist_folder(app);
}


static void row3_act(JfUi *ui, Pointer *ptr, int i, JfEvent *out, int *n, int max)
{
    if (i == 0) do_search(ui, ptr, out, n, max);
    else if (i == 1) emit(out, n, max, JF_EV_OPEN_NOWPLAYING);
    else if (i == 3) {   /* every track of the list; the core fetches the pages not loaded yet */
        int k = emit(out, n, max, JF_EV_PICK_OPEN);
        if (k >= 0) out[k].i = -1;
    }
    else emit(out, n, max, at_root(ui->app) ? JF_EV_SETTINGS_OPEN : JF_EV_PLAY_ALL);
}

/* The queue screen's button row: 0..3 the transport (as on now playing), 4 SAVE (as playlist), 5 CLR (clear, after a confirm),
 * 6 RND (shuffle), 7 REMOVE (the cursor row, no confirm), 8 EQ, 9 VOL +, 10 VOL -.  Moving a row is Shift + Up / Down
 * and the drag, both through queue_move. */
#define Q_BTNS JF_UI_QROW
static void queue_move(JfUi *ui, int delta, JfEvent *out, int *n, int max)
{
    int c = ui->lv.cursor, t = c + delta;
    if (c < 0 || t < 0 || c >= ui->app->queue_n || t >= ui->app->queue_n) return;
    int k = emit(out, n, max, JF_EV_QUEUE_MOVE);
    if (k < 0) return;
    out[k].i = c;
    out[k].j = t;
    lv_move_to(&ui->lv, t);
    ui->dirty = true;
}

/* PREV, PLAY/PAUSE, STOP, NEXT: the first four buttons of now playing and of its queue view, one action for both. */
static void transport_act(int i, JfEvent *out, int *n, int max)
{
    static const JfEventType t[4] = { JF_EV_PREV, JF_EV_PAUSE, JF_EV_STOP, JF_EV_NEXT };
    emit(out, n, max, t[i]);
}

static void eq_open(JfUi *ui, uint32_t now);

static void queue_row_act(JfUi *ui, Pointer *ptr, int i, uint32_t now, JfEvent *out, int *n, int max)
{
    if (i < 4) transport_act(i, out, n, max);
    else if (i == 4) do_save(ui, ptr, out, n, max);
    else if (i == 5) emit(out, n, max, JF_EV_QUEUE_CLEAR);
    else if (i == 6) {   /* the playing track is at 0 after a shuffle, and nothing playing leaves 0 first too: show it */
        emit(out, n, max, JF_EV_SHUFFLE);
        lv_move_to(&ui->lv, 0);
        ui->dirty = true;
    }
    else if (i == 7) {   /* the cursor row leaves the queue; with the last row gone the cursor steps up onto the new last one */
        int c = ui->lv.cursor;
        if (c < 0 || c >= ui->app->queue_n) return;
        int k = emit(out, n, max, JF_EV_QUEUE_REMOVE);
        if (k < 0) return;
        out[k].i = c;
        if (c >= ui->app->queue_n - 1 && c > 0) lv_move_to(&ui->lv, c - 1);
        ui->dirty = true;
    }
    else if (i == JF_UI_QROW_EQ) eq_open(ui, now);
    else emit(out, n, max, i == 9 ? JF_EV_VOL_UP : JF_EV_VOL_DOWN);
}

static void abar_act(int i, int row, JfEvent *out, int *n, int max);

/* The now-playing buttons: PREV, PLAY/PAUSE, STOP, NEXT, QUEUE, MIX and INSTANT MIX (of the playing track), EQ (opens the
 * equaliser panel, handled in eq_open), VOL +, VOL -. */
static void np_act(int i, JfEvent *out, int *n, int max)
{
    static const JfEventType t[JF_UI_NP] = { JF_EV_NONE, JF_EV_NONE, JF_EV_NONE, JF_EV_NONE, JF_EV_OPEN_QUEUE, JF_EV_NONE,
                                             JF_EV_NONE, JF_EV_NONE, JF_EV_VOL_UP, JF_EV_VOL_DOWN };
    if (i < 4) transport_act(i, out, n, max);
    else if (i == 5 || i == 6) abar_act(i - 2, -1, out, n, max);   /* MIX = abar 3, INSTANT MIX = abar 4 */
    else emit(out, n, max, t[i]);
}

/* The six buttons of the action bar, on list row `row` (-1 = the playing track, NOW PLAYING's MIX pair). */
static void abar_act(int i, int row, JfEvent *out, int *n, int max)
{
    static const JfEventType t[JF_UI_ABAR] = { JF_EV_ACTIVATE, JF_EV_PLAY_NEXT, JF_EV_ADD_QUEUE, JF_EV_MIX, JF_EV_INSTANT_MIX, JF_EV_PICK_OPEN };
    int k = emit(out, n, max, t[i]);
    if (k >= 0) out[k].i = row;
}

/* The playlist menu's button j: OPEN enters the playlist, DELETE asks first, CANCEL drops the menu; all on list row `row`. */
static void pmenu_act(const JfApp *app, int j, int row, JfEvent *out, int *n, int max)
{
    static const JfEventType t[3] = { JF_EV_ACTIVATE, JF_EV_PLAYLIST_DELETE, JF_EV_DISMISS };
    int k = emit(out, n, max, t[pmenu_kind(app, j)]);
    if (k >= 0 && pmenu_kind(app, j) != 2) out[k].i = row;
}

/* The equaliser panel: opened from NOW PLAYING (the EQ button), it owns the whole screen and every input
 * until DONE or CANCEL.  The loop applies what jf_ui_eq_take() hands back. */
static void eq_open(JfUi *ui, uint32_t now)
{
    int g[EQ_BANDS] = { 0 };
    if (ui->eq_gains_fn) ui->eq_gains_fn(g);
    AudioEnh en;
    memset(&en, 0, sizeof en);
    if (ui->enh_now_fn) ui->enh_now_fn(&en);
    eq_panel_open_enh(&ui->eq, g, &en, SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP, SCREEN_SAFE_RIGHT, SCREEN_SAFE_BOTTOM, now);
    ui->eq_up = true;
    ui->dirty = true;
    touch_guard_screen_changed(&ui->guard, now);
}

static void eq_input(JfUi *ui, const TouchState *ts, const InputState *in, uint32_t now)
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
        touch_guard_screen_changed(&ui->guard, now);   /* the finger that closed it must not press what is underneath */
    }
    if (eq_panel_take_dirty(&ui->eq) || !ui->eq_up) ui->dirty = true;
}

bool jf_ui_eq_take(JfUi *ui, int gains[EQ_BANDS], bool *save)
{
    if (!ui->eq_apply) return false;
    memcpy(gains, ui->eq_out, sizeof ui->eq_out);
    *save = ui->eq_save;
    ui->eq_apply = ui->eq_save = false;
    return true;
}

static void np_press(JfUi *ui, int i, uint32_t now, JfEvent *out, int *n, int max)
{
    if (i == JF_UI_NP_EQ) eq_open(ui, now);
    else np_act(i, out, n, max);
}

static void overlay_input(JfUi *ui, const TouchState *ts, const InputState *in, uint32_t now,
                          JfEvent *out, int *n, int max)
{
    const JfApp *app = ui->app;
    if (!ui->dlg_up || ui->dlg_buttons != app->overlay_buttons) {
        int cnt = 0;
        if (app->overlay_buttons & JF_BTN_RETRY) cnt++;
        cnt++;   /* CLOSE is always there */
        modal_dialog_init(&ui->dlg, JF_APP_TITLE, app->overlay_text, cnt);
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

/* ── The root header picker ────────────────────────────────────────────── */

/* The root's title is "LUDFIN · <CATEGORY>" followed by a drop-down arrow; the title and the arrow together are the
 * pressable header.  Sets the bar's title (the one place it is spelled) and returns the pressable rect, clamped to SAFE. */
static const char *root_title(JfUi *ui)
{
    snprintf(ui->head_title, sizeof ui->head_title, JF_APP_TITLE " \302\267 %s", jf_cat_name(ui->app->cat));
    return ui->head_title;
}

/* The pressable header for `title`, with room for the drop-down arrow when `arrow`; clamped to SAFE. */
static UiRect head_rect_of(JfUi *ui, const char *title, bool arrow)
{
    JfExtents e;
    jf_ui_extents_live(&e);
    ui->bar.title = title;
    ui->bar.title_extra = arrow ? TP_EXTRA : 0;
    top_bar_layout(&ui->bar);
    const TopBarLayout *L = &ui->bar.L;
    TpExt te = { e.safe_l, e.safe_t, e.safe_r, e.safe_b };
    return tp_head_rect(L->title_x, L->title_w, L->top, L->bottom, &te, arrow);
}

static UiRect head_rect(JfUi *ui) { return head_rect_of(ui, root_title(ui), true); }

/* The browse lists and the queue as a ui_zone stack, top to bottom: the bar, the list, the mini strip, the button row.  The
 * action bar is a UZ_TRAP zone in the place it is drawn: the playlist menu on the strip, the track bar in the button row. */
enum { BZ_BAR = 0, BZ_LIST, BZ_MINI, BZ_ROW, BZ_N };

static const JfEventType mini_ev[JF_UI_MINI] = { JF_EV_PAUSE, JF_EV_VOL_DOWN, JF_EV_VOL_UP };

static void browse_zones(JfUi *ui, UiZone z[UZ_ZONES])
{
    const JfApp *app = ui->app;
    memset(z, 0, UZ_ZONES * sizeof z[0]);
    if (ui->bar.has_back) zone_add(&z[BZ_BAR], top_bar_ring_rect(&ui->bar.L, TB_BACK), 0);
    if (app->screen == JF_SCR_ARTISTS) zone_add(&z[BZ_BAR], head_rect(ui), 2);   /* the header opens the category picker */
    if (ui->bar.has_exit) zone_add(&z[BZ_BAR], top_bar_ring_rect(&ui->bar.L, TB_EXIT), 1);
    z[BZ_LIST].lv = &ui->lv;
    if (pmenu_up(app)) {
        for (int j = 0; j < pmenu_n(app); j++) zone_add(&z[BZ_MINI], ui->L.mini_btn[pmenu_slot(app, j)], j);
    } else if (mini_player(app)) {
        for (int i = 0; i < JF_UI_MINI; i++) zone_add(&z[BZ_MINI], ui->L.mini_btn[i], i);
    }
    if (app->action_bar && !pmenu_up(app)) {
        for (int i = 0; i < JF_UI_ABAR; i++) zone_add(&z[BZ_ROW], ui->L.abar[i], i);
    } else if (app->screen == JF_SCR_QUEUE) {
        if (app->queue_n > 0) for (int i = 0; i < Q_BTNS; i++) zone_add(&z[BZ_ROW], ui->L.qrow[i], i);
    } else {
        const UiRect *rr = tracks4(app) ? ui->L.row4 : ui->L.row3;
        for (int i = 0; i < row_n(app); i++) if (row3_on(app, i)) zone_add(&z[BZ_ROW], rr[i], i);
    }
}

/* This screen's zones, for the ring: PLPICK and MIX are the three-zone stack, settings the bar over the list, now playing the
 * bar over its buttons, the rest the four. */
static int screen_zones(JfUi *ui, UiZone z[UZ_ZONES])
{
    const JfApp *app = ui->app;
    if (app->screen == JF_SCR_SETTINGS) { settings_zones(ui, z); return STACK_ROW; }
    if (app->screen == JF_SCR_NOWPLAYING) { np_zones(ui, z); return NPZ_N; }
    if (app->screen == JF_SCR_SIGNIN) { signin_zones(ui, z); return SIZ_N; }
    if (app->screen == JF_SCR_PLPICK || app->screen == JF_SCR_MIX) {
        stack_zones(ui, z, app->screen == JF_SCR_PLPICK ? jf_ui_plpick_add_shown(app) : !mix_locked(app));
        return STACK_N;
    }
    browse_zones(ui, z);
    return BZ_N;
}

static bool in_rect(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static TpExt pick_ext(void)
{
    JfExtents e;
    jf_ui_extents_live(&e);
    TpExt te = { e.safe_l, e.safe_t, e.safe_r, e.safe_b };
    return te;
}

static void pick_open(JfUi *ui)
{
    TpExt te = pick_ext();
    ui->pick.sel = (int)ui->app->cat;
    tp_open(&ui->pick, head_rect(ui), &te);
    ui->dirty = true;
}

/* The picker owns every input while it is up (common/top_pick.h); a pick sets the category. */
static void pick_input(JfUi *ui, const TouchState *ts, const InputState *in, uint32_t now, JfEvent *out, int *n, int max)
{
    TpEvent ev = tp_input(&ui->pick, ts, in, now);
    /* A frame only when something can have changed: an event, a finger on the panel, a pad key, or the list still
     * moving (tp_input reports that as TP_CHANGED).  An open, untouched picker costs what a static screen does. */
    if (ev != TP_NONE || ts->pressed || ts->released || ts->held ||
        bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN) || bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT) ||
        bp(in, BTN_ID_JUMP) || bp(in, BTN_ID_ACTION))
        ui->dirty = true;
    if (ev == TP_PICKED) {
        int k = emit(out, n, max, JF_EV_CATEGORY_SET);
        if (k >= 0) out[k].i = tp_take(&ui->pick);
    }
    if (ev == TP_PICKED || ev == TP_CLOSED)
        touch_guard_screen_changed(&ui->guard, now);   /* the finger that closed it must not press what is underneath */
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
    if (top_bar_info_due(&ui->bar, now) && top_bar_info_sample(&ui->bar)) {   /* the bar's CPU and clock, once a second */
        /* Only the bar's text changed: repaint just it.  While a dialog is up (EQ panel, error overlay, confirm, the
         * modal dialogs) the tick asks for nothing: a full frame under the dialog's backdrop blend takes 50-65 ms and
         * starves the audio feed, so the music breaks up once a second.  The sample is still taken, and the close path
         * sets dirty, so the bar shows fresh values once the dialog is gone.  The header picker's popup starts below
         * the bar's rule (top_pick.c tp_popup_rect), so it never covers the info rect: the bar patch is still right. */
        if (!(ui->eq_up || app->overlay || app->confirm != JF_CONFIRM_NONE || ui->dlg_up || ui->cdlg_up))
            ui->info_dirty = true;   /* no dialog up: the bar patch; under a dialog the bar waits for its close */
    }
    /* a queue can start or empty on an open screen: the list makes room for the strip, or takes it back */
    if (is_list_screen(app->screen) && mini_wanted(app) != ui->mini_on) {
        ui->mini_on = mini_wanted(app);
        ui->lv.rect = list_rect_for(ui, app->screen);
        lv_sync(&ui->lv);
        ui->dirty = true;
    }
    if (app->banner_seq != ui->seen_banner) { ui->seen_banner = app->banner_seq; ui->dirty = true; }
    if (app->tags_seq != ui->seen_tags) { ui->seen_tags = app->tags_seq; if (app->screen == JF_SCR_NOWPLAYING) ui->dirty = true; }
    if (app->fmt_seq != ui->seen_fmt) { ui->seen_fmt = app->fmt_seq; if (app->screen == JF_SCR_NOWPLAYING) ui->dirty = true; }

    if (app->overlay) {
        overlay_input(ui, ts, in, now, out, &n, max);
        return n;
    }
    ui->dlg_up = false;

    /* The INSTANT MIX question owns every input while it is up (OK / CANCEL; Esc, B and Select cancel). */
    if (app->confirm != JF_CONFIRM_NONE) {
        if (!ui->cdlg_up) {
            char msg[128], nm[64];
            ui->cdlg_skip = false;
            if (app->confirm == JF_CONFIRM_CLEAR_QUEUE) {
                snprintf(msg, sizeof msg, "CLEAR %d TRACKS?", app->queue_n);
                modal_dialog_init(&ui->cdlg, "QUEUE", msg, 2);
                modal_dialog_set_button(&ui->cdlg, 0, "YES", BTN_BG, COLOR_WHITE);
                modal_dialog_set_button(&ui->cdlg, 1, "NO", RGB(90, 40, 40), COLOR_WHITE);
            } else if (app->confirm == JF_CONFIRM_SIGN_OUT) {
                modal_dialog_init(&ui->cdlg, "SIGN OUT?", "THE QUEUE IS CLEARED", 2);
                modal_dialog_set_button(&ui->cdlg, 0, "SIGN OUT", BTN_BG, COLOR_WHITE);
                modal_dialog_set_button(&ui->cdlg, 1, "CANCEL", RGB(90, 40, 40), COLOR_WHITE);
            } else if (app->confirm == JF_CONFIRM_QUIT) {
                modal_dialog_init(&ui->cdlg, "EXIT?", "THE MUSIC STOPS", 2);
                modal_dialog_set_button(&ui->cdlg, 0, "EXIT", BTN_BG, COLOR_WHITE);
                modal_dialog_set_button(&ui->cdlg, 1, "CANCEL", RGB(90, 40, 40), COLOR_WHITE);
            } else if (app->confirm == JF_CONFIRM_ADD_DUP) {
                jf_ui_dup_title(app, msg, sizeof msg);
                ui->cdlg_skip = jf_dup_can_skip(app);   /* some but not all already in: SKIP EXISTING sits between */
                modal_dialog_init(&ui->cdlg, "ADD TO PLAYLIST", msg, ui->cdlg_skip ? 3 : 2);
                modal_dialog_set_button(&ui->cdlg, 0, "ADD ANYWAY", BTN_BG, COLOR_WHITE);
                if (ui->cdlg_skip) modal_dialog_set_button(&ui->cdlg, 1, "SKIP EXISTING", BTN_BG, COLOR_WHITE);
                modal_dialog_set_button(&ui->cdlg, ui->cdlg_skip ? 2 : 1, "CANCEL", RGB(90, 40, 40), COLOR_WHITE);
            } else if (app->confirm == JF_CONFIRM_DELETE_PLAYLIST) {
                jf_ui_delete_msg(app, msg, sizeof msg);
                modal_dialog_init(&ui->cdlg, "DELETE PLAYLIST", msg, 2);
                modal_dialog_set_button(&ui->cdlg, 0, "DELETE", BTN_BG, COLOR_WHITE);
                modal_dialog_set_button(&ui->cdlg, 1, "CANCEL", RGB(90, 40, 40), COLOR_WHITE);
            } else {
            lv_fit(nm, sizeof nm, app->confirm_seed.name, 8 * 6 * 2, 2);
            snprintf(msg, sizeof msg, "REPLACE QUEUE WITH INSTANT MIX OF %s?", nm);
            modal_dialog_init(&ui->cdlg, "INSTANT MIX", msg, 2);
            modal_dialog_set_button(&ui->cdlg, 0, "OK", BTN_BG, COLOR_WHITE);
            modal_dialog_set_button(&ui->cdlg, 1, "CANCEL", RGB(90, 40, 40), COLOR_WHITE);
            }
            modal_dialog_set_focus(&ui->cdlg, ui->cdlg_skip ? 2 : 1);
            modal_dialog_show(&ui->cdlg);
            ui->cdlg_up = true;
            ui->dirty = true;
        }
        int a = modal_dialog_input(&ui->cdlg, in, 1);
        if (a < 0) a = modal_dialog_update(&ui->cdlg, ts->x, ts->y, ts->pressed || ts->held, now);
        if (ts->pressed || ts->released || bp(in, BTN_ID_LEFT) || bp(in, BTN_ID_RIGHT) || bp(in, BTN_ID_UP) ||
            bp(in, BTN_ID_DOWN)) ui->dirty = true;
        if (a >= 0) {
            emit(out, &n, max, a == 0 ? JF_EV_CONFIRM : (a == 1 && ui->cdlg_skip) ? JF_EV_CONFIRM_SKIP : JF_EV_BACK);
            ui->cdlg_up = false;
            ui->dirty = true;
        }
        return n;
    }
    ui->cdlg_up = false;
    if (ui->eq_up && app->screen != JF_SCR_NOWPLAYING && app->screen != JF_SCR_QUEUE) ui->eq_up = false;
    if (ui->eq_up) { eq_input(ui, ts, in, now); return n; }
    if (ui->pick.up) {
        if (app->screen != JF_SCR_ARTISTS) tp_close(&ui->pick);
        else { pick_input(ui, ts, in, now, out, &n, max); return n; }
    }

    bool pad_nav = bp(in, BTN_ID_UP) || bp(in, BTN_ID_DOWN) || bp(in, BTN_ID_LEFT) ||
                   bp(in, BTN_ID_RIGHT) || bp(in, BTN_ID_JUMP) || bp(in, BTN_ID_ACTION) ||
                   bp(in, BTN_ID_NEXT) || bp(in, BTN_ID_PREV);
    if (ts->pressed) ui->zf.shown = false;
    if (pad_nav) ui->zf.shown = true;
    if (ts->pressed || ts->released || pad_nav) ui->dirty = true;
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

    if (sc == JF_SCR_QUEUE) {   /* the queue is now playing's other view: a tap on its title goes back to it */
        UiRect h = head_rect_of(ui, "QUEUE", false);
        if (ts->pressed && in_rect(h, ts->x, ts->y)) ui->head_press = true;
        if (ts->released) {
            bool hit = ui->head_press && in_rect(h, ts->x, ts->y);
            ui->head_press = false;
            if (hit) { emit(out, &n, max, JF_EV_BACK); return n; }
        }
    }
    if (sc == JF_SCR_ARTISTS && !app->action_bar) {
        /* the header (title and arrow) opens the category picker on a tap; the pad reaches it as a top bar item (Tab, then Enter) */
        TpExt te = pick_ext();
        ui->pick.sel = (int)app->cat;
        InputState nokeys;
        memset(&nokeys, 0, sizeof nokeys);
        if (tp_head_input(&ui->pick, head_rect(ui), &te, ts, &nokeys) == TP_OPENED) { ui->dirty = true; return n; }
    }

    if (sc == JF_SCR_SIGNIN) {
        bool can_sign = signin_can(ui);
        UiZone z[UZ_ZONES];
        signin_zones(ui, z);
        UzOut zo;
        uz_input(&ui->zf, z, SIZ_N, in, ts->pressed, &zo);
        if (zo.moved) ui->dirty = true;
        int pick = -1;
        for (int i = 0; i < JF_UI_FIELDS; i++) if (button_check_tap(&ui->field[i], ts, now)) pick = i;
        if (can_sign && button_check_tap(&ui->signin, ts, now)) pick = 3;
        if (button_check_tap(&ui->signexit, ts, now)) pick = 4;
        if (pick < 0 && zo.activate >= 0) pick = ui->zf.zone == SIZ_BAR ? 4 : zo.activate;   /* the bar's only button is EXIT */
        if (pick >= 0 && pick < JF_UI_FIELDS) edit_field(ui, ptr, pick);
        else if (pick == 3 && can_sign) {
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
        /* Three panes (Tab / LB-RB): the top bar, the seek bar (Left / Right seek 10 s from the drawn position) and the ten buttons
         * (PREV, PLAY/PAUSE, STOP, NEXT, QUEUE, MIX, I-MIX, EQ, VOL +, VOL -).  Enter / Space / A act on the focused item, Esc / B go back. */
        UiZone z[UZ_ZONES];
        np_zones(ui, z);
        UzOut zo;
        uz_input(&ui->zf, z, NPZ_N, in, ts->pressed, &zo);
        if (zo.moved) ui->dirty = true;
        if (zo.back) emit(out, &n, max, JF_EV_BACK);
        if (zo.left_right) {   /* the same absolute JF_EV_SEEK as the bar: the core clamps the top, a clamp here keeps the bottom at 0 */
            int target = ui->played_ms + zo.left_right * 10000;
            int k = emit(out, &n, max, JF_EV_SEEK);
            if (k >= 0) out[k].i = target < 0 ? 0 : target;
            ui->dirty = true;
        }
        if (zo.activate >= 0 && ui->zf.zone == NPZ_BAR) emit(out, &n, max, zo.activate == 0 ? JF_EV_BACK : JF_EV_QUIT);
        else if (zo.activate >= 0 && ui->zf.zone == NPZ_ROW) np_press(ui, zo.activate, now, out, &n, max);
        for (int i = 0; i < JF_UI_NP; i++)
            if (button_check_tap(&ui->np[i], ts, now)) np_press(ui, i, now, out, &n, max);
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

    if (sc == JF_SCR_PLPICK || sc == JF_SCR_MIX) {
        /* Two screens, one shape: the list over a button row, under the top bar.  PLPICK: row 0 is + NEW PLAYLIST (the
         * keyboard names it), the other rows only select; ADD (absent until something is chosen) and CLOSE.  MIX: the
         * tick rows; START (absent while the run is going and the rows are locked) and CANCEL, which the core turns
         * into "stop the run".  ui_zone makes one focus move per frame, so Down at the last row lands on the button
         * row and stays there. */
        bool pl = sc == JF_SCR_PLPICK;
        bool locked = !pl && mix_locked(app);
        bool first_on = pl ? jf_ui_plpick_add_shown(app) : !locked;
        int cnt = pl ? src_count(ui) : app->mix.opt_n;
        UiZone z[UZ_ZONES];
        stack_zones(ui, z, first_on);
        lv_sync(&ui->lv);
        mix_sync_marks(ui);
        UzOut zo;
        uz_input(&ui->zf, z, 3, in, ts->pressed, &zo);
        if (zo.moved) ui->dirty = true;
        if (zo.back) emit(out, &n, max, pl ? JF_EV_PICK_CLOSE : JF_EV_BACK);
        stack_lv_look(ui);
        LvEvent ev = lv_input(&ui->lv, ts, zo.list_in, now);
        if (ev != LV_EV_NONE) ui->dirty = true;
        bool pick = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE;
        int prow = ui->lv.cursor;
        if (zo.activate_row >= 0) { pick = true; prow = zo.activate_row; }
        if (pl && pick && prow == 0) do_pick_new(ui, ptr, out, &n, max);
        else if (pl && pick && prow > 0 && prow < cnt) {
            int k = emit(out, &n, max, JF_EV_PICK_SELECT);
            if (k >= 0) out[k].i = prow - 1;
        } else if (!pl && pick && !locked && prow >= 0 && prow < cnt) {
            int k = emit(out, &n, max, JF_EV_MIX_TOGGLE);
            if (k >= 0) out[k].i = prow;
        }
        JfEventType first_ev = pl ? JF_EV_PICK_ADD : JF_EV_MIX_START;
        JfEventType close_ev = pl ? JF_EV_PICK_CLOSE : JF_EV_BACK;
        if (zo.activate >= 0 && ui->zf.zone == STACK_BAR) emit(out, &n, max, zo.activate == 0 ? close_ev : JF_EV_QUIT);
        else if (zo.activate >= 0) emit(out, &n, max, zo.activate == 0 ? first_ev : close_ev);
        Button *b = pl ? ui->plb : ui->mixb;
        if (first_on && button_check_tap(&b[0], ts, now)) emit(out, &n, max, first_ev);
        if (button_check_tap(&b[1], ts, now)) emit(out, &n, max, close_ev);
        return n;
    }

    if (sc == JF_SCR_SETTINGS) {
        /* The rows are the list over the whole body (no button row): tap / ENTER / A act on a row, BACK leaves (the core
         * returns from the library page to the main page first), Up from the top row reaches the bar. */
        settings_sync(ui);
        int cnt = src_count(ui);
        const InputState *lin = NULL;
        UiZone z[UZ_ZONES];
        settings_zones(ui, z);
        lv_sync(&ui->lv);
        mix_sync_marks(ui);
        UzOut zo;
        uz_input(&ui->zf, z, STACK_ROW, in, ts->pressed, &zo);
        if (zo.moved) ui->dirty = true;
        if (zo.back) emit(out, &n, max, JF_EV_BACK);
        lin = zo.list_in;
        ui->lv.active = ui->zf.zone == STACK_LIST;
        LvEvent ev = lv_input(&ui->lv, ts, lin, now);
        if (ev != LV_EV_NONE) ui->dirty = true;
        bool pick = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE;
        int prow = ui->lv.cursor;
        if (zo.activate_row >= 0) { pick = true; prow = zo.activate_row; }
        if (pick && prow >= 0 && prow < cnt) {
            if (app->settings_page == JF_SETPAGE_LIBRARY) {
                int k = emit(out, &n, max, JF_EV_LIBRARY_SET);
                if (k >= 0) out[k].i = prow - 1;
            } else {
                int ids[JF_SET_COUNT];
                jf_settings_rows(app, ids);
                int k = emit(out, &n, max, JF_EV_SETTING_ACTIVATE);
                if (k >= 0) out[k].i = ids[prow];
            }
        } else if (zo.activate >= 0 && ui->zf.zone == STACK_BAR) {
            emit(out, &n, max, zo.activate == 0 ? JF_EV_BACK : JF_EV_QUIT);
        }
        return n;
    }

    /* list screens and the queue, as a ui_zone stack: bar / list / mini strip / button row.  One press makes one focus move.
     * The action bar is a UZ_TRAP zone (the track bar in the button row's place, the playlist menu on the strip's): Up / Down
     * out of it dismiss it.  A touch never moves the pad focus. */
    bool queue = sc == JF_SCR_QUEUE;
    UiZone z[UZ_ZONES];
    browse_zones(ui, z);
    int tz = pmenu_up(app) ? BZ_MINI : BZ_ROW;
    if (app->action_bar && !ui->abar_up) {
        /* The core raised the bar.  After a pad / key select focus goes onto its first button; after a touch it stays where it
         * was, except off the trap's own place, where the ids would otherwise be re-read as the bar's buttons unseen. */
        if (ui->zf.shown) uz_focus(&ui->zf, z, BZ_N, tz, 0);
        else if (ui->zf.zone == tz) uz_focus(&ui->zf, z, BZ_N, BZ_LIST, -1);
        ui->abar_tz = tz;
        ui->dirty = true;
    }
    if (!app->action_bar && ui->abar_up && ui->zf.zone == ui->abar_tz) { uz_focus(&ui->zf, z, BZ_N, BZ_LIST, -1); ui->dirty = true; }
    ui->abar_up = app->action_bar;
    if (pmenu_up(app)) {
        for (int j = 0; j < pmenu_n(app); j++)
            if (button_check_tap(&ui->pmenu[pmenu_slot(app, j)], ts, now)) pmenu_act(app, j, app->action_row, out, &n, max);
    } else if (app->action_bar) {
        for (int i = 0; i < JF_UI_ABAR; i++)
            if (button_check_tap(&ui->abar[i], ts, now)) abar_act(i, app->action_row, out, &n, max);
    }
    int count = src_count(ui);

    /* Shift + Up / Down on the queue's list pane moves the cursor row one place (Shift is BTN_ID_RUN, held) instead of walking the
     * cursor: the zone stack is shown the frame without that direction.  The one screen-specific key kept. */
    int qmove = 0;
    InputState kin;
    const InputState *zin = in;
    if (queue && ui->zf.zone == BZ_LIST && in->buttons[BTN_ID_RUN].held) {
        qmove = bp(in, BTN_ID_DOWN) - bp(in, BTN_ID_UP);
        if (qmove) {
            kin = *in;
            kin.buttons[BTN_ID_UP].pressed = kin.buttons[BTN_ID_DOWN].pressed = false;
            zin = &kin;
        }
    }
    UzOut zo;
    uz_input(&ui->zf, z, BZ_N, zin, ts->pressed, &zo);
    if (qmove) { ui->zf.shown = true; queue_move(ui, qmove, out, &n, max); }
    if (zo.moved || qmove) ui->dirty = true;
    if (zo.back) emit(out, &n, max, JF_EV_BACK);

    if (sc == JF_SCR_ARTISTS && app->cat != ui->last_cat) { ui->last_cat = app->cat; lv_reset(&ui->lv); ui->dirty = true; }

    const InputState *lin = qmove ? NULL : zo.list_in;
    stack_lv_look(ui);
    lv_sync(&ui->lv);
    ui->lv.reorder = queue && app->queue_n > 1;   /* hold a queue row and drag it to a new place */
    LvEvent ev = lv_input(&ui->lv, ts, lin, now);
    if (ev == LV_EV_REORDER) {
        int k = emit(out, &n, max, JF_EV_QUEUE_MOVE);
        if (k >= 0) { out[k].i = ui->lv.pick_from; out[k].j = ui->lv.pick_to; }
        lv_move_to(&ui->lv, ui->lv.pick_to);
    }
    if (ev != LV_EV_NONE) ui->dirty = true;
    /* A first touch on a row only selects it (a track or playlist row raises its bar; the queue's cursor just moves); a touch on the
     * selected row acts: the queue plays it, a browse row plays or opens it.  Enter / Space / A (out.activate_row) act on the cursor row:
     * the queue plays it, a browse row is selected (its bar opens) or, with its bar already up, played / opened. */
    JfEventType pe = JF_EV_NONE;
    int arow = ui->lv.cursor;
    if (queue) { if (ev == LV_EV_ACTIVATE) pe = JF_EV_QUEUE_JUMP; }
    else if (ev == LV_EV_ACTIVATE) pe = JF_EV_ACTIVATE;
    else if (ev == LV_EV_TAP) pe = JF_EV_SELECT;
    if (zo.activate_row >= 0) {
        arow = zo.activate_row;
        if (queue) pe = JF_EV_QUEUE_JUMP;
        else pe = app->action_bar && app->action_row == arow ? JF_EV_ACTIVATE : JF_EV_SELECT;
    }
    if (pe != JF_EV_NONE && arow >= 0 && arow < count) {
        int k = emit(out, &n, max, pe);
        if (k >= 0) out[k].i = arow;
    }

    if (!queue && app->list && ui->zf.zone == BZ_LIST && count > 0 && ui->lv.cursor >= count - 6 &&
        count < app->list->total && !app->list->full && !app->list->pending && !app->busy)
        emit(out, &n, max, JF_EV_NEED_MORE);

    if (zo.activate >= 0) {
        int a = zo.activate;
        if (ui->zf.zone == BZ_BAR) {
            if (a == 2) { pick_open(ui); return n; }
            emit(out, &n, max, a == 0 ? JF_EV_BACK : JF_EV_QUIT);
        } else if (ui->zf.zone == BZ_MINI) {
            if (pmenu_up(app)) pmenu_act(app, a, app->action_row, out, &n, max);
            else emit(out, &n, max, mini_ev[a]);
        } else if (app->action_bar && !pmenu_up(app)) abar_act(a, app->action_row, out, &n, max);
        else if (queue) queue_row_act(ui, ptr, a, now, out, &n, max);
        else row3_act(ui, ptr, a, out, &n, max);
    }
    if (queue && app->queue_n > 0) {
        Button *qb = ui->qrow;
        for (int i = 0; i < Q_BTNS; i++)
            if (button_check_tap(&qb[i], ts, now)) queue_row_act(ui, ptr, i, now, out, &n, max);
    } else if (!queue && (!app->action_bar || pmenu_up(app))) {
        Button *rb = tracks4(app) ? ui->row4 : ui->row3;
        for (int i = 0; i < row_n(app); i++)
            if (row3_on(app, i) && button_check_tap(&rb[i], ts, now)) row3_act(ui, ptr, i, out, &n, max);
    }
    if (mini_player(app)) {
        for (int i = 0; i < JF_UI_MINI; i++)
            if (button_check_tap(&ui->mini_b[i], ts, now)) emit(out, &n, max, mini_ev[i]);
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

/* The header count.  The artist folder's list holds albums and then tracks from other artists' albums, and the server's total
 * counts one fetch phase's items, so the folder counts its rows by type; the picker lists playlists; a search that inlined an
 * artist has more rows than the server's total, and the rows are then what the screen holds. */
bool jf_ui_head_count(const JfApp *app, char *buf, size_t cap)
{
    if (cap) buf[0] = '\0';
    const JfList *l = app->list;
    if (!l || l->total <= 0) return false;
    if (app->screen == JF_SCR_ALBUMS && app->ctx[JF_LIST_ALBUMS].sub == JF_SUB_PLAIN) {
        int a = 0, t = 0;
        for (int i = 0; i < l->n; i++) {
            if (l->items[i].type == JF_ITEM_ALBUM) a++;
            else if (l->items[i].type == JF_ITEM_TRACK) t++;
        }
        if (!a && !t) return false;
        int k = 0;
        if (a) k += snprintf(buf + k, cap - (size_t)k, "%d ALBUM%s", a, a == 1 ? "" : "S");
        if (t) snprintf(buf + k, cap - (size_t)k, "%s%d TRACK%s", a ? ", " : "", t, t == 1 ? "" : "S");
        return true;
    }
    if (app->screen == JF_SCR_PLPICK) {
        snprintf(buf, cap, "%d PLAYLIST%s", l->total, l->total == 1 ? "" : "S");
        return true;
    }
    int cnt = l->total;
    const char *what = app->screen == JF_SCR_ARTISTS ? jf_cat_name(app->cat) : app->screen == JF_SCR_SEARCH ? "RESULTS"
                     : app->screen == JF_SCR_ALBUMS ? "ALBUMS" : "TRACKS";
    if (app->screen == JF_SCR_SEARCH && l->n > cnt) cnt = l->n;
    snprintf(buf, cap, "%d %s", cnt, what);
    return true;
}

/* The delete question.  The dialog's message line holds 33 glyphs at scale 2 (400 px, then it drops to scale 1), so "DELETE "
 * and "?" leave 25 for the name; the title already says PLAYLIST. */
void jf_ui_delete_msg(const JfApp *app, char *buf, size_t cap)
{
    char nm[64];
    lv_fit(nm, sizeof nm, app->confirm_seed.name, 25 * 6 * 2, 2);
    snprintf(buf, cap, "DELETE %s?", nm);
}

static void draw_banner(JfUi *ui)
{
    const JfApp *app = ui->app;
    UiRect b = ui->L.banner;
    if (app->banner[0]) fb_draw_text(ui->fb, b.x + 4, b.y + 2, app->banner, RING, 2);
    else if (app->list && app->list->total > 0) {
        /* the server's count, so a ranked first page is not read as the whole answer (jf_ui_head_count says where rows replace it) */
        char c[48];
        if (jf_ui_head_count(app, c, sizeof c)) fb_draw_text(ui->fb, b.x + 4, b.y + 2, c, DIM, 2);
    }
    if (app->busy || app->pick_wait || app->dup_scan || app->add_pending)
        fb_draw_text(ui->fb, b.x + b.w - 8 - text_measure_width("LOADING", 2), b.y + 2, "LOADING", DIM, 2);
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
    if (signin_can(ui)) button_draw(ui->fb, &ui->signin);
    button_draw(ui->fb, &ui->signexit);
}

static void draw_row3(JfUi *ui)
{
    if (ui->app->action_bar && !pmenu_up(ui->app)) {   /* the track bar takes the button row's place; the playlist menu is on the strip */
        for (int i = 0; i < JF_UI_ABAR; i++) {
            button_draw(ui->fb, &ui->abar[i]);
        }
        return;
    }
    bool t4 = tracks4(ui->app);
    Button *rb = t4 ? ui->row4 : ui->row3;
    if (t4) {   /* the fourth button's label by screen; at scale 2 the long one would overrun its button */
        bool srch = ui->app->screen == JF_SCR_SEARCH;
        snprintf(ui->row4[3].text, sizeof ui->row4[3].text, "%s", srch ? "ADD ALL TO PLAYLIST" : "+ PLAYLIST");
        ui->row4[3].text_scale = srch ? 1 : (6 * 10 * 2 <= ui->L.row4[3].w - 12 ? 2 : 1);
    }
    if (at_root(ui->app)) snprintf(ui->row3[2].text, sizeof ui->row3[2].text, "SETTINGS");
    else snprintf(ui->row3[2].text, sizeof ui->row3[2].text, "PLAY ALL");
    for (int i = 0; i < row_n(ui->app); i++) {
        if (!row3_on(ui->app, i)) continue;
        button_draw(ui->fb, &rb[i]);
    }
}

/* The drawn glyph of transport button i (0..3) in rect r: PREV, PLAY / PAUSE (the bars while it plays), STOP, NEXT; shared
 * by now playing and its queue view. */
static void transport_icon(JfUi *ui, int i, UiRect r)
{
    static const IconKind ik[4] = { ICON_PREV, ICON_PLAY, ICON_STOP, ICON_NEXT };
    IconKind k = i == 1 && strcmp(mini_play_label(ui->app), "PAUSE") == 0 ? ICON_PAUSE : ik[i];
    icon_draw(ui->fb, k, r.x + (r.w - NP_ICON) / 2, r.y + (r.h - NP_ICON) / 2, NP_ICON, COLOR_WHITE);
}

/* One performer as it is shown: "name (role)", or the name alone. */
static void perf_text(const JfTags *tg, int k, char *out, size_t cap)
{
    if (tg->perf[k].role[0]) snprintf(out, cap, "%s (%s)", tg->perf[k].name, tg->perf[k].role);
    else snprintf(out, cap, "%s", tg->perf[k].name);
}

/* WITH: the performers, word-wrapped into at most `lines` lines of `w` px (the file-tag font).  A line breaks only between
 * performers; one wider than a line is cut to it.  The last allowed line takes everything that is left, with an ellipsis.
 * Returns the number of lines written. */
static int wrap_with(const JfTags *tg, int lines, int w, char out[][160])
{
    int k = 0, nl = 0;
    while (k < tg->perf_n && nl < lines) {
        const char *prefix = nl == 0 ? "WITH: " : "";
        if (nl == lines - 1) {
            char rest[800];
            size_t o = (size_t)snprintf(rest, sizeof rest, "%s", prefix);
            for (int j = k; j < tg->perf_n && o < sizeof rest - 1; j++) {
                char ent[160];
                perf_text(tg, j, ent, sizeof ent);
                o += (size_t)snprintf(rest + o, sizeof rest - o, "%s%s", j > k ? ", " : "", ent);
            }
            lv_fit(out[nl], 160, rest, w, TAG_FONT);
            return nl + 1;
        }
        char ln[160];
        snprintf(ln, sizeof ln, "%s", prefix);
        int taken = 0;
        for (; k < tg->perf_n; k++) {
            char ent[160], cand[160];
            perf_text(tg, k, ent, sizeof ent);
            snprintf(cand, sizeof cand, "%s%s%s", ln, taken ? ", " : "", ent);
            if (text_measure_width(cand, TAG_FONT) > w) break;
            snprintf(ln, sizeof ln, "%s", cand);
            taken++;
        }
        if (taken == 0) {                            /* one performer wider than the line: cut it */
            char ent[160], cand[160];
            perf_text(tg, k, ent, sizeof ent);
            snprintf(cand, sizeof cand, "%s%s", prefix, ent);
            lv_fit(ln, sizeof ln, cand, w, TAG_FONT);
            k++;
        }
        snprintf(out[nl], 160, "%s", ln);
        nl++;
    }
    return nl;
}

/* Does the span s[0..n) fit in w px at the file-tag font?  A span of 160 bytes or more never does. */
static bool span_fits(const char *s, size_t n, int w)
{
    char tmp[160];
    if (n >= sizeof tmp) return false;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    return text_measure_width(tmp, TAG_FONT) <= w;
}

/* A text word-wrapped into at most `lines` lines of `w` px (the file-tag font).  A line breaks only at a space; a word
 * wider than a line is cut to it.  The last allowed line takes everything that is left, with an ellipsis.  Returns the
 * number of lines written. */
static int wrap_words(const char *text, int lines, int w, char out[][160])
{
    const char *p = text;
    int nl = 0;
    while (*p == ' ') p++;
    while (*p && nl < lines) {
        if (nl == lines - 1) {
            lv_fit(out[nl], 160, p, w, TAG_FONT);
            return nl + 1;
        }
        const char *cut = NULL, *q = p;
        while (*q) {
            const char *we = q;
            while (*we && *we != ' ') we++;
            if (!span_fits(p, (size_t)(we - p), w)) break;
            cut = we;
            q = we;
            while (*q == ' ') q++;
        }
        if (cut) {
            size_t n = (size_t)(cut - p);
            memcpy(out[nl], p, n);
            out[nl][n] = '\0';
            p = cut;
        } else {                                     /* one word wider than a line: cut it */
            const char *we = p;
            while (*we && *we != ' ') we++;
            char word[160];
            size_t n = (size_t)(we - p) < sizeof word ? (size_t)(we - p) : sizeof word - 1;
            memcpy(word, p, n);
            word[n] = '\0';
            lv_fit(out[nl], 160, word, w, TAG_FONT);
            p = we;
        }
        nl++;
        while (*p == ' ') p++;
    }
    return nl;
}

/* The file-tag lines of NOW PLAYING for a band `w` px wide and `h` px high: CONDUCTOR (1 line), WORK (up to 2), WITH
 * (up to 3), ALBUM ARTIST (up to 2), then the year / genres line.  An empty field is skipped without a gap.  Lines that
 * would fall below the band are left out.  Writes into ln (JF_TAG_LINES_MAX rows) and returns the count. */
int jf_ui_tag_lines(const JfItem *it, const JfTags *tg, int w, int h, char ln[][160])
{
    int avail = h >= TAG_LINE1_H ? (h - TAG_LINE1_H) / TAG_PITCH + 1 : 0;
    int nl = 0;
    bool has_c = tg && tg->conductor[0], has_w = tg && tg->work[0], has_p = tg && tg->perf_n > 0;
    bool has_a = jf_tags_show_album_artist(it->artist, it->album_artist);

    if (has_c) snprintf(ln[nl++], 160, "CONDUCTOR: %s", tg->conductor);
    if (has_w) {
        char wt[160 + JF_TAGS_NAME_MAX];
        snprintf(wt, sizeof wt, "WORK: %s", tg->work);
        nl += wrap_words(wt, TAG_WORK_MAX, w, &ln[nl]);
    }
    if (has_p) nl += wrap_with(tg, TAG_WITH_MAX, w, &ln[nl]);
    if (has_a) {
        char at[160 + 64];
        snprintf(at, sizeof at, "ALBUM ARTIST: %s", it->album_artist);
        nl += wrap_words(at, TAG_AA_MAX, w, &ln[nl]);
    }

    /* the year / genres line: the item's own fields (not the file's) */
    char yg[160] = "", g[JF_GENRES_BYTES + 8];
    size_t o = 0;
    if (it->year > 0) o += (size_t)snprintf(yg + o, sizeof yg - o, "%d", it->year);
    if (it->genres[0] && o < sizeof yg - 1) {
        size_t gi = 0;
        for (const char *s = it->genres; *s && gi + 3 < sizeof g; s++) {
            if (*s == '\n') { if (s[1]) { g[gi++] = ','; g[gi++] = ' '; } }
            else g[gi++] = *s;
        }
        g[gi] = '\0';
        snprintf(yg + o, sizeof yg - o, "%s%s", o ? "   " : "", g);
    }
    if (yg[0]) {
        lv_fit(ln[nl], 160, yg, w, TAG_FONT);
        nl++;
    }

    return nl < avail ? nl : avail;
}

/* The file-tag lines of NOW PLAYING, drawn in the band ui->L.tags; see jf_ui_tag_lines. */
static void draw_tags(JfUi *ui, const JfItem *it, const JfTags *tg)
{
    Framebuffer *fb = ui->fb;
    UiRect r = ui->L.tags;
    char ln[TAG_ROWS_MAX][160];
    char buf[160];
    int nl = jf_ui_tag_lines(it, tg, r.w, r.h, ln);
    for (int k = 0; k < nl; k++) {
        lv_fit(buf, sizeof buf, ln[k], r.w, TAG_FONT);
        fb_draw_text(fb, r.x, r.y + k * TAG_PITCH, buf, DIM, TAG_FONT);
    }
}

static void draw_nowplaying(JfUi *ui)

{
    const JfApp *app = ui->app;
    Framebuffer *fb = ui->fb;
    const JfItem *it = jf_now(app);
    UiRect in = ui->L.info;
    char buf[128];
    int tx = in.x + 4;                       /* text column: right of the cover when there is one */
    if (it && it->album_id[0]) {
        jf_art_request(it->album_id, it->image_tag);   /* cheap; the fetch runs in jf_art_poll */
        const LvIcon *art = jf_art_get(it->album_id, it->image_tag);
        if (art && art->pixels && art->w == JF_UI_ART && art->h == JF_UI_ART)
            fb_blit_sprite(fb, art->pixels, art->w, 0, 0, in.x, in.y, art->w, art->h, 0x00000000u);
        else {                                         /* loading or none: a quiet box */
            fb_fill_rect(fb, in.x, in.y, JF_UI_ART, JF_UI_ART, PANEL);
            fb_draw_rect(fb, in.x, in.y, JF_UI_ART, JF_UI_ART, DIM);
        }
        tx = in.x + JF_UI_ART + 12;
    }
    int tw = in.x + in.w - tx - 4;
    if (it) {
        lv_fit(buf, sizeof buf, it->name, tw, 3);
        fb_draw_text(fb, tx, in.y, buf, COLOR_WHITE, 3);
        lv_fit(buf, sizeof buf, it->artist, tw, 2);
        fb_draw_text(fb, tx, in.y + 40, buf, DIM, 2);
        lv_fit(buf, sizeof buf, it->album, tw, 2);
        fb_draw_text(fb, tx, in.y + 66, buf, DIM, 2);
    }
    const char *state = app->play_state == JF_PLAY_FETCHING ? "LOADING..." : app->play_state == JF_PLAY_PAUSED ? "PAUSED"
                      : app->play_state == JF_PLAY_STOPPED ? "STOPPED"
                      : app->play_state == JF_PLAY_END ? "END OF QUEUE" : "";
    const JfTags *tg = jf_now_tags(app);
    if (it && (tg || app->login.tags_on)) draw_tags(ui, it, tg);
    if (state[0]) fb_draw_text(fb, tx, in.y + 94, state, RING, 2);
    if (it) {   /* the source file's format, in the strip under the state line; the container alone until the item read arrives */
        char src[128];
        jf_format_text(src, sizeof src, it->container, jf_now_format(app));
        if (src[0]) {
            lv_fit(buf, sizeof buf, src, tw, TAG_FONT);
            fb_draw_text(fb, tx, ui->L.source.y, buf, DIM, TAG_FONT);
        }
    }

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
    else snprintf(buf, sizeof buf, "VOLUME %d/%d", app->volume_step, AUDIO_VOLUME_MAX);
    fb_draw_text(fb, p.x + p.w - text_measure_width(buf, 2), p.y + p.h + 8, buf, COLOR_WHITE, 2);

    for (int i = 0; i < JF_UI_NP; i++) {
        button_draw(fb, &ui->np[i]);
        if (i < 4) transport_icon(ui, i, ui->L.np[i]);   /* the transport carries a drawn glyph instead of text */
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

/* The playlist menu on the strip: "<NAME>  <N> TRACKS" (the name only when the server gave no count; the name is what is cut
 * to fit, never the count) and its buttons, the focused one ringed. */
static void draw_pmenu(JfUi *ui)
{
    const JfApp *app = ui->app;
    const JfItem *it = &app->list->items[app->action_row];
    UiRect m = ui->L.mini_title;
    fb_fill_rect(ui->fb, m.x, m.y, m.w, m.h, PANEL);
    char sfx[24] = "", nm[96], t[128];
    if (it->count > 0) snprintf(sfx, sizeof sfx, "  %d TRACKS", it->count);
    lv_fit(nm, sizeof nm, it->name, m.w - 12 - text_measure_width(sfx, 2), 2);
    snprintf(t, sizeof t, "%s%s", nm, sfx);
    fb_draw_text(ui->fb, m.x + 6, m.y + (m.h - 14) / 2, t, COLOR_WHITE, 2);
    static const char *label[3] = { "OPEN", "DELETE", "CANCEL" };
    for (int j = 0; j < pmenu_n(app); j++) {
        Button *b = &ui->pmenu[pmenu_slot(app, j)];
        snprintf(b->text, sizeof b->text, "%s", label[pmenu_kind(app, j)]);
        button_draw(ui->fb, b);
    }
}

static void draw_strip(JfUi *ui)
{
    if (pmenu_up(ui->app)) draw_pmenu(ui);
    else draw_mini(ui);
}

void jf_ui_draw(JfUi *ui)
{
    const JfApp *app = ui->app;
    Framebuffer *fb = ui->fb;
    fb_clear(fb, BG);
    if (ui->eq_up) {
        eq_panel_draw(&ui->eq, fb);
        if (app->overlay && ui->dlg_up) modal_dialog_draw(&ui->dlg, fb);
        return;
    }

    const char *title = JF_APP_TITLE;
    switch (app->screen) {
    case JF_SCR_ALBUMS: case JF_SCR_TRACKS: case JF_SCR_SEARCH: case JF_SCR_MIX:
        if (app->crumb[0]) title = app->crumb;
        break;
    case JF_SCR_ARTISTS: title = root_title(ui); break;
    case JF_SCR_NOWPLAYING: title = "NOW PLAYING"; break;
    case JF_SCR_PLPICK:
        snprintf(ui->head_title, sizeof ui->head_title, "ADD %d TRACK%s TO PLAYLIST", app->pick_n, app->pick_n == 1 ? "" : "S");
        title = ui->head_title;
        break;
    case JF_SCR_QUEUE: title = "QUEUE"; break;
    case JF_SCR_SETTINGS: title = app->settings_page == JF_SETPAGE_LIBRARY ? "LIBRARY" : "SETTINGS"; break;
    default: break;
    }
    ui->bar.title = title;
    ui->bar.title_extra = app->screen == JF_SCR_ARTISTS ? TP_EXTRA : 0;
    top_bar_draw(fb, &ui->bar);
    if (app->screen == JF_SCR_ARTISTS)   /* the drop-down arrow after the title: the header opens the picker */
        tp_draw_arrow(fb, ui->bar.L.title_x, ui->bar.L.title_w, ui->bar.L.top, ui->bar.L.h);
    if (ui->zf.shown && ui->zf.zone == STACK_BAR && !app->overlay) {
        int bf = ui->zf.id;
        if (app->screen == JF_SCR_ARTISTS && bf == 2) ring(ui, head_rect(ui));
        else top_bar_draw_focus(fb, &ui->bar.L, bf == 0 ? TB_BACK : TB_EXIT);
    }

    draw_banner(ui);
    switch (app->screen) {
    case JF_SCR_SIGNIN:
        draw_signin(ui);
        break;
    case JF_SCR_NOWPLAYING:
        draw_nowplaying(ui);
        break;
    case JF_SCR_ARTISTS:
        if (mini_wanted(app)) draw_strip(ui);
        stack_lv_look(ui);
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, app->busy ? "" : "NOTHING HERE");
        draw_row3(ui);
        break;
    case JF_SCR_QUEUE:
        stack_lv_look(ui);
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, "QUEUE IS EMPTY");
        if (app->queue_n > 0) {
            Button *qb = ui->qrow;
            for (int i = 0; i < Q_BTNS; i++) {
                button_draw(fb, &qb[i]);
                if (i < 4) transport_icon(ui, i, ui->L.qrow[i]);
            }
        }
        break;
    case JF_SCR_SETTINGS:
        settings_sync(ui);
        ui->lv.active = ui->zf.zone == STACK_LIST;
        lv_sync(&ui->lv);
        mix_sync_marks(ui);
        lv_draw(fb, &ui->lv, "NOTHING HERE");
        break;
    case JF_SCR_PLPICK:
    case JF_SCR_MIX: {
        bool pl = app->screen == JF_SCR_PLPICK;
        bool first_on = pl ? jf_ui_plpick_add_shown(app) : !mix_locked(app);
        stack_lv_look(ui);
        lv_sync(&ui->lv);
        mix_sync_marks(ui);
        lv_draw(fb, &ui->lv, pl ? "" : "NOTHING TO TICK");
        Button *b = pl ? ui->plb : ui->mixb;
        for (int i = first_on ? 0 : 1; i < JF_UI_MIXBTN; i++) button_draw(fb, &b[i]);
        break;
    }
    default:
        if (mini_wanted(app)) draw_strip(ui);
        stack_lv_look(ui);
        lv_sync(&ui->lv);
        lv_draw(fb, &ui->lv, app->busy ? "" : "NOTHING HERE");
        draw_row3(ui);
        break;
    }
    if (ui->zf.zone != BZ_BAR) {   /* the focused button of the zone stack (the bar's focus is drawn above) */
        UiZone z[UZ_ZONES];
        UiRect rr;
        int nz = screen_zones(ui, z);
        if (uz_ring(&ui->zf, z, nz, &rr)) ring(ui, rr);
    }
    if (app->screen == JF_SCR_ARTISTS) tp_draw(fb, &ui->pick);
    if (app->confirm != JF_CONFIRM_NONE && ui->cdlg_up) modal_dialog_draw(&ui->cdlg, fb);
    if (app->overlay && ui->dlg_up) modal_dialog_draw(&ui->dlg, fb);
}

const AudioEnh *jf_ui_enh_out(const JfUi *ui) { return &ui->enh_out; }
