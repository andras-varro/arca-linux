/*
 * tests/jf_url_test.c — the Jellyfin wire strings: every endpoint builder's exact
 * output and its cap boundary, id validation, percent-encoding, the server
 * address parser, the playSessionId source and the two secret-carrying builders
 * (Authorization header, login body).
 *
 * Exact-string checks matter more than "contains": a builder that drops a
 * parameter or reorders them still looks plausible, and the server answers 200
 * with the wrong list.  Each builder is also run at cap == len+1 (must fit) and
 * cap == len (must be refused), the boundary an off-by-one in fin() would move.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -I common -I jellyfin tests/jf_url_test.c jellyfin/jf_url.c \
 *       -o build/jf_url_test && ./build/jf_url_test
 */

#include <stdio.h>
#include <string.h>

#include "jf_url.h"
#include "jf_api.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define UID  "0123456789abcdef0123456789abcdef"
#define ART  "fedcba9876543210fedcba9876543210"
#define ALB  "00112233445566778899AABBCCDDEEFF"
#define SES  "aaaaaaaabbbbbbbbccccccccdddddddd"

static void expect_str(int line, const char *what, int n, const char *buf, const char *want)
{
    int len = (int)strlen(want);
    CHECK(n == len, "line %d %s: returned %d, want %d", line, what, n, len);
    CHECK(strcmp(buf, want) == 0, "line %d %s: got \"%s\"\n  want \"%s\"", line, what, buf, want);
}
#define EXPECT(what, n, buf, want) expect_str(__LINE__, what, n, buf, want)

/* `call` is written against `b_` and `cap`; it runs at a roomy cap, then len+1, then len. */
#define CAP_EDGE(what, call) do { \
    char b_[512]; size_t cap = sizeof(b_); \
    int len_ = (call); \
    CHECK(len_ > 0, "%s: baseline failed", what); \
    cap = (size_t)len_ + 1; CHECK((call) == len_, "%s: cap=len+1 refused", what); \
    cap = (size_t)len_;     CHECK((call) == -1, "%s: cap=len accepted", what); \
    } while (0)

/* Bytes 00 11 22 .. ff, so the expected id is known without re-deriving the hex code. */
static int fake_ok(unsigned char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(i * 0x11);
    return (int)n;
}
static int fake_fail(unsigned char *buf, size_t n) { (void)buf; (void)n; return -1; }
static int fake_short(unsigned char *buf, size_t n) { (void)buf; return (int)n - 1; }

