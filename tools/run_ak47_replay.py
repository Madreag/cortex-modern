"""Play ak47_fire or pickup_fire through run_sim_test.make_run.

    python tools/run_ak47_replay.py --repo <tree> --out <dir> --timeout 180 -- --flags
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run
from wait_engine_idle import wait_engine_idle

DEFAULT_REPLAY = Path(r"D:\Projects\stage2_p4\fixtures\ak47_fire.ccreplay")
DEFAULT_SCRIPT = Path(r"D:\Projects\stage2_p4\fixtures\ak47_fire.txt")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--replay", type=Path, default=DEFAULT_REPLAY)
    parser.add_argument("--script", type=Path, default=DEFAULT_SCRIPT)
    parser.add_argument("--max-ticks", default="700")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("flags", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    flags = options.flags[1:] if options.flags[:1] == ["--"] else options.flags
    env = {}
    for item in options.env:
        key, _, value = item.partition("=")
        env[key] = value
    wait_engine_idle()
    argv = [
        "-net-replay",
        str(options.replay),
        "-tick-hashes",
        "-max-ticks",
        str(options.max_ticks),
        "-input-script",
        str(options.script),
        "-out",
        str(options.out.resolve() / "trace.json"),
        *flags,
    ]
    run = make_run(options.repo, argv, options.out.resolve(), timeout=options.timeout, env=env)
    try:
        record = run.start().finish()
    finally:
        run.close()
    print(record.get("exit_code"), record.get("timed_out"), record.get("pid"), flush=True)
    return 0 if record.get("exit_code") == 0 and record.get("evidence_complete") else 1


if __name__ == "__main__":
    raise SystemExit(main())
