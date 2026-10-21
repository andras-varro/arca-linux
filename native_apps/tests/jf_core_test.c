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


int main(void)
{
    test_start(); test_signin(); test_nav_paging(); test_stale(); test_queue(); test_sink_channels();
    test_failures(); test_expiry(); test_volume(); test_search(); test_retry(); test_quit(); test_categories();
    test_action_bar(); test_queue_insert(); test_mix(); test_mix_seed_and_ticks(); test_mix_flow(); test_mix_choice(); test_mix_stops();
    test_mix_cancel_and_errors(); test_mix_nowplaying(); test_save();
    test_pause_stop_seek(); test_instant_confirm(); test_back_restores_view();
    printf("jf_core_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
