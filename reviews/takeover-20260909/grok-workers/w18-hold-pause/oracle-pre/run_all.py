"""Runs every §9a two-peer gate driver in this directory, in the order the lead should take them.

Order matters: the cheapest and most diagnostic first, so a failure lands on the smallest driver.

    1. gns_provider_smoke       one plain match with the plane live; proves real crypto over a socket
    2. old_wire_fixture         --inprocess by default; §10's two arms plus a live-match control
    3. clean_leave              §7's leave protocol and the ambiguous loss that must NOT clear it
    4. reclaim_socket           the headline case: drop, relaunch, reclaim, reseat, resync
    5. crash_relaunch_provisional   §4's provisional window across two processes
    6. fencing_two_transports   §6 with two live connections holding one ticket
    7. rejoin_after_resync      resync and rematch arms
    8. peers_3_4_regression     3- and 4-peer, plus a 3-peer drop/reclaim

Every driver verifies the executable against gates/build-manifest.json first and refuses to produce
evidence against any other binary. Nothing here kills a foreign process; each driver terminates only
the jobs it started.

Run:
    python .../run_all.py                 # all eight
    python .../run_all.py --only reclaim_socket clean_leave
    python .../run_all.py --dry-run       # prints every command every driver would launch
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

GATES = Path(__file__).resolve().parent

ORDER = [
    ("gns_provider_smoke", []),
    ("old_wire_fixture", ["--inprocess"]),
    ("clean_leave", []),
    ("reclaim_socket", []),
    ("crash_relaunch_provisional", []),
    ("fencing_two_transports", []),
    ("rejoin_after_resync", []),
    ("peers_3_4_regression", []),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="*", default=None)
    # LANE EDIT: the manifest is an argument now, handed to the drivers through the env so
    # none of the eight needs its own flag.
    parser.add_argument("--build-manifest", default=None)
    parser.add_argument("--run-root", default=None)
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()
    if options.build_manifest:
        os.environ["CC_H4_BUILD_MANIFEST"] = str(Path(options.build_manifest).resolve())
    if options.run_root:
        os.environ["CC_H4_RUN_ROOT"] = str(Path(options.run_root))

    selected = [
        (name, args)
        for name, args in ORDER
        if options.only is None or name in options.only
    ]
    results = {}
    for name, args in selected:
        argv = [sys.executable, str(GATES / f"{name}.py"), *args]
        if options.dry_run:
            argv.append("--dry-run")
        print(f"=== {name} ===", flush=True)
        proc = subprocess.run(argv)
        results[name] = proc.returncode
        if proc.returncode != 0 and not options.dry_run:
            print(f"stopping: {name} did not pass", flush=True)
            break
    summary = (
        Path(os.environ.get("CC_H4_RUN_ROOT", str(GATES / "runs"))) / "run_all_summary.json"
    )
    summary.parent.mkdir(parents=True, exist_ok=True)
    summary.write_text(json.dumps(results, indent=2), encoding="utf-8")
    failed = [name for name, code in results.items() if code != 0]
    print("failed:", failed if failed else "none")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
