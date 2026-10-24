/*
 * jv_ui_layout.c — the video client's rectangles (pure).  See jv_ui.h.
 * Hit targets lie inside the safe rectangle, drawn-only rects inside the visible one; the bar's own geometry is top_bar's.
 */
#include <string.h>

#include "jv_ui.h"

#define BANNER_H  18
#define GAP        6
#define FIELD_H   52
#define TRK_H     40
#define SCROLL_W  48
#define POSTER_W  200
#define P_TOP_H    40   /* player: the title band below the safe top */
#define P_PROG_H   28   /* player: the progress row */
#define P_BACK_W   64
#define P_CLOCK_W  72
#define P_VOL_W   120
#define P_TIME_W   88   /* "h:mm:ss" at text scale 2 is 84 px wide */

static UiRect rect_of(int x, int y, int w, int h) { UiRect r = { x, y, w, h }; return r; }

void jv_ui_layout(JvLayout *L, const JvExtents *e)
{
    memset(L, 0, sizeof *L);
    int sl = e->safe_l, sw = e->safe_r - e->safe_l;
    int bb = top_bar_bottom(e->vis_t, e->safe_t);
    int y0 = bb + 3;
    L->bar_bottom = bb;
    L->banner = rect_of(sl, y0, sw, BANNER_H);

    int top = y0 + BANNER_H + 2;
    int rowy = e->safe_b - JV_UI_BTN_H - 4;
    L->list = rect_of(sl, top, sw, e->safe_b - top);
    L->list_s = rect_of(sl, top, sw, rowy - 4 - top);
    L->sbtn = rect_of(sl, rowy, sw, JV_UI_BTN_H);

    /* poster grid: the cells tile the area left of the scroll column exactly, so none overlaps */
    int gw = sw - SCROLL_W - GAP;
    int cw = gw / JV_UI_COLS, ch = (e->safe_b - top) / JV_UI_ROWS;
    for (int i = 0; i < JV_UI_CELLS; i++) {
        int r = i / JV_UI_COLS, c = i - r * JV_UI_COLS;
        L->cell[i] = rect_of(sl + c * cw, top + r * ch, cw, ch);
    }
    L->gup = rect_of(sl + sw - SCROLL_W, top, SCROLL_W, JV_UI_BTN_H);
    L->gdn = rect_of(sl + sw - SCROLL_W, e->safe_b - JV_UI_BTN_H, SCROLL_W, JV_UI_BTN_H);

    for (int i = 0; i < JV_UI_FIELDS; i++)
        L->field[i] = rect_of(sl, top + 4 + i * (FIELD_H + GAP), sw, FIELD_H);
    int wb = (sw - 2 * GAP) / 3;
    int by = top + 4 + JV_UI_FIELDS * (FIELD_H + GAP) + 8;
    L->signin = rect_of(sl, by, wb, FIELD_H);
    L->reuse = rect_of(sl + wb + GAP, by, wb, FIELD_H);
    L->signexit = rect_of(sl + 2 * (wb + GAP), by, wb, FIELD_H);

    /* home: three columns, two rows over the list area */
    int tw = (sw - 2 * GAP) / 3, th = (e->safe_b - top - GAP) / 2;
    for (int i = 0; i < JV_UI_TILES; i++) {
        int r = i / 3, c = i - r * 3;
        L->tile[i] = rect_of(sl + c * (tw + GAP), top + r * (th + GAP), tw, th);
    }

    /* details: poster left; text, AUDIO and SUBTITLES rows and the action row on the right and below */
    int trk1 = rowy - 4 - TRK_H, trk0 = trk1 - GAP - TRK_H;
    L->poster = rect_of(sl, top + 4, POSTER_W, POSTER_W);
    L->text = rect_of(sl + POSTER_W + GAP, top, sw - POSTER_W - GAP, trk0 - 4 - top);
    L->trk[0] = rect_of(sl, trk0, sw, TRK_H);
    L->trk[1] = rect_of(sl, trk1, sw, TRK_H);
    int w4 = (sw - 3 * GAP) / 4;
    for (int i = 0; i < JV_UI_ACT; i++)
        L->act[i] = rect_of(sl + i * (w4 + GAP), rowy, w4, JV_UI_BTN_H);

    /* player: two opaque bands over the full-screen picture (the video skips their rows).  Every button is inside SAFE,
     * the bands themselves span VISIBLE so they cover the bezel margin too. */
    {
        int vl = e->vis_l, vw = e->vis_r - e->vis_l;
        L->top_band = rect_of(vl, e->vis_t, vw, (e->safe_t - e->vis_t) + P_TOP_H);
        int pty = e->safe_t + 2, pth = P_TOP_H - 4;
        L->tb_back = rect_of(sl, pty, P_BACK_W, pth);
        L->tb_clock = rect_of(e->safe_r - P_CLOCK_W, pty, P_CLOCK_W, pth);
        L->tb_vol = rect_of(L->tb_clock.x - GAP - P_VOL_W, pty, P_VOL_W, pth);
        int tx = L->tb_back.x + L->tb_back.w + GAP;
        L->tb_title = rect_of(tx, pty, L->tb_vol.x - GAP - tx, pth);

        int btop = e->safe_b - (4 + JV_UI_BTN_H + 4 + P_PROG_H + 4);
        L->bot_band = rect_of(vl, btop, vw, e->vis_b - btop);
        int py = btop + 4;
        L->ppos = rect_of(sl, py, P_TIME_W, P_PROG_H);
        L->pdur = rect_of(e->safe_r - P_TIME_W, py, P_TIME_W, P_PROG_H);
        int px0 = sl + P_TIME_W + GAP;
        L->ptrack = rect_of(px0, py, (e->safe_r - P_TIME_W - GAP) - px0, P_PROG_H);
        int bw = (sw - (JV_UI_PBTN - 1) * GAP) / JV_UI_PBTN, bty = py + P_PROG_H + 4;
        for (int i = 0; i < JV_UI_PBTN; i++)
            L->pbtn[i] = rect_of(sl + i * (bw + GAP), bty, bw, JV_UI_BTN_H);

        int pcy = L->top_band.y + L->top_band.h;
        L->pic = rect_of(vl, pcy, vw, L->bot_band.y - pcy);
        L->pbanner = rect_of(sl, pcy + 4, sw, BANNER_H);
    }
}

