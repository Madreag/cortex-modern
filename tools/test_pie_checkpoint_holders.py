"""Save and freshly load each supported pie holder, checking native values and aliases."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import zipfile

from run_sim_test import make_run, seed_settings, engine_executable, file_sha256

SHAPES = ("all", "actor", "device", "activity", "global", "scene", "external-object",
          "borrowed-menu", "owned-menu", "owned-slice", "preset")


def stage(run, shape):
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir()
    fixture = Path(__file__).with_name("fixtures") / "pie_checkpoint_holders.lua"
    baseline = (Path(__file__).with_name("feel") / "FeelBaseline.lua").read_text()
    baseline = re.sub(r"^local WINDOW_TICKS = \d+;$", "local WINDOW_TICKS = 12000;", baseline, flags=re.M)
    (module / "Holders.lua").write_text(baseline + fixture.read_text().replace("__SHAPE__", shape))
    (module / "Source.lua").write_text("function CheckpointSourceScript:StartScript() end\n")
    (module / "Index.ini").write_text(
        "DataModule\n\tModuleName = User Scenes\n"
        "\tAddGlobalScript = GlobalScript\n\t\tPresetName = Checkpoint Source\n"
        "\t\tScriptPath = UserScenes.rte/Source.lua\n\t\tLuaClassName = CheckpointSourceScript\n"
        "\tAddActivity = GAScripted\n\t\tCopyOf = P4 Alpha Duel\n"
        "\t\tPresetName = Determinism P4 Alpha Duel\n\t\tScriptPath = UserScenes.rte/Holders.lua\n"
        "\t\tLuaClassName = FeelBaseline\n")
    seed_settings(run, {"EnableLuaDebugging": "0", "EnableGlobalScript": "UserScenes.rte/Checkpoint Source",
                        "AutosaveSeconds": "0"})


def logs(out):
    # The engine's own lines are in stdout; the fixture's print lines are in each runtime's console log.
    texts = [p.read_text(errors="replace") for p in sorted(out.glob("*/*.log"))]
    texts += [p.read_text(errors="replace") for p in sorted(out.rglob("LogConsole.txt"))]
    return "\n".join(texts)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--shape", choices=SHAPES, default="all")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="an extra engine environment value, for a diagnostic run")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    env = {"CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1", **dict(item.split("=", 1) for item in a.env)}
    capture = make_run(a.repo, ["-scenario", "P4 Alpha Duel", "-seed", "42", "-num-lua-states", "4",
                              "-max-ticks", "122", "-checkpoint-capture-selftest"], a.out / "capture", 300, env=env)
    stage(capture, a.shape)
    try:
        captured = capture.start().finish()
    finally:
        capture.close()
    text = logs(a.out)
    rows = re.findall(r"^\[checkpoint-capture-selftest\] (PASS|FAIL) tick=(\d+) .*", text, re.M)
    archive = Path(capture.cwd) / "Autosaves/capture-120/image.ccsave"
    checks = {"capture_exit": captured.get("exit_code") == 0 and not captured.get("timed_out"),
              "two_exact_captures": rows == [("PASS", "60"), ("PASS", "120")],
              "archive": archive.is_file()}
    restored = None
    verified = []
    if all(checks.values()):
        with zipfile.ZipFile(archive) as z:
            checks["archive_integrity"] = z.testzip() is None
        load = make_run(a.repo, ["-load-game", "pie-holders", "-checkpoint-restore-continue", "-max-ticks", "120"],
                        a.out / "load", 300, env=env)
        saved = Path(load.cwd) / "Userdata/UserSavedGames.rte"
        saved.mkdir()
        (saved / "Index.ini").write_text("DataModule\n\tModuleName = User Saved Games\n")
        shutil.copy2(archive, saved / "pie-holders.ccsave")
        shutil.copytree(Path(capture.cwd) / "Userdata/UserScenes.rte", Path(load.cwd) / "Userdata/UserScenes.rte")
        seed_settings(load, {"EnableLuaDebugging": "0", "AutosaveSeconds": "0",
                             "EnableGlobalScript": "UserScenes.rte/Checkpoint Source"})
        try:
            restored = load.start().finish()
        finally:
            load.close()
        loaded = logs(a.out / "load")
        # stdout/stderr live directly in this run directory.
        loaded += "\n".join(p.read_text(errors="replace") for p in (a.out / "load").glob("*.log"))
        verified = list(map(int, re.findall(r"\[pie-holder-fixture\][ _]+verified[ _]+shape=" + re.escape(a.shape) + r"[ _]+updates=(\d+)", loaded)))
        continuation = re.search(r"\[checkpoint-restore-continue\] start=(\d+) end=(\d+).*verdict=(true|1)", loaded)
        checks.update(load_exit=restored.get("exit_code") == 0 and not restored.get("timed_out"),
                      loaded='[load-game] loaded "pie-holders"' in loaded,
                      continued=bool(continuation and int(continuation[2]) - int(continuation[1]) == 120),
                      restored_values_and_aliases=len(set(verified)) >= 4,
                      no_restore_error=not re.search(r"stack traceback|saved script state did not restore|pie holder alias changed|values changed|"
                                                     r"dropped its contributed slice|stopped aliasing|lost its actor|has no device submenu|could not plant", loaded))
    result = {"pass": all(checks.values()), "shape": a.shape, "checks": checks, "capture": captured,
              "load": restored, "verified": verified,
              "exe_sha256": file_sha256(engine_executable(a.repo))}
    (a.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result[k] for k in ("pass", "shape", "checks", "verified")}))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
