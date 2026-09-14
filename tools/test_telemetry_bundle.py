"""Exercise diagnostics through isolated runs; all runs menu+replay, pause is explicit.

The pause arm does not record a replay, so its diagnostics ZIPs contain none.
"""

import argparse
import hashlib
import json
import os
import re
import secrets
import struct
import threading
import zipfile
from pathlib import Path

from run_sim_test import make_run

CAP = 8 * 1024 * 1024
LOG_CAP = 512 * 1024
MEMBERS = {"LogConsole.txt", "NetMatch.log", "JoinIdentity.json", "DesyncHeal.json",
           "Settings.ini", "SystemInfo.json", "Executable.json", "Replay.status.json", "manifest.json"}
IDENTITY_FIELDS = {"game_version", "network_protocol_version", "controller_frame_version",
                   "controller_frame_encoded_size", "delta_time_bits", "ai_update_interval",
                   "pathfinder_grid_node_size", "recommended_moid_count", "particle_settling",
                   "mo_subtraction", "num_lua_states", "num_lua_states_override", "selected_module",
                   "scenario_test_module_loaded", "lockstep_codec_version", "enabled_global_scripts"}
SECRET_KEYS = ("SessionDirectoryInstallKey", "NetworkTurnPass", "NetworkTurnUser", "SessionDirectoryCertSha256")
SECRET_NEEDLES = ("Pass", "Password", "Secret", "Token", "PrivateKey", "Credential", "Ticket")


def secret_settings_key(name: str) -> bool:
    return name in SECRET_KEYS or any(needle in name for needle in SECRET_NEEDLES)


def plant_bundle_secrets(run) -> dict:
    path = run.cwd / "Userdata/Settings.ini"
    settings = path.read_text(encoding="utf-8")
    token = secrets.token_hex(8)
    planted = {
        "NetworkTurnPass": f"hunter2-{token}",
        "SessionDirectoryInstallKey": f"fakeinstall{token}",
        "NetworkTurnUser": f"turnuser{token}",
        "SessionDirectoryCertSha256": secrets.token_hex(32),
        "NetworkInputDelayFrames": "0",
    }
    for key, value in planted.items():
        settings, count = re.subn(rf"(?m)^(\s*{key}\s*=\s*)[^\r\n]*", lambda match, value=value: match[1] + value, settings)
        if count == 0:
            settings += f"\n\t{key} = {value}\n"
    path.write_text(settings, encoding="utf-8")
    metadata_path = run.out / "runtime.json"
    if metadata_path.exists():
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["settings_sha256"] = sha(path)
        metadata.setdefault("settings_overrides", {}).update(planted)
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return planted


def check_no_secrets(contents: dict, original: bytes, planted: dict | None) -> None:
    delay_line = None
    for line in original.splitlines():
        text = line.decode("utf-8", errors="replace")
        match = re.match(r"^(\s*)([^;/#=\s][^=]*?)\s*=\s*(.*)$", text)
        if not match:
            continue
        key, value = match.group(2).strip(), match.group(3)
        if key == "NetworkInputDelayFrames":
            delay_line = line
        if secret_settings_key(key) and value:
            token = value.encode("utf-8")
            for name, data in contents.items():
                if token in data:
                    leaked = next((row.decode("utf-8", errors="replace") for row in data.splitlines() if token in row), name)
                    raise AssertionError(f"no_secrets: {key} leaked in {name}: {leaked}")
    assert delay_line is not None, "NetworkInputDelayFrames missing from private Settings.ini"
    assert delay_line in contents["Settings.ini"].splitlines(), "harmless NetworkInputDelayFrames line was rewritten"
    if not planted:
        return
    for key, value in planted.items():
        if key == "NetworkInputDelayFrames":
            continue
        assert value.encode("utf-8") in original, f"{key} missing from private Settings.ini after the run"
        token = value.encode("utf-8")
        for name, data in contents.items():
            if token in data:
                leaked = next((row.decode("utf-8", errors="replace") for row in data.splitlines() if token in row), name)
                raise AssertionError(f"no_secrets: {key} leaked in {name}: {leaked}")
    listed = json.loads(contents["manifest.json"]).get("redacted")
    assert isinstance(listed, list), "manifest.json missing redacted key names"
    missing = [key for key in planted if key != "NetworkInputDelayFrames" and key not in listed]
    assert not missing, f"manifest.json redacted missing {missing}: {listed}"


