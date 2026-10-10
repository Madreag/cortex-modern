"""Write the two-player interaction scenes consumed by e2e_video.py.

The same scenes run locally as detectors and on assigned boxes as relay proofs.
All input belongs to MenuAutomation or the engine probe; there is no desktop input.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "e2e"
SETTINGS = {
    "SessionDirectoryUrl": "directory.broserver.com",
    "NetworkHostRelayMode": "Directory",
    "NetworkConnectionMode": "RelayOnly",
    "NetworkIceEnable": 1,
    "NetworkPortMapEnable": 0,
    "NetworkMatchStatusMode": "Always",
}


def menu(command, **kwargs):
    return dict(op="menu", command=command, **kwargs)


def wait(**kwargs):
    return dict(op="wait", **kwargs)


def host_setup(players=2, cpu=False):
    lines = [
        "wait_ms 1800", "activate ButtonMainToMultiplayer",
        "settext TextMultiplayerName Captain", "activate ButtonMultiplayerHostGame",
        "combo_select ComboHostActivity Skirmish Defense", "combo_select ComboHostScene Grasslands",
        "setup_host_port {PORT}", f"combo_select ComboHostPlayers {players}",
        "activate ButtonHostOptions", "activate TabHostPageConnection",
        "combo_select ComboHostNetVisibility Public (default)",
        "combo_select ComboHostNetRelay Game service (default)",
    ]
    if cpu:
        lines += ["activate TabHostPageSeats", "combo_select ComboHostSeatType2 CPU",
                  "combo_select ComboHostSeatType3 CPU"]
    lines += ["activate ButtonHostOptApply", "activate ButtonHostOptBack",
              "activate ButtonMultiplayerCreate", "wait_substate Lobby 90",
              "video_mark fight15-host-listening", "wait_connected 2 90", "wait_ms 1200"]
    return lines


def join_setup():
    return ["wait_ms 1800", "activate ButtonMainToMultiplayer",
            "settext TextMultiplayerName Joiner", "activate ButtonMultiplayerJoinGame",
            "wait_row GameRowPort{PORT} 90", "click_row GameRowPort{PORT}",
            "assert_enabled ButtonMultiplayerConnect 1", "activate ButtonMultiplayerConnect",
            "wait_connected 2 90", "wait_substate Lobby 90", "wait_ms 1200"]


def watches(prefix, text, state="substate:Lobby"):
    return [f"text_watch start {prefix}-state equals {state} LabelMultiplayerStatus {text}",
            f"text_watch start {prefix}-layout layout {state}",
            f"text_watch start {prefix}-duplicates duplicates {state}",
            f"text_watch start {prefix}-persistent shown {state} LabelMultiplayerStatus"]


def assert_watches(prefix):
    return [f"text_watch assert {prefix}-{kind}" for kind in ("state", "layout", "duplicates", "persistent")]


def probe(steps, timeout=180000):
    return json.dumps(dict(schema=1, timeout_ms=timeout, steps=steps), indent=2) + "\n"


def emit(number, title, host, joiner, host_probe, join_probe, checklist, timeout=220):
    name = f"fight15-s{number:02d}"
    scripts = {"host.menu": "\n".join(host) + "\n", "joiner.menu": "\n".join(joiner) + "\n",
               "host.probe": host_probe, "joiner.probe": join_probe}
    peers = [dict(name=peer, menu_script=f"{peer}.menu", probe=f"{peer}.probe",
                  settings=dict(SETTINGS, NetworkDisplayName="Captain" if peer == "host" else "Joiner"))
             for peer in ("host", "joiner")]
    document = dict(schema=1, name=name, title=title, port_base=49410 + number * 2,
                    size="960x540", timeout_s=timeout, peers=peers, scripts=scripts, checklist=checklist)
    (ROOT / f"{name}.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def lobby_probe(mark):
    return probe([wait(service="Lobby", scope="menu"), dict(op="assert_relay", scope="menu"),
                  menu(f"video_mark {mark}"), wait(elapsed_ms=1200, scope="menu"),
                  dict(op="screenshot_pair", name=mark, scope="menu"), dict(op="finish")])


def lobby_scenes():
    host, joiner = host_setup(), join_setup()
    host += watches("lobby-host", "Waiting for Joiner to press Ready")
    joiner += watches("lobby-joiner", "Press Ready when you're ready to play")
    for lines, peer in ((host, "host"), (joiner, "joiner")):
        lines += ["assert_label LabelLobbyPlayer0 Captain", "assert_label LabelLobbyPlayer1 Joiner",
                  "assert_text_fits LabelLobbyPlayer0", "assert_text_fits LabelLobbyPlayer1",
                  "assert_no_overlap LabelLobbyPlayer0 LabelLobbyPlayer1", f"video_mark lobby-{peer}",
                  f"chat all {peer} answers", "wait_ms 1200"]
    host += ["wait_label LabelLobbyChatAny joiner answers", "activate ButtonLobbyOptions",
             "activate TabHostPageSeats", "assert_visible CollectionBoxHostPageSeats 1",
             "assert_text_fits LabelHostOptionsTitle", "video_mark seats-host", "wait_ms 1400",
             "activate ButtonHostOptBack", "wait_ms 1400"] + assert_watches("lobby-host")
    joiner += ["wait_label LabelLobbyChatAny host answers", "activate ButtonLobbyEditSetup",
               "activate TabHostPageSeats", "assert_visible CollectionBoxHostPageSeats 1",
               "assert_enabled ButtonHostOptApply 0", "video_mark seats-joiner", "wait_ms 1400",
               "activate ButtonHostOptBack", "wait_ms 1400"] + assert_watches("lobby-joiner")
    for lines in (host, joiner):
        lines += ["wait_file {PROBE_DIR}/done.json 120", "wait_ms 1400", "exit"]
    checks = [dict(id=f"lobby-{peer}", peer=peer, mark=f"lobby-{peer}", screen="MultiplayerScreen",
                   what="Names, exact waiting line, no duplicates, persistent state and panel bounds; both players answer.",
                   events=["assert_label LabelLobbyPlayer0.*PASS", "assert_label LabelLobbyPlayer1.*PASS",
                           f"text_watch assert lobby-{peer}-state .*PASS",
                           f"text_watch assert lobby-{peer}-layout .*PASS",
                           f"text_watch assert lobby-{peer}-duplicates .*PASS",
                           f"text_watch assert lobby-{peer}-persistent .*PASS"])
              for peer in ("host", "joiner")]
    checks += [dict(id=f"seats-{peer}", peer=peer, mark=f"seats-{peer}", screen="MultiplayerScreen",
                    what="The real Seats page is visible on each peer.", events=["assert_visible CollectionBoxHostPageSeats.*PASS"])
               for peer in ("host", "joiner")]
    emit(1, "Public directory lobby and Seats over the relay", host, joiner,
         lobby_probe("relay-host"), lobby_probe("relay-joiner"), checks)

    host, joiner = host_setup(), join_setup()
    host += ["assert_label LabelMultiplayerStatus Waiting for Joiner to press Ready",
             "video_mark ready-before-host", "wait_remote_ready 90", "wait_ms 1200"]
    host += watches("ready-host", "Everyone is ready - press Start Match")
    joiner += ["video_mark ready-before-joiner", "assert_label ButtonMultiplayerReady Ready",
               "wait_ms 2500", "activate ButtonMultiplayerReady", "wait_ms 1200"]
    joiner += watches("ready-joiner", "You're ready - waiting for the host to start the match")
    for lines, peer in ((host, "host"), (joiner, "joiner")):
        lines += [f"video_mark ready-after-{peer}", "wait_ms 1600"] + assert_watches(f"ready-{peer}")
        lines += ["wait_file {PROBE_DIR}/done.json 120", "wait_ms 1400", "exit"]
    host.insert(-3, "assert_enabled ButtonMultiplayerStart 1")
    checks = [dict(id=f"ready-{peer}", peer=peer, mark=f"ready-after-{peer}", screen="MultiplayerScreen",
                   what="A held Ready click changes the exact state and it stays drawn inside the lobby.",
                   events=[f"text_watch assert ready-{peer}-{kind} .*PASS" for kind in ("state", "layout", "duplicates", "persistent")])
              for peer in ("host", "joiner")]
    emit(2, "Joiner Ready and host immediate-start eligibility", host, joiner,
         lobby_probe("ready-relay-host"), lobby_probe("ready-relay-joiner"), checks)

    host, joiner = host_setup(), join_setup()
    host += watches("countdown-before", "Waiting for Joiner to press Ready")
    host += ["video_mark countdown-before-host", "wait_ms 4000"] + assert_watches("countdown-before")
    host += ["activate ButtonMultiplayerStart", "wait_label LabelMultiplayerStatus Starting in 30 s",
             "video_mark countdown-30-host", "assert_label ButtonMultiplayerStart Cancel Start",
             "assert_text_fits LabelMultiplayerStatus", "wait_label LabelMultiplayerStatus Starting in 28 s",
             "video_mark countdown-28-host", "wait_ms 1200", "activate ButtonMultiplayerStart", "wait_ms 1200"]
    host += watches("countdown-cancel", "Waiting for Joiner to press Ready")
    host += ["video_mark countdown-cancel-host", "wait_ms 3000"] + assert_watches("countdown-cancel")
    joiner += ["video_mark countdown-before-joiner", "assert_label LabelMultiplayerStatus Press Ready when you're ready to play",
               "wait_label LabelMultiplayerStatus The host is starting the match in 30 s",
               "video_mark countdown-30-joiner", "assert_text_fits LabelMultiplayerStatus",
               "wait_label LabelMultiplayerStatus The host is starting the match in 28 s",
               "video_mark countdown-28-joiner", "wait_ms 1200",
               "wait_label LabelMultiplayerStatus Press Ready when you're ready to play"]
    joiner += watches("countdown-joiner-cancel", "Press Ready when you're ready to play")
    joiner += ["video_mark countdown-cancel-joiner", "wait_ms 2500"] + assert_watches("countdown-joiner-cancel")
    for lines in (host, joiner):
        lines += ["wait_file {PROBE_DIR}/done.json 120", "wait_ms 1400", "exit"]
    checks = [dict(id=f"countdown-{state}-{peer}", peer=peer, mark=f"countdown-{state}-{peer}", screen="MultiplayerScreen",
                   what="Countdown only follows Start, ticks visibly, and cancels on a later held click.",
                   events=["assert_text_fits LabelMultiplayerStatus .*PASS"] if state == "30" else
                          [f"text_watch assert {'countdown-cancel' if peer == 'host' else 'countdown-joiner-cancel'}-state .*PASS"])
              for state in ("30", "cancel") for peer in ("host", "joiner")]
    emit(3, "Unready Start counts down from thirty seconds and cancels", host, joiner,
         lobby_probe("countdown-relay-host"), lobby_probe("countdown-relay-joiner"), checks)


if __name__ == "__main__":
    lobby_scenes()
