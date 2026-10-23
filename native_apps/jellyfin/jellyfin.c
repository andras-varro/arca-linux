/*
 * jellyfin.c — Ludfin music client (Jellyfin server): wiring only.
 * Input -> JfEvent -> jf_step -> execute JfCmds (jf_api / audio / jf_login) -> draw.
 * All policy is in jf_core.c, all screens in jf_ui.c.  Leaving the app stops the music.
 */
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "framebuffer.h"
#include "touch_input.h"
#include "gamepad.h"
#include "hardware.h"
#include "pointer.h"
#include "audio.h"
#include "audio_out.h"
#include "logger.h"
#include "jf_app.h"
#include "jf_art.h"
#include "jf_ui.h"
#include "jf_queue_store.h"

#define EVQ_MAX 8

static volatile sig_atomic_t running = 1;
static void signal_handler(int sig) { (void)sig; running = 0; }

/* Stereo for a USB or Bluetooth sink; the panel speaker is permanently mono, so asking the
 * server for two channels there only doubles the bytes and the CPU. */
static int sink_channels(void) { return audio_out_device_is_onboard() ? 1 : 2; }
static JfApp app;          /* ~0.8 MB: static */
static JfApi api;
static Logger logger;      /* jf_api logs each request: kind, status, error, ms */
static JfUi  ui;
static Audio audio;
static bool  audio_ok;
static bool  track_live;   /* a stream was started and its end is still to be reported */
static const AudioFdIo stream_io = { http_stream_read, http_stream_close };
static uint32_t track_t0;

/* The gains the equaliser panel opens with: the live stream's when it is open, else the saved setting. */
static void eq_now(int g[EQ_BANDS])
{
    if (audio_ok && audio_out_is_open(&audio.out)) audio_get_eq(&audio, g);
    else eq_config_read(CONFIG_FILE_PATH, g);
}

/* The enhancer settings the panel opens with: the live stream's when it is open, else the saved setting. */
static void enh_now(AudioEnh *e)
{
    if (audio_ok && audio_out_is_open(&audio.out)) audio_get_enh(&audio, e);
    else enh_config_read(CONFIG_FILE_PATH, e);
}

static JfEvent evq[EVQ_MAX];
static int evq_n;

/* The saved queue: written whenever the queue's content, order or current entry changes (a hash tells), and once more at
 * exit with the position.  A change is a user action or a track ending, never a frame. */
