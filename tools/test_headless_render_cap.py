"""Measure headless presentation rate and the startup cap line.

A short SimBaseline run records FrameMan feel frames. The harness default must
print `[render] headless presentation cap=60hz` and stay at or under 66 presents
per second. The feel file is not passed, so RenderCapHz = 0 and = 60 stay the
override path.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402

CAP_LINE = re.compile(r"\[render\] headless presentation cap=(60hz|off)")
MAX_PRESENT_HZ = 66.0
MIN_INTERVAL_S = 1.0
MIN_FRAMES = 30
TICKS = 150


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def sample_gpu(stop: threading.Event, samples: list[str]) -> None:
    while not stop.is_set():
        try:
            text = subprocess.check_output(
                ["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                text=True,
                timeout=5,
            )
            samples.append(text.strip())
        except (OSError, subprocess.SubprocessError):
            samples.append("unavailable")
        stop.wait(0.2)


def frame_rows(raw_path: Path) -> list[dict]:
    rows = []
    if not raw_path.is_file():
        return rows
    for line in raw_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if value.get("type") == "frame" and "present_end_ms" in value:
            rows.append(value)
    active = [row for row in rows if row.get("active") is True]
    return active or rows


def measure(rows: list[dict]) -> dict:
    if len(rows) < 2:
        return {"frames": len(rows), "interval_s": 0.0, "presentations_per_second": 0.0}
    first = float(rows[0]["present_end_ms"])
    last = float(rows[-1]["present_end_ms"])
    interval_s = max(0.0, (last - first) / 1000.0)
    rate = ((len(rows) - 1) / interval_s) if interval_s > 0 else 0.0
    return {"frames": len(rows), "interval_s": interval_s, "presentations_per_second": rate}


def run_case(repo: Path, out: Path, timeout: float) -> dict:
    os.environ["CCCP_HEADLESS"] = "1"
    out = Path(out)
    feel = out / "feel"
    feel.mkdir(parents=True, exist_ok=False)
    args = [
        "-scenario",
        "SimBaseline",
        "-seed",
        "42",
        "-max-ticks",
        str(TICKS),
        "-feel-measure",
        str(feel),
    ]
    samples: list[str] = []
    stop = threading.Event()
    sampler = threading.Thread(target=sample_gpu, args=(stop, samples), daemon=True)
    sampler.start()
    run = make_run(repo, args, out / "run", timeout, env={"CCCP_HEADLESS": "1"})
    try:
        record = run.start().finish()
    finally:
        run.close()
        stop.set()
        sampler.join(timeout=2)
    runtime = out / "run" / "runtime"
    stdout = read_text(out / "run" / "stdout.log")
    console = read_text(runtime / "LogConsole.txt")
    combined = console + "\n" + stdout
    cap = CAP_LINE.search(combined)
    raw = feel / "raw.jsonl"
    stats = measure(frame_rows(raw))
    gpu_values = []
    for sample in samples:
        try:
            gpu_values.append(float(sample.split()[0].replace("%", "")))
        except (TypeError, ValueError, IndexError):
            continue
    return {
        "args": args,
        "exit_code": record.get("exit_code"),
        "timed_out": record.get("timed_out"),
        "exe_sha256": record.get("exe_sha256"),
        "stdout": str(out / "run" / "stdout.log"),
        "console": str(runtime / "LogConsole.txt"),
        "raw": str(raw),
        "cap_line": cap.group(0) if cap else "",
        "cap": cap.group(1) if cap else "",
        "gpu_samples": samples,
        "gpu_max": max(gpu_values) if gpu_values else None,
        "gpu_avg": (sum(gpu_values) / len(gpu_values)) if gpu_values else None,
        **stats,
        "log_text": combined,
    }


def score_detect(case: dict) -> dict:
    rate = float(case.get("presentations_per_second") or 0.0)
    interval = float(case.get("interval_s") or 0.0)
    frames = int(case.get("frames") or 0)
    cap_ok = case.get("cap") == "60hz"
    rate_ok = rate <= MAX_PRESENT_HZ
    measured = frames >= MIN_FRAMES and interval >= MIN_INTERVAL_S
    process_ok = case.get("exit_code") == 0 and case.get("timed_out") is not True
    ok = bool(process_ok and cap_ok and measured and rate_ok)
    if case.get("timed_out"):
        reason = "timed_out"
    elif case.get("exit_code") != 0:
        reason = f"exit_code={case.get('exit_code')}"
    elif not cap_ok:
        reason = f"missing cap=60hz line, saw {case.get('cap_line') or 'none'}"
    elif not measured:
        reason = f"frames={frames} interval_s={interval:.3f}"
    elif not rate_ok:
        reason = f"presentations_per_second={rate:.3f} > {MAX_PRESENT_HZ}"
    else:
        reason = ""
    token = "[headless-render-cap] PASS" if ok else "[headless-render-cap] FAIL"
    print(
        f"{token} cap={case.get('cap') or 'none'} frames={frames} "
        f"interval_s={interval:.3f} presentations_per_second={rate:.3f} "
        f"gpu_max={case.get('gpu_max')} gpu_avg={case.get('gpu_avg')}",
        flush=True,
    )
    return {
        "pass": ok,
        "exit_code": case.get("exit_code"),
        "timed_out": case.get("timed_out"),
        "pass_lines": 1 if ok else 0,
        "fail_lines": [] if ok else [reason],
        "fatal": [],
        "reason": reason,
        "cap": case.get("cap"),
        "frames": frames,
        "interval_s": interval,
        "presentations_per_second": rate,
        "gpu_max": case.get("gpu_max"),
        "gpu_avg": case.get("gpu_avg"),
    }


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    options = parser.parse_args()
    os.environ["CCCP_HEADLESS"] = "1"
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    case = run_case(options.repo.resolve(), root, options.timeout)
    scored = score_detect(case)
    (root / "result.json").write_text(
        json.dumps({"case": {k: v for k, v in case.items() if k != "log_text"}, "scored": scored}, indent=2),
        encoding="utf-8",
    )
    return 0 if scored["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
