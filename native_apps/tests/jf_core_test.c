/* Host regression for jellyfin/jf_core.c: the Jellyfin client's state machine.
 *
 *   cd native_apps && gcc -Wall -Wextra -I common -I jellyfin -fsanitize=undefined \
 *       -o build/jf_core_test tests/jf_core_test.c jellyfin/jf_core.c jellyfin/jf_model.c \
 *       jellyfin/jf_url.c jellyfin/jf_login.c common/config.c common/json.c -lm && ./build/jf_core_test
 *
 * Event scripts go in, emitted commands and UI-visible fields are asserted.  A
 * list "reply" is simulated the way jf_api does it: items appended to the cmd's
 * JfList, then a JfResult event.  Not wired into build-and-deploy.sh: host binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jf_core.h"
#include "jf_url.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static JfApp app;
static JfCmds cm;
static unsigned serial, sess_n;

static bool test_session(char out[33]) { snprintf(out, 33, "%032u", ++sess_n); return true; }

static void step_ev(const JfEvent *ev)
{
    jf_step(&app, ev, &cm);
    CHECK(!cm.overflow, "command overflow on event %d", ev->type);
}
static void send(JfEventType t, int i)
{
    static JfEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = t; ev.i = i;
    step_ev(&ev);
}
static void send_played(int ms) { static JfEvent ev; memset(&ev, 0, sizeof(ev)); ev.type = JF_EV_TRACK_ENDED; ev.played_ms = ms; step_ev(&ev); }
static void send_text(JfEventType t, const char *term)
{
    static JfEvent ev; memset(&ev, 0, sizeof(ev)); ev.type = t; snprintf(ev.term, sizeof(ev.term), "%s", term); step_ev(&ev);
}
static void send_signin(const char *srv, const char *user, const char *pw)
{
    static JfEvent ev; memset(&ev, 0, sizeof(ev)); ev.type = JF_EV_SIGNIN;
    snprintf(ev.server, sizeof(ev.server), "%s", srv); snprintf(ev.user, sizeof(ev.user), "%s", user);
    snprintf(ev.password, sizeof(ev.password), "%s", pw); step_ev(&ev);
}
static void send_result(const JfResult *r) { static JfEvent ev; memset(&ev, 0, sizeof(ev)); ev.type = JF_EV_RESULT; ev.res = *r; step_ev(&ev); }

static const JfCmd *find(JfCmdType t)
{
    for (int k = 0; k < cm.n; k++) if (cm.c[k].type == t) return &cm.c[k];
    return NULL;
}
static bool has(JfCmdType t) { return find(t) != NULL; }

/* append `count` items of `type` to the list, as jf_api's parse would */
static void fill(JfList *l, JfItemType type, int count, int total)
{
    for (int k = 0; k < count && l->n < l->cap; k++) {
        JfItem *it = &l->items[l->n++];
        memset(it, 0, sizeof(*it));
        snprintf(it->id, sizeof(it->id), "%032x", ++serial);
        snprintf(it->name, sizeof(it->name), "item %u", serial);
        it->type = type;
        it->norm_gain_mdb = (int)serial * 10;
        it->index = l->loaded + 1;
        l->loaded++;
    }
    l->total = total;
}
static void reply_raw(const JfCmd *c, JfErr e)
{
    JfResult r; memset(&r, 0, sizeof(r));
    r.kind = JF_KIND_LIST; r.tag = c->tag; r.list = c->list; r.err = e; r.fd = -1;
    send_result(&r);
}
static void reply_list(const JfCmd *c, JfItemType type, int count, int total)
{
    JfCmd copy = *c;
    fill(copy.list, type, count, total);
    reply_raw(&copy, JF_ERR_NONE);
}
static void reply_stream(unsigned tag, JfErr e, int fd)
{
    JfResult r; memset(&r, 0, sizeof(r));
    r.kind = JF_KIND_STREAM; r.tag = tag; r.err = e; r.fd = fd;
    send_result(&r);
}

#define UID "0000000000000000000000000000abcd"

static void signed_in(void)
{
    jf_app_init(&app);
    app.session_fn = test_session;
    static JfEvent ev; memset(&ev, 0, sizeof(ev));
    ev.type = JF_EV_START;
    snprintf(ev.login.server, sizeof(ev.login.server), "10.0.0.1:8096");
    snprintf(ev.login.user, sizeof(ev.login.user), "bob");
    snprintf(ev.login.user_id, sizeof(ev.login.user_id), UID);
    snprintf(ev.login.token, sizeof(ev.login.token), "tok");
    ev.login.volume_step = 10;
    step_ev(&ev);
}
static void artists_loaded(int count, int total)
{
    JfCmd c = *find(JF_CMD_REQUEST);
    reply_list(&c, JF_ITEM_ARTIST, count, total);
}
/* sign in, load 3 artists, open the first, then its second album, load `count` tracks */
static void in_tracks(int count, int total)
{
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_ACTIVATE, 0);
    JfCmd a = *find(JF_CMD_REQUEST); reply_list(&a, JF_ITEM_ALBUM, 2, 2);
    send(JF_EV_ACTIVATE, 1);
    JfCmd t = *find(JF_CMD_REQUEST); reply_list(&t, JF_ITEM_TRACK, count, total);
}

