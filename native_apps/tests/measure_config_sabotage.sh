#!/bin/bash
# Measure that every assertion group in native_apps/tests/config_test.c can FAIL.
#
# Each case copies common/config.{c,h} into a scratch directory, applies ONE
# targeted defect to the COPY, rebuilds the test against it and reports whether
# the group named in the case went red.  Nothing under the repo is written and no
# case restores anything with git — the pristine sources come from a scratch
# snapshot taken once, up front, and every case starts from a fresh copy of it.
#
# The sabotaged binary is run from a scratch working directory of its own, so the
# test's build/ fixtures never land in the repo either.
#
# ⚠️ Case 0 is the control on the harness itself: an untouched copy must report 0
# failures.  Without it a bad compile flag, a missing build/ directory or a stale
# expectation would make every later case "caught" for the wrong reason.
#
# ⚠️ A sabotage that fails to apply prints NO-OP EDIT rather than "0 failed",
# because a rotted sed pattern is indistinguishable from a suite that cannot see
# the breakage.  Read the output; do not just count it.
#
# ⚠️ Five groups pin DEFECTS (E, F, G, I and J's directory case).  A pinned defect
# whose sabotage goes uncaught means nothing was pinned, so those cases are
# sabotaged with the plausible FIX — the change a later session would reach for —
# and the group must go red on it.

set -u

REPO=/mnt/c/work/arca
TEST=${ARCA_CONFIG_TEST:-/mnt/c/work/rw-scratch/c6/config_test.c}
W=$(mktemp -d /tmp/cfgsab.XXXXXX)
trap 'rm -rf "$W"' EXIT

if [ ! -r "$TEST" ]; then
    echo "no test source at $TEST" >&2
    exit 2
fi

mkdir -p "$W/pristine" "$W/run/build"
cp "$REPO/native_apps/common/config.c" "$REPO/native_apps/common/config.h" "$W/pristine/"

caught=0
missed=0
broken=0

# run <group-letters|none> <name> <sed-args…>   ("-" applies no sabotage)
run() {
    local groups="$1" name="$2"
    shift 2
    rm -rf "$W/c"
    mkdir -p "$W/c"
    cp "$W/pristine/config.c" "$W/pristine/config.h" "$W/c/"

    if [ "$1" != "-" ]; then
        if ! ( cd "$W/c" && "$@" ); then
            printf '  %-4s %-56s SABOTAGE DID NOT APPLY\n' "$groups" "$name"
            broken=$((broken + 1))
            return
        fi
        if diff -q "$W/c/config.c" "$W/pristine/config.c" >/dev/null &&
           diff -q "$W/c/config.h" "$W/pristine/config.h" >/dev/null; then
            printf '  %-4s %-56s NO-OP EDIT — pattern rotted\n' "$groups" "$name"
            broken=$((broken + 1))
            return
        fi
    fi

    if ! gcc -Wall -Wextra -Wno-unused-parameter -I "$W/c" -o "$W/t" \
             "$TEST" "$W/c/config.c" -lm 2>"$W/cc.log"; then
        printf '  %-4s %-56s DID NOT COMPILE (%s)\n' "$groups" "$name" \
               "$(head -1 "$W/cc.log")"
        broken=$((broken + 1))
        return
    fi

    # ⚠️ stdbuf, because a sabotage that CRASHES loses whatever libc had buffered
    # into the pipe, and a caught sabotage then reads as an undetected one.
    local out rc n red g note
    rm -rf "$W/run"
    mkdir -p "$W/run/build"
    out=$(cd "$W/run" && stdbuf -oL timeout 60 "$W/t" 2>&1)
    rc=$?
    n=$(printf '%s\n' "$out" | grep -c '^  FAIL ')

    if [ "$groups" = "none" ]; then
        if [ "$n" -eq 0 ] && [ "$rc" -eq 0 ]; then
            printf '  %-4s %-56s CONTROL OK (0 failed)\n' "$groups" "$name"
            caught=$((caught + 1))
        else
            printf '  %-4s %-56s CONTROL BROKEN (%s failed, rc=%s)\n' \
                   "$groups" "$name" "$n" "$rc"
            broken=$((broken + 1))
            printf '%s\n' "$out" | grep '^  FAIL ' | sed 's/^/         /' | head -6
        fi
        return
    fi

    red=yes
    for g in $(printf '%s' "$groups" | fold -w1); do
        printf '%s\n' "$out" | grep -q "^  FAIL $g " || red=no
    done

    note=""
    if [ "$rc" -ge 124 ]; then
        note=", then rc=$rc"
    fi

    if [ "$red" = yes ]; then
        printf '  %-4s %-56s caught (%s failed%s)\n' "$groups" "$name" "$n" "$note"
        caught=$((caught + 1))
    else
        printf '  %-4s %-56s NOT CAUGHT (%s failed, rc=%s)\n' \
               "$groups" "$name" "$n" "$rc"
        missed=$((missed + 1))
    fi
    printf '%s\n' "$out" | grep '^  FAIL ' | sed 's/^/         /' | head -4
}

