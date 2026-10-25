/* retro — libretro frontend for Arca: shared declarations.
 * The libretro ABI subset is declared here (stable ABI), not taken from libretro.h. */
#ifndef RETRO_H
#define RETRO_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { PF_0RGB1555 = 0, PF_XRGB8888 = 1, PF_RGB565 = 2 };
#define RDEV_JOYPAD 1
#define RDEV_ANALOG 5
#define RDEV_KEYBOARD 3
#define RJ_SELECT 2
#define RJ_START 3

double now_s(void);

/* video backend: init once, present the core's newest frame, close restores */
typedef struct {
    const char *name;
    int  (*init)(void);
    /* convert/scale (timed as "conv") then flip on the next vsync ("present") */
    void (*frame)(const void *data, unsigned w, unsigned h, size_t pitch, int fmt, float aspect);
    void (*present)(void);
    void (*fill)(uint16_t rgb565);   /* whole screen, both pages (loading screen) */
    void (*close)(void);
} VideoBackend;
extern const VideoBackend video_sw, video_overlay;
extern double vstat_conv, vstat_present;   /* seconds, accumulated */
extern unsigned vstat_vsync_waits;
void video_dst_rect(unsigned w, unsigned h, float aspect, int integer,
                    unsigned *x, unsigned *y, unsigned *dw, unsigned *dh);
int  fb0_open(void);       /* shared: fd of /dev/fb0, 16bpp 2-page mode */
void fb0_restore(void);
void vsync_guard(double pan_time);   /* returns once a vsync happened after pan_time */
void vsync_wait(void);

/* audio */
int  aud_open(double core_rate, double core_fps);
void aud_push(const int16_t *stereo, size_t frames);
void aud_service(void);
void aud_stats(unsigned *ring_underruns, unsigned *dev_starved, double *level_ms, double *ratio);
void aud_close(void);

/* input */
void in_open(void);
void in_poll(void);
int16_t in_state(unsigned port, unsigned dev, unsigned idx, unsigned id);
bool in_quit(void);
extern bool in_computer;   /* keyboard goes to the core's keyboard, not the joypad */
extern void (*in_kbd_cb)(bool down, unsigned keycode, uint32_t ch, uint16_t mods);
void in_close(void);
#endif
