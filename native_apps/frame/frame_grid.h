/*
 * frame_grid.h — the picture grid's pure logic: where a library picture's thumbnail lives, which picture is on
 * which page, how the cursor moves, and which pictures are marked for deletion.  No I/O, no framebuffer;
 * tests/frame_grid_test.c drives it on the host.
 */
#ifndef FRAME_GRID_H
#define FRAME_GRID_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_THUMB_W    160
#define FRAME_THUMB_H    120
#define FRAME_THUMB_DIR  ".thumbs"      /* inside the library; a hidden name, so the slideshow's scan never lists it */

/* The thumbnail of library picture `name`, relative to the library directory: ".thumbs/<name>".  Returns the
 * length, or -1 for an empty name, a name holding '/', or a buffer that is too small. */
int frame_thumb_name(const char *name, char *out, size_t n);

typedef struct {
    int      n;                 /* pictures in the library */
    int      cols, rows;        /* tiles per page: cols x rows */
    int      cursor;            /* picture under the cursor, 0..n-1 (0 when n == 0) */
    uint8_t *marks;             /* caller-owned, n bytes, non-zero = marked for deletion */
} FrameGrid;

void frame_grid_init(FrameGrid *g, int n, int cols, int rows, uint8_t *marks);
int  frame_grid_per_page(const FrameGrid *g);
int  frame_grid_pages(const FrameGrid *g);              /* at least 1 */
int  frame_grid_page(const FrameGrid *g);               /* page holding the cursor */
int  frame_grid_index(const FrameGrid *g, int page, int slot);   /* picture in that tile, -1 when the tile is empty */
void frame_grid_move(FrameGrid *g, int dx, int dy);     /* one tile; down off a partial last row lands on the last picture */
void frame_grid_page_step(FrameGrid *g, int d);         /* a page back / forward, same tile where it exists, else the last picture */
bool frame_grid_toggle(FrameGrid *g, int i);            /* flip the mark of picture i; false when i is out of range */
int  frame_grid_marked(const FrameGrid *g);
int  frame_grid_collect(const FrameGrid *g, int *out, int cap);   /* indices of marked pictures, ascending */
void frame_grid_clear_marks(FrameGrid *g);
/* The library shrank to n pictures (marks cleared, cursor clamped). */
void frame_grid_resize(FrameGrid *g, int n);
#endif
