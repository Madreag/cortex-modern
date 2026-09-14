"""Stock fakelag100_auto on this tree, ports 47563/47564.

    python tools/run_ak47_fl100.py --repo D:/Projects/item4-feel --out D:/mx/w90/fl100
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

from wait_engine_idle import wait_engine_idle

STAGE = Path("D:/Projects/stage2_p4")
FIXTURE = STAGE / "fixtures/single_shot.txt"


def sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    sys.modules[name] = loaded
    spec.loader.exec_module(loaded)
    return loaded


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path("D:/Projects/item4-feel"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    parser.add_argument("--no-fixture", action="store_true")
    options = parser.parse_args()
    exe = options.repo / "Cortex Command.exe"
    print("exe sha256:", sha256(exe), flush=True)
    sys.path.insert(0, str(options.repo / "tools"))
    sys.path.insert(0, str(STAGE))
    suite = load(STAGE / "recovery_e2e.py", "lane_e2e_w90_fl")
    suite.EXE = exe
    suite.REPO = options.repo
    spec = copy.deepcopy(suite.LANES["fakelag100_auto"])
    spec["port"] = 47563
    if not options.no_fixture:
        spec["host"] = list(spec.get("host", [])) + ["-input-script", str(options.fixture)]
    wait_engine_idle()
    suite.ROOT, suite.OUT = options.out, options.out
    print(f"fl100 port={spec['port']} host={spec.get('host')}", flush=True)
    result = suite.lane("fl100", spec)
    summary = {"pass": result["pass"], "failed": [c["name"] for c in result["checks"] if c["status"] != "pass"]}
    detail = {c["name"]: c["detail"] for c in result["checks"]}
    for key in ("simgated_strict", "prediction_executed", "host_process_exit", "client_process_exit", "replay_compare"):
        if key in detail:
            print(f"    {key}: {detail[key][:220]}", flush=True)
    (options.out / "lane_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
