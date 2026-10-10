"""Detect retained preview effects with held-clock draws, then compare a single-player frame."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from PIL import Image, ImageChops

from run_sim_test import make_run, seed_settings


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def stage_scene(run, name, fixture):
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(exist_ok=True)
    index = (
        "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
        f"\tAddActivity = GAScripted\n\t\tPresetName = Determinism {name}\n"
        f"\t\tSceneName = Grasslands\n\t\tScriptPath = UserScenes.rte/{name}.lua\n"
        f"\t\tLuaClassName = {name}\n\t\tMinTeamsRequired = 1\n\t\tIsTestActivity = 1\n"
        "\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n"
    )
    (module / "Index.ini").write_text(index, encoding="utf-8")
    (module / f"{name}.lua").write_bytes(fixture.read_bytes())


def execute(run):
    try:
        return run.start().finish()
    finally:
        run.close()


def detector(repo, root, executable):
    root.mkdir(parents=True, exist_ok=False)
    native = root / "effects.json"
    menu = root / "hold.menu.txt"
    menu.write_text(f"wait_file {native.as_posix()} 230\nexit\n", encoding="utf-8")
    args = ["-scenario", "EffectsLinger", "-seed", "42", "-max-ticks", "120", "-out", root / "trace.json",
            "-menu-script", menu, "-menu-script-out", root / "menu.json"]
    run = make_run(repo, args, root / "engine", timeout=240,
                   env={"CCCP_HEADLESS": "1", "CCCP_TEST_EFFECTS_LINGER": str(native)})
    if executable:
        run.argv[0] = str(executable.resolve())
    run.env["CCCP_SETTINGSPATH"] = str(Path(run.cwd) / "Userdata/Settings.ini")
    seed_settings(run, {"LocalPrediction": 1, "LocalPredictionMaxTicks": 20, "ResolutionX": 960,
                        "ResolutionY": 540, "ResolutionMultiplier": 1, "ShowAdvancedPerfStats": 0})
    stage_scene(run, "EffectsLinger", repo / "tools/fixtures/EffectsLinger.lua")
    record = execute(run)
    observed = json.loads(native.read_text(encoding="utf-8")) if native.is_file() else {}
    healthy = record.get("exit_code") == 0 and not record.get("timed_out") and len(observed.get("cases", [])) == 2 and len(observed.get("preview_cases", [])) == 6
    result = {"record": record, "native": observed, "complete": healthy, "pass": healthy and observed.get("pass") is True,
              "topology": "single-box", "proof": False, "scope": "production shot, MO lifetime and glow draw list with held input and ticks owed"}
    write_json(root / "result.json", result)
    for rate in (8, 60):
        cases = [row for row in observed.get("cases", []) if row["tps"] == rate]
        preview_cases = [row for row in observed.get("preview_cases", []) if row["tps"] == rate]
        passed = healthy and len(cases) == 1 and all(row["pass"] for row in cases) and all(row["pass"] for row in preview_cases)
        late = sum(row.get("late_glows", 0) for row in cases)
        expired = sum(row.get("stale_flash_observations", 0) + row.get("stale_explosion_observations", 0) for row in cases)
        old = sum(not row.get("preview_bound", False) for row in cases)
        orphan = sum(row["remaining_ghosts"] for row in preview_cases)
        print(f"{'GREEN' if passed else 'RED'} {rate} tps: late_glows={late} stale_effects={expired} older_than_max={old} orphan_previews={orphan}", flush=True)
    return result


def single_player(repo, root, executable):
    root.mkdir(parents=True, exist_ok=False)
    probe = root / "probe/probe.json"
    steps = [
        {"op": "wait_file", "path": "Userdata/UserScenes.rte/connection-guard-ready.json", "scope": "menu"},
        {"op": "wait", "paused": True, "sim_at_least": 4, "scope": "menu"},
        {"op": "wait", "renders": 4, "scope": "menu"},
        {"op": "assert", "equals": {"service": "Idle", "screen": "Gameplay", "paused": True,
                                  "sim_frame": 4, "local_actor_alive": True},
         "connections_absent": True, "scope": "menu"},
        {"op": "screenshot_pair", "name": "guard", "scope": "menu"},
        {"op": "signal", "name": "done", "scope": "menu"},
        {"op": "finish"},
    ]
    write_json(probe, {"schema": 1, "timeout_ms": 90000, "steps": steps})
    menu = root / "hold.menu.txt"
    menu.write_text(f"wait_file {probe.parent.as_posix()}/done.json 230\nwait_ms 250\nexit\n", encoding="utf-8")
    args = ["-scenario", "ConnectionGuard", "-seed", "42", "-max-ticks", "120", "-out", root / "trace.json",
            "-menu-script", menu, "-menu-script-out", root / "menu.json"]
    run = make_run(repo, args, root / "engine", timeout=240,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)})
    if executable:
        run.argv[0] = str(executable.resolve())
    run.env["CCCP_SETTINGSPATH"] = str(Path(run.cwd) / "Userdata/Settings.ini")
    seed_settings(run, {"ResolutionX": 960, "ResolutionY": 540, "ResolutionMultiplier": 1,
                        "ShowAdvancedPerfStats": 0})
    stage_scene(run, "ConnectionGuard", repo / "tools/fixtures/EffectsLingerGuard.lua")
    record = execute(run)
    output = probe.parent / "net-ui-result.json"
    observed = json.loads(output.read_text(encoding="utf-8")) if output.is_file() else {}
    frames = list((Path(run.cwd) / "ScreenShots").glob("guard_composited*.png"))
    return {"record": record, "probe": observed, "frame": str(frames[0]) if len(frames) == 1 else None,
            "pass": record.get("exit_code") == 0 and not record.get("timed_out") and
                    observed.get("pass") is True and observed.get("complete") is True and len(frames) == 1}


def compare_guard(before, after):
    result = {"pass": False, "before": before, "after": after}
    if before["pass"] and after["pass"]:
        with Image.open(before["frame"]) as a, Image.open(after["frame"]) as b:
            a, b = a.convert("RGB"), b.convert("RGB")
            if a.size == b.size == (960, 540):
                changed = sum(pixel != (0, 0, 0) for pixel in ImageChops.difference(a, b).getdata())
                result.update(pass_=changed == 0, changed_pixels=changed,
                              before_rgb_sha256=hashlib.sha256(a.tobytes()).hexdigest(),
                              after_rgb_sha256=hashlib.sha256(b.tobytes()).hexdigest())
                result["pass"] = result.pop("pass_")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--guard-before-exe", type=Path)
    args = parser.parse_args()
    args.repo = args.repo.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    result = detector(args.repo, args.out / "detector", args.exe)
    if args.guard_before_exe:
        before = single_player(args.repo, args.out / "guard-before", args.guard_before_exe)
        after = single_player(args.repo, args.out / "guard-after", args.exe)
        guard = compare_guard(before, after)
        write_json(args.out / "single-player.json", guard)
        print("SINGLE PLAYER changed_pixels=" + str(guard.get("changed_pixels", "missing")), flush=True)
        result["pass"] &= guard["pass"]
        result["single_player"] = guard
    write_json(args.out / "summary.json", result)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
