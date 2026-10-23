/*
 * jf_core.h — the Ludfin client's state machine.  PURE: no socket, no file,
 * no clock, no framebuffer, no jf_api call.  jf_step() takes one event, changes
 * the JfApp, and writes up to JF_CMDS_MAX commands that the main loop executes.
 * The only outside input is the session-id source (JfApp.session_fn), so a test
 * can make it deterministic.
 *
 * How the main loop drives it:
 *   1. jf_app_init() once (JfApp is ~2.4 MB (sizeof, measured with the artist and inline-search stores; ~2.0 MB before them): make it static), then jf_login_load() and jf_settings_load()
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
#include <stdint.h>
#include "jf_model.h"
#include "jf_login.h"
#include "jf_api.h"
#include "jf_url.h"

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
#define JF_CAP_SEARCH    400      /* 200 hits plus one inlined artist (JF_CAP_ARTINL fits the splice) */
#define JF_CAP_MIX       200      /* an instant mix: JF_MIX_LIMIT items fit */
#define JF_CAP_SEED        2
#define JF_CAP_GENRES    100      /* JF_MIX_GENRE_PAGE */
#define JF_CAP_POOL       40      /* JF_MIX_PAGE */
#define JF_CAP_LIBS       16      /* music libraries of one server */
#define JF_CAP_PLPICK    100      /* the playlists the add-to-playlist picker lists */
#define JF_CAP_PLCHK      50      /* one page of a playlist's track ids in the duplicate scan (the scan clears it between pages) */
#define JF_PAGE_PLPICK    50
#define JF_PAGE_PLCHK     50
#define JF_PAGE_ARTALL    40      /* PLAY ALL on an artist folder: JF_CAP_ARTALL = JF_QUEUE_MAX tracks in 5 pages */
#define JF_PAGE_ARTINL    40
#define JF_CAP_ARTALL    200
#define JF_CAP_ARTINL    300      /* an artist inlined in a search: albums, then the tracks the filter keeps */
#define JF_ART_PAGES_MAX  20      /* tracks pages one artist folder asks for: a prolific artist whose tracks all sit on filtered albums must end */
#define JF_DUP_PAGES_MAX  40      /* pages the duplicate scan reads (2000 entries) before it gives up and asks */
#define JF_ADD_BATCH      10      /* ids per add request: they ride in the query and 10 fit the path */

/* MIX.  A page of 50 Audio items with Fields=Genres is ~66 KB and ~4700 tokens (measured), too near the old 5000-token
 * budget, so 40 (measured up to 5259 tokens and 83670 bytes: JF_TOKENS_PAGE is 8000 and JF_API_BODY_BYTES 160 KB for it); a genre-index page of 100 with SongCount is ~38 KB and ~3700 tokens. */
#define JF_MIX_OPT_MAX    20      /* tick rows: the genres of one track + artist + decade (JF_GENRES_BYTES holds < 20 names) */
#define JF_MIX_VARIANTS    4      /* spellings of one genre key whose ids are kept (the path holds 4 x 32 hex) */
#define JF_MIX_WANT      100      /* matches a mix tries for */
#define JF_MIX_PAGE       40
#define JF_MIX_GENRE_PAGE 100
#define JF_MIX_REQ_MAX     8      /* pool requests, then take what is found */
#define JF_MIX_SMALL     120      /* a server total up to this is paged in a stable order and shuffled here: random pages would repeat */
#define JF_QUEUE_MAX     200
#define JF_NAV_MAX        16
#define JF_CMDS_MAX        5
#define JF_PATH_MAX      512
#define JF_FAIL_LIMIT      3      /* consecutive failed tracks that stop playback */
#define JF_SHORT_MS     5000      /* a track that played less than this failed */

