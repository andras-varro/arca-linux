/* retro video backends: "sw" (NN scale to RGB565 in RAM, one copy into fb0's
 * back page, pan) and "overlay" (native-size RGB565 into fb1, DSS vid1 scales).
 * fb0 runs 16bpp with yres_virtual 960 (32bpp gets no second page, measured).
 * A pan is latched by the DSS at the next vsync (shadow registers + GO bit:
 * inferred from the omapfb driver, not measured), so present() pans at once and
 * the NEXT write into that page first waits until a vsync has passed. */
#define _GNU_SOURCE
#include "retro.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define OMAP_IO(nr)      _IO('O', nr)
#define OMAP_IOW(nr, t)  _IOW('O', nr, t)
struct ofb_plane { uint32_t pos_x, pos_y; uint8_t enabled, channel_out, mirror, mem_idx;
    uint32_t out_width, out_height; uint32_t reserved2[12]; };
struct ofb_mem { uint32_t size; uint8_t type, reserved[3]; };
#define OMAPFB_SETUP_PLANE  OMAP_IOW(52, struct ofb_plane)
#define OMAPFB_QUERY_PLANE  OMAP_IOW(53, struct ofb_plane)
#define OMAPFB_SETUP_MEM    OMAP_IOW(55, struct ofb_mem)
#define OMAPFB_QUERY_MEM    OMAP_IOW(56, struct ofb_mem)
#define OMAPFB_WAITFORVSYNC OMAP_IO(57)
#define PW 800
#define PH 480

double vstat_conv, vstat_present;
unsigned vstat_vsync_waits;

static int fd0 = -1, have_orig;
static struct fb_var_screeninfo orig0, var0;
static uint16_t *fb0m; static size_t fb0len; static unsigned line0;   /* in pixels */
static double vs_last, vs_period = 1.0 / 60.5;   /* last seen vsync, period */

void vsync_wait(void)
{
    if (fd0 >= 0) ioctl(fd0, OMAPFB_WAITFORVSYNC, 0);
    double t = now_s();
    if (vs_last > 0 && t - vs_last > 0.012 && t - vs_last < 0.021)
        vs_period = vs_period * 0.95 + (t - vs_last) * 0.05;
    vs_last = t; vstat_vsync_waits++;
}

/* A vsync after pan_time has happened if the predicted next vsync after
 * pan_time is in the past; otherwise wait for it. */
void vsync_guard(double pan_time)
{
    if (vs_last <= 0) { vsync_wait(); return; }
    double k = (pan_time - vs_last) / vs_period;
    double next = vs_last + ((double)(long)k + 1.0) * vs_period;
    if (now_s() < next + 0.001) vsync_wait();
}

int fb0_open(void)
{
    if (fd0 >= 0) return 0;
    fd0 = open("/dev/fb0", O_RDWR);
    if (fd0 < 0 || ioctl(fd0, FBIOGET_VSCREENINFO, &orig0) < 0) { perror("fb0"); return -1; }
    have_orig = 1;
    struct fb_var_screeninfo v = orig0;
    v.xres = v.xres_virtual = PW; v.yres = PH; v.yres_virtual = 2 * PH;
    v.xoffset = v.yoffset = 0; v.bits_per_pixel = 16; v.activate = FB_ACTIVATE_NOW;
    memset(&v.red, 0, 3 * sizeof v.red); memset(&v.transp, 0, sizeof v.transp);
    v.red.offset = 11; v.red.length = 5; v.green.offset = 5; v.green.length = 6; v.blue.length = 5;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd0, FBIOPUT_VSCREENINFO, &v) < 0 || ioctl(fd0, FBIOGET_VSCREENINFO, &var0) < 0 ||
        ioctl(fd0, FBIOGET_FSCREENINFO, &fix) < 0) { perror("fb0 mode"); return -1; }
    if (var0.bits_per_pixel != 16 || var0.yres_virtual < 2 * PH) {
        fprintf(stderr, "retro: fb0 gave %ubpp virt %u, need 16bpp x2 pages\n",
                var0.bits_per_pixel, var0.yres_virtual); return -1;
    }
    line0 = fix.line_length / 2; fb0len = fix.smem_len;
    fb0m = mmap(NULL, fb0len, PROT_READ | PROT_WRITE, MAP_SHARED, fd0, 0);
    if (fb0m == MAP_FAILED) { fb0m = NULL; perror("fb0 mmap"); return -1; }
    memset(fb0m, 0, (size_t)line0 * 2 * PH * 2);
    return 0;
}

