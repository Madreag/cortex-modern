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


def duration_agreement(row_text, summary):
    ticks = summary.get("running_ticks") if summary else None
    seconds = ticks // 60 if isinstance(ticks, int) else None
    expected = f"{seconds // 60:02d}:{seconds % 60:02d}" if seconds is not None else None
    actual = row_text.rsplit(" | ", 1)[-1]
    return {"pass": bool(expected) and actual == expected == summary.get("duration"),
            "row": actual, "summary": expected, "running_ticks": ticks}


def menu_script(date, duration=None):
    script = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\n"
              "assert_screen MultiplayerScreen\nassert_control ButtonMultiplayerReplays\n"
              "activate ButtonMultiplayerReplays\nwait 10\nassert_substate ReplayBrowser\n"
              "assert_control ListReplays\nassert_control LabelReplaySelected\n")
    for index, name in enumerate(NAMES):
        for expected in (name, date, "P4 Alpha Duel / Grasslands", "2 peers", *(("| " + duration,) if duration else ())):
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
               "activate ButtonReplayPlay\nwait_ms 5000\n"
               "assert_screen MultiplayerScreen\nassert_substate ReplayBrowser\n"
               "assert_label LabelReplayStatus Playback finished:\n"
               "assert_label LabelReplaySelected 02-second.ccreplay\nscreenshot replays_repeat_return\n"
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


