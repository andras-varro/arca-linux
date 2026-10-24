/* jv_core.c — see jv_core.h.  No socket, no clock, no framebuffer: commands out, events in. */
#include <stdio.h>
#include <string.h>
#include "jv_core.h"

static void copy(char *d, size_t cap, const char *s) { snprintf(d, cap, "%s", s ? s : ""); }

static JvCmd *emit(JvCmds *out, JvCmdType t)
{
    if (out->n >= JV_CMDS_MAX) { out->overflow = true; static JvCmd junk; memset(&junk, 0, sizeof junk); return &junk; }
    JvCmd *c = &out->c[out->n++];
    memset(c, 0, sizeof *c);
    c->type = t;
    c->fd = -1;
    return c;
}

static void banner(JvCore *c, const char *t) { copy(c->banner, sizeof c->banner, t); }

static void set_overlay(JvCore *c, const char *t, unsigned buttons)
{
    c->overlay = true; copy(c->overlay_text, sizeof c->overlay_text, t); c->overlay_buttons = buttons;
}

static void err_text(char *o, size_t cap, JfErr e)
{
    switch (e) {
    case JF_ERR_UNREACHABLE: copy(o, cap, "SERVER UNREACHABLE"); break;
    case JF_ERR_FORBIDDEN:   copy(o, cap, "NOT ALLOWED"); break;
    case JF_ERR_NOTFOUND:    copy(o, cap, "NOT FOUND"); break;
    case JF_ERR_SERVER:      copy(o, cap, "SERVER ERROR"); break;
    default:                 copy(o, cap, "BAD REPLY"); break;
    }
}

static bool new_session(JvCore *c, char out[33])
{
    return (c->session_fn ? c->session_fn : jf_session_id)(out);
}

static void update_busy(JvCore *c)
{
    c->busy = c->auth_pending || c->lists[0].pending || c->detail_tag != 0 || c->home_phase != 0;
}

/* ── init ──────────────────────────────────────────────────────────────── */

void jv_core_init(JvCore *c, const char *conf_path, const char *reuse_path, const char *id_suffix)
{
    memset(c, 0, sizeof *c);
    copy(c->conf_path, sizeof c->conf_path, conf_path);
    copy(c->reuse_path, sizeof c->reuse_path, reuse_path);
    copy(c->id_suffix, sizeof c->id_suffix, id_suffix);
    jv_list_init(&c->lists[0], c->store_main, JV_CAP_LIST);
    jv_list_init(&c->lists[1], c->store_resume, JV_CAP_PROBE);
    jv_list_init(&c->lists[2], c->store_next, JV_CAP_PROBE);
    jv_list_init(&c->libs, c->store_libs, JV_CAP_LIBS);
    jv_settings_defaults(&c->set);
    c->channels = 1; c->rate = 44100; c->max_w = JV_STREAM_MAX_W;
    c->sel_audio = c->sel_sub = -1;
    c->search_sec[0] = c->search_sec[1] = -1;
    c->screen = JV_SCR_SIGNIN;
}

long jv_core_position(const JvCore *c) { return c->seek_pending ? c->seek_target_ms : c->pos_ms; }
bool jv_core_playing(const JvCore *c) { return c->screen == JV_SCR_PLAYER; }
void jv_note_view(JvCore *c, int top, int cursor) { c->view_top = top; c->view_cursor = cursor; }

/* ── requests ──────────────────────────────────────────────────────────── */

static unsigned req_parse(JvCore *c, JvCmds *out, const char *path, JfParseFn fn, void *dst, int group)
{
    JvCmd *r = emit(out, JV_CMD_REQUEST);
    r->kind = JF_KIND_PARSE;
    r->tag = ++c->req_tag;
    r->fn = fn; r->dst = dst; r->group = group;
    copy(r->path, sizeof r->path, path);
    return r->tag;
}

static void set_server(JvCore *c, JvCmds *out, const char *host, int port, bool tls)
{
    JvCmd *s = emit(out, JV_CMD_SET_SERVER);
    copy(s->host, sizeof s->host, host); s->port = port; s->tls = tls;
    (void)c;
    emit(out, JV_CMD_SET_AUTH);
}

/* the home chain: libraries, then the CONTINUE and NEXT UP probes (sequential, one group) */
static void home_step(JvCore *c, JvCmds *out)
{
    char p[JF_API_PATH_MAX];
    int n = -1;
    JfParseFn fn = jv_parse_items;
    void *dst = NULL;
    if (c->home_phase == 1) { n = jf_path_views(p, sizeof p, c->login.user_id); jv_list_clear(&c->libs); dst = &c->libs; }
    else if (c->home_phase == 2) { n = jv_path_resume(p, sizeof p, c->login.user_id, JV_PAGE_SMALL); jv_list_clear(&c->lists[1]); dst = &c->lists[1]; }
    else if (c->home_phase == 3) { n = jv_path_next_up(p, sizeof p, c->login.user_id, JV_PAGE_SMALL); jv_list_clear(&c->lists[2]); dst = &c->lists[2]; }
    if (n < 0 || !dst) { c->home_phase = 0; c->home_tag = 0; c->home_ready = true; update_busy(c); return; }
    c->home_tag = req_parse(c, out, p, fn, dst, JV_GRP_HOME);
}

