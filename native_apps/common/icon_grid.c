/*
 * icon_grid — see icon_grid.h. Extracted from app_launcher.c unchanged in what it
 * draws, so the launcher renders the same pixels it did before the move.
 */
#include "icon_grid.h"
#include "common.h"
#include "ppm.h"
#include <stdio.h>
#include <stdlib.h>

#define TILE_GAP_X      20
#define TILE_GAP_Y      20
#define LABEL_SCALE     2
#define DOTS_H          30
#define MAX_TILE_W      200
#define MAX_TILE_H      150
#define TILE_RADIUS     ICON_GRID_RING_RADIUS
#define PAGE_EDGE_BAND  50

#define TILE_BG         RGB(45, 45, 60)
#define TILE_BORDER     RGB(70, 70, 90)
#define TILE_HL_BG      RGB(65, 65, 85)
#define TILE_SEL_BORDER RGB(0, 220, 255)
#define LABEL_COLOR     RGB(220, 220, 220)
#define DOT_ACTIVE      RGB(200, 200, 220)
#define DOT_INACTIVE    RGB(80, 80, 100)
#define CHEVRON_COLOR   RGB(150, 150, 170)

static const uint32_t LETTER_COLORS[] = {
    0xFF2980B9,  /* Blue        */
    0xFF27AE60,  /* Green       */
    0xFFC0392B,  /* Red         */
    0xFF8E44AD,  /* Purple      */
    0xFFF39C12,  /* Orange      */
    0xFF16A085,  /* Teal        */
    0xFFD35400,  /* Dark Orange */
    0xFF34495E,  /* Dark Slate  */
};
#define NUM_LETTER_COLORS (sizeof(LETTER_COLORS) / sizeof(LETTER_COLORS[0]))

void icon_grid_layout(IconGrid *g, const Framebuffer *fb, int top_reserve,
                      const char *tag) {
    if (fb->portrait_mode) {
        g->cols = 2;
        g->rows = 3;
    } else {
        g->cols = 3;
        g->rows = 2;
    }
    g->per_page = g->cols * g->rows;

    /* Tile sizes fit the available space, capped at the maximum */
    int max_w   = (SCREEN_SAFE_WIDTH - (g->cols - 1) * TILE_GAP_X) / g->cols;
    int avail_h = SCREEN_SAFE_HEIGHT - top_reserve - DOTS_H;
    int max_h   = (avail_h - (g->rows - 1) * TILE_GAP_Y) / g->rows;

    g->tile_w = (max_w < MAX_TILE_W) ? max_w : MAX_TILE_W;
    g->tile_h = (max_h < MAX_TILE_H) ? max_h : MAX_TILE_H;

    /* Centre the grid within the safe area */
    g->content_w = g->cols * g->tile_w + (g->cols - 1) * TILE_GAP_X;
    g->content_h = g->rows * g->tile_h + (g->rows - 1) * TILE_GAP_Y;

    g->left = SCREEN_SAFE_LEFT + (SCREEN_SAFE_WIDTH - g->content_w) / 2;
    g->top  = SCREEN_SAFE_TOP + top_reserve + (avail_h - g->content_h) / 2;

    /* ⚠️ The RECEIPT. A layout that puts a row past the bottom of the touchable
     * rect looks perfect in a framebuffer screenshot and is simply dead to a
     * finger, so the derivation prints itself and says whether it fits. */
    bool fits = (g->top + g->content_h) <= SCREEN_SAFE_BOTTOM;
    printf("%s: safe %dx%d at (%d,%d)  tiles %dx%d %dx%d  grid_top %d "
           "grid_h %d %s\n",
           tag, SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT, SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP,
           g->cols, g->rows, g->tile_w, g->tile_h, g->top,
           g->content_h, fits ? "fits" : "⚠ PAST SAFE BOTTOM");
}

int icon_grid_pages(const IconGrid *g, int count) {
    int pages = (count + g->per_page - 1) / g->per_page;
    return pages < 1 ? 1 : pages;
}

void icon_grid_tile_xy(const IconGrid *g, int idx_on_page, int *x, int *y) {
    int col = idx_on_page % g->cols;
    int row = idx_on_page / g->cols;
    *x = g->left + col * (g->tile_w + TILE_GAP_X);
    *y = g->top  + row * (g->tile_h + TILE_GAP_Y);
}

static void draw_letter_icon(Framebuffer *fb, int x, int y,
                             char letter, uint32_t color) {
    fb_fill_rect(fb, x, y, ICON_GRID_ICON_SIZE, ICON_GRID_ICON_SIZE, color);
    fb_draw_rect(fb, x, y, ICON_GRID_ICON_SIZE, ICON_GRID_ICON_SIZE, COLOR_WHITE);

    char s[2] = { letter, '\0' };
    text_draw_centered(fb, x + ICON_GRID_ICON_SIZE / 2, y + ICON_GRID_ICON_SIZE / 2,
                       s, COLOR_WHITE, 7);
}

