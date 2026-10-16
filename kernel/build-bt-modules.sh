#!/bin/bash
# build-bt-modules.sh - Build Bluetooth (+ uhid, uinput, AF_ALG) modules that load on OUR image
# Run this script in WSL (Linux), after build-image.sh. The image's config has CONFIG_BT unset, so
# these modules cannot come from ~/rw-kbuild-image: this script copies that tree to its own work
# dir, enables BT there as modules, builds, and keeps only the .ko set Bluetooth needs. The image
# tree is read, never written; the script checks that at the end.
#
# The copy's vmlinux is NOT the image. A module built here is safe only if every symbol it imports
# from vmlinux exists in the image's vmlinux with the same CRC, so that is checked against the
# image's own vmlinux (its __crc_ symbols), not against the copy's.
#
# Produces: <out>/*.ko, <out>/load-order.txt (insmod order, dependencies first).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE_WORK="${HOME}/rw-kbuild-image"
WORK_DIR="${HOME}/rw-kbuild-bt"
PATCH_DIR="${SCRIPT_DIR}/patches-modules"
CROSS_COMPILE="arm-linux-gnueabihf-"
ARCH="arm"
OUT_DIR=""
REUSE=0

# scripts/config verb + symbol. RFKILL is a module so bluetoothd finds /dev/rfkill; without it
# bluetoothd logs an error at every start. BT_RFCOMM is built for BlueALSA's hfp-ag.
# BT_HS off removes A2MP, the BleedingTooth entry point (CVE-2020-12351/12352: no pairing needed, only
# the BD_ADDR); HID pads and A2DP never use High Speed.
CONFIG_WANT=(
    "module BT" "enable BT_BREDR" "enable BT_LE" "disable BT_HS" "module BT_HIDP"
    "module BT_HCIBTUSB" "enable BT_HCIBTUSB_BCM" "enable BT_HCIBTUSB_RTL"
    "module UHID" "module CRYPTO_USER_API_HASH" "module CRYPTO_USER_API_SKCIPHER"
    "module INPUT_UINPUT" "module RFKILL" "module BT_RFCOMM"
)
# Modules wanted by name. Their symbol dependencies are added from each .modinfo depends= field.
ROOTS=(bluetooth btusb hidp rfcomm uhid uinput algif_hash algif_skcipher)
# Reached only through crypto_alloc_*() / request_module(), so no `depends=` line names them:
# SMP's cmac(aes) and ecdh, bluetoothd's AF_ALG ecb(aes) and cmac(aes), cryptomgr to instantiate
# any template, and the default RNG that ecdh_generic's key generation can ask for. They are
# resolved BEFORE the ROOTS, so load-order.txt puts them ahead of btusb: btusb probes an adapter
# already on the bus the moment it loads, and the SMP setup that follows allocates cmac(aes)
# and ecdh straight away. jitterentropy_rng is not here although drbg can use it: measured on
# .188, insmod refuses it ("host not compliant with requirements: 2") and drbg loads without it.
RUNTIME_ROOTS=(cryptomgr cmac ecb sha256_generic ecdh_generic hmac drbg)

