/* retro — libretro frontend for Arca (Ludbox Linux).
 *
 *   retro [-v sw|overlay] <core.so> <content|->
 *
 * Runs the core at its own fps against the 60.5 Hz panel and shows the newest
 * frame (video backends: retro_video.c), downmixes and resamples its audio to
 * the one audio_out stream (retro_audio.c), maps USB pads/keyboards
 * (retro_input.c).  Pause menu (save/load state, quit): pad Select+Start, keyboard F12.
 *
 * Env: RETRO_VIDEO=sw|overlay (the -v flag wins; default from
 * /home/root/data/retro/retro.cfg line "video=...", else sw), RETRO_SYS
 * (system dir), RETRO_SAVES, RETRO_OPTS="k=v,k=v", RETRO_LOGLEVEL (core log
 * threshold 0 debug..3 error, default 2), RETRO_FRAMES=N (exit after N frames).
 * Core options: legacy SET_VARIABLES defaults, then built-in defaults for the
 * slow cores below, then <opts dir>/<core>.opt (key=value lines), then RETRO_OPTS, then
 * <content path>.opt (per game, beside the content, key=value lines, wins over all).
 * One stats line per second on stderr. */
#define _GNU_SOURCE
#include "retro.h"
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DATA "/home/root/data/retro"

struct sysinfo_ { const char *name, *version, *exts; bool need_fullpath, block_extract; };
struct geom { unsigned bw, bh, mw, mh; float aspect; };
struct avinfo { struct geom g; double fps, rate; };
struct gameinfo { const char *path; const void *data; size_t size; const char *meta; };
struct var_ { const char *key, *value; };
struct ftcb_ { void (*cb)(int64_t); int64_t ref; };
struct kbcb_ { void (*cb)(bool, unsigned, uint32_t, uint16_t); };

double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int pixfmt = PF_0RGB1555, loglevel = 2;
static const char *sysdir = DATA "/system", *savedir = DATA "/saves";
static char corename[64];
static struct ftcb_ ftcb;
static struct avinfo av;
static const VideoBackend *vb = &video_sw;
static bool want_video, quit_core, vars_changed;
static bool ff_on;      /* core asked for fast-forward (VICE warp while loading): unthrottled, sparse picture, no audio */
static bool no_game_ok;                 /* the core declared SET_SUPPORT_NO_GAME (cmd 18) */
static unsigned long st_vframes;
static bool grab;                       /* video_cb keeps the next frame as RGB565 for the pause menu */
static uint16_t *bgbuf; static unsigned bg_w, bg_h; static float bg_aspect;

struct var { const char *key; char val[64]; };
static struct var vars[512]; static int nvars;
static char *ovr[256]; static int novr;   /* "key=value" overrides, in priority order */

static const char *builtin_defaults[] = {
    "vice_sid_engine=FastSID", "vice_sound_sample_rate=22050",   /* reSID: 19.5 s first frame */
    "ep128emu_wait=5", "ep128emu_sdhq=0", NULL };

static void add_ovr(const char *kv) { if (novr < 256 && strchr(kv, '=')) ovr[novr++] = strdup(kv); }

static void opt_file(const char *p)
{
    char line[256];
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s && *s != '#') add_ovr(s);
    }
    fclose(f); fprintf(stderr, "retro: options from %s\n", p);
}

static void load_overrides(const char *content)
{
    for (int i = 0; builtin_defaults[i]; i++) add_ovr(builtin_defaults[i]);
    char p[512];
    const char *od = getenv("RETRO_OPTDIR") ? getenv("RETRO_OPTDIR") : DATA "/options";
    snprintf(p, sizeof p, "%s/%s.opt", od, corename);
    opt_file(p);
    const char *e = getenv("RETRO_OPTS");
    while (e && *e) {
        size_t n = strcspn(e, ","); char kv[160];
        if (n < sizeof kv) { memcpy(kv, e, n); kv[n] = 0; add_ovr(kv); }
        e += n; if (*e) e++;
    }
    /* per game, last so it wins over the system opts=: <content path>.opt, beside the content */
    if (strcmp(content, "-")) {
        snprintf(p, sizeof p, "%s.opt", content);
        opt_file(p);
    }
}