static void test_builders(void)
{
    char b[512];
    int n;

    n = jf_path_auth(b, sizeof(b));
    EXPECT("auth", n, b, "/Users/AuthenticateByName");
    CAP_EDGE("auth", jf_path_auth(b_, cap));

    n = jf_path_artists(b, sizeof(b), UID, 0, 50);
    EXPECT("artists", n, b, "/Artists/AlbumArtists?userId=" UID "&StartIndex=0&Limit=50");
    CAP_EDGE("artists", jf_path_artists(b_, cap, UID, 0, 50));

    n = jf_path_albums(b, sizeof(b), UID, ART, 100, 25);
    EXPECT("albums", n, b, "/Items?userId=" UID "&ArtistIds=" ART
           "&IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName&StartIndex=100&Limit=25");
    CAP_EDGE("albums", jf_path_albums(b_, cap, UID, ART, 100, 25));

    n = jf_path_tracks(b, sizeof(b), UID, ALB, 0, 200);
    EXPECT("tracks", n, b, "/Items?userId=" UID "&ParentId=" ALB
           "&IncludeItemTypes=Audio&SortBy=ParentIndexNumber,IndexNumber&StartIndex=0&Limit=200");
    CAP_EDGE("tracks", jf_path_tracks(b_, cap, UID, ALB, 0, 200));

    n = jf_path_search(b, sizeof(b), UID, "AC/DC & co", 5, 10);
    EXPECT("search", n, b, "/Items?userId=" UID
           "&IncludeItemTypes=Audio,MusicAlbum,MusicArtist&Recursive=true"
           "&SearchTerm=AC%2FDC%20%26%20co&StartIndex=5&Limit=10");
    CAP_EDGE("search", jf_path_search(b_, cap, UID, "AC/DC & co", 5, 10));

    n = jf_path_albums_all(b, sizeof(b), UID, 30, 30);
    EXPECT("albums_all", n, b, "/Items?userId=" UID
           "&IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName&StartIndex=30&Limit=30");
    CAP_EDGE("albums_all", jf_path_albums_all(b_, cap, UID, 30, 30));

    n = jf_path_playlists(b, sizeof(b), UID, 0, 50);
    EXPECT("playlists", n, b, "/Items?userId=" UID "&IncludeItemTypes=Playlist&Recursive=true&StartIndex=0&Limit=50");
    CAP_EDGE("playlists", jf_path_playlists(b_, cap, UID, 0, 50));

    n = jf_path_genres(b, sizeof(b), UID, 50, 50);
    EXPECT("genres", n, b, "/MusicGenres?userId=" UID "&StartIndex=50&Limit=50");
    CAP_EDGE("genres", jf_path_genres(b_, cap, UID, 50, 50));

    n = jf_path_songs(b, sizeof(b), UID, 25, 25);
    EXPECT("songs", n, b, "/Items?userId=" UID
           "&IncludeItemTypes=Audio&Recursive=true&SortBy=SortName&StartIndex=25&Limit=25");
    CAP_EDGE("songs", jf_path_songs(b_, cap, UID, 25, 25));

    n = jf_path_playlist_items(b, sizeof(b), UID, ALB, 0, 25);
    EXPECT("playlist_items", n, b, "/Playlists/" ALB "/Items?userId=" UID "&StartIndex=0&Limit=25");
    CAP_EDGE("playlist_items", jf_path_playlist_items(b_, cap, UID, ALB, 0, 25));

    n = jf_path_genre_albums(b, sizeof(b), UID, ART, 0, 30);
    EXPECT("genre_albums", n, b, "/Items?userId=" UID "&GenreIds=" ART
           "&IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName&StartIndex=0&Limit=30");
    CAP_EDGE("genre_albums", jf_path_genre_albums(b_, cap, UID, ART, 0, 30));

    n = jf_path_stream(b, sizeof(b), ALB, SES, 1);
    EXPECT("stream", n, b, "/Audio/" ALB "/stream?static=false&container=wav&audioCodec=pcm_s16le"
           "&audioSampleRate=44100&audioChannels=1&playSessionId=" SES);
    CAP_EDGE("stream", jf_path_stream(b_, cap, ALB, SES, 1));

    /* a seek: StartTimeTicks, 1 tick = 100 ns, so ms * 10000 (measured against the live server) */
    n = jf_path_stream_at(b, sizeof(b), ALB, SES, 90000, 1);
    EXPECT("stream_at 90 s", n, b, "/Audio/" ALB "/stream?static=false&container=wav&audioCodec=pcm_s16le"
           "&audioSampleRate=44100&audioChannels=1&playSessionId=" SES "&StartTimeTicks=900000000");
    CAP_EDGE("stream_at", jf_path_stream_at(b_, cap, ALB, SES, 90000, 1));
    char plain[512];
    jf_path_stream(plain, sizeof(plain), ALB, SES, 1);
    n = jf_path_stream_at(b, sizeof(b), ALB, SES, 0, 1);
    EXPECT("stream_at 0 = plain", n, b, plain);
    n = jf_path_stream_at(b, sizeof(b), ALB, SES, 4000000, 1);   /* 4000 s: 4e10 ticks overflows 32 bits */
    EXPECT("stream_at 4000 s", n, b, "/Audio/" ALB "/stream?static=false&container=wav&audioCodec=pcm_s16le"
           "&audioSampleRate=44100&audioChannels=1&playSessionId=" SES "&StartTimeTicks=40000000000");
    CHECK(jf_path_stream_at(b, sizeof(b), ALB, SES, -1, 1) == -1, "stream_at: negative offset");
    CHECK(jf_path_stream_at(b, sizeof(b), "bad", SES, 5, 1) == -1, "stream_at: bad id");

    /* the sink decides the channel count: 2 only for exactly 2, and the rest of the URL is the same */
    n = jf_path_stream(b, sizeof(b), ALB, SES, 2);
    EXPECT("stream stereo", n, b, "/Audio/" ALB "/stream?static=false&container=wav&audioCodec=pcm_s16le"
           "&audioSampleRate=44100&audioChannels=2&playSessionId=" SES);
    CAP_EDGE("stream stereo", jf_path_stream(b_, cap, ALB, SES, 2));
    n = jf_path_stream_at(b, sizeof(b), ALB, SES, 90000, 2);
    EXPECT("stream_at stereo", n, b, "/Audio/" ALB "/stream?static=false&container=wav&audioCodec=pcm_s16le"
           "&audioSampleRate=44100&audioChannels=2&playSessionId=" SES "&StartTimeTicks=900000000");
    jf_path_stream(b, sizeof(b), ALB, SES, 0);
    CHECK(strstr(b, "audioChannels=1&") != NULL, "channels 0 asks for mono");
    jf_path_stream(b, sizeof(b), ALB, SES, 6);
    CHECK(strstr(b, "audioChannels=1&") != NULL, "channels 6 asks for mono");
}

