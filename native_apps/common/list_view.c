/* list_view — see list_view.h.  The top half is pure; the drawing is last. */
#include "list_view.h"
#include "common.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>

#define LV_REPEAT_DELAY_MS 400
#define LV_REPEAT_EVERY_MS  80
#define LV_THUMB_MIN        12

/* The look follows the Bluetooth page rows. */
#define LV_COLOR_BG       RGB(18, 18, 28)
#define LV_COLOR_ROW      RGB(30, 30, 45)
#define LV_COLOR_ROW_BD   RGB(50, 50, 70)
#define LV_COLOR_CUR      RGB(30, 70, 120)
#define LV_COLOR_CUR_OFF  RGB(42, 52, 76)
#define LV_COLOR_MARK     RGB(255, 210, 60)
#define LV_COLOR_DIM      RGB(110, 110, 120)
#define LV_COLOR_LABEL    RGB(180, 180, 180)
#define LV_COLOR_EDGE_ON  RGB(90, 150, 255)
#define LV_COLOR_EDGE_OFF RGB(70, 70, 90)

/* ── Pure arithmetic ────────────────────────────────────────────────────── */

int lv_vis_rows(int rect_h, int row_h) {
    int inner = rect_h - 2 * LV_BORDER;
    if (row_h <= 0 || inner < row_h) return 0;
    return inner / row_h;
}

int lv_max_top(int count, int vis) {
    int m = count - vis;
    return m > 0 ? m : 0;
}

int lv_clamp_top(int top, int count, int vis) {
    int m = lv_max_top(count, vis);
    if (top > m) top = m;
    if (top < 0) top = 0;
    return top;
}

int lv_clamp_cursor(int cur, int count) {
    if (count <= 0) return -1;
    if (cur < 0) return 0;
    if (cur > count - 1) return count - 1;
    return cur;
}

int lv_top_for_cursor(int top, int cursor, int count, int vis) {
    if (cursor >= 0 && vis > 0) {
        if (cursor < top) top = cursor;
        else if (cursor >= top + vis) top = cursor - vis + 1;
    }
    return lv_clamp_top(top, count, vis);
}

int lv_cursor_after(int cur, int count, int vis, LvKey key) {
    int page = vis > 1 ? vis - 1 : 1;
    if (count <= 0) return -1;
    if (cur < 0) cur = 0;
    switch (key) {
    case LV_KEY_UP:   cur -= 1; break;
    case LV_KEY_DOWN: cur += 1; break;
    case LV_KEY_PGUP: cur -= page; break;
    case LV_KEY_PGDN: cur += page; break;
    case LV_KEY_HOME: cur = 0; break;
    case LV_KEY_END:  cur = count - 1; break;
    default: break;
    }
    return lv_clamp_cursor(cur, count);
}

int lv_hit_row(int top, int y, int row_h, int vis, int count) {
    if (row_h <= 0 || y < 0) return -1;
    int r = y / row_h;
    if (r >= vis) return -1;
    r += top;
    return (r >= 0 && r < count) ? r : -1;
}

void lv_thumb(int count, int vis, int top, int track_h, int *off, int *len) {
    int l = track_h, o = 0;
    if (count > vis && vis > 0 && track_h > 0) {
        l = track_h * vis / count;
        if (l < LV_THUMB_MIN) l = LV_THUMB_MIN;
        if (l > track_h) l = track_h;
        top = lv_clamp_top(top, count, vis);
        o = (track_h - l) * top / lv_max_top(count, vis);
    }
    *off = o;
    *len = l;
}

bool lv_repeat_due(uint32_t since, uint32_t last, uint32_t now) {
    return (uint32_t)(now - since) >= LV_REPEAT_DELAY_MS &&
           (uint32_t)(now - last) >= LV_REPEAT_EVERY_MS;
}

void lv_fit(char *dest, int size, const char *src, int max_w, int scale) {
    if (!dest || size <= 0) return;
    int len = (int)strlen(src);
    int cw = text_measure_width("M", scale);
    int keep = len, ndots = 0;                /* keep is in BYTES, a whole number of code points */
    if (max_w > 0 && cw > 0 && text_measure_width(src, scale) > max_w) {
        int fit = max_w / cw;                 /* code points that fit */
        ndots = fit < 3 ? fit : 3;
        keep = (int)utf8_prefix_bytes(src, fit > 3 ? (size_t)(fit - 3) : 0);
    }
    int room = size - 1;
    if (keep + ndots > room) {
        ndots = 3 < room ? 3 : room;
        keep = (int)utf8_cut_boundary(src, (size_t)(room - ndots));
    }
    memcpy(dest, src, (size_t)keep);
    for (int i = 0; i < ndots; i++) dest[keep + i] = '.';
    dest[keep + ndots] = '\0';
}

