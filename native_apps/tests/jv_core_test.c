/* Host regression for jfvideo/jv_core.c: the video client's state machine.
 *
 *   cd native_apps && gcc -Wall -Wextra -I common -I jf_shared -I jfvideo -fsanitize=undefined,address \
 *       -o build/jv_core_test tests/jv_core_test.c jfvideo/jv_core.c jfvideo/jv_model.c jfvideo/jv_tracks.c \
 *       jfvideo/jv_url.c jfvideo/jv_settings.c jf_shared/jf_model.c jf_shared/jf_url.c jf_shared/jf_login.c \
 *       common/config.c common/json.c -lm && setarch $(uname -m) -R ./build/jv_core_test
 *
 * Run from native_apps/ (the gate does): replies are the fixtures in tests/fixtures/jv/, parsed by the
 * core's own parser exactly as jf_api would, then fed back as JV_EV_RESULT.  Time is the `now_ms` argument.
 * Login files live in a mkdtemp directory.  Not wired into build-and-deploy.sh: host binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "jv_core.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
#define STREQ(a, b) (strcmp((a), (b)) == 0)

static JvCore core;
static JvCmds cm;
static JsonTok toks[JF_TOKENS_PAGE];
static unsigned sess_n;
static long long now;
static char dir[64], own[96], other[96];

static bool test_session(char out[33]) { snprintf(out, 33, "%032u", ++sess_n); return true; }

static void step(JvEventType t, int i)
{
    static JvEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = t; ev.i = i;
    jv_step(&core, &ev, now, &cm);
    CHECK(!cm.overflow, "command overflow on event %d", t);
}
static void send_result(const JfResult *r) { static JvEvent ev; memset(&ev, 0, sizeof ev); ev.type = JV_EV_RESULT; ev.res = *r; jv_step(&core, &ev, now, &cm); }
static void send_signin(const char *srv, const char *user, const char *pw)
{
    static JvEvent ev; memset(&ev, 0, sizeof ev); ev.type = JV_EV_SIGNIN;
    snprintf(ev.server, sizeof ev.server, "%s", srv); snprintf(ev.user, sizeof ev.user, "%s", user);
    snprintf(ev.password, sizeof ev.password, "%s", pw); jv_step(&core, &ev, now, &cm);
}

static const JvCmd *find(JvCmdType t) { for (int k = 0; k < cm.n; k++) if (cm.c[k].type == t) return &cm.c[k]; return NULL; }
static int count(JvCmdType t) { int n = 0; for (int k = 0; k < cm.n; k++) if (cm.c[k].type == t) n++; return n; }
static const JvCmd *req(JfKind kind, int api)
{
    for (int k = 0; k < cm.n; k++)
        if (cm.c[k].type == JV_CMD_REQUEST && cm.c[k].kind == kind && cm.c[k].api == api) return &cm.c[k];
    return NULL;
}
static int count_req(JfKind kind, int api)
{
    int n = 0;
    for (int k = 0; k < cm.n; k++) if (cm.c[k].type == JV_CMD_REQUEST && cm.c[k].kind == kind && cm.c[k].api == api) n++;
    return n;
}
static const JvCmd *post_to(const char *path)
{
    for (int k = 0; k < cm.n; k++)
        if (cm.c[k].type == JV_CMD_REQUEST && cm.c[k].kind == JF_KIND_POST && STREQ(cm.c[k].path, path)) return &cm.c[k];
    return NULL;
}

static char *slurp(const char *name)
{
    char path[160];
    snprintf(path, sizeof path, "tests/fixtures/jv/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(path, sizeof path, "fixtures/jv/%s", name); f = fopen(path, "rb"); }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = '\0'; fclose(f);
    return b;
}

/* answer a PARSE request the way jf_api does: the cmd's parser fills the cmd's storage, then a result */
static void reply_fixture(const JvCmd *c0, const char *fixture)
{
    JvCmd c = *c0;
    char *b = slurp(fixture);
    CHECK(b != NULL, "fixture %s", fixture);
    if (!b) return;
    JfResult r; memset(&r, 0, sizeof r);
    r.kind = JF_KIND_PARSE; r.tag = c.tag; r.fd = -1; r.dst = c.dst;
    r.parse = c.fn(b, strlen(b), toks, JF_TOKENS_PAGE, c.dst);
    r.err = r.parse == JF_PARSE_OK ? JF_ERR_NONE : JF_ERR_BADREPLY;
    free(b);
    send_result(&r);
}
static void reply_err(JfKind kind, unsigned tag, JfErr e)
{
    JfResult r; memset(&r, 0, sizeof r);
    r.kind = kind; r.tag = tag; r.err = e; r.fd = -1;
    send_result(&r);
}
static void reply_stream(unsigned tag, int fd)
{
    JfResult r; memset(&r, 0, sizeof r);
    r.kind = JF_KIND_STREAM; r.tag = tag; r.err = fd >= 0 ? JF_ERR_NONE : JF_ERR_UNREACHABLE; r.fd = fd;
    send_result(&r);
}

