#!/usr/bin/env python3
# Byte-diff two luaprobe CSVs (same probe binary, two platforms) per op.
# Usage: luaprobe_diff.py luaprobe_arm64.csv luaprobe_linux.csv
import sys, collections
a_path, b_path = sys.argv[1], sys.argv[2]
def load(p):
    d = {}
    with open(p) as f:
        for line in f:
            op, arg, res = line.rstrip("\n").split(",")
            d.setdefault(op, {})[arg] = res
    return d
A, B = load(a_path), load(b_path)
ops = sorted(set(A) | set(B))
print(f"{'op':<10} {'status':<10} {'#diff/#args':<14} example (arg -> A vs B)")
diverged = []
for op in ops:
    a, b = A.get(op, {}), B.get(op, {})
    args = sorted(set(a) & set(b))
    diffs = [(x, a[x], b[x]) for x in args if a[x] != b[x]]
    if diffs:
        diverged.append(op)
        x, av, bv = diffs[0]
        print(f"{op:<10} {'DIVERGES':<10} {len(diffs)}/{len(args):<10} {x} -> {av} vs {bv}")
    else:
        print(f"{op:<10} {'identical':<10} {0}/{len(args)}")
print()
print("DIVERGENT OPS:", diverged if diverged else "NONE (3-way: A==B for all probed ops)")