static void go_home(JvCore *c, JvCmds *out)
{
    c->screen = JV_SCR_HOME; c->kind = JV_K_NONE; c->list = NULL; c->nav_n = 0;
    c->crumb[0] = '\0';
    c->home_phase = 1;
    home_step(c, out);
    update_busy(c);
}

static void go_signin(JvCore *c)
{
    c->screen = JV_SCR_SIGNIN; c->list = NULL; c->nav_n = 0; c->focus_password = false;
    copy(c->signin_server, sizeof c->signin_server, c->login.server);
    copy(c->signin_user, sizeof c->signin_user, c->login.user);
}

static void session_lost(JvCore *c)
{
    jf_login_clear(&c->login, c->conf_path);
    go_signin(c);
    banner(c, "SESSION EXPIRED");
}

/* the first page (or the next one) of the list the screen shows */
static void req_list(JvCore *c, JvCmds *out, bool first)
{
    JvList *l = &c->lists[0];
    const char *u = c->login.user_id;
    char p[JF_API_PATH_MAX];
    int n = -1, start = first ? 0 : l->loaded;
    if (first) { jv_list_clear(l); c->search_sec[0] = c->search_sec[1] = -1; }
    switch (c->kind) {
    case JV_K_RESUME:  n = jv_path_resume(p, sizeof p, u, JV_PAGE_SMALL); break;
    case JV_K_NEXTUP:  n = jv_path_next_up(p, sizeof p, u, JV_PAGE_SMALL); break;
    case JV_K_MOVIES:  n = jv_path_movies(p, sizeof p, u, c->set.library_movies, start, JV_PAGE); break;
    case JV_K_SHOWS:   n = jv_path_shows(p, sizeof p, u, c->set.library_shows, start, JV_PAGE); break;
    case JV_K_SEASONS: n = jv_path_seasons(p, sizeof p, u, c->cur_id); break;
    case JV_K_EPISODES:n = jv_path_episodes(p, sizeof p, u, c->cur_id, c->cur_id2); break;
    case JV_K_PERSON:  n = jv_path_by_person(p, sizeof p, u, c->cur_id, start, JV_PAGE); break;
    case JV_K_GENRE:   n = jv_path_by_genre(p, sizeof p, u, c->cur_id, start, JV_PAGE); break;
    case JV_K_SIMILAR: n = jv_path_similar(p, sizeof p, u, c->cur_id, JV_PAGE_SIMILAR); break;
    case JV_K_SEARCH:
        if (c->search_phase < 1 || c->search_phase > 3) c->search_phase = 1;
        n = c->search_phase == 1 ? jv_path_search_items(p, sizeof p, u, c->search_term, JV_PAGE_SEARCH)
          : c->search_phase == 2 ? jv_path_search_people(p, sizeof p, c->search_term, JV_PAGE_PEOPLE)
                                 : jv_path_search_genres(p, sizeof p, c->search_term, JV_PAGE_PEOPLE);
        break;
    default: break;
    }
    if (n < 0) { banner(c, "BAD REQUEST"); l->pending = false; return; }
    c->list_tag = req_parse(c, out, p, jv_parse_items, l, JV_GRP_LIST);
    l->pending = true;
    c->retry = out->c[out->n - 1];
    c->retry.type = JV_CMD_REQUEST;
    update_busy(c);
}

static void req_detail(JvCore *c, JvCmds *out, const char *id)
{
    char p[JF_API_PATH_MAX];
    if (jv_path_detail(p, sizeof p, c->login.user_id, id) < 0) { banner(c, "BAD REQUEST"); return; }
    c->detail_tag = req_parse(c, out, p, jv_parse_detail, &c->detail_in, JV_GRP_DETAIL);
    c->retry = out->c[out->n - 1];
    update_busy(c);
}

/* ── navigation ────────────────────────────────────────────────────────── */

static JvScreen screen_of(JvKind k) { return k == JV_K_SEASONS ? JV_SCR_SERIES : k == JV_K_EPISODES ? JV_SCR_SEASON : JV_SCR_LIST; }