static void test_start(void)
{
    jf_app_init(&app);
    app.session_fn = test_session;
    static JfEvent ev; memset(&ev, 0, sizeof(ev)); ev.type = JF_EV_START;
    snprintf(ev.login.server, sizeof(ev.login.server), "10.0.0.1:8096");
    snprintf(ev.login.user, sizeof(ev.login.user), "bob");
    ev.login.volume_step = 7;
    step_ev(&ev);
    CHECK(app.screen == JF_SCR_SIGNIN && cm.n == 0, "no token -> SIGNIN, no commands (screen %d, n %d)", app.screen, cm.n);
    CHECK(app.volume_step == 7 && strcmp(app.login.user, "bob") == 0, "login kept");

    signed_in();
    CHECK(app.screen == JF_SCR_ARTISTS && app.list == &app.lists[JF_LIST_ARTISTS], "token -> ARTISTS");
    CHECK(cm.n == 3 && cm.c[0].type == JF_CMD_SET_SERVER && cm.c[1].type == JF_CMD_SET_AUTH && cm.c[2].type == JF_CMD_REQUEST,
          "START emits SET_SERVER, SET_AUTH, REQUEST (n %d)", cm.n);
    CHECK(strcmp(cm.c[0].host, "10.0.0.1") == 0 && cm.c[0].port == 8096, "server host/port");
    char want[JF_PATH_MAX];
    jf_path_artists(want, sizeof(want), UID, 0, JF_PAGE_ARTISTS);
    CHECK(cm.c[2].kind == JF_KIND_LIST && strcmp(cm.c[2].path, want) == 0 && cm.c[2].list == app.list, "first page request");
    CHECK(cm.c[2].tag == app.list->generation && app.list->pending && app.busy, "tag = generation, pending, busy");
}

static void test_signin(void)
{
    jf_app_init(&app); app.session_fn = test_session;
    send_signin("https://x", "bob", "pw");
    CHECK(cm.n == 0 && strcmp(app.banner, "HTTP ONLY") == 0, "https refused");
    send_signin("", "bob", "pw");
    CHECK(cm.n == 0 && app.banner[0], "empty server refused");
    send_signin("host", "", "pw");
    CHECK(cm.n == 0 && app.banner[0], "empty user refused");
    send_signin("http://10.0.0.1/", "bob", "secret");
    CHECK(cm.c[0].type == JF_CMD_SET_SERVER && cm.c[0].port == 8096 && strcmp(cm.c[0].host, "10.0.0.1") == 0, "SET_SERVER parsed");
    CHECK(cm.n == 3 && cm.c[1].type == JF_CMD_SET_AUTH && cm.c[2].type == JF_CMD_REQUEST,
          "SIGNIN sets the identity before the AUTH request: an empty DeviceId is refused by the server");
    const JfCmd *r = find(JF_CMD_REQUEST);
    CHECK(r && r->kind == JF_KIND_AUTH && strcmp(r->user, "bob") == 0 && strcmp(r->password, "secret") == 0, "AUTH request carries credentials");
    CHECK(app.busy && strcmp(app.login.server, "10.0.0.1:8096") == 0, "busy, server kept canonical");
    unsigned tag = r->tag;

    JfResult res; memset(&res, 0, sizeof(res));
    res.kind = JF_KIND_AUTH; res.tag = tag + 5; res.err = JF_ERR_UNAUTH; res.status = 401; res.fd = -1;
    send_result(&res);
    CHECK(app.busy, "stale AUTH tag ignored");
    res.tag = tag;
    send_result(&res);
    CHECK(app.screen == JF_SCR_SIGNIN && cm.n == 0 && strcmp(app.banner, "WRONG USER OR PASSWORD") == 0 && !app.busy,
          "401 on AUTH: banner, stay, no retry (n %d)", cm.n);
    CHECK(strcmp(app.login.user, "bob") == 0, "user kept after 401");

    send_signin("10.0.0.1", "bob", "secret");
    tag = find(JF_CMD_REQUEST)->tag;
    res.tag = tag; res.err = JF_ERR_UNREACHABLE; res.status = 0;
    send_result(&res);
    CHECK(cm.n == 0 && app.screen == JF_SCR_SIGNIN && strcmp(app.banner, "SERVER UNREACHABLE") == 0, "AUTH never retried by the core");

    send_signin("10.0.0.1", "bob", "secret");
    tag = find(JF_CMD_REQUEST)->tag;
    res.tag = tag; res.err = JF_ERR_BADREPLY; res.status = 415;
    send_result(&res);
    CHECK(strcmp(app.banner, "UNEXPECTED REPLY (HTTP 415)") == 0, "an unexpected status is named in the banner: %s", app.banner);
    send_signin("10.0.0.1", "bob", "secret");
    tag = find(JF_CMD_REQUEST)->tag;
    res.tag = tag; res.err = JF_ERR_BADREPLY; res.status = 200;
    send_result(&res);
    CHECK(strcmp(app.banner, "UNEXPECTED REPLY") == 0, "a 200 that did not parse names no status: %s", app.banner);

    send_signin("10.0.0.1", "bob", "secret");
    tag = find(JF_CMD_REQUEST)->tag;
    memset(&res, 0, sizeof(res));
    res.kind = JF_KIND_AUTH; res.tag = tag; res.fd = -1;
    snprintf(res.token, sizeof(res.token), "TOKEN"); snprintf(res.user_id, sizeof(res.user_id), UID);
    send_result(&res);
    CHECK(app.screen == JF_SCR_ARTISTS && cm.n == 3, "AUTH ok -> ARTISTS (n %d)", cm.n);
    CHECK(cm.c[0].type == JF_CMD_SET_AUTH && cm.c[1].type == JF_CMD_SAVE_LOGIN && cm.c[2].type == JF_CMD_REQUEST && cm.c[2].kind == JF_KIND_LIST,
          "SET_AUTH, SAVE_LOGIN, first artists page");
    CHECK(strcmp(app.login.token, "TOKEN") == 0 && strcmp(app.login.user_id, UID) == 0, "login filled");
}

