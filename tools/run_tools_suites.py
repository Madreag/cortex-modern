"""Run the tools' own suites: the checks that read the tree and never launch the engine.

`run_selftests.py` scores engine selftests; these read source and fixtures instead, so they run
here. Every suite is a separate process and the exit code is the worst of them.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

# The inventory tools live beside the lead's tools, outside the repository; a box without them reports N/A.
INVENTORY = Path(os.environ.get('CC_INVENTORY_DIR') or "D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory")
SUITES = (
    ("relay-gate", ["relay_gate_test.py"]),
    ("relay-cloudflare", ["relay_cloudflare_test.py"]),
    ("session-directory", ["session_directory/test_session_directory.py"]),
    ("acceptance-harness", ["test_acceptance_harness.py"]),
    ("e2e-video", ["test_e2e_video.py"]),
    ("lobby-wire", ["test_net_lobby_wire.py"]),
    ("settings-seed", ["test_settings_seed.py"]),
    ("launch-budget", ["-m", "unittest", "feel.test_launch_budget"]),
    ("cross-driver", ["-m", "unittest", "feel.test_cross_driver"]),
    ("cross-report", ["-m", "unittest", "feel.test_report"]),
    ("cross-oracles", ["-m", "unittest", "feel.test_cross_oracles"]),
    ("autosave-restore-oracles", ["-m", "unittest", "test_autosave_restore"]),
    ("compare-snapshots", ["test_compare_snapshots.py"]),
    ("snapshot-inventory-roles", ["test_snapshot_inventory_roles.py"]),
    ("snapshot-runtime", ["snapshot_runtime.py", "--self-test"]),
    ("print-discipline", ["test_print_discipline.py"]),
    ("menu-readback-platform", ["test_menu_readback.py", "--self-test"]),
    ("main-arg-loop", ["test_main_arg_loop.py"]),
    ("checkpoint-field-stamps", ["test_checkpoint_field_stamps.py"]),
    ("selftest-sanitizer-rows", ["test_run_selftests.py"]),
    ("selftest-runner-quiet-tail", ["run_selftests.py", "--self-test"]),
    ("plane-value-getters", ["test_plane_value_getters.py"]),
    ("edith-remote-box", ["edith/remote_box.py", "--self-test"]),
    ("ubsan-suppressions", ["sanitizers/check_ubsan_supp.py", "--self-test"]),
    ("vw-battery", ["vw_battery.py", "--self-test"]),
    ("mod-api-census-guard", ["mod_api_census.py", "--self-test"]),
    ("runner-feel-marker", ["test_win32_runner_feel_marker.py"]),
    ("runner-limits", ["test_win32_runner_limits.py"]),
    ("feel-engine-placement", ["test_feel_placement.py"]),
    ("feel-recorder-on-off-scope", ["test_feel_on_off_proof.py"]),
    ("soak-judgement", ["test_soak_two_peer.py"]),
    ("inventory-run-split", [str(INVENTORY / "run_split.py"), "--self-test"]),
    ("inventory-run-stream", [str(INVENTORY / "run_stream.py"), "--self-test"]),
    ("inventory-extract-defects", [str(INVENTORY / "extract_defects.py"), "--self-test"]),
    ("inventory-merge-defects", [str(INVENTORY / "merge_defects.py"), "--self-test"]),
    ("inventory-acceptance-manifest", [str(INVENTORY / "acceptance_manifest.py"), "--self-test"]),
    ("acceptance-collection", [str(INVENTORY / "test_acceptance_collection.py")]),
    ("acceptance-rows", ["-m", "unittest", "test_acceptance_ally_build", "test_acceptance_box_lease", "test_acceptance_box_mods",
                         "test_acceptance_clock_brackets", "test_acceptance_control_plan_integration", "test_acceptance_cross_report",
                         "test_acceptance_deferred", "test_acceptance_evidence", "test_acceptance_fixed_captures",
                         "test_acceptance_fixed_gates", "test_acceptance_frozen_report", "test_acceptance_frozen_tools",
                         "test_acceptance_image_report", "test_acceptance_local_host", "test_acceptance_mod",
                         "test_acceptance_native_admission", "test_acceptance_native_load", "test_acceptance_pipeline_controls",
                         "test_acceptance_posix_build", "test_acceptance_private_runtime", "test_acceptance_remote_tasks",
                         "test_acceptance_retained_links", "test_acceptance_rows", "test_acceptance_spectator",
                         "test_acceptance_spectator_tasks", "test_acceptance_storage", "test_acceptance_tip_runtime",
                         "test_acceptance_transfer", "test_cross_peers_public_row"]),
    ("world-rows", ["-m", "unittest", "test_world_image_sizes", "test_world_mod_cross", "test_world_soak", "test_world_soak_tasks"]),
)
# The cross driver's suite reads the Windows boxes' trees and ctypes.WinDLL; the other platforms run the cross peers, not this suite.
# The acceptance and world row suites test the Windows coordinator, which names its boxes' D: trees.
WINDOWS_ONLY = {"runner-feel-marker", "runner-limits", "feel-engine-placement", "cross-driver", "acceptance-rows", "world-rows"}


def run(repo: Path, name: str, argv: list[str], timeout: float) -> tuple[str, int, str]:
    command = [sys.executable, *argv] if argv[0] == '-m' else [sys.executable, str(repo / "tools" / argv[0]), *argv[1:]]
    try:
        done = subprocess.run(command, cwd=str(repo / "tools"), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return name, 124, f"timed out after {timeout}s"
    return name, done.returncode, (done.stdout + done.stderr)[-2000:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--only", action="append", default=[], help="run just these suite names")
    parser.add_argument('--out', type=Path, help='write the collection verdict JSON and its log')
    args = parser.parse_args()
    worst = 0
    results, logs, inputs = [], [], [Path(__file__)]
    for name, argv in SUITES:
        if args.only and name not in args.only:
            continue
        script = Path(argv[0])
        if argv[0] == '-m':
            inputs += [args.repo / 'tools' / (name.replace('.', '/') + '.py') for name in argv[2:]]
        else:
            inputs.append(script if script.is_absolute() else args.repo / 'tools' / script)
        if name in WINDOWS_ONLY and sys.platform != "win32" or script.is_absolute() and not script.is_file():
            line = f"[tools-suites] N/A {name}: {'Windows only' if name in WINDOWS_ONLY else f'{script} is absent on this box'}"
            print(line); logs.append(line)
            results.append(dict(name=name, status='NOT APPLICABLE', reason=line))
            continue
        name, code, output = run(args.repo.resolve(), name, argv, args.timeout)
        line = f"[tools-suites] {'PASS' if code == 0 else 'FAIL'} {name} exit={code}"
        print(line); logs.extend([line, output])
        results.append(dict(name=name, status='PASS' if code == 0 else 'FAIL', exit_code=code))
        if code != 0:
            print(output)
            worst = code
    if args.out:
        from verdict_artifact import write_verdict
        args.out.parent.mkdir(parents=True, exist_ok=True)
        log = args.out.with_suffix('.log'); log.write_text('\n'.join(logs) + '\n', encoding='utf-8')
        counts = {key: sum(row['status'] == value for row in results) for key, value in
                  (('passed', 'PASS'), ('failed', 'FAIL'), ('not_applicable', 'NOT APPLICABLE'))}
        write_verdict(args.out, passed=bool(counts['passed']) and not counts['failed'], counts=counts,
                      inputs=inputs, log=log, suites=results)
    return worst


if __name__ == "__main__":
    raise SystemExit(main())