typedef enum { JF_SCR_SIGNIN = 0, JF_SCR_ARTISTS, JF_SCR_ALBUMS, JF_SCR_TRACKS,
               JF_SCR_SEARCH, JF_SCR_NOWPLAYING, JF_SCR_QUEUE,
               JF_SCR_MIX,      /* the tick screen of MIX: JfApp.mix.opt */
               JF_SCR_SETTINGS, /* the settings view: rows from jf_settings_rows(), JfApp.settings_page */
               JF_SCR_PLPICK    /* ADD TO PLAYLIST: the playlist list (JF_LIST_PLPICK), pick_sel marks the chosen row */ } JfScreen;

typedef enum { JF_LIST_ARTISTS = 0, JF_LIST_ALBUMS, JF_LIST_TRACKS, JF_LIST_SEARCH,
               JF_LIST_MIX,       /* no screen of its own: the reply of an INSTANT MIX request */
               JF_LIST_SEED,      /* no screen: the one-row reply that gives MIX its seed's genres, year and artist */
               JF_LIST_GENRES,    /* no screen: one page of the genre index, folded into the ticked keys and cleared */
               JF_LIST_POOL,      /* no screen: one page of candidate tracks for a MIX */
               JF_LIST_LIBS,      /* no screen of its own: the music libraries (Views reply), items are JF_ITEM_LIBRARY; JfApp.lib_n = items stored */
               JF_LIST_PLPICK,    /* the playlists of the ADD TO PLAYLIST picker (screen JF_SCR_PLPICK); not library-scoped */
               JF_LIST_PLCHK,     /* no screen: pages of the chosen playlist's track ids, scanned for duplicates and cleared between pages */
               JF_LIST_ARTALL,    /* no screen: PLAY ALL of an artist folder, the artist's tracks in album order (store JF_CAP_ARTALL) */
               JF_LIST_ARTINL,    /* no screen: a searched artist's albums then tracks (the artist-folder phases), spliced into the search list when done */
               JF_LIST_COUNT } JfListId;

/* The root browse category (JF_SCR_ARTISTS shows it), in the order JF_EV_CATEGORY cycles. */
typedef enum { JF_CAT_ALBUMS = 0, JF_CAT_ARTISTS, JF_CAT_PLAYLISTS, JF_CAT_GENRES, JF_CAT_SONGS, JF_CAT_COUNT } JfCat;

/** The category name as the screens show it (ALBUMS, ARTISTS, ...); "" for a value outside the enum. */
const char *jf_cat_name(JfCat c);
/** The neighbouring category in enum order, wrapping at both ends: dir > 0 the next, otherwise the previous. */
JfCat jf_cat_step(JfCat c, int dir);

/* What a TRACKS list is the child of: an album (plain), a genre (the tracks whose own genre matches) or a playlist.
 * An ALBUMS list is an artist's (plain). */
typedef enum { JF_SUB_PLAIN = 0, JF_SUB_GENRE, JF_SUB_PLAYLIST,
               JF_SUB_ALBUM   /* JF_LIST_ARTALL only: the id is an album, its tracks in disc / track order (PLAIN = an artist's tracks) */ } JfSub;
enum { JF_FLAT_OFF = 0, JF_FLAT_PLAY, JF_FLAT_PICK };

/* PAUSED: audio stopped, position held in JfApp.pos_ms (resume restarts the stream there).  STOPPED: stopped by the user, queue and
 * queue_cur kept; PAUSE then restarts the current track from 0. */
typedef enum { JF_PLAY_IDLE = 0, JF_PLAY_FETCHING, JF_PLAY_PLAYING, JF_PLAY_END, JF_PLAY_PAUSED, JF_PLAY_STOPPED } JfPlayState;

typedef enum { JF_CONFIRM_NONE = 0, JF_CONFIRM_INSTANT_MIX, JF_CONFIRM_CLEAR_QUEUE, JF_CONFIRM_SIGN_OUT,
               JF_CONFIRM_DELETE_PLAYLIST,  /* confirm_seed = the playlist row; CONFIRM sends the DELETE */
               JF_CONFIRM_QUIT,          /* QUIT while music plays: CONFIRM stops and leaves */
               JF_CONFIRM_ADD_DUP           /* the add would duplicate (dup_hits of pick_n already in add_name) or could not be checked (dup_unknown); CONFIRM adds all */ } JfConfirm;

