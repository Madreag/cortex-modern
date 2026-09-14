"""Write a private Settings.ini with every L01 key at a non-default, plus a broken file for the FAIL demo.

Python only. Does not launch the engine. Scores a captured FAIL log through run_selftests.py --score-stdout.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

KEYS = [
    ("NetworkDisplayName", "AlphaPilot"),
    ("NetworkMatchStatusMode", "Always"),
    ("NetworkToastsEnabled", "0"),
    ("NetworkChatVisible", "0"),
    ("NetworkChatDefaultScope", "Team"),
    ("NetworkChatNotify", "0"),
    ("NetworkChatSound", "1"),
    ("NetworkChatTextSize", "Large"),
    ("NetworkAutoReconnect", "0"),
    ("NetworkOfferStoredRejoin", "0"),
    ("NetworkDiagnosticsDirectory", "D:/tmp/telemetry-alt"),
    ("NetworkRecordReplays", "0"),
    ("NetworkHostDelayPolicy", "Fixed"),
    ("NetworkHostAutoRepair", "0"),
    ("NetworkHostIdleWaitMinutes", "0"),
    ("NetworkHostVisibility", "Unlisted"),
]


def write_settings(path: Path, rows: list[tuple[str, str]]) -> None:
    lines = ["SettingsMan"]
    for key, value in rows:
        lines.append(f"\t{key} = {value}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    options = parser.parse_args()
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    good = out / "Settings.ini"
    broken = out / "broken-Settings.ini"
    fail_log = out / "settings-preferences-selftest-FAIL.stdout"
    write_settings(good, KEYS)
    write_settings(broken, [("NetworkDisplayName", "BrokenName")])
    fail_log.write_text("[settings-preferences-selftest] FAIL NetworkDisplayName\n", encoding="utf-8")

    scored = subprocess.run(
        [
            sys.executable,
            str(options.repo / "tools" / "run_selftests.py"),
            "--score-stdout",
            str(fail_log),
            "--name",
            "settings-preferences-selftest",
            "--exit-code",
            "0",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    (out / "score-fail.stdout").write_text(scored.stdout + scored.stderr, encoding="utf-8")
    print(f"wrote {good}")
    print(f"wrote {broken}")
    print(f"wrote {fail_log}")
    print(f"score_exit={scored.returncode}")
    print(scored.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