static void push_here(JvCore *c, const char *id)
{
    if (c->nav_n >= JV_NAV_MAX) { memmove(c->nav, c->nav + 1, sizeof(c->nav[0]) * (JV_NAV_MAX - 1)); c->nav_n = JV_NAV_MAX - 1; }
    JvNavEntry *e = &c->nav[c->nav_n++];
    memset(e, 0, sizeof *e);
    e->screen = c->screen == JV_SCR_PLAYER || c->screen == JV_SCR_CAST ? JV_SCR_DETAILS : c->screen;
    e->kind = c->kind;
    copy(e->id, sizeof e->id, id ? id : c->cur_id);
    copy(e->id2, sizeof e->id2, c->cur_id2);
    copy(e->title, sizeof e->title, c->crumb);
    e->top = c->view_top; e->cursor = c->view_cursor;
}

/* open a list screen; `push` false replaces the current one */
static void enter_list(JvCore *c, JvCmds *out, JvKind k, const char *id, const char *id2, const char *title, bool push)
{
    if (push) push_here(c, c->screen == JV_SCR_DETAILS ? c->detail.it.b.id : NULL);
    c->kind = k;
    c->screen = screen_of(k);
    copy(c->cur_id, sizeof c->cur_id, id); copy(c->cur_id2, sizeof c->cur_id2, id2);
    copy(c->crumb, sizeof c->crumb, title);
    c->list = &c->lists[0];
    c->overlay = false; c->banner[0] = '\0';
    c->search_phase = k == JV_K_SEARCH ? 1 : 0;
    req_list(c, out, true);
}

static void enter_details(JvCore *c, JvCmds *out, const char *id, const char *title)
{
    push_here(c, NULL);
    c->screen = JV_SCR_DETAILS;
    c->list = NULL;
    c->detail_ready = false;
    c->detail_refresh = false;
    copy(c->cur_id, sizeof c->cur_id, id);
    copy(c->crumb, sizeof c->crumb, title);
    c->overlay = false; c->banner[0] = '\0';
    req_detail(c, out, id);
}

static void pop(JvCore *c, JvCmds *out)
{
    if (c->nav_n == 0) return;
    JvScreen from = c->screen;
    JvNavEntry e = c->nav[--c->nav_n];
    c->overlay = false; c->banner[0] = '\0';
    c->restore_top = e.top; c->restore_cursor = e.cursor; c->restore_seq++;
    if (e.screen == JV_SCR_HOME) { go_home(c, out); return; }
    c->screen = e.screen;
    c->kind = e.kind;
    copy(c->crumb, sizeof c->crumb, e.title);
    copy(c->cur_id, sizeof c->cur_id, e.id);
    copy(c->cur_id2, sizeof c->cur_id2, e.id2);
    c->detail_tag = 0;
    if (e.screen == JV_SCR_DETAILS) {
        c->list = NULL;
        if (from == JV_SCR_CAST) { update_busy(c); return; }      /* the detail is still the one on screen */
        c->detail_refresh = from == JV_SCR_PLAYER && c->detail_ready && strcmp(c->detail.it.b.id, e.id) == 0;
        if (!c->detail_refresh) c->detail_ready = false;
        req_detail(c, out, e.id);
    } else {
        c->list = &c->lists[0];
        c->search_phase = c->kind == JV_K_SEARCH ? 1 : 0;
        req_list(c, out, true);
    }
}

/* ── playback ──────────────────────────────────────────────────────────── */

static int audio_index(const JvCore *c) { return c->sel_audio >= 0 && c->sel_audio < c->detail.n_audio ? c->detail.audio[c->sel_audio].index : -1; }
static int sub_index(const JvCore *c) { return c->sel_sub >= 0 && c->sel_sub < c->detail.n_subs ? c->detail.subs[c->sel_sub].index : -1; }

static void post_report(JvCore *c, JvCmds *out, int which, long pos, const char *event)
{
    JvReport r;
    memset(&r, 0, sizeof r);
    r.item_id = c->play.item_id; r.media_source_id = c->play.media_source_id; r.session_id = c->play.session_id;
    r.position_ms = pos < 0 ? 0 : pos;
    r.audio_index = c->play.audio_index; r.sub_index = c->play.sub_index;
    r.paused = c->play_state == JV_PLAY_PAUSED;
    r.event = event;
    char path[JF_API_PATH_MAX], body[JV_BODY_MAX];
    int pn = which == 0 ? jv_path_playing(path, sizeof path) : which == 1 ? jv_path_progress(path, sizeof path) : jv_path_stopped(path, sizeof path);
    int bn = which == 0 ? jv_body_started(body, sizeof body, &r) : which == 1 ? jv_body_progress(body, sizeof body, &r) : jv_body_stopped(body, sizeof body, &r);
    if (pn < 0 || bn < 0) return;
    JvCmd *q = emit(out, JV_CMD_REQUEST);
    q->kind = JF_KIND_POST; q->tag = ++c->req_tag; q->n = bn;
    copy(q->path, sizeof q->path, path); memcpy(q->body, body, (size_t)bn + 1);
    c->next_report_ms = c->now_ms + JV_PROGRESS_MS;
}

