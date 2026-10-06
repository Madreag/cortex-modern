"""Run socket-free engine selftests and score them from stdout PASS tokens.

A suite that exits 0 with no `[<selftest>] PASS` line is FAIL. A FAIL token or a
missing final PASS is FAIL. Use --score-stdout to score a captured log without a launch.

Windows and POSIX both run it: on POSIX the binary comes from the POSIX runner
(`CCCP_TEST_BINARY`, else `<repo>/build-gns/CortexCommand`), and the digest below works
on the Python the Mac ships, which has no `hashlib.file_digest`.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

SELFTEST_FIXTURES = {
    "mod-api-shims": ["mod-api-shims.lua"],
}

SELFTESTS = [
    "controller-frame",
    "net-protocol",
    "net-identity",
    "net-session",
    "net-lockstep",
    "net-lockstep-released-claims",
    "net-lockstep-release-paths",
    "net-lockstep-seat-succession",
    "net-lockstep-seat-admission",
    "net-lockstep-cli",
    "net-match",
    "net-auth",
    "net-admission",
    "net-reconnect",
    "net-reconnect-session",
    "net-world-join",
    "net-rejoin-matrix",
    "net-roster",
    "camera-null-scene",
    "rotate-primitive",
    "sim-checksum",
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
    "single-player-reference",
    "preview-invariance",
    "preview-binding-exhaustive",
    "joystick-updater",
]
# Rows whose verdict carries a wall-clock window, so a loaded box can fail them without a defect. They run last, one at
# a time, after every other row has finished (the quiet tail). A measured product failure remains red. The collection
# owner may retry a recorded pre-launch HARNESS block under its existing policy. A row of SELFTESTS always runs
# in the tail; the others join it with --quiet-rows, and --quiet-rows only runs the tail alone. Their budgets never move.
LOAD_SENSITIVE = {
    # runner stalled state transfer amid session keepalives; the snapshot-load keepalive's 300 ms window.
    "net-match": ["-net-match-selftest"],
    # threaded_synced_update_pass_timing: 1,024 registered MOs, 150 us added per pass.
    "script-graph": ["-script-graph-selftest"],
}
# Existing standalone quiet-box helper budget; it does not authorize replacing a product failure.
QUIET_WAIT_S = 300
# The wall-clock budget checks inside those rows. A sanitizer build instruments every access, so its timings measure
# the instrumentation: there the budget lines are reported, not judged, and every other check still decides the row.
WALL_CLOCK_CHECKS = {"script-graph": ("threaded_synced_update_pass_timing", "threaded_synced_update_pass_timing_under_load")}
# A load-sensitive row's time cap per sanitizer: TSan's script-graph walk runs past the suite's 1800 s on the Mac.
SANITIZER_ROW_TIMEOUT = {"script-graph": {"tsan": 3600}}
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
        measured = "; ".join(re.sub(rf"^\[{re.escape(tag)}\] FAIL ", "", line)[:200] for line in budget)
        scored.update(**{"pass": True, "reason": f"sanitizer: budget not gated ({sanitizer} build), measured: {measured}"})
    scored["sanitizer"] = sanitizer
    scored["informational_budget_lines"] = budget
    return scored


# Every binding walked in a preview window on the pickup_fire replay's world: fixtures join at tick 150, the walk runs at 154.
BINDING_WALK_ARGS = ["-net-replay", "tools/fixtures/pickup_fire.ccreplay", "-input-script", "tools/fixtures/pickup_fire.txt",
                     "-max-ticks", "155", "-preview-binding-exhaustive-selftest"]
BINDING_WALK_FIXTURES = ["pickup_fire.ccreplay", "pickup_fire.txt"]
BINDING_WALK_TIMEOUT = 1800
# The TSan walk takes about 1030 s alone on the Mac and about 3100 s in a throttled GUI-session job.
BINDING_WALK_SANITIZER_TIMEOUT = {"tsan": 3600}
BINDING_WALK_CLASS = re.compile(r"^\[bindx\] class \S+ ", re.M)
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


class Interrupted(Exception):
    """A signal ended the suite; the result already written names the row it cut."""


def _interrupt(signum, _frame):
    raise Interrupted(signal.Signals(signum).name)


# A compiler or linker loads the box as much as another engine does.
BUILD_PROCESSES = {"cl.exe", "link.exe", "cc1", "cc1plus", "clang", "clang++", "ld", "ninja"}


def load_processes():
    """The engines and the builds running on the box as name:pid lists, or None when the process list cannot be read."""
    try:
        if sys.platform == "win32":
            listing = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=60).stdout
            rows = [(row[0], row[1]) for row in csv.reader(listing.splitlines()) if len(row) > 1]
        else:
            listing = subprocess.run(["ps", "-axo", "pid=,comm="], capture_output=True, text=True, timeout=60).stdout
            rows = [(Path(comm.strip()).name, pid) for pid, _, comm in (line.strip().partition(" ") for line in listing.splitlines())]
    except (OSError, subprocess.SubprocessError):
        return None
    return {"engines": [f"{name}:{pid}" for name, pid in rows if name.lower().startswith("cortex command") or name == "CortexCommand"],
            "builds": [f"{name}:{pid}" for name, pid in rows if name.lower() in BUILD_PROCESSES]}


def box_state():
    state = load_processes() or {"engines": None, "builds": None}
    if hasattr(os, "getloadavg"):
        state["load"] = [round(value, 2) for value in os.getloadavg()]
    return state


def wait_for_quiet_box(limit_s=QUIET_WAIT_S, poll_s=5.0):
    """Waits until no engine and no build runs on the box (other lanes' included), up to limit_s; the rerun starts either way."""
    start = time.monotonic()
    while True:
        state = box_state()
        state["waited_s"] = round(time.monotonic() - start, 1)
        if state["engines"] is None:
            return {**state, "quiet": None}
        busy = state["engines"] or state["builds"]
        if not busy or state["waited_s"] >= limit_s:
            return {**state, "quiet": not busy}
        time.sleep(poll_s)


def attempt_summary(scored, case):
    keys = ("pass", "reason", "exit_code", "timed_out", "fail_lines", "fatal", "box")
    return {"dir": str(case), **{key: scored.get(key) for key in keys}}


SP_REFERENCE = {
    "dump_sha256": "f2c46632cd337cfd5bc2eec5307bc4661c715d54a4ce3c251c4b6e9f86bcce79",
    "dump_bytes": 5445623,
    "portable_dump_sha256": "e0f297dbab31209869b02f1274cfb7fc208d3c6793b58d8517aee04750581a1b",
    "tick_hashes_sha256": "ddd5a0c7a4d20b9ef5f3be9cdd6d30376f97304c4feaa93283409adc0dc0d89f",
    "compared_trace_sha256": "fecd87e5e76c4419b0ef74b2ca73a03526dab9214532fe5cbbb98c5a2ff6f9a1",
}
SP_REFERENCE_INDEX = (
    "DataModule\n\tModuleName = User Scenes\n\tScanFolderContents = 1\n\tIgnoreMissingItems = 1\n"
    "\tAddActivity = GAScripted\n\t\tPresetName = Determinism PieSwitchSP\n\t\tSceneName = Grasslands\n"
    "\t\tScriptPath = UserScenes.rte/PieSwitchSP.lua\n\t\tLuaClassName = PieSwitchSP\n\t\tMinTeamsRequired = 1\n"
    "\t\tIsTestActivity = 1\n\t\tDefaultRequireClearPathToOrbit = 0\n\t\tDefaultFogOfWar = 0\n\t\tDefaultDeployUnits = 0\n"
)
SP_REFERENCE_HEXFLOAT = re.compile(rb"[-+]?0x[0-9a-f]+(?:\.[0-9a-f]*)?p[-+][0-9]+", re.I)


def reference_json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def run_single_player_reference(repo, make_run, case, timeout):
    """The fixed pie-close reference checks every dump field and tick hash without a retained runtime."""
    fixtures = repo / "tools/pie_lockstep/fixtures"
    trace = case / "trace.json"
    dump = case / "trace.json.simdump.txt"
    argv = ["-scenario", "PieSwitchSP", "-seed", "42", "-max-ticks", "320", "-tick-hashes", "-num-lua-states", "4",
            "-input-script", str(fixtures / "pie_open_then_next.txt"), "-out", str(trace)]
    run = make_run(repo, argv, case, timeout, env={"CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1", "CC_SIM_DUMP": "27:320"},
                   expected=[trace, dump])
    module = Path(run.cwd) / "Userdata/UserScenes.rte"
    module.mkdir(exist_ok=True)
    (module / "Index.ini").write_text(SP_REFERENCE_INDEX, encoding="utf-8")
    (module / "PieSwitchSP.lua").write_bytes((fixtures / "PieSwitchSP.lua").read_bytes())
    try:
        record = run.start().finish()
    finally:
        run.close()
    failures, observed = [], {}
    if record.get("exit_code") != 0:
        failures.append(f"single-player engine exit_code={record.get('exit_code')}")
    if record.get("timed_out"):
        failures.append("single-player engine timed out")
    if not record.get("evidence_complete"):
        failures.append("single-player run evidence is incomplete")
    try:
        raw = dump.read_bytes()
        observed["dump_sha256"] = hashlib.sha256(raw).hexdigest()
        observed["dump_bytes"] = len(raw)
        # Hexadecimal float spellings vary between standard libraries; their exact values stay in the portable dump.
        portable = SP_REFERENCE_HEXFLOAT.sub(lambda match: float.fromhex(match[0].decode("ascii")).hex().encode("ascii"), raw)
        portable = portable.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        observed["portable_dump_sha256"] = hashlib.sha256(portable).hexdigest()
        wanted_dump = ("dump_sha256", "dump_bytes") if sys.platform == "win32" else ("portable_dump_sha256",)
        for key in wanted_dump:
            if observed[key] != SP_REFERENCE[key]:
                failures.append(f"single-player {key} differs: got {observed[key]}, reference {SP_REFERENCE[key]}")
        document = json.loads(trace.read_text(encoding="utf-8-sig"))
        if len(document["runs"]) != 1:
            raise ValueError(f"single-player trace has {len(document['runs'])} runs")
        result = document["runs"][0]
        hashes = result["tick_hashes"]
        observed["tick_count"] = len(hashes)
        if [row["tick"] for row in hashes] != list(range(1, 321)):
            failures.append(f"single-player tick coverage differs: got {len(hashes)} rows")
        if result["scenario"] != "PieSwitchSP" or result["seed"] != 42 or result["ticks"] != 320 or result["passed"] is not True:
            failures.append("single-player scenario, seed, completion or tick count differs")
        observed["tick_hashes_sha256"] = reference_json_digest(hashes)
        compared = {key: result[key] for key in ("scenario", "seed", "ticks", "passed", "sim_config", "numeric", "strings", "final_total_hash", "tick_hashes")}
        compared["numeric"] = {key: value for key, value in compared["numeric"].items() if key != "__wall_seconds"}
        observed["compared_trace_sha256"] = reference_json_digest(compared)
        for key in ("tick_hashes_sha256", "compared_trace_sha256"):
            if observed[key] != SP_REFERENCE[key]:
                failures.append(f"single-player {key} differs: got {observed[key]}, reference {SP_REFERENCE[key]}")
    except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
        failures.append(f"single-player reference artifacts are invalid: {error}")
    scored = {"pass": not failures, "reason": "; ".join(failures), "fail_lines": failures,
              "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"), "binary": record.get("exe_sha256"),
              "reference": SP_REFERENCE, "observed": observed}
    (case / "reference_comparison.json").write_text(json.dumps(scored, indent=2), encoding="utf-8")
    return scored


