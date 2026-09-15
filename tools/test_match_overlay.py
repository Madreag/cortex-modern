"""Detect the match status widget, toast lifetime, HUD clearance and simulation invariance.

Two menu-launched peers run at each viewport under each NetworkMatchStatusMode.
Quiet and paused capture-on/off pairs compare complete traces. Host presses P to
pause; Guest presses P to resume. The network UI probe reads the drawn labels and
their rectangles. Additional arms force the widget's other triggers: the F6 seats
panel, a wall-clock stall on the guest, a guest drop that parks the host in
the reconnect hold, and the same drop from a peer whose name is the longest the
wire carries; that drop owes the player two toasts under every mode, the
notice and the wait banner with the reclaim window. Off never paints the widget
(event toasts still appear), Auto
paints it on triggers and three seconds past recovery, Always paints it
throughout. When it is up it must sit in the measured free zone: below 480 rows a
single-line strip between the funds block and the controller icon, at or above it
the top-right box. Every probe screenshot is read back like a tick capture: the
widget's frame whole, its text inside it, clear of the HUD and of the seats panel.
"""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

SCRATCH = Path("D:/mx/swe-overlay-layout-20260914")
PORT_BASE, PORT_COUNT = 48260, 10
TICKS = 900
DROP_SIGNAL = "host-past-290"
CAPTURE_TICKS = (60, 270, 500, 680, 880)
BOX_WIDTH, BOX_HEIGHT, BOX_TOP, BOX_MARGIN = 252, 76, 32, 8
STRIP_TOP, STRIP_LEFT, STRIP_RIGHT_MARGIN = 2, 152, 40
COMPACT_MAX_HEIGHT = 480
# The widest the widget itself can be: the compact band is 448 px, the box 252. The seats panel's
# frame is wider than this at every supported size, so it is never mistaken for the widget.
WIDGET_MAX_WIDTH = 460
# The longest display name the wire carries (NetProtocol.h c_MaxDisplayNameBytes = 64).
LONG_GUEST_NAME = "Guest-" + "0123456789" * 5 + "-LONGEND"
STATUS = "LabelNetMatchStatus"
STATUS_BOX = "BoxNetMatchStatus"
PANEL = "NetworkSeats"
TOAST = "LabelNetMatchToastNewest"
# The toast stack draws oldest first, so the two rows read as the order they were pushed in.
TOAST_FIRST, TOAST_SECOND = "LabelNetMatchToast0", "LabelNetMatchToast1"
PAUSED = "Match paused by Host"
RESUMED = "Match resumed by Guest"
HOLD_BANNER = re.compile(r"Match paused: waiting for (\S+) to return \((\d+)s left\)")
HOLD_BANNER_LINE = re.compile(r"^\[net-match\] match paused waiting for (\S+) \((\d+)s left, tick (\d+)\)$")
WIDGET_FILL = (20, 22, 27)
WIDGET_BORDER = (59, 65, 83)
WIDGET_ACCENTS = ((108, 118, 168), (170, 120, 0))
CAPTURE = re.compile(r"^\[net-match-screenshot\] applied_tick=(\d+) name=(\S+) queued=([01])$")
PAUSE_EVENT = re.compile(r"^\[net-match\] match paused at tick (\d+) sim ms \d+$")
RESUME_EVENT = re.compile(r"^\[net-match\] match resumed at tick (\d+) sim ms \d+$")

# The widget's mode axis: every arm runs once per mode. "auto" is the persisted
# default, so the capture-switch pairs are taken from its runs.
MODES = ("auto", "off", "always")
MODE_INI = {"off": "Off", "auto": "Auto", "always": "Always"}

# Arms beyond the original four drive the remaining visibility states. "event" is
# the scripted pause/resume; "f6" opens the seats panel; "stall" makes the guest
# sleep 8 s at tick 300 so the host waits on frames; "leave" kills the guest once
# the host is past 290 so the host holds its dropped seat; "long_name" runs that
# drop with a guest named the longest name the wire carries.
ARMS = {
    "on": {"captures": True},
    "off": {},
    "pause_on": {"captures": True, "event": True},
    "pause_off": {"event": True},
    "f6": {"f6": True},
    "stall": {"stall": True},
    "hold": {"leave": True},
    "holdlong": {"leave": True, "long_name": True},
}

# The HUD furniture the widget must never touch, measured on the retained
# captures: funds/health block top-left, the controller icon top-right (drawn for
# the first 30 s), and the right-side band the alarm marker and actor labels
# occupy. The bottom band carries nothing fixed in-match in this build, so it is
# not listed; the toast stack's band is still checked against the same rects.
def hud_rects(size):
    w, h = size
    rects = {"funds_block": (0, 0, 144, 18), "controller_icon": (w - 38, 0, 38, 18)}
    if (w, h) == (960, 540):
        rects["right_markers"] = (660, 140, 300, 140)
    else:
        sx, sy = w / 640, h / 360
        rects["right_markers"] = (int(500 * sx), int(88 * sy), int(140 * sx), int(112 * sy))
    return rects


