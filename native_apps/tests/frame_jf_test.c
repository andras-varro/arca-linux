/*
 * frame_jf_test.c — host regression for frame/frame_jf.c (Jellyfin photo paths, library key, reply parsers).
 *
 * Build: gcc -Wall -Wextra -I common -I frame -I jf_shared -fsanitize=undefined,address tests/frame_jf_test.c frame/frame_jf.c jf_shared/jf_url.c common/json.c -o frame_jf_test
 */
#include <stdio.h>
#include <string.h>

#include "frame_jf.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define UID "0123456789abcdef0123456789ABCDEF"
#define PID "fedcba9876543210fedcba9876543210"
#define IDU "ABCDEF0123456789ABCDEF0123456789"

static void test_paths(void)
{
    char b[400];
    int n = fj_path_views(b, sizeof b, UID);
    CHECK(n == (int)strlen(b) && !strcmp(b, "/Users/" UID "/Views"), "views: %s", b);
    CHECK(fj_path_views(b, sizeof b, "bad") == -1, "views bad uid");
    CHECK(fj_path_views(b, sizeof b, "0123456789abcdef0123456789abcd/f") == -1, "views uid with slash");

    n = fj_path_children(b, sizeof b, UID, PID, 20, 50);
    CHECK(n == (int)strlen(b) && !strcmp(b, "/Users/" UID "/Items?ParentId=" PID "&IncludeItemTypes=PhotoAlbum,Folder,Photo"
          "&SortBy=SortName&SortOrder=Ascending&StartIndex=20&Limit=50"), "children: %s", b);
    n = fj_path_photos(b, sizeof b, UID, PID, 0, 100);
    CHECK(n == (int)strlen(b) && !strcmp(b, "/Users/" UID "/Items?ParentId=" PID "&Recursive=true&IncludeItemTypes=Photo"
          "&SortBy=SortName&SortOrder=Ascending&StartIndex=0&Limit=100"), "photos: %s", b);
    CHECK(fj_path_children(b, sizeof b, UID, "short", 0, 10) == -1, "children bad parent");
    CHECK(fj_path_children(b, sizeof b, "x/y", PID, 0, 10) == -1, "children bad uid");
    CHECK(fj_path_photos(b, sizeof b, UID, PID, -1, 10) == -1, "photos negative start");
    CHECK(fj_path_photos(b, sizeof b, UID, PID, 0, -5) == -1, "photos negative limit");
    CHECK(fj_path_photos(b, 40, UID, PID, 0, 10) == -1, "photos cap too small");
    CHECK(fj_path_children(b, 40, UID, PID, 0, 10) == -1, "children cap too small");

    n = fj_path_image(b, sizeof b, PID, 800, 480);
    CHECK(n == (int)strlen(b) && !strcmp(b, "/Items/" PID "/Images/Primary?fillWidth=800&fillHeight=480&quality=90&format=Jpg"), "image: %s", b);
    CHECK(fj_path_image(b, sizeof b, "0123456789abcdef0123456789abcde", 800, 480) == -1, "image 31-char id");
    CHECK(fj_path_image(b, 20, PID, 800, 480) == -1, "image cap");
    CHECK(fj_path_image(b, sizeof b, PID, 0, 480) == -1, "image zero width");

    n = fj_path_original(b, sizeof b, PID);
    CHECK(n == (int)strlen(b) && !strcmp(b, "/Items/" PID "/Download"), "original: %s", b);
    CHECK(fj_path_original(b, sizeof b, "") == -1 && fj_path_original(b, sizeof b, NULL) == -1, "original bad id");
    CHECK(fj_path_original(b, 10, PID) == -1, "original cap");
}

static void test_key(void)
{
    char k[40];
    CHECK(fj_key_from_id(k, sizeof k, IDU) == 39 && !strcmp(k, "jf-abcdef0123456789abcdef0123456789.jpg"), "key: %s", k);
    CHECK(fj_key_from_id(k, sizeof k, PID) == 39 && !strcmp(k, "jf-" PID ".jpg"), "lowercase id unchanged");
    CHECK(fj_key_from_id(k, sizeof k, "0123456789abcdef0123456789abcde") == -1, "key 31-char id");
    CHECK(fj_key_from_id(k, sizeof k, "0123456789abcdef0123456789abcdeg") == -1, "key non-hex id");
    CHECK(fj_key_from_id(k, 39, IDU) == -1, "key cap 39 (no room for NUL)");
}

#define J(items, tail) "{\"Items\":[" items "]" tail "}"
#define ITEM(id, name, type) "{\"Id\":\"" id "\",\"Name\":\"" name "\",\"Type\":\"" type "\",\"Extra\":{\"a\":[1,2]}}"