static void fresh(void)
{
    jv_core_init(&core, own, other, "-v");
    core.session_fn = test_session;
    sess_n = 0; now = 100000;
}

/* an own login with a token already saved: START lands on HOME and walks the probe chain */
static void home_ready(void)
{
    JfLogin l; memset(&l, 0, sizeof l);
    snprintf(l.server, sizeof l.server, "10.0.0.1:8096"); snprintf(l.user, sizeof l.user, "bob");
    snprintf(l.user_id, sizeof l.user_id, "0000000000000000000000000000abcd"); snprintf(l.token, sizeof l.token, "tok");
    snprintf(l.device_id, sizeof l.device_id, "arca-t-v");
    jf_login_save(&l, own);
    fresh();
    step(JV_EV_START, 0);
    const JvCmd *c = req(JF_KIND_PARSE, 0);
    if (!c) return;
    reply_fixture(c, "views.json");
    c = req(JF_KIND_PARSE, 0); if (c) reply_fixture(c, "resume.json");
    c = req(JF_KIND_PARSE, 0); if (c) reply_fixture(c, "nextup.json");
}

/* HOME -> SHOWS tile -> series.json loaded */
static void in_shows(void)
{
    home_ready();
    step(JV_EV_HOME, JV_TILE_SHOWS);
    const JvCmd *c = req(JF_KIND_PARSE, 0);
    if (c) reply_fixture(c, "series.json");
}

/* HOME -> MOVIES -> first row -> detail.json loaded, on DETAILS */
static void in_details(void)
{
    home_ready();
    step(JV_EV_HOME, JV_TILE_MOVIES);
    const JvCmd *c = req(JF_KIND_PARSE, 0);
    if (c) reply_fixture(c, "movies.json");
    step(JV_EV_ACTIVATE, 0);
    c = req(JF_KIND_PARSE, 0);
    if (c) reply_fixture(c, "detail.json");
}

/* DETAILS -> PLAY at start_ms -> stream fd 7 -> playing */
static void in_player(long start_ms)
{
    core.set.audio_lang[0] = '\0'; snprintf(core.set.audio_lang, sizeof core.set.audio_lang, "eng,hun");
    in_details();
    core.set.sub_mode = JV_SUB_OFF;
    step(JV_EV_PLAY, (int)start_ms);
    const JvCmd *s = req(JF_KIND_STREAM, 1);
    if (s) reply_stream(s->tag, 7);
}

/* ── tests ─────────────────────────────────────────────────────────────── */

