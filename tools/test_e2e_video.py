"""The e2e video driver's own rows: scenario shape, token substitution, checklist resolution, encode commands.

    python tools/test_e2e_video.py --repo <tree> --out <dir>

Nothing here launches the engine or touches a capture: every row runs against a synthetic frame index in
its own temporary directory, so this suite is safe to run on a machine that is building. The rows that need
a real capture are named at the bottom of the result and are the driver's own first run's job.
"""

import argparse
import contextlib
import json
from pathlib import Path
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import e2e_video as driver  # noqa: E402

SCENARIOS = ("sp-smoke", "mp-host-join", "mp-reconnect-repair", "world-late-join", "ui-surfaces",
             "mod-void-wanderers")


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
    return ok


def check_review(results, scratch):
    scenario = driver.load_scenario("mp-host-join")
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