static void end_job(JvCore *c, JvCmds *out, const char *sid)
{
    char p[JF_API_PATH_MAX];
    if (jv_path_end_job(p, sizeof p, c->login.device_id, sid) < 0) return;
    JvCmd *w = emit(out, JV_CMD_REQUEST);
    w->kind = JF_KIND_WRITE; w->tag = ++c->req_tag;
    copy(w->method, sizeof w->method, "DELETE"); copy(w->path, sizeof w->path, p);
}

static void request_stream(JvCore *c, JvCmds *out)
{
    char p[JF_API_PATH_MAX];
    if (jv_path_stream(p, sizeof p, c->play.item_id, c->play.media_source_id, c->play.channels, c->play.rate, c->play.max_w,
                       c->play.audio_index, c->play.start_ms, c->play.session_id, c->login.device_id) < 0) {
        c->play_state = JV_PLAY_IDLE; banner(c, "BAD REQUEST"); return;
    }
    JvCmd *s = emit(out, JV_CMD_REQUEST);
    s->kind = JF_KIND_STREAM; s->tag = c->stream_tag = ++c->req_tag;
    copy(s->path, sizeof s->path, p);
    c->play_state = JV_PLAY_FETCHING;
    c->pos_ms = c->play.start_ms;
}

/* a new stream at start_ms: the old decoder and encoding job end, the new session id is fresh */
static void restart_stream(JvCore *c, JvCmds *out, long start_ms, const char *event)
{
    long rt = c->play.runtime_ms;
    if (start_ms < 0) start_ms = 0;
    if (rt > 0 && start_ms > rt - 1000) start_ms = rt > 1000 ? rt - 1000 : 0;
    char old[33];
    copy(old, sizeof old, c->play.session_id);
    if (c->play_state == JV_PLAY_PLAYING || c->play_state == JV_PLAY_PAUSED) emit(out, JV_CMD_PLAYER_STOP);
    else if (c->play_state == JV_PLAY_FETCHING) { JvCmd *k = emit(out, JV_CMD_CANCEL); k->kind = JF_KIND_STREAM; }
    end_job(c, out, old);
    if (!new_session(c, c->play.session_id)) { c->play_state = JV_PLAY_IDLE; banner(c, "NO SESSION ID"); return; }
    c->play.start_ms = start_ms;
    c->play.generation++;
    c->seek_pending = false;
    c->play_state = JV_PLAY_FETCHING;   /* the report below says "not paused" */
    post_report(c, out, 1, start_ms, event);
    request_stream(c, out);
}

static void load_sub(JvCore *c, JvCmds *out)
{
    emit(out, JV_CMD_SUB_CLEAR);
    if (c->play.sub_index < 0) return;
    char p[JF_API_PATH_MAX];
    if (jv_path_subtitle(p, sizeof p, c->play.item_id, c->play.media_source_id, c->play.sub_index) < 0) return;
    JvCmd *s = emit(out, JV_CMD_REQUEST);
    s->kind = JF_KIND_STREAM; s->api = 1; s->tag = c->sub_tag = ++c->req_tag;
    copy(s->path, sizeof s->path, p);
}

static void on_play(JvCore *c, JvCmds *out, long start_ms)
{
    if (c->screen != JV_SCR_DETAILS || !c->detail_ready || !c->detail.msid[0] || c->play_state != JV_PLAY_IDLE) return;
    memset(&c->play, 0, sizeof c->play);
    copy(c->play.item_id, sizeof c->play.item_id, c->detail.it.b.id);
    copy(c->play.media_source_id, sizeof c->play.media_source_id, c->detail.msid);
    if (!new_session(c, c->play.session_id)) { banner(c, "NO SESSION ID"); return; }
    c->play.audio_index = audio_index(c); c->play.sub_index = sub_index(c);
    c->play.runtime_ms = (long)c->detail.it.b.runtime_s * 1000L;
    c->play.channels = c->channels; c->play.rate = c->rate; c->play.max_w = c->max_w;
    c->play.start_ms = start_ms < 0 ? 0 : start_ms;
    c->play.generation = 1;
    c->started_sent = false; c->restarted_once = false; c->seek_pending = false;
    push_here(c, c->detail.it.b.id);
    c->screen = JV_SCR_PLAYER;
    request_stream(c, out);
}

