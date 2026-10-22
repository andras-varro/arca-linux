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
static int max_cmds;     /* the most commands one step emitted since the test that cares reset it */

static bool test_session(char out[33]) { snprintf(out, 33, "%032u", ++sess_n); return true; }

static void step_ev(const JfEvent *ev)
{
    jf_step(&app, ev, &cm);
    CHECK(!cm.overflow, "command overflow on event %d", ev->type);
    if (ev->type != JF_EV_START && cm.n > max_cmds) max_cmds = cm.n;   /* START is the session set-up, not a step of a flow under test */
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
    /* a restored session also asks for its delete policy (one USER request, after the list request) */
    CHECK(cm.n == 4 && cm.c[3].type == JF_CMD_REQUEST && cm.c[3].kind == JF_KIND_USER && cm.c[0].type == JF_CMD_SET_SERVER && cm.c[1].type == JF_CMD_SET_AUTH && cm.c[2].type == JF_CMD_REQUEST,
          "START emits SET_SERVER, SET_AUTH, REQUEST, USER (n %d)", cm.n);
    CHECK(strcmp(cm.c[0].host, "10.0.0.1") == 0 && cm.c[0].port == 8096, "server host/port");
    char want[JF_PATH_MAX];
    jf_path_artists(want, sizeof(want), UID, 0, JF_PAGE_ARTISTS);
    CHECK(cm.c[2].kind == JF_KIND_LIST && strcmp(cm.c[2].path, want) == 0 && cm.c[2].list == app.list, "first page request");
    CHECK(cm.c[2].tag == app.list->generation && app.list->pending && app.busy, "tag = generation, pending, busy");
}

static void test_signin(void)
{
    jf_app_init(&app); app.session_fn = test_session;
    send_signin("http://8.8.8.8", "bob", "pw");
    CHECK(cm.n == 0 && strcmp(app.banner, "USE HTTPS://") == 0, "http to a public address refused");
    send_signin("http://example.com", "bob", "pw");
    CHECK(cm.n == 0 && strcmp(app.banner, "USE HTTPS://") == 0, "http to a hostname refused");
    send_signin("https://nas.example.com:8443/", "", "pw");
    CHECK(cm.n == 0 && strcmp(app.banner, "ENTER THE USER NAME") == 0, "https accepted (user checked next)");
    send_signin("https://nas.example.com:8443/", "bob", "pw");
    CHECK(cm.c[0].type == JF_CMD_SET_SERVER && cm.c[0].tls && cm.c[0].port == 8443 && strcmp(cm.c[0].host, "nas.example.com") == 0,
          "https sign-in: SET_SERVER carries tls, port 8443");
    CHECK(strcmp(app.login.server, "https://nas.example.com:8443/") == 0, "https server saved as typed");
    send_signin("", "bob", "pw");
    CHECK(cm.n == 0 && app.banner[0], "empty server refused");
    send_signin("host", "", "pw");
    CHECK(cm.n == 0 && app.banner[0], "empty user refused");
    send_signin("http://10.0.0.1/", "bob", "secret");
    CHECK(cm.c[0].type == JF_CMD_SET_SERVER && cm.c[0].port == 8096 && !cm.c[0].tls && strcmp(cm.c[0].host, "10.0.0.1") == 0, "SET_SERVER parsed, plain");
    CHECK(cm.n == 3 && cm.c[1].type == JF_CMD_SET_AUTH && cm.c[2].type == JF_CMD_REQUEST,
          "SIGNIN sets the identity before the AUTH request: an empty DeviceId is refused by the server");
    const JfCmd *r = find(JF_CMD_REQUEST);
    CHECK(r && r->kind == JF_KIND_AUTH && strcmp(r->user, "bob") == 0 && strcmp(r->password, "secret") == 0, "AUTH request carries credentials");
    CHECK(app.busy && strcmp(app.login.server, "http://10.0.0.1/") == 0, "busy, server kept as typed");
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
    jf_path_artist_albums(want, sizeof(want), UID, app.lists[JF_LIST_ARTISTS].items[1].id, 0, JF_PAGE_ALBUMS);
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
    reply_list(&second, JF_ITEM_ALBUM, 2, 5);   /* more albums than this page: the artist folder waits for the scroll */
    CHECK(!al->pending && al->n == 2, "current reply accepted");
    reply_raw(&second, JF_ERR_SERVER);
    CHECK(!app.overlay, "a repeated reply for a settled request is dropped");
}

static int sink_stereo(void) { return 2; }static int sink_mono(void)   { return 1; }/* The channel count asked of the server follows the sink, and START_AUDIO carries what was asked. */static void test_sink_channels(void){    for (int pass = 0; pass < 3; pass++) {        in_tracks(5, 5);        app.sink_channels_fn = pass == 0 ? sink_stereo : pass == 1 ? sink_mono : NULL;        send(JF_EV_ACTIVATE, 2);        const JfCmd *p = find(JF_CMD_PLAY_STREAM);        CHECK(p && strstr(p->path, pass == 0 ? "audioChannels=2&" : "audioChannels=1&") != NULL,              "pass %d: stream path follows the sink: %s", pass, p ? p->path : "(none)");        CHECK(app.stream_channels == (pass == 0 ? 2 : 1), "pass %d: stream_channels %d", pass, app.stream_channels);    }}
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
    jf_path_stream(want, sizeof(want), tl->items[2].id, sess, 1);
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
    CHECK(app.screen == JF_SCR_QUEUE && app.queue_cur == 1 && has(JF_CMD_PLAY_STREAM), "jump plays that entry and stays on the queue");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_NOWPLAYING, "BACK from the queue -> now playing");
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
    CHECK(cm.n == 1 && cm.c[0].list == &app.lists[JF_LIST_ARTALL], "PLAY ALL on an artist folder fetches its whole discography");
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
    /* closing while music plays asks first; CONFIRM then stops and leaves, BACK keeps playing */
    in_tracks(3, 3);
    app.queue_n = 3; app.queue_cur = 0; app.play_state = JF_PLAY_PLAYING;
    send(JF_EV_QUIT, 0);
    CHECK(cm.n == 0 && app.confirm == JF_CONFIRM_QUIT, "QUIT while playing: asks, emits nothing");
    send(JF_EV_BACK, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE && cm.n == 0 && app.play_state == JF_PLAY_PLAYING, "BACK on the quit question keeps playing");
    send(JF_EV_QUIT, 0);
    send(JF_EV_CONFIRM, 0);
    CHECK(cm.n == 2 && cm.c[0].type == JF_CMD_STOP && cm.c[1].type == JF_CMD_EXIT && app.confirm == JF_CONFIRM_NONE, "CONFIRM on the quit question -> STOP, EXIT");
    in_tracks(3, 3);
    app.queue_n = 3; app.play_state = JF_PLAY_PAUSED;
    send(JF_EV_QUIT, 0);
    CHECK(cm.n == 2 && cm.c[1].type == JF_CMD_EXIT && app.confirm == JF_CONFIRM_NONE, "QUIT while paused leaves at once");
    jf_app_init(&app);
    send(JF_EV_BACK, 0);
    CHECK(has(JF_CMD_EXIT), "BACK on SIGNIN leaves");
    CHECK(sizeof(JfApp) < 3000000, "JfApp stays small enough to be static (%zu)", sizeof(JfApp));
}

/* The neighbour labels of the root's bottom row, and picking a category outright (the header picker). */
static void test_category_pick(void)
{
    /* neighbours in enum order, wrapping at both ends */
    CHECK(jf_cat_step(JF_CAT_ALBUMS, -1) == JF_CAT_SONGS && strcmp(jf_cat_name(jf_cat_step(JF_CAT_ALBUMS, -1)), "SONGS") == 0,
          "before ALBUMS wraps to SONGS");
    CHECK(jf_cat_step(JF_CAT_ALBUMS, 1) == JF_CAT_ARTISTS && strcmp(jf_cat_name(jf_cat_step(JF_CAT_ALBUMS, 1)), "ARTISTS") == 0,
          "after ALBUMS is ARTISTS");
    CHECK(jf_cat_step(JF_CAT_SONGS, 1) == JF_CAT_ALBUMS && strcmp(jf_cat_name(jf_cat_step(JF_CAT_SONGS, 1)), "ALBUMS") == 0,
          "after SONGS wraps to ALBUMS");
    CHECK(jf_cat_step(JF_CAT_SONGS, -1) == JF_CAT_GENRES, "before SONGS is GENRES");
    CHECK(strcmp(jf_cat_name(JF_CAT_PLAYLISTS), "PLAYLISTS") == 0 && jf_cat_name((JfCat)JF_CAT_COUNT)[0] == '\0' &&
          jf_cat_name((JfCat)-1)[0] == '\0', "names, and none for a value outside the enum");
    for (int c = 0; c < JF_CAT_COUNT; c++)
        CHECK(jf_cat_step(jf_cat_step((JfCat)c, 1), -1) == (JfCat)c, "step forward then back returns to %d", c);

    char want[JF_PATH_MAX];
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_CATEGORY_SET, JF_CAT_SONGS);
    jf_path_songs(want, sizeof(want), UID, 0, JF_PAGE_TRACKS);
    CHECK(app.cat == JF_CAT_SONGS && find(JF_CMD_REQUEST) && strcmp(find(JF_CMD_REQUEST)->path, want) == 0 &&
          strcmp(app.crumb, "SONGS") == 0 && app.nav_n == 0, "SET jumps straight to SONGS (cat %d)", app.cat);
    send(JF_EV_CATEGORY_SET, JF_CAT_SONGS);
    CHECK(!has(JF_CMD_REQUEST) && app.cat == JF_CAT_SONGS, "SET to the category already shown reloads nothing");
    send(JF_EV_CATEGORY_SET, 17);
    send(JF_EV_CATEGORY_SET, -3);
    CHECK(app.cat == JF_CAT_SONGS && !has(JF_CMD_REQUEST), "SET outside the enum is ignored");
    send(JF_EV_CATEGORY_SET, JF_CAT_ALBUMS);
    CHECK(app.cat == JF_CAT_ALBUMS && has(JF_CMD_REQUEST), "SET from SONGS back to ALBUMS");
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


/* ── the action bar, queue insertion, instant mix, save ─────────────────── */

static void start_playing(int row)
{
    send(JF_EV_ACTIVATE, row);
    unsigned tag = find(JF_CMD_PLAY_STREAM)->tag;
    reply_stream(tag, JF_ERR_NONE, 40);
    send(JF_EV_STREAM_STARTED, 0);
}
static void save_result(unsigned tag, JfErr e, int status)
{
    JfResult r; memset(&r, 0, sizeof(r));
    r.kind = JF_KIND_SAVE; r.tag = tag; r.err = e; r.status = status; r.fd = -1;
    send_result(&r);
}
/* a playing queue of `n` entries that are not the list's rows */
static void fake_queue(int n)
{
    for (int k = 0; k < n; k++) {
        memset(&app.queue[k], 0, sizeof(app.queue[k]));
        app.queue[k].type = JF_ITEM_TRACK;
        snprintf(app.queue[k].id, sizeof(app.queue[k].id), "%032x", 0x7000 + k);
        snprintf(app.queue[k].name, sizeof(app.queue[k].name), "q%d", k);
    }
    app.queue_n = n; app.queue_cur = 0; app.play_state = JF_PLAY_PLAYING;
}

static void test_action_bar(void)
{
    in_tracks(5, 5);
    send(JF_EV_SELECT, 1);
    CHECK(app.action_bar && app.action_row == 1 && cm.n == 0 && app.play_state == JF_PLAY_IDLE, "SELECT on a track raises the bar, plays nothing");
    send(JF_EV_SELECT, 1);
    CHECK(!app.action_bar && cm.n == 0, "the same row again drops the bar");
    send(JF_EV_SELECT, 1); send(JF_EV_SELECT, 3);
    CHECK(app.action_bar && app.action_row == 3, "another track row moves the bar");
    send(JF_EV_BACK, 0);
    CHECK(!app.action_bar && app.screen == JF_SCR_TRACKS && cm.n == 0, "BACK only drops the bar (screen %d)", app.screen);
    send(JF_EV_SELECT, 2); send(JF_EV_NEED_MORE, 0); send(JF_EV_VOL_UP, 0);
    CHECK(app.action_bar && app.action_row == 2, "more rows arriving and volume keep the bar");
    send(JF_EV_DISMISS, 0);
    CHECK(!app.action_bar, "DISMISS drops it");
    send(JF_EV_SELECT, 2); send(JF_EV_ACTIVATE, 2);
    CHECK(!app.action_bar && app.queue_n == 3 && has(JF_CMD_PLAY_STREAM), "PLAY (ACTIVATE) plays from the row and drops the bar");
    send(JF_EV_SELECT, 2); send(JF_EV_OPEN_NOWPLAYING, 0);
    CHECK(app.screen == JF_SCR_NOWPLAYING && !app.action_bar, "a screen change drops the bar");
    send(JF_EV_BACK, 0);
    send(JF_EV_SELECT, 2); send_text(JF_EV_SEARCH, "blue");
    CHECK(app.screen == JF_SCR_SEARCH && !app.action_bar, "a search drops the bar");
    send(JF_EV_BACK, 0);
    CHECK(!app.action_bar, "still down after BACK");

    in_tracks(3, 3);
    send(JF_EV_SELECT, 1); send(JF_EV_PLAY_NEXT, 1);
    CHECK(!app.action_bar, "an action drops the bar");

    signed_in(); artists_loaded(3, 3);
    send(JF_EV_SELECT, 0);
    CHECK(app.screen == JF_SCR_ALBUMS && !app.action_bar && has(JF_CMD_REQUEST), "SELECT on a non-track row opens it");
}

static void test_queue_insert(void)
{
    in_tracks(6, 6);
    JfList *tl = &app.lists[JF_LIST_TRACKS];
    send(JF_EV_PLAY_NEXT, 3);
    CHECK(app.queue_n == 1 && app.queue_cur == 0 && strcmp(app.queue[0].id, tl->items[3].id) == 0 &&
          has(JF_CMD_PLAY_STREAM) && !has(JF_CMD_STOP) && app.play_state == JF_PLAY_FETCHING &&
          strncmp(app.queue_name, "Queue: ", 7) == 0,
          "PLAY NEXT with an empty queue plays that one track (n %d)", app.queue_n);

    start_playing(0);                               /* queue = rows 0..5 */
    char old1[33]; snprintf(old1, sizeof old1, "%s", app.queue[1].id);
    send(JF_EV_PLAY_NEXT, 4);
    CHECK(app.queue_n == 7 && app.queue_cur == 0 && strcmp(app.queue[1].id, tl->items[4].id) == 0 &&
          strcmp(app.queue[2].id, old1) == 0 && cm.n == 0 && strcmp(app.banner, "QUEUED NEXT") == 0,
          "PLAY NEXT lands after the playing entry, shifts the rest, plays nothing (n %d banner %s)", app.queue_n, app.banner);
    app.queue_cur = 3;
    send(JF_EV_PLAY_NEXT, 5);
    CHECK(app.queue_n == 8 && app.queue_cur == 3 && strcmp(app.queue[4].id, tl->items[5].id) == 0, "PLAY NEXT in the middle");
    app.queue_cur = app.queue_n - 1;
    send(JF_EV_PLAY_NEXT, 2);
    CHECK(app.queue_n == 9 && strcmp(app.queue[8].id, tl->items[2].id) == 0 && app.queue_cur == 7, "PLAY NEXT at the end appends");

    fake_queue(4);
    char l3[33]; snprintf(l3, sizeof l3, "%s", app.queue[3].id);
    send(JF_EV_ADD_QUEUE, 1);
    CHECK(app.queue_n == 5 && strcmp(app.queue[4].id, tl->items[1].id) == 0 && strcmp(app.queue[3].id, l3) == 0 &&
          cm.n == 0 && strcmp(app.banner, "ADDED TO QUEUE") == 0, "ADD appends");
    fake_queue(JF_QUEUE_MAX);
    char snap[33]; snprintf(snap, sizeof snap, "%s", app.queue[1].id);
    send(JF_EV_ADD_QUEUE, 1);
    CHECK(app.queue_n == JF_QUEUE_MAX && strcmp(app.banner, "QUEUE FULL") == 0 && cm.n == 0, "ADD on a full queue: notice, unchanged");
    send(JF_EV_PLAY_NEXT, 1);
    CHECK(app.queue_n == JF_QUEUE_MAX && strcmp(app.banner, "QUEUE FULL") == 0 && strcmp(app.queue[1].id, snap) == 0 && cm.n == 0,
          "PLAY NEXT on a full queue: notice, unchanged");

    app.queue_n = 0; app.play_state = JF_PLAY_IDLE;
    send(JF_EV_ADD_QUEUE, 2);
    CHECK(app.queue_n == 1 && has(JF_CMD_PLAY_STREAM) && strcmp(app.queue[0].id, tl->items[2].id) == 0, "ADD with nothing playing starts it");
    app.play_state = JF_PLAY_END; app.queue_n = 3;
    send(JF_EV_ADD_QUEUE, 4);
    CHECK(app.queue_n == 1 && has(JF_CMD_PLAY_STREAM) && !has(JF_CMD_STOP) && strcmp(app.queue[0].id, tl->items[4].id) == 0,
          "ADD after the queue ended starts a fresh queue (n %d)", app.queue_n);
    app.play_state = JF_PLAY_END; app.queue_n = 3;
    send(JF_EV_PLAY_NEXT, 5);
    CHECK(app.queue_n == 1 && has(JF_CMD_PLAY_STREAM) && strcmp(app.queue[0].id, tl->items[5].id) == 0, "PLAY NEXT after the queue ended starts a fresh queue");

    signed_in(); artists_loaded(3, 3);
    send(JF_EV_ADD_QUEUE, 0); send(JF_EV_PLAY_NEXT, 0);
    CHECK(app.queue_n == 0 && cm.n == 0, "a non-track row cannot be queued");
}