def rects_intersect(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and ax + aw > bx and ay < by + bh and ay + ah > by


def occluding_rects(rect, size):
    return [name for name, hud in hud_rects(size).items() if rects_intersect(rect, hud)]


def find_widget_paint(image, size):
    """Bounding box of the drawn widget, or None. The widget's own border color
    (59,65,83) frames it; a >=60px contiguous run marks a horizontal edge, which
    no HUD element or this scene reproduces. The band covers both layouts."""
    width, height = size
    pixels = image.load()
    region_bottom = min(BOX_TOP + BOX_HEIGHT + 22, height)
    for y in range(region_bottom):
        x = 0
        while x < width:
            if pixels[x, y] != WIDGET_BORDER:
                x += 1
                continue
            x0 = x
            while x < width and pixels[x, y] == WIDGET_BORDER:
                x += 1
            if x - x0 < 60:
                continue
            bottom = y
            for yy in range(y + 1, region_bottom):
                if any(pixels[xx, yy] == WIDGET_BORDER for xx in range(x0, x)):
                    bottom = yy
            if bottom - y + 1 >= 8:
                return (x0, y, x - x0, bottom - y + 1)
    return None


def widget_frame(image, size):
    """The widget's complete frame, or None with the reason it is not whole. Its border colour (59,65,83)
    draws the top edge, both side columns and the bottom edge; a top edge wider than the widget can be is
    the seats panel's, and a frame something paints over loses its sides, so neither reads as the widget."""
    width, height = size
    pixels = image.load()
    limit = min(BOX_TOP + BOX_HEIGHT + 22, height)
    for y in range(limit):
        x = 0
        while x < width:
            if pixels[x, y] != WIDGET_BORDER:
                x += 1
                continue
            x0 = x
            while x < width and pixels[x, y] == WIDGET_BORDER:
                x += 1
            span = x - x0
            if span < 60 or span > WIDGET_MAX_WIDTH:
                continue
            for bottom in range(y + 8, limit):
                if all(pixels[px, bottom] == WIDGET_BORDER for px in range(x0, x0 + span)):
                    broken = [py for py in range(y + 1, bottom)
                              if pixels[x0, py] != WIDGET_BORDER or pixels[x0 + span - 1, py] != WIDGET_BORDER]
                    if broken:
                        return {"rect": None, "reason": f"frame at x{x0} y{y} w{span}: side rows painted over {broken}"}
                    return {"rect": (x0, y, span, bottom - y + 1), "reason": "whole"}
            return {"rect": None, "reason": f"frame at x{x0} y{y} w{span}: no bottom edge above row {limit}"}
    return {"rect": None, "reason": "no widget-sized top edge"}


def in_free_zone(box, size):
    """The measured free zone: below 480 rows the single-line strip between the funds block and the
    controller icon, at or above it the top-right box at its design position."""
    width, height = size
    if box is None:
        return False
    if height < COMPACT_MAX_HEIGHT:
        return box[1] <= STRIP_TOP + 1 and box[3] <= 20 and box[0] >= STRIP_LEFT \
            and box[0] + box[2] <= width - STRIP_RIGHT_MARGIN
    return tuple(box) == (width - BOX_WIDTH - BOX_MARGIN, BOX_TOP, BOX_WIDTH, BOX_HEIGHT)


def interior_ink(image, rect):
    """Pixels inside the widget's frame that are not its fill: its text must be drawn in it,
    not somewhere else on the screen. Skips the border and the accent line under it."""
    pixels = image.load()
    x, y, width, height = rect
    return sum(pixels[px, py] != WIDGET_FILL
               for py in range(y + 3, y + height - 1)
               for px in range(x + 2, x + width - 2))


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True)
    return result.stdout


def pin(repo):
    return {"head": git(repo, "rev-parse", "HEAD").decode().strip(),
            "diff_sha256": hashlib.sha256(git(repo, "diff", "HEAD", "--")).hexdigest(),
            "exe_sha256": sha256(repo / "Cortex Command.exe")}


def require_pin(repo, expected):
    actual = pin(repo)
    if actual != expected:
        raise RuntimeError(f"source or executable changed: {actual}")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def read_log(out):
    return "\n".join(path.read_text(encoding="utf-8", errors="replace")
                     for path in (out / "stdout.log", out / "stderr.log") if path.exists())


def set_settings(runtime, values):
    path = runtime / "Userdata" / "Settings.ini"
    settings = path.read_text(encoding="utf-8")
    for name, value in values.items():
        settings, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*",
                                  lambda match: match[1] + str(value), settings)
        if count == 0:
            # A missing property must land at the SettingsMan top level, not inside
            # the trailing Player4Scheme child object, so it anchors on ResolutionY.
            settings, anchored = re.subn(r"(?m)^(\s*ResolutionY\s*=\s*[^\r\n]*)",
                                         lambda match: match[1] + f"\n\t{name} = {value}", settings, count=1)
            if anchored != 1:
                raise RuntimeError(f"no SettingsMan anchor for private {name} setting")
        elif count != 1:
            raise RuntimeError(f"expected at most one private {name} setting, found {count}")
    path.write_text(settings, encoding="utf-8")


def menu_script(who, port, name=None):
    text = f"wait 40\nactivate ButtonMainToMultiplayer\nwait 12\nsettext TextMultiplayerName {name or who}\n"
    if who == "Host":
        text += ("activate ButtonMultiplayerHostGame\nwait 10\n"
                 f"settext TextHostPort {port}\nsettext TextHostPlayers 2\n"
                 "activate ButtonMultiplayerCreate\nwait_connected 2\nwait_remote_ready\n"
                 "wait_all_ready\nactivate ButtonMultiplayerStart\n")
    else:
        text += ("activate ButtonMultiplayerJoinGame\nwait 10\nsettext TextJoinAddress 127.0.0.1\n"
                 f"settext TextJoinPort {port}\nactivate ButtonMultiplayerConnect\nwait_connected 2\n"
                 "activate ButtonMultiplayerReady\nwait_remote_ready\n")
    return text + "wait 99999\n"


