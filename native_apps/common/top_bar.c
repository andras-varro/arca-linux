/* top_bar.c — see top_bar.h. */
#include "top_bar.h"
#include "status_line.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TB_BG        RGB(40, 40, 58)
#define TB_RULE      RGB(60, 60, 80)
#define TB_INFO_FG   RGB(200, 200, 220)
#define TB_BACK_BG   RGB(35, 35, 50)
#define TB_TEXT_SCALE 2             /* message and info */
#define TB_CHAR_W    6              /* one glyph at scale 1, spacing included */

int top_bar_bottom(int visible_top, int safe_top) {
    int by_height = visible_top + TOP_BAR_MIN_H;
    int by_touch  = safe_top + TOP_BAR_MIN_TOUCH_H;
    return by_height > by_touch ? by_height : by_touch;
}

void top_bar_init(TopBar *t, bool has_back, bool has_exit) {
    memset(t, 0, sizeof(*t));
    t->has_back = has_back;
    t->has_exit = has_exit;
    t->title_color = COLOR_WHITE;
    t->message_color = COLOR_ORANGE;
    t->right = t->right_buf;           /* empty until the first sample */
    button_init_full(&t->back, 0, 0, TOP_BAR_BTN_W, TOP_BAR_MIN_H, "<",
                     TB_BACK_BG, COLOR_WHITE, BTN_HIGHLIGHT_COLOR, 3);
    button_init_full(&t->exit, 0, 0, TOP_BAR_BTN_W, TOP_BAR_MIN_H, "X",
                     BTN_EXIT_COLOR, COLOR_WHITE, BTN_HIGHLIGHT_COLOR, 2);
}

static int clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void top_bar_compute(TopBarLayout *L, const TopBar *t, const TbExtents *e) {
    memset(L, 0, sizeof(*L));
    L->left = e->vis_left;
    L->right = e->vis_right;
    L->top = e->top;
    L->bottom = e->bottom;
    L->h = e->bottom - e->top;
    L->msg_chars = -1;
    L->title_chars = -1;

    /* The pressable extent is consumed from both ends: back and message from
     * the left, exit and info from the right.  What is left is the title's. */
    int lo = e->safe_left, hi = e->safe_right;
    if (t->has_back) {
        L->back.x = lo;
        L->back.w = TOP_BAR_BTN_W;
        lo += TOP_BAR_BTN_W;
    }
    if (t->has_exit) {
        hi -= TOP_BAR_BTN_W;
        L->exit.x = hi;
        L->exit.w = TOP_BAR_BTN_W;
    }
    if (t->right && t->right[0]) {
        int w = text_measure_width(t->right, TB_TEXT_SCALE);
        L->info.w = w;
        L->info.x = hi - TOP_BAR_PAD - w;
        if (L->info.x < lo) L->info.x = lo;
        hi = L->info.x;
    }
    if (t->message && t->message[0]) {
        int w = text_measure_width(t->message, TB_TEXT_SCALE) + 2 * TOP_BAR_PAD;
        if (lo + w > hi) {               /* no room: cut, never overlap the info */
            w = hi - lo > 0 ? hi - lo : 0;
            int text_room = w - 2 * TOP_BAR_PAD;
            L->msg_chars = text_room > 0 ? text_room / (TB_CHAR_W * TB_TEXT_SCALE) : 0;
        }
        L->msg.x = lo;
        L->msg.w = w;
        lo += w;
    }

    if (t->title && t->title[0]) {
        int free_lo = lo + TOP_BAR_PAD, free_hi = hi - TOP_BAR_PAD;
        int room = free_hi - free_lo;
        int smax = clamp_i(t->title_scale ? t->title_scale : 3, 1, 3);
        int w = 0, scale = 0, chars = -1;
        for (int s = smax; s >= 1 && !scale; s--) {
            int tw = text_measure_width(t->title, s);
            if (tw <= room) { scale = s; w = tw; }
        }
        if (!scale && t->title_cut) {
            int keep = room / TB_CHAR_W - 2;       /* the ".." takes two */
            if (keep >= 3) { scale = 1; chars = keep; w = (keep + 2) * TB_CHAR_W; }
        }
        if (scale) {
            L->title_scale = scale;
            L->title_chars = chars;
            L->title_w = w;
            /* On the screen's centre when that clears the neighbours, else as
             * near it as the room allows. */
            int cx = (e->vis_left + e->vis_right) / 2;
            L->title_x = clamp_i(cx - w / 2, free_lo, free_hi - w);
        }
    }
}

static bool span_has(TbSpan s, int x) {
    return s.w > 0 && x >= s.x && x < s.x + s.w;
}

TbHit top_bar_hit(const TopBarLayout *L, int x, int y) {
    if (y < L->top || y >= L->bottom) return TB_NONE;
    if (span_has(L->exit, x)) return TB_EXIT;
    if (span_has(L->back, x)) return TB_BACK;
    if (span_has(L->msg, x))  return TB_MSG;
    return TB_NONE;
}

UiRect top_bar_ring_rect(const TopBarLayout *L, TbHit which) {
    TbSpan s = { 0, 0 };
    if (which == TB_BACK) s = L->back;
    else if (which == TB_EXIT) s = L->exit;
    else if (which == TB_MSG) s = L->msg;
    UiRect r = { s.x + TOP_BAR_RING_INSET, L->top + TOP_BAR_RING_INSET,
                 s.w - 2 * TOP_BAR_RING_INSET, L->h - 2 * TOP_BAR_RING_INSET };
    if (s.w <= 0) { r.x = r.y = r.w = r.h = 0; }
    return r;
}

