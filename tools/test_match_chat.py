"""In-match chat band: host send appears on the guest, and the band overlaps no occupier.

Writes the network-UI probe scripts the completion pass runs. Does not launch unless --launch
is set. Viewports 640x360, 960x540 and 1280x720, each at both chat text sizes. The steps measure
the running match they launch, which is past its setup editor by sim 60, so every layout step reads
in match mode. The host send_chat step is the line; the guest waits for LabelMatchChatNewest within
120 renders; both put a seat message on screen and open the seats panel, so the layout assertion
names the two occupiers that are
really there, then open the entry with the chat key and assert what the carved run holds at that
size: a history row above the entry at every supported size and text size, 640x360 with Large
included, where the entry shrinks and then the panel gives up rows to make room for it (see
history_row_fits).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import threading
from pathlib import Path

SIZES = ((640, 360), (960, 540), (1280, 720))
TEXT_SIZES = ("small", "large")
# The row rule holds at every size the game supports; the detector launches the three shortest of them.
SUPPORTED_HEIGHTS = (360, 540, 720, 1080)
CHAT_LINE = "hello from host"
SEAT_MESSAGE = "chat layout occupier"
TICKS = 900
# This detector owns 48700-48711 (two peers per size and text size). The launch driver owns
# 48320-48539 and 48630-48649, the menu readback 48270-48279/48380-48399/48530-48559/48840-48859/
# 49180-49199, the autosave restore 48720-48739.
PORT_BASE = 48700

# The layout the band is held to, as NetModerationGUI lays it out. The fonts are the two skin fonts
# (Base.rte/GUIs/Skins/FontSmall.png is 10 rows tall, FontLarge.png 15), and a row is floored at 12
# rows however short the font is.
SMALL_FONT_HEIGHT, LARGE_FONT_HEIGHT = 10, 15
LINE_HEIGHT = {"small": max(12, SMALL_FONT_HEIGHT) + 4, "large": max(12, LARGE_FONT_HEIGHT) + 4}
PANEL_GAP, PANEL_HEIGHT, COMPACT_MAX_HEIGHT = 4, 344, 480
STRIP_BAND_BOTTOM, STATUS_BOX_TOP, STATUS_BOX_HEIGHT = 20, 32, 76
PANEL_ROW_HEIGHT = max(12, SMALL_FONT_HEIGHT) + 8
# The seat message these steps put on screen: the non-centered band the large font lays out at y=12.
MESSAGE_TOP, MESSAGE_HEIGHT = 12, LARGE_FONT_HEIGHT


def chat_top_limit(height):
    """The first row the band may use: under the seat's message where the message is in the top half."""
    return MESSAGE_TOP + MESSAGE_HEIGHT + 4 if MESSAGE_TOP + MESSAGE_HEIGHT <= height // 2 else 4


