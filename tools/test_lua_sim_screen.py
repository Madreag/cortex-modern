"""The shipped activities' simulation must not follow this machine's window size.

Runs the stock Skirmish Defense - the LandingZoneMap activity the AI picks its delivery zones with -
twice, same seed, same ticks, one process each, with only the window size differing, and compares the
on-wire tick hashes strictly. A matched-size pair is the control. The AI drops' landing X is printed
by the arm's activity, so a divergence can be named in game terms instead of hashes.
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare_sim_traces import strict_compare
from run_sim_test import make_run
from test_telemetry_bundle import set_visual_resolution
from test_window_size_sim import first_object_divergence, parse_size, sha256_file

REPO = Path(__file__).resolve().parents[1]
FIXTURE = REPO / "tools/fixtures/stock_skirmish_activity.lua"
PRESET = "Determinism Stock Skirmish"
INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = " + PRESET + "\n"
    "\t\tSceneName = Ketanot Hills\n\t\tScriptPath = UserScenes.rte/StockSkirmish.lua\n"
    "\t\tLuaClassName = StockSkirmish\n\t\tMinTeamsRequired = 2\n\t\tIsTestActivity = 1\n"
    "\t\tTeamOfPlayer1 = 0\n\t\tPlayer1IsHuman = 1\n"
    "\t\tDefaultFogOfWar = 1\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultDeployUnits = 0\n"
)
PROBE = re.compile(r"\[lzprobe\] simms=(\d+) drop=(\w+) x=(-?[\d.]+)")


def write_module(module: Path, arm_trace: bool) -> Path:
    """The arm's preset, its activity script and its options, as a Userdata module directory."""
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX, encoding="utf-8")
    (module / "StockSkirmish.lua").write_bytes(FIXTURE.read_bytes())
    (module / "StockSkirmishOptions.lua").write_text(
        "return { armTrace = " + ("true" if arm_trace else "false") + " }\n", encoding="utf-8")
    return module


def stage_user_module(runtime: Path) -> None:
    """The arm's module, staged beside the private runtime of a single-process run."""
    write_module(Path(runtime) / "Userdata/UserScenes.rte", True)


def log_text(out: Path) -> str:
    text = ""
    for name in ("stdout.log", "runtime/LogConsole.txt"):
        path = out / name
        if path.exists():
            text += path.read_text(errors="replace")
    return text


def run_one(repo: Path, out: Path, size, ticks: int, seed: int, timeout: float, sim_dump: bool) -> dict:
    args = ["-scenario", PRESET, "-seed", str(seed), "-max-ticks", str(ticks), "-scenario-run-past-end",
            "-tick-hashes", "-out", str(out / "trace.json")]
    env = {"CCCP_HEADLESS": "1"}
    if sim_dump:
        env["CC_SIM_DUMP"] = f"1:{ticks}"
    run = make_run(repo, args, out, timeout, env=env)
    set_visual_resolution(run, *size)
    stage_user_module(Path(run.cwd))
    try:
        record = run.start().finish()
    except Exception as error:  # noqa: BLE001 - the record is the evidence either way
        record = {"error": repr(error)}
    finally:
        run.close()
    drops = [{"simms": int(simms), "drop": drop, "x": float(x)} for simms, drop, x in PROBE.findall(log_text(out))]
    return {"size": list(size), "record": {key: record.get(key) for key in ("pid", "exit_code", "timed_out", "elapsed_seconds")},
            "drops": drops, "out": str(out)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--emit-module", type=Path,
                        help="write the arm's Userdata module here for a two-peer run and exit")
    parser.add_argument("--ticks", type=int, default=2700)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--a-size", default="960x540")
    parser.add_argument("--b-size", default="640x360")
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--sim-dump", action="store_true", help="keep per-object CC_SIM_DUMP rows for attribution")
    options = parser.parse_args()
    if options.emit_module:
        # A match peer takes the same preset and script with the trace arming off.
        print(write_module(options.emit_module, False))
        return 0
    if not options.out:
        parser.error("--out is required unless --emit-module is set")

    options.out.mkdir(parents=True, exist_ok=False)
    sizes = {"a": parse_size(options.a_size), "b": parse_size(options.b_size)}
    result = {"ticks": options.ticks, "seed": options.seed, "preset": PRESET,
              "exe_sha256": sha256_file(options.repo / "Cortex Command.exe"), "runs": {}}
    for label, size in sizes.items():
        result["runs"][label] = run_one(options.repo, options.out / label, size, options.ticks,
                                        options.seed, options.timeout, options.sim_dump)
    traces = {label: options.out / label / "trace.json" for label in sizes}
    if all(path.exists() for path in traces.values()):
        passed, comparison = strict_compare(traces["a"], traces["b"], options.ticks)
    else:
        passed, comparison = False, {"reasons": ["a run wrote no trace"]}
    result["simulation"] = comparison
    result["objects"] = first_object_divergence(options.out / "a/trace.json.simdump.txt",
                                                options.out / "b/trace.json.simdump.txt")
    result["drops_match"] = result["runs"]["a"]["drops"] == result["runs"]["b"]["drops"]
    result["processes"] = {label: bool(result["runs"][label]["record"].get("exit_code") == 0
                                       and not result["runs"][label]["record"].get("timed_out")) for label in sizes}
    result["passed"] = bool(passed and result["drops_match"] and all(result["processes"].values()))
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    drops = len(result["runs"]["a"]["drops"])
    if result["passed"]:
        print(f"PASS lua sim screen: {comparison['compared_ticks']} ticks identical at "
              f"{options.a_size} vs {options.b_size}, {drops} AI drops at the same landing zones")
    else:
        print("FAIL lua sim screen: " + "; ".join(comparison.get("reasons") or ["a run did not finish"]), file=sys.stderr)
        print(f"drops a={result['runs']['a']['drops'][:4]} b={result['runs']['b']['drops'][:4]}", file=sys.stderr)
        objects = result["objects"]
        if objects.get("available") and objects.get("tick"):
            print(f"first differing object: tick {objects['tick']} {objects.get('object', 'census')} "
                  f"{json.dumps(objects.get('fields', []))}", file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
