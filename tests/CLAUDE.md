# tests/CLAUDE.md

The host-side regressions. Loaded when you work in `tests/`.

There is **no CI** — nothing runs on push — but there is a runner and a lint gate: `run-all.sh`, below.
"Tests" are three disjoint things: host-gcc
regressions over pure-logic functions (`native_apps/tests/*_test.c`), shell regressions over host
tooling (`tests/*_test.sh`, run directly), and interactive on-device diagnostic tools. Nothing here
needs a device; several need `sudo`, and each says so in its own header.

**Case counts live in each suite's own header and output, never in prose.** A number written into a
doc is a number a later session carries stale — three in-tree records claimed one suite had 163, 155 and
94 cases and disagreed with each other. Run the suite and read the count. (For the record: `163` was the
one that was right, measured 2026-08-15 — which is the point. You cannot tell which of three prose
numbers is current without running it, so do not write a fourth.)

## `run-all.sh` — the host gate

**One command runs every test that needs no device, and `deploy-all.sh` and `release.sh` both run it and
refuse to build if it fails** (`--skip-tests` opts out). Three phases, each driven by a data table in the
script: the shell suites here, the host-gcc regressions under `native_apps/tests/` (plus a `-fsyntax-only` pass over `control_panel/*.c`, read from `build-and-deploy.sh`; warnings do not fail it, as in the ARM build), and `shellcheck`
over every tracked `*.sh`. `--list` prints the tables; `--scope=deploy` omits `doc_check.sh`, which grades
documentation and cannot change a deployed byte; `--self-test` runs the negative controls. Exit 0 pass,
1 a subject failed, **2 the gate could not judge** — an empty subject list, a required tool absent, a
`*_test.c` in neither table, or a suite that itself exited 2. Two is deliberately not "a test failed": a
gate that inspected nothing is a worse reason to deploy than a red one. Its cost, and which suite
dominates it, are in the script's own header, measured.

⚠️ **A SKIP IS NOT A PASS, and grading on exit status alone makes it one — in EVERY phase.**
`commission_offline_test.sh` exits 0 when it cannot get root, so skips are counted and named apart from
passes. Phase 2 carried no such branch until a host C regression needed a facility only root has here:
`gamepad_announce_test.c` synthesises an evdev node through **this host's** `/dev/uinput`, so on a normal
run it skips, and exit 0 alone would have reported it as having tested its subject. (That says nothing
about the device, which has no `/dev/uinput` and where writing to an event node still delivers nothing.)
The detection reads the RUN output only, never the build log above it, so a compiler warning quoting a
source comment cannot mark a test skipped; a phase-2 exit of 2 is a harness error on the same footing as
a suite's. ⚠️ **A phase-2 test signals a skip by printing `skip` as the FIRST token on a line**, and the
pattern is anchored on that: matching the word anywhere reported three PASSING tests as skipped, because
they name cases *"a middle gap is skipped"*, *"the RIFF pad byte is skipped"*. Phase 1's guard — no
`N passed, M failed` line — cannot be borrowed, because these tests print four different verdict shapes
and none is that one, so the test is vacuously true here. **Understating coverage is the same defect as
absorbing a skip**, and the control for that direction is a stub that passes while saying the word.

⚠️ **A phase-2 control needs a whole miniature `native_apps/` tree, and three things make that fixture
lie.** `phase_ctests` takes the tree it compiles in from the parent of `ARCA_CTEST_DIR`, so the override
redirects the build and not merely the subject scan — otherwise a control points at a fixture and grades
the real sources. A stub C source must be written with a quoted heredoc, because `bash` `printf` turns
the `\n` inside a C string literal into a real newline that will not compile. And every stub needs
`#undef main`, because one row passes a `-Dmain=` that renames the stub's own entry point. The control
that catches all three is **the unmodified fixture must pass**: without it, the skip control passed while
the other fourteen rows were silently failing to build.