static void test_login_reuse(void)
{
    JfLogin m; memset(&m, 0, sizeof m);
    snprintf(m.server, sizeof m.server, "10.0.0.9:8096"); snprintf(m.user, sizeof m.user, "alice");
    snprintf(m.user_id, sizeof m.user_id, "0000000000000000000000000000beef"); snprintf(m.token, sizeof m.token, "musictok");
    snprintf(m.device_id, sizeof m.device_id, "arca-t");
    jf_login_save(&m, other);
    unlink(own);

    fresh();
    step(JV_EV_START, 0);
    CHECK(core.screen == JV_SCR_SIGNIN, "no own login -> sign in");
    CHECK(core.reuse_available && core.signin_server[0] == '\0' && core.signin_user[0] == '\0', "reuse offered, nothing prefilled yet");
    step(JV_EV_REUSE_LOGIN, 0);
    CHECK(STREQ(core.signin_server, "10.0.0.9:8096") && STREQ(core.signin_user, "alice") && core.focus_password,
          "prefilled '%s' '%s' focus %d", core.signin_server, core.signin_user, core.focus_password);
    CHECK(core.login.token[0] == '\0', "the other app's token is not taken");
    CHECK(core.login.device_id[0] && strlen(core.login.device_id) > 2 &&
          STREQ(core.login.device_id + strlen(core.login.device_id) - 2, "-v"), "own device id '%s'", core.login.device_id);

    send_signin(core.signin_server, core.signin_user, "pw");
    const JvCmd *a = req(JF_KIND_AUTH, 0);
    CHECK(a && STREQ(a->user, "alice") && STREQ(a->password, "pw") && count(JV_CMD_SET_SERVER) == 1 && count(JV_CMD_SET_AUTH) == 1,
          "auth request and set-up");
    unsigned tag = a ? a->tag : 0;
    JfResult r; memset(&r, 0, sizeof r);
    r.kind = JF_KIND_AUTH; r.tag = tag; r.fd = -1; snprintf(r.token, sizeof r.token, "videotok"); snprintf(r.user_id, sizeof r.user_id, "0000000000000000000000000000f00d");
    send_result(&r);
    CHECK(core.screen == JV_SCR_HOME && STREQ(core.login.token, "videotok"), "signed in");
    JfLogin chk; jf_login_load(&chk, own, "-v");
    CHECK(STREQ(chk.token, "videotok") && STREQ(chk.user, "alice") && STREQ(chk.device_id, core.login.device_id), "own file saved with its own token");
    jf_login_load(&chk, other, "");
    CHECK(STREQ(chk.token, "musictok") && STREQ(chk.device_id, "arca-t"), "the other app's file is untouched");

    /* a wrong password stays on the screen */
    unlink(own); fresh(); step(JV_EV_START, 0);
    send_signin("10.0.0.9:8096", "alice", "bad");
    a = req(JF_KIND_AUTH, 0);
    if (a) reply_err(JF_KIND_AUTH, a->tag, JF_ERR_UNAUTH);
    CHECK(core.screen == JV_SCR_SIGNIN && core.banner[0], "wrong password -> banner");
}

static void test_home(void)
{
    home_ready();
    CHECK(core.screen == JV_SCR_HOME && core.home_ready && core.home_movies && core.home_shows, "home chain done");
    CHECK(core.home_resume_n == 0 && core.home_next_n == 0, "empty CONTINUE / NEXT UP");
    step(JV_EV_HOME, JV_TILE_RESUME);
    CHECK(core.screen == JV_SCR_HOME && cm.n == 0, "a hidden tile does nothing");
}

static void test_nav_superseded(void)
{
    in_shows();
    CHECK(core.screen == JV_SCR_LIST && core.kind == JV_K_SHOWS && core.lists[0].n > 0, "series listed (%d)", core.lists[0].n);
    jv_note_view(&core, 3, 5);
    step(JV_EV_ACTIVATE, 0);
    const JvCmd *b = req(JF_KIND_PARSE, 0);
    CHECK(b && core.screen == JV_SCR_SERIES && strstr(b->path, "/Seasons"), "seasons requested");
    unsigned stale = b ? b->tag : 0;
    step(JV_EV_BACK, 0);                               /* leaves before the reply: the seasons request is superseded */
    const JvCmd *c = req(JF_KIND_PARSE, 0);
    CHECK(c && c->group == JV_GRP_LIST && strstr(c->path, "IncludeItemTypes=Series") && c->tag != stale, "shows requested again");
    unsigned good = c ? c->tag : 0;
    reply_err(JF_KIND_PARSE, stale, JF_ERR_UNREACHABLE);       /* a late reply for the left view */
    CHECK(!core.overlay && cm.n == 0, "stale reply ignored (overlay %d, cmds %d)", core.overlay, cm.n);
    JvCmd cc = *c;
    reply_fixture(&cc, "series.json");
    CHECK(core.screen == JV_SCR_LIST && core.lists[0].n > 0 && core.restore_top == 3 && core.restore_cursor == 5 && core.restore_seq == 1,
          "pop restores the view (%d,%d,%u)", core.restore_top, core.restore_cursor, core.restore_seq);
    (void)good;
    step(JV_EV_BACK, 0);
    CHECK(core.screen == JV_SCR_HOME && req(JF_KIND_PARSE, 0) != NULL, "back to HOME re-probes");
}

