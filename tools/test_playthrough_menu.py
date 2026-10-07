"""Run one menu regression through the normal headless engine runner."""
import argparse
import json
import os
from pathlib import Path

from run_sim_test import make_run, seed_settings

LANDING = "wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nassert_substate Landing\n"
SETUP = LANDING + "activate ButtonMultiplayerHostGame\nwait 5\ncombo_select ComboHostPlayers 4\nwait 4\n"
ADVANCED = SETUP + "activate ButtonHostOptions\nwait 5\n"
LOBBY = SETUP + "activate ButtonMultiplayerCreate\nwait_state Starting 60\nwait 20\nassert_substate Lobby\n"

CASES = {
    "capacity": (ADVANCED + "activate TabHostPageTiming\nwait 4\nassert_label LabelHostNetMode capacity 4\nassert_label LabelHostNetMode Player host\n", {}),
    "footer-revert": (ADVANCED + "activate TabHostPageConnection\nwait 4\nsettext TextHostNetPort 41011\nwait 4\nassert_enabled ButtonHostOptApply 1\nsettext TextHostNetPort 41010\nwait 4\nassert_enabled ButtonHostOptApply 0\nassert_label_absent LabelHostOptStatus Changes ready\n", {}),
    "footer-dirty": (ADVANCED + "activate TabHostPageConnection\nwait 4\nsettext TextHostNetPort 41011\nwait 4\nactivate ButtonHostOptApply\nwait 4\nsettext TextHostNetPort 41012\nwait 4\nassert_enabled ButtonHostOptApply 1\nassert_label_absent LabelHostOptStatus Applied\n", {}),
    "footer-live": (LOBBY + "activate ButtonLobbyOptions\nwait 4\nactivate TabHostPageSeats\nwait 4\nassert_enabled ButtonHostOptApply 0\nassert_label_absent LabelHostOptStatus Changes ready\n", {}),
    "scope": (LOBBY + "activate ButtonLobbyOptions\nwait 4\nactivate TabHostPageSession\nwait 4\nassert_label LabelHostSessNote this lobby\n", {}),
    "empty-seat": (LOBBY + "activate ButtonLobbyOptions\nwait 4\nactivate TabHostPageSeats\nwait 4\nactivate ButtonHostSeatDetails1\nwait 4\nassert_enabled ButtonHostSeatDlgKick 0\nassert_enabled ButtonHostSeatDlgBan 0\nassert_label LabelHostSeatDlgActionHint No player holds this seat\n", {}),
    "lobby-count": (LOBBY + "activate ButtonLobbyOptions\nwait 4\nactivate TabHostPageSession\nwait 4\nassert_label LabelHostSessSeats 1 connected\nassert_label LabelHostSessSeats 4 configured\n", {}),
    "description": (SETUP + "assert_label LabelHostActivityAbout human players\nassert_text_fits LabelHostActivityAbout\nassert_no_overlap LabelHostActivityAbout LabelHostScene\nassert_rect_inside ButtonMultiplayerCreate MultiplayerHostPanel\nassert_inside_screen MultiplayerHostPanel\n", {}),
    "relay-copy": (ADVANCED + "activate TabHostPageConnection\nwait 4\nassert_label LabelHostNetIce Automatic connection\nassert_label LabelHostNetIceHint Relay only\nassert_label_absent LabelHostNetIceHint connects players directly\nassert_label LabelHostNetRelay Match relay\nassert_label LabelHostRelayHint Relay only\n", {"NetworkConnectionMode": "RelayOnly"}),
    "deadzone": ("wait 40\nactivate ButtonMainToOptions\nwait 5\nactivate TabInputSettings\nwait 4\nactivate ButtonP3Clear\nwait 3\nactivate ButtonP3Clear\nwait 3\nactivate ButtonP3NextDevice\nwait 3\nactivate ButtonP3NextDevice\nwait 3\nassert_visible LabelP3DeadzoneType 1\nassert_no_overlap SliderP3Sensitivity LabelP3DeadzoneType\n", {}),
    "joining": (LANDING + "activate ButtonMultiplayerJoinGame\nwait 4\nactivate ButtonJoinByAddress\nwait 4\nsettext TextJoinAddress 127.0.0.1\nsettext TextJoinPort 47991\nactivate ButtonJoinAddressGo\nwait 4\nassert_substate JoinSetup\nassert_label ButtonJoinBack Cancel\nactivate ButtonJoinBack\nwait 4\nwait_state Idle 60\nassert_label ButtonJoinBack Back\nactivate ButtonJoinBack\nwait 4\nassert_substate Landing\n", {}),
    "mapping-wheel": ("wait 40\nactivate ButtonMainToOptions\nwait 5\nactivate TabInputSettings\nwait 4\nactivate ButtonP1Config\nwait 4\nassert_value ScrollbarScrollingMappingBox 0\nwheel_control LabelInputName1 -1\nwait 4\nassert_value ScrollbarScrollingMappingBox 10\nwheel_control ButtonInputKey1 -1\nwait 4\nassert_value ScrollbarScrollingMappingBox 20\n", {}),
    "saved-empty": ("wait 40\nactivate ButtonSaveOrLoadGame\nwait 5\nassert_label DescriptionLabel No saved games yet\nassert_enabled ButtonLoad 0\nassert_enabled ButtonDelete 0\n", {}),
    "browser-activity": (LANDING + "activate ButtonMultiplayerJoinGame\nwait 5\nassert_game_row_activity ListLanGames\n", {}),
    "browser-mods": (LANDING + "activate ButtonMultiplayerJoinGame\nwait 5\nassert_game_row_refusal\n", {}),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()
    os.environ["CC_RUNNER_IGNORE_FULLSCREEN"] = "1"
    script, settings = CASES[options.case]
    script_path = options.out.resolve().parent / (options.out.name + ".txt")
    script_path.parent.mkdir(parents=True, exist_ok=True)
    script_path.write_text(script + "dump_host_options\nexit\n", encoding="utf-8")
    run = make_run(options.repo, ["-menu-script", script_path], options.out, timeout=300,
                   env={"CCCP_HEADLESS": "1", "CC_RUNNER_IGNORE_FULLSCREEN": "1"})
    seed_settings(run, settings)
    try:
        record = run.start().finish()
    finally:
        run.close()
    print(json.dumps({key: record.get(key) for key in ("exit_code", "timed_out", "verdict_lines")}, indent=2))
    return 0 if record["exit_code"] == 0 and record["evidence_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
