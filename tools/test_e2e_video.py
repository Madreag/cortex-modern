"""The e2e video driver's own rows: scenario shape, token substitution, checklist resolution, encode commands.

    python tools/test_e2e_video.py --repo <tree> --out <dir>

Nothing here launches the engine or touches a capture: every row runs against a synthetic frame index in
its own temporary directory, so this suite is safe to run on a machine that is building. The rows that need
a real capture are named at the bottom of the result and are the driver's own first run's job.
"""

import argparse
import contextlib
import json
import os
import re
from pathlib import Path
import sys
import tempfile
import threading
from types import SimpleNamespace
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import e2e_video as driver  # noqa: E402

SCENARIOS = ("sp-smoke", "mp-host-join", "mp-reconnect-repair", "world-late-join", "ui-surfaces",
             "mod-void-wanderers", "mp-leave", "mp-rematch", "mp-rollback-lag",
             "mp-moderation", "mp-host-migration", "mp-resume-from-disk", "mp-direct-vs-relay",
             "mod-void-wanderers-multiplayer", "mp-host-join-cross")


def row(results, name, ok, detail=""):
    results.append({"row": name, "pass": bool(ok), "detail": detail})
    print(f"[e2e-video] {'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
    return ok


def check_capture_binary(results, scratch):
    platform = sys.platform
    previous = os.environ.pop("CCCP_TEST_BINARY", None)
    try:
        sys.platform = "win32"
        ok = row(results, "manifest/windows-runner-binary", driver.capture_binary(scratch) == scratch.resolve() / "Cortex Command.exe")
        sys.platform = "darwin"
        ok &= row(results, "manifest/posix-default-binary", driver.capture_binary(scratch) == scratch.resolve() / "build-gns/CortexCommand")
        os.environ["CCCP_TEST_BINARY"] = str(scratch / "custom-engine")
        ok &= row(results, "manifest/posix-override-binary", driver.capture_binary(scratch) == (scratch / "custom-engine").resolve())
        return ok
    finally:
        sys.platform = platform
        if previous is None:
            os.environ.pop("CCCP_TEST_BINARY", None)
        else:
            os.environ["CCCP_TEST_BINARY"] = previous


def check_scratch_limit(results, scratch):
    resolver = getattr(driver, "scratch_limit", None)
    if resolver is None:
        return row(results, "scratch/selected-limit-is-supported", False)
    options = SimpleNamespace(scratch_limit_bytes=None)
    ok = row(results, "scratch/default-five-billion", resolver(options) == 5_000_000_000)
    retained = {"scratch_limit_bytes": 8_000_000_000}
    ok &= row(results, "scratch/finalizer-retains-selected-limit", resolver(options, retained) == 8_000_000_000)
    options.scratch_limit_bytes = 7_000_000_000
    ok &= row(results, "scratch/explicit-finalizer-limit", resolver(options, retained) == 7_000_000_000)
    for value in (0, -1, "0", "-1", "1.5", "invalid"):
        try:
            driver.positive_bytes(value)
            rejected = False
        except argparse.ArgumentTypeError:
            rejected = True
        ok &= row(results, f"scratch/rejects-{value}", rejected)
    ok &= row(results, "scratch/positive-byte-count", driver.positive_bytes("8000000000") == 8_000_000_000)
    for limit, expected in ((8_000_000_000, False), (6_000_000_000, True), (5_000_000_000, True)):
        with patch.object(driver, "scratch_bytes", return_value=6_000_000_000):
            try:
                driver.check_scratch_budget(scratch, limit)
                stopped = False
            except RuntimeError as error:
                stopped = str(limit) in str(error) and "no cleanup performed" in str(error)
        ok &= row(results, f"scratch/enforces-{limit}", stopped == expected)
    return ok


def check_module_requirements(results, scratch):
    repo = scratch / "module-requirements"
    module = repo / "Data/VoidWanderers.rte"
    module.mkdir(parents=True)
    (module / "Index.ini").write_text("DataModule\n\tSupportedGameVersion = 6.2.2\n", encoding="utf-8")
    ok = True
    for name in ("mod-void-wanderers", "mod-void-wanderers-multiplayer"):
        scenario = driver.load_scenario(name)
        ok &= row(results, f"{name}/version-warning-does-not-block-launch", not driver.requirement_findings(repo, scenario))
        missing = driver.requirement_findings(repo / "absent", scenario)
        ok &= row(results, f"{name}/missing-module-still-blocks", len(missing) == 1 and missing[0]["class"] == "data")
    exact = {"requires_version": [{"module": "VoidWanderers.rte", "version": "7.0.0", "reason": "Exact version required"}]}
    findings = driver.requirement_findings(repo, exact)
    ok &= row(results, "requirements/explicit-version-contract-is-preserved", len(findings) == 1 and
              findings[0]["declared"] == "6.2.2" and findings[0]["required"] == "7.0.0")
    return ok


def check_rematch_contract(results):
    scenario = driver.load_scenario("mp-rematch")
    runs = {run["name"]: run for run in scenario.get("runs", [])}
    ok = row(results, "rematch/two-distinct-runs", set(runs) == {"rematch", "injected-desync"})
    if not ok:
        return False
    regular, injected = runs["rematch"], runs["injected-desync"]
    ok &= row(results, "rematch/full-second-round-hash-range", regular.get("hash_gate") == {
        "name": "round2-hashes", "peers": ["host", "client"], "first_tick": 1, "cap": 600})
    peers = {peer["name"]: peer for peer in injected["peers"]}
    host_args, client_args = peers["host"]["args"], peers["client"]["args"]
    ok &= row(results, "rematch/one-sided-existing-perturbation", "-determinism-selftest-perturb" in host_args and
              "-determinism-selftest-perturb" not in client_args and
              host_args[host_args.index("-determinism-selftest-perturb-tick") + 1] == "240")
    ok &= row(results, "rematch/automatic-repair-enabled", all("-net-match-e2e-resync" in peer["args"] and
              "-net-match-service-e2e" in peer["args"] for peer in peers.values()))
    probes = [json.loads(driver.scenario_text(scenario, peer["probe"])) for peer in peers.values()]
    ok &= row(results, "rematch/no-manual-repair-substitution", all("ButtonMatchRepairNow" not in json.dumps(probe) for probe in probes))
    items = [item for item in scenario["checklist"] if item.get("run") == "injected-desync"]
    ok &= row(results, "rematch/repair-needs-video-and-native-evidence", all(
        any(item.get("peer") == name and item.get("screen") == "ResyncOverlay" and
            any("reloading from the host snapshot" in pattern for pattern in item.get("log_regex", [])) for item in items) and
        any(item.get("peer") == name and item.get("readback") and
            any("match relaunched from the snapshot" in pattern for pattern in item.get("log_regex", [])) for item in items)
        for name in peers))
    return ok


def check_scenarios(results):
    """Every scenario parses, names peers, points at scripts that exist and keeps its own port slice."""
    ok = True
    seen_bases = {}
    for name in SCENARIOS:
        scenario = driver.load_scenario(name)
        ok &= row(results, f"{name}/schema", scenario["schema"] == 1 and scenario["name"] == name)
        base = scenario.get("port_base", driver.PORT_LO)
        runs = scenario.get("runs") or [{"name": "run0", "peers": scenario.get("peers", [])}]
        ports = [driver.port_for(index, base) for index in range(len(runs))]
        inside = all(driver.PORT_LO <= port <= driver.PORT_HI for port in ports)
        clash = [other for other, used in seen_bases.items() if set(used) & set(ports)]
        seen_bases[name] = ports
        ok &= row(results, f"{name}/ports", inside and not clash, f"{ports} clash={clash}")
        for index, run in enumerate(runs):
            peers = run.get("peers") or scenario.get("peers") or []
            ok &= row(results, f"{name}/run{index}/peers", bool(peers), str([peer["name"] for peer in peers]))
            names = [peer["name"] for peer in peers]
            ok &= row(results, f"{name}/run{index}/unique-peers", len(set(names)) == len(names))
            for peer in peers:
                for key in ("menu_script", "input_script", "probe"):
                    if not peer.get(key):
                        continue
                    try:
                        text = driver.scenario_text(scenario, peer[key])
                    except SystemExit as error:
                        ok &= row(results, f"{name}/{peer['name']}/{key}", False, str(error))
                        continue
                    if key != "probe":
                        ok &= row(results, f"{name}/{peer['name']}/{key}", bool(text.strip()))
                        continue
                    probe = json.loads(text)
                    fits = (probe.get("schema") == 1 and 0 < probe.get("timeout_ms", 0) <= 180000 and
                            0 < len(probe.get("steps", [])) <= 256)
                    ok &= row(results, f"{name}/{peer['name']}/probe", fits,
                              f"{len(probe.get('steps', []))} steps, {probe.get('timeout_ms')} ms")
        checklist = scenario.get("checklist", [])
        ids = [item["id"] for item in checklist]
        ok &= row(results, f"{name}/checklist", bool(ids) and len(set(ids)) == len(ids), f"{len(ids)} items")
        ok &= row(results, f"{name}/checklist-what", all(item.get("what") for item in checklist))
        # An item that cannot be driven names why, so a reviewer never reads a gap as a pass.
        ok &= row(results, f"{name}/checklist-blocked-named",
                  all(item.get("blocked_by") for item in checklist if item.get("assert") == "not scripted"))
    return ok


def check_substitution(results):
    tokens = {"PORT": 49411, "PROBE_DIR": Path("D:/x/host-stage/probe"), "PEER": "host"}
    text = driver.substitute("settext TextHostPort {PORT}\nwait_file {PROBE_DIR}/done.json 240\n", tokens)
    ok = row(results, "substitute/text", "49411" in text and "{PORT}" not in text and "{PROBE_DIR}" not in text)
    args = driver.substitute(["-net-port", "{PORT}", "-peer", "{PEER}"], tokens)
    ok &= row(results, "substitute/list", args == ["-net-port", "49411", "-peer", "host"], str(args))
    nested = driver.substitute({"a": ["{PEER}"], "b": {"c": "{PORT}"}}, tokens)
    ok &= row(results, "substitute/nested", nested == {"a": ["host"], "b": {"c": "49411"}}, str(nested))
    ok &= row(results, "substitute/unknown-token-left", driver.substitute("{NOPE}", tokens) == "{NOPE}")
    ok &= row(results, 'substitute/quoted-native-path-has-no-escapes', driver.substitute('dump_match_identity "{PROBE_DIR}/id.json"', tokens) == 'dump_match_identity "D:/x/host-stage/probe/id.json"')
    return ok


def check_launch_contract(results, scratch):
    stage = scratch / "launch"
    stage.mkdir()
    scenario = {"scripts": {"probe.json": json.dumps({"schema": 1, "steps": [
        {"op": "wait_file", "path": "{PROBE_DIR_client}/done.json"}]}), "menu.txt": "exit\n"}}
    peer = {"probe": "probe.json", "menu_script": "menu.txt", "args": ["-net-port", "{PORT}"]}
    tokens = {"PROBE_DIR_client": r"D:\mx\client-stage\probe", "VIDEO": stage / "video",
              "MENU_SCRIPT": stage / "menu.txt", "PORT": 49400}
    env = driver.stage_peer(scenario, peer, stage, tokens)
    probe = json.loads(Path(env["CC_TEST_NET_UI_SCRIPT"]).read_text(encoding="utf-8"))
    ok = row(results, "launch/windows-probe-path",
             probe["steps"][0]["path"] == r"D:\mx\client-stage\probe/done.json")
    args = driver.peer_arguments(peer, tokens, 30)
    ok &= row(results, "launch/menu-script-passed",
              args[args.index("-menu-script") + 1] == str(stage / "menu.txt"), str(args))
    ok &= row(results, "launch/headless", env["CCCP_HEADLESS"] == "1")
    return ok


def check_index_and_checklist(results, scratch):
    """A synthetic capture: the checklist resolves to the frames that carry each screen and tick."""
    video = scratch / "video"
    (video / "frames").mkdir(parents=True)
    rows = []
    for frame in range(12):
        screen = "MainScreen" if frame < 4 else ("game" if frame < 9 else "Pause")
        rows.append({"frame": frame, "wall_ms": frame * 33, "sim_tick": frame * 50,
                     "screen": screen, "resolution": [960, 540], "saved": True})
    (video / "frames.jsonl").write_text("".join(json.dumps(line) + "\n" for line in rows), encoding="utf-8")
    (video / "manifest.json").write_text(json.dumps(
        {"schema": 1, "fps": 30, "frames_saved": 12, "frames_dropped": 0, "frames_rate_limited": 3,
         "resolution": [960, 540], "first_sim_tick": 0, "last_sim_tick": 550}) + "\n", encoding="utf-8")

    read = driver.read_index(video)
    ok = row(results, "index/read", len(read) == 12 and read[0]["frame"] == 0, f"{len(read)} rows")
    ok &= row(results, "manifest/read", driver.read_manifest(video)["fps"] == 30)
    ok &= row(results, "index/missing-is-empty", driver.read_index(scratch / "nothing") == [])
    ok &= row(results, "manifest/missing-is-empty", driver.read_manifest(scratch / "nothing") == {})

    ok &= row(results, "checklist/by-screen", driver.frame_range(read, {"screen": "game"}) == [4, 8])
    ok &= row(results, "checklist/by-screen-and-ticks",
              driver.frame_range(read, {"screen": "game", "sim_ticks": [250, 350]}) == [5, 7])
    ok &= row(results, "checklist/never-seen", driver.frame_range(read, {"screen": "PauseMatchOptions"}) is None)
    ok &= row(results, "checklist/ticks-only", driver.frame_range(read, {"sim_ticks": [0, 100]}) == [0, 2])

    # A truncated index is what a killed peer leaves: it still reads, and the checklist still resolves.
    torn = scratch / "torn"
    (torn / "frames").mkdir(parents=True)
    (torn / "frames.jsonl").write_text(json.dumps(rows[0]) + "\n" + json.dumps(rows[1])[:20], encoding="utf-8")
    ok &= row(results, "index/torn-line-skipped", len(driver.read_index(torn)) == 1)
    return ok


def check_encode(results, scratch):
    """The encode and sheet refuse an empty capture by name instead of shelling out to nothing."""
    empty = scratch / "empty"
    (empty / "frames").mkdir(parents=True)
    result = driver.encode("ffmpeg", empty, 30, scratch / "out.mp4")
    ok = row(results, "encode/no-frames", result["encoded"] is False and "no frames" in result["reason"])
    sheet = driver.contact_sheet(empty, [], scratch / "sheet.png", 15, "ffmpeg")
    ok &= row(results, "sheet/no-frames", sheet["written"] is False)
    missing = driver.encode(None, scratch / "video", 30, scratch / "out.mp4")
    ok &= row(results, "encode/no-ffmpeg-empty-capture", missing["encoded"] is False and "no frames" in missing["reason"])
    (scratch / "video/frames/frame-000000.png").write_bytes(b"frame")
    missing = driver.encode(None, scratch / "video", 30, scratch / "out.mp4")
    ok &= row(results, "encode/no-ffmpeg", missing["encoded"] is False and "ffmpeg" in missing["reason"])
    located = driver.find_ffmpeg()
    row(results, "encode/ffmpeg-located", True, str(located))
    if located:
        from PIL import Image
        video = scratch / "timed-video"
        (video / "frames").mkdir(parents=True)
        rows = [{"frame": frame, "wall_ms": wall, "saved": True} for frame, wall in enumerate((1000, 1100, 2100))]
        for row_value in rows:
            Image.new("RGB", (16, 16), (row_value["frame"] * 100, 20, 30)).save(
                video / "frames" / f"frame-{row_value['frame']:06d}.png")
        (video / "frames.jsonl").write_text("".join(json.dumps(value) + "\n" for value in rows), encoding="utf-8")
        result = driver.encode(located, video, 10, scratch / "timed.mp4")
        duration = float(result.get("ffprobe", {}).get("format", {}).get("duration", 0))
        ok &= row(results, "encode/keeps-one-second-stall", result["encoded"] and 1.1 <= duration <= 1.4, str(result))
    return ok


def check_review(results, scratch):
    scenario = {"name": "paired-review", "checklist": [
        {"id": "mp-play", "screen": "game", "what": "Both peers have gameplay evidence."}]}
    capture = {"name": "run0", "peers": [
        {"peer": "host", "root": str(scratch / "host"), "video_dir": str(scratch / "video"),
         "probe_dir": str(scratch / "host-stage/probe"), "index": driver.read_index(scratch / "video"),
         "menu_script_failures": [], "video": None, "contact_sheet": None},
        {"peer": "client", "root": str(scratch / "client"), "video_dir": str(scratch / "video"),
         "probe_dir": str(scratch / "client-stage/probe"), "index": driver.read_index(scratch / "video"),
         "menu_script_failures": ["[menu-script] FAILED: assert_substate"], "video": None,
         "contact_sheet": None}]}
    out = scratch / "review"
    out.mkdir()
    document = driver.review(scenario, capture, out)
    ok = row(results, "review/written", (out / "review.json").is_file())
    ok &= row(results, "review/every-item-resolved", all("state" in item for item in document["checklist"]))
    unpeered = [item for item in document["checklist"] if item["id"] == "mp-play"]
    ok &= row(results, "review/peerless-item-covers-both", len(unpeered) == 2, str(len(unpeered)))
    ok &= row(results, "review/failures-carried",
              document["failures"]["client"] == ["[menu-script] FAILED: assert_substate"])
    scenario_items = [item for item in document["checklist"] if item["id"] != "no-assert-dialogs"]
    ok &= row(results, "review/no-probe-is-named",
              all(item.get("probe") == "none" for item in scenario_items))
    # The dialog row is written for every capture: a player would have had to answer each line it lists.
    dialog_rows = [item for item in document["checklist"] if item["id"] == "no-assert-dialogs"]
    ok &= row(results, "review/assert-dialog-row-present",
              len(dialog_rows) == 1 and dialog_rows[0]["probe"] == "pass" and dialog_rows[0]["assert_dialogs"] == [],
              str(dialog_rows))
    (scratch / "host").mkdir(parents=True, exist_ok=True)
    (scratch / "host/stdout.log").write_text(
        "[menu-script] loaded 2 steps\nRTE Assert (headless, continued like Ignore): Assertion in file 'X.cpp'\n",
        encoding="utf-8")
    fired = driver.review(scenario, capture, out)
    flagged = [item for item in fired["checklist"] if item["id"] == "no-assert-dialogs"][0]
    ok &= row(results, "review/assert-dialog-row-reports-the-line",
              flagged["probe"] == "fail" and "continued like Ignore" in flagged["finding"]["reason"]
              and flagged["assert_dialogs"][0]["peer"] == "host", str(flagged.get("assert_dialogs")))
    (scratch / "host/stdout.log").unlink()
    document = driver.review(scenario, capture, out)
    ok &= row(results, "review/verdict-is-not-a-pass", document["verdict"] == "agent-review-required")
    ok &= row(results, "review/no-mp4-is-not-video-evidence", all(item["frames"] is None for item in document["checklist"]))
    capture["peers"][0]["record"] = {"exit_code": 1, "timed_out": False}
    document = driver.review({"name": "exit-review", "checklist": []}, capture, out)
    ok &= row(results, "review/failed-exit-is-a-run-finding", len(document["run_findings"]) == 1)
    ok &= row(results, "review/summary-rejects-unseen-run-failure", driver.review_only(SimpleNamespace(review_only=out)) == 1)
    capture["peers"][0]["record"].update(exit_code=137, injected_termination="scenario drop after recorded tick 601")
    capture["peers"][0]["expected_termination"] = True
    document = driver.review({"name": "exit-review", "checklist": []}, capture, out)
    ok &= row(results, "review/planned-drop-keeps-native-exit", not document["run_findings"] and capture["peers"][0]["record"]["exit_code"] == 137)
    marker_video = scratch / "markers"
    marker_video.mkdir()
    (marker_video / "events.jsonl").write_text(''.join(json.dumps(value) + '\n' for value in [
        {"wall_ms": 10, "message": "video_mark first"}, {"wall_ms": 12, "message": "assert_label Value shown PASS"},
        {"wall_ms": 25, "message": "video_mark next"}]), encoding="utf-8")
    record = {"video_dir": str(marker_video), "probe_dir": str(scratch / "absent-probe"), "index": [
        {"frame": index, "wall_ms": at, "sim_tick": 0, "screen": "SettingsScreen", "saved": True}
        for index, at in enumerate((0, 10, 20, 30))]}
    frames, observed = driver.item_evidence(record, {"mark": "first", "screen": "SettingsScreen", "events": ["assert_label Value shown PASS"]})
    ok &= row(results, "review/marker-bounds", frames == [1, 2] and observed["probe"] == "pass", str(frames))
    tied = scratch / "markers-tied"
    tied.mkdir()
    (tied / "events.jsonl").write_text(''.join(json.dumps(value) + '\n' for value in [
        {"wall_ms": 10, "message": "video_mark first"}, {"wall_ms": 25, "message": "assert_label Value shown PASS"},
        {"wall_ms": 25, "message": "video_mark next"}, {"wall_ms": 25, "message": "assert_label Next shown PASS"}]), encoding="utf-8")
    tied_record = {**record, "video_dir": str(tied)}
    _, observed = driver.item_evidence(tied_record, {"mark": "first", "screen": "SettingsScreen", "events": ["assert_label Value shown PASS"]})
    ok &= row(results, "review/assert-in-the-next-marks-millisecond-kept", observed["probe"] == "pass")
    _, observed = driver.item_evidence(tied_record, {"mark": "next", "screen": "SettingsScreen", "events": ["assert_label Value shown PASS"]})
    ok &= row(results, "review/earlier-marks-assert-not-borrowed", observed["probe"] == "fail")
    _, observed = driver.item_evidence(tied_record, {"mark": "next", "screen": "SettingsScreen", "events": ["assert_label Next shown PASS"]})
    ok &= row(results, "review/next-marks-own-assert-kept", observed["probe"] == "pass")
    frames, observed = driver.item_evidence(record, {"mark": "missing", "screen": "SettingsScreen"})
    ok &= row(results, "review/missing-marker-is-not-a-screen-pass", frames is None and observed["probe"] == "not-reached")
    return ok


def check_interruption(results, scratch):
    out = scratch / "interrupted"
    first = out / "first"
    first.mkdir(parents=True)
    scenario = {"name": "interrupted", "runs": [{"name": "first", "peers": [{"name": "host"}]},
                                                {"name": "second", "peers": [{"name": "client"}]}],
                "checklist": [{"id": "play", "screen": "game", "what": "Gameplay is visible."}]}
    run = {"name": "first", "root": str(first), "size": "640x360", "interrupted": "test interruption",
           "peers": [{"peer": "host", "root": str(first / "host"), "video_dir": str(scratch / "video"),
                      "probe_dir": str(first / "probe"), "record": {}, "manifest": {},
                      "index": driver.read_index(scratch / "video"), "menu_script_failures": []}]}
    capture = {"scenario": "interrupted", "scenario_definition": scenario, "runs": [run], "source": {}, "exe": {},
               "started": "test", "fps": 6, "command": [], "interrupted": "test interruption"}
    driver.review(scenario, run, first)
    manifest = driver.scenario_manifest(capture, out, 1)
    review = driver.aggregate_review(capture, out)
    ok = row(results, "interruption/manifest-keeps-saved-frames", manifest["frame_count"] == 12 and manifest["interrupted"] == "test interruption")
    started_items = [item for item in review["checklist"] if item["id"] != "no-assert-dialogs"]
    ok &= row(results, "interruption/unstarted-checklist-retained", len(started_items) == 2 and started_items[1]["run"] == "second")
    ok &= row(results, "interruption/missing-video-explained", all(item["frames"] is None and item["finding"]["reason"] == "test interruption" for item in review["checklist"]))
    return ok


def check_item_assertions(results, scratch):
    root = scratch / "item-assertions"
    probe = root / "probe"
    probe.mkdir(parents=True)
    observed = {"complete": True, "pass": True, "script": {"steps": [{"op": "wait"}, {"op": "assert"}]},
                "steps": [{"index": 0, "observed": {"sim_frame": 100}}, {"index": 1, "observed": {"sim_frame": 700}}]}
    path = probe / "net-ui-result.json"
    path.write_text(json.dumps(observed), encoding="utf-8")
    record = {"root": str(root), "video_dir": str(root / "video"), "index": [], "probe_dir": str(probe)}
    _, evidence = driver.item_evidence(record, {"probe_steps": [0, 1], "sim_progress": 600})
    ok = row(results, "review/simulation-progress", evidence["probe"] == "pass")
    observed["steps"][1]["observed"]["sim_frame"] = 200
    path.write_text(json.dumps(observed), encoding="utf-8")
    _, evidence = driver.item_evidence(record, {"probe_steps": [0, 1], "sim_progress": 600})
    ok &= row(results, "review/missing-simulation-progress", evidence["probe"] == "fail")
    (root / "runtime").mkdir()
    (root / "runtime/LogConsole.txt").write_text("parked brains\nERROR: Lua failure\n", encoding="utf-8")
    checks = driver.log_assertions(root, ["parked brains"], ["^ERROR:"])
    ok &= row(results, "review/console-errors-retained", checks[0]["matches"][0]["line"] == 1 and checks[1]["matches"][0]["line"] == 2 and checks[1]["forbidden"])
    _, evidence = driver.item_evidence(record, {"log_regex": ["parked brains"], "forbidden_log_regex": ["^ERROR:"]})
    ok &= row(results, "review/positive-log-cannot-hide-lua-error", evidence["probe"] == "fail")
    observed['steps'][0]['observed']['control'] = {'text': 'Host connection lost / RTT -- ms'}
    path.write_text(json.dumps(observed), encoding='utf-8')
    item = {'readback': [{'step': 0, 'path': ['control', 'text'], 'not_contains': 'LIVE'}]}
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, 'review/forbidden-control-text', evidence['readback_assertions'][0]['pass'])
    item['readback'][0]['step'] = 2
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, 'review/missing-control-is-not-negative-proof', evidence['probe'] == 'fail')
    return ok