static void test_mix(void)
{
    in_tracks(4, 4);
    JfList *tl = &app.lists[JF_LIST_TRACKS];
    send(JF_EV_MIX, 1);                  /* the real MIX is in test_mix_* below: it asks for the seed and queues nothing */
    CHECK(cm.n == 1 && app.queue_n == 0 && strcmp(app.banner, "MIX: COMING") != 0, "MIX is no placeholder any more");
    send(JF_EV_BACK, 0);

    start_playing(0);
    send(JF_EV_SELECT, 2);
    send(JF_EV_INSTANT_MIX, 2);
    send(JF_EV_CONFIRM, 0);
    const JfCmd *c = find(JF_CMD_REQUEST);
    CHECK(c && cm.n == 1 && c->kind == JF_KIND_LIST && c->list == &app.lists[JF_LIST_MIX] && !app.action_bar,
          "INSTANT MIX requests a LIST into the mix store and drops the bar");
    CHECK(c && strstr(c->path, "/InstantMix") && strstr(c->path, tl->items[2].id) && strstr(c->path, UID),
          "path names the endpoint, the seed and the user: %s", c ? c->path : "-");
    CHECK(app.busy && app.queue_n == 4 && app.play_state == JF_PLAY_PLAYING, "busy while fetching; the queue and audio are untouched");
    JfCmd req = *c;
    reply_list(&req, JF_ITEM_TRACK, 5, 5);
    CHECK(app.queue_n == 5 && app.queue_cur == 0 && strcmp(app.queue[0].id, app.lists[JF_LIST_MIX].items[0].id) == 0,
          "the mix replaces the queue (n %d)", app.queue_n);
    CHECK(cm.n == 2 && cm.c[0].type == JF_CMD_STOP && cm.c[1].type == JF_CMD_PLAY_STREAM && strstr(cm.c[1].path, app.queue[0].id),
          "stop the playing audio, stream the mix's first item");
    char want[96]; snprintf(want, sizeof want, "Mix: %.80s", tl->items[2].name);
    CHECK(strcmp(app.queue_name, want) == 0, "queue_name = '%s' (got '%s')", want, app.queue_name);

    /* seed -1 = the playing track */
    reply_stream(cm.c[1].tag, JF_ERR_NONE, 42); send(JF_EV_STREAM_STARTED, 0);
    send(JF_EV_INSTANT_MIX, -1);
    send(JF_EV_CONFIRM, 0);
    c = find(JF_CMD_REQUEST);
    CHECK(c && strstr(c->path, app.queue[0].id), "INSTANT MIX from NOW PLAYING seeds with the playing track");

    /* stale: a second request supersedes the first */
    JfCmd first = *c;
    send(JF_EV_INSTANT_MIX, -1);
    send(JF_EV_CONFIRM, 0);
    int qn = app.queue_n;
    reply_list(&first, JF_ITEM_TRACK, 3, 3);
    CHECK(app.queue_n == qn && cm.n == 0, "a superseded mix reply is ignored");

    /* empty answer: queue stays */
    in_tracks(4, 4); start_playing(0);
    send(JF_EV_INSTANT_MIX, 1);
    send(JF_EV_CONFIRM, 0);
    req = *find(JF_CMD_REQUEST);
    reply_list(&req, JF_ITEM_TRACK, 0, 0);
    CHECK(strcmp(app.banner, "NO MIX FOUND") == 0 && app.queue_n == 4 && cm.n == 0 && !app.busy, "empty mix: notice, queue kept");

    /* errors */
    send(JF_EV_INSTANT_MIX, 1);
    send(JF_EV_CONFIRM, 0);
    req = *find(JF_CMD_REQUEST);
    reply_raw(&req, JF_ERR_SERVER);
    CHECK(app.overlay && (app.overlay_buttons & JF_BTN_RETRY) && app.queue_n == 4, "mix server error: overlay with RETRY");
    send(JF_EV_RETRY, 0);
    CHECK(has(JF_CMD_REQUEST) && find(JF_CMD_REQUEST)->list == &app.lists[JF_LIST_MIX] && strstr(find(JF_CMD_REQUEST)->path, "/InstantMix"),
          "RETRY re-asks the mix");
    req = *find(JF_CMD_REQUEST);
    reply_raw(&req, JF_ERR_NOTFOUND);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && strcmp(app.overlay_text, "NOT FOUND") == 0, "mix 404: CLOSE only");
    send(JF_EV_CLOSE_ERROR, 0);
    send(JF_EV_INSTANT_MIX, 1);
    send(JF_EV_CONFIRM, 0);
    req = *find(JF_CMD_REQUEST);
    reply_raw(&req, JF_ERR_UNAUTH);
    CHECK(app.screen == JF_SCR_SIGNIN, "mix 401 -> session expired");

    /* idle: no STOP before the first stream */
    in_tracks(4, 4);
    send(JF_EV_INSTANT_MIX, 0);
    req = *find(JF_CMD_REQUEST);
    reply_list(&req, JF_ITEM_TRACK, 3, 3);
    CHECK(cm.n == 1 && cm.c[0].type == JF_CMD_PLAY_STREAM && app.queue_n == 3, "idle: the mix starts without a STOP");
}

/* ── MIX on a track: seed fetch, tick screen, genre index, candidate pool ─── */

#define ARTID "000000000000000000000000a47157ee"
#define GID(n) (0x200000u + (n))      /* genre-index ids */
#define PID(n) (0x100000u + (n))      /* pool track ids  */

static JfItem mk_item(JfItemType t, unsigned n, const char *name, const char *genres)
{
    JfItem it;
    memset(&it, 0, sizeof(it));
    it.type = t;
    snprintf(it.id, sizeof(it.id), "%032x", n);
    snprintf(it.name, sizeof(it.name), "%s", name);
    if (genres) snprintf(it.genres, sizeof(it.genres), "%s", genres);
    return it;
}
static const char *hexid(unsigned n) { static char b[4][33]; static int k; k = (k + 1) & 3; snprintf(b[k], 33, "%032x", n); return b[k]; }

/* the caller's JfCmd is a copy: jf_step overwrites `cm` */
static void reply_items(const JfCmd *c, const JfItem *src, int n, int total)
{
    JfCmd copy = *c;
    JfList *l = copy.list;
    for (int k = 0; k < n && l->n < l->cap; k++) l->items[l->n++] = src[k];
    l->loaded += n;
    l->total = total;
    reply_raw(&copy, JF_ERR_NONE);
}

struct gs { const char *name; unsigned idn; int count; };
static void reply_genre_page(const JfCmd *c, int start, int total, const struct gs *sp, int nsp)
{
    char want[JF_PATH_MAX];
    jf_path_genre_index(want, sizeof(want), UID, start, JF_MIX_GENRE_PAGE);
    CHECK(c->kind == JF_KIND_LIST && c->list == &app.lists[JF_LIST_GENRES] && strcmp(c->path, want) == 0,
          "genre index request for start %d: %s", start, c->path);
    int size = total - start < JF_MIX_GENRE_PAGE ? total - start : JF_MIX_GENRE_PAGE;
    static JfItem page[JF_MIX_GENRE_PAGE];
    for (int k = 0; k < size; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "Filler %d", start + k);
        page[k] = k < nsp ? mk_item(JF_ITEM_GENRE, sp[k].idn, sp[k].name, NULL) : mk_item(JF_ITEM_GENRE, 0x300000u + (unsigned)(start + k), nm, NULL);
        page[k].count = k < nsp ? sp[k].count : 7;
    }
    reply_items(c, page, size, total);
}

/* a track list with a known artist, MIX on row 1, the seed reply: ends on the tick screen (or a banner) */
static void mix_open(const char *genres, int year, bool artist)
{
    in_tracks(4, 4);
    snprintf(app.lists[JF_LIST_TRACKS].items[1].artist, sizeof(app.lists[JF_LIST_TRACKS].items[1].artist), "The Band");
    send(JF_EV_MIX, 1);
    JfCmd c = *find(JF_CMD_REQUEST);
    JfItem s = app.lists[JF_LIST_TRACKS].items[1];
    snprintf(s.genres, sizeof(s.genres), "%s", genres);
    s.year = year;
    if (artist) snprintf(s.artist_id, sizeof(s.artist_id), "%s", ARTID);
    reply_items(&c, &s, 1, 1);
}

/* Rock+Pop ticked, START, the index in one page with the given counts: stops with the first pool request pending */
static void mix_run(int rock, int pop)
{
    mix_open("Rock\nPop\n", 0, false);
    send(JF_EV_MIX_START, 0);
    JfCmd c = *find(JF_CMD_REQUEST);
    struct gs sp[2] = { { "Rock", GID(1), rock }, { "Pop", GID(2), pop } };
    reply_genre_page(&c, 0, 3, sp, 2);
}

static const JfCmd *pool_req(void)
{
    const JfCmd *c = find(JF_CMD_REQUEST);
    return c && c->list == &app.lists[JF_LIST_POOL] ? c : NULL;
}

/* pool pages: `n` items per page; the first `match` carry Rock+Pop, the others Rock only; ids run on (or follow the start) */
static struct { int total, n, match; bool by_start; unsigned ctr; int starts[16]; bool rnd[16]; int reqs; } pg;
static void pg_reset(int total, int n, int match, bool by_start)
{
    memset(&pg, 0, sizeof(pg));
    pg.total = total; pg.n = n; pg.match = match; pg.by_start = by_start;
}
static int pg_run(int max)
{
    while (pg.reqs < max) {
        const JfCmd *p = pool_req();
        if (!p) break;
        JfCmd c = *p;
        const char *s = strstr(c.path, "StartIndex=");
        int start = s ? atoi(s + 11) : -1;
        pg.starts[pg.reqs] = start;
        pg.rnd[pg.reqs] = strstr(c.path, "SortBy=Random") != NULL;
        pg.reqs++;
        int n = pg.n;
        if (pg.by_start && start + n > pg.total) n = pg.total - start;
        static JfItem page[JF_MIX_PAGE];
        for (int k = 0; k < n; k++)
            page[k] = mk_item(JF_ITEM_TRACK, PID(pg.by_start ? (unsigned)(start + k + 1) : ++pg.ctr), "T", k < pg.match ? "Rock\nPop\n" : "Rock\n");
        reply_items(&c, page, n, pg.total);
    }
    return pg.reqs;
}
static bool queue_unique(void)
{
    for (int a = 0; a < app.queue_n; a++)
        for (int b = a + 1; b < app.queue_n; b++) if (strcmp(app.queue[a].id, app.queue[b].id) == 0) return false;
    return true;
}

static void test_mix_seed_and_ticks(void)
{
    in_tracks(4, 4);
    send(JF_EV_MIX, 99);
    CHECK(cm.n == 0 && app.mix.phase == JF_MIX_OFF, "MIX on a row that is not there: nothing");
    send(JF_EV_MIX, 1);
    const JfCmd *c = find(JF_CMD_REQUEST);
    char want[JF_PATH_MAX];
    jf_path_item_by_id(want, sizeof(want), UID, app.lists[JF_LIST_TRACKS].items[1].id);
    CHECK(c && cm.n == 1 && c->kind == JF_KIND_LIST && c->list == &app.lists[JF_LIST_SEED] && strcmp(c->path, want) == 0,
          "seed request: Ids=<row id> into the seed list (%s)", c ? c->path : "-");
    CHECK(app.mix.phase == JF_MIX_SEED && app.screen == JF_SCR_TRACKS && app.busy && !app.banner[0], "SEED phase, still on the tracks, busy");
    JfCmd seedreq = *c;

    send(JF_EV_VOL_UP, 0);
    CHECK(app.mix.phase == JF_MIX_SEED && !has(JF_CMD_CANCEL), "volume does not abandon the seed fetch");
    send(JF_EV_NEED_MORE, 0);
    CHECK(app.mix.phase == JF_MIX_SEED && !has(JF_CMD_CANCEL), "NEED_MORE does not abandon the seed fetch");
    int nav0 = app.nav_n;
    send(JF_EV_BACK, 0);
    CHECK(app.mix.phase == JF_MIX_OFF && has(JF_CMD_CANCEL) && app.screen == JF_SCR_TRACKS && app.nav_n == nav0 && !app.busy,
          "BACK abandons the fetch and is spent: still on the tracks (screen %d nav %d)", app.screen, app.nav_n);
    JfItem s = app.lists[JF_LIST_TRACKS].items[1];
    snprintf(s.genres, sizeof(s.genres), "Rock\n");
    reply_items(&seedreq, &s, 1, 1);
    CHECK(app.screen == JF_SCR_TRACKS && app.mix.phase == JF_MIX_OFF && cm.n == 0, "a seed reply after the abandon is ignored");

    send(JF_EV_MIX, 1);
    send(JF_EV_ACTIVATE, 0);
    CHECK(app.mix.phase == JF_MIX_OFF && has(JF_CMD_CANCEL), "another intent abandons the fetch too");

    /* the tick screen: genres deduped by key, own spelling, artist and decade */
    mix_open("Rock\nPop Music\nPop\n  \nHip-Hop\n", 1984, true);
    const JfMix *m = &app.mix;
    CHECK(app.screen == JF_SCR_MIX && m->phase == JF_MIX_TICKS && !app.busy, "tick screen (screen %d phase %d)", app.screen, m->phase);
    CHECK(m->opt_n == 5, "5 rows: Rock, Pop Music, Hip-Hop, artist, decade (got %d)", m->opt_n);
    if (m->opt_n == 5) {
        CHECK(m->opt[0].kind == JF_MIXOPT_GENRE && strcmp(m->opt[0].label, "Rock") == 0, "row 0 '%s'", m->opt[0].label);
        CHECK(m->opt[1].kind == JF_MIXOPT_GENRE && strcmp(m->opt[1].label, "Pop Music") == 0, "row 1 keeps the first spelling: '%s'", m->opt[1].label);
        CHECK(m->opt[2].kind == JF_MIXOPT_GENRE && strcmp(m->opt[2].label, "Hip-Hop") == 0, "row 2 '%s'", m->opt[2].label);
        CHECK(m->opt[3].kind == JF_MIXOPT_ARTIST && strcmp(m->opt[3].label, "SAME ARTIST") == 0, "row 3 '%s'", m->opt[3].label);
        CHECK(m->opt[4].kind == JF_MIXOPT_DECADE && strcmp(m->opt[4].label, "SAME DECADE (1980S)") == 0, "row 4 '%s'", m->opt[4].label);
        CHECK(m->opt[0].on && m->opt[1].on && m->opt[2].on && !m->opt[3].on && !m->opt[4].on, "genres on, artist and decade off");
    }
    CHECK(strcmp(m->artist_id, ARTID) == 0 && m->year == 1984, "artist id and year kept");

    send(JF_EV_MIX_TOGGLE, 3);
    CHECK(app.mix.opt[3].on && cm.n == 0, "toggle turns a row on");
    send(JF_EV_MIX_TOGGLE, 3);
    CHECK(!app.mix.opt[3].on, "and off again");
    send(JF_EV_MIX_TOGGLE, 0);
    CHECK(!app.mix.opt[0].on && app.mix.opt[1].on, "toggle changes only its row");
    send(JF_EV_MIX_TOGGLE, -1); send(JF_EV_MIX_TOGGLE, 5); send(JF_EV_MIX_TOGGLE, 1000);
    CHECK(!app.mix.opt[0].on && app.mix.opt[1].on && app.mix.opt[2].on && !app.mix.opt[3].on && !app.mix.opt[4].on, "out-of-range toggles change nothing");

    send(JF_EV_MIX_TOGGLE, 1); send(JF_EV_MIX_TOGGLE, 2);
    send(JF_EV_MIX_START, 0);
    CHECK(strcmp(app.banner, "TICK AT LEAST ONE") == 0 && cm.n == 0 && app.mix.phase == JF_MIX_TICKS && app.screen == JF_SCR_MIX && !app.busy,
          "START with nothing ticked: banner, no command (banner '%s', n %d)", app.banner, cm.n);

    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_TRACKS && app.mix.phase == JF_MIX_OFF, "BACK on the ticks leaves (screen %d)", app.screen);
    send(JF_EV_MIX_TOGGLE, 0);
    send(JF_EV_MIX_START, 0);
    CHECK(cm.n == 0 && app.screen == JF_SCR_TRACKS, "toggle and START outside the MIX screen do nothing");

    mix_open("Rock\n", 0, false);
    CHECK(app.screen == JF_SCR_MIX && app.mix.opt_n == 1 && app.mix.opt[0].on, "no artist id and no year: the genre row only (n %d)", app.mix.opt_n);
    mix_open("Rock\n", 999, true);
    CHECK(app.mix.opt_n == 2 && app.mix.opt[1].kind == JF_MIXOPT_ARTIST, "a year below 1000 gives no decade row (n %d)", app.mix.opt_n);
    mix_open("", 1999, false);
    CHECK(app.mix.opt_n == 1 && app.mix.opt[0].kind == JF_MIXOPT_DECADE && strcmp(app.mix.opt[0].label, "SAME DECADE (1990S)") == 0,
          "no genres: the decade row alone, 1999 -> 1990S ('%s')", app.mix.opt[0].label);
    mix_open("", 0, false);
    CHECK(app.screen == JF_SCR_TRACKS && strcmp(app.banner, "NOTHING TO MIX ON") == 0 && app.mix.phase == JF_MIX_OFF, "nothing known: banner (%s)", app.banner);
    in_tracks(4, 4);
    send(JF_EV_MIX, 1);
    JfCmd none = *find(JF_CMD_REQUEST);
    reply_items(&none, NULL, 0, 0);
    CHECK(app.screen == JF_SCR_TRACKS && strcmp(app.banner, "TRACK NOT FOUND") == 0, "empty seed reply: banner (%s)", app.banner);
}