static void test_ids(void)
{
    char b[512];
    const char *short_id = "0123456789abcdef0123456789abcde";
    const char *long_id  = "0123456789abcdef0123456789abcdef0";
    const char *bad_chr  = "0123456789abcdef0123456789abcdeg";
    const char *inject   = "0123456789abcdef0123456789abc/..";

    CHECK(jf_id_valid(UID), "valid id refused");
    CHECK(jf_id_valid(ALB), "uppercase hex id refused");
    CHECK(!jf_id_valid(NULL), "NULL id accepted");
    CHECK(!jf_id_valid(""), "empty id accepted");
    CHECK(!jf_id_valid(short_id), "31 digits accepted");
    CHECK(!jf_id_valid(long_id), "33 digits accepted");
    CHECK(!jf_id_valid(bad_chr), "non-hex char accepted");
    CHECK(!jf_id_valid(inject), "path characters accepted");

    CHECK(jf_path_artists(b, sizeof(b), short_id, 0, 1) == -1, "artists: bad uid");
    CHECK(jf_path_artists(b, sizeof(b), NULL, 0, 1) == -1, "artists: NULL uid");
    CHECK(jf_path_albums(b, sizeof(b), bad_chr, ART, 0, 1) == -1, "albums: bad uid");
    CHECK(jf_path_albums(b, sizeof(b), UID, inject, 0, 1) == -1, "albums: bad artist");
    CHECK(jf_path_tracks(b, sizeof(b), long_id, ALB, 0, 1) == -1, "tracks: bad uid");
    CHECK(jf_path_tracks(b, sizeof(b), UID, "", 0, 1) == -1, "tracks: bad album");
    CHECK(jf_path_search(b, sizeof(b), short_id, "x", 0, 1) == -1, "search: bad uid");
    CHECK(jf_path_search(b, sizeof(b), UID, NULL, 0, 1) == -1, "search: NULL term");
    CHECK(jf_path_playlist_items(b, sizeof(b), UID, "", 0, 1) == -1, "playlist_items: bad id");
    CHECK(jf_path_genre_albums(b, sizeof(b), UID, long_id, 0, 1) == -1, "genre_albums: bad id");
    CHECK(jf_path_genres(b, sizeof(b), short_id, 0, 1) == -1, "genres: bad uid");
    CHECK(jf_path_songs(b, sizeof(b), short_id, 0, 1) == -1, "songs: bad uid");
    CHECK(jf_path_albums_all(b, sizeof(b), short_id, 0, 1) == -1, "albums_all: bad uid");
    CHECK(jf_path_playlists(b, sizeof(b), short_id, 0, 1) == -1, "playlists: bad uid");
    CHECK(jf_path_stream(b, sizeof(b), inject, SES, 1) == -1, "stream: bad id");
    CHECK(jf_path_stream(b, sizeof(b), ALB, "short", 1) == -1, "stream: bad session");
}

static void test_session(void)
{
    char a[JF_ID_LEN + 1], c[JF_ID_LEN + 1];

    CHECK(jf_session_id(a) && jf_session_id(c), "urandom session id failed");
    CHECK(strcmp(a, c) != 0, "two urandom session ids equal: %s", a);
    CHECK(strlen(a) == JF_ID_LEN, "session id length %d", (int)strlen(a));
    int lower = 1;
    for (const char *p = a; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) lower = 0;
    CHECK(lower, "session id not lowercase hex: %s", a);
    CHECK(jf_id_valid(a), "session id fails jf_id_valid");

    jf_url_set_random(fake_ok);
    CHECK(jf_session_id(a), "fake random refused");
    CHECK(strcmp(a, "00112233445566778899aabbccddeeff") == 0, "fake random gave %s", a);
    CHECK(jf_session_id(c) && strcmp(a, c) == 0, "fake random not deterministic");

    jf_url_set_random(fake_fail);
    strcpy(a, "junk");
    CHECK(!jf_session_id(a), "failing random reported success");
    CHECK(a[0] == '\0', "failing random left a non-empty id");
    jf_url_set_random(fake_short);
    CHECK(!jf_session_id(a), "short read reported success");

    jf_url_set_random(NULL);
    CHECK(jf_session_id(a) && jf_session_id(c) && strcmp(a, c) != 0, "NULL did not restore urandom");
}

