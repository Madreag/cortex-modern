"""Write the two-player interaction scenes consumed by e2e_video.py.

The same scenes run locally as detectors and on assigned boxes as relay proofs.
All input belongs to MenuAutomation or the engine probe; there is no desktop input.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "e2e"
# The scenes main() writes; None writes every scene a function emits.
SELECTED = None
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


def host_setup(players=2, cpu=False, brain_loss=False):
    lines = [
        "wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
        "settext TextMultiplayerName Captain", "activate ButtonMultiplayerHostGame",
        "wait_ms 500", "assert_substate HostSetup",
        "combo_select ComboHostActivity Skirmish Defense", "wait_combo ComboHostScene Grasslands", "combo_select ComboHostScene Grasslands",
        "setup_host_port {PORT}", f"combo_select ComboHostPlayers {players}",
        "activate ButtonHostOptions", "wait_ms 500", "assert_substate HostOptions", "activate TabHostPageConnection",
        "combo_select ComboHostNetVisibility Public (default)",
        "combo_select ComboHostNetRelay Game service (default)",
    ]
    if cpu:
        lines += ["activate TabHostPageSeats", "video_mark cpu-seats-host", "combo_select ComboHostSeatType2 CPU",
                  "combo_select ComboHostSeatType3 CPU",
                  "assert_label ComboHostSeatType0 Open", "assert_label ComboHostSeatType1 Open",
                  "assert_label ComboHostSeatType2 CPU", "assert_label ComboHostSeatType3 CPU",
                  "assert_text_fits LabelHostSeatState2", "assert_text_fits LabelHostSeatState3",
                   "wait_ms 1400", "assert_label LabelHostSeatPlayers Human players",
                   "video_mark cpu-human-count-host", "combo_select ComboHostSeatPlayers 2",
                   "assert_label ComboHostSeatPlayers 2", "assert_label ComboHostSeatType2 CPU",
                   "assert_label ComboHostSeatType3 CPU", "wait_ms 1400", "activate TabHostPageConnection"]
    lines += ["assert_label ComboHostNetVisibility Public (default)",
              "assert_label ComboHostNetRelay Game service (default)"]
    if brain_loss:
        lines += ["activate TabHostPageRules", "combo_select ComboHostRulesBrainless End the match",
                  "assert_label ComboHostRulesBrainless End the match"]
    if cpu or brain_loss:
        lines += ["activate ButtonHostOptApply", "wait_ms 400"]
    lines += ["activate ButtonHostOptBack", "wait_ms 500",
              "activate ButtonMultiplayerCreate", "wait_substate Lobby 90", "write_lobby {PROBE_DIR}/lobby.json 90",
              "video_mark fight15-host-listening", f"wait_connected {players if cpu else 2} 90", "wait_ms 1200"]
    return lines


# The lobby the run's own host listed, by its directory session: another lobby can list on the same port.
OWN_LOBBY = "PublicGameRowLobby:{PROBE_DIR_host}/lobby.json"


def join_own_lobby():
    return ["wait_file {PROBE_DIR_host}/lobby.json 90", f"wait_row {OWN_LOBBY} 90", f"click_row {OWN_LOBBY}"]


def join_setup(connected=2, name="Joiner"):
    return ["wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
            f"settext TextMultiplayerName {name}", "activate ButtonMultiplayerJoinGame",
            "wait_ms 600", "assert_substate JoinSetup",
            *join_own_lobby(),
            "assert_label LabelJoinSelected internet",
            "assert_enabled ButtonMultiplayerConnect 1", "activate ButtonMultiplayerConnect",
            f"wait_connected {connected} 90", "wait_substate Lobby 90", "wait_ms 1200"]


def watches(prefix, text, state="substate:Lobby", rule="equals"):
    text = "json:" + json.dumps(text) if "\n" in text else text
    return [f"text_watch start {prefix}-state {rule} {state} LabelMultiplayerStatus {text}",
            f"text_watch start {prefix}-layout layout {state}",
            f"text_watch start {prefix}-duplicates duplicates {state}",
            f"text_watch start {prefix}-persistent shown {state} LabelMultiplayerStatus"]


def assert_watches(prefix):
    return [f"text_watch assert {prefix}-{kind}" for kind in ("state", "layout", "duplicates", "persistent")]


def watch_pass(name):
    return rf'text_watch .*"watch"\s*:\s*"{name}".*PASS'


# The status box's numbers, shown only under Settings > Network > Show network diagnostics.
STATUS_NUMBERS = ["RTT ", "PACE ", " waits "]


def lobby_wait(peer, other="Joiner", within_ms=90000):
    text = f"Waiting for {other} to press Ready" if peer == "host" else "Press Ready when you're ready to play"
    return wait(scope="menu", screen="MultiplayerScreen", control="LabelMultiplayerStatus", text_contains=text, within_ms=within_ms)


def probe(steps, timeout=180000):
    steps = [part for step in steps for part in
             ([dict(op="signal", name="done", scope="menu"), step] if step["op"] == "finish" else [step])]
    return json.dumps(dict(schema=1, timeout_ms=timeout, step_timeout_ms=60000, label_dump=dict(every_ms=5000), steps=steps), indent=2) + "\n"


def emit(number, title, host, joiner, host_probe, join_probe, checklist, timeout=220, matching_log_lines=(), engine_args=(),
         size="960x540", settings=None, peer_settings=None, peer_args=None, peer_env=None, extra=None):
    name = f"fight15-s{number:02d}"
    if SELECTED is not None and name not in SELECTED:
        return
    scripts = {"host.menu": "\n".join(host) + "\n", "joiner.menu": "\n".join(joiner) + "\n",
               "host.probe": host_probe, "joiner.probe": join_probe}
    peers = [dict(name=peer, menu_script=f"{peer}.menu", probe=f"{peer}.probe",
                  env=dict(CCCP_TEST_READBACK_TIMING="1", **(peer_env or {}).get(peer, {})),
                  settings=dict(SETTINGS if settings is None else settings, NetworkDisplayName="Captain" if peer == "host" else "Joiner",
                                **(peer_settings or {}).get(peer, {})))
             for peer in ("host", "joiner")]
    for peer in peers:
        args = list(engine_args) + list((peer_args or {}).get(peer["name"], ()))
        if args:
            peer["args"] = args
    document = dict(schema=1, name=name, title=title, port_base=49410 + number * 2,
                    size=size, timeout_s=timeout, public_directory=True, peers=peers, scripts=scripts, checklist=checklist)
    if matching_log_lines:
        document["matching_log_lines"] = list(matching_log_lines)
    document.update(extra or {})
    (ROOT / f"{name}.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8", newline="\n")


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
             "assert_label LabelMultiplayerStatus Starting in 30 s - waiting for Joiner to press Ready",
             "assert_text_fits LabelMultiplayerStatus"]
    host += watches("countdown-active-host", "Starting in", rule="contains")
    host += ["wait_label LabelMultiplayerStatus Starting in 28 s", "video_mark countdown-28-host",
             "assert_label LabelMultiplayerStatus Starting in 28 s - waiting for Joiner to press Ready", "wait_ms 1200"]
    host += assert_watches("countdown-active-host") + ["activate ButtonMultiplayerStart", "wait_ms 1200"]
    host += watches("countdown-cancel", "Waiting for Joiner to press Ready")
    host += ["video_mark countdown-cancel-host", "wait_ms 3000"] + assert_watches("countdown-cancel")
    joiner += ["video_mark countdown-before-joiner", "assert_label LabelMultiplayerStatus Press Ready when you're ready to play",
               "wait_label LabelMultiplayerStatus The host is starting the match in 30 s",
               "video_mark countdown-30-joiner", "assert_label LabelMultiplayerStatus The host is starting the match in 30 s - press Ready",
               "assert_text_fits LabelMultiplayerStatus"]
    joiner += watches("countdown-active-joiner", "The host is starting the match in", rule="contains")
    joiner += ["wait_label LabelMultiplayerStatus The host is starting the match in 28 s", "video_mark countdown-28-joiner",
               "assert_label LabelMultiplayerStatus The host is starting the match in 28 s - press Ready"]
    joiner += assert_watches("countdown-active-joiner") + ["wait_ms 1200", "wait_label LabelMultiplayerStatus Press Ready when you're ready to play"]
    joiner += watches("countdown-joiner-cancel", "Press Ready when you're ready to play")
    joiner += ["video_mark countdown-cancel-joiner", "wait_ms 2500"] + assert_watches("countdown-joiner-cancel")
    for lines in (host, joiner):
        lines += ["wait_file {PROBE_DIR}/done.json 120", "wait_ms 1400", "exit"]
    checks = []
    for peer in ("host", "joiner"):
        for state in ("30", "28", "cancel"):
            text = (f"Starting in {state} s - waiting for Joiner to press Ready" if peer == "host" else
                    f"The host is starting the match in {state} s - press Ready")
            events = [rf'^assert_label LabelMultiplayerStatus "{text}" text="{text}" PASS$'] if state != "cancel" else []
            if state == "30": events += ["assert_text_fits LabelMultiplayerStatus .*PASS"]
            if state == "28": events += [watch_pass(f"countdown-active-{peer}-{kind}") for kind in ("state", "layout", "duplicates", "persistent")]
            if state == "cancel":
                prefix = "countdown-cancel" if peer == "host" else "countdown-joiner-cancel"
                events += [watch_pass(f"{prefix}-{kind}") for kind in ("state", "layout", "duplicates", "persistent")]
            checks.append(dict(id=f"countdown-{state}-{peer}", peer=peer, mark=f"countdown-{state}-{peer}", screen="MultiplayerScreen",
                               what="Exact countdown text stays visible during world preparation, ticks from thirty to twenty-eight, and cancels on a later held click.",
                               events=events))
    emit(3, "Unready Start counts down from thirty seconds and cancels", host, joiner,
         lobby_probe("countdown-relay-host"), lobby_probe("countdown-relay-joiner"), checks)


def key(name):
    return [dict(op="key_down", key=name), wait(renders=3, sim_advanced=2), dict(op="key_up", key=name), wait(renders=3)]


def click(button="left"):
    return [dict(op="game_mouse", button=button, down=True), wait(renders=3, sim_advanced=2),
            dict(op="game_mouse", button=button, down=False), wait(renders=3)]


def capture(mark, screen="game"):
    return [menu(f"video_mark {mark}", capture_screen=screen), wait(elapsed_ms=1200, scope="menu"),
            dict(op="screenshot_pair", name=mark, scope="menu")]


def game_menu(peer, timeout=360, brain_loss=False):
    lines = host_setup(brain_loss=brain_loss) if peer == "host" else join_setup()
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


def place(peer, occupied=False, x=None, relative_to=None, tag=""):
    fraction = x if x is not None else (0.35 if peer == "host" else 0.65)
    steps = [wait(editing=True), dict(op="assert_relay"), dict(op="editor_pick", input_player=0),
             dict(op="editor_move", input_player=0, x_fraction=fraction), wait(renders=5, elapsed_ms=1000)]
    if relative_to:
        # The cursor is the prefab's center (96, 96); its brain chamber is at (48, 48).
        steps[3].update(relative_to=relative_to, offset_x=-48, offset_y=-48)
    # The host reads its own placement after a capture, so the joiner leaves it that window before placing.
    if peer == "joiner": steps += [wait(setup_ready=1), wait(elapsed_ms=2500)]
    if peer == "joiner" and occupied:
        steps += [wait(setup_ready=1), dict(op="editor_move", input_player=0, x_fraction=0.35)]
        steps += capture(f"occupied-preview{tag}")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Occupied by Captain's brain - choose another spot"))]
        steps += click()
        steps += [dict(op="assert_editor", input_player=0, equals=dict(ready=False, submitted=False)),
                  dict(op="editor_move", input_player=0, x_fraction=fraction), wait(renders=5, elapsed_ms=1000)]
    steps += capture(f"valid-preview-{peer}{tag}")
    steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Valid spot - click to INSTALL your brain")),
              dict(op="assert_net_ui_clear", player=0 if peer == "host" else 1),
              dict(op="assert_control", control="LabelNetMatchStatus", text_contains="0 of 2" if peer == "host" else "1 of 2", fits=True, inside="BoxNetMatchStatus")]
    steps += click()
    if peer == "host":
        steps += [wait(setup_ready=1)] + capture(f"placed-host{tag}")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(ready=True, submitted=True, screen_text="READY to start - wait for others to finish...")),
                  dict(op="assert_net_ui_clear", player=0),
                  dict(op="assert_control", control="LabelNetMatchStatus", text_contains="1 of 2", fits=True, inside="BoxNetMatchStatus")]
    steps += key("Return")
    steps += [wait(editing=False, paused=False), dict(op="assert", equals=dict(setup_ready=2, setup_humans=2, local_actor_alive=True)),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2))]
    steps += capture(f"playing-{peer}{tag}")
    return steps


def scene_checks(peer, steps):
    checks = []
    mark, start, screen = None, 0, "game"
    for index, step in enumerate(steps + [menu("video_mark scene-end")]):
        if step.get("op") == "menu" and step.get("command", "").startswith("video_mark "):
            if mark is not None:
                indices = [i for i in range(start, index)
                           if steps[i]["op"].startswith("assert") or
                           (steps[i]["op"] == "menu" and steps[i].get("command", "").startswith(("assert_", "text_watch assert ")))]
                if indices:
                    checks.append(dict(id=f"{peer}-{mark}", peer=peer, mark=mark, screen=screen,
                                       what="Captured real scene state, exact label text, panel bounds and the live player.", probe_steps=indices))
            mark, start, screen = step["command"].split(" ", 1)[1], index, step.get("capture_screen", "game")
    return checks


def buy(peer, fire=True, landing_offset=None, craft="Base.rte/Rocket MK2", soldier="Coalition.rte/Soldier Light", rebuy_lost=False):
    steps = [dict(op="game_mouse", button="right", down=True), wait(renders=3, sim_advanced=2),
             dict(op="wait_scene", input_player=0, equals=dict(pie_visible=True)),
             dict(op="pie_point", input_player=0, command=6),
             dict(op="wait_scene", input_player=0, equals=dict(pie_description="Buy Menu - click to open"))]
    steps += capture(f"pie-buy-{peer}")
    steps += [dict(op="assert_pie", input_player=0, equals=dict(visible=True, description="Buy Menu - click to open"))]
    steps += click()
    steps += [dict(op="game_mouse", button="right", down=False), wait(renders=5, elapsed_ms=1200)]
    steps += capture(f"shop-{peer}")
    steps += [dict(op="assert_buy", input_player=0, equals=dict(visible=True, enabled=True, buy_allowed=True)),
              menu("activate OrderClearButton", scope="buy", input_player=0),
              menu("activate CraftTab", scope="buy", input_player=0), dict(op="shop_pick", input_player=0, preset=craft),
              menu("activate BodiesTab", scope="buy", input_player=0), dict(op="shop_pick", input_player=0, preset=soldier),
              menu("activate GunsTab", scope="buy", input_player=0), dict(op="shop_pick", input_player=0, preset="Coalition.rte/Assault Rifle")]
    steps += capture(f"order-{peer}")
    landing = dict(op="landing_zone_move", input_player=0, within_ms=15000)
    if landing_offset is not None: landing["offset_x"] = landing_offset
    steps += [dict(op="assert_buy", input_player=0, equals=dict(cart=[soldier, "Coalition.rte/Assault Rifle"], craft=craft, passengers=1), remember="order"),
              dict(op="assert_control", scope="buy", input_player=0, control="BuyButton", equals=dict(visible=True, enabled=True), fits=True, inside="BuyGUIBox"),
              menu("activate BuyButton", scope="buy", input_player=0),
              landing]
    steps += capture(f"landing-zone-{peer}")
    steps += [dict(op="assert_scene", input_player=0, equals=dict(landing_zone_selection=True,
              screen_text="Choose your landing zone... Hold UP or DOWN to place multiple orders"))]
    steps += click()
    steps += [
              dict(op="wait_scene", input_player=0, funds_delta_from="order"),
              dict(op="assert_scene", input_player=0, funds_delta_from="order"),
              dict(op="wait_scene", input_player=0, delivered=soldier)]
    select = [dict(op="actor_next_until", input_player=0, preset=soldier),
              dict(op="wait_scene", input_player=0, equals=dict(preset=soldier, weapon="Coalition.rte/Assault Rifle"))]
    steps += capture(f"arrival-{peer}")
    steps += [dict(op="assert_scene", input_player=0, delivered=soldier, equals=dict(alive=True)),
              dict(op="assert_buy", input_player=0, equals=dict(visible=False, enabled=False))]
    if rebuy_lost:
        # A unit the fight takes before its player does is bought again from the pie, up to three times.
        select[0].update(lost_rebuy_back=len(steps), lost_rebuy_attempts=3)
    steps += select
    if fire:
        steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, preset=soldier, weapon="Coalition.rte/Assault Rifle"), remember="before-fire")]
        steps += [dict(op="game_mouse", down=True), wait(renders=5, sim_advanced=12)]
        steps += capture(f"fire-{peer}")
        steps += [dict(op="assert_scene", input_player=0, rounds_less_than="before-fire", equals=dict(alive=True, fired=True, weapon="Coalition.rte/Assault Rifle")), dict(op="game_mouse", down=False)]
    return steps


def pause_end():
    return key("Escape") + [wait(screen="Pause"), menu("activate ButtonEndMatch"),
                              wait(screen="PauseLeaveConfirm"), menu("activate ButtonLeaveConfirm")]


def say(text):
    return key("CHAT") + [wait(chat_entry_open=True), menu(f"type_text TextMatchChatInput enter {text}"),
                          wait(chat_text_once=text)]


# A returned seat's saving is judged on the first save named after its return and 600 ticks past it; at the save proof's
# ten-second interval that is up to two intervals of play after the return.
RETURN_PLAY_ON_TICKS = 1200


def complete_live_scene(peer, number, play_on=0):
    # The menu script resumes only after the activity returns to the lobby.
    # The joiner acknowledges its captured result before the host ends the match, after play_on more ticks of play.
    joined = f"joiner answers scene {number}"
    hosted = f"host answers scene {number}"
    captured = f"joiner captured scene {number}"
    steps = say(joined) + [wait(chat_text_once=hosted)] if peer == "joiner" else [wait(chat_text_once=joined)] + say(hosted)
    steps += capture(f"answered-{peer}") + [dict(op="assert_scene", input_player=0, equals=dict(alive=True)), dict(op="assert_relay")]
    steps += say(captured) if peer == "joiner" else [wait(chat_text_once=captured)] + ([wait(sim_advanced=play_on)] if play_on else []) + pause_end()
    return steps + [lobby_wait(peer), dict(op="finish")]


def game_scenes():
    host, joiner = place("host"), place("joiner", occupied=True)
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2))] + complete_live_scene(peer, 4)
    emit(4, "One held brain click, valid preview, shared count and occupied-spot refusal",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))

    host, joiner = place("host") + buy("host"), place("joiner") + buy("joiner")
    for peer, steps in (("host", host), ("joiner", joiner)): steps += complete_live_scene(peer, 5)
    emit(5, "Brain pie, own funds, soldier and weapon delivery, selection and firing",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))

    # The plateau gives the shot a clear path; the dropship keeps its engines beside the passenger.
    # Let the passenger finish landing before taking control.
    host = place("host", x=0.4) + buy("host", fire=False, landing_offset=-180, craft="Base.rte/Dropship MK1")
    joiner = place("joiner", x=0.48)
    host += [dict(op="aim_brain", input_player=0, target_player=1)]
    host += capture("aimed-at-brain-host") + [dict(op="assert_scene", input_player=0,
             equals=dict(alive=True, preset="Coalition.rte/Soldier Light", weapon="Coalition.rte/Assault Rifle"))]
    host += [dict(op="game_mouse", down=True), dict(op="aim_brain", input_player=0, target_player=1,
             until_end=True, scope="menu", within_ms=15000), dict(op="game_mouse", down=False)]
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [lobby_wait(peer)]
        steps += capture(f"brain-loss-{peer}", "MultiplayerScreen")
        # The lobby names the loss in a player's words; the rule's script line stays in the log and the Details dialog.
        steps += [menu("assert_label LabelLastMatchSummary Joiner's brain was destroyed"),
                  menu("assert_label_absent LabelLastMatchSummary .lua"), menu("assert_label_absent LabelLastMatchSummary tick"),
                  menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
    checks = scene_checks("host", host) + scene_checks("joiner", joiner)
    checks += [dict(id=f"brain-loss-console-{peer}", peer=peer, mark=f"brain-loss-{peer}", screen="MultiplayerScreen",
                    what="The activity's brain-loss rule, brain identity and tick appear in each peer's end reason.",
                    log_regex=[r"\[net-match\] brain lost: Joiner's Brain Case .* at tick ",
                               r"\[net-match\] activity end: Skirmish Defense ended at tick .*SkirmishDefense.lua:397.*Brain Case",
                               r"NETWORK: Match complete: " + (r"Victory!" if peer == "host" else r"Defeat") + r" - .*SkirmishDefense.lua:397.*Brain Case"])
               for peer in ("host", "joiner")]
    emit(6, "Actual weapon damage ends by the activity's brain-loss rule on both peers",
         game_menu("host", brain_loss=True), game_menu("joiner"), game_probe(host), game_probe(joiner), checks,
         matching_log_lines=[r"^\[net-match\] brain lost: .+$", r"^\[net-match\] activity end: .+$"])

    host, joiner = place("host"), place("joiner")
    # The host reads the seat while its player is away at the menu; the player presses Rejoin Match after that.
    host += key("F6") + [wait(panel_open=True), wait(held_peer="Joiner"), dict(op="wait_file", path="{PROBE_DIR_joiner}/left.json", within_ms=120000)]
    host += capture("left-seat-held") + [dict(op="assert", equals=dict(service="Running", local_actor_alive=True), held_peer="Joiner"),
            dict(op="assert_control", control="NetworkSeatDetail@Joiner", text_contains="AI", fits=True, inside="NetworkSeats"),
            # The status box tells the host why the seat is held; its numbers stay behind Show network diagnostics.
            dict(op="assert_control", control="LabelNetMatchStatus", text_contains="Joiner: Left", text_lacks=STATUS_NUMBERS, fits=True, inside="BoxNetMatchStatus"),
            dict(op="signal", name="left-read"), wait(returned_peer="Joiner", within_ms=90000)] + key("F6") + [wait(panel_open=False)]
    host += capture("rejoined-host") + [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="assert_relay")] + complete_live_scene("host", 7, RETURN_PLAY_ON_TICKS)
    joiner += key("Escape") + [wait(screen="Pause"), menu("activate ButtonLeaveMatch"), wait(screen="PauseLeaveConfirm")]
    joiner += capture("leave-consequence", "PauseLeaveConfirm") + [menu("assert_text_fits ButtonLeaveConfirm"), menu("activate ButtonLeaveConfirm"),
               wait(screen="MultiplayerScreen", scope="menu"), dict(op="signal", name="left", scope="menu"),
               dict(op="wait_file", path="{PROBE_DIR_host}/left-read.json", scope="menu", within_ms=120000),
               menu("activate ButtonMultiplayerReconnect"), wait(service="Running")]
    joiner += capture("rejoined-joiner") + [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="assert_relay")] + complete_live_scene("joiner", 7)
    emit(7, "Leave holds the live seat to AI and Rejoin Match returns the same player",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))

    moderation_scene()

    host, joiner = place("host"), place("joiner")
    host += pause_end()
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [lobby_wait(peer)]
        steps += capture(f"rematch-waiting-{peer}", "MultiplayerScreen")
        expected = "Waiting for Joiner to press Ready" if peer == "host" else "Press Ready when you're ready to play"
        steps += [menu(f"text_watch start rematch-state equals substate:Lobby LabelMultiplayerStatus {expected}"),
                  menu("text_watch start rematch-duplicates duplicates substate:Lobby"),
                  menu("text_watch start rematch-layout layout substate:Lobby"), wait(elapsed_ms=4000, scope="menu"),
                  menu("text_watch assert rematch-state"), menu("text_watch assert rematch-duplicates"), menu("text_watch assert rematch-layout")]
    # Both peers have finished watching the rematch lobby before either starts its countdown.
    joiner += [menu("type_text TextLobbyChat enter joiner watches the rematch countdown")]
    host += [wait(scope="menu", chat_text_once="joiner watches the rematch countdown"),
             menu("assert_label LabelLobbyChatAny joiner watches the rematch countdown")]
    host += [menu("activate ButtonMultiplayerStart"), wait(scope="menu", control="LabelMultiplayerStatus", text_contains="Starting in 30 s")]
    host += [menu("assert_label LabelMultiplayerStatus Starting in 30 s - waiting for Joiner to press Ready")]
    host += [menu(command) for command in watches("rematch-active-host", "Starting in", rule="contains")]
    host += capture("rematch-countdown-host", "MultiplayerScreen") + [menu("assert_label ButtonMultiplayerStart Cancel Start"), wait(elapsed_ms=2200, scope="menu")]
    host += [menu(command) for command in assert_watches("rematch-active-host")]
    host += [wait(scope="menu", chat_text_once="joiner checked the rematch countdown"),
             menu("assert_label LabelLobbyChatAny joiner checked the rematch countdown")]
    host += [menu("activate ButtonMultiplayerStart"), wait(elapsed_ms=1200, scope="menu"), menu("assert_label LabelMultiplayerStatus Waiting for Joiner to press Ready"), dict(op="finish")]
    joiner += [wait(scope="menu", control="LabelMultiplayerStatus", text_contains="The host is starting the match in 30 s")]
    joiner += [menu("assert_label LabelMultiplayerStatus The host is starting the match in 30 s - press Ready")]
    joiner += [menu(command) for command in watches("rematch-active-joiner", "The host is starting the match in", rule="contains")]
    joiner += capture("rematch-countdown-joiner", "MultiplayerScreen") + [menu("assert_text_fits LabelMultiplayerStatus")]
    joiner += [menu(command) for command in assert_watches("rematch-active-joiner")]
    joiner += [menu("type_text TextLobbyChat enter joiner checked the rematch countdown")]
    joiner += [wait(elapsed_ms=4000, scope="menu"), menu("assert_label LabelMultiplayerStatus Press Ready when you're ready to play"), dict(op="finish")]
    emit(9, "Rematch waiting names, no premature countdown, thirty-second Start and Cancel",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner),
         scene_checks("host", host) + scene_checks("joiner", joiner))


# Stock reinforced fill buried around the bunker and again before its door, offsets from its center: the endless waves
# wear an undefended brain down inside twenty minutes, and get through fill above the ground or by the chamber's door,
# which the pathfinder always treats as open.
COLLAR = ((-144, -144), (-48, -144), (48, -144), (144, -144), (-144, -48), (-144, 48), (144, -48), (144, 48), (240, 48))


def defenses(player):
    steps = []
    for offset_x, offset_y in COLLAR:
        steps += [wait(editing=True), dict(op="editor_pick", input_player=0, brain=False, **{"class": "TerrainObject"}, preset="Reinforced Concrete Fill"),
                  dict(op="editor_move", input_player=0, relative_to="bunker", offset_x=offset_x, offset_y=offset_y), wait(picker_open=False, player=player)]
        steps += click() + [wait(sim_advanced=30, elapsed_ms=600), dict(op="assert_editor", input_player=0, equals=dict(placement_refused=False))]
    return steps


def defense_lines(seats):
    return [rf"\[net-match\] editor purchase applied: seat={seat} preset=Base.rte/Reinforced Concrete Fill funds=" for seat in seats]


def ai_fight():
    host_menu, join_menu = host_setup(4, cpu=True), join_setup(4)
    # These are stock activity options: Infinite gold selects Skirmish Defense's endless AI mode.
    insert = host_menu.index("activate ButtonHostOptApply")
    host_menu[insert:insert] = ["activate TabHostPageRules", "slider_set SliderHostRulesDifficulty 0",
                              "slider_set SliderHostRulesGold 31000", "setcheck CheckHostRulesClearPath 0"]
    host_menu += ["wait_remote_ready 90", "activate ButtonMultiplayerStart", "wait_file {PROBE_DIR}/done.json 1600", "wait_ms 1400", "exit"]
    join_menu += ["activate ButtonMultiplayerReady", "wait_file {PROBE_DIR}/done.json 1600", "wait_ms 1400", "exit"]
    probes, checks = {}, [dict(id="cpu-seats-host", peer="host", mark="cpu-seats-host", screen="MultiplayerScreen",
                              what="Two held picks replace the open seats with CPU teams and remain selected after refresh.",
                              events=['^combo_select ComboHostSeatType2 CPU pick "CPU" in ComboHostSeatType2 PASS$',
                                      '^combo_select ComboHostSeatType3 CPU pick "CPU" in ComboHostSeatType3 PASS$',
                                      '^assert_label ComboHostSeatType2 "CPU" text="CPU" PASS$',
                                      '^assert_label ComboHostSeatType3 "CPU" text="CPU" PASS$']),
                         dict(id="cpu-human-count-host", peer="host", mark="cpu-human-count-host", screen="MultiplayerScreen",
                              what="The Human players count is two while both CPU seats remain selected.",
                              events=['^combo_select ComboHostSeatPlayers 2 pick "2" in ComboHostSeatPlayers PASS$',
                                      '^assert_label ComboHostSeatPlayers "2" text="2" PASS$',
                                      '^assert_label ComboHostSeatType2 "CPU" text="CPU" PASS$',
                                      '^assert_label ComboHostSeatType3 "CPU" text="CPU" PASS$'])]
    for peer, fraction in (("host", 0.3), ("joiner", 0.7)):
        steps = [wait(editing=True), dict(op="editor_pick", input_player=0, brain=False, **{"class": "BunkerAssembly"}, preset="Brain Chamber Left 2x2 - 1"),
                 dict(op="editor_move", input_player=0, x_fraction=fraction, y=1104, remember="bunker"),
                 wait(picker_open=False, player=0 if peer == "host" else 1)]
        steps += capture(f"bunker-preview-{peer}")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Click to ADD a new object - Drag for precision"))]
        steps += click() + [wait(sim_advanced=90, elapsed_ms=1600)] + capture(f"bunker-installed-{peer}")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(placement_refused=False))]
        steps += defenses(0 if peer == "host" else 1)
        steps += place(peer, x=fraction, relative_to="bunker")
        for minute in range(1, 21):
            steps += [dict(op="measure_minute", within_ms=70000)]
            other = "joiner" if peer == "host" else "host"
            steps += say(f"{peer} answers minute {minute}") + [wait(chat_text_once=f"{other} answers minute {minute}"), wait(elapsed_ms=1500)]
            steps += capture(f"minute-{minute:02d}-{peer}")
            steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=2)), dict(op="assert_relay")]
        if peer == "host": steps += pause_end()
        steps += [lobby_wait(peer)]
        steps += capture(f"fight-ended-{peer}", "MultiplayerScreen") + [menu("assert_label LabelLastMatchSummary Match ended by host"),
                                                                    menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
        probes[peer] = game_probe(steps, timeout=1600000)
        checks += scene_checks(peer, steps)
        checks.append(dict(id=f"shared-bunkers-{peer}", peer=peer, mark=f"playing-{peer}", screen="game",
                           what="Both bunkers and their reinforced fill reached the shared scene before the brains started playing.",
                           log_regex=[rf"\[net-match\] editor purchase applied: seat={seat} preset=Base.rte/Brain Chamber Left 2x2 - 1 funds="
                                      for seat in (0, 1)] + defense_lines((0, 1))))
        checks.append(dict(id=f"twenty-minutes-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="MultiplayerScreen",
                           what="Twenty full running minutes at >=58 ticks/s, living local player, AI weapon fire and named end reason.",
                            continuous_minutes=dict(count=20, minimum_tps=58.0),
                           log_regex=[r"\[fight15-scene\] minute=20 pace=", r"\[net-match\].*host"]))
    emit(10, "Twenty-minute stock endless AI fight with scripted bunker and brain setup",
         host_menu, join_menu, probes["host"], probes["joiner"], checks, timeout=1750,
         engine_args=("-memory-census-seconds", "60"))


def moderation_scene():
    if SELECTED is not None and "fight15-s08" not in SELECTED:
        return
    document, runs, scripts, checks = None, [], {}, []
    for action, part, reason in (("kick", "Remove", "removed"), ("ban", "Ban", "banned")):
        host, joiner = place("host"), place("joiner")
        button = f"NetworkSeat{part}@Joiner"
        host += key("F6") + [wait(panel_open=True), menu(f"activate {button}")]
        host += capture(f"confirm-{action}") + [dict(op="assert_control", control=button, text_contains=f"Confirm: {'remove' if action == 'kick' else 'ban'} Joiner", fits=True, inside="NetworkSeats"),
                menu(f"activate {button}")]
        # Refusing a banned connection is an admission event, not an error in the host's running match.
        # The joiner checks its refusal screen; the host's retained log must name that admission refusal.
        host += ([wait(elapsed_ms=90000, sim_advanced=120, within_ms=95000)] + capture("ban-refused-host")
                 if action == "ban" else [wait(elapsed_ms=25000)])
        host += [dict(op="assert_scene", input_player=0, equals=dict(alive=True))] + key("F6") + [wait(panel_open=False)]
        host += pause_end() + [wait(screen="MultiplayerScreen", scope="menu"), dict(op="finish")]
        joiner += [wait(screen="MultiplayerScreen", scope="menu")]
        joiner += capture(f"{action}-joiner", "MultiplayerScreen") + [dict(op="assert_control", scope="menu", control="LabelMultiplayerLandingStatus",
                equals=dict(text=f"The host {reason} you from this session", visible=True), fits=True, inside="MultiplayerLandingPanel")]
        if action == "ban":
            # The directory heartbeat must advertise the held seat before the new join can reach the host's ban check.
            joiner += [menu("activate ButtonMultiplayerJoinGame"), dict(op="wait_public_row", scope="menu", name=OWN_LOBBY,
                       state="running", held_at_least=1, joinable=True),
                       menu(f"click_row {OWN_LOBBY}"), menu("activate ButtonMultiplayerConnect"),
                       wait(scope="menu", control="LabelJoinSelected", text_contains="The host banned you from this session")]
            joiner += capture("banned-rejoin-refused", "MultiplayerScreen") + [dict(op="assert_control", scope="menu", control="LabelJoinSelected",
                    equals=dict(text="The host banned you from this session", visible=True), fits=True)]
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
        if action == "ban":
            checks.append(dict(id="ban-host-refuses-readmission", run=action, peer="host", mark="ban-refused-host", screen="game",
                               what="The live host refuses the banned player's new admission while its own match keeps running.",
                               log_regex=[r"\[net-session\] admission refused reason=ParticipantBanned role=host .*The host banned you from this session"]))
    document.pop("peers", None)
    document.update(runs=runs, scripts=scripts, checklist=checks)
    (ROOT / "fight15-s08.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def menu_probe(steps, timeout=360000):
    """A probe that starts with the menus and follows the same player into the match."""
    document = json.loads(probe(steps, timeout))
    document.update(sim_clock="lockstep")
    return json.dumps(document, indent=2) + "\n"


def lobby_exit():
    return ["wait_file {PROBE_DIR}/done.json 360", "wait_ms 1400", "exit"]


def own_seat_held_scene():
    """D1 and D2: the host's own seat held on cue reads its line once, and the status box says why in words, its numbers left to diagnostics."""
    stall_tick, stall_ms = 3000, 2500
    host, joiner = place("host"), place("joiner")
    host += [wait(control="LabelNetMatchStatus", text_contains="until you are back", within_ms=90000)] + capture("own-seat-held-host")
    host += [wait(control="LabelNetMatchStatus", text_contains="Everyone is connected", within_ms=90000)] + capture("own-seat-back-host")
    host += [dict(op="assert_control", control="LabelNetMatchStatus", text_contains="F6: Players", text_lacks=STATUS_NUMBERS, fits=True, inside="BoxNetMatchStatus")]
    host += complete_live_scene("host", 14)
    joiner += [wait(control="LabelNetMatchStatus", text_contains="The AI is playing for Captain", within_ms=90000)] + capture("host-held-joiner")
    joiner += complete_live_scene("joiner", 14)
    checks = scene_checks("host", host) + scene_checks("joiner", joiner)
    checks.append(dict(id="host-seat-held-host", peer="host", mark="own-seat-held-host", screen="game",
                       what=f"The host's own simulation stalled {stall_ms} ms at tick {stall_tick}: its seat was held to the AI and came back.",
                       log_regex=[rf"\[net-test\] live stall frame=\d+ ms={stall_ms}", r"\[net-seat-ai\] peer=1 held=1 frame=", r"\[net-match\] seat-reclaimed peer=1 frame="]))
    stall = {"peer": "host", "first_tick": stall_tick - 60, "last_tick": stall_tick + 240,
             "reason": f"the scene's own stall of the host's simulation, {stall_ms} ms at tick {stall_tick}"}
    emit(14, "The host's own seat held on cue says so once, and the status box gives reasons in words",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner), checks,
         peer_args={"host": ["-net-test-live-stall", f"{stall_tick}:{stall_ms}"]}, peer_env={"host": {"CC_TEST_RETURN_INPUT_DETAILS": "1"}},
         extra=dict(drawn_frames_exclude=[dict(stall)], allowed_stills=[dict(stall)]))