static void test_mix_flow(void)
{
    mix_open("Rock\nPop Music\nPop\n", 1984, true);
    int nav_before = app.nav_n - 1;
    send(JF_EV_MIX_TOGGLE, 2);       /* artist on */
    send(JF_EV_MIX_TOGGLE, 3);       /* decade on */
    send(JF_EV_MIX_START, 0);
    const JfCmd *c = find(JF_CMD_REQUEST);
    CHECK(c && cm.n == 1 && app.mix.phase == JF_MIX_INDEX && app.busy && app.screen == JF_SCR_MIX, "START asks for the genre index");
    CHECK(app.mix.gk_n == 2 && strcmp(app.mix.gk[0].key, "rock") == 0 && strcmp(app.mix.gk[1].key, "pop") == 0, "ticked keys: %d, '%s', '%s'",
          app.mix.gk_n, app.mix.gk[0].key, app.mix.gk[1].key);

    /* the index over three pages: spellings fold into their key, a fifth variant adds count but no id */
    JfCmd r = *c;
    struct gs p1[] = { { "Pop", GID(1), 500 }, { "Rock", GID(2), 300 } };
    reply_genre_page(&r, 0, 250, p1, 2);
    c = find(JF_CMD_REQUEST);
    CHECK(c && cm.n == 1 && app.mix.phase == JF_MIX_INDEX && app.mix.consumed == 100, "page 1 read; next page requested (consumed %d)", app.mix.consumed);
    r = *c;
    struct gs p2[] = { { "Pop Music", GID(3), 50 }, { "Rock Music", GID(4), 20 }, { "Hip Hop", GID(9), 99 } };
    reply_genre_page(&r, 100, 250, p2, 3);
    c = find(JF_CMD_REQUEST);
    CHECK(c && app.mix.consumed == 200 && c->list == &app.lists[JF_LIST_GENRES], "page 2 read; third requested (consumed %d)", app.mix.consumed);
    r = *c;
    struct gs p3[] = { { "ROCK", GID(5), 10 }, { "rock_", GID(6), 5 }, { "Rock - Music", GID(7), 1 } };
    reply_genre_page(&r, 200, 250, p3, 3);
    CHECK(app.mix.gk[0].nids == JF_MIX_VARIANTS && app.mix.gk[1].nids == 2, "rock keeps %d ids, pop %d", app.mix.gk[0].nids, app.mix.gk[1].nids);
    CHECK(app.mix.gk[0].count == 336 && app.mix.gk[1].count == 550, "counts summed over all variants: rock %ld pop %ld", app.mix.gk[0].count, app.mix.gk[1].count);

    /* rock (336) is rarer than pop (550): its ids ask the server, pop's do not */
    c = find(JF_CMD_REQUEST);
    const char *rock_ids[] = { hexid(GID(2)), hexid(GID(4)), hexid(GID(5)), hexid(GID(6)) };
    char want[JF_PATH_MAX];
    jf_path_mix_pool(want, sizeof(want), UID, rock_ids, 4, ARTID, 1980, true, 0, JF_MIX_PAGE);
    CHECK(c && cm.n == 1 && c->list == &app.lists[JF_LIST_POOL] && strcmp(c->path, want) == 0 && app.mix.phase == JF_MIX_POOL && app.mix.chosen == 0,
          "pool request: the rarer key's four ids, artist, decade, Random\n  got  %s\n  want %s", c ? c->path : "-", want);
    CHECK(c && !strstr(c->path, hexid(GID(1))) && !strstr(c->path, hexid(GID(3))) && !strstr(c->path, hexid(GID(7))),
          "pop's ids and the fifth rock id are not in the path");

    /* page 1 (40): seed, an album, a track lacking Pop, two kept in other spellings, a repeat, 34 plain keepers */
    JfItem pg1[JF_MIX_PAGE];
    pg1[0] = app.mix.seed;
    pg1[1] = mk_item(JF_ITEM_ALBUM, PID(900), "Album", "Rock\nPop\n");
    pg1[2] = mk_item(JF_ITEM_TRACK, PID(901), "OnlyRock", "Rock\n");
    pg1[3] = mk_item(JF_ITEM_TRACK, PID(1), "A", "rock\nPOP MUSIC\n");
    pg1[4] = mk_item(JF_ITEM_TRACK, PID(2), "B", "Pop\nRock\n");
    pg1[5] = pg1[3];
    for (int k = 6; k < JF_MIX_PAGE; k++) pg1[k] = mk_item(JF_ITEM_TRACK, PID(10 + (unsigned)k), "T", "Rock\nPop\n");
    r = *c;
    reply_items(&r, pg1, JF_MIX_PAGE, 5000);
    c = find(JF_CMD_REQUEST);
    CHECK(c && c->list == &app.lists[JF_LIST_POOL] && app.mix.found_n == 36 && strcmp(c->path, want) == 0 && app.busy,
          "after page 1: 36 kept (seed, album, 'Rock' only and the repeat dropped), same request again (found %d)", app.mix.found_n);

    /* page 2: a repeat from page 1 and 39 new -> 75; page 3: 40 new, the mix stops at 100 */
    JfItem pg2[JF_MIX_PAGE];
    pg2[0] = pg1[4];
    for (int k = 1; k < JF_MIX_PAGE; k++) pg2[k] = mk_item(JF_ITEM_TRACK, PID(100 + (unsigned)k), "T", "Pop\nRock\n");
    r = *c;
    reply_items(&r, pg2, JF_MIX_PAGE, 5000);
    CHECK(app.mix.found_n == 75, "page 2: the cross-page repeat dropped (found %d)", app.mix.found_n);
    c = find(JF_CMD_REQUEST);
    CHECK(c && c->list == &app.lists[JF_LIST_POOL], "a third page is asked for");
    JfItem pg3[JF_MIX_PAGE];
    for (int k = 0; k < JF_MIX_PAGE; k++) pg3[k] = mk_item(JF_ITEM_TRACK, PID(200 + (unsigned)k), "T", "Rock\nPop\n");
    r = *c;
    reply_items(&r, pg3, JF_MIX_PAGE, 5000);

    CHECK(!pool_req() && app.mix.phase == JF_MIX_OFF, "100 found: no fourth request, run over");
    CHECK(app.screen == JF_SCR_TRACKS && app.nav_n == nav_before, "the tick screen is popped (screen %d nav %d, want %d)", app.screen, app.nav_n, nav_before);
    CHECK(app.queue_n == 101 && queue_unique(), "queue = seed + 100 matches, each once (n %d)", app.queue_n);
    CHECK(strcmp(app.queue[0].id, app.lists[JF_LIST_TRACKS].items[1].id) == 0, "the seed leads");
    int bad = 0;
    for (int k = 1; k < app.queue_n; k++) {
        const char *id = app.queue[k].id;
        if (!strcmp(id, hexid(PID(900))) || !strcmp(id, hexid(PID(901))) || !strcmp(id, app.queue[0].id)) bad++;
    }
    CHECK(bad == 0, "no album, no Rock-only track, no second seed in the queue (%d)", bad);
    int inorder = 0;
    for (int k = 2; k < app.queue_n; k++) if (strcmp(app.queue[k - 1].id, app.queue[k].id) < 0) inorder++;
    CHECK(inorder < app.queue_n - 2, "the matches are shuffled, not in page order");
    CHECK(app.queue_cur == 0 && has(JF_CMD_PLAY_STREAM) && strstr(find(JF_CMD_PLAY_STREAM)->path, app.queue[0].id) && !has(JF_CMD_STOP),
          "the seed starts (nothing was playing: no STOP)");
    CHECK(strcmp(app.queue_name, "Similar: Rock+Pop Music+The Band (1980s)") == 0, "queue_name '%s'", app.queue_name);
}

static void test_mix_choice(void)
{
    char want[JF_PATH_MAX];
    const char *r1[] = { hexid(GID(1)) }, *p1[] = { hexid(GID(2)) };

    mix_run(100, 900);                   /* Rock is rarer, and small: stable order from the start */
    const JfCmd *c = pool_req();
    jf_path_mix_pool(want, sizeof(want), UID, r1, 1, NULL, 0, false, 0, JF_MIX_PAGE);
    CHECK(c && strcmp(c->path, want) == 0, "rock 100 < pop 900: rock's id, SortName, start 0\n  got  %s\n  want %s", c ? c->path : "-", want);
    CHECK(c && !strstr(c->path, hexid(GID(2))), "pop's id is not asked for");

    mix_run(900, 100);
    c = pool_req();
    jf_path_mix_pool(want, sizeof(want), UID, p1, 1, NULL, 0, false, 0, JF_MIX_PAGE);
    CHECK(c && strcmp(c->path, want) == 0, "pop 100 < rock 900: pop's id\n  got  %s\n  want %s", c ? c->path : "-", want);
    CHECK(c && !strstr(c->path, hexid(GID(1))), "rock's id is not asked for");

    mix_run(500, 900);
    c = pool_req();
    jf_path_mix_pool(want, sizeof(want), UID, r1, 1, NULL, 0, true, 0, JF_MIX_PAGE);
    CHECK(c && strcmp(c->path, want) == 0, "a count over %d starts in Random order", JF_MIX_SMALL);
    mix_run(JF_MIX_SMALL, 900);
    c = pool_req();
    CHECK(c && strstr(c->path, "SortBy=SortName"), "a count of exactly %d starts stable", JF_MIX_SMALL);
    mix_run(JF_MIX_SMALL + 1, 900);
    c = pool_req();
    CHECK(c && strstr(c->path, "SortBy=Random"), "a count of %d starts Random", JF_MIX_SMALL + 1);

    /* a ticked key the index does not have: nothing can match, no pool request */
    mix_open("Rock\nPop\n", 0, false);
    send(JF_EV_MIX_START, 0);
    JfCmd r = *find(JF_CMD_REQUEST);
    struct gs only_rock[1] = { { "Rock", GID(1), 10 } };
    reply_genre_page(&r, 0, 3, only_rock, 1);
    CHECK(!pool_req() && cm.n == 0 && app.screen == JF_SCR_MIX && app.mix.phase == JF_MIX_TICKS && strcmp(app.banner, "NO MIX FOUND") == 0 && !app.busy,
          "a ticked genre the server lacks: NO MIX FOUND on the tick screen, no pool request (banner '%s', n %d)", app.banner, cm.n);

    /* artist/decade only: straight to the pool, no index */
    mix_open("Rock\n", 1984, true);
    send(JF_EV_MIX_TOGGLE, 0); send(JF_EV_MIX_TOGGLE, 1);
    send(JF_EV_MIX_START, 0);
    c = find(JF_CMD_REQUEST);
    jf_path_mix_pool(want, sizeof(want), UID, NULL, 0, ARTID, 0, true, 0, JF_MIX_PAGE);
    CHECK(c && cm.n == 1 && c->list == &app.lists[JF_LIST_POOL] && strcmp(c->path, want) == 0 && app.mix.phase == JF_MIX_POOL,
          "artist only: pool directly\n  got  %s\n  want %s", c ? c->path : "-", want);
    mix_open("Rock\n", 1984, true);
    send(JF_EV_MIX_TOGGLE, 0); send(JF_EV_MIX_TOGGLE, 2);
    send(JF_EV_MIX_START, 0);
    c = find(JF_CMD_REQUEST);
    jf_path_mix_pool(want, sizeof(want), UID, NULL, 0, NULL, 1980, true, 0, JF_MIX_PAGE);
    CHECK(c && cm.n == 1 && strcmp(c->path, want) == 0, "decade only: Years list, no ids\n  got  %s\n  want %s", c ? c->path : "-", want);
}

static void test_mix_stops(void)
{
    /* request cap: 5 keepers per page, never 100 -> exactly JF_MIX_REQ_MAX requests, then what was found */
    mix_run(500, 900);
    pg_reset(5000, JF_MIX_PAGE, 5, false);
    int reqs = pg_run(JF_MIX_REQ_MAX + 4);
    CHECK(reqs == JF_MIX_REQ_MAX, "request cap: %d pool requests, want %d", reqs, JF_MIX_REQ_MAX);
    CHECK(!pool_req() && app.queue_n == 1 + 5 * JF_MIX_REQ_MAX && has(JF_CMD_PLAY_STREAM), "then the %d found play (queue %d)", 5 * JF_MIX_REQ_MAX, app.queue_n);
    CHECK(queue_unique() && strcmp(app.queue_name, "Similar: Rock+Pop") == 0, "name '%s'", app.queue_name);

    /* an empty page ends it */
    mix_run(500, 900);
    pg_reset(5000, JF_MIX_PAGE, 10, false);
    pg_run(1);
    CHECK(pool_req() != NULL, "10 found, the second page is asked for");
    JfCmd c = *pool_req();
    reply_items(&c, NULL, 0, 5000);
    CHECK(!pool_req() && app.queue_n == 11 && app.mix.phase == JF_MIX_OFF, "an empty page ends the run with what was found (queue %d)", app.queue_n);

    /* a short random page: the whole answer was in it */
    mix_run(500, 900);
    pg_reset(5000, 17, 17, false);
    reqs = pg_run(5);
    CHECK(reqs == 1 && app.queue_n == 18 && !pool_req(), "random page of 17 < %d: one request, 17 found (reqs %d queue %d)", JF_MIX_PAGE, reqs, app.queue_n);

    /* a full random page that is the whole total */
    mix_run(500, 900);
    pg_reset(JF_MIX_PAGE, JF_MIX_PAGE, JF_MIX_PAGE, false);
    reqs = pg_run(5);
    CHECK(reqs == 1 && app.queue_n == 1 + JF_MIX_PAGE, "random page with loaded == total: one request (reqs %d queue %d)", reqs, app.queue_n);

    /* a random page of a small total: switch once to stable order, from 0, and the repeats are not doubled */
    mix_run(500, 900);
    pg_reset(90, JF_MIX_PAGE, JF_MIX_PAGE, true);
    reqs = pg_run(10);
    CHECK(reqs == 4, "1 random + 3 stable requests (got %d)", reqs);
    CHECK(pg.rnd[0] && !pg.rnd[1] && !pg.rnd[2] && !pg.rnd[3], "the first is Random, the rest SortName");
    CHECK(pg.starts[0] == 0 && pg.starts[1] == 0 && pg.starts[2] == 40 && pg.starts[3] == 80,
          "starts %d %d %d %d: stable paging restarts at 0 then follows what is consumed", pg.starts[0], pg.starts[1], pg.starts[2], pg.starts[3]);
    CHECK(!pool_req() && app.queue_n == 91 && queue_unique(), "consumed 90 >= total 90 ends it: 90 distinct found (queue %d)", app.queue_n);

    /* stable from the index */
    mix_run(100, 900);
    pg_reset(100, JF_MIX_PAGE, JF_MIX_PAGE, true);
    reqs = pg_run(10);
    CHECK(reqs == 3 && pg.starts[0] == 0 && pg.starts[1] == 40 && pg.starts[2] == 80 && !pg.rnd[0], "stable run: 3 requests at 0, 40, 80 (got %d)", reqs);
    CHECK(app.queue_n == 101 && queue_unique(), "all 100 found (queue %d)", app.queue_n);

    /* stable, total 70: consumed 80 >= 70 on the second page */
    mix_run(100, 900);
    pg_reset(70, JF_MIX_PAGE, JF_MIX_PAGE, true);
    reqs = pg_run(10);
    CHECK(reqs == 2 && !pool_req() && app.queue_n == 71, "stable run of 70: two requests (got %d, queue %d)", reqs, app.queue_n);

    /* nothing found: banner, stay on the ticks, queue untouched */
    mix_run(500, 900);
    int qn = app.queue_n;
    JfItem only[2];
    only[0] = app.mix.seed;
    only[1] = mk_item(JF_ITEM_TRACK, PID(5), "OnlyRock", "Rock\n");
    JfCmd p = *pool_req();
    reply_items(&p, only, 2, 2);
    CHECK(strcmp(app.banner, "NO MIX FOUND") == 0 && app.screen == JF_SCR_MIX && app.mix.phase == JF_MIX_TICKS && !app.busy && cm.n == 0 && app.queue_n == qn,
          "0 found: NO MIX FOUND, tick screen kept, no commands (banner '%s' screen %d n %d)", app.banner, app.screen, cm.n);
    send(JF_EV_MIX_START, 0);
    CHECK(app.mix.phase == JF_MIX_INDEX && find(JF_CMD_REQUEST) && find(JF_CMD_REQUEST)->list == &app.lists[JF_LIST_GENRES], "START again runs it again");
}

