/* retro_bench: headless libretro frontend for measuring a core.
 *
 *   retro_bench <core.so> [content-file|-] [frames=600] [dump.ppm]
 *
 * Build (soft-float, Cortex-A8; no -march/-mcpu that implies idiv):
 *   arm-linux-gnueabi-gcc -march=armv7-a -mtune=cortex-a8 -mfpu=neon \
 *     -mfloat-abi=softfp -O2 retro/tools/retro_bench.c -o retro_bench -ldl
 *
 * Env: RETRO_SYS = system/save directory (default "."). RETRO_STATE=<file>: a
 * retro_serialize image (a frontend ".sN" state file) loaded right after the
 * warm-up frames, before the timed ones. Runs N frames with no
 * input, prints timing and audio/video statistics, optionally dumps the last
 * frame as a P6 PPM. Not a real frontend: no video output, audio or input.
 * RETRO_PROF=<file>: sample the PC of whichever thread is on the CPU (SIGPROF,
 * process CPU time) during the timed frames; the file holds /proc/self/maps
 * then one "tid pc" line per sample. Resolve on the host with
 * retro/tools/prof_resolve.py against an unstripped copy of the core. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROF_MAX (1 << 18)
static uint32_t prof_pc[PROF_MAX], prof_tid[PROF_MAX];
static volatile unsigned prof_n;

static void prof_sig(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    unsigned i = __sync_fetch_and_add(&prof_n, 1);
    if (i >= PROF_MAX) return;
    prof_pc[i] = ((ucontext_t *)uc)->uc_mcontext.arm_pc;
    prof_tid[i] = (uint32_t)syscall(SYS_gettid);
}

static void prof_start(void)
{
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
}

static void prof_stop(const char *path)
{
    struct itimerval it; memset(&it, 0, sizeof it);
    setitimer(ITIMER_PROF, &it, NULL);
    FILE *o = fopen(path, "w"), *m = fopen("/proc/self/maps", "r");
    if (!o || !m) { perror(path); return; }
    char line[512];
    while (fgets(line, sizeof line, m)) fprintf(o, "map %s", line);
    fclose(m);
    unsigned n = prof_n < PROF_MAX ? prof_n : PROF_MAX;
    for (unsigned i = 0; i < n; i++) fprintf(o, "%u %08x\n", prof_tid[i], prof_pc[i]);
    fclose(o);
    printf("profile: %u samples -> %s\n", n, path);
}

/* --- the subset of libretro.h we need (the ABI is stable) --- */
struct retro_system_info { const char *library_name, *library_version, *valid_extensions;
    int need_fullpath, block_extract; };
struct retro_game_geometry { unsigned base_width, base_height, max_width, max_height;
    float aspect_ratio; };
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_log_callback { void (*log)(int level, const char *fmt, ...); };
struct retro_variable { const char *key, *value; };
struct retro_frame_time_callback { void (*callback)(int64_t usec); int64_t reference; };

#define ENV_GET_CAN_DUPE 3
#define ENV_SET_VARIABLES 16
#define ENV_GET_CORE_OPTIONS_VERSION 52
#define ENV_GET_SYSTEM_DIRECTORY 9
#define ENV_SET_PIXEL_FORMAT 10
#define ENV_GET_VARIABLE 15
#define ENV_SET_SUPPORT_NO_GAME 18
#define ENV_GET_LOG_INTERFACE 27
#define ENV_GET_SAVE_DIRECTORY 31
#define ENV_SET_FRAME_TIME_CALLBACK 21
#define ENV_EXPERIMENTAL 0x10000
#define PF_0RGB1555 0
#define PF_XRGB8888 1
#define PF_RGB565 2

static int pixfmt = PF_0RGB1555;           /* libretro default */
static const char *sysdir = ".";
static int no_game_ok;
static struct retro_frame_time_callback ftcb;
static unsigned char *last; static size_t last_cap;
static unsigned lw, lh; static size_t lpitch;
static unsigned long frames_video, dupes, audio_frames, audio_calls;
static unsigned seen_cmd[64]; static int nseen;

/* Core options: we announce option version 0 so cores use the legacy
 * SET_VARIABLES ("Desc; a|b|c", first value = default) and answer GET_VARIABLE
 * with those defaults. Without this, cores that read their sample rate or
 * similar from an option get 0 (VICE then divides by zero).
 * RETRO_OPTS="key=value,key=value" overrides a default (the value is not
 * checked against the choices); RETRO_LIST_OPTS=1 prints every option with
 * its choices, which is where the keys and values come from. */
struct var { const char *key; char val[48]; };
static struct var vars[512]; static int nvars;

static void override_var(struct var *d)
{
    const char *o = getenv("RETRO_OPTS");
    size_t kl = strlen(d->key);
    while (o && *o) {
        size_t n = strcspn(o, ",");
        if (n > kl && o[kl] == '=' && !strncmp(o, d->key, kl)) {
            size_t vl = n - kl - 1;
            if (vl >= sizeof d->val) vl = sizeof d->val - 1;
            memcpy(d->val, o + kl + 1, vl); d->val[vl] = 0;
            printf("[opt] %s=%s (override)\n", d->key, d->val);
        }
        o += n; if (*o) o++;
    }
}