static void test_encode(void)
{
    char b[64];
    int n;

    n = jf_url_encode(b, sizeof(b), "a b");        EXPECT("space", n, b, "a%20b");
    n = jf_url_encode(b, sizeof(b), "a&b");        EXPECT("amp", n, b, "a%26b");
    n = jf_url_encode(b, sizeof(b), "a#b");        EXPECT("hash", n, b, "a%23b");
    n = jf_url_encode(b, sizeof(b), "100%");       EXPECT("percent", n, b, "100%25");
    n = jf_url_encode(b, sizeof(b), "\xC3\xA9");   EXPECT("e-acute", n, b, "%C3%A9");
    n = jf_url_encode(b, sizeof(b), "AZaz09-_.~"); EXPECT("unreserved", n, b, "AZaz09-_.~");
    n = jf_url_encode(b, sizeof(b), "");           EXPECT("empty", n, b, "");
    n = jf_url_encode(b, sizeof(b), "/?=+");       EXPECT("reserved", n, b, "%2F%3F%3D%2B");

    CHECK(jf_url_encode(b, 7, "ab c") == 6, "encode: exact fit (len 6, cap 7) refused");
    CHECK(jf_url_encode(b, 6, "ab c") == -1, "encode: cap == len accepted");
    CHECK(b[0] == '\0', "encode: overflow left text");
    CHECK(jf_url_encode(b, 3, "\xC3\xA9") == -1, "encode: 3-char escape into cap 3 accepted");
    CHECK(jf_url_encode(b, 0, "a") == -1, "encode: cap 0 accepted");
    CHECK(jf_url_encode(b, sizeof(b), NULL) == -1, "encode: NULL input accepted");
}

static void test_server(void)
{
    char h[JF_HOST_MAX];
    int p;
    bool t;

/* `tls` starts opposite to the expectation, so a parser that never writes it fails. */
#define SRV_OK(in, host, port, want_tls) do { p = -1; t = !(want_tls); \
    CHECK(jf_parse_server(in, h, sizeof(h), &p, &t) == JF_SERVER_OK, "\"%s\" refused", in); \
    CHECK(strcmp(h, host) == 0, "\"%s\": host \"%s\"", in, h); \
    CHECK(p == port, "\"%s\": port %d", in, p); \
    CHECK(t == (want_tls), "\"%s\": tls %d", in, (int)t); } while (0)
/* The port and tls must stay untouched on failure: the caller keeps its previous setting. */
#define SRV_NO(in, res) do { p = -1; t = true; \
    CHECK(jf_parse_server(in, h, sizeof(h), &p, &t) == (res), "\"%s\": wrong verdict", in); \
    CHECK(p == -1, "\"%s\": port written on failure", in); \
    CHECK(t, "\"%s\": tls written on failure", in); } while (0)

    /* scheme given: https is TLS on 443; http is plain only to a private IPv4 literal */
    SRV_OK("https://10.0.0.5", "10.0.0.5", 443, true);
    SRV_OK("https://8.8.8.8", "8.8.8.8", 443, true);
    SRV_OK("https://nas.example.com", "nas.example.com", 443, true);
    SRV_OK("https://nas.example.com:8443/", "nas.example.com", 8443, true);
    SRV_OK("http://192.168.1.5", "192.168.1.5", 8096, false);
    SRV_OK("http://192.168.1.5:9000/", "192.168.1.5", 9000, false);
    SRV_OK("http://10.0.0.1/", "10.0.0.1", 8096, false);
    SRV_OK("http://172.16.0.1", "172.16.0.1", 8096, false);
    SRV_OK("http://192.168.7.7/", "192.168.7.7", 8096, false);
    SRV_NO("http://8.8.8.8", JF_SERVER_INSECURE);
    SRV_NO("http://example.com", JF_SERVER_INSECURE);
    SRV_NO("http://example.com:8096", JF_SERVER_INSECURE);
    SRV_NO("http://172.15.0.1", JF_SERVER_INSECURE);
    SRV_NO("http://172.32.0.1", JF_SERVER_INSECURE);
    SRV_NO("http://192.169.0.1", JF_SERVER_INSECURE);
    SRV_NO("http://11.0.0.1", JF_SERVER_INSECURE);
    SRV_NO("http://192.168.1.5.evil.com", JF_SERVER_INSECURE);
    SRV_NO("http://10.example.com", JF_SERVER_INSECURE);
    SRV_NO("http://256.1.1.1", JF_SERVER_INSECURE);
    SRV_NO("http://10.0.0", JF_SERVER_INSECURE);

    /* no scheme: a private IPv4 literal is http on 8096; a hostname or public IP is https on 443 */
    SRV_OK("10.0.0.1", "10.0.0.1", 8096, false);
    SRV_OK("172.31.255.255", "172.31.255.255", 8096, false);
    SRV_OK("10.255.255.255", "10.255.255.255", 8096, false);
    SRV_OK("192.168.0.1:80", "192.168.0.1", 80, false);
    SRV_OK("1.2.3.4", "1.2.3.4", 443, true);
    SRV_OK("172.15.0.1", "172.15.0.1", 443, true);
    SRV_OK("172.32.0.1", "172.32.0.1", 443, true);
    SRV_OK("11.0.0.1", "11.0.0.1", 443, true);
    SRV_OK("192.168.1.5.evil.com", "192.168.1.5.evil.com", 443, true);
    SRV_OK("10.example.com", "10.example.com", 443, true);
    SRV_OK("256.1.1.1", "256.1.1.1", 443, true);
    SRV_OK("10.0.0", "10.0.0", 443, true);
    SRV_OK("10.0.0.1.5", "10.0.0.1.5", 443, true);
    SRV_OK("example.com", "example.com", 443, true);
    SRV_OK("example.com:8443", "example.com", 8443, true);
    SRV_OK("host:8096", "host", 8096, true);
    SRV_OK("my-host_1.lan:80//", "my-host_1.lan", 80, true);
    SRV_OK("h:1", "h", 1, true);
    SRV_OK("h:65535", "h", 65535, true);

    SRV_NO("", JF_SERVER_EMPTY);
    SRV_NO("http://", JF_SERVER_EMPTY);
    SRV_NO("https://", JF_SERVER_EMPTY);
    SRV_NO("a\rb", JF_SERVER_BAD);
    SRV_NO("a\nb", JF_SERVER_BAD);
    SRV_NO("a b", JF_SERVER_BAD);
    SRV_NO("a\tb", JF_SERVER_BAD);
    SRV_NO("h:0", JF_SERVER_BAD);
    SRV_NO("h:65536", JF_SERVER_BAD);
    SRV_NO("h:", JF_SERVER_BAD);
    SRV_NO("h:80x", JF_SERVER_BAD);
    SRV_NO("h:123456", JF_SERVER_BAD);
    SRV_NO(":80", JF_SERVER_BAD);
    SRV_NO("h:80:90", JF_SERVER_BAD);
    SRV_NO("https://h:0", JF_SERVER_BAD);

    char small[4];
    CHECK(jf_parse_server("abc", small, sizeof(small), &p, &t) == JF_SERVER_OK, "host fitting cap refused");
    CHECK(jf_parse_server("abcd", small, sizeof(small), &p, &t) == JF_SERVER_BAD, "host longer than cap accepted");
    CHECK(jf_parse_server("abcd:80", small, sizeof(small), &p, &t) == JF_SERVER_BAD, "host+port longer than cap accepted");
}

