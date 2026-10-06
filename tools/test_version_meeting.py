"""Two versions meeting read why: an older build and this one, each way round.

The older build is a tree of the public line (its executable and its own Data); this tree is the newer. In each direction
one hosts and the other joins by address. The join is refused; this build's side must read a sentence that says which
of the two has the newer game. What the older build reads is recorded as it is (its text cannot change).

  python tools/test_version_meeting.py --older D:/Projects/takeover-build --out D:/mx/<lane>/versions --port 49836
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from run_sim_test import make_run, engine_executable, file_sha256  # noqa: E402
import test_menu_readback as readback  # noqa: E402

# What this build's player reads, in each role.
NEWER_JOINER_READS = "This host runs an older game version."
NEWER_HOST_READS = "A player could not join: Their game is an older version than yours."


def scripts(port, newer_hosts):
    host = readback.LANDING + "activate ButtonMultiplayerHostGame\nwait_ms 400\n" + f"setup_host_port {port}\n" \
        "activate ButtonMultiplayerCreate\nwait 15\nwait_ms 30000\ndump_lobby\n"
    client = (readback.LANDING + "activate ButtonMultiplayerJoinGame\nwait_ms 400\nactivate ButtonJoinByAddress\nwait 4\n"
              f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\nactivate ButtonJoinAddressGo\n")
    if newer_hosts:
        # A refused player is named under the lobby, in its error line.
        host += f"wait_label LabelMultiplayerError {NEWER_HOST_READS}\nassert_text_fits LabelMultiplayerError\ndump_lobby\nexit\n"
        client = readback.in_base_words(client + "wait_ms 20000\ndump_lobby\nexit\n")
    else:
        host = readback.in_base_words(host + "exit\n")
        client += f"wait_state Failed\nwait_ms 500\ndump_lobby\nassert_label LabelJoinSelected {NEWER_JOINER_READS}\nassert_text_fits LabelJoinSelected\nexit\n"
    return host, client


def texts(run):
    """Every line either engine printed about the refusal, and its lobby dumps' words."""
    found = []
    log = Path(run.out) / "stdout.log"
    if log.exists():
        found += [line for line in log.read_text(errors="replace").splitlines()
                  if any(word in line for word in ("refus", "could not join", "version", "FAILED", "assert_label", "wait_label"))][-25:]
    for dump in sorted((Path(run.cwd) / "ScreenShots").glob("dump_lobby_*.json")):
        data = json.loads(dump.read_text(encoding="utf-8"))
        found.append({key: data.get(key) for key in ("status", "status_text", "error", "error_text", "labels") if key in data} or
                     {"keys": sorted(data)[:30]})
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--older", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=49836)
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    trees = {"newer": options.repo.resolve(), "older": options.older.resolve()}
    result = {"pass": True, "trees": {name: {"path": str(tree), "head": subprocess.check_output(["git", "-C", str(tree), "rev-parse", "HEAD"], text=True).strip(),
                                             "exe_sha256": file_sha256(engine_executable(tree))} for name, tree in trees.items()}, "directions": {}}
    for index, newer_hosts in enumerate((False, True)):
        name = "newer-hosts" if newer_hosts else "older-hosts"
        port = options.port + index
        host_text, client_text = scripts(port, newer_hosts)
        direction = root / name
        runs = {}
        for who, tree, text in (("host", trees["newer" if newer_hosts else "older"], host_text), ("client", trees["older" if newer_hosts else "newer"], client_text)):
            script = direction / f"{who}-menu.txt"
            script.parent.mkdir(parents=True, exist_ok=True)
            script.write_text(text, encoding="utf-8")
            runs[who] = make_run(tree, ["-menu-script", str(script)], direction / who, 240, env={"CCCP_HEADLESS": "1"})
        records = {}

        def drive(who):
            records[who] = runs[who].start().finish()

        threads = []
        for who in ("host", "client"):
            threads.append(threading.Thread(target=drive, args=(who,)))
            threads[-1].start()
            time.sleep(2)
        for thread in threads:
            thread.join()
        newer = "host" if newer_hosts else "client"
        verdict = records[newer].get("exit_code") == 0
        result["directions"][name] = {"pass": verdict, "newer_side": newer, "exit_codes": {who: records[who].get("exit_code") for who in records},
                                      "reads": {who: texts(runs[who]) for who in runs}}
        result["pass"] = result["pass"] and verdict
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[versions] {'PASS' if result['pass'] else 'FAIL'} {root / 'result.json'}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
