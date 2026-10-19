/*
 * jf_core.h — the Jellyfin client's state machine.  PURE: no socket, no file,
 * no clock, no framebuffer, no jf_api call.  jf_step() takes one event, changes
 * the JfApp, and writes up to JF_CMDS_MAX commands that the main loop executes.
 * The only outside input is the session-id source (JfApp.session_fn), so a test
 * can make it deterministic.
 *
 * How the main loop drives it:
 *   1. jf_app_init() once (JfApp is ~0.8 MB: make it static), then jf_login_load()
 *      into a JfLogin and send JF_EV_START with it.
 *   2. Every frame: turn input into user-intent events, jf_api_poll() results into
 *      JF_EV_RESULT, audio facts into JF_EV_STREAM_STARTED / JF_EV_TRACK_ENDED.
 *   3. jf_step() each event; execute the JfCmds in order (see JfCmdType).
 *   4. Draw from the JfApp fields below; nothing else is needed.
 *   5. On JF_CMD_EXIT leave the loop (the core has already emitted STOP).
 *
 * Rules the loop must keep:
 *   - Never send JF_EV_TRACK_ENDED for a stop the core commanded (STOP).
 *   - JF_CMD_REQUEST's password is wiped by the loop right after jf_api_enqueue_auth.
 *   - A list that a REQUEST targets is JfApp-owned: hand the cmd's `list` to jf_api
 *     untouched; the core already cleared it when the request is a first page.
 */

#ifndef ARCA_JF_CORE_H
#define ARCA_JF_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include "jf_model.h"
#include "jf_login.h"
#include "jf_api.h"

/* Page sizes (items per request) and storage caps per list.  The artists cap
 * covers the 1505 album artists and the 1120 genres of the test server (the root list
 * also holds albums, playlists and songs: of 39280 songs it keeps the first 1600); past a cap the list is `full`
 * and paging stops (search is the way to reach the rest). */
#define JF_PAGE_ARTISTS   50
#define JF_PAGE_ALBUMS    30
#define JF_PAGE_TRACKS    25
#define JF_PAGE_SEARCH    20
#define JF_CAP_ARTISTS  1600
#define JF_CAP_ALBUMS    300
#define JF_CAP_TRACKS    300
#define JF_CAP_SEARCH    200
#define JF_CAP_MIX       200      /* an instant mix: JF_MIX_LIMIT items fit */
#define JF_QUEUE_MAX     200
#define JF_NAV_MAX        16
#define JF_CMDS_MAX        4
#define JF_PATH_MAX      512
#define JF_FAIL_LIMIT      3      /* consecutive failed tracks that stop playback */
#define JF_SHORT_MS     5000      /* a track that played less than this failed */

typedef enum { JF_SCR_SIGNIN = 0, JF_SCR_ARTISTS, JF_SCR_ALBUMS, JF_SCR_TRACKS,
               JF_SCR_SEARCH, JF_SCR_NOWPLAYING, JF_SCR_QUEUE } JfScreen;

typedef enum { JF_LIST_ARTISTS = 0, JF_LIST_ALBUMS, JF_LIST_TRACKS, JF_LIST_SEARCH,
               JF_LIST_MIX,       /* no screen of its own: the reply of an INSTANT MIX request */
               JF_LIST_COUNT } JfListId;

/* The root browse category (JF_SCR_ARTISTS shows it), in the order JF_EV_CATEGORY cycles. */
typedef enum { JF_CAT_ALBUMS = 0, JF_CAT_ARTISTS, JF_CAT_PLAYLISTS, JF_CAT_GENRES, JF_CAT_SONGS, JF_CAT_COUNT } JfCat;

/* What an ALBUMS or TRACKS list is the child of: ALBUMS of an artist (plain) or of a genre,
 * TRACKS of an album (plain) or of a playlist. */
typedef enum { JF_SUB_PLAIN = 0, JF_SUB_GENRE, JF_SUB_PLAYLIST } JfSub;

typedef enum { JF_PLAY_IDLE = 0, JF_PLAY_FETCHING, JF_PLAY_PLAYING, JF_PLAY_END } JfPlayState;

/* Buttons of the error overlay (JfApp.overlay_buttons). */
#define JF_BTN_RETRY  1u
#define JF_BTN_CLOSE  2u

