/* fb_bench: display-path benchmark for the planned libretro frontend.
 *
 *   fb_bench [--test copy|flip|scale|overlay|all] [--frames N] [--help]
 *
 * Build (soft-float, Cortex-A8; no -march/-mcpu that implies idiv):
 *   arm-linux-gnueabi-gcc -march=armv7-a -mtune=cortex-a8 -mfpu=neon \
 *     -mfloat-abi=softfp -O2 retro/tools/fb_bench.c -o fb_bench
 *
 * Measures on /dev/fb0 at 16 and 32 bpp (yres_virtual = 2*yres requested, the
 * driver's answer is printed): full-frame memcpy into the mapped fb (copy),
 * FBIOPAN_DISPLAY flips with and without OMAPFB_WAITFORVSYNC (flip), software
 * nearest-neighbour upscale of 256x240 / 320x240 / 384x272 RGB565 sources to
 * 800x480 (scale), and, as a separate measurement only, a scaled DSS overlay on
 * /dev/fb1 plus VRFB rotation through var.rotate on fb0 (overlay). The project
 * deliberately does not use the DSS scaler in apps. overlay is NOT part of
 * "all"; ask for it by name. The original fb0 var screeninfo is restored on
 * exit and on SIGINT/SIGTERM. Stop the running app first (it owns the panel).
 * One result per line, plain text. Needs root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* <linux/omapfb.h> from the host kernel headers; the numbers below are the
 * 4.14 uapi values (type 'O', arch-neutral) kept here so the build does not
 * depend on the sysroot shipping the header. */
#define OMAP_IO(nr)      _IO('O', nr)
#define OMAP_IOW(nr, t)  _IOW('O', nr, t)
struct fbb_plane_info { uint32_t pos_x, pos_y; uint8_t enabled, channel_out, mirror, mem_idx;
    uint32_t out_width, out_height; uint32_t reserved2[12]; };
struct fbb_mem_info { uint32_t size; uint8_t type, reserved[3]; };
#define OMAPFB_SETUP_PLANE  OMAP_IOW(52, struct fbb_plane_info)
#define OMAPFB_QUERY_PLANE  OMAP_IOW(53, struct fbb_plane_info)
#define OMAPFB_SETUP_MEM    OMAP_IOW(55, struct fbb_mem_info)
#define OMAPFB_QUERY_MEM    OMAP_IOW(56, struct fbb_mem_info)
#define OMAPFB_WAITFORVSYNC OMAP_IO(57)

#define PW 800
#define PH 480

static int fd0 = -1;
static struct fb_var_screeninfo orig_var;
static int have_orig;
static unsigned char *fbmem; static size_t fblen;
static int frames = 120;

/* fb1 state, restored by the signal handler too */
static int fd1 = -1;
static int ovl_active;
static struct fb_var_screeninfo ovl_var;
static struct fbb_plane_info ovl_plane;
static struct fbb_mem_info ovl_mem;

static double elapsed(const struct timespec *t0)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) * 1e-9;
}
static void now(struct timespec *t) { clock_gettime(CLOCK_MONOTONIC, t); }

static void restore_all(void)
{
    if (fd1 >= 0 && ovl_active) {
        struct fbb_plane_info off = ovl_plane; off.enabled = 0;
        ioctl(fd1, OMAPFB_SETUP_PLANE, &off);
        ioctl(fd1, OMAPFB_SETUP_MEM, &ovl_mem);
        ioctl(fd1, FBIOPUT_VSCREENINFO, &ovl_var);
        ioctl(fd1, OMAPFB_SETUP_PLANE, &ovl_plane);
        ovl_active = 0;
    }
    if (fd0 >= 0 && have_orig) {
        ioctl(fd0, FBIOPUT_VSCREENINFO, &orig_var);
        ioctl(fd0, FBIOPAN_DISPLAY, &orig_var);
    }
}

static void on_signal(int sig)
{
    restore_all();
    _exit(128 + sig);
}