static uint32_t queue_sig;
static uint32_t queue_signature(void)
{
    uint32_t h = 2166136261u ^ (uint32_t)app.queue_n;
    h = (h ^ (uint32_t)app.queue_cur) * 16777619u;
    for (int i = 0; i < app.queue_n; i++)
        for (const char *p = app.queue[i].id; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h;
}

/* The saved queue and the art cache belong to one server and user: delete both files (the decoded tiles in memory
 * are keyed by album id and age out on their own). */
static void purge_local(void)
{
    unlink(JF_QUEUE_PATH);
    DIR *d = opendir(JF_ART_CACHE_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char p[256];
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", JF_ART_CACHE_DIR, e->d_name) < (int)sizeof p) unlink(p);
    }
    closedir(d);
}

static void queue_persist(int pos_ms)
{
    jf_queue_save(JF_QUEUE_PATH, app.queue, app.queue_n, app.queue_cur, pos_ms);
    queue_sig = queue_signature();
}

static void push(const JfEvent *e)
{
    if (evq_n < EVQ_MAX) evq[evq_n++] = *e;
}

static void push_type(JfEventType t, int played_ms)
{
    JfEvent e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.played_ms = played_ms;
    push(&e);
}

static char art_host[JF_HOST_MAX + 1];     /* the server the cover art is fetched from (set with the API) */
static int art_port;

static void execute(const JfCmds *cmds, uint32_t now)
{
    for (int i = 0; i < cmds->n; i++) {
        const JfCmd *c = &cmds->c[i];
        switch (c->type) {
        case JF_CMD_SET_SERVER:
            jf_api_set_server(&api, c->host, c->port);
            jf_api_set_tls(&api, c->tls);
            snprintf(art_host, sizeof art_host, "%s", c->host);
            art_port = c->port;
            jf_art_set_tls(c->tls);
            break;
        case JF_CMD_SET_AUTH:
            jf_api_set_auth(&api, JF_APP_NAME, app.login.device_id, app.login.token);
            jf_art_set_server(art_host, art_port, JF_APP_NAME, app.login.device_id, app.login.token);
            break;
        case JF_CMD_REQUEST:
            if (c->kind == JF_KIND_AUTH) {
                if (jf_api_enqueue_auth(&api, c->tag, c->user, c->password) < 0) {
                    JfEvent e;
                    memset(&e, 0, sizeof e);
                    e.type = JF_EV_RESULT;
                    e.res.kind = JF_KIND_AUTH; e.res.tag = c->tag; e.res.err = JF_ERR_BADREPLY; e.res.fd = -1;
                    push(&e);
                }
                jf_wipe((void *)c->password, sizeof c->password);
            } else if (c->kind == JF_KIND_WRITE || c->kind == JF_KIND_USER) {
                int rc = c->kind == JF_KIND_WRITE ? jf_api_enqueue_write(&api, c->tag, c->method, c->path)
                                                  : jf_api_enqueue_user(&api, c->tag, c->path);
                if (rc < 0) {
                    JfEvent e;
                    memset(&e, 0, sizeof e);
                    e.type = JF_EV_RESULT;
                    e.res.kind = c->kind; e.res.tag = c->tag; e.res.err = JF_ERR_BADREPLY; e.res.fd = -1;
                    push(&e);
                }
            } else if (c->kind == JF_KIND_SAVE) {
                /* the ids are the queue's first c->n, or the picked ids; the core does not change either until this step ends */
                const char *ids[JF_SAVE_IDS_MAX];
                int n = c->n < JF_SAVE_IDS_MAX ? c->n : JF_SAVE_IDS_MAX;
                for (int k = 0; k < n; k++)
                    ids[k] = c->ids_src == JF_IDS_PICK ? app.pick_ids[k] : app.queue[k].id;
                if (jf_api_enqueue_save(&api, c->tag, c->name, app.login.user_id, ids, n) < 0) {
                    JfEvent e;
                    memset(&e, 0, sizeof e);
                    e.type = JF_EV_RESULT;
                    e.res.kind = JF_KIND_SAVE; e.res.tag = c->tag; e.res.err = JF_ERR_BADREPLY; e.res.fd = -1;
                    push(&e);
                }
            } else if (jf_api_enqueue_list(&api, c->tag, c->path, c->list) < 0) {
                JfEvent e;
                memset(&e, 0, sizeof e);
                e.type = JF_EV_RESULT;
                e.res.kind = JF_KIND_LIST; e.res.tag = c->tag; e.res.err = JF_ERR_BADREPLY;
                e.res.list = c->list; e.res.fd = -1;
                push(&e);
            }
            break;
        case JF_CMD_CANCEL:
            jf_api_cancel_kind(&api, c->kind);
            break;
        case JF_CMD_PLAY_STREAM:
            if (jf_api_enqueue_stream(&api, c->tag, c->path) < 0) {
                JfEvent e;
                memset(&e, 0, sizeof e);
                e.type = JF_EV_RESULT;
                e.res.kind = JF_KIND_STREAM; e.res.tag = c->tag; e.res.err = JF_ERR_BADREPLY; e.res.fd = -1;
                push(&e);
            }
            break;
        case JF_CMD_START_AUDIO:
            if (audio_ok && audio_music_start_fd(&audio, c->fd, c->channels == 2 ? 2 : 1)) {
                audio_music_set_gain(&audio, c->norm_mdb, c->user_mdb);
                track_live = true;
                track_t0 = now;
                push_type(JF_EV_STREAM_STARTED, 0);
            } else {
                http_stream_close(c->fd);
                push_type(JF_EV_TRACK_ENDED, 0);
            }
            break;
        case JF_CMD_CLOSE_FD:
            http_stream_close(c->fd);
            break;
        case JF_CMD_STOP:
            track_live = false;
            if (audio_ok) audio_music_stop(&audio);
            break;
        case JF_CMD_SET_GAIN:
            if (audio_ok) audio_music_set_gain(&audio, c->norm_mdb, c->user_mdb);
            break;
        case JF_CMD_SAVE_LOGIN:
            jf_login_save(&app.login, JF_CONF_PATH);
            jf_settings_save(&app.login, JF_SETTINGS_PATH);
            break;
        case JF_CMD_CLEAR_LOGIN:
            jf_login_clear(&app.login, JF_CONF_PATH);
            break;
        case JF_CMD_PURGE_LOCAL:
            purge_local();
            break;
        case JF_CMD_EXIT:
            running = 0;
            break;
        default:
            break;
        }
    }
}

static void step_event(const JfEvent *ev, uint32_t now)
{
    static JfCmds cmds;
    JfEvent e2 = *ev;
    if (e2.type == JF_EV_PAUSE || e2.type == JF_EV_PREV) e2.played_ms = jf_position_ms(&app, track_live ? (int)(now - track_t0) : 0);
    jf_step(&app, &e2, &cmds);
    execute(&cmds, now);
    if (queue_signature() != queue_sig) queue_persist(0);
    jf_ui_mark_dirty(&ui);
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

    /* A deeper cushion than the games' ~139 ms against UI frames that stall the
     * loop: 350 ms asks for 8 periods of 2048, which the ring cap (half of 16
     * periods) grants exactly at 44100 Hz (~372 ms).  The price is that pause,
     * seek, volume and EQ are heard that much later: queued audio plays out. */
    audio_out_set_lead_ms(350);
    audio_ok = audio_init(&audio) == 0;
    audio_set_fd_io(&audio, &stream_io);   /* https streams read through the TLS layer */
    if (!audio_ok) fprintf(stderr, JF_APP_BIN ": no audio output\n");

    jf_app_init(&app);
    jf_seed(&app, (uint32_t)time(NULL) * 2654435761u);
    {   /* the saved queue comes back paused at its track: nothing plays until PLAY */
        static JfItem saved[JF_QUEUE_MAX];
        int cur = 0, pos = 0;
        int n = jf_queue_load(JF_QUEUE_PATH, saved, JF_QUEUE_MAX, &cur, &pos);
        if (n > 0) jf_queue_restore(&app, saved, n, cur, pos);
        queue_sig = queue_signature();
    }
    app.sink_channels_fn = sink_channels;
    if (jf_api_init(&api) != 0) {
        fprintf(stderr, JF_APP_BIN ": out of memory\n");
        fb_close(&fb);
        return 1;
    }
    logger_init(&logger, JF_APP_BIN, LOG_LEVEL_INFO, false);
    api.log = &logger;
    jf_ui_init(&ui, &fb, &touch, &gp, &app);
    ui.eq_gains_fn = eq_now;
    ui.enh_now_fn = enh_now;
    jf_art_init(JF_ART_CACHE_DIR, JF_UI_ART);

    {
        JfEvent *e = &evq[0];
        memset(e, 0, sizeof *e);
        e->type = JF_EV_START;
        jf_login_load(&e->login, JF_CONF_PATH);
        jf_settings_load(&e->login, JF_SETTINGS_PATH);
        evq_n = 1;
    }

    unsigned art_gen = jf_art_generation();
    InputState in;
    memset(&in, 0, sizeof in);
    bool first = true;

    while (running) {
        uint32_t now = get_time_ms();

        if (audio_ok) audio_pump(&audio);

        touch_poll(&touch);
        TouchState ts = touch_get_state(&touch);
        gamepad_poll(&gp, &in, 0, 0, false);
        gamepad_tick(&gp, now);
        bool navkey = false;
        for (int i = 0; i < BTN_ID_COUNT; i++) if (in.buttons[i].pressed) navkey = true;
        pointer_update(&ptr, &in, ts.pressed, navkey, &ts);

        static JfEvent uev[4];
        int un = first ? 0 : jf_ui_input(&ui, &ts, &in, &ptr, now, uev, 4);
        first = false;
        {   /* the list view as the finger left it, before this frame's events can open something */
            int vt, vc;
            if (jf_ui_view(&ui, &vt, &vc)) jf_note_view(&app, vt, vc);
        }
        for (int i = 0; i < un; i++) push(&uev[i]);
        {   /* the equaliser panel: every edit is heard at once, DONE also saves */
            int eg[EQ_BANDS];
            bool esave;
            if (jf_ui_eq_take(&ui, eg, &esave)) {
                if (audio_ok) audio_set_eq(&audio, eg);
                if (audio_ok) audio_set_enh(&audio, jf_ui_enh_out(&ui));
                if (esave) eq_panel_store_file_both(CONFIG_FILE_PATH, eg, jf_ui_enh_out(&ui));
            }
        }

        static JfResult res;
        jf_art_poll(now);
        if (jf_art_generation() != art_gen) { art_gen = jf_art_generation(); jf_ui_mark_dirty(&ui); }
        while (jf_api_poll(&api, &res)) {
            JfEvent e;
            memset(&e, 0, sizeof e);
            e.type = JF_EV_RESULT;
            e.res = res;
            push(&e);
            memset(&res, 0, sizeof res);
        }

        if (track_live && audio_ok && !audio_music_active(&audio)) {
            track_live = false;
            push_type(JF_EV_TRACK_ENDED, (int)(now - track_t0));
        }
        jf_ui_set_played(&ui, jf_position_ms(&app, track_live ? (int)(now - track_t0) : 0));

        for (int i = 0; i < evq_n; i++) {
            JfEvent *e = &evq[i];
            JfEvent copy = *e;
            step_event(&copy, now);      /* may push more; evq_n grows */
            jf_wipe(&copy.password, sizeof copy.password);
            jf_wipe(&e->password, sizeof e->password);
        }
        evq_n = 0;

        bool redraw = jf_ui_take_dirty(&ui);
        if (redraw) {
            jf_ui_draw(&ui);
            fb_swap(&fb);
            pointer_invalidate(&ptr);
            pointer_paint(&ptr, &fb);
        }
        UiRect info_r;
        bool info_up = !redraw && jf_ui_take_info(&ui, &info_r);
        if (info_up) {   /* the bar's text alone, already presented: put the cursor back over it */
            pointer_invalidate(&ptr);
            pointer_paint(&ptr, &fb);
        }
        bool ptr_moved = !redraw && !info_up && pointer_paint(&ptr, &fb);
        fb_overtemp_poll(&fb);
        bool active = redraw || ptr_moved || app.busy || jf_art_busy() || lv_needs_frames(&ui.lv) || (audio_ok && audio_pump_active(&audio));
        usleep(active ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
    }

    if (app.queue_n > 0)   /* the position the track was left at, so a restart resumes there */
        queue_persist(jf_position_ms(&app, track_live ? (int)(get_time_ms() - track_t0) : 0));
    if (audio_ok) {
        audio_music_stop(&audio);
        for (int i = 0; i < 20; i++) { audio_pump(&audio); usleep(16667); }
        audio_close(&audio);
    }
    jf_art_shutdown();
    jf_api_free(&api);
    logger_close(&logger);
    jf_ui_free(&ui);
    jf_login_wipe(&app.login);
    hw_leds_off();
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    gamepad_close(&gp);
    touch_close(&touch);
    fb_close(&fb);
    return 0;
}
