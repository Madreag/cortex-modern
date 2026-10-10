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
        "wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
        "settext TextMultiplayerName Captain", "activate ButtonMultiplayerHostGame",
        "wait_ms 500", "assert_substate HostSetup",
        "combo_select ComboHostActivity Skirmish Defense", "combo_select ComboHostScene Grasslands",
        "setup_host_port {PORT}", f"combo_select ComboHostPlayers {players}",
        "activate ButtonHostOptions", "wait_ms 500", "assert_substate HostOptions", "activate TabHostPageConnection",
        "combo_select ComboHostNetVisibility Public (default)",
        "combo_select ComboHostNetRelay Game service (default)",
    ]
    if cpu:
        lines += ["activate TabHostPageSeats", "combo_select ComboHostSeatType2 CPU",
                  "combo_select ComboHostSeatType3 CPU", "activate TabHostPageConnection"]
    lines += ["assert_label ComboHostNetVisibility Public (default)",
              "assert_label ComboHostNetRelay Game service (default)"]
    if cpu:
        lines += ["activate ButtonHostOptApply", "wait_ms 400"]
    lines += ["activate ButtonHostOptBack", "wait_ms 500",
              "activate ButtonMultiplayerCreate", "wait_substate Lobby 90",
              "video_mark fight15-host-listening", "wait_connected 2 90", "wait_ms 1200"]
    return lines


def join_setup():
    return ["wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
            "settext TextMultiplayerName Joiner", "activate ButtonMultiplayerJoinGame",
            "wait_ms 600", "assert_substate JoinSetup",
            "wait_row PublicGameRowPort{PORT} 90", "click_row PublicGameRowPort{PORT}",
            "assert_label LabelJoinSelected internet",
            "assert_enabled ButtonMultiplayerConnect 1", "activate ButtonMultiplayerConnect",
            "wait_connected 2 90", "wait_substate Lobby 90", "wait_ms 1200"]


def watches(prefix, text, state="substate:Lobby"):
    text = "json:" + json.dumps(text) if "\n" in text else text
    return [f"text_watch start {prefix}-state equals {state} LabelMultiplayerStatus {text}",
            f"text_watch start {prefix}-layout layout {state}",
            f"text_watch start {prefix}-duplicates duplicates {state}",
            f"text_watch start {prefix}-persistent shown {state} LabelMultiplayerStatus"]


def assert_watches(prefix):
    return [f"text_watch assert {prefix}-{kind}" for kind in ("state", "layout", "duplicates", "persistent")]


def watch_pass(name):
    return rf'text_watch .*"watch"\s*:\s*"{name}".*PASS'


def lobby_wait(peer):
    text = "Waiting for Joiner to press Ready" if peer == "host" else "Press Ready when you're ready to play"
    return wait(scope="menu", screen="MultiplayerScreen", control="LabelMultiplayerStatus", text_contains=text)


def probe(steps, timeout=180000):
    steps = [part for step in steps for part in
             ([dict(op="signal", name="done", scope="menu"), step] if step["op"] == "finish" else [step])]
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
    peer = "host" if mark.endswith("host") else "joiner"
    return probe([lobby_wait(peer), wait(elapsed_ms=18000, scope="menu"),
                  dict(op="assert_relay", scope="menu"),
                  menu(f"video_mark {mark}"), wait(elapsed_ms=1200, scope="menu"),
                  dict(op="screenshot_pair", name=mark, scope="menu"), dict(op="finish")])


