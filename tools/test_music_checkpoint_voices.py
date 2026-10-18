"""Stress the production music checkpoint visitor against changing live voices."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from run_sim_test import make_run
from run_selftests import sanitizer_build


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    run = make_run(args.repo, ["-script-graph-selftest"], args.out, args.timeout,
                   env={"CCCP_HEADLESS": "1", "CC_TEST_MUSIC_CHECKPOINT_ONLY": "1"})
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = "\n".join((args.out / name).read_text(encoding="utf-8", errors="replace")
                    for name in ("stdout.log", "stderr.log") if (args.out / name).exists())
    stress = re.search(r"voice_map_stress boundaries=(\d+) saves=(\d+) mutations=(\d+) mismatches=(\d+)", log)
    counts = dict(zip(("boundaries", "saves", "mutations", "mismatches"), map(int, stress.groups()))) if stress else {}
    sanitizer_error = bool(re.search(r"AddressSanitizer|ThreadSanitizer|runtime error:", log))
    passed = (record.get("exit_code") == 0 and not record.get("timed_out")
              and counts.get("boundaries") == 8 and counts.get("saves") == 1024
              and counts.get("mutations", 0) > 0 and counts.get("mismatches") == 0
              and "[music-voice-map-selftest] PASS" in log
              and not re.search(r"\bFAIL\b|RTE Abort|RTE Assert", log) and not sanitizer_error)
    exe = args.repo / "Cortex Command.exe"
    if not exe.exists():
        from posix_test_runner import resolve_binary
        exe = Path(resolve_binary(args.repo))
    result = {"pass": passed, "engine": record, "counts": counts,
              "sanitizer": sanitizer_build(exe), "sanitizer_error": sanitizer_error,
              "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("pass", "counts", "sanitizer", "sanitizer_error")}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
