/* eq_panel.c — the equaliser panel widget; see eq_panel.h.  Integer layout math only. */
#include "eq_panel.h"
#include <stdio.h>
#include <string.h>

#define GAP        6
#define TITLE_H    28
#define BTN_H      44
#define PRESET_H   40
#define LABEL_H    22          /* the dB readout above, the Hz label below, each column */
#define PANEL_BG   RGB(16, 18, 26)
#define BTN_BG     RGB(40, 64, 104)
#define BTN_HL     RGB(70, 104, 160)
#define RAIL       RGB(70, 80, 100)
#define FILL       RGB(70, 104, 160)
#define DIMTXT     RGB(140, 150, 170)

static UiRect rect_of(int x, int y, int w, int h) { UiRect r = { x, y, w, h }; return r; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void eq_panel_layout(EqPanelLayout *L, int sl, int st, int sr, int sb)
{
    memset(L, 0, sizeof *L);
    int sw = sr - sl;
    L->title = rect_of(sl, st, sw, TITLE_H);
    int by = sb - BTN_H;
    int bw = (sw - GAP) / 2;
    L->cancel = rect_of(sl, by, bw, BTN_H);
    L->done = rect_of(sl + bw + GAP, by, sw - bw - GAP, BTN_H);
    int py = by - GAP - PRESET_H;
    int pw = (sw - (EQP_PRESETS - 1) * GAP) / EQP_PRESETS;
    for (int i = 0; i < EQP_PRESETS; i++)
        L->preset[i] = rect_of(sl + i * (pw + GAP), py, pw, PRESET_H);
    int top = st + TITLE_H + 4;
    int bot = py - GAP;
    int cw = sw / EQ_BANDS;
    for (int b = 0; b < EQ_BANDS; b++) {
        L->col[b] = rect_of(sl + b * cw, top, cw, bot - top);
        int tw = cw < 40 ? cw : 40;
        L->track[b] = rect_of(L->col[b].x + (cw - tw) / 2, top + LABEL_H + 4, tw, bot - top - 2 * LABEL_H - 8);
    }
}

int eq_panel_db_at(const UiRect *t, int y)
{
    int range = t->h - 1;
    if (range < 1) return 0;
    int pos = clampi(y - t->y, 0, range);
    return EQ_MAX_DB - (2 * EQ_MAX_DB * pos + range / 2) / range;
}

int eq_panel_y_of_db(const UiRect *t, int db)
{
    int range = t->h - 1;
    db = clampi(db, -EQ_MAX_DB, EQ_MAX_DB);
    return t->y + ((EQ_MAX_DB - db) * range + EQ_MAX_DB) / (2 * EQ_MAX_DB);
}

int eq_panel_band_at(const EqPanelLayout *L, int x, int y)
{
    if (y < L->col[0].y || y >= L->col[0].y + L->col[0].h) return -1;
    int best = 0, bd = 1 << 30;
    for (int b = 0; b < EQ_BANDS; b++) {
        int c = L->col[b].x + L->col[b].w / 2;
        int d = x > c ? x - c : c - x;
        if (d < bd) { bd = d; best = b; }
    }
    return best;
}

int eq_panel_preset(const EqPanel *p) { return eq_preset_match(p->gains); }

static bool same(const int *a, const int *b) { return memcmp(a, b, EQ_BANDS * sizeof(int)) == 0; }

EqPanelEvent eq_panel_key(EqPanel *p, EqPanelKey k)
{
    p->dirty = true;
    if (k == EQK_BACK) { memcpy(p->gains, p->orig, sizeof p->gains); return EQP_CANCEL; }
    if (k == EQK_NEXT) { p->zone = (EqPanelZone)((p->zone + 1) % EQP_ZONES); return EQP_NONE; }
    if (k == EQK_PREV) { p->zone = (EqPanelZone)((p->zone + EQP_ZONES - 1) % EQP_ZONES); return EQP_NONE; }
    switch (p->zone) {
    case EQP_ZONE_SLIDERS:
        if (k == EQK_LEFT)  p->band = clampi(p->band - 1, 0, EQ_BANDS - 1);
        if (k == EQK_RIGHT) p->band = clampi(p->band + 1, 0, EQ_BANDS - 1);
        if (k == EQK_ACT)   p->zone = EQP_ZONE_PRESETS;
        if (k == EQK_UP || k == EQK_DOWN) {
            int v = clampi(p->gains[p->band] + (k == EQK_UP ? 1 : -1), -EQ_MAX_DB, EQ_MAX_DB);
            if (v != p->gains[p->band]) { p->gains[p->band] = v; return EQP_CHANGED; }
        }
        break;
    case EQP_ZONE_PRESETS:
        if (k == EQK_LEFT)  p->preset_focus = clampi(p->preset_focus - 1, 0, EQP_PRESETS - 1);
        if (k == EQK_RIGHT) p->preset_focus = clampi(p->preset_focus + 1, 0, EQP_PRESETS - 1);
        if (k == EQK_UP)    p->zone = EQP_ZONE_SLIDERS;
        if (k == EQK_DOWN)  p->zone = EQP_ZONE_BUTTONS;
        if (k == EQK_ACT && p->preset_focus < eq_preset_count()) {
            const int *g = eq_preset_gains(p->preset_focus);
            bool ch = !same(p->gains, g);
            memcpy(p->gains, g, sizeof p->gains);
            return ch ? EQP_CHANGED : EQP_NONE;
        }
        break;
    case EQP_ZONE_BUTTONS:
    default:
        if (k == EQK_LEFT)  p->btn_focus = 0;
        if (k == EQK_RIGHT) p->btn_focus = 1;
        if (k == EQK_UP)    p->zone = EQP_ZONE_PRESETS;
        if (k == EQK_ACT) {
            if (p->btn_focus == 1) return EQP_DONE;
            memcpy(p->gains, p->orig, sizeof p->gains);
            return EQP_CANCEL;
        }
        break;
    }
    return EQP_NONE;
}

void eq_panel_db_text(char *buf, int cap, int db)
{
    if (db > 0) snprintf(buf, (size_t)cap, "+%d", db);
    else snprintf(buf, (size_t)cap, "%d", db);
}

bool eq_panel_set_from_y(EqPanel *p, int band, int y)
{
    if (band < 0 || band >= EQ_BANDS) return false;
    int v = eq_panel_db_at(&p->L.track[band], y);
    if (v == p->gains[band]) return false;
    p->gains[band] = v;
    return true;
}

void eq_panel_config_value(const int gains[EQ_BANDS], char *buf, int cap)
{
    int m = eq_preset_match(gains);
    if (m >= 0) snprintf(buf, (size_t)cap, "%s", eq_preset_name(m));
    else eq_format(gains, buf, cap);
}

static void style(Button *b, UiRect r, const char *text)
{
    int scale = text_measure_width(text, 2) + 12 <= r.w ? 2 : 1;
    button_init_full(b, r.x, r.y, r.w, r.h, text, BTN_BG, COLOR_WHITE, BTN_HL, scale);
    b->max_text_width = r.w - 8;
}

void eq_panel_open(EqPanel *p, const int gains[EQ_BANDS], int sl, int st, int sr, int sb, uint32_t now)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < EQ_BANDS; i++) p->gains[i] = clampi(gains[i], -EQ_MAX_DB, EQ_MAX_DB);
    memcpy(p->orig, p->gains, sizeof p->orig);
    p->drag = -1;
    eq_panel_layout(&p->L, sl, st, sr, sb);
    int m = eq_preset_match(p->gains);
    p->preset_focus = m >= 0 ? m : 0;
    for (int i = 0; i < EQP_PRESETS; i++)
        style(&p->preset_b[i], p->L.preset[i], i < eq_preset_count() ? eq_preset_name(i) : "");
    style(&p->cancel_b, p->L.cancel, "CANCEL");
    style(&p->done_b, p->L.done, "DONE");
    p->btn_focus = 1;
    touch_guard_init(&p->guard);
    touch_guard_screen_changed(&p->guard, now);   /* the tap that opened it must not reach it */
    p->dirty = true;
}

