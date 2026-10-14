"""Write fight15-s11: the twenty-minute relay fight of fight15-s10 with four human players and no CPU seat.

The host and three joiners each take one box. The joiners come into the lobby one after another, so each holds the
seat of its order; the host starts a countdown the joiners answer with Ready; every player installs its brain (three
in bunkers, the last on open ground), buys a soldier through the brain's pie menu and fires it; each minute every
player chats; after the twenty minutes the host's soldier destroys the open-ground brain, which the others see, and
the host ends the match.
"""
import json

from fight15_scenes import ROOT, SETTINGS, buy, capture, click, game_probe, key, menu, pause_end, say, scene_checks, wait

PLAYERS = ("host", "joiner1", "joiner2", "joiner3")
NAMES = {"host": "Captain", "joiner1": "Joiner1", "joiner2": "Joiner2", "joiner3": "Joiner3"}
SEAT = {peer: index for index, peer in enumerate(PLAYERS)}
# Every brain lies apart from every soldier's line of fire; the open-ground brain is the host soldier's last target.
BRAIN_X = {"host": 0.3, "joiner1": 0.7, "joiner2": 0.1, "joiner3": 0.4}
BUNKERED = ("host", "joiner1", "joiner2")
LOSER = "joiner3"
# Each first burst aims at a bunkered brain, so it ends in the ground on the way.
FIRST_TARGET = {"host": "joiner1", "joiner1": "host", "joiner2": "host", "joiner3": "joiner1"}
LANDING_OFFSET = {"host": -180, "joiner3": -180}
TICKS = 72000


def others(peer):
    return [other for other in PLAYERS if other != peer]


def host_menu():
    lines = ["wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
             "settext TextMultiplayerName Captain", "activate ButtonMultiplayerHostGame", "wait_ms 500", "assert_substate HostSetup",
             "combo_select ComboHostActivity Skirmish Defense", "combo_select ComboHostScene Grasslands",
             "setup_host_port {PORT}", "combo_select ComboHostPlayers 4",
             "activate ButtonHostOptions", "wait_ms 500", "assert_substate HostOptions", "activate TabHostPageConnection",
             "combo_select ComboHostNetVisibility Public (default)", "combo_select ComboHostNetRelay Game service (default)",
             "activate TabHostPageSeats", "video_mark four-seats-host"]
    lines += [f"assert_label ComboHostSeatType{seat} Open" for seat in range(4)]
    lines += ["assert_label LabelHostSeatPlayers Human players", "assert_label ComboHostSeatPlayers 4", "wait_ms 1400",
              "activate TabHostPageConnection", "assert_label ComboHostNetVisibility Public (default)",
              "assert_label ComboHostNetRelay Game service (default)",
              # These are stock activity options, as in the two-player fight.
              "activate TabHostPageRules", "slider_set SliderHostRulesDifficulty 0", "slider_set SliderHostRulesGold 31000",
              "setcheck CheckHostRulesClearPath 0", "activate ButtonHostOptApply", "wait_ms 400",
              "activate ButtonHostOptBack", "wait_ms 500", "activate ButtonMultiplayerCreate", "wait_substate Lobby 90",
              "video_mark fight15-host-listening", "wait_connected 4 300", "wait_ms 1200", "video_mark four-players-host"]
    lines += [f"assert_label LabelLobbyPlayer{SEAT[peer]} {NAMES[peer]}" for peer in PLAYERS]
    lines += [f"assert_text_fits LabelLobbyPlayer{seat}" for seat in range(4)]
    # Nobody is Ready yet: the Start counts down, and the joiners answer it.
    lines += ["activate ButtonMultiplayerStart", "wait_label LabelMultiplayerStatus Starting in", "video_mark countdown-host",
              "assert_label ButtonMultiplayerStart Cancel Start", "assert_text_fits LabelMultiplayerStatus",
              "touch_file {PROBE_DIR}/countdown.mark", "wait_file {PROBE_DIR}/done.json 2000", "wait_ms 1400", "exit"]
    return lines


