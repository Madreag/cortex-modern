"""Focused rendered UI detectors. One host-only fixture, one engine at a time; this is not multiplayer proof."""
import argparse
import json
from pathlib import Path
import subprocess

from run_sim_test import make_run, seed_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=("chat", "menus"), required=True)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    args.repo, args.out = args.repo.resolve(), args.out.resolve()
    inputs = args.out.parent / (args.out.name + "_inputs")
    inputs.mkdir(parents=True, exist_ok=False)
    steps = [{"op": "wait", "service": "Running", "lockstep_frame_at_least": 100}]

    def key(name, menu=False):
        scope = {"scope": "menu"} if menu else {}
        steps.extend([{"op": "key_down", "key": name, **scope}, {"op": "key_up", "key": name, **scope}])

    def shot(name, menu=False):
        steps.append({"op": "wait", "renders": 4, **({"scope": "menu"} if menu else {})})
        steps.append({"op": "screenshot_pair", "name": name, **({"scope": "menu"} if menu else {})})

    def panel_click(control):
        steps.extend([{"op": "mouse_down", "control": control}, {"op": "wait", "renders": 3},
                      {"op": "mouse_up", "control": control}, {"op": "wait", "renders": 4}])

    if args.case == "chat":
        key("CHAT")
        steps.append({"op": "wait", "chat_entry_open": True})
        steps.append({"op": "wait", "renders": 4})
        steps.append({"op": "menu", "command": "type_text TextMatchChatInput enter Bring heavy weapons to the left bunker after this wave. Hold fire until everyone arrives; keep gold for a brain. END"})
        steps.append({"op": "wait", "chat_entry_open": False})
        shot("01-sent-chat")
        steps.append({"op": "assert_control", "control": "LabelMatchChatNewest"})
        key("CHAT")
        steps.append({"op": "wait", "chat_entry_open": True})
        steps.append({"op": "wait", "renders": 4})
        steps.append({"op": "menu", "command": "type_text TextMatchChatInput plain draft-kept"})
        shot("02-chat-entry")
        key("Escape")
        shot("03-chat-cancel", True)
        steps.append({"op": "assert_control", "control": "TextMatchChatInput"})
        key("CHAT")
        steps.append({"op": "wait", "chat_entry_open": True})
        steps.append({"op": "wait", "elapsed_ms": 1100})
        for line in range(5):
            steps.append({"op": "send_chat", "text": f"History {line}: " + "W" * 110})
        steps.append({"op": "wait", "renders": 4})
        steps.append({"op": "menu", "command": "type_text TextMatchChatInput enter rate-kept"})
        steps.append({"op": "wait", "control": "LabelMatchChatAudience", "text_contains": "Not sent:"})
        shot("04-rate-refused")
        steps.append({"op": "assert_control", "control": "TextMatchChatInput"})
        steps.append({"op": "assert_control", "control": "LabelMatchChatNewest"})
        steps.append({"op": "menu", "command": "activate ButtonMatchChatOlder"})
        steps.append({"op": "wait", "renders": 4})
        steps.append({"op": "assert_control", "control": "LabelMatchChatNewest"})
        steps.append({"op": "menu", "command": "activate ButtonMatchChatNewer"})
        steps.append({"op": "wait", "renders": 4})
        steps.append({"op": "assert_control", "control": "LabelMatchChatNewest"})
        key("Escape")
    else:
        key("F6")
        steps.append({"op": "wait", "panel_open": True})
        shot("01-players")
        steps.append({"op": "assert_control", "control": "NetworkSeatsRoster"})
        panel_click("NetworkSeatsOptions")
        steps.append({"op": "assert_control", "control": "NetworkSeatsOptionsText", "text_contains": "Team 1", "fits": True})
        shot("03-rules")
        panel_click("NetworkSeatsOptions")
        steps.append({"op": "assert_control", "control": "NetworkSeatsOptionsText", "text_contains": "Input delay:", "fits": True})
        shot("04-connection")
        key("F6")
        steps.append({"op": "wait", "panel_open": False})
        key("Escape")
        steps.append({"op": "wait", "screen": "Pause"})
        steps.append({"op": "wait", "renders": 4})
        steps.append({"op": "menu", "command": "activate ButtonEndMatch"})
        shot("02-end-confirm", True)
        steps.append({"op": "menu", "command": "assert_text_fits ButtonLeaveConfirm", "scope": "menu"})
        steps.append({"op": "assert_control", "control": "LabelLeaveConfirm", "fits": True, "scope": "menu"})
        steps.append({"op": "menu", "command": "activate ButtonLeaveCancel", "scope": "menu"})
        steps.append({"op": "wait", "screen": "Pause", "scope": "menu"})
        key("CHAT", True)
        steps.append({"op": "wait", "renders": 4, "scope": "menu"})
        steps.append({"op": "assert", "name": "pause-keeps-text-focus", "scope": "menu", "equals": {"screen": "Pause"}})
        key("Escape", True)
        steps.append({"op": "wait", "screen": "Gameplay"})
    steps.extend([{"op": "signal", "name": "done", "scope": "menu"}, {"op": "finish", "scope": "menu"}])
    script = {"schema": 1, "activate_phase": "Running", "activation_timeout_ms": 45000,
              "timeout_ms": 45000, "label_dump": {"every_ms": 250}, "steps": steps}
    probe = inputs / "probe.json"
    probe.write_text(json.dumps(script, indent=2), encoding="utf-8")
    menu = inputs / "menu.txt"
    menu.write_text("\n".join(["wait_ms 1980", "activate ButtonMainToMultiplayer", "wait_ms 300",
        "settext TextMultiplayerName Host", "activate ButtonMultiplayerHostGame", "wait_ms 300",
        "combo_select ComboHostActivity P4 Alpha Duel", "wait_ms 300", f"setup_host_port {args.port}",
        "combo_select ComboHostPlayers 2", "activate ButtonMultiplayerCreate", "wait_ms 1200",
        "activate ButtonMultiplayerStart", f"wait_file {(inputs / 'done.json').as_posix()} 65", "wait_ms 300", "exit"]), encoding="utf-8")
    run = make_run(args.repo, ["-menu-script", menu, "-num-lua-states", "4", "-net-match-ticks", "3600"],
                   args.out, timeout=90, env={"CC_TEST_NET_UI_SCRIPT": str(probe.resolve())})
    seed_settings(run, {"ResolutionX": args.width, "ResolutionY": args.height, "NetworkChatDefaultScope": "team", "NetworkIceEnable": 0})
    try:
        record = run.start().finish()
    finally:
        run.close()
    result_path = inputs / "net-ui-result.json"
    result = json.loads(result_path.read_text()) if result_path.is_file() else {}
    observations = result.get("steps", [])
    checks = {}
    if args.case == "chat":
        line = next((row["observed"]["control"]["text"] for row in observations if row["op"] == "assert_control"), "")
        checks["saved_team_audience"] = any(line["scope"] == 1 and line["text"].endswith("END") for row in observations for line in row["observed"].get("chat_history", []))
        checks["valid_message_tail_readable"] = "END" in line
        cancelled = next((row["observed"] for row in observations if row["op"] == "screenshot_pair" and steps[row["index"]].get("name") == "03-chat-cancel"), {})
        checks["escape_only_closes_chat"] = cancelled.get("screen") == "Gameplay" and not cancelled.get("net_ui", {}).get("chat_entry_open", True)
        checks["cancel_keeps_draft"] = any(row["observed"].get("control", {}).get("text") == "draft-kept" for row in observations if row["op"] == "assert_control")
        refused = next((row["observed"] for row in observations if row["op"] == "screenshot_pair" and steps[row["index"]].get("name") == "04-rate-refused"), {})
        checks["refused_send_keeps_entry_and_draft"] = refused.get("net_ui", {}).get("chat_entry_open", False) and any(row["observed"].get("control", {}).get("text") == "rate-kept" for row in observations if row["op"] == "assert_control")
        newest = [row["observed"].get("control", {}).get("text") for row in observations if row["op"] == "assert_control" and steps[row["index"]].get("control") == "LabelMatchChatNewest"]
        checks["history_navigation_reaches_older_and_returns"] = len(newest) == 4 and newest[-3] != newest[-2] and newest[-3] == newest[-1]
    else:
        roster = next((row["observed"]["control"]["text"] for row in observations if row["op"] == "assert_control"), "")
        checks["roster_names_teams"] = "Team 1" in roster
        panel = next((row["observed"].get("net_ui", {}).get("seats_panel", {}) for row in observations if row["op"] == "screenshot_pair" and steps[row["index"]].get("name") == "01-players"), {})
        checks["two_player_panel_is_compact"] = panel.get("h", 344) < 300 and panel.get("w", 600) < 600
        ended = next((row["observed"] for row in observations if row["op"] == "screenshot_pair" and steps[row["index"]].get("name") == "02-end-confirm"), {})
        checks["end_requires_confirmation"] = ended.get("service") == "Running" and ended.get("screen") == "PauseLeaveConfirm"
        rules = [row["observed"].get("control", {}).get("text", "") for row in observations if row["op"] == "assert_control" and steps[row["index"]].get("control") == "NetworkSeatsOptionsText"]
        checks["rules_and_connection_are_separate"] = len(rules) == 2 and "Team 1" in rules[0] and "Input delay:" not in rules[0] and "Input delay:" in rules[1]
        focus = next((row["observed"] for row in observations if row["op"] == "assert" and steps[row["index"]].get("name") == "pause-keeps-text-focus"), {})
        checks["pause_keeps_text_focus"] = focus.get("screen") == "Pause" and not focus.get("net_ui", {}).get("chat_entry_open", True)
    checks["probe_completed"] = result.get("pass", False) and record.get("exit_code") == 0 and not record.get("timed_out", True)
    receipt = {"topology": "host-only UI fixture", "proof": False, "purpose": __doc__, "checks": checks,
               "head": subprocess.check_output(["git", "-C", str(args.repo), "rev-parse", "HEAD"], text=True).strip(),
               "exe_sha256": record.get("exe_sha256"), "runner_exit": record.get("exit_code")}
    (args.out / "ui-verdict.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