def lobby_scenes():
    host, joiner = host_setup(), join_setup()
    host += watches("lobby-host", "Waiting for Joiner to press Ready")
    joiner += watches("lobby-joiner", "Press Ready when you're ready to play")
    for lines, peer in ((host, "host"), (joiner, "joiner")):
        lines += [f"video_mark lobby-{peer}", "assert_label LabelLobbyPlayer0 Captain", "assert_label LabelLobbyPlayer1 Joiner",
                  "assert_text_fits LabelLobbyPlayer0", "assert_text_fits LabelLobbyPlayer1",
                  "assert_no_overlap LabelLobbyPlayer0 LabelLobbyPlayer1",
                  f"chat all {peer} answers", "wait_ms 1200"]
    host += ["wait_label LabelLobbyChatAny joiner answers"] + assert_watches("lobby-host")
    host += ["wait_ms 2000", "activate ButtonLobbyOptions",
             "activate TabHostPageSeats", "video_mark seats-host", "assert_visible CollectionBoxHostPageSeats 1",
             "assert_text_fits LabelHostOptionsTitle", "wait_ms 1400",
             "activate ButtonHostOptBack", "wait_ms 1400"]
    joiner += ["wait_label LabelLobbyChatAny host answers"] + assert_watches("lobby-joiner")
    joiner += ["activate ButtonLobbyEditSetup",
               "activate TabHostPageSeats", "video_mark seats-joiner", "assert_visible CollectionBoxHostPageSeats 1",
               "assert_enabled ButtonHostOptApply 0", "wait_ms 1400",
               "activate ButtonHostOptBack", "wait_ms 1400"]
    for lines in (host, joiner):
        lines += ["wait_file {PROBE_DIR}/done.json 120", "wait_ms 1400", "exit"]
    checks = [dict(id=f"lobby-{peer}", peer=peer, mark=f"lobby-{peer}", screen="MultiplayerScreen",
                   what="Names, exact waiting line, no duplicates, persistent state and panel bounds; both players answer.",
                   events=["assert_label LabelLobbyPlayer0.*PASS", "assert_label LabelLobbyPlayer1.*PASS",
                           *(watch_pass(f"lobby-{peer}-{kind}") for kind in ("state", "layout", "duplicates", "persistent"))])
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
    joiner += watches("ready-joiner", "You're ready\nwaiting for the host to start the match")
    for lines, peer in ((host, "host"), (joiner, "joiner")):
        lines += [f"video_mark ready-after-{peer}", "wait_ms 1600"] + assert_watches(f"ready-{peer}")
        lines += ["wait_file {PROBE_DIR}/done.json 120", "wait_ms 1400", "exit"]
    host.insert(-3, "assert_enabled ButtonMultiplayerStart 1")
    checks = [dict(id=f"ready-{peer}", peer=peer, mark=f"ready-after-{peer}", screen="MultiplayerScreen",
                   what="A held Ready click changes the exact state and it stays drawn inside the lobby.",
                   events=[watch_pass(f"ready-{peer}-{kind}") for kind in ("state", "layout", "duplicates", "persistent")])
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
                          [watch_pass(f"{'countdown-cancel' if peer == 'host' else 'countdown-joiner-cancel'}-state")])
              for state in ("30", "cancel") for peer in ("host", "joiner")]
    emit(3, "Unready Start counts down from thirty seconds and cancels", host, joiner,
         lobby_probe("countdown-relay-host"), lobby_probe("countdown-relay-joiner"), checks)


def key(name):
    return [dict(op="key_down", key=name), wait(renders=3, sim_advanced=2), dict(op="key_up", key=name), wait(renders=3)]


def click(button="left"):
    return [dict(op="game_mouse", button=button, down=True), wait(renders=3, sim_advanced=2),
            dict(op="game_mouse", button=button, down=False), wait(renders=3)]


def capture(mark):
    return [menu(f"video_mark {mark}"), wait(elapsed_ms=1200, scope="menu"),
            dict(op="screenshot_pair", name=mark, scope="menu")]


def game_menu(peer, timeout=360):
    lines = host_setup() if peer == "host" else join_setup()
    if peer == "host":
        lines += ["wait_remote_ready 90", "activate ButtonMultiplayerStart"]
    else:
        lines += ["activate ButtonMultiplayerReady"]
    lines += [f"wait_file {{PROBE_DIR}}/done.json {timeout}", "wait_ms 1400", "exit"]
    return lines