def check_listed_rows(results, scratch):
    root = scratch / "listed-rows"
    root.mkdir(parents=True)
    record = {"root": str(root), "video_dir": str(root / "video"), "index": [], "probe_dir": str(root / "probe")}
    own = "[LAN] Host - P4 A... Duel (1/2) 127.0.0.1:49402"
    other = "[LAN] Host - P4 A... Duel (1/2) 127.0.0.1:49466"

    def listing(rows, shown):
        text = ("[menu-script] assert_text_fits ListLanGames " + "".join(f" row={json.dumps(r)} width=221 available=244" for r in rows)
                + " PASS\n" + f'[menu-script] assert_label TextJoinPort "" text="{shown}" PASS\n')
        (root / "stdout.log").write_text(text, encoding="utf-8")

    rows_item = {"own_session_rows": {"control": "ListLanGames", "expected": 1, "address": "127.0.0.1"}}
    port_item = {"join_port_follows_list": {"control": "ListLanGames", "field": "TextJoinPort"}}
    listing([other, own], "49466")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok = row(results, "listed-rows/other-session-not-counted", evidence["probe"] == "pass" and evidence["own_session_rows"]["other_sessions"] == [other])
    _, evidence = driver.item_evidence(record, port_item, 49402)
    ok &= row(results, "listed-rows/port-follows-first-listed-row", evidence["probe"] == "pass")
    listing([other, own], "49402")
    _, evidence = driver.item_evidence(record, port_item, 49402)
    ok &= row(results, "listed-rows/port-not-following-list-fails", evidence["probe"] == "fail")
    listing([own, own.replace("127.0.0.1", "192.168.50.130")], "49402")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok &= row(results, "listed-rows/own-session-twice-fails", evidence["probe"] == "fail" and len(evidence["own_session_rows"]["own_rows"]) == 2)
    listing([own.replace("127.0.0.1", "192.168.50.130")], "49402")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok &= row(results, "listed-rows/own-session-off-loopback-fails", evidence["probe"] == "fail")
    (root / "stdout.log").write_text("[menu-script] assert_label TextJoinPort \"\" text=\"49402\" PASS\n", encoding="utf-8")
    _, evidence = driver.item_evidence(record, rows_item, 49402)
    ok &= row(results, "listed-rows/missing-readback-fails", evidence["probe"] == "fail" and evidence["own_session_rows"]["rows"] is None)
    directory = root / "directory"
    directory.mkdir()
    ok &= row(results, "directory-session/unlisted-is-none", driver.directory_session(directory, 49475) is None)
    (directory / "listed.json").write_text(json.dumps({"sessions": [{"listen_port": 49443, "session_id": "other"},
                                                                     {"listen_port": 49475, "session_id": "ours"}]}), encoding="utf-8")
    ok &= row(results, "directory-session/own-port-row", driver.directory_session(directory, 49475) == "ours")
    menu = root / "menu.txt"
    menu.write_text("settext TextJoinAddress session:{DIRECTORY_SESSION}\n", encoding="utf-8")
    bound = driver.bind_directory_session(menu, "ours")
    ok &= row(results, "directory-session/bound-into-script", bound and menu.read_text(encoding="utf-8") == "settext TextJoinAddress session:ours\n")
    return ok


