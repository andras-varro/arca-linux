/*
 * audio_gain_test.c — the live music gain, on the host.
 *
 * Covers the pure half of the gain hook in common/audio_gen.c: the cap and mute
 * floor (audio_gain_total_mdb), the dB -> peak map (audio_gain_peak), the exact
 * sample multiply (audio_voice_scale), the saturation of a boosted voice on the
 * bus, and the per-voice slew (audio_mix_set_voice_peak).
 *
 * ⚠️ This host has sizeof(long) == 8; the target has 4.  The 32-bit overflow the
 * multiply is built to avoid cannot occur here, so audio_voice_scale() is compared
 * with a 64-bit floor reference AND its largest intermediate product is checked
 * to stay under 2^31 (the bound that matters on the target).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_gen.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* floor(a / 32768) for any sign, in 64 bits */
static long long floor_div15(long long a) { return a >= 0 ? a >> 15 : -((-a + 32767) >> 15); }

static void test_total(void)
{
    CHECK(audio_gain_total_mdb(17900, 0) == AUDIO_GAIN_MAX_MDB, "17.9 dB alone caps");
    CHECK(audio_gain_total_mdb(10000, 6000) == AUDIO_GAIN_MAX_MDB, "10+6 caps");
    CHECK(audio_gain_total_mdb(-3600, 0) == -3600, "plain");
    CHECK(audio_gain_total_mdb(-3600, 3000) == -600, "sum");
    CHECK(audio_gain_total_mdb(12000, 0) == 12000, "exactly the cap");
    CHECK(audio_gain_total_mdb(0, -100000) == AUDIO_GAIN_MIN_MDB, "step 0 floor");
    CHECK(audio_gain_total_mdb(40000, -100000) == AUDIO_GAIN_MIN_MDB, "norm up to +40 dB still mutes at step 0");
    CHECK(audio_gain_total_mdb(2000000000, 2000000000) == AUDIO_GAIN_MAX_MDB, "int overflow high");
    CHECK(audio_gain_total_mdb(-2000000000, -2000000000) == AUDIO_GAIN_MIN_MDB, "int overflow low");
    CHECK(audio_gain_peak(32767, audio_gain_total_mdb(0, -100000)) == 0, "mute -> peak 0");
    CHECK(audio_gain_peak(32767, AUDIO_GAIN_MIN_MDB) == 0, "floor itself mutes");
    CHECK(audio_gain_peak(0, 0) == 0, "no base");
}

static void test_peak(void)
{
    int worst = 0;
    double worst_db = 0;
    for (int mdb = AUDIO_GAIN_MIN_MDB + 1000; mdb <= AUDIO_GAIN_MAX_MDB; mdb += 100) {
        int p = audio_gain_peak(32768, mdb);
        double ref = 32768.0 * pow(10.0, mdb / 20000.0);
        if (ref > AUDIO_GAIN_PEAK_MAX) ref = AUDIO_GAIN_PEAK_MAX;
        if (p < 100) continue;                    /* a few LSB of rounding dominate below ~-50 dB */
        double db = fabs(20.0 * log10((double)p / ref));
        if (db > worst_db) { worst_db = db; worst = mdb; }
        CHECK(p <= AUDIO_GAIN_PEAK_MAX, "peak %d over max at %d", p, mdb);
    }
    CHECK(worst_db < 0.1, "worst error %.4f dB at %d mdb", worst_db, worst);
    CHECK(audio_gain_peak(32768, 0) == 32768, "0 dB is unity");
    CHECK(audio_gain_peak(32768, 12000) > 130000 && audio_gain_peak(32768, 12000) <= AUDIO_GAIN_PEAK_MAX, "+12 dB ~4x");
    CHECK(audio_gain_peak(32768, 999999) == audio_gain_peak(32768, 12000), "argument above cap is clamped");
    CHECK(audio_gain_peak(32768, -6000) > 16000 && audio_gain_peak(32768, -6000) < 16600, "-6 dB ~half");
    CHECK(audio_gain_peak(AUDIO_GAIN_PEAK_MAX, 12000) == AUDIO_GAIN_PEAK_MAX, "never above max");
}

static void test_scale(void)
{
    static const int peaks[] = { 0, 1, 2, 100, 16384, 20000, 32766, 32767, 32768, 32769, 40000,
                                 65535, 65536, 65537, 98765, 131071, AUDIO_GAIN_PEAK_MAX };
    long long maxprod = 0;
    for (size_t k = 0; k < sizeof(peaks) / sizeof(peaks[0]); k++) {
        int pk = peaks[k];
        long bad = 0, oldbad = 0;
        for (int s = -32768; s <= 32767; s++) {
            long got = audio_voice_scale((int16_t)s, pk);
            long long want = floor_div15((long long)s * pk);
            if (got != want) bad++;
            if (pk <= 32767) {
                long old = (long)(((long long)s * pk) >> 15);
                if (got != old) oldbad++;
            }
            long long p1 = (long long)s * (pk >> 15), p2 = (long long)s * (pk & 32767);
            if (p1 < 0) p1 = -p1;
            if (p2 < 0) p2 = -p2;
            if (p1 > maxprod) maxprod = p1;
            if (p2 > maxprod) maxprod = p2;
        }
        CHECK(bad == 0, "peak %d: %ld samples differ from floor(s*peak/32768)", pk, bad);
        CHECK(oldbad == 0, "peak %d: %ld samples differ from the old (s*peak)>>15", pk, oldbad);
    }
    CHECK(maxprod < (1LL << 31), "an intermediate product reaches %lld (>= 2^31 wraps on a 32-bit long)", maxprod);
}

