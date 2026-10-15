"""Write fight15-s11 and fight15-s12: fight15-s10's twenty-minute stock endless AI fight with four, then three, human
players together on one side against the CPU team the co-op mode adds; and fight15-s13, its two-player co-op soldier turns alone.

The host and each joiner take one box each. The joiners come into the lobby one after another, so each holds the
seat of its order; the host starts a countdown the joiners answer with Ready; every player installs its brain in a
bunker after the one before it, buys a soldier through the brain's pie menu and fires it; each minute every player
chats; after the twenty minutes the host ends the match.
"""
import json

from fight15_scenes import ROOT, SETTINGS, buy, capture, click, game_probe, join_own_lobby, key, menu, pause_end, say, scene_checks, wait

NAMES = {"host": "Captain", "joiner1": "Joiner1", "joiner2": "Joiner2", "joiner3": "Joiner3"}
BUNKER_X = {"host": 0.3, "joiner1": 0.7, "joiner2": 0.1, "joiner3": 0.5}
# Every player buys its own kind of soldier, so a Next Actor press can stop only at the player's own.
SOLDIER = {"host": "Coalition.rte/Soldier Light", "joiner1": "Coalition.rte/Soldier Heavy", "joiner2": "Ronin.rte/Raider",
           "joiner3": "Browncoats.rte/Browncoat"}
TICKS = 72000
PLAYERS = SEAT = None


def players(count):
    global PLAYERS, SEAT
    PLAYERS = ("host", *(f"joiner{index}" for index in range(1, count)))
    SEAT = {peer: index for index, peer in enumerate(PLAYERS)}


def others(peer):
    return [other for other in PLAYERS if other != peer]


def relay():
    return dict(op="assert_relay", players=len(PLAYERS))


def host_menu():
    count = len(PLAYERS)
    lines = ["wait_ms 1800", "activate ButtonMainToMultiplayer", "wait_ms 600", "assert_substate Landing",
             "settext TextMultiplayerName Captain", "activate ButtonMultiplayerHostGame", "wait_ms 500", "assert_substate HostSetup",
             "combo_select ComboHostActivity Skirmish Defense", "combo_select ComboHostScene Grasslands",
             # Players versus AI seats every human on one team and adds the CPU team the fight is against.
             "combo_select ComboHostMode Players versus AI", "setup_host_port {PORT}", f"combo_select ComboHostPlayers {count}",
             "activate ButtonHostOptions", "wait_ms 500", "assert_substate HostOptions", "activate TabHostPageConnection",
             "combo_select ComboHostNetVisibility Public (default)", "combo_select ComboHostNetRelay Game service (default)",
             "activate TabHostPageSeats", "video_mark seats-host"]
    lines += [f"assert_label ComboHostSeatType{seat} Open" for seat in range(count)] + [f"assert_label ComboHostSeatType{count} CPU"]
    lines += ["assert_label LabelHostSeatPlayers Human players", f"assert_label ComboHostSeatPlayers {count}", "wait_ms 1400",
              "activate TabHostPageConnection", "assert_label ComboHostNetVisibility Public (default)",
              "assert_label ComboHostNetRelay Game service (default)",
              # These are stock activity options: Infinite gold selects Skirmish Defense's endless AI mode.
              "activate TabHostPageRules", "slider_set SliderHostRulesDifficulty 0", "slider_set SliderHostRulesGold 31000",
              "setcheck CheckHostRulesClearPath 0", "activate ButtonHostOptApply", "wait_ms 400",
              "activate ButtonHostOptBack", "wait_ms 500", "activate ButtonMultiplayerCreate", "wait_substate Lobby 90",
              "write_lobby {PROBE_DIR}/lobby.json 90",
              # The lobby counts the CPU team among its connected members, as fight15-s10's count does.
              "video_mark fight15-host-listening", f"wait_connected {count + 1} 300", "wait_ms 1200", "video_mark players-host"]
    lines += [f"assert_label LabelLobbyPlayer{SEAT[peer]} {NAMES[peer]}" for peer in PLAYERS]
    lines += [f"assert_text_fits LabelLobbyPlayer{seat}" for seat in range(count)]
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
    lines += [*join_own_lobby(), "assert_label LabelJoinSelected internet",
              "assert_enabled ButtonMultiplayerConnect 1", "activate ButtonMultiplayerConnect", "wait_substate Lobby 90", "wait_ms 1200",
              "touch_file {PROBE_DIR}/joined.mark", "wait_file {PROBE_DIR_host}/countdown.mark 300",
              "wait_label LabelMultiplayerStatus The host is starting the match in", f"video_mark countdown-{peer}",
              "assert_text_fits LabelMultiplayerStatus", f"wait_ms {500 + 2000 * (order - 1)}", "activate ButtonMultiplayerReady",
              "wait_file {PROBE_DIR}/done.json 2000", "wait_ms 1400", "exit"]
    return lines