def sha(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def read_log(root: Path) -> str:
    return "\n".join((root / name).read_text(encoding="utf-8", errors="replace")
                     for name in ("stdout.log", "stderr.log") if (root / name).exists())


def inspect_bundle(runtime: Path, exe_sha: str, replay: Path | None = None, secrets: dict | None = None) -> dict:
    archives = sorted((runtime / "Telemetry").glob("diag-*.zip"))
    if len(archives) != 1:
        raise AssertionError(f"expected one telemetry zip, found {len(archives)} in {runtime / 'Telemetry'}")
    path = archives[0]
    assert re.fullmatch(r"diag-\d{8}-\d{6}\.zip", path.name), path.name
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        expected = MEMBERS | ({"Replay.ccrp"} if replay is not None else set())
        assert len(names) == len(set(names)), "duplicate zip members"
        assert set(names) == expected, f"members {sorted(names)} != {sorted(expected)}"
        for entry in archive.infolist():
            assert not entry.is_dir() and 0 <= entry.file_size <= CAP, (entry.filename, entry.file_size)
        assert archive.testzip() is None, "zip CRC failure"
        contents = {name: archive.read(name) for name in names}
    manifest = json.loads(contents["manifest.json"])
    listed = manifest["members"]
    assert len(listed) == len(contents) - 1, "manifest member count"
    assert {row["name"] for row in listed} == set(contents) - {"manifest.json"}, "manifest coverage"
    for row in listed:
        data = contents[row["name"]]
        assert row["size"] == len(data), f"size mismatch: {row['name']}"
        assert row["sha256"] == hashlib.sha256(data).hexdigest(), f"sha256 mismatch: {row['name']}"
    assert len(contents["LogConsole.txt"]) <= LOG_CAP, "console tail cap"
    assert len(contents["NetMatch.log"]) <= LOG_CAP, "network tail cap"
    identity = json.loads(contents["JoinIdentity.json"])
    assert IDENTITY_FIELDS == set(identity["deterministic_config"]), "identity inputs incomplete"
    assert re.fullmatch(r"[0-9a-f]{64}", identity["module_manifest_hash"]), "module manifest hash"
    executable = json.loads(contents["Executable.json"])
    assert executable["sha256"] == exe_sha, "executable digest"
    assert executable["version"], "executable version"
    system = json.loads(contents["SystemInfo.json"])
    assert {"os", "cpu", "gpu", "memory_mb"} <= system.keys(), "system info fields"
    assert all(system[key] for key in ("os", "cpu", "gpu")), "empty system description"
    assert system["memory_mb"] > 0, "memory size"
    original = (runtime / "Userdata/Settings.ini").read_bytes()
    check_no_secrets(contents, original, secrets)
    assert "present" in json.loads(contents["DesyncHeal.json"]), "desync/heal status"
    status = json.loads(contents["Replay.status.json"])
    assert manifest["replay"] == status, "manifest replay truncation/status differs"
    if replay is not None:
        assert status["included"] and not status["truncated"], status
        assert next(row for row in listed if row["name"] == "Replay.ccrp")["truncated"] == status["truncated"], "replay member truncation flag"
        assert contents["Replay.ccrp"] == replay.read_bytes(), "replay differs from closed tick-boundary recording"
        assert contents["Replay.ccrp"][:4] == b"CCRP", "replay header"
        assert contents["Replay.ccrp"][-4:] == b"\xff\xff\xff\xff", "replay end marker"
        network = contents["NetMatch.log"].decode("utf-8", errors="replace")
        assert "[net-match]" in network and "[net-lockstep]" in network, "network log capture"
    else:
        assert not status["included"], status
    return {"zip": str(path), "sha256": sha(path), "members": listed, "exe_sha256": exe_sha}


def run_menu(repo: Path, root: Path, exe_sha: str) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    script = root / "menu.txt"
    script.write_text("wait 40\nactivate ButtonMainToMultiplayer\nwait 12\n"
                      "post_command ButtonSaveDiagnostics\nwait 12\nexit\n", encoding="utf-8")
    run = make_run(repo, ["-menu-script", str(script)], root / "menu", 240, env={"CCCP_HEADLESS": "1"})
    planted = plant_bundle_secrets(run)
    try:
        record = run.start().finish()
    finally:
        run.close()
    log = read_log(root / "menu")
    assert record.get("exit_code") == 0 and not record.get("timed_out"), f"menu run failed: {log[-6000:]}"
    assert "ButtonSaveDiagnostics" in log, "diagnostics command was not exercised"
    return inspect_bundle(run.cwd, exe_sha, secrets=planted)


def run_replay(repo: Path, root: Path, port: int, exe_sha: str) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    runs, records, recordings = {}, {}, {}
    for who in ("host", "client"):
        recordings[who] = root / f"{who}.ccrp"
        args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
                "-net-match-ticks", "400", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                "-telemetry-bundle", "-net-replay-out", str(recordings[who]),
                "-net-match-report", str(root / f"{who}_report.json")]
        args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
        runs[who] = make_run(repo, args, root / who, 300, env={"CCCP_HEADLESS": "1"})
    planted = {who: plant_bundle_secrets(run) for who, run in runs.items()}

    def drive(who: str) -> None:
        try:
            records[who] = runs[who].start().finish()
        except Exception as error:
            records[who] = {"error": repr(error)}

    threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
    try:
        threads[0].start()
        threading.Event().wait(1)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    result = {}
    for who, run in runs.items():
        assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
        result[who] = inspect_bundle(run.cwd, exe_sha, recordings[who], secrets=planted[who])
    return result


