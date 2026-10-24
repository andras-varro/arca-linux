/*
 * vid_dec_test.c — host regression for jfvideo/vid_dec: the decoder thread must
 * deliver exactly what a plain pl_mpeg decode of the same file delivers, whatever
 * the byte-arrival pattern, and must stop promptly.
 *
 * Clip: argv[1] or /mnt/c/work/rw-scratch/vid_test.mpg (made with ffmpeg, see the
 * SKIP message).  The reference is pl_mpeg itself (plm_create_with_filename), so
 * this proves the threaded path adds, drops, reorders and mis-stamps nothing; it
 * cannot prove pl_mpeg decodes correctly.
 *
 * Build (from native_apps):
 *   gcc -Wall -Wextra -O1 -fsanitize=address,undefined -I common -I jfvideo \
 *       tests/vid_dec_test.c jfvideo/vid_dec.c jfvideo/vid_ring.c -lpthread -lm -o vdt
 */
#define _GNU_SOURCE
#include <stdio.h>
#include "vid_dec.h"
#include "pl_mpeg.h"          /* declarations only: vid_dec.c holds the implementation */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void msleep(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* ---- reference decode ------------------------------------------------------- */

typedef struct {
    uint8_t *file; size_t flen;
    int      nframes; uint64_t *yh, *ch;
    float   *aud; size_t acount;           /* frames; 2 floats each */
    int      w, h, rate;
} Ref;

static int load_ref(Ref *r, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); r->flen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    r->file = malloc(r->flen);
    if (fread(r->file, 1, r->flen, f) != r->flen) return -1;
    fclose(f);
    plm_t *p = plm_create_with_filename(path);
    if (!p) return -1;
    r->w = plm_get_width(p); r->h = plm_get_height(p); r->rate = plm_get_samplerate(p);
    int cap = 4096;
    r->yh = malloc(sizeof(uint64_t) * (size_t)cap); r->ch = malloc(sizeof(uint64_t) * (size_t)cap);
    plm_frame_t *fr;
    while ((fr = plm_decode_video(p))) {
        if (r->nframes == cap) { cap *= 2; r->yh = realloc(r->yh, sizeof(uint64_t) * (size_t)cap); r->ch = realloc(r->ch, sizeof(uint64_t) * (size_t)cap); }
        r->yh[r->nframes] = fnv(fr->y.data, (size_t)fr->y.width * fr->y.height);
        r->ch[r->nframes] = fnv(fr->cb.data, (size_t)fr->cb.width * fr->cb.height) ^ fnv(fr->cr.data, (size_t)fr->cr.width * fr->cr.height);
        r->nframes++;
    }
    size_t acap = 1 << 20;
    r->aud = malloc(sizeof(float) * 2 * acap);
    plm_samples_t *s;
    while ((s = plm_decode_audio(p))) {
        if (r->acount + s->count > acap) { acap *= 2; r->aud = realloc(r->aud, sizeof(float) * 2 * acap); }
        memcpy(r->aud + 2 * r->acount, s->interleaved, sizeof(float) * 2 * s->count);
        r->acount += s->count;
    }
    plm_destroy(p);
    return 0;
}

/* ---- pipe writer ------------------------------------------------------------ */

typedef struct {
    int fd; const uint8_t *data; size_t len;
    int stall;                      /* after len bytes wait for release before closing */
    atomic_int release;
    pthread_t th;
} Writer;

static void *writer_main(void *a)
{
    Writer *w = a;
    uint32_t rng = 12345u;
    size_t off = 0; int n = 0;
    while (off < w->len) {
        rng = rng * 1664525u + 1013904223u;
        size_t c = 1 + ((rng >> 8) & 8191u) % 8192u;
        if (c > w->len - off) c = w->len - off;
        ssize_t k = write(w->fd, w->data + off, c);
        if (k < 0) { if (errno == EINTR) continue; break; }   /* EPIPE: reader gone */
        off += (size_t)k;
        if ((++n & 31) == 0) usleep(300);
    }
    if (w->stall) while (!atomic_load(&w->release)) msleep(5);
    close(w->fd);
    return NULL;
}

/* ---- decoder thread --------------------------------------------------------- */

typedef struct {
    VidDec *d; VidFrameRing *fr; VidPcmRing *pr; VidDecCfg cfg;
    atomic_int done; int rc; pthread_t th;
} Dec;

static void *dec_main(void *a)
{
    Dec *x = a;
    x->rc = vid_dec_run(x->d, x->fr, x->pr, &x->cfg);
    atomic_store(&x->done, 1);
    return NULL;
}

#define FSLOTS 8
#define PFRAMES 65536u