static void test_detail_play_url(void)
{
    sess_n = 0;
    snprintf(core.set.audio_lang, sizeof core.set.audio_lang, "%s", "eng,hun");
    home_ready();
    snprintf(core.set.audio_lang, sizeof core.set.audio_lang, "eng,hun");
    core.set.sub_mode = JV_SUB_LANG; snprintf(core.set.sub_lang, sizeof core.set.sub_lang, "hun");
    step(JV_EV_HOME, JV_TILE_MOVIES);
    reply_fixture(req(JF_KIND_PARSE, 0), "movies.json");
    step(JV_EV_ACTIVATE, 0);
    const JvCmd *d = req(JF_KIND_PARSE, 0);
    CHECK(d && core.screen == JV_SCR_DETAILS && strstr(d->path, "/Items/") && d->fn == jv_parse_detail && d->group == JV_GRP_DETAIL, "detail requested");
    reply_fixture(d, "detail.json");
    CHECK(core.detail_ready && core.sel_audio == 1 && core.sel_sub == 1, "preferences chose audio %d sub %d", core.sel_audio, core.sel_sub);
    core.channels = 2; core.rate = 48000;
    step(JV_EV_PLAY, 90000);
    const JvCmd *s = req(JF_KIND_STREAM, 1);
    CHECK(s && strstr(s->path, "AudioStreamIndex=2") && strstr(s->path, "StartTimeTicks=900000000") &&
          strstr(s->path, "PlaySessionId=00000000000000000000000000000001") && strstr(s->path, "DeviceId=") &&
          strstr(s->path, "audioChannels=2") && strstr(s->path, "audioSampleRate=48000"), "stream url: %s", s ? s->path : "(none)");
    CHECK(core.screen == JV_SCR_PLAYER && core.play_state == JV_PLAY_FETCHING && core.play.audio_index == 2 && core.play.start_ms == 90000,
          "play request");
    reply_stream(s->tag, 7);
    CHECK(core.play_state == JV_PLAY_PLAYING && find(JV_CMD_PLAYER_START) && find(JV_CMD_PLAYER_START)->fd == 7, "player started");
    const JvCmd *p = post_to("/Sessions/Playing");
    CHECK(p && strstr(p->body, "00000000000000000000000000000001") && strstr(p->body, "\"AudioStreamIndex\":2"), "started report: %s", p ? p->body : "(none)");
    const JvCmd *sub = req(JF_KIND_STREAM, 0);
    CHECK(sub && strstr(sub->path, "/Subtitles/4/") && find(JV_CMD_SUB_CLEAR), "subtitle track fetched on the first instance");
}

static void test_progress_cadence(void)
{
    in_player(0);
    long long t0 = now;
    int progress = 0; long long at[8]; memset(at, 0, sizeof at);
    for (int k = 1; k <= 70; k++) {                    /* 35 s in 500 ms frames */
        now = t0 + 500LL * k;
        step(JV_EV_POS, 500 * k);
        step(JV_EV_TICK, 0);
        if (post_to("/Sessions/Playing/Progress") && progress < 8) at[progress++] = now - t0;
    }
    CHECK(progress == 3 && at[0] == 10000 && at[1] == 20000 && at[2] == 30000, "progress at %lld %lld %lld (n=%d)", at[0], at[1], at[2], progress);
    step(JV_EV_PAUSE, 0);
    CHECK(post_to("/Sessions/Playing/Progress") && strstr(post_to("/Sessions/Playing/Progress")->body, "\"IsPaused\":true") && find(JV_CMD_PLAYER_PAUSE), "pause report");
    int more = 0;
    for (int k = 1; k <= 40; k++) { now += 500; step(JV_EV_TICK, 0); if (cm.n) more++; }
    CHECK(more == 2, "paused: a progress ping every 10 s, nothing else (%d)", more);
}

static void test_seek_burst(void)
{
    in_player(0);
    step(JV_EV_POS, 60000);
    char old[33]; snprintf(old, sizeof old, "%s", core.play.session_id);
    int restarts = 0;
    for (int k = 0; k < 5; k++) {
        now += 100;
        step(JV_EV_SEEK_BY, 30000);
        restarts += count_req(JF_KIND_STREAM, 1);
        step(JV_EV_TICK, 0);
        restarts += count_req(JF_KIND_STREAM, 1);
    }
    CHECK(restarts == 0 && core.seek_pending && jv_core_position(&core) == 210000, "burst held (restarts %d, shown %ld)", restarts, jv_core_position(&core));
    now += 600; step(JV_EV_TICK, 0);
    CHECK(cm.n == 0, "not yet quiet for 700 ms");
    now += 100; step(JV_EV_TICK, 0);
    const JvCmd *s = req(JF_KIND_STREAM, 1);
    CHECK(count_req(JF_KIND_STREAM, 1) == 1 && s && strstr(s->path, "StartTimeTicks=2100000000") && count(JV_CMD_PLAYER_STOP) == 1, "one restart at the target");
    CHECK(req(JF_KIND_WRITE, 1) && strstr(req(JF_KIND_WRITE, 1)->path, old) && strstr(req(JF_KIND_WRITE, 1)->path, "ActiveEncodings"), "old job deleted");
    CHECK(!STREQ(old, core.play.session_id) && s && strstr(s->path, core.play.session_id), "fresh session id");
    CHECK(!core.seek_pending && core.play_state == JV_PLAY_FETCHING, "state after restart");
    int again = 0;
    for (int k = 0; k < 20; k++) { now += 100; step(JV_EV_TICK, 0); if (req(JF_KIND_STREAM, 1)) again++; }
    CHECK(again == 0, "no second restart");
}

