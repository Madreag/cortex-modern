"""Run the retained A7/FG6D gates, then assert restored shared seats and local input zero."""
from __future__ import annotations

import argparse
import contextlib
import copy
import importlib.util
import json
import os
from pathlib import Path
import sys
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from check_reconnect import absent, journal, local_view, pair, physical_input, require
from phase_b import EXE, REPO, ROOT, footprint, instrument_runner, sha, stamp

A7_DEFAULT = Path("D:/Projects/reviews/takeover-20260909/a7-repaired-driver")
A7 = Path(os.environ.get("A7_DRIVER_DIR") or A7_DEFAULT)
FG6D = Path("D:/Projects/reviews/takeover-20260909/grok-workers/fg6d-battery")
SUBSTITUTION = ("substitute_commit", "substitute_returner_wins", "substitute_host_cancel", "substitute_bounds")


def driver_dir(path=None):
    # A runner on another host stages the retained driver beside itself.
    global A7
    A7 = Path(path or os.environ.get("A7_DRIVER_DIR") or A7_DEFAULT)
    return A7


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def judge(function):
    try:
        return function()
    except Exception as error:
        return {"pass": False, "error": f"{type(error).__name__}: {error}"}


def allocated(root):
    root.mkdir(parents=True, exist_ok=False)
    return root


def install_local_control_oracle(driver, log):
    original = driver.validate_local_control

    def validate(observed, expected):
        # Keep both oracle runs and both raw events. The old terminal assertion assumes
        # that a canonical seat is a physical input index; assert each namespace explicitly.
        row = {"stamp": stamp(), "observed": observed, "expected": expected}
        row["original"] = judge(lambda: (original(observed, expected), {"pass": True})[1])
        try:
            player = 0 if observed["peer"] == "host" else 1
            local_view(observed, player, [0, 0])
            local_view(expected, player, [0, 0])
            require(observed["seat_player"] == expected["seat_player"], "canonical seat changed on return")
            original({**observed, "seat_player": physical_input(observed)},
                     {**expected, "seat_player": physical_input(expected)})
            row["explicit_seat_and_input"] = {"pass": True}
        except Exception as error:
            row["explicit_seat_and_input"] = {"pass": False, "error": str(error)}
            raise
        finally:
            with Path(log).open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(row) + "\n")

    driver.validate_local_control = validate
    return original


def seat_driver(log, directory=None, module=None):
    """The retained A7 driver with the canonical seat oracle installed, for a runner on any host."""
    directory = Path(directory or A7)
    if module is None:
        load("a7_support", directory / "a7_support.py")
        module = load("seat_a7_driver", directory / "a7_driver.py")
    install_local_control_oracle(module, log)
    return module


def a7_run(manifest):
    support = load("a7_support", A7 / "a7_support.py")
    support.REPO = REPO
    driver = load("seat_a7_driver", A7 / "a7_driver.py")
    driver.REPO = REPO
    driver.now = stamp
    driver.ARMS = copy.deepcopy(driver.ARMS)
    for index, spec in enumerate(driver.ARMS.values()):
        spec["port"] = 43575 + index
        spec["capabilities"].add("shared_seat_view_v1")
    root = support.validate_output(ROOT / "a7")
    seat_driver(root / "local-control-oracle-runs.jsonl", module=driver)
    pin_factory = lambda source, path: support.Pin(source, path, repo=REPO)
    with support.clean_environment() as removed:
        result = driver.run_suite(root, 40, manifest, tuple(driver.ARMS), pin_factory=pin_factory)
    checks = {}
    for arm in result["arms"]:
        name = arm["arm"]
        folder = root / driver.ARMS[name]["folder"]
        returned = "client" if name in ("stagger_seat_survives", "leave_ack_dropped") else "returner"
        round_id = arm.get("context", {}).get("initial_round" if returned == "client" else "resumed_round")
        checks[name] = judge(lambda folder=folder, returned=returned, name=name, round_id=round_id:
            pair(journal(folder / "host/events.jsonl"), journal(folder / returned / "events.jsonl"),
                 1, [0, 0] if name == "coop_hand_back" else [0, 1], round_id=round_id))
    write(root / "seat-checks.json", {"stamp": stamp(), "checks": checks, "original_suite": result["execution_pass"],
        "inherited_test_variables_removed": removed,
        "oracle_change": "Run the original local-control validator on raw events, then assert canonical seat and physical input separately and run its physical-zero check on controller_input. Both runs and raw events are retained; every other original A7 assertion is unchanged."})
    return {"pass": result["execution_pass"] and len(checks) == 5 and all(row["pass"] for row in checks.values()), "checks": checks}


