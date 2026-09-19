"""Check the print discipline the drivers depend on: one tag per line, one write per line.

A diagnostic line a parser reads has to reach the log whole. A multi-inserter print
(``std::cout << tag << field << ...``) lets another thread's own line land inside it, so
every tagged line in the files below goes out through the single-write helpers in
Source/System/System.cpp.

Source scan by default; ``--log <stdout.log>`` also reads a captured run for a line that
carries two tags, which is the shape the torn line in the ledger had
("[telemetry] saved ....zip[menu-script] file:..."). No engine, no launch.

Run: python tools/test_print_discipline.py --repo <tree> [--log <stdout.log> ...] [--out result.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

# The tagged prints whose files are held to the discipline, and the tag each file owns.
DISCIPLINED = (
    ("Source/Main.cpp", "ai-write-script"),
    ("Source/Main.cpp", "audio"),
    ("Source/Main.cpp", "autosave"),
    ("Source/Main.cpp", "camera-null-scene-selftest"),
    ("Source/Main.cpp", "chat"),
    ("Source/Main.cpp", "chat-send"),
    ("Source/Main.cpp", "contract-audit"),
    ("Source/Main.cpp", "contract-audit-continuation"),
    ("Source/Main.cpp", "contract-audit-pending"),
    ("Source/Main.cpp", "cow-checkpoint-selftest"),
    ("Source/Main.cpp", "digital-aim-speed"),
    ("Source/Main.cpp", "e2e"),
    ("Source/Main.cpp", "feel"),
    ("Source/Main.cpp", "global-callback-selftest"),
    ("Source/Main.cpp", "harness-order"),
    ("Source/Main.cpp", "input-script"),
    ("Source/Main.cpp", "load-game"),
    ("Source/Main.cpp", "localpred"),
    ("Source/Main.cpp", "lpinv"),
    ("Source/Main.cpp", "menu-mp"),
    ("Source/Main.cpp", "menu-script"),
    ("Source/Main.cpp", "net-chat-script"),
    ("Source/Main.cpp", "net-directory-list"),
    ("Source/Main.cpp", "net-directory-probe"),
    ("Source/Main.cpp", "net-directory-signal-probe"),
    ("Source/Main.cpp", "net-discovery-selftest"),
    ("Source/Main.cpp", "net-h4-fault"),
    ("Source/Main.cpp", "net-identity-dump"),
    ("Source/Main.cpp", "net-lockstep"),
    ("Source/Main.cpp", "net-match"),
    ("Source/Main.cpp", "net-match-e2e"),
    ("Source/Main.cpp", "net-match-e2e-spawn"),
    ("Source/Main.cpp", "net-match-screenshot"),
    ("Source/Main.cpp", "net-match-service-e2e"),
    ("Source/Main.cpp", "net-port-map-probe"),
    ("Source/Main.cpp", "net-replay"),
    ("Source/Main.cpp", "net-replay-dump"),
    ("Source/Main.cpp", "net-replay-verify"),
    ("Source/Main.cpp", "net-session"),
    ("Source/Main.cpp", "pace"),
    ("Source/Main.cpp", "preview-event-selftest"),
    ("Source/Main.cpp", "preview-funds-driver"),
    ("Source/Main.cpp", "preview-hud-selftest"),
    ("Source/Main.cpp", "rbfuzz"),
    ("Source/Main.cpp", "rbprobe"),
    ("Source/Main.cpp", "reader-empty-selftest"),
    ("Source/Main.cpp", "replay-end"),
    ("Source/Main.cpp", "save-selftest"),
    ("Source/Main.cpp", "scenario"),
    ("Source/Main.cpp", "script-graph-selftest"),
    ("Source/Main.cpp", "selftest"),
    ("Source/Main.cpp", "setaside-latch"),
    ("Source/Main.cpp", "sim-dump"),
    ("Source/Main.cpp", "snapshot-audio-boundary"),
    ("Source/Main.cpp", "snapshot-roundtrip"),
    ("Source/Main.cpp", "terrain-dump"),
    ("Source/Managers/AudioMan.cpp", "audibility"),
    ("Source/Managers/AudioMan.cpp", "audio"),
    ("Source/Managers/AudioMan.cpp", "audio-boundary"),
    ("Source/Managers/AudioMan.cpp", "audio-checkpoint"),
    ("Source/Managers/AudioMan.cpp", "audio-checkpoint-selftest"),
    ("Source/Managers/AudioMan.cpp", "audio-cursor-negative"),
    ("Source/Managers/AudioMan.cpp", "audio-logical-selftest"),
    ("Source/Managers/AudioMan.cpp", "audio-output"),
    ("Source/Managers/AudioMan.cpp", "audio-playback-continuation"),
    ("Source/Managers/AudioMan.cpp", "audio-playback-progress"),
    ("Source/Managers/AudioMan.cpp", "audio-test-output"),
    ("Source/Managers/AudioMan.cpp", "preview-event"),
    ("Source/Managers/AudioMan.cpp", "sound-scope"),
    ("Source/Activities/GameActivity.cpp", "checkpoint"),
    ("Source/Activities/GameActivity.cpp", "controller-boundary-selftest"),
    ("Source/Activities/GameActivity.cpp", "native-reference-selftest"),
    ("Source/Activities/GameActivity.cpp", "net-inventory-held"),
    ("Source/Activities/GameActivity.cpp", "net-inventory-resync"),
    ("Source/Activities/GameActivity.cpp", "net-inventory-resync-fixture"),
    ("Source/Activities/GameActivity.cpp", "net-inventory-slots"),
    ("Source/Activities/GameActivity.cpp", "net-local-above-head"),
    ("Source/Activities/GameActivity.cpp", "net-local-all-players"),
    ("Source/Activities/GameActivity.cpp", "net-local-area-picker"),
    ("Source/Activities/GameActivity.cpp", "net-local-banner"),
    ("Source/Activities/GameActivity.cpp", "net-local-highlight"),
    ("Source/Activities/GameActivity.cpp", "net-local-intensity"),
    ("Source/Activities/GameActivity.cpp", "net-local-menu"),
    ("Source/Activities/GameActivity.cpp", "net-local-picker"),
    ("Source/Activities/GameActivity.cpp", "net-local-ui-graph"),
    ("Source/Activities/GameActivity.cpp", "net-local-ui-publication"),
    ("Source/Activities/GameActivity.cpp", "net-local-ui-selftest"),
    ("Source/Activities/GameActivity.cpp", "net-local-ui-slots"),
    ("Source/Activities/GameActivity.cpp", "net-match"),
    ("Source/Activities/GameActivity.cpp", "net-reclaim"),
    ("Source/Activities/GameActivity.cpp", "net-reclaim-native"),
    ("Source/Activities/GameActivity.cpp", "net-relaunch-inventory-probe"),
    ("Source/Activities/GameActivity.cpp", "p4-duel"),
    ("Source/Activities/GameActivity.cpp", "spectate-follow"),
    ("Source/CI/DeterminismCheck.cpp", "determinism-check"),
    ("Source/System/TelemetryBundle.cpp", "telemetry"),
    ("Source/CI/NetModerationGUIProbe.cpp", "net-ui-probe"),
)
FPRINTF_STDERR = re.compile(r'std::fprintf\(stderr,\s*"RTE ')
WRITER = re.compile(r"void WriteWholeLine\(std::ostream& stream, const std::string& line\)")
LOCKED_WRITE = re.compile(r"std::scoped_lock printLock\(PrintLock\(\)\);")
FAULT_WRITE = re.compile(r"std::unique_lock<std::mutex> printLock\(PrintLock\(\), std::try_to_lock\);")
# Any line of a captured log that carries a second tag after its first one is a torn line.
TWO_TAGS = re.compile(r"^\[[a-z0-9-]+\][^\n]*\[[a-z0-9-]+\]")


def streamed(tag: str) -> re.Pattern:
    return re.compile(r'std::(?:cout|cerr)\s*<<\s*(?:std::format\()?"\[' + re.escape(tag) + r'\]')


def hits(path: Path, pattern: re.Pattern) -> list:
    if not path.is_file():
        return [{"file": str(path), "line": 0, "text": "file missing"}]
    found = []
    for number, text in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if pattern.search(text):
            found.append({"file": str(path), "line": number, "text": text.strip()})
    return found


def check(repo: Path, logs: list) -> dict:
    main, rte_error, system = repo / "Source/Main.cpp", repo / "Source/System/RTEError.cpp", repo / "Source/System/System.cpp"
    system_text = system.read_text(encoding="utf-8", errors="replace") if system.is_file() else ""
    main_text = main.read_text(encoding="utf-8", errors="replace") if main.is_file() else ""
    rows = {}
    for name, tag in DISCIPLINED:
        rows[f"{tag}_lines_are_one_write"] = hits(repo / name, streamed(tag))
    rows["rte_error_lines_use_the_print_lock"] = hits(rte_error, FPRINTF_STDERR)
    rows["menu_script_helper_present"] = [] if "void MenuScriptPrint(const std::string& line)" in main_text else \
        [{"file": str(main), "line": 0, "text": "MenuScriptPrint helper missing"}]
    rows["one_locked_write_per_line"] = [] if WRITER.search(system_text) and LOCKED_WRITE.search(system_text) else \
        [{"file": str(system), "line": 0, "text": "WriteWholeLine under PrintLock missing"}]
    rows["fault_paths_never_wait"] = [] if FAULT_WRITE.search(system_text) else \
        [{"file": str(system), "line": 0, "text": "fault writer does not try_lock"}]
    rows["print_to_cli_writes_whole_lines"] = [] if "WriteWholeLine(std::cout, ComposeCliLine(stringToPrint))" in system_text else \
        [{"file": str(system), "line": 0, "text": "PrintToCLI does not use WriteWholeLine"}]
    torn = []
    for log in logs:
        path = Path(log)
        if not path.is_file():
            torn.append({"file": str(path), "line": 0, "text": "log missing"})
            continue
        for number, text in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if TWO_TAGS.match(text):
                torn.append({"file": str(path), "line": number, "text": text.strip()[:300]})
    if logs:
        rows["captured_log_has_no_torn_line"] = torn
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--log", action="append", default=[], help="captured stdout log to scan for torn lines")
    parser.add_argument("--out", type=Path, help="write the result JSON here")
    options = parser.parse_args()
    rows = check(options.repo.resolve(), options.log)
    failed = 0
    for name, offenders in rows.items():
        if offenders:
            failed += 1
            for row in offenders:
                print("{}:{}: {}".format(row["file"], row["line"], row["text"]))
            print("[print-discipline] {} FAIL: {} observed".format(name, len(offenders)))
        else:
            print("[print-discipline] {} PASS".format(name))
    result = {"pass": failed == 0, "rows": {name: offenders for name, offenders in rows.items()},
              "repo": str(options.repo.resolve()), "logs": [str(log) for log in options.log]}
    if options.out:
        options.out.parent.mkdir(parents=True, exist_ok=True)
        options.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("[print-discipline] " + ("PASS" if result["pass"] else "FAIL"))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