static void test_seek_step(void)
{
    in_player(0);
    step(JV_EV_POS, 100000);
    static const long UP[] = { 110000, 140000, 200000, 500000 };   /* 10 s, 30 s, 1 min, 5 min */
    int ok = 1;
    for (int k = 0; k < 4; k++) {
        now += 100; step(JV_EV_SEEK_STEP, 1);
        if (jv_core_position(&core) != UP[k]) { ok = 0; CHECK(0, "step %d lands on %ld, wanted %ld", k, jv_core_position(&core), UP[k]); }
    }
    CHECK(ok, "ladder 10 s, 30 s, 1 min, 5 min");
    now += 100; step(JV_EV_SEEK_STEP, 1);
    CHECK(jv_core_position(&core) == 800000 || jv_core_position(&core) == core.play.runtime_ms - 1000, "capped at 5 min (%ld)", jv_core_position(&core));
    /* the other direction starts over at 10 s */
    long before = jv_core_position(&core);
    now += 100; step(JV_EV_SEEK_STEP, -1);
    CHECK(jv_core_position(&core) == before - 10000, "direction change restarts the ladder (%ld from %ld)", jv_core_position(&core), before);
    now += 100; step(JV_EV_SEEK_STEP, -1);
    CHECK(jv_core_position(&core) == before - 40000, "and climbs again (%ld)", jv_core_position(&core));
    /* quiet period: the restart fires and the next burst starts at 10 s again */
    now += 800; step(JV_EV_TICK, 0);
    const JvCmd *s = req(JF_KIND_STREAM, 1);
    CHECK(s && !core.seek_pending, "quiet period restarts the stream");
    if (s) reply_stream(s->tag, 8);
    long p0 = core.pos_ms;
    now += 100; step(JV_EV_SEEK_STEP, 1);
    CHECK(jv_core_position(&core) == p0 + 10000, "after the quiet period the ladder is back at 10 s (%ld from %ld)", jv_core_position(&core), p0);
    /* clamps at the start */
    step(JV_EV_SEEK_TO, 3000);
    now += 100; step(JV_EV_SEEK_STEP, -1);
    CHECK(jv_core_position(&core) == 0, "clamped at 0 (%ld)", jv_core_position(&core));
    /* works while paused */
    now += 800; step(JV_EV_TICK, 0);
    s = req(JF_KIND_STREAM, 1); if (s) reply_stream(s->tag, 9);
    step(JV_EV_POS, 60000);
    step(JV_EV_PAUSE, 0);
    now += 100; step(JV_EV_SEEK_STEP, 1);
    CHECK(core.play_state == JV_PLAY_PAUSED && jv_core_position(&core) == 70000, "step while paused (%ld)", jv_core_position(&core));
}

static void test_audio_switch(void)
{
    in_player(0);
    step(JV_EV_POS, 123000);
    char old[33]; snprintf(old, sizeof old, "%s", core.play.session_id);
    step(JV_EV_AUDIO_SET, 0);
    const JvCmd *s = req(JF_KIND_STREAM, 1);
    CHECK(s && strstr(s->path, "AudioStreamIndex=1") && strstr(s->path, "StartTimeTicks=1230000000") && !strstr(s->path, old) && !STREQ(old, core.play.session_id),
          "audio switch restarts at the position: %s", s ? s->path : "(none)");
    CHECK(req(JF_KIND_WRITE, 1) && strstr(req(JF_KIND_WRITE, 1)->path, old), "old job deleted");
    CHECK(!post_to("/Sessions/Playing") && post_to("/Sessions/Playing/Progress"), "reported as progress, not a second start");
    reply_stream(s->tag, 8);
    CHECK(core.play_state == JV_PLAY_PLAYING && !post_to("/Sessions/Playing"), "second stream playing without a second start");
    /* a subtitle change is only an SRT reload */
    step(JV_EV_SUB_SET, 1);
    CHECK(count_req(JF_KIND_STREAM, 1) == 0 && req(JF_KIND_STREAM, 0) && count(JV_CMD_PLAYER_STOP) == 0 && core.play.sub_index == 4, "subtitle change reloads only the SRT");
}

