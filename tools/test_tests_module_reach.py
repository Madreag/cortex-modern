"""Which launches load the engine's own Tests.rte module: the harness match modes, and no player launch.

Each case dumps the network identity manifest, whose module list is the engine's own record of what
LoadAllDataModules installed, then exits before any session starts.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from run_sim_test import make_run

TEST_MODULE = "Tests.rte"


def loaded_modules(manifest: Path) -> list[str]:
    if not manifest.is_file():
        return []
    return [entry["file_name"] for entry in json.loads(manifest.read_text(encoding="utf-8"))["modules"]]


def probe(repo: Path, root: Path, name: str, flags: list[str], timeout: float) -> dict:
    manifest = (root / name / "identity.json").resolve()
    run = make_run(repo, [*flags, "-net-identity-dump", str(manifest)], root / name, timeout,
                   env={"CCCP_HEADLESS": "1"}, expected=[manifest])
    try:
        record = run.start().finish()
    finally:
        run.close()
    modules = loaded_modules(manifest)
    return {"flags": flags, "exit_code": record.get("exit_code"), "timed_out": record.get("timed_out"),
            "module_count": len(modules), "modules": modules, "test_module_loaded": TEST_MODULE in modules,
            "stdout": str(root / name / "stdout.log")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--port", type=int, default=48690)
    parser.add_argument("--replay", type=Path, help="a recorded net match, for the playback case")
    options = parser.parse_args()
    repo, root = options.repo.resolve(), options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    cases = {
        "player_menu": ([], False),
        "player_net_host": (["-net-host", "-net-port", str(options.port)], False),
        "harness_match_e2e": (["-net-match-service-e2e"], True),
    }
    if options.replay:
        cases["harness_match_replay"] = (["-net-replay", str(options.replay.resolve())], True)
    result, checks = {}, {}
    for name, (flags, expected) in cases.items():
        result[name] = probe(repo, root, name, flags, options.timeout)
        result[name]["expected"] = expected
        checks[name] = result[name]["test_module_loaded"] == expected
        print(f"{'PASS' if checks[name] else 'FAIL'} {name}: {TEST_MODULE} loaded="
              f"{str(result[name]['test_module_loaded']).lower()} expected={str(expected).lower()} "
              f"modules={result[name]['module_count']} flags={' '.join(flags) or '(none)'}")
    result["checks"] = checks
    result["passed"] = all(checks.values())
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
