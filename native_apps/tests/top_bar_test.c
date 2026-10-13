/* Host-side regression for the shared top bar (common/top_bar.c): its height rule
 * and its layout.  The drawing and the /proc reads are not here; the geometry
 * they stand on is.
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I. -I common \
 *       -o build/top_bar_test tests/top_bar_test.c common/top_bar.c \
 *       common/status_line.c control_panel/cpu_load.c common/common.c \
 *       common/framebuffer.c common/touch_input.c common/hardware.c common/config.c \
 *       common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c \
 *       common/audio_out.c common/audio_wav.c common/start_menu.c common/gamepad.c \
 *       common/input_slots.c common/input_scan.c -lm && ./build/top_bar_test
 *
 * What it asserts, and why:
 *   A  top_bar_bottom(): the bar is >= 44 px from the visible top AND >= 40 rows
 *      of it are below the touchable edge, at EVERY inset 0..48 (the cap
 *      FB_TOUCH_INSET_MAX), and it is tight (one of the two is exact) so it is
 *      never taller than needed.  Literal rows for the measured unit (inset 17)
 *      and for the same unit in panel coordinates (13 / 30).  And the claim the
 *      Control Panel relies on: its content never starts lower than under the
 *      old 44 px page bar, so no page lost a pixel;
 *   B  the sections run left to right in order, inside the pressable extents,
 *      without overlapping; the exit X is flush with the pressable right edge;
 *   C  hits are full bar height - the row at the very top counts, the rule row
 *      under the bar does not - and a section that is absent hits nothing;
 *   D  the title: "Arca Launcher" mixed-case is centred on the screen when it
 *      fits, steps down a scale then is cut with ".." rather than overlapping
 *      its neighbours, and disappears when there is not room for three
 *      characters (portrait, message showing);
 *   E  top_bar_layout() reads the live SCREEN_* macros;
 *   F  top_bar_info_due(): once a second, wrap-safe, first call is due.
 *   G  top_bar_info_sample() follows a zone change: a long-running app never calls
 *      tzset() and glibc localtime_r() does not re-read the zone by itself
 */
#include "common/top_bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static TbExtents ext(int w, int vtop, int inset_top, int inset_x) {
    TbExtents e;
    e.vis_left = 0;           e.vis_right = w;
    e.safe_left = inset_x;    e.safe_right = w - inset_x;
    e.top = vtop;             e.bottom = top_bar_bottom(vtop, inset_top);
    return e;
}

