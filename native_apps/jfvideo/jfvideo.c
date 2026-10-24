/*
 * jfvideo.c — Ludkino, the Jellyfin video client: wiring only.
 * Input -> JvEvent -> jv_step -> execute JvCmds (jf_api / player / art) -> draw.
 * All policy is in jv_core.c, all screens in jv_ui.c.
 *
 * Two JfApi instances: [0] metadata and the subtitle SRT, [1] the stream, the reports and the encoding-job DELETEs
 * (a stream open can take seconds and must not hold the reports behind it; two STREAMs on one instance supersede
 * each other, which is why the SRT is on [0]); JvCmd.api picks one.
 *
 * The player (vid_player: decoder thread, mix-bus voice, picture, subtitles) is driven through the player_*() hooks
 * below; while a picture plays on the player screen it owns the frame (see video_owns_frame).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "framebuffer.h"
#include "touch_input.h"
#include "gamepad.h"
#include "hardware.h"
#include "pointer.h"
#include "audio_out.h"
#include "logger.h"
#include "jv_app.h"
#include "jv_core.h"
#include "jv_ui.h"
#include "vid_player.h"
#include "audio.h"
#include "config.h"
#include "eq_panel.h"
#include "jf_login.h"

#ifdef JV_LOOP_PROFILE
static unsigned long long prof_us[6], prof_t0, prof_next;
static unsigned prof_iter, prof_redraw;
static unsigned long long prof_now_us(void)
{
    struct timespec pts;
    clock_gettime(CLOCK_MONOTONIC, &pts);
    return (unsigned long long)pts.tv_sec * 1000000ULL + (unsigned long long)(pts.tv_nsec / 1000);
}
/* close section s: add the time since the previous mark */
#define PROF_MARK(s) do { unsigned long long n_ = prof_now_us(); prof_us[s] += n_ - prof_t0; prof_t0 = n_; } while (0)
#else
#define PROF_MARK(s) do { } while (0)
#endif

#define EVQ_MAX 12
#define API_N   2
#define EXIT_PUMP_MS 2000

static volatile sig_atomic_t running = 1;
static void signal_handler(int sig) { (void)sig; running = 0; }

static JvCore core;                 /* ~0.2 MB: static */
static JfApi  api[API_N];
static Logger logger;
static JvUi   ui;
static JvEvent evq[EVQ_MAX];
static int evq_n;
static bool exit_cmd;               /* the core asked to end */

/* the server the art instances fetch from: filled by SET_SERVER, used at SET_AUTH */
static char art_host[JF_HOST_MAX + 1];
static int  art_port;
static bool art_tls;

static void push(const JvEvent *e)
{
    if (evq_n < EVQ_MAX) evq[evq_n++] = *e;
}

static void push_type(JvEventType t, int i)
{
    JvEvent e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.i = i;
    push(&e);
}

/* a request that could not even be queued answers as a bad reply, so the core's wait ends */
static void push_failed(const JvCmd *c)
{
    JvEvent e;
    memset(&e, 0, sizeof e);
    e.type = JV_EV_RESULT;
    e.res.kind = c->kind;
    e.res.tag = c->tag;
    e.res.err = JF_ERR_BADREPLY;
    e.res.fd = -1;
    e.res.dst = c->dst;
    push(&e);
}

/* ── the player ───────────────────────────────────────────────────────────── */
static Audio audio;
static bool audio_ok;
static VidPlayer *vp;
static bool eq_open, eq_was_playing;   /* the equaliser panel is up; the picture ran when it opened */
static bool force_full, bands_force;   /* next frame: full redraw; redraw the bands */
static bool pic_prev;                  /* the picture owned the frame last frame */
static int  band_top, band_bot;        /* what vid_player_set_bands() holds now */
static uint32_t band_t;                /* when the bands were last presented */
static char ban_prev[48];
static bool busy_prev;