def check_frame_gaps(results, scratch):
    root = scratch / "frame-gaps"
    root.mkdir(parents=True)
    index = [{"frame": 0, "wall_ms": 0, "screen": "MultiplayerScreen"},
             {"frame": 1, "wall_ms": 160, "screen": "MultiplayerScreen"},
             {"frame": 2, "wall_ms": 320, "screen": "MultiplayerScreen"}]
    record = {"root": str(root), "video_dir": str(root / "video"), "index": index, "probe_dir": str(root / "probe")}
    item = {"frame_gap": {"max_ms": 1000}}
    _, evidence = driver.item_evidence(record, item)
    ok = row(results, "review/frame-gap-clean-menu", evidence["probe"] == "pass" and evidence["frame_gap"]["worst"]["gap_ms"] == 160)
    index[2]["wall_ms"] = 2500
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "review/frame-gap-render-stall", evidence["probe"] == "fail" and evidence["frame_gap"]["over"][0]["gap_ms"] == 2340,
              json.dumps(evidence["frame_gap"]["over"]))
    index[2]["screen"] = "Loading"
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "review/frame-gap-ignores-loading", evidence["probe"] == "pass")
    _, evidence = driver.item_evidence({**record, "index": []}, item)
    ok &= row(results, "review/frame-gap-needs-frames", evidence["probe"] == "fail")
    return ok


