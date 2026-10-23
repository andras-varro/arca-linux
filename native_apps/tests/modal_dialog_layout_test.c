/* Host-side regression for the ModalDialog layout (modal_dialog_layout).
 *
 * For 1..4 buttons and a one-line and a three-line message, with and without
 * screen insets: no button or title sits on the message text, buttons clear
 * the title and each other, and the dialog and its buttons stay inside
 * SCREEN_SAFE.  It also pins the 1- and 2-button geometry of a one-line
 * message to the values those dialogs had before the layout existed.
 *
 * Runs on the DEV MACHINE with native gcc.  Build and run from native_apps:
 *
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o /tmp/modal_dialog_layout_test tests/modal_dialog_layout_test.c \
 *       common/icon_grid.c common/ppm.c common/common.c common/framebuffer.c common/font.c \
 *       common/touch_input.c common/hardware.c common/config.c common/highscore.c \
 *       common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c \
 *       common/audio_eq.c common/audio_enh.c common/audio_wav.c common/start_menu.c \
 *       common/gamepad.c common/input_slots.c common/input_scan.c -lm && /tmp/modal_dialog_layout_test
 *
 * What it cannot see: pixels.  Where a glyph is drawn inside a rect is the
 * font's and text_draw_centered()'s business; this checks the rects.
 */
#include "common.h"
#include <stdio.h>

static int fails;

static void check(int ok, const char *what, int n, int inset) {
    if (!ok) {
        printf("  FAIL %-44s buttons=%d inset=%d\n", what, n, inset);
        fails++;
    }
}

/* Two rects share a point.  An empty rect (absent part) overlaps nothing. */
static int overlaps(ModalRect a, ModalRect b) {
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) return 0;
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* in lies entirely within out. */
static int inside(ModalRect in, ModalRect out) {
    return in.x >= out.x && in.y >= out.y &&
           in.x + in.w <= out.x + out.w && in.y + in.h <= out.y + out.h;
}

static const char *one_line = "6 ARE ALREADY IN THE LIST";
static const char *three_lines =
    "6 ARE ALREADY IN THE LIST\nSKIP THEM OR ADD THEM ALL\nNOTHING IS CHANGED YET";

int main(void) {
    const int insets[2] = { 0, 30 };
    const char *msgs[2] = { one_line, three_lines };

    for (int ii = 0; ii < 2; ii++) {
        int v = insets[ii];
        screen_touch_inset_top = v;
        screen_touch_inset_left = v;
        screen_touch_inset_right = v;
        screen_touch_inset_bottom = v;
        ModalRect safe = { SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP,
                           SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT };

        for (int mi = 0; mi < 2; mi++) {
            for (int n = 1; n <= 4; n++) {
                ModalDialog d;
                modal_dialog_init(&d, "QUEUE", msgs[mi], n);
                for (int i = 0; i < n; i++)
                    modal_dialog_set_button(&d, i, "ADD ALL", 0, 0);

                ModalDialogLayout lay;
                modal_dialog_layout(&d, 800, 480, &lay);
                ModalRect dlg = lay.dialog;

                check(inside(dlg, safe), "dialog inside SCREEN_SAFE", n, v);
                check(lay.message.h > 0, "message rect present", n, v);
                check(lay.title.h > 0, "title rect present", n, v);
                check(inside(lay.message, dlg), "message inside dialog", n, v);
                check(inside(lay.title, dlg), "title inside dialog", n, v);
                check(!overlaps(lay.title, lay.message), "title clear of message", n, v);

                for (int i = 0; i < n; i++) {
                    ModalRect b = lay.button[i];
                    check(inside(b, dlg), "button inside dialog", n, v);
                    check(inside(b, safe), "button inside SCREEN_SAFE", n, v);
                    check(!overlaps(b, lay.message), "button clear of message", n, v);
                    check(!overlaps(b, lay.title), "button clear of title", n, v);
                    for (int j = i + 1; j < n; j++)
                        check(!overlaps(b, lay.button[j]), "buttons do not overlap", n, v);
                }
            }
        }
    }

    /* Golden: one-line message, 1 and 2 buttons keep their original geometry
     * (dialog 420x200 centred on 800x480, 2-button row 25 px above its bottom). */
    for (int n = 1; n <= 2; n++) {
        ModalDialog d;
        modal_dialog_init(&d, "QUEUE", one_line, n);
        for (int i = 0; i < n; i++)
            modal_dialog_set_button(&d, i, "OK", 0, 0);
        ModalDialogLayout lay;
        modal_dialog_layout(&d, 800, 480, &lay);
        check(lay.dialog.x == 190 && lay.dialog.y == 140 &&
              lay.dialog.w == 420 && lay.dialog.h == 200,
              "1-2 button one-line: dialog rect unchanged", n, 0);
        if (n == 1) {
            check(lay.button[0].x == 300 && lay.button[0].y == 276 &&
                  lay.button[0].w == 200 && lay.button[0].h == 44,
                  "1 button: button rect unchanged", n, 0);
        } else {
            check(lay.button[0].x == 235 && lay.button[0].y == 265 &&
                  lay.button[1].x == 415 && lay.button[1].y == 265 &&
                  lay.button[0].w == 150 && lay.button[0].h == 50,
                  "2 buttons: button rects unchanged", n, 0);
        }
    }

    if (fails) {
        printf("modal_dialog_layout_test: %d FAILED\n", fails);
        return 1;
    }
    printf("modal_dialog_layout_test: all passed\n");
    return 0;
}