static void test_nav_paging(void)
{
    signed_in(); artists_loaded(3, 3);
    CHECK(!app.busy && app.list->n == 3, "artists shown");
    send(JF_EV_ACTIVATE, 1);
    const JfCmd *r = find(JF_CMD_REQUEST);
    char want[JF_PATH_MAX];
    jf_path_albums(want, sizeof(want), UID, app.lists[JF_LIST_ARTISTS].items[1].id, 0, JF_PAGE_ALBUMS);
    CHECK(app.screen == JF_SCR_ALBUMS && r && strcmp(r->path, want) == 0, "artist -> ALBUMS request");
    CHECK(strcmp(app.crumb, app.lists[JF_LIST_ARTISTS].items[1].name) == 0, "crumb = artist name");
    JfCmd a = *r; reply_list(&a, JF_ITEM_ALBUM, 2, 2);
    send(JF_EV_ACTIVATE, 0);
    CHECK(app.screen == JF_SCR_TRACKS, "album -> TRACKS");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ALBUMS && cm.n == 0 && app.lists[JF_LIST_ALBUMS].n == 2, "BACK pops, albums intact, no request");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ARTISTS && cm.n == 0 && app.lists[JF_LIST_ARTISTS].n == 3, "BACK to ARTISTS, no reload");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ARTISTS && cm.n == 2 && cm.c[0].type == JF_CMD_STOP && cm.c[1].type == JF_CMD_EXIT, "BACK on ARTISTS -> STOP, EXIT");

    /* paging */
    signed_in(); artists_loaded(50, 120);
    send(JF_EV_NEED_MORE, 0);
    jf_path_artists(want, sizeof(want), UID, 50, JF_PAGE_ARTISTS);
    r = find(JF_CMD_REQUEST);
    CHECK(r && strcmp(r->path, want) == 0 && app.list->pending, "NEED_MORE requests StartIndex = loaded");
    JfCmd page2 = *r;
    send(JF_EV_NEED_MORE, 0);
    CHECK(cm.n == 0, "no second request while pending");
    reply_list(&page2, JF_ITEM_ARTIST, 70, 120);
    send(JF_EV_NEED_MORE, 0);
    CHECK(cm.n == 0 && app.list->n == 120, "no request when loaded == total");
    signed_in(); artists_loaded(50, 50);
    send(JF_EV_NEED_MORE, 0);
    CHECK(cm.n == 0, "no request when everything is loaded");
    signed_in(); artists_loaded(50, 120);
    app.list->full = true;
    send(JF_EV_NEED_MORE, 0);
    CHECK(cm.n == 0, "no request when the list is full");
    /* a request for another list supersedes a page in flight: the first is no longer pending */
    signed_in(); artists_loaded(3, 120);
    send(JF_EV_NEED_MORE, 0);
    CHECK(app.lists[JF_LIST_ARTISTS].pending, "page in flight");
    send(JF_EV_ACTIVATE, 0);
    CHECK(!app.lists[JF_LIST_ARTISTS].pending && app.lists[JF_LIST_ALBUMS].pending, "new LIST supersedes the page in flight");
    /* a page that consumed nothing ends paging */
    signed_in(); artists_loaded(50, 120);
    send(JF_EV_NEED_MORE, 0);
    JfCmd empty = *find(JF_CMD_REQUEST);
    reply_list(&empty, JF_ITEM_ARTIST, 0, 120);
    send(JF_EV_NEED_MORE, 0);
    CHECK(cm.n == 0 && app.list->total == 50, "empty page ends paging");
}

static void test_stale(void)
{
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_ACTIVATE, 0);
    JfCmd first = *find(JF_CMD_REQUEST);
    send(JF_EV_BACK, 0);
    send(JF_EV_ACTIVATE, 1);
    JfCmd second = *find(JF_CMD_REQUEST);
    CHECK(second.tag != first.tag && second.list == first.list, "re-target bumps the generation");
    JfList *al = &app.lists[JF_LIST_ALBUMS];
    reply_raw(&first, JF_ERR_NONE);
    CHECK(al->pending, "stale reply dropped: still waiting for the current one");
    reply_raw(&first, JF_ERR_SERVER);
    CHECK(!app.overlay && al->pending, "stale error dropped: no overlay");
    reply_list(&second, JF_ITEM_ALBUM, 2, 2);
    CHECK(!al->pending && al->n == 2, "current reply accepted");
    reply_raw(&second, JF_ERR_SERVER);
    CHECK(!app.overlay, "a repeated reply for a settled request is dropped");
}