def setup(peer):
    """The bunker, then the brain: each player installs its brain after the one before it, so every count reads alike."""
    order, fraction = SEAT[peer], BUNKER_X[peer]
    steps = [wait(editing=True), dict(op="editor_pick", input_player=0, brain=False, **{"class": "BunkerAssembly"}, preset="Brain Chamber Left 2x2 - 1"),
             dict(op="editor_move", input_player=0, x_fraction=fraction, y=1104, remember="bunker"), wait(picker_open=False, player="local")]
    steps += capture(f"bunker-preview-{peer}")
    steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Click to ADD a new object - Drag for precision"))]
    steps += click() + [wait(sim_advanced=90, elapsed_ms=1600)] + capture(f"bunker-installed-{peer}")
    steps += [dict(op="assert_editor", input_player=0, equals=dict(placement_refused=False))]
    # The cursor is the prefab's center (96, 96); its brain chamber is at (48, 48).
    steps += [wait(editing=True), relay(), dict(op="editor_pick", input_player=0),
              dict(op="editor_move", input_player=0, x_fraction=fraction, relative_to="bunker", offset_x=-48, offset_y=-48),
              wait(renders=5, elapsed_ms=1000)]
    if order:
        steps += [wait(setup_ready=order), wait(elapsed_ms=2500)]
    steps += capture(f"valid-preview-{peer}")
    steps += [dict(op="assert_editor", input_player=0, equals=dict(screen_text="Valid spot - click to INSTALL your brain")),
              dict(op="assert_net_ui_clear", player=order),
              dict(op="assert_control", control="LabelNetMatchStatus", text_contains=f"{order} of {len(PLAYERS)}", fits=True, inside="BoxNetMatchStatus")]
    steps += click()
    if peer == "host":
        steps += [wait(setup_ready=1)] + capture("placed-host")
        steps += [dict(op="assert_editor", input_player=0, equals=dict(ready=True, submitted=True, screen_text="READY to start - wait for others to finish...")),
                  dict(op="assert_net_ui_clear", player=0),
                  dict(op="assert_control", control="LabelNetMatchStatus", text_contains=f"1 of {len(PLAYERS)}", fits=True, inside="BoxNetMatchStatus")]
    steps += key("Return")
    steps += [wait(editing=False, paused=False), dict(op="assert", equals=dict(setup_ready=len(PLAYERS), setup_humans=len(PLAYERS), local_actor_alive=True)),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=len(PLAYERS)))]
    return steps + capture(f"playing-{peer}")


def watch(other):
    """This peer's own committed state shows the other player's seat on that player's soldier, then that player back on its brain."""
    return [dict(op="wait_scene", player=SEAT[other], equals=dict(preset=SOLDIER[other]), within_ms=300000),
            dict(op="wait_file", path=f"{{PROBE_DIR_{other}}}/soldier-done.json", within_ms=300000)]


