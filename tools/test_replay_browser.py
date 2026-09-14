"""Browse two retained replay copies, play the second, and confirm its deletion.

Run unchanged on the control (the Replays button has no handler) and the tip.
Only individual fixture files are copied, into each run's private Userdata.
"""

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import shutil

from run_sim_test import make_run
from test_lobby_chat import read_log, set_resolution
from test_post_match_report import LABELS, SIZES, capture_geometry, latest_capture, pin, sha256


NAMES = ("01-first.ccreplay", "02-second.ccreplay")
FIXTURE = Path("D:/Projects/stage2_p4/fixtures/pickup_fire.ccreplay")
PLAYBACK = re.compile(r"\[net-replay\] playback finished[^\n]*ticks=(\d+)[^\n]*outcome=completed[^\n]*frames=(\d+)[^\n]*end_marker=1")


def menu_script(date):
    script = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\n"
              "assert_screen MultiplayerScreen\nassert_control ButtonMultiplayerReplays\n"
              "activate ButtonMultiplayerReplays\nwait 10\nassert_substate ReplayBrowser\n"
              "assert_control ListReplays\nassert_control LabelReplaySelected\n")
    for index, name in enumerate(NAMES):
        for expected in (name, date, "P4 Alpha Duel / Grasslands", "2 peers"):
            script += f"assert_label LabelReplayRow{index} {expected}\n"
    script += ("assert_label LabelReplaySelected 01-first.ccreplay\nscreenshot replays_list\n"
               "activate LabelReplayRow1\nwait 10\nassert_label LabelReplaySelected 02-second.ccreplay\n"
               "screenshot replays_selected\nactivate ButtonReplayDelete\nwait 10\n"
               "assert_label LabelReplayDelete Delete 02-second.ccreplay?\n"
               "assert_enabled ButtonReplayPlay 0\nscreenshot replays_cancel_confirm\n"
               "activate ButtonReplayDeleteCancel\nwait 10\nassert_enabled ButtonReplayPlay 1\n"
               "activate ButtonReplayPlay\nwait_ms 5000\n"
               "assert_screen MultiplayerScreen\nassert_substate ReplayBrowser\n"
               "assert_label LabelReplayStatus Playback finished:\n"
               "assert_label LabelReplaySelected 02-second.ccreplay\nscreenshot replays_return\n"
               "activate ButtonReplayBack\nwait 10\nassert_substate Landing\n"
               "activate ButtonMultiplayerReplays\nwait 10\nassert_substate ReplayBrowser\n"
               "assert_label LabelReplaySelected 02-second.ccreplay\n"
               "activate ButtonReplayDelete\nwait 10\nscreenshot replays_delete_confirm\n"
               "activate ButtonReplayDeleteConfirm\nwait 10\n"
               "assert_label LabelReplayStatus Deleted 02-second.ccreplay\n"
               "assert_label LabelReplaySelected 01-first.ccreplay\n"
               "assert_label LabelReplayRow0 01-first.ccreplay\nscreenshot replays_deleted\n"
               "activate ButtonReplayBack\nwait 10\nassert_substate Landing\nscreenshot replays_back\nexit\n")
    return script


def selection_pixels(before, after, geometry):
    from PIL import Image
    with Image.open(before) as source:
        first = source.convert("RGB")
    with Image.open(after) as source:
        second = source.convert("RGB")
    extent = geometry["panel"]
    if not extent or geometry["top"] is None:
        return 0
    top = geometry["top"]
    region = (extent[1] + 12, top + 50, extent[2] - 12, top + 132)
    return sum(a != b for a, b in zip(first.crop(region).getdata(), second.crop(region).getdata()))


def run_size(repo, root, size, fixture, expected):
    root.mkdir(parents=True, exist_ok=False)
    checks, details = {}, {}
    before = pin(repo, expected)
    fixture_hash = sha256(fixture)
    date = datetime.fromtimestamp(fixture.stat().st_mtime, timezone(timedelta(hours=-7))).strftime("%Y-%m-%d %H:%M MST")
    script = root / "browser.txt"
    script.write_text(menu_script(date), encoding="utf-8")
    run = make_run(repo, ["-menu-script", script, "-num-lua-states", 4], root / "browser", 240,
                   env={"CCCP_HEADLESS": "1"})
    try:
        set_resolution(run.cwd, *size)
        directory = run.cwd / "Userdata/Replays"
        directory.mkdir()
        for name in NAMES:
            shutil.copy2(fixture, directory / name)
        details["seed"] = {"source": str(fixture), "sha256": fixture_hash, "date": date,
                           "copies": [str(directory / name) for name in NAMES]}
        record = run.start().finish()
        details["record"] = record
        log = read_log(run.out)
        labels = LABELS.findall(log)
        failures = re.findall(r"^.*(?:FAILED|FAIL|EXCEPTION_|RTE Assert|RTE Abort|Runtime Error).*$", log, re.M)
        details["failures"] = failures
        details["labels"] = labels
        checks["process"] = record["exit_code"] == 0 and not record["timed_out"]
        checks["desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
        checks["exe"] = record["exe_sha256"] == expected
        checks["assertions"] = not failures and len(labels) >= 17 and all(status == "PASS" for _, _, status in labels)
        checks["second_played"] = any("[net-replay] playing back " in line and NAMES[1] in line for line in log.splitlines())
        playback = PLAYBACK.findall(log)
        details["playback"] = playback
        checks["playback_completed"] = len(playback) == 1 and all(int(ticks) >= 60 and int(frames) >= 60 for ticks, frames in playback)
        checks["returned_to_browser"] = "assert_substate expected=ReplayBrowser actual=ReplayBrowser PASS" in log and "Playback finished:" in log
        checks["deleted_only_second"] = not (directory / NAMES[1]).exists() and sha256(directory / NAMES[0]) == fixture_hash
        checks["source_unchanged"] = sha256(fixture) == fixture_hash
        checks["no_network_failure"] = "[net-match] controller sync failed" not in log
        for stem in ("replays_list", "replays_selected", "replays_cancel_confirm", "replays_return", "replays_delete_confirm", "replays_deleted", "replays_back"):
            capture = capture_geometry(latest_capture(run, stem), size)
            details[stem] = capture
            checks[stem] = capture["dimensions"] == list(size) and capture["inside_viewport"]
        changed = selection_pixels(latest_capture(run, "replays_list"), latest_capture(run, "replays_selected"), details["replays_list"])
        details["selection_changed_pixels"] = changed
        checks["selection_visible"] = changed >= 16
        checks["pin_unchanged"] = pin(repo, expected) == before
    except Exception as error:
        details["error"] = repr(error)
        checks["completed"] = False
    finally:
        run.close()
    result = {"pass": bool(checks) and all(checks.values()), "checks": checks, "details": details, "pin": before}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--fixture", type=Path, default=FIXTURE)
    args = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("family lock exists; no driver may run")
    os.environ["CCCP_HEADLESS"] = "1"
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = {f"{w}x{h}": run_size(args.repo.resolve(), root / f"{w}x{h}", (w, h), args.fixture, args.exe_sha256.lower())
               for w, h in SIZES}
    result = {"pass": all(row["pass"] for row in results.values()), "runs": results,
              "driver_sha256": sha256(__file__), "fixture_sha256": sha256(args.fixture)}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "out": str(root), "exe_sha256": args.exe_sha256}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
