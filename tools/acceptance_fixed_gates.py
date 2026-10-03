"""Apply the landed cross-oracle evidence rules to the new rows' declared scopes."""
from __future__ import annotations

from pathlib import Path
import re

from feel.report import parse_fullstate, compare_fullstate_histories, reclaim_sample_obligations


def identity(manifest, peers):
    row = manifest.get("acceptance_row")
    errors, diagnostics = [], []
    host_box = manifest.get('acceptance_host_box', manifest.get('world_host_box', 'EROL-PC'))
    if host_box not in (('EROL-PC', 'Z13', 'ALLY') if row == 'world-soak' else ('EROL-PC', 'Z13')):
        errors.append("host box is outside the lead's named hosts for this row")
    expected = {"erol":host_box, "edith":"EDITH", "mac":"Mac", "linux":"Linux"}
    if row == "world-soak":
        expected = {"erol":host_box, "edith-first":"EDITH", "edith":"EDITH"}
    actual = {entry.get("name"):entry.get("box") for entry in manifest.get("instances", [])}
    if actual != expected or set(peers) != set(expected):
        errors.append("named instances and peer evidence do not match the row's required boxes")
    boxes = {entry.get("name") for entry in manifest.get("boxes", [])}
    if boxes != set(expected.values()):
        errors.append("box set differs from the declared row")
    specs = manifest.get("specs", [])
    if manifest.get("host") != "erol" or [entry.get("peer") for entry in specs if entry.get("role") == "host"] != ["erol"]:
        errors.append("the world or match host role is not uniquely bound")
    for name, box in expected.items():
        matched = [entry for entry in specs if entry.get("peer") == name and entry.get("box") == box]
        if len(matched) != 1 or matched[0].get("role") != ("host" if name == "erol" else "player"):
            errors.append(name+": native instance role differs from its box contract")
    preflights = manifest.get("preflights", {})
    machines = [preflights.get(box, {}).get("machine_id") for box in boxes]
    if not all(machines) or len(set(machines)) != len(boxes):
        errors.append("distinct machine identities are absent or repeated")
    source = manifest.get("source_sha")
    if not re.fullmatch(r"[0-9a-f]{40}", str(source)):
        errors.append("source commit receipt missing")
    for name, box in expected.items():
        pre = preflights.get(box, {})
        build, record = pre.get("build", {}), peers.get(name, {}).get("record", {})
        exe = pre.get("executable_sha256")
        if pre.get('head') != source:
            diagnostics.append(name+": checkout head differs from the measured build's source commit")
        if (build.get("commit") != source or not re.fullmatch(r"[0-9a-f]{64}", str(exe))
                or build.get("executable_sha256") != exe or record.get("exe_sha256") != exe):
            errors.append(name+": source, build, measured binary and runner receipts do not agree")
        if record.get("started") is not True:
            errors.append(name+": native process launch was not witnessed")
    return dict(passed=not errors, failures=errors, diagnostics=diagnostics, expected=expected)


def capture_evidence(inputs, intervals, cadence):
    from cross_report import fullstate_expected
    comparisons, errors = [], []
    all_documents = {}
    for name in inputs["peers"]:
        path = Path(inputs["paths"][name])/"engine/stdout.log"
        document = parse_fullstate([path])
        # The shared parser uses these to bind samples but does not export unsampled announcements.
        document["announcements"] = []
        for number, line in enumerate(path.read_text(encoding="utf-8-sig", errors="replace").splitlines(), 1):
            if not line.startswith('[fullstate-context] '):
                continue
            match = re.fullmatch(r'\[fullstate-context\] tick=(\d+) round=(\d+) label=(\S+) path=(.+)', line)
            if not match:
                errors.append(f'{name}: malformed capture announcement at line {number}')
                continue
            document["announcements"].append(dict(key=(int(match[2]), int(match[1]), match[3]), log=str(path), line=number))
        all_documents[name] = document
    host_rows = inputs["live"].get("erol", [])
    host_expected = fullstate_expected(host_rows, cadence) if cadence else []
    for names, first, last in intervals:
        documents = {name:all_documents[name] for name in names}
        expected = {key for key in host_expected if first <= key[1] <= last}
        rounds = {row.get("round") for row in host_rows if type(row.get("tick")) is int and first <= row["tick"] <= last}
        ranges = [dict(match=round_id, first=first, last=last) for round_id in rounds if round_id is not None]
        obligations = reclaim_sample_obligations(documents, ranges)
        expected.update(map(tuple, obligations["expected"]))
        expected.update(tuple(sample["key"]) for document in documents.values() for sample in document["samples"]
                        if sample["key"][2] == "landed" and first <= sample["key"][1] <= last)
        comparison = compare_fullstate_histories(documents, sorted(expected))
        comparison["obligations"] = obligations
        comparison["passed"] &= not obligations["invalid"]
        # A missing writer result cannot disappear merely because no sample line was emitted.
        missing = []
        for name, document in documents.items():
            samples = {(tuple(sample["key"]), sample["log"]) for sample in document["samples"]}
            for announced in document.get("announcements", []):
                key = tuple(announced["key"])
                if (first <= key[1] <= last or key[2] in ("canonical", "restored")) and (key, announced["log"]) not in samples:
                    missing.append(dict(peer=name, key=key, line=announced["line"]))
        comparison["missing_announced_captures"] = missing
        comparison["passed"] &= not missing
        comparisons.append(dict(peers=names, first=first, last=last, **comparison))
    if inputs["manifest"]["acceptance_row"].startswith("world-"):
        late = all_documents.get("edith", {})
        if not any(sample["key"][2] == "restored" for sample in late.get("samples", [])):
            errors.append("late joiner has no native restored full-state sample")
    return dict(passed=bool(comparisons) and all(row["passed"] for row in comparisons) and not errors,
                comparisons=comparisons, failures=errors)


