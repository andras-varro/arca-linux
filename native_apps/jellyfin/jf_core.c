/*
 * jf_core.c — see jf_core.h.  One event in, up to JF_CMDS_MAX commands out.
 */

#include "jf_core.h"
#include "jf_url.h"

#include <stdio.h>
#include <string.h>

static const int PAGE[JF_LIST_COUNT] = { JF_PAGE_ARTISTS, JF_PAGE_ALBUMS, JF_PAGE_TRACKS, JF_PAGE_SEARCH, JF_MIX_LIMIT,
                                         1, JF_MIX_GENRE_PAGE, JF_MIX_PAGE };

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
    case JF_LIST_MIX:     return jf_path_instant_mix(out, cap, uid, id, JF_MIX_LIMIT);
    case JF_LIST_SEED:    return jf_path_item_by_id(out, cap, uid, id);
    case JF_LIST_GENRES:  return jf_path_genre_index(out, cap, uid, start, lim);
    case JF_LIST_POOL: {
        const JfMix *m = &app->mix;
        const char *ids[JF_MIX_VARIANTS];
        int n = 0;
        if (m->chosen >= 0) for (; n < m->gk[m->chosen].nids; n++) ids[n] = m->gk[m->chosen].ids[n];
        return jf_path_mix_pool(out, cap, uid, ids, n, m->use_artist ? m->artist_id : NULL, m->decade, m->random, start, lim);
    }
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
    app->action_bar = false;      /* the rows under it are gone */
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
    e->top = app->view_top;
    e->cursor = app->view_cursor;
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
    app->restore_top = app->view_top = e.top;
    app->restore_cursor = app->view_cursor = e.cursor;
    app->restore_seq++;
    int lid = list_of_screen(e.screen);
    /* The list is shared by every visit to its screen: if another visit
     * re-aimed it, aim it back rather than show the wrong rows. */
    if (lid >= 0 && (strcmp(app->ctx[lid].id, e.id) != 0 || app->ctx[lid].sub != e.sub))
        retarget(app, lid, e.id, e.title, e.sub, out);
}

void jf_note_view(JfApp *app, int top, int cursor)
{
    app->view_top = top;
    app->view_cursor = cursor;
}

static void go_signin(JfApp *app)
{
    app->screen = JF_SCR_SIGNIN;
    app->nav_n = 0;
    app->play_wait = false;
    app->action_bar = false;
    app->confirm = JF_CONFIRM_NONE;
}

static void refresh(JfApp *app)
{
    int lid = cur_list(app);
    app->list = lid >= 0 ? &app->lists[lid] : NULL;
    if (app->action_bar && (!app->list || app->action_row < 0 || app->action_row >= app->list->n)) app->action_bar = false;
    switch (app->screen) {
    case JF_SCR_SIGNIN:     copy(app->crumb, sizeof(app->crumb), "SIGN IN"); break;
    case JF_SCR_ARTISTS:    copy(app->crumb, sizeof(app->crumb), CAT_NAME[app->cat]); break;
    case JF_SCR_ALBUMS:
    case JF_SCR_TRACKS:     copy(app->crumb, sizeof(app->crumb), app->ctx[lid].title); break;
    case JF_SCR_SEARCH:     snprintf(app->crumb, sizeof(app->crumb), "SEARCH: %s", app->ctx[lid].id); break;
    case JF_SCR_NOWPLAYING: copy(app->crumb, sizeof(app->crumb), "NOW PLAYING"); break;
    case JF_SCR_QUEUE:      copy(app->crumb, sizeof(app->crumb), "QUEUE"); break;
    case JF_SCR_MIX:        snprintf(app->crumb, sizeof(app->crumb), "MIX: %.100s", app->mix.seed.name); break;
    }
    bool any = app->auth_pending || app->save_pending || app->play_wait || app->play_state == JF_PLAY_FETCHING;
    for (int k = 0; k < JF_LIST_COUNT; k++) any = any || app->lists[k].pending;
    app->busy = any;
}

/* ── playback ──────────────────────────────────────────────────────────── */

static void play_track_at(JfApp *app, int idx, int start_ms, bool stop_first, JfCmds *out);
static void play_track(JfApp *app, int idx, bool stop_first, JfCmds *out) { play_track_at(app, idx, 0, stop_first, out); }

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