def label_assert(control, text=None, visible=True):
    step = {"op": "assert_control", "control": control, "equals": {"visible": visible}, "fits": True}
    if text is not None:
        step["text_contains"] = text
    return step


def text_assert(control, text):
    """The text alone. A banner that was never pushed leaves its row empty, and the probe reads
    equals before text_contains, so this is what names the missing content in the failure."""
    return {"op": "assert_control", "control": control, "equals": {}, "text_contains": text}


def probe_script(who, size, arm, mode):
    width, height = size
    compact = height < COMPACT_MAX_HEIGHT
    guest = LONG_GUEST_NAME if arm.get("long_name") else "Guest"
    hidden = {"op": "assert_control", "control": STATUS_BOX, "equals": {"visible": False}, "fits": True}
    shown = {"op": "assert_control", "control": STATUS_BOX, "equals": {"visible": True}, "fits": True}
    def wanted(trigger):
        # The widget at a probe point: Always paints it, Off never does, Auto only
        # while a trigger or its three-second recovery linger is up.
        return mode == "always" or (mode == "auto" and trigger)
    def widget(trigger):
        return shown if wanted(trigger) else hidden
    def shot(name, visible=None):
        # The capture oracle reads its expectation back out of the script; the engine ignores the key.
        step = {"op": "screenshot", "name": name}
        if visible is not None:
            step["widget"] = visible
        return step
    status_reads = [label_assert(STATUS, "NET [F6]" if compact else "NET STATUS"),
                    label_assert(STATUS, "D 3" if compact else "D 3 ticks / 50.0 ms"),
                    label_assert(STATUS, "RTT "),
                    label_assert(STATUS, "PACE ")]
    steps = [{"op": "wait", "service": "Running"}, {"op": "wait", "sim_at_least": 60}]
    steps.append(widget(False))
    if mode == "always":
        steps += status_reads + [label_assert(STATUS, "LIVE")]
    if arm.get("event"):
        steps += [{"op": "wait", "sim_at_least": 240}]
        if who == "Host":
            steps += [{"op": "key_down", "key": "P", "sim_at": 240}, {"op": "key_up", "key": "P", "sim_at": 241}]
        steps += [
            {"op": "wait", "sim_at_least": 270},
            label_assert(TOAST, PAUSED),
            widget(True),
        ]
        if mode != "off":
            steps += [label_assert(STATUS, "PAUSED")]
        steps += [shot(f"widget-paused-{who.lower()}", wanted(True))]
        steps += [
            {"op": "wait", "sim_at_least": 430},
            {"op": "assert_control", "control": TOAST, "equals": {"visible": False, "text": ""}},
            {"op": "wait", "sim_at_least": 480},
        ]
        if who == "Guest":
            steps += [{"op": "key_down", "key": "P", "sim_at": 480}, {"op": "key_up", "key": "P", "sim_at": 481}]
        # The resume lands near 663; Auto lingers three seconds, so the widget is
        # still up at 680 and long gone by 880.
        steps += [{"op": "wait", "sim_at_least": 680}, label_assert(TOAST, RESUMED), widget(True)]
        if mode != "off":
            steps += [label_assert(STATUS, "LIVE")]
    if arm.get("f6"):
        if who == "Host":
            steps += [
                {"op": "wait", "sim_at_least": 200},
                {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                {"op": "wait", "panel_open": True},
                widget(True),
                {"op": "assert_control", "control": "NetworkSeatsTitle", "equals": {"visible": True}},
                # The panel's own rectangle is what the capture is measured against.
                {"op": "assert_control", "control": PANEL, "equals": {"visible": True}, "fits": True},
                shot("widget-f6-panel-host", wanted(True)),
                {"op": "key_down", "key": "F6"}, {"op": "key_up", "key": "F6"},
                {"op": "wait", "panel_open": False},
            ]
    if arm.get("stall"):
        if who == "Host":
            # Two seconds into the guest's 8 s sleep, whatever the mode had up before it. A visibility
            # wait cannot mark the stall: Always already has the widget up, so it would never wait.
            steps += [{"op": "wait", "sim_at_least": 290}, {"op": "wait", "elapsed_ms": 2000}]
            if mode == "off":
                steps += [hidden, shot("widget-stall-off-host", False)]
            else:
                # The stall UI pump keeps drawing and polling while the guest sleeps.
                steps += [
                    shown,
                    label_assert(STATUS, "WAITING FOR FRAMES"),
                    shot("widget-stalled-host", True),
                ]
    if arm.get("leave"):
        if who == "Host":
            # The driver drops the guest on this signal, so the host is always past 290 when its link dies.
            steps += [{"op": "wait", "sim_at_least": 290}, {"op": "signal", "name": DROP_SIGNAL}]
            # The drop notice and the wait banner are toasts, so every mode owes the player both: who
            # left, and how long the match waits for them. The hold engages with the drop, and the
            # banner rides the next stall pump, so the two rows are up together.
            steps += [
                {"op": "wait", "control": TOAST, "equals": {"visible": True}},
                label_assert(TOAST_FIRST, f"{guest} dropped"),
                {"op": "wait", "renders": 2},
                # Whatever the name's length, the banner owes the player the peer and the reclaim window.
                text_assert(TOAST_SECOND, f"Match paused: waiting for {guest} to return ("),
                text_assert(TOAST_SECOND, "s left)"),
                label_assert(TOAST_SECOND, f"Match paused: waiting for {guest} to return"),
                # The hold engages seconds after the drop, so the widget's own state is read later.
                shot(f"toast-hold-banner-{mode}-host", False if mode == "off" else None),
            ]
            if mode == "off":
                # The banners carry the whole recovery story in Off; the widget stays away.
                steps += [hidden, shot("widget-hold-off-host", False)]
            else:
                steps += [
                    {"op": "wait", "control": STATUS, "equals": {"visible": True}},
                    # The seat hold engages after the transport's give-up, seconds after the drop.
                    {"op": "wait", "elapsed_ms": 6000},
                    shown,
                    label_assert(STATUS, "WAITING FOR" if compact else "Waiting for"),
                    shot("widget-hold-host", True),
                ]
        else:
            # The guest is killed mid-match, so its own probe has to be done before the drop.
            steps += [{"op": "wait", "sim_at_least": 250}, widget(False)]
        steps.append({"op": "finish"})
        return {"schema": 1, "timeout_ms": 180000, "steps": steps}
    steps += [{"op": "wait", "sim_at_least": 880}]
    # At 880 only Always is up: the stall clears near its own tick, so Auto's three seconds are long spent.
    steps.append(widget(False))
    if mode == "always":
        steps += status_reads + [label_assert(STATUS, "LIVE")]
    steps += [{"op": "assert_control", "control": TOAST, "equals": {"visible": False, "text": ""}},
              {"op": "finish"}]
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def run_pair(repo, root, port, size, arm, mode, timeout, expected_pin):
    require_pin(repo, expected_pin)
    root.mkdir(parents=True, exist_ok=False)
    runs, records = {}, {}
    try:
        for who in ("Host", "Guest"):
            inputs = root / f"{who}_inputs"
            inputs.mkdir()
            menu = inputs / "menu.txt"
            name = LONG_GUEST_NAME if arm.get("long_name") and who == "Guest" else who
            menu.write_text(menu_script(who, port, name), encoding="utf-8")
            probe = inputs / "probe.json"
            probe.write_text(json.dumps(probe_script(who, size, arm, mode), indent=2), encoding="utf-8")
            flags = ["-menu-script", menu, "-num-lua-states", 4, "-tick-hashes", "-max-ticks", TICKS,
                     "-net-match-ticks", TICKS, "-out", root / f"{who}_trace.json",
                     "-net-match-report", root / f"{who}_report.json"]
            if arm.get("captures"):
                flags += ["-net-match-screenshot-ticks", ",".join(map(str, CAPTURE_TICKS))]
            if arm.get("stall") and who == "Guest":
                flags += ["-net-match-e2e-stall"]
            runs[who] = make_run(repo, flags, root / who, timeout,
                                 env={"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)})
            values = {"ResolutionX": size[0], "ResolutionY": size[1],
                      "NetworkMatchStatusMode": MODE_INI[mode],
                      # The delay box is read-only under the auto policy; the floor is a setting.
                      "NetworkInputDelayFrames": 3}
            set_settings(runs[who].cwd, values)

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        def drop_guest():
            # A killed peer is what holds the host's seat; a clean quit just ends the match.
            signal = root / "Host_inputs" / f"{DROP_SIGNAL}.json"
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if signal.exists():
                    break
                threading.Event().wait(0.2)
            try:
                runs["Guest"].terminate(reason="scripted mid-match drop")
            except RuntimeError:
                pass

        host = threading.Thread(target=drive, args=("Host",), daemon=True)
        guest = threading.Thread(target=drive, args=("Guest",), daemon=True)
        host.start()
        threading.Event().wait(2.0)
        guest.start()
        dropper = threading.Thread(target=drop_guest, daemon=True) if arm.get("leave") else None
        if dropper:
            dropper.start()
        host.join()
        if arm.get("leave"):
            try:
                runs["Guest"].terminate(reason="scripted mid-match drop")
            except RuntimeError:
                pass
        guest.join()
    finally:
        for run in runs.values():
            run.close()
    require_pin(repo, expected_pin)
    return records


def probe_observations(probe):
    steps = probe.get("script", {}).get("steps", [])
    for result in probe.get("steps", []):
        index = result.get("index", -1)
        if 0 <= index < len(steps):
            yield steps[index], result["observed"]


def tick_expected_visible(arm, mode, tick):
    if mode == "always":
        return True
    if mode == "off":
        return False
    # Auto: the pause covers 270 and 500, the post-resume linger still shows at 680.
    return bool(arm.get("event")) and tick in (270, 500, 680)


def image_oracle(path, size, arm, mode, name, tick):
    from PIL import Image

    with Image.open(path) as source:
        image = source.convert("RGB")
    if image.size != tuple(size):
        return {"pass": False, "actual_size": list(image.size), "expected_size": list(size)}
    width, height = size
    pixels = image.load()
    box = find_widget_paint(image, size)
    expected_visible = tick_expected_visible(arm, mode, tick)
    result = {"path": str(path), "sha256": sha256(path), "size": list(image.size), "mode": mode,
              "arm": name, "tick": tick, "expected_visible": expected_visible,
              "widget_paint": box, "pass": True}
    if expected_visible:
        result["occluding"] = occluding_rects(box, size) if box else ["widget_missing"]
        if height < COMPACT_MAX_HEIGHT:
            result["expected_zone"] = [STRIP_LEFT, 0, width - STRIP_LEFT - STRIP_RIGHT_MARGIN, 20]
        else:
            result["expected_rect"] = [width - BOX_WIDTH - BOX_MARGIN, BOX_TOP, BOX_WIDTH, BOX_HEIGHT]
        result["pass"] = in_free_zone(box, size) and not result["occluding"]
        result["interior_ink"] = interior_ink(image, box) if box else 0
        result["pass"] = result["pass"] and result["interior_ink"] > 0
        result["checks"] = {"paint_found": box is not None, "occluding": result["occluding"],
                            "text_in_widget": result["interior_ink"] > 0,
                            "compact_strip": height >= COMPACT_MAX_HEIGHT or result["pass"]}
    else:
        result["pass"] = box is None
        result["checks"] = {"paint_absent": box is None}
    if arm.get("event") and tick == 270:
        toast_width = min(520, width - 32)
        left, top = (width - toast_width) // 2, height - 28
        background = all(pixels[px, top] == WIDGET_FILL for px in range(left, left + toast_width))
        toast_ink = sum(pixels[px, py] != WIDGET_FILL for py in range(top + 4, top + 16)
                        for px in range(left + 8, left + toast_width - 8))
        toast_rect = (left, top, toast_width, 18)
        result.update(toast_background=background, toast_ink=toast_ink,
                      toast_occluding=occluding_rects(toast_rect, size))
        result["pass"] = result["pass"] and background and toast_ink > 0 and not result["toast_occluding"]
    return result


def probe_shot_oracle(path, size, expected_visible, panel_rect):
    """A probe screenshot judged on its pixels, not on a control's visible flag: the widget's frame whole,
    its text drawn inside it, in the free zone, clear of the HUD and - where the screen has the rows for
    both - of the seats panel. A visible flag on a widget the panel paints over is what this catches."""
    from PIL import Image

    with Image.open(path) as source:
        image = source.convert("RGB")
    if image.size != tuple(size):
        return {"pass": False, "path": str(path), "actual_size": list(image.size), "expected_size": list(size)}
    found = widget_frame(image, size)
    box = found["rect"]
    result = {"path": str(path), "sha256": sha256(path), "size": list(image.size),
              "expected_visible": expected_visible, "widget_paint": list(box) if box else None,
              "frame": found["reason"], "panel_rect": list(panel_rect) if panel_rect else None, "pass": True}
    if expected_visible is None:
        result["checks"] = {}
        return result
    if not expected_visible:
        result["pass"] = box is None
        result["checks"] = {"paint_absent": box is None}
        return result
    result["occluding"] = occluding_rects(box, size) if box else ["widget_missing"]
    result["interior_ink"] = interior_ink(image, box) if box else 0
    # A short screen has no rows for both, so the strip draws over the panel; a tall one must clear it.
    panel_clear = box is None or panel_rect is None or size[1] < COMPACT_MAX_HEIGHT \
        or not rects_intersect(box, panel_rect)
    result["checks"] = {"paint_found": box is not None, "occluding": result["occluding"],
                        "text_in_widget": result["interior_ink"] > 0,
                        "in_free_zone": in_free_zone(box, size), "panel_clear": panel_clear}
    result["pass"] = box is not None and not result["occluding"] and result["interior_ink"] > 0 \
        and in_free_zone(box, size) and panel_clear
    return result


def inspect_pair(root, records, size, arm, mode, name):
    checks, details = {}, {"records": records, "captures": {}, "shots": {}, "probes": {}, "events": {},
                         "occlusion": {}, "mode": mode, "arm": name}
    leaving = bool(arm.get("leave"))
    for who in ("Host", "Guest"):
        record = records.get(who, {})
        log = read_log(root / who)
        if leaving and who == "Guest":
            # Only the driver's own kill proves the scripted drop; a self-crash exits nonzero too.
            checks[f"{who}_exit"] = (record.get("injected_termination") is not None
                                     and record.get("exit_code") not in (0, None))
        elif leaving:
            # The dropped peer never comes back, so the host ends on the lost link instead of its tick budget.
            checks[f"{who}_exit"] = not record.get("timed_out", True) and "lockstep wait: PeerLeft" in log
        else:
            checks[f"{who}_exit"] = record.get("exit_code") == 0 and not record.get("timed_out", True)
        checks[f"{who}_evidence"] = record.get("evidence_complete") is True
        checks[f"{who}_menu_match"] = "[menu-mp] launching the match" in log
        checks[f"{who}_no_menu_failure"] = "[menu-script] FAILED" not in log
        probe_path = root / f"{who}_inputs" / "net-ui-result.json"
        probe = read_json(probe_path) if probe_path.exists() else {}
        details["probes"][who] = {"path": str(probe_path), "error": probe.get("error"),
                                  "failed_step": probe.get("failed_step")}
        checks[f"{who}_probe_complete"] = probe.get("pass") is True and probe.get("complete") is True
        observations = list(probe_observations(probe))
        status_texts = [obs["control"]["text"] for step, obs in observations
                        if step.get("control") == STATUS and obs.get("control", {}).get("visible")]
        widget_rects = [tuple(obs["control"]["rect"]) for step, obs in observations
                        if step.get("control") == STATUS_BOX and obs.get("control", {}).get("visible")]
        toast_rects = [tuple(obs["control"]["rect"]) for step, obs in observations
                       if step.get("control") == TOAST and obs.get("control", {}).get("visible")]
        # The label carries the text and is placed on its own, so its rectangle is read too.
        status_rects = [tuple(obs["control"]["rect"]) for step, obs in observations
                        if step.get("control") == STATUS and obs.get("control", {}).get("visible")]
        details["occlusion"][who] = {"widget_rects": widget_rects, "toast_rects": toast_rects,
                                     "status_rects": status_rects}
        checks[f"{who}_widget_rects_clear"] = all(not occluding_rects(rect, size) for rect in widget_rects)
        checks[f"{who}_status_rects_clear"] = all(not occluding_rects(rect, size) for rect in status_rects)
        checks[f"{who}_toast_rects_clear"] = all(not occluding_rects(rect, size) for rect in toast_rects)
        if mode == "off":
            checks[f"{who}_widget_never_painted"] = not widget_rects
        if mode == "always":
            checks[f"{who}_widget_seen"] = bool(widget_rects)
        # Only the host reads the widget through a stall or a hold; in Auto its peer never sees one.
        expect_status = mode == "always" or (mode == "auto" and (arm.get("event") or ((arm.get("stall") or arm.get("leave")) and who == "Host")))
        if expect_status:
            waiting_for = [text for text in status_texts if re.search(r"waiting for", text, re.I)]
            running = [text for text in status_texts if not re.search(r"waiting for", text, re.I)]
            # The compact strip gives up its metrics tail before it shortens a name, so a
            # wire-max guest name's wait banner legitimately carries no RTT/PACE there.
            # "WAITING FOR FRAMES" still fits whole, so it keeps its metrics asserted.
            metrics_dropped = arm.get("long_name") and size[1] < COMPACT_MAX_HEIGHT
            named_waits = [text for text in waiting_for if "FRAMES" not in text]
            metric_texts = running + [text for text in waiting_for
                                      if not metrics_dropped or "FRAMES" in text]
            # A held seat has no peer left to measure, so its read carries the dash instead of a number.
            checks[f"{who}_live_rtt"] = bool(status_texts) and all(
                re.search(r"(?:^|\n| )RTT (?:\d+|--) ms", text) if re.search(r"waiting for", text, re.I)
                else re.search(r"(?:^|\n| )RTT \d+ ms", text) for text in metric_texts)
            checks[f"{who}_pace_field"] = bool(status_texts) and all(re.search(r"PACE \d+(?:\.\d+)? tps", text) for text in metric_texts)
            if metrics_dropped:
                # What the fallback keeps is still asserted: the name (whole or its
                # ellipsis) and the line's own count; the metrics are asserted gone.
                # The hold banner is the host's - the dropped peer's lines carry no
                # wait, so the non-empty requirement is the host's alone.
                must_hold = who == "Host"
                checks[f"{who}_fallback_name_kept"] = (not must_hold or bool(named_waits)) and all(
                    LONG_GUEST_NAME in text or "..." in text for text in named_waits)
                checks[f"{who}_fallback_count_kept"] = all(
                    re.search(r"\(\d+ s\)|\d+ of \d+", text) for text in named_waits)
                checks[f"{who}_fallback_metrics_dropped"] = all(
                    "RTT" not in text and "PACE" not in text for text in named_waits)
            # An arm that read no status text at all passed both pace checks vacuously, so the guard stays.
            # A stalled or held Host has no peer left to pace against, so only it may read waiting-for alone.
            held = who == "Host" and bool(arm.get("stall") or arm.get("leave"))
            checks[f"{who}_measured_pace"] = bool(status_texts) and (bool(running) or held) and all(
                re.search(r"PACE (?:[1-9]\d*(?:\.\d+)?|0\.[1-9]\d*) tps", text) for text in running)
        capture_lines = [(int(match[1]), match[2], match[3] == "1") for line in log.splitlines()
                         if (match := CAPTURE.match(line))]
        shots = sorted((root / who / "runtime" / "ScreenShots").glob("net_match_tick_*.png"))
        details["captures"][who] = {"queued": capture_lines, "images": []}
        expected_ticks = list(CAPTURE_TICKS) if arm.get("captures") else []
        checks[f"{who}_capture_ticks"] = [tick for tick, _, ok in capture_lines if ok] == expected_ticks
        checks[f"{who}_capture_count"] = len(shots) == len(expected_ticks)
        for tick in expected_ticks:
            matching = [path for path in shots if path.name.startswith(f"net_match_tick_{tick}_round_")]
            image_result = image_oracle(matching[0], size, arm, mode, name, tick) if len(matching) == 1 else {"pass": False, "reason": "missing or duplicate capture", "tick": tick}
            details["captures"][who]["images"].append(image_result)
            checks[f"{who}_pixels_{tick}"] = image_result["pass"]
        # Every probe screenshot is judged on its pixels too, against the seats panel when one is open.
        panel_rects = [tuple(obs["control"]["rect"]) for step, obs in observations
                       if step.get("control") == PANEL and obs.get("control", {}).get("visible")]
        details["shots"][who] = []
        for step, obs in observations:
            if step.get("op") != "screenshot":
                continue
            path = Path(obs["screenshot"]) if "screenshot" in obs else None
            shot_result = probe_shot_oracle(path, size, step.get("widget"), panel_rects[-1] if panel_rects else None) \
                if path and path.exists() else {"pass": False, "reason": "missing probe capture", "name": step["name"]}
            details["shots"][who].append(shot_result)
            checks[f"{who}_shot_{step['name']}"] = shot_result["pass"]
        if arm.get("event"):
            pauses = [int(match[1]) for line in log.splitlines() if (match := PAUSE_EVENT.match(line))]
            resumes = [int(match[1]) for line in log.splitlines() if (match := RESUME_EVENT.match(line))]
            toast_reads = [obs for step, obs in observations if step.get("control") == TOAST and "control" in obs]
            paused_reads = [obs for obs in toast_reads if obs["control"].get("text") == PAUSED and obs["control"].get("visible")]
            resumed_reads = [obs for obs in toast_reads if obs["control"].get("text") == RESUMED and obs["control"].get("visible")]
            expired_reads = [obs for obs in toast_reads if obs["control"].get("text") == "" and not obs["control"].get("visible")]
            presses = [(step["op"], obs["sim_frame"]) for step, obs in observations
                       if step.get("key") == "P" and step["op"] in ("key_down", "key_up")]
            checks[f"{who}_scripted_P_press"] = [op for op, _ in presses] == ["key_down", "key_up"]
            checks[f"{who}_pause_toast_within_30"] = len(pauses) == 1 and bool(paused_reads) and 0 <= paused_reads[0]["sim_frame"] - pauses[0] <= 30
            checks[f"{who}_pause_toast_gone_within_200"] = len(pauses) == 1 and bool(expired_reads) and 0 < expired_reads[0]["sim_frame"] - pauses[0] <= 200
            checks[f"{who}_resume_toast_within_30"] = len(resumes) == 1 and bool(resumed_reads) and 0 <= resumed_reads[0]["sim_frame"] - resumes[0] <= 30
            details["events"][who] = {"pause_ticks": pauses, "resume_ticks": resumes,
                                      "scripted_P_ticks": presses,
                                      "toast_reads": toast_reads,
                                      "lines": [{"line": index, "text": line} for index, line in enumerate(log.splitlines(), 1)
                                                if PAUSE_EVENT.match(line) or RESUME_EVENT.match(line)]}
        if arm.get("stall"):
            waiting_reads = [obs for step, obs in observations
                             if step.get("control") == STATUS and "WAITING FOR FRAMES" in obs.get("control", {}).get("text", "")]
            details["events"].setdefault(who, {})["stall_reads"] = waiting_reads
            if who == "Host":
                if mode == "off":
                    checks["Host_stall_widget_absent"] = not waiting_reads and not widget_rects
                else:
                    checks["Host_stall_widget_seen"] = bool(waiting_reads)
        if leaving:
            hold_reads = [obs for step, obs in observations
                          if step.get("control") == STATUS and obs.get("control", {}).get("visible")
                          and ("WAITING FOR" in obs["control"].get("text", "") or "Waiting for" in obs["control"].get("text", ""))]
            toast_reads = [obs for step, obs in observations
                           if step.get("control") in (TOAST, TOAST_FIRST, TOAST_SECOND) and "control" in obs]
            hold_toast_reads = [obs for obs in toast_reads if "dropped" in obs["control"].get("text", "")]
            banner_reads = [obs for obs in toast_reads
                            if obs["control"].get("visible") and HOLD_BANNER.search(obs["control"].get("text", ""))]
            resumed_toast_reads = [obs for obs in toast_reads if obs["control"].get("text", "").startswith("Match resumed")]
            # The stack itself carries the order: the drop notice stays the older row under the banner.
            row_reads = {control: [obs for step, obs in observations
                                   if step.get("control") == control and "control" in obs]
                         for control in (TOAST_FIRST, TOAST_SECOND)}
            banner_lines = [{"line": index, "text": line} for index, line in enumerate(log.splitlines(), 1)
                            if HOLD_BANNER_LINE.match(line)]
            details["events"].setdefault(who, {})["hold_reads"] = hold_reads
            details["events"][who]["hold_toast_reads"] = hold_toast_reads
            details["events"][who]["hold_banner_reads"] = banner_reads
            details["events"][who]["hold_banner_lines"] = banner_lines
            if who == "Host":
                # Whatever the widget mode: the drop notice, then the wait banner naming the player and
                # the reclaim window, then the round's own ending once nobody reclaims the seat.
                checks["Host_hold_toast_seen"] = bool(hold_toast_reads)
                checks["Host_hold_banner_seen"] = bool(banner_reads)
                checks["Host_hold_banner_after_drop"] = bool(row_reads[TOAST_FIRST]) and bool(row_reads[TOAST_SECOND]) \
                    and all("dropped" in obs["control"].get("text", "") for obs in row_reads[TOAST_FIRST]) \
                    and all(HOLD_BANNER.search(obs["control"].get("text", "")) for obs in row_reads[TOAST_SECOND])
                checks["Host_hold_banner_line"] = len(banner_lines) == 1
                if arm.get("long_name"):
                    # The engine carried the whole 64-byte name, so what the rows show is a layout choice.
                    checks["Host_long_name_delivered"] = any(HOLD_BANNER_LINE.match(entry["text"])[1] == LONG_GUEST_NAME
                                                             for entry in banner_lines)
                checks["Host_hold_recovery_or_end"] = bool(resumed_toast_reads) or "lockstep wait: PeerLeft" in log
                if mode == "off":
                    checks["Host_hold_widget_absent"] = not hold_reads and not widget_rects
                else:
                    checks["Host_hold_widget_seen"] = bool(hold_reads)
        if arm.get("f6") and who == "Host":
            panel_reads = [obs.get("panel_open") for step, obs in observations if "panel_open" in obs]
            checks["Host_f6_panel_seen"] = any(panel_reads)
    if not leaving:
        ok, compared = strict_compare(root / "Host_trace.json", root / "Guest_trace.json", expected_ticks=TICKS)
        checks["complete_peer_hashes"] = ok
        if arm.get("event"):
            checks["paused_ticks_covered"] = ok and compared["paused_ticks"] > 0
        details["peer_hashes"] = compared
    return {"pass": all(checks.values()), "checks": checks, "details": details}


def compare_capture_switch(on, off, who, paused):
    first, second = on / f"{who}_trace.json", off / f"{who}_trace.json"
    ok, detail = strict_compare(first, second, expected_ticks=TICKS)
    if ok:
        a, b = read_json(first), read_json(second)
        detail["all_recorded_hashes_identical"] = a["runs"][0]["tick_hashes"] == b["runs"][0]["tick_hashes"]
        ok = detail["all_recorded_hashes_identical"]
    if paused:
        presses = []
        for root in (on, off):
            probe = read_json(root / f"{who}_inputs" / "net-ui-result.json")
            presses.append([(step["op"], obs["sim_frame"]) for step, obs in probe_observations(probe)
                            if step.get("key") == "P" and step["op"] in ("key_down", "key_up")])
        detail["scripted_P_ticks"] = presses
        detail["same_scripted_P_ticks"] = presses[0] == presses[1] and len(presses[0]) == 2
        detail["paused_path_covered"] = detail["paused_ticks"] > 0
        ok = ok and detail["same_scripted_P_ticks"] and detail["paused_path_covered"]
    return {"pass": ok, "detail": detail}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--exe-sha256", required=True)
    parser.add_argument("--timeout", type=int, default=600)
    # A second lane runs the same matrix from its own scratch root and port range.
    parser.add_argument("--scratch", type=Path, default=SCRATCH)
    parser.add_argument("--port-base", type=int, default=PORT_BASE)
    parser.add_argument("--sizes", default="640x360,960x540", help="viewports to run, in order")
    # A selection runs one arm against one build; the full matrix is still the default.
    parser.add_argument("--modes", default=",".join(MODES), help="status modes to run, in order")
    parser.add_argument("--arms", default=",".join(ARMS), help="arms to run, in order")
    options = parser.parse_args()
    if Path("D:/mx/LEAD_FAMILY.lock").exists():
        parser.error("verification family owns the machine; no driver may start")
    repo, root = options.repo.resolve(), options.out.resolve()
    scratch = options.scratch.resolve()
    if not root.is_relative_to(scratch) or root == scratch:
        parser.error(f"--out must name a fresh run beneath {scratch}")
    global make_run, strict_compare
    sys.path.insert(0, str(repo / "tools"))
    from compare_sim_traces import strict_compare
    from run_sim_test import make_run
    os.environ["CCCP_HEADLESS"] = "1"
    root.mkdir(parents=True, exist_ok=False)
    ports = (options.port_base + index % PORT_COUNT for index in itertools.count())
    result = {"pass": False, "checks": {}, "pairs": {}, "capture_switch": {},
              "ticks_per_run": TICKS, "ports": list(range(options.port_base, options.port_base + PORT_COUNT)),
              "sizes": options.sizes,
              "driver_sha256": sha256(__file__),
              "peer_comparator_sha256": sha256(repo / "tools" / "compare_sim_traces.py"),
              "peer_hash_scope": "unchanged strict_compare: controller excluded; every other subsystem at every tick",
              "capture_switch_hash_scope": "every recorded hash, including controller and total"}
    try:
        result["pin_before"] = pin(repo)
        if result["pin_before"]["exe_sha256"].lower() != options.exe_sha256.lower():
            raise RuntimeError("the requested executable hash does not match")
        sizes = [tuple(int(part) for part in entry.split("x")) for entry in options.sizes.split(",")]
        modes = [entry for entry in options.modes.split(",") if entry]
        arm_names = [entry for entry in options.arms.split(",") if entry]
        unknown = [name for name in modes if name not in MODES] + [name for name in arm_names if name not in ARMS]
        if unknown:
            raise RuntimeError(f"unknown mode or arm: {unknown}")
        # The capture switch compares two arms of the same pair, so it only runs when both are selected.
        switches = [(prefix, paused) for prefix, paused in (("", False), ("pause_", True))
                    if "auto" in modes and f"{prefix}on" in arm_names and f"{prefix}off" in arm_names]
        expected_checks = len(sizes) * (len(modes) * len(arm_names) + 2 * len(switches))
        result["selection"] = {"sizes": options.sizes, "modes": modes, "arms": arm_names,
                               "expected_checks": expected_checks}
        for size in sizes:
            tag = f"{size[0]}x{size[1]}"
            for mode in modes:
                for arm_name in arm_names:
                    name = f"{tag}_{mode}_{arm_name}"
                    pair_root = root / name
                    records = run_pair(repo, pair_root, next(ports), size, ARMS[arm_name], mode, options.timeout, result["pin_before"])
                    pair = inspect_pair(pair_root, records, size, ARMS[arm_name], mode, name)
                    result["pairs"][name] = pair
                    result["checks"][name] = pair["pass"]
                    (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
                    if not pair["pass"]:
                        raise RuntimeError(f"{name}: " + ", ".join(key for key, passed in pair["checks"].items() if not passed))
            for prefix, paused in switches:
                for who in ("Host", "Guest"):
                    comparison = compare_capture_switch(root / f"{tag}_auto_{prefix}on", root / f"{tag}_auto_{prefix}off", who, paused)
                    result["capture_switch"][f"{tag}_{prefix}{who}_capture_switch"] = comparison
                    result["checks"][f"{tag}_{prefix}{who}_capture_switch"] = comparison["pass"]
        result["pass"] = all(result["checks"].values()) and len(result["checks"]) == expected_checks
    except Exception as error:
        result["error"] = str(error)
    finally:
        result["pin_after"] = pin(repo)
        if result.get("pin_before") != result["pin_after"]:
            result["pass"] = False
            result["error"] = "source or executable changed during the driver"
        (root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"pass": result["pass"], "error": result.get("error"), "out": str(root),
                      "exe_sha256": result["pin_after"]["exe_sha256"]}), flush=True)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
