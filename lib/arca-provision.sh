#!/bin/bash
#
# lib/arca-provision.sh — read device-files/provision-rules.conf, compile it into a plan,
#                   and apply that plan either OFFLINE (to a mounted card) or LIVE
#                   (as an interpreter piped to the device over ssh).
#
# SOURCED, not executed:   . "$REPO_ROOT/lib/arca-provision.sh"
#                          (needs lib/arca-identify.sh sourced first — arca_offline_path does the
#                          p3/p2 mapping and arca_offline_base_ok is the guard)
#
# ── The decisions are not in this file ──────────────────────────────────────
#
# Every file, link, mode and config edit lives in device-files/provision-rules.conf
# with a reason per entry, read by BOTH consumers:
#
#   provision.sh           live, over SSH
#   rootfs/commission-offline.sh  offline, card in a reader
#
# so the two cannot drift. They HAD drifted: the online path removed stale rc*.d
# links before relinking and the offline path did not.
#
# ── Two executors, and why the online one is a generated script ─────────────
#
# The offline executor is a shell function here. The online one CANNOT be, because
# the work happens on the far side of an ssh pipe — so arca_provision_online_script
# emits the interpreter as text and provision.sh pipes it to `ssh <t> sh -s`
# together with the plan. Consequences worth knowing:
#
#   - It is /bin/sh for BusyBox ash, not bash. No [[, no arrays, no local.
#   - `install` is the one verb it cannot do alone: the source bytes are on the
#     host. provision.sh scp's them first and the interpreter only chmods.
#   - It honours $ARCA_PROVISION_ROOT, which is "" on a device. That is what lets
#     tests/arca_provision_test.sh group E run it against a copied tree and compare
#     its dry run with the offline one — a comparison of two executors rather than
#     of one executor and a wish.
#
# ── The plan format, which is the interface between the two ─────────────────
#
# Tab-separated, four fields, emitted in DEPENDENCY order (not file order):
#
#   unlink	-	<path-or-glob>	-
#   install	<mode>	<device-path>	<repo-relative-source>
#   link	-	<link-path>	<link-target>
#   link-opt	-	<link-path>	<link-target>
#
# unlink before link (a glob would eat the link just made), install before link (a
# link to a not-yet-written file dangles on a card). Paths are DEVICE-absolute; mapping them onto
# p3/p2 is the offline executor's job.

ARCA_PROVISION_TYPES="install link link-opt unlink"
ARCA_PROVISION_GROUPS_ALL="base mdns usb bluetooth backup"
ARCA_PROVISION_GROUPS_DEFAULT="base mdns usb bluetooth backup"
ARCA_PROVISION_GROUPS_OPTIONAL="mdns usb bluetooth backup"

arca_provision_default_groups()  { echo "$ARCA_PROVISION_GROUPS_DEFAULT"; }
arca_provision_optional_groups() { echo "$ARCA_PROVISION_GROUPS_OPTIONAL"; }

# ---------------------------------------------------------------------------
# arca_provision_rules_file
#
# Resolved from this file's own location, so a caller in a subdirectory or one
# invoked through a symlink still finds it.
# ---------------------------------------------------------------------------
arca_provision_rules_file() {
    local d
    d=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)
    echo "$d/device-files/provision-rules.conf"
}