static void store_vars(const struct retro_variable *v)
{
    int list = getenv("RETRO_LIST_OPTS") != NULL;
    for (; v->key && nvars < 512; v++) {
        const char *s = strstr(v->value, "; ");
        struct var *d = &vars[nvars++];
        d->key = v->key; d->val[0] = 0;
        if (list) printf("[opts] %s: %s\n", v->key, v->value);
        if (!s) continue;
        s += 2;
        size_t n = strcspn(s, "|");
        if (n >= sizeof d->val) n = sizeof d->val - 1;
        memcpy(d->val, s, n); d->val[n] = 0;
        override_var(d);
    }
}

static int get_var(struct retro_variable *v)
{
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].key, v->key)) { v->value = vars[i].val; return 1; }
    v->value = NULL; return 0;
}

static void logfn(int level, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    static int minlvl = -1;
    if (minlvl < 0) minlvl = getenv("RETRO_LOGLEVEL") ? atoi(getenv("RETRO_LOGLEVEL")) : 0;
    if (level < minlvl) { va_end(ap); return; }
    printf("[core log %d] ", level); vprintf(fmt, ap); va_end(ap);
}

static int env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case ENV_GET_CAN_DUPE: *(unsigned char *)data = 1; return 1;
    case ENV_GET_SYSTEM_DIRECTORY:
    case ENV_GET_SAVE_DIRECTORY: *(const char **)data = sysdir; return 1;
    case ENV_SET_PIXEL_FORMAT: {
        int f = *(int *)data;
        if (f != PF_RGB565 && f != PF_XRGB8888 && f != PF_0RGB1555) return 0;
        pixfmt = f; return 1; }
    case ENV_GET_VARIABLE: return get_var(data);
    case ENV_SET_VARIABLES: store_vars(data); return 1;
    case ENV_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return 1;
    case ENV_SET_SUPPORT_NO_GAME: no_game_ok = *(unsigned char *)data; return 1;
    case ENV_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logfn; return 1;
    case ENV_SET_FRAME_TIME_CALLBACK: ftcb = *(struct retro_frame_time_callback *)data; return 1;
    }
    for (int i = 0; i < nseen; i++) if (seen_cmd[i] == cmd) return 0;
    if (nseen < 64) seen_cmd[nseen++] = cmd;
    printf("[env] unhandled cmd %u%s (answered false)\n", cmd & 0xffff,
           (cmd & ENV_EXPERIMENTAL) ? " (EXPERIMENTAL flag)" : "");
    return 0;
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    frames_video++;
    if (!data) { dupes++; return; }
    if (pitch * h > last_cap) { last_cap = pitch * h; last = realloc(last, last_cap); }
    memcpy(last, data, pitch * h);
    lw = w; lh = h; lpitch = pitch;
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; audio_frames++; audio_calls++; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; audio_frames += n; audio_calls++; return n; }
static void poll_cb(void) {}
static int16_t state_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

