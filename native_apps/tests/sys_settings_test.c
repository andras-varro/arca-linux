/* Host regression for the System page's pure logic (control_panel/sys_settings.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/sys_settings_test \
 *       tests/sys_settings_test.c control_panel/sys_settings.c && ./build/sys_settings_test
 *
 * A  sshd_config text -> mode: PasswordAuthentication yes/no, comments,
 *    '=' and mixed case, first occurrence wins, Match ends the global section,
 *    absent/hybrid/NULL are UNKNOWN;
 * B  days in month, including the century leap rules;
 * C  field stepping: wraps, the day clamp after a month or year change, the
 *    year limits;
 * D  the date -s string, and an invalid value formats to nothing;
 * E  rewriting PasswordAuthentication (absent, commented, duplicate, Match, CRLF);
 * F  the authorized_keys plausibility check;
 * G  the three modes as sshd_config rewrites, the off marker, hybrid/unknown reads;
 * H  root login (PermitRootLogin) read and rewrite;
 * I  the lockout guard over mode + root login + key + password;
 * J  the root shadow line and the hash check;
 * K  the host name check;
 * L  authorized_keys permission checks (ownership and mode for sshd StrictModes).
 */
#include "control_panel/sys_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void mode(const char *text, SshMode want, const char *what) {
    SshMode got = sys_ssh_mode(text);
    CHECK(got == want, "%s: mode %d, want %d", what, (int)got, (int)want);
}

