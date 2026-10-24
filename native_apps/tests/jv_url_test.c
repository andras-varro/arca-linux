/*
 * tests/jv_url_test.c — the video client's exact URLs and JSON bodies, each also at cap == len+1 (fits) and
 * cap == len (refused).
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -std=c11 -Wall -Wextra -I common -I jf_shared -I jfvideo tests/jv_url_test.c jfvideo/jv_url.c \
 *       jf_shared/jf_url.c -o build/jv_url_test && ./build/jv_url_test
 */

#include <stdio.h>
#include <string.h>

#include "jv_url.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define UID "0123456789abcdef0123456789abcdef"
#define SER "fedcba9876543210fedcba9876543210"
#define SEA "00112233445566778899aabbccddeeff"
#define SES "aaaaaaaabbbbbbbbccccccccdddddddd"
#define LIB "11111111222222223333333344444444"

#define EXPECT(what, n, buf, want) do { int len_ = (int)strlen(want); \
    CHECK((n) == len_, "%s: returned %d want %d", what, (n), len_); \
    CHECK(strcmp(buf, want) == 0, "%s:\n  got  \"%s\"\n  want \"%s\"", what, buf, want); } while (0)

#define CAP_EDGE(what, call) do { char b_[640]; size_t cap = sizeof b_; int len_ = (call); \
    CHECK(len_ > 0, "%s: baseline failed", what); \
    cap = (size_t)len_ + 1; CHECK((call) == len_, "%s: cap=len+1 refused", what); \
    cap = (size_t)len_;     CHECK((call) == -1, "%s: cap=len accepted", what); } while (0)