def panel_top(height, line_height):
    """Where the open seats panel sits, including the run it reserves for an open chat entry."""
    band = (STRIP_BAND_BOTTOM if height < COMPACT_MAX_HEIGHT else STATUS_BOX_TOP + STATUS_BOX_HEIGHT) + PANEL_GAP
    lowest = max(0, height - PANEL_HEIGHT - PANEL_GAP)
    centred = min(max((height - PANEL_HEIGHT) // 2, min(band, lowest)), lowest)
    if height >= COMPACT_MAX_HEIGHT:
        return centred
    # A compact screen keeps the strip band with its toast row above the panel, and an open entry's
    # history row with its tight entry above that, so the panel takes the rows under the lower run.
    return max(centred, STRIP_BAND_BOTTOM + PANEL_ROW_HEIGHT + PANEL_GAP,
               chat_top_limit(height) + line_height + (line_height + 4) + PANEL_GAP)


def chat_available(height, line_height):
    """The run the band owns: under the seat's message and above the open seats panel."""
    return panel_top(height, line_height) - PANEL_GAP - chat_top_limit(height)


def occupier_steps():
    """The occupiers the band must clear: the seats panel and the seat's own message band."""
    return [
        {"op": "screen_message", "text": SEAT_MESSAGE, "screen": 0, "duration_ms": 60000},
        {"op": "key_down", "key": "F6"},
        {"op": "key_up", "key": "F6"},
        {"op": "wait", "panel_open": True},
        {"op": "wait", "renders": 4, "seat_text_contains": SEAT_MESSAGE, "player": 0},
    ]


def history_row_fits(height, text_size):
    """Whether the run the band owns holds one history row above the tight entry at that size.

    The arithmetic is the band's own: a row is lineH = max(12, font height) + 4 and the tight entry
    is lineH + 4, so the run has to reach 2 * lineH + 4 - 36 for the small font, 42 for the large.
    The run is chat_available(): under the seat's message band, which is 12 + 15 + 4 = 31 here, and
    above the seats panel, whose top reserves that same minimum on a compact screen.
    """
    if height not in SUPPORTED_HEIGHTS:
        raise ValueError(f"{height} is not a supported screen height")
    line_height = LINE_HEIGHT[text_size]
    return chat_available(height, line_height) >= line_height + (line_height + 4)


def open_entry_steps(size, text_size):
    """The chat key opens the entry; the band keeps what fits above it and nothing else."""
    width, height = size
    rows = [{"op": "assert_control", "control": "LabelMatchChat0",
             "equals": {"visible": history_row_fits(height, text_size)}}]
    return [
        {"op": "key_down", "key": "CHAT"},
        {"op": "key_up", "key": "CHAT"},
        {"op": "wait", "renders": 4, "chat_entry_open": True},
        {"op": "assert_control", "control": "TextMatchChatInput", "equals": {"visible": True}},
        *rows,
        {"op": "assert_net_ui_clear", "match": True, "chat_layout": True, "entry_open": True,
         "occupiers": ["seats_panel", "text_band"], "player": 0, "status": False},
        {"op": "key_down", "key": "Escape"},
        {"op": "key_up", "key": "Escape"},
        {"op": "wait", "renders": 4, "chat_entry_open": False},
        {"op": "assert_net_ui_clear", "match": True, "chat_layout": True, "entry_open": False,
         "occupiers": ["seats_panel", "text_band"], "player": 0, "status": False},
    ]


def host_steps(size, text_size):
    return [
        {"op": "wait", "service": "Running"},
        {"op": "wait", "sim_at_least": 60},
        {"op": "send_chat", "text": CHAT_LINE, "scope": "all"},
        {"op": "wait", "renders": 8, "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        {"op": "assert_control", "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        *occupier_steps(),
        *open_entry_steps(size, text_size),
        {"op": "finish"},
    ]


def guest_steps(size, text_size):
    return [
        {"op": "wait", "service": "Running"},
        {"op": "wait", "sim_at_least": 60},
        {"op": "wait", "renders": 120, "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        {"op": "assert_control", "control": "LabelMatchChatNewest", "text_contains": CHAT_LINE},
        *occupier_steps(),
        *open_entry_steps(size, text_size),
        {"op": "finish"},
    ]


def cases():
    """Every viewport at every chat text size, in the order their ports are handed out."""
    return [(size, text_size) for size in SIZES for text_size in TEXT_SIZES]


def script(steps):
    return {"schema": 1, "timeout_ms": 180000, "steps": steps}


def write_scripts(root: Path) -> dict[str, str]:
    written = {}
    for (width, height), text_size in cases():
        for who, steps in (("host", host_steps((width, height), text_size)),
                           ("guest", guest_steps((width, height), text_size))):
            path = root / f"match-chat-{width}x{height}-{text_size}-{who}.json"
            path.write_text(json.dumps(script(steps), indent=2) + "\n", encoding="utf-8")
            written[f"{width}x{height}-{text_size}-{who}"] = str(path)
    return written


def peer_args(who, port, root):
    args = ["-net-match-service-e2e", "-net-port", str(port), "-net-match-peers", "2",
            "-net-match-ticks", str(TICKS), "-net-match-input-delay", "3", "-net-autosave-seconds", "0",
            "-tick-hashes", "-out", str(root / f"{who}_trace.json"),
            "-net-match-report", str(root / f"{who}_report.json")]
    args += ["-net-host"] if who == "host" else ["-net-join", "127.0.0.1"]
    return args


def set_settings(runtime, values):
    path = runtime / "Userdata" / "Settings.ini"
    settings = path.read_text(encoding="utf-8")
    for name, value in values.items():
        settings, count = re.subn(rf"(?m)^(\s*{name}\s*=\s*)[^\r\n]*",
                                  lambda match: match[1] + str(value), settings)
        if count == 0:
            settings, anchored = re.subn(r"(?m)^(\s*ResolutionY\s*=\s*[^\r\n]*)",
                                         lambda match: match[1] + f"\n\t{name} = {value}", settings, count=1)
            if anchored != 1:
                raise RuntimeError(f"no SettingsMan anchor for private {name} setting")
        elif count != 1:
            raise RuntimeError(f"expected at most one private {name} setting, found {count}")
    path.write_text(settings, encoding="utf-8")


def launch_size(make_run, repo, root, size, text_size, port, timeout):
    width, height = size
    case = root / f"{width}x{height}-{text_size}"
    case.mkdir(parents=True, exist_ok=False)
    records, runs = {}, {}
    try:
        for who, steps in (("host", host_steps(size, text_size)), ("guest", guest_steps(size, text_size))):
            inputs = case / f"{who}_inputs"
            inputs.mkdir()
            probe = inputs / "probe.json"
            probe.write_text(json.dumps(script(steps), indent=2) + "\n", encoding="utf-8")
            env = {"CCCP_HEADLESS": "1", "CC_TEST_NET_UI_SCRIPT": str(probe)}
            runs[who] = make_run(repo, peer_args(who, port, case), case / who, timeout, env=env)
            set_settings(runs[who].cwd, {"ResolutionX": width, "ResolutionY": height,
                                         "NetworkChatVisible": 1, "NetworkChatTextSize": text_size})

        def drive(who):
            try:
                records[who] = runs[who].start().finish()
            except Exception as error:
                records[who] = {"error": repr(error)}

        threads = [threading.Thread(target=drive, args=(who,), daemon=True) for who in ("host", "guest")]
        threads[0].start()
        threading.Event().wait(2.0)
        threads[1].start()
        for thread in threads:
            thread.join()
    finally:
        for run in runs.values():
            run.close()
    return {"records": records, "logs": {who: str(case / who) for who in ("host", "guest")}}


def inspect(root: Path, size, text_size) -> dict:
    width, height = size
    case = root / f"{width}x{height}-{text_size}"
    detail = {"pass": False, "guest_saw_line": False, "layout": {}}
    for who in ("host", "guest"):
        result = case / f"{who}_inputs" / "net-ui-result.json"
        if not result.exists():
            detail[f"{who}_missing"] = True
            return detail
        payload = json.loads(result.read_text(encoding="utf-8"))
        if not payload.get("pass"):
            detail[f"{who}_error"] = payload.get("error")
            return detail
        for step in payload.get("steps", []):
            observed = step.get("observed", {})
            control = observed.get("control") or {}
            if CHAT_LINE in str(control.get("text", "")):
                detail["guest_saw_line" if who == "guest" else "host_saw_line"] = True
            net_ui = observed.get("net_ui")
            if net_ui and step.get("op") == "assert_net_ui_clear":
                detail["layout"][who] = net_ui
    detail["pass"] = bool(detail.get("guest_saw_line")) and "host" in detail["layout"] and "guest" in detail["layout"]
    return detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--launch", action="store_true", help="PHASE B only: two-peer engine launch")
    options = parser.parse_args()
    root = options.out.resolve()
    root.mkdir(parents=True, exist_ok=True)
    written = write_scripts(root)
    print(json.dumps({"scripts": written}, indent=2))
    if not options.launch:
        return 0
    sys.path.insert(0, str(options.repo / "tools"))
    from run_sim_test import make_run  # noqa: PLC0415
    os.environ["CCCP_HEADLESS"] = "1"
    result = {"pass": False, "sizes": {}}
    for index, (size, text_size) in enumerate(cases()):
        outcome = launch_size(make_run, options.repo.resolve(), root, size, text_size,
                              PORT_BASE + index * 2, options.timeout)
        result["sizes"][f"{size[0]}x{size[1]}-{text_size}"] = {
            "outcome": outcome, "inspect": inspect(root, size, text_size)}
    result["pass"] = all(row["inspect"]["pass"] for row in result["sizes"].values())
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"pass": result["pass"]}, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