static void on_stream(JvCore *c, const JfResult *r, JvCmds *out)
{
    if (c->play_state != JV_PLAY_FETCHING) { if (r->fd >= 0) { JvCmd *k = emit(out, JV_CMD_CLOSE_FD); k->fd = r->fd; } return; }
    if (r->err != JF_ERR_NONE || r->fd < 0) {
        char t[48]; err_text(t, sizeof t, r->err);
        if (c->started_sent) post_report(c, out, 2, c->play.start_ms, NULL);
        c->play_state = JV_PLAY_IDLE; c->seek_pending = false;
        pop(c, out);
        set_overlay(c, t, JV_BTN_CLOSE);
        return;
    }
    c->play_state = JV_PLAY_PLAYING;
    JvCmd *s = emit(out, JV_CMD_PLAYER_START);
    s->fd = r->fd;
    c->next_report_ms = c->now_ms + JV_PROGRESS_MS;
    if (!c->started_sent) {
        c->started_sent = true;
        post_report(c, out, 0, c->play.start_ms, NULL);
        load_sub(c, out);
    }
}

/* stop: reports and the encoding job end; the screen change is the caller's */
static void stop_play(JvCore *c, JvCmds *out, bool eof)
{
    if (c->play_state == JV_PLAY_IDLE) return;
    long pos = eof && c->play.runtime_ms > 0 ? c->play.runtime_ms : jv_core_position(c);
    if (c->play_state == JV_PLAY_FETCHING) { JvCmd *k = emit(out, JV_CMD_CANCEL); k->kind = JF_KIND_STREAM; }
    else emit(out, JV_CMD_PLAYER_STOP);
    JvCmd *k = emit(out, JV_CMD_CANCEL); k->kind = JF_KIND_STREAM; k->api = 1;
    emit(out, JV_CMD_SUB_CLEAR);
    if (c->started_sent) post_report(c, out, 2, pos, NULL);
    end_job(c, out, c->play.session_id);
    c->play_state = JV_PLAY_IDLE; c->seek_pending = false; c->started_sent = false;
}

static void change_sub(JvCore *c, JvCmds *out, int pos)
{
    c->sel_sub = pos < 0 || pos >= c->detail.n_subs ? -1 : pos;
    if (c->screen != JV_SCR_PLAYER) return;
    c->play.sub_index = sub_index(c);
    { JvCmd *k = emit(out, JV_CMD_CANCEL); k->kind = JF_KIND_STREAM; k->api = 1; }
    load_sub(c, out);
    if (c->play_state == JV_PLAY_PLAYING || c->play_state == JV_PLAY_PAUSED) post_report(c, out, 1, jv_core_position(c), "timeupdate");
}

static void change_audio(JvCore *c, JvCmds *out, int pos)
{
    if (pos < 0 || pos >= c->detail.n_audio) return;
    c->sel_audio = pos;
    if (c->screen != JV_SCR_PLAYER || c->play_state == JV_PLAY_IDLE) return;
    c->play.audio_index = audio_index(c);
    restart_stream(c, out, jv_core_position(c), "timeupdate");
}

static void seek(JvCore *c, long target)
{
    if (c->screen != JV_SCR_PLAYER || c->play_state == JV_PLAY_IDLE) return;
    long rt = c->play.runtime_ms;
    if (target < 0) target = 0;
    if (rt > 1000 && target > rt - 1000) target = rt - 1000;
    c->seek_pending = true; c->seek_target_ms = target;
    c->seek_due_ms = c->now_ms + JV_SEEK_QUIET_MS;
}

static void on_tick(JvCore *c, JvCmds *out)
{
    if (c->screen != JV_SCR_PLAYER) return;
    if (c->seek_pending && c->now_ms >= c->seek_due_ms) { restart_stream(c, out, c->seek_target_ms, "seek"); return; }
    if (c->play_state == JV_PLAY_PLAYING && c->now_ms >= c->next_report_ms) post_report(c, out, 1, jv_core_position(c), "timeupdate");
}

/* ── results ───────────────────────────────────────────────────────────── */

static void fail(JvCore *c, const JfResult *r, bool retry)
{
    if (r->err == JF_ERR_UNAUTH) { session_lost(c); return; }
    char t[48]; err_text(t, sizeof t, r->err);
    set_overlay(c, t, retry ? (JV_BTN_RETRY | JV_BTN_CLOSE) : JV_BTN_CLOSE);
}

static void on_auth(JvCore *c, const JfResult *r, JvCmds *out)
{
    if (!c->auth_pending || r->tag != c->auth_tag) return;
    c->auth_pending = false;
    if (r->err == JF_ERR_UNAUTH) { banner(c, "WRONG USER OR PASSWORD"); update_busy(c); return; }
    if (r->err != JF_ERR_NONE || !r->token[0] || !r->user_id[0]) { char t[48]; err_text(t, sizeof t, r->err); banner(c, t); update_busy(c); return; }
    copy(c->login.token, sizeof c->login.token, r->token);
    copy(c->login.user_id, sizeof c->login.user_id, r->user_id);
    jf_login_save(&c->login, c->conf_path);
    emit(out, JV_CMD_SET_AUTH);
    go_home(c, out);
}