def h4_run(manifest, runner):
    os.environ.update(CC_H4_REPO=str(REPO), CC_H4_BUILD_MANIFEST=str(manifest), CC_H4_RUN_ROOT=str(ROOT / "h4"))
    root = allocated(ROOT / "h4")
    original_make = runner.make_run
    attempt = uuid.uuid4().hex
    support = sys.modules["a7_support"]
    pin = support.Pin(40, manifest, repo=REPO)

    def verify_lane():
        pin.verify(artifacts=True)
        require(support.GuardedFactory().engine_count() == 0, "another engine occupies the reconnect lane")

    def with_journal(*args, **kwargs):
        out = Path(args[2] if len(args) >= 3 else kwargs["out"])
        env = dict(kwargs.get("env") or {})
        env.update(CC_A7_EVENT_LOG=str(out / "events.jsonl"), CC_A7_RUN_ID=attempt,
                   CC_A7_PEER=out.name, CC_A7_BINARY_SHA256=sha(EXE), CCCP_HEADLESS="1")
        kwargs["env"] = env
        return original_make(*args, **kwargs)

    runner.make_run = with_journal
    results = {}
    try:
        verify_lane()
        common = load("common", FG6D / "h4gates/common.py")
        common.out_root = lambda name: allocated(root / "rejoin")
        rejoin = load("seat_rejoin", FG6D / "h4gates/rejoin_after_resync.py")
        rejoin.PORT_RESYNC, rejoin.PORT_REMATCH = 43587, 43588
        previous = sys.argv
        try:
            sys.argv = [str(rejoin.__file__), "--arm", "both"]
            original_result = rejoin.main()
        finally:
            sys.argv = previous
        directory = root / "rejoin"
        returned = judge(lambda: pair(journal(directory / "resync_host/events.jsonl"),
            journal(directory / "resync_client2/events.jsonl"), 1, [0, 1]))
        rematch = judge(lambda: pair(journal(directory / "rematch_host/events.jsonl"),
            journal(directory / "rematch_client/events.jsonl"), 1, [0, 1]))
        results["rejoin_after_resync"] = {"pass": original_result == 0 and returned["pass"] and rematch["pass"],
                                         "original_exit": original_result, "returner": returned, "rematch": rematch}
        write(directory / "seat-checks.json", results["rejoin_after_resync"])
        verify_lane()
        substitution = load("seat_substitution", REPO / "tools/h4_substitution_gates.py")
        substitution.PORTS = {name: 43582 + index for index, name in enumerate(SUBSTITUTION)}
        substitution.out_root = lambda base, name: allocated(Path(base) / name)
        for name in SUBSTITUTION:
            verify_lane()
            original_result = substitution.run_gate(name, REPO, root, False, sha(EXE))
            directory = root / name
            host = lambda: journal(directory / "host/events.jsonl")
            stayer = judge(lambda: pair(host(), journal(directory / "stayer/events.jsonl"), 2, [0, 1, 2]))
            checks = {"stayer": stayer}
            if name in ("substitute_commit", "substitute_returner_wins"):
                peer = "substitute" if name == "substitute_commit" else "returner"
                checks[peer] = judge(lambda: pair(host(), journal(directory / peer / "events.jsonl"), 1, [0, 1, 2]))
            # Cancel/bounds deliberately admit nobody. Assert that absence as well as the
            # surviving peer's exact seat/brain/input facts; do not invent a returner.
            denied = ["substitute"] if name in ("substitute_returner_wins", "substitute_host_cancel") else (
                ["applicant1", "applicant2", "applicant3"] if name == "substitute_bounds" else [])
            for peer in denied:
                checks[peer] = judge(lambda peer=peer: absent(journal(directory / peer / "events.jsonl")))
            results[name] = {"pass": original_result == 0 and all(row["pass"] for row in checks.values()),
                             "original_exit": original_result, "checks": checks}
            write(directory / "seat-checks.json", results[name])
            verify_lane()
            footprint()
    finally:
        runner.make_run = original_make
    return {"pass": len(results) == 5 and all(row["pass"] for row in results.values()), "rows": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-manifest", type=Path, default=ROOT / "reconnect-build.json")
    parser.add_argument("--a7-driver-dir", type=Path, default=None)
    args = parser.parse_args()
    driver_dir(args.a7_driver_dir)
    os.environ["CCCP_HEADLESS"] = "1"
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    build = json.loads(args.build_manifest.read_text(encoding="utf-8"))
    require(sha(EXE) == build["exe_sha256"], "executable differs from the compiled input manifest")
    runner = instrument_runner(build["exe_sha256"])
    results = {"stamp": stamp(), "build_manifest": str(args.build_manifest), "build_sha256": sha(args.build_manifest),
               "driver_sha256": sha(__file__), "check_sha256": sha(HERE / "check_reconnect.py"), "a7_driver_dir": str(A7),
               "retained_drivers": {str(path): sha(path) for path in (A7 / "a7_driver.py", A7 / "a7_support.py",
                   FG6D / "h4gates/common.py", FG6D / "h4gates/rejoin_after_resync.py", REPO / "tools/h4_substitution_gates.py",
                   REPO / "tools/h4_gate_evidence.py")}}
    with (ROOT / "reconnect.log").open("x", encoding="utf-8") as stream, contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
        results["a7"] = judge(lambda: a7_run(args.build_manifest))
        results["h4"] = judge(lambda: h4_run(args.build_manifest, runner))
    require(all(sha(path) == digest for path, digest in results["retained_drivers"].items()), "retained reconnect driver changed")
    results["pass"] = results["a7"]["pass"] and results["h4"]["pass"]
    write(ROOT / "reconnect-result.json", results)
    print(json.dumps(results, indent=2))
    return 0 if results["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