static void usage(void)
{
    puts("usage: fb_bench [--test copy|flip|scale|overlay|all] [--frames N]\n"
         "  copy     memcpy of a full 800x480 frame into the fb, 16 and 32 bpp\n"
         "  flip     FBIOPAN_DISPLAY rate, with and without OMAPFB_WAITFORVSYNC\n"
         "  scale    software nearest-neighbour upscale of emulator frame sizes\n"
         "  overlay  fb1 DSS overlay upload + VRFB rotation (separate; not in all)\n"
         "  all      copy + flip + scale (default)\n"
         "  --frames N  iterations per measurement (default 120)\n"
         "Stop the running app first. Restores the fb0 mode on exit/SIGINT/SIGTERM.");
}

static void unmap_fb(void)
{
    if (fbmem) { munmap(fbmem, fblen); fbmem = NULL; }
}

/* Set a 800x480 mode with a 2-page virtual height; print what the driver gave.
 * rotate is the VRFB rotation request (0 normally). Returns 0 on success. */
static int set_mode(int bpp, int rotate, struct fb_var_screeninfo *out, struct fb_fix_screeninfo *fix)
{
    struct fb_var_screeninfo v = orig_var;
    unmap_fb();
    v.xres = PW; v.yres = PH; v.xres_virtual = PW; v.yres_virtual = 2 * PH;
    v.xoffset = v.yoffset = 0; v.bits_per_pixel = bpp; v.rotate = rotate;
    v.activate = FB_ACTIVATE_NOW;
    memset(&v.red, 0, 3 * sizeof v.red); memset(&v.transp, 0, sizeof v.transp);
    if (bpp == 16) { v.red.offset = 11; v.red.length = 5; v.green.offset = 5; v.green.length = 6;
                     v.blue.offset = 0; v.blue.length = 5; }
    else { v.red.offset = 16; v.red.length = 8; v.green.offset = 8; v.green.length = 8;
           v.blue.offset = 0; v.blue.length = 8; }
    if (ioctl(fd0, FBIOPUT_VSCREENINFO, &v) < 0) {
        printf("mode bpp=%d rotate=%d: FBIOPUT_VSCREENINFO failed: %s\n", bpp, rotate, strerror(errno));
        return -1;
    }
    if (ioctl(fd0, FBIOGET_VSCREENINFO, out) < 0 || ioctl(fd0, FBIOGET_FSCREENINFO, fix) < 0) {
        printf("mode bpp=%d: readback failed: %s\n", bpp, strerror(errno));
        return -1;
    }
    printf("mode req bpp=%d rotate=%d virt=%dx%d -> got %ux%u virt=%ux%u bpp=%u rotate=%u "
           "line=%u smem_len=%u two_pages=%s\n",
           bpp, rotate, PW, 2 * PH, out->xres, out->yres, out->xres_virtual, out->yres_virtual,
           out->bits_per_pixel, out->rotate, fix->line_length, fix->smem_len,
           (out->yres_virtual >= 2 * out->yres && fix->smem_len >= 2 * fix->line_length * out->yres)
               ? "yes" : "no");
    fblen = fix->smem_len;
    fbmem = mmap(NULL, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd0, 0);
    if (fbmem == MAP_FAILED) {
        printf("mode bpp=%d: mmap failed: %s\n", bpp, strerror(errno));
        fbmem = NULL; return -1;
    }
    return 0;
}

static unsigned char *make_frame(size_t n)
{
    unsigned char *p = malloc(n);
    if (!p) { perror("malloc"); exit(1); }
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(i * 7 + (i >> 8));
    return p;
}

static void report_rate(const char *what, double secs, size_t bytes_per)
{
    double per = secs / frames;
    if (bytes_per)
        printf("%s: %.3f ms/frame %.1f fps %.1f MB/s\n", what, per * 1e3, 1.0 / per,
               (double)bytes_per / per / 1048576.0);
    else
        printf("%s: %.3f ms/frame %.1f fps\n", what, per * 1e3, 1.0 / per);
}

