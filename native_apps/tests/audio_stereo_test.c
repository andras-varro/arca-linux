/*
 * audio_stereo_test.c — the stereo-capable mix bus (shape B), on the host.
 *
 * A voice may be MONO or STEREO.  `audio_mix_render_stereo()` writes interleaved
 * L,R: a mono voice adds the same value to both sides, a stereo voice adds each
 * of its channels to its own side.  `audio_mix_render()` (mono) is unchanged for
 * mono voices and is what ScummVM and every mono-only app keep using.
 *
 * GROUP A is the golden: a fixed mono scenario (tones, delayed tones, a mono
 * sample voice, HARD and SOFT limiters, chunked renders) rendered by the
 * production `audio_mix_render` AND by `legacy_render`, a verbatim copy of the
 * pre-change function, and the two must hash identically.  No captured constant
 * can rot; the control is the old code itself.  It must hold at every stage:
 * it is the "mono apps stay byte-identical" claim made checkable.
 *
 * Every other group uses ASYMMETRIC signals (L != R everywhere).  A symmetric
 * test passes a render that swaps or drops a channel.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -I common -o build/audio_stereo_test \
 *       tests/audio_stereo_test.c common/audio_gen.c -lm && ./build/audio_stereo_test
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "audio_gen.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ── a deterministic source, a pure function of the absolute frame index ── */
typedef struct { int channels; long pos; long total; int lmul, rmul, amp; } Src;

static int16_t src_l(const Src *s, long i) { return (int16_t)(((i * s->lmul) % 2001 - 1000) * s->amp / 1000); }
static int16_t src_r(const Src *s, long i) { return (int16_t)(((i * s->rmul) % 3001 - 1500) * s->amp / 1500); }

static long src_fill(void *ctx, int16_t *dst, long frames)
{
    Src *s = (Src *)ctx;
    long n = s->total - s->pos;
    if (n > frames) n = frames;
    if (n < 0) n = 0;
    for (long i = 0; i < n; i++) {
        if (s->channels == 2) {
            dst[2 * i]     = src_l(s, s->pos + i);
            dst[2 * i + 1] = src_r(s, s->pos + i);
        } else {
            dst[i] = src_l(s, s->pos + i);
        }
    }
    s->pos += n;
    return n;
}

/* ── group A: the mono golden ───────────────────────────────────────────
 * `legacy_render` is the PRE-CHANGE audio_mix_render, transcribed verbatim from
 * HEAD before the stereo work (mono voices only), so the golden needs no captured
 * constant: it is the old code, run beside the new on the same input. */
static long legacy_render(AudioMixer *m, int16_t *mono, long frames)
{
    if (audio_mix_pending(m) <= 0) return 0;
    for (long i = 0; i < frames; i++) {
        int32_t acc = 0;
        for (int vi = 0; vi < AUDIO_MAX_VOICES; vi++) {
            AudioVoice *vo = &m->v[vi];
            if (!vo->active) continue;
            if (vo->delay > 0) { vo->delay--; continue; }
            if (vo->peak != vo->peak_goal) {
                int d = vo->peak_goal - vo->peak;
                if (d > 0) vo->peak += (d > vo->peak_step) ? vo->peak_step : d;
                else       vo->peak -= (-d > vo->peak_step) ? vo->peak_step : -d;
            }
            double env = audio_tone_env(vo->pos, vo->frames, vo->attack, vo->release);
            if (vo->kind == AUDIO_VOICE_SAMPLE) {
                if (vo->buf_pos >= vo->buf_len && !vo->drained) {
                    long got = vo->fill(vo->ctx, vo->buf, vo->buf_cap);
                    vo->buf_len = (got > 0) ? got : 0;
                    vo->buf_pos = 0;
                    if (vo->buf_len < vo->buf_cap) vo->drained = true;
                }
                if (vo->buf_pos >= vo->buf_len) { vo->active = false; continue; }
                int16_t s = vo->buf[vo->buf_pos++];
                acc += (int32_t)((double)audio_voice_scale(s, vo->peak) * env);
                if (++vo->pos >= vo->frames) vo->active = false;
                continue;
            }
            acc += (int32_t)((double)vo->peak * env * sin(vo->phase));
            vo->phase += vo->phase_step;
            if (vo->phase > 2.0 * M_PI) vo->phase -= 2.0 * M_PI;
            if (++vo->pos >= vo->frames) vo->active = false;
        }
        int32_t out = audio_mix_limit_at(acc, m->limit, m->knee);
        if (out != acc) {
            if (m->limit == AUDIO_MIX_HARD) m->clipped++;
            else                            m->limited++;
        }
        if      (out >  32767) { out =  32767; m->clipped++; }
        else if (out < -32768) { out = -32768; m->clipped++; }
        mono[i] = (int16_t)out;
    }
    return frames;
}

