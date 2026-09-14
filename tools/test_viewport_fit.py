"""Viewport-fit detector: the mismatch refusal at 640x360 must keep the whole
multiplayer container, its panel edges and the Back button inside the viewport,
and the long diagnostic must stay reachable through the label's overflow scroll.

    python test_viewport_fit.py --repo D:/Projects/item7-ux \
        --out D:/mx/ui-viewport-complete-20260913/detector --port 47871 \
        --exe-sha256 <exe hash>

Three phases, each one host + one refused joiner on loopback:

  viewport  the joiner stages Base1.rte plus 'ß'*70 + '.rte' (the unchanged
            encoded-name fixture; the wire name arrives as 64 x 0xDF), then
            presses Back via 'post_command ButtonBackToMain' - the same
            GUIEvent::Command path a click takes - and must reach MainScreen.
  tall      the joiner's mod set fills Install/Remove/Update so the status
            wraps past the status room in the minimum-width panel; the timed
            frames must show the label band moving while every pixel outside
            it stays put. Skipped by --no-scroll-input, which must fail closed.
  seam      the joiner stages an '@'-run name whose produced panel width is
            driven to 5 mod 8 (a second attempt corrects the residue); the
            bitmap's last column/row under the frame's colour-key edge must be
            filler or frame pixels, never the untouched BASE_FILL.

Geometry/visibility assertions are pixel checks on the menu-script screenshots
(no control-rect readback seam exists; documented, not invented):

  - the landing panel's gray extent must have both edges strictly inside the
    viewport columns and rows (baseline left edge is clipped off-screen: RED)
  - the Back button's frame must be fully inside the viewport below the panel
  - status text ink must exist inside the panel region (the high-bit name draws)
  - the staged name is one long token: a row of near-contiguous ink spanning it
    must exist (FontLarge has width-only blank cells for 0xDF, so a renderer
    that cannot draw the name shows nothing; a clipped panel cannot produce the
    run either)
  - when the measured token width exceeds the label's inner width (font-cell
    widths are measured from the skin PNGs, mirroring GUIFont::Load), the
    horizontal overflow scroll must move the band and bring the line tail to
    the window's right quarter; when it fits, the ink run must simply end
    inside the panel
  - text ink stays inside the panel's horizontal extent

PNG IHDR dimensions are asserted equal to --width/--height. The baseline oracle
'panel_wider_than_viewport' in test_encoded_name.py stays untouched; this file
is the disclosed separate detector. No pixel judgement calls are made about
aesthetics - edges, containment and motion only.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


LABEL = "LabelMultiplayerLandingStatus"
PREFIX = "This host's mods do not match yours."
BASE_NAME = "Base1.rte"
SZ70_NAME = "ß" * 70 + ".rte"
# The '@' cell is 9px in FontLarge; the produced panel is word+constant wide,
# so the driver adjusts the run length until the produced width lands on 5 mod 8.
SEAM_CHAR = "@"
# Install+Remove+Update groups each list up to 6 names; filling all three makes
# the status wrap past the 153px room at 640x360, so the label's vertical
# overflow scroll has to engage. Updates are the same dirs at Version 1 vs 2.
TALL_JOINER_MODS = [(BASE_NAME, "Base1", 1)] + \
    [("RemovedModule%02d.rte" % i, "Removed%02d" % i, 1) for i in range(6)] + \
    [("UpdatedModule%02d.rte" % i, "Updated%02d" % i, 1) for i in range(6)]
TALL_HOST_MODS = [(BASE_NAME, "Base1", 1)] + \
    [("InstalledModule%02d.rte" % i, "Installed%02d" % i, 1) for i in range(6)] + \
    [("UpdatedModule%02d.rte" % i, "Updated%02d" % i, 2) for i in range(6)]
PANEL_GRAY = (59, 65, 83)
BUTTON_BLUE = (108, 118, 168)
GOLD = (170, 120, 0)


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(repo, *args, binary=False):
    proc = subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                          text=not binary)
    if proc.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
    return proc.stdout if binary else proc.stdout.strip()


def pin_state(repo):
    return {"head": git(repo, "rev-parse", "HEAD"),
            "diff_sha256": hashlib.sha256(git(repo, "diff", "HEAD", "--", binary=True)).hexdigest(),
            "exe_sha256": sha256_file(Path(repo) / "Cortex Command.exe")}


class PinDrift(RuntimeError):
    pass


def require_pin(repo, expected_exe, baseline, checks, tag):
    state = pin_state(repo)
    checks[f"pin_{tag}"] = (state["exe_sha256"] == expected_exe
                            and state["head"] == baseline["head"]
                            and state["diff_sha256"] == baseline["diff_sha256"])
    if not checks[f"pin_{tag}"]:
        raise PinDrift(f"pin drift at {tag}: {state}")
    return state


def close_all(runs):
    errors = []
    for key, run in runs.items():
        try:
            run.close()
        except Exception as error:
            errors.append(f"{key}: {error}")
    return errors


def game_version(repo):
    header = Path(repo) / "Source/System/GameVersion.h"
    match = re.search(r'c_VersionString\s*=\s*"([^"]+)"', header.read_text(encoding="utf-8"))
    if not match:
        raise RuntimeError("could not read c_VersionString from " + str(header))
    return match.group(1)


def resolve_tools(repo):
    tools = Path(repo).resolve() / "tools"
    if not (tools / "run_sim_test.py").is_file():
        raise RuntimeError("no tools/run_sim_test.py under " + str(repo))
    sys.path.insert(0, str(tools))
    import run_sim_test
    import test_lobby_lifecycle
    return run_sim_test.make_run, test_lobby_lifecycle.wait_for_log


def stage_module(run, dir_name, friendly, game_ver, module_version=1):
    module_dir = run.cwd / "Mods" / dir_name
    index_path = module_dir / "Index.ini"
    if len(str(index_path)) > 259:
        raise RuntimeError("path too long before staging: " + str(index_path))
    module_dir.mkdir()
    ini = (f"DataModule\n\tModuleName = {friendly}\n\tSupportedGameVersion = {game_ver}\n"
           f"\tVersion = {module_version}\n")
    (module_dir / "Index.ini").write_text(ini, encoding="utf-8")
    return module_dir


def set_resolution(run, res_x, res_y):
    ini = run.cwd / "Userdata" / "Settings.ini"
    original = ini.read_text(encoding="utf-8")
    patched = original
    for name, value in {"ResolutionX": res_x, "ResolutionY": res_y}.items():
        patched, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*",
                                 lambda m: m[1] + str(value), patched)
        if count == 0:
            patched += f"\n\t{name} = {value}\n"
    ini.write_text(patched, encoding="utf-8")
    return {"original_sha256": hashlib.sha256(original.encode("utf-8")).hexdigest(),
            "patched_sha256": hashlib.sha256(patched.encode("utf-8")).hexdigest()}


def png_size(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return [int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")]


def near(pixel, color, tol):
    return all(abs(pixel[i] - color[i]) <= tol for i in range(3))


def panel_extent(px, width, height):
    """The gray panel's horizontal extent: median left/right over rows whose
    longest contiguous panel-gray run is at least 60px. Returns
    (median_row, left, right, right)."""
    rows = []
    for y in range(10, height - 10):
        run = left = 0
        best_run = best_left = best_right = 0
        for x in range(width):
            if near(px[x, y], PANEL_GRAY, 8):
                if run == 0:
                    left = x
                run += 1
                if run > best_run:
                    best_run, best_left, best_right = run, left, x
            else:
                run = 0
        if best_run >= 60:
            rows.append((y, best_left, best_right))
    if not rows:
        return None
    rows.sort(key=lambda r: r[0])
    mid_y = rows[len(rows) // 2][0]
    lefts = sorted(r[1] for r in rows)
    rights = sorted(r[2] for r in rows)
    left = lefts[len(lefts) // 2]
    right = rights[len(rights) // 2]
    return (mid_y, left, right, right)


def panel_vertical(px, width, height):
    """Topmost/bottommost rows that carry a >100px panel-gray run."""
    top = bottom = None
    for y in range(height):
        run = longest = cur = 0
        for x in range(width):
            if near(px[x, y], PANEL_GRAY, 8):
                cur += 1
                longest = max(longest, cur)
            else:
                cur = 0
        if longest > 100:
            if top is None:
                top = y
            bottom = y
    return top, bottom


def is_gold_ink(pixel):
    r, g, b = pixel[:3]
    return r > 150 and g > 100 and r > b + 60


def gold_ink_columns(px, x0, x1, y0, y1):
    cols = set()
    for y in range(y0, y1):
        for x in range(x0, x1):
            if is_gold_ink(px[x, y]):
                cols.add(x)
    return cols


def band_diff(img_a, img_b, x0, x1, y0, y1):
    pa, pb = img_a.load(), img_b.load()
    total = diff = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += 1
            a, b = pa[x, y][:3], pb[x, y][:3]
            if abs(a[0] - b[0]) > 24 or abs(a[1] - b[1]) > 24 or abs(a[2] - b[2]) > 24:
                diff += 1
    return diff / max(1, total)


def is_ink(pixel):
    # Text ink is whatever is not panel fill or frame colors inside the panel:
    # the fallback skin font renders white/gray-green, not the usual gold.
    return not (near(pixel, PANEL_GRAY, 8)
                or near(pixel, BUTTON_BLUE, 25)
                or near(pixel, (24, 28, 55), 25))


def longest_ink_run(px, x0, x1, y0, y1, gap_tol=2):
    """Longest near-contiguous run of text-ink columns on one row: glyph cells
    are separated by at most gap_tol blank columns. Returns
    (span, row, run_x0, run_x1) or None."""
    best = None
    for y in range(y0, y1):
        run_start = last_ink = -1
        row_best = None
        for x in range(x0, x1):
            if is_ink(px[x, y]):
                if run_start < 0:
                    run_start = x
                last_ink = x
            elif run_start >= 0 and x - last_ink > gap_tol:
                span = last_ink - run_start + 1
                if row_best is None or span > row_best[0]:
                    row_best = (span, run_start, last_ink)
                run_start = -1
        if run_start >= 0:
            span = last_ink - run_start + 1
            if row_best is None or span > row_best[0]:
                row_best = (span, run_start, last_ink)
        if row_best and (best is None or row_best[0] > best[0]):
            best = (row_best[0], y, row_best[1], row_best[2])
    return best


def font_cell_width(png_path, char_index):
    """Cell width of one glyph, replicating GUIFont::Load's red-separator scan
    so the detector can predict the measured token width instead of guessing."""
    from PIL import Image
    img = Image.open(png_path).convert("RGBA")
    width, height = img.size
    px = img.load()
    red = px[0, 0]
    font_h = next(y for y in range(1, height) if px[0, y] == red)
    x, y, col = 1, 0, 0
    for chr_ in range(32, 256):
        cell_w = 0
        for n in range(x, width):
            if px[n, y] == red:
                break
            cell_w += 1
        if chr_ == char_index:
            return max(0, cell_w - 1)
        x += cell_w + 1
        col += 1
        if col >= 16:
            col = 0
            x = 1
            y += font_h
            if y + font_h > height:
                break
    return 0


def outside_band_diff(img_a, img_b, excl):
    """Fraction of changed pixels over the whole frame, skipping the exclusion
    rectangle (x0, x1, y0, y1). Scrolling inside the label band must leave every
    other pixel untouched."""
    x0, x1, y0, y1 = excl
    pa, pb = img_a.load(), img_b.load()
    width, height = img_a.size
    total = diff = 0
    for y in range(height):
        for x in range(width):
            if x0 <= x < x1 and y0 <= y < y1:
                continue
            total += 1
            a, b = pa[x, y][:3], pb[x, y][:3]
            if abs(a[0] - b[0]) > 24 or abs(a[1] - b[1]) > 24 or abs(a[2] - b[2]) > 24:
                diff += 1
    return diff / max(1, total)


FRAME_BLUE = (108, 118, 168)


def is_hole(pixel):
    """A transparent panel pixel shows the backdrop: every channel is dark,
    unlike the panel's navy frame bezel (24,28,55)."""
    return pixel[0] < 45 and pixel[1] < 45 and pixel[2] < 48