static void test_stop(void)
{
    in_player(0);
    step(JV_EV_POS, 45000);
    char sid[33]; snprintf(sid, sizeof sid, "%s", core.play.session_id);
    step(JV_EV_STOP, 0);
    const JvCmd *p = post_to("/Sessions/Playing/Stopped");
    CHECK(p && strstr(p->body, sid) && strstr(p->body, "\"PositionTicks\":450000000"), "stopped report: %s", p ? p->body : "(none)");
    const JvCmd *w = req(JF_KIND_WRITE, 1);
    CHECK(w && STREQ(w->method, "DELETE") && strstr(w->path, "ActiveEncodings") && strstr(w->path, sid), "DELETE ActiveEncodings");
    CHECK(count(JV_CMD_PLAYER_STOP) == 1 && core.play_state == JV_PLAY_IDLE && core.screen == JV_SCR_DETAILS && req(JF_KIND_PARSE, 0), "back on details, detail refreshed");
    /* quit while playing: the same reports, then EXIT last */
    in_player(0);
    step(JV_EV_QUIT, 0);
    CHECK(post_to("/Sessions/Playing/Stopped") && req(JF_KIND_WRITE, 1) && cm.c[cm.n - 1].type == JV_CMD_EXIT, "quit reports before it exits");
}

static void test_failed_post(void)
{
    in_player(0);
    /* the started report went out with the stream result; fail it, and a DELETE too */
    unsigned started_tag = 0;
    {   /* replay the start: stop, play again, find the tag */
        step(JV_EV_STOP, 0);
        step(JV_EV_PLAY, 0);
        const JvCmd *s = req(JF_KIND_STREAM, 1);
        reply_stream(s->tag, 9);
        const JvCmd *p = post_to("/Sessions/Playing");
        started_tag = p ? p->tag : 0;
    }
    CHECK(started_tag != 0, "started report tag");
    reply_err(JF_KIND_POST, started_tag, JF_ERR_UNREACHABLE);
    CHECK(cm.n == 0 && core.report_fail == 1, "failed POST: no command out (%d), counted %d", cm.n, core.report_fail);
    reply_err(JF_KIND_WRITE, 12345, JF_ERR_SERVER);
    CHECK(cm.n == 0 && core.report_fail == 2, "failed DELETE is not retried either");
    int starts = 0, progress = 0;
    for (int k = 0; k < 50; k++) {
        now += 500; step(JV_EV_POS, 500 * (k + 1)); step(JV_EV_TICK, 0);
        if (post_to("/Sessions/Playing")) starts++;
        if (post_to("/Sessions/Playing/Progress")) progress++;
    }
    CHECK(starts == 0 && progress == 2, "later: no repeat of the started report (%d), the timer's reports go on (%d)", starts, progress);
}

/* ── playback control: instances, DELETE safety, paused restarts ───────── */

static int writes_for(const char *sid, int api)
{
    int n = 0;
    for (int k = 0; k < cm.n; k++)
        if (cm.c[k].type == JV_CMD_REQUEST && cm.c[k].kind == JF_KIND_WRITE && cm.c[k].api == api && STREQ(cm.c[k].method, "DELETE") && strstr(cm.c[k].path, sid)) n++;
    return n;
}

static void test_api_instances(void)
{
    in_player(0);
    step(JV_EV_POS, 45000);
    char sid[33]; snprintf(sid, sizeof sid, "%s", core.play.session_id);
    step(JV_EV_AUDIO_SET, 0);
    const JvCmd *s = req(JF_KIND_STREAM, 1);
    const JvCmd *pr = post_to("/Sessions/Playing/Progress");
    CHECK(s && count_req(JF_KIND_STREAM, 0) == 0, "stream on the second instance");
    CHECK(pr && pr->api == 1 && writes_for(sid, 1) == 1 && writes_for(sid, 0) == 0, "restart: report and DELETE on the second instance");
    if (!s) return;
    reply_stream(s->tag, 8);
    step(JV_EV_STOP, 0);
    const JvCmd *st = post_to("/Sessions/Playing/Stopped");
    CHECK(st && st->api == 1 && writes_for(core.play.session_id, 1) == 1, "stop: Stopped report and DELETE on the second instance");
    CHECK(req(JF_KIND_PARSE, 0) != NULL, "metadata stays on the first");
}