⚠️ **Match a suite's output on a DECOLOURISED copy.** The suites colour their own words, so that skip
arrives as `ESC[1;33mskip ESC[0m` — the character before `skip` is `m`, and a pattern anchored on
`[^a-z]` cannot match it. That is how this gate reported a skipped suite as a pass while a self-test
control built on a plain-text fixture said the detection worked: **a fixture cleaner than reality proves
nothing.** The ratchet control sank the same way — `shellcheck` 0.7.0 emits nothing at all for
`f=x` then `echo $f`, because it can prove the value safe, so a fixture written that way is clean and the
control passes having exercised no path. Use positional parameters, whose expansion it cannot prove safe.

⚠️ **Where a gate call is INSERTED is load-bearing, and a suite is what catches it wrong.** Put the call
ahead of a script's cheap local refusals and it preempts them: in `release.sh` it briefly sat before the
`--out` checks, `arca_release_test.sh` runs that script in a temp tree with no `tests/` directory, `bash`
returned 127, and the suite fell from all-green to 30 failures with its own A0 control declaring every
refusal beneath it unfalsifiable. **The call belongs after every local refusal and immediately before the
first build.** That suite's group D holds it there: it stubs the gate, dials the verdict in per case, and
witnesses `--skip-tests` with a marker file, because "the gate did not run" cannot be read off an exit
status that a passing gate produces too.

**`shellcheck` runs in two tiers, because the repo carries a backlog that is not this gate's to fix.**
Tier 1 is a hard zero: every finding at `error:` severity, plus every `SC11xx` — the family that means
shellcheck could not do its job. ⚠️ **`SC1124` makes the tool exit 1 having analysed NOTHING in that
file**, which reads exactly like a clean run that found one small problem, so a voided analysis must
never pass. Tier 2 is a ratchet over `tests/shellcheck-baseline.txt`, one row per `(file, code)` with its
count: a count going up, or a new pair, fails; a count going down passes and says the baseline can be
tightened. That ratchet is why **no shipped script carries a `disable=` directive for the backlog** —
suppressing it would add `SC1124` risk of its own — and why refreshing the baseline is a separate
`--only=shellcheck-baseline` mode, since a gate that refreshes its own baseline as a side effect can
never fail. Regenerate it deliberately, and only after reading what changed. Triaged 2026-10-04 and left as is (operator ruling): the remaining rows are deliberate or style, and none is a defect.
⚠️ **A `# shellcheck` comment whose first word is not followed by a valid directive voids the whole
file's analysis** (exit 1, `SC1072`/`SC1073`) — and *prose* is enough: a comment explaining a directive
and opening with the checker's name is parsed as one (measured: a file's count fell 69 → 2). A small
post-patch total is therefore not evidence; the expected per-code distribution is. A directive in front
of one `case` branch does it too — put it before the function. **`-x` changes nothing** (measured
byte-identical); `# shellcheck source-path=SCRIPTDIR` clears `SC1091` and is valid only above the first
command (four findings with it removed or moved below `set -e`, zero above; worked example at the top of
`native_apps/build-and-deploy.sh`). **Measure in the gate's shape** — `shellcheck -f gcc $(git ls-files --
'*.sh')` from the repo root — never one file alone: alone, it reports four `SC1091` the gate does not.

⚠️ **Running the whole gate as root does NOT reach phase 2 on this host, so a root-only C regression is
reached by compiling and running that one test as root** — its own header carries the command. Two
independent reasons, both artefacts of the shell rather than the repo: a host-only suite that exits 2 as
root, and phase 2 returning without printing a word while any harness error stands; then `git ls-files`
fails on dubious ownership, so phase 3 finds no scripts. **A phase that grades nothing prints nothing
here** — read the summary's harness-error count, never the absence of red rows.

## `doc_check.sh` — the documentation invariants

Host-only, no device, no toolchain. Each group has its own negative control; `--self-test` runs them all
and names what each one caught, so the run itself says how many groups there are.

**Group A — every markdown anchor resolves.** A cross-file `<doc>.md#<fragment>` and a same-file
`(#<fragment>)` alike must name a slugified heading that exists in the target. This is the check that
makes moving or retitling a section safe: ~120 of these anchors point into `SYSTEM_ANALYSIS.md` from
`.sh`, `.c`, `.py`, `.conf` and extensionless-shebang comments, which markdownlint never reads, so a retitled heading dangles
them silently. The slug rule was measured against anchors already in the tree, not assumed — every
character outside `[a-z0-9 _-]` is deleted and then spaces become hyphens, so an em-dash leaves **both**
its spaces behind and the slug gets a double hyphen. ⚠️ **It is fence-aware, and has to be**: this
repo's docs are full of `# comment` lines inside ```` ```bash ```` blocks, and counting one of those as a
heading is a false PASS — a dangling anchor that resolves. Two deliberate softenings, both documented in
the source: a path that does not exist beside the citing file is retried from the repo root (most of
these anchors are human pointers in comments, not links, and writing `../` into 25 of them to satisfy a
checker is worse than the check), and a fragment of 3 or 6 hex digits is skipped as a CSS colour
(`browser_games/README.md` documents its palette that way).