static void test_header(void)
{
    char b[256], want[256];
    int n;
    const char *base = "Authorization: MediaBrowser Client=\"Arca\", Device=\"RW\", DeviceId=\"d1\", Version=\"1\"";

    n = jf_auth_header(b, sizeof(b), "RW", "d1", NULL);
    CHECK(n > 0 && strstr(b, "Token=") == NULL, "NULL token left a Token: %s", b);
    snprintf(want, sizeof(want), "%s\r\n", base);
    EXPECT("header no token", n, b, want);

    n = jf_auth_header(b, sizeof(b), "RW", "d1", "");
    CHECK(n > 0 && strstr(b, "Token=") == NULL, "empty token left a Token: %s", b);

    n = jf_auth_header(b, sizeof(b), "RW", "d1", "tok123");
    snprintf(want, sizeof(want), "%s, Token=\"tok123\"\r\n", base);
    EXPECT("header token", n, b, want);

    CHECK(jf_auth_header(b, sizeof(b), "RW", "", NULL) == -1, "empty device_id (the server answers 500)");
    CHECK(jf_auth_header(b, sizeof(b), "", "d1", NULL) == -1, "empty device");
    CHECK(jf_auth_header(b, sizeof(b), "R\"W", "d1", NULL) == -1, "quote in device");
    CHECK(jf_auth_header(b, sizeof(b), "RW", "d\\1", NULL) == -1, "backslash in device_id");
    CHECK(jf_auth_header(b, sizeof(b), "R\rW", "d1", NULL) == -1, "CR in device");
    CHECK(jf_auth_header(b, sizeof(b), "RW", "d\n1", NULL) == -1, "LF in device_id");
    CHECK(jf_auth_header(b, sizeof(b), "RW", "d1", "t\"k") == -1, "quote in token");
    CHECK(jf_auth_header(b, sizeof(b), "RW", "d1", "t\\k") == -1, "backslash in token");
    CHECK(jf_auth_header(b, sizeof(b), "RW", "d1", "t\rk") == -1, "CR in token");
    CHECK(jf_auth_header(b, sizeof(b), "RW", "d1", "t\nk") == -1, "LF in token");
    CHECK(jf_auth_header(b, sizeof(b), NULL, "d1", NULL) == -1, "NULL device");

    CHECK(jf_auth_header(b, (size_t)n + 1, "RW", "d1", "tok123") == n, "header cap=len+1 refused");
    CHECK(jf_auth_header(b, (size_t)n, "RW", "d1", "tok123") == -1, "header cap=len accepted");
}

