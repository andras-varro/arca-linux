#!/bin/bash
#
# deploy-all.sh — Build and deploy all components to an Arca device
#
# Scans the workspace for build-and-deploy.sh scripts and runs them in
# order.  After all components are deployed, sets the app launcher as the
# default boot app.
#
# Usage:
#   ./deploy-all.sh <ip>                     # build + deploy everything
#   ./deploy-all.sh <ip> <component>         # build + deploy one component
#   ./deploy-all.sh --list                   # show discovered components
#   ./deploy-all.sh --from-bundle <b> <ip>   # install a bundle; build NOTHING
#   ./deploy-all.sh --from-release <tag> <ip>  # fetch that release, then the above
#
# Components are detected automatically from subdirectories containing a
# build-and-deploy.sh script.  They are deployed in a deterministic order:
# native_apps first (provides the launcher), then the rest alphabetically.
#
# ── --from-bundle: the delivery mode, and why it is here ────────────────────
#
# Every other mode BUILDS, so every other mode needs the ARM cross-compilers
# (arm-linux-gnueabi-gcc, and arm-linux-gnueabihf-gcc for vnc_client).
# Someone being handed a device has no toolchain — that is what a release bundle is
# for — so before this existed, the offline card pass was the only way to put
# binaries on a unit even for someone who already had SSH to it.
#
# It is the SSH twin of commissioning/commission-offline.sh's install loop and shares its
# authority: arca_bundle_install_ssh in lib/arca-bundle.sh, one implementation, modes
# DECLARED by the manifest rather than carried by the transfer.
#
# ── --from-release: the same mode, over the network ─────────────────────────
#
# Identical downstream — it resolves a published release to a local tarball and
# then IS --from-bundle.  The fetch lives in lib/arca-release.sh, the one library
# here that opens a socket, and it happens AFTER the SSH gate below: a device that
# cannot be reached is worth finding out about before 126 MB, not after.
#
# Prerequisites:
#   - Device set up with commissioning/provision.sh (one-time)
#   - ARM cross-compiler installed          — except for --from-bundle/--from-release
#   - WSL (for ScummVM builds)              — except for --from-bundle/--from-release
#   ./setup-build-env.sh installs the host half of those; it is the one place the
#   package set is written down, and the component scripts only report what is missing.

set -e
_START_SECONDS=$(date +%s)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=lib/arca-bundle.sh
. "$SCRIPT_DIR/lib/arca-bundle.sh"
# shellcheck source=lib/arca-release.sh
. "$SCRIPT_DIR/lib/arca-release.sh"
# shellcheck source=lib/arca-ssh.sh
. "$SCRIPT_DIR/lib/arca-ssh.sh"
# shellcheck source=lib/product.sh
. "$SCRIPT_DIR/lib/product.sh"

DEVICE_IP="${1:-}"

# ── colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "[$(date '+%H:%M:%S')] ${GREEN}  ✓ $*${NC}"; }
info() { echo -e "[$(date '+%H:%M:%S')] ${YELLOW}  → $*${NC}"; }
warn() { echo -e "[$(date '+%H:%M:%S')] ${BLUE}  ! $*${NC}"; }
err()  { echo -e "[$(date '+%H:%M:%S')] ${RED}  ✗ $*${NC}"; exit 1; }

