/*
 * vid_player.h — the video player of Ludkino: decoder thread, mix-bus voice, clock, picture and subtitles.
 * One long-lived object; start() hands it a stream fd, tick() runs on the main thread once per loop AFTER
 * audio_pump().  The decoder is vid_dec on its own nice+5 thread (never RT); a TLS stream is relayed through a
 * pipe because vid_dec reads the raw fd.  Landscape only.
 */
#ifndef ARCA_VID_PLAYER_H
#define ARCA_VID_PLAYER_H

#include <stdbool.h>
#include "audio.h"
#include "framebuffer.h"
#include "logger.h"

typedef struct VidPlayer VidPlayer;

typedef struct {
    int  channels;        /**< 1 or 2: the sink */
    int  offset_ms;       /**< A/V offset of the sink, > 0 shows video later */
    bool agc;
    bool tls;             /**< the fd is a TLS stream (read it through http_stream_read) */
    long start_ms;        /**< media time of the stream's first picture */
    int  sub_scale;       /**< 2 or 3 */
} VidPlayerCfg;

VidPlayer *vid_player_new(Audio *audio, bool audio_ok, Framebuffer *fb, Logger *log);
void vid_player_free(VidPlayer *p);
/** Takes the fd (closed by stop/end).  Stops a running playback first.  0 ok, -1 failed (fd closed). */
int  vid_player_start(VidPlayer *p, int fd, const VidPlayerCfg *cfg);
/** Stops without an event; joins the thread, closes the fd.  Subtitles stay. */
void vid_player_stop(VidPlayer *p);
void vid_player_pause(VidPlayer *p, bool paused);
/** Main thread, once per loop after audio_pump().  Returns -1 normally, else the PLAYER_ENDED code
 *  (1 = end of stream, 0 = failure); the player has stopped itself then. */
int  vid_player_tick(VidPlayer *p, unsigned long long now_ms);
/** Media position in ms, -1 when nothing plays or before the first sound/picture. */
long vid_player_position(const VidPlayer *p);
bool vid_player_active(const VidPlayer *p);

/** Logical rows from the top / bottom of the surface that the UI draws (overlay bars); 0, 0 = none.  present() leaves them
 *  alone, and the subtitle sprite sits above the bottom band.  Main thread. */
void vid_player_set_bands(VidPlayer *p, int top_h, int bot_h);
/** One-off redraw of the picture area outside the bands: the head frame (not released), else black.  For un-pausing a hidden
 *  overlay or closing a full-screen panel while paused (nothing is presented then).  Main thread only. */
void vid_player_repaint(VidPlayer *p);
/** User volume in milli-dB (the Ludfin jf_volume_mdb scale), summed by audio_gain_total_mdb(0, .).  Applied when the voice is
 *  added and slewed on a live one; remembered across start() / seeks.  Default 0. */
void vid_player_set_gain_mdb(VidPlayer *p, int user_mdb);
/** Suggested loop sleep in microseconds while playing (<= max_us). */
int  vid_player_wait_us(const VidPlayer *p, int max_us);

void vid_player_sub_feed(VidPlayer *p, int fd, bool tls);   /**< the SRT stream; read incrementally in tick() */
void vid_player_sub_clear(VidPlayer *p);

#endif
