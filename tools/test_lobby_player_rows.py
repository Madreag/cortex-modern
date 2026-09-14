"""Exercise the multiplayer lobby player rows at their worst legal width.

Four peers land in one lobby: the host and three joiners, each named with a
64-byte display name (the wire cap; the textbox's typed-input cap does not apply
to scripted settext). The host's auto input delay renders "(auto, Nms ping)",
joiner rows carry team/role words plus connection quality and ping, and one
joiner stays unready so both role words show. A fourth name is all high-byte
cp1252 so glyph coverage past ASCII is exercised too.

The oracle is pixel-only, measured inside each row's label box - the ini box
(X=8 W=288) when the panel keeps its 300px default, or the fitted box
(X=12 W=panel-24) once the panel widens to hold the widest row:

    rowN_populated          the box holds text ink
    rowN_single_line        ink forms one contiguous band - a wrapped row shows
                            a second band inside the same 16px box
    rowN_no_top_clip        no ink in the box's top edge rows - a wrapped first
                            line's sliced glyph tops jam against it
    rowN_no_bottom_clip     no ink in the box's bottom edge rows - the wrapped
                            second line lands there

On the control build the host row wraps, so the band/clip checks fail.
On the repaired build the panel widens (contentWidth = max(300, widest row + 24)
capped at m_RootBoxMaxWidth - 12) and only when the cap still cannot hold the
row is the name elided with 0x85, so all checks pass and the log's
assert_label dumps prove the delay text stayed whole.

--reference <png> compares a control-run capture of the same lobby. The panel
widens by recentering, so siblings hold position up to the odd-width parity
drift the stock shift math already has; the check diffs the intersection of
both panels minus the rows' band under each row's best translation in +-4px -
every pixel row must align under one small dx. The backdrop outside the panel
animates between runs and is not compared.
"""

import argparse
import json
from pathlib import Path
import re
import sys

from PIL import Image

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from test_viewport_fit import (PinDrift, close_all, game_version, near,  # noqa: E402
                               pin_state, require_pin, resolve_tools, set_resolution, sha256_file)

PANEL_GRAY = (59, 65, 83)
ROW_X_INI, ROW_W_INI, ROW_H, ROW_Y0, ROW_STEP = 8, 288, 16, 66, 18
PANEL_W_INI = 300
ELLIPSIS = b"\x85"

HOST_NAME = "H" * 64
JOINER_NAMES = [
    ("JoinerAlpha" + "x" * 64)[:64],
    "À" * 64,
    ("JoinerZulu" + "z" * 64)[:64],
]


