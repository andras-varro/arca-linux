/* Host-side regression for common/ui_zone.c — pad/keyboard focus over a stack of zones.
 *
 * Runs on the DEV MACHINE with native gcc; the module is pure arithmetic over rectangles and a ListView's cursor and
 * count, so nothing here needs a device.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/ui_zone_test tests/ui_zone_test.c common/ui_zone.c common/ui_focus.c \
 *       && ./build/ui_zone_test
 *
 * The fixtures are the SHAPES the Jellyfin list screens hand in, written by hand: a top bar (BACK, EXIT), a list, and
 * a button row under it (ADD, CLOSE); plus a wrapped two-row zone and a one-row trap zone (the action bar).  The
 * decisive case is Down at the last row: it must land on the button row and STAY there, with the list not taking the
 * same key (the same-frame bounce this module exists to prevent).
 *
 * What it cannot see: that a screen builds its zones from the same rectangles it hit-tests, and the drawn ring; both
 * are the screen's, judged on the panel.  The list is a bare ListView with only cursor and count set.
 */
#include "ui_zone.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static InputState key(int btn) {
    InputState in;
    memset(&in, 0, sizeof(in));
    if (btn >= 0) in.buttons[btn].pressed = true;
    return in;
}

static void set_list(ListView *lv, int count, int cursor) {
    memset(lv, 0, sizeof(*lv));
    lv->count = count;
    lv->cursor = cursor;
}

static void btn(UiZone *z, int x, int y, int id) {
    z->r[z->n] = (UiRect){ x, y, 120, 40 };
    z->id[z->n] = id;
    z->n++;
}

/* zones: 0 bar (BACK 0, EXIT 1), 1 list, 2 row (ADD 10 [if add], CLOSE 11) */
static void screen(UiZone z[3], ListView *lv, bool add) {
    memset(z, 0, 3 * sizeof(UiZone));
    btn(&z[0], 10, 5, 0);
    btn(&z[0], 600, 5, 1);
    z[1].lv = lv;
    if (add) btn(&z[2], 100, 420, 10);
    btn(&z[2], 300, 420, 11);
}

