#!/usr/bin/env python3
"""Run A7 arms one after another. A parallel group binds the returner's connect-gate under Mac load."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True, help="directory that holds run_a7_mac.py")
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("arms", nargs="+", help="arm names, in the order they must run")
    options = parser.parse_args(argv)
    runner = options.harness / "run_a7_mac.py"
    if not runner.is_file():
        raise SystemExit("missing A7 wrapper: " + str(runner))
    options.out.mkdir(parents=True, exist_ok=True)
    results = []
    for index, arm in enumerate(options.arms, 1):
        out = options.out / f"{index:02d}-{arm}"
        if out.exists():
            raise SystemExit("refusing existing run directory: " + str(out))
        command = [sys.executable, str(runner), str(options.repo), str(options.manifest),
                   str(options.build_root), str(out), arm]
        print("running A7 arm sequentially:", arm, flush=True)
        proc = subprocess.run(command, cwd=str(options.harness),
                              env={**os.environ, "CCCP_HEADLESS": "1"})
        results.append({"arm": arm, "out": str(out), "exit": proc.returncode})
        (options.out / "progress.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        if proc.returncode not in (0, 1):
            print("wrapper non-oracle exit", proc.returncode, "for", arm, "; continuing", flush=True)
    (options.out / "group-result.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
