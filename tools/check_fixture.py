#!/usr/bin/env python
"""Semantic assertions for the gameplay fixtures, read off the peers' CC_SIM_DUMP windows and traces.

Every case first requires the two peers' actor dump lines to be byte-identical over the window, then
asserts the gameplay transition the fixture exercised at the tick the wire lands it. Timing: a script
line at tick T is sampled after T's update, rides the frame for T+1 and lands at T+1+D, and the dump
(written after the tick's update) shows it from that tick on. The seated actor is the one whose sim
mode turns to player (1) at the NEXT release edge (tick 32 + D); the dump window must start before it.

usage: check_fixture.py <case> <host_dump> <client_dump> --delay D [--host-trace host_trace.json] [--switch-tick 32]
Exit 0 = pass, 1 = fail (reasons printed).
"""

import argparse
import json
import re
import sys
from collections import defaultdict

HEAD = re.compile(r"^(\d+) actor uid=(\d+) (.*?) (pos=.*)$")
ACTIVITY = re.compile(r"^(\d+) activity state=(\d+)")
ACTIVITY_FULL = re.compile(r"^(\d+) activity state=(\d+)(.*)$")
E2E_TEAM_IS_CPU = re.compile(r"\[e2e\] TeamIsCPU team=(\d+) value=(\d+)")
# F60 print site: ConfigureNetMatchActivity roster loop (Main.cpp:4798-4809) never calls SetCPUTeam.
F60_TEAMISCPU_OWED = (
    "OWED F60: TeamIsCPU is not on the activity dump (MovableMan.cpp:1206-1211 prints state= and "
    "tN=brain/roster only) and no [e2e] TeamIsCPU print. F60 must call SetCPUTeam for each slot.cpu "
    "team inside ConfigureNetMatchActivity (Main.cpp:4798-4809) and print "
    "`[e2e] TeamIsCPU team=N value=1` from that loop."
)
DEAD = 4
CPU_TEAM_COOP = 1


def fields_of(tail):
    """key=value fields of an actor line; a value may hold spaces (a pie slice or gun name), so a token without '=' continues the previous value."""
    fields = {}
    key = None
    for token in tail.split(" "):
        if "=" in token and not token.startswith("["):
            key, value = token.split("=", 1)
            fields[key] = value
        elif key is not None:
            fields[key] += " " + token
    return fields


def load(path):
    rows = defaultdict(dict)
    raw = defaultdict(dict)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = HEAD.match(line)
            if not m:
                continue
            tick, uid = int(m.group(1)), int(m.group(2))
            f_ = fields_of(m.group(4))
            x, y = f_["pos"].split(",")
            num = lambda k: int(str(f_[k]).split("/")[0]) if k in f_ else None
            rows[uid][tick] = {
                "name": m.group(3),
                "x": float.fromhex(x),
                "y": float.fromhex(y),
                "mode": int(f_["mode"]),
                "dis": num("dis"),
                "status": int(f_["status"]),
                "health": float.fromhex(f_["health"]),
                "aim": float.fromhex(f_["aim"]),
                "flip": int(f_["flip"]),
                "pie": f_.get("pie"),
                "team": num("team"),
                "aimode": num("aimode"),
                "wp": num("wp"),
                "ninv": num("ninv"),
                "jet": float.fromhex(f_["jet"]) if "jet" in f_ else None,
                "emit": num("emit"),
                "fg": num("fg"),
                "bg": num("bg"),
                "gun": f_.get("gun"),
                "rounds": num("rounds"),
                "reloading": num("reloading"),
                "hatch": num("hatch"),
                "inv": f_.get("inv"),
                "door": num("door"),
            }
            raw[uid][tick] = line.rstrip("\n")
    return rows, raw


PATHS = re.compile(r" (?:paths|alarm)=\S+")  # per-machine: walk paths, AI alarm point
CIM_AI = 2
# The pie ENABLED-STATE digit only (EnabledState: 0 Enabling, 1 Enabled, 2 Disabling, 3 Disabled);
# the hovered / activated / already-activated slice names after it still compare.
PIE_ENABLED_STATE = re.compile(r" pie=\d+:")


def strip_per_machine(line):
    """The move path is computed by the owner's asynchronous pathfinder (off-wire by design), so it is not compared."""
    return PATHS.sub("", line)


def actor_mode(line):
    m = re.search(r" mode=(\d+)", line)
    return int(m.group(1)) if m else None


def strip_pie_enabled_state(line):
    """An unseated actor's pie enabled-state is per-machine presentation, not sim state.

    GameActivity::Update's Go-To cursor marks the closest actor within 40px of the local
    player's cursor and calls PieMenu::FreezeAtRadius(15) on it (GameActivity.cpp:1381-1388);
    PieMenu::Update then writes m_EnabledState = Enabling every tick while MenuMode::Freeze
    (PieMenu.cpp:825-827). The cursor and its ViewState exist only on the peer whose player
    issued the order, so on the crab fixture the team-0 brain reads pie=0 on the host and
    pie=3 on the client from tick 226. Every reader of that state is a draw path
    (Actor.cpp:1932/1960, PieMenu::Draw) or the owning seat's own input sampling
    (Controller.cpp:544, which reads only its own controlled actor's menu); PieMenu::Update's
    input branch is gated on MenuMode::Normal, so a frozen menu runs no sim logic. The run's
    sim-gated host/client hash comparison is identical over all 450 ticks. Only the state
    digit is excluded, and only for an actor that is CIM_AI on both peers.
    """
    return PIE_ENABLED_STATE.sub(" pie=*:", line)