int main(void) {
    mode("PermitRootLogin prohibit-password\nPasswordAuthentication no\n", SSH_MODE_KEY_ONLY, "key group");
    mode("PasswordAuthentication yes\nPermitRootLogin yes\n", SSH_MODE_PASSWORD, "password group");
    mode("#PasswordAuthentication no\nPasswordAuthentication yes\nPermitRootLogin yes\n", SSH_MODE_PASSWORD, "comment skipped");
    mode("  passwordauthentication=NO\r\n", SSH_MODE_KEY_ONLY, "case, '=', CRLF, indent");
    mode("PasswordAuthentication no\nPasswordAuthentication yes\n", SSH_MODE_KEY_ONLY, "first occurrence wins");
    mode("Port 22\nMatch User x\nPasswordAuthentication no\n", SSH_MODE_UNKNOWN, "Match ends the global section");
    mode("PasswordAuthentication yes\n", SSH_MODE_PASSWORD, "password mode does not need root login yes");
    mode("PasswordAuthentication yes\nPermitRootLogin prohibit-password\n", SSH_MODE_PASSWORD, "root login is a separate setting");
    mode("PermitRootLogin yes\n", SSH_MODE_UNKNOWN, "password directive absent");
    mode("", SSH_MODE_UNKNOWN, "empty");
    mode(NULL, SSH_MODE_UNKNOWN, "NULL");

    CHECK(sys_days_in_month(2024, 2) == 29, "2024 leap");
    CHECK(sys_days_in_month(2025, 2) == 28, "2025 not leap");
    CHECK(sys_days_in_month(2100, 2) == 28, "2100 not leap");
    CHECK(sys_days_in_month(2000, 2) == 29, "2000 leap");
    CHECK(sys_days_in_month(2026, 4) == 30 && sys_days_in_month(2026, 12) == 31, "30/31");
    CHECK(sys_days_in_month(2026, 0) == 0 && sys_days_in_month(2026, 13) == 0, "bad month");

    SysDateTime d = { 2026, 1, 31, 23, 59 };
    sys_dt_step(&d, SYS_DT_MON, +1);
    CHECK(d.mon == 2 && d.day == 28, "Jan 31 + month -> Feb 28, got %d-%d", d.mon, d.day);
    d = (SysDateTime){ 2028, 3, 31, 0, 0 };
    sys_dt_step(&d, SYS_DT_MON, -1);
    CHECK(d.mon == 2 && d.day == 29, "leap Mar 31 - month -> Feb 29");
    d = (SysDateTime){ 2028, 2, 29, 0, 0 };
    sys_dt_step(&d, SYS_DT_YEAR, +1);
    CHECK(d.year == 2029 && d.day == 28, "Feb 29 + year -> Feb 28");
    d = (SysDateTime){ 2026, 12, 1, 0, 0 };
    sys_dt_step(&d, SYS_DT_MON, +1);
    CHECK(d.mon == 1, "month wraps");
    d = (SysDateTime){ 2026, 2, 1, 0, 0 };
    sys_dt_step(&d, SYS_DT_DAY, -1);
    CHECK(d.day == 28, "day wraps to month end");
    d = (SysDateTime){ 2026, 2, 28, 23, 59 };
    sys_dt_step(&d, SYS_DT_HOUR, +1); sys_dt_step(&d, SYS_DT_MIN, +1);
    CHECK(d.hour == 0 && d.min == 0 && d.day == 28, "hour and minute wrap, day untouched");
    d.year = SYS_YEAR_MAX; sys_dt_step(&d, SYS_DT_YEAR, +1);
    CHECK(d.year == SYS_YEAR_MAX, "year clamps high");
    d.year = SYS_YEAR_MIN; sys_dt_step(&d, SYS_DT_YEAR, -1);
    CHECK(d.year == SYS_YEAR_MIN, "year clamps low");

    char b[32];
    d = (SysDateTime){ 2026, 10, 6, 9, 5 };
    sys_dt_format(&d, b, sizeof(b));
    CHECK(strcmp(b, "2026-10-06 09:05:00") == 0, "format \"%s\"", b);
    d = (SysDateTime){ 2026, 11, 31, 9, 5 };
    sys_dt_format(&d, b, sizeof(b));
    CHECK(b[0] == '\0', "invalid formats to nothing");

    CHECK(sys_tz_count() >= 10, "zone list size %d", sys_tz_count());
    CHECK(strcmp(sys_tz_name(0), "UTC") == 0, "first zone UTC");
    CHECK(sys_tz_name(-1) == NULL && sys_tz_name(sys_tz_count()) == NULL, "name out of range");
    CHECK(sys_tz_from_link("/usr/share/zoneinfo/America/Chicago") == 2, "chicago index");
    CHECK(sys_tz_from_link("../usr/share/zoneinfo/Europe/Budapest") == 7, "relative target");
    CHECK(sys_tz_from_link("/usr/share/zoneinfo/UTC") == 0, "UTC target");
    CHECK(sys_tz_from_link("/usr/share/zoneinfo/America/Chicago2") == -1, "suffix is not a match");
    CHECK(sys_tz_from_link("/usr/share/zoneinfo/Pacific/Fiji") == -1, "unlisted zone");
    CHECK(sys_tz_from_link("/etc/foo") == -1 && sys_tz_from_link(NULL) == -1, "not a zoneinfo link");
    CHECK(sys_tz_step(0, -1) == sys_tz_count() - 1, "step back wraps");
    CHECK(sys_tz_step(sys_tz_count() - 1, +1) == 0, "step forward wraps");
    CHECK(sys_tz_step(2, +1) == 3 && sys_tz_step(2, -1) == 1, "plain steps");
    CHECK(sys_tz_step(-1, +1) == 0 && sys_tz_step(-1, -1) == sys_tz_count() - 1, "unlisted start");
    for (int i = 0; i < sys_tz_count(); i++) {
        char l[96];
        snprintf(l, sizeof(l), "/usr/share/zoneinfo/%s", sys_tz_name(i));
        CHECK(sys_tz_from_link(l) == i, "round trip %d", i);
    }

    /* E  rewriting PasswordAuthentication */
    {
        char o[512];
        int n;
        n = sys_ssh_set_password_auth("PermitRootLogin yes\nPasswordAuthentication yes\nPubkeyAuthentication yes\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PermitRootLogin yes\nPasswordAuthentication no\nPubkeyAuthentication yes\n") == 0, "E1 active line replaced: [%s]", o);
        CHECK(n == (int)strlen(o), "E1 length");
        n = sys_ssh_set_password_auth("PermitRootLogin yes\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PermitRootLogin yes\nPasswordAuthentication no\n") == 0, "E2 absent: appended: [%s]", o);
        n = sys_ssh_set_password_auth("PermitRootLogin yes", true, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PermitRootLogin yes\nPasswordAuthentication yes\n") == 0, "E3 no trailing newline: [%s]", o);
        n = sys_ssh_set_password_auth("#PasswordAuthentication yes\nPort 22\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "#PasswordAuthentication yes\nPort 22\nPasswordAuthentication no\n") == 0, "E4 comment kept, directive added: [%s]", o);
        n = sys_ssh_set_password_auth("#PasswordAuthentication yes\nPasswordauthentication = yes\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "#PasswordAuthentication yes\nPasswordAuthentication no\n") == 0, "E5 case and '=' form replaced: [%s]", o);
        n = sys_ssh_set_password_auth("PasswordAuthentication yes\nPort 22\nPasswordAuthentication yes\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PasswordAuthentication no\nPort 22\nPasswordAuthentication no\n") == 0, "E6 duplicates all rewritten: [%s]", o);
        n = sys_ssh_set_password_auth("Port 22\nMatch User bob\n  PasswordAuthentication yes\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "Port 22\nPasswordAuthentication no\nMatch User bob\n  PasswordAuthentication yes\n") == 0, "E7 absent before Match: inserted before it, Match body untouched: [%s]", o);
        n = sys_ssh_set_password_auth("PasswordAuthentication yes\nMatch User bob\n  PasswordAuthentication yes\n", false, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PasswordAuthentication no\nMatch User bob\n  PasswordAuthentication yes\n") == 0, "E8 present before Match: only global edited: [%s]", o);
        n = sys_ssh_set_password_auth("Port 22\r\nPasswordAuthentication yes\r\n", false, o, sizeof(o));
        CHECK(n > 0 && strchr(o, '\r') == NULL && strstr(o, "PasswordAuthentication no\n") != NULL, "E9 CRLF in, LF out");
        n = sys_ssh_set_password_auth("", true, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PasswordAuthentication yes\n") == 0, "E10 empty text: [%s]", o);
        n = sys_ssh_set_password_auth("PermitRootLogin yes\nPasswordAuthentication yes\n", false, o, 10);
        CHECK(n == -1, "E11 too small refuses, got %d", n);
        CHECK(sys_ssh_set_password_auth(NULL, true, o, sizeof(o)) == -1, "E12 NULL");
        /* round trip through the reader */
        n = sys_ssh_set_password_auth("PermitRootLogin yes\nPasswordAuthentication yes\n", false, o, sizeof(o));
        CHECK(n > 0 && sys_ssh_mode(o) == SSH_MODE_KEY_ONLY, "E13 reads back KEY ONLY");
        char o2[512];
        n = sys_ssh_set_password_auth(o, true, o2, sizeof(o2));
        CHECK(n > 0 && sys_ssh_mode(o2) == SSH_MODE_PASSWORD, "E14 and back to PASSWORD");
        CHECK(strstr(o2, "PermitRootLogin yes") != NULL, "E15 PermitRootLogin kept");
    }

    /* F  authorized_keys lockout guard */
    {
        const char *k = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFooBarBazQuxQuuxFooBar user@host\n";
        CHECK(sys_authkeys_plausible(k), "F1 ed25519 line");
        CHECK(sys_authkeys_plausible("ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQClWVzG0lyP6XVhk6KVBMm6\n"), "F2 rsa, no comment");
        CHECK(sys_authkeys_plausible("# note\n\n  \ncommand=\"/bin/true\",no-pty ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTY= x\n"), "F3 options + ecdsa after comment");
        CHECK(sys_authkeys_plausible("sk-ssh-ed25519@openssh.com AAAAGnNrLXNzaC1lZDI1NTE5QG9wZW5zc2guY29tAAAA\r\n"), "F4 sk key, CRLF");
        CHECK(!sys_authkeys_plausible(""), "F5 empty");
        CHECK(!sys_authkeys_plausible("# ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQClWVzG0lyP6XVhk6KVBMm6\n"), "F6 commented key");
        CHECK(!sys_authkeys_plausible("ssh-rsa short\n"), "F7 blob too short");
        CHECK(!sys_authkeys_plausible("hello world this is not a key at all\n"), "F8 no key type");
        CHECK(!sys_authkeys_plausible("ssh-rsa\n"), "F9 type only");
        CHECK(!sys_authkeys_plausible(NULL), "F10 NULL");
    }

    /* G  the three modes, as sshd_config text */
    {
        char o[1024], o2[1024];
        int n;
        const char *base = "PermitRootLogin prohibit-password\nPubkeyAuthentication yes\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n";
        n = sys_ssh_set_mode(base, SSH_MODE_PASSWORD, o, sizeof(o));
        CHECK(n > 0 && sys_ssh_mode(o) == SSH_MODE_PASSWORD, "G1 key -> key+password reads back");
        CHECK(strstr(o, "PubkeyAuthentication yes") != NULL, "G2 key login stays on in a password mode");
        CHECK(strstr(o, "PermitRootLogin prohibit-password") != NULL, "G3 mode leaves root login alone");
        n = sys_ssh_set_mode(o, SSH_MODE_KEY_ONLY, o2, sizeof(o2));
        CHECK(n > 0 && sys_ssh_mode(o2) == SSH_MODE_KEY_ONLY && strcmp(o2, base) == 0, "G4 and back, text identical: [%s]", o2);
        /* a config that switched pubkey off must not survive a mode set */
        n = sys_ssh_set_mode("PubkeyAuthentication no\nPasswordAuthentication yes\n", SSH_MODE_PASSWORD, o, sizeof(o));
        CHECK(n > 0 && strstr(o, "PubkeyAuthentication yes") && !strstr(o, "PubkeyAuthentication no"), "G5 pubkey no is rewritten to yes: [%s]", o);
        /* no Pubkey line: not invented (sshd's default is yes) */
        n = sys_ssh_set_mode("Port 22\n", SSH_MODE_KEY_ONLY, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "Port 22\nPasswordAuthentication no\n") == 0, "G6 pubkey line not added: [%s]", o);
        /* Match body untouched */
        n = sys_ssh_set_mode("PasswordAuthentication no\nMatch User x\n  PubkeyAuthentication no\n", SSH_MODE_PASSWORD, o, sizeof(o));
        CHECK(n > 0 && strstr(o, "Match User x\n  PubkeyAuthentication no\n") != NULL && strncmp(o, "PasswordAuthentication yes\n", 27) == 0, "G7 Match block kept: [%s]", o);
        CHECK(sys_ssh_set_mode(base, SSH_MODE_OFF, o, sizeof(o)) == -1, "G8 OFF is not a config mode");
        CHECK(sys_ssh_set_mode(base, SSH_MODE_UNKNOWN, o, sizeof(o)) == -1, "G9 UNKNOWN refused");
        CHECK(sys_ssh_set_mode(base, SSH_MODE_PASSWORD, o, 20) == -1, "G10 too small");
        /* the reader: negative controls, hybrid and unknown configs */
        mode("PasswordAuthentication yes\nPubkeyAuthentication no\n", SSH_MODE_UNKNOWN, "G11 password only is not a mode");
        mode("PasswordAuthentication no\nPubkeyAuthentication no\n", SSH_MODE_UNKNOWN, "G12 nothing can log in");
        mode("PasswordAuthentication maybe\n", SSH_MODE_UNKNOWN, "G13 garbage value");
        mode("PasswordAuthentication yes\nPermitRootLogin prohibit-password\n", SSH_MODE_PASSWORD, "G14 password mode needs no root yes");
        mode("PubkeyAuthentication no\nPasswordAuthentication no\nPasswordAuthentication yes\n", SSH_MODE_UNKNOWN, "G15 pubkey off, first pw wins");
        CHECK(sys_ssh_effective(SSH_MODE_KEY_ONLY, true) == SSH_MODE_OFF, "G16 marker means OFF");
        CHECK(sys_ssh_effective(SSH_MODE_PASSWORD, true) == SSH_MODE_OFF, "G17 marker beats password");
        CHECK(sys_ssh_effective(SSH_MODE_UNKNOWN, true) == SSH_MODE_OFF, "G18 marker beats unknown");
        CHECK(sys_ssh_effective(SSH_MODE_PASSWORD, false) == SSH_MODE_PASSWORD, "G19 no marker, config shows");
        CHECK(sys_ssh_effective(SSH_MODE_UNKNOWN, false) == SSH_MODE_UNKNOWN, "G20 unknown stays unknown");
    }

    /* H  root login */
    {
        char o[1024];
        int n;
        CHECK(sys_ssh_root_login("PermitRootLogin prohibit-password\n") == SSH_ROOT_KEY, "H1 prohibit-password");
        CHECK(sys_ssh_root_login("PermitRootLogin without-password\n") == SSH_ROOT_KEY, "H2 old spelling");
        CHECK(sys_ssh_root_login("permitrootlogin = YES\n") == SSH_ROOT_PASSWORD, "H3 yes, case and '='");
        CHECK(sys_ssh_root_login("Port 22\n") == SSH_ROOT_KEY, "H4 absent: sshd default is key only");
        CHECK(sys_ssh_root_login("PermitRootLogin no\n") == SSH_ROOT_UNKNOWN, "H5 no is not ours");
        CHECK(sys_ssh_root_login("PermitRootLogin forced-commands-only\n") == SSH_ROOT_UNKNOWN, "H6 forced-commands");
        CHECK(sys_ssh_root_login("PermitRootLogin yes\nPermitRootLogin no\n") == SSH_ROOT_PASSWORD, "H7 first wins");
        CHECK(sys_ssh_root_login("Match User x\nPermitRootLogin yes\n") == SSH_ROOT_KEY, "H8 Match ends the global section");
        CHECK(sys_ssh_root_login(NULL) == SSH_ROOT_UNKNOWN, "H9 NULL");
        n = sys_ssh_set_root_login("PermitRootLogin prohibit-password\nPasswordAuthentication no\n", SSH_ROOT_PASSWORD, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PermitRootLogin yes\nPasswordAuthentication no\n") == 0, "H10 to yes: [%s]", o);
        n = sys_ssh_set_root_login("PermitRootLogin yes\n", SSH_ROOT_KEY, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "PermitRootLogin prohibit-password\n") == 0, "H11 to prohibit-password: [%s]", o);
        n = sys_ssh_set_root_login("Port 22\n", SSH_ROOT_PASSWORD, o, sizeof(o));
        CHECK(n > 0 && strcmp(o, "Port 22\nPermitRootLogin yes\n") == 0, "H12 absent: appended: [%s]", o);
        n = sys_ssh_set_root_login("PermitRootLogin yes\nPort 22\nPermitRootLogin yes\n", SSH_ROOT_KEY, o, sizeof(o));
        CHECK(n > 0 && sys_ssh_root_login(o) == SSH_ROOT_KEY && strstr(o, "yes") == NULL, "H13 duplicates all rewritten: [%s]", o);
        CHECK(sys_ssh_set_root_login("", SSH_ROOT_UNKNOWN, o, sizeof(o)) == -1, "H14 UNKNOWN refused");
        mode("PasswordAuthentication no\nPermitRootLogin yes\n", SSH_MODE_KEY_ONLY, "H15 mode reader ignores root login");
    }

    /* I  the lockout guard over a mode + root login + what exists
     *    (mode, root login, has_key, has_password; NULL = allowed) */
    {
        CHECK(sys_ssh_refuse(SSH_MODE_OFF, SSH_ROOT_KEY, false, false) == NULL, "I1 off needs nothing");
        CHECK(sys_ssh_refuse(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, true, false) == NULL, "I2 key only with a key");
        CHECK(sys_ssh_refuse(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, false, true) != NULL, "I3 key only without a key, password set");
        CHECK(sys_ssh_refuse(SSH_MODE_KEY_ONLY, SSH_ROOT_PASSWORD, false, true) != NULL, "I4 key only never reaches the password");
        CHECK(sys_ssh_refuse(SSH_MODE_PASSWORD, SSH_ROOT_PASSWORD, false, false) != NULL, "I5 password mode without a password");
        CHECK(sys_ssh_refuse(SSH_MODE_PASSWORD, SSH_ROOT_KEY, true, false) != NULL, "I6 password mode needs a password even with a key");
        CHECK(sys_ssh_refuse(SSH_MODE_PASSWORD, SSH_ROOT_PASSWORD, false, true) == NULL, "I7 password + root yes, no key: reachable");
        CHECK(sys_ssh_refuse(SSH_MODE_PASSWORD, SSH_ROOT_KEY, false, true) != NULL, "I8 password set but root key-only and no key: locked out");
        CHECK(sys_ssh_refuse(SSH_MODE_PASSWORD, SSH_ROOT_KEY, true, true) == NULL, "I9 password set, key present, root key-only");
        CHECK(sys_ssh_refuse(SSH_MODE_PASSWORD, SSH_ROOT_PASSWORD, true, true) == NULL, "I10 everything present");
        CHECK(sys_ssh_refuse(SSH_MODE_OFF, SSH_ROOT_PASSWORD, false, false) != NULL, "I11 root login yes needs a password even when off");
        CHECK(sys_ssh_refuse(SSH_MODE_KEY_ONLY, SSH_ROOT_PASSWORD, true, false) != NULL, "I12 root login yes needs a password");
        CHECK(sys_ssh_refuse(SSH_MODE_UNKNOWN, SSH_ROOT_KEY, true, true) != NULL, "I13 unknown mode refused");
        const char *why = sys_ssh_refuse(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, false, false);
        CHECK(why && strlen(why) > 0 && strlen(why) < 60, "I14 reason fits a status line");

        /* the fresh card: Key only, root key-only, no key, password just set */
        CHECK(sys_ssh_refuse_change(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, SSH_MODE_KEY_ONLY, SSH_ROOT_PASSWORD,
                                    false, true) == NULL, "I15 fresh card: root login allow is a way out");
        CHECK(sys_ssh_refuse_change(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, SSH_MODE_PASSWORD, SSH_ROOT_KEY,
                                    false, true) == NULL, "I16 fresh card: key + password is a way out");
        CHECK(sys_ssh_refuse_change(SSH_MODE_KEY_ONLY, SSH_ROOT_PASSWORD, SSH_MODE_PASSWORD, SSH_ROOT_PASSWORD,
                                    false, true) == NULL, "I17 then key + password opens it");
        CHECK(sys_ssh_refuse_change(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, SSH_MODE_PASSWORD, SSH_ROOT_KEY,
                                    false, false) != NULL, "I18 fresh card, no password: key + password still refused");
        CHECK(sys_ssh_refuse_change(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, SSH_MODE_KEY_ONLY, SSH_ROOT_PASSWORD,
                                    false, false) != NULL, "I19 fresh card, no password: root login allow still refused");
        /* control: from an open state the lockout still bites */
        CHECK(sys_ssh_refuse_change(SSH_MODE_PASSWORD, SSH_ROOT_PASSWORD, SSH_MODE_PASSWORD, SSH_ROOT_KEY,
                                    false, true) != NULL, "I20 open by password, no key: root key-only refused");
        CHECK(sys_ssh_refuse_change(SSH_MODE_PASSWORD, SSH_ROOT_PASSWORD, SSH_MODE_KEY_ONLY, SSH_ROOT_PASSWORD,
                                    false, true) != NULL, "I21 open by password, no key: key only refused");
        CHECK(sys_ssh_refuse_change(SSH_MODE_OFF, SSH_ROOT_KEY, SSH_MODE_KEY_ONLY, SSH_ROOT_KEY,
                                    false, false) == NULL, "I22 off to key only with no key: no worse than off");
        CHECK(sys_ssh_refuse_change(SSH_MODE_KEY_ONLY, SSH_ROOT_KEY, SSH_MODE_UNKNOWN, SSH_ROOT_KEY,
                                    false, true) != NULL, "I23 unknown target refused from a locked state");
    }

    /* J  the shadow file and the hash */
    {
        char o[512];
        int n;
        const char *sh = "root:*:::::::\ndaemon:*:::::::\nsshd:*:::::::\n";
        const char *hash = "$6$abcdefgh$0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./abcdefghijklmnopqrstuv";
        CHECK(!sys_shadow_root_has_password(sh), "J1 locked root is no password");
        CHECK(!sys_shadow_root_has_password("root::::::::\n"), "J2 empty field is no password");
        CHECK(!sys_shadow_root_has_password("root:!:::::::\n"), "J3 '!' is locked");
        CHECK(!sys_shadow_root_has_password("root:!$6$aaaaaaaa$bbbbbbbbbbbb:::::::\n"), "J4 locked hash is locked");
        CHECK(!sys_shadow_root_has_password("daemon:$6$aa$bbbbbbbbbbbbbb:::::::\n"), "J5 other users do not count");
        CHECK(!sys_shadow_root_has_password(NULL) && !sys_shadow_root_has_password(""), "J6 NULL, empty");
        CHECK(sys_shadow_root_has_password("daemon:*:::::::\nroot:$6$aa$bbbbbbbbbbbbbbbbbb:19000:0:99999:7:::\n"), "J7 hash set, not first line");
        CHECK(sys_hash_plausible(hash), "J8 sha512 crypt string");
        CHECK(sys_hash_plausible("$1$salt$abcdefghijklmnopqrstuv"), "J9 md5 crypt");
        CHECK(!sys_hash_plausible(""), "J10 empty");
        CHECK(!sys_hash_plausible("Password: "), "J11 a prompt is not a hash");
        CHECK(!sys_hash_plausible("$6$ab:cd$xxxxxxxxxxxxxxxxxxxxxx"), "J12 ':' would split the field");
        CHECK(!sys_hash_plausible("$6$abcd\n$xxxxxxxxxxxxxxxxxxxxxx"), "J13 newline would add a line");
        CHECK(!sys_hash_plausible("*"), "J14 locked marker is not a hash");
        CHECK(!sys_hash_plausible("$6$a"), "J15 too short");
        CHECK(!sys_hash_plausible(NULL), "J16 NULL");
        n = sys_shadow_set_root(sh, hash, o, sizeof(o));
        CHECK(n > 0 && n == (int)strlen(o), "J17 length");
        CHECK(sys_shadow_root_has_password(o), "J18 reads back as set");
        CHECK(strstr(o, "daemon:*:::::::\nsshd:*:::::::\n") != NULL, "J19 other lines byte-identical");
        CHECK(strncmp(o, "root:$6$abcdefgh$", 17) == 0 && strstr(o, "root:*") == NULL, "J20 root line replaced: %.30s", o);
        n = sys_shadow_set_root("daemon:*:::::::\nroot:*:19000:0:99999:7:::\n", hash, o, sizeof(o));
        CHECK(n > 0 && strstr(o, ":19000:0:99999:7:::\n") != NULL, "J21 later fields of the root line kept");
        CHECK(sys_shadow_set_root("daemon:*:::::::\n", hash, o, sizeof(o)) == -1, "J22 no root line: refused, nothing invented");
        CHECK(sys_shadow_set_root(sh, "Password:", o, sizeof(o)) == -1, "J23 implausible hash refused");
        CHECK(sys_shadow_set_root(sh, hash, o, 20) == -1, "J24 too small");
        CHECK(sys_shadow_set_root(NULL, hash, o, sizeof(o)) == -1, "J25 NULL");
        n = sys_shadow_set_root("root:*:::::::\r\n", hash, o, sizeof(o));
        CHECK(n > 0 && strchr(o, '\r') == NULL, "J26 CRLF dropped");
    }

    /* K  the host name, as set-hostname accepts it */
    CHECK(sys_hostname_valid("rw09") && sys_hostname_valid("a") && sys_hostname_valid("Arca-12-x"), "K1 plain names");
    CHECK(!sys_hostname_valid("") && !sys_hostname_valid(NULL), "K2 empty, NULL");
    CHECK(!sys_hostname_valid("rw09.local") && !sys_hostname_valid("a b") && !sys_hostname_valid("a_b"), "K3 dot, space, underscore");
    CHECK(!sys_hostname_valid("-rw") && !sys_hostname_valid("rw-"), "K4 hyphen at an end");
    CHECK(sys_hostname_valid("rw-09"), "K5 inner hyphen");
    {
        char l[80];
        memset(l, 'a', 63); l[63] = '\0';
        CHECK(sys_hostname_valid(l), "K6 63 characters");
        memset(l, 'a', 64); l[64] = '\0';
        CHECK(!sys_hostname_valid(l), "K7 64 characters");
    }
    CHECK(!sys_hostname_valid("r\xc3\xb6w"), "K8 non-ASCII");

    /* L  authorized_keys permission checks */
    {
        /* Create a temporary directory structure to test permissions */
        char tmpdir[256], home[256], sshdir[256], keyfile[256];
        char *tmpbase = getenv("TMPDIR");
        if (!tmpbase) tmpbase = "/tmp";
        snprintf(tmpdir, sizeof(tmpdir), "%s/sys_settings_test_XXXXXX", tmpbase);
        mkdtemp(tmpdir);

        snprintf(home, sizeof(home), "%s/root", tmpdir);
        snprintf(sshdir, sizeof(sshdir), "%s/.ssh", home);
        snprintf(keyfile, sizeof(keyfile), "%s/authorized_keys", sshdir);

        mkdir(home, 0755);
        mkdir(sshdir, 0755);

        /* Create authorized_keys file with good permissions */
        FILE *f = fopen(keyfile, "w");
        if (f) { fputs("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFoo\n", f); fclose(f); }
        chmod(home, 0700);
        chmod(sshdir, 0700);
        chmod(keyfile, 0600);
        chown(home, getuid(), -1);
        chown(sshdir, getuid(), -1);
        chown(keyfile, getuid(), -1);

        CHECK(sys_authkeys_safe(keyfile, getuid()), "L1 good perms: owned by uid, 0700/0700/0600");

        /* Test group-writable authorized_keys */
        chmod(keyfile, 0660);
        CHECK(!sys_authkeys_safe(keyfile, getuid()), "L2 group-writable file fails");
        chmod(keyfile, 0600);

        /* Test group-writable .ssh dir */
        chmod(sshdir, 0770);
        CHECK(!sys_authkeys_safe(keyfile, getuid()), "L3 group-writable .ssh dir fails");
        chmod(sshdir, 0700);

        /* Test group-writable home */
        chmod(home, 0770);
        CHECK(!sys_authkeys_safe(keyfile, getuid()), "L4 group-writable home fails");
        chmod(home, 0700);

        /* Test world-writable authorized_keys */
        chmod(keyfile, 0666);
        CHECK(!sys_authkeys_safe(keyfile, getuid()), "L5 world-writable file fails");
        chmod(keyfile, 0600);

        /* Cleanup */
        unlink(keyfile);
        rmdir(sshdir);
        rmdir(home);
        rmdir(tmpdir);
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("sys_settings_test: all passed\n");
    return 0;
}
