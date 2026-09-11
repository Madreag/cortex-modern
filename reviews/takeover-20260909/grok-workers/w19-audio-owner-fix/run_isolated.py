"""Launch the engine through win32_test_runner with a private runtime under D:\\mx\\w19."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(r"D:\Projects\audio-owner-registry")
sys.path.insert(0, str(REPO / "tools"))
from win32_test_runner import IsolatedRun  # noqa: E402


def sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def prepare_runtime(repo: Path, out: Path) -> Path:
    runtime = out / "runtime"
    runtime.mkdir(parents=True, exist_ok=False)
    for name in ("Mods", "ScreenShots", "Userdata", "Temp"):
        (runtime / name).mkdir()
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "New-Item -ItemType Junction -Path "
            + quote(runtime / "Data")
            + " -Target "
            + quote(repo / "Data")
            + " | Out-Null",
        ],
        check=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    settings = (repo / "Userdata/Settings.ini").read_text(encoding="utf-8-sig")
    values = {
        "MuteMaster": "1",
        "MuteMusic": "1",
        "MuteSounds": "1",
        "MasterVolume": "0",
        "MusicVolume": "0",
        "SoundVolume": "0",
        "Fullscreen": "0",
        "SkipIntro": "1",
        "EnableVSync": "0",
        "ResolutionX": "960",
        "ResolutionY": "540",
        "UseMultiDisplays": "0",
    }
    for name, value in values.items():
        pattern = rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*"
        settings, count = re.subn(pattern, lambda match: match[1] + value, settings)
        if count == 0:
            settings += f"\n\t{name} = {value}\n"
    (runtime / "Userdata/Settings.ini").write_text(settings, encoding="utf-8")
    return runtime


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("args", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    args = options.args[1:] if options.args[:1] == ["--"] else options.args
    if not args:
        parser.error("supply game arguments after --")
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(options.repo.resolve(), out)
    argv = [str(options.exe.resolve()), "-headless", *map(str, args)]
    env = {"TEMP": str(runtime / "Temp"), "TMP": str(runtime / "Temp")}
    run = IsolatedRun(argv, runtime, out, options.timeout, env=env)
    try:
        record = run.start().finish()
    finally:
        run.close()
    summary = {
        k: record.get(k)
        for k in (
            "pid",
            "exit_code",
            "timed_out",
            "cwd",
            "exe_path",
            "exe_sha256",
            "verdict_lines",
            "elapsed_seconds",
        )
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if record["exit_code"] == 0 and not record.get("timed_out") else 1


if __name__ == "__main__":
    raise SystemExit(main())