static void on_home(JvCore *c, const JfResult *r, JvCmds *out)
{
    if (r->tag != c->home_tag) return;
    if (r->err == JF_ERR_UNAUTH) { c->home_phase = 0; c->home_tag = 0; session_lost(c); return; }
    if (c->home_phase == 1) {
        c->home_movies = c->home_shows = false;
        for (int k = 0; k < c->libs.n; k++) {
            if (c->libs.items[k].type == JV_LIB_MOVIES) c->home_movies = true;
            if (c->libs.items[k].type == JV_LIB_SHOWS) c->home_shows = true;
        }
    }
    if (c->home_phase == 2) c->home_resume_n = r->err == JF_ERR_NONE ? c->lists[1].n : 0;
    if (c->home_phase == 3) c->home_next_n = r->err == JF_ERR_NONE ? c->lists[2].n : 0;
    c->home_phase++;
    if (c->home_phase > 3) { c->home_phase = 0; c->home_tag = 0; c->home_ready = true; update_busy(c); return; }
    home_step(c, out);
}

static void on_list(JvCore *c, const JfResult *r, JvCmds *out)
{
    if (r->tag != c->list_tag) return;
    JvList *l = &c->lists[0];
    l->pending = false;
    if (r->err != JF_ERR_NONE) { update_busy(c); fail(c, r, true); return; }
    if (c->kind == JV_K_SEARCH && c->search_phase < 3) {
        c->search_sec[c->search_phase - 1] = l->n;
        c->search_phase++;
        req_list(c, out, false);
        return;
    }
    update_busy(c);
}

static void pick_tracks(JvCore *c)
{
    const JvDetail *d = &c->detail;
    c->sel_audio = jv_pick_audio(d->audio, d->n_audio, c->set.audio_lang);
    c->sel_sub = jv_pick_sub(d->subs, d->n_subs, c->set.sub_mode, c->set.sub_lang, c->set.audio_lang,
                             c->sel_audio >= 0 ? &d->audio[c->sel_audio] : NULL);
}

static void on_detail(JvCore *c, const JfResult *r)
{
    if (r->tag != c->detail_tag) return;
    c->detail_tag = 0;
    update_busy(c);
    if (r->err != JF_ERR_NONE) { c->detail_refresh = false; fail(c, r, true); return; }
    c->detail = c->detail_in;
    c->detail_ready = true;
    if (c->screen == JV_SCR_DETAILS && !c->detail_refresh) { pick_tracks(c); }
    if (c->sel_audio >= c->detail.n_audio) c->sel_audio = c->detail.n_audio ? 0 : -1;
    if (c->sel_sub >= c->detail.n_subs) c->sel_sub = -1;
    c->detail_refresh = false;
}

static void on_result(JvCore *c, const JfResult *r, JvCmds *out)
{
    switch (r->kind) {
    case JF_KIND_AUTH: on_auth(c, r, out); break;
    case JF_KIND_PARSE:
        if (r->tag == c->home_tag) on_home(c, r, out);
        else if (r->tag == c->list_tag) on_list(c, r, out);
        else if (r->tag == c->detail_tag) on_detail(c, r);
        break;
    case JF_KIND_POST: case JF_KIND_WRITE:
        if (r->err != JF_ERR_NONE) c->report_fail++;     /* never retried: the next report carries the newer position */
        break;
    case JF_KIND_STREAM:
        if (r->tag == c->stream_tag) on_stream(c, r, out);
        else if (r->tag == c->sub_tag && c->sub_tag) {
            c->sub_tag = 0;
            if (r->err == JF_ERR_NONE && r->fd >= 0) { JvCmd *f = emit(out, JV_CMD_SUB_FEED); f->fd = r->fd; }
            else if (r->fd >= 0) { JvCmd *k = emit(out, JV_CMD_CLOSE_FD); k->fd = r->fd; }
        } else if (r->fd >= 0) { JvCmd *k = emit(out, JV_CMD_CLOSE_FD); k->fd = r->fd; }
        break;
    default: break;
    }
}

/* ── intents ───────────────────────────────────────────────────────────── */