void fb0_restore(void)
{
    if (fd0 >= 0 && have_orig) {
        if (fb0m) memset(fb0m, 0, (size_t)line0 * 2 * PH * 2);
        ioctl(fd0, FBIOPUT_VSCREENINFO, &orig0); ioctl(fd0, FBIOPAN_DISPLAY, &orig0);
    }
}

static int pan0(unsigned page)
{
    var0.yoffset = page * PH; var0.xoffset = 0;
    return ioctl(fd0, FBIOPAN_DISPLAY, &var0);
}

/* Destination rectangle on the 800x480 panel (square pixels assumed). integer:
 * snap the height to an integer multiple of h when that fills >= 90%. */
void video_dst_rect(unsigned w, unsigned h, float aspect, int integer,
                    unsigned *x, unsigned *y, unsigned *dw, unsigned *dh)
{
    if (aspect <= 0.01f) aspect = (float)w / (float)h;
    unsigned H = PH, k = PH / h;
    if (integer && k >= 1 && k * h * 10 >= PH * 9) H = k * h;
    unsigned W = (unsigned)(H * aspect + 0.5f);
    if (W > PW) { W = PW; H = (unsigned)(PW / aspect + 0.5f); if (H > PH) H = PH; }
    W &= ~1u;
    *dw = W; *dh = H; *x = ((PW - W) / 2) & ~1u; *y = (PH - H) / 2;
}

static inline void row_to_565(uint16_t *d, const void *s, unsigned w, int fmt)
{
    if (fmt == PF_RGB565) { memcpy(d, s, w * 2); return; }
    if (fmt == PF_XRGB8888) {
        const uint32_t *p = s;
        for (unsigned i = 0; i < w; i++) { uint32_t c = p[i];
            d[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F)); }
        return;
    }
    const uint16_t *p = s;
    for (unsigned i = 0; i < w; i++) { uint16_t c = p[i];
        d[i] = (uint16_t)(((c & 0x7FE0) << 1) | ((c >> 4) & 0x20) | (c & 0x1F)); }
}

/* ---- sw backend ---- */
static uint16_t *sw_ram, *sw_line;
static unsigned sw_w, sw_h, sw_x, sw_y, sw_dw, sw_dh, sw_page = 1, sw_clear = 2;
static float sw_aspect;
static unsigned *sw_xmap, *sw_ymap;
static int sw_ready;
static double sw_pan_t;

static int sw_init(void)
{
    if (fb0_open()) return -1;
    sw_ram = malloc(PW * PH * 2); sw_line = malloc(4096 * 2);
    sw_xmap = malloc(PW * sizeof *sw_xmap); sw_ymap = malloc(PH * sizeof *sw_ymap);
    pan0(0); sw_page = 1;
    return sw_ram && sw_line && sw_xmap && sw_ymap ? 0 : -1;
}