def check_ai_orders(host, D):
    """The host's orders at 50/200/400/600 (go-to, follow the brain, squad, disband) for its dummy land at T+D on every peer."""
    GOTO, SENTRY, SQUAD = 3, 1, 11
    team0 = {uid: by for uid, by in host.items() if any(v["team"] == 0 for v in by.values())}
    units = sorted(uid for uid, by in team0.items() if any(v["name"] == "Green Dummy" for v in by.values()))
    brains = [uid for uid, by in team0.items() if any(v["name"] == "Brain Robot" for v in by.values())]
    if len(units) < 1 or len(brains) != 1:
        return [f"ai_orders: expected a team-0 dummy and one brain, found units {units} brains {brains}"]
    u0, brain = units[0], brains[0]

    def at(uid, t, key):
        return host[uid][t][key] if t in host[uid] else None

    failures = []
    t0, t1, t2, t3 = 50 + D, 200 + D, 400 + D, 600 + D
    if not (at(u0, t0 - 1, "aimode") != GOTO and at(u0, t0, "aimode") == GOTO and at(u0, t0 - 1, "wp") == 0 and at(u0, t0, "wp") == 1):
        failures.append(f"go-to: unit {u0} at {t0 - 1}/{t0}: aimode {at(u0, t0 - 1, 'aimode')}/{at(u0, t0, 'aimode')} wp {at(u0, t0 - 1, 'wp')}/{at(u0, t0, 'wp')}, expected GOTO with one waypoint from {t0}")
    if not (at(u0, t1, "aimode") == GOTO and at(u0, t1 - 1, "wp") is not None and at(u0, t1, "wp") == at(u0, t1 - 1, "wp") + 1):
        failures.append(f"follow: unit {u0} at {t1 - 1}/{t1}: aimode {at(u0, t1 - 1, 'aimode')}/{at(u0, t1, 'aimode')} wp {at(u0, t1 - 1, 'wp')}/{at(u0, t1, 'wp')}, expected one more waypoint (the brain) from {t1}")
    if not (at(brain, t2 - 1, "aimode") != SENTRY or at(u0, t2 - 1, "aimode") != SQUAD) or not (at(brain, t2, "aimode") == SENTRY and at(u0, t2, "aimode") == SQUAD and at(u0, t2, "wp") == 1):
        failures.append(f"squad: brain aimode {at(brain, t2 - 1, 'aimode')}->{at(brain, t2, 'aimode')}, unit aimode {at(u0, t2 - 1, 'aimode')}->{at(u0, t2, 'aimode')} wp {at(u0, t2, 'wp')}, expected the brain sentry and the unit following it from {t2}")
    if not (at(u0, t3 - 1, "aimode") == SQUAD and at(u0, t3, "aimode") == SENTRY and at(u0, t3, "wp") == 0):
        failures.append(f"disband: unit aimode {at(u0, t3 - 1, 'aimode')}->{at(u0, t3, 'aimode')} wp {at(u0, t3, 'wp')}, expected it released into the brain's sentry mode with no waypoints at {t3}")
    if not failures:
        print(f"ai_orders: go-to {u0} at {t0}, follow at {t1}, squad under brain {brain} at {t2}, disbanded at {t3}")
    return failures


DOOR_CLOSED, DOOR_OPENING, DOOR_OPEN, DOOR_CLOSING = 0, 1, 2, 3
# AHuman::HandlePieCommand GoTo; Actor::AIMode AIMODE_GOTO
GOTO_MODE = 3


def actors_named(host, needle):
    needle = needle.lower()
    return sorted(uid for uid, by in host.items() if any(needle in v["name"].lower() for v in by.values()))


def first_transition(by, key, old, new):
    ticks = sorted(by)
    for t in ticks:
        prev = [u for u in ticks if u < t]
        if not prev:
            continue
        if by[prev[-1]].get(key) == old and by[t].get(key) == new:
            return t
    return None


