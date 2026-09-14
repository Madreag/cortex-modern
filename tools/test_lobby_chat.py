"""Lobby/match chat routing over the session wire, driven by -net-chat-script.

Two service-e2e peers run the same 300-tick match on loopback while a chat script on each
side schedules sends at fixed sim ticks. The oracle is the [chat] receive lines each peer
prints when it drains the session's presentation sink:

  * host sends "hello from host" (all) at tick 50 and "team only" (team) at tick 60,
  * client sends "hi from client" (all) at tick 70,
  * the default PvP roster puts the host on team 0 and the client on team 1, so the team
    line must reach nobody but the host's own echo,
  * each peer's [chat] list must equal the expected (from, scope, text) sequence exactly -
    any missing, extra, truncated, or reordered line fails,
  * chat is presentation-only, so the chat run's tick hashes must match between the two
    peers AND match a control pair that ran the same match with no chat script,
  * a separate arm proves -net-chat-script without -net-match-service-e2e is refused.
"""

import argparse
import json
import re
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sim_test import make_run  # noqa: E402
from compare_sim_traces import strict_compare  # noqa: E402

CHAT_LINE = re.compile(r"^\[chat\] tick=(\d+) from=(\d+) scope=(all|team) text=(.*)$")
SEND_LINE = re.compile(r"^\[chat-send\] tick=(\d+) scope=(all|team) ok=([01]) text=(.*)$")

HOST_SCRIPT = "50 all hello from host\n60 team team only\n"
CLIENT_SCRIPT = "70 all hi from client\n"

# (sender session id, scope, text): host is session id 0, the client's assigned id is 1.
EXPECTED = {
    "host": [(0, "all", "hello from host"), (0, "team", "team only"), (1, "all", "hi from client")],
    "client": [(0, "all", "hello from host"), (1, "all", "hi from client")],
}


def peer_args(root: Path, who: str, port: int, ticks: int, script: Path | None) -> list:
    args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-ticks", str(ticks),
            "-net-match-peers", "2", "-net-match-input-delay", "3",
            "-tick-hashes", "-max-ticks", str(ticks),
            "-out", str(root / f"{who}_trace.json"),
            "-net-match-report", str(root / f"{who}_report.json")]
    if script is not None:
        args += ["-net-chat-script", str(script)]
    args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
    return args


def chat_lines(log_text: str) -> list:
    return [(int(m.group(2)), m.group(3), m.group(4)) for line in log_text.splitlines()
            if (m := CHAT_LINE.match(line))]


def send_lines(log_text: str) -> list:
    return [(int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)) for line in log_text.splitlines()
            if (m := SEND_LINE.match(line))]


def read_log(out: Path) -> str:
    text = ""
    for name in ("stdout.log", "stderr.log"):
        path = out / name
        if path.exists():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def run_pair(repo: Path, root: Path, port: int, ticks: int, scripts: dict) -> dict:
    root.mkdir(parents=True, exist_ok=True)
    script_paths = {}
    for who, text in scripts.items():
        script_paths[who] = root / f"{who}_chat.txt"
        script_paths[who].write_text(text, encoding="utf-8")
    runs = {who: make_run(repo, peer_args(root, who, port, ticks, script_paths.get(who)), root / who, 300)
            for who in ("host", "client")}
    records = {}

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as exc:  # noqa: BLE001
            records[who] = {"error": repr(exc)}

    threads = [threading.Thread(target=drive, args=(who,), daemon=True) for who in ("host", "client")]
    threads[0].start()
    # The client needs a listening socket to join; the host's e2e setup is quick but not instant.
    threading.Event().wait(1.0)
    threads[1].start()
    for thread in threads:
        thread.join()
    for run in runs.values():
        run.close()
    logs = {who: read_log(root / who) for who in ("host", "client")}
    return {"records": records, "logs": logs, "argvs": {who: peer_args(root, who, port, ticks, script_paths.get(who))
                                                        for who in ("host", "client")}}