def real_panel_edges(px, width, height, extent, gtop, gbottom):
    """Real bitmap edges: the opaque blue frame column plus the outermost
    bitmap column past it (colour-key over filler on the fix, the painted
    bezel or a gap on the old tiling). Returns (left, right, top, bottom)."""
    _, gleft, gright, _ = extent
    def blue_col(x):
        hits = sum(1 for y in range(gtop + 4, gbottom - 3) if near(px[x, y], FRAME_BLUE, 28))
        return hits > (gbottom - gtop - 7) * 0.6
    def blue_row(y):
        hits = sum(1 for x in range(gleft + 4, gright - 3) if near(px[x, y], FRAME_BLUE, 28))
        return hits > (gright - gleft - 7) * 0.6
    left_frame = next((x for x in range(gleft - 1, max(0, gleft - 8), -1) if blue_col(x)), gleft)
    right_frame = next((x for x in range(gright + 1, min(width, gright + 8)) if blue_col(x)), gright)
    top_frame = next((y for y in range(gtop - 1, max(0, gtop - 8), -1) if blue_row(y)), gtop)
    bottom_frame = next((y for y in range(gbottom + 1, min(height, gbottom + 8)) if blue_row(y)), gbottom)
    return left_frame - 1, right_frame + 1, top_frame - 1, bottom_frame + 1