static long fill_const(void *ctx, int16_t *dst, long frames)
{
    int16_t v = *(int16_t *)ctx;
    for (long i = 0; i < frames; i++) dst[i] = v;
    return frames;
}

static void test_saturate(void)
{
    static int16_t buf[256];
    static int16_t out[20000];
    int16_t hi = 32767, lo = -32768;
    AudioMixer *m = calloc(1, sizeof(*m));
    audio_mix_init(m, 44100);
    audio_mix_set_limit(m, AUDIO_MIX_HARD);
    int slot = audio_mix_add_sample(m, fill_const, &hi, buf, 256, 44100, 1);
    CHECK(slot >= 0, "add_sample");
    CHECK(audio_mix_set_voice_peak(m, slot, audio_mix_voice_gen(m, slot), 4 * 32768), "set peak");
    audio_mix_render(m, out, 20000);
    long wrong = 0, sat = 0;
    for (int i = 2000; i < 20000; i++) {       /* past any attack */
        if (out[i] != 32767) wrong++; else sat++;
    }
    CHECK(wrong == 0, "%ld samples are not 32767 (wrapped?)", wrong);
    CHECK(sat > 0, "no saturated samples");
    CHECK(m->clipped > 0, "clipped counter did not move");

    audio_mix_init(m, 44100);
    audio_mix_set_limit(m, AUDIO_MIX_HARD);
    slot = audio_mix_add_sample(m, fill_const, &lo, buf, 256, 44100, 1);
    audio_mix_set_voice_peak(m, slot, audio_mix_voice_gen(m, slot), 4 * 32768);
    audio_mix_render(m, out, 20000);
    wrong = 0;
    for (int i = 2000; i < 20000; i++) if (out[i] != -32768) wrong++;
    CHECK(wrong == 0, "%ld negative samples are not -32768", wrong);
    free(m);
}

static void test_slew(void)
{
    static int16_t buf[256], out[1];
    int16_t v = 1000;
    AudioMixer *m = calloc(1, sizeof(*m));
    audio_mix_init(m, 44100);
    int slot = audio_mix_add_sample(m, fill_const, &v, buf, 256, 400000, 1000);
    uint32_t gen = audio_mix_voice_gen(m, slot);
    CHECK(m->v[slot].pos == 0, "fresh voice");
    CHECK(audio_mix_set_voice_peak(m, slot, gen, 30000), "set at pos 0");
    CHECK(m->v[slot].peak == 30000, "immediate at pos 0 (peak %d)", m->v[slot].peak);

    for (int i = 0; i < 100; i++) audio_mix_render(m, out, 1);
    CHECK(m->v[slot].pos > 0, "voice has sounded");
    CHECK(m->v[slot].peak == 30000, "settled at %d", m->v[slot].peak);

    int goals[] = { 100000, 0, 5, AUDIO_GAIN_PEAK_MAX };
    for (size_t g = 0; g < sizeof(goals) / sizeof(goals[0]); g++) {
        int goal = goals[g];
        CHECK(audio_mix_set_voice_peak(m, slot, gen, goal), "set %d", goal);
        int step = m->v[slot].peak_step;
        int prev = m->v[slot].peak, maxd = 0, reached = -1;
        long budget = 44100L * 25 / 1000;
        for (long i = 0; i < budget + 10; i++) {
            audio_mix_render(m, out, 1);
            int d = abs(m->v[slot].peak - prev);
            if (d > maxd) maxd = d;
            prev = m->v[slot].peak;
            if (prev == goal && reached < 0) reached = (int)i + 1;
        }
        CHECK(maxd <= step, "goal %d: per-sample step %d exceeds peak_step %d", goal, maxd, step);
        CHECK(reached > 0 && reached <= budget, "goal %d reached after %d frames (budget %ld)", goal, reached, budget);
        CHECK(m->v[slot].peak == goal, "goal %d settled at %d", goal, m->v[slot].peak);
    }
    CHECK(audio_mix_set_voice_peak(m, slot, gen, 1000000), "above max accepted");
    CHECK(m->v[slot].peak_goal == AUDIO_GAIN_PEAK_MAX, "clamped to max (%d)", m->v[slot].peak_goal);

    int goal_before = m->v[slot].peak_goal;
    CHECK(!audio_mix_set_voice_peak(m, slot, gen, -1), "negative refused");
    CHECK(!audio_mix_set_voice_peak(m, slot, gen + 1, 500), "stale gen refused");
    CHECK(!audio_mix_set_voice_peak(m, slot ^ 1, gen, 500), "other slot refused");
    CHECK(!audio_mix_set_voice_peak(m, -1, gen, 500), "slot -1 refused");
    CHECK(!audio_mix_set_voice_peak(m, AUDIO_MAX_VOICES, gen, 500), "slot past end refused");
    CHECK(!audio_mix_set_voice_peak(NULL, slot, gen, 500), "NULL mixer refused");
    CHECK(m->v[slot].peak_goal == goal_before, "refusals did not touch the goal");

    audio_mix_init(m, 44100);                  /* every voice gone, then one added again */
    int slot2 = audio_mix_add_sample(m, fill_const, &v, buf, 256, 4000, 1000);
    CHECK(slot2 == slot, "slot reused (%d vs %d)", slot2, slot);
    CHECK(!audio_mix_set_voice_peak(m, slot, gen, 500) || audio_mix_voice_gen(m, slot) == gen,
          "old handle must be stale after reuse");
    free(m);
}

int main(void)
{
    test_total();
    test_peak();
    test_scale();
    test_saturate();
    test_slew();
    printf("audio_gain_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
