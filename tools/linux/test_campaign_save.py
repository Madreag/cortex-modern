"""Campaign-format control using retained INI and its explicitly referenced files."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    probe = root / "probe/script.json"
    probe.parent.mkdir()
    probe.write_text(json.dumps({"schema": 1, "timeout_ms": 180000, "steps": [
        {"op": "wait", "elapsed_ms": 6500, "scope": "menu"},
        {"op": "menu", "command": "meta_command NewLoadButton"},
        {"op": "wait", "elapsed_ms": 500, "scope": "menu"},
        {"op": "menu", "command": "meta_command LoadButton"},
        {"op": "wait", "elapsed_ms": 5000, "scope": "menu"},
        {"op": "menu", "command": "meta_command ContinueButton"},
        {"op": "wait", "elapsed_ms": 15000}, {"op": "signal", "name": "started"},
        {"op": "wait", "elapsed_ms": 12000}, {"op": "signal", "name": "played"},
        {"op": "menu", "command": "game_key Escape down"},
        {"op": "menu", "command": "game_key Escape up"}, {"op": "finish"}]}))
    script = root / "load.menu.txt"
    script.write_text("wait_ms 1500\nactivate ButtonMainToMetaGame\nwait_ms 500\nactivate ButtonContinue\n"
                      f"wait_file {probe.parent / 'played.json'} 180\nwait_ms 500\nexit\n")
    run = make_run(args.repo.resolve(), ["-menu-script", script], root / "engine", 210,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)})
    target = Path(run.cwd) / "Userdata/UserSavesConquest.rte"
    target.mkdir(exist_ok=True)
    files = []
    with zipfile.ZipFile(args.inputs) as archive:
        for name in archive.namelist():
            if Path(name).name != name:
                raise ValueError("campaign inputs must be individual files")
            data = archive.read(name)
            (target / name).write_bytes(data)
            files.append({"name": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    result = {"pass": False, "files": files, "source": str(args.inputs.resolve())}
    try:
        result["record"] = run.start().finish()
        outcome = json.loads((probe.parent / "net-ui-result.json").read_text())
        readings = [step["observed"] for step in outcome.get("steps", []) if step["op"] == "signal"]
        frames = [observed["sim_frame"] for observed in readings]
        console = Path(run.cwd) / "LogConsole.txt"
        text = console.read_text(errors="replace") if console.exists() else ""
        result.update(probe=outcome, ticks=frames[-1] - frames[0] if frames else 0,
                      loaded="Successfully loaded Metagame 'AutoSave'" in text,
                      playing=len(readings) == 2 and all(not observed["paused"] and not observed["editing"] for observed in readings))
        result["pass"] = (result["loaded"] and result["playing"] and outcome.get("pass")
                          and result["ticks"] >= 600 and result["record"]["exit_code"] == 0)
    except Exception as error:
        result["error"] = repr(error)
    finally:
        run.close()
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key not in ("files", "record", "probe")}))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
