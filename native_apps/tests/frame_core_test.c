/*
 * frame_core_test.c — host regression for frame/frame_core.c (import: EXIF orientation 1-8 in both byte
 * orders, box-filter downscale, fit, PNG, motion-photo tail, truncation; load: centring and black bars).
 * Every picture is generated here with stb_image_write; no photos.
 *
 * Build: gcc -I common -I frame -fsanitize=undefined,address -fno-sanitize=shift tests/frame_core_test.c frame/frame_core.c -o frame_core_test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"      /* declarations only: frame_core.c holds the implementation */

#include "frame_core.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t *b; size_t n, cap; } buf_t;
static void buf_put(void *ctx, void *d, int n)
{
    buf_t *s = ctx;
    if (s->n + (size_t)n > s->cap) { s->cap = (s->n + (size_t)n) * 2; s->b = realloc(s->b, s->cap); }
    memcpy(s->b + s->n, d, (size_t)n);
    s->n += (size_t)n;
}
static int write_file(const char *p, const uint8_t *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    return w == n;
}

static const uint8_t COL[4][3] = { {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0} };   /* R G B Y */
static const char *COLNAME = "RGBY";

/* quadrant picture: TL=R TR=G BL=B BR=Y */
static uint8_t *quad_rgb(int w, int h)
{
    uint8_t *p = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            memcpy(p + ((size_t)y * w + x) * 3, COL[(y >= h / 2) * 2 + (x >= w / 2)], 3);
    return p;
}

static void jpeg_of(const uint8_t *rgb, int w, int h, buf_t *out)
{
    memset(out, 0, sizeof *out);
    stbi_write_jpg_to_func(buf_put, out, w, h, 3, rgb, 95);
}

/* insert an APP1 Exif segment with the given orientation at `at` */
static void insert_exif(buf_t *j, size_t at, int orient, int le)
{
    uint8_t seg[2 + 2 + 6 + 8 + 2 + 12 + 4], *p = seg;
    *p++ = 0xFF; *p++ = 0xE1;
    size_t len = sizeof seg - 2;
    *p++ = (uint8_t)(len >> 8); *p++ = (uint8_t)len;
    memcpy(p, "Exif\0\0", 6); p += 6;
#define W16(v) do { if (le) { *p++ = (uint8_t)(v); *p++ = (uint8_t)((v) >> 8); } else { *p++ = (uint8_t)((v) >> 8); *p++ = (uint8_t)(v); } } while (0)
    *p++ = le ? 'I' : 'M'; *p++ = le ? 'I' : 'M';
    W16(42);
    if (le) { *p++ = 8; *p++ = 0; *p++ = 0; *p++ = 0; } else { *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 8; }
    W16(1);                       /* one entry */
    W16(0x0112); W16(3);          /* Orientation, SHORT */
    if (le) { *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0; } else { *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 1; }
    W16(orient); W16(0);
    if (le) { *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; } else { p += 4; }
    j->b = realloc(j->b, j->n + sizeof seg);
    memmove(j->b + at + sizeof seg, j->b + at, j->n - at);
    memcpy(j->b + at, seg, sizeof seg);
    j->n += sizeof seg;
}

static uint32_t buf[1024 * 1024];
static int near_col(uint32_t px, const uint8_t *c, int tol)
{
    int r = (int)(px >> 16 & 255) - c[0], g = (int)(px >> 8 & 255) - c[1], b = (int)(px & 255) - c[2];
    return abs(r) <= tol && abs(g) <= tol && abs(b) <= tol;
}

/* expected displayed corners (TL TR BL BR) per EXIF orientation, indexes into COL */
static const char *EXPECT[9] = { 0, "RGBY", "GRYB", "YBGR", "BYRG", "RBGY", "BRYG", "YGBR", "GYRB" };