def set_visual_resolution(run, width: int, height: int) -> None:
    path = run.cwd / "Userdata/Settings.ini"
    settings = path.read_text(encoding="utf-8")
    for key, value in (("ResolutionX", width), ("ResolutionY", height)):
        settings, count = re.subn(rf"(?m)^(\s*{key}\s*=\s*)[^\r\n]*", lambda match: match[1] + str(value), settings)
        assert count == 1, key
    path.write_text(settings, encoding="utf-8")
    metadata_path = run.out / "runtime.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    metadata["settings_sha256"] = sha(path)
    metadata["settings_overrides"].update(ResolutionX=str(width), ResolutionY=str(height))
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


def inspect_pictures(runtime: Path, prefix: str, width: int, height: int, count: int) -> list:
    paths = sorted((runtime / "ScreenShots").glob(f"{prefix}*.png"))
    assert len(paths) == count, f"expected {count} screenshots, found {len(paths)} in {runtime / 'ScreenShots'}"
    pictures = []
    for path in paths:
        with path.open("rb") as source:
            header = source.read(24)
        assert header[:8] == b"\x89PNG\r\n\x1a\n", path
        dimensions = struct.unpack(">II", header[16:24])
        assert dimensions == (width, height), (path, dimensions)
        pictures.append({"path": str(path), "dimensions": list(dimensions), "sha256": sha(path), "size": path.stat().st_size})
    return pictures


