"""Prove an AI pass's script writes land on every peer: the two-peer end-to-end arm for F72-B.

The staged activity seats one AI actor whose own pass sends it a message (its receiver writes
self.Vel, the pattern BrowncoatBoss.lua uses) and gibs a free particle. Both are owner-only
decisions, so on an executable without the fix the writes land on the producing peer alone: the
per-tick strict comparison diverges and the peers' probe rows disagree. With the fix both peers make
the calls at the committed tick.

    python tools/test_net_ai_pass_writes.py --out <dir> --port 4841x [--exe-repo <tree>]

--exe-repo names the tree whose executable runs (its Data is junctioned read-only by the harness);
the default is this worktree. Every launch goes through the harness's isolated runner.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ACTIVITY = REPO / "tools/fixtures/ai_pass_writes_activity.lua"
WRITER = REPO / "tools/fixtures/ai_pass_writer.lua"
HARNESS = Path("D:/Projects/stage2_p4/recovery_e2e.py")
FAMILY_LOCK = Path("D:/mx/LEAD_FAMILY.lock")
BATTERY_LOCK = Path("D:/mx/LEAD_BATTERY.lock")
EXCLUSIVE_LOCK = Path("D:/mx/LEAD_EXCLUSIVE.lock")
PORT_RANGE = range(48400, 48420)
LANE_PORT_RANGE = range(48670, 48680)  # A second lane range, so this arm can run beside another one.
PRESET = "Determinism AI Pass Writes"
PROBE = re.compile(r"\[f72b-probe\] simms=(\d+) heard=(-?\d+) vel=([^ ]+) health=([^ ]+) victim=(\d) particles=(\d+)")

INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/AIPassWrites.lua\n"
    "\t\tLuaClassName = AIPassWrites\n\t\tMinTeamsRequired = 2\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n"
    "\t\tDefaultFogOfWar = 0\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultDeployUnits = 0\n"
)


def sha256(path: Path) -> str:
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def refuse_on_locks() -> None:
    for lock in (FAMILY_LOCK, BATTERY_LOCK, EXCLUSIVE_LOCK):
        if lock.exists():
            raise SystemExit(f"refusing to run: {lock} exists")


def stage_user_module(runtime: Path) -> Path:
    """The staged UserScenes module: the arm's activity and the actor script its AI pass runs."""
    module = Path(runtime) / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "AIPassWrites.lua").write_bytes(ACTIVITY.read_bytes())
    (module / "AIPassWriter.lua").write_bytes(WRITER.read_bytes())
    return module


def peer_log(run_dir: Path) -> str:
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = run_dir / name
        if path.exists():
            text += path.read_text(errors="replace")
    return text


def probe_rows(text: str) -> list[dict]:
    return [{"simms": int(simms), "heard": int(heard), "vel": vel, "health": health,
             "victim": int(victim), "particles": int(particles)}
            for simms, heard, vel, health, victim, particles in PROBE.findall(text)]


def check(checks: list[dict], name: str, ok: bool, detail: str, evidence: list) -> None:
    checks.append({"name": name, "status": "pass" if ok else "fail", "detail": detail,
                   "evidence": [str(item) for item in evidence]})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48414)
    parser.add_argument("--ticks", type=int, default=600)
    parser.add_argument("--timeout", type=float, default=420)
    parser.add_argument("--exe-repo", type=Path, default=REPO)
    args = parser.parse_args()
    if args.port not in PORT_RANGE and args.port not in LANE_PORT_RANGE:
        parser.error(f"port is outside {PORT_RANGE.start}..{PORT_RANGE.stop - 1} and "
                     f"{LANE_PORT_RANGE.start}..{LANE_PORT_RANGE.stop - 1}")
    refuse_on_locks()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    exe_repo = args.exe_repo.resolve()
    exe = exe_repo / "Cortex Command.exe"
    if not exe.exists():
        raise SystemExit(f"no executable at {exe}")

    sys.path[:0] = [str(REPO / "tools"), str(HARNESS.parent)]
    spec = importlib.util.spec_from_file_location("ai_pass_writes_harness", HARNESS)
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    harness.REPO, harness.EXE = exe_repo, exe
    harness.ROOT, harness.OUT = out, out / "e2e"

    # The activity is staged into the run's own UserScenes module, so the match names it.
    common = ["-net-match-service-preset", PRESET, "-net-match-service-module", "UserScenes.rte",
              "-net-match-mode", "pvpve"]
    lane = {"port": args.port, "delay": 0, "ticks": args.ticks, "timeout": args.timeout,
            "mode": "normal",
            "what": "an AI pass sends a message whose receiver writes sim state, and gibs a free object",
            "host": list(common), "client": list(common)}
    original_run = harness.run_isolated

    def prepare(*positional, **keywords):
        run = original_run(*positional, **keywords)
        stage_user_module(Path(run.cwd))
        return run

    harness.run_isolated = prepare
    result = harness.lane("ai_pass_writes", lane)
    logs = {peer: peer_log(out / "e2e/ai_pass_writes" / peer) for peer in ("host", "client")}
    rows = {peer: probe_rows(text) for peer, text in logs.items()}
    checks: list[dict] = []
    for peer in ("host", "client"):
        evidence = [out / "e2e/ai_pass_writes" / peer / "stdout.log"]
        heard = [row["heard"] for row in rows[peer]]
        check(checks, f"{peer}_heard_the_message", any(value > 0 for value in heard),
              f"heard={heard[:8]}", evidence)
        gone = [row["victim"] for row in rows[peer]]
        # The victim has to be there first and gone after: a target that never existed proves nothing.
        check(checks, f"{peer}_victim_gibbed", bool(gone) and gone[0] == 1 and gone[-1] == 0,
              f"victim={gone[:8]}", evidence)
    check(checks, "peers_agree", bool(rows["host"]) and rows["host"] == rows["client"],
          f"host rows={len(rows['host'])} client rows={len(rows['client'])} "
          f"first mismatch={next((pair for pair in zip(rows['host'], rows['client']) if pair[0] != pair[1]), None)}",
          [out / "e2e/ai_pass_writes"])
    compare_path = out / "e2e/ai_pass_writes/strict_compare.json"
    compare = json.loads(compare_path.read_text()) if compare_path.exists() else {}
    check(checks, "no_divergence", compare.get("first_divergence") is None and bool(compare.get("compared_ticks")),
          f"compared_ticks={compare.get('compared_ticks')} first_divergence={compare.get('first_divergence')} "
          f"subsystems={compare.get('divergent_subsystems')}", [compare_path])

    result["exe"] = str(exe)
    result["exe_sha256"] = sha256(exe)
    result["ai_pass_checks"] = checks
    result["ai_pass_pass"] = all(row["status"] == "pass" for row in checks)
    (out / "result.json").write_text(json.dumps(result, indent=2, default=str))
    print(json.dumps({"arm": "ai_pass_writes", "exe": str(exe), "exe_sha256": result["exe_sha256"],
                      "pass": result["ai_pass_pass"],
                      "checks": {row["name"]: row["status"] for row in checks}}, indent=2))
    return 0 if result["ai_pass_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
