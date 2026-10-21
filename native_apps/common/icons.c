/* icons.c — see icons.h. */
#include "icons.h"

/* Width of the right-pointing triangle of height `t` on its row r (0..t-1): 1 at the top, widest in the
 * middle, 1 at the bottom, 45 degrees either side. */
static int tri_w(int t, int r)
{
    int d = 2 * r + 1 - t;          /* twice the distance from the middle, signed */
    if (d < 0) d = -d;
    return t - d > 0 ? t - d : 0;
}

static int bar_w(int size) { return (size + 3) >> 2; }

int icon_row_spans(IconKind kind, int size, int row, int xs[2 * ICON_MAX_SPANS])
{
    if (size < 2 || row < 0 || row >= size) return 0;
    switch (kind) {
    case ICON_PLAY: {
        int w = tri_w(size, row);
        if (w <= 0) return 0;
        xs[0] = 0; xs[1] = w;
        return 1;
    }
    case ICON_PAUSE: {
        int b = bar_w(size);
        if (size < 2 * b + 1) b = (size - 1) >> 1;
        if (b < 1) return 0;
        xs[0] = 0;        xs[1] = b;
        xs[2] = size - b; xs[3] = size;
        return 2;
    }
    case ICON_STOP: {
        int i = size >> 3;
        if (row < i || row >= size - i) return 0;
        xs[0] = i; xs[1] = size - i;
        return 1;
    }
    case ICON_NEXT: case ICON_PREV: {
        int b = bar_w(size);
        int t = size - b - 1;             /* the triangle's square box; one column of gap before the bar */
        if (t < 1) return 0;
        int off = (size - t) >> 1;
        bool next = kind == ICON_NEXT;
        int n = 0, w = (row >= off && row < off + t) ? tri_w(t, row - off) : 0;
        if (next) {
            if (w > 0) { xs[2 * n] = 0; xs[2 * n + 1] = w; n++; }
            xs[2 * n] = size - b; xs[2 * n + 1] = size; n++;
        } else {
            xs[2 * n] = 0; xs[2 * n + 1] = b; n++;
            if (w > 0) { xs[2 * n] = size - w; xs[2 * n + 1] = size; n++; }
        }
        return n;
    }
    case ICON_DROPDOWN: {
        int h = (size + 1) >> 1;
        int off = (size - h) >> 1;
        int r = row - off;
        if (r < 0 || r >= h) return 0;
        xs[0] = r; xs[1] = size - r;
        return xs[0] < xs[1] ? 1 : 0;
    }
    default: return 0;
    }
}

void icon_draw(Framebuffer *fb, IconKind kind, int x, int y, int size, uint32_t color)
{
    int xs[2 * ICON_MAX_SPANS];
    for (int row = 0; row < size; row++) {
        int n = icon_row_spans(kind, size, row, xs);
        for (int i = 0; i < n; i++)
            fb_fill_rect(fb, x + xs[2 * i], y + row, xs[2 * i + 1] - xs[2 * i], 1, color);
    }
}