/* The settings view is a table of rows (the extension point: a new setting is one JfSettingId and one line in
 * jf_settings_rows()).  Page MAIN lists the rows; page LIBRARY lists ALL LIBRARIES then each lists[JF_LIST_LIBS] item. */
typedef enum { JF_SET_LIBRARY = 0, JF_SET_SIGNOUT, JF_SET_TAP, JF_SET_TAGS, JF_SET_COUNT } JfSettingId;   /* rows show in the order LIBRARY, TAP, FILE TAGS, SIGN OUT; TAP steps login.tap_action and saves; TAGS toggles login.tags_on and saves */
typedef enum { JF_SETPAGE_MAIN = 0, JF_SETPAGE_LIBRARY } JfSettingsPage;

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
    JF_EV_MIX,           /* .i: list row, or -1 = the playing track.  Fetches the track's genres, then opens the tick screen (JF_SCR_MIX) */
    JF_EV_MIX_TOGGLE,    /* .i: tick row (JfApp.mix.opt); only on the MIX screen while ticking */
    JF_EV_MIX_START,     /* start the mix with the ticks; none ticked = a notice.  BACK cancels a running mix, then leaves */
    JF_EV_INSTANT_MIX,   /* .i: list row, or -1 = the playing track; the mix replaces the queue and plays */
    JF_EV_SAVE_PLAYLIST, /* .term: the playlist name (the UI never sends an empty one); saves the queue's first JF_SAVE_IDS_MAX */
    JF_EV_DISMISS,       /* drop the action bar */
    JF_EV_PLAYLIST_DELETE,/* .i: list row of a playlist (the Playlists view, from its action bar): asks first (confirm = DELETE_PLAYLIST, confirm_seed = the row, so the
                          * UI names it); the CONFIRM sends one WRITE DELETE and the result shows a banner.  Ignored while a delete is pending */
    JF_EV_PICK_OPEN,     /* ADD TO PLAYLIST.  .i = list row of a track (the action bar's sixth button), or -1 = every track of the TRACKS screen's list
                          * (an album or a playlist; never an artist's albums).  Collects the ids into pick_ids / pick_n (the first JF_SAVE_IDS_MAX, banner
                          * "FIRST 100 TRACKS ONLY" when cut; unloaded pages are fetched first, pick_wait) and opens JF_SCR_PLPICK with pick_sel = -1.
                          * Ignored while an add is in flight */
    JF_EV_PICK_SELECT,   /* .i = list row of JF_LIST_PLPICK (the UI shows "+ NEW PLAYLIST" at row 0 and maps k+1 -> k): sets pick_sel; opens nothing */
    JF_EV_PICK_NEW,      /* .term = the new playlist's name (never empty): one SAVE with the pick_ids, banner "ADDED TO <name>", the picker list reloads
                          * and pick_sel lands on the new row by name */
    JF_EV_PICK_ADD,      /* add pick_ids to the playlist at pick_sel.  A duplicate scan comes first (confirm = JF_CONFIRM_ADD_DUP when it finds any, or cannot
                          * tell); then batches of JF_ADD_BATCH ids, one WRITE POST each; the picker stays open */
    JF_EV_PICK_CLOSE,    /* = BACK from the picker */
    JF_EV_BACK,
    JF_EV_PLAY_ALL,
    JF_EV_NEXT,
    JF_EV_PREV,          /* .played_ms = the position now (jellyfin.c fills it): past JF_PREV_RESTART_MS the track restarts, else the previous queue entry plays (the first entry restarts) */
    JF_EV_SHUFFLE,       /* QUEUE screen: random order; the track playing keeps playing and moves to position 0 (nothing playing: all shuffled) */
    JF_EV_QUEUE_MOVE,    /* QUEUE screen: .i = entry index, .j = the index it moves to; the track playing keeps playing */
    JF_EV_QUEUE_REMOVE,  /* QUEUE screen: .i = entry index (the UI's selected row); no confirm.  An entry before the playing one
                          * only shifts it; the playing entry is replaced by the one after it (played at once), or, when it was
                          * last, playback ends on the new last entry; the only entry empties the queue like a confirmed CLR.
                          * The UI clamps its cursor to queue_n-1 afterwards.  No command saves the queue: jellyfin.c hashes it each frame */
    JF_EV_QUEUE_CLEAR,   /* QUEUE screen, CLR: asks first (confirm = CLEAR_QUEUE); the CONFIRM then empties the queue, stops playback and leaves the NOW PLAYING / QUEUE screens */
    JF_EV_PAUSE,         /* toggle: PLAYING/FETCHING -> PAUSED, PAUSED -> resume, STOPPED/END -> restart the current track from 0.  .played_ms = the position now (jellyfin.c fills it from jf_position_ms) */
    JF_EV_STOP,          /* playback ends, queue and queue_cur stay (STOPPED) */
    JF_EV_SEEK,          /* .i: target position in ms (clamped to [0, runtime)); restarts the stream there, or only moves pos_ms while PAUSED */
    JF_EV_CONFIRM,       /* proceed with app->confirm */
    JF_EV_CONFIRM_SKIP,  /* JF_CONFIRM_ADD_DUP only, and only when jf_dup_can_skip(): add the pick_ids not already in the playlist (pick_ids is compacted); otherwise ignored */
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
    JF_EV_CATEGORY_SET,  /* .i: a JfCat to show (the root header picker); same conditions as JF_EV_CATEGORY, no-op when it is already shown */
    JF_EV_SETTINGS_OPEN, /* root only (ARTISTS, nav empty): opens JF_SCR_SETTINGS, page MAIN, and fetches the libraries; BACK leaves (from page LIBRARY it returns to MAIN) */
    JF_EV_SETTING_ACTIVATE, /* .i: a JfSettingId present in jf_settings_rows(); SETTINGS screen, page MAIN.  LIBRARY opens page LIBRARY, SIGNOUT raises confirm = JF_CONFIRM_SIGN_OUT (CONFIRM signs out) */
    JF_EV_LIBRARY_SET,   /* .i: -1 = ALL LIBRARIES, else an index into lists[JF_LIST_LIBS].items; SETTINGS page LIBRARY with lib_n >= 2.  Saves, returns to the root and reloads it; the queue keeps playing */
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
    int j;
    char server[64];     /* as typed: "host", "host:port", "http://host:port/" */
    char user[64];
    char password[64];
    char term[96];
    JfResult res;
    JfLogin login;
} JfEvent;