def game_probe(steps, timeout=360000):
    document = json.loads(probe(steps, timeout))
    document.update(activate_phase="Running", sim_clock="lockstep")
    return json.dumps(document, indent=2) + "\n"


def place(peer, occupied=False, x=None, relative_to=None):
    fraction = x if x is not None else (0.35 if peer == "host" else 0.65)
    steps = [wait(editing=True), dict(op="assert_relay"), dict(op="editor_pick", input_player=0),
             dict(op="editor_move", input_player=0, x_fraction=fraction), wait(renders=5, elapsed_ms=1000)]
    if relative_to:
        # The prefab's brain chamber is (48, 48) from its top-left, (96, 96) from its cursor.
        steps[3].update(relative_to=relative_to, offset_x=-48, offset_y=-48)
    if peer == "joiner": steps += [wait(setup_ready=1)]
    if peer == "joiner" and occupied:
        steps += [wait(setup_ready=1), dict(op="editor_move", input_player=0, x_fraction=0.35)]
        steps += capture("occupied-preview")
        steps += [dict(op="assert_editor", input_player=0, screen_text_contains="Occupied by Captain's brain - choose another spot")]
        steps += click()
        steps += [dict(op="assert_editor", input_player=0, equals=dict(ready=False, submitted=False)),
                  dict(op="editor_move", input_player=0, x_fraction=fraction), wait(renders=5, elapsed_ms=1000)]
    steps += capture(f"valid-preview-{peer}")
    steps += [dict(op="assert_editor", input_player=0, screen_text_contains="Valid spot - click to INSTALL your brain"),
              dict(op="assert_control", control="LabelNetMatchStatus", text_contains="0 of 2" if peer == "host" else "1 of 2", fits=True, inside="BoxNetMatchStatus")]
    steps += click()
    if peer == "host":
        steps += [wait(setup_ready=1)] + capture("placed-host")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(ready=True, submitted=True)),
                  dict(op="assert_control", control="LabelNetMatchStatus", text_contains="1 of 2", fits=True, inside="BoxNetMatchStatus")]
    steps += key("Return")
    steps += [wait(editing=False, paused=False), dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2))]
    steps += capture(f"playing-{peer}")
    return steps


def scene_checks(peer, steps):
    checks = []
    mark, start = None, 0
    for index, step in enumerate(steps + [menu("video_mark scene-end")]):
        if step.get("op") == "menu" and step.get("command", "").startswith("video_mark "):
            if mark is not None:
                indices = [i for i in range(start, index)
                           if steps[i]["op"].startswith("assert") or
                           (steps[i]["op"] == "menu" and steps[i].get("command", "").startswith(("assert_", "text_watch assert ")))]
                if indices:
                    checks.append(dict(id=f"{peer}-{mark}", peer=peer, mark=mark, screen="game",
                                       what="Captured real scene state, exact label text, panel bounds and the live player.", probe_steps=indices))
            mark, start = step["command"].split(" ", 1)[1], index
    return checks


