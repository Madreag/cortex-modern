"""Acceptance receipts derived from a reserved cross run's retained native files."""
from __future__ import annotations

import json
from pathlib import Path
import re

from acceptance_evidence import fullstate_hashes, live_hashes, peer_receipt, rows
from acceptance_rows import judge
from acceptance_runtime import write_json
from world_soak import census_receipts


def load(path, default=None):
    path = Path(path)
    return json.loads(path.read_text(encoding="utf-8-sig")) if path.is_file() else default


def native_labels(own):
    texts = []
    index = load(own/"native-screens.json", [])
    from acceptance_mod import sha256
    paths = []
    for entry in index:
        name = entry.get("name", "")
        if Path(name).name != name:
            raise ValueError("retained screen receipt contains a non-leaf path")
        path = own/"native-screens"/name
        if not path.is_file() or path.stat().st_size != entry.get("bytes") or sha256(path) != entry.get("sha256"):
            raise ValueError("retained screen dump differs from its native receipt")
        if re.fullmatch(r"dump_host_options_\d+\.json", name):
            paths.append(path)
    for path in paths:
        document = load(path, {})
        if document.get("screen") != "MultiplayerScreen":
            continue
        for control in document.get("controls", []):
            if control.get("name") == "LabelMultiplayerLandingStatus" and control.get("visible") is True and control.get("text"):
                texts.append(control["text"])
    return "\n".join(texts)


ENGINE_FINDING = re.compile(
    r"RTE Assert|FATAL:|EXCEPTION_ACCESS_VIOLATION|Runtime Error due to unhandled exception|Rejected .*command|"
    r"\[cross-record\] FAIL|\[net-ui-probe\] FAIL|\[net-match-service-e2e\].*(?:FAIL|setup failed)|"
    r"\[net-match\] controller sync failed:|\[net-plane\].*ASSERT|"
    r"\[fullstate(?:-refusal)?\].*(?:failed:|refused:|problem=)|Desync:|desync at|admission refused|"
    r"\[Lua error\]|Segmentation fault", re.I)


def native_prerequisites(native, record, log, preflight, peers):
    """Keep the ordinary cross driver's native checks alongside the new row oracle."""
    failures = []
    if native.get("exit_code") != 0 or native.get("setup_error") != "" or native.get("runtime_error") != "":
        failures.append("native match outcome missing or failed")
    if record.get("exit_code") != 0 or record.get("timed_out") is not False:
        failures.append("native runner did not complete successfully")
    expected = preflight.get("executable_sha256")
    if not re.fullmatch(r"[0-9a-f]{64}", str(expected)) or record.get("exe_sha256") != expected:
        failures.append("running binary differs from or lacks its preflight identity")
    desync = native.get("desync_check", {})
    if (desync.get("mismatches") != 0 or type(desync.get("compares")) is not int or desync["compares"] <= 0
            or type(desync.get("compare_margin")) is not int or desync["compare_margin"] < 0):
        failures.append("native desync check missing or failed")
    config = native.get("service", {}).get("runner", {}).get("match_config", {})
    if config.get("peer_count") != peers:
        failures.append("native adopted peer count differs from the row")
    lines = [str(number) for number, line in enumerate(log.splitlines(), 1) if ENGINE_FINDING.search(line)]
    if lines:
        failures.append("engine findings in stdout.log lines "+", ".join(lines))
    return failures