static void store_vars(const struct var_ *v)
{
    for (; v->key && nvars < 512; v++) {
        struct var *d = &vars[nvars++];
        d->key = v->key; d->val[0] = 0;
        const char *s = strstr(v->value, "; ");
        if (s) { s += 2; size_t n = strcspn(s, "|"); if (n >= sizeof d->val) n = sizeof d->val - 1;
                 memcpy(d->val, s, n); d->val[n] = 0; }
        size_t kl = strlen(d->key);
        for (int i = 0; i < novr; i++)            /* last match wins */
            if (!strncmp(ovr[i], d->key, kl) && ovr[i][kl] == '=') {
                snprintf(d->val, sizeof d->val, "%s", ovr[i] + kl + 1);
                fprintf(stderr, "retro: option %s=%s\n", d->key, d->val);
            }
    }
}

static void logfn(int level, const char *fmt, ...)
{
    if (level < loglevel) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[core %d] ", level); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* libretro disk control (cmd 13 legacy = first 7 members, 58 ext = all 10); NULL members are tolerated */
struct diskcb_ {
    bool (*set_eject)(bool); bool (*get_eject)(void); unsigned (*get_index)(void); bool (*set_index)(unsigned);
    unsigned (*get_num)(void); bool (*replace)(unsigned, const void *); bool (*add)(void);
    bool (*set_initial)(unsigned, const char *); bool (*get_path)(unsigned, char *, size_t);
    bool (*get_label)(unsigned, char *, size_t);
};
static struct diskcb_ disk;
static unsigned disk_n(void) { return disk.get_num ? disk.get_num() : 0; }
static unsigned disk_cur(void) { return disk.get_index ? disk.get_index() : 0; }
/* eject -> select -> insert, as a frontend must; the core attaches the image on insert */
static bool disk_set(unsigned i)
{
    if (!disk.set_eject || !disk.set_index || i >= disk_n()) return false;
    disk.set_eject(true);
    bool ok = disk.set_index(i);
    disk.set_eject(false);
    fprintf(stderr, "retro: disk %u/%u %s\n", i + 1, disk_n(), ok ? "inserted" : "REFUSED");
    return ok;
}

static int env_cb(unsigned cmd, void *data)
{
    switch (cmd & 0xffff) {
    case 13: memset(&disk, 0, sizeof disk);                         /* SET_DISK_CONTROL_INTERFACE */
             if (data) memcpy(&disk, data, 7 * sizeof(void *));
             return 1;
    case 58: memset(&disk, 0, sizeof disk);                         /* SET_DISK_CONTROL_EXT_INTERFACE */
             if (data) memcpy(&disk, data, sizeof disk);
             return 1;
    case 57: *(unsigned *)data = 1; return 1;                       /* GET_DISK_CONTROL_INTERFACE_VERSION */
    case 3: *(bool *)data = true; return 1;                         /* GET_CAN_DUPE */
    case 49: if (data) *(bool *)data = ff_on; return 1;              /* GET_FASTFORWARDING */
    case 64: if (data) ff_on = ((struct { float ratio; bool ff, notify, inhibit; } *)data)->ff;   /* SET_FASTFORWARDING_OVERRIDE */
             return 1;
    case 6: fprintf(stderr, "retro: core message: %s\n", *(const char **)data); return 1;
    case 7: quit_core = true; return 1;                              /* SHUTDOWN */
    case 8: return 1;                                                /* SET_PERFORMANCE_LEVEL */
    case 9: *(const char **)data = sysdir; return 1;
    case 10: { int f = *(int *)data; if (f < 0 || f > 2) return 0; pixfmt = f; return 1; }
    case 11: return 1;                                               /* SET_INPUT_DESCRIPTORS */
    case 12: in_kbd_cb = ((struct kbcb_ *)data)->cb; in_computer = true; return 1;
    case 15: { struct var_ *v = data;
        for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].key, v->key)) { v->value = vars[i].val; return 1; }
        v->value = NULL; return 0; }
    case 16: store_vars(data); return 1;
    case 17: *(bool *)data = vars_changed; vars_changed = false; return 1;
    case 18: no_game_ok = true; return 1;                            /* SET_SUPPORT_NO_GAME */
    case 21: ftcb = *(struct ftcb_ *)data; return 1;
    case 24: *(uint64_t *)data = (1u << RDEV_JOYPAD) | (1u << RDEV_KEYBOARD) | (1u << RDEV_ANALOG); return 1;
    case 27: ((void (**)(int, const char *, ...))data)[0] = logfn; return 1;
    case 31: *(const char **)data = savedir; return 1;
    case 32: { struct avinfo *a = data; av = *a; return 1; }       /* SET_SYSTEM_AV_INFO */
    case 35: return 1;                                               /* SET_CONTROLLER_INFO */
    case 37: av.g = *(struct geom *)data; return 1;                 /* SET_GEOMETRY */
    case 47: if (data) *(int *)data = ff_on ? (want_video ? 1 : 0) : 3; return 1;                  /* GET_AUDIO_VIDEO_ENABLE */
    case 52: *(unsigned *)data = 0; return 1;                       /* legacy options only */
    }
    return 0;
}