static void test_mix_cancel_and_errors(void)
{
    /* BACK during the pool: cancel, stay on the ticks; a late reply is ignored; BACK again leaves */
    mix_run(500, 900);
    JfCmd late = *pool_req();
    send(JF_EV_BACK, 0);
    CHECK(app.mix.phase == JF_MIX_TICKS && app.screen == JF_SCR_MIX && !app.busy, "BACK in the pool cancels the run (phase %d screen %d)", app.mix.phase, app.screen);
    const JfCmd *cc = find(JF_CMD_CANCEL);
    CHECK(cc && cc->kind == JF_KIND_LIST && !pool_req() && !has(JF_CMD_PLAY_STREAM), "a CANCEL, no request, no playback");
    JfItem pg1[JF_MIX_PAGE];
    for (int k = 0; k < JF_MIX_PAGE; k++) pg1[k] = mk_item(JF_ITEM_TRACK, PID((unsigned)k + 1), "T", "Rock\nPop\n");
    reply_items(&late, pg1, JF_MIX_PAGE, 5000);
    CHECK(cm.n == 0 && app.queue_n == 0 && app.screen == JF_SCR_MIX && app.mix.phase == JF_MIX_TICKS, "the stale pool reply changes nothing (n %d queue %d)", cm.n, app.queue_n);
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_TRACKS && app.mix.phase == JF_MIX_OFF, "the second BACK leaves");

    /* BACK during the index */
    mix_open("Rock\nPop\n", 0, false);
    send(JF_EV_MIX_START, 0);
    JfCmd idx = *find(JF_CMD_REQUEST);
    send(JF_EV_BACK, 0);
    CHECK(app.mix.phase == JF_MIX_TICKS && app.screen == JF_SCR_MIX && has(JF_CMD_CANCEL) && !find(JF_CMD_REQUEST), "BACK in the index cancels too");
    JfItem g[2] = { mk_item(JF_ITEM_GENRE, GID(1), "Rock", NULL), mk_item(JF_ITEM_GENRE, GID(2), "Pop", NULL) };
    reply_items(&idx, g, 2, 2);
    CHECK(cm.n == 0 && app.mix.phase == JF_MIX_TICKS, "the stale index reply starts nothing");

    /* errors */
    mix_run(500, 900);
    JfCmd pr = *pool_req();
    reply_raw(&pr, JF_ERR_SERVER);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && app.mix.phase == JF_MIX_TICKS && app.screen == JF_SCR_MIX && !app.busy,
          "pool error: overlay with CLOSE only, back to the ticks (buttons %u phase %d)", app.overlay_buttons, app.mix.phase);
    send(JF_EV_RETRY, 0);
    CHECK(cm.n == 0 && app.overlay, "RETRY does nothing");
    send(JF_EV_CLOSE_ERROR, 0);
    CHECK(!app.overlay && app.screen == JF_SCR_MIX, "CLOSE returns to the ticks");

    mix_open("Rock\nPop\n", 0, false);
    send(JF_EV_MIX_START, 0);
    JfCmd ir = *find(JF_CMD_REQUEST);
    reply_raw(&ir, JF_ERR_UNREACHABLE);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && app.mix.phase == JF_MIX_TICKS, "index error: CLOSE only (buttons %u)", app.overlay_buttons);

    in_tracks(4, 4);
    send(JF_EV_MIX, 1);
    JfCmd sr = *find(JF_CMD_REQUEST);
    reply_raw(&sr, JF_ERR_NOTFOUND);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && app.mix.phase == JF_MIX_OFF && app.screen == JF_SCR_TRACKS, "seed error: CLOSE only, MIX off");

    mix_run(500, 900);
    pr = *pool_req();
    reply_raw(&pr, JF_ERR_UNAUTH);
    CHECK(app.screen == JF_SCR_SIGNIN && app.mix.phase == JF_MIX_OFF, "401 in the pool: signed out, MIX off");
}

static void test_mix_nowplaying(void)
{
    in_tracks(4, 4);
    start_playing(0);
    send(JF_EV_OPEN_NOWPLAYING, 0);
    CHECK(app.screen == JF_SCR_NOWPLAYING, "now playing");
    send(JF_EV_MIX, -1);
    const JfCmd *c = find(JF_CMD_REQUEST);
    char want[JF_PATH_MAX];
    jf_path_item_by_id(want, sizeof(want), UID, app.queue[app.queue_cur].id);
    CHECK(c && c->list == &app.lists[JF_LIST_SEED] && strcmp(c->path, want) == 0, "i = -1 seeds with the playing track (%s)", c ? c->path : "-");
    JfCmd sr = *c;
    JfItem s = app.queue[app.queue_cur];
    snprintf(s.genres, sizeof(s.genres), "Rock\n");
    s.year = 1984;
    reply_items(&sr, &s, 1, 1);
    CHECK(app.screen == JF_SCR_MIX && app.mix.opt_n == 2, "tick screen from NOW PLAYING (screen %d n %d)", app.screen, app.mix.opt_n);
    send(JF_EV_MIX_TOGGLE, 1);
    send(JF_EV_MIX_START, 0);
    JfCmd ir = *find(JF_CMD_REQUEST);
    struct gs sp[1] = { { "Rock", GID(1), 400 } };
    reply_genre_page(&ir, 0, 1, sp, 1);
    pg_reset(5000, JF_MIX_PAGE, JF_MIX_PAGE, false);
    pg_run(3);
    CHECK(app.screen == JF_SCR_NOWPLAYING && app.queue_n == 101 && app.queue_cur == 0, "the run pops back to NOW PLAYING with a 101-track queue (screen %d queue %d)", app.screen, app.queue_n);
    CHECK(cm.n == 2 && cm.c[0].type == JF_CMD_STOP && cm.c[1].type == JF_CMD_PLAY_STREAM, "audio was live: STOP, then the seed streams");
    CHECK(strcmp(app.queue_name, "Similar: Rock (1980s)") == 0, "queue_name '%s'", app.queue_name);
}

static void test_save(void)
{
    in_tracks(5, 5);
    send_text(JF_EV_SAVE_PLAYLIST, "x");
    CHECK(cm.n == 0, "no save on an empty queue");
    start_playing(0);
    send_text(JF_EV_SAVE_PLAYLIST, "");
    CHECK(cm.n == 0, "no save without a name");
    send_text(JF_EV_SAVE_PLAYLIST, "My List");
    const JfCmd *c = find(JF_CMD_REQUEST);
    CHECK(c && cm.n == 1 && c->kind == JF_KIND_SAVE && strcmp(c->name, "My List") == 0 && c->n == 5 && c->tag != 0,
          "SAVE command: kind, name, n = queue size");
    CHECK(c && app.busy && app.retry.kind != JF_KIND_SAVE, "busy while the POST is out; never stored for RETRY");
    unsigned tag = c->tag;
    send_text(JF_EV_SAVE_PLAYLIST, "Other");
    CHECK(cm.n == 0, "a second save is ignored while one is pending");
    save_result(tag + 7, JF_ERR_NONE, 200);
    CHECK(app.busy && !app.overlay, "a result with another tag is ignored");
    save_result(tag, JF_ERR_NONE, 200);
    CHECK(!app.busy && strcmp(app.banner, "SAVED: My List") == 0, "success: banner (%s)", app.banner);

    fake_queue(150);
    send_text(JF_EV_SAVE_PLAYLIST, "Big");
    c = find(JF_CMD_REQUEST);
    CHECK(c && c->n == JF_SAVE_IDS_MAX, "n capped at %d (got %d)", JF_SAVE_IDS_MAX, c ? c->n : -1);
    tag = c ? c->tag : 0;
    save_result(tag, JF_ERR_SERVER, 500);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && !app.busy, "failure: overlay, CLOSE only");
    send(JF_EV_RETRY, 0);
    CHECK(cm.n == 0 && app.overlay, "RETRY does nothing for a save");
    send(JF_EV_CLOSE_ERROR, 0);
    send_text(JF_EV_SAVE_PLAYLIST, "Again");
    c = find(JF_CMD_REQUEST);
    CHECK(c && c->kind == JF_KIND_SAVE && c->tag != tag, "a new save after a failure is allowed, with a new tag");
    save_result(c ? c->tag : 0, JF_ERR_UNAUTH, 401);
    CHECK(app.screen == JF_SCR_SIGNIN && !app.busy, "save 401 -> session expired");
}

/* ── pause / stop / seek, and the INSTANT MIX confirmation ─────────────── */

static void send_pos(JfEventType t, int ms)
{
    static JfEvent ev; memset(&ev, 0, sizeof(ev)); ev.type = t; ev.played_ms = ms; step_ev(&ev);
}
static void playing_album(void)
{
    in_tracks(4, 4); start_playing(0);
    for (int k = 0; k < app.queue_n; k++) app.queue[k].runtime_s = 200;
}

static void test_pause_stop_seek(void)
{
    playing_album();
    send_pos(JF_EV_PAUSE, 42000);
    CHECK(app.play_state == JF_PLAY_PAUSED && app.pos_ms == 42000 && cm.n == 1 && cm.c[0].type == JF_CMD_STOP, "pause: STOP only, held at 42 s");
    CHECK(app.queue_cur == 0 && app.queue_n == 4 && !app.busy && jf_position_ms(&app, 777) == 42000, "pause keeps the queue; position is held");
    send_played(1000);
    CHECK(cm.n == 0 && app.play_state == JF_PLAY_PAUSED, "a stale TRACK_ENDED is ignored while paused");
    send(JF_EV_PAUSE, 0);
    const JfCmd *c = find(JF_CMD_PLAY_STREAM);
    CHECK(cm.n == 1 && c && strstr(c->path, "StartTimeTicks=420000000") && app.play_state == JF_PLAY_FETCHING && app.start_ms == 42000,
          "resume: one stream request at 42 s (%s)", c ? c->path : "-");
    reply_stream(c->tag, JF_ERR_NONE, 41); send(JF_EV_STREAM_STARTED, 0);
    CHECK(app.play_state == JF_PLAY_PLAYING && jf_position_ms(&app, 3000) == 45000, "playing again, position = start + elapsed");

    send(JF_EV_STOP, 0);
    CHECK(app.play_state == JF_PLAY_STOPPED && cm.n == 1 && cm.c[0].type == JF_CMD_STOP && app.queue_n == 4 && app.queue_cur == 0 && jf_position_ms(&app, 9) == 0,
          "stop: STOP, queue and index kept, position 0");
    send(JF_EV_STOP, 0);
    CHECK(cm.n == 0, "stop twice does nothing");
    send(JF_EV_PAUSE, 0);
    c = find(JF_CMD_PLAY_STREAM);
    CHECK(c && !strstr(c->path, "StartTimeTicks") && app.play_state == JF_PLAY_FETCHING && app.queue_cur == 0, "play after stop restarts the track from 0");

    send(JF_EV_STOP, 0);                       /* while FETCHING */
    CHECK(app.play_state == JF_PLAY_STOPPED && has(JF_CMD_STOP) && has(JF_CMD_CANCEL), "stop while loading cancels the request");
    reply_stream(c->tag, JF_ERR_NONE, 43);
    CHECK(has(JF_CMD_CLOSE_FD) && !has(JF_CMD_START_AUDIO), "its late stream is closed, not played");

    playing_album();
    send(JF_EV_NEXT, 0); send_pos(JF_EV_PAUSE, 0);          /* pause while FETCHING holds the start offset */
    CHECK(app.play_state == JF_PLAY_PAUSED && app.pos_ms == 0 && has(JF_CMD_CANCEL) && has(JF_CMD_STOP), "pause while loading");
    send(JF_EV_NEXT, 0);
    CHECK(app.queue_cur == 2 && cm.n == 1 && cm.c[0].type == JF_CMD_PLAY_STREAM && app.start_ms == 0, "next while paused starts the next track from 0, no STOP");

    jf_app_init(&app);
    send(JF_EV_PAUSE, 0); send(JF_EV_STOP, 0);
    CHECK(cm.n == 0, "pause / stop with an empty queue do nothing");

    /* seek */
    playing_album();
    send(JF_EV_SEEK, 90000);
    c = find(JF_CMD_PLAY_STREAM);
    CHECK(cm.n == 2 && cm.c[0].type == JF_CMD_STOP && c && strstr(c->path, "StartTimeTicks=900000000") && app.start_ms == 90000 && app.play_state == JF_PLAY_FETCHING,
          "seek: STOP then a stream at 90 s");
    reply_stream(c->tag, JF_ERR_NONE, 44); send(JF_EV_STREAM_STARTED, 0);
    send(JF_EV_SEEK, 99999999);
    c = find(JF_CMD_PLAY_STREAM);
    CHECK(c && strstr(c->path, "StartTimeTicks=1999990000") && app.start_ms == 199999, "seek clamps below the runtime (%d)", app.start_ms);
    reply_stream(c->tag, JF_ERR_NONE, 45); send(JF_EV_STREAM_STARTED, 0);
    send(JF_EV_SEEK, -5);
    c = find(JF_CMD_PLAY_STREAM);
    CHECK(c && !strstr(c->path, "StartTimeTicks") && app.start_ms == 0, "seek clamps at 0");
    reply_stream(c->tag, JF_ERR_NONE, 46); send(JF_EV_STREAM_STARTED, 0);
    send(JF_EV_SEEK, 199000);
    reply_stream(find(JF_CMD_PLAY_STREAM)->tag, JF_ERR_NONE, 47); send(JF_EV_STREAM_STARTED, 0);
    send_played(900);
    CHECK(app.fail_streak == 0 && app.queue_cur == 1 && has(JF_CMD_PLAY_STREAM), "a seek to the last second that ends fast is no failure; next track");
    send_played(900); send_played(900);
    send(JF_EV_NEXT, 0);
    app.fail_streak = 0;
    reply_stream(find(JF_CMD_PLAY_STREAM)->tag, JF_ERR_NONE, 48); send(JF_EV_STREAM_STARTED, 0);
    send_played(900);
    CHECK(app.fail_streak == 1, "a normal track that ends in under 5 s still counts as failed");

    playing_album();
    send_pos(JF_EV_PAUSE, 10000);
    send(JF_EV_SEEK, 120000);
    CHECK(cm.n == 0 && app.pos_ms == 120000 && app.play_state == JF_PLAY_PAUSED, "seek while paused only moves the position");
    send(JF_EV_PAUSE, 0);
    CHECK(strstr(find(JF_CMD_PLAY_STREAM)->path, "StartTimeTicks=1200000000") != NULL, "resume starts at the seeked position");
    send(JF_EV_STOP, 0); send(JF_EV_SEEK, 5000);
    CHECK(cm.n == 0 && app.play_state == JF_PLAY_STOPPED, "seek while stopped is ignored");
    playing_album(); app.queue[0].runtime_s = 0; send(JF_EV_SEEK, 5000);
    CHECK(cm.n == 0, "seek without a known runtime is ignored");
}

static void test_instant_confirm(void)
{
    playing_album();
    send(JF_EV_SELECT, 2);
    send(JF_EV_INSTANT_MIX, 2);
    CHECK(cm.n == 0 && app.confirm == JF_CONFIRM_INSTANT_MIX && !app.busy && app.queue_n == 4 && app.play_state == JF_PLAY_PLAYING,
          "non-empty queue: a question, no request");
    char id[40]; snprintf(id, sizeof id, "%s", app.lists[JF_LIST_TRACKS].items[2].id);
    send(JF_EV_BACK, 0);
    CHECK(cm.n == 0 && app.confirm == JF_CONFIRM_NONE && app.screen == JF_SCR_TRACKS && app.queue_n == 4 && app.play_state == JF_PLAY_PLAYING,
          "BACK cancels and is spent (no navigation)");
    send(JF_EV_INSTANT_MIX, -1); send(JF_EV_DISMISS, 0);
    CHECK(cm.n == 0 && app.confirm == JF_CONFIRM_NONE, "DISMISS cancels");
    send(JF_EV_CONFIRM, 0);
    CHECK(cm.n == 0, "CONFIRM with nothing pending does nothing");
    send(JF_EV_INSTANT_MIX, 2); send(JF_EV_VOL_UP, 0);
    CHECK(app.confirm == JF_CONFIRM_INSTANT_MIX, "volume keeps the question");
    send(JF_EV_CONFIRM, 0);
    const JfCmd *c = find(JF_CMD_REQUEST);
    CHECK(c && c->list == &app.lists[JF_LIST_MIX] && strstr(c->path, id) && app.confirm == JF_CONFIRM_NONE && app.queue_n == 4,
          "CONFIRM fetches with the stored seed; queue untouched until the reply");
    in_tracks(4, 4);
    send(JF_EV_INSTANT_MIX, 1);
    CHECK(has(JF_CMD_REQUEST) && app.confirm == JF_CONFIRM_NONE, "empty queue: no question");
}

