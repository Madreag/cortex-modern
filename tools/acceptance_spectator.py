"""Spectator scenario preflight and ownership-receipt collection."""
import json
from pathlib import Path

from acceptance_evidence import live_hashes, peer_receipt, rows
from acceptance_rows import judge

SEATED = ('seated-one', 'seated-two', 'seated-three')
PEERS = ('host', *SEATED, 'spectator')


def requirements(repo, scenario):
    if scenario.get("acceptance_row") != "spectator":
        return []
    peers = scenario.get("peers", [])
    faults = []
    if len(peers) != 5 or {p["name"] for p in peers} != set(PEERS):
        faults.append("spectator row requires a dedicated host, three seated processes and one watcher")
    slowed = [p["name"] for p in peers if p.get("env", {}).get("CCCP_TEST_SIM_COST_US")]
    if slowed != ["spectator"]:
        faults.append("simulation cost must affect only the watcher")
    sources = {"CC_TEST_WORLD_MAX_SPECTATORS": Path(repo)/"Source/Network/NetMatchService.cpp",
               "dump_world_ownership": Path(repo)/"Source/Menus/MenuAutomation.cpp",
               "world_spectator_cost_window": Path(repo)/"Source/Main.cpp"}
    for lever in scenario.get("required_engine_levers", []):
        source = sources.get(lever)
        if source is None or not source.is_file() or lever not in source.read_text(encoding="utf-8"):
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
    remote = (root/'spectator-remote.json').is_file()
    plan = read(root/'spectator-remote.json') if remote else {}
    placement = plan.get('placement')
    def base(peer):
        if placement:
            return root/'boxes'/placement[peer]
        return root/'boxes'/('LAPTOP' if peer=='host' else 'REMOTE') if remote else root
    host_probe = base('host')/"host-stage/probe"
    spectator_probe = base('spectator')/"spectator-stage/probe"
    before = read(host_probe/"before-release.ownership.json")
    after = read(host_probe/"after-promotion.ownership.json")
    image = read(spectator_probe/"spectator-image.ownership.json")
    cost = read(spectator_probe/"crawl-complete.ownership.json")
    promoted = read(spectator_probe/"promoted-input.ownership.json")
    departing = read(base('seated-one')/"seated-one-stage/probe/departing-seat.ownership.json")
    watch = {**image.get("watch", {}), **cost.get("watch", {})}
    facts = dict(configuration=before.get("configuration", {}), seated=list(SEATED), clock_box='remote' if remote else 'pc',
                 spectator="spectator", peers={}, throttle=cost.get("sim_cost", {}),
                 promotion=promoted.get("promotion", {}), watch=watch)
    errors = ownership_agreement(after.get("promotion", {}), facts["promotion"], departing.get("ownership", {}))
    if placement:
        from acceptance_clock_brackets import collect_brackets
        facts['clock_box'] = None
        facts['clock_brackets'] = {}
        try:
            facts['clock_brackets'] = collect_brackets(root, plan, facts['throttle'])
        except (OSError, ValueError, KeyError, TypeError) as error:
            errors.append('throttle: native clock brackets unavailable: '+str(error))
    # The native client receipt binds the applied input; only the host's native
    # receipt can authorize the release. Keep both sides and their agreement.
    facts['promotion']['host_authorized'] = after.get('promotion', {}).get('host_authorized')
    for peer in (*SEATED,'spectator'):
        path = base(peer)/peer/"stdout.log"
        log = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        record = read(base(peer)/peer/"result.json") or read(base(peer)/peer/"launch.json")
        try:
            live = base(peer)/(peer+'-live.jsonl')
            if peer in SEATED:
                if placement:
                    bracket = facts['clock_brackets']['peers'][peer]
                    low, high = bracket['begin']['sim_tick']-1, bracket['end']['sim_tick']+1
                    box = placement[peer].lower()
                else:
                    native = [row for row in rows(live) if row.get('phase')=='live' and type(row.get('wall_ms')) in (int,float)]
                    low = max(row['tick'] for row in native if row['wall_ms'] <= facts['throttle']['start_ms'])
                    high = min(row['tick'] for row in native if row['wall_ms'] >= facts['throttle']['end_ms'])
                    box = facts['clock_box']
                facts["peers"][peer] = peer_receipt(box, live, log, record, low, high)
            else:
                facts['peers'][peer] = dict(box=placement[peer].lower() if placement else facts['clock_box'],
                                          completed=record.get('exit_code')==0 and not record.get('timed_out',False))
        except (ValueError, TypeError, KeyError, OSError) as error:
            errors.append(f"{peer}: native timing unavailable: {error}")
    watch = facts["watch"]
    try:
        watch["hashes"] = live_hashes({p: base(p)/(p+"-live.jsonl") for p in ("host", "spectator")}, watch["first"], watch["last"])
    except (ValueError, TypeError, KeyError, OSError) as error:
        errors.append("watch: native hashes unavailable: "+str(error))
    result = judge("spectator", facts)
    result["failures"].extend(errors)
    result["passed"] = not result["failures"]
    result["facts"] = facts
    return result