def wrapped_height_px(text, inner_px, cell_w):
    """Greedy word-wrap replication of GUIFont::DrawAligned: words break on
    spaces only; a word that does not fit the remaining line starts a new one.
    Returns the predicted text height in pixels (FontLarge rows are 15px)."""
    lines, cur = 1, 0
    for word in text.split(" "):
        width = sum(cell_w(b) for b in word.encode("utf-8"))
        if cur and cur + 1 + width > inner_px:
            lines += 1
            cur = 0
        cur += width if cur == 0 else 1 + width
    return lines * 15


def button_extent(px, width, y0, y1):
    """The Back button's horizontal extent: the first contiguous run of the
    button frame blue between 60 and 220px long inside the y-band."""
    best = None
    for y in range(y0, y1):
        run = 0
        start = -1
        for x in range(width):
            if near(px[x, y], BUTTON_BLUE, 25):
                if run == 0:
                    start = x
                run += 1
            else:
                if 60 <= run <= 220 and (best is None or run > best[2] - best[1]):
                    best = (y, start, x - 1)
                run = 0
        if 60 <= run <= 220 and (best is None or run > best[2] - best[1]):
            best = (y, start, width - 1)
    return best


def gold_text_rows(px, width, height):
    """Rows carrying >=10 gold text-ink pixels. The main menu's own items are
    unframed gold text (its buttons draw no blue frame), so the real menu
    scores ~150+ while a bare starfield or a black frame scores ~0."""
    return sum(1 for y in range(height)
               if sum(1 for x in range(width) if is_gold_ink(px[x, y])) >= 10)


def coarse_cell_diff(img_a, img_b, cell=16, pix_tol=24, frac=0.2):
    """Share of grid cells whose changed-pixel fraction exceeds frac. Whole
    fixed UI regions read as changed cells; sparse starfield twinkle does not."""
    pa, pb = img_a.load(), img_b.load()
    width, height = img_a.size
    changed = total = 0
    for cy in range(0, height, cell):
        for cx in range(0, width, cell):
            total += 1
            diff = n = 0
            for y in range(cy, min(cy + cell, height)):
                for x in range(cx, min(cx + cell, width)):
                    n += 1
                    a, b = pa[x, y][:3], pb[x, y][:3]
                    if abs(a[0] - b[0]) > pix_tol or abs(a[1] - b[1]) > pix_tol or abs(a[2] - b[2]) > pix_tol:
                        diff += 1
            if diff > frac * n:
                changed += 1
    return changed / max(1, total)