usage() {
    cat <<'EOF'
Usage: build-bt-modules.sh --out <dir> [--image <work>] [--work <dir>] [--reuse]

Copies ~/rw-kbuild-image/linux-4.14.52 (built by build-image.sh) to <work>/linux-4.14.52, applies
kernel/patches-modules/*.patch there, enables BT/btusb/hidp/uhid/uinput/AF_ALG as modules,
runs olddefconfig and `make modules`, then copies the Bluetooth module closure to <dir>.

Checks, each fatal on failure:
  - every symbol a collected module imports from vmlinux is exported by the IMAGE's vmlinux
    with the same CRC (so the copy's config change did not reach an ABI the modules use)
  - every module's vermagic equals that of a module build-modules.sh built for the image
  - the image tree's .config, Module.symvers and vmlinux are unchanged afterwards
A config change to a built-in (=y) symbol is printed as a WARNING: it means the copy's vmlinux
differs from the image's, which the CRC check then has to clear.

  --out <dir>   Where the .ko files (debug info stripped) and load-order.txt go. Existing *.ko
                there are removed.
  --image <work> The image's build tree parent: the --work given to build-image.sh, which must
                be the image the unit boots (default: ~/rw-kbuild-image).
  --work <dir>  Work dir in the WSL filesystem (default: ~/rw-kbuild-bt). Refused under /mnt.
  --reuse       Keep an existing copy (incremental rebuild) instead of re-copying the image tree.
  -h, --help    This text.

Nothing is deployed and nothing in the repo is written.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --out)     shift; OUT_DIR="${1:?--out needs a directory}" ;;
        --work)    shift; WORK_DIR="${1:?--work needs a directory}" ;;
        --image)   shift; IMAGE_WORK="${1:?--image needs a directory}" ;;
        --reuse)   REUSE=1 ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "ERROR: unknown argument $1"; usage; exit 1 ;;
    esac
    shift
done
[ -n "$OUT_DIR" ] || { usage; exit 1; }
IMAGE_DIR="${IMAGE_WORK}/linux-4.14.52"
REF_KO="${IMAGE_WORK}/modules/cy8ctmg120_ts/cy8ctmg120_ts.ko"
KDIR="${WORK_DIR}/linux-4.14.52"

for f in .config Module.symvers vmlinux; do
    [ -f "${IMAGE_DIR}/${f}" ] || { echo "ERROR: ${IMAGE_DIR}/${f} missing; run build-image.sh first."; exit 1; }
done
case "$(realpath -m "$WORK_DIR")" in
    /mnt/*) echo "ERROR: work dir ${WORK_DIR} is on DrvFs. Use a path under \$HOME."; exit 1 ;;
esac
case "$(realpath -m "$KDIR")" in
    "$(realpath -m "$IMAGE_DIR")") echo "ERROR: the work tree would be the image tree itself."; exit 1 ;;
esac
for tool in "${CROSS_COMPILE}gcc" "${CROSS_COMPILE}nm" "${CROSS_COMPILE}readelf" "${CROSS_COMPILE}strip" modprobe; do
    command -v "$tool" &>/dev/null || { echo "ERROR: ${tool} not found."; exit 1; }
done

# One field of a .ko's .modinfo. Not host `modinfo -F`: measured on this host's kmod, it stops at
# the first empty string, and a module whose own .modinfo ends in alignment padding (ecb, hmac,
# sha256_generic, drbg, ...) then reads as having no vermagic and no depends at all.
ko_field() {
    "${CROSS_COMPILE}readelf" -p .modinfo "$1" | sed -nE "s/^ *\[ *[0-9a-f]+\] +$2=//p"
}

# Fingerprint the image tree so the end of the run can prove it was not touched.
image_fingerprint() {
    md5sum "${IMAGE_DIR}/.config" "${IMAGE_DIR}/Module.symvers" "${IMAGE_DIR}/vmlinux" | cut -d' ' -f1 | tr '\n' ' '
}
IMAGE_FP_BEFORE="$(image_fingerprint)"

# Same pins as build-image.sh, so the copy's vmlinux relink embeds the same identity strings.
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
EPOCH=$(git -C "$REPO_DIR" log -1 --format=%ct 2>/dev/null || date +%s)
KBUILD_BUILD_TIMESTAMP="$(date -u -d "@${EPOCH}")"
export SOURCE_DATE_EPOCH="$EPOCH" KBUILD_BUILD_TIMESTAMP KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_USER=arca KBUILD_BUILD_HOST=build

echo "[1/7] Copying the image tree..."
if [ "$REUSE" -eq 1 ] && [ -f "${KDIR}/Makefile" ]; then
    echo "  --reuse: keeping ${KDIR}"
else
    mkdir -p "$WORK_DIR"
    rm -rf "$KDIR"
    # A real copy, never hardlinks: the build rewrites files in place, which through a hardlink
    # would rewrite the image tree's copy too.
    cp -a "$IMAGE_DIR" "$KDIR"
    echo "  ${IMAGE_DIR} -> ${KDIR} ($(du -sh "$KDIR" | cut -f1))"
fi
cp "${IMAGE_DIR}/.config" "${KDIR}/.config"

echo; echo "[2/7] Applying module-only patches from ${PATCH_DIR}..."
mapfile -t PATCHES < <(find "$PATCH_DIR" -maxdepth 1 -name '*.patch' 2>/dev/null | sort)
for p in "${PATCHES[@]}"; do
    # --forward on the reverse probe: without it `-R --batch` on an unpatched tree exits 0.
    if patch -d "$KDIR" -p1 -R --forward --dry-run -s < "$p" &>/dev/null; then
        echo "  Already applied: $(basename "$p")"
        continue
    fi
    patch -d "$KDIR" -p1 --forward --batch --no-backup-if-mismatch -s < "$p" \
        || { echo "ERROR: $(basename "$p") did not apply."; exit 1; }
    patch -d "$KDIR" -p1 -R --forward --dry-run -s < "$p" >/dev/null \
        || { echo "ERROR: $(basename "$p") reported success but does not reverse."; exit 1; }
    echo "  Applied: $(basename "$p")"
done

echo; echo "[3/7] Configuring..."
for w in "${CONFIG_WANT[@]}"; do
    read -r verb sym <<< "$w"
    "${KDIR}/scripts/config" --file "${KDIR}/.config" "--${verb}" "$sym"
done
make -C "$KDIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig >/dev/null
for w in "${CONFIG_WANT[@]}"; do
    read -r verb sym <<< "$w"
    case "$verb" in
        module)  want="CONFIG_${sym}=m" ;;
        enable)  want="CONFIG_${sym}=y" ;;
        disable) want="# CONFIG_${sym} is not set" ;;
    esac
    # A disabled symbol whose menu is off does not appear at all, which is as good as unset.
    if [ "$verb" = disable ]; then
        grep -qE "^CONFIG_${sym}=" "${KDIR}/.config" && { echo "ERROR: CONFIG_${sym} is set after olddefconfig"; exit 1; }
    else
        grep -qxF "$want" "${KDIR}/.config" || { echo "ERROR: .config lacks '${want}' after olddefconfig"; exit 1; }
    fi
done

# sym=value for every symbol, "n" for "is not set"; absent symbols print as "-".
config_map() {
    sed -nE 's/^(CONFIG_[A-Za-z0-9_]+)=(.*)$/\1 \2/p; s/^# (CONFIG_[A-Za-z0-9_]+) is not set$/\1 n/p' "$1" | sort
}
echo "  Changed symbols (image -> copy):"
builtin_changes=0
while read -r sym old new; do
    flag=""
    if [ "$old" = y ] || [ "$new" = y ]; then flag="   <-- WARNING: =y, vmlinux may differ (compared after the build)"; builtin_changes=$((builtin_changes + 1)); fi
    echo "    ${sym}: ${old} -> ${new}${flag}"
done < <(join -a1 -a2 -e - -o 0,1.2,2.2 <(config_map "${IMAGE_DIR}/.config") <(config_map "${KDIR}/.config") \
         | awk '$2 != $3')
echo "  Built-in changes: ${builtin_changes}"

echo; echo "[4/7] Building modules with -j$(nproc)..."
start=$SECONDS
make -C "$KDIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE -j"$(nproc)" modules
echo "  Built in $((SECONDS - start)) s."
# With MODVERSIONS, `make modules` relinks vmlinux. Identical bytes settle the built-in warnings.
if cmp -s "${KDIR}/vmlinux" "${IMAGE_DIR}/vmlinux"; then
    echo "  The copy's vmlinux is byte-identical to the image's: no config change reached it."
else
    echo "  WARNING: the copy's vmlinux differs from the image's; the CRC check below decides."
fi

echo; echo "[5/7] Resolving the module closure..."
declare -A KO_PATH=()
while IFS= read -r ko; do
    n="$(basename "$ko" .ko)"; n="${n//-/_}"
    KO_PATH[$n]="$ko"
done < <(find "$KDIR" -name '*.ko' -not -path '*/.tmp_versions/*')
declare -A SEEN=()
ORDER=()
visit() {
    local m="$1" d
    [ -n "${SEEN[$m]:-}" ] && return 0
    SEEN[$m]=1
    [ -n "${KO_PATH[$m]:-}" ] || { echo "ERROR: module ${m} is needed but was not built"; exit 1; }
    for d in $(ko_field "${KO_PATH[$m]}" depends | tr ',' ' '); do
        visit "${d//-/_}"
    done
    ORDER+=("$m")
}
for m in "${RUNTIME_ROOTS[@]}"; do
    if [ -n "${KO_PATH[$m]:-}" ]; then
        visit "$m"
    elif grep -qE "/${m//_/[-_]}\.ko$" "${KDIR}/modules.builtin" 2>/dev/null; then
        echo "  ${m}: built in"
    else
        echo "  WARNING: ${m} is neither a module nor built in"
    fi
done
for m in "${ROOTS[@]}"; do visit "$m"; done
echo "  ${#ORDER[@]} modules."

echo; echo "[6/7] Checking vermagic and imported symbol CRCs against the image..."
fail=0
if [ -f "$REF_KO" ]; then
    want_vm="$(ko_field "$REF_KO" vermagic)"
    echo "  Reference vermagic ($(basename "$REF_KO")): ${want_vm}"
else
    want_vm="$(cat "${IMAGE_DIR}/include/config/kernel.release") SMP mod_unload modversions ARMv7 p2v8"
    echo "  WARNING: ${REF_KO} missing; comparing with the expected string '${want_vm}'"
fi
for m in "${ORDER[@]}"; do
    vm="$(ko_field "${KO_PATH[$m]}" vermagic)"
    [ "$vm" = "$want_vm" ] || { echo "  FAIL vermagic ${m}: '${vm}'"; fail=1; }
done
# The image's CRCs straight from its vmlinux; its Module.symvers is cross-checked against them.
declare -A IMG_CRC=()
while read -r val sym; do IMG_CRC[$sym]="0x${val}"; done \
    < <("${CROSS_COMPILE}nm" "${IMAGE_DIR}/vmlinux" | awk '$2 == "A" && $3 ~ /^__crc_/ { sub(/^__crc_/, "", $3); print $1, $3 }')
stale=0
while read -r crc sym mod _; do
    [ "$mod" = vmlinux ] || continue
    [ "${IMG_CRC[$sym]:-}" = "$crc" ] || stale=$((stale + 1))
done < "${IMAGE_DIR}/Module.symvers"
[ "$stale" -eq 0 ] || { echo "  FAIL: image Module.symvers disagrees with image vmlinux on ${stale} CRCs"; fail=1; }
# Symbols the collected modules export to each other.
declare -A SET_EXPORT=()
for m in "${ORDER[@]}"; do
    rel="${KO_PATH[$m]#"${KDIR}"/}"; rel="${rel%.ko}"
    while read -r _ sym mod _; do
        [ "$mod" = "$rel" ] && SET_EXPORT[$sym]=1
    done < "${KDIR}/Module.symvers"
done
checked=0
for m in "${ORDER[@]}"; do
    while read -r crc sym; do
        [ "$sym" = module_layout ] || [ -z "${SET_EXPORT[$sym]:-}" ] || continue
        checked=$((checked + 1))
        img="${IMG_CRC[$sym]:-}"
        # nm pads to 8 hex digits, --dump-modversions prints 0x%08x: compare numerically.
        if [ -z "$img" ]; then
            echo "  FAIL ${m}: imports ${sym}, which the image's vmlinux does not export"; fail=1
        elif [ $((crc)) -ne $((img)) ]; then
            echo "  FAIL ${m}: ${sym} CRC ${crc}, image has ${img}"; fail=1
        fi
    done < <(modprobe --dump-modversions "${KO_PATH[$m]}")
done
echo "  ${checked} vmlinux imports checked across ${#ORDER[@]} modules; ${#IMG_CRC[@]} image CRCs; image symvers consistent: $([ "$stale" -eq 0 ] && echo yes || echo no)."
IMAGE_FP_AFTER="$(image_fingerprint)"
[ "$IMAGE_FP_BEFORE" = "$IMAGE_FP_AFTER" ] || { echo "  FAIL: the image tree changed during this run"; fail=1; }
[ "$fail" -eq 0 ] || { echo "ERROR: checks failed; nothing copied to ${OUT_DIR}."; exit 1; }
echo "  PASS: vermagic, CRCs and image tree."

echo; echo "[7/7] Copying to ${OUT_DIR}..."
mkdir -p "$OUT_DIR"
rm -f "${OUT_DIR}"/*.ko
: > "${OUT_DIR}/load-order.txt"
total=0
for m in "${ORDER[@]}"; do
    ko="${KO_PATH[$m]}"
    b="$(basename "$ko")"
    # --strip-debug is what INSTALL_MOD_STRIP=1 does: .modinfo, __versions and .symtab stay.
    "${CROSS_COMPILE}strip" --strip-debug -o "${OUT_DIR}/${b}" "$ko"
    echo "$b" >> "${OUT_DIR}/load-order.txt"
    sz=$(stat -c %s "${OUT_DIR}/${b}"); total=$((total + sz))
    printf '  %-26s %8d B  md5 %s\n' "$b" "$sz" "$(md5sum "${OUT_DIR}/${b}" | cut -d' ' -f1)"
done
echo "  Total: ${total} B in ${#ORDER[@]} modules."
echo "  Load order (dependencies first; also in load-order.txt):"
echo "    for m in \$(cat load-order.txt); do insmod \"\$m\"; done"
sed 's/^/    /' "${OUT_DIR}/load-order.txt"