typedef enum {
    JF_EV_NONE = 0,
    /* user intents */
    JF_EV_START,         /* .login: the loaded login; once, first */
    JF_EV_SIGNIN,        /* .server .user .password */
    JF_EV_ACTIVATE,      /* .i: row index in app->list->items; a track row PLAYS from there on (the action bar's PLAY) */
    JF_EV_SELECT,        /* .i: a tap / ENTER on a list row.  A track row raises the ACTION BAR on it (the same row
                          * again drops it, another track row moves it); any other row opens as ACTIVATE does */
    JF_EV_PLAY_NEXT,     /* .i: list row (>= 0, a track); inserts after the queue position, or plays it when nothing plays */
    JF_EV_ADD_QUEUE,     /* .i: list row; appends, or plays it when nothing plays */
    JF_EV_MIX,           /* .i: list row, or -1 = the playing track.  Placeholder: the core only shows "MIX: COMING" */
    JF_EV_INSTANT_MIX,   /* .i: list row, or -1 = the playing track; the mix replaces the queue and plays */
    JF_EV_SAVE_PLAYLIST, /* .term: the playlist name (the UI never sends an empty one); saves the queue's first JF_SAVE_IDS_MAX */
    JF_EV_DISMISS,       /* drop the action bar */
    JF_EV_BACK,
    JF_EV_PLAY_ALL,
    JF_EV_NEXT,
    JF_EV_VOL_UP,
    JF_EV_VOL_DOWN,
    JF_EV_SEARCH,        /* .term; empty = cancel any search in flight */
    JF_EV_OPEN_NOWPLAYING,
    JF_EV_OPEN_QUEUE,    /* only from NOWPLAYING */
    JF_EV_QUEUE_JUMP,    /* .i: queue index; only on the QUEUE screen */
    JF_EV_RETRY,         /* overlay button */
    JF_EV_CLOSE_ERROR,   /* overlay button */
    JF_EV_NEED_MORE,     /* the list cursor is near the end of the loaded rows */
    JF_EV_CATEGORY,      /* .i: +1 next, -1 previous root category; only at the root (ARTISTS screen, nav empty) */
    JF_EV_QUIT,          /* the X / EXIT button */
    /* results and audio facts */
    JF_EV_RESULT,        /* .res: a copy of the JfResult from jf_api_poll */
    JF_EV_STREAM_STARTED,/* audio_music_start_fd succeeded */
    JF_EV_TRACK_ENDED    /* .played_ms: how long the track actually played; also
                          * sent when audio_music_start_fd failed (played_ms 0) */
} JfEventType;

typedef struct {
    JfEventType type;
    int i;
    int played_ms;
    char server[64];     /* as typed: "host", "host:port", "http://host:port/" */
    char user[64];
    char password[64];
    char term[96];
    JfResult res;
    JfLogin login;
} JfEvent;

typedef enum {
    JF_CMD_NONE = 0,
    JF_CMD_SET_SERVER,   /* jf_api_set_server(.host, .port) */
    JF_CMD_SET_AUTH,     /* jf_api_set_auth(device, app->login.device_id, app->login.token) */
    JF_CMD_REQUEST,      /* .kind AUTH: jf_api_enqueue_auth(.tag, .user, .password);
                          * .kind LIST: jf_api_enqueue_list(.tag, .path, .list)  (also the INSTANT MIX request)
                          * .kind SAVE: jf_api_enqueue_save(.tag, .name, app->login.user_id, ids, .n) with ids = the
                          *   ids of app->queue[0..n); POST, never retried (no app->retry) */
    JF_CMD_CANCEL,       /* jf_api_cancel_kind(.kind) */
    JF_CMD_PLAY_STREAM,  /* jf_api_enqueue_stream(.tag, .path); .norm_mdb/.user_mdb repeat the gain */
    JF_CMD_START_AUDIO,  /* audio_music_start_fd(.fd), then audio_music_set_gain(.norm_mdb, .user_mdb);
                          * then send JF_EV_STREAM_STARTED (or TRACK_ENDED played_ms 0 on failure) */
    JF_CMD_CLOSE_FD,     /* close(.fd): a stream result the core does not want */
    JF_CMD_STOP,         /* stop the music voice */
    JF_CMD_SET_GAIN,     /* audio_music_set_gain(.norm_mdb, .user_mdb) */
    JF_CMD_SAVE_LOGIN,   /* jf_login_save(&app->login) */
    JF_CMD_CLEAR_LOGIN,  /* jf_login_clear(&app->login); the core already emptied the token */
    JF_CMD_EXIT
} JfCmdType;

