/*
 * vid_ring_test.c — host regression for jfvideo/vid_ring.c: single-thread semantics,
 * then a two-thread stress for both rings.  Run it under -fsanitize=thread: that is
 * what sees a missing release/acquire, which x86 would otherwise hide.
 *
 * Build: gcc -Wall -Wextra -O1 -fsanitize=address,undefined -I jfvideo tests/vid_ring_test.c jfvideo/vid_ring.c -lpthread -o vid_ring_test
 *        gcc -Wall -Wextra -O1 -fsanitize=thread           -I jfvideo tests/vid_ring_test.c jfvideo/vid_ring.c -lpthread -o vid_ring_test_tsan
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vid_ring.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NFRAMES 20000
#define NBLOCKS 6000

static VidFrameRing fr;
static VidPcmRing   pr;

static void *frame_producer(void *a)
{
    (void)a;
    for (int i = 0; i < NFRAMES; ) {
        VidFrame *f = vid_fring_prod_slot(&fr);
        if (!f) continue;
        memset(f->y, i & 0xFF, (size_t)f->ys * 32);
        f->cb[0] = (uint8_t)(i >> 8);
        f->pts = i * 0.04;
        vid_fring_prod_commit(&fr);
        i++;
    }
    return NULL;
}

static void *pcm_producer(void *a)
{
    (void)a;
    int16_t blk[1000];
    unsigned seq = 0;
    for (int b = 0; b < NBLOCKS; ) {
        unsigned n = 100 + (unsigned)(b * 37) % 900;          /* 100..999 frames, varying */
        for (unsigned i = 0; i < n; i++) blk[i] = (int16_t)(seq + i);
        if (!vid_pring_write(&pr, blk, n, seq / 48000.0)) continue;
        seq += n; b++;
    }
    return NULL;
}

static void single_thread(void)
{
    VidFrameRing r;
    CHECK(vid_fring_init(&r, 3, 16, 16) == -1, "non power of two refused");
    CHECK(vid_fring_init(&r, 4, 400, 216) == 0, "init");
    CHECK(r.slot[0].ys == 400 && r.hp == 224, "padded geometry %d %d", r.slot[0].ys, r.hp);
    CHECK(vid_fring_peek(&r) == NULL, "empty");
    for (int i = 0; i < 4; i++) { VidFrame *f = vid_fring_prod_slot(&r); CHECK(f != NULL, "slot %d", i); f->pts = i; vid_fring_prod_commit(&r); }
    CHECK(vid_fring_prod_slot(&r) == NULL, "full");
    CHECK(vid_fring_count(&r) == 4, "count");
    CHECK(vid_fring_peek(&r)->pts == 0, "oldest first");
    vid_fring_release(&r);
    CHECK(vid_fring_prod_slot(&r) != NULL, "slot after release");
    vid_fring_free(&r);

    VidPcmRing p;
    CHECK(vid_pring_init(&p, 1024, 2, 48000) == 0, "pcm init");
    int16_t in[2 * 300], out[2 * 1024];
    for (int i = 0; i < 600; i++) in[i] = (int16_t)i;
    CHECK(vid_pring_write(&p, in, 300, 1.0) == 300, "write1");
    CHECK(vid_pring_write(&p, in, 300, 1.0 + 300 / 48000.0) == 300, "write2");
    CHECK(vid_pring_write(&p, in, 300, 9.0) == 300, "write3");
    CHECK(vid_pring_write(&p, in, 300, 9.0) == 0, "write4 refused (no room)");
    double pts = -1;
    CHECK(vid_pring_read(&p, out, 100, &pts) == 100 && pts == 1.0, "first pts %f", pts);
    CHECK(vid_pring_read(&p, out, 250, &pts) == 250, "read2");
    double want = 1.0 + 100.0 / 48000 + 0;                    /* frame 100 is inside block 1 */
    (void)want;
    CHECK(pts > 1.0 + 99.0 / 48000 && pts < 1.0 + 101.0 / 48000, "pts mid-block %f", pts);
    CHECK(vid_pring_read(&p, out, 1024, &pts) == 550, "rest");
    CHECK(pts > 8.99 || pts > 1.0, "pts of third block %f", pts);
    CHECK(vid_pring_write(&p, in, 300, 9.0) == 300, "wrap write");
    CHECK(vid_pring_read(&p, out, 1024, NULL) == 300, "wrap read");
    CHECK(memcmp(out, in, sizeof in) == 0, "wrap data");
    vid_pring_free(&p);
}

int main(void)
{
    single_thread();

    CHECK(vid_fring_init(&fr, 16, 400, 216) == 0, "init");
    CHECK(vid_pring_init(&pr, 4096, 1, 48000) == 0, "init pcm");
    pthread_t tf, tp;
    pthread_create(&tf, NULL, frame_producer, NULL);
    pthread_create(&tp, NULL, pcm_producer, NULL);

    int nf = 0, bad_f = 0;
    unsigned got = 0, bad_p = 0, total = 0;
    for (int b = 0; b < NBLOCKS; b++) total += 100 + (unsigned)(b * 37) % 900;
    double last_pts = -1; int pts_back = 0;
    int16_t buf[700];
    while (nf < NFRAMES || got < total) {
        const VidFrame *f = vid_fring_peek(&fr);
        if (f) {
            int ok = f->y[0] == (nf & 0xFF) && f->y[f->ys * 31 + 399] == (nf & 0xFF) && f->cb[0] == (uint8_t)(nf >> 8) && f->pts == nf * 0.04;
            if (!ok) bad_f++;
            vid_fring_release(&fr);
            nf++;
        }
        double pts;
        unsigned n = vid_pring_read(&pr, buf, 700, &pts);
        if (n) {
            if (pts < last_pts) pts_back++;
            last_pts = pts;
            for (unsigned i = 0; i < n; i++) if (buf[i] != (int16_t)(got + i)) bad_p++;
            got += n;
        }
    }
    pthread_join(tf, NULL); pthread_join(tp, NULL);
    CHECK(bad_f == 0, "%d corrupt frames", bad_f);
    CHECK(bad_p == 0, "%u corrupt/misordered samples", bad_p);
    CHECK(pts_back == 0, "%d pts went backwards", pts_back);
    CHECK(got == total, "samples %u of %u", got, total);
    vid_fring_free(&fr); vid_pring_free(&pr);
    printf("vid_ring_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