void lv_drag_begin(LvDrag *d, int y) {
    d->active = true;
    d->dragging = false;
    d->down_y = y;
    d->anchor_y = y;
}

int lv_drag_move(LvDrag *d, int y, int row_h) {
    if (!d->active || row_h <= 0) return 0;
    if (!d->dragging) {
        int dy = y - d->down_y;
        if (dy < 0) dy = -dy;
        if (dy <= LV_DRAG_DEADZONE) return 0;
        d->dragging = true;
        d->anchor_y = y;     /* the dead zone itself is not scrolled */
        return 0;
    }
    int steps = (y - d->anchor_y) / row_h;      /* toward zero, as log_page.c */
    d->anchor_y += steps * row_h;
    return -steps;
}

/* Velocity of the finger over the LV_FLING_WINDOW_MS before it stopped moving,
 * in milli-rows/s.  The release is seen a frame or two after the last move, so
 * a still tail up to LV_FLING_HOLD_MS is dropped; a longer one is a hold. */
int lv_fling_velocity(const LvSample *s, int n, int row_h) {
    if (!s || n < 2 || row_h <= 0) return 0;
    int last = n - 1;
    while (last > 0 && abs(s[last - 1].y - s[n - 1].y) <= LV_FLING_STILL_PX) last--;
    if ((uint32_t)(s[n - 1].ms - s[last].ms) > LV_FLING_HOLD_MS) return 0;
    uint32_t newest = s[last].ms;
    int first = last;
    while (first > 0 && (uint32_t)(newest - s[first - 1].ms) <= LV_FLING_WINDOW_MS) first--;
    uint32_t dt = newest - s[first].ms;
    if (dt < LV_FLING_MIN_MS) return 0;
    /* dy px over dt ms, finger up = toward later rows: -dy*1e6 / (dt*row_h) */
    int dy = s[last].y - s[first].y;
    int v = (-dy * 1000) / row_h;            /* milli-rows moved */
    v = (v * 1000) / (int)dt;                /* per second */
    if (v > LV_FLING_MAX) v = LV_FLING_MAX;
    if (v < -LV_FLING_MAX) v = -LV_FLING_MAX;
    if (v < LV_FLING_START && v > -LV_FLING_START) return 0;
    return v;
}

static void fling_stop(ListView *lv) {
    lv->fling_v = 0;
    lv->fling_acc = 0;
}

/* The cursor comes along with the scroll, or lv_sync() would scroll back to it. */
static void carry_cursor(ListView *lv, int vis) {
    int cur = lv->cursor;
    if (cur >= 0 && cur < lv->top) cur = lv->top;
    if (cur >= 0 && vis > 0 && cur >= lv->top + vis) cur = lv->top + vis - 1;
    if (cur != lv->cursor) { lv->cursor = cur; lv->cursor_dragged = true; }
}

static LvEvent fling_step(ListView *lv, uint32_t now_ms) {
    if (lv->fling_v == 0) return LV_EV_NONE;
    uint32_t dt = now_ms - lv->fling_last;
    lv->fling_last = now_ms;
    if (dt > 100) dt = 100;                  /* a stalled frame must not jump the list */
    int vis = lv_visible_rows(lv);
    lv->fling_acc += (lv->fling_v * (int)dt) / 1000;
    int rows = lv->fling_acc / 1000;         /* toward zero */
    lv->fling_acc -= rows * 1000;
    lv->fling_v -= (lv->fling_v * (int)dt) / LV_FLING_DECAY_MS;

    LvEvent ev = LV_EV_NONE;
    if (rows != 0) {
        int before = lv->top;
        lv->top = lv_clamp_top(lv->top + rows, lv->count, vis);
        if (lv->top != before) ev = LV_EV_CHANGED;
        carry_cursor(lv, vis);
        if (lv->top != before + rows) fling_stop(lv);       /* clamped: hit an end */
    }
    if (lv->fling_v < LV_FLING_STOP && lv->fling_v > -LV_FLING_STOP) fling_stop(lv);
    return ev;
}

/* ── The widget ─────────────────────────────────────────────────────────── */