void jv_ui_extents_live(JvExtents *e)
{
    e->vis_l = SCREEN_VISIBLE_LEFT;  e->vis_t = SCREEN_VISIBLE_TOP;
    e->vis_r = SCREEN_VISIBLE_RIGHT; e->vis_b = SCREEN_VISIBLE_BOTTOM;
    e->safe_l = SCREEN_SAFE_LEFT;    e->safe_t = SCREEN_SAFE_TOP;
    e->safe_r = SCREEN_SAFE_RIGHT;   e->safe_b = SCREEN_SAFE_BOTTOM;
}

/* x -> ms on the progress track: pure, monotonic, 0 at the first pixel and exactly runtime_ms at the last; the one divide runs per tap */
long jv_ui_progress_ms(const JvLayout *L, int x, long runtime_ms)
{
    int tw = L->ptrack.w;
    if (runtime_ms <= 0 || tw <= 1) return 0;
    int dx = x - L->ptrack.x;
    if (dx < 0) dx = 0;
    if (dx > tw - 1) dx = tw - 1;
    return (long)((long long)dx * runtime_ms / (tw - 1));
}

/* ms -> filled width of the track (0..w); once per repaint of the bar */
int jv_ui_progress_w(const JvLayout *L, long pos_ms, long runtime_ms)
{
    int tw = L->ptrack.w;
    if (runtime_ms <= 0 || pos_ms <= 0) return 0;
    if (pos_ms >= runtime_ms) return tw;
    return (int)((long long)pos_ms * tw / runtime_ms);
}