def turn(peer):
    order = SEAT[peer]
    steps = []
    # One player at a time takes its soldier: the Next Actor presses pass a teammate's soldier only after its player is back on its
    # brain. Every peer watches each other player's switch, so a seat's selection is the same on every peer.
    for other in PLAYERS[:order]:
        steps += watch(other)
    # The soldier comes down by rocket, the player takes it and fires (a shot passes the shooter's own side), then goes back to
    # its brain in the bunker, where the AI's attack does not reach it.
    steps += buy(peer, soldier=SOLDIER[peer])
    steps += [dict(op="actor_next_until", input_player=0, preset="Base.rte/Brain Case"),
              dict(op="wait_scene", input_player=0, equals=dict(preset="Base.rte/Brain Case")),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, preset="Base.rte/Brain Case", brain_count=len(PLAYERS))),
              dict(op="signal", name="soldier-done")]
    for other in PLAYERS[order + 1:]:
        steps += watch(other)
    return steps


def fight(peer):
    steps = turn(peer) + [dict(op="pan_scene", input_player=0, y=600, speed=8)] + capture(f"look-{peer}")
    steps += [dict(op="pan_scene", input_player=0, y=600, speed=8),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=len(PLAYERS), screen_text="Select a body to switch control to..."))]
    steps += capture(f"ai-view-{peer}")
    for minute in range(1, 21):
        steps += [dict(op="measure_minute", within_ms=70000)] + say(f"{peer} answers minute {minute}")
        steps += [wait(chat_text_once=f"{other} answers minute {minute}") for other in others(peer)] + [wait(elapsed_ms=1500)]
        steps += capture(f"minute-{minute:02d}-{peer}")
        steps += [dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=len(PLAYERS))), relay()]
    steps += [wait(service="Running", editing=False, paused=False, lockstep_frame_at_least=TICKS + 1000),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=len(PLAYERS))), relay(),
              dict(op="signal", name="fight-ticks-complete")]
    steps += [dict(op="wait_file", path=f"{{PROBE_DIR_{other}}}/fight-ticks-complete.json") for other in others(peer)]
    steps += [dict(op="pan_scene", enabled=False), wait(renders=3, sim_advanced=90),
              dict(op="assert_scene", input_player=0, equals=dict(alive=True, brain_count=len(PLAYERS), screen_text=""))]
    return steps


def ending(peer):
    steps = pause_end() if peer == "host" else []
    status = "to press Ready" if peer == "host" else "Press Ready when you're ready to play"
    steps += [wait(scope="menu", screen="MultiplayerScreen", control="LabelMultiplayerStatus", text_contains=status, within_ms=90000)]
    steps += capture(f"fight-ended-{peer}", "MultiplayerScreen") + [menu("assert_label LabelLastMatchSummary Match ended by host"),
                                                                    menu("assert_text_fits LabelLastMatchSummary"), dict(op="finish")]
    return steps


def switch_check(peer):
    """Every player has taken, fired and left its own soldier before the host ends the match."""
    return [dict(op="wait_file", path=f"{{PROBE_DIR_{other}}}/soldier-done.json", within_ms=300000) for other in others(peer)] + \
        [dict(op="assert_scene", input_player=0, equals=dict(alive=True, preset="Base.rte/Brain Case", brain_count=len(PLAYERS))), relay()]


def checklist_items(peer, steps, minutes=True):
    checks = scene_checks(peer, steps)
    bunkers = [rf"\[net-match\] editor purchase applied: seat={SEAT[owner]} preset=Base.rte/Brain Chamber Left 2x2 - 1 funds=" for owner in PLAYERS]
    checks.append(dict(id=f"shared-bunkers-{peer}", peer=peer, mark=f"playing-{peer}", screen="game",
                       what="Every bunker placement reached the shared scene before the brains started playing.", log_regex=bunkers))
    if minutes:
        checks.append(dict(id=f"twenty-minutes-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="MultiplayerScreen",
                           what="Twenty full running minutes at >=58 ticks/s, living local player, AI weapon fire and named end reason.",
                           continuous_minutes=dict(count=20, minimum_tps=58.0), log_regex=[r"\[fight15-scene\] minute=20 pace=", r"\[net-match\].*host"]))
        progress = [index for index, step in enumerate(steps) if step.get("lockstep_frame_at_least") or step.get("name") == "fight-ticks-complete" or
                    (step["op"] == "wait_file" and "fight-ticks-complete" in step["path"])]
        checks.append(dict(id=f"running-ticks-{peer}", peer=peer, mark=f"minute-20-{peer}", screen="game",
                           what=f"The living player completes at least {TICKS} running ticks before any player ends the fight.",
                           probe_steps=progress, sim_progress=TICKS))
    checks.append(dict(id=f"fight-hashes-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="MultiplayerScreen",
                       what="Every running tick has matching shared component hashes on every player, with complete uninterrupted coverage.",
                       gate="fight-hashes"))
    return checks