static void on_start(JvCore *c, JvCmds *out)
{
    jf_login_load(&c->login, c->conf_path, c->id_suffix);
    JfLogin other;
    memset(&other, 0, sizeof other);
    if (c->reuse_path[0] && jf_login_load(&other, c->reuse_path, "") == 0 && other.server[0] && other.user[0]) {
        c->reuse_available = true;
        copy(c->reuse_server, sizeof c->reuse_server, other.server);
        copy(c->reuse_user, sizeof c->reuse_user, other.user);
    }
    jf_login_wipe(&other);          /* its token is not ours to keep */
    char host[JF_HOST_MAX];
    int port = 0; bool tls = false;
    if (c->login.token[0] && c->login.user_id[0] && jf_parse_server(c->login.server, host, sizeof host, &port, &tls) == JF_SERVER_OK) {
        set_server(c, out, host, port, tls);
        go_home(c, out);
    } else go_signin(c);
}

static void on_signin(JvCore *c, const JvEvent *ev, JvCmds *out)
{
    if (c->screen != JV_SCR_SIGNIN) return;
    char host[JF_HOST_MAX];
    int port = 0; bool tls = false;
    switch (jf_parse_server(ev->server, host, sizeof host, &port, &tls)) {
    case JF_SERVER_OK: break;
    case JF_SERVER_EMPTY: banner(c, "ENTER THE SERVER ADDRESS"); return;
    case JF_SERVER_INSECURE: banner(c, "USE HTTPS://"); return;
    default: banner(c, "BAD SERVER ADDRESS"); return;
    }
    if (!ev->user[0]) { banner(c, "ENTER THE USER NAME"); return; }
    copy(c->login.server, sizeof c->login.server, ev->server);
    copy(c->login.user, sizeof c->login.user, ev->user);
    c->banner[0] = '\0';
    set_server(c, out, host, port, tls);
    JvCmd *q = emit(out, JV_CMD_REQUEST);
    q->kind = JF_KIND_AUTH; q->tag = c->auth_tag = ++c->req_tag;
    copy(q->user, sizeof q->user, ev->user); copy(q->password, sizeof q->password, ev->password);
    c->auth_pending = true;
    update_busy(c);
}

static void on_activate(JvCore *c, JvCmds *out, int i)
{
    if (c->screen != JV_SCR_LIST && c->screen != JV_SCR_SERIES && c->screen != JV_SCR_SEASON) return;
    JvList *l = &c->lists[0];
    if (i < 0 || i >= l->n) return;
    const JvItem *it = &l->items[i];
    switch (it->type) {
    case JV_MOVIE: case JV_EPISODE: enter_details(c, out, it->b.id, it->b.name); break;
    case JV_SERIES: enter_list(c, out, JV_K_SEASONS, it->b.id, "", it->b.name, true); break;
    case JV_SEASON: enter_list(c, out, JV_K_EPISODES, it->series_id, it->b.id, it->b.name, true); break;
    case JV_PERSON: enter_list(c, out, JV_K_PERSON, it->b.id, "", it->b.name, true); break;
    case JV_GENRE:  enter_list(c, out, JV_K_GENRE, it->b.id, "", it->b.name, true); break;
    default: break;
    }
}

static void on_search(JvCore *c, JvCmds *out, const char *term)
{
    if (c->screen == JV_SCR_SIGNIN || c->screen == JV_SCR_PLAYER) return;
    if (!term[0]) { JvCmd *k = emit(out, JV_CMD_CANCEL); k->kind = JF_KIND_PARSE; c->list_tag = 0; c->lists[0].pending = false; update_busy(c); return; }
    copy(c->search_term, sizeof c->search_term, term);
    bool replace = c->screen == JV_SCR_LIST && c->kind == JV_K_SEARCH;
    enter_list(c, out, JV_K_SEARCH, "", "", "SEARCH", !replace);
}

static void on_retry(JvCore *c, JvCmds *out)
{
    c->overlay = false;
    if (c->retry.type != JV_CMD_REQUEST) return;
    JvCmd *r = emit(out, JV_CMD_REQUEST);
    *r = c->retry;
    r->tag = ++c->req_tag;
    if (r->fn == jv_parse_detail) c->detail_tag = r->tag;
    else { c->list_tag = r->tag; c->lists[0].pending = true; }
    c->retry = *r;
    update_busy(c);
}