static uint32_t mono_scenario(int limit, int hot, int legacy, uint32_t *clipped, uint32_t *limited)
{
    static int16_t buf[256];
    Src src = { 1, 0, 20000, 37, 91, 30000 };
    AudioMixer m;
    audio_mix_init(&m, 44100);
    audio_mix_set_limit(&m, limit);
    int pk = hot ? 30000 : 6144;
    audio_mix_add(&m, 440, 800, 0, pk);
    audio_mix_add(&m, 880, 300, 100, pk);
    audio_mix_add(&m, 660, 500, 250, pk);
    audio_mix_add_sample(&m, src_fill, &src, buf, 256, src.total, pk);

    static const long chunks[] = { 333, 1024, 77, 2048, 5, 4096, 999, 1500 };
    uint32_t h = 2166136261u;
    long done = 0;
    for (int c = 0; done < 40000; c++) {
        static int16_t out[4096];
        long want = chunks[c % 8];
        long got = legacy ? legacy_render(&m, out, want) : audio_mix_render(&m, out, want);
        if (got > 0) h = fnv(h, out, (size_t)got * 2);
        done += want;
    }
    h = fnv(h, &m.clipped, sizeof m.clipped);
    h = fnv(h, &m.limited, sizeof m.limited);
    *clipped = m.clipped;
    *limited = m.limited;
    return h;
}

static void group_a(void)
{
    printf("A: mono golden (production vs the pre-change render)\n");
    static const struct { int limit, hot; const char *name; } sc[] = {
        { AUDIO_MIX_HARD, 0, "HARD" }, { AUDIO_MIX_SOFT, 0, "SOFT" },
        { AUDIO_MIX_HARD, 1, "HARD hot" }, { AUDIO_MIX_SOFT, 1, "SOFT hot" },
    };
    for (int k = 0; k < 4; k++) {
        uint32_t c0, l0, c1, l1;
        uint32_t hl = mono_scenario(sc[k].limit, sc[k].hot, 1, &c0, &l0);
        uint32_t hn = mono_scenario(sc[k].limit, sc[k].hot, 0, &c1, &l1);
        printf("  %-9s legacy %08x new %08x clip=%u lim=%u\n", sc[k].name, hl, hn, c1, l1);
        CHECK(hl == hn, "%s: production mono render differs from the pre-change render", sc[k].name);
        CHECK(c0 == c1 && l0 == l1, "%s: counters differ", sc[k].name);
        if (sc[k].hot) CHECK(c1 + l1 > 0, "%s: hot scenario never reached the limiter", sc[k].name);
    }
}

#ifndef NO_STEREO_API

/* Render n frames of one voice stereo into out (2n int16). */
static AudioMixer one_voice(Src *src, int16_t *buf, int channels, int peak)
{
    AudioMixer m;
    audio_mix_init(&m, 44100);
    int slot = audio_mix_add_sample_ch(&m, src_fill, src, buf, 256, src->total, peak, channels);
    CHECK(slot >= 0, "add_sample_ch(%d) slot %d", channels, slot);
    return m;
}

