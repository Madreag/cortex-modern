#!/usr/bin/env python3
"""W1 step 1 preflight: capture tree state and compare to handoff manifest."""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(r"D:\Projects\takeover-fixes")
SCRATCH = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w1-commit-split\scratch")
HANDOFF_MANIFEST = Path(r"D:\Projects\reviews\takeover-20260909\handoff-20260910\manifest.json")
COMMANDS_LOG = SCRATCH.parent / "commands.log"


def log_cmd(cwd: Path, line: str, returncode: int, extra: str = "") -> None:
    with COMMANDS_LOG.open("a", encoding="utf-8") as fh:
        fh.write(f"{datetime.now(timezone.utc).isoformat()} cwd={cwd}\n{line}\nexit={returncode}\n")
        if extra:
            fh.write(extra)
            if not extra.endswith("\n"):
                fh.write("\n")
        fh.write("\n")


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    cwd = cwd or REPO
    print("+ " + " ".join(cmd), flush=True)
    result = subprocess.run(cmd, cwd=cwd, capture_output=True)
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    extra = ""
    if stdout:
        extra += "--- stdout ---\n" + stdout
        if not stdout.endswith("\n"):
            extra += "\n"
    if stderr:
        extra += "--- stderr ---\n" + stderr
        if not stderr.endswith("\n"):
            extra += "\n"
    log_cmd(cwd, " ".join(cmd), result.returncode, extra)
    result.stdout_text = stdout
    result.stderr_text = stderr
    return result


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    if not COMMANDS_LOG.exists():
        COMMANDS_LOG.write_text("", encoding="utf-8")

    stash = run(["git", "stash", "list"])
    (SCRATCH / "stash-list.txt").write_text(stash.stdout_text, encoding="utf-8")
    stash_empty = stash.stdout_text.strip() == "" and stash.returncode == 0

    status = run(["git", "status", "--porcelain"])
    (SCRATCH / "status-before.txt").write_text(status.stdout_text, encoding="utf-8")

    raw = subprocess.run(["git", "diff", "--binary"], cwd=REPO, capture_output=True)
    (SCRATCH / "tracked-before.patch").write_bytes(raw.stdout)
    log_cmd(REPO, "git diff --binary", raw.returncode, f"bytes={len(raw.stdout)}\n")

    others = run(["git", "ls-files", "--others", "--exclude-standard"])
    untracked = [line.replace("\\", "/") for line in others.stdout_text.splitlines() if line.strip()]
    (SCRATCH / "untracked-list-before.txt").write_text(
        "\n".join(untracked) + ("\n" if untracked else ""), encoding="utf-8"
    )

    dest_root = SCRATCH / "untracked-before"
    if dest_root.exists():
        shutil.rmtree(dest_root)
    dest_root.mkdir(parents=True, exist_ok=True)
    for rel in untracked:
        src = REPO / rel
        dest = dest_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)

    status_paths: dict[str, str] = {}
    for line in status.stdout_text.splitlines():
        if not line:
            continue
        code = line[:2]
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        path = path.strip().strip('"').replace("\\", "/")
        status_paths[path] = code

    manifest = json.loads(HANDOFF_MANIFEST.read_text(encoding="utf-8"))
    manifest_files = manifest["files"]

    # Hash every status path plus every manifest path (manifest is vs main).
    to_hash = sorted(set(status_paths) | set(manifest_files))
    hashes: dict[str, dict] = {}
    for path in to_hash:
        full = REPO / path
        entry = {"status": status_paths.get(path, "committed-or-absent")}
        if full.is_file():
            entry["sha256"] = sha256_file(full)
            entry["size"] = full.stat().st_size
        elif not full.exists():
            entry["sha256"] = None
            entry["size"] = None
            entry["error"] = "missing"
        else:
            entry["sha256"] = None
            entry["size"] = None
            entry["error"] = "not a regular file"
        hashes[path] = entry

    (SCRATCH / "hashes-before.json").write_text(
        json.dumps(hashes, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    mismatches = []
    missing_in_tree = []
    extra_in_tree = []
    lib_files = []

    for path in sorted(status_paths):
        if path.endswith(".lib"):
            lib_files.append(path)
        elif path not in manifest_files:
            extra_in_tree.append(path)

    for path, meta in sorted(manifest_files.items()):
        full = REPO / path
        if not full.is_file():
            missing_in_tree.append(path)
            continue
        actual = hashes[path]["sha256"]
        if actual != meta["sha256"]:
            mismatches.append({
                "path": path,
                "manifest_sha256": meta["sha256"],
                "tree_sha256": actual,
                "manifest_size": meta.get("size"),
                "tree_size": hashes[path].get("size"),
            })

    head = run(["git", "rev-parse", "HEAD"])
    compare = {
        "stash_empty": stash_empty,
        "stash_list": stash.stdout_text.strip().splitlines(),
        "head": head.stdout_text.strip(),
        "manifest_head": manifest["head"],
        "head_ok": head.stdout_text.strip() == manifest["head"],
        "mismatches": mismatches,
        "missing_in_tree": missing_in_tree,
        "extra_in_tree": extra_in_tree,
        "lib_files": lib_files,
        "manifest_file_count": len(manifest_files),
        "status_path_count": len(status_paths),
        "hashes_match_manifest": not mismatches and not missing_in_tree and not extra_in_tree,
        "ok": stash_empty and not mismatches and not missing_in_tree and not extra_in_tree,
    }
    (SCRATCH / "manifest-compare-before.json").write_text(
        json.dumps(compare, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        "ok": compare["ok"],
        "stash_empty": stash_empty,
        "stash_list": compare["stash_list"],
        "head_ok": compare["head_ok"],
        "hashes_match_manifest": compare["hashes_match_manifest"],
        "mismatch_count": len(mismatches),
        "missing_in_tree": missing_in_tree,
        "extra_in_tree": extra_in_tree,
        "lib_files": lib_files,
    }, indent=2))
    if not stash_empty:
        print("STOP: git stash list is not empty")
        return 2
    if not compare["hashes_match_manifest"]:
        print("STOP: tree differs from handoff manifest")
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