echo "measure_config_sabotage.sh — one defect per case, against copies only"
echo

run none "0 PRISTINE control: an untouched copy must be green" -

run A "1 the writer separates key from value with ':'" \
    sed -i 's|fprintf(f, "%s=%s|fprintf(f, "%s:%s|' config.c
run A "2 an absent file loads as success" \
    sed -i '/^int config_load/,/^}/ s|if (!f) return -1;|if (!f) return 0;|' config.c
run BH "3 the trailing trim only eats spaces, not tabs or CR" \
    sed -i '/^static char \*trim/,/^}/ s|isspace((unsigned char)\*end)|(*end == 0x20)|' config.c
run B "4 a comment line is stored as a key" \
    sed -i 's|\*trimmed == .#.|*trimmed == 1|' config.c
run B "5 an empty key name is stored" \
    sed -i 's|if (\*key ==.*continue;|if (0) continue;|' config.c
run BG "6 a line with no '=' becomes a key with an empty value" \
    sed -i 's|if (!eq) continue;|if (!eq) { char *k = trim(trimmed); if (*k \&\& cfg->count < CONFIG_MAX_KEYS) { strncpy(cfg->entries[cfg->count].key, k, CONFIG_KEY_LEN - 1); cfg->entries[cfg->count].key[CONFIG_KEY_LEN - 1] = 0; cfg->entries[cfg->count].value[0] = 0; cfg->count++; } continue; }|' config.c
run C "7 config_get_int stops checking the value is numeric" \
    sed -i 's|if (!isdigit((unsigned char)\*p)) return default_val;||' config.c
run C "8 'on' is no longer a true spelling" \
    sed -i 's|str_eq_nocase(val, "on")|str_eq_nocase(val, "onx")|' config.c
run C "9 the case-insensitive compare becomes case-sensitive" \
    sed -i 's|if (tolower((unsigned char)\*a) != tolower((unsigned char)\*b))|if (*a != *b)|' config.c
run D "10 config_set appends instead of updating in place" \
    sed -i 's|    if (idx >= 0) {|    if (idx >= 0 \&\& idx < 0) {|' config.c
run D "11 config_remove reports false after removing" \
    sed -i 's|^    return true;|    return false;|' config.c
run D "12 config_remove leaves a hole instead of shifting" \
    sed -i 's|cfg->entries\[i\] = cfg->entries\[i + 1\];|cfg->entries[i] = cfg->entries[i];|' config.c
run D "13 config_clear forgets to zero the count" \
    sed -i '/^void config_clear/,/^}/ s|cfg->count = 0;|(void)0;|' config.c
