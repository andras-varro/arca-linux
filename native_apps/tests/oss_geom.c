/* oss_geom.c — Probe for the continuous-stream unification.
 *
 * Answers, for BOTH client configurations, in one run on one unit:
 *   - what the OSS shim GRANTS (rate / bits / channels), read back with SOUND_PCM_READ_*
 *   - the ring geometry it hands us with NO SNDCTL_DSP_SETFRAGMENT (fragsize, fragstotal)
 *
 * Why this exists. The shared device half's central arithmetic is "hold a lead of
 * AUDIO_PUMP_LEAD_PERIODS (3) whole device periods", and that constant was calibrated at
 * 44100/stereo where a period is 2048 frames (139 ms). ScummVM runs 22050 MONO, whose period
 * is unmeasured. If the shim gives mono a 4096-frame period, three of them is ~557 ms of lead
 * — a worse regression than the click the unification exists to fix. So the lead has to be a
 * per-client number derived from a measurement, not a shared constant.
 *
 * It also settles two standing questions:
 *   - the ~506 ms vs 743 ms ring contradiction (audio.c:233 and :396 reason from 506; the
 *     group-M test fixture says 32768 frames = 743 ms)
 *   - what audio.c's SNDCTL_DSP_STEREO request actually grants. That ioctl is documented in
 *     audio.c:74 as SILENTLY IGNORED on this hardware, and /proc/.../hw_params cannot answer
 *     it: the shim's plugin layer converts, so hw_params reports the stereo-only HARDWARE
 *     substream whatever the client asked for. Only SOUND_PCM_READ_CHANNELS sees the client
 *     side.
 *
 * Writes nothing to the device and does not touch GPIO12, so it makes no sound and needs no
 * listener. Standalone probe: uses no Audio struct and is deliberately NOT in
 * build-and-deploy.sh (same class as ch_test.c, oss_diag.c, oss_play.c).
 *
 * Build:
 *   arm-linux-gnueabihf-gcc -Wall -Wextra -O2 -static -o build/oss_geom tests/oss_geom.c
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

/* How the channel count is requested. The two shipped clients disagree, and the difference is
 * the point of the probe: audio.c uses the deprecated STEREO ioctl, oss-mixer.cpp uses
 * CHANNELS. */
typedef enum { REQ_STEREO_IOCTL, REQ_CHANNELS_IOCTL } ChanRequest;

static const char *req_name(ChanRequest r) {
    return (r == REQ_STEREO_IOCTL) ? "SNDCTL_DSP_STEREO" : "SNDCTL_DSP_CHANNELS";
}