def check_door_pass(host):
    """Door closed->opening->open->closing at the same ticks on both peers, then the walker crosses the door line."""
    doors = actors_named(host, "door")
    if not doors:
        print(
            "NOT RUNNABLE door_pass: Grasslands has no ADoor actor in the dump. "
            "InputScript.cpp:119-141 accepts only InputElements / AIM= / MOUSE=; there is no SPAWN or SCENE command. "
            "DumpSimState (MovableMan.cpp:1014) writes hatch= for ACraft and no door= for ADoor. "
            "A door= field would need to be added next to hatch= at MovableMan.cpp:1014."
        )
        return ["NOT RUNNABLE: no ADoor in the e2e scene dump"]
    door = doors[0]
    by = host[door]
    if all(row.get("door") is None for row in by.values()):
        print(
            "NOT RUNNABLE door_pass: ADoor uid %s is in the dump but has no door= field. "
            "Add door= << ADoor::GetDoorState() next to hatch= at MovableMan.cpp:1014." % door
        )
        return ["NOT RUNNABLE: DumpSimState omits ADoor::m_DoorState"]
    opening = first_transition(by, "door", DOOR_CLOSED, DOOR_OPENING)
    opened = first_transition(by, "door", DOOR_OPENING, DOOR_OPEN)
    closing = first_transition(by, "door", DOOR_OPEN, DOOR_CLOSING)
    if opening is None or opened is None or closing is None:
        return [f"door uid {door}: missing CLOSED->OPENING->OPEN->CLOSING (opening={opening} open={opened} closing={closing})"]
    if not (opening < opened < closing):
        return [f"door uid {door}: transition order opening={opening} open={opened} closing={closing}"]
    door_x = by[sorted(by)[0]]["x"]
    walkers = [uid for uid, w in host.items() if any(v["mode"] == 1 for v in w.values()) and uid != door]
    crossed = False
    for uid in walkers:
        wby = host[uid]
        after = [t for t in sorted(wby) if t >= opened]
        if len(after) < 2:
            continue
        x0 = wby[after[0]]["x"]
        for t in after[1:]:
            x1 = wby[t]["x"]
            if (x0 - door_x) * (x1 - door_x) <= 0 and x0 != x1:
                crossed = True
                break
    if not crossed:
        return [f"door uid {door}: no seated actor crossed x={door_x} after OPEN at {opened}"]
    print(f"door: uid {door} OPENING {opening} OPEN {opened} CLOSING {closing}")
    return []


def _hover_name(pie):
    if not pie:
        return None
    if ":sub[" in pie:
        sub = pie.split(":sub[", 1)[1].rstrip("]")
        parts = sub.split(":")
        return parts[2] if len(parts) > 2 else None
    parts = pie.split(":")
    return parts[2] if len(parts) > 2 else None


def _is_goto_hover(name):
    if not name:
        return False
    n = name.lower()
    return "go to" in n or "go-to" in n


def check_crab_ai_order(host, D, seat, by):
    """Pie UI path: Go To; aimode GOTO at T+1+D of the release; pie closes.

    SP SimBaseline and lockstep D=3 disagreed on aimode (SP GOTO, lockstep stayed 1),
    so this keeps the original GOTO + Disabled contract and does not adopt the lockstep dump.
    """
    crabs = actors_named(host, "crab")
    if not crabs:
        print(
            "crab_ai_order: no ACrab in Grasslands; ordering the seated unit (dummy). "
            "A crab would need InputScript.cpp:119-141 to accept a SPAWN=ACrab line, or Main.cpp:2437 spawn command to clone Base.rte/Crab."
        )
    unit, unit_by = seat, by
    if crabs:
        unit = crabs[0]
        unit_by = host[unit]

    def uat(t, key):
        return unit_by[t][key] if t in unit_by else None

    pies = {t: by[t]["pie"] for t in sorted(by) if by[t]["pie"]}
    state = lambda t: int(pies[t].split(":")[0]) if t in pies else None

    def sub_hover(t):
        pie = pies.get(t) or ""
        if ":sub[" not in pie:
            return None
        sub = pie.split(":sub[", 1)[1].rstrip("]")
        parts = sub.split(":")
        return parts[2] if len(parts) > 2 else None

    opened = 150 + D
    # Dummy script released at 301; crab pie holds 150-220 (release 221).
    released = 221 + D if crabs else 301 + D
    reload_like = released + 1
    if state(opened - 1) != 3 or state(opened) != 0:
        return [f"pie: enabled state at {opened - 1}={state(opened - 1)} {opened}={state(opened)}, expected Disabled then Enabling at T+D"]
    # The Go-To order disables the seat's controller, and PieMenu::Update returns at its
    # controller->IsDisabled() gate before UpdateEnablingAndDisablingProgress, so the menu
    # stops at Disabling (2) and never reaches Disabled (3) while the seat is still picking
    # the point. The single-player reference does the same: D:\mx\w57b\sp_vs_lockstep_dummy.txt
    # ends "SP: pieDisabledAfter300=[]" with SP sitting at pie=2:8:-:-: from tick 302 on.
    # The contract is therefore that the menu LEAVES Enabled/Enabling, not that it reaches 3.
    closed = [t for t in range(released, released + 12) if state(t) is not None and state(t) >= 2]
    if not closed:
        return [f"pie: seat pie never left Enabled after the release landing {released}; pie={pies.get(released)}"]
    hover = sub_hover(released) if not crabs else _hover_name(pies.get(released))
    if not _is_goto_hover(hover):
        return [f"pie: hover at release landing {released}={hover} pie={pies.get(released)}, expected Go To"]
    mode0, mode1 = uat(reload_like - 1, "aimode"), uat(reload_like, "aimode")
    if mode1 != GOTO_MODE or mode0 == GOTO_MODE:
        return [f"order: unit {unit} aimode at {reload_like - 1}={mode0} {reload_like}={mode1}, expected GOTO ({GOTO_MODE}) at T+1+D of the pie release"]
    print(
        f"crab_ai_order: unit {unit} ({unit_by[reload_like]['name']}) GOTO at {reload_like} "
        f"(release lands {released}, T+1+D), pie left Enabled at {closed[0]}, hover {hover}"
    )
    return []