run E "14 THE FIX: config_load skips a key it already holds" \
    sed -i 's|if (cfg->count < CONFIG_MAX_KEYS) {|if (cfg->count < CONFIG_MAX_KEYS \&\& find_key(cfg, key) < 0) {|' config.c
run E "15 find_key answers with the LAST match, not the first" \
    sed -i '/^static int find_key/,/^}/ s|for (int i = 0; i < cfg->count; i++)|for (int i = cfg->count - 1; i >= 0; i--)|' config.c
run F "16 THE FIX: an over-full file is refused with -1" \
    sed -i 's|if (cfg->count < CONFIG_MAX_KEYS) {|if (cfg->count >= CONFIG_MAX_KEYS) { fclose(f); return -1; } if (cfg->count < CONFIG_MAX_KEYS) {|' config.c
run F "17 the load ceiling is four keys short of the array" \
    sed -i 's|if (cfg->count < CONFIG_MAX_KEYS) {|if (cfg->count < CONFIG_MAX_KEYS - 4) {|' config.c
run F "18 config_set overwrites the last slot instead of refusing" \
    sed -i 's|if (cfg->count >= CONFIG_MAX_KEYS) return;|if (cfg->count >= CONFIG_MAX_KEYS) cfg->count = CONFIG_MAX_KEYS - 1;|' config.c
run G "19 THE FIX: the line buffer is big enough not to split" \
    sed -i 's|char line\[CONFIG_KEY_LEN + CONFIG_VAL_LEN + 16\];|char line[1024];|' config.c
run G "20 the stored value is cut eight bytes early" \
    sed -i 's|value, val, CONFIG_VAL_LEN - 1);|value, val, CONFIG_VAL_LEN - 9);|' config.c
run I "21 the stored key is cut two bytes early" \
    sed -i 's|key, key, CONFIG_KEY_LEN - 1);|key, key, CONFIG_KEY_LEN - 3);|' config.c
run I "22 config_init_path drops a byte of the path" \
    sed -i '/^void config_init_path/,/^}/ s|sizeof(cfg->filepath) - 1|sizeof(cfg->filepath) - 2|' config.c
run J "23 a temp name that would truncate is accepted" \
    sed -i 's|if (n < 0 \|\| (size_t)n >= tmp_sz) {|if (n < 0) {|' config.c
run J "24 an unopenable path reports success" \
    sed -i '/^int config_save/,/^}/ s|if (!f) return -1;|if (!f) return 0;|' config.c
run J "25 the parent directory is never created" \
    sed -i 's|mkdir(dir, 0755);|(void)dir;|' config.c
run J "26 abort keeps the temp file on disk" \
    sed -i 's|if (tmp_path \&\& tmp_path\[0\]) unlink(tmp_path);|(void)tmp_path;|' config.c
run J "27 THE FIX: a directory path is refused" \
    sed -i 's|char line\[CONFIG_KEY_LEN + CONFIG_VAL_LEN + 16\];|char line[CONFIG_KEY_LEN + CONFIG_VAL_LEN + 16]; { struct stat st; if (fstat(fileno(f), \&st) == 0 \&\& S_ISDIR(st.st_mode)) { fclose(f); return -1; } }|' config.c
run K "28 the sound-set path loses its extension" \
    sed -i 's|"%s/%s%s", CONFIG_SOUND_SET_DIR,|"%s/%s%.0s", CONFIG_SOUND_SET_DIR,|' config.c
run K "29 an empty tag is passed straight through" \
    sed -i 's|(tag \&\& \*tag) ? tag : "default"|tag ? tag : "default"|' config.c

echo
if [ "$missed" -eq 0 ] && [ "$broken" -eq 0 ]; then
    verdict=PASSED
else
    verdict=FAILED
fi
printf '%s: %d caught, %d NOT caught, %d harness problems\n' \
       "$verdict" "$caught" "$missed" "$broken"
[ "$missed" -eq 0 ] && [ "$broken" -eq 0 ]
