"""The e2e video driver's own rows: scenario shape, token substitution, checklist resolution, encode commands.

    python tools/test_e2e_video.py --repo <tree> --out <dir>

Nothing here launches the engine or touches a capture: every row runs against a synthetic frame index in
its own temporary directory, so this suite is safe to run on a machine that is building. The rows that need
a real capture are named at the bottom of the result and are the driver's own first run's job.
"""

import argparse
import contextlib
import json
import re
from pathlib import Path
import sys
import tempfile
import threading
from types import SimpleNamespace

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import e2e_video as driver  # noqa: E402

SCENARIOS = ("sp-smoke", "mp-host-join", "mp-reconnect-repair", "world-late-join", "ui-surfaces",
             "mod-void-wanderers", "mp-leave", "mp-rematch", "mp-rollback-lag",
             "mp-moderation", "mp-host-migration", "mp-resume-from-disk", "mp-direct-vs-relay",
             "mod-void-wanderers-multiplayer")


def row(results, name, ok, detail=""):
    results.append({"row": name, "pass": bool(ok), "detail": detail})
    print(f"[e2e-video] {'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
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
    ok &= row(results, "review/no-probe-is-named",
              all(item.get("probe") == "none" for item in document["checklist"]))
    ok &= row(results, "review/verdict-is-not-a-pass", document["verdict"] == "agent-review-required")
    ok &= row(results, "review/no-mp4-is-not-video-evidence", all(item["frames"] is None for item in document["checklist"]))
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
    ok &= row(results, "interruption/unstarted-checklist-retained", len(review["checklist"]) == 2 and review["checklist"][1]["run"] == "second")
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
    return ok


def check_stop_request(results, scratch):
    out = scratch / "stop-request"
    out.mkdir()
    (out / "stop-request.json").write_text('{"reason":"unit stop"}', encoding="utf-8")
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
    return row(results, "interruption/stops-and-closes-runner", len(handles) == 1 and handles[0].closed and
               "unit stop" in captured["interrupted"] and captured["peers"][0]["record"]["exit_code"] == 137)


def check_finalizer(results, scratch):
    out = scratch / "finalize"
    peer = out / "first/host"
    (peer / "video").mkdir(parents=True)
    (peer / "launch.json").write_text(json.dumps({"started": True, "exe_sha256": "retained-exe"}), encoding="utf-8")
    (peer / "video/frames.jsonl").write_text(json.dumps({"frame": 0, "wall_ms": 100, "sim_tick": 1, "screen": "game"}) + "\n", encoding="utf-8")
    scenario = {"name": "finalize", "peers": [{"name": "host"}], "runs": [{"name": "first"}, {"name": "second"}],
                "checklist": [{"id": "game", "what": "Game is drawn.", "screen": "game"}]}
    capture = {"scenario": "finalize", "scenario_definition": scenario, "runs": [], "source": {"tip": "retained-tip"},
               "exe": {"sha256": "retained-exe"}, "fps": 3, "started": driver.stamp(), "command": []}
    (out / "capture.json").write_text(json.dumps(capture), encoding="utf-8")
    code = driver.finalize_only(SimpleNamespace(finalize_only=out, metadata_only=True, scratch_root=scratch, sheet_every=3))
    manifest = json.loads((out / "manifest.json").read_text())
    review = json.loads((out / "review.json").read_text())
    saved = json.loads((out / "capture.json").read_text())
    ok = row(results, "finalize/keeps-provenance-and-frames", code == 1 and manifest["frame_count"] == 1 and manifest["source"]["tip"] == "retained-tip")
    ok &= row(results, "finalize/does-not-invent-process-exit", saved["runs"][0]["peers"][0]["record"]["exit_code"] is None)
    ok &= row(results, "finalize/names-unstarted-run", len(review["checklist"]) == 2 and review["checklist"][1]["run"] == "second")
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
    rows[3]["subsystems"]["controller"] = "c" * 64
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-controller")
    ok &= row(results, "migration/controller-is-not-excluded", result["status"] == "FAIL" and result["first_difference"] == 4)
    driver.write_json(paths[1], {"runs": [{"tick_hashes": rows[:-1]}]})
    result = driver.compare_hash_range(*paths, 3, 5, scratch / "range-missing")
    ok &= row(results, "migration/missing-cap-fails", result["status"] == "FAIL" and not result["full_rows_equal"])
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
        ok &= check_substitution(results)
        ok &= check_launch_contract(results, scratch)
        ok &= check_index_and_checklist(results, scratch)
        ok &= check_encode(results, scratch)
        ok &= check_review(results, scratch)
        ok &= check_interruption(results, scratch)
        ok &= check_item_assertions(results, scratch)
        ok &= check_stop_request(results, scratch)
        ok &= check_finalizer(results, scratch)
        ok &= check_completion(results, scratch)
        ok &= check_resume_seams(results, scratch)
        ok &= check_menu_commands(results)
        ok &= check_drop_receipts(results, scratch)
        ok &= check_gameplay_epochs(results, scratch)
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