def find_panel(px, width, height):
    """The lobby panel: rows whose longest contiguous panel-gray run clears 200px."""
    flagged = []
    for y in range(height):
        run = left = best_run = best_left = best_right = 0
        for x in range(width):
            if near(px[x, y], PANEL_GRAY, 10):
                if run == 0:
                    left = x
                run += 1
                if run > best_run:
                    best_run, best_left, best_right = run, left, x
            else:
                run = 0
        if best_run >= 200:
            flagged.append((y, best_left, best_right))
    if not flagged:
        return None
    top = flagged[0][0]
    lefts = sorted(r[1] for r in flagged)
    rights = sorted(r[2] for r in flagged)
    return top, lefts[len(lefts) // 2], rights[len(rights) // 2], flagged[-1][0]


def ink_counts(px, x0, y0, w, h):
    counts = []
    for dy in range(h):
        counts.append(sum(1 for x in range(x0, x0 + w)
                          if not near(px[x, y0 + dy], PANEL_GRAY, 14)))
    return counts


def bands(counts):
    """Maximal runs of inked rows, split only on fully clear rows."""
    out, start = [], None
    for dy, n in enumerate(counts + [0]):
        if n > 0 and start is None:
            start = dy
        elif n == 0 and start is not None:
            out.append((start, dy - 1))
            start = None
    return out


def menu_script(name, host, port, players):
    script = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
    if host:
        return script + (f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\n"
                         f"settext TextHostPlayers {players}\nsettext TextHostInputDelay 3\n"
                         "activate ButtonMultiplayerCreate\n")
    return script + ("activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                     f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=47941)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--reference", type=Path, default=None,
                        help="control-run capture; outside the row boxes every pixel must match")
    options = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{64}", options.exe_sha256):
        parser.error("--exe-sha256 must be 64 lowercase hex chars")

    make_run, wait_for_log = resolve_tools(options.repo)
    repo = options.repo.resolve()
    before = pin_state(repo)
    if before["exe_sha256"] != options.exe_sha256:
        raise SystemExit("exe pin mismatch at start: " + before["exe_sha256"])

    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    result = {"pass": False, "version": game_version(repo),
              "viewport_request": [options.width, options.height], "pin_before": before}
    checks, details = {}, {}
    runs, records = {}, {}

    def start(name, host, suffix, ready=True):
        require_pin(repo, options.exe_sha256, before, checks, f"{name}_prelaunch")
        script = menu_script(name, host, options.port, 4)
        path = root / f"{name}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((script + suffix).encode("cp1252"))
        run = make_run(repo, ["-menu-script", path, "-num-lua-states", 4], root / name, 200)
        runs[name] = run
        details[f"settings_{name.lower()}"] = set_resolution(run, options.width, options.height)
        run.start()
        return run

    try:
        host_suffix = ("wait_connected 4\nwait 120\ndump_lobby\n"
                       "assert_label LabelLobbyPlayer0 auto\nassert_label LabelLobbyPlayer1 Team\n"
                       "assert_label LabelLobbyPlayer2 Team\nassert_label LabelLobbyPlayer3 Team\n"
                       "screenshot rows-t0\nwait 40\nscreenshot rows-t1\nexit\n")
        host = start(HOST_NAME, True, host_suffix)
        wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
        for index, name in enumerate(JOINER_NAMES):
            ready = index < 2
            suffix = "wait_connected 4\n"
            if ready:
                suffix += "activate ButtonMultiplayerReady\n"
            suffix += "wait 900\nexit\n"
            start(name, False, suffix)

        for key, run in runs.items():
            records[key] = run.finish()

        shots = sorted((root / HOST_NAME / "runtime/ScreenShots").glob("rows-t0_*.png"))
        details["screenshots"] = [str(p) for p in shots]
        checks["shot_taken"] = len(shots) >= 1
        host_log = (root / HOST_NAME / "stdout.log").read_bytes()

        for i in range(4):
            m = re.search(rb'assert_label LabelLobbyPlayer' + str(i).encode()
                          + rb' "[^"]*" text="(.*?)" (PASS|FAIL)', host_log, re.S)
            details[f"row{i}_text"] = m.group(1).decode("cp1252") if m else None
            details[f"row{i}_elided"] = bool(m and ELLIPSIS in m.group(1))
        m0 = re.search(rb'assert_label LabelLobbyPlayer0 "[^"]*" text="(.*?)" (PASS|FAIL)', host_log, re.S)
        checks["row0_delay_text_whole"] = bool(m0 and b"(auto," in m0.group(1) and b"ping)" in m0.group(1))
        checks["host_reached_lobby"] = "activate ButtonMultiplayerCreate ok=1" in host_log.decode("cp1252", errors="replace")

        if shots:
            im = Image.open(shots[0]).convert("RGB")
            px, (w, h) = im.load(), im.size
            details["png_size"] = [w, h]
            checks["png_size_matches"] = [w, h] == [options.width, options.height]
            panel = find_panel(px, w, h)
            checks["panel_found"] = panel is not None
            if panel:
                top, left, right, bottom = panel
                panel_w = right - left + 1
                details["panel"] = {"top": top, "left": left, "right": right,
                                    "bottom": bottom, "width": panel_w}
                # A widened panel moved its rows to the diagnostic-label margin
                # convention (X=12, W=panel-24); an untouched panel keeps the
                # ini box (X=8, W=288).
                if panel_w > PANEL_W_INI:
                    row_x, row_w = 12, panel_w - 24
                else:
                    row_x, row_w = ROW_X_INI, ROW_W_INI
                details["row_box"] = {"x": row_x, "w": row_w}
                for i in range(4):
                    y0 = top + ROW_Y0 + ROW_STEP * i
                    counts = ink_counts(px, left + row_x, y0, row_w, ROW_H)
                    row_bands = bands(counts)
                    extent = [dx for dx in range(row_w)
                              if any(not near(px[left + row_x + dx, y0 + dy], PANEL_GRAY, 14)
                                     for dy in range(ROW_H))]
                    details[f"row{i}_ink"] = counts
                    details[f"row{i}_bands"] = row_bands
                    details[f"row{i}_ink_extent_px"] = (max(extent) - min(extent) + 1) if extent else 0
                    checks[f"row{i}_populated"] = sum(counts) > 20
                    checks[f"row{i}_single_line"] = len(row_bands) == 1
                    checks[f"row{i}_no_top_clip"] = (counts[0] + counts[1]) <= 8
                    checks[f"row{i}_no_bottom_clip"] = (counts[14] + counts[15]) <= 8
                if options.reference:
                    # Widening recenters the panel so siblings keep position up
                    # to a parity pixel; every pixel row of the panels'
                    # intersection (minus the union row band) must match the
                    # control under one horizontal translation dx in +-4.
                    ref = Image.open(options.reference).convert("RGB")
                    checks["reference_size_matches"] = ref.size == (w, h)
                    rpx = ref.load()
                    ref_panel = find_panel(rpx, w, h) if ref.size == (w, h) else None
                    bad_rows, dx_seen = [], {}
                    if ref_panel:
                        rtop, rleft, rright, rbottom = ref_panel
                        details["reference_panel"] = {"top": rtop, "left": rleft,
                                                      "right": rright, "bottom": rbottom,
                                                      "width": rright - rleft + 1}
                        # 6px margin: a shifted sample past the ref panel edge
                        # would read its frame and alias as a fake diff.
                        ix_lo, ix_hi = max(left, rleft) + 6, min(right, rright) - 6
                        iy_lo, iy_hi = max(top, rtop), min(bottom, rbottom)
                        ex_lo = min(left + row_x, rleft + ROW_X_INI) - 6
                        ex_hi = max(left + row_x + row_w, rleft + ROW_X_INI + ROW_W_INI) + 6
                        ey_lo = min(top, rtop) + ROW_Y0 - 6
                        ey_hi = min(top, rtop) + ROW_Y0 + ROW_STEP * 3 + ROW_H + 6
                        for y in range(iy_lo, iy_hi + 1):
                            best_dx, best_n = 0, None
                            for dx in range(-4, 5):
                                n = sum(1 for x in range(ix_lo, ix_hi + 1)
                                        if not (ex_lo <= x < ex_hi and ey_lo <= y < ey_hi)
                                        and 0 <= x + dx < w and px[x, y] != rpx[x + dx, y])
                                if best_n is None or n < best_n or (n == best_n and abs(dx) < abs(best_dx)):
                                    best_dx, best_n = dx, n
                            if best_n:
                                bad_rows.append((y, best_dx, best_n))
                            else:
                                dx_seen[best_dx] = dx_seen.get(best_dx, 0) + 1
                    details["shift_dx_histogram"] = dx_seen
                    details["unmatched_pixel_rows"] = bad_rows[:20]
                    details["unmatched_pixel_row_count"] = len(bad_rows)
                    checks["shared_panel_shifted_identical"] = not bad_rows
    finally:
        errors = close_all(runs)
        if errors:
            details["close_errors"] = errors
        after = pin_state(repo)
        result["pin_after"] = after
        checks["exe_unchanged"] = after["exe_sha256"] == options.exe_sha256
        checks["tree_unchanged"] = (after["head"] == before["head"]
                                   and after["diff_sha256"] == before["diff_sha256"])

    result["checks"] = checks
    result["details"] = details
    result["records"] = {k: v for k, v in records.items()}
    result["pass"] = all(checks.values()) and not details.get("close_errors")
    (root / "result.json").write_text(json.dumps(result, indent=2, default=str))
    print(json.dumps({"pass": result["pass"], "checks": checks}, indent=2))
    sys.exit(0 if result["pass"] else 1)


if __name__ == "__main__":
    main()