static void draw_ppm_icon(Framebuffer *fb, int x, int y, const uint32_t *pixels) {
    for (int py = 0; py < ICON_GRID_ICON_SIZE; py++)
        for (int px = 0; px < ICON_GRID_ICON_SIZE; px++)
            fb_draw_pixel(fb, x + px, y + py, pixels[py * ICON_GRID_ICON_SIZE + px]);
}

void icon_grid_draw_tile(Framebuffer *fb, const IconGrid *g, int tx, int ty,
                         const char *label, const uint32_t *icon,
                         uint32_t letter_color, bool highlight) {
    fb_fill_rounded_rect(fb, tx, ty, g->tile_w, g->tile_h, TILE_RADIUS,
                         highlight ? TILE_HL_BG : TILE_BG);
    fb_draw_rounded_rect(fb, tx, ty, g->tile_w, g->tile_h, TILE_RADIUS, TILE_BORDER);

    /* Icon — centred horizontally, 8 px from top of tile */
    int icon_x = tx + (g->tile_w - ICON_GRID_ICON_SIZE) / 2;
    int icon_y = ty + 8;

    if (icon) {
        draw_ppm_icon(fb, icon_x, icon_y, icon);
    } else {
        char letter = label[0];
        if (letter >= 'a' && letter <= 'z') letter -= 32;
        draw_letter_icon(fb, icon_x, icon_y, letter, letter_color);
    }

    /* Label — centred below icon */
    int label_y = icon_y + ICON_GRID_ICON_SIZE + 10;
    text_draw_centered(fb, tx + g->tile_w / 2, label_y, label, LABEL_COLOR, LABEL_SCALE);
}

void icon_grid_draw_ring(Framebuffer *fb, int x, int y, int w, int h) {
    icon_grid_draw_ring_r(fb, x, y, w, h, TILE_RADIUS);
}

void icon_grid_draw_ring_r(Framebuffer *fb, int x, int y, int w, int h, int radius) {
    int bw = 3;
    if (radius > (h >> 1)) radius = h >> 1;     /* a short widget: no corner overlap */
    for (int b = 0; b < bw; b++)
        fb_draw_rounded_rect(fb, x - bw + b, y - bw + b,
                             w + 2 * (bw - b), h + 2 * (bw - b),
                             radius + bw - b, TILE_SEL_BORDER);
}

void icon_grid_draw_selection(Framebuffer *fb, const IconGrid *g, int tx, int ty) {
    icon_grid_draw_ring(fb, tx, ty, g->tile_w, g->tile_h);
}

void icon_grid_draw_paging(Framebuffer *fb, const IconGrid *g, int page, int pages) {
    /* Chevrons are decoration only — icon_grid_page_hit() hit-tests the page-flip
     * bands against SCREEN_SAFE_*. They sit in the visible band beside the grid. */
    int ay = g->top + g->content_h / 2 - 10;
    if (page > 0)
        fb_draw_text(fb, SCREEN_VISIBLE_LEFT + 10, ay, "<", CHEVRON_COLOR, 3);
    if (page < pages - 1)
        fb_draw_text(fb, SCREEN_VISIBLE_RIGHT - 20, ay, ">", CHEVRON_COLOR, 3);

    if (pages <= 1) return;

    int dot_r   = 5;
    int dot_gap = 20;
    int total_w = pages * dot_r * 2 + (pages - 1) * (dot_gap - dot_r * 2);
    int sx = fb->width / 2 - total_w / 2;
    int y  = SCREEN_VISIBLE_BOTTOM - 15;
    for (int i = 0; i < pages; i++)
        fb_fill_circle(fb, sx + i * dot_gap, y, dot_r,
                       (i == page) ? DOT_ACTIVE : DOT_INACTIVE);
}

int icon_grid_hit(const IconGrid *g, int count_on_page, int x, int y) {
    for (int i = 0; i < count_on_page; i++) {
        int tx, ty;
        icon_grid_tile_xy(g, i, &tx, &ty);
        if (x >= tx && x < tx + g->tile_w && y >= ty && y < ty + g->tile_h)
            return i;
    }
    return -1;
}

int icon_grid_page_hit(int x, int page, int pages) {
    if (x < SCREEN_SAFE_LEFT + PAGE_EDGE_BAND && page > 0) return -1;
    if (x > SCREEN_SAFE_RIGHT - PAGE_EDGE_BAND && page < pages - 1) return 1;
    return 0;
}

