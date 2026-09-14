"""Detect the match status widget, toast lifetime, captures and simulation invariance.

Two menu-launched peers run at each viewport. Quiet capture-on/off pairs compare
complete traces; a separate pair presses the real synced pause key and resumes.
The unchanged network UI probe reads the drawn labels and their rectangles.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading

SCRATCH = Path("D:/mx/astra-match-overlay-20260913")
TICKS = 900
CAPTURE_TICKS = (60, 270, 880)
BOX_WIDTH, BOX_HEIGHT, BOX_TOP, BOX_MARGIN = 252, 76, 32, 8
STATUS = "LabelNetMatchStatus"
TOAST = "LabelNetMatchToastNewest"
PAUSED = "Match paused by Host"
RESUMED = "Match resumed by Host"
CAPTURE = re.compile(r"^\[net-match-screenshot\] applied_tick=(\d+) name=(\S+) queued=([01])$")
PAUSE_EVENT = re.compile(r"^\[net-match\] match paused at tick (\d+) sim ms \d+$")
RESUME_EVENT = re.compile(r"^\[net-match\] match resumed at tick (\d+) sim ms \d+$")


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True)
    return result.stdout


def pin(repo):
    return {"head": git(repo, "rev-parse", "HEAD").decode().strip(),
            "diff_sha256": hashlib.sha256(git(repo, "diff", "HEAD", "--")).hexdigest(),
            "exe_sha256": sha256(repo / "Cortex Command.exe")}


def require_pin(repo, expected):
    actual = pin(repo)
    if actual != expected:
        raise RuntimeError(f"source or executable changed: {actual}")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def read_log(out):
    return "\n".join(path.read_text(encoding="utf-8", errors="replace")
                     for path in (out / "stdout.log", out / "stderr.log") if path.exists())


def set_resolution(runtime, width, height):
    path = runtime / "Userdata" / "Settings.ini"
    settings = path.read_text(encoding="utf-8")
    for name, value in (("ResolutionX", width), ("ResolutionY", height)):
        settings, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*",
                                  lambda match: match[1] + str(value), settings)
        if count != 1:
            raise RuntimeError(f"expected one private {name} setting, found {count}")
    path.write_text(settings, encoding="utf-8")


def menu_script(who, port):
    text = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {who}\n"
    if who == "Host":
        text += ("activate ButtonMultiplayerHostGame\nwait 10\n"
                 f"settext TextHostPort {port}\nsettext TextHostPlayers 2\nsettext TextHostInputDelay 3\n"
                 "activate ButtonMultiplayerCreate\nwait_connected 2\nwait_remote_ready\n"
                 "wait_all_ready\nactivate ButtonMultiplayerStart\n")
    else:
        text += ("activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                 f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\nwait_connected 2\n"
                 "activate ButtonMultiplayerReady\nwait_remote_ready\n")
    return text + "wait 99999\n"


def label_assert(control, text=None, visible=True):
    step = {"op": "assert_control", "control": control, "equals": {"visible": visible}, "fits": True}
    if text is not None:
        step["text_contains"] = text
    return step


def probe_script(who, width, event):
    box = [width - BOX_WIDTH - BOX_MARGIN, BOX_TOP, BOX_WIDTH, BOX_HEIGHT]
    steps = [
        {"op": "wait", "service": "Running"},
        {"op": "wait", "sim_at_least": 60},
        {"op": "assert_control", "control": "BoxNetMatchStatus", "equals": {"visible": True, "rect": box}, "fits": True},
        label_assert(STATUS, "NET STATUS"),
        label_assert(STATUS, "D 3 ticks / 50.0 ms"),
        label_assert(STATUS, "max peer" if who == "Host" else "host link"),
        label_assert(STATUS, "PACE "),
    ]
    if event:
        steps += [{"op": "wait", "sim_at_least": 240}]
        if who == "Host":
            steps += [{"op": "key_down", "key": "P"}, {"op": "key_up", "key": "P"}]
        steps += [
            {"op": "wait", "sim_at_least": 270},
            label_assert(TOAST, PAUSED),
            label_assert(STATUS, "PAUSED"),
            {"op": "wait", "sim_at_least": 430},
            {"op": "assert_control", "control": TOAST, "equals": {"visible": False, "text": ""}},
            {"op": "wait", "sim_at_least": 480},
        ]
        if who == "Host":
            steps += [{"op": "key_down", "key": "P"}, {"op": "key_up", "key": "P"}]
        steps += [{"op": "wait", "sim_at_least": 680}, label_assert(TOAST, RESUMED)]
    steps += [{"op": "wait", "sim_at_least": 880}, label_assert(STATUS, "LIVE"),
              {"op": "assert_control", "control": TOAST, "equals": {"visible": False, "text": ""}},
              {"op": "finish"}]
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def run_pair(repo, root, port, size, captures, event, timeout, expected_pin):
    require_pin(repo, expected_pin)
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    try:
        for who in ("Host", "Guest"):
            inputs = root / f"{who}_inputs"
            inputs.mkdir()
            menu = inputs / "menu.txt"
            menu.write_text(menu_script(who, port), encoding="utf-8")
            probe = inputs / "probe.json"
            probe.write_text(json.dumps(probe_script(who, size[0], event), indent=2), encoding="utf-8")
            flags = ["-menu-script", menu, "-num-lua-states", 4, "-tick-hashes", "-max-ticks", TICKS,
                     "-net-match-ticks", TICKS, "-out", root / f"{who}_trace.json",
                     "-net-match-report", root / f"{who}_report.json"]
            if captures:
                flags += ["-net-match-screenshot-ticks", ",".join(map(str, CAPTURE_TICKS))]
            runs[who] = make_run(repo, flags, root / who, timeout,
                                 env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)})
            set_resolution(runs[who].cwd, *size)

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        host = threading.Thread(target=drive, args=("Host",), daemon=True)
        guest = threading.Thread(target=drive, args=("Guest",), daemon=True)
        host.start()
        threading.Event().wait(2.0)
        guest.start()
        host.join()
        guest.join()
    finally:
        for run in runs.values():
            run.close()
    require_pin(repo, expected_pin)
    return records


def probe_observations(probe):
    steps = probe.get("script", {}).get("steps", [])
    for result in probe.get("steps", []):
        index = result.get("index", -1)
        if 0 <= index < len(steps):
            yield steps[index], result["observed"]


def image_oracle(path, size, event_capture):
    from PIL import Image

    with Image.open(path) as source:
        image = source.convert("RGB")
    if image.size != tuple(size):
        return {"pass": False, "actual_size": list(image.size), "expected_size": list(size)}
    width, height = size
    x, y = width - BOX_WIDTH - BOX_MARGIN, BOX_TOP
    pixels = image.load()
    border = (59, 65, 83)
    edge = [(px, y) for px in range(x, x + BOX_WIDTH)]
    edge += [(px, y + BOX_HEIGHT - 1) for px in range(x, x + BOX_WIDTH)]
    edge += [(x, py) for py in range(y + 1, y + BOX_HEIGHT - 1)]
    edge += [(x + BOX_WIDTH - 1, py) for py in range(y + 1, y + BOX_HEIGHT - 1)]
    matched = sum(pixels[point] == border for point in edge)
    fill = (20, 22, 27)
    ink = sum(pixels[px, py] != fill for py in range(y + 6, y + BOX_HEIGHT - 6)
              for px in range(x + 6, x + BOX_WIDTH - 6))
    result = {"path": str(path), "sha256": sha256(path), "size": list(image.size),
              "box": [x, y, BOX_WIDTH, BOX_HEIGHT], "border_matches": matched,
              "border_pixels": len(edge), "interior_ink": ink,
              "pass": matched == len(edge) and ink > 0}
    if event_capture:
        toast_width = min(520, width - 32)
        left, top = (width - toast_width) // 2, height - 28
        background = all(pixels[px, top] == fill for px in range(left, left + toast_width))
        toast_ink = sum(pixels[px, py] != fill for py in range(top + 4, top + 16)
                       for px in range(left + 8, left + toast_width - 8))
        result.update(toast_background=background, toast_ink=toast_ink)
        result["pass"] = result["pass"] and background and toast_ink > 0
    return result


def inspect_pair(root, records, size, captures, event):
    checks, details = {}, {"records": records, "captures": {}, "probes": {}, "events": {}}
    for who in ("Host", "Guest"):
        record = records.get(who, {})
        checks[f"{who}_exit"] = record.get("exit_code") == 0 and not record.get("timed_out", True)
        checks[f"{who}_evidence"] = record.get("evidence_complete") is True
        log = read_log(root / who)
        checks[f"{who}_menu_match"] = "[menu-mp] launching the match" in log
        checks[f"{who}_no_menu_failure"] = "[menu-script] FAILED" not in log
        probe_path = root / f"{who}_inputs" / "net-ui-result.json"
        probe = read_json(probe_path) if probe_path.exists() else {}
        details["probes"][who] = {"path": str(probe_path), "error": probe.get("error"),
                                  "failed_step": probe.get("failed_step")}
        checks[f"{who}_probe_complete"] = probe.get("pass") is True and probe.get("complete") is True
        observations = list(probe_observations(probe))
        status_texts = [obs["control"]["text"] for step, obs in observations
                        if step.get("control") == STATUS and "control" in obs]
        checks[f"{who}_live_rtt"] = bool(status_texts) and all(re.search(r"\nRTT \d+ ms / ", text) for text in status_texts)
        checks[f"{who}_measured_pace"] = bool(status_texts) and all(re.search(r"\nPACE (?:[1-9]\d*(?:\.\d+)?|0\.[1-9]\d*) tps", text) for text in status_texts)
        capture_lines = [(int(match[1]), match[2], match[3] == "1") for line in log.splitlines()
                         if (match := CAPTURE.match(line))]
        shots = sorted((root / who / "runtime" / "ScreenShots").glob("net_match_tick_*.png"))
        details["captures"][who] = {"queued": capture_lines, "images": []}
        expected_ticks = list(CAPTURE_TICKS) if captures else []
        checks[f"{who}_capture_ticks"] = [tick for tick, _, ok in capture_lines if ok] == expected_ticks
        checks[f"{who}_capture_count"] = len(shots) == len(expected_ticks)
        for tick in expected_ticks:
            matching = [path for path in shots if path.name.startswith(f"net_match_tick_{tick}_round_")]
            image_result = image_oracle(matching[0], size, event and tick == 270) if len(matching) == 1 else {"pass": False, "reason": "missing or duplicate capture", "tick": tick}
            details["captures"][who]["images"].append(image_result)
            checks[f"{who}_pixels_{tick}"] = image_result["pass"]
        if event:
            pauses = [int(match[1]) for line in log.splitlines() if (match := PAUSE_EVENT.match(line))]
            resumes = [int(match[1]) for line in log.splitlines() if (match := RESUME_EVENT.match(line))]
            toast_reads = [obs for step, obs in observations if step.get("control") == TOAST and "control" in obs]
            paused_reads = [obs for obs in toast_reads if obs["control"].get("text") == PAUSED and obs["control"].get("visible")]
            resumed_reads = [obs for obs in toast_reads if obs["control"].get("text") == RESUMED and obs["control"].get("visible")]
            expired_reads = [obs for obs in toast_reads if obs["control"].get("text") == "" and not obs["control"].get("visible")]
            checks[f"{who}_pause_toast_within_30"] = len(pauses) == 1 and bool(paused_reads) and 0 <= paused_reads[0]["sim_frame"] - pauses[0] <= 30
            checks[f"{who}_pause_toast_gone_within_200"] = len(pauses) == 1 and bool(expired_reads) and 0 < expired_reads[0]["sim_frame"] - pauses[0] <= 200
            checks[f"{who}_resume_toast_within_30"] = len(resumes) == 1 and bool(resumed_reads) and 0 <= resumed_reads[0]["sim_frame"] - resumes[0] <= 30
            details["events"][who] = {"pause_ticks": pauses, "resume_ticks": resumes,
                                      "toast_reads": toast_reads,
                                      "lines": [{"line": index, "text": line} for index, line in enumerate(log.splitlines(), 1)
                                                if PAUSE_EVENT.match(line) or RESUME_EVENT.match(line)]}
    ok, compared = strict_compare(root / "Host_trace.json", root / "Guest_trace.json", expected_ticks=TICKS)
    checks["complete_peer_hashes"] = ok
    details["peer_hashes"] = compared
    return {"pass": all(checks.values()), "checks": checks, "details": details}


def compare_capture_switch(on, off, who):
    first, second = on / f"{who}_trace.json", off / f"{who}_trace.json"
    ok, detail = strict_compare(first, second, expected_ticks=TICKS)
    if ok:
        a, b = read_json(first), read_json(second)
        detail["all_recorded_hashes_identical"] = a["runs"][0]["tick_hashes"] == b["runs"][0]["tick_hashes"]
        ok = detail["all_recorded_hashes_identical"]
    return {"pass": ok, "detail": detail}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--timeout", type=int, default=600)
    options = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("verification family owns the machine; no driver may start")
    repo, root = options.repo.resolve(), options.out.resolve()
    if not root.is_relative_to(SCRATCH.resolve()) or root == SCRATCH.resolve():
        parser.error(f"--out must name a fresh run beneath {SCRATCH}")
    global make_run, strict_compare
    sys.path.insert(0, str(repo / "tools"))
    from compare_sim_traces import strict_compare
    from run_sim_test import make_run
    os.environ["CCCP_HEADLESS"] = "1"
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "checks": {}, "pairs": {}, "capture_switch": {},
              "ticks_per_run": TICKS, "ports": list(range(48201, 48207)),
              "driver_sha256": sha256(__file__),
              "peer_comparator_sha256": sha256(repo / "tools" / "compare_sim_traces.py"),
              "peer_hash_scope": "unchanged strict_compare: controller excluded; every other subsystem at every tick",
              "capture_switch_hash_scope": "every recorded hash, including controller and total"}
    try:
        result["pin_before"] = pin(repo)
        if result["pin_before"]["exe_sha256"].lower() != options.exe_sha256.lower():
            raise RuntimeError("the requested executable hash does not match")
        for index, size in enumerate(((640, 360), (960, 540))):
            tag = f"{size[0]}x{size[1]}"
            for arm, captures, event in (("on", True, False), ("off", False, False), ("pause", True, True)):
                name = f"{tag}_{arm}"
                port = 48201 + index * 3 + (0 if arm == "on" else 1 if arm == "off" else 2)
                pair_root = root / name
                records = run_pair(repo, pair_root, port, size, captures, event, options.timeout, result["pin_before"])
                pair = inspect_pair(pair_root, records, size, captures, event)
                result["pairs"][name] = pair
                result["checks"][name] = pair["pass"]
                (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
                if not pair["pass"]:
                    raise RuntimeError(f"{name}: " + ", ".join(key for key, passed in pair["checks"].items() if not passed))
            for who in ("Host", "Guest"):
                comparison = compare_capture_switch(root / f"{tag}_on", root / f"{tag}_off", who)
                result["capture_switch"][f"{tag}_{who}"] = comparison
                result["checks"][f"{tag}_{who}_capture_switch"] = comparison["pass"]
        result["pass"] = all(result["checks"].values()) and len(result["checks"]) == 10
    except Exception as error:
        result["error"] = str(error)
    finally:
        result["pin_after"] = pin(repo)
        if result.get("pin_before") != result["pin_after"]:
            result["pass"] = False
            result["error"] = "source or executable changed during the driver"
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"), "out": str(root),
                      "exe_sha256": result["pin_after"]["exe_sha256"]}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