# ---------------------------------------------------------------------------
# arca_provision_parse FILE
#
# Echo "<type>\t<group>\t<mode>\t<target>\t<source>" per record, dropping the
# reason. A tab inside the reason must not shift the first five fields, so the
# reason is "field 6 onwards" rather than "field 6".
# ---------------------------------------------------------------------------
arca_provision_parse() {
    [ -f "$1" ] || { echo "arca_provision_parse: no such file: $1" >&2; return 1; }
    awk -F'\t' '
        /^[ \t]*#/ { next }
        /^[ \t]*$/ { next }
        NF >= 6    { printf "%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5 }
    ' "$1"
}

# ---------------------------------------------------------------------------
# arca_provision_validate FILE [REPO_ROOT]
#
# Echo every problem; return 1 if there were any.
# ---------------------------------------------------------------------------
arca_provision_validate() {
    local file="$1" repo="${2:-}" out
    [ -f "$file" ] || { echo "  no such file: $file"; return 1; }
    if [ -z "$repo" ]; then
        repo=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)
    fi

    out=$(awk -F'\t' -v types="$ARCA_PROVISION_TYPES" -v groups="$ARCA_PROVISION_GROUPS_ALL" \
              -v repo="$repo" '
        BEGIN {
            n = split(types, t, " ");  for (i = 1; i <= n; i++) TYPE[t[i]] = 1
            n = split(groups, g, " "); for (i = 1; i <= n; i++) GROUP[g[i]] = 1
            # Which types use which columns. Everything else must be "-", because a
            # value in a column the type ignores is a value somebody expected to
            # take effect.
            WANTMODE["install"] = 1
            WANTSRC["install"] = 1; WANTSRC["link"] = 1; WANTSRC["link-opt"] = 1
        }
        /^[ \t]*#/ { next }
        /^[ \t]*$/ { next }
        {
            records++
            if (NF < 6) {
                # Also catches a space-separated line, which arrives as NF == 1.
                printf "  line %d: %d tab-separated field(s), need 6 (<type> <group> <mode> <target> <source> <reason>): %s\n", NR, NF, $0
                bad++; next
            }
            type = $1; group = $2; mode = $3; target = $4; src = $5
            for (i = 1; i <= 6; i++) {
                if ($i == "") { printf "  line %d: field %d is empty (use \"-\" for not-applicable)\n", NR, i; bad++; empty = 1 }
            }
            if (empty) { empty = 0; next }

            if (!(type in TYPE))   { printf "  line %d: unknown record type \"%s\"\n", NR, type; bad++; next }
            if (!(group in GROUP)) { printf "  line %d: unknown group \"%s\"\n", NR, group; bad++ }

            # ── target ──
            if (target !~ /^\//)   { printf "  line %d: target is not absolute: %s\n", NR, target; bad++ }
            if (target ~ /(^|\/)\.\.(\/|$)/) { printf "  line %d: target contains \"..\": %s\n", NR, target; bad++ }
            if (target ~ /\/\//)   { printf "  line %d: target contains \"//\": %s\n", NR, target; bad++ }
            if (target ~ /\/$/)    { printf "  line %d: target has a trailing slash: %s\n", NR, target; bad++ }
            if (target == "/")     { printf "  line %d: target is \"/\"\n", NR; bad++ }

            # ⚠️ rc0.d and rc6.d are SHUTDOWN, not startup. Unreachable through this
            # file by construction, the same guarantee as p1s absence from ARCA_PART_ROLES.
            if (target ~ /\/rc[06]\.d(\/|$)/) {
                printf "  line %d: rc0.d and rc6.d are shutdown, not startup — no rule may name them: %s\n", NR, target
                bad++
            }

            # A glob is allowed only in the LAST component: the executors quote the
            # directory part so a base containing a space still resolves, which means
            # a mid-path glob would be taken literally and match nothing, silently.
            dir = target; sub(/\/[^\/]*$/, "", dir)
            if (dir ~ /[*?[]/) {
                printf "  line %d: a glob is only allowed in the last component: %s\n", NR, target
                bad++
            }
            if (type != "unlink" && target ~ /[*?]/) {
                printf "  line %d: only unlink may take a glob target: %s\n", NR, target
                bad++
            }

            # ── mode: DECLARED, never read off disk ──
            # /mnt/c reports every file 0777 and discards chmod, so a mode taken from
            # the source on the dev host would be a constant, not a measurement.
            if (type in WANTMODE) {
                if (mode !~ /^[0-7][0-7][0-7][0-7]?$/) {
                    printf "  line %d: %s needs a declared octal mode, got \"%s\"\n", NR, type, mode
                    bad++
                }
            } else if (mode != "-") {
                printf "  line %d: %s ignores the mode column, so it must be \"-\", not \"%s\"\n", NR, type, mode
                bad++
            }

            # ── source ──
            if (type in WANTSRC) {
                if (src == "-") { printf "  line %d: %s needs a source\n", NR, type; bad++; next }
            } else if (src != "-") {
                printf "  line %d: %s ignores the source column, so it must be \"-\", not \"%s\"\n", NR, type, src
                bad++
            }

            if (type == "install") {
                if (src ~ /^\//) {
                    printf "  line %d: an install source is repo-relative, not absolute: %s\n", NR, src
                    bad++
                } else if (src ~ /(^|\/)\.\.(\/|$)/) {
                    printf "  line %d: install source contains \"..\": %s\n", NR, src
                    bad++
                } else if (system("test -f " repo "/" src) != 0) {
                    printf "  line %d: install source is not in the repo: %s\n", NR, src
                    bad++
                }
            }

            # ⚠️ A link source must be RELATIVE. An absolute symlink target is correct
            # on a running device and DANGLING on a mounted card, where /etc lives at
            # $BASE/root/etc — and a dangling rc5.d link is skipped in silence at boot.
            # This is the one defect this file could introduce that nothing downstream
            # would catch.
            if (type == "link" || type == "link-opt") {
                if (src ~ /^\//) {
                    printf "  line %d: a link source must be RELATIVE (it dangles on a mounted card otherwise): %s\n", NR, src
                    bad++
                }
            }

        }
        END {
            if (records == 0) { print "  no records at all — every line is a comment or blank"; bad++ }
            exit(bad > 0 ? 1 : 0)
        }
    ' "$file")

    if [ -n "$out" ]; then
        printf '%s\n' "$out"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# arca_provision_plan FILE GROUPS
#
# Compile FILE into a plan for the enabled GROUPS (space-separated, must include
# "base"), emitted in dependency order.
#
# A disabled group simply contributes nothing: not installing a file has no
# counterpart that could then remove it by surprise.
# ---------------------------------------------------------------------------
arca_provision_plan() {
    local file="$1" groups="$2" g found

    [ -f "$file" ] || { echo "arca_provision_plan: no such file: $file" >&2; return 1; }

    case " $groups " in
        *" base "*) ;;
        *) echo "arca_provision_plan: the group list must include 'base' (it cannot be switched off)" >&2
           return 1 ;;
    esac
    for g in $groups; do
        found=0
        case " $ARCA_PROVISION_GROUPS_ALL " in *" $g "*) found=1 ;; esac
        [ "$found" = 1 ] || { echo "arca_provision_plan: unknown group '$g'" >&2; return 1; }
    done

    if ! arca_provision_validate "$file" >/dev/null; then
        echo "arca_provision_plan: $file does not validate:" >&2
        arca_provision_validate "$file" >&2
        return 1
    fi

    _arca_provision_emit "$file" "$groups"
}

# ---------------------------------------------------------------------------
# arca_provision_plan_component FILE GROUP
#
# Compile ONLY GROUP's records — for a COMPONENT deploy script that installs its
# own group's verbatim files standalone, without provisioning a whole device.
# usb_host/build-and-deploy.sh (usb) and bluetooth/build-and-deploy.sh (bluetooth) call it.
#
# ⚠️ A separate entry point with a separate name rather than a flag on
# arca_provision_plan, so that a commissioning path cannot reach a base-less plan by
# mistyping a group list — and it refuses `base` for the mirror-image reason: the
# base group is a whole device's boot service, not one component's payload.
#
# What this buys: the component script does not restate its own install records,
# modes or boot links. Before this it carried its own scp/chmod/ln -sf sequence,
# which is precisely the drift one shared plan compiler exists to remove — and it HAD drifted, in that
# it installed the module loader as /etc/init.d/S89xpad-modules while the rest of
# the repo names init scripts after what they are.
# ---------------------------------------------------------------------------
arca_provision_plan_component() {
    local file="$1" group="$2" found=0

    [ -f "$file" ]  || { echo "arca_provision_plan_component: no such file: $file" >&2; return 1; }
    [ -n "$group" ] || { echo "arca_provision_plan_component: no group" >&2; return 1; }

    case " $ARCA_PROVISION_GROUPS_OPTIONAL " in *" $group "*) found=1 ;; esac
    if [ "$found" != 1 ]; then
        echo "arca_provision_plan_component: '$group' is not an optional group ($ARCA_PROVISION_GROUPS_OPTIONAL)" >&2
        return 1
    fi

    if ! arca_provision_validate "$file" >/dev/null; then
        echo "arca_provision_plan_component: $file does not validate:" >&2
        arca_provision_validate "$file" >&2
        return 1
    fi

    _arca_provision_emit "$file" "$group"
}

# ---------------------------------------------------------------------------
# _arca_provision_emit FILE GROUPS
#
# The ordering, in ONE place. ⚠️ Order is emitted here, not read from the file, so
# a rule added in the wrong place in the data file cannot produce a plan that
# installs after it links.
# ---------------------------------------------------------------------------
_arca_provision_emit() {
    local file="$1" groups="$2"
    arca_provision_parse "$file" | awk -F'\t' -v enabled=" $groups " '
        function on(g) { return index(enabled, " " g " ") > 0 }
        {
            if (!on($2)) next
            rec = $1 "\t" $3 "\t" $4 "\t" $5
            if      ($1 == "unlink")    unl[++nu]  = rec
            else if ($1 == "install")   ins[++ni]  = rec
            else if ($1 == "link")      lnk[++nl]  = rec
            else if ($1 == "link-opt")  lnk[++nl]  = rec
        }
        END {
            for (i = 1; i <= nu; i++) print unl[i]   # stale links first
            for (i = 1; i <= ni; i++) print ins[i]   # then the payload
            for (i = 1; i <= nl; i++) print lnk[i]   # link only what exists
        }'
}

# ---------------------------------------------------------------------------
# arca_provision_canonical
#
# Filter: stdin is either executor's dry-run output, stdout is the comparable set.
#
# ⚠️ This is what makes "one list, two executors" checkable rather than intended.
# Both executors print
#
#     <verb> <mode> <device-path> <source>    -> <resolved-host-path>
#
# and this strips the resolved path, because that is the ONE thing that legitimately
# differs (`/etc/...` on a device, `$BASE/root/etc/...` offline). Everything to the
# left must match exactly, so a wrong mode, a wrong source or a dropped record all
# show up as a diff.
#
# ⚠️ The separator before `->` is a TAB, not spaces. `s/ *->.*//` silently strips
# nothing and every line then carries its own host path, so the two sets differ on
# all of them and the diff says "1,31c1,31" — which reads like a total mismatch
# rather than a broken filter.
# ---------------------------------------------------------------------------
arca_provision_canonical() {
    sed -n 's/^  would \([a-z-]*\) *//p' | sed 's/[[:space:]]*->.*$//; s/[[:space:]]*$//' | LC_ALL=C sort
}

# ---------------------------------------------------------------------------
# arca_provision_apply_offline BASE PLANFILE REPO_ROOT
#
# The OFFLINE executor. ARCA_PROVISION_DRY=1 resolves and prints everything and
# changes nothing.
# ---------------------------------------------------------------------------
arca_provision_apply_offline() {
    local base="$1" plan="$2" repo="$3"
    local kind mode target src dest hostdir name m rc=0

    arca_offline_base_ok "$base" || return 1
    [ -f "$plan" ] || { echo "arca_provision_apply_offline: no such plan: $plan" >&2; return 1; }
    [ -d "$repo" ] || { echo "arca_provision_apply_offline: no such repo root: $repo" >&2; return 1; }

    # resolve <device-path> -> host path, refusing anything that lands outside base.
    _arcap_resolve() {
        local dev="$1" out
        out=$(arca_offline_path "$base" "$dev") || return 1
        case "$out/" in
            "${base%/}"/*) ;;
            *) echo "  refusing $dev — resolved to $out, outside $base" >&2; return 1 ;;
        esac
        printf '%s\n' "$out"
    }

    while IFS=$'\t' read -r kind mode target src; do
        [ -n "$kind" ] || continue
        case "$kind" in
            unlink)
                hostdir=$(_arcap_resolve "${target%/*}") || { rc=1; continue; }
                name="${target##*/}"
                # ⚠️ A dry run prints the RECORD, once, and never the per-match
                # expansion. What matched is card state, not plan — and printing
                # both makes the two executors' sets differ by whatever happens to
                # be on this particular card, which is exactly the asymmetry the
                # comparison exists to detect.
                if [ -n "${ARCA_PROVISION_DRY:-}" ]; then
                    printf '  would unlink    -     %s\t-\t-> %s/%s\n' "$target" "$hostdir" "$name"
                    continue
                fi
                # Only the last component is unquoted, so the glob expands and a base
                # containing a space still resolves. -e is false for a dangling
                # symlink and a dangling symlink is exactly what an offline tool
                # sees, so test -L too.
                for m in "$hostdir"/$name; do
                    [ -e "$m" ] || [ -L "$m" ] || continue
                    printf '  unlink          %s\n' "$m"; rm -f "$m"
                done
                ;;
            install)
                dest=$(_arcap_resolve "$target") || { rc=1; continue; }
                if [ ! -f "$repo/$src" ]; then
                    echo "  MISSING source $repo/$src" >&2; rc=1; continue
                fi
                if [ -n "${ARCA_PROVISION_DRY:-}" ]; then
                    printf '  would install   %-5s %s\t%s\t-> %s\n' "$mode" "$target" "$src" "$dest"
                else
                    mkdir -p "$(dirname "$dest")"
                    cp "$repo/$src" "$dest" && chmod "$mode" "$dest" \
                        && printf '  install         %-5s %s\n' "$mode" "$dest" || rc=1
                fi
                ;;
            link|link-opt)
                dest=$(_arcap_resolve "$target") || { rc=1; continue; }
                if [ -n "${ARCA_PROVISION_DRY:-}" ]; then
                    printf '  would %-9s -     %s\t%s\t-> %s\n' "$kind" "$target" "$src" "$dest"
                else
                    mkdir -p "$(dirname "$dest")"
                    # link-opt: skip rather than dangle. The link target is relative
                    # to the link's own directory, so resolve it there.
                    if [ "$kind" = "link-opt" ] && [ ! -e "$(dirname "$dest")/$src" ]; then
                        printf '  skip link-opt   %s (target %s is absent on this image)\n' "$target" "$src"
                        continue
                    fi
                    ln -sf "$src" "$dest" && printf '  link            %s -> %s\n' "$dest" "$src" || rc=1
                fi
                ;;
            *) echo "  unknown plan verb: $kind" >&2; rc=1 ;;
        esac
    done < "$plan"

    unset -f _arcap_resolve
    return "$rc"
}