def check_stop_request(results, scratch):
    out = scratch / "stop-request"
    out.mkdir()
    (out / "stop-request.json").write_text('{"class":"engine","reason":"unit stop"}', encoding="utf-8")
    handles = []

    class Handle:
        def __init__(self, root):
            self.out = self.cwd = Path(root)
            self.out.mkdir()
            self.stopped = threading.Event()
            self.closed = False
            handles.append(self)

        def start(self):
            return self

        def finish(self):
            if not self.stopped.wait(5):
                return {"exit_code": 124, "timed_out": True}
            return {"exit_code": 137, "injected_termination": self.reason}

        def terminate(self, reason):
            self.reason = reason
            self.stopped.set()

        def close(self):
            self.closed = True

    original_make, original_seed = driver.make_run, driver.seed_settings
    try:
        driver.make_run = lambda repo, args, root, timeout, env: Handle(root)
        driver.seed_settings = lambda handle, seed: None
        options = SimpleNamespace(repo=scratch, size="640x360", fps=3, port=49478, scratch_root=scratch)
        scenario = {"name": "stop", "path": "synthetic", "peers": [{"name": "host", "args": []}]}
        captured = driver.run_one(options, scenario, {"name": "first"}, 0, out)
    finally:
        driver.make_run, driver.seed_settings = original_make, original_seed
    ok = row(results, "interruption/stops-and-closes-runner", len(handles) == 1 and handles[0].closed and
               "unit stop" in captured["interrupted"] and captured["peers"][0]["record"]["exit_code"] == 137)
    scenario['checklist'] = [{'id': 'play', 'what': 'The match continues.', 'screen': 'game'}]
    report = driver.review(scenario, captured, out)
    ok &= row(results, 'interruption/engine-stop-keeps-its-class', report['run_findings'][0]['class'] == 'engine' and report['checklist'][0]['finding']['class'] == 'engine')
    return ok


def check_finalizer(results, scratch):
    out = scratch / "finalize"
    peer = out / "first/host"
    (peer / "video").mkdir(parents=True)
    (peer / "launch.json").write_text(json.dumps({"started": True, "exe_sha256": "retained-exe"}), encoding="utf-8")
    (peer / "video/frames.jsonl").write_text(json.dumps({"frame": 0, "wall_ms": 100, "sim_tick": 1, "screen": "game"}) + "\n", encoding="utf-8")
    scenario = {"name": "finalize", "peers": [{"name": "host"}], "runs": [{"name": "first"}, {"name": "second"}],
                "checklist": [{"id": "game", "what": "Game is drawn.", "screen": "game"}]}
    capture = {"scenario": "finalize", "scenario_definition": scenario, "runs": [], "source": {"tip": "retained-tip"},
               "exe": {"sha256": "retained-exe"}, "fps": 3, "started": driver.stamp(), "command": [],
               "scratch_root": str(scratch), "scratch_limit_bytes": 8_000_000_000}
    (out / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
    code = driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=True, scratch_root=scratch, sheet_every=3))
    manifest = json.loads((out / "manifest.json").read_text())
    review = json.loads((out / "review.json").read_text())
    saved = json.loads((out / "capture.json").read_text())
    ok = row(results, "finalize/keeps-provenance-and-frames", code == 1 and manifest["frame_count"] == 1 and manifest["source"]["tip"] == "retained-tip")
    ok &= row(results, "finalize/does-not-invent-process-exit", saved["runs"][0]["peers"][0]["record"]["exit_code"] is None)
    finalized_items = [item for item in review["checklist"] if item["id"] != "no-assert-dialogs"]
    ok &= row(results, "finalize/names-unstarted-run", len(finalized_items) == 2 and finalized_items[1]["run"] == "second")
    ok &= row(results, "finalize/manifest-retains-budget", manifest.get("scratch_limit_bytes") == 8_000_000_000 and
              manifest.get("scratch_root") == str(scratch))
    with patch.object(driver, "scratch_bytes", return_value=6_000_000_000), patch.object(driver, "render") as rendered:
        driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=False, scratch_root=None,
                                            scratch_limit_bytes=None, sheet_every=3))
        ok &= row(results, "finalize/encodes-below-retained-budget", rendered.call_count == 1)
    with patch.object(driver, "scratch_bytes", return_value=8_000_000_000), patch.object(driver, "render") as rendered:
        driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=False, scratch_root=None,
                                            scratch_limit_bytes=None, sheet_every=3))
        ok &= row(results, "finalize/stops-encoding-at-budget", rendered.call_count == 0)
    saved["scenario_definition"]["runs"] = [{"name": "first"}]
    saved.pop("interrupted", None)
    saved["runs"][0].pop("interrupted", None)
    (peer / "launch.json").write_text(json.dumps({"started": True, "exit_code": 0, "elapsed_seconds": 7.5}), encoding="utf-8")
    (out / "capture.json").write_text(json.dumps(saved), encoding="utf-8")
    (out / "manifest.json").write_text(json.dumps({"wall_seconds": 7.5}), encoding="utf-8")
    driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=True, scratch_root=scratch, sheet_every=3))
    complete = json.loads((out / "capture.json").read_text())
    measured = json.loads((out / "manifest.json").read_text())
    ok &= row(results, "finalize/preserves-completed-run-and-wall-time", not complete.get("interrupted") and measured["wall_seconds"] == 7.5)
    return ok


