"""Replay the fight's duplicate dropped device through its crashing tick on both binaries.

The retained recording has no end marker because the game crashed. This checks an
explicit input prefix through that tick; it does not claim the recording is complete.
Run through the box's guarded headless session task.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from run_sim_test import file_sha256, make_run, seed_settings


def run_case(repo, recording, out, exe, tick, uid, preset, birth_tick):
    out.mkdir(parents=True, exist_ok=False)
    observer = out / "lifetime-observer.json"
    observer.write_text(json.dumps({"schema": 1, "timeout_ms": 1800000,
                                    "step_timeout_ms": 1800000, "activate_phase": "Running",
                                    "steps": [{"op": "wait", "elapsed_ms": 1800000}]}) + "\n",
                        encoding="utf-8", newline="\n")
    run = make_run(repo, ["-net-replay", str(recording), "-max-ticks", str(tick)], out,
                   timeout=900, env={"CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1",
                                     "CC_TEST_MO_LIFETIME_TRACE": preset,
                                     "CC_TEST_NET_UI_SCRIPT": str(observer)})
    if exe:
        run.argv[0] = str(exe.resolve())
    seed_settings(run, {"ResolutionX": 960, "ResolutionY": 540, "ResolutionMultiplier": 1})
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = (out / "stdout.log").read_text(encoding="utf-8", errors="replace")
    events = []
    for line_number, line in enumerate(log.splitlines(), 1):
        event = re.match(r"\[mo-lifetime\] (\S+) tick=(\d+) pointer=(\S+) uid=(\d+) preset=", line)
        if event and int(event[4]) == uid:
            events.append({"stage": event[1], "tick": int(event[2]), "pointer": event[3],
                           "line": line_number, "text": line})
    additions = [event for event in events if event["stage"] in ("add-item", "add-device-particle")
                 and event["tick"] == birth_tick]
    pointers = {event["pointer"] for event in additions}
    fatal = [(n, line) for n, line in enumerate(log.splitlines(), 1)
             if "FATAL: unhandled exception 0xC0000005" in line]
    destroyed_at_crash = any(event["stage"] == "destroy" and event["tick"] == tick
                             and event["pointer"] in pointers for event in events)
    completed = re.search(r"\[net-replay\] playback finished .*ticks=(\d+) .*outcome=TickCap "
                          r"frames=(\d+) last_tick=(\d+) .*exit=0", log)
    red = (record.get("exit_code") == 3 and not record.get("timed_out")
           and len(additions) == 2 and len(pointers) == 1 and destroyed_at_crash and bool(fatal))
    green = (record.get("exit_code") == 0 and not record.get("timed_out")
             and record.get("evidence_complete") and len(additions) == 1 and not fatal
             and completed is not None and int(completed[1]) == tick and int(completed[3]) == tick)
    return {"record": record, "events": events, "fatal": fatal, "red": bool(red),
            "green": bool(green), "stdout": str(out / "stdout.log"),
            "executable_sha256": file_sha256(run.argv[0])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--base-exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tick", type=int, default=38825)
    parser.add_argument("--birth-tick", type=int, default=32499)
    parser.add_argument("--uid", type=int, default=1560335)
    parser.add_argument("--preset", default="Micro Pulsar")
    options = parser.parse_args()
    options.out.mkdir(parents=True, exist_ok=False)
    result = {"recording": str(options.recording.resolve()),
              "recording_sha256": file_sha256(options.recording),
              "prefix_last_tick": options.tick, "complete_recording_proof": False,
              "topology": "single-engine replay", "multi_machine_proof": False}
    for name, exe in (("base", options.base_exe), ("tip", None)):
        case = run_case(options.repo, options.recording.resolve(), options.out / name,
                        exe, options.tick, options.uid, options.preset, options.birth_tick)
        result[name] = case
        passed = case["red"] if name == "base" else case["green"]
        print(f"[match-device-lifetime] {'RED' if name == 'base' and passed else 'GREEN' if passed else 'FAIL'} "
              f"{name} tick={options.tick} exit={case['record'].get('exit_code')} "
              f"stdout={case['stdout']}", flush=True)
    result["pass"] = result["base"]["red"] and result["tip"]["green"]
    (options.out / "result.json").write_text(json.dumps(result, indent=2) + "\n",
                                            encoding="utf-8", newline="\n")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
