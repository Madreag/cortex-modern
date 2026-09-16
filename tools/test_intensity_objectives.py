"""Two-peer comparer: saved intensity and objective positions stay shared.

The fixture drives GameIntensityCalculator and AddObjectivePoint(AboveHeadPos).
Saved values are compared through snapshot_runtime.project. Written, not run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / "tools/fixtures/intensity_objectives.lua"
PRESET = "Determinism Intensity Objectives"
SAVED = re.compile(
    r"\[intensity-objectives\] saved=([^\s]+) head=([-\d.]+),([-\d.]+) cam=([-\d.]+),([-\d.]+)"
)
INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/IntensityObjectives.lua\n"
    "\t\tLuaClassName = IntensityObjectives\n\t\tMinTeamsRequired = 2\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n"
    "\t\tDefaultFogOfWar = 0\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultDeployUnits = 0\n"
)


def sha(path: Path) -> str:
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def peer_log(run_dir: Path) -> str:
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = run_dir / name
        if path.exists():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def last_row(text: str):
    matches = SAVED.findall(text)
    if not matches:
        return None
    saved, head_x, head_y, cam_x, cam_y = matches[-1]
    return {
        "saved": saved,
        "head": (float(head_x), float(head_y)),
        "cam": (float(cam_x), float(cam_y)),
    }


def stage_module(runtime: Path) -> None:
    module = Path(runtime) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "IntensityObjectives.lua").write_bytes(FIXTURE.read_bytes())


def compare_peers(rows: dict[str, dict]):
    sys.path.insert(0, str(REPO / "tools"))
    import snapshot_runtime as runtime
    from test_compare_snapshots import RuntimeProjectionTests

    helper = RuntimeProjectionTests()
    host = helper.shared_intensity_activity(rows["host"]["saved"], rows["host"]["head"])[0]
    client = helper.shared_intensity_activity(rows["client"]["saved"], rows["client"]["head"])[0]
    return runtime.project(host, True) == runtime.project(client, True)


def inspect(root: Path, peers: tuple[str, ...]) -> dict:
    checks = {}
    details = {"saved": {}, "head": {}, "cam": {}}
    rows = {}
    for who in peers:
        row = last_row(peer_log(root / who))
        details["saved"][who] = None if row is None else row["saved"]
        details["head"][who] = None if row is None else row["head"]
        details["cam"][who] = None if row is None else row["cam"]
        checks[f"{who}_saved"] = row is not None and row["saved"] not in ("", "-0.5")
        if row is not None:
            rows[who] = row
    host = rows.get("host")
    client = rows.get("client")
    checks["cameras_differ"] = bool(host and client and host["cam"] != client["cam"])
    checks["saved_equal"] = bool(host and client and host["saved"] == client["saved"])
    checks["heads_equal"] = bool(host and client and host["head"] == client["head"])
    if host and client:
        checks["comparer_shared"] = compare_peers({"host": host, "client": client})
    else:
        checks["comparer_shared"] = False
    return {"pass": all(checks.values()), "checks": checks, "details": details}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48541)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--ticks", type=int, default=180)
    options = parser.parse_args()
    sys.path.insert(0, str(options.repo / "tools"))
    from run_sim_test import make_run

    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    os.environ["CCCP_HEADLESS"] = "1"
    peers = ("host", "client")
    runs = {}
    try:
        for who in peers:
            flags = ["-seed", "42", "-free-run-sim", "-net-match-service-e2e",
                     "-net-port", str(options.port), "-net-match-peers", "2",
                     "-net-match-ticks", str(options.ticks), "-net-match-input-delay", "3",
                     "-net-match-service-preset", PRESET, "-net-match-service-module", "UserScenes.rte",
                     "-net-autosave-seconds", "0",
                     "-net-match-report", str(root / f"{who}_report.json")]
            flags += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
            env = {"CCCP_HEADLESS": "1", "CCCP_INTENSITY_CAMERA": who}
            run = make_run(options.repo, flags, root / who, timeout=options.timeout, env=env)
            stage_module(Path(run.cwd))
            runs[who] = run
        for run in runs.values():
            run.start()
        for run in runs.values():
            run.wait()
        result = inspect(root, peers)
        result["driver_sha256"] = sha(Path(__file__))
        result["fixture_sha256"] = sha(FIXTURE)
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("PASS" if result["pass"] else "FAIL", "intensity_objectives", result["details"])
        return 0 if result["pass"] else 1
    finally:
        for run in runs.values():
            run.close()


if __name__ == "__main__":
    raise SystemExit(main())