# ── discover components ─────────────────────────────────────────────────────
# Returns an ordered list: native_apps first, then the rest alphabetically.
discover_components() {
    local components=()
    local first=""

    for script in "$SCRIPT_DIR"/*/build-and-deploy.sh; do
        [ -f "$script" ] || continue
        local dir
        dir="$(basename "$(dirname "$script")")"

        # Skip directories that aren't real components
        # (e.g. build/, partitions/, browser_games/ have no deploy script)
        if [[ "$dir" == "native_apps" ]]; then
            first="$dir"
        else
            components+=("$dir")
        fi
    done

    # Sort the rest alphabetically
    IFS=$'\n' components=($(sort <<<"${components[*]}")); unset IFS

    # native_apps first (provides the launcher — other manifests depend on it)
    if [[ -n "$first" ]]; then
        echo "$first"
    fi
    for c in "${components[@]}"; do
        echo "$c"
    done
}

# ── list mode ───────────────────────────────────────────────────────────────
if [[ "$DEVICE_IP" == "--list" ]]; then
    echo ""
    echo "Discovered components:"
    echo ""
    idx=1
    while IFS= read -r comp; do
        local_script="$SCRIPT_DIR/$comp/build-and-deploy.sh"
        desc=$(grep -m1 '^# [A-Za-z]' "$local_script" | sed 's/^# *//')
        printf "  %d. %-22s  %s\n" "$idx" "$comp" "$desc"
        idx=$((idx + 1))
    done < <(discover_components)
    echo ""
    echo "Usage: $0 <ip> [component]"
    exit 0
fi

# ── --from-bundle mode: install a staged bundle, build nothing ─────────────
#
# Parsed before the IPv4 validation below because the IP is $2 here, not $1.
#
# --from-release is the same mode with a download in front of it, so it sets the
# tag and the block below resolves it to a tarball once SSH is known to work.
# The tag is REQUIRED and `latest` is a legal value: guessing from the shape of
# the argument ("does this look like an IP or a tag") is how a typo becomes a
# deploy of the wrong release.
FROM_BUNDLE=""
FROM_RELEASE=""
if [[ "$DEVICE_IP" == "--from-bundle" ]]; then
    FROM_BUNDLE="${2:-}"
    DEVICE_IP="${3:-}"
    [[ -n "$FROM_BUNDLE" ]] || err "--from-bundle needs a tarball or staged directory"
    [[ -n "$DEVICE_IP" ]]   || err "--from-bundle <bundle> <ip> — the IP is missing"
elif [[ "$DEVICE_IP" == "--from-release" ]]; then
    FROM_RELEASE="${2:-}"
    DEVICE_IP="${3:-}"
    [[ -n "$FROM_RELEASE" ]] || err "--from-release needs a tag, or the word 'latest'"
    [[ -n "$DEVICE_IP" ]]    || err "--from-release <tag|latest> <ip> — the IP is missing"
fi

# ── usage ───────────────────────────────────────────────────────────────────
if [[ -z "$DEVICE_IP" ]]; then
    echo "Usage: $0 <ip> [component]"
    echo "       $0 --from-bundle <tar.gz|dir> <ip>"
    echo "       $0 --from-release <tag|latest> <ip>"
    echo "       $0 --list"
    echo ""
    echo "Builds and deploys all components (or a single one) to the device."
    echo "Sets native_apps/app_launcher as the default boot app."
    echo ""
    echo "  --from-bundle   Install a release bundle over SSH and build NOTHING."
    echo "                  Needs no cross-compiler; this is the delivery mode."
    echo "                  Make one with:  ./release.sh --stage-only"
    echo "  --from-release  The same, from a published GitHub release: it is"
    echo "                  downloaded, its sha256 checked against the digest"
    echo "                  GitHub publishes, and cached under build/release-cache"
    echo "                  so a second unit costs no second download."
    echo "  --skip-tests    Deploy without running the host gate first. The gate"
    echo "                  (tests/run-all.sh --scope=deploy, ~150 s) otherwise"
    echo "                  blocks the deploy if any suite fails."
    echo ""
    echo "Examples:"
    echo "  $0 192.168.50.53              # deploy everything"
    echo "  $0 192.168.50.53 vnc_client   # deploy VNC client only"
    echo "  $0 --from-bundle build/release 192.168.50.53"
    echo "  $0 --from-release latest 192.168.50.53"
    echo "  $0 --list                     # show available components"
    exit 1
fi

FILTER="${2:-}"
[[ -n "$FROM_BUNDLE" || -n "$FROM_RELEASE" ]] && FILTER=""

# ── --skip-tests ────────────────────────────────────────────────────────────
# Sniffed out of the positional arguments rather than parsed by a flag loop,
# because this script has never had one and $2 is the component filter.  Both
# `deploy-all.sh <ip> --skip-tests` and `deploy-all.sh <ip> <component>
# --skip-tests` work; the first form must also stop --skip-tests being taken
# for a component name, which is what the FILTER reassignment below does.
SKIP_TESTS=0
if [[ "$FILTER" == "--skip-tests" ]]; then
    SKIP_TESTS=1
    FILTER="${3:-}"
fi
[[ "${3:-}" == "--skip-tests" ]] && SKIP_TESTS=1

# ── validate the device IP before building anything ─────────────────────────
# `./deploy-all.sh vnc_client` (forgetting the IP) used to build *everything*,
# including the multi-minute ScummVM build, and only fail at `ssh root@vnc_client`
# minutes later.  The single most likely typo is a component name in $1, so name
# that case explicitly.
IPV4_RE='^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$'
if [[ ! "$DEVICE_IP" =~ $IPV4_RE ]]; then
    echo ""
    if [[ -f "$SCRIPT_DIR/$DEVICE_IP/build-and-deploy.sh" ]]; then
        echo "  '$DEVICE_IP' is a component, not an IP — the IP comes first:"
        echo "      $0 <ip> $DEVICE_IP"
    else
        echo "  Not an IPv4 address: '$DEVICE_IP'"
    fi
    echo ""
    echo "Usage: $0 <ip> [component]"
    echo "       $0 --list"
    exit 1
fi

# ── main ────────────────────────────────────────────────────────────────────
echo ""
echo "╔═══════════════════════════════════════╗"
printf '║  %-37s║\n' "$PRODUCT_NAME Deploy All"
echo "╚═══════════════════════════════════════╝"
info "Started — $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

# ── --from-bundle: install and stop. No component loop, no compiler. ────────
if [[ -n "$FROM_BUNDLE" || -n "$FROM_RELEASE" ]]; then
    DEVICE="root@${DEVICE_IP}"
    arca_ssh_gate "$DEVICE" || err "Cannot continue without SSH to $DEVICE"
    ok "SSH OK"

    # --from-release becomes --from-bundle here, and deliberately not sooner: the
    # gate above is cheap and the download is 126 MB.
    if [[ -n "$FROM_RELEASE" ]]; then
        GH_REPO="$(arca_release_repo "$SCRIPT_DIR")" \
            || err "could not work out which GitHub repo to fetch from — see above"
        info "Fetching release $FROM_RELEASE from $GH_REPO"
        FROM_BUNDLE="$(arca_release_fetch "$GH_REPO" "$FROM_RELEASE" \
                          "$(arca_release_cache_dir "$SCRIPT_DIR")")" \
            || err "could not fetch release $FROM_RELEASE — nothing was installed"
    fi

    # A tarball is unpacked into a temp dir; a directory is used where it is.
    BUNDLE_TMP=""
    if [[ -d "$FROM_BUNDLE" ]]; then
        BUNDLE_DIR="$(cd "$FROM_BUNDLE" && pwd)"
    elif [[ -f "$FROM_BUNDLE" ]]; then
        BUNDLE_TMP=$(mktemp -d)
        # shellcheck disable=SC2064  # $BUNDLE_TMP is meant to expand now
        trap "rm -rf '$BUNDLE_TMP'" EXIT
        info "Unpacking $(basename "$FROM_BUNDLE")"
        tar -xzf "$FROM_BUNDLE" -C "$BUNDLE_TMP" || err "could not unpack $FROM_BUNDLE"
        # release.sh tars the staged directory, so root/ may be one level down.
        if [[ -d "$BUNDLE_TMP/root" ]]; then
            BUNDLE_DIR="$BUNDLE_TMP"
        else
            BUNDLE_DIR=$(find "$BUNDLE_TMP" -maxdepth 2 -type d -name root -print -quit)
            BUNDLE_DIR="${BUNDLE_DIR%/root}"
            [[ -n "$BUNDLE_DIR" ]] || err "no root/ directory inside $FROM_BUNDLE"
        fi
    else
        err "no such bundle: $FROM_BUNDLE"
    fi

    info "Components in the bundle: $(arca_bundle_components "$BUNDLE_DIR" | tr '\n' ' ')"

    # Stop whatever holds /dev/fb0 before overwriting the binary behind it. The init
    # script is the only implementation of "what is running" — it walks /proc/*/exe
    # rather than matching a name, because the process holding the framebuffer is
    # usually the grandchild app_launcher started and its basename is in no config.
    if ssh "$DEVICE" '[ -x /etc/init.d/arca-app ]' 2>/dev/null; then
        info "Stopping the running app"
        ssh "$DEVICE" '/etc/init.d/arca-app stop' 2>&1 | sed 's/^/    /' || true
    else
        warn "no /etc/init.d/arca-app — run ./commissioning/provision.sh $DEVICE_IP first"
    fi

    echo ""
    if arca_bundle_install_ssh "$DEVICE" "$BUNDLE_DIR"; then
        ok "Bundle installed"
    else
        err "the bundle install failed — see the messages above"
    fi

    # default-app, then restart, exactly as the build path does below.
    echo ""
    info "Default → /opt/arca/app_launcher"
    ssh "$DEVICE" "mkdir -p /opt/arca && echo '/opt/arca/app_launcher' > /opt/arca/default-app"
    if ssh "$DEVICE" '[ -x /opt/arca/app_launcher ]' 2>/dev/null; then
        ok "Default app set and present"
    else
        warn "default-app names /opt/arca/app_launcher, which is NOT installed"
        warn "  the init script would respawn nothing — check the bundle's components"
    fi
    if ssh "$DEVICE" '[ -f /etc/init.d/arca-app ]' 2>/dev/null; then
        info "Starting the launcher"
        ssh "$DEVICE" '/etc/init.d/arca-app start' 2>&1 | grep -v '^$' | sed 's/^/    /'
        ok "Launcher running"
    fi

    echo ""
    ok "Installed to $DEVICE_IP from $(basename "$FROM_BUNDLE") — nothing was built"
    _END_SECONDS=$(date +%s)
    _ELAPSED=$((_END_SECONDS - _START_SECONDS))
    printf "[$(date '+%H:%M:%S')] Total time: %dm%02ds\n" $((_ELAPSED / 60)) $((_ELAPSED % 60))
    echo ""
    exit 0
fi

# ── the SSH gate, ONCE, before the component loop ──────────────────────────
#
# The component scripts each have their own gate — they are meant to run standalone —
# but if the key is missing, going through this one first means the offer is made
# once instead of once per component, and the loop below never stops to prompt.
# Nothing else here needs a device, so this is also the earliest point a wrong IP can
# be reported.
arca_ssh_gate "root@${DEVICE_IP}" || err "Cannot continue without SSH to root@${DEVICE_IP}"
ok "SSH OK"
echo ""

# ── the host gate, before anything is built or uploaded ────────────────────
#
# Until this call, no build or deploy path in the repo ran a test.  The twelve
# suites in tests/ and the host-gcc regressions under native_apps/tests/
# were each an invocation someone had to remember, so a change could reach a
# device having been graded by nobody.
#
# --scope=deploy omits doc_check.sh: it grades documentation and cannot affect
# a deployed byte.  Measured 2026-09-08 on this host: ~150 s for what remains,
# less than one ScummVM rebuild.  It runs AFTER the SSH gate above so a wrong
# IP still fails in a second rather than in three minutes.
#
# ⚠️ Exit 2 is not "a test failed" — it means the gate could not judge (an
# empty subject list, a missing tool, a suite that itself exited 2).  That is a
# worse reason to deploy than a red test, so it stops here too.
if [[ $SKIP_TESTS -eq 1 ]]; then
    warn "host gate SKIPPED (--skip-tests) — nothing has graded this tree"
else
    echo "Host gate: tests/run-all.sh --scope=deploy  (~150 s; --skip-tests to bypass)"
    gate_rc=0
    bash "$SCRIPT_DIR/tests/run-all.sh" --scope=deploy || gate_rc=$?
    case "$gate_rc" in
        0) ok "host gate PASS" ;;
        2) err "host gate could not judge (exit 2) — fix the harness, then deploy" ;;
        *) err "host gate FAILED — refusing to deploy. Override with --skip-tests." ;;
    esac
