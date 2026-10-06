"""Read the installed mod's lettering from captured pictures and grade its scenes."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

from PIL import Image

ERROR_LINES = re.compile(r"(?:^|PRINT:\s*)ERROR:|RTE Aborted|RTE Assert|RTE Error|Assertion failed|Lua error|stack traceback:|stopped a preview hook|missing.preset|preset.*not found", re.I | re.M)
HASH_SUBSYSTEMS = {"actor_timers", "actors", "attachables", "carve_math", "controller", "controller_route", "funds", "items",
                   "lua_state", "particles", "rot_angle", "rot_angvel", "scene", "sim_rng", "terrain", "tick"}
HASH_CORE = {"actors", "funds", "lua_state", "rot_angle", "rot_angvel", "scene", "sim_rng", "terrain", "tick"}
PAUSED_HASH_CORE = {"funds", "lua_state", "scene", "sim_rng", "terrain", "tick"}

def lettering(package: Path, text: str) -> tuple[list[tuple[int, int]], int, int]:
    source = (package / "Scripts/Lib_Generic.lua").read_text(encoding="utf-8")
    chars = {}
    for char, index, width, dx, dy in re.findall(
        r'CF\["Chars"\]\["(.)"\] = \{ (\d+), (\d+), (?:nil|Vector\((-?\d+), (-?\d+)\)) \}', source
    ):
        chars[char] = (int(index), int(width) - 2, int(dx or 0), int(dy or 0))
    pixels, cursor = set(), 0
    for char in text:
        index, advance, dx, dy = chars[char]
        glyph = Image.open(package / f"UI/Letters/Letter{index - 1:03d}.png").convert("RGBA")
        for y in range(glyph.height):
            for x in range(glyph.width):
                r, g, b, alpha = glyph.getpixel((x, y))
                if alpha and r >= 220 and g >= 200:
                    pixels.add((cursor + dx + x - glyph.width // 2, dy + y - glyph.height // 2))
        cursor += advance
    left, top = min(x for x, y in pixels), min(y for x, y in pixels)
    pixels = sorted((x - left, y - top) for x, y in pixels)
    return pixels, max(x for x, y in pixels) + 1, max(y for x, y in pixels) + 1


def find_words(picture: Image.Image, package: Path, text: str) -> dict:
    pixels, width, height = lettering(package, text)
    ink = set(pixels)
    gaps = [(x, y) for y in range(height) for x in range(width)
            if all((x + dx, y + dy) not in ink for dx in range(-1, 2) for dy in range(-1, 2))]
    if not gaps:
        gaps = [(x, y) for y in [-2, height + 1] for x in range(-2, width + 2)]
        gaps += [(x, y) for x in [-2, width + 1] for y in range(height)]
    rgb = picture.convert("RGB")
    rows = []
    for y in range(rgb.height):
        bits = 0
        for x in range(rgb.width):
            r, g, b = rgb.getpixel((x, y))
            # A join flash also draws the same glyphs in white.
            if r >= 170 and g >= 150 and r >= b + 30 or min(r, g, b) >= 170:
                bits |= 1 << x
        rows.append(bits)
    anchors = [pixels[i * (len(pixels) - 1) // 3] for i in range(4)]
    best = {"text": text, "confidence": 0.0, "background_confidence": 0.0, "rect": None, "pass": False}
    for y in range(max(0, rgb.height - height + 1)):
        possible = 0
        for ax, ay in anchors:
            possible |= rows[y + ay] >> ax
        possible &= (1 << max(0, rgb.width - width + 1)) - 1
        while possible:
            lowest = possible & -possible
            x = lowest.bit_length() - 1
            possible -= lowest
            confidence = sum(bool(rows[y + py] & (1 << (x + px))) for px, py in pixels) / len(pixels)
            if confidence > best["confidence"]:
                background = sum(0 <= y + py < rgb.height and 0 <= x + px < rgb.width
                                 and not rows[y + py] & (1 << (x + px)) for px, py in gaps) / len(gaps)
                best.update(confidence=confidence, background_confidence=background, rect=[x, y, width, height],
                            **{"pass": confidence >= 0.95 and background >= 0.95})
    return best


def faction_buttons(picture: Image.Image, package: Path) -> list[list[int]]:
    """Locate the banner borders the mod draws around its offered factions."""
    def border(color):
        r, g, b = color
        return r >= 100 and abs(r - g) < 30 and abs(r - b) < 30

    banner = Image.open(package / "UI/Generic/FactionBannerIdle.png").convert("RGB")
    points = [(x, y) for y in range(banner.height) for x in range(banner.width)
              if border(banner.getpixel((x, y)))]
    left, top = min(x for x, y in points), min(y for x, y in points)
    points = [(x - left, y - top) for x, y in points]
    width, height = max(x for x, y in points) + 1, max(y for x, y in points) + 1
    rgb = picture.convert("RGB")
    rows = [sum(1 << x for x in range(rgb.width) if border(rgb.getpixel((x, y))))
            for y in range(rgb.height)]
    found = []
    for y in range(rgb.height - height + 1):
        possible = rows[y] & (rows[y] >> (width - 1)) & rows[y + height - 1]
        possible &= (1 << (rgb.width - width + 1)) - 1
        while possible:
            lowest = possible & -possible
            x = lowest.bit_length() - 1
            possible -= lowest
            if sum(bool(rows[y + dy] & (1 << (x + dx))) for dx, dy in points) == len(points):
                found.append([x + banner.width // 2 - left, y + banner.height // 2 - top])
    if not found:
        raise ValueError("the faction picture has no complete banner border")
    return sorted((point for point in found if point[1] == min(p[1] for p in found)), key=lambda point: point[0])


def compile_choices(package: Path, menu: Path, factions: Path, output: Path) -> dict:
    menu_image, factions_image = Image.open(menu), Image.open(factions)
    if menu_image.size != factions_image.size:
        raise ValueError("menu and faction pictures have different sizes")
    witnesses = [find_words(menu_image, package, word) for word in ["New game", "Load game"]]
    witnesses += [find_words(factions_image, package, word) for word in ["START NEW GAME", "SELECT STARTING FACTION"]]
    if not all(check["pass"] for check in witnesses):
        raise ValueError(f"menu or faction lettering is missing: {witnesses}")
    buttons = faction_buttons(factions_image, package)
    if len(buttons) < 6:
        raise ValueError(f"only {len(buttons)} visible factions; six are required to start")
    x, y, width, height = witnesses[0]["rect"]
    target = [x + width // 2, y + height // 2]
    cursor = [menu_image.width // 2, menu_image.height // 2]
    lines = ["# Coordinates come from the captured mod lettering and faction banner borders."]
    # The first press follows the controller's existing release debounce.
    for index, (tick, point) in enumerate([(20, target), *[(24 + index * 2, point) for index, point in enumerate(buttons[:6])]]):
        delta = [point[0] - cursor[0], point[1] - cursor[1]]
        press = tick + 2 if index == 0 else tick
        lines += [f"player=0 {tick} {tick} MOUSE={delta[0]},{delta[1]}", f"player=0 {press} {press} FIRE"]
        cursor = point
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"menu": str(menu), "factions": str(factions), "witnesses": witnesses,
            "buttons": buttons, "cursor_after_choices": cursor, "input": str(output)}


def compile_play(package: Path, chosen: Path, layout: Path, host: Path, client: Path) -> dict:
    """Finish the hand input from the visible confirmation button, then drive both brains."""
    picture = Image.open(chosen)
    words = [find_words(picture, package, word) for word in ["START NEW GAME", "OK"]]
    if not all(word["pass"] for word in words):
        raise ValueError(f"the selected-faction picture has no visible confirmation: {words}")
    previous = json.loads(layout.read_text(encoding="utf-8"))
    cursor = previous["cursor_after_choices"]
    x, y, width, height = words[1]["rect"]
    target = [x + width // 2, y + height // 2]
    prefix = []
    for line in host.read_text(encoding="utf-8").splitlines():
        match = re.match(r"player=0 (\d+) ", line)
        if line.startswith("# Coordinates") or match and int(match[1]) <= 34:
            prefix.append(line)
    prefix += [f"player=0 36 36 MOUSE={target[0] - cursor[0]},{target[1] - cursor[1]}", "player=0 36 37 FIRE"]
    for peer, path in [(0, host), (1, client)]:
        lines = prefix.copy() if peer == 0 else ["# The joining player's local slot drives stable seat 1 in the match."]
        detach = 90 if peer == 0 else 125
        lines += ["# DOWN detaches this player's brain through the mod's own brain panel.", f"player=0 {detach} {detach + 4} L_DOWN"]
        for index in range(10):
            start = 160 + index * 250
            forward, back = ("L_RIGHT", "L_LEFT") if peer == 0 else ("L_LEFT", "L_RIGHT")
            lines += [f"player=0 {start} {start + 89} {forward}", f"player=0 {start + 110} {start + 199} {back}",
                      f"player=0 {start + 35} {start + 39} JUMP", f"player=0 {start + 145} {start + 149} JUMP"]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return {"chosen": str(chosen), "witnesses": words, "ok": target, "host": str(host), "client": str(client)}


def hand_input(path: Path) -> dict:
    moves, presses = [], []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"player=0 (\d+) (\d+) (.+)", line.strip())
        if not match:
            continue
        first, last, elements = int(match[1]), int(match[2]), match[3].split()
        if "FIRE" in elements:
            presses.append((first, last))
        moves += [(first, element) for element in elements if element.startswith("MOUSE=") and element != "MOUSE=0,0"]
    valid = bool(moves and presses) and all(first <= last and any(tick < first for tick, value in moves)
                                           and not any(other_first <= last + 1 <= other_last for other_first, other_last in presses)
                                           for first, last in presses)
    return {"pass": valid, "pointer_moves": len(moves), "button_presses": len(presses),
            "release_ticks": [last + 1 for first, last in presses]}


def compare_every_tick(host: Path | list[dict], client: Path | list[dict]) -> dict:
    def read(path):
        records = path if isinstance(path, list) else [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
        if any(record.get("type") == "abandon_from" for record in records):
            raise ValueError(f"{path}: played history is abandoned")
        records = [record for record in records if "subsystems" in record]
        if not records:
            raise ValueError(f"{path}: no tick hashes")
        if len({record["round"] for record in records}) != 1:
            raise ValueError(f"{path}: expected one played round")
        ticks = [record["tick"] for record in records]
        if ticks != list(range(ticks[0], ticks[-1] + 1)):
            raise ValueError(f"{path}: missing, repeated or reordered tick")
        for record in records:
            core = PAUSED_HASH_CORE if record.get("paused") else HASH_CORE
            if not core.issubset(record.get("subsystems", {})) or any(
                    not re.fullmatch(r"[0-9a-f]{64}", record.get(field, "")) for field in ["total", "sim_gated"]):
                raise ValueError(f"{path}: tick {record['tick']} has incomplete hashes")
            if type(record.get("paused")) is not bool:
                raise ValueError(f"{path}: tick {record['tick']} has no paused state")
            if any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in record["subsystems"].values()):
                raise ValueError(f"{path}: tick {record['tick']} has an invalid subsystem hash")
        return records

    try:
        left, right = read(host), read(client)
    except (OSError, ValueError, KeyError) as error:
        return {"pass": False, "error": str(error), "compared_ticks": 0, "excluded_hash_fields": []}
    result = {"pass": True, "compared_ticks": 0, "host_ticks": len(left), "client_ticks": len(right),
              "first_tick": left[0]["tick"], "last_tick": left[-1]["tick"], "excluded_hash_fields": []}
    if [record["tick"] for record in left] != [record["tick"] for record in right]:
        result.update({"pass": False, "error": "the peers do not record every same played tick"})
        return result
    for a, b in zip(left, right):
        fields = sorted(name for name in a["subsystems"].keys() | b["subsystems"].keys()
                        if a["subsystems"].get(name) != b["subsystems"].get(name))
        fields += [name for name in ["total", "sim_gated", "paused", "round"] if a.get(name) != b.get(name)]
        if fields:
            result.update({"pass": False, "error": f"tick {a['tick']} differs in {', '.join(fields)}",
                           "first_divergence": a["tick"], "fields": fields})
            break
        result["compared_ticks"] += 1
    return result


def recording_failures(review: dict, peers: list[str]) -> list[str]:
    probes = {row["id"]: row.get("probe") for row in review["checklist"]}
    return [f"{name}: expected pass, got {probes.get(name, 'missing')}"
            for peer in peers for kind in ["rate", "stills"]
            if probes.get(name := f"recording-{kind}-{peer}") != "pass"]


def checker_self_test(package: Path) -> bool:
    import copy

    picture = Image.new("RGB", (960, 540))
    checks = [("black_has_no_menu", not find_words(picture, package, "New game")["pass"])]
    checks.append(("solid_has_no_short_button", not find_words(Image.new("RGB", (960, 540), "white"), package, "OK")["pass"]))
    checks.append(("preview_lua_error_fails", bool(ERROR_LINES.search("PREVIEW: script stopped a preview hook after an arithmetic error"))))
    recorded = {"checklist": [{"id": "recording-rate-sp", "probe": "pass"}, {"id": "recording-stills-sp", "probe": "pass"}]}
    checks.append(("complete_recording_passes", not recording_failures(recorded, ["sp"])))
    incomplete = copy.deepcopy(recorded)
    incomplete["checklist"][1]["probe"] = "incomplete"
    checks.append(("unrecorded_stills_fail", bool(recording_failures(incomplete, ["sp"]))))
    checks.append(("missing_recording_rate_fails", bool(recording_failures({"checklist": recorded["checklist"][1:]}, ["sp"]))))
    left = [{"round": 1, "tick": tick, "total": "a" * 64, "sim_gated": "a" * 64, "paused": False,
             "subsystems": {name: "b" * 64 for name in HASH_SUBSYSTEMS}} for tick in range(1, 4)]
    checks.append(("every_equal_hash_passes", compare_every_tick(left, copy.deepcopy(left))["pass"]))
    for field in sorted(HASH_SUBSYSTEMS):
        right = copy.deepcopy(left)
        right[1]["subsystems"][field] = "c" * 64
        result = compare_every_tick(left, right)
        checks.append((f"{field}_difference_fails", not result["pass"] and result.get("fields") == [field]))
    checks.append(("missing_tick_fails", not compare_every_tick(left, [left[0], left[2]])["pass"]))
    checks.append(("short_peer_fails", not compare_every_tick(left, left[:2])["pass"]))
    sparse = copy.deepcopy(left)
    for record in sparse:
        record["subsystems"] = {key: value for key, value in record["subsystems"].items() if key in HASH_CORE}
    checks.append(("equal_sparse_object_subsystems_pass", compare_every_tick(sparse, copy.deepcopy(sparse))["pass"]))
    checks.append(("field_present_on_one_peer_fails", not compare_every_tick(sparse, left)["pass"]))
    for name, passed in checks:
        print(f"[vw-scene-checker] {'PASS' if passed else 'FAIL'} {name}")
    return all(passed for name, passed in checks)


def grade_capture(repo: Path, capture: Path) -> dict:
    data = json.loads(capture.read_text(encoding="utf-8"))
    package = repo / "Data/VoidWanderers.rte"
    results = []
    for run in data["runs"]:
        if not run.get("peers"):
            results.append({"name": run["name"], "pass": False, "errors": ["no engine peer is recorded"]})
            continue
        root = Path(run["root"])
        arm = {"name": run["name"], "pass": True, "peers": []}
        for peer in run["peers"]:
            peer_root = Path(peer["root"])
            runtime = peer_root / "runtime"
            errors = []
            logs = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in
                             [peer_root / "stdout.log", peer_root / "console.log", runtime / "LogLoadingWarning.txt",
                              runtime / "AbortLog.txt"] if path.is_file())
            record = peer.get("record", {})
            if record.get("exit_code") != 0 or record.get("timed_out"):
                errors.append(f"engine exit={record.get('exit_code')} timeout={record.get('timed_out')}")
            forbidden = [line[:300] for line in logs.splitlines() if ERROR_LINES.search(line)]
            if forbidden:
                errors.append(f"engine or Lua error: {forbidden[0]}")
            desync = [line[:300] for line in logs.splitlines() if re.search(r"\bdesync\b", line, re.I)]
            if desync:
                errors.append(f"{len(desync)} desync lines")
            shots = runtime / "ScreenShots"
            start = sorted(shots.glob("vw_start_*.png"))
            menu = [find_words(Image.open(start[0]), package, word) for word in ["New game", "Load game"]] if start else []
            if peer["peer"] in ["sp", "host"] and (len(menu) != 2 or not all(check["pass"] for check in menu)):
                errors.append("visible New game / Load game witness missing")
            if "form=START NEW GAME" not in logs:
                errors.append("the hand press does not reach the mod's new-game form")
            input_path = root / f"{peer['peer']}-stage/input.txt"
            if peer["peer"] in ["sp", "host"] and not hand_input(input_path)["pass"]:
                errors.append("menu input has no pointer move, button down and later release")
            if "[vw-visible] live_brain_under_dead_banner" in logs:
                errors.append("the death banner labels a living player brain")
            ship = sorted(shots.glob("vw_ship_*.png"))
            played_shots = sorted(shots.glob("vw_played_*.png"))
            if not ship or "[vw-ship] first_tick=" not in logs:
                errors.append("Vessel Lynx first playable screen witness missing")
            else:
                visible = sum(max(pixel) > 16 for pixel in Image.open(ship[0]).convert("RGB").getdata())
                if visible < 1000:
                    errors.append("the first ship picture is blank")
                if Image.open(ship[0]).size != (960, 540):
                    errors.append("the ship picture is not 960x540")
                if not played_shots or Image.open(ship[0]).convert("RGB").tobytes() == Image.open(played_shots[0]).convert("RGB").tobytes():
                    errors.append("the mod's ship picture does not change during play")
            play_ticks = [int(value) for value in re.findall(r"scene=Vessel Lynx mode=Vessel play_ticks=(\d+)", logs)]
            played = max(play_ticks, default=0)
            if played < 1800:
                errors.append(f"only {played} ship play ticks after the new game; 1800 required")
            seats = {}
            for tick, player, actor, x, y, uid, detached, brain_player, left, right in re.findall(
                    r"\[vw-seat\] tick=(\d+) player=(\d+) actor=(.+?) x=([-\d.]+) y=([-\d.]+) uid=(\d+) detached=(\w+) brain_player=(\d+) left=(true|false) right=(true|false)", logs):
                seats.setdefault(int(player), []).append({"tick": int(tick), "actor": actor, "x": float(x), "y": float(y),
                                                         "uid": uid, "detached": detached, "brain_player": int(brain_player),
                                                         "left": left == "true", "right": right == "true"})
            required = [0] if len(run["peers"]) == 1 else [0, 1]
            responses = {}
            for player in required:
                rows = [row for row in seats.get(player, []) if row["detached"] == "True" and row["brain_player"] == player + 1]
                answers = {direction: any(a["uid"] == b["uid"] and a[direction] and b[direction] and
                                          b["tick"] == a["tick"] + 60 and (b["x"] - a["x"]) * sign >= 10
                                          for a, b in zip(rows, rows[1:]))
                           for direction, sign in [("left", -1), ("right", 1)]}
                moved = all(answers.values())
                responses[player] = {"detached": bool(rows), "moved": moved, "directions": answers}
                if not rows or not moved:
                    errors.append(f"player {player} detach and movement response missing")
            state = {"peer": peer["peer"], "pass": not errors, "errors": errors, "menu": menu,
                     "play_ticks": played, "responses": responses, "seats": seats,
                     "desync_lines": len(desync), "ship_picture": str(ship[0]) if ship else None}
            arm["peers"].append(state)
            arm["pass"] = arm["pass"] and state["pass"]
        if len(run["peers"]) == 2:
            arm["hashes"] = compare_every_tick(root / "host-live.jsonl", root / "client-live.jsonl")
            arm["pass"] = arm["pass"] and arm["hashes"]["pass"]
            for name in ["host", "client"]:
                report = root / f"{name}-match.json"
                if not report.is_file():
                    arm["pass"] = False
                    arm.setdefault("errors", []).append(f"{name} match report missing")
                else:
                    match = json.loads(report.read_text(encoding="utf-8"))
                    completed = match.get("last_match") if "last_match" in match else match
                    completed = completed if isinstance(completed, dict) else {}
                    if (not completed or match.get("error") or match.get("private_rejoin", {}).get("error") or
                            match.get("setup_error") or match.get("runtime_error") or completed.get("resyncs") != 0 or
                            ("last_match" not in match and match.get("exit_code") != 0)):
                        arm["pass"] = False
                        arm.setdefault("errors", []).append(f"{name} match report has an error or resync")
                    hashes = arm["hashes"]
                    if (hashes.get("first_tick") != 1 or hashes.get("last_tick") != completed.get("running_ticks") or
                            hashes.get("compared_ticks") != completed.get("running_ticks")):
                        arm["pass"] = False
                        arm.setdefault("errors", []).append(f"{name} hash trace does not cover every tick in its match report")
            if arm["peers"][0]["seats"] != arm["peers"][1]["seats"]:
                arm["pass"] = False
                arm.setdefault("errors", []).append("the peers observe different player actors or play responses")
        review = json.loads((root / "review.json").read_text(encoding="utf-8"))
        incomplete = recording_failures(review, [peer["peer"] for peer in run["peers"]])
        if incomplete:
            arm["pass"] = False
            arm.setdefault("errors", []).extend(incomplete)
        failed = [row["id"] for row in review["checklist"] if row.get("probe") == "fail"]
        if failed or review.get("run_findings"):
            arm["pass"] = False
            arm.setdefault("errors", []).append(f"scene recording checks fail: {failed}; findings={review.get('run_findings', [])}")
        results.append(arm)
    result = {"pass": bool(results) and all(arm["pass"] for arm in results), "arms": results}
    (capture.parent / "mod-play-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--picture", type=Path)
    parser.add_argument("--words", default="New game|Load game")
    parser.add_argument("--file-probe", type=Path, metavar="OUT")
    parser.add_argument("--file-probe-default", type=Path, metavar="OUT")
    parser.add_argument("--compile-choices", type=Path, metavar="MENU_PNG")
    parser.add_argument("--compile-play", type=Path, metavar="CHOSEN_FACTIONS_PNG")
    parser.add_argument("--layout", type=Path)
    parser.add_argument("--client-input", type=Path)
    parser.add_argument("--factions", type=Path, metavar="FACTIONS_PNG")
    parser.add_argument("--input-output", type=Path)
    parser.add_argument("--grade", type=Path, metavar="CAPTURE_JSON")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    package = options.repo / "Data/VoidWanderers.rte"
    if options.self_test:
        return int(not checker_self_test(package))
    if options.grade:
        result = grade_capture(options.repo.resolve(), options.grade.resolve())
        summary = {**result, "arms": [{**arm, "peers": [{key: value for key, value in peer.items() if key != "seats"}
                                                        for peer in arm["peers"]]} for arm in result["arms"]]}
        print(json.dumps(summary, indent=2))
        return int(not result["pass"])
    if options.compile_choices:
        if not options.factions or not options.input_output:
            parser.error("--compile-choices requires --factions and --input-output")
        print(json.dumps(compile_choices(package, options.compile_choices, options.factions, options.input_output), indent=2))
        return 0
    if options.compile_play:
        if not options.layout or not options.input_output or not options.client_input:
            parser.error("--compile-play requires --layout, --input-output and --client-input")
        print(json.dumps(compile_play(package, options.compile_play, options.layout, options.input_output, options.client_input), indent=2))
        return 0
    if options.file_probe or options.file_probe_default:
        spec = importlib.util.spec_from_file_location("vw_files", Path(__file__).with_name("mod-void-wanderers-files.py"))
        files = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(files)
        result = files.file_probe(options.repo.resolve(), (options.file_probe or options.file_probe_default).resolve(), bool(options.file_probe))
        print(json.dumps({key: value for key, value in result.items() if key != "record"}, indent=2))
        return int(not result["pass"])
    if options.picture:
        checks = [find_words(Image.open(options.picture), package, word) for word in options.words.split("|")]
        print(json.dumps(checks, indent=2))
        return int(not all(check["pass"] for check in checks))
    parser.error("--picture is required")


if __name__ == "__main__":
    sys.exit(main())