/* a DELETE that the ring refused (the loop answers BADREPLY) or the server failed is sent again, then counted */
static void test_delete_retry(void)
{
    in_player(0);
    char sid[33]; snprintf(sid, sizeof sid, "%s", core.play.session_id);
    step(JV_EV_STOP, 0);
    const JvCmd *w = req(JF_KIND_WRITE, 1);
    unsigned tag = w ? w->tag : 0;
    CHECK(tag && writes_for(sid, 1) == 1, "first DELETE");
    int fail0 = core.report_fail;
    reply_err(JF_KIND_WRITE, tag, JF_ERR_BADREPLY);
    CHECK(cm.n == 0 && core.report_fail == fail0, "failure alone sends nothing yet");
    now += 600; step(JV_EV_TICK, 0);
    w = req(JF_KIND_WRITE, 1);
    CHECK(w && writes_for(sid, 1) == 1, "DELETE sent again after a failure");
    if (!w) return;
    reply_err(JF_KIND_WRITE, w->tag, JF_ERR_BADREPLY);
    now += 600; step(JV_EV_TICK, 0);
    w = req(JF_KIND_WRITE, 1);
    CHECK(w && writes_for(sid, 1) == 1, "and a third time");
    if (!w) return;
    reply_err(JF_KIND_WRITE, w->tag, JF_ERR_BADREPLY);
    CHECK(core.report_fail == fail0 + 1, "counted once, when the retries are used up");
    for (int k = 0; k < 20; k++) { now += 600; step(JV_EV_TICK, 0); CHECK(cm.n == 0, "no fourth DELETE"); }
    /* a DELETE that went through is not repeated */
    in_player(0);
    snprintf(sid, sizeof sid, "%s", core.play.session_id);
    step(JV_EV_STOP, 0);
    w = req(JF_KIND_WRITE, 1);
    if (w) reply_err(JF_KIND_WRITE, w->tag, JF_ERR_NONE);
    int again = 0;
    for (int k = 0; k < 20; k++) { now += 600; step(JV_EV_TICK, 0); again += writes_for(sid, 1); }
    CHECK(again == 0, "an answered DELETE is not repeated (%d)", again);
}

/* cancelling a stream that is still opening: the server may not have its job yet, so the DELETE goes twice */
static void test_cancelled_open_second_delete(void)
{
    in_player(0);
    step(JV_EV_POS, 45000);
    step(JV_EV_AUDIO_SET, 0);                       /* FETCHING */
    char fetching[33]; snprintf(fetching, sizeof fetching, "%s", core.play.session_id);
    step(JV_EV_AUDIO_SET, 1);                       /* cancels the open */
    CHECK(writes_for(fetching, 1) == 1, "DELETE at once");
    now += 1000; step(JV_EV_TICK, 0);
    CHECK(writes_for(fetching, 1) == 0, "not yet at 1 s");
    now += 600; step(JV_EV_TICK, 0);
    CHECK(writes_for(fetching, 1) == 1, "again at about 1.5 s");
    int more = 0;
    for (int k = 0; k < 20; k++) { now += 500; step(JV_EV_TICK, 0); more += writes_for(fetching, 1); }
    CHECK(more == 0, "and no more (%d)", more);
    /* stop during an open */
    char cur[33]; snprintf(cur, sizeof cur, "%s", core.play.session_id);
    step(JV_EV_STOP, 0);
    CHECK(writes_for(cur, 1) == 1, "stop during open: DELETE at once");
    now += 1600; step(JV_EV_TICK, 0);
    CHECK(writes_for(cur, 1) == 1, "stop during open: and again");
    /* a stream that was playing is stopped once only */
    in_player(0);
    snprintf(cur, sizeof cur, "%s", core.play.session_id);
    step(JV_EV_STOP, 0);
    now += 1600; step(JV_EV_TICK, 0);
    CHECK(writes_for(cur, 1) == 0, "a playing stream DELETE is not doubled");
}