/* B: a stereo voice's channels are two independent mono renders. */
static void group_b(void)
{
    printf("B: stereo voice == two mono renders\n");
    enum { N = 6000 };
    static int16_t bs[512], bl[256];
    static int16_t st[2 * N], ml[N];
    Src s = { 2, 0, N, 37, 91, 20000 };
    AudioMixer m = one_voice(&s, bs, 2, 9000);
    long got = audio_mix_render_stereo(&m, st, N);
    CHECK(got == N, "rendered %ld", got);

    /* Oracle: the same L (resp. R) samples as a MONO voice through the unchanged mono render. */
    Src sl = { 1, 0, N, 37, 91, 20000 };
    AudioMixer a = one_voice(&sl, bl, 1, 9000);
    audio_mix_render(&a, ml, N);
    long diff_l = 0, differ = 0, nonzero_r = 0;
    for (long i = 0; i < N; i++) {
        if (st[2 * i] != ml[i]) diff_l++;
        if (st[2 * i] != st[2 * i + 1]) differ++;
        if (st[2 * i + 1] != 0) nonzero_r++;
    }
    CHECK(diff_l == 0, "L channel differs from the mono render of L in %ld frames", diff_l);
    CHECK(differ > N / 2, "L != R in only %ld of %d frames", differ, N);
    CHECK(nonzero_r > N / 2, "R channel is silent/dropped (%ld nonzero)", nonzero_r);

    /* R oracle: analytic, with the shaping the voice applies, mid-voice where env == 1. */
    long bad = 0;
    for (long i = 600; i < N - 1000; i++) {
        long want = audio_voice_scale(src_r(&s, i), 9000);
        if (st[2 * i + 1] != want) bad++;
    }
    CHECK(bad == 0, "R channel differs from its analytic value in %ld mid-voice frames", bad);
}

/* C: mono voices in the stereo render are L==R and equal interleave(mono render). */
static void group_c(void)
{
    printf("C: mono voices through the stereo render\n");
    enum { N = 30000 };
    static int16_t buf1[256], buf2[256];
    static int16_t mono[N], il[2 * N], st[2 * N];
    Src s1 = { 1, 0, 20000, 37, 91, 30000 }, s2 = s1;
    AudioMixer a, b;
    audio_mix_init(&a, 44100); audio_mix_init(&b, 44100);
    audio_mix_add(&a, 440, 600, 0, 30000);  audio_mix_add(&b, 440, 600, 0, 30000);
    audio_mix_add(&a, 880, 300, 50, 30000); audio_mix_add(&b, 880, 300, 50, 30000);
    audio_mix_add_sample(&a, src_fill, &s1, buf1, 256, s1.total, 30000);
    audio_mix_add_sample(&b, src_fill, &s2, buf2, 256, s2.total, 30000);
    audio_mix_render(&a, mono, N);
    audio_interleave(mono, N, 2, il);
    audio_mix_render_stereo(&b, st, N);
    CHECK(memcmp(il, st, sizeof il) == 0, "stereo render of mono voices != interleave(mono render)");
    CHECK(a.clipped == b.clipped && a.limited == b.limited,
          "counters mono %u/%u stereo %u/%u", a.clipped, a.limited, b.clipped, b.limited);
    CHECK(b.clipped > 0, "scenario never clipped; counter check proves nothing (%u)", b.clipped);
}

/* D: incremental property, with a stereo voice and a mono tone together. */
static void group_d(void)
{
    printf("D: N+M frames == one N+M render\n");
    enum { N = 9000 };
    static int16_t b1[512], b2[512], one[2 * N], two[2 * N];
    Src s1 = { 2, 0, N, 37, 91, 25000 }, s2 = s1;
    AudioMixer a, b;
    audio_mix_init(&a, 44100); audio_mix_init(&b, 44100);
    audio_mix_add_sample_ch(&a, src_fill, &s1, b1, 256, N, 20000, 2);
    audio_mix_add_sample_ch(&b, src_fill, &s2, b2, 256, N, 20000, 2);
    audio_mix_add(&a, 523, 150, 20, 8000); audio_mix_add(&b, 523, 150, 20, 8000);
    audio_mix_render_stereo(&a, one, N);
    long done = 0; const long ch[] = { 1, 700, 255, 1000, 3, 4096 };
    for (int c = 0; done < N; c++) {
        long w = ch[c % 6]; if (w > N - done) w = N - done;
        audio_mix_render_stereo(&b, two + 2 * done, w);
        done += w;
    }
    CHECK(memcmp(one, two, sizeof one) == 0, "chunked stereo render differs from one-shot");
}