def seat_dialog_parts():
    """The lobby seat dialog opened on the joined player, the rejoin a removed player tries, and the probe both sides finish with."""
    seat = ["activate ButtonLobbyOptions", "wait_ms 600", "assert_substate HostOptions", "activate TabHostPageSeats", "wait_ms 600",
            "activate ButtonHostSeatDetails1", "wait_ms 900", "assert_visible HostSeatDialog 1", "assert_label LabelHostSeatDlgName Joiner"]
    # The list shows the freed seat once the directory has the host's next listing; a hand waits to read it before Connect.
    rejoin = ["activate ButtonMultiplayerJoinGame", "wait_ms 600", "assert_substate JoinSetup", *join_own_lobby(), "wait_label LabelJoinSelected 1/2 players",
              "activate ButtonMultiplayerConnect"]
    # The relay is read while both players are seated; the removal comes after both have read it.
    done = probe([dict(op="wait_file", path="{PROBE_DIR}/relay-check.json", scope="menu", within_ms=300000), dict(op="assert_relay", scope="menu"),
                  dict(op="signal", name="relay-ok", scope="menu"), dict(op="wait_file", path="{PROBE_DIR}/scene-end.json", scope="menu", within_ms=400000),
                  dict(op="finish")], 400000)
    return seat, rejoin, done