def buy(peer, fire=True):
    steps = [dict(op="game_mouse", button="right", down=True), wait(renders=3, sim_advanced=2),
             dict(op="pie_point", input_player=0, command=6), wait(renders=3, sim_advanced=2)]
    steps += capture(f"pie-buy-{peer}")
    steps += [dict(op="assert_pie", input_player=0, equals=dict(visible=True, description="Buy Menu - click to open"))]
    steps += click()
    steps += [dict(op="game_mouse", button="right", down=False), wait(renders=5, elapsed_ms=1200)]
    steps += capture(f"shop-{peer}")
    steps += [dict(op="assert_buy", input_player=0, equals=dict(visible=True, enabled=True, buy_allowed=True)),
              menu("activate OrderClearButton", scope="buy", input_player=0),
              menu("activate CraftTab", scope="buy", input_player=0), dict(op="shop_pick", input_player=0, preset="Base.rte/Rocket MK2"),
              menu("activate BodiesTab", scope="buy", input_player=0), dict(op="shop_pick", input_player=0, preset="Coalition.rte/Soldier Light"),
              menu("activate GunsTab", scope="buy", input_player=0), dict(op="shop_pick", input_player=0, preset="Coalition.rte/Assault Rifle")]
    steps += capture(f"order-{peer}")
    steps += [dict(op="assert_buy", input_player=0, equals=dict(cart=["Coalition.rte/Soldier Light", "Coalition.rte/Assault Rifle"], craft="Base.rte/Rocket MK2", passengers=1), remember="order"),
              dict(op="assert_control", scope="buy", input_player=0, control="BuyButton", equals=dict(visible=True, enabled=True), fits=True, inside="BuyGUIBox"),
              menu("activate BuyButton", scope="buy", input_player=0),
              dict(op="wait_scene", input_player=0, funds_delta_from="order"),
              dict(op="assert_scene", input_player=0, funds_delta_from="order"),
              dict(op="wait_scene", input_player=0, delivered="Coalition.rte/Soldier Light")]
    steps += capture(f"arrival-{peer}")
    steps += [dict(op="assert_scene", input_player=0, delivered="Coalition.rte/Soldier Light", equals=dict(alive=True))]
    steps += key("E")
    steps += [dict(op="wait_scene", input_player=0, equals=dict(preset="Coalition.rte/Soldier Light", weapon="Coalition.rte/Assault Rifle"))]
    if fire:
        steps += [dict(op="game_mouse", down=True), wait(renders=5, sim_advanced=12)]
        steps += capture(f"fire-{peer}")
        steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, fired=True, weapon="Coalition.rte/Assault Rifle")), dict(op="game_mouse", down=False)]
    return steps


def pause_end():
    return key("Escape") + [wait(screen="Pause"), menu("activate ButtonEndMatch"),
                              wait(screen="PauseLeaveConfirm"), menu("activate ButtonLeaveConfirm")]