static void sw_frame(const void *data, unsigned w, unsigned h, size_t pitch, int fmt, float aspect)
{
    double t0 = now_s();
    if (w != sw_w || h != sw_h || aspect != sw_aspect) {
        if (w > 4096 || h > 4096) return;
        sw_w = w; sw_h = h; sw_aspect = aspect;
        video_dst_rect(w, h, aspect, 1, &sw_x, &sw_y, &sw_dw, &sw_dh);
        for (unsigned i = 0; i < sw_dw; i++) sw_xmap[i] = (unsigned)(((uint64_t)i * w * 2 + w) / (2 * sw_dw));
        for (unsigned j = 0; j < sw_dh; j++) sw_ymap[j] = (unsigned)(((uint64_t)j * h * 2 + h) / (2 * sw_dh));
        sw_clear = 2;   /* both pages need their borders blanked */
        fprintf(stderr, "retro: video %ux%u -> %ux%u at %u,%u (sw)\n", w, h, sw_dw, sw_dh, sw_x, sw_y);
    }
    unsigned prev = ~0u;
    for (unsigned j = 0; j < sw_dh; j++) {
        uint16_t *d = sw_ram + (size_t)j * sw_dw;
        unsigned sy = sw_ymap[j];
        if (sy == prev) { memcpy(d, d - sw_dw, sw_dw * 2); continue; }
        prev = sy;
        const unsigned char *s = (const unsigned char *)data + (size_t)sy * pitch;
        if (sw_dw == w) { row_to_565(d, s, w, fmt); continue; }
        row_to_565(sw_line, s, w, fmt);
        if (sw_dw == 2 * w) {
            uint32_t *d32 = (uint32_t *)d;
            for (unsigned i = 0; i < w; i++) { uint32_t p = sw_line[i]; d32[i] = p | (p << 16); }
        } else
            for (unsigned i = 0; i < sw_dw; i++) d[i] = sw_line[sw_xmap[i]];
    }
    sw_ready = 1;
    vstat_conv += now_s() - t0;
}

static void sw_present(void)
{
    if (!sw_ready) return;
    double t0 = now_s();
    vsync_guard(sw_pan_t);              /* the back page may still be on screen */
    uint16_t *pg = fb0m + (size_t)sw_page * PH * line0;
    if (sw_clear) { memset(pg, 0, (size_t)line0 * PH * 2); sw_clear--; }
    if (sw_dw == PW && line0 == PW)
        memcpy(pg + (size_t)sw_y * PW, sw_ram, (size_t)PW * sw_dh * 2);
    else
        for (unsigned j = 0; j < sw_dh; j++)
            memcpy(pg + (size_t)(sw_y + j) * line0 + sw_x, sw_ram + (size_t)j * sw_dw, sw_dw * 2);
    pan0(sw_page);
    sw_pan_t = now_s();
    sw_page ^= 1; sw_ready = 0;
    vstat_present += sw_pan_t - t0;
}

static void fb0_fill(uint16_t c)
{
    if (!fb0m) return;
    size_t n = (size_t)line0 * 2 * PH;
    for (size_t i = 0; i < n; i++) fb0m[i] = c;
}

static void sw_close(void) { fb0_restore(); }

const VideoBackend video_sw = { "sw", sw_init, sw_frame, sw_present, fb0_fill, sw_close };

/* ---- overlay backend (fb1 / vid1, DSS scaler with its stock coefficients) ---- */
static int fd1 = -1, ov_saved, ov_pages;
static struct fb_var_screeninfo ov_orig, ov_var;
static struct ofb_plane ov_porig;
static struct ofb_mem ov_morig;
static unsigned char *ov_map; static size_t ov_maplen; static unsigned ov_line;
static unsigned ov_w, ov_h, ov_page; static float ov_aspect;
static int ov_ready; static double ov_pan_t;

static int ov_init(void)
{
    if (fb0_open()) return -1;
    pan0(0);
    fd1 = open("/dev/fb1", O_RDWR);
    if (fd1 < 0 || ioctl(fd1, FBIOGET_VSCREENINFO, &ov_orig) < 0 ||
        ioctl(fd1, OMAPFB_QUERY_PLANE, &ov_porig) < 0 || ioctl(fd1, OMAPFB_QUERY_MEM, &ov_morig) < 0) {
        perror("retro: overlay fb1"); return -1;
    }
    ov_saved = 1;
    return 0;
}