static void player_start(const JvCmd *c)
{
    if (!vp) { http_stream_close(c->fd); push_type(JV_EV_PLAYER_ENDED, 0); return; }
    VidPlayerCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.channels = core.channels;
    cfg.offset_ms = core.set.av_offset_ms[audio_out_device_is_onboard() ? JV_SINK_ONBOARD : JV_SINK_USB];
    cfg.agc = core.set.agc;
    cfg.tls = art_tls;
    cfg.start_ms = core.play.start_ms;
    cfg.sub_scale = core.set.sub_scale;
    if (vid_player_start(vp, c->fd, &cfg) < 0) push_type(JV_EV_PLAYER_ENDED, 0);
}
static void player_stop(void) { if (vp) vid_player_stop(vp); }
static void player_pause(bool paused) { if (vp) vid_player_pause(vp, paused); }
static void player_sub_feed(int fd)
{
    if (vp) vid_player_sub_feed(vp, fd, art_tls);
    else http_stream_close(fd);
}
static void player_sub_clear(void) { if (vp) vid_player_sub_clear(vp); }
/* the media position now in ms, or -1 when nothing plays */
static long player_position(void) { return vp ? vid_player_position(vp) : -1; }

/* What the UI overlay shows on the player screen.  While a picture plays the video owns the frame, so the UI
 * redraws (and overwrites the picture) only when one of these changes. */
typedef struct {
    JvScreen screen; bool overlay, busy, seek_pending; unsigned buttons, generation;
    JvPlayState state; int audio_index, sub_index;
    char overlay_text[48], banner[48];
} VisSig;
static VisSig vis_prev;
static bool vis_changed;

static void vis_note(void)
{
    VisSig s;
    memset(&s, 0, sizeof s);
    s.screen = core.screen; s.overlay = core.overlay; s.busy = core.busy; s.seek_pending = core.seek_pending;
    s.buttons = core.overlay_buttons; s.generation = core.play.generation; s.state = core.play_state;
    s.audio_index = core.play.audio_index; s.sub_index = core.play.sub_index;
    snprintf(s.overlay_text, sizeof s.overlay_text, "%s", core.overlay_text);
    snprintf(s.banner, sizeof s.banner, "%s", core.banner);
    if (memcmp(&s, &vis_prev, sizeof s) != 0) { vis_prev = s; vis_changed = true; }
}

static bool video_owns_frame(void)
{
    return vp && vid_player_active(vp) && core.screen == JV_SCR_PLAYER && !core.overlay;
}

/* ── volume, bands, equaliser ─────────────────────────────────────────────── */
static int vol_level = JF_VOLUME_DEFAULT;

/* Single volume entry point: every level change goes through here (not persisted); a system-volume job replaces its body. */
static void volume_step(int delta)
{
    int s = vol_level + delta;
    if (s < 0) s = 0;
    if (s > JF_VOLUME_MAX) s = JF_VOLUME_MAX;
    vol_level = s;
    if (vp) vid_player_set_gain_mdb(vp, jf_volume_mdb(s));
    jv_ui_set_volume(&ui, s, JF_VOLUME_MAX);
    bands_force = true;
}

/* The gains the panel opens with: the live stream's when it is open, else the saved setting (as Ludfin does). */
static void eq_now(int g[EQ_BANDS])
{
    if (audio_ok && audio_out_is_open(&audio.out)) audio_get_eq(&audio, g);
    else eq_config_read(CONFIG_FILE_PATH, g);
}
static void enh_now(AudioEnh *e)
{
    if (audio_ok && audio_out_is_open(&audio.out)) audio_get_enh(&audio, e);
    else enh_config_read(CONFIG_FILE_PATH, e);
}

/* The equaliser is full frame: pause the picture (through the core, so the reports stay true), remember whether it ran. */
static void eq_begin(uint32_t now)
{
    int g[EQ_BANDS];
    AudioEnh en;
    memset(&en, 0, sizeof en);
    eq_was_playing = core.play_state == JV_PLAY_PLAYING;
    if (eq_was_playing) push_type(JV_EV_PAUSE, 0);
    eq_now(g);
    enh_now(&en);
    jv_ui_eq_open(&ui, g, &en, now);
    eq_open = true;
    force_full = true;
}

