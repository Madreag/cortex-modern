"""Whether VSync holds a visible window's presents to its display's refresh.

One headed engine plays a short single-player baseline with VSync on and no
presentation cap, then the recorded presents per second are held against the
refresh the engine read from its display. A platform that drops the swap
interval leaves the engine's own pacing to hold them; `--drop-swap-interval`
makes this platform drop it as well. Run it where a display shows the window:
a desktop session, never a headless or hidden one.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run, seed_settings  # noqa: E402
from test_headless_render_cap import frame_rows, measure, read_text  # noqa: E402

RENDER_LINE = re.compile(r"\[render\] video=(\S+) vsync=(on|off) swap_interval set=(yes|no) reads=(-?\d+)")
TARGET = re.compile(r"\[frame-pace\] .*?target_hz=([0-9.]+)")
PACING_LINE = "[render] the swap does not wait for the display under VSync"
TICKS = 600
# A refresh shows one frame; a few percent covers the clocks of the measure.
MARGIN = 1.05


def run_case(repo: Path, out: Path, timeout: float, drop_swap_interval: bool) -> dict:
    out.mkdir(parents=True, exist_ok=False)
    feel = out / "feel"
    feel.mkdir()
    # VSync is a visible window's: the runner draws headless unless asked for one.
    env = {"CC_RUNNER_HEADED": "1", "CC_TEST_FEEL_HEADED": "1", "CC_TEST_FRAME_PACE": "1", **({"CC_TEST_SWAP_INTERVAL_DROPPED": "1"} if drop_swap_interval else {})}
    args = ["-scenario", "SimBaseline", "-seed", "42", "-max-ticks", str(TICKS), "-feel-measure", str(feel)]
    run = make_run(repo, args, out / "run", timeout, env=env)
    seed_settings(run, {"EnableVSync": 1, "Fullscreen": 0})
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = read_text(out / "run" / "stdout.log") + "\n" + read_text(Path(run.cwd) / "LogConsole.txt")
    render = RENDER_LINE.search(log)
    targets = [float(value) for value in TARGET.findall(log)]
    refresh = max(targets) if targets else 0.0
    stats = measure(frame_rows(feel / "raw.jsonl"))
    rate = stats["presentations_per_second"]
    held = refresh > 0 and stats["frames"] >= 60 and rate <= refresh * MARGIN
    reasons = []
    if record.get("exit_code") != 0 or record.get("timed_out"):
        reasons.append(f"engine exit={record.get('exit_code')} timed_out={record.get('timed_out')}")
    if not render or render.group(2) != "on":
        reasons.append("no '[render] ... vsync=on' line")
    if refresh <= 0:
        reasons.append("no display refresh in the frame-pace lines")
    if not held:
        reasons.append(f"presents_per_second={rate:.1f} past the display's {refresh:.0f} Hz")
    result = {"pass": not reasons, "reasons": reasons, "drop_swap_interval": drop_swap_interval,
              "video": render.group(1) if render else "", "swap_interval_set": render.group(3) if render else "",
              "swap_interval_reads": int(render.group(4)) if render else None, "refresh_hz": refresh,
              "engine_paced": PACING_LINE in log, **stats, "exit_code": record.get("exit_code"),
              "timed_out": record.get("timed_out"), "exe_sha256": record.get("exe_sha256"), "stdout": str(out / "run" / "stdout.log")}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--drop-swap-interval", action="store_true", help="make this platform drop the swap interval the window asks for")
    args = parser.parse_args()
    result = run_case(args.repo.resolve(), args.out.resolve(), args.timeout, args.drop_swap_interval)
    print(f"[vsync-holds] {'PASS' if result['pass'] else 'FAIL'} video={result['video']} swap_interval_set={result['swap_interval_set']} "
          f"reads={result['swap_interval_reads']} refresh_hz={result['refresh_hz']:.0f} presents_per_second={result['presentations_per_second']:.1f} "
          f"frames={result['frames']} engine_paced={int(result['engine_paced'])}" + ("" if result["pass"] else " - " + "; ".join(result["reasons"])), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