def seated_for_removal(host, joiner):
    """Both sides read their relay route before the host removes the joined player."""
    host += ["touch_file {PROBE_DIR}/relay-check.json", "wait_file {PROBE_DIR}/relay-ok.json 60", "wait_file {PROBE_DIR_joiner}/relay-ok.json 60"]
    joiner += ["touch_file {PROBE_DIR}/relay-check.json"]


def seat_dialog_checks(marks):
    checks = [dict(id=mark, peer=peer, mark=mark, screen="MultiplayerScreen", what="The lobby seat dialog's removal reached the player, who stays out of the match.",
                   events=[f"^video_mark {mark}"]) for peer, names in marks.items() for mark in names]
    checks[0]["events"] += ["^assert_enabled ButtonHostSeatDlgKick .*PASS", "^assert_enabled ButtonHostSeatDlgBan .*PASS"]
    return checks


def seat_dialog_scene():
    """D3, D9 and G6: Kick in the lobby seat dialog asks for a second press that names what it does, removes the connected
    player, and the removed player stays refused for the match by name."""
    seat, rejoin, done = seat_dialog_parts()
    host, joiner = host_setup(), join_setup()
    seated_for_removal(host, joiner)
    host += seat + ["video_mark seat-dialog-host", "assert_enabled ButtonHostSeatDlgKick 1", "assert_enabled ButtonHostSeatDlgBan 1",
            "assert_text_fits LabelHostSeatDlgActionHint", "assert_no_overlap LabelHostSeatDlgActionHint ButtonHostSeatDlgBan"]
    host += ["activate ButtonHostSeatDlgKick", "wait_ms 600", "video_mark kick-confirm-host", "assert_visible HostSeatDialog 1",
             "assert_label ButtonHostSeatDlgKick Confirm kick", "assert_label LabelHostSeatDlgActionHint Press Confirm kick to remove Joiner from this session",
             "assert_text_fits LabelHostSeatDlgActionHint", "assert_text_fits ButtonHostSeatDlgKick", "activate ButtonHostSeatDlgKick", "wait_ms 1200",
             "assert_visible HostSeatDialog 0", "activate ButtonHostOptBack", "wait_substate Lobby 30", "wait_label LabelLobbyPlayer1 Open seat",
             "video_mark kicked-host", "wait_file {PROBE_DIR_joiner}/kick-refused.json 120"] + lobby_exit()
    joiner += ["video_mark seated-joiner", "wait_label LabelMultiplayerLandingStatus The host removed you from this session",
              "video_mark kicked-joiner", "assert_text_fits LabelMultiplayerLandingStatus"]
    joiner += rejoin + ["wait_label LabelJoinSelected The host removed you from this session", "video_mark kick-refused-joiner",
               "touch_file {PROBE_DIR}/kick-refused.json"] + lobby_exit()
    host[-3:-3] = ["touch_file {PROBE_DIR}/scene-end.json"]
    joiner[-3:-3] = ["touch_file {PROBE_DIR}/scene-end.json"]
    checks = seat_dialog_checks({"host": ["seat-dialog-host", "kick-confirm-host", "kicked-host"], "joiner": ["kicked-joiner", "kick-refused-joiner"]})
    emit(15, "Kick in the lobby seat dialog asks first, removes a connected player, and the player stays out of the match", host, joiner, done, done,
         checks, timeout=420)


