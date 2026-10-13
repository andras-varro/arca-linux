/*
 * alsa_stub/alsa/asoundlib.h — a TEST-ONLY stand-in for alsa-lib's header.
 *
 * Put first on the include path (`-I tests/alsa_stub`) together with
 * `-DAUDIO_OUT_HAVE_ALSA`, it lets a host regression compile the ALSA backend in
 * `common/audio_out.c` unchanged, with no libasound present: this header only
 * DECLARES what that file uses, and the test defines every function, so the test
 * decides what each ALSA call answers.  Nothing in the device build can reach it —
 * both device build paths put the real alsa-lib prefix on the path instead.
 *
 * ⚠️ Declares only the subset `audio_out.c` calls.  A new snd_* call there fails
 * this build with an implicit-declaration error, which is the intended signal:
 * add it here and give it an answer in the test.
 */
#ifndef ARCA_TEST_ALSA_STUB_H
#define ARCA_TEST_ALSA_STUB_H

#include <stddef.h>
#include <sys/types.h>

typedef struct _snd_pcm           snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long          snd_pcm_sframes_t;

typedef enum { SND_PCM_STREAM_PLAYBACK = 0 } snd_pcm_stream_t;
typedef enum { SND_PCM_ACCESS_RW_INTERLEAVED = 3 } snd_pcm_access_t;
typedef enum { SND_PCM_FORMAT_S16_LE = 2 } snd_pcm_format_t;
#define SND_PCM_NONBLOCK 0x1
/* The real header's values, so a state name read in a log matches the device's. */
typedef enum {
    SND_PCM_STATE_OPEN = 0, SND_PCM_STATE_SETUP, SND_PCM_STATE_PREPARED,
    SND_PCM_STATE_RUNNING, SND_PCM_STATE_XRUN, SND_PCM_STATE_DRAINING,
    SND_PCM_STATE_PAUSED, SND_PCM_STATE_SUSPENDED, SND_PCM_STATE_DISCONNECTED
} snd_pcm_state_t;

/* Stack storage in the real header; a static block is enough for one caller. */
#define snd_pcm_hw_params_alloca(p) \
    do { static long arca_hw_blk[16]; *(p) = (snd_pcm_hw_params_t *)arca_hw_blk; } while (0)
#define snd_pcm_sw_params_alloca(p) \
    do { static long arca_sw_blk[16]; *(p) = (snd_pcm_sw_params_t *)arca_sw_blk; } while (0)

int  snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode);
int  snd_pcm_close(snd_pcm_t *pcm);
int  snd_pcm_prepare(snd_pcm_t *pcm);
int  snd_pcm_resume(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_pcm_avail(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t size);
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm);
const char *snd_pcm_state_name(snd_pcm_state_t state);
const char *snd_strerror(int errnum);
int  snd_pcm_format_width(snd_pcm_format_t format);

int  snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p);
int  snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_access_t a);
int  snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_format_t f);
int  snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                     unsigned int *val, int *dir);
int  snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                         unsigned int *val);
int  snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                            snd_pcm_uframes_t *val, int *dir);
int  snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                            snd_pcm_uframes_t *val);
int  snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p);
int  snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *p, unsigned int *val, int *dir);
int  snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *p, unsigned int *val);
int  snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *p,
                                       snd_pcm_uframes_t *val, int *dir);
int  snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *p, snd_pcm_uframes_t *val);

int  snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *p);
int  snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *p,
                                           snd_pcm_uframes_t val);
int  snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *p);

#endif /* ARCA_TEST_ALSA_STUB_H */