static void test_queue(void)
{
    in_tracks(5, 5);
    JfList *tl = &app.lists[JF_LIST_TRACKS];
    send(JF_EV_ACTIVATE, 2);
    CHECK(app.queue_n == 3 && app.queue_cur == 0 && strcmp(app.queue[0].id, tl->items[2].id) == 0 &&
          strcmp(app.queue[2].id, tl->items[4].id) == 0, "queue = tracks from row 2 (n %d)", app.queue_n);
    CHECK(cm.n == 1 && cm.c[0].type == JF_CMD_PLAY_STREAM && app.play_state == JF_PLAY_FETCHING, "one PLAY_STREAM, no STOP when idle");
    const JfCmd *p = &cm.c[0];
    char want[JF_PATH_MAX], sess[33]; snprintf(sess, sizeof(sess), "%032u", sess_n);
    jf_path_stream(want, sizeof(want), tl->items[2].id, sess);
    CHECK(strcmp(p->path, want) == 0 && p->norm_mdb == tl->items[2].norm_gain_mdb && p->user_mdb == 0, "stream path, norm gain, volume 10 = 0 mdb");
    unsigned tag = p->tag;
    reply_stream(tag + 9, JF_ERR_NONE, 77);
    CHECK(cm.n == 1 && cm.c[0].type == JF_CMD_CLOSE_FD && cm.c[0].fd == 77, "stale stream fd is closed");
    reply_stream(tag, JF_ERR_NONE, 33);
    CHECK(cm.n == 1 && cm.c[0].type == JF_CMD_START_AUDIO && cm.c[0].fd == 33 && cm.c[0].norm_mdb == tl->items[2].norm_gain_mdb, "START_AUDIO");
    send(JF_EV_STREAM_STARTED, 0);
    CHECK(app.play_state == JF_PLAY_PLAYING && cm.n == 0, "PLAYING");
    CHECK(jf_now(&app) == &app.queue[0], "now = queue[0]");

    send_played(180000);
    CHECK(cm.n == 1 && cm.c[0].type == JF_CMD_PLAY_STREAM && app.queue_cur == 1 && cm.c[0].tag == tag + 1, "good end -> next track");
    reply_stream(cm.c[0].tag, JF_ERR_NONE, 34); send(JF_EV_STREAM_STARTED, 0);
    send(JF_EV_NEXT, 0);
    CHECK(cm.n == 2 && cm.c[0].type == JF_CMD_STOP && cm.c[1].type == JF_CMD_PLAY_STREAM && app.queue_cur == 2, "NEXT while playing: STOP + next");
    reply_stream(cm.c[1].tag, JF_ERR_NONE, 35); send(JF_EV_STREAM_STARTED, 0);
    send_played(200000);
    CHECK(app.play_state == JF_PLAY_END && has(JF_CMD_STOP) && !has(JF_CMD_PLAY_STREAM) && app.queue_cur == 2, "end of queue -> END, STOP");
    send(JF_EV_NEXT, 0);
    CHECK(app.play_state == JF_PLAY_END && !has(JF_CMD_PLAY_STREAM), "NEXT at the end stays at the end");

    /* queue jump */
    send(JF_EV_OPEN_NOWPLAYING, 0);
    CHECK(app.screen == JF_SCR_NOWPLAYING, "now playing");
    send(JF_EV_OPEN_QUEUE, 0);
    CHECK(app.screen == JF_SCR_QUEUE, "queue");
    send(JF_EV_QUEUE_JUMP, 1);
    CHECK(app.screen == JF_SCR_NOWPLAYING && app.queue_cur == 1 && has(JF_CMD_PLAY_STREAM), "jump plays that entry, back on now playing");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_TRACKS, "BACK from now playing -> TRACKS");

    /* mid-album: pages missing */
    in_tracks(25, 30);
    tl = &app.lists[JF_LIST_TRACKS];
    send(JF_EV_ACTIVATE, 3);
    const JfCmd *r = find(JF_CMD_REQUEST);
    jf_path_tracks(want, sizeof(want), UID, app.ctx[JF_LIST_TRACKS].id, 25, JF_PAGE_TRACKS);
    CHECK(cm.n == 1 && r && strcmp(r->path, want) == 0 && !has(JF_CMD_PLAY_STREAM) && app.busy, "missing pages fetched before playing");
    JfCmd rest = *r;
    reply_list(&rest, JF_ITEM_TRACK, 5, 30);
    CHECK(app.queue_n == 27 && has(JF_CMD_PLAY_STREAM) && strcmp(app.queue[0].id, tl->items[3].id) == 0, "queue of 27 after the rest arrives (n %d)", app.queue_n);
    /* leaving cancels the wait */
    in_tracks(25, 30);
    send(JF_EV_ACTIVATE, 3);
    JfCmd w = *find(JF_CMD_REQUEST);
    send(JF_EV_BACK, 0);
    reply_list(&w, JF_ITEM_TRACK, 5, 30);
    CHECK(!has(JF_CMD_PLAY_STREAM), "BACK cancels a pending play");
    /* PLAY ALL */
    in_tracks(5, 5);
    send(JF_EV_PLAY_ALL, 0);
    CHECK(app.queue_n == 5 && has(JF_CMD_PLAY_STREAM), "PLAY ALL on TRACKS");
    send(JF_EV_BACK, 0);
    send(JF_EV_PLAY_ALL, 0);
    CHECK(cm.n == 0, "PLAY ALL ignored on ALBUMS");
}

