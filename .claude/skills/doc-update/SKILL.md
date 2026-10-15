---
name: doc-update
description: Add to, correct, or close out content in this repo's documentation — the four top-level docs and the eight directory-scoped CLAUDE.md files, and kernel/README.md — under the one-fact-one-home rules. Use whenever a session produces a durable fact, a new open item, a closed item, or a correction to a document. Routes the content to the right file, checks it is not already elsewhere, writes it in the repo's shape, runs tests/doc_check.sh, and commits.
---

# doc-update

Documentation here was cleaned up over a week in August 2026: 891 → 713, 2287 → 1477, 2214 → 1872.
**That cost was not caused by badly written additions. It was caused by well-written additions to the
wrong file, and by nobody deleting anything.** This skill exists to stop the same bill arriving twice.

It does not restate the rules — they live in the documents themselves and in `tests/doc_check.sh`. It is
the **order of operations** that gets skipped, so that is what is written down here.

⚠️ **Two things this skill cannot do, and neither is a formality.** It cannot tell whether a claim is
measured — it can only force the sentence to say so. And it cannot reliably find a duplicate: `grep -F`
is defeated by rewording, by case and by substring, all three of which happened in one afternoon. So a
green gate means "nothing I was told to check is broken", never "the docs are clean".

## Step 1 — classify, before opening any file

Ask these in order and stop at the first yes. The routing mistake is the expensive one.

| The content is… | It goes in | Test that catches you |
|---|---|---|
| true of the device no matter what we build — silicon, boot chain, OS, a subsystem's behaviour, a trap | `SYSTEM_ANALYSIS.md` | would it still be true if this repo did not exist? |
| a part, connector, header, or the enclosure — something you could point at on the board | `HARDWARE.md` | is it a *thing* rather than a *behaviour*? |
| work not done yet — a bug, a feature, a phase table | `IMPROVEMENT_PLAN.md` | is there an action in it? if not, it is not an item |
| how to write code in one directory — an API rule, a call-order constraint, a shape to copy | that directory's `CLAUDE.md` | would a person editing *only that directory* need it? |
| how our kernel image is built, what each `kernel/patches`/`kernel/dts` change is for, a driver's state, a reverse-engineering method | `kernel/README.md` | what the *vendor* kernel does is a device fact → `SYSTEM_ANALYSIS.md`; only build, patch and method prose stays here |
| what a script does, or how an operator runs a bring-up | `README.md` | is the audience an operator rather than an author? |
| a licence fact | `LICENSE.md` | — |
| something I must know before the first edit, in any directory | root `CLAUDE.md` — **budget 16 lines, the tightest in the repo** | is it worth loading in *every* session? |

⚠️ **A device fact discovered while writing code still belongs in `SYSTEM_ANALYSIS.md`, not in the
component doc where you found it.** That single confusion is where most of the 1,900 deleted lines came
from. The component doc gets a pointer.

⚠️ **If it does not fit a row, it may not belong in a document at all.** A session's narrative, a
hypothesis not yet tested, a hedge — those go in the plan file under `~/.claude/plans/`, which is outside
the repo and outside the gate. Ask "will someone need this in six months?" before writing anything.

## Step 2 — ask "is this reference *here*?" of the destination's neighbours

**Not of the file you are about to edit — of the other files.** This question found 40 lines in one
afternoon after prose compression had stopped paying, and it is the whole difference between a document
that stays 700 lines and one that reaches 891.

```bash
# From the repo root. Pick 2-4 distinctive tokens from what you are about to write.
for t in "<token1>" "<token2>"; do
  printf '%-24s ' "$t"
  for f in SYSTEM_ANALYSIS.md HARDWARE.md IMPROVEMENT_PLAN.md CLAUDE.md \
           native_apps/CLAUDE.md lib/CLAUDE.md rootfs/CLAUDE.md \
           device-files/CLAUDE.md tests/CLAUDE.md kernel/CLAUDE.md kernel/README.md; do
    printf '%s:%s ' "$f" "$(grep -cF -- "$t" "$f")"
  done; echo
done
```

Any nonzero hit outside the intended destination is a decision, not noise:

- **already stated where it belongs** → write a pointer, or nothing at all. Do not restate it.
- **stated in the wrong file** → this is a *move*, so go to Step 4 before writing.
- ⚠️ **stated as a pointer that no longer resolves** → read the destination. Every pointer checked
  against its destination during the cleanup — six of six — turned out to be wrong: the destination had
  drifted, or a copy had come back the other way. **"See X" that X does not satisfy is worse than the
  duplication it replaced.**

## Step 3 — write it in the repo's shape

One paragraph, and it carries four things: **the rule in bold**, the mechanism, the measurement that
proves it, and the identifier (`file.c:line`, a function, a constant) that lets the next reader check it.

⚠️ **A war story compresses to its rule PLUS the measurement — not to its rule.** Assuming otherwise is
what mispriced two of the cleanup's phases. If you cannot state the measurement, you are writing a
hypothesis, and it belongs in the plan file.

Forbidden shapes, each of which has cost real time here:

- **No was-then-is-now.** Documents hold current truth. Delete the old sentence; do not narrate the change.
- ⚠️ **No count in prose.** A number in a doc is a promise to re-measure that nobody keeps — three
  in-tree records claimed a suite had 163, 155 and 94 cases and disagreed. **Write the rule plus the
  command that checks it.** Same for line counts and call-site counts.
- **No hedge promoted to a confirmation.** *"I think it works"* is reported as a hedge; *"latent, not
  reproducible from this host"* is the honest form. In `SYSTEM_ANALYSIS.md`, tag it: `[inferred]`,
  `[unverified]`, `[n=1]` — the legend is that file's §1.
- ⚠️ **Never write a plan entry ID into any file but `IMPROVEMENT_PLAN.md`.** The plan is a queue: an
  entry is executed and then deleted, so a citation of one has a scheduled expiry built in and the reason
  it was carrying dies with the entry. Naming the **file** is fine; naming an **ID** is not. A comment
  therefore states its own reason in full, and no ID sits beside it. `tests/doc_check.sh` group B is the
  gate, and it counts any citation outside the plan, resolving or not.
- ⚠️ **Never write a real-looking plan ID or anchor when *describing* these shapes.** Write `<ID>` and
  `#<fragment>`, and bracket the **far** side of any delimiter — group A's path component matches the
  empty string, so a bracketed filename in front of a bare `#` is still a hit. This has fired eleven
  times. Anything written to *measure* the gate goes outside the tree entirely.

Mechanics: anchor `Edit` on text you intend to **keep**. Never bulk-edit with a python script — it
rewrites the file's line endings. Fix inbound anchors whenever you retitle a heading.

⚠️ **A stale claim is corrected with a measurement, not deleted.** This is the other half of "documents hold
current truth", and the instinct gets it backwards. Five stale claims surfaced during the cleanup and the
temptation each time was to cut the doubtful sentence — but deleting it loses the reason someone wrote it,
and the next session re-derives the same thing from source. Re-measure and replace. **If you cannot measure
it now, tag it `[unverified]` and file the measurement as an item** — that is what an item is for.

⚠️ **Overwrite, do not append.** A correction written beside the thing it corrects leaves two claims in the
file and the reader cannot tell which is current. The old sentence goes.

## Step 4 — closing out and moving: delete before you add

**This is the step that actually keeps the size down, and it is the one that gets skipped.**

- **A closed item is deleted outright** from `IMPROVEMENT_PLAN.md` — no closed-work ledger. ⚠️ **Re-home
  the reason it carried FIRST**, as a self-contained statement in the destination document that needs no
  ID to be read; then delete the entry. Nothing outside the plan cites an ID, so there is no citation to
  repair and a deletion can never move group B — a gate demanding that citations resolve would instead
  make keeping dead entries alive the cheapest way to stay green.
- ⚠️ **The ID namespace collides with the shell suites' own case labels** — `arca_provision_test.sh`,
  `arca_ssh_test.sh` and the sabotage harnesses all label cases in exactly the plan's shape. Those are
  working pointers, not citations: **disambiguate the sentence** ("that suite's cases B6/C3a/C5a") rather
  than deleting one.
- **A fact that moves needs a group C receipt row in `tests/doc_check.sh`, written BEFORE the deletion** —
  that is what proves the destination already holds it. Pick the token with `grep -cF` across the repo
  **first**: it must be unique to the block being moved, not merely distinctive. Seven part numbers and
  the number `54000` all looked ideal and would have reported `NOT MOVED` forever (`49054000.gpio`).
- ⚠️ **Three reasons a verified move can carry no row**: the destination **rewords** the fact, **shouts**
  it (case), or the token is a **substring** of live text. Then the verification is a grep recorded in
  the commit message, and the pointer must still be read at the destination by hand.

## Step 5 — gate, then commit

```bash
# >120 s from Git Bash: background it, and redirect OUTSIDE the repo tree.
./tests/doc_check.sh > ~/dc_out.txt 2>&1        # run_in_background, then Read the file
awk 'index($0,"\r")>0 {c++} END {print c+0}' <each edited file>   # MUST be 0
bash -n tests/doc_check.sh                     # if you touched it
```

⚠️ **`grep -c $'\r'` reports every line of every file** — the pattern degrades to empty. Use the `awk`.

Expect all five groups green before committing. What each result means:

- **A goes UP when you do this right** — a pointer is an anchor, so replacing a duplicated block with a
  pointer *adds* to that count. It can also stay flat while you change things: re-labelling a link moves
  the count as much as adding one.
- **B moves only if you wrote a citation, never because you deleted an entry.** It is at zero **by
  measurement, not by gate** — three blind shapes, listed in `tests/CLAUDE.md`. Re-run the two scans from
  `~/.claude/plans/peaceful-herding-valiant.md` → Phase 2b on any file you rewrapped.
- **C moves by exactly the rows you added.**
- **D over ceiling means the addition was not paid for.** Two answers, and only two: delete as much as
  you added (usually from the document that should never have held it), or raise the ceiling in this
  commit and say why. ⚠️ **Do not silently raise it** — that is the failure mode the group exists to catch.

Then commit, in one commit with the code or docs it describes. The message says **which groups moved and
why**, and carries any verification that could not become a receipt row as an explicit grep. Sign off with:

```
Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
```