**Group B** — no file outside `IMPROVEMENT_PLAN.md` may cite a plan ID **at all**, resolving or not. The
plan is a queue whose entries are deleted once done, so every citation of one is a dangling reference
waiting to happen. `doc_check.sh` is exempt: it documents the shapes it searches for.

**Group C — extraction receipts, and it checks a MOVE rather than a copy.** Each row is a distinctive
token, the file that must now hold it, and the file it must have *left* (`-` for "no source to leave").
"One fact, one home" is the rule, so a token present in **both** files is the drift the cleanup exists to
remove, and the check fires on it. Run it before a deletion to confirm the destination has the fact, and
after to confirm it survived. ⚠️ **Three shapes of the same defect it cannot see, all under-reporting:**
a token can be present while the sentence around it is wrong (three of phase 2's 83 comments were exactly
that — the ID matched perfectly and the claim was stale); a fact extracted with no row here is invisible,
so a clean run means "every receipt I was told about holds", never "nothing was lost"; and it cannot tell
a paragraph. ⚠️ **Every key is a DURABLE token — an identifier, filename, constant, measured number or
board designator — and 14 of them used to be prose phrases** until 2026-08-19. That was the gate's own
advice being ignored by its own table, and it cost two sessions: rewording a sentence legitimately reads
as a lost fact, so the gate failed for a reason unrelated to the defect it exists to catch. Each new key
was measured present at its destination and absent from its source before it went in. When you add a row,
`grep -n` the destination for a `#define`, a function name, a `file.c:line` or a number in the same
paragraph — never key it on a sentence. `RECEIPTS_FILE` exists only so `--self-test` can drive the group
over a fixture table.

**Group D — a per-document ceiling counted in NON-BLANK lines, and the only group not about correctness.**
Every addition that grew the docs to 891, 2287 and 2214 lines was individually justified, so judging
additions one at a time does not bound the total; a ceiling does, because it is arithmetic and survives a
session that has forgotten the rule. When it fires there are exactly two answers: **pay for the addition**
(delete, or move the block to the document whose job it is) or **raise the ceiling in a commit that argues
for it** — ⚠️ silently raising it is the failure mode the group exists to catch. `/doc-update` is the
authoring half. ⚠️ **Blank lines are excluded because counting them made whitespace legal currency**: a
2-line overage paid off by deleting two blank lines passed the gate and broke the markdown, since a
column-0 paragraph that loses the blank line above it becomes a lazy continuation. The ceilings were
re-derived when the basis changed, preserving each file's margin exactly, so no file gained or lost
headroom. ⚠️ **Two things it still cannot see**: a paragraph rewrapped to 200 characters passes while
getting longer, and a missing file would pass as a skip — so a ceiling naming a path that is not there is
a **failure**, which matters because phase 5 split `HARDWARE.md` out of `SYSTEM_ANALYSIS.md`.

**Group E — structural markdown health, two defects that render as plausible markdown.** A file with no
trailing newline (`Edit`, `cat`, `head -N` splices and heredoc rewrites all drop it, and the next append
then lands *on* the last line rather than after it), and a column-0 paragraph directly after an **indented**
bullet, which CommonMark folds into that bullet. E1 found one live violation on its first run
(`scummvm-arca/SCUMMVM_DEV.md`, 2026-08-19), which is its positive control on the real tree; E2's is
in the fixture. ⚠️ Its blind spots all under-count, deliberately: a paragraph opening `*Note:*` reads as a
list marker, a column-0 `|` table row is not flagged, and the column-0-bullet case is out of scope because
it does not read as correct. `scummvm-icons/` is pruned from the walk for this group's sake — a vendored
upstream tree whose `.github` templates are not ours to fix (measured: 5 scannable files, 0 anchors).