const int *eq_panel_gains(const EqPanel *p) { return p->gains; }

bool eq_panel_take_dirty(EqPanel *p)
{
    bool d = p->dirty || button_take_dirty();
    p->dirty = false;
    return d;
}

static bool bp(const InputState *in, int id) { return in && in->buttons[id].pressed; }

EqPanelEvent eq_panel_input(EqPanel *p, const TouchState *ts, const InputState *in, uint32_t now)
{
    TouchState gated;
    if (!ts || !touch_guard_pass(&p->guard, ts->pressed || ts->held, now)) {
        memset(&gated, 0, sizeof gated);
        ts = &gated;
    }
    EqPanelEvent ev = EQP_NONE;
    static const struct { int id; EqPanelKey k; } map[] = {
        { BTN_ID_UP, EQK_UP }, { BTN_ID_DOWN, EQK_DOWN }, { BTN_ID_LEFT, EQK_LEFT }, { BTN_ID_RIGHT, EQK_RIGHT },
        { BTN_ID_ACTION, EQK_ACT }, { BTN_ID_JUMP, EQK_ACT }, { BTN_ID_RUN, EQK_NEXT }, { BTN_ID_PGDN, EQK_NEXT },
        { BTN_ID_PGUP, EQK_PREV }, { BTN_ID_BACK, EQK_BACK }, { BTN_ID_PAUSE, EQK_BACK },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (bp(in, map[i].id)) {
            EqPanelEvent e = eq_panel_key(p, map[i].k);
            if (e == EQP_DONE || e == EQP_CANCEL) return e;
            if (e != EQP_NONE) ev = e;
        }

    if (ts->pressed) {
        int b = eq_panel_band_at(&p->L, ts->x, ts->y);
        if (b >= 0) { p->drag = b; p->band = b; p->zone = EQP_ZONE_SLIDERS; }
    }
    if (p->drag >= 0 && (ts->pressed || ts->held)) {
        if (eq_panel_set_from_y(p, p->drag, ts->y)) ev = EQP_CHANGED;
        p->dirty = true;
    }
    if (ts->released || (!ts->held && !ts->pressed)) {
        if (p->drag >= 0) p->dirty = true;
        p->drag = -1;
    }
    for (int i = 0; i < EQP_PRESETS && i < eq_preset_count(); i++)
        if (button_check_tap(&p->preset_b[i], ts, now)) {
            p->preset_focus = i; p->zone = EQP_ZONE_PRESETS;
            const int *g = eq_preset_gains(i);
            if (!same(p->gains, g)) { memcpy(p->gains, g, sizeof p->gains); ev = EQP_CHANGED; }
            p->dirty = true;
        }
    if (button_check_tap(&p->cancel_b, ts, now)) {
        memcpy(p->gains, p->orig, sizeof p->gains);
        return EQP_CANCEL;
    }
    if (button_check_tap(&p->done_b, ts, now)) return EQP_DONE;
    if (ts->pressed || ts->released) p->dirty = true;
    return ev;
}

static void ring(Framebuffer *fb, UiRect r)
{
    fb_draw_rect(fb, r.x - 2, r.y - 2, r.w + 4, r.h + 4, COLOR_YELLOW);
    fb_draw_rect(fb, r.x - 3, r.y - 3, r.w + 6, r.h + 6, COLOR_YELLOW);
}

void eq_panel_draw(EqPanel *p, Framebuffer *fb)
{
    const EqPanelLayout *L = &p->L;
    fb_clear(fb, PANEL_BG);
    text_draw_centered(fb, L->title.x + L->title.w / 2, L->title.y + L->title.h / 2, "EQUALISER", COLOR_WHITE, 3);
    static const char *hz[EQ_BANDS] = { "60", "250", "1K", "4K", "12K" };
    for (int b = 0; b < EQ_BANDS; b++) {
        const UiRect *t = &L->track[b];
        int cx = t->x + t->w / 2;
        bool sel = p->zone == EQP_ZONE_SLIDERS && p->band == b;
        fb_fill_rect(fb, cx - 3, t->y, 6, t->h, RAIL);
        int y0 = eq_panel_y_of_db(t, 0), yv = eq_panel_y_of_db(t, p->gains[b]);
        fb_fill_rect(fb, cx - 3, yv < y0 ? yv : y0, 6, (yv < y0 ? y0 - yv : yv - y0) + 1, FILL);
        fb_fill_rect(fb, t->x, y0, t->w, 1, DIMTXT);               /* the 0 dB line */
        uint32_t kc = sel ? COLOR_YELLOW : COLOR_WHITE;
        fb_fill_rect(fb, t->x, yv - 6, t->w, 13, kc);
        char v[8];
        eq_panel_db_text(v, sizeof v, p->gains[b]);
        text_draw_centered(fb, cx, L->col[b].y + LABEL_H / 2 + 2, v, sel ? COLOR_YELLOW : COLOR_WHITE, 2);
        text_draw_centered(fb, cx, t->y + t->h + 4 + LABEL_H / 2, hz[b], sel ? COLOR_YELLOW : DIMTXT, 2);
        if (sel) ring(fb, rect_of(L->col[b].x + 2, L->col[b].y, L->col[b].w - 4, L->col[b].h - 2));
    }
    int m = eq_panel_preset(p);
    for (int i = 0; i < EQP_PRESETS; i++) {
        if (i == m) button_set_colors(&p->preset_b[i], COLOR_YELLOW, COLOR_BLACK, COLOR_YELLOW);
        else button_set_colors(&p->preset_b[i], BTN_BG, COLOR_WHITE, BTN_HL);
        button_draw(fb, &p->preset_b[i]);
        if (p->zone == EQP_ZONE_PRESETS && p->preset_focus == i) ring(fb, L->preset[i]);
    }
    button_draw(fb, &p->cancel_b);
    button_draw(fb, &p->done_b);
    if (p->zone == EQP_ZONE_BUTTONS) ring(fb, p->btn_focus == 1 ? L->done : L->cancel);
}

int eq_panel_store(Config *cfg, const int gains[EQ_BANDS])
{
    char v[CONFIG_VAL_LEN];
    eq_panel_config_value(gains, v, sizeof v);
    config_set(cfg, EQ_CONFIG_KEY, v);
    return config_save(cfg);
}

int eq_panel_store_file(const char *path, const int gains[EQ_BANDS])
{
    Config cfg;
    config_init_path(&cfg, path);
    config_load(&cfg);              /* -1 = no file yet: start from empty */
    return eq_panel_store(&cfg, gains);
}