static void eq_end(void)
{
    eq_open = false;
    if (eq_was_playing && core.play_state == JV_PLAY_PAUSED) push_type(JV_EV_PAUSE, 0);
    eq_was_playing = false;
    force_full = true;       /* the next full swap is followed by vid_player_repaint() */
}

/* The rows (from the top / from the bottom of the surface) the video must leave alone: the overlay's bands while it is shown. */
static void bands_want(int *top, int *bot)
{
    *top = *bot = 0;
    if (core.screen != JV_SCR_PLAYER || core.overlay || eq_open || !jv_ui_overlay_shown(&ui)) return;
    UiRect t, b;
    jv_ui_band_rects(&ui, &t, &b);
    *top = t.y + t.h;
    *bot = ui.fb->height - b.y;
    if (*top < 0) *top = 0;
    if (*bot < 0) *bot = 0;
}

/* Hand a changed band geometry to the player; true when it changed.  Hiding while paused repaints the rows the bands covered. */
static bool bands_sync(void)
{
    int t, b;
    bands_want(&t, &b);
    if (t == band_top && b == band_bot) return false;
    band_top = t;
    band_bot = b;
    if (!t && !b) force_full = true;   /* hiding: the letterbox under the bands goes back to black */
    if (vp) {
        vid_player_set_bands(vp, t, b);
        if (!t && !b && core.play_state == JV_PLAY_PAUSED && core.screen == JV_SCR_PLAYER && !eq_open) vid_player_repaint(vp);
    }
    return true;
}

static void execute(const JvCmds *cmds)
{
    for (int i = 0; i < cmds->n; i++) {
        const JvCmd *c = &cmds->c[i];
        int a = (c->api >= 0 && c->api < API_N) ? c->api : 0;
        switch (c->type) {
        case JV_CMD_SET_SERVER:
            for (int k = 0; k < API_N; k++) {
                jf_api_set_server(&api[k], c->host, c->port);
                jf_api_set_tls(&api[k], c->tls);
            }
            snprintf(art_host, sizeof art_host, "%s", c->host);
            art_port = c->port;
            art_tls = c->tls;
            break;
        case JV_CMD_SET_AUTH:
            for (int k = 0; k < API_N; k++)
                jf_api_set_auth(&api[k], JF_APP_NAME, JF_APP_NAME, core.login.device_id, core.login.token);
            jv_ui_art_server(&ui, art_host, art_port, art_tls, JF_APP_NAME, JF_APP_NAME,
                             core.login.device_id, core.login.token);
            break;
        case JV_CMD_REQUEST: {
            int rc;
            switch (c->kind) {
            case JF_KIND_AUTH:
                rc = jf_api_enqueue_auth(&api[a], c->tag, c->user, c->password);
                jf_wipe((void *)c->password, sizeof c->password);
                break;
            case JF_KIND_PARSE:  rc = jf_api_enqueue_parse(&api[a], c->tag, c->path, c->fn, c->dst, c->group); break;
            case JF_KIND_POST:   rc = jf_api_enqueue_post_json(&api[a], c->tag, c->path, c->body, (size_t)c->n); break;
            case JF_KIND_WRITE:  rc = jf_api_enqueue_write(&api[a], c->tag, c->method, c->path); break;
            case JF_KIND_STREAM: rc = jf_api_enqueue_stream(&api[a], c->tag, c->path); break;
            default:             rc = -1; break;
            }
            if (rc < 0) push_failed(c);
            break;
        }
        case JV_CMD_CANCEL:
            jf_api_cancel_kind(&api[a], c->kind);
            break;
        case JV_CMD_PLAYER_START:  player_start(c); break;
        case JV_CMD_PLAYER_STOP:   player_stop(); break;
        case JV_CMD_PLAYER_PAUSE:  player_pause(true); break;
        case JV_CMD_PLAYER_RESUME: player_pause(false); break;
        case JV_CMD_SUB_FEED:      player_sub_feed(c->fd); break;
        case JV_CMD_SUB_CLEAR:     player_sub_clear(); break;
        case JV_CMD_CLOSE_FD:      http_stream_close(c->fd); break;
        case JV_CMD_SAVE_LAST: {
            JvLast r = c->last;
            r.saved_at = (long long)time(NULL);
            if (jv_last_save(&r, JF_LAST_PATH) < 0) LOG_WARN(&logger, "last: cannot write " JF_LAST_PATH);
            break;
        }
        case JV_CMD_CLEAR_LAST:    jv_last_clear(JF_LAST_PATH); break;
        case JV_CMD_EXIT:
            exit_cmd = true;
            running = 0;
            break;
        default:
            break;
        }
    }
}