static void test_failures(void)
{
    in_tracks(6, 6);
    send(JF_EV_ACTIVATE, 0);
    reply_stream(cm.c[0].tag, JF_ERR_NONE, 40); send(JF_EV_STREAM_STARTED, 0);
    send_played(1000);
    CHECK(has(JF_CMD_PLAY_STREAM) && app.queue_cur == 1 && !app.overlay, "1st short track: skip");
    send(JF_EV_STREAM_STARTED, 0);
    send_played(2000);
    CHECK(has(JF_CMD_PLAY_STREAM) && app.queue_cur == 2 && !app.overlay, "2nd short track: skip");
    send(JF_EV_STREAM_STARTED, 0);
    send_played(3000);
    CHECK(!has(JF_CMD_PLAY_STREAM) && has(JF_CMD_STOP) && app.overlay && app.overlay_buttons == JF_BTN_CLOSE &&
          app.play_state == JF_PLAY_IDLE, "3rd consecutive short track: STOP + overlay");
    send(JF_EV_CLOSE_ERROR, 0);
    CHECK(!app.overlay, "overlay closes");

    in_tracks(8, 8);
    send(JF_EV_ACTIVATE, 0);
    send(JF_EV_STREAM_STARTED, 0);
    send_played(1000); send(JF_EV_STREAM_STARTED, 0);
    send_played(1000); send(JF_EV_STREAM_STARTED, 0);
    send_played(60000); send(JF_EV_STREAM_STARTED, 0);      /* a good track resets the streak */
    send_played(1000); send(JF_EV_STREAM_STARTED, 0);
    send_played(1000);
    CHECK(!app.overlay && has(JF_CMD_PLAY_STREAM) && app.fail_streak == 2, "a good track resets the streak (streak %d)", app.fail_streak);

    /* a stream that fails to open counts too */
    in_tracks(6, 6);
    send(JF_EV_ACTIVATE, 0);
    reply_stream(cm.c[0].tag, JF_ERR_SERVER, -1);
    CHECK(has(JF_CMD_PLAY_STREAM) && app.queue_cur == 1 && !has(JF_CMD_START_AUDIO), "stream error -> next track");
    reply_stream(cm.c[0].tag, JF_ERR_UNREACHABLE, -1);
    reply_stream(cm.c[0].tag, JF_ERR_NOTFOUND, -1);
    CHECK(app.overlay && app.play_state == JF_PLAY_IDLE, "three stream errors -> overlay");
}

static void test_expiry(void)
{
    in_tracks(5, 5);
    send(JF_EV_ACTIVATE, 0);
    reply_stream(cm.c[0].tag, JF_ERR_NONE, 40); send(JF_EV_STREAM_STARTED, 0);
    /* back to artists, start paging, get 401 */
    send(JF_EV_BACK, 0); send(JF_EV_BACK, 0);
    app.lists[JF_LIST_ARTISTS].total = 99;
    send(JF_EV_NEED_MORE, 0);
    JfCmd page = *find(JF_CMD_REQUEST);
    reply_raw(&page, JF_ERR_UNAUTH);
    CHECK(app.screen == JF_SCR_SIGNIN && has(JF_CMD_CLEAR_LOGIN), "401 -> SIGNIN + CLEAR_LOGIN");
    CHECK(!has(JF_CMD_STOP) && !has(JF_CMD_EXIT) && !has(JF_CMD_PLAY_STREAM), "music not stopped, nothing fetched");
    CHECK(app.login.token[0] == '\0' && strcmp(app.login.server, "10.0.0.1:8096") == 0 && strcmp(app.login.user, "bob") == 0, "token cleared, server and user kept");
    CHECK(strcmp(app.banner, "SESSION EXPIRED, SIGN IN AGAIN") == 0 && !app.overlay && app.play_state == JF_PLAY_PLAYING, "banner, still PLAYING");
    send_played(200000);
    CHECK(cm.n == 0 && app.play_state == JF_PLAY_IDLE, "track end after expiry fetches no next track (n %d)", cm.n);

    /* 401 on the stream itself */
    in_tracks(5, 5);
    send(JF_EV_ACTIVATE, 0);
    reply_stream(cm.c[0].tag, JF_ERR_UNAUTH, -1);
    CHECK(app.screen == JF_SCR_SIGNIN && has(JF_CMD_CLEAR_LOGIN) && !has(JF_CMD_PLAY_STREAM) && !has(JF_CMD_STOP), "401 on stream -> SIGNIN");
}