def check_completion(results, scratch):
    probe = scratch / "completion-probe"
    probe.mkdir()
    peer = {"probe_dir": str(probe), "record": {"exit_code": 0, "timed_out": False},
            "index": [{"frame": 0}], "video": "retained.mp4", "menu_script_failures": []}
    ok = row(results, "completion/clean", driver.peer_completed(peer))
    (probe / "probe.json").write_text('{}', encoding="utf-8")
    (probe / "net-ui-result.json").write_text('{"pass": false, "complete": false}', encoding="utf-8")
    ok &= row(results, "completion/probe-failure-with-zero-exit", not driver.peer_completed(peer))
    (probe / "net-ui-result.json").write_text('{"pass": true, "complete": true}', encoding="utf-8")
    peer["record"].update(exit_code=137, injected_termination="scenario drop after recorded tick 900")
    ok &= row(results, "completion/unplanned-exit", not driver.peer_completed(peer))
    peer["expected_termination"] = True
    ok &= row(results, "completion/planned-drop", driver.peer_completed(peer))
    peer["record"]["injected_termination"] = "another scenario peer failed"
    ok &= row(results, "completion/observer-abort-is-not-a-planned-drop", not driver.peer_completed(peer))
    return ok


def check_drop_receipts(results, scratch):
    root = scratch / "drop-receipt"
    video = root / "video"
    video.mkdir(parents=True)
    record = {"root": str(root), "video_dir": str(video), "probe_dir": str(root / "probe"),
              "record": {}, "index": [{"frame": 0, "wall_ms": 1000, "sim_tick": 601, "screen": "game"}]}
    item = {"id": "drop", "drop_tick": 601, "screen": "game"}
    _, evidence = driver.item_evidence(record, item)
    ok = row(results, "drop/no-receipt-is-not-proof", evidence["probe"] == "fail")
    driver.write_json(video / "injected-drop.json", {"requested_tick": 601, "last_recorded_frame": record["index"][0]})
    record["record"]["injected_termination"] = "capture interrupted"
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "drop/observer-stop-is-not-injected-drop", evidence["probe"] == "fail")
    record["record"]["injected_termination"] = "scenario drop after recorded tick 601"
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "drop/receipt-and-runner-termination", evidence["probe"] == "pass")
    record.update(peer="client", video=str(root / "client.mp4"), menu_script_failures=[])
    host = {**record, "peer": "host"}
    scenario = {"name": "drop", "checklist": [{"id": "host-sees-drop", "peer": "host", "screen": "game",
                 "peer_drop": {"peer": "client", "tick": 601}}]}
    capture = {"name": "run0", "peers": [host, record]}
    document = driver.review(scenario, capture, root)
    ok &= row(results, "drop/survivor-requires-peer-receipt", document["checklist"][0]["peer_drop"]["pass"])
    record["record"]["injected_termination"] = None
    document = driver.review(scenario, capture, root)
    ok &= row(results, "drop/peer-failure-is-not-planned-drop", document["checklist"][0]["probe"] == "fail")
    (root / "runtime").mkdir()
    (root / "runtime/LogConsole.txt").write_text("later process\n")
    (root / "console.log").write_text("original process\n")
    matches = driver.log_assertions(root, ["original process"], ["later process"])
    ok &= row(results, "resume/console-evidence-is-stable", len(matches[0]["matches"]) == 1 and not matches[1]["matches"])
    probe = root / "probe"
    probe.mkdir()
    value = {"steps": [{"index": 7, "observed": {"control": {"text": "ClientA is now hosting", "visible": True}}}]}
    driver.write_json(probe / "net-ui-result.json", value)
    item = {"screen": "game", "probe_steps": [7], "readback": [{"step": 7, "path": ["control", "text"], "contains": "is now hosting"}, {"step": 7, "path": ["control", "visible"], "equals": True}]}
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "readback/visible-named-toast", evidence["probe"] == "pass")
    value["steps"][0]["observed"]["control"]["visible"] = False
    driver.write_json(probe / "net-ui-result.json", value)
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "readback/hidden-text-is-not-visible-proof", evidence["probe"] == "fail")
    value["steps"][0]["observed"]["control"].update(visible=True, text="LIVE")
    driver.write_json(probe / "net-ui-result.json", value)
    _, evidence = driver.item_evidence(record, item)
    ok &= row(results, "readback/changed-toast-fails-without-blocking-observer", evidence["probe"] == "fail")
    return ok