def set_resolution(runtime: Path, x: int, y: int) -> None:
    """Retargets the prepared runtime's own Settings.ini - the repo copy stays untouched."""
    settings = runtime / "Userdata" / "Settings.ini"
    text = settings.read_text(encoding="utf-8")
    for name, value in (("ResolutionX", x), ("ResolutionY", y)):
        text, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*", rf"\g<1>{value}", text)
        if count == 0:
            text += f"\n\t{name} = {value}\n"
    settings.write_text(text, encoding="utf-8")


def shots_menu_script(who: str, port: int, res_tag: str) -> str:
    head = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {who}\n"
    if who == "Host":
        return (head + "activate ButtonMultiplayerHostGame\nwait 10\n"
                f"settext TextHostPort {port}\nsettext TextHostPlayers 2\nsettext TextHostInputDelay 3\n"
                "activate ButtonMultiplayerCreate\nwait_connected 2\n"
                "chat all lobby hello at minimum viewport\nwait 15\n"
                "chat team for my team only\nwait 15\n"
                "settext TextLobbyChat typed but not sent\nwait 10\n"
                f"screenshot lobby_chat_{res_tag}\n"
                "assert_label LabelLobbyChat7 hello from the client seat\n"
                "dump_lobby\nexit\n")
    return (head + "activate ButtonMultiplayerJoinGame\nwait 10\n"
            f"settext TextJoinAddress 127.0.0.1\nsettext TextJoinPort {port}\n"
            "activate ButtonMultiplayerConnect\nwait_connected 2\nwait 20\n"
            "chat all hello from the client seat\nwait 30\nexit\n")


