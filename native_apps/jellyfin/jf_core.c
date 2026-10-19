/*
 * jf_core.c — see jf_core.h.  One event in, up to JF_CMDS_MAX commands out.
 */

#include "jf_core.h"
#include "jf_url.h"

#include <stdio.h>
#include <string.h>

static const int PAGE[JF_LIST_COUNT] = { JF_PAGE_ARTISTS, JF_PAGE_ALBUMS, JF_PAGE_TRACKS, JF_PAGE_SEARCH };

/* ── small helpers ─────────────────────────────────────────────────────── */

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

int jf_volume_mdb(int step)
{
    if (step <= 0) return -100000;
    return (step - 10) * 3000;
}

const JfItem *jf_now(const JfApp *app)
{
    return app->queue_n > 0 && app->queue_cur >= 0 && app->queue_cur < app->queue_n
               ? &app->queue[app->queue_cur] : NULL;
}

static JfCmd *emit(JfCmds *out, JfCmdType type)
{
    static JfCmd scratch;
    if (out->n >= JF_CMDS_MAX) { out->overflow = true; memset(&scratch, 0, sizeof(scratch)); return &scratch; }
    JfCmd *c = &out->c[out->n++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->fd = -1;
    return c;
}

static void set_banner(JfApp *app, const char *text)
{
    copy(app->banner, sizeof(app->banner), text);
    app->banner_seq++;
}

static void set_overlay(JfApp *app, const char *text, unsigned buttons)
{
    app->overlay = true;
    copy(app->overlay_text, sizeof(app->overlay_text), text);
    app->overlay_buttons = buttons;
}

static int list_of_screen(JfScreen s)
{
    switch (s) {
    case JF_SCR_ARTISTS: return JF_LIST_ARTISTS;
    case JF_SCR_ALBUMS:  return JF_LIST_ALBUMS;
    case JF_SCR_TRACKS:  return JF_LIST_TRACKS;
    case JF_SCR_SEARCH:  return JF_LIST_SEARCH;
    default:             return -1;
    }
}

static int cur_list(const JfApp *app) { return list_of_screen(app->screen); }

static bool have_token(const JfApp *app) { return app->login.token[0] != '\0'; }

/* ── list requests ─────────────────────────────────────────────────────── */

static const char *const CAT_NAME[JF_CAT_COUNT] = { "ALBUMS", "ARTISTS", "PLAYLISTS", "GENRES", "SONGS" };

static int build_path(const JfApp *app, int lid, int start, char *out, size_t cap)
{
    const char *uid = app->login.user_id;
    const char *id = app->ctx[lid].id;
    int sub = app->ctx[lid].sub;
    int lim = PAGE[lid];
    switch (lid) {
    case JF_LIST_ARTISTS:
        switch (app->cat) {
        case JF_CAT_ALBUMS:    return jf_path_albums_all(out, cap, uid, start, lim);
        case JF_CAT_PLAYLISTS: return jf_path_playlists(out, cap, uid, start, lim);
        case JF_CAT_GENRES:    return jf_path_genres(out, cap, uid, start, lim);
        case JF_CAT_SONGS:     return jf_path_songs(out, cap, uid, start, JF_PAGE_TRACKS);
        default:               return jf_path_artists(out, cap, uid, start, lim);
        }
    case JF_LIST_ALBUMS:
        return sub == JF_SUB_GENRE ? jf_path_genre_albums(out, cap, uid, id, start, lim)
                                   : jf_path_albums(out, cap, uid, id, start, lim);
    case JF_LIST_TRACKS:
        return sub == JF_SUB_PLAYLIST ? jf_path_playlist_items(out, cap, uid, id, start, lim)
                                      : jf_path_tracks(out, cap, uid, id, start, lim);
    default:              return jf_path_search(out, cap, uid, id, start, lim);
    }
}

/* Request the page that starts at `start`.  A new LIST supersedes every other
 * queued or in-flight LIST in jf_api, so the other lists stop being pending. */
static bool request_page(JfApp *app, int lid, int start, JfCmds *out)
{
    JfCmd *c = emit(out, JF_CMD_REQUEST);
    c->kind = JF_KIND_LIST;
    c->list = &app->lists[lid];
    c->tag = c->list->generation;
    if (build_path(app, lid, start, c->path, sizeof(c->path)) < 0) {
        out->n--;
        set_banner(app, "BAD REQUEST");
        return false;
    }
    for (int k = 0; k < JF_LIST_COUNT; k++) app->lists[k].pending = false;
    app->lists[lid].pending = true;
    app->req_loaded[lid] = app->lists[lid].loaded;
    app->retry = *c;
    return true;
}

/* Point a list at a new subject: empty it, bump its generation, ask for page one. */
static void retarget(JfApp *app, int lid, const char *id, const char *title, int sub, JfCmds *out)
{
    JfList *l = &app->lists[lid];
    app->ctx[lid].sub = sub;
    copy(app->ctx[lid].id, sizeof(app->ctx[lid].id), id);
    copy(app->ctx[lid].title, sizeof(app->ctx[lid].title), title);
    jf_list_clear(l);
    l->pending = false;
    l->generation++;
    app->play_wait = false;
    request_page(app, lid, 0, out);
}

/* ── navigation ────────────────────────────────────────────────────────── */

static void nav_push(JfApp *app)
{
    if (app->nav_n == JF_NAV_MAX) {
        memmove(&app->nav[0], &app->nav[1], sizeof(app->nav[0]) * (JF_NAV_MAX - 1));
        app->nav_n--;
    }
    JfNavEntry *e = &app->nav[app->nav_n++];
    int lid = cur_list(app);
    e->screen = app->screen;
    e->sub = lid >= 0 ? app->ctx[lid].sub : 0;
    copy(e->id, sizeof(e->id), lid >= 0 ? app->ctx[lid].id : "");
    copy(e->title, sizeof(e->title), lid >= 0 ? app->ctx[lid].title : "");
}

/* Show a list screen on a new subject, with the current screen below it. */
static void enter(JfApp *app, JfScreen scr, const char *id, const char *title, int sub, JfCmds *out)
{
    int lid = list_of_screen(scr);
    nav_push(app);
    app->screen = scr;
    retarget(app, lid, id, title, sub, out);
}

static void pop(JfApp *app, JfCmds *out)
{
    JfNavEntry e = app->nav[--app->nav_n];
    app->play_wait = false;
    app->screen = e.screen;
    int lid = list_of_screen(e.screen);
    /* The list is shared by every visit to its screen: if another visit
     * re-aimed it, aim it back rather than show the wrong rows. */
    if (lid >= 0 && (strcmp(app->ctx[lid].id, e.id) != 0 || app->ctx[lid].sub != e.sub))
        retarget(app, lid, e.id, e.title, e.sub, out);
}

static void go_signin(JfApp *app)
{
    app->screen = JF_SCR_SIGNIN;
    app->nav_n = 0;
    app->play_wait = false;
}

static void refresh(JfApp *app)
{
    int lid = cur_list(app);
    app->list = lid >= 0 ? &app->lists[lid] : NULL;
    switch (app->screen) {
    case JF_SCR_SIGNIN:     copy(app->crumb, sizeof(app->crumb), "SIGN IN"); break;
    case JF_SCR_ARTISTS:    copy(app->crumb, sizeof(app->crumb), CAT_NAME[app->cat]); break;
    case JF_SCR_ALBUMS:
    case JF_SCR_TRACKS:     copy(app->crumb, sizeof(app->crumb), app->ctx[lid].title); break;
    case JF_SCR_SEARCH:     snprintf(app->crumb, sizeof(app->crumb), "SEARCH: %s", app->ctx[lid].id); break;
    case JF_SCR_NOWPLAYING: copy(app->crumb, sizeof(app->crumb), "NOW PLAYING"); break;
    case JF_SCR_QUEUE:      copy(app->crumb, sizeof(app->crumb), "QUEUE"); break;
    }
    bool any = app->auth_pending || app->play_wait || app->play_state == JF_PLAY_FETCHING;
    for (int k = 0; k < JF_LIST_COUNT; k++) any = any || app->lists[k].pending;
    app->busy = any;
}

/* ── playback ──────────────────────────────────────────────────────────── */

static void play_track(JfApp *app, int idx, bool stop_first, JfCmds *out);

static void fail_stop(JfApp *app, JfCmds *out)
{
    app->play_state = JF_PLAY_IDLE;
    set_overlay(app, "PLAYBACK FAILED", JF_BTN_CLOSE);
    emit(out, JF_CMD_STOP);
}

static void advance(JfApp *app, JfCmds *out)
{
    if (app->queue_cur + 1 < app->queue_n) {
        play_track(app, app->queue_cur + 1, false, out);
    } else {
        app->play_state = JF_PLAY_END;
        emit(out, JF_CMD_STOP);
    }
}

static void track_failed(JfApp *app, JfCmds *out)
{
    if (++app->fail_streak >= JF_FAIL_LIMIT) { fail_stop(app, out); return; }
    advance(app, out);
}

static void play_track(JfApp *app, int idx, bool stop_first, JfCmds *out)
{
    if (stop_first) emit(out, JF_CMD_STOP);
    app->queue_cur = idx;
    app->play_state = JF_PLAY_FETCHING;
    char sess[33];
    JfCmd *c = emit(out, JF_CMD_PLAY_STREAM);
    c->kind = JF_KIND_STREAM;
    c->tag = ++app->stream_tag;
    c->norm_mdb = app->queue[idx].norm_gain_mdb;
    c->user_mdb = jf_volume_mdb(app->volume_step);
    if (!app->session_fn || !app->session_fn(sess) ||
        jf_path_stream(c->path, sizeof(c->path), app->queue[idx].id, sess) < 0) {
        out->n--;
        track_failed(app, out);
    }
}

/* Fill the queue from list `lid` starting at row `from` (tracks only) and start it. */
static void start_queue(JfApp *app, int lid, int from, JfCmds *out)
{
    const JfList *l = &app->lists[lid];
    app->queue_n = 0;
    for (int k = from; k < l->n && app->queue_n < JF_QUEUE_MAX; k++)
        if (l->items[k].type == JF_ITEM_TRACK) app->queue[app->queue_n++] = l->items[k];
    if (app->queue_n == 0) return;
    app->fail_streak = 0;
    play_track(app, 0, app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING, out);
}

/* An album's tracks are queued whole: if pages are missing, fetch them first. */
static void play_from(JfApp *app, int lid, int from, JfCmds *out)
{
    const JfList *l = &app->lists[lid];
    if (lid == JF_LIST_TRACKS && l->loaded < l->total && !l->full) {
        app->play_wait = true;
        app->play_wait_from = from;
        if (!l->pending) request_page(app, lid, l->loaded, out);
        return;
    }
    start_queue(app, lid, from, out);
}

/* ── session loss and error text ───────────────────────────────────────── */

static void session_expired(JfApp *app, JfCmds *out)
{
    jf_wipe(app->login.token, sizeof(app->login.token));
    app->login.user_id[0] = '\0';
    emit(out, JF_CMD_CLEAR_LOGIN);
    bool any = false;
    for (int k = 0; k < JF_LIST_COUNT; k++) { any = any || app->lists[k].pending; app->lists[k].pending = false; }
    if (any) emit(out, JF_CMD_CANCEL)->kind = JF_KIND_LIST;
    if (app->play_state == JF_PLAY_FETCHING) {
        emit(out, JF_CMD_CANCEL)->kind = JF_KIND_STREAM;
        app->play_state = JF_PLAY_IDLE;
    }
    app->overlay = false;
    app->auth_pending = false;
    go_signin(app);
    set_banner(app, "SESSION EXPIRED, SIGN IN AGAIN");
}

/* The status is named for a reply nobody planned for, so a panel report says which one. */
static const char *err_text(char *buf, size_t cap, JfErr e, int status)
{
    const char *t;
    switch (e) {
    case JF_ERR_UNREACHABLE: return "SERVER UNREACHABLE";
    case JF_ERR_FORBIDDEN:   return "ACCESS DENIED";
    case JF_ERR_NOTFOUND:    return "NOT FOUND";
    case JF_ERR_SERVER:      t = "SERVER ERROR"; break;
    default:                 t = "UNEXPECTED REPLY"; break;
    }
    if (status <= 0 || status == 200) return t;
    snprintf(buf, cap, "%s (HTTP %d)", t, status);
    return buf;
}

/* ── results ───────────────────────────────────────────────────────────── */

static void on_auth(JfApp *app, const JfResult *r, JfCmds *out)
{
    if (!app->auth_pending || r->tag != app->auth_tag) return;
    app->auth_pending = false;
    if (r->err == JF_ERR_UNAUTH) { set_banner(app, "WRONG USER OR PASSWORD"); return; }
    if (r->err != JF_ERR_NONE || !r->token[0] || !r->user_id[0]) { char t[48]; set_banner(app, err_text(t, sizeof t, r->err, r->status)); return; }
    copy(app->login.token, sizeof(app->login.token), r->token);
    copy(app->login.user_id, sizeof(app->login.user_id), r->user_id);
    emit(out, JF_CMD_SET_AUTH);
    emit(out, JF_CMD_SAVE_LOGIN);
    app->nav_n = 0;
    app->screen = JF_SCR_ARTISTS;
    app->cat = JF_CAT_ARTISTS;
    retarget(app, JF_LIST_ARTISTS, "", CAT_NAME[app->cat], JF_SUB_PLAIN, out);
}

static void on_list(JfApp *app, const JfResult *r, JfCmds *out)
{
    int lid = -1;
    for (int k = 0; k < JF_LIST_COUNT; k++) if (r->list == &app->lists[k]) lid = k;
    if (lid < 0) return;
    JfList *l = &app->lists[lid];
    if (r->tag != l->generation || !l->pending) return;   /* stale: re-aimed or superseded */
    l->pending = false;
    if (r->err == JF_ERR_UNAUTH) { session_expired(app, out); return; }
    JfErr e = r->err;
    if (e == JF_ERR_NONE && r->parse != JF_PARSE_OK) e = JF_ERR_BADREPLY;
    if (e != JF_ERR_NONE) {
        bool again = e == JF_ERR_UNREACHABLE || e == JF_ERR_SERVER;
        app->play_wait = false;
        char t[48];
        set_overlay(app, err_text(t, sizeof t, e, r->status), again ? (JF_BTN_RETRY | JF_BTN_CLOSE) : JF_BTN_CLOSE);
        return;
    }
    /* A page that consumed nothing while the server claims more would be asked for forever. */
    if (l->loaded <= app->req_loaded[lid]) l->total = l->loaded;
    if (app->play_wait && lid == JF_LIST_TRACKS) {
        if (l->loaded < l->total && !l->full) request_page(app, lid, l->loaded, out);
        else { app->play_wait = false; start_queue(app, lid, app->play_wait_from, out); }
    }
}

static void on_stream(JfApp *app, const JfResult *r, JfCmds *out)
{
    bool want = app->play_state == JF_PLAY_FETCHING && r->tag == app->stream_tag && have_token(app);
    if (r->err == JF_ERR_UNAUTH && r->tag == app->stream_tag && app->play_state == JF_PLAY_FETCHING) {
        session_expired(app, out);
        return;
    }
    if (!want) {
        if (r->err == JF_ERR_NONE && r->fd >= 0) emit(out, JF_CMD_CLOSE_FD)->fd = r->fd;
        return;
    }
    if (r->err != JF_ERR_NONE || r->fd < 0) { track_failed(app, out); return; }
    JfCmd *c = emit(out, JF_CMD_START_AUDIO);
    c->fd = r->fd;
    c->tag = r->tag;
    c->norm_mdb = app->queue[app->queue_cur].norm_gain_mdb;
    c->user_mdb = jf_volume_mdb(app->volume_step);
}

static void on_result(JfApp *app, const JfResult *r, JfCmds *out)
{
    switch (r->kind) {
    case JF_KIND_AUTH:   on_auth(app, r, out); break;
    case JF_KIND_LIST:   on_list(app, r, out); break;
    case JF_KIND_STREAM: on_stream(app, r, out); break;
    default: break;
    }
}

/* ── user intents ──────────────────────────────────────────────────────── */

static void on_start(JfApp *app, const JfLogin *lg, JfCmds *out)
{
    app->login = *lg;
    int v = lg->volume_step;
    app->volume_step = app->login.volume_step = v < 0 ? 0 : v > JF_VOLUME_MAX ? JF_VOLUME_MAX : v;
    char host[JF_HOST_MAX];
    int port = 0;
    if (lg->token[0] && lg->user_id[0] && jf_parse_server(lg->server, host, sizeof(host), &port) == JF_SERVER_OK) {
        JfCmd *c = emit(out, JF_CMD_SET_SERVER);
        copy(c->host, sizeof(c->host), host);
        c->port = port;
        emit(out, JF_CMD_SET_AUTH);
        app->screen = JF_SCR_ARTISTS;
        app->cat = JF_CAT_ARTISTS;
        retarget(app, JF_LIST_ARTISTS, "", CAT_NAME[app->cat], JF_SUB_PLAIN, out);
    } else {
        go_signin(app);
    }
}

static void on_signin(JfApp *app, const JfEvent *ev, JfCmds *out)
{
    if (app->screen != JF_SCR_SIGNIN) return;
    char host[JF_HOST_MAX];
    int port = 0;
    switch (jf_parse_server(ev->server, host, sizeof(host), &port)) {
    case JF_SERVER_OK: break;
    case JF_SERVER_EMPTY: set_banner(app, "ENTER THE SERVER ADDRESS"); return;
    case JF_SERVER_HTTPS: set_banner(app, "HTTP ONLY"); return;
    default: set_banner(app, "BAD SERVER ADDRESS"); return;
    }
    if (!ev->user[0]) { set_banner(app, "ENTER THE USER NAME"); return; }
    char canon[JF_HOST_MAX + 8];
    snprintf(canon, sizeof(canon), "%s:%d", host, port);
    if (strlen(canon) >= sizeof(app->login.server)) { set_banner(app, "BAD SERVER ADDRESS"); return; }
    memcpy(app->login.server, canon, strlen(canon) + 1);
    copy(app->login.user, sizeof(app->login.user), ev->user);
    JfCmd *s = emit(out, JF_CMD_SET_SERVER);
    copy(s->host, sizeof(s->host), host);
    s->port = port;
    emit(out, JF_CMD_SET_AUTH);   /* the device identity: the server refuses an empty DeviceId */
    JfCmd *c = emit(out, JF_CMD_REQUEST);
    c->kind = JF_KIND_AUTH;
    c->tag = ++app->auth_tag;
    copy(c->user, sizeof(c->user), ev->user);
    copy(c->password, sizeof(c->password), ev->password);
    app->auth_pending = true;
}

static void on_activate(JfApp *app, int i, JfCmds *out)
{
    int lid = cur_list(app);
    if (lid < 0 || i < 0 || i >= app->lists[lid].n) return;
    const JfItem *it = &app->lists[lid].items[i];
    switch (it->type) {
    case JF_ITEM_ARTIST:   enter(app, JF_SCR_ALBUMS, it->id, it->name, JF_SUB_PLAIN, out); break;
    case JF_ITEM_GENRE:    enter(app, JF_SCR_ALBUMS, it->id, it->name, JF_SUB_GENRE, out); break;
    case JF_ITEM_ALBUM:    enter(app, JF_SCR_TRACKS, it->id, it->name, JF_SUB_PLAIN, out); break;
    case JF_ITEM_PLAYLIST: enter(app, JF_SCR_TRACKS, it->id, it->name, JF_SUB_PLAYLIST, out); break;
    case JF_ITEM_TRACK:  play_from(app, lid, i, out); break;
    default: break;
    }
}

static void on_play_all(JfApp *app, JfCmds *out)
{
    int lid = cur_list(app);
    if (lid != JF_LIST_TRACKS && lid != JF_LIST_SEARCH) return;
    const JfList *l = &app->lists[lid];
    for (int k = 0; k < l->n; k++)
        if (l->items[k].type == JF_ITEM_TRACK) { play_from(app, lid, k, out); return; }
}

static void on_next(JfApp *app, JfCmds *out)
{
    if (app->queue_n == 0 || !have_token(app)) return;
    bool live = app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING;
    app->fail_streak = 0;
    if (app->queue_cur + 1 < app->queue_n) {
        play_track(app, app->queue_cur + 1, live, out);
    } else {
        if (app->play_state == JF_PLAY_FETCHING) emit(out, JF_CMD_CANCEL)->kind = JF_KIND_STREAM;
        app->play_state = JF_PLAY_END;
        emit(out, JF_CMD_STOP);
    }
}

static void on_volume(JfApp *app, int delta, JfCmds *out)
{
    int v = app->volume_step + delta;
    if (v < 0 || v > JF_VOLUME_MAX) return;
    app->volume_step = app->login.volume_step = v;
    JfCmd *g = emit(out, JF_CMD_SET_GAIN);
    const JfItem *now = jf_now(app);
    g->norm_mdb = now ? now->norm_gain_mdb : 0;
    g->user_mdb = jf_volume_mdb(v);
    emit(out, JF_CMD_SAVE_LOGIN);
}

static void on_search(JfApp *app, const char *term, JfCmds *out)
{
    if (!term[0]) {
        if (app->lists[JF_LIST_SEARCH].pending) {
            app->lists[JF_LIST_SEARCH].pending = false;
            emit(out, JF_CMD_CANCEL)->kind = JF_KIND_LIST;
        }
        return;
    }
    if (cur_list(app) < 0) return;
    char t[96];
    copy(t, sizeof(t), term);
    if (app->screen == JF_SCR_SEARCH) retarget(app, JF_LIST_SEARCH, t, "", JF_SUB_PLAIN, out);
    else enter(app, JF_SCR_SEARCH, t, "", JF_SUB_PLAIN, out);
}

static void on_back(JfApp *app, JfCmds *out)
{
    if (app->screen == JF_SCR_SIGNIN || app->nav_n == 0) {
        emit(out, JF_CMD_STOP);
        emit(out, JF_CMD_EXIT);
        return;
    }
    pop(app, out);
}

/* Root level only: step the category and reload the root list (the nav stack is empty, so it does not grow). */
static void on_category(JfApp *app, int dir, JfCmds *out)
{
    if (app->screen != JF_SCR_ARTISTS || app->nav_n != 0 || !have_token(app) || dir == 0) return;
    app->cat = (JfCat)((app->cat + (dir > 0 ? 1 : JF_CAT_COUNT - 1)) % JF_CAT_COUNT);
    retarget(app, JF_LIST_ARTISTS, "", CAT_NAME[app->cat], JF_SUB_PLAIN, out);
}

static void on_need_more(JfApp *app, JfCmds *out)
{
    int lid = cur_list(app);
    if (lid < 0 || !have_token(app)) return;
    const JfList *l = &app->lists[lid];
    if (l->loaded >= l->total || l->pending || l->full) return;
    request_page(app, lid, l->loaded, out);
}

static void on_track_ended(JfApp *app, int played_ms, JfCmds *out)
{
    if (app->play_state != JF_PLAY_PLAYING) return;
    if (!have_token(app)) { app->play_state = JF_PLAY_IDLE; return; }   /* expired: fetch nothing */
    if (played_ms < JF_SHORT_MS) { track_failed(app, out); return; }
    app->fail_streak = 0;
    advance(app, out);
}

void jf_app_init(JfApp *app)
{
    memset(app, 0, sizeof(*app));
    jf_list_init(&app->lists[JF_LIST_ARTISTS], app->store_artists, JF_CAP_ARTISTS);
    jf_list_init(&app->lists[JF_LIST_ALBUMS], app->store_albums, JF_CAP_ALBUMS);
    jf_list_init(&app->lists[JF_LIST_TRACKS], app->store_tracks, JF_CAP_TRACKS);
    jf_list_init(&app->lists[JF_LIST_SEARCH], app->store_search, JF_CAP_SEARCH);
    app->cat = JF_CAT_ARTISTS;
    app->session_fn = jf_session_id;
    app->volume_step = JF_VOLUME_DEFAULT;
    app->login.volume_step = JF_VOLUME_DEFAULT;
    app->screen = JF_SCR_SIGNIN;
    refresh(app);
}

void jf_step(JfApp *app, const JfEvent *ev, JfCmds *out)
{
    out->n = 0;
    out->overflow = false;

    switch (ev->type) {
    case JF_EV_RESULT:         on_result(app, &ev->res, out); goto done;
    case JF_EV_STREAM_STARTED:
        if (app->play_state == JF_PLAY_FETCHING) app->play_state = JF_PLAY_PLAYING;
        goto done;
    case JF_EV_TRACK_ENDED:    on_track_ended(app, ev->played_ms, out); goto done;
    case JF_EV_START:          on_start(app, &ev->login, out); goto done;
    case JF_EV_QUIT:
        emit(out, JF_CMD_STOP);
        emit(out, JF_CMD_EXIT);
        goto done;
    default: break;
    }

    /* The overlay is modal: only its own buttons (and BACK = CLOSE) act. */
    if (app->overlay) {
        if (ev->type == JF_EV_RETRY && (app->overlay_buttons & JF_BTN_RETRY)) {
            app->overlay = false;
            JfCmd *c = emit(out, JF_CMD_REQUEST);
            *c = app->retry;
            for (int k = 0; k < JF_LIST_COUNT; k++) app->lists[k].pending = false;
            if (c->list) c->list->pending = true;
        } else if (ev->type == JF_EV_CLOSE_ERROR || ev->type == JF_EV_BACK) {
            app->overlay = false;
        }
        goto done;
    }

    if (ev->type != JF_EV_NEED_MORE) app->banner[0] = '\0';

    switch (ev->type) {
    case JF_EV_SIGNIN:   on_signin(app, ev, out); break;
    case JF_EV_ACTIVATE: on_activate(app, ev->i, out); break;
    case JF_EV_BACK:     on_back(app, out); break;
    case JF_EV_PLAY_ALL: on_play_all(app, out); break;
    case JF_EV_NEXT:     on_next(app, out); break;
    case JF_EV_VOL_UP:   on_volume(app, +1, out); break;
    case JF_EV_VOL_DOWN: on_volume(app, -1, out); break;
    case JF_EV_SEARCH:   on_search(app, ev->term, out); break;
    case JF_EV_NEED_MORE: on_need_more(app, out); break;
    case JF_EV_CATEGORY: on_category(app, ev->i, out); break;
    case JF_EV_OPEN_NOWPLAYING:
        if (cur_list(app) >= 0 && app->queue_n > 0) { nav_push(app); app->screen = JF_SCR_NOWPLAYING; }
        break;
    case JF_EV_OPEN_QUEUE:
        if (app->screen == JF_SCR_NOWPLAYING && app->queue_n > 0) { nav_push(app); app->screen = JF_SCR_QUEUE; }
        break;
    case JF_EV_QUEUE_JUMP:
        if (app->screen == JF_SCR_QUEUE && ev->i >= 0 && ev->i < app->queue_n && have_token(app)) {
            pop(app, out);
            app->fail_streak = 0;
            play_track(app, ev->i, app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING, out);
        }
        break;
    default: break;
    }
done:
    refresh(app);
}
