/* retro pause menu: Resume / Save state / Load state / Slot / Quit.
 * Drawn INTO a dimmed copy of the core's last frame at core resolution (6x9 font from
 * common/font.c) and sent through the active video backend, so overlay and sw show it
 * unchanged and no plane, scale_mode or page state has to be touched.  The core is not
 * run while the menu is up; the audio ring is emptied by the caller.  States live in
 * <data>/states/<system>/<content or "nogame">.s<N>, written tmp + fsync + rename. */
#include "retro.h"
#include "font.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STATES "/home/root/data/retro/states"
#define NSLOT 3
#define NITEM 5

static uint16_t *img;
static unsigned iw, ih;

static void text(int x, int y, const char *s, uint16_t col, int sc)
{
    for (; *s; s++, x += FONT_ADVANCE * sc) {
        if ((unsigned char)*s < 0x20) continue;
        const uint16_t *g = font_glyph((unsigned char)*s);
        for (int c = 0; c < FONT_COLS; c++)
            for (int r = 0; r < FONT_ROWS; r++)
                if (g[c] & (1u << r))
                    for (int dy = 0; dy < sc; dy++)
                        for (int dx = 0; dx < sc; dx++) {
                            int px = x + c * sc + dx, py = y + r * sc + dy;
                            if (px >= 0 && py >= 0 && (unsigned)px < iw && (unsigned)py < ih) img[(unsigned)py * iw + (unsigned)px] = col;
                        }
    }
}

static void box(int x, int y, int w, int h, uint16_t col)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && (unsigned)i < iw && (unsigned)j < ih) img[(unsigned)j * iw + (unsigned)i] = col;
}

static void slot_path(char *p, size_t n, const char *sys, const char *content, int slot)
{
    const char *b = strrchr(content, '/'); b = b ? b + 1 : content;
    if (!strcmp(content, "-")) b = "nogame";
    snprintf(p, n, "%s/%s/%s.s%d", STATES, sys, b, slot);
}

static void slot_when(const char *path, char *out, size_t n)
{
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(out, n, "empty"); return; }
    struct tm tm; time_t t = st.st_mtime; tzset(); localtime_r(&t, &tm);
    strftime(out, n, "%m-%d %H:%M", &tm);
}

static void mkdirs(const char *file)
{
    char d[430]; snprintf(d, sizeof d, "%s", file);
    char *e = strrchr(d, '/'); if (e) *e = 0;
    for (char *p = d + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
    mkdir(d, 0755);
}

static void do_save(const struct menu_core *c, const char *path, char *msg, size_t mn, int slot)
{
    double t0 = now_s();
    size_t sz = c->size();
    void *buf = sz ? malloc(sz) : NULL;
    if (!buf || !c->ser(buf, sz)) {
        snprintf(msg, mn, "Save failed: core refused (%zu bytes)", sz);
        fprintf(stderr, "retro: state save failed: serialize returned false, size %zu\n", sz);
        free(buf); return;
    }
    char tmp[430]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdirs(path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(buf, 1, sz, f) == sz && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, path) != 0) ok = 0;
    if (!ok) { unlink(tmp); snprintf(msg, mn, "Save failed: %s", strerror(errno)); }
    else snprintf(msg, mn, "Saved to slot %d", slot);
    fprintf(stderr, "retro: state save %s %s (%zu bytes, %.0f ms)\n", path, ok ? "ok" : "FAILED", sz, 1000 * (now_s() - t0));
    free(buf);
}