typedef enum {
    JF_CMD_NONE = 0,
    JF_CMD_SET_SERVER,   /* jf_api_set_server(.host, .port), jf_api_set_tls(.tls) and jf_art_set_tls(.tls) */
    JF_CMD_SET_AUTH,     /* jf_api_set_auth(device, app->login.device_id, app->login.token) */
    JF_CMD_REQUEST,      /* .kind AUTH: jf_api_enqueue_auth(.tag, .user, .password);
                          * .kind LIST: jf_api_enqueue_list(.tag, .path, .list)  (also the INSTANT MIX request)
                          * .kind SAVE: jf_api_enqueue_save(.tag, .name, app->login.user_id, ids, .n) with ids = the
                          *   ids of app->queue[0..n) (.ids_src JF_IDS_QUEUE) or app->pick_ids[0..n) (JF_IDS_PICK);
                          *   POST, never retried (no app->retry)
                          * .kind WRITE: jf_api_enqueue_write(.tag, .method, .path): DELETE, or POST with an empty body;
                          *   never retried, the result is the status
                          * .kind USER: jf_api_enqueue_user(.tag, .path): GET of one user object, its Policy in the result
                          * .kind TAGS: jf_api_enqueue_tags(.tag, .path, .range): the head of the playing file; a failure to enqueue
                          *   is pushed back as a BADREPLY result (the core then simply shows no file tags) */
    JF_CMD_CANCEL,       /* jf_api_cancel_kind(.kind) */
    JF_CMD_PLAY_STREAM,  /* jf_api_enqueue_stream(.tag, .path); .norm_mdb/.user_mdb repeat the gain */
    JF_CMD_START_AUDIO,  /* audio_music_start_fd(.fd), then audio_music_set_gain(.norm_mdb, .user_mdb);
                          * then send JF_EV_STREAM_STARTED (or TRACK_ENDED played_ms 0 on failure) */
    JF_CMD_CLOSE_FD,     /* close(.fd): a stream result the core does not want */
    JF_CMD_STOP,         /* stop the music voice */
    JF_CMD_SET_GAIN,     /* audio_music_set_gain(.norm_mdb, .user_mdb) */
    JF_CMD_SAVE_LOGIN,   /* jf_login_save(&app->login) and jf_settings_save(&app->login) */
    JF_CMD_CLEAR_LOGIN,  /* jf_login_clear(&app->login); the core already emptied the token */
    JF_CMD_PURGE_LOCAL,  /* delete the saved queue file and the art cache (disk and memory tiles): the queue belonged to another server or user, or the user signed out */
    JF_CMD_EXIT
} JfCmdType;
typedef enum { JF_IDS_QUEUE = 0, JF_IDS_PICK } JfIdsSrc;

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
    bool tls;            /* SET_SERVER: the server is reached over TLS */
    int fd;
    int channels;        /* START_AUDIO: 1 or 2, what the URL asked the server for */
    int norm_mdb;
    int user_mdb;
    char name[96];       /* SAVE: the playlist name */
    int n;               /* SAVE: how many queue ids */
    char method[8];      /* WRITE: "POST" or "DELETE" */
    int ids_src;         /* SAVE: JF_IDS_QUEUE (app->queue[]) or JF_IDS_PICK (app->pick_ids[]) */
    int range;           /* REQUEST kind TAGS: bytes to read from the start of the file (jf_api_enqueue_tags) */
} JfCmd;