def joiner_menu(peer):
    order = SEAT[peer]
    lines = ["wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
             f"settext TextMultiplayerName {NAMES[peer]}", "activate ButtonMultiplayerJoinGame", "wait_ms 600", "assert_substate JoinSetup"]
    if order > 1:
        # The one before is in the lobby first, so the seats follow the joiners' order.
        lines += [f"wait_file {{PROBE_DIR_joiner{order - 1}}}/joined.mark 300"]
    lines += ["wait_row PublicGameRowPort{PORT} 90", "click_row PublicGameRowPort{PORT}", "assert_label LabelJoinSelected internet",
              "assert_enabled ButtonMultiplayerConnect 1", "activate ButtonMultiplayerConnect", "wait_substate Lobby 90", "wait_ms 1200",
              "touch_file {PROBE_DIR}/joined.mark", "wait_file {PROBE_DIR_host}/countdown.mark 300",
              "wait_label LabelMultiplayerStatus The host is starting the match in", f"video_mark countdown-{peer}",
              "assert_text_fits LabelMultiplayerStatus", f"wait_ms {500 + 2000 * (order - 1)}", "activate ButtonMultiplayerReady",
              "wait_file {PROBE_DIR}/done.json 2000", "wait_ms 1400", "exit"]
    return lines


def setup(peer):
    """The bunker, then the brain: each player installs its brain after the one before it, so every count reads alike."""
    order, fraction = SEAT[peer], BRAIN_X[peer]
    steps = []
    if peer in BUNKERED:
        steps += [wait(editing=True), dict(op="editor_pick", input_player=0, brain=False, **{"class": "BunkerAssembly"}, preset="Brain Chamber Left 2x2 - 1"),
                  dict(op="editor_move", input_player=0, x_fraction=fraction, y=1104, remember="bunker"), wait(picker_open=False, player="local")]
        steps += capture(f"bunker-preview-{peer}")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Click to ADD a new object - Drag for precision"))]
        steps += click() + [wait(sim_advanced=90, elapsed_ms=1600)] + capture(f"bunker-installed-{peer}")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(placement_refused=False))]
    move = dict(op="editor_move", input_player=0, x_fraction=fraction)
    if peer in BUNKERED:
        # The cursor is the prefab's center (96, 96); its brain chamber is at (48, 48).
        move.update(relative_to="bunker", offset_x=-48, offset_y=-48)
    steps += [wait(editing=True), dict(op="assert_relay"), dict(op="editor_pick", input_player=0), move, wait(renders=5, elapsed_ms=1000)]
    if order:
        steps += [wait(setup_ready=order), wait(elapsed_ms=2500)]
    steps += capture(f"valid-preview-{peer}")
    steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Valid spot - click to INSTALL your brain")),
              dict(op="assert_net_ui_clear", player=order),
              dict(op="assert_control", control="LabelNetMatchStatus", text_contains=f"{order} of 4", fits=True, inside="BoxNetMatchStatus")]
    steps += click()
    if peer == "host":
        steps += [wait(setup_ready=1)] + capture("placed-host")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(ready=True, submitted=True, screen_text="READY to start - wait for others to finish...")),
                  dict(op="assert_net_ui_clear", player=0),
                  dict(op="assert_control", control="LabelNetMatchStatus", text_contains="1 of 4", fits=True, inside="BoxNetMatchStatus")]
    steps += key("Return")
    steps += [wait(editing=False, paused=False), dict(op="assert", equals=dict(setup_ready=4, setup_humans=4, local_actor_alive=True)),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=4))]
    return steps + capture(f"playing-{peer}")


def first_burst(peer):
    """The soldier comes down by rocket, the player takes it, aims at a bunkered brain and fires one burst."""
    steps = buy(peer, fire=False, landing_offset=LANDING_OFFSET.get(peer))
    steps += [dict(op="aim_brain", input_player=0, target_player=SEAT[FIRST_TARGET[peer]]),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, preset="Coalition.rte/Soldier Light", weapon="Coalition.rte/Assault Rifle"), remember="before-fire"),
              dict(op="game_mouse", down=True), wait(renders=5, sim_advanced=12)]
    steps += capture(f"fire-{peer}")
    steps += [dict(op="assert_scene", input_player=0, rounds_less_than="before-fire", equals=dict(alive=True, fired=True, weapon="Coalition.rte/Assault Rifle")),
              dict(op="game_mouse", down=False)]
    return steps