static void test_orientations(void)
{
    const int W = 64, H = 32, T = 64;
    for (int o = 1; o <= 8; o++) {
        for (int le = 0; le < 2; le++) {
            uint8_t *rgb = quad_rgb(W, H);
            buf_t j; jpeg_of(rgb, W, H, &j); free(rgb);
            /* alternate the position: right after SOI, or after the JFIF APP0 stb writes first */
            size_t at = 2;
            if (le) at = 2 + 2 + ((size_t)j.b[4] << 8 | j.b[5]);
            if (o > 1 || le) insert_exif(&j, at, o, le);
            CHECK(frame_exif_orientation(j.b, j.n) == (o > 1 || le ? o : 1), "exif parse o=%d le=%d", o, le);
            write_file("/tmp/fc_src.jpg", j.b, j.n);
            free(j.b);

            frame_info_t in;
            int rc = frame_import("/tmp/fc_src.jpg", "/tmp/fc_out.jpg", T, T, 0, &in);
            CHECK(rc == FRAME_OK, "import o=%d le=%d rc=%d", o, le, rc);
            if (rc != FRAME_OK) continue;
            int sw = o >= 5 ? H : W, sh = o >= 5 ? W : H;
            CHECK(in.out_w == sw && in.out_h == sh && in.orient == o, "o=%d size %dx%d orient %d", o, in.out_w, in.out_h, in.orient);
            CHECK(access("/tmp/fc_out.jpg.tmp", F_OK) != 0, "tmp left behind");

            frame_info_t ld;
            rc = frame_load("/tmp/fc_out.jpg", buf, T, T, T, &ld);
            CHECK(rc == FRAME_OK && ld.out_w == sw && ld.out_h == sh, "load o=%d", o);
            int ox = (T - sw) / 2, oy = (T - sh) / 2;
            int cx[2] = { ox + sw / 4, ox + 3 * sw / 4 }, cy[2] = { oy + sh / 4, oy + 3 * sh / 4 };
            for (int q = 0; q < 4; q++) {
                uint32_t px = buf[cy[q / 2] * T + cx[q % 2]];
                const uint8_t *want = COL[strchr(COLNAME, EXPECT[o][q]) - COLNAME];
                CHECK(near_col(px, want, 40), "o=%d le=%d quadrant %d got %06x want %c", o, le, q, px & 0xFFFFFF, EXPECT[o][q]);
            }
            CHECK((buf[0] == 0 && buf[T * T - 1] == 0) || (sw == T && sh == T), "o=%d bars not black", o);
        }
    }
}

static void test_fit(void)
{
    frame_info_t in;
    uint8_t *flat = malloc(400 * 100 * 3);
    memset(flat, 200, 400 * 100 * 3);
    buf_t j; jpeg_of(flat, 400, 100, &j); write_file("/tmp/fc_src.jpg", j.b, j.n); free(j.b);
    CHECK(frame_import("/tmp/fc_src.jpg", "/tmp/fc_out.jpg", 200, 100, 0, &in) == FRAME_OK && in.out_w == 200 && in.out_h == 50, "wide fit %dx%d", in.out_w, in.out_h);
    jpeg_of(flat, 100, 400, &j); write_file("/tmp/fc_src.jpg", j.b, j.n); free(j.b);
    CHECK(frame_import("/tmp/fc_src.jpg", "/tmp/fc_out.jpg", 200, 100, 0, &in) == FRAME_OK && in.out_w == 25 && in.out_h == 100, "tall fit %dx%d", in.out_w, in.out_h);
    jpeg_of(flat, 40, 10, &j); write_file("/tmp/fc_src.jpg", j.b, j.n); free(j.b);
    CHECK(frame_import("/tmp/fc_src.jpg", "/tmp/fc_out.jpg", 200, 100, 0, &in) == FRAME_OK && in.out_w == 40 && in.out_h == 10, "small stays %dx%d", in.out_w, in.out_h);
    free(flat);
}

static void test_antialias(void)
{
    /* 1-pixel checkerboard: a point sample is 0 or 255, an area average is ~128 */
    static const int dims[3][4] = { {800, 600, 200, 150}, {1000, 700, 300, 210}, {997, 641, 256, 165} };
    for (int d = 0; d < 3; d++) {
        int w = dims[d][0], h = dims[d][1];
        uint8_t *p = malloc((size_t)w * h * 3);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                memset(p + ((size_t)y * w + x) * 3, ((x ^ y) & 1) ? 255 : 0, 3);
        buf_t j; jpeg_of(p, w, h, &j); write_file("/tmp/fc_src.jpg", j.b, j.n); free(j.b); free(p);
        frame_info_t in;
        int rc = frame_import("/tmp/fc_src.jpg", "/tmp/fc_out.jpg", dims[d][2], dims[d][3], 98, &in);
        CHECK(rc == FRAME_OK, "checker import rc=%d", rc);
        if (rc != FRAME_OK) continue;
        frame_load("/tmp/fc_out.jpg", buf, in.out_w, in.out_h, in.out_w, NULL);
        int bad = 0;
        for (int y = 4; y < in.out_h - 4; y++)
            for (int x = 4; x < in.out_w - 4; x++) {
                int g = buf[y * in.out_w + x] & 255;
                if (g < 112 || g > 144) bad++;
            }
        CHECK(bad == 0, "checker %dx%d: %d pixels away from mid grey (aliasing)", w, h, bad);
    }
}

