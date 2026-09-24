"""The headless assert policy's own row: an assert is answered the way a player answers it, and the run goes on.

    python tools/test_headless_assert.py --repo <tree> --out <dir> [--timeout 120]

A scripted run fires one assert through the menu automation. The row passes only when all three hold:
the fault line names a continued run, a later menu-script verdict proves the run kept going, and the process
still exits non-zero - a dismissed dialog is never a green run.
"""
import argparse
import json
import sys
from pathlib import Path

CONTINUED = "RTE Assert (headless, continued like Ignore)"
SHUTDOWN = "[assert] the run continued past an assert"


def run_case(repo: Path, out: Path, timeout: float = 120.0) -> dict:
    sys.path.insert(0, str(Path(repo) / "tools"))
    from run_sim_test import make_run  # noqa: PLC0415

    out.mkdir(parents=True, exist_ok=True)
    script = out / "assert.menu.txt"
    script.write_text("assert_screen MainScreen\nfire_assert a scripted assert for the headless policy row\n"
                      "assert_screen MainScreen\nexit\n", encoding="utf-8")
    run = make_run(repo, ["-menu-script", str(script)], out / "run", timeout)
    try:
        record = run.start().finish()
    finally:
        run.close()
    stdout = (out / "run/stdout.log").read_text(encoding="utf-8", errors="replace") if (out / "run/stdout.log").is_file() else ""
    lines = stdout.splitlines()
    # The POSIX runner keeps stderr, where the fault and ShutDown lines go, in its own file.
    stderr_path = out / "run/stderr.log"
    errors = stderr_path.read_text(encoding="utf-8", errors="replace").splitlines() if stderr_path.is_file() else []
    fault = next((line for line in lines if CONTINUED in line), "")
    after = [line for line in lines[lines.index(fault) + 1:] if "[menu-script] assert_screen" in line] if fault else []
    if not fault and errors:
        # Two files keep no order between them; the fire_assert step's verdict prints once the assert has returned.
        fault = next((line for line in errors if CONTINUED in line), "")
        fired = next((index for index, line in enumerate(lines) if line.startswith("[menu-script] fire_assert ")), None)
        after = [line for line in lines[fired + 1:] if "[menu-script] assert_screen" in line] if fault and fired is not None else []
    exit_code = record.get("exit_code")
    result = {
        "selftest": "headless-assert-continues",
        "exit_code": exit_code,
        "timed_out": bool(record.get("timed_out")),
        "fault_line": fault,
        "verdicts_after_assert": after[:2],
        "shutdown_line": next((line for line in lines + errors if SHUTDOWN in line), ""),
        "exe_sha256": record.get("exe_sha256"),
        "stdout": str(out / "run/stdout.log"),
    }
    result["pass"] = bool(fault and after and result["shutdown_line"] and exit_code not in (0, None)
                          and not result["timed_out"])
    result["reason"] = "" if result["pass"] else (
        "no continued-assert fault line" if not fault else
        "no menu verdict after the assert" if not after else
        "no ShutDown assert line" if not result["shutdown_line"] else
        f"exit_code={exit_code}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    options = parser.parse_args()
    result = run_case(options.repo.resolve(), options.out.resolve(), options.timeout)
    print(json.dumps(result, indent=2))
    print(f"[headless-assert-continues] {'PASS' if result['pass'] else 'FAIL'} {result['reason']}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
