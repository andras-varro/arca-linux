/* vid_player.c — see vid_player.h.  The main thread consumes both rings (the mix-bus fill callback runs inside
 * audio_pump(), so it is the PCM ring reader); the decoder thread produces. */
#include "vid_player.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "audio_gen.h"
#include "font.h"
#include "http.h"
#include "utf8.h"
#include "vid_clock.h"
#include "vid_dec.h"
#include "vid_level.h"
#include "vid_ring.h"
#include "vid_srt.h"

#define VBUF_FRAMES   1024
#define VOICE_TOTAL   (1L << 30)
#define FRAME_SLOTS   16
#define PCM_FRAMES    (1u << 17)
#define SUB_CAP       (2u * 1024 * 1024)
#define STATS_MS      10000
#define STALL_MS      20000

static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

typedef struct { int sub_fd; bool tls; uint8_t *buf; size_t len, cap; VidSrt srt; bool parsed;
                 int cue; uint32_t *spr; int sw, sh, scap_w, scap_h; bool spr_on; } Subs;

struct VidPlayer {
    Audio *audio; bool audio_ok; Framebuffer *fb; Logger *log;
    VidPlayerCfg cfg;
    bool on, paused;
    int fd, dec_fd, relay_w;                 /* stream; what vid_dec reads; relay pipe write end (-1: none) */
    pthread_t th, relay_th; bool th_up, relay_up;
    atomic_int stop, ready, done, result;    /* ready: rings built; done: thread over; result: vid_dec_run's */
    VidDec *dec; VidDecInfo info; VidFrameRing fr; VidPcmRing pr; VidLevel lvl;
    int16_t vbuf[VBUF_FRAMES * 2]; int slot; uint32_t gen; bool voice, fill_on, base_set; double base_pts;
    long long padded; unsigned underruns;
    VidClock clk; bool clk_on; double first_pts; bool first_set; double mono0; double wait_s; bool wait_set;
    double frozen_pos; bool has_pos; double buf_since;
    bool dry_seen;                           /* a tick found the frame ring empty since the last frame left it */
    long drop_late, drop_starved, empty_ticks, last_late, last_starved, last_empty;
    uint32_t *line; uint8_t clampt[1024];
    int16_t yy[256], crr[256], cbb[256], crg[256], cbg[256];
    int px0, py0, pw, ph;                    /* picture rectangle, logical coordinates */
    long shown, dropped, last_shown, last_dropped; double err_sum, err_max; long err_n; double stats_next;
    Subs sub;
};

/* ── mix-bus voice ───────────────────────────────────────────────────────── */
static long vp_fill(void *ctx, int16_t *dst, long frames)
{
    VidPlayer *p = ctx;
    if (!p->fill_on) return 0;
    int nch = p->pr.nch;
    long got = 0;
    while (got < frames) {
        double pts;
        unsigned n = vid_pring_read(&p->pr, dst + got * nch, (unsigned)(frames - got), &pts);
        if (n == 0) break;
        if (!p->base_set) { p->base_set = true; p->base_pts = pts; }
        got += n;
    }
    if (got < frames) {                      /* dry: pad silence and tell the clock it was not audio */
        memset(dst + got * nch, 0, (size_t)(frames - got) * (size_t)nch * sizeof(int16_t));
        if (p->base_set) { p->padded += frames - got; p->underruns++; }
    }
    return frames;
}

static void voice_release(VidPlayer *p)
{
    p->fill_on = false;
    if (p->voice && p->audio_ok) audio_mix_release_voice(&p->audio->mix, p->slot, p->gen);
    p->voice = false;
}

static bool voice_add(VidPlayer *p)
{
    if (!p->audio_ok) return false;
    p->base_set = false; p->padded = 0; p->fill_on = true;
    int peak = audio_voice_peak(p->audio->vol);
    if (peak < 1) peak = 1;
    int slot = audio_mix_add_sample_ch(&p->audio->mix, vp_fill, p, p->vbuf, VBUF_FRAMES, VOICE_TOTAL, peak, p->pr.nch);
    if (slot < 0) { p->fill_on = false; return false; }
    p->slot = slot; p->gen = audio_mix_voice_gen(&p->audio->mix, slot); p->voice = true; p->clk_on = false;
    return true;
}

/* ── threads ─────────────────────────────────────────────────────────────── */
static void pcm_cb(void *u, const float *in, int frames, int16_t *out) { vid_level_process(u, in, frames, out); }