⚠️ **It counted its own prose three times, in three different shapes** — a quoted heredoc fixture, then
the header sentence about bare IDs, then the header sentence about the no-`.md` citation form. Each one
inflated the number the run reported. **Both scans now skip `doc_check.sh` itself**, and its coverage
comes from the `--self-test` fixture, a throwaway tree that cannot be mistaken for a real finding. The
general form of this: a gate that documents the pattern it searches for will match its own
documentation, so **ask which part of the count is the harness before believing the count**.

⚠️ **Two scans, and a clean zero from either is evidence about what that pass looks at, not about the
repo.** The qualified form is `IMPROVEMENT_PLAN.md <ID>` with the `.md` optional; the bare form is
`see <ID>` and the parenthesised one. Both are per-line `awk`. When you add a scan, ask what it cannot see.

⚠️ **Group B has three blind shapes, and citations have lived in all three while both scans reported
zero.** Those were fixed by hand, so **the tree is at zero by measurement, not by gate:**

| blind shape | why |
|---|---|
| an ID **list** inside one marker | the bare scan consumes the delimiter and has no continuation loop; the qualified scan does have one |
| a citation **wrapped onto the next line** | both scans are per-line `awk`, so a line ending in the filename and an ID starting the next matches neither |
| plan vocabulary beside a bare ID — *the `<ID>` fix*, *Closes `<ID>`*, *`<ID>`'s window* | the marker set is `(`, `see`, `is`, `was` and their trailing space, deliberately so |

⚠️ **The other direction is a permanent false POSITIVE: the ID namespace collides with function-key names
and with these suites' own case labels.** `F1`–`F12` are keys, and the shell suites label cases in exactly
the plan's shape, so a parenthesised `F5` in a keybinding table is a group B hit with nothing wrong behind
it. **Disambiguate the sentence** — "that suite's cases B6/C3a/C5a" reads as a case label to a human and to
the scan alike — never delete a working pointer, and never exclude the file.

⚠️ **Do not answer this with a whole-tree ID-token census** — that collision means "every ID-shaped token
with no heading" returns hundreds of hits that cannot be triaged, one suite alone contributing ~70.
Anchor on **vocabulary** instead: the three shapes above gave 11 findings and **zero** false positives.
Recipe: `~/.claude/plans/peaceful-herding-valiant.md` → Phase 2b.

⚠️ **And the throwaway scanner that measured this became the TENTH self-count** — it sat at the repo
root, its header named a case label in parentheses to explain the collision, and group B counted it:
**PASS → FAIL (1)** on a tree whose real residue was zero. A one-off scanner belongs outside the scanned
tree, or its documentation does.

⚠️ **Deleting an entry can never move this group, and that is why it counts citations rather than
resolution.** A gate demanding every cited ID resolve makes closing an entry fail until its citations are
rewritten, so the cheapest way to stay green is to keep dead entries alive: the gate holds the queue open.

⚠️ **Write `<ID>`, not a real-looking one, whenever you describe these shapes in prose.** `doc_check.sh`
skips its own source, but it does **not** skip this file — so a parenthesised example spelled with a
plausible ID here is counted as a finding, and three of them in one sentence moved the reported number
41 → 44 on 2026-08-15. Then the sentence warning about it did it a fourth time, 42 → 41, which is the
whole lesson in one line. Same defect as the three the gate's own header records, one file over. **A gate
that documents the pattern it searches for will match its own documentation wherever that documentation
lives.** ⚠️ **Group A made that five and six on its first run, and then a seventh** — it reported the two
spelled-out example anchors this file and `CLAUDE.md` used while describing it, and the sentence written
to record *that* contained one more. **Bracket the *fragment*, always: `#<fragment>`, `<doc>.md#<fragment>`,
`<ID>`.** Bracketing only the filename half does not work — group A's path part matches the empty string,
so a bracketed filename in front of a bare `#`-plus-section-number is still a hit, which is how the count
went back to 1 three separate times while this was being written.