def run_shots(repo: Path, root: Path, port: int) -> dict:
    """One lobby at 640x360 and one at 960x540: the host's panel carries real received lines."""
    root.mkdir(parents=True, exist_ok=True)
    result = {"checks": {}, "details": {}}
    for res_tag, res in (("640x360", (640, 360)), ("960x540", (960, 540))):
        arm = root / res_tag
        runs, records = {}, {}
        for who in ("host", "client"):
            script = arm / f"{who}.txt"
            arm.mkdir(parents=True, exist_ok=True)
            script.write_text(shots_menu_script("Host" if who == "host" else "Guest", port, res_tag), encoding="utf-8")
            runs[who] = make_run(repo, ["-menu-script", str(script), "-net-match-report", str(arm / f"{who}_report.json")],
                                 arm / who, 240)
            if who == "host":
                set_resolution(runs[who].cwd, *res)

        def drive(name: str) -> None:
            try:
                records[name] = runs[name].start().finish()
            except Exception as exc:  # noqa: BLE001
                records[name] = {"error": repr(exc)}

        host = threading.Thread(target=drive, args=("host",), daemon=True)
        host.start()
        threading.Event().wait(1.5)
        client = threading.Thread(target=drive, args=("client",), daemon=True)
        client.start()
        host.join()
        client.join()
        for run in runs.values():
            run.close()
        host_log = read_log(arm / "host")
        # SaveScreenToPNG stamps the name with a timestamp; take the newest match.
        shots_dir = arm / "host" / "runtime" / "ScreenShots"
        found = sorted(shots_dir.glob(f"lobby_chat_{res_tag}*.png"), key=lambda p: p.stat().st_mtime) \
            if shots_dir.exists() else []
        shot = found[-1] if found else shots_dir / f"lobby_chat_{res_tag}.png"
        result["details"][res_tag] = {
            "host_exit": records.get("host", {}).get("exit_code"),
            "screenshot": str(shot), "screenshot_exists": shot.exists(),
            "label_assert": [l for l in host_log.splitlines() if "LabelLobbyChat" in l],
            "chat_sends": [l for l in host_log.splitlines() if "[menu-script] chat" in l],
        }
        result["checks"][f"{res_tag}_exit"] = records.get("host", {}).get("exit_code") == 0
        result["checks"][f"{res_tag}_shot"] = shot.exists() and shot.stat().st_size > 0
        result["checks"][f"{res_tag}_label"] = any("PASS" in l for l in result["details"][res_tag]["label_assert"])
    result["pass"] = all(result["checks"].values())
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=48051)
    parser.add_argument("--ticks", type=int, default=300)
    parser.add_argument("--shots", action="store_true",
                        help="run only the menu-script screenshot arms (lobby at 640x360 and 960x540)")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "checks": {}, "details": {}}

    try:
        if options.shots:
            result = run_shots(options.repo, root, options.port)
            (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            print(json.dumps({"pass": result["pass"], "failed": [k for k, v in result["checks"].items() if not v],
                              "out": str(root)}), flush=True)
            return 0 if result["pass"] else 1
        chat = run_pair(options.repo, root / "chat", options.port, options.ticks,
                        {"host": HOST_SCRIPT, "client": CLIENT_SCRIPT})
        control = run_pair(options.repo, root / "control", options.port + 2, options.ticks, {})

        for who in ("host", "client"):
            result["details"][f"{who}_exit"] = chat["records"][who].get("exit_code")
            result["checks"][f"{who}_exit"] = chat["records"][who].get("exit_code") == 0
            result["details"][f"{who}_chat_lines"] = chat_lines(chat["logs"][who])
            result["details"][f"{who}_sends"] = send_lines(chat["logs"][who])
            result["checks"][f"{who}_chat_lines"] = result["details"][f"{who}_chat_lines"] == EXPECTED[who]
        result["checks"]["host_sends_ok"] = all(ok == 1 for _, _, ok, _ in result["details"]["host_sends"]) \
            and len(result["details"]["host_sends"]) == 2
        result["checks"]["client_sends_ok"] = all(ok == 1 for _, _, ok, _ in result["details"]["client_sends"]) \
            and len(result["details"]["client_sends"]) == 1
        # The negative half of team scope lives on the string level too: no leak anywhere in the log.
        result["checks"]["team_line_never_on_client"] = "team only" not in chat["logs"]["client"]
        result["checks"]["control_no_chat"] = all("[chat]" not in log for log in control["logs"].values())
        result["checks"]["control_exits"] = all(control["records"][who].get("exit_code") == 0
                                                for who in ("host", "client"))

        for who in ("host", "client"):
            chat_trace = root / "chat" / f"{who}_trace.json"
            control_trace = root / "control" / f"{who}_trace.json"
            if chat_trace.exists() and control_trace.exists():
                ok, detail = strict_compare(str(chat_trace), str(control_trace), expected_ticks=options.ticks)
            else:
                ok, detail = False, {"reasons": ["missing trace"]}
            result["details"][f"{who}_vs_control"] = detail
            result["checks"][f"{who}_trace_matches_control"] = ok
        ht, ct = root / "chat" / "host_trace.json", root / "chat" / "client_trace.json"
        if ht.exists() and ct.exists():
            ok, detail = strict_compare(str(ht), str(ct), expected_ticks=options.ticks)
        else:
            ok, detail = False, {"reasons": ["missing traces"]}
        result["details"]["host_vs_client_trace"] = detail
        result["checks"]["peers_trace_identical"] = ok

        # Refusal arm: the chat script is session traffic - outside the headless match it is refused.
        refusal_out = root / "refusal"
        refusal_run = make_run(options.repo, ["-net-chat-script", root / "chat" / "host_chat.txt"], refusal_out, 60)
        try:
            refusal_record = refusal_run.start().finish()
        finally:
            refusal_run.close()
        refusal_log = read_log(refusal_out)
        result["details"]["refusal"] = {"exit": refusal_record.get("exit_code"),
                                        "lines": [l for l in refusal_log.splitlines() if "chat" in l.lower()][:4]}
        result["checks"]["refused_without_e2e"] = refusal_record.get("exit_code") not in (0, None)

        result["pass"] = all(result["checks"].values())
    except Exception as error:  # noqa: BLE001
        result["error"] = str(error)
    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [k for k, v in result["checks"].items() if not v], "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