static void step_event(const JvEvent *ev, long long now)
{
    static JvCmds cmds;
    memset(&cmds, 0, sizeof cmds);
    jv_step(&core, ev, now, &cmds);
    execute(&cmds);
    vis_note();
    if (ev->type != JV_EV_TICK || cmds.n > 0) jv_ui_mark_dirty(&ui);   /* an idle tick changes nothing the screen shows */
}

/* Results of both instances go to the core as events. */
static void drain_results(void)
{
    static JfResult res;
    for (int k = 0; k < API_N; k++) {
        while (jf_api_poll(&api[k], &res)) {
            JvEvent e;
            memset(&e, 0, sizeof e);
            e.type = JV_EV_RESULT;
            e.res = res;
            push(&e);
            memset(&res, 0, sizeof res);
        }
    }
}

static bool apis_busy(void)
{
    for (int k = 0; k < API_N; k++) if (jf_api_busy(&api[k])) return true;
    return false;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *fb_device = (argc > 1) ? argv[1] : "/dev/fb0";
    const char *touch_device = (argc > 2) ? argv[2] : "/dev/input/touchscreen0";

    int lock_fd = acquire_instance_lock(JF_APP_BIN);
    if (lock_fd < 0) {
        fprintf(stderr, JF_APP_BIN ": another instance is already running\n");
        return 1;
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    hw_init();
    hw_set_backlight(100);

    fb_set_bpp(fb_device, 32);
    Framebuffer fb;
    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, JF_APP_BIN ": cannot open framebuffer %s\n", fb_device);
        return 1;
    }
    fb_clear(&fb, COLOR_BLACK);
    text_draw_centered(&fb, fb.width / 2, fb.height / 2, "LOADING...", COLOR_WHITE, 3);
    fb_swap(&fb);
    TouchInput touch;
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, JF_APP_BIN ": cannot open touch %s\n", touch_device);
        fb_close(&fb);
        return 1;
    }
    touch_set_screen_size(&touch, fb.width, fb.height);

    GamepadManager gp;
    gamepad_init(&gp);
    Pointer ptr;
    pointer_init(&ptr);

    for (int k = 0; k < API_N; k++) {
        if (jf_api_init(&api[k]) != 0) {
            fprintf(stderr, JF_APP_BIN ": out of memory\n");
            fb_close(&fb);
            return 1;
        }
    }
    logger_init(&logger, JF_APP_BIN, LOG_LEVEL_INFO, false);
    for (int k = 0; k < API_N; k++) api[k].log = &logger;

    jv_core_init(&core, JF_CONF_PATH, JV_REUSE_LOGIN_PATH, JV_DEVICE_ID_SUFFIX);
    jv_settings_load(&core.set, JF_SETTINGS_PATH);
    jv_last_load(&core.last, JF_LAST_PATH);
    core.channels = audio_out_device_is_onboard() ? 1 : 2;   /* what the sink can take; rate and max_w keep the core's defaults */

    jv_ui_init(&ui, &fb, &touch, &gp, &core, JF_ART_CACHE_DIR);
    audio_ok = audio_init(&audio) == 0;
    vp = vid_player_new(&audio, audio_ok, &fb, &logger);
    if (!vp) LOG_ERROR(&logger, "player: out of memory");
    volume_step(0);   /* applies the default level to the player */

    push_type(JV_EV_START, 0);

    InputState in;
    memset(&in, 0, sizeof in);
    bool first = true;

    while (running) {
        uint32_t now = get_time_ms();
#ifdef JV_LOOP_PROFILE
        prof_t0 = prof_now_us();
#endif

        touch_poll(&touch);
        TouchState ts = touch_get_state(&touch);
        gamepad_poll(&gp, &in, 0, 0, false);
        gamepad_tick(&gp, now);
        bool navkey = false;
        for (int i = 0; i < BTN_ID_COUNT; i++) if (in.buttons[i].pressed) navkey = true;
        pointer_update(&ptr, &in, ts.pressed, navkey, &ts);

        static JvEvent uev[4];
        int un = first ? 0 : jv_ui_input(&ui, &ts, &in, &ptr, now, uev, 4);
        first = false;
        {   /* the list view as the finger left it, before this frame's events can open something */
            int vt, vc;
            if (jv_ui_view(&ui, &vt, &vc)) jv_note_view(&core, vt, vc);
        }
        for (int i = 0; i < un; i++) push(&uev[i]);
        {   /* what the overlay asked for: volume and the equaliser (neither is a core event) */
            unsigned act = jv_ui_take_actions(&ui);
            if (act & JV_UI_ACTB_VOL_UP) volume_step(+1);
            if (act & JV_UI_ACTB_VOL_DN) volume_step(-1);
            if ((act & JV_UI_ACTB_EQ_OPEN) && !eq_open && core.screen == JV_SCR_PLAYER) eq_begin(now);
            int eg[EQ_BANDS];
            bool esave;
            if (jv_ui_eq_take(&ui, eg, &esave)) {   /* every edit is heard at once, DONE also saves */
                if (audio_ok) audio_set_eq(&audio, eg);
                if (audio_ok) audio_set_enh(&audio, jv_ui_enh_out(&ui));
                if (esave) eq_panel_store_file_both(CONFIG_FILE_PATH, eg, jv_ui_enh_out(&ui));
            }
            if (eq_open && (jv_ui_eq_closed(&ui) || core.screen != JV_SCR_PLAYER)) eq_end();
        }

        PROF_MARK(0);
        jv_ui_art_poll(&ui, now);
        PROF_MARK(1);
        drain_results();
        PROF_MARK(2);

        if (audio_ok) audio_pump(&audio);
        if (vp) {
            int ended = vid_player_tick(vp, now);
            if (ended >= 0) push_type(JV_EV_PLAYER_ENDED, ended);
        }
        long pos = player_position();
        if (pos >= 0 && core.play_state == JV_PLAY_PLAYING) push_type(JV_EV_POS, (int)pos);
        push_type(JV_EV_TICK, 0);

        for (int i = 0; i < evq_n; i++) {      /* may push more; evq_n grows */
            JvEvent copy = evq[i];
            step_event(&copy, (long long)now);
            jf_wipe(&copy.password, sizeof copy.password);
            jf_wipe(&evq[i].password, sizeof evq[i].password);
        }
        evq_n = 0;
        PROF_MARK(3);

        /* The picture owns the front buffer while it plays (the video writes fb->buffer; the UI draws in the back
         * buffer).  So: bands (and only they) are presented with fb_swap_rect, on change and once a second; a full
         * swap happens only when the picture is not what is on screen, or just before a repaint of it. */
        bool bands_moved = bands_sync();
        bool pic = video_owns_frame() && !eq_open;
        bool ban_chg = strcmp(ban_prev, core.banner) != 0 || busy_prev != core.busy;
        snprintf(ban_prev, sizeof ban_prev, "%s", core.banner);
        busy_prev = core.busy;
        bool dirty = jv_ui_take_dirty(&ui);
        bool bdirty = jv_ui_take_bands_dirty(&ui);
        bool shown = pic && jv_ui_overlay_shown(&ui);
        bool full, bands_only = false;
        if (pic) {
            full = ban_chg || force_full || !pic_prev;
            bands_only = !full && shown &&
                         (bands_moved || bdirty || bands_force || vis_changed || (now - band_t) >= 1000);
        } else {
            full = dirty || force_full;
        }
        pic_prev = pic;
        force_full = bands_force = false;
        vis_changed = false;
        bool redraw = full || bands_only;
        if (full) {
            if (core.screen == JV_SCR_PLAYER) fb_clear(&fb, COLOR_BLACK);
            jv_ui_draw(&ui);
            fb_swap(&fb);
            if (pic && !core.banner[0] && !core.busy) vid_player_repaint(vp);   /* the picture the full swap covered */
            band_t = now;
        } else if (bands_only) {
            UiRect bt, bb;
            jv_ui_draw(&ui);
            jv_ui_band_rects(&ui, &bt, &bb);
            fb_swap_rect(&fb, bt.x, bt.y, bt.w, bt.h);
            fb_swap_rect(&fb, bb.x, bb.y, bb.w, bb.h);
            band_t = now;
        }
        if (redraw) {
            pointer_invalidate(&ptr);
            pointer_paint(&ptr, &fb);
        }
        bool ptr_moved = !redraw && pointer_paint(&ptr, &fb);
        fb_overtemp_poll(&fb);
        bool active = redraw || ptr_moved || core.busy || lv_needs_frames(&ui.lv);
#ifdef JV_LOOP_PROFILE
        PROF_MARK(4);
        prof_iter++;
        if (redraw) prof_redraw++;
        if (prof_now_us() >= prof_next) {
            if (prof_next)
                LOG_INFO(&logger, "loop profile us/10s: input %llu art %llu drain %llu step %llu draw %llu iters %u redraws %u",
                         prof_us[0], prof_us[1], prof_us[2], prof_us[3], prof_us[4], prof_iter, prof_redraw);
            memset(prof_us, 0, sizeof prof_us);
            prof_iter = prof_redraw = 0;
            prof_next = prof_now_us() + 10000000ULL;
        }
#endif
        if (audio_ok && audio_pump_active(&audio)) active = true;
        int delay = active ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US;
        if (vp && vid_player_active(vp)) delay = vid_player_wait_us(vp, FRAME_DELAY_ACTIVE_US);
        usleep(delay);
    }

    /* Leaving: a signal skips the core's own EXIT, so ask for the stop reports here; then let the queue empty. */
    if (!exit_cmd) {
        JvEvent q;
        memset(&q, 0, sizeof q);
        q.type = JV_EV_QUIT;
        step_event(&q, (long long)get_time_ms());
    }
    player_stop();
    if (audio_ok) {
        for (int i = 0; i < 20; i++) { audio_pump(&audio); usleep(16667); }
        audio_close(&audio);
    }
    if (vp) vid_player_free(vp);
    {
        uint32_t t0 = get_time_ms();
        static JfResult res;
        while (get_time_ms() - t0 < EXIT_PUMP_MS && apis_busy()) {
            for (int k = 0; k < API_N; k++)
                while (jf_api_poll(&api[k], &res)) {
                    if (res.fd >= 0) http_stream_close(res.fd);
                    memset(&res, 0, sizeof res);
                }
            usleep(20000);
        }
    }

    for (int k = 0; k < API_N; k++) jf_api_free(&api[k]);
    logger_close(&logger);
    jv_ui_free(&ui);
    jf_login_wipe(&core.login);
    hw_leds_off();
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    gamepad_close(&gp);
    touch_close(&touch);
    fb_close(&fb);
    return 0;
}