static void test_copy(int bpp)
{
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    char tag[96]; struct timespec t0;
    if (set_mode(bpp, 0, &v, &f) < 0) return;
    size_t n = (size_t)f.line_length * v.yres;
    unsigned char *src = make_frame(n), *ram = malloc(n);
    if (!ram) { perror("malloc"); exit(1); }
    memcpy(fbmem, src, n);                        /* warm the mapping */
    now(&t0);
    for (int i = 0; i < frames; i++) { src[0] = (unsigned char)i; memcpy(ram, src, n); }
    snprintf(tag, sizeof tag, "copy bpp=%d ram->ram (reference) bytes=%lu", bpp, (unsigned long)n);
    report_rate(tag, elapsed(&t0), n);
    now(&t0);
    for (int i = 0; i < frames; i++) { src[0] = (unsigned char)i; memcpy(fbmem, src, n); }
    snprintf(tag, sizeof tag, "copy bpp=%d ram->fb page0 bytes=%lu", bpp, (unsigned long)n);
    report_rate(tag, elapsed(&t0), n);
    if (v.yres_virtual >= 2 * v.yres) {
        now(&t0);
        for (int i = 0; i < frames; i++) { src[0] = (unsigned char)i; memcpy(fbmem + n, src, n); }
        snprintf(tag, sizeof tag, "copy bpp=%d ram->fb page1 bytes=%lu", bpp, (unsigned long)n);
        report_rate(tag, elapsed(&t0), n);
    }
    free(src); free(ram);
}

static int pan(unsigned y, struct fb_var_screeninfo *v)
{
    v->xoffset = 0; v->yoffset = y;
    return ioctl(fd0, FBIOPAN_DISPLAY, v);
}

static void test_flip(int bpp)
{
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    char tag[96]; struct timespec t0;
    if (set_mode(bpp, 0, &v, &f) < 0) return;
    if (v.yres_virtual < 2 * v.yres) {
        printf("flip bpp=%d: unsupported (no second page: yres_virtual=%u)\n", bpp, v.yres_virtual);
        return;
    }
    size_t n = (size_t)f.line_length * v.yres;
    unsigned char *src = make_frame(n);

    /* the refresh rate itself: vsync waits back to back */
    now(&t0);
    int ok = 1;
    for (int i = 0; i < frames; i++)
        if (ioctl(fd0, OMAPFB_WAITFORVSYNC, 0) < 0) { ok = 0; break; }
    if (ok) {
        snprintf(tag, sizeof tag, "flip bpp=%d vsync wait only", bpp);
        report_rate(tag, elapsed(&t0), 0);
    } else
        printf("flip bpp=%d: OMAPFB_WAITFORVSYNC unsupported (%s)\n", bpp, strerror(errno));

    now(&t0);
    for (int i = 0; i < frames; i++)
        if (pan((unsigned)(i & 1) * v.yres, &v) < 0) {
            printf("flip bpp=%d: FBIOPAN_DISPLAY failed: %s\n", bpp, strerror(errno));
            free(src); return;
        }
    snprintf(tag, sizeof tag, "flip bpp=%d pan, no wait", bpp);
    report_rate(tag, elapsed(&t0), 0);

    if (ok) {
        now(&t0);
        for (int i = 0; i < frames; i++) {
            ioctl(fd0, OMAPFB_WAITFORVSYNC, 0);
            pan((unsigned)(i & 1) * v.yres, &v);
        }
        snprintf(tag, sizeof tag, "flip bpp=%d wait + pan", bpp);
        report_rate(tag, elapsed(&t0), 0);

        /* the real loop: draw into the hidden page, wait, flip */
        now(&t0);
        for (int i = 0; i < frames; i++) {
            unsigned back = (unsigned)((i + 1) & 1);
            src[0] = (unsigned char)i;
            memcpy(fbmem + (size_t)back * n, src, n);
            ioctl(fd0, OMAPFB_WAITFORVSYNC, 0);
            pan(back * v.yres, &v);
        }
        snprintf(tag, sizeof tag, "flip bpp=%d copy + wait + pan (frame loop)", bpp);
        report_rate(tag, elapsed(&t0), n);
    }
    pan(0, &v);
    free(src);
}

