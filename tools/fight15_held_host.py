"""Write fight15-s10h: fight15-s10's twenty-minute stock endless AI fight with the host's own simulation stalled past the slow
player bound on cue, so every run holds the host's seat to the AI and brings the host back while the joiner plays on."""
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "e2e"
# In minute two of the fight (the first minute ends near tick 5,000 on every box), long past the 3-tick bound.
STALL_TICK = 6000
STALL_MS = 2000
# The bound (3 ticks) plus the hold decision's trip, at the round's 60 ticks a second.
HOLD_WITHIN_TICKS = 120


def held_host(document):
    scene = copy.deepcopy(document)
    scene["name"] = "fight15-s10h"
    scene["title"] = document["title"] + " - the host's own seat held on cue and back"
    host = next(peer for peer in scene["peers"] if peer["name"] == "host")
    host["args"] = host["args"] + ["-net-test-live-stall", f"{STALL_TICK}:{STALL_MS}"]
    # The seat's AI line is a detail line.
    host["env"] = {**host.get("env", {}), "CC_TEST_RETURN_INPUT_DETAILS": "1"}
    # The stall freezes the host's own screen by design: its frames and its still are the lever's, named for the stall's ticks alone and
    # left out of those two rules.
    reason = f"the scene's own stall of the host's simulation, {STALL_MS} ms at tick {STALL_TICK}"
    stall = {"peer": "host", "first_tick": STALL_TICK - 60, "last_tick": STALL_TICK + 120, "reason": reason}
    scene["drawn_frames_exclude"] = [dict(stall)]
    scene["allowed_stills"] = [dict(stall)]
    # The hold must follow the stall, within the bound and the decision's trip; a hold the host earns later on its own is not this one.
    held = "|".join(str(STALL_TICK // 10 + offset) for offset in range(HOLD_WITHIN_TICKS // 10))
    scene["checklist"].append({
        "id": "host-seat-held-host", "peer": "host", "mark": "minute-02-host", "screen": "game",
        "what": f"The host's own simulation stalled {STALL_MS} ms at tick {STALL_TICK}: the session plane held the host's seat to the AI "
                f"within {HOLD_WITHIN_TICKS} ticks and the host came back to it while the joiner played on.",
        "log_regex": [f"\\[net-test\\] live stall frame=\\d+ ms={STALL_MS}", f"\\[net-seat-ai\\] peer=1 held=1 frame=({held})\\d ",
                      "\\[net-match\\] seat-reclaimed peer=1 frame="]})
    # The joiner never waits on the stalled host past A1's bound: over 50 ms on any frame from the stall to its hold's end.
    waited = "|".join(str(STALL_TICK // 100 + offset) for offset in range(2))
    scene["checklist"].append({
        "id": "joiner-never-waited-joiner", "peer": "joiner", "mark": "minute-02-joiner", "screen": "game",
        "what": f"The joiner never waited over 50 ms on the host from its stall at tick {STALL_TICK} to {STALL_TICK + 199}.",
        "log_regex": ["\\[fight15-scene\\] minute=2 pace="],
        "forbidden_log_regex": [f"\\[net-frame-wait\\] frame=({waited})\\d\\d wait_ms=(\\d{{3,}}|5[1-9]|[6-9]\\d) "]})
    return scene


def main():
    source = json.loads((ROOT / "fight15-s10.json").read_text(encoding="utf-8"))
    (ROOT / "fight15-s10h.json").write_text(json.dumps(held_host(source), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
