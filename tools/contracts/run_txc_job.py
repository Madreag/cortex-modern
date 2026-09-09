"""Run one named job of a retained transaction-control command artifact on a chosen build.

The artifact (transaction-controls-source20.json) records complete argv arrays that name the tree they were produced
against. run_remaining_gates.py rewrites the interpreter, --out and --build-manifest for its own run; this rewrites
those and moves the harness and fixture paths to the tree being tested, so a worktree can run the retained job without
depending on another tree's working files. Everything else in the recorded argv is used verbatim.
"""

from pathlib import Path
import argparse
import json
import subprocess
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--job",
        required=True,
        help="job id in the command artifact, e.g. valid or refusals",
    )
    parser.add_argument(
        "--commands",
        type=Path,
        default=Path(
            "D:/Projects/reviews/recovery-2026-09-07/contract-audit/transaction-controls-source20.json"
        ),
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="the tree to test",
    )
    parser.add_argument(
        "--recorded-repo",
        default="D:\\Projects\\p4b-interp-validation",
        help="the tree the recorded argv names, whose paths move to --repo",
    )
    parser.add_argument(
        "--runs",
        type=Path,
        required=True,
        help="where to put the run directory and log",
    )
    parser.add_argument(
        "--manifest", type=Path, required=True, help="build manifest of --repo"
    )
    parser.add_argument(
        "--suffix", default="40", help="suffix for the run directory name"
    )
    parser.add_argument(
        "--print", action="store_true", help="show the rewritten argv without running"
    )
    options = parser.parse_args()

    recorded = options.recorded_repo.rstrip("\\/") + "\\"
    job = next(
        item
        for item in json.loads(options.commands.read_text(encoding="utf-8"))["jobs"]
        if item["id"] == options.job
    )
    argv = list(job["argv"])
    argv[0] = sys.executable
    for index, value in enumerate(argv):
        if value.startswith(recorded):
            argv[index] = str(options.repo / value[len(recorded) :].replace("\\", "/"))
    out = options.runs / f"full-runtime-transaction-{options.job}-{options.suffix}"
    argv[argv.index("--out") + 1] = str(out)
    argv[argv.index("--build-manifest") + 1] = str(options.manifest.resolve())

    if options.print:
        print(json.dumps({"job": options.job, "argv": argv}, indent=2))
        return 0

    options.runs.mkdir(parents=True, exist_ok=True)
    record = {
        "job": options.job,
        "purpose": job.get("purpose"),
        "argv": argv,
        "out": str(out),
        "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    (options.runs / f"{options.job}.command.json").write_text(
        json.dumps(record, indent=2)
    )
    print(json.dumps(record, indent=2), flush=True)

    with (options.runs / f"{options.job}.log").open("ab") as stream:
        stream.write(("\n$ " + " ".join(argv) + "\n").encode())
        stream.flush()
        code = subprocess.run(
            argv, stdout=stream, stderr=subprocess.STDOUT, cwd=options.repo
        ).returncode

    result = out / "result.json"
    verdict = (
        json.loads(result.read_text(encoding="utf-8"))
        if result.exists()
        else {"missing": True}
    )
    record.update(
        exit=code,
        finished=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        verdict={
            key: verdict.get(key)
            for key in (
                "status",
                "complete",
                "source_unchanged",
                "continuation_checks_passed",
            )
            if key in verdict
        },
    )
    (options.runs / f"{options.job}.command.json").write_text(
        json.dumps(record, indent=2)
    )
    print(
        json.dumps(
            {"job": options.job, "exit": code, "verdict": record["verdict"]}, indent=2
        ),
        flush=True,
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
