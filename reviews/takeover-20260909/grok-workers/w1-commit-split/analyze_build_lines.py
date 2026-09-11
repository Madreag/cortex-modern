#!/usr/bin/env python3
"""Map added lines in the three build files to plan commits."""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
BASE = "60cb6981462d30400d372ca778d440c82a5bcbe3"
PLAN = json.loads((SCRATCH / "plan.json").read_text(encoding="utf-8"))
BUILD_FILES = [
    "RTEA.vcxproj",
    "RTEA.vcxproj.filters",
    "Source/Network/meson.build",
]
# Last plan commit is the one we drop.
COMMITS = [c for c in PLAN["commits"] if c["subject"] != "Add the new network and moderation sources to the build"]

# Compile-unit stem -> commit index (1-based among COMMITS)
UNIT_TO_COMMIT: dict[str, int] = {}
for i, c in enumerate(COMMITS, 1):
    for f in c["files"]:
        name = Path(f).name
        UNIT_TO_COMMIT[name] = i
        UNIT_TO_COMMIT[name.replace("\\", "/")] = i


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git_show(rev_path: str) -> bytes:
    r = subprocess.run(["git", "show", rev_path], cwd=REPO, capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f"git show {rev_path} failed: {r.stderr.decode()}")
    return r.stdout


def named_units(line: str) -> list[str]:
    """Return compile-unit filenames mentioned on a build-file line."""
    found = []
    # meson: 'Foo.cpp',
    for m in re.finditer(r"'([^']+\.(?:cpp|h|c|cc|hpp))'", line):
        found.append(Path(m.group(1)).name)
    # vcxproj: Include="Source\Network\Foo.cpp"
    for m in re.finditer(r'Include="([^"]+)"', line):
        found.append(Path(m.group(1).replace("\\", "/")).name)
    # filters sometimes repeat the include on a child-less line
    return found


def main() -> None:
    unmapped = []
    line_map = {}
    per_file = {}

    for rel in BUILD_FILES:
        final = (REPO / rel).read_bytes()
        base = git_show(f"{BASE}:{rel}")
        # Keep newline style of FINAL
        final_text = final.decode("utf-8")
        base_text = base.decode("utf-8")
        # git blobs are typically LF; working tree may be CRLF or LF
        final_lines = final_text.splitlines(keepends=True)
        base_lines_set = set(base_text.splitlines())
        # Also compare stripped-newline versions for membership
        base_stripped = {ln.rstrip("\r\n") for ln in base_text.splitlines()}

        added = []
        for idx, line in enumerate(final_lines, 1):
            stripped = line.rstrip("\r\n")
            if stripped in base_stripped:
                continue
            units = named_units(stripped)
            commit_idx = None
            reason = None
            if units:
                idxs = {UNIT_TO_COMMIT[u] for u in units if u in UNIT_TO_COMMIT}
                unknown = [u for u in units if u not in UNIT_TO_COMMIT]
                if unknown and not idxs:
                    reason = f"names units not in plan: {unknown}"
                elif unknown and idxs:
                    reason = f"mixed known {sorted(idxs)} and unknown {unknown}"
                elif len(idxs) == 1:
                    commit_idx = next(iter(idxs))
                    reason = f"unit {units}"
                elif len(idxs) > 1:
                    reason = f"names units from multiple commits {sorted(idxs)}: {units}"
                else:
                    reason = f"units {units} not in UNIT_TO_COMMIT"
            else:
                reason = "no compile unit named"
            rec = {
                "line_no": idx,
                "text": stripped,
                "units": units,
                "commit_index": commit_idx,
                "reason": reason,
            }
            added.append(rec)
            if commit_idx is None:
                unmapped.append({"file": rel, **rec})

        per_file[rel] = {
            "final_sha256": sha256_bytes(final),
            "base_sha256": sha256_bytes(base),
            "final_nlines": len(final_lines),
            "added": added,
        }
        line_map[rel] = added

    out = {
        "unit_to_commit": UNIT_TO_COMMIT,
        "commit_subjects": [c["subject"] for c in COMMITS],
        "files": per_file,
        "unmapped": unmapped,
    }
    dest = SCRATCH / "build-lines-map.json"
    dest.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {dest}")
    print(f"unmapped={len(unmapped)}")
    for u in unmapped:
        print(f"  {u['file']}:{u['line_no']}: {u['reason']}")
        print(f"    {u['text'][:160]}")


if __name__ == "__main__":
    main()
