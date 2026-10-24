#include "jv_url.h"

#include <stdio.h>
#include <string.h>

static int fin(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? -1 : n; }

int jv_path_resume(char *out, size_t cap, const char *uid, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap, "/Users/%s/Items/Resume?MediaTypes=Video&Limit=%d&Fields=UserData", uid, limit), cap);
}

int jv_path_next_up(char *out, size_t cap, const char *uid, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap, "/Shows/NextUp?UserId=%s&Limit=%d", uid, limit), cap);
}

static int typed_page(char *out, size_t cap, const char *uid, const char *lib, const char *type, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    int n = fin(snprintf(out, cap, "/Users/%s/Items?IncludeItemTypes=%s&Recursive=true&SortBy=SortName"
                         "&StartIndex=%d&Limit=%d&Fields=UserData", uid, type, start, limit), cap);
    if (n < 0) return -1;
    return jf_path_scope(out, cap, lib);
}

int jv_path_movies(char *out, size_t cap, const char *uid, const char *lib, int start, int limit)
{
    return typed_page(out, cap, uid, lib, "Movie", start, limit);
}

int jv_path_shows(char *out, size_t cap, const char *uid, const char *lib, int start, int limit)
{
    return typed_page(out, cap, uid, lib, "Series", start, limit);
}

int jv_path_seasons(char *out, size_t cap, const char *uid, const char *series_id)
{
    if (!jf_id_valid(uid) || !jf_id_valid(series_id)) return -1;
    return fin(snprintf(out, cap, "/Shows/%s/Seasons?UserId=%s", series_id, uid), cap);
}

int jv_path_episodes(char *out, size_t cap, const char *uid, const char *series_id, const char *season_id)
{
    if (!jf_id_valid(uid) || !jf_id_valid(series_id) || !jf_id_valid(season_id)) return -1;
    return fin(snprintf(out, cap, "/Shows/%s/Episodes?UserId=%s&SeasonId=%s&Fields=UserData",
                        series_id, uid, season_id), cap);
}

int jv_path_similar(char *out, size_t cap, const char *uid, const char *id, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(id)) return -1;
    return fin(snprintf(out, cap, "/Items/%s/Similar?UserId=%s&Limit=%d", id, uid, limit), cap);
}

int jv_path_search_items(char *out, size_t cap, const char *uid, const char *term, int limit)
{
    char enc[256];
    if (!jf_id_valid(uid) || !term || jf_url_encode(enc, sizeof enc, term) < 0) return -1;
    return fin(snprintf(out, cap, "/Users/%s/Items?SearchTerm=%s&IncludeItemTypes=Movie,Series,Episode"
                        "&Recursive=true&Limit=%d", uid, enc, limit), cap);
}

int jv_path_search_people(char *out, size_t cap, const char *term, int limit)
{
    char enc[256];
    if (!term || jf_url_encode(enc, sizeof enc, term) < 0) return -1;
    return fin(snprintf(out, cap, "/Persons?SearchTerm=%s&Limit=%d", enc, limit), cap);
}

int jv_path_search_genres(char *out, size_t cap, const char *term, int limit)
{
    char enc[256];
    if (!term || jf_url_encode(enc, sizeof enc, term) < 0) return -1;
    return fin(snprintf(out, cap, "/Genres?SearchTerm=%s&IncludeItemTypes=Movie,Series&Limit=%d", enc, limit), cap);
}

int jv_path_by_person(char *out, size_t cap, const char *uid, const char *person_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(person_id)) return -1;
    return fin(snprintf(out, cap, "/Users/%s/Items?PersonIds=%s&IncludeItemTypes=Movie,Series&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d&Fields=UserData", uid, person_id, start, limit), cap);
}

int jv_path_by_genre(char *out, size_t cap, const char *uid, const char *genre_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(genre_id)) return -1;
    return fin(snprintf(out, cap, "/Users/%s/Items?GenreIds=%s&IncludeItemTypes=Movie,Series&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d&Fields=UserData", uid, genre_id, start, limit), cap);
}

int jv_path_detail(char *out, size_t cap, const char *uid, const char *id)
{
    if (!jf_id_valid(uid) || !jf_id_valid(id)) return -1;
    return fin(snprintf(out, cap, "/Users/%s/Items/%s?Fields=People,Overview,Genres,MediaSources", uid, id), cap);
}

