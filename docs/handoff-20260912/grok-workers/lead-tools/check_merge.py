"""Compare, per file, the merged tree's diff vs HEAD against the other parent's diff from the merge base.

For an exact keep-both resolution the multisets of +/- lines agree; any difference is printed.
Usage: python check_merge.py <tree> <other-parent> [files...]
"""
import collections
import pathlib
import subprocess
import sys

tree = sys.argv[1]
other = sys.argv[2]
files = sys.argv[3:]


def run(*args):
    return subprocess.run(["git", *args], cwd=tree, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout


mb = run("merge-base", "HEAD", other).strip()
print("merge-base", mb)
if not files:
    files = [l.split("\t")[-1] for l in run("diff", "--name-only", mb, other).splitlines()]


def changed_lines(diff_text):
    counter = collections.Counter()
    for line in diff_text.splitlines():
        if line.startswith(("+++", "---")):
            continue
        if line.startswith(("+", "-")):
            counter[line.rstrip("\r")] += 1
    return counter


total_diffs = 0
for f in files:
    vs_head = changed_lines(run("diff", "-U0", "HEAD", "--", f))
    theirs = changed_lines(run("diff", "-U0", mb, other, "--", f))
    only_merge = vs_head - theirs
    only_theirs = theirs - vs_head
    if not only_merge and not only_theirs:
        print(f"OK   {f}: {sum(vs_head.values())} changed lines match W-side diff exactly")
        continue
    total_diffs += 1
    print(f"DIFF {f}:")
    for line, n in only_merge.items():
        print(f"   merge-only x{n}: {line[:200]}")
    for line, n in only_theirs.items():
        print(f"   theirs-only x{n}: {line[:200]}")
print("files with differences:", total_diffs)