static void test_volume(void)
{
    signed_in(); artists_loaded(3, 3);
    CHECK(app.volume_step == 10, "default step");
    for (int k = 0; k < 4; k++) send(JF_EV_VOL_UP, 0);
    CHECK(app.volume_step == 14 && cm.n == 2 && cm.c[0].type == JF_CMD_SET_GAIN && cm.c[0].user_mdb == 12000 && cm.c[1].type == JF_CMD_SAVE_LOGIN,
          "step 14 = +12000 mdb, SET_GAIN + SAVE_LOGIN");
    send(JF_EV_VOL_UP, 0);
    CHECK(app.volume_step == 14 && cm.n == 0, "clamped at 14, nothing emitted");
    CHECK(app.login.volume_step == 14, "login carries the step");
    for (int k = 0; k < 13; k++) send(JF_EV_VOL_DOWN, 0);
    CHECK(app.volume_step == 1 && cm.c[0].user_mdb == -27000, "step 1");
    send(JF_EV_VOL_DOWN, 0);
    CHECK(app.volume_step == 0 && cm.c[0].user_mdb == -100000, "step 0 = mute value");
    send(JF_EV_VOL_DOWN, 0);
    CHECK(app.volume_step == 0 && cm.n == 0, "clamped at 0");
    CHECK(jf_volume_mdb(10) == 0 && jf_volume_mdb(0) == -100000 && jf_volume_mdb(14) == 12000, "jf_volume_mdb");
    /* norm gain of the playing track rides along */
    in_tracks(3, 3);
    send(JF_EV_ACTIVATE, 1);
    send(JF_EV_VOL_UP, 0);
    CHECK(cm.c[0].norm_mdb == app.lists[JF_LIST_TRACKS].items[1].norm_gain_mdb, "SET_GAIN carries the playing track's norm");
}

static void test_search(void)
{
    signed_in(); artists_loaded(3, 3);
    send_text(JF_EV_SEARCH, "");
    CHECK(cm.n == 0, "empty search with nothing pending does nothing");
    send_text(JF_EV_SEARCH, "blue note");
    const JfCmd *r = find(JF_CMD_REQUEST);
    char want[JF_PATH_MAX];
    jf_path_search(want, sizeof(want), UID, "blue note", 0, JF_PAGE_SEARCH);
    CHECK(app.screen == JF_SCR_SEARCH && r && strcmp(r->path, want) == 0, "search request");
    CHECK(strcmp(app.crumb, "SEARCH: blue note") == 0, "crumb");
    send_text(JF_EV_SEARCH, "");
    CHECK(has(JF_CMD_CANCEL) && find(JF_CMD_CANCEL)->kind == JF_KIND_LIST && !app.lists[JF_LIST_SEARCH].pending, "empty term cancels");
    send_text(JF_EV_SEARCH, "blue");
    JfCmd sr = *find(JF_CMD_REQUEST);
    JfList *sl = &app.lists[JF_LIST_SEARCH];
    fill(sl, JF_ITEM_ALBUM, 1, 5); fill(sl, JF_ITEM_ARTIST, 1, 5); fill(sl, JF_ITEM_TRACK, 1, 5);
    fill(sl, JF_ITEM_ALBUM, 1, 5); fill(sl, JF_ITEM_TRACK, 1, 5);
    reply_raw(&sr, JF_ERR_NONE);
    send(JF_EV_ACTIVATE, 1);
    CHECK(app.screen == JF_SCR_ALBUMS && has(JF_CMD_REQUEST), "artist in search -> ALBUMS");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_SEARCH && cm.n == 0 && sl->n == 5, "BACK to the search results, intact");
    send(JF_EV_ACTIVATE, 0);
    CHECK(app.screen == JF_SCR_TRACKS && has(JF_CMD_REQUEST), "album in search -> TRACKS");
    send(JF_EV_BACK, 0);
    send(JF_EV_ACTIVATE, 2);
    CHECK(app.queue_n == 2 && strcmp(app.queue[0].id, sl->items[2].id) == 0 && strcmp(app.queue[1].id, sl->items[4].id) == 0 &&
          has(JF_CMD_PLAY_STREAM), "track in search plays with the following tracks only (n %d)", app.queue_n);
    send(JF_EV_PLAY_ALL, 0);
    CHECK(app.queue_n == 2 && strcmp(app.queue[0].id, sl->items[2].id) == 0, "PLAY ALL on search starts at the first track");

    /* the albums list re-aimed by a later visit is aimed back on BACK */
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_ACTIVATE, 0);
    JfCmd a = *find(JF_CMD_REQUEST); reply_list(&a, JF_ITEM_ALBUM, 2, 2);
    char first_id[96]; snprintf(first_id, sizeof(first_id), "%s", app.ctx[JF_LIST_ALBUMS].id);
    send_text(JF_EV_SEARCH, "x");
    JfCmd s = *find(JF_CMD_REQUEST);
    fill(&app.lists[JF_LIST_SEARCH], JF_ITEM_ARTIST, 1, 1); reply_raw(&s, JF_ERR_NONE);
    send(JF_EV_ACTIVATE, 0);
    CHECK(strcmp(app.ctx[JF_LIST_ALBUMS].id, first_id) != 0, "albums re-aimed by the search artist");
    send(JF_EV_BACK, 0);
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ALBUMS && strcmp(app.ctx[JF_LIST_ALBUMS].id, first_id) == 0 && has(JF_CMD_REQUEST),
          "BACK to ALBUMS re-requests the original artist");
}