def game_scenes():
    host, joiner = place("host"), place("joiner", occupied=True)
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="finish")]
    emit(4, "One held brain click, valid preview, shared count and occupied-spot refusal",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))

    host, joiner = place("host") + buy("host"), place("joiner") + buy("joiner")
    for steps in (host, joiner): steps += [dict(op="assert_relay"), dict(op="finish")]
    emit(5, "Brain pie, own funds, soldier and weapon delivery, selection and firing",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))

    host, joiner = place("host", x=0.48) + buy("host", fire=False), place("joiner", x=0.52)
    host += [dict(op="aim_brain", input_player=0, target_player=1)]
    host += [dict(op="game_mouse", down=True), wait(elapsed_ms=12000), dict(op="game_mouse", down=False)]
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [lobby_wait(peer)]
        steps += capture(f"brain-loss-{peer}")
        steps += [menu("assert_label LabelLastMatchSummary Brain"), menu("assert_label LabelLastMatchSummary SkirmishDefense.lua:397"),
                  menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
    checks = scene_checks("host", host) + scene_checks("joiner", joiner)
    checks += [dict(id=f"brain-loss-console-{peer}", peer=peer, mark=f"brain-loss-{peer}", screen="MultiplayerScreen",
                    what="The activity's brain-loss rule, brain identity and tick appear in each peer's end reason.",
                    log_regex=[r"\[net-match\] brain lost: Joiner's Brain Case .* at tick ",
                               r"\[net-match\] activity end: Skirmish Defense ended at tick .*SkirmishDefense.lua:397.*Brain Case"])
               for peer in ("host", "joiner")]
    emit(6, "Actual weapon damage ends by the activity's brain-loss rule on both peers",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner), checks)

    host, joiner = place("host"), place("joiner")
    host += key("F6") + [wait(panel_open=True), wait(held_peer="Joiner")]
    host += capture("left-seat-held") + [dict(op="assert", equals=dict(service="Running", local_actor_alive=True), held_peer="Joiner"),
            dict(op="assert_control", control="NetworkSeatDetail@Joiner", text_contains="AI", fits=True, inside="NetworkSeats"),
            wait(returned_peer="Joiner")]
    host += capture("rejoined-host") + [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="assert_relay"), dict(op="finish")]
    joiner += key("Escape") + [wait(screen="Pause"), menu("activate ButtonLeaveMatch"), wait(screen="PauseLeaveConfirm")]
    joiner += capture("leave-consequence") + [menu("assert_text_fits ButtonLeaveConfirm"), menu("activate ButtonLeaveConfirm"),
               wait(screen="MultiplayerScreen", scope="menu"), menu("activate ButtonMultiplayerReconnect"), wait(service="Running")]
    joiner += capture("rejoined-joiner") + [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="assert_relay"), dict(op="finish")]
    emit(7, "Leave holds the live seat to AI and Rejoin Match returns the same player",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))

    moderation_scene()

    host, joiner = place("host"), place("joiner")
    host += pause_end()
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [lobby_wait(peer)]
        steps += capture(f"rematch-waiting-{peer}")
        expected = "Waiting for Joiner to press Ready" if peer == "host" else "Press Ready when you're ready to play"
        steps += [menu(f"text_watch start rematch-state equals substate:Lobby LabelMultiplayerStatus {expected}"),
                  menu("text_watch start rematch-duplicates duplicates substate:Lobby"),
                  menu("text_watch start rematch-layout layout substate:Lobby"), wait(elapsed_ms=4000, scope="menu"),
                  menu("text_watch assert rematch-state"), menu("text_watch assert rematch-duplicates"), menu("text_watch assert rematch-layout")]
    host += [menu("activate ButtonMultiplayerStart"), wait(scope="menu", control="LabelMultiplayerStatus", text_contains="Starting in 30 s")]
    host += capture("rematch-countdown-host") + [menu("assert_label ButtonMultiplayerStart Cancel Start"), wait(elapsed_ms=2200, scope="menu"),
            menu("activate ButtonMultiplayerStart"), wait(elapsed_ms=1200, scope="menu"), menu("assert_label LabelMultiplayerStatus Waiting for Joiner to press Ready"), dict(op="finish")]
    joiner += [wait(scope="menu", control="LabelMultiplayerStatus", text_contains="The host is starting the match in 30 s")]
    joiner += capture("rematch-countdown-joiner") + [menu("assert_text_fits LabelMultiplayerStatus"), wait(elapsed_ms=4000, scope="menu"),
              menu("assert_label LabelMultiplayerStatus Press Ready when you're ready to play"), dict(op="finish")]
    emit(9, "Rematch waiting names, no premature countdown, thirty-second Start and Cancel",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))


