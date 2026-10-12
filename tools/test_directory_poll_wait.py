"""One host posts through a local directory while its signal poll is disarmed."""

import argparse
import json
from pathlib import Path
import subprocess

from run_sim_test import engine_executable, make_run
from test_directory_cadence import digest, local_directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=41941)
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(exist_ok=False)
    result = {"source_tip": subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
              "binary_sha256": digest(engine_executable(repo)), "public_directory": False}
    with local_directory(out / "directory", "poll-wait", args.port) as (server, requests, pin):
        run = make_run(repo, ["-net-directory-selftest"], out / "host", timeout=30,
                       env={"CCCP_TEST_DIRECTORY_CASE": "host-post-wait", "CCCP_TEST_DIRECTORY_URL": f"https://127.0.0.1:{args.port}",
                            "CCCP_TEST_DIRECTORY_PIN": pin})
        try:
            record = run.start().finish()
        finally:
            run.close()
    events = requests.rows
    posts = [e for e in events if e["kind"] == "signal" and e["status"] == 429]
    beats = [e for e in events if e["kind"] == "heartbeat"]
    wait_end = max((e["reply_at_ms"] + e["retry_wait_s"] * 1000 for e in posts), default=0)
    checks = {"post_wait": len(posts) == 1, "no_poll": not any(e["kind"] == "signals" for e in events),
              "heartbeat_after_wait": bool(beats) and all(e["at_ms"] >= wait_end and e["status"] == 200 for e in beats),
              "heartbeat_bound": bool(beats) and beats[0]["at_ms"] <= wait_end + 1250,
              "engine_exit": record["exit_code"] == 0 and not record["timed_out"]}
    result.update(pass_=all(checks.values()), checks=checks, record=record, requests=events)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"pass": result["pass_"], "checks": checks, "out": str(out)}))
    return 0 if result["pass_"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