typedef struct {
    JfCmd c[JF_CMDS_MAX];
    int n;
    bool overflow;       /* a step wanted more than JF_CMDS_MAX (a bug; tests assert false) */
} JfCmds;

typedef bool (*JfSessionFn)(char out[33]);
/** How many channels the current sink can use (1 or 2); asked once per stream request. */
typedef int (*JfSinkChannelsFn)(void);

typedef struct { JfScreen screen; char id[96]; char title[96]; int sub; int top, cursor; } JfNavEntry;
typedef struct { char id[96]; char title[96]; int sub; } JfListCtx;

typedef enum { JF_MIXOPT_GENRE = 0, JF_MIXOPT_ARTIST, JF_MIXOPT_DECADE } JfMixOptKind;

/* One tick row.  GENRE rows come first (the track's own spelling, deduped by jf_genre_key), then SAME ARTIST (absent
 * without an artist id), then SAME DECADE (absent without a year; the label names it: "SAME DECADE (1980S)"). */
typedef struct { JfMixOptKind kind; char label[96]; bool on; } JfMixOpt;

/* SEED: the track's genres are being fetched.  TICKS: JF_SCR_MIX waits for ticks.  INDEX: walking the genre index to find the
 * ids of the ticked keys.  POOL: collecting matches (`found_n` is the progress to show: "MIXING... 37"). */
typedef enum { JF_MIX_OFF = 0, JF_MIX_SEED, JF_MIX_TICKS, JF_MIX_INDEX, JF_MIX_POOL } JfMixPhase;

typedef struct {
    char key[64];
    char ids[JF_MIX_VARIANTS][JF_ID_LEN + 1];
    int nids;
    long count;              /* sum of the variants' SongCount */
} JfMixKey;