static void place_button(Button *b, TbSpan s, const TopBarLayout *L, bool present) {
    b->x = s.x;
    b->y = L->top;
    b->width = s.w;
    b->height = L->h;
    b->disabled = !present;          /* an absent button takes no tap and no focus */
}

void top_bar_layout(TopBar *t) {
    TbExtents e;
    e.vis_left = SCREEN_VISIBLE_LEFT;   e.vis_right = SCREEN_VISIBLE_RIGHT;
    e.safe_left = SCREEN_SAFE_LEFT;     e.safe_right = SCREEN_SAFE_RIGHT;
    e.top = SCREEN_VISIBLE_TOP;         e.bottom = TOP_BAR_BOTTOM;
    top_bar_compute(&t->L, t, &e);
    place_button(&t->back, t->L.back, &t->L, t->has_back);
    place_button(&t->exit, t->L.exit, &t->L, t->has_exit);
}

/* src's first `chars` characters then "..", into out; chars < 0 = all of it. */
static const char *cut_text(char *out, size_t n, const char *src, int chars) {
    if (chars < 0) return src;
    size_t keep = (size_t)chars;
    if (keep > n - 3) keep = n - 3;
    memcpy(out, src, keep);
    out[keep] = '.';
    out[keep + 1] = '.';
    out[keep + 2] = '\0';
    return out;
}

void top_bar_draw(Framebuffer *fb, TopBar *t) {
    top_bar_layout(t);
    const TopBarLayout *L = &t->L;
    int cy = L->top + L->h / 2;       /* the whole bar's middle, not the pressable part's */

    fb_fill_rect(fb, L->left, L->top, L->right - L->left, L->h, TB_BG);
    fb_fill_rect(fb, L->left, L->bottom, L->right - L->left, 1, TB_RULE);

    if (L->msg.w > 0 && L->msg_chars != 0) {
        char buf[64];
        const char *m = t->message;
        if (L->msg_chars >= 0) {           /* cut without the dots: it is a tap target's label */
            size_t keep = (size_t)L->msg_chars;
            if (keep > sizeof(buf) - 1) keep = sizeof(buf) - 1;
            memcpy(buf, m, keep);
            buf[keep] = '\0';
            m = buf;
        }
        fb_draw_text(fb, L->msg.x + TOP_BAR_PAD, cy - text_measure_height(TB_TEXT_SCALE) / 2,
                     m, t->message_color, TB_TEXT_SCALE);
    }
    if (L->title_scale) {
        char buf[96];
        text_draw_centered(fb, L->title_x + L->title_w / 2, cy,
                           cut_text(buf, sizeof(buf), t->title, L->title_chars),
                           t->title_color, L->title_scale);
    }
    if (L->info.w > 0)
        fb_draw_text(fb, L->info.x, cy - text_measure_height(TB_TEXT_SCALE) / 2,
                     t->right, TB_INFO_FG, TB_TEXT_SCALE);

    if (t->has_back) button_draw(fb, &t->back);
    if (t->has_exit) button_draw_exit(fb, &t->exit);
}

void top_bar_receipt(const TopBar *t, const char *tag) {
    int pressable = t->L.bottom - SCREEN_SAFE_TOP;
    printf("%s: bar rows %d..%d (%d tall), %d pressable below safe top %d  %s\n",
           tag, t->L.top, t->L.bottom, t->L.h, pressable, SCREEN_SAFE_TOP,
           pressable >= TOP_BAR_MIN_TOUCH_H ? "ok" : "⚠ TOO FEW PRESSABLE ROWS");
}

/* ── The right-hand text ──────────────────────────────────────────────────── */

bool top_bar_info_due(TopBar *t, uint32_t now_ms) {
    if (t->info_next_ms && (int32_t)(now_ms - t->info_next_ms) < 0) return false;
    t->info_next_ms = now_ms + 1000;
    if (t->info_next_ms == 0) t->info_next_ms = 1;
    return true;
}

void top_bar_info_invalidate(TopBar *t) {
    t->info_next_ms = 0;
}

bool top_bar_info_sample(TopBar *t) {
    char stat_buf[2048] = "", up_buf[64] = "";
    FILE *f = fopen("/proc/stat", "r");
    if (f) { size_t n = fread(stat_buf, 1, sizeof(stat_buf) - 1, f); stat_buf[n] = '\0'; fclose(f); }
    f = fopen("/proc/uptime", "r");
    if (f) { size_t n = fread(up_buf, 1, sizeof(up_buf) - 1, f); up_buf[n] = '\0'; fclose(f); }
    CpuSnap cur = status_cpu_snap(stat_buf, up_buf);
    int pct = status_cpu_percent(&t->cpu_prev, &cur, (unsigned)sysconf(_SC_CLK_TCK));
    if (cur.valid) t->cpu_prev = cur;

    time_t now = time(NULL);
    struct tm tm;
    int hour = -1, min = -1;
    if (localtime_r(&now, &tm)) { hour = tm.tm_hour; min = tm.tm_min; }
    char right[sizeof(t->right_buf)];
    status_line_format(right, sizeof(right), pct, hour, min);

    bool changed = strcmp(right, t->right_buf) != 0;
    memcpy(t->right_buf, right, sizeof(right));
    t->right = t->right_buf;
    return changed;
}