def ban_scene():
    """D3, D9 and G6: Ban in the lobby seat dialog asks first and bans the connected player; Banned players lists the ban and
    Remove ban lifts it with its removal, saying the player may ask to join again, and the player's next Connect is let in."""
    seat, rejoin, done = seat_dialog_parts()
    host, joiner = host_setup(), join_setup()
    seated_for_removal(host, joiner)
    host += seat + ["video_mark seat-dialog-host", "assert_enabled ButtonHostSeatDlgKick 1", "assert_enabled ButtonHostSeatDlgBan 1"]
    host += ["activate ButtonHostSeatDlgBan", "wait_ms 600", "video_mark ban-confirm-host", "assert_visible HostSeatDialog 1",
             "assert_label ButtonHostSeatDlgBan Confirm ban", "assert_label LabelHostSeatDlgActionHint Press Confirm ban to ban Joiner from this session",
             "assert_text_fits LabelHostSeatDlgActionHint", "activate ButtonHostSeatDlgBan", "wait_ms 1200", "assert_visible HostSeatDialog 0",
             "activate TabHostPageSession", "wait_ms 600", "activate ButtonHostSessBanned", "wait_ms 900", "video_mark banned-list-host",
             "assert_visible HostBannedDialog 1", "assert_label LabelHostBannedList Joiner", "assert_label LabelHostBannedList session",
             "wait_file {PROBE_DIR_joiner}/refused.json 120", "activate ButtonHostBannedRemove", "wait_ms 300", "video_mark ban-lifted-host",
             "wait_label LabelHostBannedStatus Remove ban: Done. They may ask to join again, this match included.", "assert_text_fits LabelHostBannedStatus",
             "assert_label_absent LabelHostBannedList Joiner", "touch_file {PROBE_DIR}/unbanned.json", "activate ButtonHostBannedClose", "wait_ms 600",
             "activate ButtonHostOptBack", "wait_substate Lobby 30", "wait_file {PROBE_DIR_joiner}/readmitted.json 120"] + lobby_exit()
    joiner += ["video_mark seated-joiner", "wait_label LabelMultiplayerLandingStatus The host banned you from this session",
              "video_mark banned-joiner", "assert_text_fits LabelMultiplayerLandingStatus"]
    joiner += rejoin + ["wait_label LabelJoinSelected The host banned you from this session", "video_mark ban-refused-joiner", "touch_file {PROBE_DIR}/refused.json",
               "wait_file {PROBE_DIR_host}/unbanned.json 120", "activate ButtonMultiplayerConnect",
               "wait_connected 2 90", "wait_substate Lobby 90", "video_mark readmitted-joiner",
               "touch_file {PROBE_DIR}/readmitted.json"] + lobby_exit()
    host[-3:-3] = ["touch_file {PROBE_DIR}/scene-end.json"]
    joiner[-3:-3] = ["touch_file {PROBE_DIR}/scene-end.json"]
    checks = seat_dialog_checks({"host": ["seat-dialog-host", "ban-confirm-host", "banned-list-host", "ban-lifted-host"],
                                 "joiner": ["banned-joiner", "ban-refused-joiner", "readmitted-joiner"]})
    emit(31, "Ban in the lobby seat dialog asks first; Banned players lists it and Remove ban lifts it, the player let in again", host, joiner,
         done, done, checks, timeout=420)


