/*
 * jv_core.h — the video client's state machine.  UI-free and socket-free: jv_step() takes one event and the
 * current time, changes the JvCore, and writes up to JV_CMDS_MAX commands that the main loop executes (jf_api
 * calls, player calls).  The only outside inputs are the time argument, the session-id source and the two login
 * files named in jv_core_init(); the only files it touches are those (jf_login_load / jf_login_save).
 *
 * How the main loop drives it:
 *   1. jv_core_init() once (JvCore is ~0.2 MB: make it static), fill `set` from jv_settings_load(), then send
 *      JV_EV_START.  The loop sets `channels`, `rate` and `max_w` (what the audio sink and the picture can take)
 *      before any JV_EV_PLAY.
 *   2. Every frame: user intents -> events, jf_api_poll() results (either JfApi) -> JV_EV_RESULT, the player's
 *      facts -> JV_EV_POS / JV_EV_PLAYER_ENDED, and one JV_EV_TICK; the time is the monotonic ms of that frame.
 *   3. Execute the commands in order.  Draw from the fields below.
 *   4. On JV_CMD_EXIT the core has already emitted the stop reports: pump jf_api for up to 2 s so they leave, then end.
 *
 * Rules: reports (started / progress / stopped) are POSTs and the core never sends one twice; a failed one only
 * counts in `report_fail`; a failed encoding-job DELETE is sent again (JvDel).  Every request carries a tag the core numbers; a result whose tag is not the one the core
 * waits for is dropped (the user has left that view).  The loop wipes a JV_CMD_REQUEST AUTH password right after
 * jf_api_enqueue_auth.
 */

#ifndef ARCA_JV_CORE_H
#define ARCA_JV_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include "jf_api.h"
#include "jf_login.h"
#include "jv_model.h"
#include "jv_settings.h"
#include "jv_tracks.h"
#include "jv_url.h"

#define JV_PAGE            30
#define JV_PAGE_SMALL      24      /* resume, next up */
#define JV_PAGE_SEARCH     40
#define JV_PAGE_PEOPLE     10
#define JV_PAGE_SIMILAR    12
#define JV_CAP_LIST       300
#define JV_CAP_PROBE       24
#define JV_CAP_LIBS        16
#define JV_NAV_MAX         12
#define JV_CMDS_MAX        10
#define JV_PROGRESS_MS  10000      /* progress report cadence while playing */
#define JV_SEEK_QUIET_MS  700      /* a seek burst restarts the stream once this long after its last press */
#define JV_SEEK_LADDER_MS { 10000, 30000, 60000, 300000 }   /* JV_EV_SEEK_STEP: the step of the 1st, 2nd, 3rd, 4th-and-later press of a burst */
#define JV_BODY_MAX       640
#define JV_DEL_N            4      /* encoding-job DELETEs the core keeps track of (retry, second send) */
#define JV_DEL_RETRY_MS   500      /* a failed DELETE is sent again this long after the failure, twice at most */
#define JV_DEL_TWICE_MS  1500      /* a DELETE for a stream that was still opening is sent a second time this long after the first */

/* jf_api groups: a new PARSE of a group replaces any queued or in-flight one of it */
enum { JV_GRP_LIST = 1, JV_GRP_DETAIL = 2, JV_GRP_HOME = 3 };

typedef enum { JV_SCR_SIGNIN = 0, JV_SCR_HOME, JV_SCR_LIST, JV_SCR_SERIES, JV_SCR_SEASON, JV_SCR_DETAILS,
               JV_SCR_CAST, JV_SCR_PLAYER } JvScreen;

/* what a list screen shows (one request kind each) */
typedef enum { JV_K_NONE = 0, JV_K_RESUME, JV_K_NEXTUP, JV_K_MOVIES, JV_K_SHOWS, JV_K_SEASONS, JV_K_EPISODES,
               JV_K_SEARCH, JV_K_PERSON, JV_K_GENRE, JV_K_SIMILAR } JvKind;

typedef enum { JV_TILE_RESUME = 0, JV_TILE_NEXTUP, JV_TILE_MOVIES, JV_TILE_SHOWS } JvTile;

/* IDLE: nothing plays.  FETCHING: the stream is requested.  PLAYING / PAUSED: the player has the fd. */
typedef enum { JV_PLAY_IDLE = 0, JV_PLAY_FETCHING, JV_PLAY_PLAYING, JV_PLAY_PAUSED } JvPlayState;