The bare scan matches only parenthetical and `see`/`is`/`was`-prefixed IDs, because a bare `B2` in prose
is indistinguishable from a register name — `vnc_client/deps/`'s vendored libjpeg-turbo is full of `B0`,
`B1`, `B8`. That directory is pruned, along with `scummvm/`, `scummvm-icons/`,
`usb_host/linux-4.14.52/` and the card captures — none of them our source, and most of them would
otherwise blow the walk's time budget.

## Writing a check

**Write the failing version first.** There is no CI, so a test that has only ever been seen passing is
not evidence that it can fail. Compile it against the pre-fix source and count the failures.

**Give every new check a negative control, and ask which part of the count is the harness.** A gate
that reports a number can be wrong in both directions, and this repo has hit both — a gate counting
artifacts it could not actually read (false negative), a `/proc` scanner counting its own `grep` argv
(false positive), and the `sdiv`/`udiv` gate firing ~9 times on a *stripped* binary and zero on the
identical file unstripped. **Put search patterns and multi-line harnesses in files, not in argv.** Skip
inputs your tool cannot inspect rather than passing them. **If a fix is supposed to drive a number to
zero, check that it reaches zero** — a small residue is the tell, both times it happened.

⚠️ **Extract the wiring under test; never restate it.** A decision that is unreachable by running its
script is tested by **extracting it from the shipped file** (by line range or function) and running it
against stubs. A harness that re-emits the assignment under test itself **repairs the sabotage it was
meant to catch**.

## What a suite cannot see

- ⚠️ **File modes are unobservable on this host.** `/mnt/c` is DrvFs 9p: it reports every file
  `-rwxrwxrwx` and silently discards `chmod`. A missing-`+x` bug can neither fire nor be demonstrated
  here, and you cannot build a negative control for one by `chmod`ing under `/mnt/c`. The offline
  installer's `+x` assertion is a real measurement only because it runs against ext4.
- ⚠️ **The card captures under `partitions/` and `partitions.new/` contain no symlinks at all.** They
  were copied through Windows, which dropped every one: `bin/sh` and `bin/busybox` are simply absent,
  and `etc/rc0.d` … `etc/rc6.d`, `etc/rcS.d` are all **empty directories**. Fine as a source for
  regular files; **worthless for any question about what starts at boot or where a symlink points.**
  Build fixtures for boot-link work **synthetically, with real symlinks**, and put them under `/tmp` —
  DrvFs cannot hold a symlink, which is why `tests/make-fake-card.sh` insists on it.
- ⚠️ **`dash -n` catches parse errors and CRLF, not bashisms.** `[[ -n "$x" ]]` parses fine under dash
  — `[[` is read as a command name — so it passes and then fails at boot with `[[: not found`.
- ⚠️ **A missing `arm-linux-gnueabihf-objdump` is a refusal, not a pass.** `check-arm-safe.sh` skips
  non-ARM files, so a run that inspected nothing must not report a clean bill. ⚠️ **And a stripped
  binary cannot be gated at all**: `objdump` needs the symbol table to tell Thumb-2 from ARM and
  invents `sdiv`/`udiv` without it, so the checker returns **2** ("could not judge"). ⚠️ **Never read
  that status through `xargs`** — it collapses any 1–125 onto 123 and erases the difference between "a
  real hit" and "could not judge".
- ⚠️ **An EAR verdict answers the question you asked, not the one you meant.** Asking "which is the
  lowest **audible** tone" and reading the answer as "all of them are **clean**" fabricated a
  clean-vs-distorted discriminator that the device log then refuted — a lone canned sound and a lone
  tone are the same `audio_mix_add()` call, with no clipping on either. Audibility and quality are two
  questions and one answer cannot serve both; ask them separately, and ask the panel log which state
  each verdict was given in.