int jv_path_stream(char *out, size_t cap, const char *id, const char *media_source_id, int channels, int rate,
                   int max_w, int audio_index, long start_ms, const char *session_id, const char *device_id)
{
    char dev[160];
    size_t n;
    int r;
    if (!jf_id_valid(id) || !jf_id_valid(media_source_id) || !jf_id_valid(session_id)) return -1;
    if (!device_id || !device_id[0] || jf_url_encode(dev, sizeof dev, device_id) < 0) return -1;
    if (rate < 8000 || rate > 48000 || max_w < JV_STREAM_MIN_W || max_w > JV_STREAM_MAX_W || start_ms < 0) return -1;
    r = fin(snprintf(out, cap, "/Videos/%s/stream.mpg?container=mpeg&MediaSourceId=%s&videoCodec=mpeg1video"
                     "&audioCodec=mp2&audioChannels=%d&audioSampleRate=%d&maxWidth=%d&maxHeight=%d&videoBitRate=%d",
                     id, media_source_id, channels == 2 ? 2 : 1, rate, max_w, JV_STREAM_H, JV_STREAM_VBITRATE), cap);
    if (r < 0) return -1;
    n = (size_t)r;
    if (audio_index >= 0) {
        r = fin(snprintf(out + n, cap - n, "&AudioStreamIndex=%d", audio_index), cap - n);
        if (r < 0) return -1;
        n += (size_t)r;
    }
    if (start_ms > 0) {
        r = fin(snprintf(out + n, cap - n, "&StartTimeTicks=%lld", (long long)start_ms * 10000LL), cap - n);
        if (r < 0) return -1;
        n += (size_t)r;
    }
    r = fin(snprintf(out + n, cap - n, "&PlaySessionId=%s&DeviceId=%s", session_id, dev), cap - n);
    return r < 0 ? -1 : (int)(n + (size_t)r);
}

int jv_path_subtitle(char *out, size_t cap, const char *id, const char *media_source_id, int index)
{
    if (!jf_id_valid(id) || !jf_id_valid(media_source_id) || index < 0) return -1;
    return fin(snprintf(out, cap, "/Videos/%s/%s/Subtitles/%d/0/Stream.srt", id, media_source_id, index), cap);
}

int jv_path_end_job(char *out, size_t cap, const char *device_id, const char *session_id)
{
    char dev[160];
    if (!jf_id_valid(session_id)) return -1;
    if (!device_id || !device_id[0] || jf_url_encode(dev, sizeof dev, device_id) < 0) return -1;
    return fin(snprintf(out, cap, "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s", dev, session_id), cap);
}

int jv_path_playing(char *out, size_t cap)  { return fin(snprintf(out, cap, "/Sessions/Playing"), cap); }
int jv_path_progress(char *out, size_t cap) { return fin(snprintf(out, cap, "/Sessions/Playing/Progress"), cap); }
int jv_path_stopped(char *out, size_t cap)  { return fin(snprintf(out, cap, "/Sessions/Playing/Stopped"), cap); }

static int report_ok(const JvReport *r)
{
    return r && jf_id_valid(r->item_id) && jf_id_valid(r->media_source_id) && jf_id_valid(r->session_id)
           && r->position_ms >= 0;
}

static int event_ok(const char *e)
{
    static const char *const ok[] = { "timeupdate", "pause", "unpause", "seek" };
    if (!e) return 1;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (!strcmp(e, ok[i])) return 1;
    return 0;
}

/* Ids and the event name are checked above, so nothing here needs JSON escaping. */
static int body(char *out, size_t cap, const JvReport *r, int full, int with_event)
{
    int n;
    if (!report_ok(r) || !event_ok(r->event)) return -1;
    n = snprintf(out, cap, "{\"ItemId\":\"%s\",\"MediaSourceId\":\"%s\",\"PlaySessionId\":\"%s\",\"PositionTicks\":%lld",
                 r->item_id, r->media_source_id, r->session_id, (long long)r->position_ms * 10000LL);
    if (fin(n, cap) < 0) return -1;
    if (full) {
        int m = snprintf(out + n, cap - (size_t)n,
                         ",\"AudioStreamIndex\":%d,\"SubtitleStreamIndex\":%d,\"PlayMethod\":\"Transcode\","
                         "\"CanSeek\":true,\"IsPaused\":%s",
                         r->audio_index < 0 ? 0 : r->audio_index, r->sub_index < 0 ? -1 : r->sub_index,
                         r->paused ? "true" : "false");
        if (fin(m, cap - (size_t)n) < 0) return -1;
        n += m;
    }
    if (with_event && r->event) {
        int m = snprintf(out + n, cap - (size_t)n, ",\"EventName\":\"%s\"", r->event);
        if (fin(m, cap - (size_t)n) < 0) return -1;
        n += m;
    }
    if ((size_t)n + 2 > cap) return -1;
    out[n++] = '}';
    out[n] = '\0';
    return n;
}

int jv_body_started(char *out, size_t cap, const JvReport *r)  { return body(out, cap, r, 1, 0); }
int jv_body_progress(char *out, size_t cap, const JvReport *r) { return body(out, cap, r, 1, 1); }
int jv_body_stopped(char *out, size_t cap, const JvReport *r)  { return body(out, cap, r, 0, 0); }