static void test_png_and_tail(void)
{
    uint8_t *rgb = quad_rgb(64, 32);
    buf_t pn = {0};
    stbi_write_png_to_func(buf_put, &pn, 64, 32, 3, rgb, 64 * 3);
    write_file("/tmp/fc_src.png", pn.b, pn.n); free(pn.b);
    frame_info_t in;
    CHECK(frame_import("/tmp/fc_src.png", "/tmp/fc_out.jpg", 64, 64, 0, &in) == FRAME_OK && in.out_w == 64 && in.out_h == 32 && in.orient == 1, "png import");
    unlink("/tmp/fc_src.png");

    /* Pixel motion photo: a JPEG, then an MP4 (which may well contain FF D8 / FF D9 bytes) */
    buf_t j; jpeg_of(rgb, 64, 32, &j); free(rgb);
    insert_exif(&j, 2, 6, 0);
    size_t jn = j.n;
    static const uint8_t mp4[] = { 0, 0, 0, 24, 'f', 't', 'y', 'p', 'm', 'p', '4', '2', 0xFF, 0xD8, 0xFF, 0xE1, 0, 9, 'j', 'u', 'n', 'k', 0xFF, 0xD9, 1, 2, 3, 4 };
    j.b = realloc(j.b, jn + sizeof mp4);
    memcpy(j.b + jn, mp4, sizeof mp4);
    write_file("/tmp/fc_src.jpg", j.b, jn + sizeof mp4); free(j.b);
    int rc = frame_import("/tmp/fc_src.jpg", "/tmp/fc_out.jpg", 64, 64, 0, &in);
    CHECK(rc == FRAME_OK && in.out_w == 32 && in.out_h == 64 && in.orient == 6, "motion photo rc=%d %dx%d o=%d", rc, in.out_w, in.out_h, in.orient);
}

static void test_failures(void)
{
    uint8_t *rgb = quad_rgb(200, 100);
    buf_t j; jpeg_of(rgb, 200, 100, &j); free(rgb);
    unlink("/tmp/fc_bad.jpg");
    for (int k = 0; k < 4; k++) {
        size_t cut = k == 0 ? 20 : k == 1 ? j.n / 4 : k == 2 ? j.n / 2 : j.n - 3;
        write_file("/tmp/fc_src.jpg", j.b, cut);
        int rc = frame_import("/tmp/fc_src.jpg", "/tmp/fc_bad.jpg", 800, 480, 0, NULL);
        CHECK(rc == FRAME_E_FORMAT, "truncated at %zu of %zu: rc=%d (want FORMAT)", cut, j.n, rc);
        CHECK(access("/tmp/fc_bad.jpg", F_OK) != 0 && access("/tmp/fc_bad.jpg.tmp", F_OK) != 0, "truncated left output behind");
    }
    free(j.b);
    uint8_t junk[300]; for (int i = 0; i < 300; i++) junk[i] = (uint8_t)(i * 7 + 3);
    write_file("/tmp/fc_src.jpg", junk, sizeof junk);
    CHECK(frame_import("/tmp/fc_src.jpg", "/tmp/fc_bad.jpg", 800, 480, 0, NULL) == FRAME_E_FORMAT, "junk");
    CHECK(frame_import("/tmp/fc_nonexistent.jpg", "/tmp/fc_bad.jpg", 800, 480, 0, NULL) == FRAME_E_IO, "missing");
    CHECK(frame_import("/tmp/fc_src.jpg", "/tmp/fc_bad.jpg", 0, 480, 0, NULL) == FRAME_E_ARG, "bad arg");
    CHECK(frame_load("/tmp/fc_src.jpg", buf, 10, 10, 10, NULL) == FRAME_E_FORMAT, "load junk");
    CHECK(frame_exif_orientation(junk, sizeof junk) == 1 && frame_exif_orientation(NULL, 0) == 1, "orientation of junk");
    /* a hostile Exif block: every prefix of a valid file must parse without reading past the end (ASAN) */
    uint8_t *q = quad_rgb(16, 16); buf_t e; jpeg_of(q, 16, 16, &e); free(q);
    insert_exif(&e, 2, 3, 1);
    for (size_t n = 0; n < 60 && n <= e.n; n++) {
        uint8_t *copy = malloc(n ? n : 1); memcpy(copy, e.b, n);
        (void)frame_exif_orientation(copy, n); free(copy);
    }
    free(e.b);
}