def measure_pause_busy_centre(initial_path: Path, busy_path: Path) -> dict:
    from collections import Counter
    from PIL import Image

    with Image.open(initial_path) as source:
        initial = source.convert("RGB")
    with Image.open(busy_path) as source:
        busy = source.convert("RGB")
    assert initial.size == busy.size, "pause frame sizes differ"
    width, height = initial.size
    old, new = initial.load(), busy.load()
    colors = Counter(old[x, y] for y in range(height) for x in range(width)
                     if old[x, y][0] >= 200 and old[x, y][1] >= 120 and old[x, y][2] <= 80
                     and old[x, y][0] > old[x, y][1])
    assert colors, "initial pause frame has no amber ink"
    ink = colors.most_common(1)[0][0]
    initial_rows = [sum(old[x, y] == ink for x in range(width)) for y in range(height)]
    removed_rows = [sum(old[x, y] == ink and new[x, y] != ink for x in range(width)) for y in range(height)]
    bands, start = [], None
    for y, count in enumerate(initial_rows + [0]):
        if count and start is None:
            start = y
        elif not count and start is not None:
            bands.append((start, y))
            start = None
    selected = max(range(len(bands)), key=lambda index: sum(removed_rows[bands[index][0]:bands[index][1]]))
    idle_band = bands[selected]
    removed = sum(removed_rows[idle_band[0]:idle_band[1]])
    assert removed > 0, "pause busy frame does not replace an idle text row"
    # Adjacent idle rows define the cell, including room below the glyph baseline.
    top = (bands[selected - 1][1] + idle_band[0]) // 2 if selected else 0
    bottom = (idle_band[1] + bands[selected + 1][0] + 1) // 2 if selected + 1 < len(bands) else height
    added = [(x, y) for y in range(top, bottom) for x in range(width) if new[x, y] == ink and old[x, y] != ink]
    assert added, "pause busy row has no new amber ink"
    left, right = min(x for x, y in added), max(x for x, y in added)
    centre, axis = (left + right) / 2, width / 2
    return {"initial": str(initial_path), "busy": str(busy_path), "ink_rgb": list(ink),
            "initial_bands": bands, "idle_band": idle_band, "measurement_band": [top, bottom],
            "removed_pixels": removed, "added_pixels": len(added), "ink_left": left, "ink_right": right,
            "centre": centre, "axis": axis, "offset": centre - axis, "limit_px": 3,
            "passed": abs(centre - axis) <= 3}