int main(void)
{
    char b[640];
    int n;

    n = jv_path_resume(b, sizeof b, UID, 24);
    EXPECT("resume", n, b, "/Users/" UID "/Items/Resume?MediaTypes=Video&Limit=24&Fields=UserData");
    n = jv_path_next_up(b, sizeof b, UID, 24);
    EXPECT("next up", n, b, "/Shows/NextUp?UserId=" UID "&Limit=24");
    n = jv_path_movies(b, sizeof b, UID, "", 60, 30);
    EXPECT("movies all", n, b, "/Users/" UID "/Items?IncludeItemTypes=Movie&Recursive=true&SortBy=SortName"
           "&StartIndex=60&Limit=30&Fields=UserData");
    n = jv_path_movies(b, sizeof b, UID, LIB, 0, 30);
    EXPECT("movies in lib", n, b, "/Users/" UID "/Items?IncludeItemTypes=Movie&Recursive=true&SortBy=SortName"
           "&StartIndex=0&Limit=30&Fields=UserData&ParentId=" LIB);
    n = jv_path_shows(b, sizeof b, UID, NULL, 0, 30);
    EXPECT("shows", n, b, "/Users/" UID "/Items?IncludeItemTypes=Series&Recursive=true&SortBy=SortName"
           "&StartIndex=0&Limit=30&Fields=UserData");
    CHECK(jv_path_movies(b, sizeof b, UID, "zz", 0, 30) == -1, "bad library id refused");
    n = jv_path_seasons(b, sizeof b, UID, SER);
    EXPECT("seasons", n, b, "/Shows/" SER "/Seasons?UserId=" UID);
    n = jv_path_episodes(b, sizeof b, UID, SER, SEA);
    EXPECT("episodes", n, b, "/Shows/" SER "/Episodes?UserId=" UID "&SeasonId=" SEA "&Fields=UserData");
    n = jv_path_similar(b, sizeof b, UID, SER, 12);
    EXPECT("similar", n, b, "/Items/" SER "/Similar?UserId=" UID "&Limit=12");
    n = jv_path_search_items(b, sizeof b, UID, "a b&c", 40);
    EXPECT("search items", n, b, "/Users/" UID "/Items?SearchTerm=a%20b%26c&IncludeItemTypes=Movie,Series,Episode"
           "&Recursive=true&Limit=40");
    n = jv_path_search_people(b, sizeof b, "Kov\xc3\xa1" "cs", 10);
    EXPECT("search people", n, b, "/Persons?SearchTerm=Kov%C3%A1cs&Limit=10");
    n = jv_path_search_genres(b, sizeof b, "dra", 10);
    EXPECT("search genres", n, b, "/Genres?SearchTerm=dra&IncludeItemTypes=Movie,Series&Limit=10");
    n = jv_path_by_person(b, sizeof b, UID, SEA, 0, 30);
    EXPECT("by person", n, b, "/Users/" UID "/Items?PersonIds=" SEA "&IncludeItemTypes=Movie,Series&Recursive=true"
           "&SortBy=SortName&StartIndex=0&Limit=30&Fields=UserData");
    n = jv_path_by_genre(b, sizeof b, UID, SEA, 30, 30);
    EXPECT("by genre", n, b, "/Users/" UID "/Items?GenreIds=" SEA "&IncludeItemTypes=Movie,Series&Recursive=true"
           "&SortBy=SortName&StartIndex=30&Limit=30&Fields=UserData");
    n = jv_path_detail(b, sizeof b, UID, SER);
    EXPECT("detail", n, b, "/Users/" UID "/Items/" SER "?Fields=People,Overview,Genres,MediaSources");

    n = jv_path_stream(b, sizeof b, SER, SEA, 1, 44100, 400, 2, 0, SES, "arca-1a2b-v");
    EXPECT("stream from start", n, b, "/Videos/" SER "/stream.mpg?container=mpeg&MediaSourceId=" SEA
           "&videoCodec=mpeg1video&audioCodec=mp2&audioChannels=1&audioSampleRate=44100&maxWidth=400&maxHeight=240"
           "&videoBitRate=800000&AudioStreamIndex=2&PlaySessionId=" SES "&DeviceId=arca-1a2b-v");
    n = jv_path_stream(b, sizeof b, SER, SEA, 2, 48000, 320, -1, 3723456, SES, "a b");
    EXPECT("stream seek, stereo, no audio index", n, b, "/Videos/" SER "/stream.mpg?container=mpeg&MediaSourceId=" SEA
           "&videoCodec=mpeg1video&audioCodec=mp2&audioChannels=2&audioSampleRate=48000&maxWidth=320&maxHeight=240"
           "&videoBitRate=800000&StartTimeTicks=37234560000&PlaySessionId=" SES "&DeviceId=a%20b");
    n = jv_path_stream(b, sizeof b, SER, SEA, 7, 44100, 400, 0, 0, SES, "d");
    CHECK(strstr(b, "audioChannels=1&") && strstr(b, "AudioStreamIndex=0&"), "odd channel count -> 1; index 0 is kept");
    CHECK(strstr(b, "api_key") == NULL && strstr(b, "Token") == NULL, "no token in the URL");
    CHECK(jv_path_stream(b, sizeof b, SER, SEA, 1, 44100, 401, 0, 0, SES, "d") == -1, "width over max refused");
    CHECK(jv_path_stream(b, sizeof b, SER, SEA, 1, 44100, 400, 0, -1, SES, "d") == -1, "negative start refused");
    CHECK(jv_path_stream(b, sizeof b, SER, SEA, 1, 100, 400, 0, 0, SES, "d") == -1, "silly rate refused");
    CHECK(jv_path_stream(b, sizeof b, SER, SEA, 1, 44100, 400, 0, 0, SES, "") == -1, "empty device id refused");
    CHECK(jv_path_stream(b, sizeof b, SER, "zz", 1, 44100, 400, 0, 0, SES, "d") == -1, "bad source id refused");
    CHECK(jv_path_stream(b, sizeof b, SER, SEA, 1, 44100, 400, 0, 0, "short", "d") == -1, "bad session id refused");

    n = jv_path_subtitle(b, sizeof b, SER, SEA, 5);
    EXPECT("subtitle", n, b, "/Videos/" SER "/" SEA "/Subtitles/5/0/Stream.srt");
    CHECK(jv_path_subtitle(b, sizeof b, SER, SEA, -1) == -1, "negative subtitle index refused");
    n = jv_path_end_job(b, sizeof b, "arca-1a2b-v", SES);
    EXPECT("end job", n, b, "/Videos/ActiveEncodings?deviceId=arca-1a2b-v&playSessionId=" SES);
    CHECK(jv_path_end_job(b, sizeof b, "d", "xx") == -1, "end job: bad session id refused");
    n = jv_path_playing(b, sizeof b);  EXPECT("playing", n, b, "/Sessions/Playing");
    n = jv_path_progress(b, sizeof b); EXPECT("progress", n, b, "/Sessions/Playing/Progress");
    n = jv_path_stopped(b, sizeof b);  EXPECT("stopped", n, b, "/Sessions/Playing/Stopped");

    JvReport r = { SER, SEA, SES, 90500, 2, -1, 0, NULL };
    n = jv_body_started(b, sizeof b, &r);
    EXPECT("started", n, b, "{\"ItemId\":\"" SER "\",\"MediaSourceId\":\"" SEA "\",\"PlaySessionId\":\"" SES
           "\",\"PositionTicks\":905000000,\"AudioStreamIndex\":2,\"SubtitleStreamIndex\":-1,"
           "\"PlayMethod\":\"Transcode\",\"CanSeek\":true,\"IsPaused\":false}");
    r.paused = 1; r.sub_index = 4; r.event = "pause";
    n = jv_body_progress(b, sizeof b, &r);
    EXPECT("progress body", n, b, "{\"ItemId\":\"" SER "\",\"MediaSourceId\":\"" SEA "\",\"PlaySessionId\":\"" SES
           "\",\"PositionTicks\":905000000,\"AudioStreamIndex\":2,\"SubtitleStreamIndex\":4,"
           "\"PlayMethod\":\"Transcode\",\"CanSeek\":true,\"IsPaused\":true,\"EventName\":\"pause\"}");
    n = jv_body_stopped(b, sizeof b, &r);
    EXPECT("stopped body", n, b, "{\"ItemId\":\"" SER "\",\"MediaSourceId\":\"" SEA "\",\"PlaySessionId\":\"" SES
           "\",\"PositionTicks\":905000000}");
    r.event = "bogus\"";
    CHECK(jv_body_progress(b, sizeof b, &r) == -1, "unknown event name refused");
    r.event = NULL; r.position_ms = -1;
    CHECK(jv_body_started(b, sizeof b, &r) == -1, "negative position refused");
    r.position_ms = 0; r.item_id = "x";
    CHECK(jv_body_started(b, sizeof b, &r) == -1, "bad item id refused");
    r.item_id = SER;

    CAP_EDGE("resume", jv_path_resume(b_, cap, UID, 24));
    CAP_EDGE("movies lib", jv_path_movies(b_, cap, UID, LIB, 0, 30));
    CAP_EDGE("search", jv_path_search_items(b_, cap, UID, "x y", 40));
    CAP_EDGE("stream", jv_path_stream(b_, cap, SER, SEA, 1, 44100, 400, 2, 5000, SES, "dev"));
    CAP_EDGE("stream no extras", jv_path_stream(b_, cap, SER, SEA, 1, 44100, 400, -1, 0, SES, "dev"));
    CAP_EDGE("subtitle", jv_path_subtitle(b_, cap, SER, SEA, 12));
    CAP_EDGE("end job", jv_path_end_job(b_, cap, "dev", SES));
    CAP_EDGE("started", jv_body_started(b_, cap, &r));
    r.event = "seek";
    CAP_EDGE("progress body", jv_body_progress(b_, cap, &r));
    CAP_EDGE("stopped body", jv_body_stopped(b_, cap, &r));
    CAP_EDGE("playing", jv_path_playing(b_, cap));

    printf("jv_url_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
