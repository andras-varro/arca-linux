#!/usr/bin/env bash
# tests/arca_backup_test.sh - host regression for device-files/arca-backup, the selectable backup engine.
#
# Runs the real script under dash (the nearest thing here to BusyBox ash) against a fake root
# (ARCA_ROOT) in a temp dir. Needs GNU tar and gzip; no device, no root. Run it in WSL.
#
# ARCA_BACKUP=<path> points the test at another engine; a missing one must FAIL every check, which is
# how the test was first seen to fail. Cannot see: BusyBox tar's own output format (the link check
# reads `tar tv`, whose name column BusyBox shares with GNU by measurement of the format only).

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="${ARCA_BACKUP:-$HERE/device-files/arca-backup}"
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }
check() { # check <label> <expected> <actual>
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1"; printf '       want: %s\n       got:  %s\n' "$2" "$3"; fi
}

suite() {
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
R="$W/root"
mkdir -p "$R/tmp" mkdir -p "$R/opt/arca/backup.d" "$R/etc" "$R/home/root/data/ssh" "$R/home/root/.ssh" "$R/opt/games" "$W/tmp"
item() { printf 'name=%s\npaths=%s\nunit=%s\nsecret=%s\nreboot=%s\n' "$2" "$3" "$4" "$5" "$6" > "$R/opt/arca/backup.d/$1.item"; }
item scores "High scores" "home/root/data/*.hig" no no no
item hostname "Host name" "etc/hostname etc/hosts" yes no yes
item ssh "SSH keys" "home/root/.ssh home/root/data/ssh" yes yes yes
echo myhost > "$R/etc/hostname"
printf 'VERSION_ID=1.2\n' > "$R/etc/os-release"
echo hosts > "$R/etc/hosts"
echo 100 > "$R/home/root/data/snake.hig"
echo 200 > "$R/home/root/data/tetris.hig"
echo nested > "$R/home/root/data/ssh/key"
echo auth > "$R/home/root/.ssh/authorized_keys"
echo unrelated > "$R/etc/unrelated"

run() { PATH="${SHIM:+$SHIM:}$PATH" ARCA_ROOT="$R" TMPDIR="$W/tmp" dash "$ENGINE" "$@" 2>&1; }

echo "== items"
check "items sorted, flags and names" "item hostname unit=yes secret=no reboot=yes Host name
item scores unit=no secret=no reboot=no High scores
item ssh unit=yes secret=yes reboot=yes SSH keys" "$(run items)"

echo "== export and contents"
SNAP=$(cd "$R" && find . | sort)
OUT=$(run export "$W/stick/ludbox-backup" scores hostname)
case "$OUT" in "exported $W/stick/ludbox-backup/ludbox-backup-myhost-"*.tar.gz) ok "export prints the path" ;; *) bad "export prints the path: $OUT" ;; esac
ARC=${OUT#exported }
members=$(tar -tzf "$ARC" 2> /dev/null | sort | tr '\n' ' ')
check "archive members" "etc/hostname etc/hosts home/root/data/snake.hig home/root/data/tetris.hig tmp/ludbox-backup.info " "$members"
check "export leaves the fake root unchanged (no stray file anywhere)" "$SNAP" "$(cd "$R" && find . | sort)"
check "export of an unknown id is refused" "error unknown item nope" "$(run export "$W/x" nope)"
D=$(tar -xzOf "$ARC" tmp/ludbox-backup.info | sed -n 's/^date=//p')
check "contents" "info host=myhost version=1.2 date=$D
item scores unit=no secret=no reboot=no High scores
item hostname unit=yes secret=no reboot=yes Host name" "$(run contents "$ARC")"
check "archives lists it" "archive $(basename "$ARC")" "$(run archives "$W/stick/ludbox-backup")"
check "archives of a missing dir is empty, exit 0" "" "$(run archives "$W/none"; echo -n)"

echo "== round trip into a different unit"
R_SAVE="$R"
R="$W/root2"
mkdir -p "$R/opt/arca/backup.d" "$R/etc" "$R/home/root/data"
item scores "High scores" "home/root/data/*.hig" no no no
item hostname "Host name" "etc/hostname etc/hosts" yes no yes
item ssh "SSH keys" "home/root/.ssh" yes yes yes
echo other > "$R/etc/hostname"
echo keepme > "$R/etc/unrelated"
echo old > "$R/home/root/data/old.hig"
check "import reports items and reboot" "imported scores
imported hostname
done reboot=yes" "$(run import "$ARC" scores hostname)"
check "hostname restored" "myhost" "$(cat "$R/etc/hostname")"
check "score restored" "100" "$(cat "$R/home/root/data/snake.hig")"
check "unrelated file kept" "keepme" "$(cat "$R/etc/unrelated")"
check "existing score not deleted" "old" "$(cat "$R/home/root/data/old.hig")"
rm -f "$R/home/root/data/snake.hig"
check "scores alone: reboot=no" "imported scores
done reboot=no" "$(run import "$ARC" scores)"
check "scores alone did not write hostname" "myhost" "$(cat "$R/etc/hostname")"
check "id the archive lacks is refused" "error item not in archive ssh" "$(run import "$ARC" ssh)"
check "id the unit does not declare is refused" "error unknown item bogus" "$(run import "$ARC" bogus)"
rm -f "$R/etc/hosts"
# An item declared by the importing unit narrower than the archive: only declared paths are written.
item hostname "Host name" "etc/hostname" yes no yes
run import "$ARC" hostname > /dev/null
check "undeclared member of a selected id is not written" "no" "$([ -e "$R/etc/hosts" ] && echo yes || echo no)"
R="$R_SAVE"

echo "== hostile archives (python tarfile: GNU tar strips / and .. on create)"
H="$W/hostile"
mkdir -p "$H"
cat > "$H/mk.py" <<'PY'
import io, sys, tarfile
out, kind, ids = sys.argv[1:4]
t = tarfile.open(out, "w:gz")
def f(n, d=b"x"):
    i = tarfile.TarInfo(n); i.size = len(d); t.addfile(i, io.BytesIO(d))
f("tmp/ludbox-backup.info", ("host=h\nversion=1\ndate=d\nids=%s\n" % ids).encode())
S = "home/root/data/scummvm-saves/"
if kind == "abs": f("/tmp/arca-backup-abs-probe")
if kind == "dotdot": f("../../arca-backup-dd-probe")
if kind == "inner": f(S + "../../../../tmp/arca-backup-inner-probe")
if kind == "hard":
    i = tarfile.TarInfo(S + "h"); i.type = tarfile.LNKTYPE; i.linkname = "etc/hostname"; t.addfile(i)
if kind == "sym":
    i = tarfile.TarInfo("home/root/.ssh/lnk"); i.type = tarfile.SYMTYPE; i.linkname = "/tmp"; t.addfile(i)
    f("home/root/.ssh/lnk/arca-backup-sym-probe")
if kind == "clean": f(S + "ok.000", b"saved")
t.close()
PY
item saves "Saves" "home/root/data/scummvm-saves" no no no
item ssh "SSH keys" "home/root/.ssh" yes yes yes
rm -f /tmp/arca-backup-abs-probe /tmp/arca-backup-inner-probe /tmp/arca-backup-sym-probe /tmp/arca-backup-dd-probe
before=$(cd "$R" && find . -type f | sort | xargs md5sum | md5sum)
for k in abs dotdot inner hard sym; do
  ids=saves; [ "$k" = sym ] && ids=ssh
  python3 -I "$H/mk.py" "$H/$k.tar.gz" "$k" "$ids"
  case "$(run import "$H/$k.tar.gz" "$ids" | head -n 1)" in
    "error unsafe member"*) ok "$k: archive refused" ;;
    *) bad "$k: archive refused" ;;
  esac
