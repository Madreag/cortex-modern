"""Write fight15-s10h: fight15-s10's twenty-minute stock endless AI fight with the host's own simulation stalled past the slow
player bound on cue, so every run holds the host's seat to the AI and brings the host back while the joiner plays on."""
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "e2e"
# In minute two of the fight (the first minute ends near tick 5,000 on every box), long past the 3-tick bound.
STALL_TICK = 6000
STALL_MS = 2000


def held_host(document):
    scene = copy.deepcopy(document)
    scene["name"] = "fight15-s10h"
    scene["title"] = document["title"] + " - the host's own seat held on cue and back"
    host = next(peer for peer in scene["peers"] if peer["name"] == "host")
    host["args"] = host["args"] + ["-net-test-live-stall", f"{STALL_TICK}:{STALL_MS}"]
    # The seat's AI line is a detail line.
    host["env"] = {**host.get("env", {}), "CC_TEST_RETURN_INPUT_DETAILS": "1"}
    # The stall freezes the host's own screen by design: its frames and its still are the lever's, named and left out of those two rules.
    reason = f"the scene's own stall of the host's simulation, {STALL_MS} ms at tick {STALL_TICK}"
    scene["drawn_frames_exclude"] = [{"peer": "host", "first_tick": STALL_TICK - 60, "last_tick": STALL_TICK + 120, "reason": reason}]
    scene["allowed_stills"] = [{"peer": "host", "from_mark": "minute-01-host", "to_mark": "minute-02-host", "reason": reason}]
    scene["checklist"].append({
        "id": "host-seat-held-host", "peer": "host", "mark": "minute-02-host", "screen": "game",
        "what": f"The host's own simulation stalled {STALL_MS} ms at tick {STALL_TICK}: the session plane held the host's seat to the AI "
                "and the host came back to it while the joiner played on.",
        "log_regex": [f"\\[net-test\\] live stall frame=\\d+ ms={STALL_MS}", "\\[net-seat-ai\\] peer=1 held=1 ",
                      "\\[net-match\\] seat-reclaimed peer=1 frame="]})
    return scene


def main():
    source = json.loads((ROOT / "fight15-s10.json").read_text(encoding="utf-8"))
    (ROOT / "fight15-s10h.json").write_text(json.dumps(held_host(source), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
