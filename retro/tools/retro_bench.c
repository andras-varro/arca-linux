/* retro_bench: headless libretro frontend for measuring a core.
 *
 *   retro_bench <core.so> [content-file|-] [frames=600] [dump.ppm]
 *
 * Build (soft-float, Cortex-A8; no -march/-mcpu that implies idiv):
 *   arm-linux-gnueabi-gcc -march=armv7-a -mtune=cortex-a8 -mfpu=neon \
 *     -mfloat-abi=softfp -O2 retro/tools/retro_bench.c -o retro_bench -ldl
 *
 * Env: RETRO_SYS = system/save directory (default "."). Runs N frames with no
 * input, prints timing and audio/video statistics, optionally dumps the last
 * frame as a P6 PPM. Not a real frontend: no video output, audio or input. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define ENV_GET_CAN_DUPE 3
#define ENV_SET_VARIABLES 16
#define ENV_GET_CORE_OPTIONS_VERSION 52
#define ENV_GET_SYSTEM_DIRECTORY 9
#define ENV_SET_PIXEL_FORMAT 10
#define ENV_GET_VARIABLE 15
#define ENV_SET_SUPPORT_NO_GAME 18
#define ENV_GET_LOG_INTERFACE 27
#define ENV_GET_SAVE_DIRECTORY 31
#define ENV_EXPERIMENTAL 0x10000
#define PF_0RGB1555 0
#define PF_XRGB8888 1
#define PF_RGB565 2

static int pixfmt = PF_0RGB1555;           /* libretro default */
static const char *sysdir = ".";
static int no_game_ok;
static unsigned char *last; static size_t last_cap;
static unsigned lw, lh; static size_t lpitch;
static unsigned long frames_video, dupes, audio_frames, audio_calls;
static unsigned seen_cmd[64]; static int nseen;

/* Core options: we announce option version 0 so cores use the legacy
 * SET_VARIABLES ("Desc; a|b|c", first value = default) and answer GET_VARIABLE
 * with those defaults. Without this, cores that read their sample rate or
 * similar from an option get 0 (VICE then divides by zero). */
struct var { const char *key; char val[48]; };
static struct var vars[512]; static int nvars;

static void store_vars(const struct retro_variable *v)
{
    for (; v->key && nvars < 512; v++) {
        const char *s = strstr(v->value, "; ");
        struct var *d = &vars[nvars++];
        d->key = v->key; d->val[0] = 0;
        if (!s) continue;
        s += 2;
        size_t n = strcspn(s, "|");
        if (n >= sizeof d->val) n = sizeof d->val - 1;
        memcpy(d->val, s, n); d->val[n] = 0;
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

    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < nframes; i++) run();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("frames run: %d  wall: %.3f s  speed: %.2f frames/s (%.2fx of core fps)\n", nframes, s,
           nframes / s, nframes / s / (av.timing.fps > 0 ? av.timing.fps : 1));
    printf("video callbacks: %lu  dupes(NULL data): %lu  last frame %ux%u pitch %lu\n", frames_video, dupes,
           lw, lh, (unsigned long)lpitch);
    printf("audio frames: %lu in %lu calls = %.2f per video frame (expected %.2f)\n", audio_frames, audio_calls,
           (double)audio_frames / nframes, av.timing.fps > 0 ? av.timing.sample_rate / av.timing.fps : 0.0);
    if (dump && dump_ppm(dump) == 0) printf("dumped %s\n", dump);
    deinit(); free(buf);
    return 0;
}
