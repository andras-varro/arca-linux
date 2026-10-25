#!/usr/bin/env python3
"""Resolve a retro_bench RETRO_PROF file into per-thread function totals.

  prof_resolve.py <profile> [<name>.so=<unstripped copy> ...] [--top N]

A sample in a mapped file named on the command line is resolved to a function
with arm-linux-gnueabi-addr2line against the unstripped copy; any other
sample is counted under its file's basename (libc, the bench, [unknown])."""
import subprocess
import sys
from collections import Counter, defaultdict

def main():
    args = sys.argv[1:]
    top = 25
    if "--top" in args:
        i = args.index("--top"); top = int(args[i + 1]); del args[i:i + 2]
    prof, syms = args[0], dict(a.split("=", 1) for a in args[1:])
    maps, samples = [], []
    for line in open(prof):
        if line.startswith("map "):
            f = line[4:].split()
            if len(f) >= 6 and "x" in f[1]:
                lo, hi = (int(x, 16) for x in f[0].split("-"))
                maps.append((lo, hi, int(f[2], 16), f[5].rsplit("/", 1)[-1]))
        elif line.strip():
            tid, pc = line.split()
            samples.append((tid, int(pc, 16)))
    where = []
    want = defaultdict(set)
    for tid, pc in samples:
        for lo, hi, off, name in maps:
            if lo <= pc < hi:
                where.append((tid, name, pc - lo + off))
                if name in syms:
                    want[name].add(pc - lo + off)
                break
        else:
            where.append((tid, "[unknown]", 0))
    fn = {}
    for name, offs in want.items():
        offs = sorted(offs)
        out = subprocess.run(["arm-linux-gnueabi-addr2line", "-f", "-C", "-e", syms[name]],
                             input="".join("%x\n" % o for o in offs), capture_output=True,
                             text=True, check=True).stdout.splitlines()
        for o, f in zip(offs, out[0::2]):
            fn[(name, o)] = f
    total = Counter()
    per_tid = defaultdict(Counter)
    for tid, name, off in where:
        label = name + ": " + fn[(name, off)] if (name, off) in fn else name
        total[label] += 1
        per_tid[tid][label] += 1
    n = len(where)
    print("%d samples" % n)
    for tid, c in sorted(per_tid.items(), key=lambda t: -sum(t[1].values())):
        print("\n== thread %s: %d samples (%.1f%%)" % (tid, sum(c.values()), 100.0 * sum(c.values()) / n))
        for label, k in c.most_common(top):
            print("%6.1f%%  %s" % (100.0 * k / n, label[:150]))

main()