def evaluate(inputs, facts):
    from cross_report import completed_workload, coverage, memory_verdict
    manifest, peers, events = (inputs[key] for key in ("manifest", "peers", "events"))
    row, end = manifest["acceptance_row"], manifest["ticks"]
    active = {name:peer for name, peer in peers.items() if not (row == "mod-refusal" and name == "linux")}
    joined = facts.get("join", {}).get("activation_tick")
    intervals = []
    if row.startswith("world-"):
        if type(joined) is int and 1 < joined <= end:
            intervals = [([name for name in active if name != "edith"], 1, joined-1), (list(active), joined, end)]
    else:
        intervals = [(list(active), 1, end)]
    workload = {}
    for name in active:
        first = joined if row.startswith("world-") and name == "edith" else 1
        workload[name] = completed_workload([dict(name=name)], events, end-first+1) if type(first) is int and 1 <= first <= end else dict(passed=False)
        workload[name]["passed"] &= active[name].get("native_final_tick") == end and active[name].get("native_completion", {}).get("completion") == "completed"
    matrix = coverage(events, active, {**manifest, "scenario":"soak" if row == "world-soak" else "match"})
    # R2 explicitly uses item 17's ordinary-match gates. Preserve the shared
    # oracle's exact 1201-tick/no-fault/no-world applicability test.
    memory_manifest = {**manifest, 'acceptance_row':17} if row in ('mod-match', 'mod-refusal') else manifest
    memory = memory_verdict(active, memory_manifest)
    bound = identity(manifest, peers)
    captures = capture_evidence(inputs, intervals, manifest.get("fullstate_every", 0))
    findings = [finding for finding in inputs.get("findings", []) if not (row == "mod-refusal" and finding.get("peer") == "linux"
                and re.search(r"admission refused[^\n]*ModuleManifestMismatch", finding.get("text", "")))]
    checks = dict(identity=bound["passed"], measured_workload=bool(workload) and all(value["passed"] for value in workload.values()),
                  coverage_minima=all(value["status"] in ("PASS", "NOT APPLICABLE") for value in matrix if value.get("required", True)),
                  memory_bounds=memory["status"] in ("PASS", "NOT APPLICABLE"), instrumentation=bool(active) and all(peer.get("instrument_valid") is True for peer in active.values()),
                  quiet_feel=bool(active) and all(peer.get("feel_gated") is True and peer.get("feel_pass") is True for peer in active.values()),
                  record_integrity=bool(active) and all(peer.get("presentation_valid") is True and peer.get("tick_timing_valid") is True
                      and any(event.get("type") == "tick_timing" for event in events.get(name, []))
                      and not any(event.get("type") in ("record_loss", "record_rotation", "malformed_record") for event in events.get(name, []))
                      for name, peer in active.items()),
                  complete_captures=captures["passed"], no_engine_findings=not findings,
                  binary_admission=bool(inputs.get("capabilities")) and all(inputs["capabilities"].get(box["name"], {}).get("peer_limit", 0) >= len(active)
                      for box in manifest["boxes"]),
                  no_mixed_builds=not inputs.get("mixed_builds"))
    return dict(passed=all(checks.values()), checks=checks, identity=bound, workload=workload, coverage=matrix,
                memory=memory, captures=captures, findings=[{key:value for key,value in finding.items() if key in ("peer", "path", "line", "kind")} for finding in findings],
                failures=[key+": fixed harness evidence missing or failed" for key, passed in checks.items() if not passed])