def build_report(root):
    from cross_report import peer_root
    root = Path(root)
    manifest = load(root/"manifest.json", {})
    row = manifest["acceptance_row"]
    specs = {("pc" if spec["peer"] == "erol" else spec["peer"]): spec for spec in manifest["specs"]}
    paths = {name: peer_root(root, manifest, spec) for name, spec in specs.items()}
    logs = {name: (own/"engine/stdout.log").read_text(encoding="utf-8", errors="replace")
            if (own/"engine/stdout.log").is_file() else "" for name, own in paths.items()}
    documents = {name: own/"live.jsonl" for name, own in paths.items()}
    facts, failures = {}, []
    preflights = manifest.get("preflights", {})
    if len(preflights) != len(manifest["boxes"]) or any(not p.get("machine_id") for p in preflights.values()) or \
            len({p.get("machine_id") for p in preflights.values()}) != len(manifest["boxes"]):
        failures.append("preflight: distinct real machine receipts missing")
    if manifest.get("driver_findings") != []:
        failures.append("driver: a launch, reservation, payload or fetch finding remains")
    end = manifest["ticks"]
    start = 1
    comparing = list(paths)
    if row.startswith("world-"):
        activated = re.findall(r"(?m)^\[net-world\] activate peer=(\d+) at=(\d+)\s*$", logs.get("pc", ""))
        completed = re.findall(r"(?m)^\[net-world\] catch-up complete peer=(\d+) at=(\d+)", logs.get("edith", ""))
        released = load(root/"late-join-released.json", {})
        start = int(completed[-1][1]) if completed else None
        if not isinstance(start, int) or completed[-1] not in activated:
            failures.append("join: native activation and late-join receipt do not agree")
            start = None
        facts["join"] = dict(peer="edith", host_tick=released.get("host_tick"), activation_tick=start, last_tick=end,
                             directory=manifest.get("directory_mode"), public_directory_down=manifest.get("public_directory_down"),
                             own_certificate=manifest.get("own_certificate"),
                             nat_to_nat=bool(re.search(r"\[net-ice\][^\n]*srflx", logs.get("edith", ""))) and bool(re.search(r"\[net-ice\][^\n]*srflx", logs.get("pc", ""))),
                             stun=bool(re.search(r"\[net-ice\][^\n]*srflx", logs.get("edith", ""))),
                             route="direct" if re.search(r"\[net-ice\][^\n]*route=direct", logs.get("edith", "")) else None)
        facts["transfer"] = load(paths.get("edith", root)/"acceptance-transfer.json", {})
    elif row == "mod-refusal":
        comparing.remove("linux")
    for name in comparing:
        own, spec = paths[name], specs[name]
        native = load(own/"match-report.json", {})
        record = load(own/"record.json", {})
        for error in native_prerequisites(native, record, logs[name], preflights.get(spec["box"], {}), len(paths)-(row == "mod-refusal")):
            failures.append(f"{name}: {error}")
    try:
        facts["live"] = live_hashes({n: documents[n] for n in comparing}, start, end) if start else {}
        facts["fullstate"] = fullstate_hashes({n: paths[n]/"engine/stdout.log" for n in comparing}, start, end) if start else {}
        facts["peers"] = {name: peer_receipt("edith" if name == "edith-first" else name, documents[name], logs[name], load(paths[name]/"record.json", {}),
                                             start if row.startswith("world-") and name == "edith" and start else 1, end)
                          for name in paths if name != "linux" or row != "mod-refusal"}
    except (OSError, ValueError, TypeError) as error:
        failures.append("native receipts: "+str(error))
    if row.startswith("mod-"):
        facts.update(module="VoidWanderers.rte", activity="Void Wanderers", installed_activity="Void Wanderers")
        facts["tree_hashes"] = {name: preflights.get(spec["box"], {}).get("acceptance_module", {}).get("tree_sha256") for name, spec in specs.items()}
        for name, own in paths.items():
            # The activity must be named by an adopted native config, never only by launch flags.
            try:
                adopted = [r for r in rows(own/"events.jsonl") if r.get("type") == "adopted_config"]
            except (OSError, ValueError):
                adopted = []
            if not specs[name].get("module_refusal") and not any(r.get("config", {}).get("activity_preset") == "Void Wanderers" and r.get("config", {}).get("activity_module") == "VoidWanderers.rte" for r in adopted):
                failures.append(f"{name}: native mod activity identity missing")
    if row == "mod-refusal":
        own = paths["linux"]
        from e2e_video import screen_watch_results, menu_script_failures
        watches = screen_watch_results(own/'engine')
        if not watches or any(value.get('offences') or not value.get('summary') or value['summary'].get('violations') != 0 for value in watches.values()):
            failures.append('refusal: shared screen assertions missing or failed')
        if menu_script_failures(own/'engine'):
            failures.append('refusal: menu assertion failed')
        mutation = load(own/"mutation-summary.json", {})
        restored = load(own/"mutation.json.restored.json", {})
        try: joined = any(r.get("phase") == "live" for r in rows(documents["linux"]))
        except FileNotFoundError: joined = False
        facts["refusal"] = dict(**mutation, joiner="linux", altered_box="linux", restored=restored.get("tree_sha256"),
                                log_text=logs["linux"], landing_text=native_labels(own), joined=joined,
                                refusal_tick=load(root/"late-join-released.json", {}).get("host_tick"),
                                survivors=["pc", "edith", "mac"])
    if row == "world-soak":
        observed = load(paths["pc"]/"soak-elapsed.json", {})
        released = load(root/"late-join-released.json", {})
        facts["soak"] = {**manifest["soak"], "elapsed_s": observed.get("elapsed_s"),
                         "late_join_elapsed_s": released.get("host_elapsed_s")}
        facts["soak"]["journal"] = load(paths["pc"]/"journal-sizes.json", [])
        if start:
            try:
                facts['initial_live'] = live_hashes({n:documents[n] for n in ('pc','edith-first')}, 1, start-1)
                facts['initial_fullstate'] = fullstate_hashes({n:paths[n]/'engine/stdout.log' for n in ('pc','edith-first')}, 1, start-1)
            except (OSError, ValueError, TypeError) as error:
                failures.append('initial world history: '+str(error))
        facts["census"] = {}
        for name, text in logs.items():
            census = census_receipts(text)
            facts["census"][name] = census["raw_series"]
            write_json(root/f"census-{name}.json", census)
    try:
        result = judge(row, facts)
    except (KeyError, TypeError, ValueError) as error:
        result = dict(row=row, passed=False, failures=["incomplete native receipts: "+str(error)])
    result["failures"].extend(failures)
    result["passed"] = not result["failures"]
    result["v1_passed"] = result["passed"]
    write_json(root/"acceptance-facts.json", facts)
    write_json(root/"result.json", result)
    (root/"verdict.txt").write_text(f"{row}: {'PASS' if result['passed'] else 'FAIL'}\n"+"\n".join(result["failures"])+"\n", encoding="utf-8")
    return result
