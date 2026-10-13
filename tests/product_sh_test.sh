#!/bin/bash
#
# product_sh_test.sh — regression for lib/product.sh
#
# Host-only, no device, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/product_sh_test.sh"
#
# WHAT IT COVERS (each case sources a COPY of the library from a throwaway tree whose
# rootfs/board/arca/os-release it controls, because the library finds that file from its
# own location):
#
#   A  the shipped os-release gives a non-empty PRODUCT_NAME and a PRODUCT_STEM that is
#      its first word, lowercase, [a-z0-9] only
#   B  quoting: double, single, unquoted, CRLF; PRETTY_NAME before NAME does not win
#   C  refusals: no file, no NAME= line, an empty NAME — return 1 with a message and
#      PRODUCT_* left empty (a host script must not name a file from a guess)
#   D  the file is parsed, never sourced: a line that would run a command does nothing
#
# The count at the end includes a floor: a file that silently ran zero cases reports
# success just as loudly as one that ran all of them.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
pass=0; fail=0
ok()  { pass=$((pass+1)); echo -e "  ${GREEN}PASS${NC}  $*"; }
bad() { fail=$((fail+1)); echo -e "  ${RED}FAIL${NC}  $*"; }

# fixture <content> -> prints a tree dir holding the library copy and that os-release
fixture() {
    local d; d="$(mktemp -d "$T/t.XXXXXX")"
    mkdir -p "$d/lib" "$d/rootfs/board/arca"
    cp "$REPO_DIR/lib/product.sh" "$d/lib/product.sh"
    [ -n "${1+x}" ] && printf '%b' "$1" > "$d/rootfs/board/arca/os-release"
    printf '%s' "$d"
}

# run <tree> -> "rc|NAME|STEM" from a fresh shell that sources the tree's copy
run() {
    bash -c '. "$1/lib/product.sh" 2>"$1/err"; rc=$?; printf "%s|%s|%s" "$rc" "${PRODUCT_NAME-}" "${PRODUCT_STEM-}"' _ "$1"
}

expect() { # what | tree | want
    local got; got="$(run "$2")"
    if [ "$got" = "$3" ]; then ok "$1"; else bad "$1 — got [$got], want [$3]"; fi
}

# A — the shipped file
got="$(bash -c '. "$1/lib/product.sh"; printf "%s|%s" "$PRODUCT_NAME" "$PRODUCT_STEM"' _ "$REPO_DIR")"
name="${got%%|*}"; stem="${got#*|}"
if [ -n "$name" ] && [ -n "$stem" ]; then ok "A1 shipped os-release gives [$name] / [$stem]"; else bad "A1 shipped os-release gave [$got]"; fi
first="$(printf '%s' "${name%% *}" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9')"
if [ "$stem" = "$first" ]; then ok "A2 the stem is the name's first word, lowercased"; else bad "A2 stem [$stem] != [$first]"; fi
case "$stem" in *[!a-z0-9]*) bad "A3 stem [$stem] has a character outside [a-z0-9]" ;; *) ok "A3 stem is [a-z0-9] only" ;; esac

# B — quoting
expect "B1 double quoted"        "$(fixture 'NAME="Foo Bar"\n')"                       "0|Foo Bar|foo"
expect "B2 single quoted"        "$(fixture "NAME='Foo Bar'\n")"                      "0|Foo Bar|foo"
expect "B3 unquoted"             "$(fixture 'NAME=Plain\n')"                           "0|Plain|plain"
expect "B4 CRLF"                 "$(fixture 'NAME="Crlf Os"\r\n')"                     "0|Crlf Os|crlf"
expect "B5 PRETTY_NAME first"    "$(fixture 'PRETTY_NAME="Wrong"\nNAME="Right Os"\n')" "0|Right Os|right"
expect "B6 punctuation in stem"  "$(fixture 'NAME="A-B! C"\n')"                        "0|A-B! C|ab"

# C — refusals
expect "C1 no file"              "$(fixture)"                                          "1||"
expect "C2 no NAME= line"        "$(fixture 'ID=x\nPRETTY_NAME="Only"\n')"             "1||"
expect "C3 empty NAME"           "$(fixture 'NAME=""\n')"                              "1||"
expect "C4 NAME with no usable stem" "$(fixture 'NAME="!!!"\n')"                       "1||"
d="$(fixture)"; run "$d" > /dev/null
if grep -q 'no usable NAME=' "$d/err"; then ok "C5 the refusal says why on stderr"; else bad "C5 no message on stderr"; fi

# D — parsed, not sourced
d="$(fixture 'NAME="Safe"\ntouch "'"$T"'/ran"\n')"
got="$(run "$d")"
if [ "$got" = "0|Safe|safe" ] && [ ! -e "$T/ran" ]; then ok "D1 a command line in os-release is not run"; else bad "D1 got [$got], marker exists: $([ -e "$T/ran" ] && echo yes || echo no)"; fi

total=$((pass+fail))
echo ""
echo "  $pass passed, $fail failed, $total total"
[ "$total" -ge 15 ] || { echo "  FLOOR: only $total cases ran"; exit 1; }
[ "$fail" -eq 0 ]