static void test_retry(void)
{
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_ACTIVATE, 0);
    JfCmd req = *find(JF_CMD_REQUEST);
    reply_raw(&req, JF_ERR_UNREACHABLE);
    CHECK(app.overlay && app.overlay_buttons == (JF_BTN_RETRY | JF_BTN_CLOSE) && !app.busy, "UNREACHABLE -> RETRY/CLOSE");
    send(JF_EV_ACTIVATE, 0);
    CHECK(cm.n == 0, "overlay is modal");
    send(JF_EV_RETRY, 0);
    const JfCmd *r = find(JF_CMD_REQUEST);
    CHECK(cm.n == 1 && r && strcmp(r->path, req.path) == 0 && r->tag == req.tag && r->list == req.list && r->kind == req.kind &&
          !app.overlay && app.busy && req.list->pending, "RETRY re-issues the identical request");
    JfCmd again = *r;
    reply_raw(&again, JF_ERR_SERVER);
    CHECK(app.overlay && (app.overlay_buttons & JF_BTN_RETRY), "SERVER -> RETRY");
    send(JF_EV_CLOSE_ERROR, 0);
    CHECK(!app.overlay && cm.n == 0, "CLOSE");
    send(JF_EV_BACK, 0);
    send(JF_EV_ACTIVATE, 0);
    req = *find(JF_CMD_REQUEST);
    reply_raw(&req, JF_ERR_BADREPLY);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && strcmp(app.overlay_text, "UNEXPECTED REPLY") == 0, "BADREPLY -> CLOSE only");
    send(JF_EV_RETRY, 0);
    CHECK(cm.n == 0 && app.overlay, "RETRY ignored without the button");
    send(JF_EV_BACK, 0);
    CHECK(!app.overlay, "BACK closes the overlay");
    /* parse failure with err NONE is a bad reply too */
    send(JF_EV_BACK, 0);
    send(JF_EV_ACTIVATE, 0);
    req = *find(JF_CMD_REQUEST);
    JfResult res; memset(&res, 0, sizeof(res)); res.kind = JF_KIND_LIST; res.tag = req.tag; res.list = req.list; res.parse = JF_PARSE_BAD; res.fd = -1;
    send_result(&res);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE, "parse failure -> UNEXPECTED REPLY");
}

static void test_quit(void)
{
    in_tracks(3, 3);
    send(JF_EV_QUIT, 0);
    CHECK(cm.n == 2 && cm.c[0].type == JF_CMD_STOP && cm.c[1].type == JF_CMD_EXIT, "QUIT -> STOP, EXIT");
    jf_app_init(&app);
    send(JF_EV_BACK, 0);
    CHECK(has(JF_CMD_EXIT), "BACK on SIGNIN leaves");
    CHECK(sizeof(JfApp) < 2000000, "JfApp stays small enough to be static (%zu)", sizeof(JfApp));
}