uint32_t *icon_grid_load_icon(const char *path) {
    if (!path || !path[0]) return NULL;

    int w, h;
    uint32_t *raw = ppm_load(path, &w, &h);
    if (!raw) return NULL;
    if (w == ICON_GRID_ICON_SIZE && h == ICON_GRID_ICON_SIZE) return raw;

    uint32_t *scaled = ppm_scale(raw, w, h, ICON_GRID_ICON_SIZE, ICON_GRID_ICON_SIZE);
    free(raw);
    return scaled;
}

uint32_t icon_grid_letter_color(const char *label) {
    unsigned h = 0;
    for (const char *p = label; *p; p++)
        h = h * 31 + (unsigned char)*p;
    return LETTER_COLORS[h % NUM_LETTER_COLORS];
}

int icon_grid_nav(const IconGrid *g, int count, int cur, UiDir d) {
    if (cur < 0) return cur;
    switch (d) {
    case UI_DIR_RIGHT: return cur + 1 < count       ? cur + 1       : 0;
    case UI_DIR_LEFT:  return cur > 0               ? cur - 1       : count - 1;
    case UI_DIR_DOWN:  return cur + g->cols < count ? cur + g->cols : cur;
    default:           return cur - g->cols >= 0    ? cur - g->cols : cur;
    }
}

int icon_grid_nav_exit(const IconGrid *g, int count, int page, int cur,
                       int *from, UiDir d, bool has_msg) {
    int base = page * g->per_page;
    if (cur == ICON_GRID_NAV_MSG && !has_msg) cur = ICON_GRID_NAV_EXIT;
    if (cur == ICON_GRID_NAV_MSG)
        return (d == UI_DIR_RIGHT || d == UI_DIR_DOWN) ? ICON_GRID_NAV_EXIT : cur;
    if (cur == ICON_GRID_NAV_EXIT) {
        if (has_msg && (d == UI_DIR_LEFT || d == UI_DIR_UP)) return ICON_GRID_NAV_MSG;
        if (d != UI_DIR_DOWN) return cur;
        int t = *from;
        return (t >= base && t < base + g->per_page && t < count) ? t : base;
    }
    if (d == UI_DIR_UP && cur >= base && cur < base + g->cols) {
        *from = cur;
        return ICON_GRID_NAV_EXIT;
    }
    return icon_grid_nav(g, count, cur, d);
}

/* ── Home-grid focus model (icon_grid.h) ──────────────────────────────────── */

void icon_grid_focus_init(IconGridFocus *f) {
    f->sel = -1;
    f->shown = false;
    f->exit_from = -1;
}

int icon_grid_focus_dir(bool up, bool down, bool left, bool right) {
    return up ? UI_DIR_UP : down ? UI_DIR_DOWN : left ? UI_DIR_LEFT
         : right ? UI_DIR_RIGHT : -1;
}

/* The shown page's tile in the selection's slot, clamped to what the page
 * holds; the X when there are no tiles. */
static int focus_anchor(const IconGrid *g, int count, int page, int slot) {
    if (count <= 0) return ICON_GRID_NAV_EXIT;
    int base = page * g->per_page;
    int idx = base + (slot > 0 ? slot : 0);
    return idx < count ? idx : count - 1;
}

int icon_grid_focus_frame(const IconGrid *g, int count, int *page,
                          IconGridFocus *f, bool touch_press, int dir,
                          bool enter, bool has_msg) {
    int pages = icon_grid_pages(g, count);
    if (*page >= pages) *page = pages - 1;
    if (*page < 0) *page = 0;

    if (touch_press) f->shown = false;

    /* The message went while selected: the X, ring state kept. */
    if (f->sel == ICON_GRID_NAV_MSG && !has_msg) f->sel = ICON_GRID_NAV_EXIT;

    /* The page invariant: a list that shrank, then a page flipped under the
     * selection. */
    if (f->sel >= count) f->sel = count > 0 ? count - 1 : -1;
    if (f->sel >= 0 && f->sel / g->per_page != *page)
        f->sel = focus_anchor(g, count, *page, f->sel % g->per_page);

    bool seen = f->shown;
    if (dir >= 0 || enter) {
        if (!f->shown) {
            f->shown = true;
            if (f->sel == -1) f->sel = focus_anchor(g, count, *page, 0);
        } else if (dir >= 0) {
            if (f->sel == -1) f->sel = focus_anchor(g, count, *page, 0);
            int next = icon_grid_nav_exit(g, count, *page, f->sel,
                                          &f->exit_from, (UiDir)dir, has_msg);
            if (next < count) {           /* Down from the X with no tiles stays */
                f->sel = next;
                if (next >= 0) *page = next / g->per_page;
            }
        }
    }

    if (enter && seen && dir < 0 && f->sel != -1) return f->sel;
    return -1;
}

void icon_grid_focus_land(const IconGrid *g, IconGridFocus *f, int *page,
                          int idx) {
    f->sel = idx;
    if (idx >= 0) *page = idx / g->per_page;
}