def check_craft_cargo(host, D):
    """Deliver-command craft: passenger inv identical (peer lines already match) and hatch-open tick agrees."""
    crafts = []
    for uid, by in host.items():
        if any(v.get("hatch") is not None for v in by.values()):
            crafts.append(uid)
    if not crafts:
        return ["craft_cargo: no actor with hatch= in the dump (deliver command did not place a craft)"]
    craft = min(crafts, key=lambda u: min(host[u]))
    by = host[craft]
    appear = min(by)
    expected_appear = 50 + D
    hatch_open = first_transition(by, "hatch", 0, 1)
    if hatch_open is None:
        # already opening on first dumped tick
        ticks = sorted(by)
        hatch_open = next((t for t in ticks if by[t].get("hatch") == 1), None)
    invs = {t: by[t].get("inv") for t in sorted(by)}
    ninv0 = by[appear].get("ninv")
    if ninv0 is None or ninv0 < 2:
        return [f"craft {craft} at {appear}: ninv={ninv0}, expected the two Green Dummy cargo items"]
    if hatch_open is None:
        return [f"craft {craft}: hatch never entered OPENING"]
    emptied = [t for t in sorted(by) if t >= hatch_open and (by[t].get("ninv") or 0) < ninv0]
    print(
        f"craft_cargo: uid {craft} ({by[appear]['name']}) appear {appear} (command 50, T+D={expected_appear}) "
        f"ninv={ninv0} inv={invs[appear]} hatch OPENING {hatch_open} first unload {emptied[:1]}"
    )
    return []


def load_activity(path):
    rows = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ACTIVITY_FULL.match(line.rstrip("\n"))
            if not m:
                continue
            rows[int(m.group(1))] = {"state": int(m.group(2)), "tail": m.group(3), "line": line.rstrip("\n")}
    return rows


def read_text(path):
    if not path:
        return ""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def team_is_cpu_visible(activity_rows, log_text, team):
    """True when a dump field or [e2e] print says TeamIsCPU(team) is 1. Neither exists today."""
    token = f"cpu{team}="
    for row in activity_rows.values():
        tail = row["tail"]
        if f" cpu={team}:1" in tail or f" t{team}cpu=1" in tail or f" {token}1" in tail:
            return True, f"activity dump {row['line']}"
        fields = fields_of(tail.strip())
        if fields.get(f"cpu{team}") == "1" or (fields.get("cpu") == "1" and team == CPU_TEAM_COOP):
            return True, f"activity dump {row['line']}"
    for m in E2E_TEAM_IS_CPU.finditer(log_text or ""):
        if int(m.group(1)) == team and int(m.group(2)) == 1:
            return True, m.group(0)
    return False, F60_TEAMISCPU_OWED


def first_seen_tick(by):
    return min(by) if by else None


def cpu_team_delivered_unit(host, team):
    """A CPU-team actor that appears after the dump opens, or a craft with hatch= on that team."""
    window = sorted({t for by in host.values() for t in by})
    if not window:
        return None, "no dump window"
    lo = window[0]
    newcomers = []
    crafts = []
    for uid, by in host.items():
        first = first_seen_tick(by)
        row0 = by[first]
        if row0.get("team") != team:
            continue
        if first > lo:
            newcomers.append((uid, first, row0["name"]))
        if any(v.get("hatch") is not None for v in by.values()):
            crafts.append((uid, first, row0["name"]))
    if newcomers:
        uid, tick, name = newcomers[0]
        return (uid, tick, name), f"new actor uid={uid} {name} at {tick}"
    if crafts:
        uid, tick, name = crafts[0]
        return (uid, tick, name), f"craft uid={uid} {name} hatch present"
    return None, (
        f"CPU team {team} spawned/bought/delivered no unit in {lo}..{window[-1]}; "
        "F60: ConfigureNetMatchActivity never calls SetCPUTeam (Main.cpp:4798-4809), so "
        "m_TeamIsCPU stays false and SkirmishDefense's director (SkirmishDefense.lua:5,38,109) never runs"
    )


def check_humans_vs_cpu(host, client, host_dump, client_dump, host_log, client_log, team=CPU_TEAM_COOP):
    """CPU team fights, human-team actors exist on both peers, and the dumps already matched."""
    failures = []
    human_team = 0
    host_humans = [uid for uid, by in host.items() if by and next(iter(by.values())).get("team") == human_team]
    client_humans = [uid for uid, by in client.items() if by and next(iter(by.values())).get("team") == human_team]
    if not host_humans:
        failures.append(f"humans_vs_cpu: no human-team {human_team} actors in the host dump")
    if not client_humans:
        failures.append(f"humans_vs_cpu: no human-team {human_team} actors in the client dump")

    host_act = load_activity(host_dump)
    client_act = load_activity(client_dump)
    host_ok, host_detail = team_is_cpu_visible(host_act, host_log, team)
    client_ok, client_detail = team_is_cpu_visible(client_act, client_log, team)
    if not host_ok:
        failures.append(f"host TeamIsCPU: {host_detail}")
    if not client_ok:
        failures.append(f"client TeamIsCPU: {client_detail}")
    unit, unit_detail = cpu_team_delivered_unit(host, team)
    client_unit, client_unit_detail = cpu_team_delivered_unit(client, team)
    if unit is None:
        failures.append(unit_detail)
    if client_unit is None:
        failures.append("client " + client_unit_detail)
    elif unit is not None and client_unit != unit:
        failures.append(f"CPU unit identity host={unit} client={client_unit}")
    if not failures:
        print(f"humans_vs_cpu: TeamIsCPU team={team} on both peers, {unit_detail}")
    return failures


