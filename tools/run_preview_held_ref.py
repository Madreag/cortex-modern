"""Install a UserScenes actor script and launch through IsolatedRun."""
import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path

TREE = Path(r"D:\Projects\value-observations")
sys.path.insert(0, str(TREE / "tools"))
from run_sim_test import prepare_runtime  # noqa: E402
from win32_test_runner import IsolatedRun  # noqa: E402

LOCK_EXCLUSIVE = Path(r"D:\mx\LEAD_EXCLUSIVE.lock")
LOCK_BATTERY = Path(r"D:\mx\LEAD_BATTERY.lock")
INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n"
    "\tIgnoreMissingItems = 1\n"
)
DEFERRAL = Path(
    r"D:\Projects\reviews\claude-review-2026-09-08\lanes\sound-ai-deferral\fixtures\sound_ai_deferral.lua"
)
SHORT_SAMPLE = TREE / "Data/Base.rte/Sounds/GUIs/FocusChange.flac"
LONG_SAMPLE = TREE / "Data/Base.rte/Sounds/Actors/JetpackLoop1.flac"


def sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def cortex_running() -> bool:
    result = __import__("subprocess").run(
        [
            "pwsh",
            "-NoProfile",
            "-Command",
            "Get-Process 'Cortex Command*' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id",
        ],
        capture_output=True,
        text=True,
    )
    return bool((result.stdout or "").strip())


def wait_gates() -> None:
    deadline_lock = time.time() + 3 * 3600
    deadline_engine = time.time() + 2 * 3600
    while LOCK_EXCLUSIVE.exists() or LOCK_BATTERY.exists():
        if time.time() > deadline_lock:
            raise SystemExit("timeout waiting for lead lock")
        print("waiting lead lock", flush=True)
        time.sleep(60)
    while cortex_running():
        if time.time() > deadline_engine:
            raise SystemExit("timeout waiting for Cortex Command")
        print("waiting Cortex Command*", flush=True)
        time.sleep(30)


def install_module(runtime: Path, files: dict[str, Path]) -> None:
    module = runtime / "Userdata/UserScenes.rte"
    module.mkdir(parents=True, exist_ok=True)
    (module / "Index.ini").write_text(INDEX)
    for name, src in files.items():
        (module / name).write_text(src.read_text(encoding="utf-8"))


def launch(out: Path, flags: list[str], files: dict[str, Path] | None, timeout: float) -> dict:
    wait_gates()
    out.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(TREE, out)
    if files:
        install_module(runtime, files)
        if "sound_ai_deferral.lua" in files:
            (runtime / "Userdata/UserScenes.rte/ProbeShort.flac").write_bytes(SHORT_SAMPLE.read_bytes())
            (runtime / "Userdata/UserScenes.rte/ProbeLong.flac").write_bytes(LONG_SAMPLE.read_bytes())
    exe = TREE / "Cortex Command.exe"
    argv = [str(exe), "-headless", *flags]
    env = {
        "TEMP": str(runtime / "Temp"),
        "TMP": str(runtime / "Temp"),
        "CCCP_HEADLESS": "1",
        "PATH": str(exe.parent) + os.pathsep + os.environ.get("PATH", ""),
    }
    process = IsolatedRun(argv, runtime, out, timeout=timeout, env=env)
    try:
        record = process.start().finish()
    finally:
        process.close()
    return {
        "exe": str(exe),
        "exe_sha256": sha256(exe),
        "exit_code": record.get("exit_code"),
        "timed_out": record.get("timed_out"),
        "argv": argv,
        "out": str(out),
    }


def first_diff_token(before: Path, after: Path) -> str:
    left = before.read_bytes()
    right = after.read_bytes()
    limit = min(len(left), len(right))
    i = 0
    while i < limit and left[i] == right[i]:
        i += 1
    if i == limit and len(left) == len(right):
        return "identical"
    chunk = right[max(0, i - 40) : min(len(right), i + 80)]
    text = chunk.decode("utf-8", "replace")
    match = re.search(r"[A-Za-z_][A-Za-z0-9_]*", text)
    return f"offset={i} token={match.group(0) if match else '?'} context={text!r}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("kind", choices=("lpinv-write", "lpinv-control", "deferral", "script-graph", "crash-green"))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--shared-slot", action="store_true")
    args = parser.parse_args()
    fixtures = TREE / "tools/fixtures"
    extra = ["-local-prediction-shared-slot"] if args.shared_slot else []
    if args.kind == "lpinv-write":
        script = fixtures / "preview_held_ref_write.lua"
        record = launch(
            args.out,
            [
                "-net-replay",
                "D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay",
                "-tick-hashes",
                "-max-ticks",
                "221",
                "-input-script",
                "D:/Projects/stage2_p4/fixtures/pickup_fire.txt",
                "-out",
                str(args.out / "trace.json"),
                "-local-prediction-depth",
                "7",
                "-local-prediction-invariance",
                "153:1,4,7,12:1,3",
                "-test-script",
                "UserScenes.rte/preview_held_ref_write.lua",
                *extra,
            ],
            {"preview_held_ref_write.lua": script},
            args.timeout,
        )
    elif args.kind == "lpinv-control":
        script = fixtures / "preview_held_ref_control.lua"
        record = launch(
            args.out,
            [
                "-net-replay",
                "D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay",
                "-tick-hashes",
                "-max-ticks",
                "221",
                "-input-script",
                "D:/Projects/stage2_p4/fixtures/pickup_fire.txt",
                "-out",
                str(args.out / "trace.json"),
                "-local-prediction-depth",
                "7",
                "-local-prediction-invariance",
                "153:1,4,7,12:1,3",
                "-test-script",
                "UserScenes.rte/preview_held_ref_control.lua",
                *extra,
            ],
            {"preview_held_ref_control.lua": script},
            args.timeout,
        )
    elif args.kind == "deferral":
        record = launch(
            args.out,
            [
                "-scenario",
                "ActorStress",
                "-seed",
                "42",
                "-max-ticks",
                "280",
                "-num-lua-states",
                "1",
                "-test-script",
                "UserScenes.rte/sound_ai_deferral.lua",
                "-out",
                str(args.out / "trace.json"),
                *extra,
            ],
            {"sound_ai_deferral.lua": DEFERRAL},
            args.timeout,
        )
    elif args.kind == "script-graph":
        record = launch(args.out, ["-script-graph-selftest", "-num-lua-states", "4"], None, args.timeout)
    else:
        record = launch(
            args.out,
            [
                "-net-replay",
                "D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay",
                "-tick-hashes",
                "-max-ticks",
                "221",
                "-input-script",
                "D:/Projects/stage2_p4/fixtures/pickup_fire.txt",
                "-local-prediction-depth",
                "7",
                "-local-prediction-event-ledger",
                "153",
                "-out",
                str(args.out / "trace.json"),
            ],
            None,
            args.timeout,
        )
    (args.out / "driver.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(json.dumps({k: record[k] for k in ("exit_code", "timed_out", "exe_sha256", "out")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