/* The root category: ALBUMS <-> ARTISTS <-> PLAYLISTS <-> GENRES <-> SONGS, default ARTISTS. */
static void test_categories(void)
{
    char want[JF_PATH_MAX];
    signed_in(); artists_loaded(3, 3);
    CHECK(app.cat == JF_CAT_ARTISTS && strcmp(app.crumb, "ARTISTS") == 0, "default category ARTISTS");

    /* the full cycle forward from ARTISTS, wrapping, with the request each step makes */
    send(JF_EV_CATEGORY, 1);
    jf_path_playlists(want, sizeof(want), UID, 0, JF_PAGE_ARTISTS);
    CHECK(app.cat == JF_CAT_PLAYLISTS && find(JF_CMD_REQUEST) && strcmp(find(JF_CMD_REQUEST)->path, want) == 0,
          "ARTISTS > PLAYLISTS requests the playlists");
    CHECK(strcmp(app.crumb, "PLAYLISTS") == 0 && app.nav_n == 0 && app.screen == JF_SCR_ARTISTS, "root replaced, no stack growth");
    send(JF_EV_CATEGORY, 1);
    jf_path_genres(want, sizeof(want), UID, 0, JF_PAGE_ARTISTS);
    CHECK(app.cat == JF_CAT_GENRES && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "> GENRES");
    send(JF_EV_CATEGORY, 1);
    jf_path_songs(want, sizeof(want), UID, 0, JF_PAGE_TRACKS);
    CHECK(app.cat == JF_CAT_SONGS && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "> SONGS pages by the track page size");
    send(JF_EV_CATEGORY, 1);
    jf_path_albums_all(want, sizeof(want), UID, 0, JF_PAGE_ARTISTS);
    CHECK(app.cat == JF_CAT_ALBUMS && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "SONGS wraps to ALBUMS");
    send(JF_EV_CATEGORY, 1);
    jf_path_artists(want, sizeof(want), UID, 0, JF_PAGE_ARTISTS);
    CHECK(app.cat == JF_CAT_ARTISTS && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "ALBUMS > ARTISTS");
    send(JF_EV_CATEGORY, -1);
    send(JF_EV_CATEGORY, -1);
    CHECK(app.cat == JF_CAT_SONGS, "backwards from ALBUMS wraps to SONGS (cat %d)", app.cat);

    /* the old request is superseded: its late reply changes nothing, and the new list is the one pending */
    JfCmd songs = *find(JF_CMD_REQUEST);
    send(JF_EV_CATEGORY, -1);                       /* SONGS -> GENRES while songs is in flight */
    JfCmd genres = *find(JF_CMD_REQUEST);
    reply_raw(&songs, JF_ERR_NONE);                /* jf_api would have cancelled it; a late one must still change nothing */
    CHECK(app.lists[JF_LIST_ARTISTS].n == 0 && app.lists[JF_LIST_ARTISTS].pending, "a stale category reply is dropped");
    reply_list(&genres, JF_ITEM_GENRE, 2, 1120);
    CHECK(app.lists[JF_LIST_ARTISTS].n == 2 && app.lists[JF_LIST_ARTISTS].total == 1120 && !app.busy, "genres shown");

    /* genre -> its albums -> tracks, BACK walks home without reloading the root */
    send(JF_EV_ACTIVATE, 1);
    jf_path_genre_albums(want, sizeof(want), UID, app.lists[JF_LIST_ARTISTS].items[1].id, 0, JF_PAGE_ALBUMS);
    CHECK(app.screen == JF_SCR_ALBUMS && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "genre opens its albums");
    CHECK(strcmp(app.crumb, app.lists[JF_LIST_ARTISTS].items[1].name) == 0, "crumb = genre name");
    JfCmd alb = *find(JF_CMD_REQUEST);
    send(JF_EV_CATEGORY, 1);
    CHECK(app.cat == JF_CAT_GENRES && app.screen == JF_SCR_ALBUMS && cm.n == 0, "no category switch below the root");
    reply_list(&alb, JF_ITEM_ALBUM, 2, 2);
    send(JF_EV_ACTIVATE, 0);
    jf_path_tracks(want, sizeof(want), UID, app.lists[JF_LIST_ALBUMS].items[0].id, 0, JF_PAGE_TRACKS);
    CHECK(app.screen == JF_SCR_TRACKS && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "a genre's album opens as a plain album");
    send(JF_EV_BACK, 0); send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ARTISTS && app.cat == JF_CAT_GENRES && app.lists[JF_LIST_ARTISTS].n == 2 && !has(JF_CMD_REQUEST),
          "BACK x2 returns to the genre list, not reloaded");
    send(JF_EV_BACK, 0);
    CHECK(has(JF_CMD_EXIT), "BACK at the root leaves");

    /* playlist -> its tracks (playlist path), PLAY ALL, BACK */
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_CATEGORY, 1);
    JfCmd pl = *find(JF_CMD_REQUEST); reply_list(&pl, JF_ITEM_PLAYLIST, 1, 1);
    send(JF_EV_ACTIVATE, 0);
    jf_path_playlist_items(want, sizeof(want), UID, app.lists[JF_LIST_ARTISTS].items[0].id, 0, JF_PAGE_TRACKS);
    CHECK(app.screen == JF_SCR_TRACKS && strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "playlist opens its items");
    JfCmd pt = *find(JF_CMD_REQUEST); reply_list(&pt, JF_ITEM_TRACK, 3, 3);
    send(JF_EV_PLAY_ALL, 0);
    CHECK(has(JF_CMD_PLAY_STREAM) && app.queue_n == 3, "PLAY ALL on a playlist queues its tracks");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ARTISTS && app.cat == JF_CAT_PLAYLISTS && app.lists[JF_LIST_ARTISTS].n == 1 && !has(JF_CMD_REQUEST),
          "BACK from a playlist returns to PLAYLISTS");

    /* a song row plays; its page extends through NEED_MORE */
    send(JF_EV_CATEGORY, 1); send(JF_EV_CATEGORY, 1);
    JfCmd sg = *find(JF_CMD_REQUEST); reply_list(&sg, JF_ITEM_TRACK, 25, 39280);
    send(JF_EV_NEED_MORE, 0);
    jf_path_songs(want, sizeof(want), UID, 25, JF_PAGE_TRACKS);
    CHECK(strcmp(find(JF_CMD_REQUEST)->path, want) == 0, "SONGS pages with StartIndex 25");
    send(JF_EV_ACTIVATE, 2);
    CHECK(has(JF_CMD_PLAY_STREAM) && app.queue_n == 25 - 2, "a song row queues from the row on");

    /* the search screen is not the root: no switching there */
    send_text(JF_EV_SEARCH, "blue");
    send(JF_EV_CATEGORY, 1);
    CHECK(app.screen == JF_SCR_SEARCH && cm.n == 0, "no category switch on SEARCH");
}

int main(void)
{
    test_start(); test_signin(); test_nav_paging(); test_stale(); test_queue();
    test_failures(); test_expiry(); test_volume(); test_search(); test_retry(); test_quit(); test_categories();
    printf("jf_core_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