static void probe(const char *label, int want_rate, int want_channels, ChanRequest how) {
    printf("\n=== %s: %d Hz, %d ch requested via %s ===\n",
           label, want_rate, want_channels, req_name(how));

    int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        printf("  open(/dev/dsp) FAILED errno=%d (%s)\n", errno, strerror(errno));
        printf("  ^ EBUSY here means another process holds the device; nothing else is\n");
        printf("    measurable until it exits. This is not a probe defect.\n");
        return;
    }

    /* Deliberately NO SNDCTL_DSP_SETFRAGMENT — this is the case both shipped clients run,
     * and the case whose geometry is in dispute. */

    /* Shipped order: SPEED -> FMT -> channels. SPEED may reset FMT and CHANNELS, so the last
     * two must come after it (audio.c:70-79). */
    int rate = want_rate;
    int rc_speed = ioctl(fd, SNDCTL_DSP_SPEED, &rate);

    int fmt = AFMT_S16_LE;
    int rc_fmt = ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);

    int rc_chan;
    if (how == REQ_STEREO_IOCTL) {
        int stereo = (want_channels == 2) ? 1 : 0;
        rc_chan = ioctl(fd, SNDCTL_DSP_STEREO, &stereo);
        printf("  set: SPEED rc=%d (arg now %d) | SETFMT rc=%d (arg now 0x%x) | STEREO rc=%d (arg now %d)\n",
               rc_speed, rate, rc_fmt, fmt, rc_chan, stereo);
    } else {
        int ch = want_channels;
        rc_chan = ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
        printf("  set: SPEED rc=%d (arg now %d) | SETFMT rc=%d (arg now 0x%x) | CHANNELS rc=%d (arg now %d)\n",
               rc_speed, rate, rc_fmt, fmt, rc_chan, ch);
    }

    /* The only numbers to trust. A set-ioctl's write-back is documented not to reflect device
     * state on this shim. */
    int got_rate = -1, got_bits = -1, got_chan = -1;
    int rc_rr = ioctl(fd, SOUND_PCM_READ_RATE,     &got_rate);
    int rc_rb = ioctl(fd, SOUND_PCM_READ_BITS,     &got_bits);
    int rc_rc = ioctl(fd, SOUND_PCM_READ_CHANNELS, &got_chan);

    printf("  GRANTED (SOUND_PCM_READ_*): rate=%d (rc=%d)  bits=%d (rc=%d)  channels=%d (rc=%d)\n",
           got_rate, rc_rr, got_bits, rc_rb, got_chan, rc_rc);
    if (got_chan != want_channels)
        printf("  *** CHANNEL REQUEST NOT HONOURED: asked %d, granted %d\n", want_channels, got_chan);
    if (got_bits != 16)
        printf("  *** FORMAT IS NOT 16-BIT: %d — audio.c:85 ignores this read-back today\n", got_bits);

    /* Ring geometry, on an untouched (empty) ring, so fragstotal*fragsize is the capacity. */
    audio_buf_info abi;
    memset(&abi, 0, sizeof(abi));
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &abi) == 0) {
        int frame_bytes = (got_chan > 0 ? got_chan : 1) * 2;
        long ring_bytes = (long)abi.fragstotal * abi.fragsize;
        long period_frames = abi.fragsize / frame_bytes;
        long ring_frames = ring_bytes / frame_bytes;

        printf("  GETOSPACE: fragments=%d fragstotal=%d fragsize=%d bytes=%d\n",
               abi.fragments, abi.fragstotal, abi.fragsize, abi.bytes);
        printf("  derived  : frame=%d B | period=%ld frames", frame_bytes, period_frames);
        if (got_rate > 0)
            printf(" (%ld ms)", period_frames * 1000L / got_rate);
        printf(" | ring=%ld frames", ring_frames);
        if (got_rate > 0)
            printf(" (%ld ms)", ring_frames * 1000L / got_rate);
        printf("\n");

        /* The number the unification actually needs: 3 whole periods, per client. */
        long lead3 = period_frames * 3;
        printf("  LEAD at 3 periods: %ld frames", lead3);
        if (got_rate > 0)
            printf(" = %ld ms", lead3 * 1000L / got_rate);
        printf("   <-- onset latency this config would pay\n");
    } else {
        printf("  GETOSPACE FAILED errno=%d (%s)\n", errno, strerror(errno));
    }

    close(fd);
}

int main(void) {
    printf("oss_geom — OSS shim geometry for both client configs, no SETFRAGMENT, no audio written\n");

    /* Exactly what native_apps/common/audio.c asks for today. */
    probe("native_apps as shipped", 44100, 2, REQ_STEREO_IOCTL);

    /* The same request expressed the other way, to see whether the deprecated ioctl is the
     * reason for whatever the line above grants. */
    probe("native_apps rate, CHANNELS ioctl", 44100, 2, REQ_CHANNELS_IOCTL);

    /* Exactly what scummvm-arca/backend-files/oss-mixer.cpp asks for today. */
    probe("ScummVM as shipped", 22050, 1, REQ_CHANNELS_IOCTL);

    /* If the shared library standardised on stereo, this is what ScummVM would run: same rate,
     * double the bytes and double its mixer's work on a core already at ~32% with Full Throttle. */
    probe("ScummVM rate, forced stereo", 22050, 2, REQ_CHANNELS_IOCTL);

    printf("\ndone\n");
    return 0;
}
