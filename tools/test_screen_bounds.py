"""Write the per-screen arrays from player slots with no screen, in an isolated game.

Runs the Tests.rte ScreenBounds scenario (and, with --case arm, the -screen-bounds-selftest arm)
through the isolated runner and scores the fixture's own verdict lines. --exe runs another build's
executable against this tree's Data, so the same fixture text scores on a reference build.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

from run_sim_test import make_run, prepare_runtime

ASAN_MARKER = "ERROR: AddressSanitizer"


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def foreign_run(repo, exe, args, out, timeout):
    """make_run with another build's executable; the runtime, Data and settings stay this tree's."""
    if sys.platform != "win32":
        raise SystemExit("--exe is a Windows path; the posix runner takes its own executable")
    from win32_test_runner import IsolatedRun  # noqa: PLC0415

    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(repo, out)
    manifest = json.loads((out / "runtime.json").read_text(encoding="utf-8"))
    manifest["executable"] = str(Path(exe).resolve())
    (out / "runtime.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    argv = [str(Path(exe).resolve()), "-headless", *map(str, args)]
    env = {"TEMP": str(runtime / "Temp"), "TMP": str(runtime / "Temp")}
    return IsolatedRun(argv, runtime, out, timeout, env=env)


def launch(repo, exe, args, out, timeout):
    run = foreign_run(repo, exe, args, out, timeout) if exe else make_run(repo, args, out, timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(errors="replace") if (out / "stdout.log").exists() else ""
    return record, log


def asan_report(log):
    """The sanitizer's report from its first line, which is the only part that names the write."""
    index = log.find(ASAN_MARKER)
    if index < 0:
        return []
    return log[index:].splitlines()[:40]


def score(name, record, log, tag):
    verdicts = [line.strip() for line in log.splitlines() if line.strip().startswith(tag)]
    return {
        "case": name,
        "exit_code": record["exit_code"],
        "timed_out": record["timed_out"],
        "checks": {
            "process": record["exit_code"] == 0 and not record["timed_out"],
            "verdicts_present": bool(verdicts),
            "no_verdict_failed": not any(" FAIL" in line for line in verdicts),
            "no_sanitizer_report": ASAN_MARKER not in log,
            "desktop": record["input_desktop_before"] == record["input_desktop_after"],
        },
        "verdicts": verdicts[:60],
        "sanitizer": asan_report(log),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe", type=Path, help="another build's Cortex Command.exe")
    parser.add_argument("--case", choices=("scenario", "arm", "both"), default="both")
    parser.add_argument("--timeout", type=float, default=240)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=True)
    exe = options.exe or options.repo / "Cortex Command.exe"
    fixture = options.repo / "Data/Tests.rte/Activities/ScreenBounds.lua"

    cases = []
    if options.case in ("scenario", "both"):
        record, log = launch(
            options.repo, options.exe,
            ["-scenario", "ScreenBounds", "-seed", 42, "-max-ticks", 60, "-out", root / "scenario/report.json"],
            root / "scenario", options.timeout)
        cases.append(score("scenario", record, log, "[screen-bounds]"))
    if options.case in ("arm", "both"):
        record, log = launch(options.repo, options.exe, ["-screen-bounds-selftest"], root / "arm", options.timeout)
        cases.append(score("arm", record, log, "[screen-bounds-selftest]"))

    summary = {
        "exe": str(exe),
        "exe_sha256": digest(exe),
        "fixture_sha256": digest(fixture),
        "driver_sha256": digest(__file__),
        "repo": str(options.repo.resolve()),
        "passed": all(all(case["checks"].values()) for case in cases),
        "cases": cases,
    }
    (root / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
