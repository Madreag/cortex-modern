"""Spectator scenario preflight and ownership-receipt collection."""
import json
from pathlib import Path

from acceptance_evidence import live_hashes, peer_receipt, rows
from acceptance_rows import judge


def requirements(repo, scenario):
    if scenario.get("acceptance_row") != "spectator":
        return []
    peers = scenario.get("peers", [])
    faults = []
    if len(peers) != 4 or {p["name"] for p in peers} != {"host", "seated-one", "seated-two", "spectator"}:
        faults.append("spectator row requires three seated processes and one watcher")
    slowed = [p["name"] for p in peers if p.get("env", {}).get("CCCP_TEST_SIM_COST_US")]
    if slowed != ["spectator"]:
        faults.append("simulation cost must affect only the watcher")
    sources = {"CC_TEST_WORLD_MAX_SPECTATORS": Path(repo)/"Source/Network/NetMatchService.cpp",
               "dump_world_ownership": Path(repo)/"Source/Menus/MenuAutomation.cpp",
               "world_spectator_cost_window": Path(repo)/"Source/Main.cpp"}
    for lever in scenario.get("required_engine_levers", []):
        source = sources.get(lever)
        if source is None or lever not in source.read_text(encoding="utf-8"):
            faults.append(f"engine receipt/lever missing: {lever}")
    return faults


def read(path):
    path = Path(path)
    return json.loads(path.read_text(encoding="utf-8-sig")) if path.is_file() else {}


def ownership_agreement(host, promoted, departing):
    errors = []
    for field in ("seat", "actor", "ticket_incarnation", "activation_tick", "freed_seat"):
        if host.get(field) is None or host.get(field) != promoted.get(field):
            errors.append(f"promotion: host and promoted peer disagree on {field} or its receipt is absent")
    if host.get("host_authorized") is not True:
        errors.append("promotion: authoritative host release/reassignment receipt missing")
    if departing.get("seat") is None or departing.get("seat") != host.get("freed_seat"):
        errors.append("promotion: released seat differs from the departing peer's authoritative binding")
    return errors


def collect(root):
    root = Path(root)
    host_probe = root/"host-stage/probe"
    spectator_probe = root/"spectator-stage/probe"
    before = read(host_probe/"before-release.ownership.json")
    after = read(host_probe/"after-promotion.ownership.json")
    image = read(spectator_probe/"spectator-image.ownership.json")
    cost = read(spectator_probe/"crawl-complete.ownership.json")
    promoted = read(spectator_probe/"promoted-input.ownership.json")
    departing = read(root/"seated-one-stage/probe/departing-seat.ownership.json")
    watch = {**image.get("watch", {}), **cost.get("watch", {})}
    facts = dict(configuration=before.get("configuration", {}), seated=["host", "seated-one", "seated-two"],
                 spectator="spectator", peers={}, throttle=cost.get("sim_cost", {}),
                 promotion=promoted.get("promotion", {}), watch=watch)
    errors = ownership_agreement(after.get("promotion", {}), facts["promotion"], departing.get("ownership", {}))
    low, high = before.get("seated_first_tick"), before.get("lockstep_frame")
    for peer in ("host", "seated-one", "seated-two", "spectator"):
        path = root/peer/"stdout.log"
        log = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        record = read(root/peer/"result.json") or read(root/peer/"launch.json")
        try:
            facts["peers"][peer] = peer_receipt("pc", root/(peer+"-live.jsonl"), log, record, low, high)
        except (ValueError, TypeError, OSError) as error:
            errors.append(f"{peer}: native timing unavailable: {error}")
    watch = facts["watch"]
    try:
        watch["hashes"] = live_hashes({p: root/(p+"-live.jsonl") for p in ("host", "spectator")}, watch["first"], watch["last"])
    except (ValueError, TypeError, KeyError, OSError) as error:
        errors.append("watch: native hashes unavailable: "+str(error))
    result = judge("spectator", facts)
    result["failures"].extend(errors)
    result["passed"] = not result["failures"]
    result["facts"] = facts
    return result
