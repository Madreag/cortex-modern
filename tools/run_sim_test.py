"""Run the existing game executable with private writable files and retained evidence."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

if __package__:
    from .win32_test_runner import IsolatedRun
else:
    from win32_test_runner import IsolatedRun


def prepare_runtime(repo, out):
    repo, out = Path(repo).resolve(), Path(out).resolve()
    runtime = out / "runtime"
    runtime.mkdir(parents=True, exist_ok=False)
    for name in ("Mods", "ScreenShots", "Userdata", "Temp"):
        (runtime / name).mkdir()
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", "New-Item -ItemType Junction -Path " + quote(runtime / "Data") + " -Target " + quote(repo / "Data") + " | Out-Null"], check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    settings = (repo / "Userdata/Settings.ini").read_text(encoding="utf-8-sig")
    values = {"MuteMaster": "1", "MuteMusic": "1", "MuteSounds": "1", "MasterVolume": "0", "MusicVolume": "0", "SoundVolume": "0", "Fullscreen": "0", "SkipIntro": "1", "EnableVSync": "0", "ResolutionX": "960", "ResolutionY": "540", "UseMultiDisplays": "0"}
    for name, value in values.items():
        pattern = rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*"
        settings, count = re.subn(pattern, lambda match: match[1] + value, settings)
        if count == 0:
            settings += f"\n\t{name} = {value}\n"
    (runtime / "Userdata/Settings.ini").write_text(settings, encoding="utf-8")
    manifest = {"executable": str(repo / "Cortex Command.exe"), "cwd": str(runtime), "data": str(repo / "Data"), "settings_sha256": hashlib.sha256(settings.encode()).hexdigest(), "settings_overrides": values}
    (out / "runtime.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return runtime


def make_run(repo, args, out, timeout=120, env=None, expected=None):
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(repo, out)
    argv = [str(Path(repo).resolve() / "Cortex Command.exe"), "-headless", *map(str, args)]
    private_env = dict(env or {})
    private_env.update(TEMP=str(runtime / "Temp"), TMP=str(runtime / "Temp"))
    return IsolatedRun(argv, runtime, out, timeout, env=private_env, evidence_expected=expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--expect", type=Path, action="append", default=[])
    parser.add_argument("args", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    args = options.args[1:] if options.args[:1] == ["--"] else options.args
    if not args:
        parser.error("supply explicit game test arguments after --")
    run = make_run(options.repo, args, options.out, options.timeout, expected=options.expect)
    try:
        record = run.start().finish()
    finally:
        run.close()
    print(json.dumps({k: record.get(k) for k in ("pid", "exit_code", "timed_out", "cwd", "verdict_lines", "input_desktop_before", "input_desktop_after")}, indent=2))
    return 0 if record["exit_code"] == 0 and record["evidence_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