/* BACK returns to the list as it was: the UI reports the view, the core hands it back on each pop. */
static void test_back_restores_view(void)
{
    signed_in(); artists_loaded(50, 50);
    jf_note_view(&app, 17, 21);
    send(JF_EV_ACTIVATE, 21);
    JfCmd a = *find(JF_CMD_REQUEST); reply_list(&a, JF_ITEM_ALBUM, 30, 30);
    jf_note_view(&app, 0, 0);                         /* the new screen starts at its top */
    jf_note_view(&app, 5, 9);
    unsigned seq = app.restore_seq;
    send(JF_EV_ACTIVATE, 9);
    CHECK(app.screen == JF_SCR_TRACKS, "album -> TRACKS");
    jf_note_view(&app, 0, 2);
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ALBUMS && app.restore_seq == seq + 1 && app.restore_top == 5 && app.restore_cursor == 9,
          "BACK to ALBUMS restores %d/%d (seq %u)", app.restore_top, app.restore_cursor, app.restore_seq);
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_ARTISTS && app.restore_seq == seq + 2 && app.restore_top == 17 && app.restore_cursor == 21,
          "BACK to ARTISTS restores %d/%d", app.restore_top, app.restore_cursor);
    /* a song played from TRACKS: NOW PLAYING pushes TRACKS, BACK restores it */
    in_tracks(40, 40);
    jf_note_view(&app, 12, 15);
    send(JF_EV_ACTIVATE, 15);
    send(JF_EV_OPEN_NOWPLAYING, 0);
    CHECK(app.screen == JF_SCR_NOWPLAYING, "now playing");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_TRACKS && app.restore_top == 12 && app.restore_cursor == 15,
          "BACK from NOW PLAYING restores %d/%d", app.restore_top, app.restore_cursor);
}



/* PREV, SHUFFLE and QUEUE_MOVE */
static uint32_t lcg_state;
static uint32_t lcg_draw(void *ctx) { (void)ctx; lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state; }
static uint32_t fixed_draw(void *ctx) { return *(uint32_t *)ctx; }

static int qpos(const char *name)
{
    for (int k = 0; k < app.queue_n; k++) if (strcmp(app.queue[k].name, name) == 0) return k;
    return -1;
}

static void test_prev(void)
{
    playing_album();
    send(JF_EV_NEXT, 0); send(JF_EV_NEXT, 0);
    CHECK(app.queue_cur == 2, "NEXT twice -> entry 2");
    send_pos(JF_EV_PREV, 1000);
    CHECK(app.queue_cur == 1 && has(JF_CMD_PLAY_STREAM) && app.start_ms == 0, "PREV within 3 s -> the previous entry (cur %d)", app.queue_cur);
    send_pos(JF_EV_PREV, 3001);
    CHECK(app.queue_cur == 1 && has(JF_CMD_PLAY_STREAM) && app.start_ms == 0, "PREV past 3 s restarts the track (cur %d)", app.queue_cur);
    send_pos(JF_EV_PREV, 3000);
    CHECK(app.queue_cur == 0, "PREV at exactly 3 s still goes back (cur %d)", app.queue_cur);
    send_pos(JF_EV_PREV, 500);
    CHECK(app.queue_cur == 0 && has(JF_CMD_PLAY_STREAM), "PREV on the first entry restarts it");
}

static void test_queue_edit(void)
{
    /* move: the playing track keeps playing wherever the others go */
    fake_queue(5);
    CHECK(jf_queue_move(&app, 3, 1) && qpos("q3") == 1 && qpos("q1") == 2 && qpos("q4") == 4 && app.queue_cur == 0, "move up past none of cur");
    app.queue_cur = 2;                              /* q1 plays */
    CHECK(jf_queue_move(&app, 3, 0) && app.queue_cur == 3 && strcmp(app.queue[app.queue_cur].name, "q1") == 0, "an entry moved above cur pushes cur down");
    CHECK(jf_queue_move(&app, 0, 4) && app.queue_cur == 2 && strcmp(app.queue[app.queue_cur].name, "q1") == 0, "an entry moved from above cur to below pulls cur up");
    CHECK(jf_queue_move(&app, app.queue_cur, 0) && app.queue_cur == 0 && strcmp(app.queue[0].name, "q1") == 0, "moving the playing entry carries cur");
    CHECK(!jf_queue_move(&app, 0, 5) && !jf_queue_move(&app, -1, 2) && app.queue_n == 5, "out of range refused");
    /* through the core, only on the QUEUE screen */
    fake_queue(4);
    static JfEvent ev; memset(&ev, 0, sizeof ev); ev.type = JF_EV_QUEUE_MOVE; ev.i = 2; ev.j = 1;
    app.screen = JF_SCR_TRACKS; step_ev(&ev);
    CHECK(qpos("q2") == 2, "QUEUE_MOVE off the queue screen does nothing");
    app.screen = JF_SCR_QUEUE; step_ev(&ev);
    CHECK(qpos("q2") == 1 && cm.n == 0, "QUEUE_MOVE reorders and sends no command");

    /* shuffle while playing: cur to position 0, a permutation, deterministic under a seed */
    fake_queue(8); app.queue_cur = 5;
    lcg_state = 12345;
    jf_queue_shuffle(&app, lcg_draw, NULL);
    int seen = 0;
    for (int k = 0; k < 8; k++) { char nm[8]; snprintf(nm, sizeof nm, "q%d", k); int p = qpos(nm); if (p >= 0) seen |= 1 << p; }
    CHECK(seen == 0xFF && app.queue_n == 8, "shuffle keeps every entry once");
    CHECK(app.queue_cur == 0 && strcmp(app.queue[0].name, "q5") == 0, "the playing entry is at 0 and still the current one");
    bool moved = false;
    for (int k = 1; k < 8; k++) { char nm[8]; snprintf(nm, sizeof nm, "q%d", k < 5 ? k - 1 : k); if (strcmp(app.queue[k].name, nm) != 0) moved = true; }
    CHECK(moved, "the rest is actually reordered");
    /* rng stuck at 0 -> every pick is index 0 of its range: the order is a fixed rotation, so the bound is right */
    fake_queue(4); uint32_t zero = 0;
    jf_queue_shuffle(&app, fixed_draw, &zero);
    CHECK(strcmp(app.queue[0].name, "q0") == 0 && strcmp(app.queue[3].name, "q1") == 0, "draw 0 picks the lowest of each range");
    uint32_t top = 0xFFFFFFFFu;
    CHECK(jf_rand_below(fixed_draw, &top, 7) == 6 && jf_rand_below(fixed_draw, &zero, 7) == 0 && jf_rand_below(fixed_draw, &top, 1) == 0, "multiply-shift bound stays in [0,n)");

    /* shuffle with nothing playing: all shuffled, cur follows its entry */
    fake_queue(8); app.play_state = JF_PLAY_STOPPED; app.queue_cur = 5;
    lcg_state = 99;
    jf_queue_shuffle(&app, lcg_draw, NULL);
    CHECK(strcmp(app.queue[app.queue_cur].name, "q5") == 0, "stopped: cur follows q5 to %d", app.queue_cur);
    /* through the core */
    fake_queue(6); app.screen = JF_SCR_QUEUE; app.queue_cur = 3;
    static JfEvent sh; memset(&sh, 0, sizeof sh); sh.type = JF_EV_SHUFFLE; step_ev(&sh);
    CHECK(app.queue_cur == 0 && strcmp(app.queue[0].name, "q3") == 0 && app.play_state == JF_PLAY_PLAYING && cm.n == 0, "SHUFFLE event: playing track first, no restart");
}

/* REMOVE: the selected queue row goes.  Before cur: cur follows its entry; after cur: untouched; at cur: the next entry plays
 * (or playback ends on the last one, or the queue clears when it was the only one).  The saved queue needs no event: jellyfin.c
 * hashes count, cur and ids each frame, and each of these changes one of them. */
static void queue_screen(int n)
{
    in_tracks(5, 5);
    fake_queue(n);
    app.screen = JF_SCR_QUEUE;
    cm.n = 0;
}
static void remove_at(int i)
{
    static JfEvent ev; memset(&ev, 0, sizeof ev); ev.type = JF_EV_QUEUE_REMOVE; ev.i = i;
    step_ev(&ev);
}
static int count_cmds(JfCmdType t) { int c = 0; for (int k = 0; k < cm.n; k++) if (cm.c[k].type == t) c++; return c; }

static void test_queue_remove(void)
{
    queue_screen(5); app.queue_cur = 3;
    remove_at(1);
    CHECK(app.queue_n == 4 && app.queue_cur == 2 && strcmp(app.queue[2].name, "q3") == 0 && qpos("q1") < 0 && cm.n == 0,
          "remove before cur: cur follows its entry (cur %d n %d)", app.queue_cur, app.queue_n);

    queue_screen(5); app.queue_cur = 1;
    remove_at(3);
    CHECK(app.queue_n == 4 && app.queue_cur == 1 && strcmp(app.queue[1].name, "q1") == 0 && qpos("q3") < 0 && cm.n == 0,
          "remove after cur: cur unchanged, no command (cur %d)", app.queue_cur);

    queue_screen(4); app.queue_cur = 1;
    remove_at(1);
    CHECK(app.queue_n == 3 && app.queue_cur == 1 && strcmp(app.queue[1].name, "q2") == 0 && app.play_state == JF_PLAY_FETCHING &&
          count_cmds(JF_CMD_STOP) == 1 && count_cmds(JF_CMD_PLAY_STREAM) == 1 && cm.n <= 2,
          "remove cur plays the entry that slid into its place (cur %d state %d cmds %d)", app.queue_cur, app.play_state, cm.n);

    queue_screen(3); app.queue_cur = 2;
    remove_at(2);
    CHECK(app.queue_n == 2 && app.queue_cur == 1 && app.play_state == JF_PLAY_END && count_cmds(JF_CMD_STOP) == 1 &&
          count_cmds(JF_CMD_PLAY_STREAM) == 0 && cm.n <= 2,
          "remove the last cur ends playback on the new last entry (cur %d state %d)", app.queue_cur, app.play_state);

    queue_screen(1); app.queue_cur = 0;
    remove_at(0);
    CHECK(app.queue_n == 0 && app.queue_cur == 0 && app.play_state == JF_PLAY_IDLE && has(JF_CMD_STOP) && app.screen != JF_SCR_QUEUE,
          "remove the only entry clears the queue and leaves the screen (screen %d)", app.screen);

    queue_screen(4); app.queue_cur = 2;
    app.screen = JF_SCR_TRACKS;
    remove_at(1);
    CHECK(app.queue_n == 4 && app.queue_cur == 2 && cm.n == 0, "REMOVE off the QUEUE screen does nothing");

    queue_screen(4); app.queue_cur = 2;
    remove_at(-1); remove_at(4); remove_at(99);
    CHECK(app.queue_n == 4 && app.queue_cur == 2 && cm.n == 0 && !jf_queue_remove(&app, -1) && !jf_queue_remove(&app, 4),
          "a bad index removes nothing");

    queue_screen(4); app.queue_cur = 1; app.play_state = JF_PLAY_FETCHING;
    remove_at(1);
    CHECK(count_cmds(JF_CMD_CANCEL) == 0 && count_cmds(JF_CMD_STOP) == 1 && count_cmds(JF_CMD_PLAY_STREAM) == 1 &&
          app.queue_cur == 1 && app.play_state == JF_PLAY_FETCHING,
          "removing cur while FETCHING starts the next (the new tag supersedes the old stream)");
    queue_screen(3); app.queue_cur = 2; app.play_state = JF_PLAY_FETCHING;
    remove_at(2);
    CHECK(count_cmds(JF_CMD_CANCEL) == 1 && find(JF_CMD_CANCEL)->kind == JF_KIND_STREAM && app.play_state == JF_PLAY_END &&
          app.queue_cur == 1 && cm.n <= 2, "removing the last cur while FETCHING cancels the stream, ends");

    /* the pure half, no screen needed */
    fake_queue(3); app.queue_cur = 2;
    CHECK(jf_queue_remove(&app, 0) && app.queue_cur == 1 && app.queue_n == 2, "jf_queue_remove(0) shifts cur down");
}

/* CLR: the question first (nothing changes), then CONFIRM empties the queue, stops the music and leaves NOW PLAYING / QUEUE. */
static void test_queue_clear(void)
{
    in_tracks(5, 5);
    start_playing(0);
    send(JF_EV_OPEN_NOWPLAYING, 0); send(JF_EV_OPEN_QUEUE, 0);
    int n0 = app.queue_n;
    CHECK(app.screen == JF_SCR_QUEUE && n0 > 0, "setup: on the QUEUE screen with a queue (%d)", n0);
    send(JF_EV_QUEUE_CLEAR, 0);
    CHECK(app.confirm == JF_CONFIRM_CLEAR_QUEUE && cm.n == 0 && app.queue_n == n0 && app.play_state == JF_PLAY_PLAYING, "CLR asks first and changes nothing");
    send(JF_EV_BACK, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE && app.queue_n == n0 && app.screen == JF_SCR_QUEUE, "BACK cancels the question and is spent");
    send(JF_EV_QUEUE_CLEAR, 0); send(JF_EV_CONFIRM, 0);
    CHECK(app.queue_n == 0 && app.queue_cur == 0 && app.play_state == JF_PLAY_IDLE && has(JF_CMD_STOP), "CONFIRM empties the queue and stops the music");
    CHECK(app.screen == JF_SCR_TRACKS && app.confirm == JF_CONFIRM_NONE, "the queue and now-playing screens are left (screen %d)", app.screen);
    send(JF_EV_QUEUE_CLEAR, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE, "CLR off the QUEUE screen / with an empty queue asks nothing");
}

/* The saved queue comes back paused at its track; PLAY then streams that track from the saved position. */
static void test_queue_restore(void)
{
    signed_in();
    JfItem it[3];
    for (int k = 0; k < 3; k++) { memset(&it[k], 0, sizeof it[k]); it[k].type = JF_ITEM_TRACK; snprintf(it[k].id, sizeof it[k].id, "%032x", 0x900 + k); snprintf(it[k].name, sizeof it[k].name, "r%d", k); it[k].runtime_s = 200; }
    jf_queue_restore(&app, it, 0, 0, 0);
    CHECK(app.queue_n == 0, "restore of nothing changes nothing");
    jf_queue_restore(&app, it, 3, 99, 5000);
    CHECK(app.queue_n == 3 && app.queue_cur == 2 && app.play_state == JF_PLAY_PAUSED && app.pos_ms == 5000, "restored paused, index clamped");
    send(JF_EV_PAUSE, 0);
    const JfCmd *p = find(JF_CMD_PLAY_STREAM);
    CHECK(p && strstr(p->path, it[2].id) && app.play_state == JF_PLAY_FETCHING, "PLAY resumes the restored track");
}

/* Settings, library choice, sign-out, purge.  Sabotages: no PURGE_LOCAL / token left behind on sign-out; LIBRARY_SET without
 * SAVE_LOGIN or retarget; ParentId missing from the scoped root; LIBRARY_SET allowed with < 2 libraries; always/never purge on auth. */
