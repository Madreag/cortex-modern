"""Check a capped menu match's retained report at 640x360 and 960x540.

Requires the menu-match tick cap and rematch routing from the post-match lobby
branch. Run this unchanged on the control executable, then on the tip.
The control executable leaves the Last match label empty with the tip skin.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from run_sim_test import make_run
from test_lobby_chat import read_log, set_resolution
from test_lobby_lifecycle import wait_for_log
from test_viewport_fit import panel_extent, panel_vertical, png_size


SIZES = ((640, 360), (960, 540))
TICKS = 120
LABELS = re.compile(r'^\[menu-script\] assert_label (\S+) "[^"\n]*" text="(.*?)" (PASS|FAIL)$', re.M | re.S)


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pin(repo, expected):
    actual = sha256(repo / "Cortex Command.exe")
    if actual.lower() != expected.lower():
        raise RuntimeError(f"executable SHA256 differs: expected {expected}, actual {actual}")
    return {"exe_sha256": actual,
            "source_tip": subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()}


def latest_capture(run, stem):
    paths = sorted((run.out / "runtime/ScreenShots").glob(stem + "_*.png"), key=lambda path: path.stat().st_mtime_ns)
    if not paths:
        raise RuntimeError(f"missing capture {stem} in {run.out}")
    return paths[-1]


def capture_geometry(path, size):
    from PIL import Image
    with Image.open(path) as source:
        image = source.convert("RGB")
    width, height = image.size
    pixels = image.load()
    extent = panel_extent(pixels, width, height)
    top, bottom = panel_vertical(pixels, width, height)
    inside = bool(extent and top is not None and bottom is not None and
                  extent[1] >= 3 and extent[2] <= width - 4 and 0 <= top < bottom < height)
    return {"path": str(path), "sha256": sha256(path), "dimensions": png_size(path), "expected_dimensions": list(size),
            "inside_viewport": inside, "panel": extent, "top": top, "bottom": bottom}


def captured_chat_rows(path):
    """Count the populated chat rows in the panel's bottom text band from pixels."""
    from PIL import Image
    with Image.open(path) as source:
        image = source.convert("RGB")
    width, height = image.size
    pixels = image.load()
    extent = panel_extent(pixels, width, height)
    _, bottom = panel_vertical(pixels, width, height)
    if not extent or bottom is None:
        return {"count": 0, "reason": "panel not found"}
    # The field below chat is empty. The first 88 pixels of each line carry
    # "Host: chatrow", away from the centered action buttons.
    left = extent[1]
    ink_rows = [y for y in range(max(0, bottom - 104), bottom - 24)
                if sum(pixels[x, y] == (255, 255, 255) for x in range(left + 10, min(width, left + 98))) >= 8]
    bands = []
    for y in ink_rows:
        if not bands or y > bands[-1][-1] + 1:
            bands.append([y])
        else:
            bands[-1].append(y)
    glyphs = [(rows[0], rows[-1]) for rows in bands if len(rows) >= 4]
    return {"count": len(glyphs), "glyph_bands": glyphs,
            "band": [left + 10, bottom - 104, left + 98, bottom - 24]}


def end_reason_agreement(labels, summary):
    """Compare every observed round's status and Details reason with the final report."""
    suffix = " - ready up for a rematch"
    statuses = sorted({text.removesuffix(suffix) for name, text, _ in labels
                       if name in ("LabelMultiplayerStatus", "LabelLobbyStatus") and suffix in text})
    reasons = sorted({match.group(1) for name, text, _ in labels if name == "LabelLastMatchDetails"
                      for match in [re.search(r"^Result: (.*)$", text, re.M)] if match})
    reported = summary.get("result") if summary else None
    return {"pass": bool(reported) and statuses == reasons == [reported],
            "status": statuses, "details": reasons, "json": reported}