def ai_fight():
    host_menu, join_menu = host_setup(4, cpu=True), join_setup()
    # These are stock activity options: Infinite gold selects Skirmish Defense's endless AI mode.
    insert = host_menu.index("activate ButtonHostOptApply")
    host_menu[insert:insert] = ["activate TabHostPageRules", "slider_set SliderHostRulesDifficulty 0",
                              "slider_set SliderHostRulesGold 31000", "setcheck CheckHostRulesClearPath 0"]
    host_menu += ["wait_remote_ready 90", "activate ButtonMultiplayerStart", "wait_file {PROBE_DIR}/done.json 1600", "wait_ms 1400", "exit"]
    join_menu += ["activate ButtonMultiplayerReady", "wait_file {PROBE_DIR}/done.json 1600", "wait_ms 1400", "exit"]
    probes, checks = {}, []
    for peer, fraction in (("host", 0.3), ("joiner", 0.7)):
        steps = [wait(editing=True), dict(op="editor_pick", input_player=0, brain=False, **{"class": "BunkerAssembly"}, preset="Brain Chamber Left 2x2 - 1"),
                 dict(op="editor_move", input_player=0, x_fraction=fraction, remember="bunker")] + click()
        steps += place(peer, x=fraction, relative_to="bunker")
        for minute in range(1, 21):
            steps += [dict(op="measure_minute")]
            steps += key("CHAT") + [wait(chat_entry_open=True), menu(f"type_text TextMatchChatInput enter {peer} answers minute {minute}"),
                                    wait(elapsed_ms=1500)]
            steps += capture(f"minute-{minute:02d}-{peer}")
            steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="assert_relay")]
        if peer == "host": steps += pause_end()
        steps += [lobby_wait(peer)]
        steps += capture(f"fight-ended-{peer}") + [menu("assert_label LabelLastMatchSummary host"), dict(op="finish")]
        probes[peer] = game_probe(steps, timeout=1600000)
        checks += scene_checks(peer, steps)
        checks.append(dict(id=f"twenty-minutes-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="MultiplayerScreen",
                           what="Twenty full running minutes at >=58 ticks/s, living local player, AI weapon fire and named end reason.",
                           log_regex=[r"\[fight15-scene\] minute=20 pace=", r"\[net-match\].*host"]))
    emit(10, "Twenty-minute stock endless AI fight with scripted bunker and brain setup",
         host_menu, join_menu, probes["host"], probes["joiner"], checks, timeout=1750)


def moderation_scene():
    document, runs, scripts, checks = None, [], {}, []
    for action, part, reason in (("kick", "Remove", "removed"), ("ban", "Ban", "banned")):
        host, joiner = place("host"), place("joiner")
        button = f"NetworkSeat{part}@Joiner"
        host += key("F6") + [wait(panel_open=True), menu(f"activate {button}")]
        host += capture(f"confirm-{action}") + [dict(op="assert_control", control=button, text_contains=f"Confirm: {'remove' if action == 'kick' else 'ban'} Joiner", fits=True),
                menu(f"activate {button}"), wait(elapsed_ms=25000), dict(op="assert_scene", input_player=0, equals=dict(alive=True)), dict(op="finish")]
        joiner += [wait(screen="MultiplayerScreen", scope="menu")]
        joiner += capture(f"{action}-joiner") + [menu(f"assert_label LabelJoinSelected The host {reason} you from this session")]
        if action == "ban":
            joiner += [menu("activate ButtonMultiplayerJoinGame"), dict(op="wait_public_row", scope="menu", name="PublicGameRowPort{PORT}"),
                       menu("click_row PublicGameRowPort{PORT}"), menu("activate ButtonMultiplayerConnect"), wait(elapsed_ms=12000, scope="menu")]
            joiner += capture("banned-rejoin-refused") + [menu("assert_label LabelJoinSelected The host banned you from this session"), menu("assert_text_fits LabelJoinSelected")]
        joiner += [dict(op="finish")]
        emit(8, "Held kick and ban clicks and public-list refused readmission",
             game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner), [])
        path = ROOT / "fight15-s08.json"
        current = json.loads(path.read_text(encoding="utf-8"))
        if document is None: document = current
        peers = current.pop("peers")
        for peer in peers:
            for field in ("menu_script", "probe"):
                source = peer[field]; target = f"{action}-{source}"
                scripts[target] = current["scripts"][source]; peer[field] = target
        runs.append(dict(name=action, peers=peers))
        for check in scene_checks("host", host) + scene_checks("joiner", joiner):
            check["run"] = action; check["id"] = f"{action}-{check['id']}"; checks.append(check)
    document.pop("peers", None)
    document.update(runs=runs, scripts=scripts, checklist=checks)
    (ROOT / "fight15-s08.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    lobby_scenes()
    game_scenes()
    ai_fight()
