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
    int bw = (sw - 2 * GAP) / 3;
    L->cancel = rect_of(sl, by, bw, BTN_H);
    L->page = rect_of(sl + bw + GAP, by, bw, BTN_H);
    L->done = rect_of(sl + 2 * (bw + GAP), by, sw - 2 * (bw + GAP), BTN_H);
    int py = by - GAP - PRESET_H;
    int pw = (sw - (EQP_PRESETS - 1) * GAP) / EQP_PRESETS;
    for (int i = 0; i < EQP_PRESETS; i++)
        L->preset[i] = rect_of(sl + i * (pw + GAP), py, pw, PRESET_H);
    L->toggle = rect_of(sl, py, sw, PRESET_H);
    int top = st + TITLE_H + 4;
    int bot = py - GAP;
    int cw = sw / EQ_BANDS;
    for (int b = 0; b < EQ_BANDS; b++) {
        L->col[b] = rect_of(sl + b * cw, top, cw, bot - top);
        int tw = cw < 40 ? cw : 40;
        L->track[b] = rect_of(L->col[b].x + (cw - tw) / 2, top + LABEL_H + 4, tw, bot - top - 2 * LABEL_H - 8);
    }
    int etop = top + LABEL_H + 2;           /* the part headers sit above the enhancer columns */
    int ecw = sw / EQP_ENH_SLIDERS;
    for (int i = 0; i < EQP_ENH_SLIDERS; i++) {
        L->ecol[i] = rect_of(sl + i * ecw, etop, ecw, bot - etop);
        int tw = ecw < 40 ? ecw : 40;
        L->etrack[i] = rect_of(L->ecol[i].x + (ecw - tw) / 2, etop + LABEL_H + 4, tw, bot - etop - 2 * LABEL_H - 8);
    }
    for (int g = 0; g < EQP_ENH_PARTS; g++)
        L->group[g] = rect_of(sl + 2 * g * ecw, top, 2 * ecw, LABEL_H);
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

static int *enh_slot(AudioEnh *e, int i)
{
    switch (i) {
    case 0: return &e->hb_amt;
    case 1: return &e->hb_rng;
    case 2: return &e->db_amt;
    case 3: return &e->db_rng;
    case 4: return &e->ht_amt;
    case 5: return &e->ht_rng;
    default: return NULL;
    }
}

int eq_panel_enh_get(const AudioEnh *e, int i)
{
    int *s = enh_slot((AudioEnh *)e, i);
    return s ? *s : 0;
}

bool eq_panel_enh_set(AudioEnh *e, int i, int v)
{
    int *s = enh_slot(e, i);
    if (!s) return false;
    v = clampi(v, 0, EQP_ENH_MAX);
    if (v == *s) return false;
    *s = v;
    return true;
}

int eq_panel_enh_value_at(const UiRect *t, int y)
{
    int range = t->h - 1;
    if (range < 1) return 0;
    int pos = clampi(y - t->y, 0, range);
    return EQP_ENH_MAX - (EQP_ENH_MAX * pos + range / 2) / range;
}

int eq_panel_enh_y_of_value(const UiRect *t, int v)
{
    int range = t->h - 1;
    v = clampi(v, 0, EQP_ENH_MAX);
    return t->y + ((EQP_ENH_MAX - v) * range + EQP_ENH_MAX / 2) / EQP_ENH_MAX;
}