def files_page(where="main"):
    """Settings > Network > Files, opened by hand from the main menu."""
    return ["activate ButtonMainToOptions", "wait_ms 600", "select_settings_page Network", "wait_ms 450", "select_settings_page Network:Basics",
            "wait 3", "activate ButtonNetworkAdvanced", "wait 4", "select_settings_page Network:Files", "wait 4"]


def recovery_defaults_scene():
    """D4 and D6: the host's Recovery page promises held seats wait for its word and starts on autosave every five minutes."""
    host = host_setup() + ["activate ButtonLobbyOptions", "wait_ms 600", "assert_substate HostOptions", "activate TabHostPageRecovery", "wait_ms 600",
            "video_mark recovery-host", "assert_label LabelHostRecReclaim Seats stay held until you give them away",
            "assert_text_fits LabelHostRecReclaim", "assert_no_overlap LabelHostRecRejoin LabelHostRecReclaim",
            "assert_checked CheckHostRecAutosave 1", "assert_label ComboHostRecAutosaveInterval 5 minutes",
            "assert_text_fits ComboHostRecAutosaveInterval", "assert_label LabelHostRecAutosaveHint Every 60 s to 60 min; 5 min by default",
            "assert_text_fits LabelHostRecAutosaveHint",
            "combo_select ComboHostRecAutosaveInterval 10 minutes", "assert_label ComboHostRecAutosaveInterval 10 minutes", "activate ButtonHostOptApply",
            "wait_ms 900", "video_mark recovery-applied-host", "activate TabHostPageSeats", "wait_ms 600", "activate ButtonHostSeatDetails1", "wait_ms 900",
            "assert_visible HostSeatDialog 1", "video_mark seat-words-host", "assert_label ButtonHostSeatDlgWait Keep for Joiner",
            "assert_label ButtonHostSeatDlgApprove Let someone join", "assert_label ButtonHostSeatDlgCancel Cancel this approval",
            "assert_text_fits ButtonHostSeatDlgWait", "assert_text_fits ButtonHostSeatDlgApprove", "assert_text_fits ButtonHostSeatDlgCancel",
            "activate ButtonHostSeatDlgClose", "wait_ms 600", "activate ButtonHostOptBack", "wait_substate Lobby 30", "touch_file {PROBE_DIR}/applied.json"]
    host += ["wait_file {PROBE_DIR_joiner}/scene-end.json 120", "touch_file {PROBE_DIR}/scene-end.json"] + lobby_exit()
    # The joiner's Match details read the cadence the host applied.
    joiner = join_setup() + ["wait_file {PROBE_DIR_host}/applied.json 120", "wait_ms 1200", "activate ButtonLobbyEditSetup", "wait_ms 600",
              "assert_substate HostOptions", "activate TabHostPageRecovery", "wait_ms 600", "video_mark recovery-joiner",
              "assert_checked CheckHostRecAutosave 1", "assert_label ComboHostRecAutosaveInterval 10 minutes", "assert_enabled ComboHostRecAutosaveInterval 0",
              "activate ButtonHostOptBack", "wait_substate Lobby 30", "touch_file {PROBE_DIR}/scene-end.json"] + lobby_exit()
    done = probe([dict(op="wait_file", path="{PROBE_DIR}/scene-end.json", scope="menu", within_ms=300000), dict(op="assert_relay", scope="menu"),
                  dict(op="finish")], 320000)
    checks = [dict(id="recovery-host", peer="host", mark="recovery-host", screen="MultiplayerScreen",
                   what="The host's Recovery page promises held seats wait for the host's word and starts on autosave every five minutes.",
                   events=["^assert_label LabelHostRecReclaim .*PASS", "^assert_text_fits LabelHostRecReclaim.*PASS",
                           "^assert_checked CheckHostRecAutosave .*PASS", "^assert_label ComboHostRecAutosaveInterval \"5 minutes\".*PASS"]),
              dict(id="recovery-joiner", peer="joiner", mark="recovery-joiner", screen="MultiplayerScreen",
                   what="The joiner's Match details read the cadence the host applied, and cannot change it.",
                   events=["^assert_label ComboHostRecAutosaveInterval \"10 minutes\".*PASS", "^assert_enabled ComboHostRecAutosaveInterval .*PASS"])]
    emit(16, "Autosave on every five minutes by default; the host's Recovery page promises held seats wait", host, joiner, done, done, checks,
         timeout=360)


def ready_scene():
    """G1 and G2: Ready and Cancel Ready, a setup change clearing Ready, the setup cancelling a countdown, the last Ready starting at once."""
    host = host_setup() + ["assert_label LabelMultiplayerStatus Waiting for Joiner to press Ready",
            "wait_label LabelMultiplayerStatus Everyone is ready - press Start Match", "video_mark ready-host",
            "wait_label LabelMultiplayerStatus Waiting for Joiner to press Ready", "video_mark ready-taken-back-host", "touch_file {PROBE_DIR}/saw-cancel.json",
            "wait_label LabelMultiplayerStatus Everyone is ready - press Start Match",
            "activate ButtonLobbyEditSetup", "wait_ms 600", "assert_substate HostOptions", "activate TabHostPageRules", "wait_ms 400",
            "slider_set SliderHostRulesGold 3000", "activate ButtonHostOptApply", "wait_ms 900", "activate ButtonHostOptBack", "wait_substate Lobby 30",
            "wait_label LabelMultiplayerStatus Waiting for Joiner to press Ready", "video_mark setup-changed-host", "touch_file {PROBE_DIR}/changed.json",
            "wait_file {PROBE_DIR_joiner}/saw-changed.json 60",
            "activate ButtonMultiplayerStart", "wait_label LabelMultiplayerStatus Starting in", "assert_label ButtonMultiplayerStart Cancel Start",
            "activate ButtonLobbyEditSetup", "wait_ms 600", "assert_substate HostOptions",
            "assert_label LabelHostOptStatus The start countdown stopped while you change the setup.", "video_mark countdown-stopped-host",
            "wait_file {PROBE_DIR_joiner}/saw-setup-open.json 60", "activate ButtonHostOptBack", "wait_substate Lobby 30", "wait_ms 1200",
            "assert_label LabelMultiplayerStatus Waiting for Joiner to press Ready", "assert_label ButtonMultiplayerStart Start Match",
            "video_mark countdown-cancelled-host", "touch_file {PROBE_DIR}/countdown-stopped.json",
            "wait_file {PROBE_DIR_joiner}/saw-stopped.json 60", "activate ButtonMultiplayerStart", "wait_label LabelMultiplayerStatus Starting in",
            "touch_file {PROBE_DIR}/countdown-running.json"] + lobby_exit()
    joiner = join_setup() + ["assert_label ButtonMultiplayerReady Ready", "activate ButtonMultiplayerReady", "wait_label ButtonMultiplayerReady Cancel Ready",
              "assert_label LabelMultiplayerStatus You're ready", "video_mark ready-joiner", "wait_ms 1500",
              "activate ButtonMultiplayerReady", "wait_label ButtonMultiplayerReady Ready", "wait_label LabelMultiplayerStatus Press Ready when you're ready to play",
              "video_mark ready-taken-back-joiner", "wait_file {PROBE_DIR_host}/saw-cancel.json 60",
              "activate ButtonMultiplayerReady", "wait_label ButtonMultiplayerReady Cancel Ready",
              "wait_label LabelMultiplayerStatus The host changed the setup", "assert_label LabelMultiplayerStatus check it and press Ready again",
              "assert_label ButtonMultiplayerReady Ready", "video_mark setup-changed-joiner", "wait_file {PROBE_DIR_host}/changed.json 60",
              "touch_file {PROBE_DIR}/saw-changed.json",
              "wait_label LabelMultiplayerStatus The host is changing the setup", "assert_label LabelMultiplayerStatus the start waits for it", "video_mark setup-open-joiner",
              "touch_file {PROBE_DIR}/saw-setup-open.json", "wait_file {PROBE_DIR_host}/countdown-stopped.json 60",
              "wait_label LabelMultiplayerStatus press Ready", "assert_label_absent LabelMultiplayerStatus starting the match in",
              "video_mark countdown-cancelled-joiner", "touch_file {PROBE_DIR}/saw-stopped.json",
              "wait_file {PROBE_DIR_host}/countdown-running.json 60", "wait_label LabelMultiplayerStatus The host is starting the match in",
              "video_mark last-ready-joiner", "activate ButtonMultiplayerReady", "touch_file {PROBE_DIR}/last-ready.json"] + lobby_exit()
    # The last Ready starts the match at once: the countdown's remaining seconds never run out first.
    host_steps, joiner_steps = place("host"), [dict(op="wait_file", path="{PROBE_DIR}/last-ready.json", scope="menu", within_ms=300000),
                                               wait(editing=True, within_ms=15000)] + place("joiner")
    host_steps += pause_end() + [lobby_wait("host"), dict(op="finish")]
    joiner_steps += [lobby_wait("joiner"), dict(op="finish")]
    marks = {"host": ["ready-host", "ready-taken-back-host", "setup-changed-host", "countdown-stopped-host", "countdown-cancelled-host"],
             "joiner": ["ready-joiner", "ready-taken-back-joiner", "setup-changed-joiner", "setup-open-joiner", "countdown-cancelled-joiner", "last-ready-joiner"]}
    checks = [dict(id=mark, peer=peer, mark=mark, screen="MultiplayerScreen", what="Each lobby state named its next action as the player's hand changed it.",
                   events=[f"^video_mark {mark}"]) for peer, names in marks.items() for mark in names]
    checks += scene_checks("host", host_steps) + scene_checks("joiner", joiner_steps)
    emit(17, "Ready and Cancel Ready, a setup change, the setup stopping a countdown and the last Ready starting at once",
         host, joiner, game_probe(host_steps), menu_probe(joiner_steps), checks, timeout=480)


def countdown_zero_scene():
    """G2: an unready player does not hold the match: the countdown reaches zero and starts it with everyone in the lobby."""
    host = host_setup() + ["activate ButtonMultiplayerStart", "wait_label LabelMultiplayerStatus Starting in 30 s",
            "video_mark countdown-30-host", "wait_label LabelMultiplayerStatus Starting in 2 s", "video_mark countdown-2-host"] + lobby_exit()
    joiner = join_setup() + ["wait_label LabelMultiplayerStatus The host is starting the match in 30 s - press Ready", "video_mark countdown-30-joiner",
              "wait_label LabelMultiplayerStatus The host is starting the match in 2 s", "video_mark countdown-2-joiner",
              "assert_label ButtonMultiplayerReady Ready"] + lobby_exit()
    host_steps, joiner_steps = place("host"), place("joiner")
    host_steps += pause_end() + [lobby_wait("host"), dict(op="finish")]
    joiner_steps += [lobby_wait("joiner"), dict(op="finish")]
    checks = [dict(id=mark, peer=peer, mark=mark, screen="MultiplayerScreen", what="The countdown ran to its end on both screens with the joiner unready.",
                   events=[f"^video_mark {mark}"]) for peer, mark in (("host", "countdown-30-host"), ("host", "countdown-2-host"),
                                                                      ("joiner", "countdown-30-joiner"), ("joiner", "countdown-2-joiner"))]
    checks += scene_checks("host", host_steps) + scene_checks("joiner", joiner_steps)
    emit(18, "The countdown reaches zero with a player unready and starts the match with everyone",
         host, joiner, game_probe(host_steps), game_probe(joiner_steps), checks, timeout=420)