static float cur_aspect(unsigned w, unsigned h)
{
    float a = av.g.aspect > 0.01f ? av.g.aspect : (float)av.g.bw / (float)(av.g.bh ? av.g.bh : 1);
    if (av.g.bw && av.g.bh && (w != av.g.bw || h != av.g.bh))
        a = a * ((float)w * av.g.bh) / ((float)h * av.g.bw);
    return a;
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    st_vframes++;
    if (!data || !want_video) return;
    if (grab && w <= 4096 && h <= 4096) {
        free(bgbuf); bgbuf = malloc((size_t)w * h * 2);
        if (bgbuf) { for (unsigned j = 0; j < h; j++) video_row565(bgbuf + (size_t)j * w, (const unsigned char *)data + j * pitch, w, pixfmt);
                     bg_w = w; bg_h = h; bg_aspect = cur_aspect(w, h); }
    }
    vb->frame(data, w, h, pitch, pixfmt, cur_aspect(w, h));
}

static int16_t abuf[2 * 4096];
static void audio_cb(int16_t l, int16_t r) { if (ff_on) return; int16_t s[2] = { l, r }; aud_push(s, 1); }
static size_t audio_batch_cb(const int16_t *d, size_t n) { if (!ff_on) aud_push(d, n); return n; }
static void poll_cb(void) {}

static volatile sig_atomic_t got_sig;
static void on_sig(int s) { got_sig = s; }

#define SYM(var, name) do { *(void **)&var = dlsym(h, name); \
    if (!var) { fprintf(stderr, "retro: missing symbol %s\n", name); return 1; } } while (0)

