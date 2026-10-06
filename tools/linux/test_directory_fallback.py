"""Row 516: an unavailable directory explains the fallback and a typed LAN address joins."""

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_sim_test import make_run, seed_settings
from test_lobby_lifecycle import menu_script, wait_for_log
from test_menu_readback import spread, managed_case

HINT = "the session directory is not reachable: LAN games and a typed address still work"


@managed_case
def check(repo, root, port, url):
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    result = {"pass": False, "url": url, "port": port}
    execution = None
    try:
        execution = spread.prepare_case(repo, root,
            [spread.Peer("host", os="any"), spread.Peer("guest", os="windows", reviewed=True)],
            spread.Match(port, parameters={"lane": "menus", "network": "direct"}))
        host = menu_script("Host", True, 2, port)
        host += "wait_connected 2 90\nassert_substate Lobby\nwait_ms 1000\ngoto_main\nexit\n"
        guest = ("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\n"
                 "activate ButtonMultiplayerJoinGame\nwait_ms 7000\n"
                 f"assert_label LabelLanGames {HINT}\n"
                 "assert_visible LabelLanGames 1\n"
                 "assert_text_fits LabelLanGames\n"
                 f"activate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
                 "activate ButtonJoinAddressGo\nwait_connected 2 90\n"
                 "assert_substate Lobby\ndump_lobby\nwait_ms 500\ngoto_main\nexit\n")
        for who, script in (("host", host), ("guest", guest)):
            path = root / f"{who}.menu.txt"
            path.write_text(script, encoding="utf-8")
            run = execution.make_run(repo, ["-menu-script", path, "-num-lua-states", "4"], root / who, 150,
                           env={"CCCP_HEADLESS": "1"})
            seed_settings(run, {"SessionDirectoryUrl": url, "NetworkIceEnable": 0})
            runs[who] = run.start()
            if who == "host":
                wait_for_log(run, "activate ButtonMultiplayerCreate click ButtonMultiplayerCreate PASS", 90)
        records["guest"] = runs["guest"].finish()
        if records["guest"].get("exit_code") == 0:
            records["host"] = runs["host"].finish()
        log = (root / "guest/stdout.log").read_text(errors="replace")
        settings = (Path(runs["guest"].cwd) / "Userdata/Settings.ini").read_text(errors="replace")
        stored = re.search(r"(?m)^\s*SessionDirectoryUrl\s*=([^\r\n]*)", settings)
        result["checks"] = {"fallback_visible": f'assert_label LabelLanGames "{HINT}" text="{HINT}" PASS' in log,
                            "fallback_fits": "assert_text_fits LabelLanGames" in log and "FAIL" not in log,
                            "typed_join": "connected:2 -> OK" in log,
                            "url_retained": stored is not None and stored[1].strip() == url,
                            "exit_zero": all(records.get(who, {}).get("exit_code") == 0 for who in ("host", "guest"))}
        result["pass"] = all(result["checks"].values())
    except Exception as error:
        result["error"] = repr(error)
    finally:
        for run in runs.values():
            run.close()
    result["records"] = records
    receipt = execution.result() if execution else spread.read_json(root / "spread-result.json", {})
    result.update(topology="spread", peer_boxes=receipt.get("peer_boxes", {}), spread=receipt, proof=result["pass"])
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=50300)
    if spread:
        spread.add_arguments(parser)
    args = parser.parse_args()
    if not spread:
        parser.error("directory fallback requires the shared spread executor")
    args.spread = True
    spread.configure(args)
    results = {name: check(args.repo.resolve(), args.out.resolve() / name, args.port + index, url)
               for index, (name, url) in enumerate((("unset", ""), ("unreachable", "https://127.0.0.1:1/custom-directory")))}
    summary = {**results, "topology": "spread",
               "peer_boxes": {name: row["peer_boxes"] for name, row in results.items()},
               "proof": all(row["pass"] for row in results.values())}
    (args.out / "result.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({name: result["pass"] for name, result in results.items()}))
    return 0 if all(result["pass"] for result in results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