fi
echo ""

# Collect components
mapfile -t ALL_COMPONENTS < <(discover_components)

if [[ ${#ALL_COMPONENTS[@]} -eq 0 ]]; then
    err "No components found (no */build-and-deploy.sh scripts)"
fi

# Filter to a single component if requested
if [[ -n "$FILTER" ]]; then
    found=false
    for c in "${ALL_COMPONENTS[@]}"; do
        if [[ "$c" == "$FILTER" ]]; then
            found=true
            break
        fi
    done
    if ! $found; then
        err "Unknown component: $FILTER\n  Available: ${ALL_COMPONENTS[*]}"
    fi
    ALL_COMPONENTS=("$FILTER")
    info "Deploying single component: $FILTER"
else
    info "Deploying ${#ALL_COMPONENTS[@]} component(s): ${ALL_COMPONENTS[*]}"
fi
echo ""

# ── deploy each component ──────────────────────────────────────────────────
SUCCEEDED=()
FAILED=()

for comp in "${ALL_COMPONENTS[@]}"; do
    DEPLOY_SCRIPT="$SCRIPT_DIR/$comp/build-and-deploy.sh"

    echo "════════════════════════════════════════"
    info "Component: $comp"
    echo "════════════════════════════════════════"
    echo ""

    if [[ ! -x "$DEPLOY_SCRIPT" ]]; then
        chmod +x "$DEPLOY_SCRIPT"
    fi

    # Run the component's build-and-deploy with the device IP.
    # Each script handles its own build, upload, manifest, and restart.
    if (cd "$SCRIPT_DIR/$comp" && bash build-and-deploy.sh "$DEVICE_IP"); then
        SUCCEEDED+=("$comp")
        ok "$comp deployed successfully"
    else
        FAILED+=("$comp")
        echo -e "${RED}  ✗ $comp FAILED${NC}"
        warn "Continuing with remaining components..."
    fi
    echo ""
done

# ── the provenance stamp is cleared by the COMPONENT scripts, not here ──────
# /opt/arca/bundle.info names the release a unit was installed from, so a SOURCE
# deploy — which overwrites the binaries it describes — must not leave it standing.
# This used to be one `ssh rm -f` here, after the loop, which meant `deploy-all.sh <ip>`
# cleared the stamp and a single component's build-and-deploy.sh did not: a unit could
# carry a stamp naming a release whose bytes had been overwritten one component at a
# time. Each component now calls arca_bundle_clear_stamp right after its own SSH gate, so
# every path into the device is covered by one writer and this needs nothing.
#
# ── set default app (native_apps/app_launcher) ─────────────────────────────
# Only set-default if native_apps was deployed (or we're deploying everything)
DEVICE="root@${DEVICE_IP}"

set_default=false
for c in "${SUCCEEDED[@]}"; do
    [[ "$c" == "native_apps" ]] && set_default=true
done

if $set_default; then
    echo "════════════════════════════════════════"
    echo "  Setting default app"
    echo "════════════════════════════════════════"
    echo ""

    info "Default → /opt/arca/app_launcher"
    ssh "$DEVICE" "mkdir -p /opt/arca && echo '/opt/arca/app_launcher' > /opt/arca/default-app"
    ok "Default app set"

    # Restart the init service so it picks up the new default
    if ssh "$DEVICE" '[ -f /etc/init.d/arca-app ]' 2>/dev/null; then
        info "Restarting app launcher..."
        ssh "$DEVICE" '/etc/init.d/arca-app start' 2>&1 | grep -v '^$'
        ok "Launcher running"
    fi
    echo ""
fi

# ── summary ─────────────────────────────────────────────────────────────────
echo "╔═══════════════════════════════════════╗"
echo "║  Deploy Summary                       ║"
echo "╚═══════════════════════════════════════╝"
echo ""

if [[ ${#SUCCEEDED[@]} -gt 0 ]]; then
    echo -e "  ${GREEN}Succeeded:${NC} ${SUCCEEDED[*]}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "  ${RED}Failed:${NC}    ${FAILED[*]}"
fi
echo ""

if [[ ${#FAILED[@]} -gt 0 ]]; then
    warn "${#FAILED[@]} component(s) failed. Check output above for details."
    echo ""
    exit 1
fi

ok "All ${#SUCCEEDED[@]} component(s) deployed to $DEVICE_IP"
_END_SECONDS=$(date +%s)
_ELAPSED=$((_END_SECONDS - _START_SECONDS))
printf "[$(date '+%H:%M:%S')] Total time: %dm%02ds\n" $((_ELAPSED / 60)) $((_ELAPSED % 60))
echo ""
