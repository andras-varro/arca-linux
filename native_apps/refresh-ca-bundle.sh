#!/bin/bash
# Regenerate data/ca-certificates.crt, the trust store the TLS client reads from
# /opt/arca/ca-certificates.crt when /etc/ssl/certs/ca-certificates.crt is absent
# (the root filesystem ships no CA package until the next image).
#
# Source: the build host's own Mozilla bundle, /etc/ssl/certs/ca-certificates.crt
# (Ubuntu ca-certificates, run in WSL).  Buildroot's ca-certificates package was not
# built in ~/br-rw, and building it would disturb that tree, so it is not used; once
# BR2_PACKAGE_CA_CERTIFICATES=y is in an image, /etc/ssl/certs/ca-certificates.crt
# takes precedence on the device and this file is only the fallback.
#
# Usage: ./refresh-ca-bundle.sh [<source bundle>]   (run in WSL; commit the result)
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-/etc/ssl/certs/ca-certificates.crt}"
OUT="$SCRIPT_DIR/data/ca-certificates.crt"
[ -f "$SRC" ] || { echo "no bundle at $SRC"; exit 1; }
n=$(grep -c 'BEGIN CERTIFICATE' "$SRC" || true)
[ "$n" -ge 100 ] || { echo "$SRC holds only $n certificates — refusing"; exit 1; }
mkdir -p "$SCRIPT_DIR/data"
tr -d '\r' < "$SRC" > "$OUT.part"
mv "$OUT.part" "$OUT"
echo "$OUT: $n certificates, $(wc -c < "$OUT") bytes, from $SRC"