static void test_body(void)
{
    char b[128];
    int n;

    n = jf_auth_body(b, sizeof(b), "bob", "pw");
    EXPECT("plain body", n, b, "{\"Username\":\"bob\",\"Pw\":\"pw\"}");

    n = jf_auth_body(b, sizeof(b), "bob", "a\"b\\c");
    EXPECT("quote+backslash", n, b, "{\"Username\":\"bob\",\"Pw\":\"a\\\"b\\\\c\"}");

    n = jf_auth_body(b, sizeof(b), "u\"v", "x\ny\t\x01z");
    EXPECT("controls", n, b, "{\"Username\":\"u\\\"v\",\"Pw\":\"x\\u000ay\\u0009\\u0001z\"}");

    n = jf_auth_body(b, sizeof(b), "", "");
    EXPECT("empty fields", n, b, "{\"Username\":\"\",\"Pw\":\"\"}");

    n = jf_auth_body(b, sizeof(b), "bob", "pw");
    CHECK(jf_auth_body(b, (size_t)n + 1, "bob", "pw") == n, "body cap=len+1 refused");
    /* every shorter cap must fail and leave an empty string: the caller wipes it */
    for (size_t cap = 1; cap <= (size_t)n; cap++) {
        memset(b, 'X', sizeof(b));
        CHECK(jf_auth_body(b, cap, "bob", "pw") == -1, "body cap=%d accepted", (int)cap);
        CHECK(b[0] == '\0', "body cap=%d: out[0] not NUL", (int)cap);
    }
    CHECK(jf_auth_body(b, 0, "bob", "pw") == -1, "body cap 0 accepted");
    CHECK(jf_auth_body(b, sizeof(b), NULL, "pw") == -1, "NULL user accepted");
}

static void test_mix_and_save(void)
{
    char b[8192];
    int n = jf_path_instant_mix(b, sizeof(b), UID, ALB, 50);
    EXPECT("instant mix", n, b, "/Items/" ALB "/InstantMix?userId=" UID "&Limit=50&EnableImages=false&EnableUserData=false");
    CHECK(jf_path_instant_mix(b, sizeof(b), "short", ALB, 50) == -1, "bad uid accepted");
    CHECK(jf_path_instant_mix(b, sizeof(b), UID, "../x", 50) == -1, "bad track id accepted");
    CHECK(jf_path_instant_mix(b, sizeof(b), UID, NULL, 50) == -1, "NULL track id accepted");
    CAP_EDGE("instant mix", jf_path_instant_mix(b_, cap, UID, ALB, 50));

    n = jf_path_playlist_create(b, sizeof(b));
    EXPECT("playlist create", n, b, "/Playlists");
    CHECK(jf_path_playlist_create(b, 11) == 10, "create cap=len+1 refused");
    CHECK(jf_path_playlist_create(b, 10) == -1, "create cap=len accepted");

    const char *ids[JF_SAVE_IDS_MAX + 1];
    for (int i = 0; i <= JF_SAVE_IDS_MAX; i++) ids[i] = (i & 1) ? ART : ALB;

    n = jf_save_body(b, sizeof(b), "Mix", UID, ids, 2);
    EXPECT("save body", n, b, "{\"Name\":\"Mix\",\"Ids\":[\"" ALB "\",\"" ART "\"],\"UserId\":\"" UID
                              "\",\"MediaType\":\"Audio\"}");
    n = jf_save_body(b, sizeof(b), "a\"b\134c \303\251", UID, ids, 1);
    EXPECT("name escaping", n, b, "{\"Name\":\"a\134\"b\134\134c \303\251\",\"Ids\":[\"" ALB "\"],\"UserId\":\""
                                  UID "\",\"MediaType\":\"Audio\"}");
    n = jf_save_body(b, sizeof(b), "x\ny", UID, ids, 1);
    CHECK(n > 0 && strstr(b, "x\134u000ay") != NULL && strchr(b, '\n') == NULL, "newline in name not escaped");
    n = jf_save_body(b, sizeof(b), "", UID, ids, 1);
    CHECK(n > 0 && strstr(b, "\"Name\":\"\",") != NULL, "empty name refused");

    n = jf_save_body(b, sizeof(b), "Max", UID, ids, JF_SAVE_IDS_MAX);
    CHECK(n > 0 && n < 4608, "100-id body: %d bytes", n);
    CHECK(strstr(b, "\"Ids\":[\"" ALB "\",\"" ART "\",\"" ALB "\"") != NULL, "ids out of order");
    CHECK(b[n - 1] == '}' && strstr(b, "\"],\"UserId\"") != NULL, "100-id body not closed");
    CHECK(jf_save_body(b, sizeof(b), "Max", UID, ids, JF_SAVE_IDS_MAX + 1) == -1, "101 ids accepted");
    CHECK(jf_save_body(b, sizeof(b), "Max", UID, ids, 0) == -1, "0 ids accepted");
    CHECK(jf_save_body(b, sizeof(b), "Max", UID, ids, -1) == -1, "-1 ids accepted");
    CHECK(jf_save_body(b, sizeof(b), "Max", UID, NULL, 1) == -1, "NULL ids accepted");
    CHECK(jf_save_body(b, sizeof(b), NULL, UID, ids, 1) == -1, "NULL name accepted");
    CHECK(jf_save_body(b, sizeof(b), "Max", "short", ids, 1) == -1, "bad uid accepted");
    CHECK(jf_save_body(b, sizeof(b), "Max", NULL, ids, 1) == -1, "NULL uid accepted");
    const char *bad[] = { ALB, "not-an-id", ART };
    CHECK(jf_save_body(b, sizeof(b), "Max", UID, bad, 3) == -1, "bad id in the middle accepted");
    CHECK(b[0] == '\0', "refused body not empty");
    const char *nul[] = { ALB, NULL };
    CHECK(jf_save_body(b, sizeof(b), "Max", UID, nul, 2) == -1, "NULL id accepted");

    /* every shorter cap fails and leaves an empty string (the name is the user's, but the caller wipes anyway) */
    n = jf_save_body(b, sizeof(b), "Mix", UID, ids, 3);
    CHECK(jf_save_body(b, (size_t)n + 1, "Mix", UID, ids, 3) == n, "save cap=len+1 refused");
    for (size_t cap = 1; cap <= (size_t)n; cap++) {
        char c[8192];
        memset(c, 'X', sizeof(c));
        CHECK(jf_save_body(c, cap, "Mix", UID, ids, 3) == -1, "save cap=%d accepted", (int)cap);
        CHECK(c[0] == '\0', "save cap=%d: out[0] not NUL", (int)cap);
    }
    CHECK(jf_save_body(b, 0, "Mix", UID, ids, 1) == -1, "save cap 0 accepted");
}

