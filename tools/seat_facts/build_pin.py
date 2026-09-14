"""Capture compiled inputs around the single permitted Phase B build (does not build)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]
ROOT = Path("D:/mx/astra-f40-root-fix-20260914")
EXPORT = ROOT / "combined-source-40.manifest.json"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def capture():
    names = subprocess.check_output(["git", "-C", str(REPO), "ls-files", "--cached", "--others", "--exclude-standard", "-z", "Source", "RTEA.sln", "*.vcxproj", "*.props", "*.targets"]).decode().split("\0")
    return {name: {"sha256": sha(REPO / name)} for name in sorted(set(names) - {""})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("before", "after"))
    args = parser.parse_args()
    head = subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "HEAD"], text=True).strip()
    stamp = subprocess.check_output(["date", "+%Y-%m-%d %H:%M:%S MST"], text=True).strip()
    files = capture()
    if args.stage == "before":
        if EXPORT.exists():
            raise RuntimeError("source export already exists; retain it and use a separately logged build attempt")
        changed = sorted(name for name, value in capture().items() if files.get(name) != value)
        record = {"stamp": stamp, "head": head, "source_changed_during_export": changed, "files": files}
        EXPORT.write_text(json.dumps(record, indent=2), encoding="utf-8")
        if changed:
            raise RuntimeError("source changed while capturing build inputs")
    else:
        exported = json.loads(EXPORT.read_text())
        build = json.loads((ROOT / "build.json").read_text(encoding="utf-8-sig"))
        if exported["head"] != head or exported["files"] != files or build["exit_code"] != 0:
            raise RuntimeError("build failed or its source inputs changed")
        if build["exe_sha256"] != sha(REPO / "Cortex Command.exe"):
            raise RuntimeError("built executable changed")
        source = (REPO / "Source/Network/NetA7Journal.cpp").read_text()
        matches = re.findall(r'\{"capabilities", \{([^}]+)\}\}', source)
        if len(matches) != 1:
            raise RuntimeError("cannot identify the compiled A7 capability declaration")
        capabilities = re.findall(r'"([a-z0-9_]+)"', matches[0])
        if "shared_seat_view_v1" not in capabilities or len(capabilities) != len(set(capabilities)):
            raise RuntimeError("compiled A7 seat evidence declaration is absent or ambiguous")
        record = {"stamp": stamp, "head": head, "exe_sha256": build["exe_sha256"],
                  "artifacts": {str(path): sha(path) for path in (EXPORT, ROOT / "build.log", ROOT / "build.json")},
                  "a7_evidence": {"schema": 1, "capabilities": capabilities}}
        (ROOT / "reconnect-build.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(f"build input {args.stage}: PASS ({len(files)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