typedef enum {
    JV_EV_NONE = 0,
    JV_EV_START,          /* once, first: loads the own login file */
    JV_EV_REUSE_LOGIN,    /* SIGN IN screen: prefill server and user from the other app's file (when reuse_available) */
    JV_EV_SIGNIN,         /* .server .user .password */
    JV_EV_HOME,           /* .i: a JvTile */
    JV_EV_ACTIVATE,       /* .i: row of `list` (movie / episode -> details, series -> seasons, season -> episodes, person / genre -> results) */
    JV_EV_BACK,           /* pops; in the player it is STOP */
    JV_EV_NEED_MORE,      /* the cursor is near the end of the loaded rows: next page of a paged list */
    JV_EV_SEARCH,         /* .term; empty cancels */
    JV_EV_SIMILAR,        /* details: similar items */
    JV_EV_CAST_OPEN,      /* details: the people screen (no request) */
    JV_EV_CAST_ACTIVATE,  /* .i: index into detail.people: that person's items */
    JV_EV_AUDIO_SET,      /* .i: position in detail.audio.  Details: only the choice.  Player: restart at the current position, new session id */
    JV_EV_AUDIO_NEXT,
    JV_EV_SUB_SET,        /* .i: position in detail.subs or -1 = off.  Details: the choice.  Player: only the SRT is reloaded */
    JV_EV_SUB_NEXT,
    JV_EV_PLAY,           /* details: .i = start position in ms (0 = from the start; the UI passes the resume position) */
    JV_EV_PAUSE,          /* player: toggle */
    JV_EV_SEEK_BY,        /* player: .i = delta ms; bursts collapse into one restart after JV_SEEK_QUIET_MS */
    JV_EV_SEEK_STEP,      /* player: .i = +1 / -1; each press of a burst steps further: 10 s, 30 s, 1 min, 5 min (cap) */
    JV_EV_SEEK_TO,        /* player: .i = absolute ms */
    JV_EV_STOP,
    JV_EV_QUIT,
    JV_EV_RETRY, JV_EV_CLOSE_ERROR,
    JV_EV_SETTINGS,       /* `set` was changed by the UI: nothing to do but a hook for the loop; picks up the preferences at the next detail */
    /* facts */
    JV_EV_RESULT,         /* .res from jf_api_poll */
    JV_EV_POS,            /* .i: the media position now, ms; the loop sends it at least once a second while playing */
    JV_EV_PLAYER_ENDED,   /* .i: 1 = end of stream, 0 = the decoder failed */
    JV_EV_TICK
} JvEventType;

typedef struct {
    JvEventType type;
    int i;
    char server[64], user[64], password[64], term[96];
    JfResult res;
} JvEvent;

typedef enum {
    JV_CMD_NONE = 0,
    JV_CMD_SET_SERVER,    /* jf_api_set_server(.host, .port), jf_api_set_tls(.tls) on both instances */
    JV_CMD_SET_AUTH,      /* jf_api_set_auth(client, device, core->login.device_id, core->login.token) on both instances */
    JV_CMD_REQUEST,       /* on instance .api (0 metadata and subtitles, 1 stream, reports, DELETEs): .kind AUTH -> jf_api_enqueue_auth(.tag, .user, .password);
                           * PARSE -> jf_api_enqueue_parse(.tag, .path, .fn, .dst, .group);
                           * POST -> jf_api_enqueue_post_json(.tag, .path, .body, .n);
                           * WRITE -> jf_api_enqueue_write(.tag, .method, .path);
                           * STREAM -> jf_api_enqueue_stream(.tag, .path) */
    JV_CMD_CANCEL,        /* jf_api_cancel_kind(instance .api, .kind) */
    JV_CMD_PLAYER_START,  /* hand .fd to the player with core->play (a fresh decoder; the old one is already stopped) */
    JV_CMD_PLAYER_STOP,   /* stop and join the decoder, flush, release the voice */
    JV_CMD_PLAYER_PAUSE, JV_CMD_PLAYER_RESUME,
    JV_CMD_SUB_FEED,      /* hand .fd (the SRT stream) to the player's subtitle reader */
    JV_CMD_SUB_CLEAR,     /* drop the subtitle cues and the sprite */
    JV_CMD_CLOSE_FD,      /* close(.fd): a stream result the core does not want */
    JV_CMD_EXIT
} JvCmdType;

typedef struct {
    JvCmdType type;
    JfKind kind;
    unsigned tag;
    int api;
    int group;
    char path[JF_API_PATH_MAX];
    char method[8];
    char body[JV_BODY_MAX];
    int n;                /* POST: body length */
    JfParseFn fn;
    void *dst;
    char user[64], password[64];
    char host[64];
    int port;
    bool tls;
    int fd;
} JvCmd;

typedef struct { JvCmd c[JV_CMDS_MAX]; int n; bool overflow; } JvCmds;

/** What the player needs for the stream it was just handed.  `generation` bumps on every (re)start. */
typedef struct {
    char item_id[33], media_source_id[33], session_id[33];
    int audio_index;          /**< server stream index */
    int sub_index;            /**< server stream index, -1 = off */
    long start_ms;            /**< media time of the first picture: the stream's pts restart near zero */
    long runtime_ms;
    int channels, rate, max_w;
    unsigned generation;
} JvPlayReq;