/* nearest-neighbour upscale to 800x480; maps built once (divides only there) */
static void scale_blit(unsigned char *dst, size_t pitch, int bpp, const uint16_t *src,
                       int sw, int sh, const unsigned short *xmap, const unsigned short *ymap)
{
    int last_sy = -1;
    unsigned char *prev = NULL;
    for (int y = 0; y < PH; y++) {
        int sy = ymap[y];
        unsigned char *row = dst + (size_t)y * pitch;
        if (sy == last_sy) { memcpy(row, prev, (size_t)PW * (bpp >> 3)); continue; }
        const uint16_t *s = src + (size_t)sy * sw;
        if (bpp == 16) {
            uint16_t *d = (uint16_t *)row;
            for (int x = 0; x < PW; x++) d[x] = s[xmap[x]];
        } else {
            uint32_t *d = (uint32_t *)row;
            for (int x = 0; x < PW; x++) {
                uint32_t p = s[xmap[x]];
                d[x] = ((p & 0xF800u) << 8) | ((p & 0x07E0u) << 5) | ((p & 0x001Fu) << 3);
            }
        }
        last_sy = sy; prev = row;
    }
    (void)sh;
}

static void test_scale(int bpp)
{
    static const int sizes[3][2] = { {256, 240}, {320, 240}, {384, 272} };
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    unsigned short xmap[PW], ymap[PH];
    struct timespec t0; char tag[96];
    if (set_mode(bpp, 0, &v, &f) < 0) return;
    unsigned char *ram = malloc((size_t)f.line_length * PH);
    if (!ram) { perror("malloc"); exit(1); }
    for (int k = 0; k < 3; k++) {
        int sw = sizes[k][0], sh = sizes[k][1];
        uint16_t *src = (uint16_t *)make_frame((size_t)sw * sh * 2);
        for (int x = 0; x < PW; x++) xmap[x] = (unsigned short)((unsigned)x * (unsigned)sw / PW);
        for (int y = 0; y < PH; y++) ymap[y] = (unsigned short)((unsigned)y * (unsigned)sh / PH);
        scale_blit(fbmem, f.line_length, bpp, src, sw, sh, xmap, ymap);   /* warm */
        now(&t0);
        for (int i = 0; i < frames; i++) {
            src[0] = (uint16_t)i;
            scale_blit(ram, f.line_length, bpp, src, sw, sh, xmap, ymap);
        }
        snprintf(tag, sizeof tag, "scale bpp=%d %dx%d->800x480 into RAM", bpp, sw, sh);
        report_rate(tag, elapsed(&t0), 0);
        now(&t0);
        for (int i = 0; i < frames; i++) {
            src[0] = (uint16_t)i;
            scale_blit(fbmem, f.line_length, bpp, src, sw, sh, xmap, ymap);
        }
        snprintf(tag, sizeof tag, "scale bpp=%d %dx%d->800x480 direct into fb", bpp, sw, sh);
        report_rate(tag, elapsed(&t0), 0);
        now(&t0);
        for (int i = 0; i < frames; i++) {
            src[0] = (uint16_t)i;
            scale_blit(ram, f.line_length, bpp, src, sw, sh, xmap, ymap);
            memcpy(fbmem, ram, (size_t)f.line_length * PH);
        }
        snprintf(tag, sizeof tag, "scale bpp=%d %dx%d->800x480 RAM then memcpy to fb", bpp, sw, sh);
        report_rate(tag, elapsed(&t0), 0);
        free(src);
    }
    free(ram);
}