int eq_panel_enh_at(const EqPanelLayout *L, int x, int y)
{
    if (y < L->ecol[0].y || y >= L->ecol[0].y + L->ecol[0].h) return -1;
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < EQP_ENH_SLIDERS; i++) {
        int c = L->ecol[i].x + L->ecol[i].w / 2;
        int d = x > c ? x - c : c - x;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void eq_panel_enh_config_value(const AudioEnh *e, char *buf, int cap) { audio_enh_format(e, buf, (size_t)cap); }

static void eq_panel_set_page(EqPanel *p, EqPanelPage pg);

/* CANCEL and BACK: everything back to what the panel opened with (the caller applies it again). */
static void restore(EqPanel *p)
{
    memcpy(p->gains, p->orig, sizeof p->gains);
    p->enh = p->enh_orig;
}

/* Pane cycle: EQK_NEXT / EQK_PREV move to the next / previous pane (sliders, presets, buttons), wrapping.
 * Up / Down / Left / Right stay inside the pane; the sliders use Up / Down for their value. */
EqPanelEvent eq_panel_key(EqPanel *p, EqPanelKey k)
{
    p->dirty = true;
    if (k == EQK_BACK) { restore(p); return EQP_CANCEL; }
    if (k == EQK_NEXT) { p->zone = (EqPanelZone)((p->zone + 1) % EQP_ZONES); return EQP_NONE; }
    if (k == EQK_PREV) { p->zone = (EqPanelZone)((p->zone + EQP_ZONES - 1) % EQP_ZONES); return EQP_NONE; }
    bool enh = p->page == EQP_PAGE_ENH;
    switch (p->zone) {
    case EQP_ZONE_SLIDERS:
        if (enh) {
            if (k == EQK_LEFT)  p->enh_i = clampi(p->enh_i - 1, 0, EQP_ENH_SLIDERS - 1);
            if (k == EQK_RIGHT) p->enh_i = clampi(p->enh_i + 1, 0, EQP_ENH_SLIDERS - 1);
            if (k == EQK_ACT)   p->zone = EQP_ZONE_PRESETS;
            if (k == EQK_UP || k == EQK_DOWN) {
                int d = k == EQK_UP ? EQP_ENH_STEP : -EQP_ENH_STEP;
                if (eq_panel_enh_set(&p->enh, p->enh_i, eq_panel_enh_get(&p->enh, p->enh_i) + d)) return EQP_CHANGED;
            }
            break;
        }
        if (k == EQK_LEFT)  p->band = clampi(p->band - 1, 0, EQ_BANDS - 1);
        if (k == EQK_RIGHT) p->band = clampi(p->band + 1, 0, EQ_BANDS - 1);
        if (k == EQK_ACT)   p->zone = EQP_ZONE_PRESETS;
        if (k == EQK_UP || k == EQK_DOWN) {
            int v = clampi(p->gains[p->band] + (k == EQK_UP ? 1 : -1), -EQ_MAX_DB, EQ_MAX_DB);
            if (v != p->gains[p->band]) { p->gains[p->band] = v; return EQP_CHANGED; }
        }
        break;
    case EQP_ZONE_PRESETS:
        if (enh) {   /* the ON/OFF bar is the one item of this row; Up / Down do not leave the row */
            if (k == EQK_ACT) { p->enh.on = !p->enh.on; return EQP_CHANGED; }
            break;
        }
        if (k == EQK_LEFT)  p->preset_focus = clampi(p->preset_focus - 1, 0, EQP_PRESETS - 1);
        if (k == EQK_RIGHT) p->preset_focus = clampi(p->preset_focus + 1, 0, EQP_PRESETS - 1);
        if (k == EQK_ACT && p->preset_focus < eq_preset_count()) {
            const int *g = eq_preset_gains(p->preset_focus);
            bool ch = !same(p->gains, g);
            memcpy(p->gains, g, sizeof p->gains);
            return ch ? EQP_CHANGED : EQP_NONE;
        }
        break;
    case EQP_ZONE_BUTTONS:
    default:
        if (k == EQK_LEFT)  p->btn_focus = clampi(p->btn_focus - 1, 0, EQP_BTNS - 1);
        if (k == EQK_RIGHT) p->btn_focus = clampi(p->btn_focus + 1, 0, EQP_BTNS - 1);
        if (k == EQK_ACT) {
            if (p->btn_focus == EQP_BTN_DONE) return EQP_DONE;
            if (p->btn_focus == EQP_BTN_PAGE) { eq_panel_set_page(p, enh ? EQP_PAGE_EQ : EQP_PAGE_ENH); return EQP_NONE; }
            restore(p);
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

/* Restyle a button that is under a finger right now (the ENH/EQ page button, the toggle): button_init_full()
 * clears the tap latch, so the very next held frame would tap the relabelled button a second time and flip it back. */
static void restyle(Button *b, UiRect r, const char *text)
{
    bool was = b->was_pressed;
    uint32_t last = b->last_press_time_ms;
    style(b, r, text);
    b->was_pressed = was;
    b->last_press_time_ms = last;
}

static void style_toggle(EqPanel *p)
{
    if (p->toggle_shown == p->enh.on + 1) return;      /* restyling every frame would reset the button's tap latch */
    p->toggle_shown = p->enh.on + 1;
    restyle(&p->toggle_b, p->L.toggle, p->enh.on ? "ENHANCER ON" : "ENHANCER OFF");
}

static void eq_panel_set_page(EqPanel *p, EqPanelPage pg)
{
    p->page = pg;
    restyle(&p->page_b, p->L.page, pg == EQP_PAGE_ENH ? "EQ" : "ENH");
    p->drag = p->enh_drag = -1;
    p->dirty = true;
}

void eq_panel_open(EqPanel *p, const int gains[EQ_BANDS], int sl, int st, int sr, int sb, uint32_t now)
{
    AudioEnh off;
    memset(&off, 0, sizeof off);
    eq_panel_open_enh(p, gains, &off, sl, st, sr, sb, now);
}

void eq_panel_open_enh(EqPanel *p, const int gains[EQ_BANDS], const AudioEnh *enh, int sl, int st, int sr, int sb, uint32_t now)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < EQ_BANDS; i++) p->gains[i] = clampi(gains[i], -EQ_MAX_DB, EQ_MAX_DB);
    memcpy(p->orig, p->gains, sizeof p->orig);
    if (enh) p->enh = *enh;
    p->enh.on = p->enh.on ? 1 : 0;
    for (int i = 0; i < EQP_ENH_SLIDERS; i++) eq_panel_enh_set(&p->enh, i, eq_panel_enh_get(&p->enh, i));
    p->enh_orig = p->enh;
    p->drag = p->enh_drag = -1;
    eq_panel_layout(&p->L, sl, st, sr, sb);
    int m = eq_preset_match(p->gains);
    p->preset_focus = m >= 0 ? m : 0;
    for (int i = 0; i < EQP_PRESETS; i++)
        style(&p->preset_b[i], p->L.preset[i], i < eq_preset_count() ? eq_preset_name(i) : "");
    style(&p->cancel_b, p->L.cancel, "CANCEL");
    style(&p->done_b, p->L.done, "DONE");
    style_toggle(p);
    eq_panel_set_page(p, EQP_PAGE_EQ);
    p->btn_focus = EQP_BTN_DONE;
    touch_guard_init(&p->guard);
    touch_guard_screen_changed(&p->guard, now);   /* the tap that opened it must not reach it */
    p->dirty = true;
}

const int *eq_panel_gains(const EqPanel *p) { return p->gains; }
const AudioEnh *eq_panel_enh(const EqPanel *p) { return &p->enh; }

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
    /* Pane cycle, as ui_zone.c reads it: Tab / RB = next pane; NEXT while RUN (Shift) is held = previous.  RUN alone is nothing.
     * PgDn / PgUp step the same way.  Up / Down / Left / Right never leave a pane (see eq_panel_key). */
    EqPanelEvent ev = EQP_NONE;
    static const struct { int id; EqPanelKey k; } map[] = {
        { BTN_ID_UP, EQK_UP }, { BTN_ID_DOWN, EQK_DOWN }, { BTN_ID_LEFT, EQK_LEFT }, { BTN_ID_RIGHT, EQK_RIGHT },
        { BTN_ID_ACTION, EQK_ACT }, { BTN_ID_JUMP, EQK_ACT }, { BTN_ID_NEXT, EQK_NEXT }, { BTN_ID_PGDN, EQK_NEXT },
        { BTN_ID_PREV, EQK_PREV }, { BTN_ID_PGUP, EQK_PREV }, { BTN_ID_BACK, EQK_BACK }, { BTN_ID_PAUSE, EQK_BACK }, { BTN_ID_CANCEL, EQK_BACK },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (bp(in, map[i].id)) {
            EqPanelKey k = map[i].k;
            if (map[i].id == BTN_ID_NEXT && in->buttons[BTN_ID_RUN].held) k = EQK_PREV;
            EqPanelEvent e = eq_panel_key(p, k);
            if (e == EQP_DONE || e == EQP_CANCEL) return e;
            if (e != EQP_NONE) ev = e;
        }
    style_toggle(p);

    if (p->page == EQP_PAGE_ENH) {
        if (ts->pressed) {
            int b = eq_panel_enh_at(&p->L, ts->x, ts->y);
            if (b >= 0) { p->enh_drag = b; p->enh_i = b; p->zone = EQP_ZONE_SLIDERS; }
        }
        if (p->enh_drag >= 0 && (ts->pressed || ts->held)) {
            if (eq_panel_enh_set(&p->enh, p->enh_drag, eq_panel_enh_value_at(&p->L.etrack[p->enh_drag], ts->y))) ev = EQP_CHANGED;
            p->dirty = true;
        }
        if (ts->released || (!ts->held && !ts->pressed)) {
            if (p->enh_drag >= 0) p->dirty = true;
            p->enh_drag = -1;
        }
        if (button_check_tap(&p->toggle_b, ts, now)) {
            p->enh.on = !p->enh.on;
            p->zone = EQP_ZONE_PRESETS;
            style_toggle(p);
            p->dirty = true;
            ev = EQP_CHANGED;
        }
    } else {
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
    }
    if (button_check_tap(&p->cancel_b, ts, now)) {
        restore(p);
        return EQP_CANCEL;
    }
    if (button_check_tap(&p->page_b, ts, now)) {
        p->btn_focus = EQP_BTN_PAGE; p->zone = EQP_ZONE_BUTTONS;
        eq_panel_set_page(p, p->page == EQP_PAGE_ENH ? EQP_PAGE_EQ : EQP_PAGE_ENH);
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

static void draw_buttons(EqPanel *p, Framebuffer *fb)
{
    const EqPanelLayout *L = &p->L;
    button_draw(fb, &p->cancel_b);
    button_draw(fb, &p->page_b);
    button_draw(fb, &p->done_b);
    if (p->zone == EQP_ZONE_BUTTONS) {
        const UiRect *r = p->btn_focus == EQP_BTN_DONE ? &L->done : p->btn_focus == EQP_BTN_PAGE ? &L->page : &L->cancel;
        ring(fb, *r);
    }
}

static void draw_enh(EqPanel *p, Framebuffer *fb)
{
    const EqPanelLayout *L = &p->L;
    static const char *part[EQP_ENH_PARTS] = { "HARM BASS", "DRUM BASS", "HARM TREBLE" };
    static const char *kind[2] = { "AMT", "RNG" };
    fb_clear(fb, PANEL_BG);
    text_draw_centered(fb, L->title.x + L->title.w / 2, L->title.y + L->title.h / 2, "ENHANCER", COLOR_WHITE, 3);
    uint32_t dim = p->enh.on ? COLOR_WHITE : DIMTXT;
    for (int g = 0; g < EQP_ENH_PARTS; g++) {
        const UiRect *r = &L->group[g];
        text_draw_centered(fb, r->x + r->w / 2, r->y + r->h / 2, part[g], dim, 2);
        fb_fill_rect(fb, r->x + 8, r->y + r->h - 1, r->w - 16, 1, DIMTXT);
    }
    for (int i = 0; i < EQP_ENH_SLIDERS; i++) {
        const UiRect *t = &L->etrack[i];
        int cx = t->x + t->w / 2;
        bool sel = p->zone == EQP_ZONE_SLIDERS && p->enh_i == i;
        int v = eq_panel_enh_get(&p->enh, i), yv = eq_panel_enh_y_of_value(t, v);
        fb_fill_rect(fb, cx - 3, t->y, 6, t->h, RAIL);
        fb_fill_rect(fb, cx - 3, yv, 6, t->y + t->h - yv, p->enh.on ? FILL : RAIL);
        uint32_t kc = sel ? COLOR_YELLOW : dim;
        fb_fill_rect(fb, t->x, yv - 6, t->w, 13, kc);
        char s[8];
        snprintf(s, sizeof s, "%d", v);
        text_draw_centered(fb, cx, L->ecol[i].y + LABEL_H / 2 + 2, s, sel ? COLOR_YELLOW : dim, 2);
        text_draw_centered(fb, cx, t->y + t->h + 4 + LABEL_H / 2, kind[i & 1], sel ? COLOR_YELLOW : DIMTXT, 2);
        if (sel) ring(fb, rect_of(L->ecol[i].x + 2, L->ecol[i].y, L->ecol[i].w - 4, L->ecol[i].h - 2));
    }
    if (p->enh.on) button_set_colors(&p->toggle_b, COLOR_YELLOW, COLOR_BLACK, COLOR_YELLOW);
    else button_set_colors(&p->toggle_b, BTN_BG, COLOR_WHITE, BTN_HL);
    button_draw(fb, &p->toggle_b);
    if (p->zone == EQP_ZONE_PRESETS) ring(fb, L->toggle);
    draw_buttons(p, fb);
}

void eq_panel_draw(EqPanel *p, Framebuffer *fb)
{
    if (p->page == EQP_PAGE_ENH) { draw_enh(p, fb); return; }
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
    draw_buttons(p, fb);
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

int eq_panel_store_both(Config *cfg, const int gains[EQ_BANDS], const AudioEnh *enh)
{
    char v[CONFIG_VAL_LEN];
    eq_panel_config_value(gains, v, sizeof v);
    config_set(cfg, EQ_CONFIG_KEY, v);
    eq_panel_enh_config_value(enh, v, sizeof v);
    config_set(cfg, ENH_CONFIG_KEY, v);
    return config_save(cfg);
}

int eq_panel_store_file_both(const char *path, const int gains[EQ_BANDS], const AudioEnh *enh)
{
    Config cfg;
    config_init_path(&cfg, path);
    config_load(&cfg);
    return eq_panel_store_both(&cfg, gains, enh);
}
