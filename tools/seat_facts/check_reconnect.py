"""Assert canonical seat, shared brain and physical input/screen facts after admission."""
from __future__ import annotations

import json
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def local_view(row, player, teams):
    require(type(row.get("player_index")) is int and row["player_index"] == player, "canonical local seat differs")
    for name in ("input_player", "player_controller_input", "controller_input"):
        # The journal omits a physical-input column when the seat has no distinct record for it.
        if name in row:
            require(type(row[name]) is int and row[name] == 0, name + " must be physical zero")
    require(type(row.get("screen")) is int and row["screen"] == 0, "screen must be physical zero")
    require(row.get("seat_mode") == 1 and row.get("seat_player") == player, "controlled actor's canonical seat differs")
    require(row.get("player_active") is True and row.get("player_human") is True, "local seat is not an active human")
    require(type(row.get("controlled_uid")) is int and row["controlled_uid"] > 0, "missing controlled actor")
    facts = row.get("seat_facts")
    require(type(facts) is list and len(facts) == 4, "missing four-seat facts")
    for index, seat in enumerate(facts):
        require(type(seat) is dict and seat.get("player") == index, "seat facts are not contiguous")
        human = index < len(teams)
        require(seat.get("active") is human and seat.get("human") is human, "shared human roster differs")
        if human:
            require(seat.get("team") == teams[index], "shared seat team differs")
            # 0 is a dead-brain seat; the engine reports that shared fact.
            require(type(seat.get("brain_uid")) is int and seat["brain_uid"] >= 0, "human seat brain_uid is not a shared fact")
        else:
            require(seat.get("brain_uid") == 0, "inactive seat has a brain")
        for name in ("input", "screen"):
            require(type(seat.get(name)) is int and seat[name] == (0 if index == player else -1), "remote seat aliases local " + name)
    require(row.get("team") == teams[player] and row.get("brain_uid") == facts[player]["brain_uid"], "local seat/brain differs from shared facts")


def shared(row):
    # Compare every shared field. Only the explicitly local input and screen columns differ.
    return [{key: seat[key] for key in ("player", "active", "human", "team", "brain_uid")} for seat in row["seat_facts"]]


def journal(path):
    path = Path(path)
    require(path.is_file(), "missing journal: " + str(path))
    raw = path.read_bytes()
    require(raw.endswith(b"\n"), "partial returned-peer journal: " + str(path))
    rows = [json.loads(line) for line in raw.splitlines()]
    require(rows and rows[0].get("event") == "ready" and "shared_seat_view_v1" in rows[0].get("capabilities", []), "missing seat evidence capability")
    for index, row in enumerate(rows):
        require(row.get("seq") == index and row.get("journal_dropped") == 0 and row.get("event") != "evidence_gap", "journal lost observations")
        require(all(row.get(key) == rows[0].get(key) for key in ("run_id", "peer", "pid")), "journal process identity changed")
    return rows


def pair(host, returner, player, teams, round_id=None, minimum=60):
    def views(events):
        result = {}
        for row in events:
            if row.get("event") == "local_control" and (round_id is None or row.get("round_id") == round_id):
                key = (row["round_id"], row["frame"])
                require(key not in result, "duplicate local-control frame")
                result[key] = row
        return result
    left, right = views(host), views(returner)
    common = sorted(left.keys() & right.keys())
    require(common, "no shared returner frames")
    selected_round = common[-1][0] if round_id is None else round_id
    common = [key for key in common if key[0] == selected_round]
    require(len(common) >= minimum, "fewer than 60 common returner frames")
    # Match the existing A7 returner's first 61 observations, including the restore boundary.
    selected = common[:61]
    require([key[1] for key in selected] == list(range(selected[0][1], selected[-1][1] + 1)), "returner frame coverage is not contiguous")
    require(selected[0] == min(key for key in right if key[0] == selected_round), "missing host facts at the first returner frame")
    for key in selected:
        local_view(left[key], 0, teams)
        local_view(right[key], player, teams)
        require(shared(left[key]) == shared(right[key]), "peers disagree on seat/brain facts at " + str(key))
    return {"pass": True, "round": selected_round, "frames": len(selected), "first": selected[0][1], "last": selected[-1][1],
            "returner_player": player, "returner_input": 0, "returner_screen": 0, "shared_seats": shared(right[selected[0]])}


def absent(events):
    require(not any(row.get("event") in ("local_control", "running") for row in events), "unadmitted applicant acquired a gameplay seat")
    return {"pass": True, "no_gameplay_seat": True}