def menu_script(who, port):
    script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {who}\n"
    if who == "Host":
        script += (f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\n"
                   "settext TextHostPlayers 2\nsettext TextHostInputDelay 3\nsetcheck CheckHostPortMap 0\n"
                   "activate ButtonMultiplayerCreate\nwait_connected 2\nwait_remote_ready\nwait_all_ready\n")
        for row in range(1, 9):
            script += f"chat all chatrow{row}\nwait_ms 600\n"
        script += "assert_label LabelLobbyChatNewest chatrow8\nscreenshot report_before\nactivate ButtonMultiplayerStart\n"
    else:
        script += ("activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                   f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n"
                   "wait_connected 2\nactivate ButtonMultiplayerReady\n")
    script += ("wait_state Running 120\nwait_state Starting 240\nwait 20\n"
               "assert_screen MultiplayerScreen\nassert_substate Lobby\n"
               "assert_control LabelLastMatchSummary\nassert_control ButtonLastMatchDetails\n"
               "assert_label LabelMultiplayerStatus ready up for a rematch\nscreenshot report_lobby\n"
               "assert_label LabelLastMatchSummary Last match: draw\n"
               "assert_label LabelLastMatchSummary 00:02\n"
               "assert_label LabelLastMatchSummary Host, Guest\n"
               "assert_label LabelLobbyChatNewest chatrow8\nscreenshot report_summary\n"
               "activate ButtonLastMatchDetails\nwait 10\n"
               "assert_label LabelLastMatchDetails Winner: draw\n"
               "assert_label LabelLastMatchDetails Duration: 00:02 (120 ticks at 60 tps)\n"
               "assert_label LabelLastMatchDetails Host | team 1 | seat 0 | delay \n"
               "assert_label LabelLastMatchDetails Guest | team 2 | seat 1 | delay \n"
               "assert_label LabelLastMatchDetails Resyncs: 0 | Drops: 0 | Reclaims: 0 | Substitutions: 0\n"
               "assert_label LabelLastMatchDetails Final pace:\nassert_label LabelLastMatchDetails Exe:\n"
               "assert_label LabelLastMatchDetails SHA256:\nassert_label LabelLastMatchDetails Codec: Controller\n"
               "assert_enabled ButtonMultiplayerStart 0\nscreenshot report_details\n"
               "activate ButtonLastMatchClose\nwait 10\n"
               "assert_label LabelLastMatchSummary Host, Guest\ndump_lobby\n")
    script += ("wait_remote_ready\nwait_all_ready\nactivate ButtonMultiplayerStart\n" if who == "Host" else
               "activate ButtonMultiplayerReady\n")
    script += ("wait_state Running 120\nassert_label LabelLastMatchDetails\n"
               "wait_state Starting 240\nwait 20\nassert_substate Lobby\n"
               "assert_label LabelLastMatchSummary Last match: draw\n"
               "assert_label LabelLastMatchSummary 00:02\n"
               "assert_label LabelMultiplayerStatus ready up for a rematch\n"
               "activate ButtonLastMatchDetails\nwait 10\nassert_label LabelLastMatchDetails Result:\n"
               "screenshot report_second_details\nactivate ButtonLastMatchClose\nwait 10\n"
               "wait_ms 3000\nexit\n")
    return script