/* MIX: the seed lookup, the genre index and the candidate pool */
static void test_mix_paths(void)
{
    char b[1024];
    int n = jf_path_item_by_id(b, sizeof(b), UID, ART);
    EXPECT("item by id", n, b, "/Items?userId=" UID "&Ids=" ART "&Fields=Genres&EnableImages=false&EnableUserData=false");
    CHECK(jf_path_item_by_id(b, sizeof(b), "short", ART) == -1, "bad uid accepted");
    CHECK(jf_path_item_by_id(b, sizeof(b), UID, "../x") == -1, "bad item id accepted");
    CHECK(jf_path_item_by_id(b, sizeof(b), UID, NULL) == -1, "NULL item id accepted");
    CAP_EDGE("item by id", jf_path_item_by_id(b_, cap, UID, ART));

    n = jf_path_genre_index(b, sizeof(b), UID, 200, 100);
    EXPECT("genre index", n, b, "/MusicGenres?userId=" UID "&StartIndex=200&Limit=100&Fields=ItemCounts&EnableImages=false&EnableUserData=false");
    CHECK(jf_path_genre_index(b, sizeof(b), "short", 0, 100) == -1, "genre index: bad uid accepted");
    CAP_EDGE("genre index", jf_path_genre_index(b_, cap, UID, 0, 100));

    const char *g1[] = { ART };
    const char *g2[] = { ART, ALB };
    const char *g3[] = { ART, ALB, SES };
    const char *g4[] = { ART, ALB, SES, UID };

    n = jf_path_mix_pool(b, sizeof(b), UID, g1, 1, NULL, 0, true, 0, 40);
    EXPECT("pool, one genre, random", n, b, "/Items?userId=" UID "&IncludeItemTypes=Audio&Recursive=true&GenreIds=" ART
           "&SortBy=Random&StartIndex=0&Limit=40&Fields=Genres&EnableImages=false&EnableUserData=false");
    n = jf_path_mix_pool(b, sizeof(b), UID, g3, 3, "", 0, false, 80, 40);
    EXPECT("pool, three genres, stable, empty artist", n, b, "/Items?userId=" UID "&IncludeItemTypes=Audio&Recursive=true&GenreIds="
           ART "," ALB "," SES "&SortBy=SortName&StartIndex=80&Limit=40&Fields=Genres&EnableImages=false&EnableUserData=false");
    CHECK(strchr(b, '|') == NULL && !strstr(b, "%7C"), "no '|' joiner");
    CHECK(!strstr(b, "ArtistIds") && !strstr(b, "Years"), "artist and decade absent when not asked");

    n = jf_path_mix_pool(b, sizeof(b), UID, g1, 1, ALB, 1980, true, 0, 40);
    EXPECT("pool, genre + artist + decade", n, b, "/Items?userId=" UID "&IncludeItemTypes=Audio&Recursive=true&GenreIds=" ART
           "&ArtistIds=" ALB "&Years=1980,1981,1982,1983,1984,1985,1986,1987,1988,1989"
           "&SortBy=Random&StartIndex=0&Limit=40&Fields=Genres&EnableImages=false&EnableUserData=false");
    n = jf_path_mix_pool(b, sizeof(b), UID, NULL, 0, ALB, 0, true, 0, 40);
    EXPECT("pool, artist only", n, b, "/Items?userId=" UID "&IncludeItemTypes=Audio&Recursive=true&ArtistIds=" ALB
           "&SortBy=Random&StartIndex=0&Limit=40&Fields=Genres&EnableImages=false&EnableUserData=false");
    n = jf_path_mix_pool(b, sizeof(b), UID, NULL, 0, NULL, 2000, false, 40, 40);
    EXPECT("pool, decade only", n, b, "/Items?userId=" UID "&IncludeItemTypes=Audio&Recursive=true"
           "&Years=2000,2001,2002,2003,2004,2005,2006,2007,2008,2009"
           "&SortBy=SortName&StartIndex=40&Limit=40&Fields=Genres&EnableImages=false&EnableUserData=false");

    /* length budget: 4 ids + artist + decade + a large start must fit what jf_api sends, or the MIX request is refused at enqueue */
    n = jf_path_mix_pool(b, sizeof(b), UID, g4, 4, ART, 1980, false, 9999, 40);
    CHECK(n > 0 && n < JF_API_PATH_MAX, "4 ids + artist + decade + start 9999 fits JF_API_PATH_MAX (got %d)", n);
    int n2 = jf_path_mix_pool(b + 600, 400, UID, g2, 2, ART, 1980, false, 9999, 40);
    CHECK(n2 > 0 && n2 < JF_API_PATH_MAX, "2 ids + artist + decade + start 9999 fits JF_API_PATH_MAX (got %d)", n2);
    CHECK(strstr(b, "GenreIds=" ART "," ALB "," SES "," UID "&ArtistIds=") != NULL, "four ids comma-joined, in order");

    /* refusals */
    const char *bad1[] = { ART, "../x" };
    const char *bad2[] = { ART, NULL };
    CHECK(jf_path_mix_pool(b, sizeof(b), "short", g1, 1, NULL, 0, true, 0, 40) == -1, "bad uid accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, bad1, 2, NULL, 0, true, 0, 40) == -1, "bad genre id accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, bad2, 2, NULL, 0, true, 0, 40) == -1, "NULL genre id accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, g1, -1, NULL, 0, true, 0, 40) == -1, "negative id count accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, g1, 1, "nothex", 0, true, 0, 40) == -1, "bad artist id accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, g1, 1, NULL, 1985, true, 0, 40) == -1, "decade 1985 (not a multiple of 10) accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, g1, 1, NULL, 990, true, 0, 40) == -1, "decade 990 accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, g1, 1, NULL, 10000, true, 0, 40) == -1, "decade 10000 accepted");
    CHECK(jf_path_mix_pool(b, sizeof(b), UID, g1, 1, NULL, -1980, true, 0, 40) == -1, "negative decade accepted");

    /* every cap short of the length fails, for each shape */
    int shapes[3] = { 1, 2, 3 };
    for (int s = 0; s < 3; s++) {
        const char *art = shapes[s] >= 2 ? ALB : NULL;
        int dec = shapes[s] >= 3 ? 1980 : 0;
        int len = jf_path_mix_pool(b, sizeof(b), UID, g3, 3, art, dec, true, 0, 40);
        CHECK(len > 0, "shape %d baseline", s);
        int wrong = 0;
        for (size_t cap = 0; cap <= (size_t)len; cap++) {
            char c[1024];
            memset(c, 'X', sizeof c);
            if (jf_path_mix_pool(c, cap, UID, g3, 3, art, dec, true, 0, 40) != -1) wrong++;
        }
        CHECK(wrong == 0, "shape %d: %d caps <= len accepted", s, wrong);
        char c[1024];
        CHECK(jf_path_mix_pool(c, (size_t)len + 1, UID, g3, 3, art, dec, true, 0, 40) == len, "shape %d: cap=len+1 refused", s);
    }
}

int main(void)
{
    test_builders();
    test_ids();
    test_session();
    test_encode();
    test_server();
    test_header();
    test_body();
    test_mix_and_save();
    test_mix_paths();
    printf("jf_url_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
