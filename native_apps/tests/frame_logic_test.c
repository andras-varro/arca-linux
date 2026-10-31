/*
 * frame_logic_test.c — host regression for frame/frame_logic.c: importable names, the stable library key
 * (the skip-already-imported rule), and the slideshow order (sequential wrap, shuffle = permutation per
 * round, no repeat across a round boundary, step back).
 *
 * Build: gcc -I frame tests/frame_logic_test.c frame/frame_logic.c -o frame_logic_test
 */
#include <stdio.h>
#include <string.h>
#include "frame_logic.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    CHECK(frame_is_image_name("a.jpg"));
    CHECK(frame_is_image_name("A.JPEG"));
    CHECK(frame_is_image_name("x.y.Png"));
    CHECK(!frame_is_image_name("a.gif"));
    CHECK(!frame_is_image_name("jpg"));
    CHECK(!frame_is_image_name(".jpg"));
    CHECK(!frame_is_image_name("._IMG_1.jpg"));
    CHECK(!frame_is_image_name(""));
    CHECK(!frame_is_image_name("a.jpgx"));
    /* fill rule: OFF none, ALL every picture, LANDSCAPE only a picture whose orientation matches the screen */
    CHECK(!frame_fill_wanted(FRAME_FILL_OFF, 800, 600, 800, 480) && !frame_fill_wanted(FRAME_FILL_OFF, 600, 800, 800, 480));
    CHECK(frame_fill_wanted(FRAME_FILL_ALL, 800, 600, 800, 480) && frame_fill_wanted(FRAME_FILL_ALL, 600, 800, 800, 480));
    CHECK(frame_fill_wanted(FRAME_FILL_LANDSCAPE, 800, 600, 800, 480));
    CHECK(!frame_fill_wanted(FRAME_FILL_LANDSCAPE, 600, 800, 800, 480));
    CHECK(!frame_fill_wanted(FRAME_FILL_LANDSCAPE, 500, 500, 800, 480));
    CHECK(frame_fill_wanted(FRAME_FILL_LANDSCAPE, 480, 800, 480, 800));            /* a portrait screen: portrait matches */
    CHECK(!frame_fill_wanted(99, 800, 600, 800, 480) && !frame_fill_wanted(-1, 800, 600, 800, 480));


    char k1[128], k2[128], k3[128], k4[128];
    CHECK(frame_key("/media/usb0/DCIM/IMG_0001.JPG", 1234567, k1, sizeof k1) > 0);
    CHECK(frame_key("/other/place/IMG_0001.JPG", 1234567, k2, sizeof k2) > 0);
    CHECK(strcmp(k1, k2) == 0);                         /* same name+size: same key wherever it was */
    CHECK(frame_key("/x/IMG_0001.JPG", 1234568, k3, sizeof k3) > 0);
    CHECK(strcmp(k1, k3) != 0);                         /* another size: another picture */
    CHECK(frame_key("/x/IMG_0002.JPG", 1234567, k4, sizeof k4) > 0);
    CHECK(strcmp(k1, k4) != 0);
    CHECK(strstr(k1, ".jpg") && !strchr(k1, '/'));
    char t[128];
    CHECK(frame_key("/x/my photo (1).png", 5, t, sizeof t) > 0);
    CHECK(!strchr(t, ' ') && !strchr(t, '(') && !strchr(t, '/'));
    char u1[128], u2[128];
    frame_key("/x/a b.png", 5, u1, sizeof u1);
    frame_key("/x/a_b.png", 5, u2, sizeof u2);
    CHECK(strcmp(u1, u2) != 0);                         /* sanitising must not merge two names */
    char tiny[8];
    CHECK(frame_key("/x/IMG_0001.JPG", 1, tiny, sizeof tiny) == -1);

    /* sequential */
    int st[10];
    FrameOrder o;
    frame_order_init(&o, st, 4, false, 1);
    CHECK(frame_order_cur(&o) == 0);
    CHECK(frame_order_peek_next(&o) == 1);
    CHECK(frame_order_next(&o) == 1 && frame_order_next(&o) == 2 && frame_order_next(&o) == 3);
    CHECK(frame_order_peek_next(&o) == 0);
    CHECK(frame_order_next(&o) == 0);                   /* wraps */
    CHECK(frame_order_prev(&o) == 3);                   /* back before the start wraps to the end */
    CHECK(frame_order_prev(&o) == 2);
    CHECK(frame_order_next(&o) == 3);

    /* empty and single */
    frame_order_init(&o, st, 0, true, 1);
    CHECK(frame_order_cur(&o) == -1 && frame_order_next(&o) == -1 && frame_order_prev(&o) == -1);
    frame_order_init(&o, st, 1, true, 1);
    CHECK(frame_order_next(&o) == 0 && frame_order_next(&o) == 0);

    /* shuffle: each round a permutation, no repeat at the seam, over many seeds */
    int bad_perm = 0, seam = 0, moved = 0;
    for (uint32_t seed = 1; seed <= 300; seed++) {
        int n = 2 + (int)(seed % 8);
        frame_order_init(&o, st, n, true, seed);
        int last = -1;
        for (int round = 0; round < 4; round++) {
            int seen[10] = {0};
            for (int i = 0; i < n; i++) {
                int v = (round == 0 && i == 0) ? frame_order_cur(&o) : frame_order_next(&o);
                if (v < 0 || v >= n || seen[v]) bad_perm++;
                else seen[v] = 1;
                if (i == 0 && round > 0 && v == last) seam++;
                if (v != i) moved = 1;
                last = v;
            }
        }
    }
    CHECK(bad_perm == 0);
    CHECK(seam == 0);
    CHECK(moved);                                       /* it does shuffle */
    frame_order_init(&o, st, 5, true, 7);
    int a = frame_order_cur(&o), b = frame_order_next(&o), c = frame_order_prev(&o);
    CHECK(c == a && b != a);

    printf(fails ? "frame_logic_test: %d FAILED\n" : "frame_logic_test: all passed\n", fails);
    return fails ? 1 : 0;
}