static int dump_ppm(const char *path)
{
    if (!last) { fprintf(stderr, "no frame to dump\n"); return -1; }
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%u %u\n255\n", lw, lh);
    for (unsigned y = 0; y < lh; y++) {
        const unsigned char *row = last + y * lpitch;
        for (unsigned x = 0; x < lw; x++) {
            unsigned r, g, b;
            if (pixfmt == PF_XRGB8888) {
                uint32_t p = ((const uint32_t *)row)[x];
                r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255;
            } else {
                uint16_t p = ((const uint16_t *)row)[x];
                if (pixfmt == PF_RGB565) { r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3; }
                else { r = ((p >> 10) & 31) << 3; g = ((p >> 5) & 31) << 3; b = (p & 31) << 3; }
            }
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f); return 0;
}

#define SYM(var, name) do { *(void **)&var = dlsym(h, name); \
    if (!var) { fprintf(stderr, "missing symbol %s\n", name); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s core.so [content|-] [frames=600] [dump.ppm]\n", argv[0]); return 2; }
    const char *content = argc > 2 ? argv[2] : "-";
    int nframes = argc > 3 ? atoi(argv[3]) : 600;
    const char *dump = argc > 4 ? argv[4] : NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);   /* keep log order if the core crashes */
    if (getenv("RETRO_SYS")) sysdir = getenv("RETRO_SYS");

    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void (*set_env)(int (*)(unsigned, void *)), (*set_video)(void (*)(const void *, unsigned, unsigned, size_t));
    void (*set_as)(void (*)(int16_t, int16_t)), (*set_asb)(size_t (*)(const int16_t *, size_t));
    void (*set_poll)(void (*)(void)), (*set_state)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
    void (*init)(void), (*deinit)(void), (*run)(void), (*get_info)(struct retro_system_info *);
    void (*get_av)(struct retro_system_av_info *);
    int (*load)(const struct retro_game_info *);
    SYM(set_env, "retro_set_environment"); SYM(set_video, "retro_set_video_refresh");
    SYM(set_as, "retro_set_audio_sample"); SYM(set_asb, "retro_set_audio_sample_batch");
    SYM(set_poll, "retro_set_input_poll"); SYM(set_state, "retro_set_input_state");
    SYM(init, "retro_init"); SYM(deinit, "retro_deinit"); SYM(run, "retro_run");
    SYM(get_info, "retro_get_system_info"); SYM(get_av, "retro_get_system_av_info");
    SYM(load, "retro_load_game");

    set_env(env_cb); set_video(video_cb); set_as(audio_cb); set_asb(audio_batch_cb);
    set_poll(poll_cb); set_state(state_cb);
    init();
    struct retro_system_info si; memset(&si, 0, sizeof si); get_info(&si);
    printf("core: %s %s  (extensions: %s, need_fullpath=%d)\n", si.library_name, si.library_version,
           si.valid_extensions ? si.valid_extensions : "-", si.need_fullpath);

    struct retro_game_info gi; struct retro_game_info *gp = NULL; void *buf = NULL;
    if (strcmp(content, "-") != 0) {
        memset(&gi, 0, sizeof gi); gi.path = content;
        FILE *f = fopen(content, "rb"); if (!f) { perror(content); return 1; }
        fseek(f, 0, SEEK_END); gi.size = ftell(f); fseek(f, 0, SEEK_SET);
        if (!si.need_fullpath) { buf = malloc(gi.size); if (fread(buf, 1, gi.size, f) != gi.size) return 1; gi.data = buf; }
        fclose(f); gp = &gi;
    } else if (!no_game_ok) printf("note: core did not announce SET_SUPPORT_NO_GAME\n");
    if (!load(gp)) { fprintf(stderr, "retro_load_game failed\n"); return 1; }

    struct retro_system_av_info av; memset(&av, 0, sizeof av); get_av(&av);
    static const char *pfn[] = { "0RGB1555", "XRGB8888", "RGB565" };
    printf("geometry: base %ux%u max %ux%u aspect %.3f\n", av.geometry.base_width, av.geometry.base_height,
           av.geometry.max_width, av.geometry.max_height, av.geometry.aspect_ratio);
    printf("timing: %.3f fps, %.0f Hz audio\n", av.timing.fps, av.timing.sample_rate);
    printf("pixel format: %s\n", pfn[pixfmt]);

    /* Cores may defer their init to the first retro_run (VICE: ~20 s on the
     * device), so the first RETRO_WARMUP frames (default 1) are timed apart
     * and the speed is the steady state over the rest. */
    int (*unser_)(const void *, size_t) = dlsym(h, "retro_unserialize");
    int warm = getenv("RETRO_WARMUP") ? atoi(getenv("RETRO_WARMUP")) : 1;
    if (warm < 0 || warm >= nframes) warm = 0;
    struct timespec t0, tw, t1; clock_gettime(CLOCK_MONOTONIC, &t0); tw = t0;
    /* A core that set a frame-time callback (ep128emu) advances by the time it
     * is told, and by nothing if never told: report the nominal frame period. */
    for (int i = 0; i < nframes; i++) {
        if (i == warm) {
            if (getenv("RETRO_STATE")) {
                static char sb[1 << 20];
                FILE *sf = fopen(getenv("RETRO_STATE"), "rb");
                if (!sf || !unser_) { fprintf(stderr, "RETRO_STATE: cannot load\n"); return 1; }
                size_t sn = fread(sb, 1, sizeof sb, sf); fclose(sf);
                printf("unser %zu -> %d\n", sn, unser_(sb, sn));
            }
            clock_gettime(CLOCK_MONOTONIC, &tw);
            if (getenv("RETRO_PROF")) prof_start();
        }
        if (ftcb.callback) ftcb.callback(ftcb.reference);
        run();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (getenv("RETRO_PROF")) prof_stop(getenv("RETRO_PROF"));
    double sw = (tw.tv_sec - t0.tv_sec) + (tw.tv_nsec - t0.tv_nsec) / 1e9;
    double s = (t1.tv_sec - tw.tv_sec) + (t1.tv_nsec - tw.tv_nsec) / 1e9;
    int n = nframes - warm;

    printf("warmup: %d frames in %.3f s\n", warm, sw);
    printf("frames run: %d  wall: %.3f s  speed: %.2f frames/s (%.2fx of core fps)  %.2f ms/frame\n", n, s,
           n / s, n / s / (av.timing.fps > 0 ? av.timing.fps : 1), 1000 * s / n);
    printf("video callbacks: %lu  dupes(NULL data): %lu  last frame %ux%u pitch %lu\n", frames_video, dupes,
           lw, lh, (unsigned long)lpitch);
    printf("audio frames: %lu in %lu calls = %.2f per video frame (expected %.2f)\n", audio_frames, audio_calls,
           (double)audio_frames / nframes, av.timing.fps > 0 ? av.timing.sample_rate / av.timing.fps : 0.0);
    if (dump && dump_ppm(dump) == 0) printf("dumped %s\n", dump);
    deinit(); free(buf);
    return 0;
}
