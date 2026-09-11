"""Build an orphan backup branch from loose files using git plumbing only.

No worktree, no checkout index: a temporary index file is used, so concurrent
work in any worktree of the shared repository is not disturbed.
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
    r"reviews\takeover-20260909\grok-workers\WORKER_RULES.md",
    r"reviews\takeover-20260909\b2-mac-positive20\positive20.manifest.json",
]
DIRS = [r"reviews\takeover-20260909\handoff-20260910"]
MESSAGE = (
    "Back up the uncommitted takeover-fixes working set and the live docs\n\n"
    "Snapshot of the stage2/takeover-next working tree at 60cb698146 before the\n"
    "per-concern commit split: tracked patch, untracked tarball, hash manifest,\n"
    "the post-positive20 instrumentation patch with its positive20 base copies,\n"
    "and the positive20 Mac manifest, plus RESUME.md, AGENTS.md and CLAUDE.md.\n"
)


def git(*args, env=None, input_bytes=None):
    r = subprocess.run(["git", "-C", REPO, *args], capture_output=True, env=env, input=input_bytes)
    if r.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed: {r.stderr.decode(errors='replace')}")
    return r.stdout.decode().strip()


paths = list(FILES)
for d in DIRS:
    for dp, _, fns in os.walk(os.path.join(ROOT, d)):
        for fn in fns:
            paths.append(os.path.relpath(os.path.join(dp, fn), ROOT))

env = dict(os.environ)
tmp = tempfile.NamedTemporaryFile(delete=False)
tmp.close()
os.unlink(tmp.name)
env["GIT_INDEX_FILE"] = tmp.name

total = 0
for rel in sorted(paths):
    full = os.path.join(ROOT, rel)
    total += os.path.getsize(full)
    blob = git("hash-object", "-w", "--", full)
    git("update-index", "--add", "--cacheinfo", f"100644,{blob},{rel.replace(os.sep, '/')}", env=env)

tree = git("write-tree", env=env)
commit = git("commit-tree", tree, input_bytes=MESSAGE.encode())
git("update-ref", f"refs/heads/{BRANCH}", commit)
os.unlink(tmp.name)
print(f"files={len(paths)} bytes={total} tree={tree} commit={commit} branch={BRANCH}")