/* DSS overlay on fb1: native-size upload, scaled to the panel by the hardware */
static void test_overlay(void)
{
    static const int sizes[3][2] = { {256, 240}, {320, 240}, {384, 272} };
    struct fb_fix_screeninfo f1;
    struct fb_var_screeninfo v;
    struct fbb_plane_info pl; struct fbb_mem_info mi;
    struct timespec t0; char tag[96];

    fd1 = open("/dev/fb1", O_RDWR);
    if (fd1 < 0) { printf("overlay: unsupported (/dev/fb1: %s)\n", strerror(errno)); return; }
    if (ioctl(fd1, FBIOGET_VSCREENINFO, &ovl_var) < 0) {
        printf("overlay: unsupported (FBIOGET_VSCREENINFO fb1: %s)\n", strerror(errno)); goto out;
    }
    printf("overlay fb1 initial: %ux%u bpp=%u\n", ovl_var.xres, ovl_var.yres, ovl_var.bits_per_pixel);
    if (ioctl(fd1, OMAPFB_QUERY_PLANE, &ovl_plane) < 0) {
        printf("overlay: unsupported (OMAPFB_QUERY_PLANE: %s)\n", strerror(errno)); goto out;
    }
    printf("overlay plane: pos=%u,%u enabled=%u out=%ux%u mem_idx=%u\n", ovl_plane.pos_x,
           ovl_plane.pos_y, ovl_plane.enabled, ovl_plane.out_width, ovl_plane.out_height,
           ovl_plane.mem_idx);
    if (ioctl(fd1, OMAPFB_QUERY_MEM, &ovl_mem) < 0) {
        printf("overlay: unsupported (OMAPFB_QUERY_MEM: %s)\n", strerror(errno)); goto out;
    }
    printf("overlay mem: size=%u type=%u\n", ovl_mem.size, ovl_mem.type);
    ovl_active = 1;                               /* from here on, restore on exit */

    for (int k = 0; k < 3; k++) {
        int sw = sizes[k][0], sh = sizes[k][1];
        size_t n = (size_t)sw * sh * 2;
        pl = ovl_plane; pl.enabled = 0;
        ioctl(fd1, OMAPFB_SETUP_PLANE, &pl);
        mi = ovl_mem; mi.size = (uint32_t)((n + 4095) & ~(size_t)4095);
        if (ioctl(fd1, OMAPFB_SETUP_MEM, &mi) < 0) {
            printf("overlay %dx%d: OMAPFB_SETUP_MEM failed: %s\n", sw, sh, strerror(errno)); continue;
        }
        v = ovl_var;
        v.xres = v.xres_virtual = (uint32_t)sw; v.yres = v.yres_virtual = (uint32_t)sh;
        v.xoffset = v.yoffset = 0; v.bits_per_pixel = 16; v.rotate = 0;
        v.red.offset = 11; v.red.length = 5; v.green.offset = 5; v.green.length = 6;
        v.blue.offset = 0; v.blue.length = 5; v.transp.length = 0; v.activate = FB_ACTIVATE_NOW;
        if (ioctl(fd1, FBIOPUT_VSCREENINFO, &v) < 0) {
            printf("overlay %dx%d: FBIOPUT_VSCREENINFO failed: %s\n", sw, sh, strerror(errno)); continue;
        }
        ioctl(fd1, FBIOGET_VSCREENINFO, &v);
        ioctl(fd1, FBIOGET_FSCREENINFO, &f1);
        unsigned char *m = mmap(NULL, f1.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
        if (m == MAP_FAILED) {
            printf("overlay %dx%d: mmap failed: %s\n", sw, sh, strerror(errno)); continue;
        }
        pl = ovl_plane; pl.pos_x = pl.pos_y = 0; pl.out_width = PW; pl.out_height = PH; pl.enabled = 1;
        int r = ioctl(fd1, OMAPFB_SETUP_PLANE, &pl);
        printf("overlay %dx%d: got %ux%u line=%u smem_len=%u setup_plane(out 800x480)=%s\n", sw, sh,
               v.xres, v.yres, f1.line_length, f1.smem_len, r < 0 ? strerror(errno) : "ok");
        if (r == 0) {
            unsigned char *src = make_frame((size_t)f1.line_length * v.yres);
            size_t cn = (size_t)f1.line_length * v.yres;
            now(&t0);
            for (int i = 0; i < frames; i++) { src[0] = (unsigned char)i; memcpy(m, src, cn); }
            snprintf(tag, sizeof tag, "overlay upload %dx%d rgb565 native size", sw, sh);
            report_rate(tag, elapsed(&t0), cn);
            free(src);
        } else
            printf("overlay %dx%d: scaled plane unsupported\n", sw, sh);
        pl.enabled = 0; ioctl(fd1, OMAPFB_SETUP_PLANE, &pl);
        munmap(m, f1.smem_len);
    }
out:
    restore_all();
    close(fd1); fd1 = -1;
}

static void test_rotate(void)
{
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    struct timespec t0; char tag[96];
    for (int rot = 0; rot < 4; rot++) {
        if (set_mode(16, rot, &v, &f) < 0) continue;
        if ((int)v.rotate != rot) {
            printf("rotate=%d: unsupported (driver kept rotate=%u)\n", rot, v.rotate);
            continue;
        }
        size_t n = (size_t)f.line_length * v.yres;
        unsigned char *src = make_frame(n);
        now(&t0);
        for (int i = 0; i < frames; i++) { src[0] = (unsigned char)i; memcpy(fbmem, src, n); }
        snprintf(tag, sizeof tag, "rotate=%d (VRFB) copy 16bpp %ux%u bytes=%lu", rot, v.xres, v.yres,
                 (unsigned long)n);
        report_rate(tag, elapsed(&t0), n);
        free(src);
    }
}

int main(int argc, char **argv)
{
    const char *test = "all";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--test") && i + 1 < argc) test = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = atoi(argv[++i]);
            if (frames < 1) { fprintf(stderr, "fb_bench: bad --frames\n"); return 2; }
        } else { usage(); return 2; }
    }
    int all = !strcmp(test, "all");
    int do_copy = all || !strcmp(test, "copy"), do_flip = all || !strcmp(test, "flip");
    int do_scale = all || !strcmp(test, "scale"), do_ovl = !strcmp(test, "overlay");
    if (!do_copy && !do_flip && !do_scale && !do_ovl) { usage(); return 2; }

    char gov[64] = "unreadable";
    FILE *g = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (g) { if (fgets(gov, sizeof gov, g)) gov[strcspn(gov, "\n")] = 0; fclose(g); }
    printf("fb_bench: governor=%s frames=%d test=%s\n", gov, frames, test);

    fd0 = open("/dev/fb0", O_RDWR);
    if (fd0 < 0) { fprintf(stderr, "fb_bench: /dev/fb0: %s\n", strerror(errno)); return 1; }
    if (ioctl(fd0, FBIOGET_VSCREENINFO, &orig_var) < 0) {
        fprintf(stderr, "fb_bench: FBIOGET_VSCREENINFO: %s\n", strerror(errno)); return 1;
    }
    have_orig = 1;
    printf("original: %ux%u virt=%ux%u bpp=%u rotate=%u\n", orig_var.xres, orig_var.yres,
           orig_var.xres_virtual, orig_var.yres_virtual, orig_var.bits_per_pixel, orig_var.rotate);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    static const int bpps[2] = { 16, 32 };
    for (int b = 0; b < 2; b++) {
        if (do_copy) test_copy(bpps[b]);
        if (do_flip) test_flip(bpps[b]);
        if (do_scale) test_scale(bpps[b]);
    }
    if (do_ovl) { test_overlay(); test_rotate(); }

    unmap_fb();
    restore_all();
    struct fb_var_screeninfo chk;
    if (ioctl(fd0, FBIOGET_VSCREENINFO, &chk) == 0)
        printf("restored: %ux%u virt=%ux%u bpp=%u rotate=%u\n", chk.xres, chk.yres,
               chk.xres_virtual, chk.yres_virtual, chk.bits_per_pixel, chk.rotate);
    close(fd0);
    return 0;
}