- ⚠️ **A kernel counter that latches a session high-water mark cannot be attributed to one leg of an
  A/B.** `/proc/asound/…/status`'s `avail_max` is cleared by whoever reads it first and never says
  *when* its extreme happened, so a session containing both legs yields one number belonging to
  neither. The per-leg witness has to be a counter the library itself resets when the mode changes.

## The holes each suite has, named

- ⚠️ **`arca_provision_test.sh` group E cannot see the copy step and never could.** It compares two
  *plans* through `arca_provision_canonical`; a dry run copies nothing, and the offline executor has no
  `scp` to be compared against — which is how a one-of-eight install defect shipped past 94 green
  cases. **Group F is that hole**, and it asserts **8 of 8**, never "more than one", because the defect
  produced exactly one.
- **`arca_ssh_test.sh` starts a real `sshd`** on a loopback high port with an empty `AuthorizedKeysFile`
  — which needs no root — because a genuine `Permission denied` cannot be produced by a stub without
  writing the string the code is supposed to recognise.
- **`arca_provision_test.sh`'s fixtures are synthetic with real symlinks**, for the reason above.
- **`commission_offline_test.sh`** needs root and a staged bundle; every check it makes has a sabotage
  case, and its fixture builder is `tests/make-fake-card.sh`.
- **`arca_source_offer_test.sh` drives `rootfs/check-source-offer.sh` over a miniature offer and a fixture image**, never a real 0.7 GB offer; each check has a sabotage case, and `CSO=<path>` points the suite at a sabotaged copy of the checker (`MCI=<path>`, the same for `make-card-image.sh`). Its group F extracts `licence_docs_install` from the shipped script and runs it on fixture directories. Blind: the real downloads and a real card image (the map table was graded against the real one by hand), and the loop-mounted p6 fill that calls that function. `tests/measure_source_offer_sabotage.sh` is the sweep.
- ⚠️ **`setup_build_env_test.sh` drives the probe loop over FIXTURE tables, never the real package set.**
  A run against the real table on a provisioned host reports "0 missing" and proves only that this host is
  provisioned — a silenced instrument. Its `sudo` is a PATH tripwire recording argv on **every** case, so a
  sabotage forcing the install path cannot reach a live `apt` from a case that forgot the shim (measured), and
  "sudo was never called" is a real assertion — `measure_setup_build_env_sabotage.sh` includes a case that
  *does* reach it. Blind: whether the package **names** resolve on a live archive.
- ⚠️ **`smoke_first_screen_test.sh` cannot see busybox's detach/SIGHUP behaviour, the composited panel
  (`cat /dev/fb0` is the gfx plane only), or a wrong-but-colourful screen.** Its `ssh` stub models fb contents and
  must slurp stdin, or the check that the runner passes `ssh -n` is vacuous. The runner itself needs a
  device to run.
- ⚠️ **`arca_release_test.sh` can assert every refusal and only a STUBBED acceptance.** `release.sh` has no
  `--no-build` flag, so the two `--out` values that must be *accepted* — an empty directory and a re-stage
  over a real bundle — fall straight through into a four-component ARM cross-build. The suite therefore
  runs the shipped script in a temp tree against a stub component script, and cannot see whether the real
  four honour `--bundle <dir>`; what it does see is that `release.sh` calls them with exactly two
  arguments and consumes the bundle they leave behind. ⚠️ **Its `rm` is a PATH tripwire that records its
  argv and fails**, which is what makes a root spelling safe to exercise against a pre-fix copy of a guard
  whose whole defect was letting one through — so the "and the decoy's file is still there" cases witness
  the tripwire's claim, not the guard's, and the sabotage sweep leaves them green. `RELEASE_SH=<path>` grades another copy; `ARCA_SAB_COPIES_ONLY=1 tests/measure_release_sabotage.sh` runs just the copy half. ⚠️ **Group E drives `--source-offer` through a stub offer script and a stub `gh`**, so it cannot see the real offer or a real publish.

## Sabotage harnesses

`tests/measure_*_sabotage.sh` re-measure a suite against deliberately broken copies of the code, and
print the counts their headers claim. Rules learned the hard way:

- ⚠️ **Never restore a sabotage with `git checkout`** — a measurement loop that does destroys the
  uncommitted fix it is measuring.