typedef bool (*JvSessionFn)(char out[33]);

/** One encoding-job DELETE: `tag` is the request in flight (0 = none), `due_ms` the next send (0 = none), `left` retries after a failure. */
typedef struct { char sid[33]; unsigned tag; long long due_ms; int left; } JvDel;

typedef struct { JvScreen screen; JvKind kind; char id[33], id2[33], title[96]; int top, cursor; } JvNavEntry;

typedef struct {
    /* ── what the UI reads ─────────────────────────────────────────────── */
    JvScreen screen;
    JvKind kind;                     /* LIST / SERIES / SEASON: what `list` holds */
    JvList *list;                    /* the rows on a list screen; NULL otherwise */
    char crumb[96];
    bool overlay; char overlay_text[48]; unsigned overlay_buttons;   /* JV_BTN_* */
    char banner[48];
    bool busy;
    JvDetail detail;                 /* DETAILS / CAST / PLAYER */
    bool detail_ready;
    int sel_audio, sel_sub;          /* positions in detail.audio / .subs, sub -1 = off: the choice the next PLAY uses */
    int search_sec[2];               /* the search list: first row of PEOPLE and of GENRES (items come first); -1 = none */
    int restore_top, restore_cursor; unsigned restore_seq;           /* after a pop: where the list view goes back to */
    char signin_server[64], signin_user[64];   /* the sign-in fields' prefill */
    bool reuse_available;            /* the other app's file holds a server and a user */
    bool focus_password;             /* REUSE_LOGIN just prefilled: focus goes to the password field */
    bool home_ready;
    int home_resume_n, home_next_n;  /* CONTINUE / NEXT UP tiles show only when > 0 */
    bool home_movies, home_shows;
    JvPlayState play_state;
    JvPlayReq play;
    long pos_ms;                     /* last position the player reported (or the restart target) */
    bool seek_pending; long seek_target_ms;   /* a burst waiting for its quiet period: show seek_target_ms */
    int seek_dir, seek_rung;         /* JV_EV_SEEK_STEP: direction of the burst and its place on JV_SEEK_LADDER_MS */
    int report_fail;                 /* reports, and DELETEs that stayed failed after their retries */
    JvSettings set;                  /* the UI edits it, then sends JV_EV_SETTINGS */
    JfLogin login;
    int channels, rate, max_w;       /* the loop's sink facts; defaults 1, 44100, JV_STREAM_MAX_W */
    JvSessionFn session_fn;          /* NULL = jf_session_id */

    /* ── private ───────────────────────────────────────────────────────── */
    char conf_path[160], reuse_path[160], id_suffix[8];
    char reuse_server[64], reuse_user[64];
    JvNavEntry nav[JV_NAV_MAX]; int nav_n;
    JvList lists[3];                 /* 0 main, 1 resume probe, 2 next-up probe */
    JvList libs;
    JvItem store_main[JV_CAP_LIST], store_resume[JV_CAP_PROBE], store_next[JV_CAP_PROBE], store_libs[JV_CAP_LIBS];
    JvDetail detail_in;
    unsigned req_tag, list_tag, detail_tag, home_tag, auth_tag, stream_tag, sub_tag;
    int home_phase;                  /* 0 idle, 1 views, 2 resume probe, 3 next-up probe */
    bool auth_pending, list_pending_first, detail_refresh, started_sent, restarted_once;
    bool restart_paused;             /* the stream being opened replaces a paused one: it is paused again when it arrives */
    JvDel del[JV_DEL_N]; int del_next;
    JvCmd retry;
    char cur_id[33], cur_id2[33], cur_title[96];
    int search_phase;                /* 0 none, 1 items, 2 people, 3 genres */
    char search_term[96];
    long long next_report_ms, seek_due_ms;
    long long now_ms;
    int view_top, view_cursor;       /* as last reported by jv_note_view() */
} JvCore;

#define JV_BTN_RETRY 1u
#define JV_BTN_CLOSE 2u

/** Paths are the caller's: `conf_path` is this app's login file, `reuse_path` the other app's (read for server and user only), `id_suffix` the DeviceId suffix ("-v"). */
void jv_core_init(JvCore *c, const char *conf_path, const char *reuse_path, const char *id_suffix);
void jv_step(JvCore *c, const JvEvent *ev, long long now_ms, JvCmds *out);
/** The position to draw: the pending seek target, else the last reported one. */
long jv_core_position(const JvCore *c);
/** The list view of the screen showing (scroll top and cursor row), so a push can remember where to come back to. */
void jv_note_view(JvCore *c, int top, int cursor);
/** True when the PLAYER screen is up. */
bool jv_core_playing(const JvCore *c);

#endif /* ARCA_JV_CORE_H */