static void libs_open(int n)
{
    signed_in();
    artists_loaded(3, 3);
    send(JF_EV_SETTINGS_OPEN, 0);
    CHECK(app.screen == JF_SCR_SETTINGS, "settings opens");
    const JfCmd *c = find(JF_CMD_REQUEST);
    CHECK(c && strstr(c->path, "/Views"), "opening asks for the views: %s", c ? c->path : "(none)");
    JfItem it[3];
    for (int k = 0; k < n; k++) { memset(&it[k], 0, sizeof it[k]); it[k].type = JF_ITEM_LIBRARY; snprintf(it[k].id, 33, "%032x", 0xa00 + k); snprintf(it[k].name, 96, "LIB%d", k); }
    if (c) reply_items(c, it, n, n);
}
static void test_settings(void)
{
    int rows[JF_SET_COUNT];
    libs_open(2);
    CHECK(app.lib_n == 2 && jf_settings_rows(&app, rows) == 2 && rows[0] == JF_SET_LIBRARY && rows[1] == JF_SET_SIGNOUT, "2 libraries: LIBRARY + SIGN OUT rows");
    send(JF_EV_SETTING_ACTIVATE, JF_SET_LIBRARY);
    CHECK(app.settings_page == JF_SETPAGE_LIBRARY, "LIBRARY page");
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_SETTINGS && app.settings_page == JF_SETPAGE_MAIN, "BACK on the library page returns to MAIN");
    send(JF_EV_LIBRARY_SET, 1);
    CHECK(app.screen == JF_SCR_SETTINGS && !has(JF_CMD_SAVE_LOGIN), "LIBRARY_SET on page MAIN is a no-op");
    send(JF_EV_SETTING_ACTIVATE, JF_SET_LIBRARY);
    app.queue_n = 1; app.play_state = JF_PLAY_PLAYING;
    send(JF_EV_LIBRARY_SET, 1);
    const JfCmd *r = find(JF_CMD_REQUEST);
    CHECK(has(JF_CMD_SAVE_LOGIN) && r && strstr(r->path, "&ParentId=00000000000000000000000000000a01"), "choose: saved, root scoped: %s", r ? r->path : "(none)");
    CHECK(!strcmp(app.login.library_name, "LIB1") && app.screen == JF_SCR_ARTISTS && app.nav_n == 0, "back at the root");
    CHECK(app.queue_n == 1 && app.play_state == JF_PLAY_PLAYING && !has(JF_CMD_STOP), "queue keeps playing");
    send(JF_EV_LIBRARY_SET, 0);
    CHECK(!has(JF_CMD_SAVE_LOGIN), "LIBRARY_SET off the settings screen is a no-op");
    send(JF_EV_SETTINGS_OPEN, 0);
    r = find(JF_CMD_REQUEST);
    JfItem it[2]; memset(it, 0, sizeof it);
    for (int k = 0; k < 2; k++) { it[k].type = JF_ITEM_LIBRARY; snprintf(it[k].id, 33, "%032x", 0xa00 + k); }
    if (r) reply_items(r, it, 2, 2);
    send(JF_EV_SETTING_ACTIVATE, JF_SET_LIBRARY);
    send(JF_EV_LIBRARY_SET, -1);
    CHECK(!app.login.library[0] && has(JF_CMD_SAVE_LOGIN), "ALL LIBRARIES clears the choice");
    libs_open(1);
    CHECK(jf_settings_rows(&app, rows) == 1 && rows[0] == JF_SET_SIGNOUT, "1 library: LIBRARY row absent");
    send(JF_EV_SETTING_ACTIVATE, JF_SET_LIBRARY);
    CHECK(app.settings_page == JF_SETPAGE_MAIN, "an absent row cannot be activated");
}
static void test_signout(void)
{
    libs_open(2);
    snprintf(app.login.library, 33, "%032x", 0xa01);
    app.queue_n = 2; app.play_state = JF_PLAY_PLAYING;
    send(JF_EV_SETTING_ACTIVATE, JF_SET_SIGNOUT);
    CHECK(app.confirm == JF_CONFIRM_SIGN_OUT && app.screen == JF_SCR_SETTINGS, "sign-out asks first");
    send(JF_EV_BACK, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE && app.screen == JF_SCR_SETTINGS && cm.n == 0 && app.login.token[0], "BACK cancels, nothing emitted");
    send(JF_EV_SETTING_ACTIVATE, JF_SET_SIGNOUT);
    send(JF_EV_CONFIRM, 0);
    CHECK(has(JF_CMD_STOP) && has(JF_CMD_PURGE_LOCAL) && has(JF_CMD_CLEAR_LOGIN), "STOP + PURGE_LOCAL + CLEAR_LOGIN");
    CHECK(app.screen == JF_SCR_SIGNIN && !app.login.token[0] && !app.login.user_id[0] && !app.login.library[0] && app.queue_n == 0 &&
          app.play_state == JF_PLAY_IDLE && !strcmp(app.login.server, "10.0.0.1:8096") && !strcmp(app.login.user, "bob"),
          "signed out: token, id, library, queue gone; server and user kept");
    /* sign-in as a different user purges; the same user does not */
    app.queue_n = 1; send_signin("10.0.0.1:8096", "bob", "pw");
    JfResult r; memset(&r, 0, sizeof r); r.kind = JF_KIND_AUTH; r.tag = app.auth_tag; snprintf(r.token, sizeof r.token, "t2"); snprintf(r.user_id, 33, UID);
    send_result(&r);
    CHECK(!has(JF_CMD_PURGE_LOCAL), "same server and user after sign-out: no second purge");
    signed_in();
    app.queue_n = 1; app.login.token[0] = '\0'; app.screen = JF_SCR_SIGNIN;
    send_signin("10.0.0.9:8096", "alice", "pw");
    r.tag = app.auth_tag;
    send_result(&r);
    CHECK(has(JF_CMD_PURGE_LOCAL) && app.queue_n == 0, "a different server/user purges the queue and art");
    signed_in();
    app.queue_n = 1; app.login.token[0] = '\0'; app.screen = JF_SCR_SIGNIN;
    send_signin("10.0.0.1:8096", "bob", "pw");
    r.tag = app.auth_tag;
    send_result(&r);
    CHECK(!has(JF_CMD_PURGE_LOCAL) && app.queue_n == 1, "the same server and user keeps the queue");
}

/* ── DELETE PLAYLIST ───────────────────────────────────────────────────── */

static void send_write_result(unsigned tag, JfErr e, int status)
{
    JfResult r; memset(&r, 0, sizeof r);
    r.kind = JF_KIND_WRITE; r.tag = tag; r.err = e; r.status = status; r.fd = -1;
    send_result(&r);
}
static void send_user_result(unsigned tag, JfErr e, bool admin, bool del, int folders)
{
    JfResult r; memset(&r, 0, sizeof r);
    r.kind = JF_KIND_USER; r.tag = tag; r.err = e; r.fd = -1;
    r.policy.is_admin = admin; r.policy.content_deletion = del; r.policy.deletion_folders_n = folders;
    send_result(&r);
}
/* signed in on the PLAYLISTS view with `n` playlist rows, the policy answered (admin) */
static void in_playlists(int n)
{
    signed_in();
    unsigned ut = app.user_tag;
    artists_loaded(3, 3);
    send(JF_EV_CATEGORY, 1);
    JfCmd pl = *find(JF_CMD_REQUEST); reply_list(&pl, JF_ITEM_PLAYLIST, n, n);
    send_user_result(ut, JF_ERR_NONE, true, false, 0);
}
/* the one WRITE the last step emitted */
static const JfCmd *the_write(void)
{
    const JfCmd *w = NULL;
    for (int k = 0; k < cm.n; k++) if (cm.c[k].type == JF_CMD_REQUEST && cm.c[k].kind == JF_KIND_WRITE) w = &cm.c[k];
    return w;
}

static unsigned wtag(void) { const JfCmd *w = the_write(); return w ? w->tag : 0; }

static void test_playlist_delete(void)
{
    char want[JF_PATH_MAX];

    /* playlist_select_raises_bar_not_open */
    in_playlists(3);
    send(JF_EV_SELECT, 1);
    CHECK(app.action_bar && app.action_row == 1 && app.screen == JF_SCR_ARTISTS && cm.n == 0,
          "a playlist row raises the action bar instead of opening (screen %d, cmds %d)", app.screen, cm.n);
    send(JF_EV_ACTIVATE, 1);
    CHECK(app.screen == JF_SCR_TRACKS, "OPEN (ACTIVATE) still enters the playlist");

    /* playlist_delete_asks_confirm */
    in_playlists(3);
    send(JF_EV_SELECT, 2);
    send(JF_EV_PLAYLIST_DELETE, 2);
    CHECK(app.confirm == JF_CONFIRM_DELETE_PLAYLIST && cm.n == 0 && strcmp(app.confirm_seed.id, app.lists[JF_LIST_ARTISTS].items[2].id) == 0 &&
          strcmp(app.confirm_seed.name, app.lists[JF_LIST_ARTISTS].items[2].name) == 0, "DELETE asks first, names the row, sends nothing");

    /* playlist_delete_confirm_emits_delete */
    send(JF_EV_CONFIRM, 0);
    const JfCmd *w = the_write();
    jf_path_playlist_delete(want, sizeof want, app.lists[JF_LIST_ARTISTS].items[2].id);
    CHECK(w && cm.n == 1 && strcmp(w->method, "DELETE") == 0 && strcmp(w->path, want) == 0 && w->tag != 0 && app.confirm == JF_CONFIRM_NONE,
          "CONFIRM sends one DELETE /Items/<id> (cmds %d)", cm.n);
    unsigned tag = w ? w->tag : 0;

    /* playlist_delete_second_confirm_ignored_while_pending */
    send(JF_EV_SELECT, 2); send(JF_EV_PLAYLIST_DELETE, 2); send(JF_EV_CONFIRM, 0);
    CHECK(!the_write() && cm.n == 0, "a second delete while one is pending sends nothing");

    /* playlist_delete_ok_refreshes_list */
    char name[96]; snprintf(name, sizeof name, "%s", app.lists[JF_LIST_ARTISTS].items[2].name);
    send_write_result(tag, JF_ERR_NONE, 204);
    char b[128]; snprintf(b, sizeof b, "DELETED: %s", name);
    jf_path_playlists(want, sizeof want, UID, 0, JF_PAGE_ARTISTS);
    CHECK(strcmp(app.banner, b) == 0 && cm.n == 1 && has(JF_CMD_REQUEST) && find(JF_CMD_REQUEST)->kind == JF_KIND_LIST &&
          strcmp(find(JF_CMD_REQUEST)->path, want) == 0 && app.lists[JF_LIST_ARTISTS].n == 0 && !app.overlay,
          "2xx: banner '%s', Playlists list re-requested from page 0 (banner '%s')", b, app.banner);
    if (!has(JF_CMD_REQUEST)) return;   /* pre-fix source: nothing more to drive */
    JfCmd pl2 = *find(JF_CMD_REQUEST); reply_list(&pl2, JF_ITEM_PLAYLIST, 2, 2);
    CHECK(app.lists[JF_LIST_ARTISTS].n == 2, "the refreshed list shows what the server still has");
    send(JF_EV_SELECT, 0); send(JF_EV_PLAYLIST_DELETE, 0); send(JF_EV_CONFIRM, 0);
    CHECK(the_write() != NULL, "after the result a new delete is allowed");

    /* a long name still fits the banner */
    in_playlists(1);
    memset(app.lists[JF_LIST_ARTISTS].items[0].name, 'N', 90); app.lists[JF_LIST_ARTISTS].items[0].name[90] = '\0';
    send(JF_EV_PLAYLIST_DELETE, 0); send(JF_EV_CONFIRM, 0);
    send_write_result(wtag(), JF_ERR_NONE, 204);
    CHECK(strlen(app.banner) == 47 && !strncmp(app.banner, "DELETED: NNN", 12), "long name: banner cut to the field");

    /* playlist_delete_cancel_no_command */
    in_playlists(3);
    send(JF_EV_SELECT, 1); send(JF_EV_PLAYLIST_DELETE, 1);
    send(JF_EV_DISMISS, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE && cm.n == 0, "DISMISS cancels the question");
    send(JF_EV_CONFIRM, 0);
    CHECK(cm.n == 0, "a CONFIRM after the cancel does nothing");
    send(JF_EV_PLAYLIST_DELETE, 1); send(JF_EV_BACK, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE && cm.n == 0 && app.screen == JF_SCR_ARTISTS, "BACK cancels too, and is spent");

    /* playlist_delete_401_banner_no_signout / playlist_delete_403_same */
    for (int pass = 0; pass < 2; pass++) {
        in_playlists(3);
        send(JF_EV_PLAYLIST_DELETE, 0); send(JF_EV_CONFIRM, 0);
        send_write_result(wtag(), pass ? JF_ERR_FORBIDDEN : JF_ERR_UNAUTH, pass ? 403 : 401);
        CHECK(strcmp(app.banner, "SERVER REFUSED: NO DELETE RIGHT") == 0 && app.login.token[0] && !has(JF_CMD_CLEAR_LOGIN) &&
              app.screen == JF_SCR_ARTISTS && !app.overlay && app.delete_denied && app.lists[JF_LIST_ARTISTS].n == 3,
              "%s on DELETE: refusal banner, token kept, no sign-out, list untouched", pass ? "403" : "401");
        const JfCmd *u = find(JF_CMD_REQUEST);
        char up[JF_PATH_MAX]; jf_path_user(up, sizeof up, UID);
        CHECK(cm.n == 1 && u && u->kind == JF_KIND_USER && strcmp(u->path, up) == 0, "one USER re-fetch after a refusal");
        /* playlist_delete_denied_hides_button */
        CHECK(!jf_can_delete(&app), "after a refusal jf_can_delete is false even for an admin policy");
        send_user_result(u ? u->tag : 0, JF_ERR_NONE, true, false, 0);
        CHECK(!jf_can_delete(&app), "the re-fetched policy does not undo the refusal");
    }

    /* other failures: the overlay, RETRY re-sends the same DELETE */
    in_playlists(2);
    send(JF_EV_PLAYLIST_DELETE, 1); send(JF_EV_CONFIRM, 0);
    w = the_write(); tag = wtag(); snprintf(want, sizeof want, "%s", w ? w->path : "");
    send_write_result(tag, JF_ERR_UNREACHABLE, 0);
    CHECK(app.overlay && (app.overlay_buttons & JF_BTN_RETRY) && !app.delete_denied && app.login.token[0], "unreachable: overlay with RETRY");
    send(JF_EV_RETRY, 0);
    w = the_write();
    CHECK(w && strcmp(w->method, "DELETE") == 0 && strcmp(w->path, want) == 0, "RETRY re-sends the DELETE");
    send_write_result(wtag(), JF_ERR_NONE, 204);
    CHECK(!strncmp(app.banner, "DELETED: ", 9), "the retried delete succeeds");

    /* a stale / unrelated WRITE result is dropped */
    in_playlists(2);
    send_write_result(77, JF_ERR_UNAUTH, 401);
    CHECK(app.login.token[0] && !app.delete_denied && app.screen == JF_SCR_ARTISTS, "a WRITE result nobody asked for changes nothing");
}

static void test_playlist_policy(void)
{
    /* playlist_menu_policy_table: admin / content_deletion / folders>0 / none / unknown */
    static const struct { bool known, admin, del; int folders; bool want; const char *what; } T[] = {
        { true,  true,  false, 0, true,  "admin" },
        { true,  false, true,  0, true,  "EnableContentDeletion" },
        { true,  false, false, 2, true,  "folders non-empty" },
        { true,  false, false, 0, false, "none" },
        { false, true,  true,  3, false, "unknown (not answered yet)" },
    };
    for (unsigned k = 0; k < sizeof T / sizeof T[0]; k++) {
        in_playlists(1);
        app.policy.is_admin = T[k].admin; app.policy.content_deletion = T[k].del; app.policy.deletion_folders_n = T[k].folders;
        app.policy_known = T[k].known;
        CHECK(jf_can_delete(&app) == T[k].want, "jf_can_delete for %s should be %d", T[k].what, T[k].want);
    }

    /* start_fetches_user_policy_when_signed_in */
    signed_in();
    const JfCmd *u = NULL;
    for (int k = 0; k < cm.n; k++) if (cm.c[k].type == JF_CMD_REQUEST && cm.c[k].kind == JF_KIND_USER) u = &cm.c[k];
    char up[JF_PATH_MAX]; jf_path_user(up, sizeof up, UID);
    CHECK(u && strcmp(u->path, up) == 0 && u->tag != 0 && !jf_can_delete(&app), "a restored session asks for its policy once; DELETE absent until answered");
    CHECK(find(JF_CMD_REQUEST)->kind == JF_KIND_LIST, "the list request stays the first REQUEST");
    unsigned t = u ? u->tag : 0;
    send_user_result(t + 5, JF_ERR_NONE, true, true, 1);
    CHECK(!jf_can_delete(&app), "a USER reply with another tag is dropped");
    send_user_result(t, JF_ERR_NONE, false, true, 0);
    CHECK(jf_can_delete(&app) && app.policy_known, "the USER reply fills the policy");
    /* a failed fetch leaves the policy unknown, and does not sign out */
    signed_in();
    t = app.user_tag;
    send_user_result(t, JF_ERR_UNAUTH, true, true, 1);
    CHECK(!jf_can_delete(&app) && app.login.token[0], "a failed policy fetch: still unknown, nothing else happens");
    /* no token: no fetch */
    jf_app_init(&app); app.session_fn = test_session;
    { static JfEvent ev; memset(&ev, 0, sizeof ev); ev.type = JF_EV_START; step_ev(&ev); }
    CHECK(app.screen == JF_SCR_SIGNIN && !has(JF_CMD_REQUEST), "no token: no policy fetch");

    /* sign-in fills it from the auth reply */
    send_signin("10.0.0.1:8096", "bob", "pw");
    JfResult r; memset(&r, 0, sizeof r); r.kind = JF_KIND_AUTH; r.tag = app.auth_tag; snprintf(r.token, sizeof r.token, "t2"); snprintf(r.user_id, 33, UID);
    r.policy.is_admin = true;
    send_result(&r);
    CHECK(jf_can_delete(&app) && cm.n >= 1, "the auth reply carries the policy");

    /* signout_clears_policy */
    app.delete_denied = true;
    send(JF_EV_SETTINGS_OPEN, 0);
    send(JF_EV_SETTING_ACTIVATE, JF_SET_SIGNOUT);
    send(JF_EV_CONFIRM, 0);
    CHECK(!app.policy_known && !app.policy.is_admin && !app.delete_denied && !jf_can_delete(&app), "sign-out clears the policy and the refusal");
}

/* ── ADD TO PLAYLIST ───────────────────────────────────────────────────── */

