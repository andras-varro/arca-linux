#!/bin/bash
#
# Measure native_apps/tests/http_tls_test.c against deliberately broken COPIES of
# native_apps/common/http.c and print, per sabotage, the failure count and the failing checks.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && bash tests/measure_http_tls_sabotage.sh"
#
# The shipped http.c is never edited: each case copies it (and http.h) into a mktemp directory,
# edits the copy with sed, and builds the test against that directory, so there is nothing to
# restore (never `git checkout` a sabotage: it would destroy the fix under test).  A sabotage that
# changed nothing is a harness error and fails the run, because "0 failed" from an edit that did
# not apply reads exactly like a suite that cannot see the breakage.  Every case must fail.
#
# Case e also lowers the client's security level to 0: this host's distro OpenSSL already refuses
# TLS 1.1 at its default level, so deleting the min-version line alone leaves the TLS 1.1 test
# green (measured: 0 failed) and says nothing about the floor.  With the level lowered, the line is
# the only thing between the client and the old server.
#
# There is no pre-change case: the pre-TLS http_begin() has no `tls` argument, so the test does not
# even compile against it, and a compile error is not a measurement.

set -u
cd "$(dirname "$0")/../native_apps" || exit 1

SAB="$(mktemp -d /tmp/arca-http-tls-sab.XXXXXX)"
trap 'rm -rf "$SAB"' EXIT INT TERM
HARNESS_ERR=0
SRC=common/http.c

build_run() { # <dir with http.c/http.h>: prints the test's output
    gcc -Wall -Wextra -Wno-unused-parameter -I "$1" -o "$SAB/t" tests/http_tls_test.c "$1/http.c" \
        -lssl -lcrypto 2>"$SAB/cc.log" || { echo "BUILD FAILED: $(head -3 "$SAB/cc.log")"; return; }
    TMPDIR="$SAB" timeout 120 "$SAB/t" 2>&1
}

case_run() { # <label> <sed expression>
    rm -rf "$SAB/c"; mkdir "$SAB/c"
    cp "$SRC" "$SAB/c/http.c"; cp common/http.h "$SAB/c/http.h"
    sed -i "$2" "$SAB/c/http.c"
    if cmp -s "$SAB/c/http.c" "$SRC"; then
        printf '  %-52s SABOTAGE DID NOT APPLY\n' "$1"; HARNESS_ERR=1; return
    fi
    local out
    out="$(build_run "$SAB/c")"
    printf '  %-52s %s   lines:%s\n' "$1" "$(echo "$out" | grep -oE '[0-9]+ checks, [0-9]+ failed|BUILD FAILED')" \
        "$(echo "$out" | grep -oE 'http_tls_test.c:[0-9]+' | cut -d: -f2 | tr '\n' ' ')"
    echo "$out" | grep -q 'BUILD FAILED' && HARNESS_ERR=1
}

rm -rf "$SAB/base"; mkdir "$SAB/base"; cp "$SRC" "$SAB/base/http.c"; cp common/http.h "$SAB/base/http.h"
printf '  %-52s %s\n' "baseline (nothing broken)" "$(build_run "$SAB/base" | grep -oE '[0-9]+ checks, [0-9]+ failed|BUILD FAILED')"

case_run "a. verification off (SSL_VERIFY_NONE)" 's/SSL_VERIFY_PEER/SSL_VERIFY_NONE/'
case_run "b. no host / IP check (SSL_set1_host, set1_ip_asc)" \
    's/ok = X509_VERIFY_PARAM_set1_ip_asc(vp, host);/ok = 1;/; s/ok = SSL_set1_host(s, host);/ok = 1;/'
case_run "c. close without close_notify is an error" \
    's/if (e == SSL_ERROR_SYSCALL \&\& k == 0 \&\& ERR_peek_error() == 0) return 0;/(void)0;/; s/SSL_R_UNEXPECTED_EOF_WHILE_READING)$/SSL_R_UNEXPECTED_EOF_WHILE_READING \&\& 0)/; s/SSL_CTX_set_options(c, SSL_OP_IGNORE_UNEXPECTED_EOF);/(void)0;/'
case_run "d. http_stream_read always plain read()" 's/if (i < 0) return read(fd, buf, n);/return read(fd, buf, n);/'
case_run "e. no TLS 1.2 floor (min protocol version)" 's/SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);/SSL_CTX_set_security_level(c, 0);/'

[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR"; exit 1; }
