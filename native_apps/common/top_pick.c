/* top_pick.c — the top bar's drop-down; see top_pick.h. */
#include "top_pick.h"
#include "icons.h"
#include "common.h"
#include <string.h>

#define TP_BG  RGB(30, 36, 52)

static int src_count(void *ctx)
{
    const TopPick *tp = ctx;
    return tp->src.count ? tp->src.count(tp->src.ctx) : 0;
}

static void src_row(void *ctx, int index, LvRow *out)
{
    const TopPick *tp = ctx;
    out->title = tp->src.name ? tp->src.name(tp->src.ctx, index) : "";
    out->glyph = LV_GLYPH_BLANK;
}

void tp_init(TopPick *tp, TpSource src)
{
    memset(tp, 0, sizeof *tp);
    tp->src = src;
    tp->picked = -1;
    LvSource ls = { src_count, src_row, tp };
    UiRect r = { 0, 0, 10, 10 };
    lv_init(&tp->lv, ls, r, TP_ROW_H);
}

void tp_free(TopPick *tp) { lv_free(&tp->lv); }

static bool in_rect(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static bool bp(const InputState *in, int id) { return in && in->buttons[id].pressed; }

UiRect tp_head_rect(int title_x, int title_w, int top, int bottom, const TpExt *e, bool arrow)
{
    int x0 = title_x - TP_GAP, x1 = title_x + title_w + TP_GAP + (arrow ? TP_ARROW + TP_GAP : 0);
    int y0 = top > e->safe_t ? top : e->safe_t;
    if (x0 < e->safe_l) x0 = e->safe_l;
    if (x1 > e->safe_r) x1 = e->safe_r;
    UiRect r = { x0, y0, x1 - x0, bottom - y0 };
    return r;
}

UiRect tp_popup_rect(UiRect head, int count, const TpExt *e)
{
    int w = TP_W;
    if (w > e->safe_r - e->safe_l) w = e->safe_r - e->safe_l;
    int x = head.x;
    if (x + w > e->safe_r) x = e->safe_r - w;
    if (x < e->safe_l) x = e->safe_l;
    int y = head.y + head.h + 4;
    int rows = lv_vis_rows(e->safe_b - y, TP_ROW_H);
    if (rows > count) rows = count;
    if (rows < 1) rows = 1;
    UiRect r = { x, y, w, rows * TP_ROW_H + 2 * LV_BORDER };
    return r;
}

void tp_open(TopPick *tp, UiRect head, const TpExt *e)
{
    lv_sync(&tp->lv);
    tp->lv.rect = tp_popup_rect(head, tp->lv.count, e);
    tp->lv.active = true;
    lv_reset(&tp->lv);
    lv_move_to(&tp->lv, tp->sel);
    tp->up = true;
    tp->head_press = false;
}

void tp_close(TopPick *tp) { tp->up = false; tp->head_press = false; }

TpEvent tp_head_input(TopPick *tp, UiRect head, const TpExt *e, const TouchState *ts, const InputState *in)
{
    if (tp->up) return TP_NONE;
    if (head.w > 0 && ts->pressed && in_rect(head, ts->x, ts->y)) tp->head_press = true;
    if (ts->released) {
        bool hit = tp->head_press && head.w > 0 && in_rect(head, ts->x, ts->y);
        tp->head_press = false;
        if (hit) { tp_open(tp, head, e); return TP_OPENED; }
    }
    if (bp(in, BTN_ID_RUN) && head.w > 0) { tp_open(tp, head, e); return TP_OPENED; }
    return TP_NONE;
}

TpEvent tp_input(TopPick *tp, const TouchState *ts, const InputState *in, uint32_t now_ms)
{
    if (!tp->up) return TP_NONE;
    if (bp(in, BTN_ID_BACK) || bp(in, BTN_ID_PAUSE) || bp(in, BTN_ID_RUN) ||
        (ts->pressed && !in_rect(tp->lv.rect, ts->x, ts->y))) {
        tp_close(tp);
        return TP_CLOSED;
    }
    tp->lv.active = true;
    lv_sync(&tp->lv);
    LvEvent ev = lv_input(&tp->lv, ts, in, now_ms);
    bool pick = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE || bp(in, BTN_ID_JUMP);
    if (pick && tp->lv.cursor >= 0 && tp->lv.cursor < tp->lv.count) {
        tp->picked = tp->lv.cursor;
        tp_close(tp);
        return TP_PICKED;
    }
    return ev != LV_EV_NONE || lv_needs_frames(&tp->lv) ? TP_CHANGED : TP_NONE;
}

int tp_take(TopPick *tp)
{
    int p = tp->picked;
    tp->picked = -1;
    return p;
}

void tp_draw_arrow(Framebuffer *fb, int title_x, int title_w, int top, int h)
{
    icon_draw(fb, ICON_DROPDOWN, title_x + title_w + TP_GAP, top + (h - TP_ARROW) / 2, TP_ARROW, COLOR_WHITE);
}

void tp_draw(Framebuffer *fb, TopPick *tp)
{
    if (!tp->up) return;
    UiRect p = tp->lv.rect;
    fb_fill_rect(fb, p.x, p.y, p.w, p.h, TP_BG);
    tp->lv.active = true;
    lv_sync(&tp->lv);
    lv_draw(fb, &tp->lv, "");
}