typedef struct {
    JfMixPhase phase;
    JfItem seed;             /* the track as the list showed it: it leads the queue */
    char artist_id[JF_ID_LEN + 1];
    int year;
    JfMixOpt opt[JF_MIX_OPT_MAX];
    int opt_n;
    /* the run */
    JfMixKey gk[JF_MIX_OPT_MAX];   /* the ticked genre keys */
    int gk_n;
    int chosen;              /* the gk asked of the server (smallest count); -1 = none ticked */
    bool use_artist;
    int decade;              /* 0 = any year, else the first year of the decade */
    bool random;             /* pool pages: SortBy=Random, else SortName by `consumed` */
    int consumed;            /* index or pool items read so far: the next StartIndex */
    int reqs;
    int found_n;
    JfItem found[JF_MIX_WANT + 1];   /* [1..found_n] the matches; [0] is filled with the seed when the mix is queued */
    unsigned rng;
} JfMix;

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
    int restore_top, restore_cursor;  /* where the list scrolled to when the screen just returned to was left; valid when restore_seq changes */
    unsigned restore_seq;    /* bumps on every pop: the UI puts the list view back once the rows are there */
    char queue_name[96];     /* the name SAVE AS PLAYLIST offers: set by whatever built the queue ("Queue: <first>", "Mix: <seed>") */
    JfMix mix;               /* MIX: the tick rows (JF_SCR_MIX) and the run's progress; ~45 KB */
    unsigned banner_seq;     /* bumps on every new banner, so the UI can time it */
    bool busy;               /* any request in flight or being waited for: show a spinner */
    JfPlayState play_state;  /* IDLE; FETCHING (stream requested); PLAYING; END = "END OF QUEUE" */
    JfItem queue[JF_QUEUE_MAX];
    int queue_n;
    int queue_cur;           /* index of the track playing / last played; valid when queue_n > 0 */
    char pick_ids[JF_SAVE_IDS_MAX][33];   /* ADD TO PLAYLIST: the ids being added (a track, or a list's first JF_SAVE_IDS_MAX) */
    int pick_n;
    int pick_sel;            /* JF_SCR_PLPICK: list row of JF_LIST_PLPICK chosen by PICK_SELECT (-1 = none; ADD is then absent); reset by every reload of that list */
    char add_name[96];       /* the playlist an add goes to (copied when PICK_ADD starts): the dialog and the banner name it */
    int dup_hits;            /* JF_CONFIRM_ADD_DUP: how many of the pick_n ids the playlist already holds */
    bool dup_unknown;        /* JF_CONFIRM_ADD_DUP: the scan could not tell (page cap or a failed page): the UI asks "COULD NOT CHECK DUPLICATES - ADD?" */
    bool pick_wait;          /* PICK_OPEN -1 is fetching the rest of the TRACKS list before it opens the picker */
    int volume_step;         /* 0..JF_VOLUME_MAX; 0 = mute (also in .login.volume_step) */
    int start_ms;            /* where the live stream started (0, or a seek / resume offset): shown elapsed = start_ms + time since START_AUDIO */
    int pos_ms;              /* PAUSED: the held position */
    uint32_t rng_state;      /* the shuffle's generator (xorshift32); jf_seed() */
    JfSettingsPage settings_page;   /* JF_SCR_SETTINGS: which page shows */
    int lib_n;               /* music libraries known (lists[JF_LIST_LIBS].n after the last fetch; 0 before it) */
    JfConfirm confirm;       /* a modal question is up (INSTANT MIX over a non-empty queue): CONFIRM proceeds, BACK / DISMISS cancels */
    JfItem confirm_seed;     /* the seed the confirmed fetch will use */
    JfLogin login;           /* server and user are kept on 401; token is emptied */
    JfPolicy policy;         /* what the server lets this user delete, from the sign-in reply or the one USER fetch of a restored session; read it through jf_can_delete() */
    bool policy_known;       /* policy has been answered since sign-in (until then DELETE is not offered) */
    bool delete_denied;      /* the server refused a DELETE this session (401/403 although the policy allowed it): DELETE is not offered again */
    JfTags tags;             /* the file tags of the track tags_id (have false = none read); read it through jf_now_tags() */
    char tags_id[JF_ID_LEN + 1];   /* the track `tags` belongs to */
    unsigned tags_seq;       /* bumps whenever `tags` is replaced or cleared, so the UI redraws only then */

    /* ── private to jf_core.c ──────────────────────────────────────────── */
    JfList lists[JF_LIST_COUNT];
    JfListCtx ctx[JF_LIST_COUNT];
    int req_loaded[JF_LIST_COUNT];
    int req_n[JF_LIST_COUNT];            /* rows in the list when its request went out: the tail past it is the page to filter */
    struct { int phase, albums_n, pages; } art[2];   /* artist folder (lists[JF_LIST_ALBUMS], sub PLAIN) and inline artist (JF_LIST_ARTINL): phase 0 albums, 1 tracks, 2 done; rows [0,albums_n) are the albums; tracks pages asked for */
    bool inl_on;                         /* an artist is being fetched for the search list */
    int inl_pos;                         /* the search row it was removed from */
    JfItem inl_item;                     /* that row, put back if the fetch is superseded */
    JfNavEntry nav[JF_NAV_MAX];
    int nav_n;
    int view_top, view_cursor;   /* the list view of the screen showing, as last reported by jf_note_view() */
    JfCmd retry;
    unsigned auth_tag, stream_tag, save_tag;
    bool auth_pending, save_pending;
    bool tags_pending, tags_extended;    /* a TAGS read is in flight / the one extension read of this track was sent */
    unsigned tags_tag;                   /* numbers every TAGS request, so a late result of an earlier track is ignored */
    bool delete_pending, user_pending;   /* a DELETE / the policy fetch is in flight */
    unsigned delete_tag, user_tag;
    char del_name[96];                   /* the playlist being deleted, for the banner */
    char save_name[96];
    bool add_pending, save_pick;         /* an add batch is in flight / the SAVE in flight creates a playlist from pick_ids */
    unsigned add_tag, wr_tag;            /* wr_tag numbers every WRITE (add and delete), so a result belongs to exactly one of them */
    int add_sent, add_batch;             /* ids acknowledged so far / ids in the batch in flight */
    char add_pl[33];                     /* id of the playlist being added to */
    bool dup_scan;                       /* the PLCHK pages are being read */
    int dup_next, dup_pages;             /* StartIndex of the next page / pages read */
    bool pick_dup[JF_SAVE_IDS_MAX];      /* pick_ids[k] was found in the playlist */
    char pick_find[96];                  /* a created playlist's name: select its row when the reloaded list shows it */
    bool play_wait;
    int play_wait_from;
    int flat_mode;                       /* JF_FLAT_*: PLAY ALL / ADD ALL on the search list is expanding its album and artist hits through JF_LIST_ARTALL */
    int flat_hit;                        /* the next search row to flatten */
    int flat_n, flat_seen;               /* tracks collected / rows of the ARTALL list already taken */
    bool flat_cut;                       /* a track did not fit the cap */
    JfItem flat[JF_QUEUE_MAX];           /* the tracks collected so far, in hit order, duplicates by id dropped */
    int fail_streak;
    JfSessionFn session_fn;
    JfSinkChannelsFn sink_channels_fn;   /* NULL = mono */
    int stream_channels;                 /* what the stream in flight was asked for, handed to START_AUDIO */
    JfItem store_artists[JF_CAP_ARTISTS];
    JfItem store_albums[JF_CAP_ALBUMS];
    JfItem store_tracks[JF_CAP_TRACKS];
    JfItem store_search[JF_CAP_SEARCH];
    JfItem store_mix[JF_CAP_MIX];
    JfItem store_seed[JF_CAP_SEED];
    JfItem store_genres[JF_CAP_GENRES];
    JfItem store_pool[JF_CAP_POOL];
    JfItem store_libs[JF_CAP_LIBS];
    JfItem store_plpick[JF_CAP_PLPICK];
    JfItem store_plchk[JF_CAP_PLCHK];
    JfItem store_artall[JF_CAP_ARTALL];
    JfItem store_artinl[JF_CAP_ARTINL];
    char id_server[64], id_user[64];   /* the server and user the saved queue and art belong to (set at start, kept by a sign-in) */
} JfApp;