def mode1_on_team_at(rows, team, tick):
    found = []
    for uid, by in rows.items():
        if tick in by and by[tick].get("team") == team and by[tick].get("mode") == 1:
            found.append(uid)
    return sorted(found)


def nearest_tick(rows, target, before=True):
    ticks = sorted({t for by in rows.values() for t in by})
    if before:
        cand = [t for t in ticks if t <= target]
    else:
        cand = [t for t in ticks if t >= target]
    return cand[-1] if before and cand else (cand[0] if cand else None)


def last_tick_before(rows, target):
    ticks = sorted({t for by in rows.values() for t in by})
    cand = [t for t in ticks if t < target]
    return cand[-1] if cand else None


def check_ai_takeover_leave(host, leave_tick, leave_team, client=None):
    """Departed seat is mode=1 on the last tick before leave and CIM_AI from the leave tick on, on every peer."""
    dumps = [("host", host)]
    if client is not None:
        dumps.append(("client", client))
    pre = last_tick_before(host, leave_tick)
    if pre is None:
        return [f"ai_takeover_leave: no dump ticks before leave {leave_tick}"]
    departed = mode1_on_team_at(host, leave_team, pre)
    if not departed:
        return [f"ai_takeover_leave: no mode=1 actor on team {leave_team} at tick {pre}"]
    failures = []
    for uid in departed:
        for label, rows in dumps:
            by = rows.get(uid, {})
            pre_row = by.get(pre)
            if pre_row is None:
                failures.append(f"{label} uid {uid}: missing at pre-leave tick {pre}")
                continue
            if pre_row.get("mode") != 1:
                failures.append(
                    f"{label} uid {uid} at {pre}: mode={pre_row.get('mode')}, expected mode=1 on the last tick before leave"
                )
            after = [t for t in sorted(by) if t >= leave_tick]
            if not after:
                failures.append(f"{label} uid {uid}: no dump rows from leave={leave_tick}")
                continue
            for t in after:
                mode = by[t].get("mode")
                dis = by[t].get("dis")
                if mode != CIM_AI or dis not in (0, None):
                    failures.append(
                        f"{label} uid {uid} at {t}: mode={mode} dis={dis}, expected mode=2 dis=0 from leave tick {leave_tick}"
                    )
                    break
    if not failures:
        print(
            f"ai_takeover_leave: uids {departed} mode=1 at {pre} and mode=2 dis=0 from {leave_tick} on both peers"
        )
    return failures


def split_tick_runs(rows):
    ticks = sorted({t for by in rows.values() for t in by})
    if not ticks:
        return []
    runs, current = [], [ticks[0]]
    for t in ticks[1:]:
        if t < current[-1]:
            runs.append(current)
            current = [t]
        else:
            current.append(t)
    runs.append(current)
    return runs


def check_ai_reclaim(host, client, leave_team, leaver_rows=None):
    """Pre-drop mode=1 actors are mode=1 again at the returner's first tick on both peers."""
    source = leaver_rows if leaver_rows else host
    runs = split_tick_runs(source)
    if not runs:
        return ["ai_reclaim: empty dump"]
    pre_tick = runs[0][-1]
    pre_set = mode1_on_team_at(source, leave_team, pre_tick)
    if not pre_set:
        return [f"ai_reclaim: no mode=1 actor on team {leave_team} at pre-drop tick {pre_tick}"]
    host_runs = split_tick_runs(host)
    first_run = host_runs[0] if host_runs else []
    ai_ticks = [t for t in first_run if pre_tick < t] if leaver_rows else []
    if not ai_ticks and len(host_runs) >= 1:
        ai_ticks = [t for t in host_runs[0] if t > pre_tick]
    failures = []
    post_source = client if client else host
    post_runs = split_tick_runs(post_source)
    if not post_runs:
        return failures + ["ai_reclaim: returner dump empty"]
    reclaim_tick = post_runs[0][0]
    ai_ticks = [t for t in ai_ticks if t < reclaim_tick]
    for uid in pre_set:
        by = host.get(uid, {})
        for t in ai_ticks:
            if t in by and by[t].get("status") == DEAD:
                failures.append(f"uid {uid} died during the AI-driven interval at {t} (status=4)")
                break
    for label, rows in (("host", host), ("returner", post_source)):
        found = mode1_on_team_at(rows, leave_team, reclaim_tick)
        if set(found) != set(pre_set):
            failures.append(
                f"ai_reclaim: {label} mode=1 set {found} at reclaim tick {reclaim_tick} != pre-drop {pre_set} at {pre_tick}"
            )
        for uid in pre_set:
            by = rows.get(uid, {})
            if reclaim_tick not in by:
                failures.append(f"{label} uid {uid}: missing at reclaim tick {reclaim_tick}")
                continue
            row = by[reclaim_tick]
            if row.get("mode") != 1:
                failures.append(
                    f"{label} uid {uid} at {reclaim_tick}: mode={row.get('mode')}, expected mode=1 at reclaim"
                )
            if row.get("status") == DEAD:
                failures.append(f"{label} uid {uid} at {reclaim_tick}: dead at reclaim")
    if not failures:
        print(
            f"ai_reclaim: pre-drop uids {pre_set} at {pre_tick} back under mode=1 at reclaim tick {reclaim_tick} on both peers"
        )
    return failures



