"""Pre-push attribution scan. Lists every commit in <range> (default upstream/development..HEAD, or the refs you
name) whose message has a line STARTING with co-authored-by / claude-session / generated with — the same rule
strip_trailers.py strips by. A sentence that merely mentions a trailer is not a hit. Exit 1 when any hit exists.
usage: python scan_trailers.py <tree> [<rev-range or refs...>]   e.g.  scan_trailers.py D:/Projects/takeover-build upstream/development..HEAD
       python scan_trailers.py <tree> --all-branches               (every local branch and tag, minus upstream/development)"""
import re
import subprocess
import sys

FORBIDDEN = re.compile(r"^[ \t]*(co-authored-by|claude-session|generated[- ]with)\b", re.I | re.M)

tree = sys.argv[1]
args = sys.argv[2:] or ["upstream/development..HEAD"]
if args == ["--all-branches"]:
    args = ["--branches", "--tags", "--not", "upstream/development"]
raw = subprocess.run(["git", "-C", tree, "log", "--format=%H%x00%s%x00%B%x01", *args],
                     capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
hits = []
for rec in raw.split("\x01"):
    if rec.count("\x00") < 2:
        continue
    sha, subject, body = rec.split("\x00", 2)
    if FORBIDDEN.search(body):
        hits.append((sha[:10], subject.strip()[:70]))
for sha, subject in hits:
    print(f"HIT {sha} {subject}")
print(f"trailer hits: {len(hits)} in {' '.join(args)}")
sys.exit(1 if hits else 0)