/* E: the limiter is per channel; clipped counts a FRAME once, not once per channel. */
static void group_e(void)
{
    printf("E: per-channel limiter\n");
    static int16_t buf[512];
    static int16_t out[2 * 4000];
    Src s = { 2, 0, 4000, 0, 0, 0 };         /* lmul=rmul=0: constant L=-1000*amp/1000, R=-1500*amp/1500 */
    s.amp = 32000;                           /* L = -32000, R = -32000 (both hot) */
    AudioMixer m = one_voice(&s, buf, 2, 32768 * 2);   /* boost: both channels clip */
    audio_mix_render_stereo(&m, out, 4000);
    long fr_clip = 0;
    for (long i = 1000; i < 3000; i++) if (out[2 * i] == -32768 && out[2 * i + 1] == -32768) fr_clip++;
    CHECK(fr_clip == 2000, "both channels should hard-clip mid-voice (%ld)", fr_clip);
    CHECK(m.clipped >= 2000 && m.clipped <= 4000, "clipped %u: expected once per frame", m.clipped);

    /* L pinned hot, R sweeping: where R is small it must pass the limiter intact
     * while L is clamped -- the limiter is per channel, not on a shared peak. */
    Src q = { 2, 0, 4000, 0, 1, 30000 };
    static int16_t buf2[512], out2[2 * 4000];
    AudioMixer m2 = one_voice(&q, buf2, 2, 32768 * 2);
    audio_mix_render_stereo(&m2, out2, 4000);
    long lclamped = 0, rbad = 0, rchecked = 0;
    for (long i = 1000; i < 3000; i++) {
        int rs = src_r(&q, i);
        if (rs > -10000 && rs < 10000) {
            rchecked++;
            if (out2[2 * i + 1] != (int16_t)audio_voice_scale((int16_t)rs, 32768 * 2)) rbad++;
            if (out2[2 * i] == -32768) lclamped++;
        }
    }
    CHECK(rchecked > 300, "scenario never put R in range (%ld)", rchecked);
    CHECK(rbad == 0, "quiet R altered by the limiter in %ld frames", rbad);
    CHECK(lclamped == rchecked, "hot L not clamped in %ld frames", rchecked - lclamped);
}

/* F: the mono render folds a stereo voice to (L+R)>>1 and never errs on a short count. */
static void group_f(void)
{
    printf("F: mono render of a stereo voice\n");
    enum { N = 5000 };
    static int16_t b1[512], b2[512], mono[N], st[2 * N];
    Src s1 = { 2, 0, N, 37, 91, 20000 }, s2 = s1;
    AudioMixer a, b;
    audio_mix_init(&a, 44100); audio_mix_init(&b, 44100);
    audio_mix_add_sample_ch(&a, src_fill, &s1, b1, 256, N, 9000, 2);
    audio_mix_add_sample_ch(&b, src_fill, &s2, b2, 256, N, 9000, 2);
    audio_mix_render(&a, mono, N);
    audio_mix_render_stereo(&b, st, N);
    long bad = 0;
    for (long i = 0; i < N; i++) if (mono[i] != (int16_t)(((int)st[2 * i] + st[2 * i + 1]) >> 1)) bad++;
    CHECK(bad == 0, "mono downmix differs from (L+R)>>1 in %ld frames", bad);
}

/* G: argument checks and a short fill. */
static void group_g(void)
{
    printf("G: arguments and short fills\n");
    static int16_t buf[512];
    Src s = { 2, 0, 1000, 37, 91, 9000 };
    AudioMixer m;
    audio_mix_init(&m, 44100);
    CHECK(audio_mix_add_sample_ch(&m, src_fill, &s, buf, 256, 1000, 8000, 3) < 0, "3 channels accepted");
    CHECK(audio_mix_add_sample_ch(&m, src_fill, &s, buf, 256, 1000, 8000, 0) < 0, "0 channels accepted");
    CHECK(m.dropped == 0, "caller bugs must not count as drops");
    int slot = audio_mix_add_sample_ch(&m, src_fill, &s, buf, 256, 2000, 8000, 2);
    CHECK(slot >= 0, "stereo add");
    static int16_t out[2 * 3000];
    long got = audio_mix_render_stereo(&m, out, 3000);   /* source dries up at 1000 of the claimed 2000 */
    CHECK(got == 3000, "render returned %ld", got);
    CHECK(audio_mix_active(&m) == 0, "voice should end when the source ran dry");
    CHECK(audio_mix_render_stereo(&m, out, 10) == 0, "silent bus must return 0");
}

#endif /* NO_STEREO_API */

int main(void)
{
    group_a();
#ifndef NO_STEREO_API
    group_b(); group_c(); group_d(); group_e(); group_f(); group_g();
#endif
    printf("audio_stereo_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