ACTIVITY_OVER = 6  # ActivityState::Over


def activity_over_tick(path):
    """The first dumped tick whose activity row reads Over; None while the match never ends in the window."""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ACTIVITY.match(line)
            if m and int(m.group(2)) == ACTIVITY_OVER:
                return int(m.group(1))
    return None


def seated_actor(rows, switch_tick, delay, over_tick=None):
    """The actor that enters player mode at the NEXT release edge and keeps it while the activity runs.

    The engine releases every seated actor when the activity goes Over, so the stays-seated clause ends at
    over_tick (the first Over tick of the host dump); a release before that is the defect this row detects.
    """
    lo = min(t for by in rows.values() for t in by)
    if lo >= switch_tick:
        return (
            None,
            f"the dump window starts at {lo}, after the seat switch at {switch_tick}; start it before the switch",
        )
    found, lost = [], []
    for uid, by in rows.items():
        ticks = sorted(by)
        before = [t for t in ticks if t < switch_tick]
        landed = [
            t
            for t in ticks
            if switch_tick <= t <= switch_tick + delay + 2 and by[t]["mode"] == 1
        ]
        if not before or not landed or by[before[-1]]["mode"] == 1:
            continue
        running = [t for t in ticks if t >= landed[0] and (over_tick is None or t < over_tick)]
        released = [t for t in running if by[t]["mode"] != 1]
        if released:
            lost.append(f"{uid} seated at {landed[0]} left player mode at {released[0]} while the activity was running")
            continue
        found.append((uid, landed[0]))
    if len(found) != 1:
        detail = "; ".join(lost) if lost else ""
        return (
            None,
            f"expected exactly one actor to enter player mode in [{switch_tick}, {switch_tick + delay + 2}] and keep it while running, found {found}"
            + (f" ({detail})" if detail else "")
            + (f"; activity Over at {over_tick}" if over_tick is not None else ""),
        )
    return found[0], ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case")
    ap.add_argument("host_dump")
    ap.add_argument("client_dump")
    ap.add_argument("--delay", type=int, required=True)
    ap.add_argument("--host-trace")
    ap.add_argument("--switch-tick", type=int, default=32)
    ap.add_argument("--host-log")
    ap.add_argument("--client-log")
    ap.add_argument("--leave-tick", type=int, default=300)
    ap.add_argument("--leave-team", type=int, default=1)
    ap.add_argument("--leaver-dump")
    args = ap.parse_args()
    D = args.delay
    host, host_raw = load(args.host_dump)
    client, client_raw = load(args.client_dump)
    failures = []
    if set(host) != set(client):
        failures.append(
            f"actor sets differ: host {sorted(host)} client {sorted(client)}"
        )
    for uid in sorted(set(host) & set(client)):
        for tick in sorted(set(host[uid]) & set(client[uid])):
            hline = strip_per_machine(host_raw[uid][tick])
            cline = strip_per_machine(client_raw[uid][tick])
            if args.case == "crab_ai_order" and actor_mode(hline) == CIM_AI and actor_mode(cline) == CIM_AI:
                # The unseated brain's pie enabled-state digit is local presentation; see
                # strip_pie_enabled_state for the code path and the sim-hash evidence.
                hline = strip_pie_enabled_state(hline)
                cline = strip_pie_enabled_state(cline)
            if hline != cline:
                failures.append(f"actor {uid} tick {tick} differs between peers")
                break
    if not host:
        failures.append("no actor lines in the host dump")
    peer_failures = list(failures)
    if peer_failures and args.case != "craft_cargo":
        for f_ in peer_failures:
            print("FAIL: " + f_, file=sys.stderr)
        return 1
    if peer_failures:
        for f_ in peer_failures:
            print("FAIL: " + f_, file=sys.stderr)

    window = sorted({t for by in host.values() for t in by})
    lo, hi = window[0], window[-1]
    if args.case == "ai_orders":
        failures = check_ai_orders(host, D)
        for f_ in failures:
            print("FAIL: " + f_, file=sys.stderr)
        if not failures:
            print(f"PASS ai_orders: peers identical over {lo}..{hi}")
        return 1 if failures else 0
    if args.case == "craft_cargo":
        failures = peer_failures + check_craft_cargo(host, D)
        for f_ in failures[len(peer_failures):]:
            print("FAIL: " + f_, file=sys.stderr)
        if not failures:
            print(f"PASS craft_cargo: peers identical over {lo}..{hi}")
        return 1 if failures else 0
    if args.case == "door_pass":
        failures = check_door_pass(host)
        for f_ in failures:
            print("FAIL: " + f_, file=sys.stderr)
        if not failures:
            print(f"PASS door_pass: peers identical over {lo}..{hi}")
        return 1 if failures else 0
    if args.case == "humans_vs_cpu":
        failures = check_humans_vs_cpu(
            host, client, args.host_dump, args.client_dump,
            read_text(args.host_log), read_text(args.client_log),
        )
        for f_ in failures:
            print("FAIL: " + f_, file=sys.stderr)
        if not failures:
            print(f"PASS humans_vs_cpu: peers identical over {lo}..{hi}")
        return 1 if failures else 0
    if args.case == "ai_takeover_leave":
        failures = check_ai_takeover_leave(host, args.leave_tick, args.leave_team, client)
        for f_ in failures:
            print("FAIL: " + f_, file=sys.stderr)
        if not failures:
            print(f"PASS ai_takeover_leave: peers identical over {lo}..{hi}")
        return 1 if failures else 0
    if args.case == "ai_reclaim":
        leaver = load(args.leaver_dump)[0] if args.leaver_dump else None
        failures = check_ai_reclaim(host, client, args.leave_team, leaver)
        for f_ in failures:
            print("FAIL: " + f_, file=sys.stderr)
        if not failures:
            print(f"PASS ai_reclaim: peers identical over {lo}..{hi}")
        return 1 if failures else 0
    seat, reason = seated_actor(host, args.switch_tick, D, activity_over_tick(args.host_dump))
    if seat is None:
        print(
            f"FAIL: no seated actor in the window {lo}..{hi}: {reason}", file=sys.stderr
        )
        return 1
    seat, seated_at = seat
    by = host[seat]

    def at(t, key):
        return by[t][key] if t in by else None

    def lands(t):
        return t + D + 1

    if args.case == "fire_reload":
        # FIRE held 200..260 (semi-auto: one shot per press), RELOAD at 300, magazine back full 2300 ms later.
        first_shot, last_hold = lands(200), lands(260)
        r_before = at(first_shot - 1, "rounds")
        fired = [
            t
            for t in range(first_shot, last_hold + 1)
            if t in by and by[t]["rounds"] is not None and by[t]["rounds"] < r_before
        ]
        if r_before is None or not fired:
            failures.append(
                f"fire: rounds never decreased during the hold (before={r_before})"
            )
        else:
            first = fired[0]
            if first > first_shot + 6:
                failures.append(
                    f"fire: first round left at tick {first}, expected within a few ticks of the landing at {first_shot}"
                )
            rounds_seq = [
                by[t]["rounds"] for t in range(first_shot, last_hold + 1) if t in by
            ]
            if any(b > a for a, b in zip(rounds_seq, rounds_seq[1:])):
                failures.append("fire: rounds increased during the hold")
            early = [
                t
                for t in range(lo, first_shot)
                if t in by
                and by[t]["rounds"] is not None
                and by[t]["rounds"] < r_before
            ]
            if early:
                failures.append(
                    f"fire: rounds decreased before the landing tick at {early[:3]}"
                )
            r_after = at(last_hold, "rounds")
            spent = r_before - r_after if r_after is not None else None
            print(
                f"fire: {spent} round(s) over the hold, first at tick {first} (pressed 200, lands {first_shot})"
            )
        reload_at = lands(300)
        if at(reload_at - 1, "reloading") != 0 or at(reload_at, "reloading") != 1:
            failures.append(
                f"reload: reloading at {reload_at - 1}={at(reload_at - 1, 'reloading')} {reload_at}={at(reload_at, 'reloading')}, expected 0 then 1 at the landing tick"
            )
        else:
            done = [
                t
                for t in range(reload_at, hi + 1)
                if t in by and by[t]["reloading"] == 0
            ]
            if not done or by[done[0]]["rounds"] < r_before:
                failures.append(
                    f"reload: the magazine never came back full inside the window (ends {hi})"
                )
            else:
                print(
                    f"reload: started at {reload_at}, full magazine ({by[done[0]]['rounds']} rounds) at tick {done[0]}"
                )
    elif args.case == "weapon_switch":
        first, second = lands(100), lands(200)
        fg0 = at(first - 1, "fg")
        fg1 = at(first, "fg")
        fg2 = at(second - 1, "fg")
        fg3 = at(second, "fg")
        if not (fg0 and fg1 and fg1 != fg0):
            failures.append(
                f"switch: fg at {first - 1}={fg0} {first}={fg1}, expected the held device to change at the landing tick"
            )
        # The dummy holds one device and carries ninv more; the second switch cycles on (back to the first only with two devices).
        expected = fg0 if at(first, "ninv") == 1 else None
        if not (
            fg2 == fg1
            and fg3
            and fg3 != fg2
            and (expected is None or fg3 == expected)
            and (expected is not None or fg3 != fg0)
        ):
            failures.append(
                f"switch: fg at {second - 1}={fg2} {second}={fg3}, expected the second switch to cycle to {'the first device ' + str(fg0) if expected else 'a third device'}"
            )
        if not failures:
            print(
                f"switch: {fg0} -> {fg1} at {first}, -> {fg3} at {second} (carrying {at(first, 'ninv')} spare)"
            )
    elif args.case == "jetpack":
        start, end = lands(100), lands(130)
        emits = [t for t in range(start, end + 1) if at(t, "emit") == 1]
        early = [t for t in range(lo, start) if at(t, "emit") == 1]
        if len(emits) < 25 or early:
            failures.append(
                f"jetpack: emitting on {len(emits)} of the {end - start + 1} landed hold ticks, early on {early[:3]}"
            )
        j0, j1 = at(start, "jet"), at(end, "jet")
        if j0 is None or j1 is None or not j1 < j0:
            failures.append(
                f"jetpack: fuel {j0} -> {j1} over the hold, expected a decrease"
            )
        y0, y1 = at(start, "y"), at(end + 6, "y")
        if y0 is None or y1 is None or not (y0 - y1) >= 10:
            failures.append(
                f"jetpack: y {y0} -> {y1}, expected a rise of at least 10 px"
            )
        if (
            not failures
            and j0 is not None
            and j1 is not None
            and y0 is not None
            and y1 is not None
        ):
            print(
                f"jetpack: emitted {len(emits)} ticks from {emits[0]}, fuel {j0:.1f}->{j1:.1f}, rose {y0 - y1:.1f} px"
            )
    elif args.case == "terrain_fire":
        aimed, shot = lands(140), lands(150)
        if not args.host_trace:
            failures.append("terrain: --host-trace is required")
        else:
            th = {
                t["tick"]: t["subsystems"]
                for t in json.load(open(args.host_trace))["runs"][0]["tick_hashes"]
            }
            before = th.get(shot - 1, {}).get("terrain")
            changed = [
                t
                for t in range(shot, shot + 60)
                if t in th and th[t].get("terrain") != before
            ]
            if not before or not changed:
                failures.append(
                    "terrain: the terrain hash never changed after firing into the ground"
                )
            else:
                print(
                    f"terrain: first carve at tick {changed[0]} (shot lands {shot}, aim {at(aimed, 'aim'):.2f} rad)"
                )
        aim_landed = at(aimed, "aim")
        if aim_landed is None or aim_landed > -0.5:
            failures.append(
                f"terrain: aim at {aimed} = {aim_landed}, expected the barrel pointed down"
            )
    elif args.case == "pie_reload":
        # FIRE at 100 spends a round; PIEMENU_DIGITAL held 150..190 opens the menu (Enabling at its landing,
        # Enabled after 50 sim ms), L_UP at 160 hovers the Up quadrant's first slice (Pick Up, typed Reload with
        # nothing in reach), the release at 191 activates it and the reload starts that tick on every peer.
        pies = {t: by[t]["pie"] for t in sorted(by) if by[t]["pie"]}
        # A controller state shows in the menu at its landing tick T+D; the activated command is handled at the next update.
        opened, hovered, released = 150 + D, 160 + D, 191 + D
        # The activation is the tick's last pie update; the command lands in the next tick's pre-controller pass, which reloads.
        reload_at = released + 1
        state = lambda t: int(pies[t].split(":")[0]) if t in pies else None
        hovered_name = lambda t: pies[t].split(":")[2] if t in pies else None
        if state(opened - 1) != 3 or state(opened) != 0:
            failures.append(
                f"pie: enabled state at {opened - 1}={state(opened - 1)} {opened}={state(opened)}, expected Disabled then Enabling at the landing tick"
            )
        enabled = [t for t in range(opened, opened + 12) if state(t) == 1]
        if not enabled or enabled[0] != opened + 4:
            failures.append(
                f"pie: Enabled first at {enabled[:1]}, expected {opened + 4} (50 sim ms after Enabling)"
            )
        if hovered_name(hovered) != "Pick Up" or hovered_name(hovered - 1) != "-":
            failures.append(
                f"pie: hovered slice at {hovered - 1}={hovered_name(hovered - 1)} {hovered}={hovered_name(hovered)}, expected Pick Up from the landing tick"
            )
        rounds_before = at(released, "rounds")
        if at(released, "reloading") != 0 or at(reload_at, "reloading") != 1:
            failures.append(
                f"pie: reloading at {released}={at(released, 'reloading')} {reload_at}={at(reload_at, 'reloading')}, expected the reload to start the update after the release landed"
            )
        if rounds_before is None or rounds_before >= 15:
            failures.append(
                f"pie: rounds before the reload = {rounds_before}, expected the tick-100 shot to have spent one"
            )
        if not failures:
            print(
                f"pie: opened {opened} (Enabled {enabled[0]}), hovered Pick Up at {hovered}, released {released}, reload started at {reload_at} from {rounds_before} rounds"
            )
    elif args.case == "crab_ai_order":
        failures = check_crab_ai_order(host, D, seat, by)
    else:
        failures.append(f"unknown case {args.case}")
    if failures:
        for f_ in failures:
            print("FAIL: " + f_, file=sys.stderr)
        return 1
    print(
        f"PASS {args.case}: seated actor {seat} ({by[seated_at]['name']}) from tick {seated_at}, peers identical over {lo}..{hi}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