static void test_open_failure(void)
{
    in_player(0);
    step(JV_EV_POS, 60000);
    step(JV_EV_SEEK_BY, 30000);
    now += 800; step(JV_EV_TICK, 0);                /* restart at 90 s */
    char sid[33]; snprintf(sid, sizeof sid, "%s", core.play.session_id);
    CHECK(core.play_state == JV_PLAY_FETCHING && core.busy, "opening: FETCHING counts as busy");
    step(JV_EV_SEEK_BY, 30000);                     /* pressed during the open: pending target 120 s */
    reply_stream(core.stream_tag, -1);
    CHECK(writes_for(sid, 1) == 1, "open failed: the new session job is ended");
    const JvCmd *st = post_to("/Sessions/Playing/Stopped");
    CHECK(st && strstr(st->body, "\"PositionTicks\":1200000000"), "open failed: Stopped at the position shown: %s", st ? st->body : "(none)");
    CHECK(core.play_state == JV_PLAY_IDLE && core.screen == JV_SCR_DETAILS && core.overlay, "open failed: back on the details with the error");
}

static void test_paused_restart(void)
{
    in_player(0);
    step(JV_EV_POS, 60000);
    step(JV_EV_PAUSE, 0);
    step(JV_EV_SEEK_BY, 30000);
    now += 800; step(JV_EV_TICK, 0);
    const JvCmd *pr = post_to("/Sessions/Playing/Progress");
    CHECK(pr && strstr(pr->body, "\"IsPaused\":true"), "restart report says paused: %s", pr ? pr->body : "(none)");
    CHECK(core.busy, "busy while the stream opens");
    reply_stream(core.stream_tag, 9);
    CHECK(core.play_state == JV_PLAY_PAUSED && find(JV_CMD_PLAYER_START) && find(JV_CMD_PLAYER_PAUSE), "seek while paused stays paused");
    CHECK(!core.busy, "not busy once open");
    step(JV_EV_PAUSE, 0);
    CHECK(core.play_state == JV_PLAY_PLAYING && find(JV_CMD_PLAYER_RESUME), "and resumes on the next toggle");
    /* an audio switch while paused */
    step(JV_EV_PAUSE, 0);
    step(JV_EV_AUDIO_SET, 0);
    reply_stream(core.stream_tag, 10);
    CHECK(core.play_state == JV_PLAY_PAUSED && find(JV_CMD_PLAYER_PAUSE), "audio switch while paused stays paused");
    /* a restart while playing is not paused */
    step(JV_EV_PAUSE, 0);
    step(JV_EV_AUDIO_SET, 1);
    reply_stream(core.stream_tag, 11);
    CHECK(core.play_state == JV_PLAY_PLAYING && !find(JV_CMD_PLAYER_PAUSE), "a restart while playing plays");
}

static void test_paused_progress(void)
{
    in_player(0);
    step(JV_EV_POS, 60000);
    step(JV_EV_PAUSE, 0);
    int n = 0; long long at[6]; memset(at, 0, sizeof at); long long t0 = now;
    for (int k = 1; k <= 70; k++) {
        now = t0 + 500LL * k; step(JV_EV_TICK, 0);
        const JvCmd *p = post_to("/Sessions/Playing/Progress");
        if (p) { CHECK(strstr(p->body, "\"IsPaused\":true") != NULL, "paused report body"); if (n < 6) at[n] = now - t0; n++; }
    }
    CHECK(n == 3 && at[0] == 10000 && at[2] == 30000, "paused progress every 10 s (n=%d)", n);
    CHECK(core.play_state == JV_PLAY_PAUSED, "still paused");
}

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/jvcoreXXXXXX");
    if (!mkdtemp(dir)) { printf("FAIL mkdtemp\n"); return 1; }
    snprintf(own, sizeof own, "%s/own.conf", dir);
    snprintf(other, sizeof other, "%s/other.conf", dir);
    test_login_reuse();
    test_home();
    test_nav_superseded();
    test_detail_play_url();
    test_progress_cadence();
    test_seek_burst();
    test_seek_step();
    test_audio_switch();
    test_stop();
    test_failed_post();
    test_api_instances();
    test_delete_retry();
    test_cancelled_open_second_delete();
    test_open_failure();
    test_paused_restart();
    test_paused_progress();
    unlink(own); unlink(other); rmdir(dir);
    printf("jv_core_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