/* PICK_OPEN -1 on the tracks screen, the picker list answered with `pls` playlists */
static void picker_open(int tracks, int total, int pls)
{
    in_tracks(tracks, total);
    send(JF_EV_PICK_OPEN, -1);
    JfCmd c = *find(JF_CMD_REQUEST);
    reply_list(&c, JF_ITEM_PLAYLIST, pls, pls);
}
/* the LIST request the last step emitted */
static const JfCmd *list_req(void)
{
    const JfCmd *r = NULL;
    for (int k = 0; k < cm.n; k++) if (cm.c[k].type == JF_CMD_REQUEST && cm.c[k].kind == JF_KIND_LIST) r = &cm.c[k];
    return r;
}
/* a duplicate-scan page: the given ids first, then `filler` unrelated tracks; the playlist holds `total` */
static void reply_ids(const JfCmd *c, const char *const *ids, int nid, int filler, int total)
{
    JfCmd cp = *c;
    JfList *l = cp.list;
    for (int k = 0; k < nid && l->n < l->cap; k++) {
        JfItem *it = &l->items[l->n++];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "%s", ids[k]);
        it->type = JF_ITEM_TRACK;
        l->loaded++;
    }
    fill(l, JF_ITEM_TRACK, filler, total);
    reply_raw(&cp, JF_ERR_NONE);
}
static void scan_clean(void)   /* the playlist holds nothing that is being added */
{
    const JfCmd *r = list_req();
    if (r) { JfCmd c = *r; reply_list(&c, JF_ITEM_TRACK, 0, 0); }
}
/* the path an add of pick_ids[from .. from+n) to picker row `sel` must carry */
static void add_path(char *out, int from, int n, int sel)
{
    const char *ids[JF_SAVE_IDS_MAX];
    for (int k = 0; k < n; k++) ids[k] = app.pick_ids[from + k];
    jf_path_playlist_add(out, JF_PATH_MAX, UID, app.lists[JF_LIST_PLPICK].items[sel].id, ids, n);
}

static void test_pick(void)
{
    char want[JF_PATH_MAX];
    max_cmds = 0;

    /* pick_open_from_track_bar */
    in_tracks(5, 5);
    send(JF_EV_SELECT, 2);
    send(JF_EV_PICK_OPEN, 2);
    jf_path_playlists(want, sizeof want, UID, 0, JF_PAGE_PLPICK);
    const JfCmd *r = list_req();
    CHECK(app.screen == JF_SCR_PLPICK && app.pick_n == 1 && strcmp(app.pick_ids[0], app.lists[JF_LIST_TRACKS].items[2].id) == 0 &&
          app.pick_sel == -1 && cm.n == 1 && r && r->list == &app.lists[JF_LIST_PLPICK] && strcmp(r->path, want) == 0 && !app.action_bar,
          "PICK_OPEN on a bar row opens the picker with that one id and asks for the playlists (screen %d, n %d, cmds %d)", app.screen, app.pick_n, cm.n);

    /* pick_open_from_tracks_screen_collects_ids */
    in_tracks(7, 7);
    send(JF_EV_PICK_OPEN, -1);
    bool same = app.pick_n == 7;
    for (int k = 0; k < 7 && same; k++) same = strcmp(app.pick_ids[k], app.lists[JF_LIST_TRACKS].items[k].id) == 0;
    CHECK(app.screen == JF_SCR_PLPICK && same && !app.banner[0], "PICK_OPEN -1 collects every track id of the list, in order (n %d)", app.pick_n);

    /* pick_open_caps_100_banner */
    in_tracks(120, 120);
    send(JF_EV_PICK_OPEN, -1);
    CHECK(app.screen == JF_SCR_PLPICK && app.pick_n == JF_SAVE_IDS_MAX && strncmp(app.banner, "FIRST 100", 9) == 0 &&
          strcmp(app.pick_ids[99], app.lists[JF_LIST_TRACKS].items[99].id) == 0, "120 tracks: the first 100 ids and a banner (n %d, \"%s\")", app.pick_n, app.banner);
    in_tracks(100, 100);
    send(JF_EV_PICK_OPEN, -1);
    CHECK(app.pick_n == 100 && !app.banner[0], "exactly 100 is not cut: no banner");
    in_tracks(100, 300);      /* 100 loaded, more on the server: no fetch is needed to know the cut */
    send(JF_EV_PICK_OPEN, -1);
    CHECK(app.screen == JF_SCR_PLPICK && app.pick_n == 100 && strncmp(app.banner, "FIRST 100", 9) == 0 && !app.pick_wait,
          "100 loaded of 300: opens at once, cut and said");

    /* pick_open_waits_for_unloaded_pages */
    in_tracks(25, 60);
    send(JF_EV_PICK_OPEN, -1);
    r = list_req();
    CHECK(app.screen == JF_SCR_TRACKS && app.pick_wait && r && r->list == &app.lists[JF_LIST_TRACKS] && cm.n == 1 && app.pick_n == 0,
          "unloaded pages: stays on the tracks, asks for the next page (screen %d, wait %d)", app.screen, app.pick_wait);
    JfCmd t2 = *r; reply_list(&t2, JF_ITEM_TRACK, 25, 60);
    CHECK(app.screen == JF_SCR_TRACKS && app.pick_wait && list_req() && cm.n == 1, "page two: still waiting, asks for page three");
    JfCmd t3 = *list_req(); reply_list(&t3, JF_ITEM_TRACK, 10, 60);
    CHECK(app.screen == JF_SCR_PLPICK && !app.pick_wait && app.pick_n == 60 && list_req() && list_req()->list == &app.lists[JF_LIST_PLPICK],
          "the last page opens the picker with all 60 ids (screen %d, n %d)", app.screen, app.pick_n);
    in_tracks(25, 60);                      /* BACK while waiting cancels the wait */
    send(JF_EV_PICK_OPEN, -1);
    send(JF_EV_BACK, 0);
    CHECK(!app.pick_wait && app.screen == JF_SCR_ALBUMS, "BACK during the wait drops it");

    /* pick_select_does_not_open */
    picker_open(5, 5, 3);
    int nav = app.nav_n;
    send(JF_EV_PICK_SELECT, 1);
    CHECK(app.pick_sel == 1 && app.screen == JF_SCR_PLPICK && app.nav_n == nav && cm.n == 0, "PICK_SELECT marks the row and opens nothing");
    send(JF_EV_PICK_SELECT, 9);
    CHECK(app.pick_sel == 1, "a row past the list leaves the selection alone");

    /* pick_select_no_playlist_menu */
    send(JF_EV_SELECT, 2);
    CHECK(!app.action_bar && app.screen == JF_SCR_PLPICK && cm.n == 0, "a plain SELECT on a picker row neither raises the playlist menu nor opens it");
    send(JF_EV_ACTIVATE, 2);
    CHECK(app.screen == JF_SCR_PLPICK && cm.n == 0, "ACTIVATE on a picker row opens nothing either");

    /* pick_add_without_selection_noop */
    picker_open(5, 5, 3);
    send(JF_EV_PICK_ADD, 0);
    CHECK(cm.n == 0 && app.confirm == JF_CONFIRM_NONE && !app.dup_scan && !app.add_pending, "ADD without a selection does nothing");

    /* pick_add_post_path_exact (+ pick_dup_none_adds_directly) */
    picker_open(3, 3, 3);
    send(JF_EV_PICK_SELECT, 1);
    send(JF_EV_PICK_ADD, 0);
    jf_path_playlist_ids(want, sizeof want, UID, app.lists[JF_LIST_PLPICK].items[1].id, 0, JF_PAGE_PLCHK);
    r = list_req();
    CHECK(cm.n == 1 && r && r->list == &app.lists[JF_LIST_PLCHK] && strcmp(r->path, want) == 0 && !the_write(),
          "ADD scans the playlist first: one id-page request, no write yet (cmds %d)", cm.n);
    scan_clean();
    add_path(want, 0, 3, 1);
    const JfCmd *w = the_write();
    CHECK(w && cm.n == 1 && strcmp(w->method, "POST") == 0 && strcmp(w->path, want) == 0 && w->tag != 0 && app.confirm == JF_CONFIRM_NONE,
          "no duplicate: the first batch goes at once, POST /Playlists/<pl>/Items?ids=..&userId=.. (cmds %d)", cm.n);

    /* pick_add_ok_banner_stays_open */
    send_write_result(wtag(), JF_ERR_NONE, 204);
    snprintf(want, sizeof want, "ADDED TO %s", app.lists[JF_LIST_PLPICK].items[1].name);
    CHECK(strcmp(app.banner, want) == 0 && app.screen == JF_SCR_PLPICK && app.pick_sel == 1 && !app.overlay && cm.n == 0 && !app.add_pending,
          "the last batch: banner \"%s\", picker open, selection kept (got \"%s\")", want, app.banner);
    send(JF_EV_PICK_ADD, 0);                               /* the same playlist again: a new scan */
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_PLCHK], "ADD again starts a new scan");

    /* pick_add_23_ids_three_sequential_writes */
    picker_open(23, 23, 2);
    send(JF_EV_PICK_SELECT, 0);
    send(JF_EV_PICK_ADD, 0);
    scan_clean();
    int sizes[3] = { 10, 10, 3 };
    for (int b = 0; b < 3; b++) {
        w = the_write();
        add_path(want, b * 10, sizes[b], 0);
        CHECK(w && cm.n == 1 && strcmp(w->path, want) == 0, "batch %d: one POST with ids %d..%d (cmds %d)", b, b * 10, b * 10 + sizes[b] - 1, cm.n);
        if (b < 2) CHECK(app.add_pending && !app.banner[0], "batch %d: not done, no banner yet", b);
        send_write_result(wtag(), JF_ERR_NONE, 204);
        if (b < 2) CHECK(the_write() != NULL, "batch %d acknowledged: the next one follows", b);
    }
    CHECK(!the_write() && strncmp(app.banner, "ADDED TO", 8) == 0, "after the third batch: no more writes, banner");

    /* pick_add_fail_overlay */
    picker_open(23, 23, 2);
    send(JF_EV_PICK_SELECT, 0); send(JF_EV_PICK_ADD, 0); scan_clean();
    send_write_result(wtag(), JF_ERR_SERVER, 500);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE && !the_write() && !app.add_pending && app.screen == JF_SCR_PLPICK,
          "a failed batch: the overlay with CLOSE only (RETRY would re-add), the rest not sent");
    send(JF_EV_CLOSE_ERROR, 0);
    send(JF_EV_PICK_ADD, 0);
    CHECK(list_req() != NULL, "after CLOSE the add can be tried again");

    /* pick_add_401_session_expired */
    picker_open(3, 3, 2);
    send(JF_EV_PICK_SELECT, 0); send(JF_EV_PICK_ADD, 0); scan_clean();
    send_write_result(wtag(), JF_ERR_UNAUTH, 401);
    CHECK(app.screen == JF_SCR_SIGNIN && !app.login.token[0] && has(JF_CMD_CLEAR_LOGIN) && !app.add_pending && strncmp(app.banner, "SESSION EXPIRED", 15) == 0,
          "401 on an add is a lost session: sign-in, token gone");

    /* pick_dup_single_asks */
    in_tracks(4, 4);
    send(JF_EV_SELECT, 1); send(JF_EV_PICK_OPEN, 1);
    { JfCmd c = *list_req(); reply_list(&c, JF_ITEM_PLAYLIST, 2, 2); }
    send(JF_EV_PICK_SELECT, 1); send(JF_EV_PICK_ADD, 0);
    const char *one[1] = { app.pick_ids[0] };
    { JfCmd c = *list_req(); reply_ids(&c, one, 1, 4, 400); }          /* total says 400, but one id stops at the first hit */
    CHECK(app.confirm == JF_CONFIRM_ADD_DUP && app.dup_hits == 1 && !app.dup_unknown && cm.n == 0 && !the_write() && !list_req() &&
          strcmp(app.add_name, app.lists[JF_LIST_PLPICK].items[1].name) == 0,
          "one id already there: asks, names the playlist, reads no further pages (hits %d, cmds %d)", app.dup_hits, cm.n);

    /* pick_dup_scan_pages_until_total, pick_dup_confirm_adds_all */
    picker_open(3, 3, 2);
    const char *ids3[3] = { app.pick_ids[0], app.pick_ids[1], app.pick_ids[2] };
    send(JF_EV_PICK_SELECT, 1); send(JF_EV_PICK_ADD, 0);
    { JfCmd c = *list_req(); reply_ids(&c, NULL, 0, 50, 120); }
    jf_path_playlist_ids(want, sizeof want, UID, app.lists[JF_LIST_PLPICK].items[1].id, 50, JF_PAGE_PLCHK);
    CHECK(cm.n == 1 && list_req() && strcmp(list_req()->path, want) == 0 && app.confirm == JF_CONFIRM_NONE, "page 1 had none: page 2 starts at 50");
    { const char *h[2] = { ids3[0], ids3[2] }; JfCmd c = *list_req(); reply_ids(&c, h, 2, 48, 120); }
    jf_path_playlist_ids(want, sizeof want, UID, app.lists[JF_LIST_PLPICK].items[1].id, 100, JF_PAGE_PLCHK);
    CHECK(cm.n == 1 && list_req() && strcmp(list_req()->path, want) == 0, "page 2 found two: page 3 starts at 100 (more than one id keeps reading)");
    { const char *h[1] = { ids3[0] }; JfCmd c = *list_req(); reply_ids(&c, h, 1, 19, 120); }   /* the same id again does not count twice */
    CHECK(app.confirm == JF_CONFIRM_ADD_DUP && app.dup_hits == 2 && !app.dup_unknown && cm.n == 0, "the last page: 2 of 3 already there (hits %d)", app.dup_hits);
    send(JF_EV_CONFIRM, 0);
    add_path(want, 0, 3, 1);
    w = the_write();
    CHECK(w && cm.n == 1 && strcmp(w->path, want) == 0 && app.confirm == JF_CONFIRM_NONE, "ADD ANYWAY sends all three ids");
    picker_open(3, 3, 2);                                     /* CANCEL */
    send(JF_EV_PICK_SELECT, 0); send(JF_EV_PICK_ADD, 0);
    { const char *h[1] = { app.pick_ids[1] }; JfCmd c = *list_req(); reply_ids(&c, h, 1, 0, 1); }
    send(JF_EV_DISMISS, 0);
    send(JF_EV_CONFIRM, 0);
    CHECK(app.confirm == JF_CONFIRM_NONE && !the_write() && cm.n == 0, "CANCEL sends nothing, and a later CONFIRM finds nothing to confirm");

    /* pick_dup_cap_asks_could_not_check */
    picker_open(3, 3, 2);
    send(JF_EV_PICK_SELECT, 0); send(JF_EV_PICK_ADD, 0);
    int pages = 0;
    while (list_req() && pages < 60) { JfCmd c = *list_req(); pages++; reply_ids(&c, NULL, 0, 50, 5000); }
    CHECK(pages == JF_DUP_PAGES_MAX && app.confirm == JF_CONFIRM_ADD_DUP && app.dup_unknown && app.dup_hits == 0 && !the_write(),
          "an endless playlist: %d pages, then \"could not check\" instead of a silent pass (unknown %d)", pages, app.dup_unknown);
    picker_open(3, 3, 2);                                     /* a failed page asks the same */
    send(JF_EV_PICK_SELECT, 0); send(JF_EV_PICK_ADD, 0);
    { JfCmd c = *list_req(); reply_raw(&c, JF_ERR_SERVER); }
    CHECK(app.confirm == JF_CONFIRM_ADD_DUP && app.dup_unknown && !app.overlay && !the_write(), "a failed scan page: could-not-check, no overlay");

    /* pick_new_posts_create_with_pick_ids */
    picker_open(6, 6, 3);
    send_text(JF_EV_PICK_NEW, "");
    CHECK(cm.n == 0, "an empty name sends nothing");
    send_text(JF_EV_PICK_NEW, "Road trip");
    const JfCmd *s = find(JF_CMD_REQUEST);
    CHECK(cm.n == 1 && s && s->kind == JF_KIND_SAVE && strcmp(s->name, "Road trip") == 0 && s->n == 6 && s->ids_src == JF_IDS_PICK && s->tag != 0 && !list_req(),
          "NEW: one SAVE of the 6 picked ids, no duplicate scan");
    save_result(s ? s->tag : 0, JF_ERR_NONE, 200);
    r = list_req();
    jf_path_playlists(want, sizeof want, UID, 0, JF_PAGE_PLPICK);
    CHECK(strcmp(app.banner, "ADDED TO Road trip") == 0 && r && r->list == &app.lists[JF_LIST_PLPICK] && strcmp(r->path, want) == 0 && app.pick_sel == -1 && cm.n == 1,
          "created: banner, and the picker list is asked again (\"%s\")", app.banner);
    { JfCmd c = *r; fill(c.list, JF_ITEM_PLAYLIST, 4, 4); snprintf(c.list->items[2].name, 96, "Road trip"); reply_raw(&c, JF_ERR_NONE); }
    CHECK(app.pick_sel == 2 && app.screen == JF_SCR_PLPICK, "the new row is selected by name (sel %d)", app.pick_sel);
    picker_open(6, 6, 3);
    send_text(JF_EV_PICK_NEW, "x");
    save_result(find(JF_CMD_REQUEST)->tag, JF_ERR_SERVER, 500);
    CHECK(app.overlay && app.overlay_buttons == JF_BTN_CLOSE, "a failed create: the overlay, CLOSE only");

    /* pick_close_pops_to_scroll */
    in_tracks(25, 25);
    jf_note_view(&app, 12, 14);
    unsigned seq = app.restore_seq;
    send(JF_EV_PICK_OPEN, -1);
    { JfCmd c = *list_req(); reply_list(&c, JF_ITEM_PLAYLIST, 2, 2); }
    send(JF_EV_PICK_CLOSE, 0);
    CHECK(app.screen == JF_SCR_TRACKS && app.restore_top == 12 && app.restore_cursor == 14 && app.restore_seq != seq && cm.n == 0,
          "CLOSE returns to the track list at its scroll, no refetch (screen %d, top %d, cursor %d)", app.screen, app.restore_top, app.restore_cursor);
    send(JF_EV_PICK_OPEN, -1);
    send(JF_EV_BACK, 0);
    CHECK(app.screen == JF_SCR_TRACKS, "BACK does the same");
    picker_open(3, 3, 2);                                   /* closing in the middle of the scan abandons it */
    send(JF_EV_PICK_SELECT, 0); send(JF_EV_PICK_ADD, 0);
    JfCmd stale = *list_req();
    send(JF_EV_PICK_CLOSE, 0);
    reply_ids(&stale, NULL, 0, 0, 0);
    CHECK(!the_write() && app.confirm == JF_CONFIRM_NONE && !app.dup_scan, "a scan page that arrives after CLOSE adds nothing");

    /* pick_write_tags_distinct: an add and a delete never take each other's result */
    in_playlists(3);
    send(JF_EV_SELECT, 2); send(JF_EV_PLAYLIST_DELETE, 2); send(JF_EV_CONFIRM, 0);
    unsigned dtag = wtag();
    send(JF_EV_ACTIVATE, 0);
    { JfCmd c = *list_req(); reply_list(&c, JF_ITEM_TRACK, 3, 3); }
    send(JF_EV_PICK_OPEN, -1);
    { JfCmd c = *list_req(); reply_list(&c, JF_ITEM_PLAYLIST, 2, 2); }
    send(JF_EV_PICK_SELECT, 1); send(JF_EV_PICK_ADD, 0); scan_clean();
    unsigned atag = wtag();
    CHECK(atag != 0 && atag != dtag, "add tag %u differs from the pending delete's %u", atag, dtag);
    send_write_result(dtag, JF_ERR_NONE, 204);
    CHECK(app.add_pending && strncmp(app.banner, "DELETED", 7) == 0 && !the_write(), "the delete's result does not finish the add");
    send(JF_EV_PICK_OPEN, 0);
    CHECK(app.add_pending && app.screen == JF_SCR_PLPICK, "PICK_OPEN while an add is in flight is ignored");
    send_write_result(atag, JF_ERR_NONE, 204);
    CHECK(strncmp(app.banner, "ADDED TO", 8) == 0 && !app.add_pending, "the add's own result finishes it");

    /* pick_not_offered_on_artist_folder */
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_PICK_OPEN, -1);
    CHECK(app.screen == JF_SCR_ARTISTS && cm.n == 0, "not at the root");
    send(JF_EV_ACTIVATE, 0);
    { JfCmd c = *find(JF_CMD_REQUEST); reply_list(&c, JF_ITEM_ALBUM, 2, 2); }
    send(JF_EV_PICK_OPEN, -1);
    CHECK(app.screen == JF_SCR_ALBUMS && cm.n == 0 && app.pick_n == 0, "not on an artist's albums (hundreds of tracks against the 100 cap)");
    send(JF_EV_PICK_OPEN, 0);
    CHECK(app.screen == JF_SCR_ALBUMS && cm.n == 0, "an album row is no track");

    /* pick_cmds_le_2 */
    CHECK(max_cmds <= 2, "no step of the add flow emitted more than 2 commands (max %d)", max_cmds);
}


