"""Run socket-free engine selftests and score them from stdout PASS tokens.

A suite that exits 0 with no `[<selftest>] PASS` line is FAIL. A FAIL token or a
missing final PASS is FAIL. Use --score-stdout to score a captured log without a launch.

Windows and POSIX both run it: on POSIX the binary comes from the POSIX runner
(`CCCP_TEST_BINARY`, else `<repo>/build-gns/CortexCommand`), and the digest below works
on the Python the Mac ships, which has no `hashlib.file_digest`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

SELFTEST_FIXTURES = {
    "mod-api-shims": ["mod-api-shims.lua"],
}

SELFTESTS = [
    "controller-frame",
    "net-protocol",
    "net-identity",
    "net-session",
    "net-lockstep",
    "net-match",
    "net-auth",
    "net-admission",
    "net-reconnect",
    "net-reconnect-session",
    "net-world-join",
    "net-rejoin-matrix",
    "camera-null-scene",
    "rotate-primitive",
    "cow-checkpoint",
    "frame-recorder",
    "float-text",
    "combo-key",
    "rteerror",
    "module-version",
    "ext-validate-version",
    "path-prefix",
    "limb-path",
    "menu-automation",
    "mod-api-shims",
    "settings-preferences",
    "single-module-harness",
    "headless-assert-continues",
    "render-window-scripts",
    "text-wrap",
    "save-refusal-diagnosis",
    "headless-render-cap",
    "preview-invariance",
    "preview-binding-exhaustive",
]
# Rows whose verdict carries a wall-clock budget, so a loaded box can fail them without a defect: the inventory
# runs them in its quiet stream. They stay out of the default rows; --quiet-rows runs them last, one at a time,
# after every other row has finished, and --quiet-rows only runs them alone. Their budgets never move.
LOAD_SENSITIVE = {
    # threaded_synced_update_pass_timing: 1,024 registered MOs, 150 us added per pass.
    "script-graph": ["-script-graph-selftest"],
}
# The wall-clock budget checks inside those rows. A sanitizer build instruments every access, so its timings measure
# the instrumentation: there the budget lines are reported, not judged, and every other check still decides the row.
WALL_CLOCK_CHECKS = {"script-graph": ("threaded_synced_update_pass_timing", "threaded_synced_update_pass_timing_under_load")}
SANITIZER_MARKERS = {b"clang_rt.asan": "asan", b"__asan_init": "asan", b"clang_rt.tsan": "tsan", b"__tsan_init": "tsan"}


def sanitizer_build(exe: Path):
    """The sanitizer an executable links, read from its bytes (the runtime's import name or init symbol), or None."""
    overlap = max(len(marker) for marker in SANITIZER_MARKERS)
    tail = b""
    with Path(exe).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 22), b""):
            window = tail + block
            for marker, kind in SANITIZER_MARKERS.items():
                if marker in window:
                    return kind
            tail = window[-overlap:]
    return None


def score_wall_clock_informational(stdout: str, record: dict, name: str, sanitizer: str) -> dict:
    """A sanitizer build's row with its declared budget lines reported, not judged. It passes only when those lines
    are its only FAIL lines, its closing verdict is a bare PASS (the checks that print one reached the end green),
    no fatal line was printed and the engine exited 1 by itself - the exit the budget checks alone give the script-graph
    row, whose closing PASS covers the master state and whose exit adds the threaded checks. Anything else keeps the plain score."""
    tag = f"{name}-selftest"
    checks = WALL_CLOCK_CHECKS.get(name, ())
    lines = (stdout or "").splitlines()
    budget = [line for line in lines if any(re.match(rf"^\[{re.escape(tag)}\] FAIL {re.escape(check)}(\s|$)", line) for check in checks)]
    rest = "\n".join(line for line in lines if line not in budget)
    closing = [match.group(0).split("] ", 1)[1].strip() for match in re.finditer(rf"^\[{re.escape(tag)}\] (PASS|FAIL)\s*$", rest, re.M)]
    named_passes = [line for line in lines if re.match(rf"^\[{re.escape(tag)}\] PASS \S", line)]
    excused = (bool(budget) and closing == ["PASS"] and not SUITE_FAIL.search(rest) and not FATAL.search(rest) and bool(named_passes)
               and record.get("exit_code") == 1 and not record.get("timed_out"))
    scored = score_selftest(stdout, record.get("exit_code"), record.get("timed_out"), tag)
    if excused:
        scored.update(**{"pass": True, "reason": f"{sanitizer} build: {len(budget)} wall-clock budget line(s) reported, not judged"})
    scored["sanitizer"] = sanitizer
    scored["informational_budget_lines"] = budget
    return scored


