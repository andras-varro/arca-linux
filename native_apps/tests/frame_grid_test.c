/*
 * frame_grid_test.c — host regression for frame/frame_grid.c: the thumbnail name, paging, cursor movement
 * (including the partial last row and page edges), marking and the delete set.
 *
 * Build: gcc -I frame tests/frame_grid_test.c frame/frame_grid.c -o frame_grid_test
 */
#include <stdio.h>
#include <string.h>
#include "frame_grid.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    char o[64];
    CHECK(frame_thumb_name("a-1-ff.jpg", o, sizeof o) == 18 && strcmp(o, ".thumbs/a-1-ff.jpg") == 0);
    CHECK(frame_thumb_name("a/b.jpg", o, sizeof o) < 0 && frame_thumb_name("", o, sizeof o) < 0 && frame_thumb_name(NULL, o, sizeof o) < 0);
    CHECK(frame_thumb_name("a-1-ff.jpg", o, 18) < 0 && frame_thumb_name("a-1-ff.jpg", o, 19) == 18);   /* exact fit: NUL included */
    CHECK(o[0] == '.');                                                                                /* hidden: never a slide */

    uint8_t mk[40];
    FrameGrid g;
    frame_grid_init(&g, 33, 4, 3, mk);                  /* 12 per page, 3 pages */
    CHECK(frame_grid_per_page(&g) == 12 && frame_grid_pages(&g) == 3 && frame_grid_page(&g) == 0);
    CHECK(frame_grid_index(&g, 2, 8) == 32 && frame_grid_index(&g, 2, 9) == -1 && frame_grid_index(&g, 3, 0) == -1 && frame_grid_index(&g, 0, 12) == -1);
    frame_grid_move(&g, 1, 0); CHECK(g.cursor == 1);
    frame_grid_move(&g, -1, 0); frame_grid_move(&g, -1, 0); CHECK(g.cursor == 0);
    frame_grid_move(&g, 0, 1); CHECK(g.cursor == 4);
    frame_grid_move(&g, 0, -1); frame_grid_move(&g, 0, -1); CHECK(g.cursor == 0);
    g.cursor = 11; frame_grid_move(&g, 1, 0); CHECK(g.cursor == 12 && frame_grid_page(&g) == 1);        /* right off a page turns it */
    g.cursor = 31; frame_grid_move(&g, 0, 1); CHECK(g.cursor == 32);                                     /* partial last row */
    frame_grid_move(&g, 0, 1); CHECK(g.cursor == 32);
    frame_grid_move(&g, 1, 0); CHECK(g.cursor == 32);
    g.cursor = 5; frame_grid_page_step(&g, 1); CHECK(g.cursor == 17);
    frame_grid_page_step(&g, 1); CHECK(g.cursor == 29);
    g.cursor = 22; frame_grid_page_step(&g, 1); CHECK(g.cursor == 32);                                 /* short last page: last picture */
    frame_grid_page_step(&g, 1); CHECK(g.cursor == 32);
    frame_grid_page_step(&g, -1); CHECK(g.cursor == 20);
    g.cursor = 3; frame_grid_page_step(&g, -1); CHECK(g.cursor == 3);

    CHECK(frame_grid_marked(&g) == 0);
    CHECK(frame_grid_toggle(&g, 4) && frame_grid_toggle(&g, 20) && frame_grid_toggle(&g, 32));
    CHECK(!frame_grid_toggle(&g, 33) && !frame_grid_toggle(&g, -1));
    CHECK(frame_grid_marked(&g) == 3);
    frame_grid_toggle(&g, 20); CHECK(frame_grid_marked(&g) == 2);
    int del[8];
    CHECK(frame_grid_collect(&g, del, 8) == 2 && del[0] == 4 && del[1] == 32);
    CHECK(frame_grid_collect(&g, del, 1) == 1 && del[0] == 4);
    g.cursor = 32; frame_grid_resize(&g, 31);                                                            /* two deleted */
    CHECK(g.n == 31 && g.cursor == 30 && frame_grid_marked(&g) == 0 && frame_grid_pages(&g) == 3);
    frame_grid_resize(&g, 0);
    CHECK(g.cursor == 0 && frame_grid_pages(&g) == 1);
    frame_grid_move(&g, 1, 1); frame_grid_page_step(&g, 1); CHECK(g.cursor == 0 && !frame_grid_toggle(&g, 0));

    frame_grid_init(&g, 12, 4, 3, mk);                  /* exactly one page */
    CHECK(frame_grid_pages(&g) == 1);
    g.cursor = 11; frame_grid_page_step(&g, 1); CHECK(g.cursor == 11);
    frame_grid_init(&g, 13, 4, 3, mk);
    CHECK(frame_grid_pages(&g) == 2);

    printf(fails ? "frame_grid_test: %d FAILED\n" : "frame_grid_test: all passed\n", fails);
    return fails ? 1 : 0;
}