void lv_init(ListView *lv, LvSource src, UiRect rect, int row_h) {
    memset(lv, 0, sizeof(*lv));
    lv->src = src;
    lv->rect = rect;
    lv->row_h = row_h;
    lv->scale = row_h >= 28 ? 2 : 1;
    lv->held_key = -1;
    lv_sync(lv);
}

bool lv_row_edge_lit(const ListView *lv, int index) {
    return index == lv->cursor && lv->active && !lv->ring_hidden;
}

void lv_free(ListView *lv) {
    free(lv->marks);
    lv->marks = NULL;
    lv->marks_cap = 0;
}

int lv_visible_rows(const ListView *lv) {
    return lv_vis_rows(lv->rect.h, lv->row_h);
}

static void marks_clear_from(ListView *lv, int n) {
    for (int i = n; i < lv->marks_cap; i++)
        lv->marks[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

void lv_sync(ListView *lv) {
    int n = lv->src.count ? lv->src.count(lv->src.ctx) : 0;
    if (n < 0) n = 0;
    if (n < lv->count) marks_clear_from(lv, n);
    lv->count = n;
    if (n > lv->marks_cap) {
        int bytes = (n + 7) >> 3;
        uint8_t *p = realloc(lv->marks, (size_t)bytes);
        if (p) {
            memset(p + (lv->marks_cap >> 3), 0, (size_t)(bytes - (lv->marks_cap >> 3)));
            lv->marks = p;
            lv->marks_cap = bytes * 8;
        }
    }
    lv->cursor = lv_clamp_cursor(lv->cursor, n);
    lv->top = lv_top_for_cursor(lv->top, lv->cursor, n, lv_visible_rows(lv));
}

void lv_reset(ListView *lv) {
    if (lv->marks) marks_clear_from(lv, 0);
    lv->count = 0;
    lv->cursor = 0;
    lv->top = 0;
    lv->drag.active = lv->drag.dragging = false;
    lv->nsamples = 0;
    lv->swallow_tap = false;
    fling_stop(lv);
    lv->held_key = -1;
    lv_sync(lv);
    lv->top = 0;
}

void lv_move_to(ListView *lv, int index) {
    fling_stop(lv);
    lv->cursor = lv_clamp_cursor(index, lv->count);
    lv->cursor_dragged = false;
    lv->top = lv_top_for_cursor(lv->top, lv->cursor, lv->count, lv_visible_rows(lv));
}

bool lv_is_marked(const ListView *lv, int index) {
    if (!lv->marks || index < 0 || index >= lv->count || index >= lv->marks_cap) return false;
    return (lv->marks[index >> 3] >> (index & 7)) & 1;
}

int lv_marked_next(const ListView *lv, int from) {
    if (from < 0) from = 0;
    for (int i = from; i < lv->count; i++)
        if (lv_is_marked(lv, i)) return i;
    return -1;
}

int lv_mark_count(const ListView *lv) {
    int n = 0;
    for (int i = lv_marked_next(lv, 0); i >= 0; i = lv_marked_next(lv, i + 1)) n++;
    return n;
}

static void mark_toggle(ListView *lv, int i) {
    if (!lv->marks || i < 0 || i >= lv->count || i >= lv->marks_cap) return;
    lv->marks[i >> 3] ^= (uint8_t)(1u << (i & 7));
}

LvEvent lv_key(ListView *lv, LvKey key) {
    int old_cur = lv->cursor, old_top = lv->top;
    fling_stop(lv);
    bool marks_changed = false;

    switch (key) {
    case LV_KEY_MARK:
    case LV_KEY_MARK_NEXT:
        if (lv->cursor >= 0 && lv->marks) {
            mark_toggle(lv, lv->cursor);
            marks_changed = true;
            if (key == LV_KEY_MARK_NEXT)
                lv_move_to(lv, lv_cursor_after(lv->cursor, lv->count,
                                               lv_visible_rows(lv), LV_KEY_DOWN));
        }
        break;
    case LV_KEY_MARK_ALL:
        if (lv->marks) {
            for (int i = 0; i < lv->count; i++)
                lv->marks[i >> 3] |= (uint8_t)(1u << (i & 7));
            marks_changed = lv->count > 0;
        }
        break;
    case LV_KEY_MARK_CLEAR:
        marks_changed = lv_mark_count(lv) > 0;
        if (lv->marks) marks_clear_from(lv, 0);
        break;
    default:
        lv_move_to(lv, lv_cursor_after(lv->cursor, lv->count, lv_visible_rows(lv), key));
        break;
    }
    return (marks_changed || lv->cursor != old_cur || lv->top != old_top)
           ? LV_EV_CHANGED : LV_EV_NONE;
}

bool lv_pick_due(bool reorder, bool dragging, uint32_t down_ms, uint32_t now_ms) {
    return reorder && !dragging && (uint32_t)(now_ms - down_ms) >= LV_PICK_MS;
}

int lv_drop_index(int top, int y, int row_h, int vis, int count) {
    if (count <= 0 || row_h <= 0) return 0;
    int r = y < 0 ? 0 : y / row_h;
    if (r > vis - 1) r = vis - 1;
    if (r < 0) r = 0;
    int idx = top + r;
    return idx > count - 1 ? count - 1 : idx;
}

static LvEvent touch_input_frame(ListView *lv, const TouchState *ts, uint32_t now_ms) {
    LvEvent ev = LV_EV_NONE;
    int vis = lv_visible_rows(lv);
    int first_y = lv->rect.y + LV_BORDER;
    bool inside = ts->x >= lv->rect.x && ts->x < lv->rect.x + lv->rect.w &&
                  ts->y >= lv->rect.y && ts->y < lv->rect.y + lv->rect.h;

    if (ts->pressed && inside) {
        lv->swallow_tap = lv->fling_v != 0;   /* a press that stops a fling is not a tap */
        fling_stop(lv);
        lv->nsamples = 0;
        lv_drag_begin(&lv->drag, ts->y);
        lv->down_ms = now_ms;
        lv->picked = false;
    }

    /* reorder mode: held still long enough, the row under the finger is picked up; from then on the finger moves the drop row
     * and the press is no longer a scroll or a tap */
    if (lv->drag.active && !lv->picked && (ts->held || ts->pressed) &&
        lv_pick_due(lv->reorder, lv->drag.dragging, lv->down_ms, now_ms)) {
        int idx = lv_hit_row(lv->top, lv->drag.down_y - first_y, lv->row_h, vis, lv->count);
        if (idx >= 0) {
            lv->picked = true;
            lv->pick_from = lv->pick_to = idx;
            ev = LV_EV_CHANGED;
        }
    }
    if (lv->picked && lv->drag.active && (ts->held || ts->pressed)) {
        int to = lv_drop_index(lv->top, ts->y - first_y, lv->row_h, vis, lv->count);
        if (to != lv->pick_to) { lv->pick_to = to; ev = LV_EV_CHANGED; }
    }

    if (lv->drag.active && (ts->held || ts->pressed || ts->released)) {
        if (lv->nsamples == LV_SAMPLES) {
            memmove(lv->samples, lv->samples + 1, sizeof(lv->samples[0]) * (LV_SAMPLES - 1));
            lv->nsamples--;
        }
        lv->samples[lv->nsamples].ms = now_ms;
        lv->samples[lv->nsamples++].y = ts->y;
    }

    if (lv->drag.active && !lv->picked && (ts->held || ts->pressed)) {
        int d = lv_drag_move(&lv->drag, ts->y, lv->row_h);
        if (d != 0) {
            int before = lv->top;
            lv->top = lv_clamp_top(lv->top + d, lv->count, vis);
            if (lv->top != before) ev = LV_EV_CHANGED;
            carry_cursor(lv, vis);   /* or lv_sync() would scroll back to it */
        }
    }

    if (lv->drag.active && ts->released) {
        if (lv->picked) {
            ev = lv->pick_from != lv->pick_to ? LV_EV_REORDER : LV_EV_CHANGED;   /* a hold without moving is no tap */
            lv->picked = false;
        } else if (lv->drag.dragging) {
            lv->fling_v = lv_fling_velocity(lv->samples, lv->nsamples, lv->row_h);
            lv->fling_acc = 0;
            lv->fling_last = now_ms;
        } else if (!lv->swallow_tap) {
            int idx = lv_hit_row(lv->top, lv->drag.down_y - first_y, lv->row_h,
                                 vis, lv->count);
            if (idx >= 0) {
                bool again = (idx == lv->cursor && !lv->cursor_dragged);
                lv_move_to(lv, idx);
                ev = (again && (lv->active || lv->tap2_any)) ? LV_EV_ACTIVATE : LV_EV_TAP;
            }
        }
        lv->drag.active = lv->drag.dragging = false;
    } else if (lv->drag.active && !ts->held && !ts->pressed) {
        lv->drag.active = lv->drag.dragging = false;   /* the release was missed */
        lv->picked = false;
    }
    return ev;
}

LvEvent lv_input(ListView *lv, const TouchState *ts, const InputState *in,
                 uint32_t now_ms) {
    LvEvent ev = LV_EV_NONE;
    if (ts) ev = touch_input_frame(lv, ts, now_ms);
    if (lv->fling_v != 0 && fling_step(lv, now_ms) == LV_EV_CHANGED) ev = LV_EV_CHANGED;
    if (!in || !lv->active) {
        lv->held_key = -1;
        return ev;
    }

    if (in->buttons[BTN_ID_ACTION].pressed && lv->cursor >= 0) return LV_EV_ACTIVATE;

    static const struct { int btn; LvKey key; } map[] = {
        { BTN_ID_UP, LV_KEY_UP }, { BTN_ID_DOWN, LV_KEY_DOWN },
        { BTN_ID_PGUP, LV_KEY_PGUP }, { BTN_ID_PGDN, LV_KEY_PGDN },
        { BTN_ID_HOME, LV_KEY_HOME }, { BTN_ID_END, LV_KEY_END },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const ButtonState *b = &in->buttons[map[i].btn];
        if (b->pressed) {
            lv->held_key = (int)map[i].key;
            lv->held_since = lv->held_last = now_ms;
            if (lv_key(lv, map[i].key) == LV_EV_CHANGED) ev = LV_EV_CHANGED;
        } else if (b->held && lv->held_key == (int)map[i].key) {
            if (lv_repeat_due(lv->held_since, lv->held_last, now_ms)) {
                lv->held_last = now_ms;
                if (lv_key(lv, map[i].key) == LV_EV_CHANGED) ev = LV_EV_CHANGED;
            }
        } else if (lv->held_key == (int)map[i].key) {
            lv->held_key = -1;
        }
    }
    return ev;
}

bool lv_needs_frames(const ListView *lv) {
    return lv->drag.active || lv->fling_v != 0;
}

/* ── Drawing ────────────────────────────────────────────────────────────── */

static void draw_glyph(Framebuffer *fb, LvGlyph g, int x, int y, int s) {
    switch (g) {
    case LV_GLYPH_FOLDER: {
        uint32_t c = RGB(235, 170, 40);
        fb_fill_rect(fb, x, y + s / 6, s / 2, s / 5, c);                 /* tab */
        fb_fill_rect(fb, x, y + s / 4, s, s - s / 4 - s / 8, c);         /* body */
        break;
    }
    case LV_GLYPH_FILE: {
        int w = s * 2 / 3, fx = x + (s - w) / 2;
        fb_fill_rect(fb, fx, y, w, s, RGB(205, 205, 215));
        for (int i = 0; i < 3; i++)
            fb_fill_rect(fb, fx + w / 6, y + s / 4 + i * (s / 5), w - w / 3, 2,
                         RGB(90, 90, 110));
        break;
    }
    case LV_GLYPH_UP: {
        uint32_t c = RGB(0, 200, 80);
        int half = s / 2;
        for (int r = 0; r < half; r++)                                     /* head */
            fb_fill_rect(fb, x + half - r - 1, y + r, 2 * r + 2, 1, c);
        fb_fill_rect(fb, x + half - s / 8, y + half, s / 4, s - half, c);  /* stem */
        break;
    }
    case LV_GLYPH_PLAYING: {
        uint32_t c = RGB(0, 200, 80);
        int xa = x + s / 4, span = (x + s - s / 8 - 1) - xa, half = s / 2;
        for (int d = 0; d < half; d++) {                                   /* rows d from the middle */
            int w = span * (half - d) / half + 1;
            fb_fill_rect(fb, xa, y + half - 1 - d, w, 1, c);
            fb_fill_rect(fb, xa, y + half + d, w, 1, c);
        }
        break;
    }
    case LV_GLYPH_BLANK:
        break;
    default:
        fb_fill_rect(fb, x, y, s, s, RGB(25, 25, 35));
        fb_draw_rect(fb, x, y, s, s, LV_COLOR_ROW_BD);
        break;
    }
}

static void draw_row(Framebuffer *fb, const ListView *lv, int index, int x, int y, int w) {
    LvRow r;
    memset(&r, 0, sizeof(r));
    lv->src.row(lv->src.ctx, index, &r);

    int h = lv->row_h - 2;
    bool cur = (index == lv->cursor);
    bool marked = lv_is_marked(lv, index);
    uint32_t fill = !cur ? LV_COLOR_ROW : lv->active ? LV_COLOR_CUR : LV_COLOR_CUR_OFF;
    uint32_t edge = lv_row_edge_lit(lv, index) ? BTN_COLOR_HIGHLIGHT : LV_COLOR_ROW_BD;
    uint32_t ink = (r.flags & LV_ROW_DIM) ? LV_COLOR_DIM : marked ? LV_COLOR_MARK : COLOR_WHITE;
    fb_fill_rect(fb, x, y, w, h, fill);
    fb_draw_rect(fb, x, y, w, h, edge);
    if (marked) fb_fill_rect(fb, x + 1, y + 1, 3, h - 2, LV_COLOR_MARK);

    int isz = lv->row_h - 8;
    if (isz < 8) isz = 8;
    int ix = x + 8, iy = y + (h - isz) / 2;
    if (r.icon && r.icon->pixels && r.icon->w > 0 && r.icon->h > 0)
        fb_blit_sprite_scaled(fb, r.icon->pixels, r.icon->w, 0, 0, r.icon->w, r.icon->h,
                              ix, iy, isz, isz, 0xFFFFFFFF);
    else
        draw_glyph(fb, r.glyph, ix, iy, isz);

    int tx = ix + isz + 8;
    int right = x + w - 8;
    int ty = y + (h - text_measure_height(lv->scale)) / 2;
    int title_w = right - tx;
    if (r.detail && r.detail[0]) {
        int dw = text_measure_width(r.detail, lv->scale);
        fb_draw_text(fb, right - dw, ty, r.detail,
                     (r.flags & LV_ROW_DIM) ? LV_COLOR_DIM : LV_COLOR_LABEL, lv->scale);
        title_w = right - dw - 10 - tx;
    }
    char cut[128];
    lv_fit(cut, sizeof(cut), r.title ? r.title : "", title_w, lv->scale);
    fb_draw_text(fb, tx, ty, cut, ink, lv->scale);
}

void lv_draw(Framebuffer *fb, ListView *lv, const char *empty_msg) {
    const UiRect *R = &lv->rect;
    int vis = lv_visible_rows(lv);
    fb_fill_rect(fb, R->x, R->y, R->w, R->h, LV_COLOR_BG);
    fb_draw_rect(fb, R->x, R->y, R->w, R->h, lv->active ? LV_COLOR_EDGE_ON : LV_COLOR_EDGE_OFF);

    if (lv->count <= 0) {
        if (empty_msg && empty_msg[0])
            text_draw_centered(fb, R->x + R->w / 2, R->y + R->h / 2, empty_msg,
                               LV_COLOR_LABEL, lv->scale);
        return;
    }

    int x = R->x + LV_BORDER, w = R->w - 2 * LV_BORDER - LV_BAR_W;
    int top = lv_clamp_top(lv->top, lv->count, vis);
    for (int i = 0; i < vis && top + i < lv->count; i++)
        draw_row(fb, lv, top + i, x, R->y + LV_BORDER + i * lv->row_h + 1, w);

    if (lv->picked) {   /* the picked row gets a yellow edge, the drop place a yellow line */
        int rf = lv->pick_from - top, rt = lv->pick_to - top;
        if (rf >= 0 && rf < vis)
            fb_draw_rect(fb, x, R->y + LV_BORDER + rf * lv->row_h, w, lv->row_h, LV_COLOR_MARK);
        if (rt >= 0 && rt < vis) {
            int ly = R->y + LV_BORDER + (lv->pick_to <= lv->pick_from ? rt : rt + 1) * lv->row_h;
            fb_fill_rect(fb, x, ly - 1, w, 3, LV_COLOR_MARK);
        }
    }

    if (lv->count > vis && vis > 0) {
        int track = vis * lv->row_h, off, len;
        int bx = R->x + R->w - LV_BORDER - LV_BAR_W + 1;
        lv_thumb(lv->count, vis, top, track, &off, &len);
        fb_fill_rect(fb, bx, R->y + LV_BORDER, LV_BAR_W - 2, track, LV_COLOR_ROW);
        fb_fill_rect(fb, bx, R->y + LV_BORDER + off, LV_BAR_W - 2, len,
                     lv->active ? LV_COLOR_EDGE_ON : LV_COLOR_EDGE_OFF);
    }
}
