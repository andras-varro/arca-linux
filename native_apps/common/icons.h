/*
 * icons — small filled media glyphs (play, pause, stop, next, previous, drop-down arrow) drawn as
 * rectangles on the framebuffer, so a button can carry a symbol the 5x7 font has no glyph for.
 *
 * icon_row_spans() is the geometry and is pure (no framebuffer, no divide): for a glyph in a
 * size x size box it gives the filled [x0, x1) runs of one row.  icon_draw() is a loop over it, so
 * tests/icons_test.c checks the very shapes that reach the panel.
 */
#ifndef ARCA_ICONS_H
#define ARCA_ICONS_H

#include <stdint.h>
#include "framebuffer.h"

typedef enum {
    ICON_PLAY,       /* triangle, apex right */
    ICON_PAUSE,      /* two bars */
    ICON_STOP,       /* square */
    ICON_NEXT,       /* triangle, apex right, then a bar */
    ICON_PREV,       /* bar, then a triangle with its apex left */
    ICON_DROPDOWN,   /* small triangle, apex down */
    ICON_COUNT
} IconKind;

#define ICON_MAX_SPANS 2

/** The filled runs of row `row` (0 = top) of glyph `kind` in a `size` x `size` box: run i is
 *  [xs[2*i], xs[2*i+1]).  Returns the run count (0 for a blank row, an unknown kind, a row outside
 *  the box or a size under 2).  Runs lie inside [0, size), do not touch each other and are ordered. */
int icon_row_spans(IconKind kind, int size, int row, int xs[2 * ICON_MAX_SPANS]);

/** Draw glyph `kind` with its box's top-left at (x, y). */
void icon_draw(Framebuffer *fb, IconKind kind, int x, int y, int size, uint32_t color);

#endif /* ARCA_ICONS_H */