def menu_checks():
    count = len(PLAYERS)
    pass_line = lambda command, text: rf'^{command} "{text}" text="{text}" PASS$'
    checks = [dict(id="seats-host", peer="host", mark="seats-host", screen="MultiplayerScreen",
                   what="Every human seat is open beside the CPU team the co-op mode adds.",
                   events=[pass_line(f"assert_label ComboHostSeatType{seat}", "Open") for seat in range(count)] +
                          [pass_line(f"assert_label ComboHostSeatType{count}", "CPU"), pass_line("assert_label ComboHostSeatPlayers", str(count))]),
              dict(id="players-host", peer="host", mark="players-host", screen="MultiplayerScreen",
                   what="The lobby names every player in their joining order, each inside its row.",
                   events=[pass_line(f"assert_label LabelLobbyPlayer{SEAT[peer]}", NAMES[peer]) for peer in PLAYERS] +
                          [rf"^assert_text_fits LabelLobbyPlayer{seat} .*PASS" for seat in range(count)]),
              dict(id="countdown-host", peer="host", mark="countdown-host", screen="MultiplayerScreen",
                   what="The unready Start counts down: Cancel Start shows and the waiting line fits.",
                   events=[pass_line("assert_label ButtonMultiplayerStart", "Cancel Start"), r"^assert_text_fits LabelMultiplayerStatus .*PASS"])]
    checks += [dict(id=f"countdown-{peer}", peer=peer, mark=f"countdown-{peer}", screen="MultiplayerScreen",
                    what="The joiner sees the host's countdown and presses Ready during it.",
                    events=[r"^assert_text_fits LabelMultiplayerStatus .*PASS", r"^activate ButtonMultiplayerReady .*PASS"])
               for peer in others("host")]
    return checks


# The four- and three-player twenty-minute fights; the two-player scene checks only that each co-op player takes its own soldier,
# with every tick hashed alike on both peers.
SCENES = {4: (11, "Four players together against the AI over the relay: countdown, brains, Buy and fire, twenty minutes", True),
          3: (12, "Three players together against the AI over the relay: countdown, brains, Buy and fire, twenty minutes", True),
          2: (13, "Two players together against the AI: each takes and fires its own purchased soldier, alike on both peers", False)}
SWITCH_TICKS = 3000


def main():
    base = json.loads((ROOT / "fight15-s10.json").read_text(encoding="utf-8"))
    template = base["peers"][0]
    for count, (number, title, minutes) in SCENES.items():
        players(count)
        scripts, peers, checks = {}, [], menu_checks()
        for peer in PLAYERS:
            scripts[f"{peer}.menu"] = "\n".join(host_menu() if peer == "host" else joiner_menu(peer)) + "\n"
            steps = setup(peer) + (fight(peer) if minutes else turn(peer) + switch_check(peer)) + ending(peer)
            scripts[f"{peer}.probe"] = game_probe(steps, timeout=1800000 if minutes else 900000)
            checks += checklist_items(peer, steps, minutes)
            peers.append(dict(name=peer, menu_script=f"{peer}.menu", probe=f"{peer}.probe", env=dict(template["env"]),
                              settings=dict(SETTINGS, NetworkDisplayName=NAMES[peer]), args=list(template["args"])))
        document = dict(schema=1, name=f"fight15-s{number}", title=title, port_base=49410 + number * 2, size=base["size"],
                        timeout_s=2100 if minutes else 1100, public_directory=True, peers=peers, scripts=scripts, checklist=checks,
                        fight_hash_gate=dict(peers=list(PLAYERS), running_ticks=TICKS if minutes else SWITCH_TICKS))
        (ROOT / f"fight15-s{number}.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