typedef struct {
    JfCmdType type;
    JfKind kind;
    unsigned tag;
    char path[JF_PATH_MAX];
    JfList *list;
    char user[64];
    char password[64];
    char host[64];
    int port;
    int fd;
    int norm_mdb;
    int user_mdb;
    char name[96];       /* SAVE: the playlist name */
    int n;               /* SAVE: how many queue ids */
} JfCmd;

typedef struct {
    JfCmd c[JF_CMDS_MAX];
    int n;
    bool overflow;       /* a step wanted more than JF_CMDS_MAX (a bug; tests assert false) */
} JfCmds;

typedef bool (*JfSessionFn)(char out[33]);

typedef struct { JfScreen screen; char id[96]; char title[96]; int sub; } JfNavEntry;
typedef struct { char id[96]; char title[96]; int sub; } JfListCtx;

typedef struct {
    /* ── what the UI reads ─────────────────────────────────────────────── */
    JfScreen screen;
    JfCat cat;               /* the root category; the list on the ARTISTS screen holds it (JF_LIST_ARTISTS slot) */
    JfList *list;            /* the list to draw on ARTISTS/ALBUMS/TRACKS/SEARCH; NULL otherwise.
                              * Rows are list->items[0..n); n < total means more pages exist. */
    char crumb[160];         /* title text for the top bar */
    bool overlay;            /* error overlay up: modal, draw overlay_text and the buttons */
    char overlay_text[48];
    unsigned overlay_buttons;/* JF_BTN_* mask */
    char banner[48];         /* one-line notice ("" = none); stays until the next user intent */
    bool action_bar;         /* the ACTION BAR is up on list row `action_row` (a track): the UI draws it in the button row
                              * and owns focus there.  Dropped by any other user intent, a screen change or a re-aim */
    int action_row;
    char queue_name[96];     /* the name SAVE AS PLAYLIST offers: set by whatever built the queue ("Queue: <first>", "Mix: <seed>") */
    unsigned banner_seq;     /* bumps on every new banner, so the UI can time it */
    bool busy;               /* any request in flight or being waited for: show a spinner */
    JfPlayState play_state;  /* IDLE; FETCHING (stream requested); PLAYING; END = "END OF QUEUE" */
    JfItem queue[JF_QUEUE_MAX];
    int queue_n;
    int queue_cur;           /* index of the track playing / last played; valid when queue_n > 0 */
    int volume_step;         /* 0..JF_VOLUME_MAX; 0 = mute (also in .login.volume_step) */
    JfLogin login;           /* server and user are kept on 401; token is emptied */

    /* ── private to jf_core.c ──────────────────────────────────────────── */
    JfList lists[JF_LIST_COUNT];
    JfListCtx ctx[JF_LIST_COUNT];
    int req_loaded[JF_LIST_COUNT];
    JfNavEntry nav[JF_NAV_MAX];
    int nav_n;
    JfCmd retry;
    unsigned auth_tag, stream_tag, save_tag;
    bool auth_pending, save_pending;
    char save_name[96];
    bool play_wait;
    int play_wait_from;
    int fail_streak;
    JfSessionFn session_fn;
    JfItem store_artists[JF_CAP_ARTISTS];
    JfItem store_albums[JF_CAP_ALBUMS];
    JfItem store_tracks[JF_CAP_TRACKS];
    JfItem store_search[JF_CAP_SEARCH];
    JfItem store_mix[JF_CAP_MIX];
} JfApp;

/** Zero the app, wire the list storage, session_fn = jf_session_id, volume = default. */
void jf_app_init(JfApp *app);

/** Consume one event; `out` holds the commands to execute, in order (n = 0 if none). */
void jf_step(JfApp *app, const JfEvent *ev, JfCmds *out);

/** user_mdb for a volume step: 0 -> -100000 (mute, whatever the track's norm), else (step-10)*3000. */
int jf_volume_mdb(int step);

/** The queue entry playing (or last played); NULL when the queue is empty. */
const JfItem *jf_now(const JfApp *app);

#endif