static void test_load_bars(void)
{
    uint8_t *rgb = quad_rgb(64, 32);
    buf_t j; jpeg_of(rgb, 64, 32, &j); free(rgb);
    write_file("/tmp/fc_src.jpg", j.b, j.n); free(j.b);
    int stride = 120;
    for (int i = 0; i < stride * 100; i++) buf[i] = 0x00123456;
    CHECK(frame_load("/tmp/fc_src.jpg", buf, 100, 100, stride, NULL) == FRAME_OK, "load");
    CHECK(buf[5 * stride + 50] == 0 && buf[95 * stride + 50] == 0, "bars not black");
    CHECK(near_col(buf[50 * stride + 50 - 16], COL[0], 40) || near_col(buf[(34 + 8) * stride + 50 - 16], COL[0], 40), "picture not centred");
    CHECK(buf[10 * stride + 105] == 0x00123456, "wrote past dst_w into the stride padding");
    /* larger than the buffer: clipped, still centred */
    CHECK(frame_load("/tmp/fc_src.jpg", buf, 32, 16, 32, NULL) == FRAME_OK, "clip load");
    CHECK(near_col(buf[4 * 32 + 8], COL[0], 40) && near_col(buf[12 * 32 + 24], COL[3], 40), "clipped load not centred");
}
static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void test_store(void)
{
    frame_info_t in;
    uint8_t *rgb = quad_rgb(64, 32);
    buf_t j; jpeg_of(rgb, 64, 32, &j);
    write_file("/tmp/fc_src.jpg", j.b, j.n);
    unlink("/tmp/fc_st.jpg");

    /* fits, orientation 1: byte-identical */
    int rc = frame_store("/tmp/fc_src.jpg", "/tmp/fc_st.jpg", 64, 64, 0, &in);
    size_t n = 0; uint8_t *got = slurp("/tmp/fc_st.jpg", &n);
    CHECK(rc == FRAME_OK && got && n == j.n && memcmp(got, j.b, n) == 0, "fitted jpeg not copied unchanged rc=%d n=%zu", rc, n);
    CHECK(in.src_w == 64 && in.src_h == 32 && in.out_w == 64 && in.out_h == 32 && in.out_bytes == (long)j.n && in.orient == 1, "store info");
    CHECK(access("/tmp/fc_st.jpg.tmp", F_OK) != 0, "store tmp left behind");
    free(got);

    /* oversize: re-encoded, smaller dimensions */
    unlink("/tmp/fc_st.jpg");
    rc = frame_store("/tmp/fc_src.jpg", "/tmp/fc_st.jpg", 32, 32, 0, &in);
    got = slurp("/tmp/fc_st.jpg", &n);
    CHECK(rc == FRAME_OK && got && !(n == j.n && memcmp(got, j.b, n) == 0), "oversize jpeg was copied unchanged");
    CHECK(in.out_w == 32 && in.out_h == 16, "oversize fit %dx%d", in.out_w, in.out_h);
    free(got);

    /* orientation 6: goes through import, dimensions swap */
    insert_exif(&j, 2, 6, 0);
    write_file("/tmp/fc_src.jpg", j.b, j.n);
    rc = frame_store("/tmp/fc_src.jpg", "/tmp/fc_st.jpg", 64, 64, 0, &in);
    CHECK(rc == FRAME_OK && in.out_w == 32 && in.out_h == 64 && in.orient == 6, "rotated store rc=%d %dx%d o=%d", rc, in.out_w, in.out_h, in.orient);

    /* PNG goes through import */
    buf_t pn = {0};
    stbi_write_png_to_func(buf_put, &pn, 64, 32, 3, rgb, 64 * 3);
    write_file("/tmp/fc_src.png", pn.b, pn.n); free(pn.b);
    rc = frame_store("/tmp/fc_src.png", "/tmp/fc_st.jpg", 64, 64, 0, &in);
    got = slurp("/tmp/fc_st.jpg", &n);
    CHECK(rc == FRAME_OK && got && n > 2 && got[0] == 0xFF && got[1] == 0xD8, "png store rc=%d", rc);
    free(got); unlink("/tmp/fc_src.png");

    /* truncated: fails like import, leaves nothing */
    unlink("/tmp/fc_st.jpg");
    write_file("/tmp/fc_src.jpg", j.b, j.n / 2);
    rc = frame_store("/tmp/fc_src.jpg", "/tmp/fc_st.jpg", 64, 64, 0, NULL);
    CHECK(rc == FRAME_E_FORMAT, "truncated store rc=%d", rc);
    CHECK(access("/tmp/fc_st.jpg", F_OK) != 0 && access("/tmp/fc_st.jpg.tmp", F_OK) != 0, "truncated store left output");
    CHECK(frame_store("/tmp/fc_nonexistent.jpg", "/tmp/fc_st.jpg", 64, 64, 0, NULL) == FRAME_E_IO, "store missing");
    CHECK(frame_store("/tmp/fc_src.jpg", "/tmp/fc_st.jpg", 0, 64, 0, NULL) == FRAME_E_ARG, "store bad arg");
    free(j.b); free(rgb);
    unlink("/tmp/fc_st.jpg");
}


int main(void)
{
    test_orientations();
    test_fit();
    test_antialias();
    test_png_and_tail();
    test_failures();
    test_load_bars();
    test_store();
    unlink("/tmp/fc_src.jpg"); unlink("/tmp/fc_out.jpg"); unlink("/tmp/fc_bad.jpg");
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