def run_lockstep_cli(repo, make_run, case, timeout):
    """The two-peer gameplay entry runs a whole round with one frame of input delay."""
    case.mkdir(parents=True, exist_ok=False)
    runs, traces = [], []
    common = ["-net-lockstep", "-net-port", "47459", "-net-lockstep-input-delay", "1",
              "-scenario", "SimBaseline", "-seed", "42", "-max-ticks", "120", "-tick-hashes"]
    try:
        for role, network in (("host", ["-net-host"]), ("client", ["-net-join", "127.0.0.1"])):
            trace = case / (role + ".json")
            traces.append(trace)
            run = make_run(repo, common + network + ["-out", str(trace)], case / role, timeout,
                           env={"CCCP_HEADLESS": "1"}, expected=[trace])
            runs.append(run)
            run.start()
        records = [run.finish() for run in runs]
    finally:
        for run in runs:
            run.close()
    failures, hashes = [], []
    for role, record, trace in zip(("host", "client"), records, traces):
        if record.get("exit_code") != 0 or record.get("timed_out") or not record.get("evidence_complete"):
            stdout = (case / role / "stdout.log").read_text(errors="replace") + (case / role / "stderr.log").read_text(errors="replace")
            reason = next((line for line in stdout.splitlines() if "requires" in line or "failed" in line.lower()), stdout[-600:])
            failures.append(f"delay 1 {role} does not finish its round: {reason}")
        try:
            result = json.loads(trace.read_text(encoding="utf-8-sig"))["runs"][0]
            if result["passed"] is not True or result["ticks"] != 120:
                failures.append(f"{role} does not complete all 120 ticks")
            tick_hashes = result["tick_hashes"]
            if [item["tick"] for item in tick_hashes] != list(range(1, 121)):
                failures.append(f"{role} has missing tick hashes")
            hashes.append(tick_hashes)
        except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
            failures.append(f"{role} has no valid round trace: {error}")
    if len(hashes) == 2 and hashes[0] != hashes[1]:
        failures.append("the two delay 1 peers have different tick hashes")
    verdict = "FAIL" if failures else "PASS"
    line = f"[net-lockstep-cli-selftest] {verdict} delay_one_completes_a_round" + (": " + "; ".join(failures) if failures else "")
    print(line, flush=True)
    (case / "stdout.log").write_text(line + "\n", encoding="utf-8")
    return {"pass": not failures, "reason": "; ".join(failures), "fail_lines": failures,
            "exit_code": max(record.get("exit_code") or 0 for record in records),
            "timed_out": any(record.get("timed_out") for record in records), "binary": records[0].get("exe_sha256")}