def run_pause(repo: Path, root: Path, port: int, exe_sha: str) -> dict:
    root.mkdir(parents=True, exist_ok=False)
    rows = {}
    for index, (width, height) in enumerate(((640, 360), (960, 540))):
        tag = f"{width}x{height}"
        case = root / tag
        case.mkdir()
        inputs = case / "pause-input.txt"
        inputs.write_text("100 100 START\n", encoding="utf-8")
        script = case / "pause-menu.txt"
        script.write_text("wait 12\nassert_screen Pause\nassert_control ButtonSaveDiagnostics\n"
                          "assert_enabled ButtonSaveDiagnostics 1\nassert_label ButtonSaveDiagnostics save diagnostics\n"
                          "screenshot diagnostics_pause\npost_command ButtonSaveDiagnostics\n"
                          "wait 2\nassert_enabled ButtonSaveDiagnostics 0\nassert_label ButtonSaveDiagnostics saving\n"
                          "screenshot diagnostics_pause_busy\n"
                          "assert_enabled ButtonSaveDiagnostics 0\nassert_label ButtonSaveDiagnostics saving\n"
                          "screenshot diagnostics_pause_busy_confirmed\n"
                          "wait_file Telemetry/diag-*.zip\nwait_ms 50\n"
                          "assert_enabled ButtonSaveDiagnostics 1\nassert_label ButtonSaveDiagnostics save diagnostics\n"
                          "screenshot diagnostics_pause_saved\nexit\n", encoding="utf-8")
        runs, records = {}, {}
        for who in ("host", "client"):
            args = ["-net-match-service-e2e", "-net-port", str(port + index), "-net-match-peers", "2",
                    "-net-match-ticks", "400", "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
                    "-input-script", str(inputs), "-menu-script", str(script),
                    "-net-match-report", str(case / f"{who}_report.json")]
            args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
            runs[who] = make_run(repo, args, case / who, 120, env={"CCCP_HEADLESS": "1"})
            set_visual_resolution(runs[who], width, height)
        planted = {who: plant_bundle_secrets(run) for who, run in runs.items()}

        def drive(who: str) -> None:
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,)) for who in runs]
        try:
            threads[0].start()
            threading.Event().wait(1)
            threads[1].start()
            for thread in threads:
                thread.join()
        finally:
            for run in runs.values():
                run.close()
        details = {"records": records, "script_sha256": sha(script), "input_sha256": sha(inputs), "peers": {}, "errors": {}}
        for who, run in runs.items():
            try:
                assert records[who].get("exit_code") == 0 and not records[who].get("timed_out"), records[who]
                pictures = inspect_pictures(run.cwd, "diagnostics_pause", width, height, 4)
                initial = next(Path(picture["path"]) for picture in pictures if Path(picture["path"]).name.startswith("diagnostics_pause_20"))
                busy = next(Path(picture["path"]) for picture in pictures if Path(picture["path"]).name.startswith("diagnostics_pause_busy_20"))
                centre = measure_pause_busy_centre(initial, busy)
                details["peers"][who] = {"pictures": pictures, "busy_centre": centre}
                print(f"pause busy row centre {centre['centre']:.1f} axis {centre['axis']:.1f} {'PASS' if centre['passed'] else 'FAIL'} ({tag}/{who})", flush=True)
                assert centre["passed"], f"pause busy row is {centre['offset']:.1f} px from its axis"
                log = read_log(run.out)
                assert "assert_screen expected=Pause actual=Pause PASS" in log, "pause screen was not asserted"
                assert "post_command ButtonSaveDiagnostics ok=1" in log, "diagnostics command was not accepted"
                assert "file:Telemetry/diag-*.zip -> OK" in log, "diagnostics file wait did not complete"
                assert log.count("assert_enabled ButtonSaveDiagnostics expected=1 actual=1 PASS") == 2, "terminal button was not enabled"
                assert log.count('assert_label ButtonSaveDiagnostics "save diagnostics" text="save diagnostics" PASS') == 2, "terminal label differs"
                assert log.count("assert_enabled ButtonSaveDiagnostics expected=0 actual=0 PASS") == 2, "busy button was not disabled"
                assert log.count('assert_label ButtonSaveDiagnostics "saving" text="saving" PASS') == 2, "busy label differs"
                details["peers"][who]["bundle"] = inspect_bundle(run.cwd, exe_sha, secrets=planted[who])
            except Exception as error:
                details["errors"][who] = str(error)
        details["passed"] = not details["errors"]
        rows[tag] = details
        if details["passed"]:
            print(f"PASS visual Pause {tag}: two peers, 8 PNGs, 2 diagnostics zips; busy labels and restored buttons verified", flush=True)
        else:
            found = sum(len(list((run.cwd / "ScreenShots").glob("diagnostics_pause*.png"))) for run in runs.values())
            print(f"FAIL visual Pause {tag}: expected menu-script PNGs from both peers, found {found}", flush=True)
        (case / "result.json").write_text(json.dumps(details, indent=2) + "\n", encoding="utf-8")
    assert all(row["passed"] for row in rows.values()), f"pause diagnostics failed: {root}"
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arm", choices=("menu", "replay", "pause", "all"), default="all",
                        help="all runs menu+replay; use --arm pause for both pause-menu resolutions (no replay recording)")
    parser.add_argument("--port", type=int, default=48211)
    args = parser.parse_args()
    allowed = ((48211, 48219), (48280, 48289))
    if not any(lo <= args.port <= hi for lo, hi in allowed):
        parser.error("port must be in 48211..48219 or 48280..48289")
    if args.arm == "pause" and not any(lo <= args.port <= hi - 1 for lo, hi in allowed):
        parser.error("pause needs port and port+1 inside 48211..48219 or 48280..48289")
    os.environ["CCCP_HEADLESS"] = "1"
    repo, root = args.repo.resolve(), args.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"exe_sha256": sha(repo / "Cortex Command.exe"), "arms": {}}
    for arm in (("menu", "replay") if args.arm == "all" else (args.arm,)):
        try:
            if arm == "pause":
                details = run_pause(repo, root / arm, args.port, result["exe_sha256"])
            else:
                details = (run_menu(repo, root / arm, result["exe_sha256"]) if arm == "menu" else
                           run_replay(repo, root / arm, args.port, result["exe_sha256"]))
            result["arms"][arm] = {"passed": True, "details": details}
            print(f"PASS {arm}: bounded telemetry members and sha256 manifest match", flush=True)
        except Exception as error:
            result["arms"][arm] = {"passed": False, "error": str(error)}
            print(f"FAIL {arm}: {error}", flush=True)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if all(arm["passed"] for arm in result["arms"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