/** True when the search list holds a hit PLAY ALL / ADD ALL TO PLAYLIST can use: a track, or an album / artist that is expanded to its tracks. */
bool jf_search_playable(const JfApp *app);

/** The settings rows to show, in order (JfSettingId values; returns the count).  An unavailable row is absent: LIBRARY needs 2 or more music libraries. */
int jf_settings_rows(const JfApp *app, int ids[JF_SET_COUNT]);

/** True when the duplicate question offers SKIP EXISTING: the scan finished (not dup_unknown) and some but not all of pick_ids are in the playlist. */
bool jf_dup_can_skip(const JfApp *app);

/** True when DELETE may be offered on a playlist: the policy is known, the server has not refused a delete this session, and the user is an
 *  administrator, has EnableContentDeletion, or has delete-from-folders entries.  The server can still refuse (a non-admin with
 *  EnableContentDeletion got 401 on a playlist, measured): that path sets delete_denied. */
bool jf_can_delete(const JfApp *app);

/** Zero the app, wire the list storage, session_fn = jf_session_id, volume = default. */
void jf_app_init(JfApp *app);

/** Consume one event; `out` holds the commands to execute, in order (n = 0 if none). */
void jf_step(JfApp *app, const JfEvent *ev, JfCmds *out);

/** The UI reports the list view of the screen showing (scroll top and cursor row) before it hands events over, so that a screen
 *  pushed from here can be returned to as it was: the next BACK sets restore_top / restore_cursor and bumps restore_seq. */