static void play_track_at(JfApp *app, int idx, int start_ms, bool stop_first, JfCmds *out)
{
    if (stop_first) emit(out, JF_CMD_STOP);
    app->queue_cur = idx;
    app->start_ms = start_ms;
    app->pos_ms = 0;
    app->play_state = JF_PLAY_FETCHING;
    char sess[33];
    JfCmd *c = emit(out, JF_CMD_PLAY_STREAM);
    c->kind = JF_KIND_STREAM;
    c->tag = ++app->stream_tag;
    c->norm_mdb = app->queue[idx].norm_gain_mdb;
    c->user_mdb = jf_volume_mdb(app->volume_step);
    app->stream_channels = (app->sink_channels_fn && app->sink_channels_fn() == 2) ? 2 : 1;
    if (!app->session_fn || !app->session_fn(sess) ||
        jf_path_stream_at(c->path, sizeof(c->path), app->queue[idx].id, sess, start_ms,
                           app->stream_channels) < 0) {
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
    snprintf(app->queue_name, sizeof(app->queue_name), "Queue: %.80s", app->queue[0].name);
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

/* ── the action bar: PLAY NEXT, ADD TO QUEUE, MIX, INSTANT MIX ─────────── */

/* The track an action means: list row `i` of the screen's list (tracks only), or -1 = the
 * playing track.  Copied, so the caller may rewrite the queue it came from. */
static bool seed_item(const JfApp *app, int i, JfItem *out)
{
    if (i < 0) {
        const JfItem *now = jf_now(app);
        if (!now) return false;
        *out = *now;
        return true;
    }
    int lid = cur_list(app);
    if (lid < 0 || i >= app->lists[lid].n || app->lists[lid].items[i].type != JF_ITEM_TRACK) return false;
    *out = app->lists[lid].items[i];
    return true;
}

static bool nothing_plays(const JfApp *app)
{
    return app->play_state != JF_PLAY_PLAYING && app->play_state != JF_PLAY_FETCHING;
}

/* Start `n` items of `src` as a fresh queue (the current audio is stopped first if live). */
static void start_items(JfApp *app, const JfItem *src, int n, const char *name, JfCmds *out)
{
    int have = 0;
    for (int k = 0; k < n; k++) if (src[k].type == JF_ITEM_TRACK) have++;
    if (have == 0) return;               /* an empty answer leaves the queue as it is */
    bool live = !nothing_plays(app);
    app->queue_n = 0;
    for (int k = 0; k < n && app->queue_n < JF_QUEUE_MAX; k++)
        if (src[k].type == JF_ITEM_TRACK) app->queue[app->queue_n++] = src[k];
    copy(app->queue_name, sizeof(app->queue_name), name);
    app->fail_streak = 0;
    play_track(app, 0, live, out);
}

static void on_play_next(JfApp *app, int i, JfCmds *out)
{
    JfItem it;
    if (!seed_item(app, i, &it) || i < 0 || !have_token(app)) return;
    if (nothing_plays(app) || app->queue_n == 0) {
        char nm[96];
        snprintf(nm, sizeof nm, "Queue: %.80s", it.name);
        start_items(app, &it, 1, nm, out);
        return;
    }
    if (app->queue_n >= JF_QUEUE_MAX) { set_banner(app, "QUEUE FULL"); return; }
    int at = app->queue_cur + 1;           /* queue_cur stays: the playing track is not disturbed */
    memmove(&app->queue[at + 1], &app->queue[at], sizeof(app->queue[0]) * (size_t)(app->queue_n - at));
    app->queue[at] = it;
    app->queue_n++;
    set_banner(app, "QUEUED NEXT");
}

static void on_add_queue(JfApp *app, int i, JfCmds *out)
{
    JfItem it;
    if (!seed_item(app, i, &it) || i < 0 || !have_token(app)) return;
    if (nothing_plays(app) || app->queue_n == 0) {
        char nm[96];
        snprintf(nm, sizeof nm, "Queue: %.80s", it.name);
        start_items(app, &it, 1, nm, out);
        return;
    }
    if (app->queue_n >= JF_QUEUE_MAX) { set_banner(app, "QUEUE FULL"); return; }
    app->queue[app->queue_n++] = it;
    set_banner(app, "ADDED TO QUEUE");
}

/* Ask the server for a mix of `seed`.  The reply (on_mix_reply) replaces the queue. */
static void fetch_instant_mix(JfApp *app, const JfItem *seed, JfCmds *out)
{
    JfItem it = *seed;
    JfList *l = &app->lists[JF_LIST_MIX];
    app->ctx[JF_LIST_MIX].sub = 0;
    copy(app->ctx[JF_LIST_MIX].id, sizeof(app->ctx[JF_LIST_MIX].id), it.id);
    copy(app->ctx[JF_LIST_MIX].title, sizeof(app->ctx[JF_LIST_MIX].title), it.name);
    jf_list_clear(l);
    l->pending = false;
    l->generation++;
    app->play_wait = false;
    request_page(app, JF_LIST_MIX, 0, out);
}

/* With something in the queue the mix would replace it: ask first (app->confirm), the fetch waits for JF_EV_CONFIRM. */
static void on_instant_mix(JfApp *app, int i, JfCmds *out)
{
    JfItem it;
    if (!seed_item(app, i, &it) || !have_token(app)) return;
    if (app->queue_n > 0) {
        app->confirm = JF_CONFIRM_INSTANT_MIX;
        app->confirm_seed = it;
        return;
    }
    fetch_instant_mix(app, &it, out);
}

static void on_confirm(JfApp *app, JfCmds *out)
{
    if (app->confirm != JF_CONFIRM_INSTANT_MIX) return;
    app->confirm = JF_CONFIRM_NONE;
    if (have_token(app)) fetch_instant_mix(app, &app->confirm_seed, out);
}

static void on_mix_reply(JfApp *app, JfCmds *out)
{
    const JfList *l = &app->lists[JF_LIST_MIX];
    char nm[96];
    snprintf(nm, sizeof nm, "Mix: %.80s", app->ctx[JF_LIST_MIX].title);
    int tracks = 0;
    for (int k = 0; k < l->n; k++) if (l->items[k].type == JF_ITEM_TRACK) tracks++;
    if (tracks == 0) { set_banner(app, "NO MIX FOUND"); return; }
    start_items(app, l->items, l->n, nm, out);
}

/* ── MIX: seed fetch, tick screen, genre index, candidate pool ─────────── */

static unsigned mix_rand(JfMix *m)
{
    unsigned x = m->rng ? m->rng : 2463534242u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x;
    return x;
}

/* The shuffle's seed is the first 8 hex digits of a fresh session id, so a test that fixes session_fn fixes the order. */
static void mix_seed_rng(JfApp *app)
{
    char s[33];
    unsigned v = 0;
    if (app->session_fn && app->session_fn(s))
        for (int k = 0; k < 8 && s[k]; k++)
            v = v * 16u + (unsigned)(s[k] <= '9' ? s[k] - '0' : (s[k] | 32) - 'a' + 10);
    app->mix.rng = v;
}

/* Does the track's newline-separated `genres` hold a name whose key is `key`? */
static bool genres_have(const char *genres, const char *key)
{
    char line[JF_GENRES_BYTES], k[64];
    for (const char *p = genres; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l);
        line[l] = '\0';
        jf_genre_key(k, sizeof k, line);
        if (!strcmp(k, key)) return true;
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

/* Drop every request a MIX has in flight (the seed fetch, an index page, a pool page). */
static void mix_stop_requests(JfApp *app, JfCmds *out)
{
    bool any = false;
    for (int k = JF_LIST_SEED; k <= JF_LIST_POOL; k++) { any = any || app->lists[k].pending; app->lists[k].pending = false; }
    if (any) emit(out, JF_CMD_CANCEL)->kind = JF_KIND_LIST;
}

/* MIX on list row `i` (or -1 = the playing track): ask the server for the track's genres, year and artist; the
 * reply (on_seed_reply) opens the tick screen.  The list rows carry none of them. */
static void on_mix(JfApp *app, int i, JfCmds *out)
{
    JfItem it;
    if (!seed_item(app, i, &it) || !have_token(app)) return;
    JfMix *m = &app->mix;
    memset(m, 0, sizeof *m);
    m->seed = it;
    m->chosen = -1;
    m->phase = JF_MIX_SEED;
    JfList *l = &app->lists[JF_LIST_SEED];
    copy(app->ctx[JF_LIST_SEED].id, sizeof(app->ctx[JF_LIST_SEED].id), it.id);
    jf_list_clear(l);
    l->pending = false;
    l->generation++;
    app->play_wait = false;
    if (!request_page(app, JF_LIST_SEED, 0, out)) m->phase = JF_MIX_OFF;
}

static void mix_add_opt(JfMix *m, JfMixOptKind kind, const char *label, bool on)
{
    JfMixOpt *o = &m->opt[m->opt_n++];
    o->kind = kind;
    o->on = on;
    copy(o->label, sizeof o->label, label);
}

static void on_seed_reply(JfApp *app)
{
    JfMix *m = &app->mix;
    const JfList *l = &app->lists[JF_LIST_SEED];
    if (m->phase != JF_MIX_SEED) return;
    m->phase = JF_MIX_OFF;
    if (l->n < 1 || l->items[0].type != JF_ITEM_TRACK) { set_banner(app, "TRACK NOT FOUND"); return; }
    const JfItem *s = &l->items[0];
    m->year = s->year >= 1000 ? s->year : 0;
    copy(m->artist_id, sizeof m->artist_id, s->artist_id);
    m->opt_n = 0;
    char keys[JF_MIX_OPT_MAX][64];
    for (const char *p = s->genres; *p && m->opt_n < JF_MIX_OPT_MAX - 2; ) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char name[96];
        if (len >= sizeof name) len = sizeof name - 1;
        memcpy(name, p, len);
        name[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);
        if (!jf_genre_key(keys[m->opt_n], sizeof keys[0], name)) continue;
        bool dup = false;
        for (int k = 0; k < m->opt_n; k++) dup = dup || !strcmp(keys[k], keys[m->opt_n]);
        if (!dup) mix_add_opt(m, JF_MIXOPT_GENRE, name, true);
    }
    if (m->artist_id[0]) mix_add_opt(m, JF_MIXOPT_ARTIST, "SAME ARTIST", false);
    if (m->year) {
        char d[96];
        snprintf(d, sizeof d, "SAME DECADE (%dS)", m->year - m->year % 10);
        mix_add_opt(m, JF_MIXOPT_DECADE, d, false);
    }
    if (m->opt_n == 0) { set_banner(app, "NOTHING TO MIX ON"); return; }
    nav_push(app);
    app->screen = JF_SCR_MIX;
    m->phase = JF_MIX_TICKS;
}

static void on_mix_toggle(JfApp *app, int i)
{
    JfMix *m = &app->mix;
    if (app->screen != JF_SCR_MIX || m->phase != JF_MIX_TICKS || i < 0 || i >= m->opt_n) return;
    m->opt[i].on = !m->opt[i].on;
}

static void mix_cat(char *dst, size_t cap, const char *s)
{
    size_t l = strlen(dst);
    if (l + 1 < cap) snprintf(dst + l, cap - l, "%s", s);
}

/* "Similar: Rock+Pop+<artist> (1980s)": the ticked rows, cut to the field. */
static void mix_queue_name(const JfApp *app, char *out, size_t cap)
{
    const JfMix *m = &app->mix;
    snprintf(out, cap, "Similar: ");
    bool first = true;
    for (int k = 0; k < m->opt_n; k++) {
        const JfMixOpt *o = &m->opt[k];
        if (!o->on || o->kind == JF_MIXOPT_DECADE) continue;
        if (!first) mix_cat(out, cap, "+");
        mix_cat(out, cap, o->kind == JF_MIXOPT_ARTIST ? (m->seed.artist[0] ? m->seed.artist : "artist") : o->label);
        first = false;
    }
    if (m->decade) {
        char d[16];
        snprintf(d, sizeof d, " (%ds)", m->decade);
        mix_cat(out, cap, d);
    }
}

/* One pool page; random pages always start at 0, stable-order pages at what is consumed. */
static void mix_pool_request(JfApp *app, JfCmds *out)
{
    JfMix *m = &app->mix;
    JfList *l = &app->lists[JF_LIST_POOL];
    jf_list_clear(l);
    l->pending = false;
    l->generation++;
    if (!request_page(app, JF_LIST_POOL, m->random ? 0 : m->consumed, out)) { m->phase = JF_MIX_TICKS; return; }
    m->reqs++;
}

static void mix_begin_pool(JfApp *app, JfCmds *out)
{
    JfMix *m = &app->mix;
    m->phase = JF_MIX_POOL;
    m->found_n = 0;
    m->reqs = 0;
    m->consumed = 0;
    /* The index's count is an upper bound of the server total (variants may overlap): small = stable order. */
    m->random = !(m->chosen >= 0 && m->gk[m->chosen].count <= JF_MIX_SMALL);
    mix_pool_request(app, out);
}

static void on_mix_start(JfApp *app, JfCmds *out)
{
    JfMix *m = &app->mix;
    if (app->screen != JF_SCR_MIX || m->phase != JF_MIX_TICKS || !have_token(app)) return;
    int ticks = 0;
    for (int k = 0; k < m->opt_n; k++) if (m->opt[k].on) ticks++;
    if (ticks == 0) { set_banner(app, "TICK AT LEAST ONE"); return; }
    memset(m->gk, 0, sizeof m->gk);
    m->gk_n = 0;
    m->chosen = -1;
    m->use_artist = false;
    m->decade = 0;
    for (int k = 0; k < m->opt_n; k++) {
        if (!m->opt[k].on) continue;
        switch (m->opt[k].kind) {
        case JF_MIXOPT_GENRE:  jf_genre_key(m->gk[m->gk_n++].key, sizeof m->gk[0].key, m->opt[k].label); break;
        case JF_MIXOPT_ARTIST: m->use_artist = true; break;
        default:               m->decade = m->year - m->year % 10; break;
        }
    }
    m->found_n = 0;
    m->reqs = 0;
    m->consumed = 0;
    mix_seed_rng(app);
    if (m->gk_n == 0) { mix_begin_pool(app, out); return; }   /* artist and/or decade only: nothing to look up */
    m->phase = JF_MIX_INDEX;
    JfList *l = &app->lists[JF_LIST_GENRES];
    jf_list_clear(l);
    l->pending = false;
    l->generation++;
    if (!request_page(app, JF_LIST_GENRES, 0, out)) m->phase = JF_MIX_TICKS;
}

/* A page of the genre index: every genre whose key is a ticked key adds its id (and count) to that key. */
static void on_genres_reply(JfApp *app, JfCmds *out)
{
    JfMix *m = &app->mix;
    JfList *l = &app->lists[JF_LIST_GENRES];
    if (m->phase != JF_MIX_INDEX) return;
    for (int k = 0; k < l->n; k++) {
        const JfItem *it = &l->items[k];
        char key[64];
        if (it->type != JF_ITEM_GENRE || !jf_genre_key(key, sizeof key, it->name)) continue;
        for (int g = 0; g < m->gk_n; g++) {
            if (strcmp(m->gk[g].key, key)) continue;
            if (m->gk[g].nids < JF_MIX_VARIANTS) copy(m->gk[g].ids[m->gk[g].nids++], sizeof m->gk[g].ids[0], it->id);
            m->gk[g].count += it->count;
        }
    }
    m->consumed += l->loaded;
    if (l->loaded > 0 && m->consumed < l->total) {
        jf_list_clear(l);
        l->generation++;
        if (!request_page(app, JF_LIST_GENRES, m->consumed, out)) m->phase = JF_MIX_TICKS;
        return;
    }
    int best = -1;
    for (int g = 0; g < m->gk_n; g++) {
        if (m->gk[g].nids == 0) { best = -1; break; }      /* a ticked key the server has no genre for: nothing can match */
        if (best < 0 || m->gk[g].count < m->gk[best].count) best = g;
    }
    if (best < 0) { m->phase = JF_MIX_TICKS; set_banner(app, "NO MIX FOUND"); return; }
    m->chosen = best;
    mix_begin_pool(app, out);
}

/* The mix is the seed first, then the matches in a random order; the screen goes back to where MIX was raised. */
static void mix_finish(JfApp *app, JfCmds *out)
{
    JfMix *m = &app->mix;
    if (m->found_n == 0) { m->phase = JF_MIX_TICKS; set_banner(app, "NO MIX FOUND"); return; }
    for (int k = m->found_n; k > 1; k--) {                  /* found[1..found_n]; found[0] is the seed's */
        int j = 1 + (int)(mix_rand(m) % (unsigned)k);
        JfItem t = m->found[j];
        m->found[j] = m->found[k];
        m->found[k] = t;
    }
    m->found[0] = m->seed;
    char nm[96];
    mix_queue_name(app, nm, sizeof nm);
    m->phase = JF_MIX_OFF;
    if (app->screen == JF_SCR_MIX && app->nav_n > 0) pop(app, out);
    start_items(app, m->found, m->found_n + 1, nm, out);
}

/* A page of candidates: keep the tracks that carry every other ticked key (the server filtered the chosen one), once each. */
static void on_pool_reply(JfApp *app, JfCmds *out)
{
    JfMix *m = &app->mix;
    const JfList *l = &app->lists[JF_LIST_POOL];
    if (m->phase != JF_MIX_POOL) return;
    for (int k = 0; k < l->n && m->found_n < JF_MIX_WANT; k++) {
        const JfItem *it = &l->items[k];
        if (it->type != JF_ITEM_TRACK || !strcmp(it->id, m->seed.id)) continue;
        bool keep = true;
        for (int g = 0; g < m->gk_n && keep; g++) keep = g == m->chosen || genres_have(it->genres, m->gk[g].key);
        for (int f = 1; f <= m->found_n && keep; f++) keep = strcmp(m->found[f].id, it->id) != 0;
        if (keep) m->found[++m->found_n] = *it;
    }
    m->consumed += l->loaded;
    bool done = m->found_n >= JF_MIX_WANT || l->loaded == 0 || m->reqs >= JF_MIX_REQ_MAX;
    if (!done && m->random) {
        if (l->loaded < JF_MIX_PAGE || l->loaded >= l->total) done = true;   /* the whole answer was in this page */
        else if (l->total <= JF_MIX_SMALL) { m->random = false; m->consumed = 0; }   /* few: page in a stable order instead */
    } else if (!done && m->consumed >= l->total) {
        done = true;
    }
    if (done) mix_finish(app, out);
    else mix_pool_request(app, out);
}

/* SAVE AS PLAYLIST: the POST carries the first JF_SAVE_IDS_MAX queue ids (the loop reads them from
 * app->queue).  It is never put in app->retry: the overlay offers CLOSE only, so nothing re-POSTs. */
static void on_save(JfApp *app, const char *name, JfCmds *out)
{
    if (!name[0] || app->queue_n == 0 || app->save_pending || !have_token(app)) return;
    JfCmd *c = emit(out, JF_CMD_REQUEST);
    c->kind = JF_KIND_SAVE;
    c->tag = ++app->save_tag;
    copy(c->name, sizeof(c->name), name);
    c->n = app->queue_n < JF_SAVE_IDS_MAX ? app->queue_n : JF_SAVE_IDS_MAX;
    copy(app->save_name, sizeof(app->save_name), name);
    app->save_pending = true;
}

/* ── session loss and error text ───────────────────────────────────────── */

static void session_expired(JfApp *app, JfCmds *out)
{
    app->save_pending = false;
    app->mix.phase = JF_MIX_OFF;
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
        bool mixl = lid == JF_LIST_SEED || lid == JF_LIST_GENRES || lid == JF_LIST_POOL;
        if (mixl) app->mix.phase = app->mix.phase == JF_MIX_SEED ? JF_MIX_OFF : JF_MIX_TICKS;   /* CLOSE only: a retry would not know the phase */
        bool again = !mixl && (e == JF_ERR_UNREACHABLE || e == JF_ERR_SERVER);
        app->play_wait = false;
        char t[48];
        set_overlay(app, err_text(t, sizeof t, e, r->status), again ? (JF_BTN_RETRY | JF_BTN_CLOSE) : JF_BTN_CLOSE);
        return;
    }
    /* A page that consumed nothing while the server claims more would be asked for forever. */
    if (l->loaded <= app->req_loaded[lid]) l->total = l->loaded;
    if (lid == JF_LIST_MIX) { on_mix_reply(app, out); return; }
    if (lid == JF_LIST_SEED) { on_seed_reply(app); return; }
    if (lid == JF_LIST_GENRES) { on_genres_reply(app, out); return; }
    if (lid == JF_LIST_POOL) { on_pool_reply(app, out); return; }
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
    c->channels = app->stream_channels;
    c->tag = r->tag;
    c->norm_mdb = app->queue[app->queue_cur].norm_gain_mdb;
    c->user_mdb = jf_volume_mdb(app->volume_step);
}

/* The POST's answer: its tag must be the one in flight.  A failure is CLOSE only (no RETRY: it would POST again). */
static void on_save_result(JfApp *app, const JfResult *r, JfCmds *out)
{
    if (!app->save_pending || r->tag != app->save_tag) return;
    app->save_pending = false;
    if (r->err == JF_ERR_UNAUTH) { session_expired(app, out); return; }
    if (r->err != JF_ERR_NONE) {
        char t[48];
        set_overlay(app, err_text(t, sizeof t, r->err, r->status), JF_BTN_CLOSE);
        return;
    }
    char b[48];
    snprintf(b, sizeof b, "SAVED: %.40s", app->save_name);
    set_banner(app, b);
}

static void on_result(JfApp *app, const JfResult *r, JfCmds *out)
{
    switch (r->kind) {
    case JF_KIND_AUTH:   on_auth(app, r, out); break;
    case JF_KIND_LIST:   on_list(app, r, out); break;
    case JF_KIND_STREAM: on_stream(app, r, out); break;
    case JF_KIND_SAVE:   on_save_result(app, r, out); break;
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

/* A tap / ENTER on a list row.  A track row raises the action bar on it (or drops it when it is the
 * same row), anything else opens as ACTIVATE does and the bar goes away. */
static void on_select(JfApp *app, int i, JfCmds *out)
{
    int lid = cur_list(app);
    if (lid < 0 || i < 0 || i >= app->lists[lid].n) return;
    if (app->lists[lid].items[i].type != JF_ITEM_TRACK) { app->action_bar = false; on_activate(app, i, out); return; }
    if (app->action_bar && app->action_row == i) app->action_bar = false;
    else { app->action_bar = true; app->action_row = i; }
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

/* PREV: the usual player convention.  Past JF_PREV_RESTART_MS into the track it starts over; sooner, the previous entry
 * plays; the first entry always starts over. */
static void on_prev(JfApp *app, int played_ms, JfCmds *out)
{
    if (app->queue_n == 0 || !have_token(app)) return;
    bool live = app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING;
    app->fail_streak = 0;
    int idx = app->queue_cur;
    if (played_ms <= JF_PREV_RESTART_MS && idx > 0) idx--;
    play_track(app, idx, live, out);
}

uint32_t jf_rand_below(JfRng rng, void *ctx, uint32_t n)
{
    if (n <= 1) return 0;
    return (uint32_t)(((uint64_t)rng(ctx) * n) >> 32);
}

static uint32_t xorshift_draw(void *ctx)
{
    uint32_t *s = ctx, x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

void jf_seed(JfApp *app, uint32_t seed) { app->rng_state = seed ? seed : 0x9E3779B9u; }

static void queue_swap(JfApp *app, int a, int b)
{
    JfItem t = app->queue[a];
    app->queue[a] = app->queue[b];
    app->queue[b] = t;
}

void jf_queue_shuffle(JfApp *app, JfRng rng, void *ctx)
{
    int n = app->queue_n, first = 0;
    if (n < 2) return;
    bool playing = app->play_state == JF_PLAY_PLAYING || app->play_state == JF_PLAY_FETCHING ||
                   app->play_state == JF_PLAY_PAUSED;
    if (playing) {
        queue_swap(app, 0, app->queue_cur);     /* a swap, then everything behind position 0 is shuffled anyway */
        app->queue_cur = 0;
        first = 1;
    }
    /* queue_cur follows its entry through the swaps */
    int cur_slot = app->queue_cur;
    for (int i = n - 1; i > first; i--) {
        int j = first + (int)jf_rand_below(rng, ctx, (uint32_t)(i - first + 1));
        queue_swap(app, i, j);
        if (cur_slot == i) cur_slot = j;
        else if (cur_slot == j) cur_slot = i;
    }
    app->queue_cur = cur_slot;
}

bool jf_queue_move(JfApp *app, int from, int to)
{
    int n = app->queue_n;
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    JfItem t = app->queue[from];
    if (from < to) memmove(&app->queue[from], &app->queue[from + 1], sizeof(app->queue[0]) * (size_t)(to - from));
    else           memmove(&app->queue[to + 1], &app->queue[to], sizeof(app->queue[0]) * (size_t)(from - to));
    app->queue[to] = t;
    int c = app->queue_cur;
    if (c == from) c = to;
    else if (from < to && c > from && c <= to) c--;
    else if (from > to && c >= to && c < from) c++;
    app->queue_cur = c;
    return true;
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

static int runtime_ms(const JfApp *app)
{
    return app->queue_n > 0 ? app->queue[app->queue_cur].runtime_s * 1000 : 0;
}

int jf_position_ms(const JfApp *app, int elapsed_ms)
{
    int p = 0;
    if (app->play_state == JF_PLAY_PAUSED) p = app->pos_ms;
    else if (app->play_state == JF_PLAY_PLAYING) p = app->start_ms + (elapsed_ms > 0 ? elapsed_ms : 0);
    else if (app->play_state == JF_PLAY_FETCHING) p = app->start_ms;
    int rt = runtime_ms(app);
    if (rt > 0 && p > rt) p = rt;
    return p < 0 ? 0 : p;
}

/* Pause is a stop that remembers where: the fd stream cannot be held (audio_music_start_fd is "not pausable"), and resuming
 * re-requests the track at pos_ms with a fresh session id.  (A held socket would survive 120 s, measured, but nothing holds the voice.) */
static void on_pause(JfApp *app, int pos_ms, JfCmds *out)
{
    if (app->queue_n == 0 || !have_token(app)) return;
    switch (app->play_state) {
    case JF_PLAY_PLAYING:
    case JF_PLAY_FETCHING:
        if (app->play_state == JF_PLAY_FETCHING) {
            emit(out, JF_CMD_CANCEL)->kind = JF_KIND_STREAM;
            pos_ms = app->start_ms;
        }
        app->pos_ms = pos_ms < 0 ? 0 : pos_ms;
        app->play_state = JF_PLAY_PAUSED;
        emit(out, JF_CMD_STOP);
        break;
    case JF_PLAY_PAUSED:
        app->fail_streak = 0;
        play_track_at(app, app->queue_cur, app->pos_ms, false, out);
        break;
    case JF_PLAY_STOPPED:
    case JF_PLAY_END:
        app->fail_streak = 0;
        play_track(app, app->queue_cur, false, out);
        break;
    default: break;
    }
}

static void on_stop(JfApp *app, JfCmds *out)
{
    if (app->play_state != JF_PLAY_PLAYING && app->play_state != JF_PLAY_FETCHING && app->play_state != JF_PLAY_PAUSED) return;
    if (app->play_state == JF_PLAY_FETCHING) emit(out, JF_CMD_CANCEL)->kind = JF_KIND_STREAM;
    app->play_state = JF_PLAY_STOPPED;
    app->start_ms = app->pos_ms = 0;
    emit(out, JF_CMD_STOP);
}

static void on_seek(JfApp *app, int ms, JfCmds *out)
{
    int rt = runtime_ms(app);
    if (app->queue_n == 0 || rt <= 0 || !have_token(app)) return;
    if (ms < 0) ms = 0;
    if (ms > rt - 1) ms = rt - 1;
    if (app->play_state == JF_PLAY_PAUSED) { app->pos_ms = ms; return; }
    if (app->play_state != JF_PLAY_PLAYING && app->play_state != JF_PLAY_FETCHING) return;
    if (app->play_state == JF_PLAY_FETCHING) emit(out, JF_CMD_CANCEL)->kind = JF_KIND_STREAM;
    play_track_at(app, app->queue_cur, ms, true, out);
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
    if (app->mix.phase == JF_MIX_INDEX || app->mix.phase == JF_MIX_POOL) {   /* BACK cancels the run, a second one leaves */
        app->mix.phase = JF_MIX_TICKS;
        mix_stop_requests(app, out);
        return;
    }
    if (app->screen == JF_SCR_MIX) app->mix.phase = JF_MIX_OFF;
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
    if (lid < 0 || !have_token(app) || app->mix.phase == JF_MIX_SEED) return;   /* a page request would supersede the seed fetch */
    const JfList *l = &app->lists[lid];
    if (l->loaded >= l->total || l->pending || l->full) return;
    request_page(app, lid, l->loaded, out);
}

static void on_track_ended(JfApp *app, int played_ms, JfCmds *out)
{
    if (app->play_state != JF_PLAY_PLAYING) return;
    if (!have_token(app)) { app->play_state = JF_PLAY_IDLE; return; }   /* expired: fetch nothing */
    int left = runtime_ms(app) - app->start_ms;      /* a seek near the end plays little and is no failure */
    if (played_ms < JF_SHORT_MS && (left <= 0 || left >= JF_SHORT_MS)) { track_failed(app, out); return; }
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
    jf_list_init(&app->lists[JF_LIST_MIX], app->store_mix, JF_CAP_MIX);
    jf_list_init(&app->lists[JF_LIST_SEED], app->store_seed, JF_CAP_SEED);
    jf_list_init(&app->lists[JF_LIST_GENRES], app->store_genres, JF_CAP_GENRES);
    jf_list_init(&app->lists[JF_LIST_POOL], app->store_pool, JF_CAP_POOL);
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
    /* A seed fetch is not a screen yet: any intent but the passive ones abandons it, and BACK is then spent. */
    if (app->mix.phase == JF_MIX_SEED && ev->type != JF_EV_NEED_MORE && ev->type != JF_EV_VOL_UP && ev->type != JF_EV_VOL_DOWN) {
        app->mix.phase = JF_MIX_OFF;
        mix_stop_requests(app, out);
        if (ev->type == JF_EV_BACK) goto done;
    }
    /* The action bar lives for one gesture: any user intent other than the ones that keep it (more
     * rows arriving, volume, picking a row) drops it, and a BACK that found it up only drops it. */
    /* A pending question lives for one gesture too: CONFIRM acts on it, BACK / DISMISS drop it (BACK is then spent), any other
     * intent but the passive ones drops it and acts as usual. */
    bool confirm_was = app->confirm != JF_CONFIRM_NONE;
    if (confirm_was && ev->type != JF_EV_CONFIRM && ev->type != JF_EV_NEED_MORE && ev->type != JF_EV_VOL_UP && ev->type != JF_EV_VOL_DOWN)
        app->confirm = JF_CONFIRM_NONE;
    bool bar_was = app->action_bar;
    if (ev->type != JF_EV_NEED_MORE && ev->type != JF_EV_SELECT && ev->type != JF_EV_VOL_UP && ev->type != JF_EV_VOL_DOWN)
        app->action_bar = false;

    switch (ev->type) {
    case JF_EV_SIGNIN:   on_signin(app, ev, out); break;
    case JF_EV_ACTIVATE: on_activate(app, ev->i, out); break;
    case JF_EV_SELECT:   on_select(app, ev->i, out); break;
    case JF_EV_PLAY_NEXT: on_play_next(app, ev->i, out); break;
    case JF_EV_ADD_QUEUE: on_add_queue(app, ev->i, out); break;
    case JF_EV_MIX:      on_mix(app, ev->i, out); break;
    case JF_EV_MIX_TOGGLE: on_mix_toggle(app, ev->i); break;
    case JF_EV_MIX_START: on_mix_start(app, out); break;
    case JF_EV_INSTANT_MIX: on_instant_mix(app, ev->i, out); break;
    case JF_EV_SAVE_PLAYLIST: on_save(app, ev->term, out); break;
    case JF_EV_CONFIRM:  on_confirm(app, out); break;
    case JF_EV_DISMISS:  break;
    case JF_EV_BACK:     if (!bar_was && !confirm_was) on_back(app, out); break;
    case JF_EV_PLAY_ALL: on_play_all(app, out); break;
    case JF_EV_NEXT:     on_next(app, out); break;
    case JF_EV_PREV:     on_prev(app, ev->played_ms, out); break;
    case JF_EV_SHUFFLE:
        if (app->screen == JF_SCR_QUEUE) { if (!app->rng_state) jf_seed(app, 0); jf_queue_shuffle(app, xorshift_draw, &app->rng_state); }
        break;
    case JF_EV_QUEUE_MOVE:
        if (app->screen == JF_SCR_QUEUE) jf_queue_move(app, ev->i, ev->j);
        break;
    case JF_EV_PAUSE:    on_pause(app, ev->played_ms, out); break;
    case JF_EV_STOP:     on_stop(app, out); break;
    case JF_EV_SEEK:     on_seek(app, ev->i, out); break;
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