void jv_step(JvCore *c, const JvEvent *ev, long long now_ms, JvCmds *out)
{
    out->n = 0; out->overflow = false;
    c->now_ms = now_ms;
    switch (ev->type) {
    case JV_EV_START: on_start(c, out); break;
    case JV_EV_REUSE_LOGIN:
        if (c->screen == JV_SCR_SIGNIN && c->reuse_available) {
            copy(c->signin_server, sizeof c->signin_server, c->reuse_server);
            copy(c->signin_user, sizeof c->signin_user, c->reuse_user);
            c->focus_password = true;
        }
        break;
    case JV_EV_SIGNIN: on_signin(c, ev, out); break;
    case JV_EV_HOME:
        if (c->screen != JV_SCR_HOME) break;
        switch ((JvTile)ev->i) {
        case JV_TILE_RESUME: if (c->home_resume_n > 0) enter_list(c, out, JV_K_RESUME, "", "", "CONTINUE", true); break;
        case JV_TILE_NEXTUP: if (c->home_next_n > 0) enter_list(c, out, JV_K_NEXTUP, "", "", "NEXT UP", true); break;
        case JV_TILE_MOVIES: if (c->home_movies) enter_list(c, out, JV_K_MOVIES, "", "", "MOVIES", true); break;
        case JV_TILE_SHOWS:  if (c->home_shows) enter_list(c, out, JV_K_SHOWS, "", "", "SHOWS", true); break;
        }
        break;
    case JV_EV_ACTIVATE: on_activate(c, out, ev->i); break;
    case JV_EV_BACK:
        if (c->screen == JV_SCR_PLAYER) { stop_play(c, out, false); pop(c, out); }
        else if (c->overlay) c->overlay = false;
        else pop(c, out);
        break;
    case JV_EV_NEED_MORE: {
        JvList *l = &c->lists[0];
        bool paged = c->kind == JV_K_MOVIES || c->kind == JV_K_SHOWS || c->kind == JV_K_PERSON || c->kind == JV_K_GENRE;
        if (c->screen == JV_SCR_LIST && paged && !l->pending && !l->full && l->loaded > 0 && l->loaded < l->total) req_list(c, out, false);
        break;
    }
    case JV_EV_SEARCH: on_search(c, out, ev->term); break;
    case JV_EV_SIMILAR:
        if (c->screen == JV_SCR_DETAILS && c->detail_ready) enter_list(c, out, JV_K_SIMILAR, c->detail.it.b.id, "", "SIMILAR", true);
        break;
    case JV_EV_CAST_OPEN:
        if (c->screen == JV_SCR_DETAILS && c->detail_ready) { push_here(c, NULL); c->screen = JV_SCR_CAST; }
        break;
    case JV_EV_CAST_ACTIVATE:
        if (c->screen == JV_SCR_CAST && ev->i >= 0 && ev->i < c->detail.n_people && c->detail.people[ev->i].id[0])
            enter_list(c, out, JV_K_PERSON, c->detail.people[ev->i].id, "", c->detail.people[ev->i].name, true);
        break;
    case JV_EV_AUDIO_SET: change_audio(c, out, ev->i); break;
    case JV_EV_AUDIO_NEXT: change_audio(c, out, jv_next_audio(c->detail.n_audio, c->sel_audio)); break;
    case JV_EV_SUB_SET: change_sub(c, out, ev->i); break;
    case JV_EV_SUB_NEXT: change_sub(c, out, jv_next_sub(c->detail.subs, c->detail.n_subs, c->sel_sub)); break;
    case JV_EV_PLAY: on_play(c, out, ev->i); break;
    case JV_EV_PAUSE:
        if (c->play_state == JV_PLAY_PLAYING) { c->play_state = JV_PLAY_PAUSED; emit(out, JV_CMD_PLAYER_PAUSE); post_report(c, out, 1, jv_core_position(c), "pause"); }
        else if (c->play_state == JV_PLAY_PAUSED) { c->play_state = JV_PLAY_PLAYING; emit(out, JV_CMD_PLAYER_RESUME); post_report(c, out, 1, jv_core_position(c), "unpause"); }
        break;
    case JV_EV_SEEK_BY: seek(c, jv_core_position(c) + ev->i); break;
    case JV_EV_SEEK_TO: seek(c, ev->i); break;
    case JV_EV_STOP:
        if (c->screen == JV_SCR_PLAYER) { stop_play(c, out, false); pop(c, out); }
        break;
    case JV_EV_QUIT:
        stop_play(c, out, false);
        emit(out, JV_CMD_EXIT);
        break;
    case JV_EV_RETRY: on_retry(c, out); break;
    case JV_EV_CLOSE_ERROR: c->overlay = false; break;
    case JV_EV_SETTINGS: break;
    case JV_EV_RESULT: on_result(c, &ev->res, out); break;
    case JV_EV_POS: if (c->play_state != JV_PLAY_FETCHING && ev->i >= 0) c->pos_ms = ev->i; break;
    case JV_EV_PLAYER_ENDED:
        if (c->play_state != JV_PLAY_PLAYING && c->play_state != JV_PLAY_PAUSED) break;
        if (ev->i) { stop_play(c, out, true); pop(c, out); }
        else if (!c->restarted_once) { c->restarted_once = true; restart_stream(c, out, jv_core_position(c), "timeupdate"); }
        else { stop_play(c, out, false); pop(c, out); set_overlay(c, "PLAYBACK FAILED", JV_BTN_CLOSE); }
        break;
    case JV_EV_TICK: on_tick(c, out); break;
    default: break;
    }
}