done
check "refused archives wrote nothing in the root" "$before" "$(cd "$R" && find . -type f | sort | xargs md5sum | md5sum)"
check "no probe file outside the root" "no" "$(find /tmp -maxdepth 1 -name "arca-backup-*-probe" | grep -q . && echo yes || echo no)"
python3 -I "$H/mk.py" "$H/clean.tar.gz" clean saves
check "clean archive without directory entries is accepted" "imported saves
done reboot=no" "$(run import "$H/clean.tar.gz" saves)"
check "its file landed" "saved" "$(cat "$R/home/root/data/scummvm-saves/ok.000" 2> /dev/null)"
check "negative control: a clean archive is accepted" "imported scores
done reboot=no" "$(run import "$ARC" scores)"
rm -rf "$W"
}

for MODE in gnu busybox; do
  echo "######## tar/gzip: $MODE"
  SHIM=""
  if [ "$MODE" = busybox ]; then
    BB="${ARCA_BUSYBOX:-busybox}"
    if ! "$BB" tar --help 2>&1 | grep -q -- "-T"; then echo "skip the busybox pass: $BB tar has no -T (set ARCA_BUSYBOX to a 1.35+ busybox)"; continue; fi
    SHIM="$(mktemp -d)"
    for a in tar gzip; do printf '#!/bin/sh\nexec %s %s "$@"\n' "$BB" "$a" > "$SHIM/$a"; chmod +x "$SHIM/$a"; done
  fi
  suite
  [ -n "$SHIM" ] && rm -rf "$SHIM"
done

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