int main(void) {
    /* A — the height rule */
    CHECK(top_bar_bottom(0, 0)   == 44, "A: uncalibrated (inset 0) bar is 44 tall, got %d", top_bar_bottom(0, 0));
    CHECK(top_bar_bottom(0, 17)  == 57, "A: inset 17 (RW09) bar ends at row 57, got %d", top_bar_bottom(0, 17));
    CHECK(top_bar_bottom(13, 30) == 70, "A: panel coordinates 13/30 end at 70 (57 tall), got %d", top_bar_bottom(13, 30));
    CHECK(top_bar_bottom(0, 4)   == 44, "A: inset 4 is still the 44 floor, got %d", top_bar_bottom(0, 4));
    CHECK(top_bar_bottom(0, 5)   == 45, "A: inset 5 is the first the touch rule decides, got %d", top_bar_bottom(0, 5));
    CHECK(top_bar_bottom(0, 48)  == 88, "A: the capped inset 48, got %d", top_bar_bottom(0, 48));
    for (int inset = 0; inset <= 48; inset++) {
        int b = top_bar_bottom(0, inset);
        CHECK(b - 0 >= TOP_BAR_MIN_H, "A: inset %d: bar %d < %d tall", inset, b, TOP_BAR_MIN_H);
        CHECK(b - inset >= TOP_BAR_MIN_TOUCH_H, "A: inset %d: only %d pressable rows", inset, b - inset);
        CHECK(b - 0 == TOP_BAR_MIN_H || b - inset == TOP_BAR_MIN_TOUCH_H,
              "A: inset %d: bar %d is taller than either rule needs", inset, b);
        /* the old page bar put the content at SAFE_TOP + 44 + 2 */
        CHECK(b + 2 <= inset + 46, "A: inset %d: content at %d starts lower than the old %d",
              inset, b + 2, inset + 46);
    }
    /* a visible top that is not 0 shifts the 44 rule with it */
    CHECK(top_bar_bottom(10, 0) == 54, "A: visible top 10, inset 0: 54, got %d", top_bar_bottom(10, 0));

    /* B — landscape launcher: msg + title + info + exit */
    TopBar t;
    top_bar_init(&t, false, true);
    t.title = "Arca Launcher"; t.title_scale = 3;
    t.message = "NOT CALIBRATED";
    t.right = "CPU 7%  14:05";
    TbExtents e = ext(800, 0, 17, 3);
    TopBarLayout L;
    top_bar_compute(&L, &t, &e);
    CHECK(L.top == 0 && L.bottom == 57 && L.h == 57, "B: rows %d..%d h %d", L.top, L.bottom, L.h);
    CHECK(L.back.w == 0, "B: no back button when has_back is false");
    CHECK(L.exit.w == TOP_BAR_BTN_W && L.exit.x + L.exit.w == 800 - 3,
          "B: exit X flush with the pressable right edge: x %d w %d", L.exit.x, L.exit.w);
    CHECK(L.msg.w > 0 && L.msg.x >= e.safe_left, "B: message at x %d w %d", L.msg.x, L.msg.w);
    CHECK(L.info.w == text_measure_width("CPU 7%  14:05", 2), "B: info width %d", L.info.w);
    CHECK(L.info.x + L.info.w <= L.exit.x, "B: info ends %d, exit starts %d", L.info.x + L.info.w, L.exit.x);
    CHECK(L.title_scale == 3 && L.title_chars == -1, "B: title drawn whole at 3, scale %d chars %d", L.title_scale, L.title_chars);
    CHECK(L.title_w == text_measure_width("Arca Launcher", 3), "B: title width %d", L.title_w);
    CHECK(L.title_x == 400 - L.title_w / 2, "B: title centred on the screen: x %d (want %d)", L.title_x, 400 - L.title_w / 2);
    CHECK(L.msg.x + L.msg.w <= L.title_x, "B: message ends %d, title starts %d", L.msg.x + L.msg.w, L.title_x);
    CHECK(L.title_x + L.title_w <= L.info.x, "B: title ends %d, info starts %d", L.title_x + L.title_w, L.info.x);

    /* C — hits */
    CHECK(top_bar_hit(&L, L.exit.x + 5, 0)  == TB_EXIT, "C: the very top row of the X counts");
    CHECK(top_bar_hit(&L, L.exit.x + 5, 56) == TB_EXIT, "C: the last row of the X counts");
    CHECK(top_bar_hit(&L, L.exit.x + 5, 57) == TB_NONE, "C: the rule row under the bar is not the bar");
    CHECK(top_bar_hit(&L, L.exit.x + L.exit.w, 20) == TB_NONE, "C: one past the X's right edge");
    CHECK(top_bar_hit(&L, L.msg.x + 2, 30) == TB_MSG, "C: message target");
    CHECK(top_bar_hit(&L, L.msg.x + L.msg.w, 30) == TB_NONE, "C: one past the message target");
    CHECK(top_bar_hit(&L, L.info.x + 4, 30) == TB_NONE, "C: the info is display only");
    CHECK(top_bar_hit(&L, 10, 30) == TB_NONE || L.msg.x <= 10, "C: no back button means nothing at x 10 but the message");
    CHECK(top_bar_hit(&L, L.exit.x + 5, -1) == TB_NONE, "C: above the bar");
    TopBar nomsg = t; nomsg.message = NULL;
    top_bar_compute(&L, &nomsg, &e);
    CHECK(L.msg.w == 0 && top_bar_hit(&L, 100, 30) == TB_NONE, "C: absent message hits nothing");

    /* B/C — control panel page: back + title + info, no exit */
    TopBar pg;
    top_bar_init(&pg, true, false);
    pg.title = "BLUETOOTH"; pg.title_scale = 3; pg.right = "CPU 100%  14:05";
    top_bar_compute(&L, &pg, &e);
    CHECK(L.back.w == TOP_BAR_BTN_W && L.back.x == e.safe_left, "B: back flush with the pressable left edge: x %d", L.back.x);
    CHECK(L.exit.w == 0, "B: no exit on a page");
    CHECK(L.back.x + L.back.w <= L.title_x, "B: title clear of back");
    CHECK(top_bar_hit(&L, L.back.x + 1, 0) == TB_BACK, "C: back counts in the untouchable band too");
    CHECK(top_bar_hit(&L, L.back.x + L.back.w - 1, 56) == TB_BACK, "C: back, last pixel");
    CHECK(top_bar_hit(&L, L.back.x - 1, 30) == TB_NONE, "C: left of back");

    /* D — a status message in the title's place: long text steps down, then is
     * cut.  Portrait, where the room between back and info is 215 px. */
    TbExtents ep = ext(480, 0, 17, 3);
    pg.title = "BACKUP: /home/root/data/rw.conf.bak-20261012-101500";
    pg.title_scale = 2;
    pg.title_cut = true;
    top_bar_compute(&L, &pg, &ep);
    CHECK(L.title_scale == 1 && L.title_chars > 0, "D: a long status steps to scale 1 and is cut: scale %d chars %d", L.title_scale, L.title_chars);
    pg.title = "SAVED OK"; pg.title_scale = 2;
    {
        TopBarLayout S;
        top_bar_compute(&S, &pg, &ep);
        CHECK(S.title_scale == 2 && S.title_chars == -1, "D: a short status keeps scale 2 and is not cut: scale %d chars %d", S.title_scale, S.title_chars);
    }
    pg.title = "SAVED: AUDIO SETTINGS TO FILE."; pg.title_scale = 2;     /* 30 chars: 360 px at 2, 180 at 1 */
    {
        TopBarLayout S;
        top_bar_compute(&S, &pg, &ep);
        CHECK(S.title_scale == 1 && S.title_chars == -1, "D: a mid-length status steps down to scale 1 whole, uncut: scale %d chars %d", S.title_scale, S.title_chars);
        pg.title_cut = false;
        top_bar_compute(&S, &pg, &ep);
        CHECK(S.title_scale == 1 && S.title_chars == -1, "D: a fixed title steps down too: scale %d", S.title_scale);
        pg.title = "A FIXED TITLE THAT IS FAR TOO LONG FOR ANY SCALE ON THIS PORTRAIT BAR";
        top_bar_compute(&S, &pg, &ep);
        CHECK(S.title_scale == 0, "D: a fixed title that fits at no scale is dropped, not cut: scale %d", S.title_scale);
        pg.title_cut = true;
    }
    pg.title = "BACKUP: /home/root/data/rw.conf.bak-20261012-101500";
    pg.title_scale = 2;
    {
        int drawn = (L.title_chars + 2) * 6 * L.title_scale;
        CHECK(drawn <= L.info.x - (L.back.x + L.back.w) - 2 * TOP_BAR_PAD,
              "D: the cut text (%d px) must fit the room between back and info", drawn);
        CHECK(L.title_x >= L.back.x + L.back.w + TOP_BAR_PAD && L.title_x + drawn <= L.info.x - TOP_BAR_PAD,
              "D: cut title %d..%d overlaps back (%d) or info (%d)", L.title_x, L.title_x + drawn, L.back.x + L.back.w, L.info.x);
    }
    pg.title = "OK"; pg.title_scale = 3;
    top_bar_compute(&L, &pg, &ep);
    CHECK(L.title_scale == 3 && L.title_chars == -1, "D: a short title keeps its scale");

    /* D — portrait (480 wide): the launcher with the message up has no room for
     * the title, and a fixed title (title_cut false) is dropped, not mangled */
    TopBarLayout P;
    top_bar_compute(&P, &t, &ep);
    CHECK(P.title_scale == 0, "D: portrait with a message: no room, the title is not drawn (scale %d)", P.title_scale);
    CHECK(P.msg.x + P.msg.w <= P.info.x, "D: portrait message %d..%d overlaps info %d", P.msg.x, P.msg.x + P.msg.w, P.info.x);
    TopBar quiet = t; quiet.message = NULL;
    top_bar_compute(&P, &quiet, &ep);
    CHECK(P.title_scale >= 1 && P.title_x + P.title_w <= P.info.x && P.title_x >= ep.safe_left,
          "D: portrait without a message draws the title inside the room: scale %d x %d w %d info %d",
          P.title_scale, P.title_x, P.title_w, P.info.x);

    /* E — against the live screen */
    screen_base_width = 800; screen_base_height = 480;
    screen_touch_inset_top = 17; screen_touch_inset_bottom = 17;
    screen_touch_inset_left = 3; screen_touch_inset_right = 3;
    TopBar live;
    top_bar_init(&live, true, true);
    live.title = "X";
    top_bar_layout(&live);
    CHECK(live.L.bottom == 57 && live.L.back.x == 3 && live.L.exit.x == 800 - 3 - TOP_BAR_BTN_W,
          "E: live layout: bottom %d back %d exit %d", live.L.bottom, live.L.back.x, live.L.exit.x);
    CHECK(live.back.x == live.L.back.x && live.back.y == 0 && live.back.height == 57 && live.back.width == TOP_BAR_BTN_W,
          "E: back Button follows the layout: %d,%d %dx%d", live.back.x, live.back.y, live.back.width, live.back.height);
    CHECK(live.exit.x == live.L.exit.x && live.exit.height == 57, "E: exit Button follows the layout");
    CHECK(TOP_BAR_BOTTOM == 57 && TOP_BAR_GRID_RESERVE == 57 - 17 + TOP_BAR_GRID_GAP,
          "E: TOP_BAR_BOTTOM %d reserve %d", TOP_BAR_BOTTOM, TOP_BAR_GRID_RESERVE);
    screen_touch_inset_top = 0;
    top_bar_layout(&live);
    CHECK(live.L.bottom == 44, "E: re-layout after the inset changes: bottom %d", live.L.bottom);

    /* E2 — the buttons are DRAWN inside the bar with a margin, but keep the full hit target */
    screen_touch_inset_top = 17;
    top_bar_layout(&live);
    UiRect bd = top_bar_button_draw_rect(&live.L, TB_BACK);
    UiRect xd = top_bar_button_draw_rect(&live.L, TB_EXIT);
    CHECK(bd.x == live.L.back.x + TOP_BAR_BTN_INSET && bd.y == live.L.top + TOP_BAR_BTN_INSET &&
          bd.w == TOP_BAR_BTN_W - 2 * TOP_BAR_BTN_INSET && bd.h == live.L.h - 2 * TOP_BAR_BTN_INSET,
          "E2: back drawn rect %d,%d %dx%d", bd.x, bd.y, bd.w, bd.h);
    CHECK(xd.x + xd.w == live.L.exit.x + live.L.exit.w - TOP_BAR_BTN_INSET && xd.h == live.L.h - 2 * TOP_BAR_BTN_INSET,
          "E2: exit drawn rect %d,%d %dx%d", xd.x, xd.y, xd.w, xd.h);
    CHECK(top_bar_hit(&live.L, live.L.back.x, live.L.top) == TB_BACK &&
          top_bar_hit(&live.L, live.L.exit.x + live.L.exit.w - 1, live.L.bottom - 1) == TB_EXIT,
          "E2: the hit target still reaches the corners the drawing no longer covers");
    CHECK(top_bar_button_draw_rect(&live.L, TB_MSG).w == 0, "E2: only back and exit have a drawn rect");
    CHECK(live.back.width == TOP_BAR_BTN_W && live.back.height == live.L.h, "E2: the Button keeps its full hit rectangle");
    screen_touch_inset_top = 0;
    top_bar_layout(&live);

    /* F — the once-a-second clock */
    TopBar c;
    top_bar_init(&c, false, false);
    CHECK(top_bar_info_due(&c, 5000), "F: first call is due");
    CHECK(!top_bar_info_due(&c, 5999), "F: 999 ms later is not");
    CHECK(top_bar_info_due(&c, 6000), "F: 1000 ms later is");
    top_bar_info_invalidate(&c);
    CHECK(top_bar_info_due(&c, 6001), "F: invalidated: due at once");
    TopBar w;
    top_bar_init(&w, false, false);
    CHECK(top_bar_info_due(&w, 0xFFFFFF00u), "F: wrap: first");
    CHECK(!top_bar_info_due(&w, 0xFFFFFF10u), "F: wrap: before the deadline");
    CHECK(top_bar_info_due(&w, 0x00000300u), "F: wrap: past the deadline across 2^32");

    /* G — the clock follows a zone change without a restart.  A long-running app
     * (the launcher) never calls tzset() itself, and glibc's localtime_r() does
     * not re-read the zone after its first call, so the sample must.  Stand-in for
     * the device's /etc/localtime swap: the TZ variable, which tzset() re-reads
     * on a changed value (on the device TZ is unset and tzset() stats the file). */
    {
        TopBar z;
        top_bar_init(&z, false, false);
        setenv("TZ", "UTC0", 1);
        top_bar_info_sample(&z);
        int h0 = -1, m0 = -1;
        const char *clk = strrchr(z.right_buf, ' ');
        CHECK(clk && sscanf(clk + 1, "%d:%d", &h0, &m0) == 2, "G: parse clock from '%s'", z.right_buf);
        setenv("TZ", "XXX-5", 1);                 /* five hours east of UTC0 */
        top_bar_info_sample(&z);
        int h1 = -1, m1 = -1;
        clk = strrchr(z.right_buf, ' ');
        CHECK(clk && sscanf(clk + 1, "%d:%d", &h1, &m1) == 2, "G: parse clock from '%s'", z.right_buf);
        CHECK(h1 == (h0 + 5) % 24, "G: zone change UTC0 -> UTC+5 must move the clock 5 h (%02d:%02d -> %02d:%02d)",
              h0, m0, h1, m1);
        unsetenv("TZ");
    }

    if (fails) { printf("top_bar_test: %d FAILED\n", fails); return 1; }
    printf("top_bar_test: all passed\n");
    return 0;
}