def run_size(repo, root, size, port, expected):
    root.mkdir(parents=True, exist_ok=False)
    runs, records, checks, details = {}, {}, {}, {}
    before = pin(repo, expected)
    try:
        for who in ("Host", "Guest"):
            script = root / f"{who}.txt"
            script.write_text(menu_script(who, port), encoding="utf-8")
            runs[who] = make_run(repo, ["-menu-script", script, "-num-lua-states", 4,
                                       "-net-match-ticks", TICKS, "-net-match-report", root / f"{who}_report.json"],
                                  root / who, 480, env={"CCCP_HEADLESS": "1"})
            set_resolution(runs[who].cwd, *size)
        runs["Host"].start()
        wait_for_log(runs["Host"], "activate ButtonMultiplayerCreate ok=1", 120)
        runs["Guest"].start()
        with ThreadPoolExecutor(max_workers=2) as pool:
            records = dict(pool.map(lambda item: (item[0], item[1].finish()), runs.items()))
        logs = {who: read_log(run.out) for who, run in runs.items()}
        announced_delays = {}
        for who, log in logs.items():
            delays = set(re.findall(r'\[menu-script\] dump_lobby .* input_delay="Input delay: (\d+)', log))
            if len(delays) == 1:
                announced_delays[who] = int(delays.pop())
        details["announced_delays"] = announced_delays
        for who, run in runs.items():
            log = logs[who]
            console_path = run.cwd / "LogConsole.txt"
            console = console_path.read_text(encoding="utf-8", errors="replace") if console_path.exists() else ""
            labels = LABELS.findall(log)
            record = records[who]
            failures = re.findall(r"^.*(?:FAILED|FAIL|EXCEPTION_|RTE Assert|RTE Abort|Runtime Error).*$", log, re.M)
            checks[who + "_process"] = record["exit_code"] == 0 and not record["timed_out"]
            checks[who + "_desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
            checks[who + "_exe"] = record["exe_sha256"] == expected
            checks[who + "_launched"] = "[menu-mp] launching the match" in log
            checks[who + "_ended"] = "NETWORK: Match complete" in console
            checks[who + "_two_rounds"] = log.count("[menu-mp] launching the match") == 2
            clear_observations = re.findall(r'^\[menu-mp\] report at launch summary="([^"\n]*)" details="([^"\n]*)" retained=(\d+)$', log, re.M)
            checks[who + "_cleared_on_start"] = (clear_observations == [("", "", "0"), ("", "", "0")] and
                                                  any(name == "LabelLastMatchDetails" and text == "" and status == "PASS" for name, text, status in labels))
            checks[who + "_assertions"] = not failures and len(labels) >= 14 and all(status == "PASS" for _, _, status in labels)
            details[who] = {"failures": failures, "labels": labels, "record": record, "console_path": str(console_path),
                            "clear_observations": clear_observations}
            report_path = root / f"{who}_report.json"
            report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.exists() else {}
            summary = report.get("last_match", report.get("service", {}).get("last_match"))
            details[who]["summary"] = summary
            details[who]["end_reason"] = end_reason_agreement(labels, summary)
            checks[who + "_end_reason_agreement"] = details[who]["end_reason"]["pass"]
            checks[who + "_summary"] = bool(summary and summary["running_ticks"] == TICKS and summary["winner_team"] == -1 and
                                           [peer["name"] for peer in summary["peers"]] == ["Host", "Guest"])
            checks[who + "_pace_is_last_round"] = bool(summary and summary["pace"]["sim_ticks"] == TICKS)
            checks[who + "_peer_details"] = bool(summary and len(announced_delays) == 2 and all(
                peer["input_delay"] == announced_delays.get(peer["name"]) and any(
                    name == "LabelLastMatchDetails" and f'{peer["name"]} | team {peer["team"] + 1} | seat {peer["seat"]} | delay {peer["input_delay"]}' in text
                    for name, text, _ in labels) for peer in summary["peers"]))
            checks[who + "_identity"] = any(name == "LabelLastMatchDetails" and expected in text for name, text, _ in labels)
            for stem in ("report_lobby", "report_summary", "report_details"):
                capture = capture_geometry(latest_capture(run, stem), size)
                details[who][stem] = capture
                checks[who + "_" + stem] = capture["dimensions"] == list(size) and capture["inside_viewport"]
            rows = captured_chat_rows(latest_capture(run, "report_summary"))
            details[who]["chat_after"] = rows
            checks[who + "_chat_rows"] = rows["count"] >= 5
        before_shot = latest_capture(runs["Host"], "report_before")
        details["chat_before"] = {"capture": str(before_shot), **captured_chat_rows(before_shot)}
        checks["before_chat_rows"] = details["chat_before"]["count"] >= 5
        shared_fields = ("result", "winner_team", "running_ticks", "duration", "peers", "resyncs", "drops", "reclaims", "substitutions")
        checks["peer_agreement"] = bool(details["Host"]["summary"] and details["Guest"]["summary"] and all(
            details["Host"]["summary"][field] == details["Guest"]["summary"][field] for field in shared_fields))
        checks["pin_unchanged"] = pin(repo, expected) == before
    except Exception as error:
        details["error"] = repr(error)
        checks["completed"] = False
    finally:
        for run in runs.values():
            run.close()
    result = {"pass": bool(checks) and all(checks.values()), "checks": checks, "details": details, "pin": before}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--port", type=int, default=48211)
    args = parser.parse_args()
    if not 48211 <= args.port <= 48218:
        parser.error("two ports must fit 48211-48219")
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("family lock exists; no driver may run")
    os.environ["CCCP_HEADLESS"] = "1"
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    results = {f"{w}x{h}": run_size(args.repo.resolve(), root / f"{w}x{h}", (w, h), args.port + index, args.exe_sha256.lower())
               for index, (w, h) in enumerate(SIZES)}
    result = {"pass": all(row["pass"] for row in results.values()), "runs": results,
              "driver_sha256": sha256(__file__), "match_end": "menu -net-match-ticks 120; rematch routing required"}
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "out": str(root), "exe_sha256": args.exe_sha256}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