int main(void) {
    ListView lv;
    UiZone z[3];
    UiZoneFocus f;
    UzOut o;
    InputState in;

    /* 1. Down at the last row: onto the button row, and it stays there; the list does not also take the key. */
    set_list(&lv, 5, 4);
    screen(z, &lv, true);
    uz_begin(&f, 1);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 10);
    CHECK(o.moved && o.list_in == NULL && o.activate == -1 && o.left == -1);
    CHECK(lv.cursor == 4);
    in = key(-1);
    uz_input(&f, z, 3, &in, false, &o);        /* a quiet frame: still there */
    CHECK(f.zone == 2 && f.id == 10 && !o.moved);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate == 10 && f.zone == 2);

    /* 2. Up at row 0: onto the bar.  Mid-list Up and Down leave the key to the list. */
    set_list(&lv, 5, 0);
    uz_begin(&f, 1);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0 && f.id == 0 && o.list_in == NULL);
    set_list(&lv, 5, 2);
    uz_begin(&f, 1);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && o.list_in == &in && o.moved);   /* only `shown` changed */
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && o.list_in == &in);

    /* 3. Left/Right in a list zone are not consumed; ACTION is the list's too. */
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && o.list_in == &in && o.activate == -1);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && o.list_in == &in && o.activate == -1);

    /* 4. Walking down the whole screen: bar -> list is one press, list -> row one press, nothing beyond the row. */
    set_list(&lv, 1, 0);
    uz_begin(&f, 0);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && f.id == -1);
    uz_input(&f, z, 3, &in, false, &o);        /* one row: Down is at the last row at once */
    CHECK(f.zone == 2 && f.id == 10);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 10 && !o.moved);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && f.id == -1);

    /* 5. Left/Right inside the row wrap; ACTION and JUMP activate the focused id. */
    uz_begin(&f, 2);
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.id == 11 && o.moved);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.id == 10);                         /* wrapped */
    in = key(BTN_ID_LEFT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.id == 11);
    in = key(BTN_ID_JUMP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate == 11);

    /* 6. Absent button: the focused id vanishes -> the nearest present one; reappearing keeps focus where it is. */
    uz_begin(&f, 2);
    uz_input(&f, z, 3, NULL, false, &o);       /* resolves to the first button */
    CHECK(f.id == 10);
    screen(z, &lv, false);                     /* ADD gone */
    uz_input(&f, z, 3, NULL, false, &o);
    CHECK(f.zone == 2 && f.id == 11 && o.moved);
    screen(z, &lv, true);                      /* ADD back */
    uz_input(&f, z, 3, NULL, false, &o);
    CHECK(f.zone == 2 && f.id == 11 && !o.moved);
    /* the row empties under the focus -> the list zone */
    z[2].n = 0;
    uz_input(&f, z, 3, NULL, false, &o);
    CHECK(f.zone == 1 && f.id == -1 && o.moved);
    /* an absent zone is skipped by Down */
    set_list(&lv, 3, 2);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && o.moved && o.list_in == &in);   /* nothing below: the list keeps it */
    screen(z, &lv, true);
    memset(&z[2], 0, sizeof(z[2]));            /* row absent, then an action zone below it */
    z[2].n = 0;
    UiZone z4[4];
    memcpy(z4, z, 3 * sizeof(UiZone));
    memset(&z4[3], 0, sizeof(UiZone));
    btn(&z4[3], 50, 440, 20);
    uz_begin(&f, 1);
    uz_input(&f, z4, 4, &in, false, &o);
    CHECK(f.zone == 3 && f.id == 20);

    /* 7. A touch changes neither zone nor id and hides the ring. */
    set_list(&lv, 5, 4);
    screen(z, &lv, true);
    uz_begin(&f, 2);
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.shown && f.id == 11);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, true, &o);
    CHECK(!f.shown && f.zone == 2 && f.id == 11 && o.list_in == NULL && o.activate == -1 && o.moved);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, true, &o);
    CHECK(o.activate == -1 && f.zone == 2);

    /* 8. The ring: hidden until a key, never in a list zone. */
    UiRect rr;
    uz_begin(&f, 2);
    uz_input(&f, z, 3, NULL, false, &o);
    CHECK(!uz_ring(&f, z, 3, &rr));
    in = key(BTN_ID_LEFT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(uz_ring(&f, z, 3, &rr) && rr.x == z[2].r[1].x);   /* Left from ADD wrapped to CLOSE */
    uz_begin(&f, 1);
    in = key(BTN_ID_UP);
    set_list(&lv, 5, 3);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.shown && !uz_ring(&f, z, 3, &rr));

    /* 9. A wrapped two-row zone: Up/Down move inside it first, then leave it. */
    {
        UiZone w[2];
        memset(w, 0, sizeof(w));
        set_list(&lv, 4, 3);
        w[0].lv = &lv;
        btn(&w[1], 10, 400, 0);  btn(&w[1], 200, 400, 1);
        btn(&w[1], 10, 440, 2);  btn(&w[1], 200, 440, 3);
        uz_begin(&f, 1);
        uz_input(&f, w, 2, NULL, false, &o);
        CHECK(f.id == 0);
        in = key(BTN_ID_DOWN);
        uz_input(&f, w, 2, &in, false, &o);
        CHECK(f.zone == 1 && f.id == 2);
        in = key(BTN_ID_RIGHT);
        uz_input(&f, w, 2, &in, false, &o);
        CHECK(f.id == 3);
        in = key(BTN_ID_UP);
        uz_input(&f, w, 2, &in, false, &o);
        CHECK(f.zone == 1 && f.id == 1);       /* spatial: straight up from 3 */
        uz_input(&f, w, 2, &in, false, &o);
        CHECK(f.zone == 0 && f.id == -1);      /* top row: leave for the list */
    }

    /* 10. A trap zone: Up/Down with no neighbour is reported, not moved; Left/Right still walk. */
    {
        UiZone t[2];
        memset(t, 0, sizeof(t));
        set_list(&lv, 4, 3);
        t[0].lv = &lv;
        t[1].flags = UZ_TRAP;
        btn(&t[1], 10, 300, 0);  btn(&t[1], 200, 300, 1);
        uz_begin(&f, 1);
        uz_input(&f, t, 2, NULL, false, &o);
        in = key(BTN_ID_UP);
        uz_input(&f, t, 2, &in, false, &o);
        CHECK(o.left == 1 && f.zone == 1 && f.id == 0 && o.moved);
        in = key(BTN_ID_DOWN);
        uz_input(&f, t, 2, &in, false, &o);
        CHECK(o.left == 1 && f.zone == 1);
        in = key(BTN_ID_RIGHT);
        uz_input(&f, t, 2, &in, false, &o);
        CHECK(o.left == -1 && f.id == 1);
        /* the caller put focus there on a rising edge */
        uz_begin(&f, 0);
        uz_focus(&f, t, 2, 1, 1);
        CHECK(f.zone == 1 && f.id == 1 && !f.shown);
    }

    /* 11. An empty list holds focus but leaves on either vertical key. */
    set_list(&lv, 0, -1);
    screen(z, &lv, true);
    uz_begin(&f, 1);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && o.list_in == NULL);
    uz_begin(&f, 1);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0);

    /* 12. A bar at the top with nothing above: Up stays. */
    uz_begin(&f, 0);
    uz_input(&f, z, 3, NULL, false, &o);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0 && f.id == 0 && o.left == -1);

    /* ---- Cycle mode: Tab / LB-RB change pane, arrows never leave it ---- */

    /* 13. NEXT cycles bar -> list -> row -> bar (wraps); Shift+Tab (NEXT with RUN held) and PREV go back. */
    set_list(&lv, 5, 2);
    screen(z, &lv, true);
    uz_begin_cycle(&f, 0);
    in = key(BTN_ID_NEXT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && o.moved && f.shown);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 10);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0 && f.id == 0);                       /* wrapped */
    in = key(BTN_ID_NEXT);
    in.buttons[BTN_ID_RUN].held = true;                    /* Shift+Tab */
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2);                                    /* wrapped backwards */
    in = key(BTN_ID_PREV);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && lv.cursor == 2);

    /* 14. An absent pane is skipped, in both directions; a lone pane does not move. */
    set_list(&lv, 5, 0);
    screen(z, &lv, true);
    z[1].lv = NULL;
    uz_begin_cycle(&f, 0);
    in = key(BTN_ID_NEXT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2);
    in = key(BTN_ID_PREV);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0);
    z[2].n = 0;
    in = key(BTN_ID_NEXT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0 && !o.moved);

    /* 15. Down at the last list row and Up at row 0 stay in the list: no zone change, cursor untouched, the list gets the key. */
    set_list(&lv, 5, 4);
    screen(z, &lv, true);
    uz_begin_cycle(&f, 1);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && lv.cursor == 4 && o.list_in != NULL && o.list_in->buttons[BTN_ID_DOWN].pressed);
    set_list(&lv, 5, 0);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && lv.cursor == 0 && o.list_in != NULL);
    set_list(&lv, 0, -1);                                  /* an empty list too */
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1);

    /* 16. Button row: Left/Right move inside it and WRAP (Right at the last button lands on the first); Up/Down never leave. */
    set_list(&lv, 5, 0);
    screen(z, &lv, true);
    uz_begin_cycle(&f, 2);
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 11);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 10);                      /* wrapped inside the row */
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 10 && !o.moved && o.left == -1);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && f.id == 10 && !o.moved);
    uz_begin_cycle(&f, 0);                                 /* the bar: Down does not reach the list either */
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0);

    /* 17. A wrapped two-row pane: Up/Down step between its rows, and stay at the outer edges. */
    memset(z, 0, 3 * sizeof(UiZone));
    btn(&z[0], 10, 100, 0);
    btn(&z[0], 200, 100, 1);
    btn(&z[0], 10, 160, 2);
    uz_begin_cycle(&f, 0);
    uz_input(&f, z, 3, NULL, false, &o);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0 && f.id == 2);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0 && f.id == 2 && !o.moved);
    in = key(BTN_ID_UP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.id == 0);

    /* 18. A UZ_TRAP pane is an ordinary pane: Down finds no neighbour and reports nothing; Tab leaves it. */
    set_list(&lv, 5, 0);
    screen(z, &lv, true);
    z[2].flags = UZ_TRAP;
    uz_begin_cycle(&f, 2);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2 && o.left == -1);
    in = key(BTN_ID_NEXT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 0);

    /* 19. ACTION and JUMP: a button pane reports its id; a list pane reports the cursor row and hands the list no ACTION/JUMP. */
    set_list(&lv, 5, 3);
    screen(z, &lv, true);
    uz_begin_cycle(&f, 2);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate == 10 && o.activate_row == -1);
    in = key(BTN_ID_JUMP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate == 10);
    uz_begin_cycle(&f, 1);
    in = key(BTN_ID_JUMP);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate_row == 3 && o.activate == -1 && f.zone == 1);
    CHECK(o.list_in != NULL && !o.list_in->buttons[BTN_ID_JUMP].pressed && !o.list_in->buttons[BTN_ID_ACTION].pressed);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate_row == 3 && !o.list_in->buttons[BTN_ID_ACTION].pressed);
    in = key(BTN_ID_DOWN);
    set_list(&lv, 5, 1);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate_row == -1);
    set_list(&lv, 0, -1);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate_row == -1);                           /* nothing to activate */

    /* 20. BACK: Esc (PAUSE), Backspace (BACK) and pad B (CANCEL) in any pane; Shift alone (RUN) is not back. */
    set_list(&lv, 5, 0);
    screen(z, &lv, true);
    for (int zone = 0; zone < 3; zone++) {
        uz_begin_cycle(&f, zone);
        in = key(BTN_ID_PAUSE);
        uz_input(&f, z, 3, &in, false, &o);
        CHECK(o.back);
        in = key(BTN_ID_BACK);
        uz_input(&f, z, 3, &in, false, &o);
        CHECK(o.back);
        in = key(BTN_ID_CANCEL);
        uz_input(&f, z, 3, &in, false, &o);
        CHECK(o.back);
        in = key(BTN_ID_RUN);
        uz_input(&f, z, 3, &in, false, &o);
        CHECK(!o.back);
    }
    in = key(-1);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(!o.back);

    /* 21. Each pane remembers its focused button across a round trip. */
    uz_begin_cycle(&f, 2);
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.id == 11);
    in = key(BTN_ID_NEXT);
    uz_input(&f, z, 3, &in, false, &o);                    /* row -> bar */
    uz_input(&f, z, 3, &in, false, &o);                    /* bar -> list */
    uz_input(&f, z, 3, &in, false, &o);                    /* list -> row */
    CHECK(f.zone == 2 && f.id == 11);

    /* 22. A UZ_SLIDER pane reports Left/Right and moves nothing; Up/Down do nothing; ACTION still activates. */
    memset(z, 0, 3 * sizeof(UiZone));
    btn(&z[0], 10, 5, 0);
    btn(&z[1], 100, 200, 20);
    btn(&z[1], 300, 200, 21);
    z[1].flags = UZ_SLIDER;
    uz_begin_cycle(&f, 1);
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.left_right == +1 && f.zone == 1 && f.id == 20);
    in = key(BTN_ID_LEFT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.left_right == -1 && f.id == 20);
    in = key(BTN_ID_DOWN);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.left_right == 0 && f.zone == 1);
    in = key(BTN_ID_ACTION);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(o.activate == 20);

    /* 23. Old mode is untouched by the new keys: Tab does not move, Esc is not back, no slider/row output. */
    set_list(&lv, 5, 4);
    screen(z, &lv, true);
    uz_begin(&f, 1);
    in = key(BTN_ID_NEXT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && !o.back && o.activate_row == -1 && o.left_right == 0);
    in = key(BTN_ID_PAUSE);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 1 && !o.back);
    in = key(BTN_ID_DOWN);                                 /* the edge crossing is still there */
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.zone == 2);

    /* 24. A touch hides the ring in cycle mode as before and moves nothing. */
    uz_begin_cycle(&f, 2);
    in = key(BTN_ID_RIGHT);
    uz_input(&f, z, 3, &in, false, &o);
    CHECK(f.shown);
    uz_input(&f, z, 3, NULL, true, &o);
    CHECK(!f.shown && o.moved);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