def run_row(options, make_run, name, case, sanitizer):
    if name in LOAD_SENSITIVE:
        budget = max(options.timeout, SANITIZER_ROW_TIMEOUT.get(name, {}).get(sanitizer, 0))
        run = make_run(options.repo, LOAD_SENSITIVE[name], case, budget, fixtures=SELFTEST_FIXTURES.get(name))
        try:
            record = run.start().finish()
        finally:
            run.close()
        stdout = (case / "stdout.log").read_text(errors="replace") if (case / "stdout.log").exists() else ""
        scored = score_selftest(stdout, record.get("exit_code"), record.get("timed_out"), f"{name}-selftest")
        if sanitizer and not scored["pass"] and name in WALL_CLOCK_CHECKS:
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
        scored = score_detect(case_data, sanitizer)
        scored["binary"] = case_data.get("exe_sha256")
    elif name == "single-player-reference":
        scored = run_single_player_reference(options.repo, make_run, case, options.timeout)
    elif name == "net-lockstep-cli":
        scored = run_lockstep_cli(options.repo, make_run, case, options.timeout)
    elif name == "preview-invariance":
        from test_preview_invariance import run_case as invariance_case  # noqa: PLC0415

        scored = invariance_case(options.repo, case, options.timeout)
        scored["binary"] = scored.get("exe_sha256")
    else:
        walk = name == "preview-binding-exhaustive"
        budget = max(options.timeout, BINDING_WALK_SANITIZER_TIMEOUT.get(sanitizer, BINDING_WALK_TIMEOUT)) if walk else options.timeout
        run = make_run(options.repo, BINDING_WALK_ARGS if walk else [f"-{name}-selftest"], case, budget,
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
        if walk:
            scored.update(budget_s=budget, elapsed_s=record.get("elapsed_seconds"), walked_classes=len(BINDING_WALK_CLASS.findall(stdout)))
    return scored


def run_tail_row(options, make_run, name, out, sanitizer):
    """Retain the measured product result; the collection owner handles proved pre-launch HARNESS retries."""
    case = out / f"{name}-selftest"
    box = box_state()
    first = run_row(options, make_run, name, case, sanitizer)
    first["box"] = box
    return first


def plan_rows(quiet_rows=None, only=()):
    """The ordinary rows in suite order, then the quiet tail: every load-sensitive row of SELFTESTS, the others with --quiet-rows."""
    rows = [] if quiet_rows == "only" else [name for name in SELFTESTS if name not in LOAD_SENSITIVE]
    tail = [name for name in LOAD_SENSITIVE if name in SELFTESTS or quiet_rows]
    if only:
        rows = [name for name in SELFTESTS if name in only and name not in LOAD_SENSITIVE]
        tail = [name for name in LOAD_SENSITIVE if name in only]
    return rows, tail


def self_test():
    """The suite runner's own rows on stubbed engine runs: the tail's order and its quiet rerun."""
    import tempfile

    global run_row, wait_for_quiet_box
    failures = []

    def expect(ok, what):
        print(f"[run-selftests-selftest] {'PASS' if ok else 'FAIL'} {what}", flush=True)
        if not ok:
            failures.append(what)

    rows, tail = plan_rows()
    expect(tail == ["net-match"] and "net-match" not in rows and len(rows) + len(tail) == len(SELFTESTS), "default: net-match runs last, the count is the suite's")
    rows, tail = plan_rows("last")
    expect(tail == ["net-match", "script-graph"] and len(rows) + len(tail) == len(SELFTESTS) + 1, "--quiet-rows last: the tail adds script-graph")
    expect(plan_rows("only") == ([], ["net-match", "script-graph"]), "--quiet-rows only: the tail alone")
    expect(plan_rows(None, ["net-match", "rteerror"]) == (["rteerror"], ["net-match"]), "--only keeps a named row's place")
    real_row, real_wait = run_row, wait_for_quiet_box
    try:
        wait_for_quiet_box = lambda *args, **kwargs: {"engines": [], "quiet": True, "waited_s": 0.0}
        for outcomes, verdict in (([False, True], False), ([False, False], False), ([True], True)):
            calls = []

            def stub(options, make_run, name, case, sanitizer, outcomes=outcomes, calls=calls):
                passed = outcomes[len(calls)]
                calls.append(case)
                return {"pass": passed, "reason": "" if passed else f"FAIL token: [{name}-selftest] FAIL under load",
                        "exit_code": 0 if passed else 1, "timed_out": False, "fail_lines": [], "fatal": []}

            run_row = stub
            with tempfile.TemporaryDirectory() as scratch:
                scored = run_tail_row(None, None, "net-match", Path(scratch), None)
            attempts = scored.get("attempts", [])
            ok = scored['pass'] is verdict and len(calls) == 1 and not attempts
            expect(ok, f'first product result {outcomes[0]} retained; no rerun without a recorded HARNESS cause')
    finally:
        run_row, wait_for_quiet_box = real_row, real_wait
    print(f"[run-selftests-selftest] {'PASS' if not failures else 'FAIL'}", flush=True)
    return 0 if not failures else 1


def write_result(out, summary):
    (out / "result.json").write_text(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--score-stdout", type=Path, help="score a captured stdout log; no engine")
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--timed-out", action="store_true")
    parser.add_argument("--name", default="controller-frame-selftest")
    parser.add_argument("--sanitizer", choices=("asan", "tsan"),
                        help="with --score-stdout: the log is a sanitizer build's, so its wall-clock budget lines are measured")
    parser.add_argument("--result-json", type=Path, help="with --score-stdout: also write the score here")
    parser.add_argument("--launch-json", type=Path,
                        help="with --score-stdout: read the exit code and the timeout from the runner's launch.json")
    parser.add_argument("--quiet-rows", nargs="?", const="last", choices=("last", "only"),
                        help="also run every load-sensitive row in the quiet tail (only: the tail alone)")
    parser.add_argument("--only", action="append", default=[], metavar="ROW",
                        help="run just this row (repeatable); product failures remain failures")
    parser.add_argument("--self-test", action="store_true", help="the runner's own rows; no engine")
    options = parser.parse_args()

    if options.self_test:
        return self_test()
    if options.score_stdout:
        stdout = options.score_stdout.read_text(encoding="utf-8", errors="replace")
        if options.launch_json:
            launched = json.loads(options.launch_json.read_text(encoding="utf-8"))
            options.exit_code, options.timed_out = launched.get("exit_code"), bool(launched.get("timed_out"))
        scored = score_selftest(stdout, options.exit_code, options.timed_out, options.name)
        row = options.name.removesuffix("-selftest")
        if options.sanitizer and not scored["pass"] and row in WALL_CLOCK_CHECKS:
            scored = score_wall_clock_informational(stdout, {"exit_code": options.exit_code, "timed_out": options.timed_out},
                                                    row, options.sanitizer)
        if options.result_json:
            options.result_json.write_text(json.dumps({**scored, "case": row, "scored_by": "run_selftests.py --score-stdout"},
                                                      indent=2), encoding="utf-8")
        print(json.dumps(scored, indent=2))
        if scored["pass"]:
            print(f"PASS {options.name} pass_lines={scored['pass_lines']}")
        else:
            print(f"FAIL {options.name}: {scored['reason']}")
        return 0 if scored["pass"] else 1

    if options.repo is None or options.out is None:
        parser.error("--repo and --out are required unless --score-stdout is set")
    unknown = [name for name in options.only if name not in SELFTESTS and name not in LOAD_SENSITIVE]
    if unknown:
        parser.error(f"no such row: {', '.join(unknown)}")
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
    rows, tail = plan_rows(options.quiet_rows, options.only)
    summary = {"exe_sha256": exe_hash, "repo": str(options.repo), "passed": 0, "total": len(rows) + len(tail),
               "quiet_rows": tail, "sanitizer": sanitizer, "complete": False, "running": None, "results": results}
    for sig in (signal.SIGTERM, signal.SIGINT, getattr(signal, "SIGHUP", None)):
        if sig is not None:
            signal.signal(sig, _interrupt)
    try:
        for name in rows + tail:
            summary["running"] = name
            write_result(out, summary)
            try:
                if name in tail:
                    scored = run_tail_row(options, make_run, name, out, sanitizer)
                else:
                    scored = run_row(options, make_run, name, out / f"{name}-selftest", sanitizer)
            except Interrupted:
                raise
            except Exception as exc:  # noqa: BLE001 - a runner failure is this row's red, never an empty result
                scored = {"pass": False, "reason": f"runner error: {exc!r}"}
            results[name] = scored
            summary["passed"] = sum(1 for row in results.values() if row["pass"])
            print(json.dumps({"selftest": name, **{k: v for k, v in scored.items() if k != "binary"}}), flush=True)
    except Interrupted as stop:
        summary["interrupted"] = f"{stop} during {summary['running']}"
        write_result(out, summary)
        print(json.dumps({"interrupted": summary["interrupted"], "passed": summary["passed"], "total": summary["total"]}), flush=True)
        return 3
    summary.update(complete=True, running=None)
    write_result(out, summary)
    print(json.dumps({k: summary[k] for k in ("exe_sha256", "passed", "total")}, indent=2))
    return 0 if summary["passed"] == summary["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