static int dec_setup(Dec *x, int fd, atomic_int *stop, int nch)
{
    memset(x, 0, sizeof *x);
    if (vid_dec_open(&x->d, fd, stop)) return -1;
    const VidDecInfo *in = vid_dec_info(x->d);
    if (vid_fring_init(x->fr = calloc(1, sizeof *x->fr), FSLOTS, in->w, in->h)) return -1;
    if (vid_pring_init(x->pr = calloc(1, sizeof *x->pr), PFRAMES, nch, in->audio_rate)) return -1;
    x->cfg.out_channels = nch;
    return 0;
}
static void dec_teardown(Dec *x)
{
    vid_dec_close(x->d);
    if (x->fr) { vid_fring_free(x->fr); free(x->fr); }
    if (x->pr) { vid_pring_free(x->pr); free(x->pr); }
}

/* ---- consumer: drain both rings and check against the reference -------------- */

typedef struct { int frames; long pcm; double last_fpts, last_ppts; int bad_hash, bad_pts, bad_pcm, first_bad; } Got;

static int16_t refpcm(const Ref *r, size_t i, int nch, int ch)
{
    float v = nch == 1 ? (r->aud[2 * i] + r->aud[2 * i + 1]) * 0.5f : r->aud[2 * i + ch];
    v *= 32767.0f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

static void drain_once(Dec *x, const Ref *r, Got *g, int nch, int *progress)
{
    const VidFrame *f;
    while ((f = vid_fring_peek(x->fr))) {
        if (g->frames < r->nframes) {
            if (fnv(f->y, (size_t)f->ys * x->fr->hp) != r->yh[g->frames]) g->bad_hash++;
            if ((fnv(f->cb, (size_t)f->cs * (x->fr->hp / 2)) ^ fnv(f->cr, (size_t)f->cs * (x->fr->hp / 2))) != r->ch[g->frames]) g->bad_hash++;
        } else g->bad_hash++;                       /* more frames than the reference */
        if (g->bad_hash && !g->first_bad) g->first_bad = g->frames + 1;   /* 1-based index of the first bad frame */
        if (f->pts < g->last_fpts) g->bad_pts++;
        g->last_fpts = f->pts;
        g->frames++;
        vid_fring_release(x->fr);
        *progress = 1;
    }
    int16_t buf[2048 * 2];
    double pts;
    unsigned n;
    while ((n = vid_pring_read(x->pr, buf, 2048, &pts)) > 0) {
        if (pts + 1e-9 < g->last_ppts) g->bad_pts++;
        g->last_ppts = pts;
        for (unsigned i = 0; i < n; i++)
            for (int c = 0; c < nch; c++) {
                size_t k = (size_t)g->pcm + i;
                if (k >= r->acount) { g->bad_pcm++; continue; }
                int d = buf[i * (unsigned)nch + (unsigned)c] - refpcm(r, k, nch, c);
                if (d < -2 || d > 2) g->bad_pcm++;
            }
        g->pcm += (long)n;
        *progress = 1;
    }
}

/* Run the decoder over `len` bytes of the clip from a pipe or a file; compare.
 * Returns the decoder's exit code through *rc_out. */
static void run_case(const char *name, const Ref *r, int nch, size_t len, int use_file, int expect_full, int *rc_out)
{
    printf("case: %s (nch %d, %zu bytes, %s)\n", name, nch, len, use_file ? "file" : "pipe");
    int fd, pfd[2] = { -1, -1 };
    Writer w; memset(&w, 0, sizeof w);
    const char *tmp = "/mnt/c/work/rw-scratch/vid_dec_tmp.bin";
    if (use_file) {
        FILE *f = fopen(tmp, "wb");
        if (!f) { CHECK(0, "cannot create %s", tmp); return; }
        fwrite(r->file, 1, len, f); fclose(f);
        fd = open(tmp, O_RDONLY);
    } else {
        if (pipe(pfd)) { CHECK(0, "pipe"); return; }
        w.fd = pfd[1]; w.data = r->file; w.len = len;
        pthread_create(&w.th, NULL, writer_main, &w);
        fd = pfd[0];
    }
    atomic_int stop = 0;
    Dec x;
    if (dec_setup(&x, fd, &stop, nch)) { CHECK(0, "open failed"); return; }
    const VidDecInfo *in = vid_dec_info(x.d);
    CHECK(in->w == r->w && in->h == r->h && in->audio_rate == r->rate, "info %dx%d %d Hz", in->w, in->h, in->audio_rate);
    pthread_create(&x.th, NULL, dec_main, &x);
    Got g; memset(&g, 0, sizeof g);
    double t0 = now_s();
    int idle_after_done = 0, burst = 0;
    while (now_s() - t0 < 40) {
        int prog = 0;
        drain_once(&x, r, &g, nch, &prog);
        if (atomic_load(&x.done)) { if (++idle_after_done > 1) break; }
        else if (!prog) msleep(1);
        if ((g.frames & 7) == 3) msleep(2);          /* sometimes slow: the rings fill and the decoder waits */
        if (g.frames >= 40 && !burst) { burst = 1; msleep(300); }   /* a long stall: the frame ring fills completely */
    }
    CHECK(atomic_load(&x.done), "decoder did not finish in 40 s");
    pthread_join(x.th, NULL);
    if (!use_file) { close(fd); pthread_join(w.th, NULL); } else close(fd);
    *rc_out = x.rc;
    printf("  rc %d, frames %d/%d, pcm %ld/%zu, bytes_in %ld, bad hash %d pts %d pcm %d\n", x.rc, g.frames, r->nframes, g.pcm, r->acount,
           atomic_load(&vid_dec_stats(x.d)->bytes_in), g.bad_hash, g.bad_pts, g.bad_pcm);
    CHECK(x.rc == 0, "rc %d", x.rc);
    /* A truncated stream legitimately ends in a partly decoded last frame; every earlier one must match. */
    int hash_ok = g.bad_hash == 0 || (!expect_full && g.first_bad == g.frames);
    CHECK(hash_ok && g.bad_pts == 0 && g.bad_pcm == 0, "content mismatch");
    if (expect_full) {
        CHECK(g.frames == r->nframes, "frame count");
        CHECK((size_t)g.pcm == r->acount, "pcm count");
        CHECK((size_t)atomic_load(&vid_dec_stats(x.d)->bytes_in) == len, "bytes_in");
    } else {
        CHECK(g.frames > 0 && g.frames <= r->nframes, "truncated: frames %d", g.frames);
        CHECK(g.pcm > 0 && (size_t)g.pcm <= r->acount, "truncated: pcm");
    }
    dec_teardown(&x);
}

/* Stop latency.  mode 0: writer stalls with the pipe open, the consumer drains.
 * mode 1: full clip, no consumer, rings fill. */
static void stop_case(const char *name, const Ref *r, int mode)
{
    printf("case: %s\n", name);
    int pfd[2];
    if (pipe(pfd)) { CHECK(0, "pipe"); return; }
    Writer w; memset(&w, 0, sizeof w);
    w.fd = pfd[1]; w.data = r->file; w.len = mode == 0 ? r->flen / 2 : r->flen; w.stall = mode == 0;
    pthread_create(&w.th, NULL, writer_main, &w);
    atomic_int stop = 0;
    Dec x;
    if (dec_setup(&x, pfd[0], &stop, 2)) { CHECK(0, "open failed"); return; }
    pthread_create(&x.th, NULL, dec_main, &x);
    Got g; memset(&g, 0, sizeof g);
    double t0 = now_s();
    while (now_s() - t0 < 0.8) {
        int prog = 0;
        if (mode == 0) drain_once(&x, r, &g, 2, &prog);
        msleep(2);
    }
    if (mode == 1) {
        CHECK(vid_fring_count(x.fr) == FSLOTS, "frame ring not full (%u)", vid_fring_count(x.fr));
        CHECK(!atomic_load(&x.done), "decoder ended before stop");
    }
    double ts = now_s();
    atomic_store_explicit(&stop, 1, memory_order_release);
    while (!atomic_load(&x.done) && now_s() - ts < 3.0) usleep(500);
    double dt = now_s() - ts;
    int done = atomic_load(&x.done);
    printf("  stop latency %.1f ms, rc %d\n", dt * 1000, x.rc);
    CHECK(done && dt < 0.250, "did not stop within 250 ms (done %d, %.0f ms)", done, dt * 1000);
    if (done) CHECK(x.rc == 1, "rc %d, wanted 1", x.rc);
    atomic_store(&w.release, 1);                 /* let everything unwind even on failure */
    close(pfd[0]);
    pthread_join(x.th, NULL);
    pthread_join(w.th, NULL);
    dec_teardown(&x);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/mnt/c/work/rw-scratch/vid_test.mpg";
    signal(SIGPIPE, SIG_IGN);
    if (access(path, R_OK)) {
        printf("SKIP: clip not found (generate with ffmpeg -y -f lavfi -i testsrc=size=400x216:rate=24000/1001 "
               "-f lavfi -i sine=f=440:r=44100 -t 10 -c:v mpeg1video -c:a mp2 -ac 1 -f mpeg %s)\n", path);
        return 0;
    }
    static Ref ref;
    if (load_ref(&ref, path)) { printf("cannot load %s\n", path); return 1; }
    printf("reference: %dx%d, %d frames, %zu pcm frames at %d Hz\n", ref.w, ref.h, ref.nframes, ref.acount, ref.rate);
    CHECK(ref.nframes > 100 && ref.acount > 100000, "reference too small");

    int rc;
    run_case("pipe, mono out", &ref, 1, ref.flen, 0, 1, &rc);
    run_case("pipe, stereo out", &ref, 2, ref.flen, 0, 1, &rc);
    run_case("regular file", &ref, 2, ref.flen, 1, 1, &rc);
    run_case("truncated to 60 KB (pipe)", &ref, 1, 60000 < ref.flen ? 60000 : ref.flen, 0, 0, &rc);
    run_case("truncated to 60 KB (file)", &ref, 2, 60000 < ref.flen ? 60000 : ref.flen, 1, 0, &rc);
    stop_case("stop while the writer stalls mid-stream", &ref, 0);
    stop_case("stop with full rings and no consumer", &ref, 1);

    printf(g_fail ? "FAILED: %d check(s)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