def default_brain_loss_scene():
    """G4: a brain lost under the default rule ("Keep playing, humans spectate"). With two sides the loser's side is gone, so the
    activity settles the round on the side left standing (SkirmishDefense.lua's spectating rule); both players return to the
    lobby, which names the loss in a player's words."""
    host = place("host", x=0.4) + buy("host", fire=False, landing_offset=-180, craft="Base.rte/Dropship MK1")
    joiner = place("joiner", x=0.48)
    host += [dict(op="aim_brain", input_player=0, target_player=1)]
    host += capture("aimed-at-brain-host") + [dict(op="game_mouse", down=True),
             dict(op="aim_brain", input_player=0, target_player=1, until_end=True, scope="menu", within_ms=30000), dict(op="game_mouse", down=False)]
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [lobby_wait(peer)] + capture(f"brain-loss-{peer}", "MultiplayerScreen")
        steps += [menu("assert_label LabelLastMatchSummary Joiner's brain was destroyed"), menu("assert_label_absent LabelLastMatchSummary .lua"),
                  menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
    checks = scene_checks("host", host) + scene_checks("joiner", joiner)
    checks += [dict(id=f"default-rule-loss-{peer}", peer=peer, mark=f"brain-loss-{peer}", screen="MultiplayerScreen",
                    what="Under the default rule the lost brain ended the two-sided round and the lobby names it in a player's words.",
                    log_regex=[r"\[net-match\] brain lost: Joiner's Brain Case .* at tick "]) for peer in ("host", "joiner")]
    emit(20, "A brain lost under the default rule: the two-sided round settles on the side left and the lobby names the loss",
         game_menu("host"), game_menu("joiner"), game_probe(host), game_probe(joiner), checks,
         matching_log_lines=[r"^\[net-match\] brain lost: .+$", r"^\[net-match\] activity end: .+$"])


def second_rematch_scene():
    """G7: a rematch plays a second round from the rematch lobby, and the lobby returns again after it."""
    host, joiner = place("host"), place("joiner")
    host += pause_end() + [lobby_wait("host")] + capture("rematch-lobby-host", "MultiplayerScreen")
    joiner += [lobby_wait("joiner")] + capture("rematch-lobby-joiner", "MultiplayerScreen")
    joiner += [menu("activate ButtonMultiplayerReady"), wait(scope="menu", control="ButtonMultiplayerReady", text_contains="Cancel Ready")]
    host += [wait(scope="menu", control="LabelMultiplayerStatus", text_contains="Everyone is ready - press Start Match", within_ms=90000),
             menu("activate ButtonMultiplayerStart")]
    host += place("host", tag="-round2") + pause_end() + [lobby_wait("host")] + capture("second-lobby-host", "MultiplayerScreen")
    joiner += place("joiner", tag="-round2") + [lobby_wait("joiner")] + capture("second-lobby-joiner", "MultiplayerScreen")
    for peer, steps in (("host", host), ("joiner", joiner)):
        steps += [menu("assert_label LabelLastMatchSummary Match ended by host"), menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
    emit(22, "A second rematch round plays and returns to the lobby again",
         game_menu("host"), game_menu("joiner"), game_probe(host, 480000), game_probe(joiner, 480000),
         scene_checks("host", host) + scene_checks("joiner", joiner), timeout=560)


# A newcomer's settings: the harness pins (the public directory and no router mapping) and nothing else.
NEWCOMER_SETTINGS = {"SessionDirectoryUrl": "directory.broserver.com", "NetworkHostRelayMode": "Directory", "NetworkPortMapEnable": 0}


def relay_by_hand(peer):
    """A newcomer's first visit to Settings > Network: the defaults read as shipped, then Relay only picked by hand for the gate's relay."""
    return ["activate ButtonMainToOptions", "wait_ms 600", "select_settings_page Network", "wait_ms 450", "select_settings_page Network:Basics", "wait 3",
            f"video_mark network-defaults-{peer}", "assert_label LabelMatchStatusWidget Match status box",
            "assert_label LabelMatchStatusWidgetHint Shows the match's state while someone is placing, waiting or away",
            "assert_text_fits LabelMatchStatusWidgetHint", "assert_label LabelNetworkConnectionValue Automatic", "assert_label ComboMatchStatusWidget When needed",
            "assert_text_fits LabelNetworkConnectionValue", "assert_text_fits LabelMatchStatusWidget",
            # Outside a match the Files page reads this computer's own hosting default, under that name.
            "activate ButtonNetworkAdvanced", "wait 4", "select_settings_page Network:Files", "wait 4", f"video_mark files-default-{peer}",
            "assert_label LabelNetAutosaveHost Your hosting default", "assert_label LabelNetAutosave On", "assert_label LabelNetAutosaveInterval Every 5 minutes",
            "assert_text_fits LabelNetAutosaveHost", "select_settings_page Network:Basics", "wait 3", "activate ButtonNetworkConnectionChange", "wait 4",
            f"video_mark relay-by-hand-{peer}", "assert_label ComboNetworkConnection Automatic", "combo_select ComboNetworkConnection Relay only", "wait_ms 600",
            "assert_label ComboNetworkConnection Relay only", "assert_text_fits LabelNetworkConnectionHint", "post_command ButtonBackToMainMenu", "wait_ms 900",
            "assert_screen MainScreen"]


def newcomer_scene(number, size):
    """G8: a newcomer's defaults at one window size: Host a Game, Create Lobby, Join, Ready, Start, place, play."""
    host = ["wait_ms 1800"] + relay_by_hand("host") + [
        "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing", "settext TextMultiplayerName Captain", "activate ButtonMultiplayerHostGame",
        "wait_ms 500", "assert_substate HostSetup", "video_mark host-setup-defaults", "assert_text_fits ButtonMultiplayerCreate", "setup_host_port {PORT}",
        "activate ButtonMultiplayerCreate", "wait_substate Lobby 90", "write_lobby {PROBE_DIR}/lobby.json 90", "video_mark fight15-host-listening",
        "wait_connected 2 90", "wait_ms 1200", "wait_remote_ready 90", "activate ButtonMultiplayerStart"] + lobby_exit()
    joiner = ["wait_ms 1800"] + relay_by_hand("joiner") + join_setup()[1:] + ["activate ButtonMultiplayerReady"] + lobby_exit()
    host_steps, joiner_steps = place("host"), place("joiner")
    for peer, steps in (("host", host_steps), ("joiner", joiner_steps)):
        steps += complete_live_scene(peer, number)
    checks = [dict(id=f"network-defaults-{peer}", peer=peer, mark=f"network-defaults-{peer}", screen="SettingsScreen",
                   what="A newcomer's Settings > Network reads Automatic and When needed.",
                   events=["^assert_label LabelNetworkConnectionValue \"Automatic\".*PASS", "^assert_label ComboMatchStatusWidget \"When needed\".*PASS"])
              for peer in ("host", "joiner")]
    checks += [dict(id=f"relay-by-hand-{peer}", peer=peer, mark=f"relay-by-hand-{peer}", screen="SettingsScreen",
                    what="The connection reads Automatic until the hand picks Relay only for the gate's relay.",
                    events=["^assert_label ComboNetworkConnection \"Automatic\".*PASS", "^assert_label ComboNetworkConnection \"Relay only\".*PASS"])
               for peer in ("host", "joiner")]
    checks += [dict(id=f"files-default-{peer}", peer=peer, mark=f"files-default-{peer}", screen="SettingsScreen",
                    what="Outside a match the Files page shows this computer's hosting default, on every five minutes, under its own name.",
                    events=["^assert_label LabelNetAutosaveHost \"Your hosting default\".*PASS", "^assert_label LabelNetAutosave \"On\".*PASS",
                            "^assert_label LabelNetAutosaveInterval \"Every 5 minutes\".*PASS"]) for peer in ("host", "joiner")]
    checks += scene_checks("host", host_steps) + scene_checks("joiner", joiner_steps)
    emit(number, f"A newcomer's defaults at {size}: Host a Game, Create Lobby, Join, Ready, Start and play",
         host, joiner, game_probe(host_steps), game_probe(joiner_steps), checks, size=size, settings=NEWCOMER_SETTINGS, timeout=420)


def lobby_scene_pair(host_extra, joiner_extra=()):
    """A lobby both players open by hand; the host's and joiner's own checks follow, then both leave it."""
    host = host_setup() + list(host_extra) + ["touch_file {PROBE_DIR}/scene-end.json", "wait_file {PROBE_DIR_joiner}/scene-end.json 120"] + lobby_exit()
    joiner = join_setup() + list(joiner_extra) + ["touch_file {PROBE_DIR}/scene-end.json", "wait_file {PROBE_DIR_host}/scene-end.json 300"] + lobby_exit()
    done = probe([dict(op="wait_file", path="{PROBE_DIR}/scene-end.json", scope="menu", within_ms=300000), dict(op="assert_relay", scope="menu"),
                  dict(op="finish")], 320000)
    return host, joiner, done


def marked(mark, peer, what, events):
    return dict(id=mark, peer=peer, mark=mark, screen="MultiplayerScreen", what=what, events=events)


def setup_doors_scene():
    """D8 and D9: Edit setup is the game's setup alone; Advanced holds every page; ending the session asks first."""
    host = ["activate ButtonLobbyEditSetup", "wait_ms 600", "assert_substate HostOptions", "video_mark edit-setup-host",
            "assert_label LabelHostOptionsTitle E D I T   S E T U P", "assert_visible TabHostPageSeats 1", "assert_visible TabHostPageRules 1",
            "assert_visible TabHostPageConnection 0", "assert_visible TabHostPageTiming 0", "assert_visible TabHostPageRecovery 0",
            "assert_visible TabHostPageFiles 0", "assert_visible TabHostPageSession 0", "assert_visible ButtonHostOptDefaults 0",
            "activate ButtonHostOptBack", "wait_substate Lobby 30",
            "activate ButtonLobbyOptions", "wait_ms 600", "assert_substate HostOptions", "video_mark advanced-host",
            "assert_label LabelHostOptionsTitle A D V A N C E D", "assert_visible TabHostPageConnection 1", "assert_visible TabHostPageSession 1",
            "activate TabHostPageSession", "wait_ms 600", "activate ButtonHostSessEnd", "wait_ms 600", "video_mark end-confirm-host", "assert_substate HostOptions",
            "assert_label ButtonHostSessEnd Confirm end", "assert_label LabelHostSessEndHint Press Confirm end to close the game for every player",
            "assert_text_fits LabelHostSessEndHint", "activate TabHostPageRules", "wait_ms 400", "activate TabHostPageSession",
            "wait_ms 600", "assert_label ButtonHostSessEnd End session", "activate ButtonHostOptBack", "wait_substate Lobby 30"]
    joiner = ["activate ButtonLobbyEditSetup", "wait_ms 600", "assert_substate HostOptions", "video_mark match-details-joiner",
              "assert_label LabelHostOptionsTitle M A T C H   D E T A I L S", "assert_visible TabHostPageTiming 1",
              "activate ButtonHostOptBack", "wait_substate Lobby 30"]
    host, joiner, done = lobby_scene_pair(host, joiner)
    checks = [marked("edit-setup-host", "host", "Edit setup shows the game's setup alone, under its own title.",
                     ["^assert_label LabelHostOptionsTitle \"E D I T   S E T U P\".*PASS", "^assert_visible TabHostPageConnection 0.*PASS"]),
              marked("advanced-host", "host", "Advanced shows every page under its own title.", ["^assert_label LabelHostOptionsTitle \"A D V A N C E D\".*PASS"]),
              marked("end-confirm-host", "host", "End session asks for a second press and names its consequence; another page disarms it.",
                     ["^assert_label ButtonHostSessEnd \"Confirm end\".*PASS", "^assert_label ButtonHostSessEnd \"End session\".*PASS"]),
              marked("match-details-joiner", "joiner", "A joined player still reads every page of the match's details.",
                     ["^assert_label LabelHostOptionsTitle \"M A T C H   D E T A I L S\".*PASS"])]
    emit(26, "Edit setup and Advanced are two doors; ending the session asks first", host, joiner, done, done, checks, timeout=300)


TIMING_HINTS = {"ComboHostNetPolicy": "Automatic sizes each player's delay from their ping; Fixed uses the numbers you type",
                "TextHostNetMinDelay": "No player's delay drops below this; a higher floor rides out spikes but every press lands later",
                "TextHostNetSlowBound": "How late a player may fall before the AI holds their seat; lower keeps everyone else smooth",
                "ComboHostNetRedundancy": "Each packet repeats this many earlier ticks; more rides out lost packets but uses more bandwidth"}


def timing_page_scene():
    """D10 and D12: the Timing page has no control that does nothing, and each row says what its choice costs."""
    host = ["activate ButtonLobbyOptions", "wait_ms 600", "assert_substate HostOptions", "activate TabHostPageTiming", "wait_ms 600", "video_mark timing-host",
            "assert_no_caption CollectionBoxHostPageTiming Recalculate", "assert_visible LabelHostNetPing 1", "assert_text_fits LabelHostNetPing",
            "assert_label_absent LabelHostNetSlowPolicyHint later version"]
    for control, hint in TIMING_HINTS.items():
        host += [f"focus {control}", "wait_ms 300", f"assert_label LabelHostNetOptionHint {hint}", "assert_text_fits LabelHostNetOptionHint"]
    host += ["activate ButtonHostOptBack", "wait_substate Lobby 30"]
    host, joiner, done = lobby_scene_pair(host)
    checks = [marked("timing-host", "host", "The Timing page holds no do-nothing control; each row's hint names what its choice costs.",
                     ["^assert_no_caption CollectionBoxHostPageTiming .*PASS"] + [f"^assert_label LabelHostNetOptionHint .*PASS" for _ in TIMING_HINTS])]
    emit(27, "The Timing page: no do-nothing control, a cost hint for every row", host, joiner, done, done, checks, timeout=300)


HOSTING_HINTS = {"RadioNetworkDelayAuto": "Automatic sizes each player's delay from their ping; Fixed uses the frames below",
                 "TextNetworkIdleWait": "Minutes an open lobby waits with nobody joining before it closes",
                 "TextNetworkPathHorizon": "Ticks AI route searches may take; longer avoids pauses on slow machines, AI orders start later",
                 "CheckboxNetworkAutoRepair": "A match that falls out of step reloads the host's copy on every machine"}


def hosting_pages(peer):
    """Settings > Network: the Hosting page edits only the games this computer hosts, each row saying what it costs."""
    lines = ["activate ButtonMainToOptions", "wait_ms 600", "select_settings_page Network", "wait_ms 450", "select_settings_page Network:Basics", "wait 3",
             "activate ButtonNetworkAdvanced", "wait 4", "select_settings_page Network:Player", "wait 4", f"video_mark hosting-page-{peer}",
             "assert_visible CheckboxNetworkToasts 0", "assert_visible CheckboxNetworkPrediction 0", "assert_visible CheckboxNetworkDiagnostics 0"]
    for control, hint in HOSTING_HINTS.items():
        lines += [f"focus {control}", "wait_ms 300", f"assert_label LabelNetworkHostingHint {hint}", "assert_text_fits LabelNetworkHostingHint"]
    lines += ["select_settings_page Network:Chat", "wait 4", f"video_mark in-match-page-{peer}", "assert_label TabNetPageChat In match",
              "assert_visible CheckboxNetworkToasts 1", "assert_visible CheckboxNetworkPrediction 1", "assert_visible CheckboxNetworkDiagnostics 1",
              "assert_label LabelNetworkPredictionHint Your own units answer at once; the match confirms each move a moment later",
              "assert_text_fits LabelNetworkPredictionHint", "post_command ButtonBackToMainMenu", "wait_ms 900", "assert_screen MainScreen"]
    return lines


def hosting_page_scene():
    """D12: this computer's hosting defaults sit apart from its in-match options, and the router row says what it opens."""
    host = ["wait_ms 1800"] + hosting_pages("host") + host_setup()[1:]
    host[host.index("activate TabHostPageConnection"):host.index("activate TabHostPageConnection") + 1] = [
        "activate TabHostPageConnection", "assert_label CheckHostPortMap Ask my router to open the game port - players connect directly",
        "assert_text_fits CheckHostPortMap"]
    host += ["touch_file {PROBE_DIR}/scene-end.json", "wait_file {PROBE_DIR_joiner}/scene-end.json 120"] + lobby_exit()
    joiner = ["wait_ms 1800"] + hosting_pages("joiner") + join_setup()[1:] + ["touch_file {PROBE_DIR}/scene-end.json", "wait_file {PROBE_DIR_host}/scene-end.json 300"] + lobby_exit()
    done = probe([dict(op="wait_file", path="{PROBE_DIR}/scene-end.json", scope="menu", within_ms=300000), dict(op="finish")], 320000)
    # Settings pages are recorded as the settings screen.
    checks = [dict(marked(f"hosting-page-{peer}", peer, "The Hosting page holds only hosting defaults, each with what it costs.",
                          ["^assert_visible CheckboxNetworkPrediction 0.*PASS", "^assert_label LabelNetworkHostingHint .*PASS"]), screen="SettingsScreen")
              for peer in ("host", "joiner")]
    checks += [dict(marked(f"in-match-page-{peer}", peer, "This computer's in-match options have their own page.",
                           ["^assert_label TabNetPageChat \"In match\".*PASS"]), screen="SettingsScreen") for peer in ("host", "joiner")]
    emit(28, "Hosting defaults and this computer's in-match options on their own pages; the router row says what it opens",
         host, joiner, done, done, checks, timeout=300)


def apply_for_held_seat_scene():
    """G5: a newcomer finds the match full, applies for the seat a leaver holds, and the host decides by hand: Decline, then Let join."""
    number = 21
    name = f"fight15-s{number:02d}"
    if SELECTED is not None and name not in SELECTED:
        return
    host_menu = host_setup() + ["wait_remote_ready 90", "activate ButtonMultiplayerStart", "wait_file {PROBE_DIR}/done.json 600", "wait_ms 1400", "exit"]
    leaver_menu = join_setup(name="Leaver") + ["activate ButtonMultiplayerReady", "wait_file {PROBE_DIR}/done.json 600", "wait_ms 1400", "exit"]
    # The newcomer comes once the leaver has gone, reads the full match in the list, and applies with the landing's own button.
    apply = ["wait_label LabelMultiplayerLandingStatus Apply to substitute", "assert_label ButtonMultiplayerReconnect Apply to Substitute",
             "assert_text_fits LabelMultiplayerLandingStatus", "activate ButtonMultiplayerReconnect"]
    newcomer_menu = ["wait_ms 1800", "wait_file {PROBE_DIR_leaver}/left.json 300", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
                     "settext TextMultiplayerName Newcomer", "activate ButtonMultiplayerJoinGame", "wait_ms 600", "assert_substate JoinSetup", *join_own_lobby(),
                     # A full match takes an application once the list shows the held seat; a hand reads that first.
                     "wait_label_absent LabelJoinSelected cannot be joined", "activate ButtonMultiplayerConnect", "video_mark newcomer-refused", *apply, "video_mark newcomer-applied",
                     "wait_label LabelMultiplayerLandingStatus The host declined your request for this seat", "video_mark newcomer-declined",
                     "touch_file {PROBE_DIR}/declined.json", "activate ButtonMultiplayerJoinGame", "wait_ms 600", *join_own_lobby(),
                     "wait_label_absent LabelJoinSelected cannot be joined", "activate ButtonMultiplayerConnect",
                     *apply, "video_mark newcomer-applied-again", "wait_file {PROBE_DIR_host}/done.json 600", "wait_ms 1400", "exit"]
    seat_row = lambda part: f"NetworkSeat{part}@Leaver"
    press = lambda control: [dict(op="mouse_down", control=control), wait(control=control, equals=dict(pushed=True)), dict(op="mouse_up", control=control),
                             wait(renders=4)]
    host = place("host") + key("F6") + [wait(panel_open=True), wait(held_peer="Leaver", within_ms=90000)]
    host += capture("leaver-held-host") + [dict(op="assert_control", control=seat_row("Detail"), text_contains="Left ", fits=True, inside="NetworkSeats")]
    # A hand waits to see the request: the hidden row keeps its last text once a request ends.
    host += [wait(control=seat_row("Applicant"), text_contains="Newcomer", equals=dict(visible=True), within_ms=180000)] + capture("newcomer-asks-host")
    # Decline: the first press names what the second does.
    host += press(seat_row("DeclineApplicant")) + [dict(op="assert_control", control=seat_row("DeclineApplicant"), text_contains="Confirm: decline")]
    host += press(seat_row("DeclineApplicant")) + capture("declined-host")
    host += [dict(op="wait_file", path="{PROBE_DIR_newcomer}/declined.json", within_ms=120000),
             wait(control=seat_row("Applicant"), text_contains="Newcomer", equals=dict(visible=True), within_ms=180000)]
    host += [dict(op="mouse_down", control=seat_row("Applicant"), item=0), wait(control=seat_row("Applicant"), equals=dict(selected=0)),
             dict(op="mouse_up", control=seat_row("Applicant"), item=0), wait(renders=4),
             dict(op="assert_control", control=seat_row("Substitute"), equals=dict(enabled=True), text_contains="Let Newcomer join")]
    host += press(seat_row("Substitute")) + [dict(op="assert_control", control=seat_row("Substitute"), text_contains="Confirm: let Newcomer join")]
    host += press(seat_row("Substitute")) + capture("newcomer-let-in-host")
    # The newcomer joins the running match through its image; the host watches its seat until the newcomer plays it.
    host += key("F6") + [wait(panel_open=False), wait(returned_peer="Newcomer", within_ms=180000), wait(sim_advanced=600, within_ms=120000)] + capture("newcomer-playing-host")
    host += [dict(op="assert", equals=dict(service="Running", local_actor_alive=True)), dict(op="assert_relay")] + pause_end() + [lobby_wait("host", other="Newcomer"), dict(op="finish")]
    leaver = place("joiner") + key("Escape") + [wait(screen="Pause"), menu("activate ButtonLeaveMatch"), wait(screen="PauseLeaveConfirm"),
                                                menu("activate ButtonLeaveConfirm"), wait(screen="MultiplayerScreen", scope="menu"),
                                                dict(op="signal", name="left", scope="menu")]
    leaver += capture("leaver-landing", "MultiplayerScreen")
    leaver += [dict(op="wait_file", path="{PROBE_DIR_host}/done.json", scope="menu", within_ms=900000), dict(op="finish")]
    newcomer = [wait(service="Running", within_ms=600000), wait(sim_advanced=300, within_ms=120000), dict(op="assert", equals=dict(service="Running")),
                dict(op="assert_relay")]
    # The newcomer holds the seat in the rematch lobby until the host has read it there.
    newcomer += capture("newcomer-playing") + [lobby_wait("newcomer", within_ms=300000), dict(op="finish")]
    peers = []
    scripts = {}
    for role, display, menu_lines, steps in (("host", "Captain", host_menu, host), ("leaver", "Leaver", leaver_menu, leaver), ("newcomer", "Newcomer", newcomer_menu, newcomer)):
        scripts[f"{role}.menu"] = "\n".join(menu_lines) + "\n"
        scripts[f"{role}.probe"] = game_probe(steps, 900000) if role != "newcomer" else menu_probe(steps, 900000)
        peers.append(dict(name=role, menu_script=f"{role}.menu", probe=f"{role}.probe", env=dict(CCCP_TEST_READBACK_TIMING="1"),
                          settings=dict(SETTINGS, NetworkDisplayName=display)))
    checks = scene_checks("host", host) + scene_checks("leaver", leaver) + scene_checks("newcomer", newcomer)
    checks += [dict(id=mark, peer="newcomer", mark=mark, screen="MultiplayerScreen", what="The newcomer reads the full match, applies by hand and is answered.",
                    events=[f"^video_mark {mark}"]) for mark in ("newcomer-refused", "newcomer-applied", "newcomer-declined", "newcomer-applied-again")]
    document = dict(schema=1, name=name, title="A newcomer applies for a held seat; the host declines, then lets them in",
                    port_base=49410 + number * 2, size="960x540", timeout_s=960, public_directory=True, peers=peers, scripts=scripts, checklist=checks)
    (ROOT / f"{name}.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8", newline="\n")


def pick_brain_by_hand(peer):
    """The brain chosen as a player chooses it: the placement editor opens on its own object picker, and the picker's group and
    object rows are clicked by hand, every press held across frames."""
    player = 0 if peer == "host" else 1
    return [wait(picker_open=True, player=player, within_ms=20000), wait(renders=10, elapsed_ms=800)] + capture(f"picker-open-{peer}") + [
            dict(op="picker_pick", input_player=0, group="Brains", preset="Base.rte/Brain Case", within_ms=30000),
            wait(picker_open=False, player=player, within_ms=20000)]


def picker_placement_scene():
    """G3: each player picks the brain in the editor's own picker by hand, then places it with a held click."""
    sides = {}
    for peer in ("host", "joiner"):
        steps = place(peer)
        at = next(index for index, step in enumerate(steps) if step.get("op") == "editor_pick")
        steps[at:at + 1] = pick_brain_by_hand(peer)
        sides[peer] = steps + complete_live_scene(peer, 19)
    checks = scene_checks("host", sides["host"]) + scene_checks("joiner", sides["joiner"])
    emit(19, "Each player picks the brain in the editor's own picker by hand and places it with a held click",
         game_menu("host"), game_menu("joiner"), game_probe(sides["host"]), game_probe(sides["joiner"]), checks)


def emit_players(number, title, roles, checklist, timeout=420):
    """A scene of more than two players: roles are (role, display name, menu lines, probe text) with the host first."""
    name = f"fight15-s{number:02d}"
    if SELECTED is not None and name not in SELECTED:
        return
    peers, scripts = [], {}
    for role, display, menu_lines, probe_text in roles:
        # Every relay check of the scene counts its own players; the probe's default is a two-player match.
        counts = {step.get("players", 2) for step in json.loads(probe_text)["steps"] if step.get("op") == "assert_relay"}
        if counts - {len(roles)}:
            raise ValueError(f"{name} {role}: a relay check counts {sorted(counts)} players in a {len(roles)}-player scene")
        scripts[f"{role}.menu"] = "\n".join(menu_lines) + "\n"
        scripts[f"{role}.probe"] = probe_text
        peers.append(dict(name=role, menu_script=f"{role}.menu", probe=f"{role}.probe", env=dict(CCCP_TEST_READBACK_TIMING="1"),
                          settings=dict(SETTINGS, NetworkDisplayName=display)))
    document = dict(schema=1, name=name, title=title, port_base=49410 + number * 2, size="960x540", timeout_s=timeout, public_directory=True,
                    peers=peers, scripts=scripts, checklist=checklist)
    (ROOT / f"{name}.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8", newline="\n")


def three_player_countdown_scene():
    """G2: in a three-player lobby a Ready taken back during the countdown does not stop it; at zero the match starts with everyone."""
    host = host_setup(players=3)
    host[host.index("wait_connected 2 90")] = "wait_connected 3 90"
    host += ["wait_label LabelMultiplayerStatus Waiting for Joiner and Joiner2 to press Ready", "activate ButtonMultiplayerStart",
             "wait_label LabelMultiplayerStatus Starting in", "assert_label ButtonMultiplayerStart Cancel Start", "video_mark countdown-host",
             "wait_file {PROBE_DIR_joiner}/ready.json 60", "wait_label LabelMultiplayerStatus waiting for Joiner2 to press Ready",
             "video_mark one-ready-host", "wait_file {PROBE_DIR_joiner}/cancelled.json 60",
             "wait_label LabelMultiplayerStatus waiting for Joiner and Joiner2 to press Ready", "assert_label LabelMultiplayerStatus Starting in",
             "assert_label ButtonMultiplayerStart Cancel Start", "video_mark ready-taken-back-host"] + lobby_exit()
    joiner = join_setup(connected=3) + ["wait_label LabelMultiplayerStatus The host is starting the match in", "activate ButtonMultiplayerReady",
              "wait_label ButtonMultiplayerReady Cancel Ready", "touch_file {PROBE_DIR}/ready.json", "video_mark ready-joiner", "wait_ms 2500",
              "activate ButtonMultiplayerReady", "wait_label ButtonMultiplayerReady Ready", "wait_label LabelMultiplayerStatus The host is starting the match in",
              "assert_label LabelMultiplayerStatus press Ready", "touch_file {PROBE_DIR}/cancelled.json", "video_mark ready-taken-back-joiner"] + lobby_exit()
    joiner2 = join_setup(connected=3, name="Joiner2") + ["wait_label LabelMultiplayerStatus The host is starting the match in",
               "video_mark countdown-joiner2", "assert_label ButtonMultiplayerReady Ready"] + lobby_exit()
    def started(peer, ends):
        steps = [wait(editing=True, within_ms=90000), dict(op="assert", equals=dict(service="Running")), dict(op="assert_relay", players=3)]
        steps += capture(f"started-{peer}") + (pause_end() if ends else []) + [wait(scope="menu", screen="MultiplayerScreen", within_ms=120000), dict(op="finish")]
        return steps
    sides = {"host": started("host", True), "joiner": started("joiner", False), "joiner2": started("joiner2", False)}
    checks = [dict(id=mark, peer=peer, mark=mark, screen="MultiplayerScreen", what="A Ready taken back left the countdown running for everyone.",
                   events=[f"^video_mark {mark}"]) for peer, mark in (("host", "countdown-host"), ("host", "one-ready-host"), ("host", "ready-taken-back-host"),
                                                                      ("joiner", "ready-joiner"), ("joiner", "ready-taken-back-joiner"), ("joiner2", "countdown-joiner2"))]
    for peer, steps in sides.items():
        checks += scene_checks(peer, steps)
    emit_players(29, "Three players: a Ready taken back does not stop the countdown, and zero starts the match",
                 [("host", "Captain", host, game_probe(sides["host"])), ("joiner", "Joiner", joiner, game_probe(sides["joiner"])),
                  ("joiner2", "Joiner2", joiner2, game_probe(sides["joiner2"]))], checks)


def end_session_scene():
    """D9: End session asks for a second press that names its consequence, and the second press ends the game for both players."""
    host = host_setup() + [
        "activate ButtonLobbyOptions", "wait_ms 600", "assert_substate HostOptions", "activate TabHostPageSession", "wait_ms 600",
        "activate ButtonHostSessEnd", "wait_ms 600", "video_mark end-armed-host", "assert_label ButtonHostSessEnd Confirm end",
        "assert_label LabelHostSessEndHint Press Confirm end to close the game for every player", "assert_text_fits LabelHostSessEndHint",
        "assert_substate HostOptions", "touch_file {PROBE_DIR}/armed.json",
        # Both probes read the relay route while the session still stands; then the second press ends it.
        "wait_file {PROBE_DIR}/done.json 120", "wait_file {PROBE_DIR_joiner}/done.json 120",
        "activate ButtonHostSessEnd", "wait_substate Landing 60", "video_mark session-ended-host", "wait_ms 1400", "exit"]
    joiner = join_setup() + [
        "wait_file {PROBE_DIR_host}/armed.json 180", "touch_file {PROBE_DIR}/armed.json",
        "wait_substate Landing 180", "video_mark session-ended-joiner", "wait_ms 1400", "exit"]
    sides = probe([dict(op="wait_file", path="{PROBE_DIR}/armed.json", scope="menu", within_ms=300000), dict(op="assert_relay", scope="menu"),
                   dict(op="finish")], 320000)
    checks = [marked("end-armed-host", "host", "The first press of End session asks for Confirm end and names who it closes the game for.",
                     ["^assert_label ButtonHostSessEnd \"Confirm end\".*PASS",
                      "^assert_label LabelHostSessEndHint \"Press Confirm end to close the game for every player\".*PASS"]),
              marked("session-ended-host", "host", "The second press ends the session and the host is back at the multiplayer menu.",
                     ["^video_mark session-ended-host"]),
              marked("session-ended-joiner", "joiner", "The joiner is back at the multiplayer menu when the host ends the session.",
                     ["^video_mark session-ended-joiner"])]
    emit(30, "Ending the session asks first, then closes the game for every player", host, joiner, sides, sides, checks, timeout=300)


def menu_scenes():
    own_seat_held_scene()
    seat_dialog_scene()
    ban_scene()
    recovery_defaults_scene()
    ready_scene()
    countdown_zero_scene()
    default_brain_loss_scene()
    second_rematch_scene()
    for number, size in ((23, "640x360"), (24, "960x540"), (25, "1280x720")):
        newcomer_scene(number, size)
    setup_doors_scene()
    timing_page_scene()
    hosting_page_scene()
    apply_for_held_seat_scene()
    picker_placement_scene()
    three_player_countdown_scene()
    end_session_scene()


def main(argv):
    """Writes the named scenes, s01 to s32, or every scene this file owns with 'all'.

    fight15-s10's committed file carries edits made after ai_fight, so 'all' leaves it alone; name s10 to rewrite it.
    """
    global SELECTED
    owned = [f"s{number:02d}" for number in range(1, 33)]
    names = [name for name in owned if name != "s10"] if argv == ["all"] else argv
    unknown = [name for name in names if name not in owned]
    if not names or unknown:
        print(main.__doc__.strip() + ("\nunknown: " + " ".join(unknown) if unknown else ""))
        return 2
    SELECTED = {f"fight15-{name}" for name in names}
    lobby_scenes()
    game_scenes()
    if "fight15-s10" in SELECTED:
        ai_fight()
    menu_scenes()
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv[1:]))
