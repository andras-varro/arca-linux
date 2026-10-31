/* frame_grid.c — see frame_grid.h */
#include <string.h>
#include "frame_grid.h"

int frame_thumb_name(const char *name, char *out, size_t n)
{
    if (!name || !*name || strchr(name, '/')) return -1;
    size_t dl = strlen(FRAME_THUMB_DIR), need = dl + 1 + strlen(name);
    if (need + 1 > n) return -1;
    memcpy(out, FRAME_THUMB_DIR, dl);
    out[dl] = '/';
    strcpy(out + dl + 1, name);
    return (int)need;
}

void frame_grid_init(FrameGrid *g, int n, int cols, int rows, uint8_t *marks)
{
    g->n = n < 0 ? 0 : n;
    g->cols = cols < 1 ? 1 : cols;
    g->rows = rows < 1 ? 1 : rows;
    g->cursor = 0;
    g->marks = marks;
    frame_grid_clear_marks(g);
}

int frame_grid_per_page(const FrameGrid *g) { return g->cols * g->rows; }
int frame_grid_pages(const FrameGrid *g)
{
    int p = frame_grid_per_page(g);
    return g->n <= 0 ? 1 : (g->n + p - 1) / p;
}
int frame_grid_page(const FrameGrid *g) { return g->cursor / frame_grid_per_page(g); }

int frame_grid_index(const FrameGrid *g, int page, int slot)
{
    int p = frame_grid_per_page(g);
    if (page < 0 || slot < 0 || slot >= p) return -1;
    int i = page * p + slot;
    return i < g->n ? i : -1;
}

void frame_grid_move(FrameGrid *g, int dx, int dy)
{
    if (g->n <= 0) return;
    int c = g->cursor;
    if (dx) {
        c += dx;
    } else if (dy) {
        int t = c + dy * g->cols;
        if (t < 0) t = c;
        else if (t >= g->n) t = (c / g->cols < (g->n - 1) / g->cols) ? g->n - 1 : c;
        c = t;
    }
    if (c < 0) c = 0;
    if (c >= g->n) c = g->n - 1;
    g->cursor = c;
}

void frame_grid_page_step(FrameGrid *g, int d)
{
    if (g->n <= 0) return;
    int p = frame_grid_per_page(g);
    int t = g->cursor + d * p;
    if (t < 0) t = g->cursor;                       /* no page before the first */
    if (t >= g->n) t = frame_grid_page(g) >= frame_grid_pages(g) - 1 ? g->cursor : g->n - 1;
    g->cursor = t;
}

bool frame_grid_toggle(FrameGrid *g, int i)
{
    if (i < 0 || i >= g->n) return false;
    g->marks[i] = g->marks[i] ? 0 : 1;
    return true;
}

int frame_grid_marked(const FrameGrid *g)
{
    int k = 0;
    for (int i = 0; i < g->n; i++) if (g->marks[i]) k++;
    return k;
}

int frame_grid_collect(const FrameGrid *g, int *out, int cap)
{
    int k = 0;
    for (int i = 0; i < g->n && k < cap; i++) if (g->marks[i]) out[k++] = i;
    return k;
}

void frame_grid_clear_marks(FrameGrid *g)
{
    if (g->n > 0) memset(g->marks, 0, (size_t)g->n);
}

void frame_grid_resize(FrameGrid *g, int n)
{
    g->n = n < 0 ? 0 : n;
    if (g->cursor >= g->n) g->cursor = g->n > 0 ? g->n - 1 : 0;
    frame_grid_clear_marks(g);
}
