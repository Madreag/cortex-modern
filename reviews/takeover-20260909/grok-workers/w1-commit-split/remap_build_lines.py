#!/usr/bin/env python3
"""Map new build-file entries to commits. Blocks, not SequenceMatcher."""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
BASE_REV = "60cb6981462d30400d372ca778d440c82a5bcbe3"
PLAN = json.loads((SCRATCH / "plan.json").read_text(encoding="utf-8"))
BUILD_FILES = ["RTEA.vcxproj", "RTEA.vcxproj.filters", "Source/Network/meson.build"]
COMMITS = [c for c in PLAN["commits"] if c["subject"] != "Add the new network and moderation sources to the build"]

UNIT_TO_COMMIT: dict[str, int] = {}
for i, c in enumerate(COMMITS, 1):
    for f in c["files"]:
        UNIT_TO_COMMIT[Path(f).name] = i

INCLUDE_RE = re.compile(r'<(ClInclude|ClCompile)\s+Include="([^"]+)"\s*(/)?>')
MESON_RE = re.compile(r"^'([^']+\.(?:cpp|h|c|cc|hpp))',?\s*$")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unit_name(path: str) -> str:
    return Path(path.replace("\\", "/")).name


def parse_entries(rel: str, lines: list[str]) -> list[dict]:
    """Return entries [{key, start, end, unit, lines}]. start/end are 0-based half-open."""
    entries = []
    i = 0
    n = len(lines)
    if rel.endswith("meson.build"):
        while i < n:
            m = MESON_RE.match(lines[i].rstrip("\r\n"))
            if m:
                entries.append({
                    "key": m.group(1),
                    "start": i,
                    "end": i + 1,
                    "unit": Path(m.group(1)).name,
                })
            i += 1
        return entries

    is_filters = rel.endswith(".filters")
    while i < n:
        m = INCLUDE_RE.search(lines[i])
        if not m:
            i += 1
            continue
        path = m.group(2)
        self_close = bool(m.group(3)) or lines[i].rstrip().endswith("/>")
        if is_filters and not self_close:
            # expect Filter + close
            if i + 2 >= n:
                raise SystemExit(f"{rel}:{i+1}: Include without a 3-line block")
            end = i + 3
        else:
            end = i + 1
        entries.append({
            "key": path.replace("\\", "/"),
            "start": i,
            "end": end,
            "unit": unit_name(path),
        })
        i = end
    return entries


def main() -> int:
    unmapped = []
    files_out = {}
    for rel in BUILD_FILES:
        final = (REPO / rel).read_bytes()
        base = subprocess.run(["git", "show", f"{BASE_REV}:{rel}"], cwd=REPO, capture_output=True).stdout
        final_lines = final.decode("utf-8").splitlines(keepends=True)
        base_lines = base.decode("utf-8").splitlines(keepends=True)
        final_entries = parse_entries(rel, final_lines)
        base_entries = parse_entries(rel, base_lines)
        base_keys = {e["key"] for e in base_entries}
        new_entries = [e for e in final_entries if e["key"] not in base_keys]

        added = []
        covered = set()
        for e in new_entries:
            idx = UNIT_TO_COMMIT.get(e["unit"])
            reason = f"unit {e['unit']}" if idx else f"new entry unit {e['unit']} not in plan"
            for j in range(e["start"], e["end"]):
                covered.add(j)
                rec = {
                    "line_no": j + 1,
                    "text": final_lines[j].rstrip("\r\n"),
                    "units": [e["unit"]],
                    "commit_index": idx,
                    "reason": reason,
                    "entry_key": e["key"],
                }
                added.append(rec)
                if idx is None:
                    unmapped.append({"file": rel, **rec})

        # Every FINAL line that is not a covered new-entry line must exist in BASE
        # at the same relative scaffolding. Stronger: drop all new-entry lines → BASE.
        stripped = [ln for j, ln in enumerate(final_lines) if j not in covered]
        reconstructed_base = "".join(stripped).encode("utf-8")
        base_ok = reconstructed_base == base
        if not base_ok:
            # show a short mismatch
            print(f"STOP: {rel} FINAL-minus-new-entries != BASE")
            print(f"  recon {len(reconstructed_base)} base {len(base)}")
            unmapped.append({"file": rel, "line_no": 0, "text": "", "reason": "FINAL-minus-new != BASE"})

        last_touch = max((r["commit_index"] for r in added if r["commit_index"] is not None), default=None)
        intermediates = {}
        if not unmapped or not any(u.get("file") == rel for u in unmapped):
            for k in range(1, len(COMMITS) + 1):
                drop = set()
                for e in new_entries:
                    idx = UNIT_TO_COMMIT.get(e["unit"])
                    if idx is not None and idx > k:
                        drop.update(range(e["start"], e["end"]))
                kept = [ln for j, ln in enumerate(final_lines) if j not in drop]
                content = "".join(kept).encode("utf-8")
                intermediates[str(k)] = {
                    "sha256": sha256_bytes(content),
                    "nlines": len(kept),
                    "equals_final": content == final,
                    "touches": any(UNIT_TO_COMMIT.get(e["unit"]) == k for e in new_entries),
                }
            if last_touch is None or not intermediates[str(last_touch)]["equals_final"]:
                print(f"STOP: {rel} last-touch intermediate != FINAL")
                unmapped.append({"file": rel, "reason": "last-touch != FINAL"})

        files_out[rel] = {
            "final_sha256": sha256_bytes(final),
            "base_sha256": sha256_bytes(base),
            "final_nlines": len(final_lines),
            "base_reconstructed_ok": base_ok,
            "added": added,
            "last_touch_commit": last_touch,
            "intermediates": intermediates,
        }
        print(rel, "new_entries", len(new_entries), "added_lines", len(added), "base_ok", base_ok, "last_touch", last_touch)

    dest = SCRATCH / "build-lines-map.json"
    dest.write_text(json.dumps({
        "commit_subjects": [c["subject"] for c in COMMITS],
        "files": files_out,
        "unmapped": unmapped,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {dest} unmapped={len(unmapped)}")
    for u in unmapped:
        print(f"  {u}")
    return 0 if not unmapped else 2


if __name__ == "__main__":
    raise SystemExit(main())