def first_row_ink(repo, path, text, geometry):
    """Require every atlas ink pixel of row zero, including under the modal dim."""
    from PIL import Image
    with Image.open(repo / "Data/Base.rte/GUIs/Skins/Menus/FontLarge.png") as source:
        atlas = source.convert("RGB")
    with Image.open(path) as source:
        capture = source.convert("RGB")
    separator, background = atlas.getpixel((0, 0)), atlas.getpixel((atlas.width - 1, 0))
    cell_height = next(y for y in range(1, atlas.height) if atlas.getpixel((0, y)) == separator)
    glyphs = {}
    x, y = 1, 0
    for code in range(32, 127):
        stop = next(i for i in range(x, atlas.width) if atlas.getpixel((i, y)) == separator)
        glyphs[chr(code)] = (x, y, stop - x - 1)
        x = stop + 1
        if (code - 31) % 16 == 0:
            x, y = 1, y + cell_height
    # Locate the list's top frame in the unobscured browser geometry.
    left, right = geometry["panel"][1:3]
    frame_color = (108, 118, 168)
    def dim(color):
        # TrueAlphaBlender uses packed unsigned 32-bit arithmetic on the RGBA buffer.
        packed = color[0] | (color[1] << 8) | (color[2] << 16)
        rb = (((-(packed & 0xFF00FF) * 129) & 0xFFFFFFFF) // 256 + packed) & 0xFF00FF
        green = (((-(packed & 0xFF00) * 129) & 0xFFFFFFFF) // 256 + (packed & 0xFF00)) & 0xFF00
        return tuple(((rb | green) >> shift) & 255 for shift in (0, 8, 16))
    frame_rows = [y for y in range(geometry["top"] + 40, geometry["top"] + 60)
                  if sum(capture.getpixel((x, y)) in (frame_color, dim(frame_color))
                         for x in range(left + 10, right - 9)) >= right - left - 24]
    if not frame_rows:
        return {"pass": False, "reason": "list top frame missing"}
    top = min(frame_rows)
    frame = (left + 10, top - 1, right - 9, geometry["bottom"] - 93)
    cursor, origin_y = frame[0] + 4, top
    expected = []
    for char in text:
        gx, gy, width = glyphs[char]
        for dy in range(cell_height):
            for dx in range(width):
                color = atlas.getpixel((gx + dx, gy + dy))
                if color not in (separator, background):
                    expected.append((cursor + dx, origin_y + dy, color))
        cursor += width
    outside = sum(not (frame[0] + 2 <= x < frame[2] - 2 and frame[1] + 2 <= y < frame[3] - 2)
                  for x, y, _ in expected)
    missing = sum(capture.getpixel((x, y)) not in (color, dim(color)) for x, y, color in expected)
    return {"pass": bool(expected) and outside == 0 and missing == 0, "frame": frame,
            "ink_pixels": len(expected), "outside": outside, "missing": missing,
            "ink_bounds": [min(x for x, _, _ in expected), min(y for _, y, _ in expected),
                           max(x for x, _, _ in expected), max(y for _, y, _ in expected)]}


def run_size(repo, root, size, fixture, expected, summary=None):
    root.mkdir(parents=True, exist_ok=False)
    checks, details = {}, {}
    before = pin(repo, expected)
    fixture_hash = sha256(fixture)
    date = datetime.fromtimestamp(fixture.stat().st_mtime, timezone(timedelta(hours=-7))).strftime("%Y-%m-%d %H:%M MST")
    script = root / "browser.txt"
    expected_duration = duration_agreement("", summary)["summary"] if summary else None
    script.write_text(menu_script(date, expected_duration), encoding="utf-8")
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
        if summary is not None:
            details["duration_agreement"] = [duration_agreement(text, summary) for name, text, _ in labels
                                             if name in ("LabelReplayRow0", "LabelReplayRow1")]
            checks["duration_matches_summary"] = bool(details["duration_agreement"]) and all(row["pass"] for row in details["duration_agreement"])
        checks["process"] = record["exit_code"] == 0 and not record["timed_out"]
        checks["desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
        checks["exe"] = record["exe_sha256"] == expected
        checks["assertions"] = not failures and len(labels) >= 17 and all(status == "PASS" for _, _, status in labels)
        checks["second_played"] = any("[net-replay] playing back " in line and NAMES[1] in line for line in log.splitlines())
        playback = PLAYBACK.findall(log)
        details["playback"] = playback
        checks["playback_completed"] = len(playback) == 2 and all(int(ticks) >= 60 and int(frames) >= 60 for ticks, frames in playback)
        checks["returned_to_browser"] = "assert_substate expected=ReplayBrowser actual=ReplayBrowser PASS" in log and "Playback finished:" in log
        checks["deleted_only_second"] = not (directory / NAMES[1]).exists() and sha256(directory / NAMES[0]) == fixture_hash
        checks["source_unchanged"] = sha256(fixture) == fixture_hash
        checks["no_network_failure"] = "[net-match] controller sync failed" not in log
        for stem in ("replays_list", "replays_selected", "replays_cancel_confirm", "replays_return", "replays_repeat_return", "replays_delete_confirm", "replays_deleted", "replays_back"):
            capture = capture_geometry(latest_capture(run, stem), size)
            details[stem] = capture
            checks[stem] = capture["dimensions"] == list(size) and capture["inside_viewport"]
        changed = selection_pixels(latest_capture(run, "replays_list"), latest_capture(run, "replays_selected"), details["replays_list"])
        details["selection_changed_pixels"] = changed
        checks["selection_visible"] = changed >= 16
        row_text = next(text for name, text, status in labels if name == "LabelReplayRow0" and status == "PASS")
        details["first_row_ink"] = {
            stem: first_row_ink(repo, Path(capture["path"]), row_text, details["replays_list"])
            for stem, capture in details.items() if stem.startswith("replays_") and stem != "replays_back"}
        checks["first_row_ink_inside_frame"] = all(row["pass"] for row in details["first_row_ink"].values())
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
    parser.add_argument("--summary", type=Path, help="report JSON from the same match as --fixture; checks row duration against its running ticks")
    args = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("family lock exists; no driver may run")
    os.environ["CCCP_HEADLESS"] = "1"
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = json.loads(args.summary.read_text(encoding="utf-8")) if args.summary else None
    summary = report.get("last_match", report.get("service", {}).get("last_match")) if report else None
    if args.summary and not summary:
        parser.error("--summary must contain last_match from the recorded match")
    results = {f"{w}x{h}": run_size(args.repo.resolve(), root / f"{w}x{h}", (w, h), args.fixture, args.exe_sha256.lower(), summary)
               for w, h in SIZES}
    result = {"pass": all(row["pass"] for row in results.values()), "runs": results,
              "driver_sha256": sha256(__file__), "fixture_sha256": sha256(args.fixture)}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "out": str(root), "exe_sha256": args.exe_sha256}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
