"""Write door-fill: fight15-s10's stock endless AI fight, cut to three minutes on the direct route, with each player's
reinforced fill buried around its brain chamber and then laid over the chamber's closed door.

While a team's paths update, the game opens that team's doors in the terrain and closes them again, on each machine's own
ticks. Fill over a door is the terrain that override meets, so the scene checks every running tick's shared hashes,
terrain included, on both players while the AI re-routes around the filled doors. Twenty seconds into each door's life
a slit is dug through it (the CC_TEST_DOOR_DIG_AGE_MS lever), and both players must see the door whole again within
100 sim ms, the cadence of single player's path update.
"""
import json

from fight15_scenes import ROOT, click, wait

MINUTES = 3
TICKS = MINUTES * 3600
FILL = "Reinforced Concrete Fill"
# Offsets from the bunker's center: a ring around the chamber, then its lower right cell, which holds the closed door. The
# door's block goes in last, as the players turn to their brains.
LAYOUT = ((-144, -144), (-48, -144), (48, -144), (144, -144), (-144, -48), (-144, 48), (144, -48), (144, 48), (48, 48))
FILL_LINE = r"\[net-match\] editor purchase applied: seat={seat} preset=Base.rte/" + FILL + " funds="
DIG_AGE_MS = 20000
DUG_LINE = r"\[door-reseal\] door=\d+ team=\d+ dug=[1-9]\d* at_ms=\d+"
RESEALED_LINE = r"\[door-reseal\] door=\d+ team=\d+ resealed_after_ms=(\d|[1-9]\d|100)\b"
LATE_LINES = [r"\[door-reseal\] door=\d+ team=\d+ resealed_after_ms=(10[1-9]|1[1-9]\d|[2-9]\d\d|\d{4,})\b",
              r"\[door-reseal\] door=\d+ team=\d+ not resealed "]


def fill(player):
    steps = []
    for offset_x, offset_y in LAYOUT:
        steps += [wait(editing=True), dict(op="editor_pick", input_player=0, brain=False, **{"class": "TerrainObject"}, preset=FILL),
                  dict(op="editor_move", input_player=0, relative_to="bunker", offset_x=offset_x, offset_y=offset_y),
                  wait(picker_open=False, player=player)]
        steps += click() + [wait(sim_advanced=30, elapsed_ms=600), dict(op="assert_editor", input_player=0, equals=dict(placement_refused=False))]
    return steps


def shorten(steps, player):
    """The base's own fill and relay checks dropped, this layout right after the bunker, then the fight's own end once the
    round has run its ticks. The probe measures a fight's minutes only for twenty of them, so this one counts ticks.

    Answers the steps and each kept old index's new one."""
    dropped = {index for index, step in enumerate(steps) if step["op"] == "assert_relay"}
    for index, step in enumerate(steps):
        if step["op"] == "editor_pick" and step.get("preset") == FILL:
            dropped.update(range(index - 1, index + 9))
    bunker = next(index for index, step in enumerate(steps) if step == dict(op="assert_editor", input_player=0, equals=dict(placement_refused=False))) + 1
    minutes = [index for index, step in enumerate(steps) if step["op"] == "measure_minute"]
    end = next(index for index, step in enumerate(steps) if "lockstep_frame_at_least" in step)
    layout = fill(player)
    kept = [index for index in list(range(minutes[0])) + list(range(end, len(steps))) if index not in dropped]
    moved = {old: new + (len(layout) if old >= bunker else 0) for new, old in enumerate(kept)}
    out = [dict(steps[old]) for old in kept if old < bunker] + layout + [dict(steps[old]) for old in kept if old >= bunker]
    # The editor leaves the fight on whichever frame its waits end, so the end counts the fight's own ticks from there.
    del out[moved[end]]["lockstep_frame_at_least"]
    out[moved[end]].update(sim_advanced=TICKS, within_ms=(MINUTES + 2) * 60000)
    return out, moved


def main():
    scene = json.loads((ROOT / "fight15-s10.json").read_text(encoding="utf-8"))
    moves = {}
    for peer, player in (("host", 0), ("joiner", 1)):
        key = f"{peer}.probe"
        probe = json.loads(scene["scripts"][key])
        probe["steps"], moves[peer] = shorten(probe["steps"], player)
        scene["scripts"][key] = json.dumps(probe, indent=2) + "\n"
    checklist = []
    for item in scene["checklist"]:
        mark = item.get("mark", "")
        if item["id"].startswith("twenty-minutes-") or mark.startswith("minute-") and not item["id"].startswith("running-ticks-"):
            continue
        item = dict(item)
        if "probe_steps" in item:
            item["probe_steps"] = [moves[item["peer"]][index] for index in item["probe_steps"] if index in moves[item["peer"]]]
        if item["id"].startswith("running-ticks-"):
            item.update(mark=f"fight-ended-{item['peer']}", sim_progress=TICKS,
                        what=f"The living player completes at least {TICKS} running ticks before either peer ends the fight.")
        if item["id"].startswith("shared-bunkers-"):
            lines = [line for line in item["log_regex"] if FILL not in line]
            item.update(what="Both bunkers, their ring of fill and the fill over each closed door reached the shared scene before the brains started playing.",
                        log_regex=lines + [FILL_LINE.format(seat=seat) for seat in (0, 1)])
        checklist.append(item)
    for peer in ("host", "joiner"):
        checklist.append(dict(id=f"door-resealed-{peer}", peer=peer, mark=f"fight-ended-{peer}", screen="game", log_regex=[DUG_LINE, RESEALED_LINE],
                              forbidden_log_regex=LATE_LINES,
                              what="A slit dug through a closed door is whole again within 100 sim ms on this player, as single player's path update leaves it."))
    for peer in scene["peers"]:
        peer["settings"] = dict(peer["settings"], NetworkConnectionMode="Automatic")
        peer["env"] = dict(peer.get("env", {}), CC_TEST_DOOR_DIG_AGE_MS=str(DIG_AGE_MS))
    scene.update(name="door-fill", port_base=49474, timeout_s=900, checklist=checklist,
                 title="Reinforced fill over each brain chamber's closed door: every running tick's terrain alike on both players while the AI re-routes",
                 fight_hash_gate=dict(scene["fight_hash_gate"], running_ticks=TICKS))
    (ROOT / "door-fill.json").write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