# Every binding walked in a preview window on the pickup_fire replay's world: fixtures join at tick 150, the walk runs at 154.
BINDING_WALK_ARGS = ["-net-replay", "tools/fixtures/pickup_fire.ccreplay", "-input-script", "tools/fixtures/pickup_fire.txt",
                     "-max-ticks", "155", "-preview-binding-exhaustive-selftest"]
BINDING_WALK_FIXTURES = ["pickup_fire.ccreplay", "pickup_fire.txt"]
BINDING_WALK_TIMEOUT = 1800
FATAL = re.compile(
    r"^.*(?:\bFAIL\b|RTE Assert|RTE Abort|stack traceback|Stack trace \(most recent call last\)).*$",
    re.M,
)
SUITE_PASS = re.compile(r"^\[(?P<tag>[^\]]+)\] PASS\s*$", re.M)
SUITE_FAIL = re.compile(r"^\[(?P<tag>[^\]]+)\] FAIL", re.M)


def engine_executable(repo: Path) -> Path:
    if sys.platform == "win32":
        return Path(repo) / "Cortex Command.exe"
    tools = str(Path(repo).resolve() / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    from posix_test_runner import resolve_binary  # noqa: PLC0415

    return resolve_binary(repo)


def sha256_of(path: Path) -> str:
    """hashlib.file_digest is 3.11; the Mac's own Python is older, so fall back to blocks."""
    digest = getattr(hashlib, "file_digest", None)
    with Path(path).open("rb") as stream:
        if digest is not None:
            return digest(stream, "sha256").hexdigest()
        hasher = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
        return hasher.hexdigest()


def score_selftest(stdout: str, exit_code, timed_out=False, name=None) -> dict:
    """PASS only with exit 0, no timeout, no FATAL, at least one [tag] PASS, no [tag] FAIL, last suite token PASS."""
    fatal = FATAL.findall(stdout or "")
    if name == "rteerror-selftest":
        fatal = [line for line in fatal if "RTE Assert (from worker thread)" not in line]
    pass_matches = list(SUITE_PASS.finditer(stdout or ""))
    fail_matches = list(SUITE_FAIL.finditer(stdout or ""))
    pass_lines = [m.group(0) for m in pass_matches]
    fail_lines = [m.group(0) for m in fail_matches]
    expected = f"[{name}] PASS" if name else None
    named_pass = any(m.group("tag") == name for m in pass_matches) if name else bool(pass_matches)
    last_pass = False
    if pass_matches or fail_matches:
        last = max(
            [(m.end(), "PASS") for m in pass_matches] + [(m.end(), "FAIL") for m in fail_matches],
            key=lambda item: item[0],
        )
        last_pass = last[1] == "PASS"
    ok = (
        exit_code == 0
        and not timed_out
        and not fatal
        and not fail_lines
        and len(pass_lines) >= 1
        and named_pass
        and last_pass
    )
    reason = ""
    if exit_code != 0:
        reason = f"exit_code={exit_code}"
    elif timed_out:
        reason = "timed_out"
    elif fatal:
        reason = f"fatal={fatal[0]}"
    elif fail_lines:
        reason = f"FAIL token: {fail_lines[0]}"
    elif len(pass_lines) < 1:
        reason = "zero PASS tokens"
    elif name and not named_pass:
        reason = f"missing {expected}"
    elif not last_pass:
        reason = "missing final PASS"
    return {
        "pass": bool(ok),
        "exit_code": exit_code,
        "timed_out": timed_out,
        "pass_lines": len(pass_lines),
        "fail_lines": fail_lines[:3],
        "fatal": fatal[:3],
        "reason": reason,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--score-stdout", type=Path, help="score a captured stdout log; no engine")
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--timed-out", action="store_true")
    parser.add_argument("--name", default="controller-frame-selftest")
    parser.add_argument("--quiet-rows", nargs="?", const="last", choices=("last", "only"),
                        help="also run the load-sensitive rows, last and one at a time (only: those rows alone)")
    options = parser.parse_args()

    if options.score_stdout:
        stdout = options.score_stdout.read_text(encoding="utf-8", errors="replace")
        scored = score_selftest(stdout, options.exit_code, options.timed_out, options.name)
        print(json.dumps(scored, indent=2))
        if scored["pass"]:
            print(f"PASS {options.name} pass_lines={scored['pass_lines']}")
        else:
            print(f"FAIL {options.name}: {scored['reason']}")
        return 0 if scored["pass"] else 1

    if options.repo is None or options.out is None:
        parser.error("--repo and --out are required unless --score-stdout is set")
    sys.path.insert(0, str(options.repo / "tools"))
    from run_sim_test import make_run  # noqa: PLC0415

    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    exe = engine_executable(options.repo)
    if not exe.is_file():
        parser.error(f"no engine executable at {exe}; build it, or set CCCP_TEST_BINARY on POSIX")
    exe_hash = sha256_of(exe)
    sanitizer = sanitizer_build(exe)
    results = {}
    rows = [] if options.quiet_rows == "only" else list(SELFTESTS)
    quiet = list(LOAD_SENSITIVE) if options.quiet_rows else []
    for name in rows + quiet:
        case = out / f"{name}-selftest"
        if name in LOAD_SENSITIVE:
            run = make_run(options.repo, LOAD_SENSITIVE[name], case, options.timeout)
            try:
                record = run.start().finish()
            finally:
                run.close()
            stdout = (case / "stdout.log").read_text(errors="replace") if (case / "stdout.log").exists() else ""
            scored = score_selftest(stdout, record.get("exit_code"), record.get("timed_out"), f"{name}-selftest")
            if sanitizer and not scored["pass"]:
                scored = score_wall_clock_informational(stdout, record, name, sanitizer)
            scored["binary"] = record.get("exe_sha256")
            scored["load_sensitive"] = True
        elif name == "headless-assert-continues":
            from test_headless_assert import run_case as assert_case  # noqa: PLC0415

            scored = assert_case(options.repo, case, options.timeout)
            scored["binary"] = scored.get("exe_sha256")
        elif name == "ext-validate-version":
            from test_ext_validate_version import run_case as ext_case  # noqa: PLC0415

            scored = ext_case(options.repo, case, options.timeout)
            scored["binary"] = scored.get("exe_sha256")
        elif name == "single-module-harness":
            from test_single_module_harness import run_case, score_detect  # noqa: PLC0415

            case_data = run_case(options.repo, case, options.timeout, ["-module", "Tests.rte"])
            scored = score_detect(case_data)
            scored["binary"] = case_data.get("exe_sha256")
        elif name == "headless-render-cap":
            from test_headless_render_cap import run_case, score_detect  # noqa: PLC0415

            case_data = run_case(options.repo, case, options.timeout)
            scored = score_detect(case_data)
            scored["binary"] = case_data.get("exe_sha256")
        elif name == "preview-invariance":
            from test_preview_invariance import run_case as invariance_case  # noqa: PLC0415

            scored = invariance_case(options.repo, case, options.timeout)
            scored["binary"] = scored.get("exe_sha256")
        else:
            walk = name == "preview-binding-exhaustive"
            run = make_run(options.repo, BINDING_WALK_ARGS if walk else [f"-{name}-selftest"], case,
                           max(options.timeout, BINDING_WALK_TIMEOUT) if walk else options.timeout,
                           fixtures=BINDING_WALK_FIXTURES if walk else SELFTEST_FIXTURES.get(name))
            try:
                record = run.start().finish()
            finally:
                run.close()
            stdout = (
                (case / "stdout.log").read_text(errors="replace")
                if (case / "stdout.log").exists()
                else ""
            )
            scored = score_selftest(
                stdout, record.get("exit_code"), record.get("timed_out"), f"{name}-selftest"
            )
            scored["binary"] = record.get("exe_sha256")
        results[name] = scored
        print(json.dumps({"selftest": name, **{k: v for k, v in scored.items() if k != "binary"}}), flush=True)
    summary = {
        "exe_sha256": exe_hash,
        "repo": str(options.repo),
        "passed": sum(1 for r in results.values() if r["pass"]),
        "total": len(rows) + len(quiet),
        "quiet_rows": quiet,
        "sanitizer": sanitizer,
        "results": results,
    }
    (out / "result.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: summary[k] for k in ("exe_sha256", "passed", "total")}, indent=2))
    return 0 if summary["passed"] == summary["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
