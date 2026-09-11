"""Append a backup commit of loose docs, harness files and worker reports to the backup branch.

Plumbing only (temporary index, no checkout), so no worktree is disturbed.
"""
import os
import subprocess
import sys
import tempfile

REPO = r"D:\Projects\cccp"
BRANCH = "backup/takeover-fixes-wip-20260910"
ROOT = r"D:\Projects"
FILES = [
    "RESUME.md",
    "AGENTS.md",
    "CLAUDE.md",
    "STATUS.md",
    r"reviews\takeover-20260909\run_breadth.py",
    r"reviews\claude-review-2026-09-08\lanes\compat-review\fixtures\compat_review_extra.lua",
]
DIRS = [r"reviews\takeover-20260909\grok-workers", r"reviews\takeover-20260909\handoff-20260910"]
SKIP_DIRS = {"scratch-repo", "mac-files", "pre", "untracked-before", ".git", "__pycache__"}
MAX_BYTES = 3_000_000
MESSAGE = (
    "Back up the live docs, fix-group design and worker reports\n\n"
    "Third snapshot, 2026-09-11 06:55 UTC: the RESUME.md live-state entry, the STATUS.md\n"
    "board, the AGENTS.md/CLAUDE.md rules added today, FIX_GROUP_1_DESIGN.md, the W1-W22\n"
    "reports, the Mac CLI launcher and the firewall scripts.\n"
)


def git(*args, env=None, input_bytes=None):
    r = subprocess.run(["git", "-C", REPO, *args], capture_output=True, env=env, input=input_bytes)
    if r.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed: {r.stderr.decode(errors='replace')}")
    return r.stdout.decode().strip()


paths = list(FILES)
for d in DIRS:
    for dp, dns, fns in os.walk(os.path.join(ROOT, d)):
        dns[:] = [x for x in dns if x not in SKIP_DIRS]
        for fn in fns:
            full = os.path.join(dp, fn)
            if os.path.getsize(full) <= MAX_BYTES:
                paths.append(os.path.relpath(full, ROOT))

parent = git("rev-parse", f"refs/heads/{BRANCH}")
env = dict(os.environ)
tmp = tempfile.NamedTemporaryFile(delete=False)
tmp.close()
os.unlink(tmp.name)
env["GIT_INDEX_FILE"] = tmp.name
git("read-tree", parent, env=env)

total = 0
for rel in sorted(set(paths)):
    full = os.path.join(ROOT, rel)
    total += os.path.getsize(full)
    blob = git("hash-object", "-w", "--", full)
    git("update-index", "--add", "--cacheinfo", f"100644,{blob},{rel.replace(os.sep, '/')}", env=env)

tree = git("write-tree", env=env)
commit = git("commit-tree", tree, "-p", parent, input_bytes=MESSAGE.encode())
git("update-ref", f"refs/heads/{BRANCH}", commit, parent)
os.unlink(tmp.name)
print(f"files={len(set(paths))} bytes={total} parent={parent[:12]} commit={commit[:12]} branch={BRANCH}")
