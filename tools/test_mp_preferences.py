"""Write L01 Settings.ini fixtures. Does not launch the engine unless --launch is set.

The handwritten FAIL log is a scorer demo, not RED. PHASE B RED is the engine run with
CCCP_SETTINGS_PREFERENCES_SELFTEST_BROKEN pointed at broken-Settings.ini.
"""

from __future__ import annotations

import argparse
import json
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
    ("NetworkChatKey", "Y"),
    ("NetworkAutoReconnect", "0"),
    ("NetworkOfferStoredRejoin", "0"),
    ("NetworkDiagnosticsDirectory", "D:/tmp/telemetry-alt"),
    ("NetworkRecordReplays", "0"),
    ("NetworkHostDelayPolicy", "Fixed"),
    ("NetworkHostAutoRepair", "0"),
    ("NetworkHostIdleWaitMinutes", "0"),
    ("NetworkPathHorizonTicks", "45"),
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
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--launch", choices=("pass", "fail"), help="PHASE B only: runner-only engine launch")
    options = parser.parse_args()
    out = options.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    good = out / "Settings.ini"
    broken = out / "broken-Settings.ini"
    write_settings(good, KEYS)
    write_settings(broken, [("NetworkDisplayName", "BrokenName")])
    print(f"wrote {good}")
    print(f"wrote {broken}")
    print("PHASE B pass: python tools/run_sim_test.py --repo <tree> --out <out>/pass --timeout 300 -- -settings-preferences-selftest")
    print(f"PHASE B fail: python tools/test_mp_preferences.py --out <out> --repo <tree> --launch fail  (CCCP_SETTINGS_PREFERENCES_SELFTEST_BROKEN={broken})")
    print("PHASE B suite: python tools/run_selftests.py --repo <tree> --out <out>/selftests --timeout 300")

    if options.launch is None:
        return 0

    sys.path.insert(0, str(options.repo / "tools"))
    from run_sim_test import make_run  # noqa: PLC0415

    case = out / options.launch
    env = None
    if options.launch == "fail":
        env = {"CCCP_SETTINGS_PREFERENCES_SELFTEST_BROKEN": str(broken)}
    run = make_run(options.repo, ["-settings-preferences-selftest"], case, options.timeout, env=env)
    try:
        record = run.start().finish()
    finally:
        run.close()
    print(json.dumps({k: record.get(k) for k in ("pid", "exit_code", "timed_out")}, indent=2))
    return 0 if record.get("exit_code") == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