static void *dec_main(void *arg)
{
    VidPlayer *p = arg;
    setpriority(PRIO_PROCESS, 0, 5);         /* this thread only (Linux), normal scheduling class */
    int rc = -1;
    if (vid_dec_open(&p->dec, p->dec_fd, &p->stop) == 0) {
        p->info = *vid_dec_info(p->dec);
        int nch = p->cfg.channels == 2 ? 2 : 1;
        int rate = p->info.audio_rate > 0 ? p->info.audio_rate : 44100;
        if (vid_fring_init(&p->fr, FRAME_SLOTS, p->info.w, p->info.h) == 0 &&
            vid_pring_init(&p->pr, PCM_FRAMES, nch, rate) == 0) {
            vid_level_init(&p->lvl, rate, nch);
            vid_level_set(&p->lvl, p->cfg.agc, 6.0f);
            vid_clock_init(&p->clk, rate);
            VidDecCfg dc = { .out_channels = nch, .pcm_cb = pcm_cb, .user = &p->lvl, .pcm_target_s = 1.0 };
            atomic_store_explicit(&p->ready, 1, memory_order_release);
            rc = vid_dec_run(p->dec, &p->fr, &p->pr, &dc);
        }
    }
    atomic_store(&p->result, rc);
    atomic_store_explicit(&p->done, 1, memory_order_release);
    return NULL;
}

/* TLS: vid_dec reads the raw fd, so decrypt here and hand it a pipe */
static void *relay_main(void *arg)
{
    VidPlayer *p = arg;
    uint8_t buf[16384];
    while (!atomic_load(&p->stop)) {
        ssize_t n = http_stream_read(p->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            struct pollfd pf = { .fd = p->fd, .events = POLLIN };
            poll(&pf, 1, 100);
            continue;
        }
        if (n == 0) break;
        for (ssize_t off = 0; off < n && !atomic_load(&p->stop);) {
            struct pollfd pf = { .fd = p->relay_w, .events = POLLOUT };
            if (poll(&pf, 1, 100) <= 0) continue;
            ssize_t w = write(p->relay_w, buf + off, (size_t)(n - off));
            if (w > 0) off += w;
            else if (w < 0 && errno != EINTR && errno != EAGAIN) goto out;
        }
    }
out:
    close(p->relay_w);
    p->relay_w = -1;
    return NULL;
}

/* ── subtitles ───────────────────────────────────────────────────────────── */
static void subs_reset(Subs *s, bool keep_buf)
{
    if (s->sub_fd >= 0) { if (s->tls) http_stream_close(s->sub_fd); else close(s->sub_fd); s->sub_fd = -1; }
    if (s->parsed) { vid_srt_free(&s->srt); s->parsed = false; }
    s->len = 0; s->cue = -2; s->spr_on = false;
    if (!keep_buf) { free(s->buf); s->buf = NULL; s->cap = 0; free(s->spr); s->spr = NULL; s->scap_w = s->scap_h = 0; }
}