int main(int argc, char **argv)
{
    const char *vname = getenv("RETRO_VIDEO");
    char cfgv[32] = "";
    FILE *cf = fopen(DATA "/retro.cfg", "r");
    if (cf) { char l[128]; while (fgets(l, sizeof l, cf)) if (sscanf(l, "video=%31s", cfgv) == 1) break; fclose(cf); }
    if (!vname && cfgv[0]) vname = cfgv;
    int ai = 1;
    if (argc > 2 && !strcmp(argv[1], "-v")) { vname = argv[2]; ai = 3; }
    if (argc - ai < 1) {
        fprintf(stderr, "usage: retro [-v sw|overlay] <core.so> [content|-]\n"); return 2;
    }
    if (vname && !strcmp(vname, "overlay")) vb = &video_overlay;
    const char *core = argv[ai], *content = argc - ai > 1 ? argv[ai + 1] : "-";
    if (getenv("RETRO_SYS")) sysdir = getenv("RETRO_SYS");
    if (getenv("RETRO_SAVES")) savedir = getenv("RETRO_SAVES");
    if (getenv("RETRO_LOGLEVEL")) loglevel = atoi(getenv("RETRO_LOGLEVEL"));
    long max_frames = getenv("RETRO_FRAMES") ? atol(getenv("RETRO_FRAMES")) : 0;
    const char *b = strrchr(core, '/'); b = b ? b + 1 : core;
    snprintf(corename, sizeof corename, "%s", b);
    char *cut = strstr(corename, "_libretro"); if (!cut) cut = strstr(corename, ".so"); if (cut) *cut = 0;
    load_overrides(content);

    void *h = dlopen(core, RTLD_NOW);
    if (!h) { fprintf(stderr, "retro: dlopen: %s\n", dlerror()); return 1; }
    void (*set_env)(int (*)(unsigned, void *)), (*set_video)(void (*)(const void *, unsigned, unsigned, size_t));
    void (*set_as)(void (*)(int16_t, int16_t)), (*set_asb)(size_t (*)(const int16_t *, size_t));
    void (*set_poll)(void (*)(void)), (*set_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
    void (*init)(void), (*deinit)(void), (*run)(void), (*get_info)(struct sysinfo_ *);
    void (*get_av)(struct avinfo *), (*unload)(void), (*set_cdev)(unsigned, unsigned);
    void *(*mem_data)(unsigned); size_t (*mem_size)(unsigned);
    bool (*load)(const struct gameinfo *);
    size_t (*ser_size)(void); bool (*ser)(void *, size_t), (*unser)(const void *, size_t);
    SYM(set_env, "retro_set_environment"); SYM(set_video, "retro_set_video_refresh");
    SYM(set_as, "retro_set_audio_sample"); SYM(set_asb, "retro_set_audio_sample_batch");
    SYM(set_poll, "retro_set_input_poll"); SYM(set_state, "retro_set_input_state");
    SYM(init, "retro_init"); SYM(deinit, "retro_deinit"); SYM(run, "retro_run");
    SYM(get_info, "retro_get_system_info"); SYM(get_av, "retro_get_system_av_info");
    SYM(load, "retro_load_game"); SYM(unload, "retro_unload_game");
    SYM(mem_data, "retro_get_memory_data"); SYM(mem_size, "retro_get_memory_size");
    SYM(set_cdev, "retro_set_controller_port_device");
    SYM(ser_size, "retro_serialize_size"); SYM(ser, "retro_serialize"); SYM(unser, "retro_unserialize");

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGHUP, on_sig);
    if (vb->init()) { fprintf(stderr, "retro: video backend %s failed\n", vb->name); vb->close(); return 1; }
    vb->fill(0x0841);                                   /* dark loading screen */
    fprintf(stderr, "retro: video backend %s, loading %s\n", vb->name, content);

    set_env(env_cb); set_video(video_cb); set_as(audio_cb); set_asb(audio_batch_cb);
    set_poll(poll_cb); set_state(in_state);
    init();
    struct sysinfo_ si; memset(&si, 0, sizeof si); get_info(&si);
    fprintf(stderr, "retro: core %s %s\n", si.name, si.version);

    struct gameinfo gi, *gp = NULL; void *buf = NULL;
    if (!strcmp(content, "-") && !no_game_ok) {
        fprintf(stderr, "retro: core %s does not support running without content\n", corename);
        vb->close(); return 1;
    }
    if (strcmp(content, "-")) {
        memset(&gi, 0, sizeof gi); gi.path = content;
        FILE *f = fopen(content, "rb"); if (!f) { perror(content); vb->close(); return 1; }
        fseek(f, 0, SEEK_END); gi.size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        if (!si.need_fullpath) { buf = malloc(gi.size); if (!buf || fread(buf, 1, gi.size, f) != gi.size) return 1; gi.data = buf; }
        fclose(f); gp = &gi;
    }
    if (!load(gp)) { fprintf(stderr, "retro: retro_load_game failed\n"); vb->close(); return 1; }
    get_av(&av);
    set_cdev(0, RDEV_JOYPAD); set_cdev(1, RDEV_JOYPAD);
    fprintf(stderr, "retro: %ux%u aspect %.3f, %.3f fps, %.0f Hz, pixfmt %d\n",
            av.g.bw, av.g.bh, av.g.aspect, av.fps, av.rate, pixfmt);

    char srm[512] = ""; size_t srsz = mem_size(0); unsigned char *srp = mem_data(0);
    if (gp && srsz && srp) {
        const char *cb = strrchr(content, '/'); cb = cb ? cb + 1 : content;
        snprintf(srm, sizeof srm, "%s/%s.srm", savedir, cb);
        FILE *f = fopen(srm, "rb");
        if (f) { size_t n = fread(srp, 1, srsz, f); fclose(f); fprintf(stderr, "retro: loaded %zu bytes %s\n", n, srm); }
    }

    in_open();
    double fps = av.fps > 1 ? av.fps : 60.0, period = 1.0 / fps;
    aud_open(av.rate > 0 ? av.rate : 44100, fps);

    /* Main loop: run every core frame that is due (at most 3 per pass), the
     * last with video enabled, present it, keep audio serviced. */
    double next = now_s(), st_t = next, st_core = 0, st_cmax = 0, ff_pres = 0;
    unsigned st_runs = 0, st_pres = 0, st_late = 0;
    long total = 0;
    double c0, c1, cp0 = 0, pp0 = 0;
    while (!got_sig && !quit_core) {
        if (in_menu_req()) {
            /* Pause menu: let go of everything the core holds, run one more frame to have a picture
             * of the moment (the opening key never reaches the core), then stop the core and the audio. */
            in_release(); aud_service();
            if (ftcb.cb) ftcb.cb(ftcb.ref ? ftcb.ref : (int64_t)(1e6 / fps));
            grab = true; want_video = true; run(); grab = false;
            aud_pause();
            struct menu_core mc = { ser_size, ser, unser, disk_n, disk_cur, disk_set };
            const char *sn = getenv("RETRO_SYSID");
            if (bgbuf) {
                fprintf(stderr, "retro: menu open\n");
                if (menu_run(vb, &mc, bgbuf, bg_w, bg_h, bg_aspect, sn && *sn ? sn : corename, content,
                             (const volatile int *)&got_sig)) quit_core = true;
                fprintf(stderr, "retro: menu closed%s\n", quit_core ? " (quit)" : "");
            } else in_menu_mode(false);
            in_release(); aud_pause();
            next = now_s(); st_t = next;               /* no catch-up burst after the pause */
            continue;
        }
        double t = now_s();
        if (t < next && !ff_on) {
            aud_service();
            double d = next - t;
            if (d > 0.0005) usleep((useconds_t)(d * 1e6 > 4000 ? 4000 : d * 1e6));
            continue;
        }
        int due = ff_on ? 1 : (int)((t - next) / period) + 1;
        if (!ff_on && due > 3) { st_late += (unsigned)(due - 3); next = t - 2 * period; due = 3; }
        for (int k = 0; k < due; k++) {
            in_poll();
            want_video = ff_on ? t - ff_pres >= 0.1 : k == due - 1;
            if (ftcb.cb) ftcb.cb(ftcb.ref ? ftcb.ref : (int64_t)(1e6 / fps));
            c0 = now_s(); run(); c1 = now_s();
            double cv = vstat_conv - cp0; cp0 = vstat_conv;  /* conversion runs inside run() */
            double cm = (c1 - c0) - cv;
            st_core += cm; if (cm > st_cmax) st_cmax = cm;
            st_runs++; next += period; total++;
            aud_service();
        }
        if (!ff_on || want_video) { double p0 = vstat_present; vb->present(); if (vstat_present != p0) st_pres++; if (ff_on) ff_pres = t; }
        if (ff_on) next = now_s();                  /* no catch-up burst when the core leaves warp */
        (void)pp0;
        aud_service();
        if (max_frames && total >= max_frames) break;
        if (t - st_t >= 1.0) {
            unsigned ru, ds; double lms, rt; aud_stats(&ru, &ds, &lms, &rt);
            static double conv0, pres0, wait0;
            fprintf(stderr, "stats[%s] fps %u shown %u late %u | core %.2f ms (max %.1f) conv %.2f ms present %.2f ms (+vsync wait %.2f)"
                    " | audio underruns %u starved %u ring %.0f ms ratio %.4f\n", vb->name, st_runs, st_pres, st_late,
                    st_runs ? 1000 * st_core / st_runs : 0, 1000 * st_cmax,
                    st_runs ? 1000 * (vstat_conv - conv0) / (st_pres ? st_pres : 1) : 0,
                    st_pres ? 1000 * (vstat_present - pres0) / st_pres : 0,
                    st_pres ? 1000 * (vstat_wait - wait0) / st_pres : 0, ru, ds, lms, rt);
            conv0 = vstat_conv; pres0 = vstat_present; wait0 = vstat_wait;
            st_t = t; st_core = st_cmax = 0; st_runs = st_pres = st_late = 0;
        }
    }
    fprintf(stderr, "retro: exit after %ld frames%s\n", total, got_sig ? " (signal)" : "");
    if (srm[0] && srsz && srp) {
        FILE *f = fopen(srm, "wb");
        if (f) { fwrite(srp, 1, srsz, f); fclose(f); fprintf(stderr, "retro: saved %s\n", srm); }
    }
    aud_close(); in_close();
    unload(); deinit();
    vb->close();
    free(buf); (void)abuf;
    return 0;
}
