"""For a committed merge: per file, the multiset of +/- lines of (merge^1 -> merge) must equal that of (merge-base -> other)."""
import collections
import subprocess
import sys

tree, merge, other = sys.argv[1:4]


def git(*args):
    return subprocess.run(["git", "-C", tree, *args], capture_output=True, text=True, encoding="utf-8", errors="surrogateescape").stdout


first = git("rev-parse", merge + "^1").strip()
mb = git("merge-base", first, other).strip()
files = sorted(set(git("diff", "--name-only", mb, other).split()) | set(git("diff", "--name-only", first, merge).split()))


def changed(a, b, path):
    out = collections.Counter()
    for line in git("diff", a, b, "--", path).split("\n"):
        if line.startswith(("+", "-")) and not line.startswith(("+++", "---")):
            out[line] += 1
    return out


bad = 0
for path in files:
    m = changed(first, merge, path)
    w = changed(mb, other, path)
    if m == w:
        print(f"OK   {path}: {sum(m.values())} changed lines match the other side exactly")
    else:
        bad += 1
        extra = m - w
        missing = w - m
        print(f"DIFF {path}: merge-only {sum(extra.values())}, other-only {sum(missing.values())}")
        for line, n in list(extra.items())[:6]:
            print(f"     merge-only x{n}: {line[:160]}")
        for line, n in list(missing.items())[:6]:
            print(f"     other-only x{n}: {line[:160]}")
print("files with differences:", bad)