# ---------------------------------------------------------------------------
# arca_provision_plan_summary PLAN
#
# One line naming every record type present, counted from the plan. Computed, so it
# cannot drift from the plan or from a new record type: a hand-rolled count once
# accounted for 27 of 35 actions and hid the verbs it left out.
# ---------------------------------------------------------------------------
arca_provision_plan_summary() {
    local plan="$1"
    [ -f "$plan" ] || { echo "arca_provision_plan_summary: no such plan: $plan" >&2; return 1; }
    awk -F'\t' '
        NF { n[$1]++; total++ }
        END {
            # Emitted order, so the summary reads in the order the plan runs.
            split("unlink install link link-opt", o, " ")
            out = ""
            for (i = 1; i in o; i++) if (o[i] in n) {
                out = out (out == "" ? "" : ", ") n[o[i]] " " o[i]
                seen[o[i]] = 1
            }
            # A type the list above does not know about must still be counted, or
            # this line goes back to being an incomplete summary.
            for (k in n) if (!(k in seen)) out = out (out == "" ? "" : ", ") n[k] " " k
            printf "%d action(s) — %s\n", total, out
        }' "$plan"
}

# ---------------------------------------------------------------------------
# arca_provision_push_installs PLAN REPO_ROOT TARGET
#
# Put every `install` record's SOURCE bytes onto the device, ahead of the live
# executor — which can only set the declared mode, because the bytes live on the
# host. The one verb the generated interpreter cannot do alone.
#
# ⚠️ **The plan is read on fd 3, never on stdin.** `ssh` reads its own stdin and
# forwards it to the remote command, so `while read … done < "$PLAN"` with an ssh
# in the body loses the entire rest of the plan to the FIRST ssh: one file copied,
# seven missing, and the executor then correctly refusing on the seven. That was
# that defect, and it is why this function exists. `ssh -n` would fix today's body and not
# tomorrow's — fd 3 is a property of the loop, so a second stdin-reading command
# added here cannot reintroduce it.
#
# One implementation for both callers — provision.sh (the whole
# plan) and usb_host/build-and-deploy.sh (the usb group). They had a verbatim copy
# each, so they had the defect twice; and $ARCA_SSH/$ARCA_SCP (the convention
# lib/arca-bundle.sh's arca_bundle_install_ssh uses too) is what lets the regression
# drive this real code with no device.
# ---------------------------------------------------------------------------
arca_provision_push_installs() {
    local plan="$1" repo="$2" target="$3"
    local kind mode tgt src dir want got=0

    [ -f "$plan" ] || { echo "arca_provision_push_installs: no such plan: $plan" >&2; return 1; }
    [ -d "$repo" ] || { echo "arca_provision_push_installs: no such repo root: $repo" >&2; return 1; }
    [ -n "$target" ] || { echo "arca_provision_push_installs: no target given" >&2; return 1; }
    # Counted the way the loop below selects — `grep -c '^install'` would also match
    # a future `install-opt`, and an inflated want would fail a correct run.
    want=$(awk -F'\t' '$1 == "install"' "$plan" | wc -l | tr -d ' ')

    while IFS=$'\t' read -r kind mode tgt src <&3; do
        [ "$kind" = install ] || continue
        [ -f "$repo/$src" ] || { echo "  missing $repo/$src" >&2; return 1; }
        dir="${tgt%/*}"
        ${ARCA_SSH:-ssh} "$target" "mkdir -p '$dir'" \
            || { echo "  could not create $dir on $target" >&2; return 1; }
        ${ARCA_SCP:-scp} -q "$repo/$src" "$target:$tgt" \
            || { echo "  could not copy $src to $tgt" >&2; return 1; }
        printf '  copied          %-5s %s -> %s\n' "$mode" "$src" "$tgt"
        got=$((got + 1))
    done 3< "$plan"

    # The count IS the check. The stdin-eating defect above was silent at this line
    # and loud six lines later
    # in the executor, which is what made it read as an executor bug; a fix that is
    # supposed to reach 8 of 8 has to say so where the copying happens.
    if [ "$got" != "$want" ]; then
        echo "arca_provision_push_installs: copied $got of $want install record(s)" >&2
        echo "    something in the loop body consumed the plan — an ssh or other stdin reader" >&2
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# arca_provision_online_script
#
# Echo the LIVE executor as text, for `ssh <target> sh -s -- /tmp/arca-provision-plan`.
#
# ⚠️ /bin/sh for BusyBox ash: no [[, no arrays, no local, no `sed -E` guarantees
# beyond what busybox provides (it has -E). $ARCA_PROVISION_ROOT is "" on a device
# and is set only by the test harness, which is what lets the same interpreter be
# compared against the offline one.
#
# `install` arrives already scp'd — the caller puts the bytes in place and this only
# sets the mode, because the source lives on the host.
# ---------------------------------------------------------------------------
arca_provision_online_script() {
    cat <<'ONLINE'
# arca-provision live executor. Generated by arca_provision_online_script; do not edit
# on the device. R is "" in production and a test root in the harness.
PLAN="$1"
R="${ARCA_PROVISION_ROOT:-}"
DRY="${ARCA_PROVISION_DRY:-}"
rc=0

while IFS='	' read -r kind mode target src; do
    [ -n "$kind" ] || continue
    dest="$R$target"
    case "$kind" in
    unlink)
        d="$R${target%/*}"; n="${target##*/}"
        if [ -n "$DRY" ]; then
            # The record, once — never the per-match expansion. See the offline half.
            printf '  would unlink    -     %s\t-\t-> %s/%s\n' "$target" "$d" "$n"
        else
            for m in "$d"/$n; do
                [ -e "$m" ] || [ -L "$m" ] || continue
                printf '  unlink          %s\n' "$m"; rm -f "$m"
            done
        fi
        ;;
    install)
        # The bytes were scp'd by the caller; only the mode is ours to set.
        if [ -n "$DRY" ]; then
            printf '  would install   %-5s %s\t%s\t-> %s\n' "$mode" "$target" "$src" "$dest"
        elif [ -f "$dest" ]; then
            chmod "$mode" "$dest" && printf '  install         %-5s %s\n' "$mode" "$dest" || rc=1
        else
            echo "  MISSING $dest — it should have been copied before this ran" >&2; rc=1
        fi
        ;;
    link|link-opt)
        if [ -n "$DRY" ]; then
            printf '  would %-9s -     %s\t%s\t-> %s\n' "$kind" "$target" "$src" "$dest"
        else
            mkdir -p "${dest%/*}"
            if [ "$kind" = "link-opt" ] && [ ! -e "${dest%/*}/$src" ]; then
                printf '  skip link-opt   %s (target %s is absent on this image)\n' "$target" "$src"
            else
                ln -sf "$src" "$dest" && printf '  link            %s -> %s\n' "$dest" "$src" || rc=1
            fi
        fi
        ;;
    *) echo "  unknown plan verb: $kind" >&2; rc=1 ;;
    esac
done < "$PLAN"
exit "$rc"
ONLINE
}