def fight(peer):
    steps = [dict(op="pan_scene", input_player=0, y=600, speed=8)] + capture(f"look-{peer}")
    steps += [dict(op="pan_scene", input_player=0, y=600, speed=8),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=4, screen_text="Select a body to switch control to..."))]
    steps += capture(f"ai-view-{peer}")
    for minute in range(1, 21):
        steps += [dict(op="measure_minute", within_ms=70000)] + say(f"{peer} answers minute {minute}")
        steps += [wait(chat_text_once=f"{other} answers minute {minute}") for other in others(peer)] + [wait(elapsed_ms=1500)]
        steps += capture(f"minute-{minute:02d}-{peer}")
        steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=4)), dict(op="assert_relay")]
    steps += [wait(service="Running", editing=False, paused=False, lockstep_frame_at_least=TICKS + 1000),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=4)), dict(op="assert_relay"),
              dict(op="signal", name="fight-ticks-complete")]
    steps += [dict(op="wait_file", path=f"{{PROBE_DIR_{other}}}/fight-ticks-complete.json") for other in others(peer)]
    steps += [dict(op="pan_scene", enabled=False), wait(renders=3, sim_advanced=90),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=4, screen_text=""))]
    return steps


def ending(peer):
    if peer == "host":
        target = SEAT[LOSER]
        steps = [dict(op="actor_next_until", input_player=0, preset="Coalition.rte/Soldier Light"),
                 dict(op="wait_scene", input_player=0, equals=dict(preset="Coalition.rte/Soldier Light", weapon="Coalition.rte/Assault Rifle")),
                 dict(op="aim_brain", input_player=0, target_player=target)]
        steps += capture("aimed-at-brain-host") + [dict(op="assert_scene", input_player=0,
                 equals=dict(alive=True, preset="Coalition.rte/Soldier Light", weapon="Coalition.rte/Assault Rifle"))]
        steps += [dict(op="game_mouse", down=True), dict(op="aim_brain", input_player=0, target_player=target, until_dead=True, within_ms=40000),
                  dict(op="game_mouse", down=False), dict(op="wait_scene", input_player=0, equals=dict(brain_count=3), within_ms=20000)]
        steps += capture("brain-loss-host") + [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=3))]
        steps += [dict(op="wait_file", path=f"{{PROBE_DIR_{other}}}/brain-loss-seen.json") for other in others(peer)]
        steps += pause_end()
        status = "to press Ready"
    else:
        steps = [dict(op="wait_scene", input_player=0, equals=dict(brain_count=3), within_ms=120000)]
        steps += capture(f"brain-loss-{peer}") + [dict(op="assert_scene", input_player=0, equals=dict(brain_count=3)),
                                                  dict(op="signal", name="brain-loss-seen")]
        status = "Press Ready when you're ready to play"
    steps += [wait(scope="menu", screen="MultiplayerScreen", control="LabelMultiplayerStatus", text_contains=status, within_ms=90000)]
    steps += capture(f"fight-ended-{peer}", "MultiplayerScreen") + [menu("assert_label LabelLastMatchSummary Match ended by host"),
                                                                    menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
    return steps


def checklist_items(peer, steps):
    checks = scene_checks(peer, steps)
    bunkers = [rf"\[net-match\] editor purchase applied: seat={SEAT[owner]} preset=Base.rte/Brain Chamber Left 2x2 - 1 funds=" for owner in BUNKERED]
    checks.append(dict(id=f"shared-bunkers-{peer}", peer=peer, mark=f"playing-{peer}", screen="game",
                       what="Every bunker placement reached the shared scene before the brains started playing.", log_regex=bunkers))
    checks.append(dict(id=f"twenty-minutes-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="MultiplayerScreen",
                       what="Twenty full running minutes at >=58 ticks/s with a living local player and the named end reason.",
                       continuous_minutes=dict(count=20, minimum_tps=58.0), log_regex=[r"\[fight15-scene\] minute=20 pace=", r"\[net-match\].*host"]))
    progress = [index for index, step in enumerate(steps) if step.get("lockstep_frame_at_least") or step.get("name") == "fight-ticks-complete" or
                (step["op"] == "wait_file" and "fight-ticks-complete" in step["path"])]
    checks.append(dict(id=f"running-ticks-{peer}", peer=peer, mark=f"minute-20-{peer}", screen="game",
                       what=f"The living player completes at least {TICKS} running ticks before any player ends the fight.",
                       probe_steps=progress, sim_progress=TICKS))
    checks.append(dict(id=f"fight-hashes-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="MultiplayerScreen",
                       what="Every running tick has matching shared component hashes on all four players, with complete uninterrupted coverage.",
                       gate="fight-hashes"))
    checks.append(dict(id=f"brain-loss-console-{peer}", peer=peer, mark=f"brain-loss-{peer}", screen="game",
                       what="The open-ground brain's loss is named with its owner and tick on every player.",
                       log_regex=[rf"\[net-match\] brain lost: {NAMES[LOSER]}'s Brain Case .* at tick "]))
    return checks


def menu_checks():
    pass_line = lambda command, text: rf'^{command} "{text}" text="{text}" PASS$'
    checks = [dict(id="four-seats-host", peer="host", mark="four-seats-host", screen="MultiplayerScreen",
                   what="All four seats are open for human players.",
                   events=[pass_line(f"assert_label ComboHostSeatType{seat}", "Open") for seat in range(4)] +
                          [pass_line("assert_label ComboHostSeatPlayers", "4")]),
              dict(id="four-players-host", peer="host", mark="four-players-host", screen="MultiplayerScreen",
                   what="The lobby names all four players in their joining order, each inside its row.",
                   events=[pass_line(f"assert_label LabelLobbyPlayer{SEAT[peer]}", NAMES[peer]) for peer in PLAYERS] +
                          [rf"^assert_text_fits LabelLobbyPlayer{seat} .*PASS" for seat in range(4)]),
              dict(id="countdown-host", peer="host", mark="countdown-host", screen="MultiplayerScreen",
                   what="The unready Start counts down: Cancel Start shows and the waiting line fits.",
                   events=[pass_line("assert_label ButtonMultiplayerStart", "Cancel Start"), r"^assert_text_fits LabelMultiplayerStatus .*PASS"])]
    checks += [dict(id=f"countdown-{peer}", peer=peer, mark=f"countdown-{peer}", screen="MultiplayerScreen",
                    what="The joiner sees the host's countdown and presses Ready during it.",
                    events=[r"^assert_text_fits LabelMultiplayerStatus .*PASS", r"^activate ButtonMultiplayerReady .*PASS"])
               for peer in others("host")]
    return checks


def main():
    base = json.loads((ROOT / "fight15-s10.json").read_text(encoding="utf-8"))
    template = base["peers"][0]
    scripts, peers, checks = {}, [], menu_checks()
    for peer in PLAYERS:
        scripts[f"{peer}.menu"] = "\n".join(host_menu() if peer == "host" else joiner_menu(peer)) + "\n"
        steps = setup(peer) + first_burst(peer) + fight(peer) + ending(peer)
        scripts[f"{peer}.probe"] = game_probe(steps, timeout=1900000)
        checks += checklist_items(peer, steps)
        peers.append(dict(name=peer, menu_script=f"{peer}.menu", probe=f"{peer}.probe", env=dict(template["env"]),
                          settings=dict(SETTINGS, NetworkDisplayName=NAMES[peer]), args=list(template["args"])))
    document = dict(schema=1, name="fight15-s11", title="Four players over the relay: countdown, brains, Buy and fire, twenty minutes, a brain loss",
                    port_base=base["port_base"] + 2, size=base["size"], timeout_s=2100, public_directory=True, peers=peers, scripts=scripts,
                    checklist=checks, fight_hash_gate=dict(peers=list(PLAYERS), running_ticks=TICKS))
    (ROOT / "fight15-s11.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