static int do_load(const struct menu_core *c, const char *path, char *msg, size_t mn, int slot)
{
    double t0 = now_s();
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, mn, "Slot %d is empty", slot); return 0; }
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    size_t sz = c->size();
    if (fl < 0 || (size_t)fl != sz || !sz) {
        snprintf(msg, mn, "Load failed: size %ld, core wants %zu", fl, sz);
        fprintf(stderr, "retro: state load %s rejected: file %ld bytes, core %zu\n", path, fl, sz);
        fclose(f); return 0;
    }
    void *buf = malloc(sz);
    int ok = buf && fread(buf, 1, sz, f) == sz;
    fclose(f);
    if (!ok) snprintf(msg, mn, "Load failed: read error");
    else if (!(ok = c->unser(buf, sz))) snprintf(msg, mn, "Load failed: core refused");
    else snprintf(msg, mn, "Loaded slot %d", slot);
    fprintf(stderr, "retro: state load %s %s (%zu bytes, %.0f ms)\n", path, ok ? "ok" : "FAILED", sz, 1000 * (now_s() - t0));
    free(buf);
    return ok;
}

int menu_run(const VideoBackend *vb, const struct menu_core *c, const uint16_t *bg, unsigned w, unsigned h,
             float aspect, const char *sys, const char *content, const volatile int *stop)
{
    free(img); img = malloc((size_t)w * h * 2); iw = w; ih = h;
    if (!img) return 0;
    int sc = w >= 640 ? 2 : 1, sel = 0, slot = 1, dirty = 1, ret = 0, done = 0;
    char msg[96] = "";
    in_menu_mode(true);
    while (!done && !*stop) {
        if (dirty) {
            for (size_t i = 0; i < (size_t)w * h; i++) img[i] = (uint16_t)((bg[i] >> 1) & 0x7BEF);
            int lh = 12 * sc, bw = 30 * FONT_ADVANCE * sc + 8 * sc, bh = (NITEM + 4) * lh + 8 * sc;
            int bx = ((int)w - bw) / 2, by = ((int)h - bh) / 2;
            box(bx, by, bw, bh, 0x0010); box(bx, by, bw, sc, 0xFFE0); box(bx, by + bh - sc, bw, sc, 0xFFE0);
            text(bx + 4 * sc, by + 4 * sc, "PAUSED", 0xFFE0, sc);
            static const char *name[NITEM] = { "Resume", "Save state", "Load state", "", "Quit" };
            for (int i = 0; i < NITEM; i++) {
                char line[64], p[420], when[24];
                if (i == 3) {
                    slot_path(p, sizeof p, sys, content, slot); slot_when(p, when, sizeof when);
                    snprintf(line, sizeof line, "< Slot %d: %s >", slot, when);
                } else snprintf(line, sizeof line, "%s", name[i]);
                text(bx + 4 * sc, by + 4 * sc + (i + 2) * lh, i == sel ? "\x10" : " ", 0xFFFF, sc);
                text(bx + 4 * sc + 2 * FONT_ADVANCE * sc, by + 4 * sc + (i + 2) * lh, line, i == sel ? 0xFFFF : 0xAD55, sc);
            }
            text(bx + 4 * sc, by + bh - 2 * lh, msg, 0x07FF, sc);
            vb->frame(img, w, h, (size_t)w * 2, PF_RGB565, aspect); vb->present();
            dirty = 0;
        }
        in_poll(); aud_service();
        int ev = in_menu_ev();
        if (!ev) { usleep(20000); continue; }
        char p[420]; slot_path(p, sizeof p, sys, content, slot);
        dirty = 1;
        if (ev == MEV_UP) sel = (sel + NITEM - 1) % NITEM;
        else if (ev == MEV_DOWN) sel = (sel + 1) % NITEM;
        else if (ev == MEV_LEFT && sel == 3) slot = (slot + NSLOT - 2) % NSLOT + 1;
        else if (ev == MEV_RIGHT && sel == 3) slot = slot % NSLOT + 1;
        else if (ev == MEV_BACK) done = 1;
        else if (ev == MEV_OK) {
            if (sel == 0) done = 1;
            else if (sel == 1) do_save(c, p, msg, sizeof msg, slot);
            else if (sel == 2) do_load(c, p, msg, sizeof msg, slot);
            else if (sel == 3) slot = slot % NSLOT + 1;
            else { ret = 1; done = 1; }
        } else dirty = 0;
    }
    in_menu_mode(false);
    return ret;
}