/* ── artist folder and inline search ───────────────────────────────────── */

/* append `count` tracks, as a parse would, with the album artist and album id the filter reads */
static void fill_trk(JfList *l, int count, int total, const char *album_artist, const char *album_id)
{
    int from = l->n;
    fill(l, JF_ITEM_TRACK, count, total);
    for (int k = from; k < l->n; k++) {
        snprintf(l->items[k].album_artist, sizeof(l->items[k].album_artist), "%s", album_artist);
        snprintf(l->items[k].album_id, sizeof(l->items[k].album_id), "%s", album_id);
    }
}
static void reply_trk(const JfCmd *c, int count, int total, const char *album_artist, const char *album_id)
{
    JfCmd cp = *c;
    fill_trk(cp.list, count, total, album_artist, album_id);
    reply_raw(&cp, JF_ERR_NONE);
}
/* sign in, open the first artist, load its 2 albums (the one list request left is the tracks phase, page 0) */
static void artist_folder(void)
{
    signed_in(); artists_loaded(3, 3);
    send(JF_EV_ACTIVATE, 0);
    JfCmd a = *list_req(); reply_list(&a, JF_ITEM_ALBUM, 2, 2);
}
static const char *art_name(void) { return app.lists[JF_LIST_ARTISTS].items[0].name; }
static const char *alb0(void) { return app.lists[JF_LIST_ALBUMS].items[0].id; }
#define OTHER_ALB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void test_artist(void)
{
    char want[JF_PATH_MAX];
    signed_in(); artists_loaded(3, 3);
    max_cmds = 0;
    send(JF_EV_ACTIVATE, 0);
    jf_path_artist_albums(want, sizeof(want), UID, app.lists[JF_LIST_ARTISTS].items[0].id, 0, JF_PAGE_ALBUMS);
    CHECK(list_req() && strcmp(list_req()->path, want) == 0, "artist_albums_then_tracks_same_list: phase 0 asks for the artist's own albums");
    { JfCmd a = *list_req(); reply_list(&a, JF_ITEM_ALBUM, 2, 2); }
    jf_path_artist_tracks(want, sizeof(want), UID, app.lists[JF_LIST_ARTISTS].items[0].id, 0, JF_PAGE_ALBUMS);
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ALBUMS] && strcmp(list_req()->path, want) == 0,
          "the page after the last album is the tracks page 0 of the same list");
    CHECK(app.lists[JF_LIST_ALBUMS].n == 2 && app.lists[JF_LIST_ALBUMS].loaded == 0 && app.busy, "albums kept, counters reset, busy");
    { JfCmd t = *list_req(); reply_trk(&t, 3, 3, "Someone Else", "ffffffffffffffffffffffffffffffff"); }
    CHECK(app.lists[JF_LIST_ALBUMS].n == 5 && app.lists[JF_LIST_ALBUMS].items[0].type == JF_ITEM_ALBUM &&
          app.lists[JF_LIST_ALBUMS].items[2].type == JF_ITEM_TRACK && !app.busy && cm.n == 0, "tracks follow the albums in one list");

    /* artist_tracks_filter_drops_own_album */
    artist_folder();
    { JfCmd t = *list_req(); JfList *l = t.list;
      fill_trk(l, 1, 4, "Other", alb0());             /* a track of the artist's own album */
      fill_trk(l, 1, 4, art_name(), "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");   /* album artist is the artist */
      fill_trk(l, 2, 4, "Other", "dddddddddddddddddddddddddddddddd");
      reply_raw(&t, JF_ERR_NONE); }
    CHECK(app.lists[JF_LIST_ALBUMS].n == 4 && app.lists[JF_LIST_ALBUMS].loaded == 4, "own-album tracks dropped (n %d, loaded %d)", app.lists[JF_LIST_ALBUMS].n, app.lists[JF_LIST_ALBUMS].loaded);

    /* artist_tracks_filter_case_insensitive */
    artist_folder();
    { char up[96]; snprintf(up, sizeof up, "%s", art_name()); for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
      JfCmd t = *list_req(); fill_trk(t.list, 2, 2, up, "cccccccccccccccccccccccccccccccc"); reply_raw(&t, JF_ERR_NONE); }
    CHECK(app.lists[JF_LIST_ALBUMS].n == 2, "album artist in another case is still the artist (n %d)", app.lists[JF_LIST_ALBUMS].n);

    /* artist_filtered_empty_page_requests_next */
    artist_folder();
    { JfCmd t = *list_req(); fill_trk(t.list, 2, 6, "Other", alb0()); reply_raw(&t, JF_ERR_NONE); }
    jf_path_artist_tracks(want, sizeof(want), UID, app.ctx[JF_LIST_ALBUMS].id, 2, JF_PAGE_ALBUMS);
    CHECK(list_req() && strcmp(list_req()->path, want) == 0, "a page that filters to nothing asks for the next one at its loaded count");

    /* artist_phase2_cap_20_pages */
    artist_folder();
    int reqs = 0;
    for (int k = 0; k < 40 && list_req(); k++) {
        JfCmd t = *list_req(); fill_trk(t.list, 2, 1000, "Other", alb0()); reply_raw(&t, JF_ERR_NONE); reqs++;
    }
    CHECK(reqs == 20 && !list_req() && !app.busy, "a prolific artist's filtered-away pages stop at 20 (%d)", reqs);
    send(JF_EV_NEED_MORE, 0);
    CHECK(cm.n == 0, "and NEED_MORE asks for no 21st");

    /* artist_need_more_in_phase2 */
    artist_folder();
    { JfCmd t = *list_req(); reply_trk(&t, 3, 10, "Other", OTHER_ALB); }
    CHECK(cm.n == 0 && !app.busy, "a page that kept rows waits for the scroll");
    send(JF_EV_NEED_MORE, 0);
    jf_path_artist_tracks(want, sizeof(want), UID, app.ctx[JF_LIST_ALBUMS].id, 3, JF_PAGE_ALBUMS);
    CHECK(list_req() && strcmp(list_req()->path, want) == 0, "NEED_MORE asks for the next tracks page");

    /* artist_play_all_fetches_pages_then_queues */
    artist_folder();
    { JfCmd t = *list_req(); reply_trk(&t, 3, 3, "Other", OTHER_ALB); }
    send(JF_EV_PLAY_ALL, 0);
    jf_path_artist_all(want, sizeof(want), UID, app.ctx[JF_LIST_ALBUMS].id, 0, 40);
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ARTALL] && strcmp(list_req()->path, want) == 0 && app.busy, "PLAY ALL asks for the whole artist, 40 a page");
    { JfCmd t = *list_req(); reply_trk(&t, 40, 70, "Other", "a"); }
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ARTALL] && !has(JF_CMD_PLAY_STREAM), "page 2 fetched before anything plays");
    { JfCmd t = *list_req(); reply_trk(&t, 30, 70, "Other", "a"); }
    CHECK(app.queue_n == 70 && has(JF_CMD_PLAY_STREAM) && app.banner[0] == '\0', "70 queued and playing, no banner");

    /* artist_play_all_caps_200_banner */
    artist_folder();
    { JfCmd t = *list_req(); reply_trk(&t, 3, 3, "Other", OTHER_ALB); }
    send(JF_EV_PLAY_ALL, 0);
    for (int k = 0; k < 10 && list_req(); k++) { JfCmd t = *list_req(); reply_trk(&t, 40, 250, "Other", "a"); }
    CHECK(app.queue_n == JF_QUEUE_MAX && has(JF_CMD_PLAY_STREAM) && strcmp(app.banner, "FIRST 200 QUEUED") == 0,
          "250 tracks: the first 200 queue with the banner (queue %d, banner '%s')", app.queue_n, app.banner);

    /* artist_track_row_raises_action_bar */
    artist_folder();
    { JfCmd t = *list_req(); reply_trk(&t, 3, 3, "Other", OTHER_ALB); }
    send(JF_EV_SELECT, 3);
    CHECK(app.action_bar && app.action_row == 3, "a track row on the artist folder raises the bar");
    send(JF_EV_PICK_OPEN, 3);
    CHECK(app.screen == JF_SCR_PLPICK && app.pick_n == 1 && strcmp(app.pick_ids[0], app.lists[JF_LIST_ALBUMS].items[3].id) == 0, "its + PLAYLIST adds that track");
    send(JF_EV_BACK, 0);
    send(JF_EV_ADD_QUEUE, 3);
    CHECK(app.queue_n >= 1, "and ADD QUEUE queues it");

    /* artist_folder_refuses_pick_all */
    artist_folder();
    { JfCmd t = *list_req(); reply_trk(&t, 3, 3, "Other", OTHER_ALB); }
    send(JF_EV_PICK_OPEN, -1);
    CHECK(app.screen == JF_SCR_ALBUMS && cm.n == 0 && app.pick_n == 0, "no + PLAYLIST for all on the artist folder");

    /* artist_lists_library_scoped */
    signed_in(); artists_loaded(3, 3);
    snprintf(app.login.library, sizeof(app.login.library), "%s", UID);
    send(JF_EV_ACTIVATE, 0);
    CHECK(list_req() && strstr(list_req()->path, "&ParentId=" UID), "phase 0 is library-scoped");
    { JfCmd a = *list_req(); reply_list(&a, JF_ITEM_ALBUM, 1, 1); }
    CHECK(list_req() && strstr(list_req()->path, "&ParentId=" UID), "phase 1 is library-scoped");
    { JfCmd t = *list_req(); reply_trk(&t, 1, 1, "Other", OTHER_ALB); }
    send(JF_EV_PLAY_ALL, 0);
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ARTALL] && strstr(list_req()->path, "&ParentId=" UID), "PLAY ALL's list is library-scoped");

    CHECK(max_cmds <= 2, "no step of the artist flow emitted more than 2 commands (max %d)", max_cmds);
}

/* a search whose page 1 holds [track, `artists` artist rows named artist_name, track] */
static void search_page1(const char *term, int artists, const char *artist_name)
{
    signed_in(); artists_loaded(3, 3);
    send_text(JF_EV_SEARCH, term);
    JfCmd sr = *list_req();
    JfList *sl = &app.lists[JF_LIST_SEARCH];
    fill(sl, JF_ITEM_TRACK, 1, 9);
    for (int k = 0; k < artists; k++) {
        fill(sl, JF_ITEM_ARTIST, 1, 9);
        snprintf(sl->items[sl->n - 1].name, sizeof(sl->items[0].name), "%s", artist_name);
    }
    fill(sl, JF_ITEM_TRACK, 1, 9);
    reply_raw(&sr, JF_ERR_NONE);
}

static void test_search_inline(void)
{
    /* search_single_exact_artist_inlines */
    max_cmds = 0;
    search_page1("miles davis", 1, "Miles Davis");
    JfList *sl = &app.lists[JF_LIST_SEARCH];
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ARTINL] && sl->n == 2 && app.busy, "the artist row leaves, its albums are asked for (n %d)", sl->n);
    if (!list_req()) return;   /* nothing to answer: the check above is the verdict, not a crash */
    char tr_after[33]; snprintf(tr_after, sizeof tr_after, "%s", sl->items[1].id);
    { JfCmd a = *list_req(); reply_list(&a, JF_ITEM_ALBUM, 2, 2); }
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ARTINL] && strstr(list_req()->path, "IncludeItemTypes=Audio"), "then its tracks");
    { JfCmd t = *list_req(); reply_trk(&t, 2, 2, "Other", OTHER_ALB); }
    CHECK(sl->n == 6 && sl->items[0].type == JF_ITEM_TRACK && sl->items[1].type == JF_ITEM_ALBUM && sl->items[2].type == JF_ITEM_ALBUM &&
          sl->items[3].type == JF_ITEM_TRACK && sl->items[4].type == JF_ITEM_TRACK && strcmp(sl->items[5].id, tr_after) == 0 && !app.busy && cm.n == 0,
          "its rows are spliced in where it was (n %d)", sl->n);

    /* search_inline_case_insensitive */
    search_page1("  MILES davis ", 1, "Miles Davis");
    CHECK(list_req() && list_req()->list == &app.lists[JF_LIST_ARTINL], "the term matches whatever its case or padding");

    /* search_two_artists_unchanged */
    search_page1("miles davis", 2, "Miles Davis");
    CHECK(app.lists[JF_LIST_SEARCH].n == 4 && !list_req(), "two artists: no inline, both rows stay");

    /* search_one_artist_name_differs_unchanged */
    search_page1("miles", 1, "Miles Davis");
    CHECK(app.lists[JF_LIST_SEARCH].n == 3 && !list_req(), "a partial name: the row stays");

    /* search_inline_abandoned_on_new_search */
    search_page1("miles davis", 1, "Miles Davis");
    JfCmd stale = *list_req();
    send_text(JF_EV_SEARCH, "coltrane");
    bool asked = list_req() && list_req()->list == &app.lists[JF_LIST_SEARCH];
    fill(stale.list, JF_ITEM_ALBUM, 1, 1);
    reply_raw(&stale, JF_ERR_NONE);
    CHECK(app.lists[JF_LIST_SEARCH].n == 0 && asked,
          "a new search drops the inline fetch; its late reply splices nothing");
    search_page1("miles davis", 1, "Miles Davis");
    send(JF_EV_BACK, 0);
    CHECK(!app.lists[JF_LIST_ARTINL].pending && !app.busy, "BACK abandons it too");

    CHECK(max_cmds <= 2, "no step of the search flow emitted more than 2 commands (max %d)", max_cmds);
}
int main(void)
{
    test_start(); test_signin(); test_nav_paging(); test_stale(); test_queue(); test_sink_channels();
    test_failures(); test_expiry(); test_volume(); test_search(); test_retry(); test_quit(); test_categories(); test_category_pick(); test_settings(); test_signout();
    test_action_bar(); test_queue_insert(); test_mix(); test_mix_seed_and_ticks(); test_mix_flow(); test_mix_choice(); test_mix_stops();
    test_mix_cancel_and_errors(); test_mix_nowplaying(); test_save();
    test_queue_clear(); test_queue_restore();
    test_pause_stop_seek(); test_instant_confirm(); test_back_restores_view(); test_prev(); test_queue_edit(); test_queue_remove();
    test_playlist_delete(); test_playlist_policy(); test_pick(); test_artist(); test_search_inline();
    printf("jf_core_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