static void subs_poll(Subs *s)
{
    for (int k = 0; k < 4 && s->sub_fd >= 0; k++) {
        if (s->cap - s->len < 16384) {
            size_t nc = s->cap ? s->cap * 2 : 65536;
            if (nc > SUB_CAP + 16384) { subs_reset(s, true); return; }
            uint8_t *nb = realloc(s->buf, nc);
            if (!nb) { subs_reset(s, true); return; }
            s->buf = nb; s->cap = nc;
        }
        ssize_t n = s->tls ? http_stream_read(s->sub_fd, s->buf + s->len, s->cap - s->len)
                           : read(s->sub_fd, s->buf + s->len, s->cap - s->len);
        if (n > 0) { s->len += (size_t)n; if (s->len > SUB_CAP) { subs_reset(s, true); return; } continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        int fd = s->sub_fd;
        s->sub_fd = -1;                                                    /* EOF or error */
        if (s->tls) http_stream_close(fd); else close(fd);
        if (n == 0 && vid_srt_parse(&s->srt, (const char *)s->buf, s->len) >= 0) s->parsed = true;
        return;
    }
}

static int glyph_w6(uint32_t cp, void *u) { (void)cp; return 6 * *(int *)u; }

static void spr_put(Subs *s, int x, int y, int sc, uint32_t cp, uint32_t col)
{
    const uint16_t *g = font_glyph(cp);
    for (int c = 0; c < FONT_COLS; c++)
        for (int r = 0; r < FONT_ROWS; r++) {
            if (!(g[c] & (1u << r))) continue;
            for (int dy = 0; dy < sc; dy++)
                for (int dx = 0; dx < sc; dx++) {
                    int px = x + c * sc + dx, py = y + r * sc + dy;
                    if (px >= 0 && px < s->sw && py >= 0 && py < s->sh) s->spr[(size_t)py * (size_t)s->sw + (size_t)px] = col;
                }
        }
}

/* wrapped text -> ARGB sprite (0 = transparent), a 1 px black outline from four offset passes, then the white pass */
static void sprite_build(Subs *s, const char *text, int sc, int max_px)
{
    VidSrtLine ln[3];
    int n = vid_srt_wrap(text, max_px, glyph_w6, &sc, ln, 3);
    int lh = 10 * sc;
    s->sw = max_px + 4; s->sh = n * lh + 4;
    if (s->scap_w < s->sw || s->scap_h < s->sh) {
        free(s->spr);
        s->scap_w = s->sw; s->scap_h = 3 * lh + 4;
        s->spr = malloc((size_t)s->scap_w * (size_t)s->scap_h * 4);
        if (!s->spr) { s->scap_w = s->scap_h = 0; s->spr_on = false; return; }
    }
    memset(s->spr, 0, (size_t)s->sw * (size_t)s->sh * 4);
    static const int ox[5] = { -1, 1, 0, 0, 0 }, oy[5] = { 0, 0, -1, 1, 0 };
    for (int pass = 0; pass < 5; pass++)
        for (int i = 0; i < n; i++) {
            const char *q = ln[i].p, *e = ln[i].p + ln[i].len;
            int cps = 0;
            for (const char *t = q; t < e; ) { utf8_next(&t); cps++; }
            int x = 2 + ((max_px - cps * 6 * sc) >> 1);
            while (q < e) {
                uint32_t cp = utf8_next(&q);
                spr_put(s, x + ox[pass], 2 + i * lh + oy[pass], sc, cp, pass == 4 ? 0xFFFFFFu : 0x010101u);
                x += 6 * sc;
            }
        }
    s->spr_on = n > 0;
}

/* ── picture ─────────────────────────────────────────────────────────────── */
static void tabs_init(VidPlayer *p)
{
    for (int i = 0; i < 256; i++) {
        p->yy[i]  = (int16_t)(((i - 16) * 298) >> 8);
        p->crr[i] = (int16_t)(((i - 128) * 409) >> 8);
        p->cbb[i] = (int16_t)(((i - 128) * 516) >> 8);
        p->crg[i] = (int16_t)(((i - 128) * 208) >> 8);
        p->cbg[i] = (int16_t)(((i - 128) * 100) >> 8);
    }
    for (int i = 0; i < 1024; i++) { int v = i - 384; p->clampt[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
}

/* YUV420 -> XRGB 2x straight into the panel page: one converted row, written twice (sequential stores, no divide) */
static void present(VidPlayer *p, const VidFrame *f)
{
    Framebuffer *fb = p->fb;
    int w = f->w, h = f->h;
    int maxw = (int)(fb->width >> 1), maxh = (int)(fb->height >> 1);
    int cw = w > maxw ? maxw : w, ch = h > maxh ? maxh : h;
    cw &= ~1;
    int cx = (w - cw) >> 1, cy = (h - ch) >> 1;
    p->pw = cw * 2; p->ph = ch * 2;
    p->px0 = ((int)fb->width - p->pw) >> 1; p->py0 = ((int)fb->height - p->ph) >> 1;
    uint8_t *dst = (uint8_t *)fb->buffer + (size_t)(fb->view_y + (uint32_t)p->py0) * fb->line_length
                 + (size_t)(fb->view_x + (uint32_t)p->px0) * 4;
    const uint8_t *ct = p->clampt + 384;
    for (int y = 0; y < ch; y++) {
        const uint8_t *yr = f->y + (size_t)(y + cy) * (size_t)f->ys + cx;
        const uint8_t *cb = f->cb + (size_t)((y + cy) >> 1) * (size_t)f->cs + (cx >> 1);
        const uint8_t *cr = f->cr + (size_t)((y + cy) >> 1) * (size_t)f->cs + (cx >> 1);
        uint32_t *o = p->line;
        for (int x = 0; x < cw; x += 2) {
            int u = cb[x >> 1], v = cr[x >> 1];
            int rr = p->crr[v], gg = p->cbg[u] + p->crg[v], bb = p->cbb[u];
            int y0 = p->yy[yr[x]], y1 = p->yy[yr[x + 1]];
            uint32_t a = ((uint32_t)ct[y0 + rr] << 16) | ((uint32_t)ct[y0 - gg] << 8) | ct[y0 + bb];
            uint32_t b = ((uint32_t)ct[y1 + rr] << 16) | ((uint32_t)ct[y1 - gg] << 8) | ct[y1 + bb];
            o[0] = a; o[1] = a; o[2] = b; o[3] = b; o += 4;
        }
        memcpy(dst, p->line, (size_t)p->pw * 4); dst += fb->line_length;
        memcpy(dst, p->line, (size_t)p->pw * 4); dst += fb->line_length;
    }
}

static void sprite_blit(VidPlayer *p)
{
    Subs *s = &p->sub;
    if (!s->spr_on || !s->spr) return;
    int x0 = p->px0 + ((p->pw - s->sw) >> 1), y0 = p->py0 + p->ph - s->sh - 8;
    if (x0 < 0 || y0 < 0) return;
    Framebuffer *fb = p->fb;
    for (int r = 0; r < s->sh; r++) {
        uint32_t *d = (uint32_t *)((uint8_t *)fb->buffer + (size_t)(fb->view_y + (uint32_t)(y0 + r)) * fb->line_length)
                    + fb->view_x + x0;
        const uint32_t *sp = s->spr + (size_t)r * (size_t)s->sw;
        for (int c = 0; c < s->sw; c++) if (sp[c]) d[c] = sp[c];
    }
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */
VidPlayer *vid_player_new(Audio *audio, bool audio_ok, Framebuffer *fb, Logger *log)
{
    VidPlayer *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->audio = audio; p->audio_ok = audio_ok; p->fb = fb; p->log = log;
    p->fd = p->dec_fd = p->relay_w = -1; p->sub.sub_fd = -1; p->sub.cue = -2;
    p->line = malloc((size_t)fb->width * 4);
    if (!p->line) { free(p); return NULL; }
    tabs_init(p);
    return p;
}

static void close_stream(VidPlayer *p, int fd) { if (p->cfg.tls) http_stream_close(fd); else close(fd); }

void vid_player_stop(VidPlayer *p)
{
    if (!p->on) return;
    atomic_store(&p->stop, 1);
    voice_release(p);
    if (p->th_up) pthread_join(p->th, NULL);
    if (p->relay_up) pthread_join(p->relay_th, NULL);
    p->th_up = p->relay_up = false;
    if (p->dec) { vid_dec_close(p->dec); p->dec = NULL; }
    if (atomic_load(&p->ready)) { vid_fring_free(&p->fr); vid_pring_free(&p->pr); }
    if (p->dec_fd >= 0 && p->dec_fd != p->fd) close(p->dec_fd);
    if (p->relay_w >= 0) { close(p->relay_w); p->relay_w = -1; }
    if (p->fd >= 0) close_stream(p, p->fd);
    p->fd = p->dec_fd = -1; p->on = false; p->has_pos = false;
}

int vid_player_start(VidPlayer *p, int fd, const VidPlayerCfg *cfg)
{
    vid_player_stop(p);
    p->cfg = *cfg;
    if (p->fb->portrait_mode) { close_stream(p, fd); return -1; }
    p->fd = p->dec_fd = fd;
    atomic_store(&p->stop, 0); atomic_store(&p->ready, 0); atomic_store(&p->done, 0); atomic_store(&p->result, -1);
    p->voice = p->fill_on = p->base_set = p->clk_on = p->first_set = p->paused = p->has_pos = false;
    p->shown = p->dropped = p->last_shown = p->last_dropped = 0; p->err_sum = p->err_max = 0; p->err_n = 0;
    p->dry_seen = false; p->drop_late = p->drop_starved = p->empty_ticks = 0; p->last_late = p->last_starved = p->last_empty = 0;
    p->underruns = 0; p->wait_s = 0; p->wait_set = false; p->stats_next = mono_s() + STATS_MS / 1000.0; p->buf_since = 0;
    p->on = true;
    if (cfg->tls) {
        int pf[2];
        if (pipe(pf) != 0) { vid_player_stop(p); return -1; }
        p->dec_fd = pf[0]; p->relay_w = pf[1];
        if (pthread_create(&p->relay_th, NULL, relay_main, p) != 0) { vid_player_stop(p); return -1; }
        p->relay_up = true;
    } else {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    if (pthread_create(&p->th, NULL, dec_main, p) != 0) { vid_player_stop(p); return -1; }
    p->th_up = true;
    return 0;
}

void vid_player_free(VidPlayer *p)
{
    if (!p) return;
    vid_player_stop(p);
    subs_reset(&p->sub, false);
    free(p->line);
    free(p);
}

bool vid_player_active(const VidPlayer *p) { return p->on; }

long vid_player_position(const VidPlayer *p)
{
    if (!p->on || !p->has_pos) return -1;
    if (p->paused) return (long)p->frozen_pos;
    double t = (p->audio_ok && p->info.has_audio) ? vid_clock_now((VidClock *)&p->clk, mono_s()) : mono_s() - p->mono0;
    return p->cfg.start_ms + (long)((t - p->first_pts) * 1000.0);
}

void vid_player_pause(VidPlayer *p, bool paused)
{
    if (!p->on || p->paused == paused) return;
    if (paused) { p->frozen_pos = (double)vid_player_position(p); p->paused = true; voice_release(p); }
    else {
        p->paused = false;               /* tick re-adds the voice once the ring holds sound */
        p->buf_since = 0;
        if (!(p->audio_ok && p->info.has_audio)) p->mono0 = mono_s() - (p->frozen_pos - (double)p->cfg.start_ms) * 0.001 - p->first_pts;
    }
}

int vid_player_wait_us(const VidPlayer *p, int max_us)
{
    if (!p->on || !p->wait_set) return max_us;
    /* wake 2 ms ahead of the due time: a frame up to VC_EARLY (4 ms) early is shown, and usleep overshoots */
    int us = (int)(p->wait_s * 1e6) - 2000;
    return us < 500 ? 500 : us > max_us ? max_us : us;
}

void vid_player_sub_clear(VidPlayer *p) { subs_reset(&p->sub, true); }

void vid_player_sub_feed(VidPlayer *p, int fd, bool tls)
{
    subs_reset(&p->sub, true);
    p->sub.sub_fd = fd; p->sub.tls = tls;
    if (!tls) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int finish(VidPlayer *p, int code, const char *why)
{
    if (p->log) LOG_INFO(p->log, "player: ended code %d (%s) shown %ld dropped %ld underruns %u", code, why, p->shown, p->dropped, p->underruns);
    vid_player_stop(p);
    return code;
}

int vid_player_tick(VidPlayer *p, unsigned long long now_ms)
{
    (void)now_ms;
    subs_poll(&p->sub);
    if (!p->on) return -1;
    double mono = mono_s();
    if (!atomic_load_explicit(&p->ready, memory_order_acquire)) {
        if (atomic_load_explicit(&p->done, memory_order_acquire)) return finish(p, 0, "no stream headers");
        return -1;
    }
    bool done = atomic_load_explicit(&p->done, memory_order_acquire);
    bool audio = p->audio_ok && p->info.has_audio;
    unsigned pcm = vid_pring_fill(&p->pr), fq = vid_fring_count(&p->fr);
    unsigned rate = (unsigned)(p->info.audio_rate > 0 ? p->info.audio_rate : 44100);
    if (done && atomic_load(&p->result) < 0) return finish(p, 0, "decoder error");
    if (p->paused) return -1;

    /* voice: start once sound is queued; drop it (buffering) when the ring runs dry */
    if (audio) {
        if (p->voice && pcm < rate / 10 && !done) {
            voice_release(p); p->buf_since = mono; p->clk_on = false;
        } else if (!p->voice && ((pcm >= (p->first_set ? rate : rate / 2) && fq >= 2) || (done && pcm > 0))) {
            if (voice_add(p)) p->buf_since = 0;
        }
        if (!p->voice && p->buf_since == 0) p->buf_since = mono;
        if (!p->voice && !done && mono - p->buf_since > STALL_MS / 1000.0) return finish(p, 0, "stalled");
        if (p->voice && p->base_set) {
            if (!p->clk_on) {
                vid_clock_start(&p->clk, p->base_pts);
                vid_clock_set_offset(&p->clk, p->cfg.offset_ms * 0.001);
                p->clk_on = true;
                if (!p->first_set) { p->first_set = true; p->first_pts = p->base_pts; p->has_pos = true; }
            }
            long rendered = VOICE_TOTAL - audio_mix_voice_pending(&p->audio->mix, p->slot, p->gen);
            vid_clock_sample(&p->clk, rendered, p->padded, audio_pump_lead(p->audio), mono);
        }
    } else if (!p->clk_on && fq > 0) {                      /* picture only: a free-running clock */
        const VidFrame *f = vid_fring_peek(&p->fr);
        p->mono0 = mono - f->pts; p->first_pts = f->pts; p->first_set = p->clk_on = p->has_pos = true;
        vid_clock_set_offset(&p->clk, p->cfg.offset_ms * 0.001);
    }
    if (!p->clk_on) {
        if (done && fq == 0 && pcm == 0) return finish(p, p->shown > 0 ? 1 : 0, "eof");
        return -1;
    }

    double t = audio ? vid_clock_now(&p->clk, mono) : mono - p->mono0;
    double period = p->info.fps > 1.0 ? 1.0 / p->info.fps : 0.04;
    p->wait_set = false;
    if (fq == 0 && !done) { p->dry_seen = true; p->empty_ticks++; }
    for (int i = 0; i < 4; i++) {
        const VidFrame *f = vid_fring_peek(&p->fr);
        if (!f) break;
        int d = vid_clock_decide(&p->clk, f->pts, t, period);
        if (d == VID_CLK_WAIT) break;
        if (d == VID_CLK_DROP) {
            /* starved: the ring ran dry before this frame arrived (decoder late); late: it was queued in time and this loop missed it */
            if (p->dry_seen) p->drop_starved++; else p->drop_late++;
            p->dry_seen = false;
            vid_fring_release(&p->fr); p->dropped++; continue;
        }
        present(p, f);
        /* error as the viewer gets it: the clock read after the picture is on the panel, not at the start of the tick */
        double tp = audio ? vid_clock_now(&p->clk, mono_s()) : mono_s() - p->mono0;
        double e = f->pts + p->clk.offset - tp;
        if (e < 0) e = -e;
        p->err_sum += e; p->err_n++;
        if (e > p->err_max) p->err_max = e;
        vid_fring_release(&p->fr);
        p->dry_seen = false;
        p->shown++;
        Subs *s = &p->sub;
        if (s->parsed) {
            int cue = vid_srt_find(&s->srt, (int)vid_player_position(p));
            if (cue != s->cue) {
                s->cue = cue;
                if (cue < 0) s->spr_on = false;
                else { int sc = p->cfg.sub_scale == 3 ? 3 : 2; sprite_build(s, vid_srt_text(&s->srt, cue), sc, sc == 2 ? 720 : 760); }
            }
            sprite_blit(p);
        }
        break;
    }
    if (done && vid_fring_count(&p->fr) == 0 && vid_pring_fill(&p->pr) == 0) return finish(p, 1, "eof");
    /* When to come back: at the next frame's due time, measured from NOW (the present above took milliseconds,
     * and a fixed sleep after it put every following frame later until one was dropped), or soon if none is queued. */
    {
        double tn = audio ? vid_clock_now(&p->clk, mono_s()) : mono_s() - p->mono0;
        const VidFrame *nf = vid_fring_peek(&p->fr);
        p->wait_s = nf ? nf->pts + p->clk.offset - tn : 0.004;
        p->wait_set = true;
    }
    if (mono >= p->stats_next && p->log) {
        const VidDecStats *st = vid_dec_stats(p->dec);
        LOG_INFO(p->log, "player: decoded %ld shown %ld dropped %ld (late %ld starved %ld) empty-ticks %ld underruns %u av-err mean %.1f max %.1f ms",
                 atomic_load(&st->frames), p->shown - p->last_shown, p->dropped - p->last_dropped,
                 p->drop_late - p->last_late, p->drop_starved - p->last_starved, p->empty_ticks - p->last_empty, p->underruns,
                 p->err_n ? p->err_sum * 1000.0 / (double)p->err_n : 0.0, p->err_max * 1000.0);
        p->last_shown = p->shown; p->last_dropped = p->dropped; p->err_sum = p->err_max = 0; p->err_n = 0;
        p->last_late = p->drop_late; p->last_starved = p->drop_starved; p->last_empty = p->empty_ticks;
        p->stats_next = mono + STATS_MS / 1000.0;
    }
    return -1;
}