- ⚠️ **A sabotage harness must verify each edit changed the copy, and treat no-match as a harness error, not a pass** — a `sed` that applies nothing reports "0 failed", indistinguishable from a suite that
  cannot detect the breakage (every harness here now exits nonzero on it). Put the sabotages in a *file*: a pattern containing `\t` does not survive
  `wsl.exe -e bash -lc` quoting, and a pattern containing `||` silently matches nothing.
- ⚠️ **A rotted *pattern* is caught; a rotted *replacement* is not.** If the replacement names a
  variable that no longer exists it assigns the empty string and the harness prints "not caught" for the
  wrong reason. **Re-run the harness after any rename it mentions**, justify a count that moved, and
  name the surviving cases from that sabotage alone.
- ⚠️ **Sabotages that anchor on an exact full line are brittle by construction**: a second copy of
  the anchored line in the subject breaks them.
- **Include a control on the harness itself.** `measure_provision_sabotage.sh` case 5 makes the stub
  `ssh` stop reading stdin, and **F1 alone** must fail — because a stub that does not slurp its stdin
  cannot reproduce the defect and makes the whole group a vacuous pass. Similarly, a pre-fix-tree
  control is vacuous if a `set -u` suite dies on an unbound variable instead of failing assertions.
- ⚠️ **Stage only the files the suite sources — never `cp -a lib usb_host`.** A `cp -a` of a directory
  from `/mnt/c` into WSL `/tmp` can blow a 300 s budget and looks like a hung suite. And stage the
  sabotaged library **inside `lib/`**, because `arca_provision_validate` derives the repo root from its
  own `BASH_SOURCE/..`, so a copy under `/tmp` fails the baseline for a reason no sabotage produces.
- ⚠️ **An assertion can PASS by reading past what it claims to measure, and only a sabotage sweep finds
  it.** `native_apps/tests/audio_sample_test.c` group H checked "the last delivered sample is exactly 0"
  after rendering `left + 8` frames — but `audio_mix_render()` returns `frames` whatever happened, so it
  was reading *trailing silence* and passed with the envelope **deleted**. 56/56 green meant nothing.
  Render the **exact** extent, and neutralise a source that has shape of its own (group H uses a flat
  source, so nothing but the envelope can shape the tail).
  `measure_audio_sample_sabotage.sh` is that suite's sweep: **16 cases, every one seen failing**, and it
  is also what caught three other holes in the same first draft — an unreached RIFF odd-size pad byte,
  an unchecked 8-bit refusal, and a `sed` pattern spanning two lines that never applied.
- **A pre-fix tree restored from git beats a `sed` patch** as a harness's first case, where it is
  available — `measure_ssh_sabotage.sh` does this. ⚠️ **Pin it to the commit before the fix** (`622c4dc^`), never `HEAD`: restored from `HEAD` it becomes vacuous once the fix lands.

## Running them

From WSL, redirecting to a file inside the repo — ⚠️ **`wsl.exe … | tail -N` prints nothing until the
command exits, which is indistinguishable from a hung WSL.** `timeout N` anything that could loop: a
host regression that hangs is a *test result*, not a tool timeout.

```bash
wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && timeout 300 ./tests/arca_provision_test.sh > /mnt/c/work/roomwizard/out.txt 2>&1"
```

⚠️ **WSL's `/tmp` does not survive between `wsl.exe` calls** — the instance idles out and takes it with
it. Stage and use a fixture inside **one** `wsl.exe -e bash -lc`, or put it under the repo.

⚠️ **Never edit one of these scripts while a background run of it is in flight.** Bash reads a script
incrementally, so an edit shifts the byte offset under the running copy and it dies with
`syntax error near unexpected token` at a line that is perfectly valid in the file — measured 2026-08-16 on
`doc_check.sh`, in two concurrent runs, after `bash -n` had already passed and passed again afterwards. The
group results printed before the corruption are still valid measurements; the exit status is not.

`shellcheck` 0.7.0 **is** installed in this WSL — re-measured 2026-09-06 — so it is a third check here,
alongside `bash -n` and `dash -n` on anything carrying a `/bin/sh` shebang.