def post_back_is_main(post_back_path, main_start_path):
    """True when the post-Back frame shows the main menu again: no full-size
    gray multiplayer panel, its coarse structure matches this run's own
    main-start capture, and the main menu's button rows are present."""
    from PIL import Image
    img = Image.open(post_back_path).convert("RGB")
    px = img.load()
    width, height = img.size
    extent = panel_extent(px, width, height)
    if extent and extent[2] - extent[1] >= 300:
        return False, {"reason": "multiplayer panel still present", "extent": extent}
    main_img = Image.open(main_start_path).convert("RGB")
    diff = coarse_cell_diff(main_img, img)
    buttons = gold_text_rows(px, width, height)
    ok = diff <= 0.30 and buttons >= 8
    return ok, {"main_diff": round(diff, 4), "gold_rows": buttons}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, default=47871)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--no-scroll-input", action="store_true",
                        help="omit the scrollwide/scrolltall joiner inputs; the scroll checks must then fail closed")
    options = parser.parse_args()
    if not (320 <= options.width <= 7680 and 240 <= options.height <= 4320):
        parser.error("dimensions out of range")
    if not (1024 <= options.port <= 65535):
        parser.error("port out of range")
    if not re.fullmatch(r"[0-9a-f]{64}", options.exe_sha256):
        parser.error("--exe-sha256 must be 64 lowercase hex chars")

    make_run, wait_for_log = resolve_tools(options.repo)
    repo = options.repo.resolve()
    before = pin_state(repo)
    if before["exe_sha256"] != options.exe_sha256:
        raise SystemExit("exe pin mismatch at start: " + before["exe_sha256"])

    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=False)
    version = game_version(repo)
    result = {"pass": False, "version": version, "viewport_request": [options.width, options.height],
              "pin_before": before}
    checks, details = {}, {}
    runs, records = {}, {}
    phase = "viewport"

    def start(name, host, port, suffix, modules, phase="viewport"):
        require_pin(repo, options.exe_sha256, before, checks, f"{name}_prelaunch")
        script = f"wait 40\nscreenshot main-start\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name}\n"
        if host:
            script += (f"activate ButtonMultiplayerHostGame\nwait 10\nsettext TextHostPort {port}\n"
                       "settext TextHostPlayers 2\nsettext TextHostInputDelay 3\nactivate ButtonMultiplayerCreate\n")
        else:
            script += ("activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                       f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\n")
        path = root / phase / f"{name}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(script + suffix, encoding="utf-8")
        run = make_run(repo, ["-menu-script", path, "-num-lua-states", 4], root / phase / name, 200)
        runs[name] = run
        staged = [str(stage_module(run, m[0], m[1], version, m[2] if len(m) > 2 else 1))
                  for m in modules]
        settings = set_resolution(run, options.width, options.height)
        details[f"staged_{name.lower()}"] = staged
        details[f"settings_{name.lower()}"] = settings
        run.start()
        return run

    try:
        details[phase] = {"port": options.port,
                          "staged_name": {"dir": SZ70_NAME, "cp1252_len": len(SZ70_NAME.encode("cp1252"))}}
        host_script = ("wait_error could not join\nassert_substate Lobby\nassert_error could not join\n"
                       "dump_lobby\nscreenshot host-lobby-t0\nwait_ms 13000\nscreenshot host-lobby-t1\n"
                       "goto_main\nassert_screen MainScreen\nexit\n")
        host = start("Host", True, options.port, host_script, [(BASE_NAME, "Base1")])
        wait_for_log(host, "activate ButtonMultiplayerCreate ok=1")
        # post_command raises the button's Command event after the GUI update,
        # the same route a real click takes through HandleInputEvents. The
        # timed t1-t3 frames are the scripted scroll input; --no-scroll-input
        # drops them so the scroll checks below have nothing to measure.
        if options.no_scroll_input:
            joiner_script = (f"wait_state Failed\nwait 5\nassert_substate Landing\nassert_label {LABEL} yours\n"
                             "dump_lobby\nscreenshot viewport-t0\n"
                             "post_command ButtonBackToMain\nwait 12\nscreenshot post-back\nassert_screen MainScreen\nexit\n")
        else:
            joiner_script = (f"wait_state Failed\nwait 5\nassert_substate Landing\nassert_label {LABEL} yours\n"
                             "dump_lobby\nscreenshot viewport-t0\nwait_ms 9000\nscreenshot viewport-t1\n"
                             "wait_ms 9000\nscreenshot viewport-t2\nwait_ms 9000\nscreenshot viewport-t3\n"
                             "post_command ButtonBackToMain\nwait 12\nscreenshot post-back\nassert_screen MainScreen\nexit\n")
        start("Joiner", False, options.port, joiner_script,
              [(BASE_NAME, "Base1"), (SZ70_NAME, "Sz70")])
        records["Joiner"] = runs["Joiner"].finish()
        records["Host"] = runs["Host"].finish()

        # --- Phase "tall": many short names wrap the "Remove:" list past the
        # status room in the 300px panel, so the label's vertical overflow
        # scroll must move the band. Skipped entirely when --no-scroll-input
        # removes the scripted input; the checks below then fail closed.
        tall_shots = []
        tall_label = None
        if not options.no_scroll_input:
            start("TallHost", True, options.port + 1,
                  "wait_error could not join\nwait_ms 3000\ngoto_main\nassert_screen MainScreen\nexit\n",
                  [(d, f, v) for d, f, v in TALL_HOST_MODS], phase="tall")
            wait_for_log(runs["TallHost"], "activate ButtonMultiplayerCreate ok=1")
            tall_script = ("wait_state Failed\nwait 5\nassert_substate Landing\n"
                           "assert_label %s yours\nscreenshot tall-t0\n" % LABEL +
                           "".join("wait_ms 750\nscreenshot tall-t%d\n" % n for n in range(1, 9)) +
                           "post_command ButtonBackToMain\nwait 12\nscreenshot post-back\n"
                           "assert_screen MainScreen\nexit\n")
            start("TallJoiner", False, options.port + 1, tall_script,
                  [(d, f, v) for d, f, v in TALL_JOINER_MODS], phase="tall")
            records["TallJoiner"] = runs["TallJoiner"].finish()
            records["TallHost"] = runs["TallHost"].finish()
            tall_raw = (root / "tall" / "TallJoiner" / "stdout.log").read_bytes()
            tall_log = tall_raw.decode("utf-8", errors="replace")
            m = re.search(r'assert_label ' + LABEL + ' "yours" text="(.*?)" (?:PASS|FAIL)', tall_log, re.S)
            tall_label = m.group(1) if m else None
            tall_shots = sorted((root / "tall" / "TallJoiner" / "runtime/ScreenShots").glob("tall-t*_*.png"))
            details["tall_screenshots"] = [str(p) for p in tall_shots]
            checks["tall_landed"] = "assert_substate expected=Landing actual=Landing PASS" in tall_log
            checks["tall_shot_count"] = len(tall_shots) == 9
            checks["tall_back_command_posted"] = "post_command ButtonBackToMain ok=1" in tall_log
            checks["tall_returned_to_main"] = "assert_screen expected=MainScreen actual=MainScreen PASS" in tall_log
        else:
            details["tall_scroll_input"] = "omitted by --no-scroll-input"
            for key in ("tall_landed", "tall_shot_count", "tall_back_command_posted",
                        "tall_returned_to_main"):
                checks[key] = False

        # --- Phase "seam": an '@' name whose produced panel width lands on
        # 5 mod 8 - the residue where the old filler tiling left the bitmap's
        # base exposed under the frame's colour-key edge pixels. The produced
        # width is word+constant, so a second attempt corrects the residue.
        seam_logs, seam_shots, seam_width = {}, [], None
        seam_n = 57
        for attempt in range(2):
            seam_name = SEAM_CHAR * seam_n + ".rte"
            tag = "Seam" if attempt == 0 else "SeamB"
            start(tag + "Host", True, options.port + 2 + attempt * 3,
                  "wait_error could not join\nwait_ms 3000\ngoto_main\nassert_screen MainScreen\nexit\n",
                  [(BASE_NAME, "Base1")], phase="seam")
            wait_for_log(runs[tag + "Host"], "activate ButtonMultiplayerCreate ok=1")
            seam_script = ("wait_state Failed\nwait 5\nassert_substate Landing\n"
                           "assert_label %s yours\nscreenshot seam-t0\nwait_ms 3000\n"
                           "screenshot seam-t1\npost_command ButtonBackToMain\nwait 12\n"
                           "screenshot post-back\nassert_screen MainScreen\nexit\n" % LABEL)
            start(tag + "Joiner", False, options.port + 2 + attempt * 3, seam_script,
                  [(BASE_NAME, "Base1"), (seam_name, "Seam")], phase="seam")
            records[tag + "Joiner"] = runs[tag + "Joiner"].finish()
            records[tag + "Host"] = runs[tag + "Host"].finish()
            logs_key = tag + "Joiner"
            seam_logs[tag] = (root / "seam" / logs_key / "stdout.log").read_bytes().decode("utf-8", errors="replace")
            shots = sorted((root / "seam" / logs_key / "runtime/ScreenShots").glob("seam-t*_*.png"))
            details["seam%d_screenshots" % attempt] = [str(p) for p in shots]
            details["seam%d_name_len" % attempt] = len(seam_name)
            if shots:
                from PIL import Image as _Img
                spx = _Img.open(shots[0]).convert("RGB").load()
                ext = panel_extent(spx, options.width, options.height)
                if ext:
                    sgt, sgb = panel_vertical(spx, options.width, options.height)
                    srl, srr, _, _ = real_panel_edges(spx, options.width, options.height,
                                                    ext, sgt or 0, sgb or options.height - 1)
                    produced = srr - srl + 1
                    details["seam%d_produced_width" % attempt] = produced
                    seam_width = produced
                    seam_shots = shots
                    if produced % 8 == 5:
                        break
                    seam_n += (5 - produced % 8) % 8 or 8
                    seam_n = seam_n if seam_n <= 60 else seam_n - 8
        seam_log = seam_logs.get("SeamB" if (len(seam_logs) > 1 and seam_width and seam_width % 8 == 5) else "Seam", "")
        checks["seam_landed"] = "assert_substate expected=Landing actual=Landing PASS" in seam_log
        checks["seam_shot_count"] = len(seam_shots) == 2

        logs, raw = {}, {}
        for name in ("Host", "Joiner"):
            raw[name] = (root / phase / name / "stdout.log").read_bytes()
            logs[name] = raw[name].decode("utf-8", errors="replace")
        for name, log in logs.items():
            record = records[name]
            console = root / phase / name / "runtime/LogConsole.txt"
            full_log = log + ("\n" + console.read_text(errors="replace") if console.exists() else "")
            errors = re.findall(r"^.*(?:FAILED:|FAIL:|ERROR:|EXCEPTION_|RTE Assert|RTE Abort|stack traceback).*$",
                                full_log, re.M)
            checks[f"{name}_process"] = record["exit_code"] == 0 and not record["timed_out"]
            checks[f"{name}_no_errors"] = not errors
            checks[f"{name}_desktop"] = record["input_desktop_before"] == record["input_desktop_after"]
            details[name] = {"errors": errors, "exit": record["exit_code"], "binary": record["exe_sha256"]}

        checks["joiner_landed"] = "assert_substate expected=Landing actual=Landing PASS" in logs["Joiner"]
        checks["joiner_never_started"] = "dump_lobby state=Failed" in logs["Joiner"]
        wide_label = re.search(r'assert_label ' + LABEL + ' "yours" text="(.*?)" (?:PASS|FAIL)',
                               logs["Joiner"], re.S)
        details["joiner_label"] = wide_label.group(1) if wide_label else None
        checks["recognized_summary"] = bool(wide_label) and wide_label.group(1).startswith(PREFIX)
        # The label echoes raw cp1252 bytes; the 0xDF run survives the wire's 64-byte cut.
        label_body = re.search(rb'assert_label ' + LABEL.encode() + rb' "[^"]*" text="(.*?)" (?:PASS|FAIL)',
                               raw["Joiner"], re.S)
        checks["wire_name_in_label"] = bool(label_body) and b"\xdf" * 60 in label_body.group(1)
        checks["host_stayed_in_lobby"] = "assert_substate expected=Lobby actual=Lobby PASS" in logs["Host"]
        checks["host_returned_to_main"] = "assert_screen expected=MainScreen actual=MainScreen PASS" in logs["Host"]
        checks["joiner_returned_to_main"] = "assert_screen expected=MainScreen actual=MainScreen PASS" in logs["Joiner"]
        checks["back_command_posted"] = "post_command ButtonBackToMain ok=1" in logs["Joiner"]

        from PIL import Image
        joiner_shots = sorted((root / phase / "Joiner" / "runtime/ScreenShots").glob("viewport-t*_*.png"))
        host_shots = sorted((root / phase / "Host" / "runtime/ScreenShots").glob("host-lobby-t*_*.png"))
        details["joiner_screenshots"] = [str(p) for p in joiner_shots]
        details["host_screenshots"] = [str(p) for p in host_shots]
        checks["joiner_shot_count"] = len(joiner_shots) == 4
        checks["host_shot_count"] = len(host_shots) == 2
        checks["shot_dims"] = all(png_size(p) == [options.width, options.height]
                                 for p in joiner_shots + host_shots)

        if joiner_shots:
            images = [Image.open(p).convert("RGB") for p in joiner_shots]
            px0 = images[0].load()
            width, height = images[0].size

            extent = panel_extent(px0, width, height)
            details["panel_extent_t0"] = extent
            checks["panel_extent_found"] = extent is not None
            if extent:
                row, left, right, _ = extent
                details["panel_edges"] = {"row": row, "left": left, "right": right}
                checks["panel_left_edge_inside"] = left >= 3
                checks["panel_right_edge_inside"] = right <= width - 4
                checks["panel_is_full_panel"] = right - left >= 300
                top, bottom = panel_vertical(px0, width, height)
                details["panel_vertical"] = {"top": top, "bottom": bottom}
                checks["panel_top_inside"] = top is not None and top >= 0
                checks["panel_bottom_inside"] = bottom is not None and bottom <= height - 1

                edge_cols = [x for x in (0, 1, width - 2, width - 1)]
                edge_gray = sum(1 for x in edge_cols for y in range(max(0, (top or 0)), min(height, (bottom or 0) + 1))
                                if near(px0[x, y], PANEL_GRAY, 8))
                details["edge_gray_pixels"] = edge_gray
                checks["no_panel_at_viewport_edges"] = edge_gray == 0

                # Status-label rows only, inside the panel: any pixel that is not
                # fill or frame there is diagnostic text ink regardless of color.
                ink = set()
                for y in range((top or 0) + 120, (bottom or height - 1) + 1):
                    for x in range(left, right + 1):
                        if is_ink(px0[x, y]):
                            ink.add(x)
                details["status_ink_columns"] = len(ink)
                checks["status_ink_present"] = len(ink) >= 30
                # Escaped text is the F1 signature: gold ink (the skin font's
                # color) past the drawn panel edge inside the status band.
                # Backdrop stars/planet never match the gold predicate.
                escaped = gold_ink_columns(px0, 0, width, (top or 0) + 120,
                                         (bottom or height - 1) + 1)
                escaped = {x for x in escaped if x < left - 2 or x > right + 2}
                details["escaped_ink_columns"] = sorted(escaped)[:10]
                checks["ink_inside_panel"] = not escaped

                button = button_extent(px0, width, (bottom or height - 60) + 1, height - 5)
                details["back_button_extent"] = button
                checks["back_button_found"] = button is not None and button[2] - button[1] >= 60
                if button:
                    checks["back_button_inside_viewport"] = button[1] >= 3 and button[2] <= width - 4
                    # Horizontal containment only: the button's columns sit inside the
                    # panel's drawn columns. Vertical order is proven separately below.
                    checks["back_button_within_panel_columns"] = button[1] >= left - 2 and button[2] <= right + 2
                    checks["back_button_rows_below_panel"] = bottom is None or button[0] > bottom

                # The wire name is 64 x 0xDF; the label picks a skin font whose
                # atlas can draw it, so the token is one near-contiguous ink run.
                # The scan is bounded to the panel so backdrop cannot fake a run.
                name_run = longest_ink_run(px0, left, right + 1, (top or 0) + 120, bottom or height - 1)
                details["name_ink_run"] = name_run
                checks["name_glyph_ink_present"] = bool(name_run) and name_run[0] >= 240
                if name_run:
                    zoom = images[0].crop((max(0, name_run[2] - 10), max(0, name_run[1] - 4),
                                           min(width, name_run[3] + 11), min(height, name_run[1] + 12)))
                    zoom = zoom.resize((zoom.width * 4, zoom.height * 4), Image.NEAREST)
                    zoom.save(root / "fallback-zoom.png")
                    details["fallback_zoom"] = str(root / "fallback-zoom.png")

                    # Separated dimmer cells: each fallback cell keeps a 1px blank
                    # column (a contiguous bar shows none) and its ink is ~55% of
                    # the text gold, same hue (full gold fails the dim bound).
                    _span, run_y, run_x0, run_x1 = name_run
                    band = range(max(0, run_y - 1), min(height, run_y + 8))
                    gaps = sum(1 for x in range(run_x0, run_x1 + 1)
                               if all(not is_ink(px0[x, y]) for y in band))
                    from collections import Counter
                    row_inks = Counter(tuple(px0[x, y]) for y in (run_y - 1, run_y, run_y + 1)
                                       for x in range(run_x0, run_x1 + 1)
                                       if 0 <= y < height and is_ink(px0[x, y]))
                    warm = Counter({c: n for c, n in row_inks.items()
                                    if c[0] > c[1] > c[2] and c[0] > 60})
                    dom = warm.most_common(1)[0][0] if warm else (0, 0, 0)
                    details["fallback_run"] = {"blank_columns": gaps, "dominant_ink": list(dom)}
                    checks["fallback_cells_separated"] = gaps >= 16
                    checks["fallback_ink_dimmed"] = bool(warm) and dom[0] < 200

                # Mirror the engine's measure: the fallback font's cell width x
                # the 64 wire bytes predicts the token width. The label is
                # panel-24 wide; the measured gray span is the panel minus its
                # 2px left and 3px right frames, so the label is span - 19.
                skin_dir = repo / "Data/Base.rte/GUIs/Skins/Menus"
                small_w = font_cell_width(skin_dir / "FontSmall.png", 0xDF)
                large_w = font_cell_width(skin_dir / "FontLarge.png", 0xDF)
                token_small = 64 * small_w
                label_inner = (right - left + 1) - 19
                details["font_cells"] = {"FontSmall_0xDF": small_w, "FontLarge_0xDF": large_w,
                                         "token_px": token_small, "label_inner": label_inner}
                scroll_expected = token_small > label_inner + 2
                details["scroll_mode"] = "scroll" if scroll_expected else "fits"

                band_y0 = (top or 0) + int(((bottom or 0) - (top or 0)) * 0.6)
                band_y1 = (bottom or height - 1) - 4
                diffs = []
                for img in images[1:]:
                    diffs.append(band_diff(images[0], img, max(0, left), min(width, right + 1),
                                           band_y0, band_y1))
                details["scroll_band"] = {"y0": band_y0, "y1": band_y1, "x0": left, "x1": right}
                details["scroll_diffs_vs_t0"] = diffs
                checks["scroll_motion"] = (not scroll_expected) or sum(1 for d in diffs if d > 0.03) >= 2

                rx0 = left + int((right - left) * 0.75)
                right_diffs = [band_diff(images[0], img, rx0, right + 1, band_y0, band_y1)
                               for img in images[1:]]
                details["scroll_right_diffs"] = right_diffs
                checks["scroll_tail_reached"] = (not scroll_expected) or max(right_diffs, default=0) > 0.08
                if options.no_scroll_input:
                    checks["scroll_motion"] = checks["scroll_tail_reached"] = False
                checks["name_ink_within_panel"] = bool(name_run) and name_run[2] >= left - 2 and name_run[3] <= right + 2
                checks["name_token_fully_visible"] = scroll_expected or (bool(name_run) and name_run[0] >= token_small - 8)

            details["frame_shas"] = [sha256_file(p)[:16] for p in joiner_shots]
            checks["frames_not_identical"] = len(set(details["frame_shas"])) >= 2

        # LabelMultiplayerStatus is a 16px box at panel-rel Y 144; the panel's
        # controls anchor 2px above the first gray row. A wrapped status line
        # is centered over the box and its top rows are cut by the label clip,
        # so the first ink block must start inside the band and span a full
        # FontLarge glyph height - seeing ink at the band's top edge is the clip.
        checks["host_status_unclipped"] = False
        if host_shots:
            from PIL import Image as _Img2
            himg = _Img2.open(host_shots[0]).convert("RGB")
            hpx = himg.load()
            hw, hh = himg.size
            hext = panel_extent(hpx, hw, hh)
            if hext:
                htop, _hbot = panel_vertical(hpx, hw, hh)
                if htop is not None:
                    band_top = htop + 142
                    blocks = []
                    y = band_top
                    while y < band_top + 16:
                        cols = sum(1 for x in range(hext[1] + 2, hext[2] - 1)
                                   if is_gold_ink(hpx[x, y]))
                        if cols >= 3:
                            start_y = y
                            while y < band_top + 16 and sum(
                                    1 for x in range(hext[1] + 2, hext[2] - 1)
                                    if is_gold_ink(hpx[x, y])) >= 3:
                                y += 1
                            blocks.append((start_y - band_top, y - 1 - band_top))
                        else:
                            y += 1
                    details["host_status_band"] = {"top": band_top, "blocks": blocks}
                    if blocks:
                        first = blocks[0]
                        checks["host_status_unclipped"] = (first[0] >= 2
                                                         and first[1] - first[0] + 1 >= 7)

        if not options.no_scroll_input and tall_shots:
            from PIL import Image as _Image
            tall_imgs = [_Image.open(p).convert("RGB") for p in tall_shots]
            tpx = tall_imgs[0].load()
            tw, th = tall_imgs[0].size
            textent = panel_extent(tpx, tw, th)
            details["tall_panel_extent"] = textent
            if textent:
                trow, tleft, tright, _ = textent
                ttop, tbottom = panel_vertical(tpx, tw, th)
                # The status label is the panel minus its 24px horizontal inset;
                # statusRoom = ResY - (Back 20+5) - labelRelY 168 - bottomPad 14.
                inner = (tright - tleft + 1) - 24
                room = th - 25 - 168 - 14
                skin_dir = repo / "Data/Base.rte/GUIs/Skins/Menus"
                cell = lambda b: font_cell_width(skin_dir / "FontLarge.png", b)
                predicted_h = wrapped_height_px(tall_label or "", inner, cell)
                scroll_expected = predicted_h + 4 > room
                details["tall_scroll"] = {"inner": inner, "room": room,
                                        "predicted_text_px": predicted_h,
                                        "scroll_expected": scroll_expected}
                ly0, ly1 = (ttop or 0) + 120, (tbottom or th - 1) - 2
                band = (max(0, tleft - 2), min(tw, tright + 3), ly0, ly1)
                td = [band_diff(tall_imgs[0], img, *band)
                      for img in tall_imgs[1:]]
                tout = [outside_band_diff(tall_imgs[0], img, band) for img in tall_imgs[1:]]
                details["tall_band_diffs"] = td
                details["tall_outside_diffs"] = tout
                # The 512-byte wire cap bounds the status to ~14 wrapped lines:
                # it overflows the ~153px room at 640x360 but fits at 960x540,
                # so motion is only demanded when the geometry predicts scroll.
                checks["tall_scroll_motion"] = (not scroll_expected) or sum(1 for d in td if d > 0.03) >= 2
                checks["tall_scroll_confined"] = max(tout, default=1.0) < 0.01
                # The tail is proven by scroll offset, not by a glyph mask:
                # the short closing line's ink pattern is too similar to the
                # hash fragment one line above it to locate reliably. Instead
                # each frame's label-band ink set is correlated against t0 -
                # captured inside the scroll's initial wait, so offset 0 - and
                # the argmax dy is that frame's absolute scroll offset. The
                # closing line is drawn iff a frame reaches the overflow
                # distance, i.e. predicted text height minus label room.
                checks["tall_tail_reached"] = False
                if scroll_expected and tall_shots:
                    label_top = (ttop or 0) - 2 + 168
                    label_bot = label_top + room
                    xl, xr = tleft + 12, tright - 12
                    ink_sets = []
                    for img in tall_imgs:
                        ipx = img.load()
                        ink_sets.append({(x, y - label_top)
                                         for y in range(label_top, label_bot)
                                         for x in range(xl, xr)
                                         if is_ink(ipx[x, y])})
                    base = ink_sets[0]
                    offsets = []
                    for s in ink_sets[1:]:
                        if not s or not base:
                            continue
                        scored = sorted(((sum(1 for x, y in s if (x, y + dy) in base), dy)
                                         for dy in range(0, 60)), reverse=True)
                        best_m, best_d = scored[0]
                        frac = best_m / len(s)
                        runner = scored[1][0] / len(s) if len(scored) > 1 else 0.0
                        if frac >= 0.75 and frac - runner >= 0.05:
                            offsets.append(best_d)
                    overflow = predicted_h - room
                    details["tall_tail"] = {"overflow_px": overflow,
                                            "frame_offsets": offsets,
                                            "max_offset": max(offsets, default=0)}
                    checks["tall_tail_reached"] = bool(offsets) and max(offsets) >= overflow - 4
                elif not scroll_expected:
                    checks["tall_tail_reached"] = True
                    details["tall_tail"] = "no scroll expected; full text inside the band"
            else:
                checks["tall_scroll_motion"] = checks["tall_scroll_confined"] = False
                checks["tall_tail_reached"] = False
        elif options.no_scroll_input:
            checks["tall_scroll_motion"] = checks["tall_scroll_confined"] = False
            checks["tall_tail_reached"] = False

        if seam_shots:
            from PIL import Image as _Image
            simg = _Image.open(seam_shots[0]).convert("RGB")
            spx = simg.load()
            sw, sh = simg.size
            sextent = panel_extent(spx, sw, sh)
            details["seam_panel_extent"] = sextent
            if sextent:
                sgt, sgb = panel_vertical(spx, sw, sh)
                sleft, sright, stop, sbottom = real_panel_edges(
                    spx, sw, sh, sextent, sgt or 0, sgb or sh - 1)
                seam_w = sright - sleft + 1
                details["seam_panel"] = {"left": sleft, "right": sright, "top": stop,
                                         "bottom": sbottom, "width": seam_w}
                checks["seam_panel_is_seam_width"] = seam_w % 8 == 5
                # The old filler tiling stopped at the frame boundary, so at
                # widths 5 mod 8 a filler column under the frame stayed
                # transparent and the backdrop shows through - a 1px hole band.
                # The label's text cannot reach the frame region (12px inset).
                gap = sright - 2
                right_holes = sum(1 for y in range(stop + 4, sbottom - 3)
                                  for x in (gap - 1, gap, sright)
                                  if is_hole(spx[x, y]))
                bottom_holes = sum(1 for x in range(sleft + 4, sright - 3)
                                   for y in (sbottom - 2, sbottom)
                                   if is_hole(spx[x, y]))
                details["seam_right_holes"] = right_holes
                details["seam_bottom_holes"] = bottom_holes
                checks["seam_right_column_filled"] = right_holes == 0
                checks["seam_bottom_row_filled"] = bottom_holes == 0
            else:
                for key in ("seam_panel_is_seam_width", "seam_right_column_filled",
                            "seam_bottom_row_filled"):
                    checks[key] = False

        # The Back press is only proven by what the screen shows after it: each
        # arm's post-back frame is compared against the same run's main-start
        # reference, so a withheld command leaves the multiplayer panel in view.
        post_back_details = {}
        post_back_ok = []
        seam_keep = "SeamB" if (len(seam_logs) > 1 and seam_width and seam_width % 8 == 5) else "Seam"
        for arm, run_dir in (("viewport", root / "viewport" / "Joiner"),
                             ("tall", root / "tall" / "TallJoiner"),
                             ("seam", root / "seam" / (seam_keep + "Joiner"))):
            shot_dir = run_dir / "runtime" / "ScreenShots"
            post = sorted(shot_dir.glob("post-back_*.png"))
            ref = sorted(shot_dir.glob("main-start_*.png"))
            if not post or not ref:
                post_back_details[arm] = {"reason": "missing post-back or main-start capture",
                                          "post_back": [str(p) for p in post],
                                          "main_start": [str(p) for p in ref]}
                post_back_ok.append(False)
                continue
            ok, info = post_back_is_main(post[-1], ref[-1])
            info["post_back"] = str(post[-1])
            info["main_start"] = str(ref[-1])
            post_back_details[arm] = info
            post_back_ok.append(bool(ok))
        details["post_back"] = post_back_details
        checks["post_back_main_screen"] = all(post_back_ok) and bool(post_back_ok)

        result["pin_after"] = require_pin(repo, options.exe_sha256, before, checks, "final")
        result.update({"checks": checks, "details": details})
        result["pass"] = all(checks.values())
    except Exception as error:
        result["error"] = str(error)
        result["checks"] = checks
        result["details"] = details
    finally:
        try:
            result["pin_after"] = pin_state(repo)
        except Exception as pin_error:
            result["pin_after_error"] = str(pin_error)
        close_errors = close_all(runs)
        if close_errors:
            result["close_errors"] = close_errors
            result["pass"] = False
        (root / "result.json").write_text(json.dumps(result, indent=2, default=str), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"),
                      "failed": [k for k, ok in result.get("checks", {}).items() if not ok],
                      "out": str(root)}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