static int ov_config(unsigned w, unsigned h, float aspect)
{
    struct ofb_plane pl = ov_porig; pl.enabled = 0;
    ioctl(fd1, OMAPFB_SETUP_PLANE, &pl);
    if (ov_map) { munmap(ov_map, ov_maplen); ov_map = NULL; }
    for (ov_pages = 2; ov_pages >= 1; ov_pages--) {
        struct ofb_mem mi = ov_morig;
        mi.size = (uint32_t)(((size_t)w * h * 2 * ov_pages + 4095) & ~(size_t)4095);
        if (ioctl(fd1, OMAPFB_SETUP_MEM, &mi) < 0) continue;
        struct fb_var_screeninfo v = ov_orig;
        v.xres = v.xres_virtual = w; v.yres = h; v.yres_virtual = h * ov_pages;
        v.xoffset = v.yoffset = 0; v.bits_per_pixel = 16; v.rotate = 0; v.nonstd = 0;
        memset(&v.red, 0, 3 * sizeof v.red); memset(&v.transp, 0, sizeof v.transp);
        v.red.offset = 11; v.red.length = 5; v.green.offset = 5; v.green.length = 6; v.blue.length = 5;
        v.activate = FB_ACTIVATE_NOW;
        if (ioctl(fd1, FBIOPUT_VSCREENINFO, &v) < 0) continue;
        ioctl(fd1, FBIOGET_VSCREENINFO, &ov_var);
        if (ov_var.yres_virtual < h * ov_pages) continue;
        break;
    }
    if (ov_pages < 1) { perror("retro: overlay mode"); return -1; }
    struct fb_fix_screeninfo f;
    ioctl(fd1, FBIOGET_FSCREENINFO, &f);
    ov_line = f.line_length; ov_maplen = f.smem_len;
    ov_map = mmap(NULL, ov_maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    if (ov_map == MAP_FAILED) { ov_map = NULL; perror("retro: overlay mmap"); return -1; }
    memset(ov_map, 0, ov_maplen);
    unsigned x, y, dw, dh;
    video_dst_rect(w, h, aspect, 0, &x, &y, &dw, &dh);
    pl = ov_porig; pl.pos_x = x; pl.pos_y = y; pl.out_width = dw; pl.out_height = dh; pl.enabled = 1;
    if (ioctl(fd1, OMAPFB_SETUP_PLANE, &pl) < 0) { perror("retro: overlay plane"); return -1; }
    ov_w = w; ov_h = h; ov_aspect = aspect; ov_page = ov_pages > 1 ? 1 : 0;
    fprintf(stderr, "retro: video %ux%u -> %ux%u at %u,%u (overlay, %d page%s)\n",
            w, h, dw, dh, x, y, ov_pages, ov_pages > 1 ? "s" : "");
    return 0;
}

static void ov_frame(const void *data, unsigned w, unsigned h, size_t pitch, int fmt, float aspect)
{
    double t0 = now_s();
    if ((w != ov_w || h != ov_h || aspect != ov_aspect) && ov_config(w, h, aspect)) { ov_w = 0; return; }
    if (!ov_map) return;
    if (ov_pages > 1) vsync_guard(ov_pan_t);
    unsigned char *pg = ov_map + (size_t)ov_page * h * ov_line;
    for (unsigned j = 0; j < h; j++)
        row_to_565((uint16_t *)(pg + (size_t)j * ov_line), (const unsigned char *)data + j * pitch, w, fmt);
    ov_ready = 1;
    vstat_conv += now_s() - t0;
}

static void ov_present(void)
{
    if (!ov_ready) return;
    double t0 = now_s();
    if (ov_pages > 1) {
        ov_var.yoffset = ov_page * ov_h; ov_var.xoffset = 0;
        ioctl(fd1, FBIOPAN_DISPLAY, &ov_var);
        ov_page ^= 1;
    }
    ov_pan_t = now_s(); ov_ready = 0;
    vstat_present += ov_pan_t - t0;
}

static void ov_close(void)
{
    if (fd1 >= 0 && ov_saved) {
        struct ofb_plane off = ov_porig; off.enabled = 0;
        ioctl(fd1, OMAPFB_SETUP_PLANE, &off);
        if (ov_map) munmap(ov_map, ov_maplen);
        ioctl(fd1, OMAPFB_SETUP_MEM, &ov_morig);
        ioctl(fd1, FBIOPUT_VSCREENINFO, &ov_orig);
        ioctl(fd1, OMAPFB_SETUP_PLANE, &ov_porig);
        ov_saved = 0;
    }
    fb0_restore();
}

const VideoBackend video_overlay = { "overlay", ov_init, ov_frame, ov_present, fb0_fill, ov_close };