static void test_items(void)
{
    FjItem it[8];
    int total = -1;
    const char *j = J(ITEM(IDU, "Holiday", "PhotoAlbum") "," ITEM(PID, "a.jpg", "Photo") "," ITEM(UID, "Dir", "Folder") ","
                      ITEM(PID, "x", "Audio"), ",\"TotalRecordCount\":57,\"StartIndex\":0");
    int n = fj_parse_items(j, strlen(j), it, 8, &total);
    CHECK(n == 4 && total == 57, "n=%d total=%d", n, total);
    CHECK(it[0].kind == FJ_ALBUM && !strcmp(it[0].name, "Holiday") && !strcmp(it[0].id, IDU), "item 0");
    CHECK(it[1].kind == FJ_PHOTO && !strcmp(it[1].name, "a.jpg"), "item 1");
    CHECK(it[2].kind == FJ_ALBUM, "Folder is an album");
    CHECK(it[3].kind == FJ_OTHER, "unknown Type is other");

    j = J(ITEM(IDU, "A", "Photo"), "");
    CHECK(fj_parse_items(j, strlen(j), it, 8, &total) == 1 && total == 1, "TotalRecordCount absent: total=%d", total);
    CHECK(fj_parse_items(j, strlen(j), it, 8, NULL) == 1, "total NULL");

    j = J("", ",\"TotalRecordCount\":0");
    CHECK(fj_parse_items(j, strlen(j), it, 8, &total) == 0 && total == 0, "empty Items");

    j = J(ITEM("tooshort", "A", "Photo") "," ITEM(IDU, "B", "Photo"), "");
    CHECK(fj_parse_items(j, strlen(j), it, 8, &total) == 1 && !strcmp(it[0].name, "B") && total == 2, "invalid Id skipped");

    j = J("{\"Id\":\"" IDU "\",\"Type\":\"Photo\"}", "");
    CHECK(fj_parse_items(j, strlen(j), it, 8, NULL) == 1 && it[0].name[0] == '\0', "missing Name");

    j = J(ITEM(IDU, "1", "Photo") "," ITEM(IDU, "2", "Photo") "," ITEM(IDU, "3", "Photo"), ",\"TotalRecordCount\":3");
    CHECK(fj_parse_items(j, strlen(j), it, 2, &total) == 2 && total == 3, "more items than max");

    char big[1024], nm[201];
    memset(nm, 'n', 200); nm[200] = '\0';
    snprintf(big, sizeof big, J(ITEM(IDU, "%s", "Photo"), ""), nm);
    CHECK(fj_parse_items(big, strlen(big), it, 8, NULL) == 1 && strlen(it[0].name) == FJ_NAME_MAX - 1, "long name cut to %zu", strlen(it[0].name));

    j = J(ITEM(IDU, "A", "Photo"), "");
    for (size_t cut = 1; cut < strlen(j); cut += 7)
        CHECK(fj_parse_items(j, cut, it, 8, NULL) == -1, "truncated at %zu", cut);
    CHECK(fj_parse_items("not json", 8, it, 8, NULL) == -1, "non-JSON");
    CHECK(fj_parse_items("{\"Foo\":[]}", 10, it, 8, NULL) == -1, "no Items array");
    CHECK(fj_parse_items("[1,2]", 5, it, 8, NULL) == -1, "root not an object");
    CHECK(fj_parse_items(NULL, 0, it, 8, NULL) == -1, "NULL json");
}

static void test_libraries(void)
{
    FjItem it[8];
    const char *j = J("{\"Id\":\"" IDU "\",\"Name\":\"Pics\",\"CollectionType\":\"photos\"},"
                      "{\"Id\":\"" PID "\",\"Name\":\"Music\",\"CollectionType\":\"music\"},"
                      "{\"Id\":\"" UID "\",\"Name\":\"Clips\",\"CollectionType\":\"HomeVideos\"},"
                      "{\"Id\":\"" PID "\",\"Name\":\"Shows\",\"CollectionType\":\"PHOTOS\"},"
                      "{\"Id\":\"" PID "\",\"Name\":\"NoType\"},"
                      "{\"Id\":\"bad\",\"Name\":\"BadId\",\"CollectionType\":\"photos\"}", "");
    int n = fj_parse_libraries(j, strlen(j), it, 8);
    CHECK(n == 4, "libraries kept %d", n);
    CHECK(n == 4 && !strcmp(it[0].name, "Pics") && it[0].kind == FJ_ALBUM && !strcmp(it[1].name, "Clips") && !strcmp(it[2].name, "Shows") && !strcmp(it[3].name, "NoType"), "library names");
    CHECK(n == 4 && !it[0].probe && it[1].probe && !it[2].probe && it[3].probe, "probe: only photos-typed is trusted");
    CHECK(fj_parse_libraries(j, strlen(j), it, 2) == 2, "libraries capped by max");
    CHECK(fj_parse_libraries(j, strlen(j) - 5, it, 8) == -1, "libraries truncated");
    CHECK(fj_parse_libraries("garbage", 7, it, 8) == -1, "libraries non-JSON");
    CHECK(fj_parse_libraries("{\"Items\":[]}", 12, it, 8) == 0, "libraries empty");
}

static void test_loose(void)
{
    FjItem al[4], lib;
    memset(al, 0, sizeof al);
    memset(&lib, 0, sizeof lib);
    snprintf(lib.id, sizeof lib.id, "%s", IDU);
    snprintf(lib.name, sizeof lib.name, "Home Videos and Photos");
    lib.kind = FJ_ALBUM; lib.probe = 1;
    snprintf(al[0].id, sizeof al[0].id, "%s", PID); snprintf(al[0].name, sizeof al[0].name, "Trip");
    CHECK(fj_add_loose(al, 1, 4, &lib, 29) == 2, "loose album added");
    CHECK(!strcmp(al[0].name, "Home Videos and Photos") && al[0].loose && !al[0].probe && !strcmp(al[0].id, IDU) && !strcmp(al[1].name, "Trip") && !al[1].loose, "loose album first, named after the library");
    CHECK(fj_add_loose(al, 1, 4, &lib, 0) == 1, "no loose photo: no album");
    CHECK(fj_add_loose(al, 4, 4, &lib, 3) == 4, "no room: unchanged");
    char b[400];
    int n = fj_path_loose(b, sizeof b, UID, IDU, 0, 100);
    CHECK(n > 0 && strstr(b, "IncludeItemTypes=Photo") && !strstr(b, "Recursive") && strstr(b, "ParentId=" IDU), "loose path is not recursive");
    CHECK(fj_path_loose(b, sizeof b, UID, "bad", 0, 100) == -1, "loose path bad id");
}

int main(void)
{
    test_paths();
    test_key();
    test_items();
    test_libraries();
    test_loose();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