def check_gameplay_epochs(results, scratch):
    video, stage = scratch / "epochs-video", scratch / "epochs-stage"
    video.mkdir(); stage.mkdir()
    rows = [{"frame": 0, "screen": "game", "sim_tick": 10, "wall_ms": 100},
            {"frame": 1, "screen": "Pause", "sim_tick": 20, "wall_ms": 200},
            {"frame": 2, "screen": "game", "sim_tick": 30, "wall_ms": 300},
            {"frame": 3, "screen": "game", "sim_tick": 0, "wall_ms": 400},
            {"frame": 4, "screen": "game", "sim_tick": 10, "wall_ms": 500}]
    (video / "frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
    driver.gameplay_signals(video, stage, 2)
    first = json.loads((stage / "gameplay-started.json").read_text())
    second = json.loads((stage / "gameplay-epoch-2.json").read_text())
    return row(results, "epochs/waits-for-recorded-counter-reset", first["frame"] == 0 and second["frame"] == 3 and second["sim_tick"] == 0)


def check_menu_commands(results):
    repo = TOOLS.parent
    source = (repo / "Source/Main.cpp").read_text() + (repo / "Source/Menus/MenuAutomation.cpp").read_text()
    known = set(re.findall(r'(?:command|cmd|op)\s*==\s*"([a-z_]+)"', source))
    unknown = []
    for path in driver.SCENARIO_DIR.glob("*.txt"):
        if ".menu." not in path.name and path.name not in ("sp-smoke.menus.txt", "wait-for-probe.txt"):
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            words = line.strip().split()
            if not words or words[0].startswith("#"):
                continue
            verb = words[1] if words[0] == "observe" else words[0]
            if verb not in known:
                unknown.append(f"{path.name}:{number}: {verb}")
    ok = row(results, "menu/verbs-exist-in-engine", not unknown, str(unknown))
    ok &= row(results, "menu/rejects-misspelled-ready-wait", "wait_remote_ready" in known and "wait_remoteready" not in known)
    return ok


def check_resume_seams(results, scratch):
    import run_sim_test
    from compare_sim_traces import CORE
    tokens = driver.supplied_tokens(["TURN_USER=override"], {"TURN_USER": "env", "TURN_PASS": "secret"})
    ok = row(results, "tokens/environment-and-override", tokens == {"TURN_USER": "override", "TURN_PASS": "secret"})
    scenario = {"name": "tokens", "path": "unit", "peers": [{"name": "host", "args": ["{TURN_SERVER}"]}]}
    missing = driver.run_preflight(scenario, {}, [], {})
    ok &= row(results, "tokens/unresolved-prevents-launch", missing["class"] == "harness" and missing["tokens"] == ["TURN_SERVER"])
    ok &= row(results, "tokens/resolved", driver.run_preflight(scenario, {}, [], {"TURN_SERVER": "turn:example"}) is None)
    scenario["peers"][0]["args"] = ["{DIRECTORY_ROOT}/listed.json"]
    ok &= row(results, "tokens/directory-service-root", driver.run_preflight(scenario, {}, [], {}) is None)
    same = {"name": "same", "peers": [{"name": "first", "args": []}, {"name": "second", "args": [], "retain_runtime_from": {"run": "same", "peer": "first"}, "start_when": {"peer": "first", "ended": True}}]}
    ok &= row(results, "resume/same-run-owner-must-end", driver.run_preflight({"path": "unit"}, same, [], {}) is None)
    same["peers"][1]["start_when"] = {"peer": "first", "sim_tick": 10}
    ok &= row(results, "resume/rejects-concurrent-runtime-use", driver.run_preflight({"path": "unit"}, same, [], {})["class"] == "harness")
    indexed = [{"frame": 0, "screen": "game", "sim_tick": 100, "service_state": "Starting"}, {"frame": 1, "screen": "game", "sim_tick": 1200, "service_state": "Running"}]
    ok &= row(results, "frames/rejoin-requires-running-service", driver.frame_range(indexed, {"screen": "game", "service_state": "Running"}) == [1, 1])
    runtime = scratch / "retained/runtime"
    (runtime / "Userdata").mkdir(parents=True)
    (runtime / "Autosaves").mkdir()
    (runtime / "Temp").mkdir()
    (runtime / "Userdata/Settings.ini").write_text("SettingsMan\n")
    (runtime / "Userdata/NetworkIdentity.key").write_bytes(b"same-identity")
    for tick in (60, 120):
        (runtime / f"Autosaves/match-{tick}.ccmanifest").write_text(f"MatchId = match\nSavedTick = {tick}\n")
        (runtime / f"Autosaves/match-{tick}.ccsave").write_bytes(b"retained-checkpoint")
    (runtime / "Autosaves/match.admission").write_bytes(b"same-admission")
    peer = {"peer": "host", "root": str(runtime.parent), "runtime": str(runtime), "record": {"exit_code": 137}}
    captures = [{"name": "died", "peers": [peer]}]
    condition = {"run": "died", "peers": ["host"], "ended": True}
    ok &= row(results, "resume/ended-prior-run", driver.cross_run_ready(captures, condition))
    ok &= row(results, "resume/no-missing-peer", not driver.cross_run_ready(captures, {**condition, "peers": ["host", "client"]}))
    saved, evidence = driver.checkpoint_tokens(peer)
    ok &= row(results, "resume/newest-complete-checkpoint", saved == {"RESUME_MATCH": "match", "RESUME_TICK": 120} and len(evidence) == 4)
    original = run_sim_test.IsolatedRun
    try:
        run_sim_test.IsolatedRun = lambda argv, cwd, out, timeout, **kwargs: SimpleNamespace(argv=argv, cwd=cwd, out=out, **kwargs)
        handle = run_sim_test.make_run(scratch, ["-test"], scratch / "resumed/host", runtime=runtime)
        ok &= row(results, "resume/same-runtime-without-copy", handle.cwd == runtime.resolve() and not (handle.out / "runtime").exists() and (runtime / "Userdata/NetworkIdentity.key").read_bytes() == b"same-identity")
    finally:
        run_sim_test.IsolatedRun = original
    rows = [{"tick": tick, "total": "a" * 64, "subsystems": {key: "b" * 64 for key in CORE | {"controller"}}} for tick in range(1, 6)]
    paths = [scratch / "left.json", scratch / "right.json"]
    for path in paths:
        driver.write_json(path, {"runs": [{"tick_hashes": rows}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-ok")
    ok &= row(results, "migration/full-post-boundary-range", result["status"] == "PASS" and result["exclusions"] == [])
    gate_root = scratch / "round-gate"
    gate_root.mkdir()
    for name in ("host", "client"):
        driver.write_json(gate_root / f"{name}_trace.json", {"runs": [{"tick_hashes": rows}]})
    capture = {"root": str(gate_root), "peers": [{"peer": "host"}, {"peer": "client"}]}
    driver.feel_probes({"hash_gate": {"name": "round2-hashes", "peers": ["host", "client"], "first_tick": 1, "cap": 5}}, capture, {})
    ok &= row(results, "hash-gate/both-peers-share-full-record-proof", all(peer["gates"]["round2-hashes"]["status"] == "PASS" for peer in capture["peers"]))
    rows[3]["subsystems"]["controller"] = "c" * 64
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-controller")
    ok &= row(results, "migration/controller-is-not-excluded", result["status"] == "FAIL" and result["first_difference"] == 4)
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:-1]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-missing")
    ok &= row(results, "migration/missing-cap-fails", result["status"] == "FAIL" and not result["full_rows_equal"])
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:2]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-prefix-only")
    ok &= row(results, 'migration/prefix-names-first-missing-tick', result['status'] == 'FAIL' and result['first_missing_tick'] == 3)
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:2] + rows[3:]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-gap")
    ok &= row(results, 'migration/gap-keeps-strict-failure-and-missing-tick', result['status'] == 'FAIL' and result['first_missing_tick'] == 3 and bool(result['validation_errors']))
    return ok


def check_cross_capture(results, scratch):
    from e2e.cross import select_peer, identity_agrees, merge_halves
    scenario = driver.load_scenario('mp-host-join-cross')
    host = select_peer(scenario, 'host')
    client = select_peer(scenario, 'client')
    ok = row(results, 'cross/one-local-peer', [peer['name'] for peer in host['peers']] == ['host'] and [peer['name'] for peer in client['peers']] == ['client'])
    ok &= row(results, 'cross/no-remote-file-rendezvous', all('{PROBE_DIR_' not in driver.scenario_text(value, value['peers'][0]['probe']) for value in (host, client)))
    ok &= row(results, 'cross/missing-address-skips-client', driver.run_preflight(client, {'name': 'run0', 'peers': client['peers']}, [], {})['tokens'] == ['HOST_ADDRESS'])
    ok &= row(results, 'cross/host-needs-no-address-token', driver.run_preflight(host, {'name': 'run0', 'peers': host['peers']}, [], {}) is None)
    ok &= row(results, 'cross/address-token-from-environment', driver.supplied_tokens([], {'HOST_ADDRESS': '192.0.2.1'}) == {'HOST_ADDRESS': '192.0.2.1'})
    identity = {'session_id': 5, 'round': 7, 'config_hash': 'a' * 64, 'peer_id': 1, 'host': True}
    remote = {**identity, 'peer_id': 2, 'host': False}
    ok &= row(results, 'cross/shared-native-identity', identity_agrees([identity, remote]))
    ok &= row(results, 'cross/reject-different-round', not identity_agrees([identity, {**remote, 'round': 8}]))
    ok &= row(results, 'cross/reject-same-peer', not identity_agrees([identity, {**remote, 'peer_id': 1}]))
    roots = []
    for name, definition, ident, platform in [('host', host, identity, 'win32'), ('client', client, remote, 'darwin')]:
        root = scratch / ('cross-' + name)
        root.mkdir()
        roots.append(root)
        video, sheet = root / (name + '.mp4'), root / (name + '-sheet.png')
        video.write_bytes(b'synthetic contract video')
        sheet.write_bytes(b'synthetic contract sheet')
        driver.write_json(root / 'capture.json', {'synthetic': True, 'scenario': scenario['name'], 'scenario_definition': definition, 'out': str(root)})
        driver.write_json(root / 'manifest.json', {'synthetic': True, 'platform': platform, 'source': {'tip': 'synthetic'}, 'frame_count': 1, 'peers': [{'peer': name, 'match_identity': ident, 'video': driver.file_evidence(video), 'contact_sheet': driver.file_evidence(sheet)}]})
        driver.write_json(root / 'review.json', {'scenario': scenario['name'], 'checklist': [{'id': name, 'peer': name, 'frames': [0, 0], 'probe': 'pass'}]})
    ok &= row(results, 'cross/merged-media-and-contract', merge_halves(roots, scratch / 'cross-merged'))
    merged = json.loads((scratch / 'cross-merged/manifest.json').read_text())
    ok &= row(results, 'cross/shared-key-and-platforms', merged['match_identity_gate']['match_id'] == '0000000000000005-0000000000000007' and merged['match_identity_gate']['cross_platform'])
    # Every real half carries the frameless assert-dialog row; it passes on its probe and fails the merge when it fails.
    for root, name in zip(roots, ('host', 'client')):
        driver.write_json(root / 'review.json', {'scenario': scenario['name'], 'checklist': [
            {'id': name, 'peer': name, 'frames': [0, 0], 'probe': 'pass'},
            {'id': 'no-assert-dialogs', 'peer': 'all', 'frames': None, 'state': 'checked', 'probe': 'pass'}]})
    ok &= row(results, 'cross/assert-dialog-row-passes-on-its-probe', merge_halves(roots, scratch / 'cross-merged-dialogs'))
    dialog = json.loads((roots[1] / 'review.json').read_text())
    dialog['checklist'][1].update(probe='fail', finding={'class': 'engine', 'reason': 'assert dialog: synthetic'})
    driver.write_json(roots[1] / 'review.json', dialog)
    ok &= row(results, 'cross/assert-dialog-fails-the-merge', not merge_halves(roots, scratch / 'cross-merged-dialog-red'))
    dialog['checklist'][1].update(probe='pass')
    dialog['checklist'][1].pop('finding')
    driver.write_json(roots[1] / 'review.json', dialog)
    (roots[1] / 'client.mp4').write_bytes(b'changed synthetic video')
    ok &= row(results, 'cross/transferred-media-tamper-fails', not merge_halves(roots, scratch / 'cross-tampered'))
    bad = {'name': 'run0', 'peers': [{'name': 'host', 'kill_when': {'peer': 'absent', 'event': 'ready'}}]}
    ok &= row(results, 'drop/unknown-event-peer-skips-before-launch', driver.run_preflight({'peers': bad['peers']}, bad, [], {})['class'] == 'harness')
    good = {'name': 'run0', 'peers': [{'name': 'host', 'kill_when': {'peer': 'host', 'probe_complete': True}}]}
    ok &= row(results, 'drop/completed-probe-gate', driver.run_preflight({'peers': good['peers']}, good, [], {}) is None)
    result = scratch / 'completed-probe.json'
    result.write_text('{"complete":true,"pass":false}')
    ok &= row(results, 'drop/failed-probe-is-not-planned-completion', not driver.completed_probe(result))
    result.write_text('{"complete":true,')
    ok &= row(results, 'drop/partial-write-keeps-observing', not driver.completed_probe(result))
    result.write_text('{"complete":true,"pass":true}')
    ok &= row(results, 'drop/successful-probe-completes', driver.completed_probe(result))
    return ok


def check_cross_transfer(results, scratch):
    from e2e.cross import CaptureRelocation, merge_halves, select_peer
    scenario = driver.load_scenario('mp-host-join-cross')
    identities = {'host': {'session_id': 5, 'round': 7, 'config_hash': 'a' * 64, 'peer_id': 1, 'host': True},
                  'client': {'session_id': 5, 'round': 7, 'config_hash': 'a' * 64, 'peer_id': 2, 'host': False}}
    roots, originals = [], {}
    for name, platform in (('host', 'win32'), ('client', 'darwin')):
        root = scratch / ('transferred-' + name)
        (root / 'run0' / (name + '-stage') / 'probe').mkdir(parents=True)
        roots.append(root)
        original = str(root) if name == 'host' else '/Users/erol/captures/client-half'
        originals[name] = original
        def evidence(relative, data):
            path = root / relative
            path.write_bytes(data)
            return {**driver.file_evidence(path), 'path': original.replace('\\', '/') + '/' + relative}
        video = evidence(f'run0/{name}.mp4', b'synthetic transferred video')
        sheet = evidence(f'run0/{name}-sheet.png', b'synthetic transferred sheet')
        native = evidence(f'run0/{name}-stage/probe/match-identity.json', json.dumps(identities[name]).encode())
        trace = evidence(f'run0/{name}_trace.json', b'{"synthetic":true,"ticks":[1,2]}')
        peer = {'peer': name, 'match_identity': identities[name], 'video': video, 'contact_sheet': sheet,
                'identity_file': native, 'trace': trace}
        driver.write_json(root / 'capture.json', {'scenario': scenario['name'], 'scenario_definition': select_peer(scenario, name), 'out': original})
        driver.write_json(root / 'manifest.json', {'platform': platform, 'source': {'tip': 'synthetic-transfer'}, 'frame_count': 2, 'peers': [peer]})
        driver.write_json(root / 'review.json', {'scenario': scenario['name'], 'command': ['--out', original], 'checklist': [{
            'id': name, 'peer': name, 'frames': [0, 1], 'probe': 'pass', 'video': video['path'], 'contact_sheet': sheet['path'],
            'identity_file': native, 'trace': trace, 'readback': [{'path': ['control', 'visible'], 'equals': True}]}]})
    before = {str(path): path.read_bytes() for root in roots for path in (root / 'capture.json', root / 'manifest.json', root / 'review.json')}
    ok = row(results, 'cross-transfer/unix-half-relocates', merge_halves(roots, scratch / 'transferred-merged'))
    manifest = json.loads((scratch / 'transferred-merged/manifest.json').read_text())
    review = json.loads((scratch / 'transferred-merged/review.json').read_text())
    peer = next(peer for peer in manifest['peers'] if peer['peer'] == 'client')
    item = next(item for item in review['checklist'] if item['peer'] == 'client')
    ok &= row(results, 'cross-transfer/original-media-path-retained', peer['video'].get('original_path') == originals['client'] + '/run0/client.mp4')
    ok &= row(results, 'cross-transfer/review-media-and-auxiliary-paths', item['video'] == str(roots[1] / 'run0/client.mp4') and
              item['identity_file']['path'] == str(roots[1] / 'run0/client-stage/probe/match-identity.json') and
              item['trace']['path'] == str(roots[1] / 'run0/client_trace.json'))
    ok &= row(results, 'cross-transfer/provenance-and-inputs-unchanged', bool(manifest.get('relocations')) and
              all(Path(path).read_bytes() == data for path, data in before.items()))
    ok &= row(results, 'cross-transfer/property-path-is-not-a-file', item['readback'][0]['path'] == ['control', 'visible'])
    trace_path = roots[1] / 'run0/client_trace.json'
    trace_bytes = trace_path.read_bytes()
    trace_path.write_bytes(b'changed trace')
    ok &= row(results, 'cross-transfer/changed-trace-refused', not merge_halves(roots, scratch / 'transferred-bad-trace'))
    trace_path.write_bytes(trace_bytes)
    native_path = roots[1] / 'run0/client-stage/probe/match-identity.json'
    native_bytes = native_path.read_bytes()
    native_path.write_bytes(b'{"changed":true}')
    ok &= row(results, 'cross-transfer/changed-identity-refused', not merge_halves(roots, scratch / 'transferred-bad-identity'))
    native_path.write_bytes(native_bytes)
    client_manifest = json.loads((roots[1] / 'manifest.json').read_text())
    outside = scratch / 'outside-client.mp4'
    outside.write_bytes(b'synthetic transferred video')
    saved_video = client_manifest['peers'][0]['video']
    for variant, escaped in [('outside', str(outside)), ('traversal', originals['client'] + '/../outside-client.mp4')]:
        client_manifest['peers'][0]['video'] = {**saved_video, 'path': escaped}
        driver.write_json(roots[1] / 'manifest.json', client_manifest)
        ok &= row(results, f'cross-transfer/{variant}-refused', not merge_halves(roots, scratch / ('transferred-' + variant)))
    client_manifest['peers'][0]['video'] = saved_video
    saved_trace = client_manifest['peers'][0].pop('trace')
    client_manifest['cross_auxiliary'] = {'schema': 1, 'required': ['identity_file', 'trace']}
    driver.write_json(roots[1] / 'manifest.json', client_manifest)
    ok &= row(results, 'cross-transfer/fresh-capture-requires-auxiliary-hashes', not merge_halves(roots, scratch / 'transferred-missing-required-trace'))
    client_manifest['peers'][0]['trace'] = saved_trace
    driver.write_json(roots[1] / 'manifest.json', client_manifest)
    client_review = json.loads((roots[1] / 'review.json').read_text())
    client_review['checklist'][0]['video'] = str(outside)
    driver.write_json(roots[1] / 'review.json', client_review)
    ok &= row(results, 'cross-transfer/review-path-outside-refused', not merge_halves(roots, scratch / 'transferred-review-outside'))
    relocation = CaptureRelocation(originals['client'], roots[1])
    with patch.object(Path, 'resolve', return_value=scratch / 'outside-resolved-file'):
        try:
            relocation.path(originals['client'] + '/run0/client.mp4', 'video')
            rejected = False
        except ValueError:
            rejected = True
    ok &= row(results, 'cross-transfer/resolved-link-escape-refused', rejected)
    produced = scratch / 'cross-producer'
    (produced / 'probe').mkdir(parents=True)
    driver.write_json(produced / 'probe/match-identity.json', identities['host'])
    (produced / 'host_trace.json').write_text('{"synthetic":true}')
    native_peer = {'peer': 'host', 'root': str(produced), 'record': {}, 'index': [], 'manifest': {},
                   'probe_dir': str(produced / 'probe'), 'args': ['-out', str(produced / 'host_trace.json')]}
    capture = {'scenario': scenario['name'], 'scenario_definition': select_peer(scenario, 'host'),
               'source': {'tip': 'synthetic'}, 'exe': {}, 'started': driver.stamp(), 'fps': 6,
               'runs': [{'name': 'run0', 'size': '640x360', 'peers': [native_peer]}]}
    produced_manifest = driver.scenario_manifest(capture, produced, 1)
    ok &= row(results, 'cross-transfer/producer-hashes-native-identity-and-trace',
              produced_manifest.get('cross_auxiliary', {}).get('required') == ['identity_file', 'trace'] and
              produced_manifest['peers'][0]['identity_file']['sha256'] == driver.file_evidence(produced / 'probe/match-identity.json')['sha256'] and
              produced_manifest['peers'][0]['trace']['sha256'] == driver.file_evidence(produced / 'host_trace.json')['sha256'])
    return ok


def check_migration_timing(results, scratch):
    from e2e.timing import migration_timing
    root = scratch / 'timing'
    root.mkdir()
    events = [
        {'wall_ms': 2000, 'message': 'net_status peer=2 host_lost=1 local_slow=1'},
        {'wall_ms': 4600, 'message': 'net_status peer=2 host_lost=0 local_slow=1'},
        {'wall_ms': 4700, 'message': 'handover_toast peer=2 longest_ms=2700 text=Host left - ClientA is now hosting'},
        {'wall_ms': 5000, 'message': 'net_status peer=2 host_lost=0 local_slow=0'},
        {'wall_ms': 5500, 'message': 'net_status peer=2 host_lost=0 local_slow=1'}]
    (root / 'events.jsonl').write_text('\n'.join(json.dumps(event) for event in events))
    capture = {'peers': [{'peer': 'host', 'index': [{'frame': 1, 'screen': 'game', 'wall_ms': 1900}]},
                         {'peer': 'clienta', 'video_dir': str(root), 'index': [{'frame': 2, 'wall_ms': 4750}, {'frame': 3, 'wall_ms': 6000}]}]}
    item = migration_timing(capture, ['clienta'])['survivors'][0]
    ok = row(results, 'migration/timing-from-host-last-frame', item['seconds_from_host_last_frame'] == 2.8 and item['overlay_longest_ms_at_toast'] == 2700 and item['toast_nearest_frame']['frame'] == 2)
    ok &= row(results, 'migration/complete-and-truncated-slow-banners', item['local_slow_intervals'] == [{'start_ms': 2000, 'end_ms': 5000, 'seconds': 3.0, 'complete': True}, {'start_ms': 5500, 'end_ms': 6000, 'seconds': .5, 'complete': False}])
    capture['peers'][0]['index'] = []
    item = migration_timing(capture, ['clienta'])['survivors'][0]
    ok &= row(results, 'migration/no-invented-loss-time', not item['timing_complete'] and item['seconds_from_host_last_frame'] is None)
    return ok


def check_recorder_flush(results, scratch):
    """A probe-complete drop waits for the frames the recorder already took; it never waits past its bound."""
    video = scratch / "recorder-flush" / "video"
    video.mkdir(parents=True)
    stop = threading.Event()
    missing = driver.await_recorder(video, stop, timeout_s=.2)
    ok = row(results, "recorder-flush/no-output-does-not-wait", missing["flushed"] is False and missing["reason"] == "no recorder output")
    (video / "events.jsonl").write_text('{"wall_ms": 1000, "message": "video_mark toast-open"}\n{"wall_ms": 1200, "message": "net_status"}\n')
    (video / "frames.jsonl").write_text('{"frame": 0, "wall_ms": 990, "saved": true}\n')
    behind = driver.await_recorder(video, stop, timeout_s=.2)
    ok &= row(results, "recorder-flush/behind-times-out", behind["flushed"] is False and behind["reason"] == "timed out"
              and behind["target_wall_ms"] == 1200 and behind["last_frame_wall_ms"] == 990, str(behind))

    def write_late_frame():
        with (video / "frames.jsonl").open("a") as index:
            index.write('{"frame": 1, "wall_ms": 1210, "saved": true}\n')
    writer = threading.Timer(.1, write_late_frame)
    writer.start()
    caught_up = driver.await_recorder(video, stop, timeout_s=5)
    writer.join()
    ok &= row(results, "recorder-flush/waits-for-the-written-frame", caught_up["flushed"] is True and caught_up["last_frame_wall_ms"] == 1210, str(caught_up))
    stop.set()
    (video / "events.jsonl").write_text('{"wall_ms": 5000, "message": "late"}\n')
    stopped = driver.await_recorder(video, stop, timeout_s=5)
    ok &= row(results, "recorder-flush/stops-with-the-watchers", stopped["flushed"] is False and stopped["reason"] == "capture stopping", str(stopped))
    return ok


def check_log_gates(results, scratch):
    """A gate on a peer's own stdout line: a drop or a start waits for the product to print it."""
    log = scratch / "log-gate" / "stdout.log"
    log.parent.mkdir()
    log.write_text("[net-match] held client: replaying the private committed tail\n")
    pattern = r"\[net-match\] private catch-up complete frame=\d+"
    ok = row(results, "log-gate/unprinted-line-is-unmet", not driver.log_line_seen(log, pattern))
    ok &= row(results, "log-gate/missing-log-is-unmet", not driver.log_line_seen(log.parent / "absent.log", pattern))
    log.write_text(log.read_text() + "[net-match] private catch-up complete frame=742\n")
    ok &= row(results, "log-gate/printed-line-is-met", driver.log_line_seen(log, pattern))
    peers = [{"name": "client", "kill_when": {"peer": "client", "log": pattern}}]
    ok &= row(results, "log-gate/drop-on-a-log-line-passes-preflight", driver.run_preflight({"peers": peers}, {"name": "run0", "peers": peers}, [], {}) is None)
    both = [{"name": "client", "kill_when": {"peer": "client", "log": pattern, "event": "video_mark x"}}]
    ok &= row(results, "log-gate/drop-with-two-triggers-is-refused",
              driver.run_preflight({"peers": both}, {"name": "run0", "peers": both}, [], {})["class"] == "harness")
    scenario = driver.load_scenario("mp-rollback-lag")
    peers = {peer["name"]: peer for peer in scenario["runs"][0]["peers"]}
    ok &= row(results, "log-gate/rollback-lag-drops-the-client-after-catch-up", peers["client"].get("kill_when", {}).get("log") == pattern)
    ok &= row(results, "log-gate/rollback-lag-survivor-waits-for-the-client-route", peers["survivor"].get("start_when", {}).get("peer") == "host"
              and "RouteAllowed" in peers["survivor"]["start_when"].get("log", ""))
    return ok


def check_fullstate_applicability(results, scratch):
    """A pair that never started a lockstep round has no full-state verdict; one that started and shares no sample is red."""
    root = scratch / "fullstate-applicability"
    root.mkdir()
    host, client = root / "host.log", root / "client.log"
    host.write_text("[menu-script] dump_lobby state=Completed members=2\n")
    client.write_text("[menu-script] dump_lobby state=Failed members=1\n")
    lobby = driver.fullstate_verdict(host, client)
    ok = row(results, "fullstate/lobby-scenario-is-not-applicable", lobby["passed"] is None and bool(lobby["not_applicable"]), str(lobby))
    capture = {"name": "run0", "peers": [], "fullstate": {"host/client": lobby}}
    document = driver.review({"name": "lobby", "checklist": []}, capture, root)
    ok &= row(results, "fullstate/not-applicable-is-no-finding", not any("full-state" in f["reason"] for f in document["run_findings"]))
    client.write_text(driver.LOCKSTEP_ROUND_START + "1 frame=1 local_peer=2 peers=2 input_delay=3\n")
    started = driver.fullstate_verdict(host, client)
    ok &= row(results, "fullstate/started-round-without-samples-is-red", started["passed"] is False
              and started["reasons"] == ["the peers share no full-state sample"], str(started))
    capture["fullstate"] = {"host/client": started}
    document = driver.review({"name": "lobby", "checklist": []}, capture, root)
    ok &= row(results, "fullstate/red-verdict-is-a-finding", any("full-state oracle" in f["reason"] for f in document["run_findings"]))
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path)
    options = parser.parse_args()
    results = []
    if options.out:
        options.out.mkdir(parents=True, exist_ok=True)
        retained = options.out / "scratch"
        retained.mkdir()
    with contextlib.nullcontext(retained) if options.out else tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        ok = check_scenarios(results)
        ok &= check_capture_binary(results, scratch)
        ok &= check_scratch_limit(results, scratch)
        ok &= check_module_requirements(results, scratch)
        ok &= check_rematch_contract(results)
        ok &= check_substitution(results)
        ok &= check_launch_contract(results, scratch)
        ok &= check_index_and_checklist(results, scratch)
        ok &= check_encode(results, scratch)
        ok &= check_review(results, scratch)
        ok &= check_interruption(results, scratch)
        ok &= check_item_assertions(results, scratch)
        ok &= check_listed_rows(results, scratch)
        ok &= check_frame_gaps(results, scratch)
        ok &= check_stop_request(results, scratch)
        ok &= check_finalizer(results, scratch)
        ok &= check_completion(results, scratch)
        ok &= check_resume_seams(results, scratch)
        ok &= check_menu_commands(results)
        ok &= check_drop_receipts(results, scratch)
        ok &= check_gameplay_epochs(results, scratch)
        ok &= check_cross_capture(results, scratch)
        ok &= check_cross_transfer(results, scratch)
        ok &= check_migration_timing(results, scratch)
        ok &= check_recorder_flush(results, scratch)
        ok &= check_fullstate_applicability(results, scratch)
        ok &= check_log_gates(results, scratch)
    summary = {"schema": 1, "pass": bool(ok), "rows": results,
               "needs_a_real_capture": ["the engine's -record-video output itself",
                                        "ffmpeg encode of a real frame sequence",
                                        "the contact sheet's burned-in labels",
                                        "every scenario's menu script and probe against a running engine"]}
    if options.out:
        options.out.mkdir(parents=True, exist_ok=True)
        (options.out / "e2e-video-selftest.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"[e2e-video] {'PASS' if ok else 'FAIL'} {sum(1 for r in results if r['pass'])}/{len(results)} rows")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