void jf_note_view(JfApp *app, int top, int cursor);

/** user_mdb for a volume step: 0 -> -100000 (mute, whatever the track's norm), else (step-10)*3000. */
int jf_volume_mdb(int step);

/** The queue entry playing (or last played); NULL when the queue is empty. */
const JfItem *jf_now(const JfApp *app);

/** The file tags of the track playing, or NULL when none were read (setting off, not arrived, no covered format, server cut off). */
const JfTags *jf_now_tags(const JfApp *app);

/** True when file tags are on but this server answered a range read with a whole file: reads to it are off until the setting is switched off and on. */
bool jf_tags_blocked(const JfApp *app);

/** Position to show: PAUSED pos_ms; PLAYING start_ms + elapsed_ms (since START_AUDIO; 0 when no stream is live); FETCHING start_ms; else 0.  Clamped to the runtime. */
int jf_position_ms(const JfApp *app, int elapsed_ms);


/** A track further than this into its length restarts on PREV; before it, PREV goes to the previous entry. */
#define JF_PREV_RESTART_MS 3000

/** A source of 32 random bits; tests pass their own. */
typedef uint32_t (*JfRng)(void *ctx);

/** r in [0, n) from one draw, by multiply-shift (no divide on this core; the bias is below 2^-20 for a queue). */
uint32_t jf_rand_below(JfRng rng, void *ctx, uint32_t n);

/** Reseed the generator JF_EV_SHUFFLE uses (a zero seed is replaced). */
void jf_seed(JfApp *app, uint32_t seed);

/** Fisher-Yates the queue.  Playing / paused / fetching: the current entry goes to position 0 and the rest are shuffled behind it,
 *  queue_cur = 0.  Otherwise every entry is shuffled and queue_cur follows its entry. */
void jf_queue_shuffle(JfApp *app, JfRng rng, void *ctx);

/** Move queue entry `from` to index `to` (both in range; else false).  queue_cur follows the track it names. */
bool jf_queue_move(JfApp *app, int from, int to);
/* Drop entry i: queue_cur follows its entry (i < cur -> cur-1).  Pure data edit: no command, no state change, so the entry
 * being the playing one is on_queue_remove's business, which calls this and then plays or ends.  False when i is out of range. */
bool jf_queue_remove(JfApp *app, int i);

/** Put a saved queue back (start-up, before JF_EV_START): items[0..n) (tracks), `cur` clamped, PAUSED at `pos_ms` so PLAY resumes there; nothing is played.  n <= 0 changes nothing. */
void jf_queue_restore(JfApp *app, const JfItem *items, int n, int cur, int pos_ms);

#endif
